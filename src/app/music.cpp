#include "app/music.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "app/audio.h"
#include "core/snes.h"

namespace {

constexpr double kSpcHz = 1025280.0;  // 32040 Hz * 32

// Track names per song bank (IDs from docs/re/sound.md; duplicates omitted).
const std::vector<MusicPlayer::Track> kMenu = {
    {0x01, "All-Stars Title", true},
    {0x02, "Game Select", true},
};
const std::vector<MusicPlayer::Track> kSmb1 = {
    {0x14, "Super Mario Bros. Title", true},
    {0x15, "The Lost Levels Title", true},
    {0x02, "Overworld", true},
    {0x06, "Underground", true},
    {0x03, "Underwater", true},
    {0x08, "Castle", true},
    {0x01, "Coin Heaven", true},
    {0x0D, "Starman", true},
    {0x13, "Pipe Intro", false},
    {0x0C, "Course Clear", false},
    {0x0B, "World Clear", false},
    {0x04, "Castle Clear (Toad)", true},
    {0x05, "Castle Clear (Princess)", true},
    {0x09, "Lose a Life", false},
    {0x0A, "Game Over", false},
    {0x0E, "Ending (Super Mario Bros.)", true},
    {0x10, "Ending (The Lost Levels)", true},
    {0x0F, "Ending (Full Length)", false},
};
const std::vector<MusicPlayer::Track> kSmb2 = {
    {0x11, "Title Theme", false},
    {0x04, "Character Select", true},
    {0x01, "Overworld", true},
    {0x06, "Underground", true},
    {0x05, "Subspace", true},
    {0x08, "Boss", true},
    {0x07, "Wart", true},
    {0x0D, "Starman", true},
    {0x10, "Item Get / 1-Up", false},
    {0x0B, "Boss Defeated", false},
    {0x0C, "Level Clear", false},
    {0x13, "Warp", false},
    {0x12, "Short Fanfare", false},
    {0x09, "Lose a Life", false},
    {0x0A, "Game Over", false},
    {0x0E, "Ending", true},
};
const std::vector<MusicPlayer::Track> kSmb3Level = {
    {0x15, "Title", true},
    {0x16, "Battle Game", true},
    {0x02, "Overworld", true},
    {0x01, "Athletic", true},
    {0x06, "Underground", true},
    {0x03, "Underwater", true},
    {0x14, "Coin Heaven", true},
    {0x08, "Fortress", true},
    {0x10, "Airship", true},
    {0x0F, "King's Throne Room", true},
    {0x11, "Hammer Bros. Battle", true},
    {0x12, "Koopaling Battle", true},
    {0x05, "Final Battle", true},
    {0x04, "P-Switch", true},
    {0x0D, "Starman", true},
    {0x07, "Course Clear", false},
    {0x0B, "Victory", false},
    {0x13, "Royal Fanfare", false},
    {0x0C, "Short Jingle 1", false},
    {0x0E, "Short Jingle 2", false},
    {0x17, "Short Jingle 3", false},
    {0x18, "Flute Fanfare", false},
    {0x09, "Lose a Life", false},
    {0x0A, "Game Over", false},
};
const std::vector<MusicPlayer::Track> kSmb3Map = {
    {0x02, "World 1: Grass Land", true},
    {0x03, "World 2: Desert Land", true},
    {0x04, "World 3: Water Land", true},
    {0x05, "World 4: Giant Land", true},
    {0x06, "World 5: Sky Land", true},
    {0x10, "World 5: Upper Sky", true},
    {0x08, "World 6: Ice Land", true},
    {0x09, "World 7: Pipe Land", true},
    {0x0A, "World 8: Dark Land", true},
    {0x0B, "Warp Zone", true},
    {0x0C, "Toad House", true},
    {0x0D, "Spade Bonus Game", true},
    {0x0E, "Music Box", true},
    {0x07, "Course Clear", false},
    {0x01, "Ending", true},
};

// ROM locations (LoROM) of the sound data streams: [len16][addr16][data]...
// terminated by a zero length and a 16-bit exec address.
constexpr uint32_t kBootA = 0x07fc00, kBootALen = 0x400;
constexpr uint32_t kBootB = 0x3b8000, kBootBLen = 0x8000;
constexpr uint32_t kSamples = 0x0b8000;
const uint32_t kSongBlocks[MusicPlayer::kBankCount] = {0, 0x1f8000, 0x1fc000, 0x0c8000, 0x07c000};

size_t romOffset(uint32_t snesAdr) { return (size_t)((snesAdr >> 16) & 0x7f) * 0x8000 + (snesAdr & 0x7fff); }

// Walk a sound data stream, calling fn(address, data, length) for each block.
template <typename F>
bool walkStream(const std::vector<uint8_t>& s, size_t pos, F fn) {
  for (int guard = 0; guard < 64; guard++) {
    if (pos + 2 > s.size()) return false;
    size_t len = s[pos] | (s[pos + 1] << 8);
    if (len == 0) return true;
    if (pos + 4 + len > s.size()) return false;
    uint16_t adr = s[pos + 2] | (s[pos + 3] << 8);
    fn(adr, &s[pos + 4], len);
    pos += 4 + len;
  }
  return false;
}

}  // namespace

const char* MusicPlayer::bankName(int bank) {
  switch (bank) {
    case kBankMenu: return "All-Stars Menu";
    case kBankSmb1: return "Super Mario Bros. & The Lost Levels";
    case kBankSmb2: return "Super Mario Bros. 2";
    case kBankSmb3Level: return "Super Mario Bros. 3: Courses";
    case kBankSmb3Map: return "Super Mario Bros. 3: World Maps";
  }
  return "?";
}

static const char* shortBankName(int bank) {
  static const char* kShort[] = {"All-Stars Menu", "SMB1 & Lost Levels", "SMB2", "SMB3 Courses", "SMB3 World Maps"};
  return kShort[std::clamp(bank, 0, (int)MusicPlayer::kBankCount - 1)];
}

const std::vector<MusicPlayer::Track>& MusicPlayer::tracks(int bank) {
  switch (bank) {
    case kBankMenu: return kMenu;
    case kBankSmb2: return kSmb2;
    case kBankSmb3Level: return kSmb3Level;
    case kBankSmb3Map: return kSmb3Map;
    default: return kSmb1;
  }
}

int MusicPlayer::maxMusicId(int bank) {
  static const int kMax[kBankCount] = {0x02, 0x15, 0x13, 0x18, 0x10};
  return kMax[std::clamp(bank, 0, kBankCount - 1)];
}

void MusicPlayer::init(Snes* snes, SmasGame* game, Audio* audio) {
  snes_ = snes;
  game_ = game;
  audio_ = audio;
  apu_ = std::make_unique<Apu>();
  apu_->generateAudio = true;
}

void MusicPlayer::shutdown() { stop(); }

// Assemble the 64KB sound RAM for a bank: shared driver and tables from the
// boot stream, the main sample set, then the bank's song block at $C000.
bool MusicPlayer::buildAram(int bank) {
  if (!aram_[bank].empty()) return true;
  const std::vector<uint8_t>& rom = snes_->rom;
  auto slice = [&](uint32_t adr, size_t len) {
    size_t o = romOffset(adr);
    std::vector<uint8_t> v;
    if (o + len <= rom.size()) v.assign(rom.begin() + o, rom.begin() + o + len);
    return v;
  };
  std::vector<uint8_t> ram(0x10000, 0);
  std::vector<uint8_t> boot = slice(kBootA, kBootALen);
  std::vector<uint8_t> b2 = slice(kBootB, kBootBLen);
  boot.insert(boot.end(), b2.begin(), b2.end());
  bool haveDriver = false;
  walkStream(boot, 0, [&](uint16_t adr, const uint8_t* d, size_t n) {
    bool menuSongs = adr == 0xc000 && bank == kBankMenu;
    if (adr == 0x3d00 || adr == 0x3ee8 || adr == 0x0500 || menuSongs) {
      if (adr + n <= 0x10000) memcpy(&ram[adr], d, n);
      if (adr == 0x0500) haveDriver = true;
    }
  });
  std::vector<uint8_t> samples = slice(kSamples, 0x10000);
  walkStream(samples, 0, [&](uint16_t adr, const uint8_t* d, size_t n) {
    if ((adr == 0x3c00 || adr == 0x4000) && adr + n <= 0x10000) memcpy(&ram[adr], d, n);
  });
  if (bank != kBankMenu) {
    std::vector<uint8_t> songs = slice(kSongBlocks[bank], 0x8000);
    walkStream(songs, 0, [&](uint16_t adr, const uint8_t* d, size_t n) {
      if (adr + n <= 0x10000) memcpy(&ram[adr], d, n);
    });
  }
  if (!haveDriver) return false;
  aram_[bank] = std::move(ram);
  return true;
}

bool MusicPlayer::startDriver(int bank) {
  if (!buildAram(bank)) return false;
  apu_->reset(true);
  memcpy(apu_->ram, aram_[bank].data(), 0x10000);
  apu_->r.pc = 0x0500;
  apu_->r.sp = 0xef;
  spcTime_ = (double)apu_->cycles();
  loadedBank_ = bank;
  driverRunning_ = true;
  // The driver clears its input latches during init: let it settle first.
  runApu(0.05, false);
  return true;
}

void MusicPlayer::runApu(double seconds, bool output) {
  spcTime_ += seconds * kSpcHz;
  apu_->runTo((uint64_t)spcTime_);
  if (msuOn_ && msu_ && !apu_->samples.empty()) {
    msu_->mix(apu_->samples.data(), apu_->samples.size() / 2, 32040.0);
    // (the bars: the pack's sound has no voices, so eight slices of its level)
    const size_t frames = apu_->samples.size() / 2;
    for (int v = 0; v < 8; v++) {
      int peak = 0;
      for (size_t i = frames * v / 8; i < frames * (v + 1) / 8; i++)
        peak = std::max(peak, std::abs((int)apu_->samples[i * 2 + (v & 1)]));
      voiceLevel_[v] = std::max(std::min(1.0f, peak / 12000.0f), voiceLevel_[v] * 0.86f);
    }
  }
  if (output && audio_ && !apu_->samples.empty()) {
    audio_->push(apu_->samples.data(), apu_->samples.size() / 2, 32040.0);
  }
  // meter levels for the visualizer
  for (int v = 0; v < 8 && !msuOn_; v++) {
    float lvl = std::min(1.0f, std::abs(apu_->dsp.voices[v].lastOut) / 12000.0f + apu_->dsp.voices[v].env / 4096.0f);
    voiceLevel_[v] = std::max(lvl, voiceLevel_[v] * 0.86f);
  }
  apu_->samples.clear();
}

void MusicPlayer::setMsu(const std::wstring& base, const std::string& name, float gain) {
  stop();
  msu_.reset();
  msuName_ = name;
  if (base.empty()) return;
  msu_ = std::make_unique<Msu1>();
  msu_->open(base);
  msu_->setGain(gain);
}

void MusicPlayer::playId(int bank, uint8_t id, int trackIndex) {
  if (loadedBank_ != bank || !driverRunning_) {
    if (!startDriver(bank)) return;
  }
  curBank_ = bank;
  curTrack_ = trackIndex;
  curId_ = id;
  // The pack's track for the song, if it has one: a song's number plus its
  // game's place in the packs' numbering (as the MSU-1 patches count them:
  // Super Mario Bros. / The Lost Levels from 1, Super Mario Bros. 2 from 33,
  // Super Mario Bros. 3's map from 65 and levels from 97, the menu from 129).
  msuOn_ = false;
  if (msu_ && usePack_ && id) {
    static const int kFirst[kBankCount] = {0x80, 0x00, 0x20, 0x60, 0x40};  // (kBankMenu .. kBankSmb3Map)
    const int t = id + kFirst[std::clamp(bank, 0, kBankCount - 1)];
    msu_->write(0x2006, 0xff);
    msu_->write(0x2004, (uint8_t)t);
    msu_->write(0x2005, (uint8_t)(t >> 8));
    if (!(msu_->read(0x2000) & 0x08)) {
      const auto& list = tracks(bank);
      const bool loops = trackIndex < 0 || trackIndex >= (int)list.size() || list[trackIndex].loops;
      msu_->write(0x2007, loops ? 3 : 1);
      msuOn_ = true;
    }
  }
  // Starting the same ID again needs an edge: clear the port for a moment.
  // (The pack's song: the console's music stopped, its sound effects kept.)
  apu_->inPorts[2] = 0;
  runApu(0.02, false);
  apu_->inPorts[2] = msuOn_ ? 0xf0 : id;
  playing_ = true;
  playTime_ = 0;
  if (audio_) audio_->flush();
}

void MusicPlayer::playSfx(int port, uint8_t value) {
  if (!driverRunning_) {
    if (!startDriver(loadedBank_ >= 0 ? loadedBank_ : kBankSmb1)) return;
  }
  apu_->inPorts[port] = 0;
  pulses_.push_back({port, value, 0, 0.02});
  playing_ = true;
}

bool MusicPlayer::testRender(int bank, uint8_t id, double seconds, int* peak, double* rms) {
  Audio* saved = audio_;
  audio_ = nullptr;
  playId(bank, id, -1);
  double sum = 0;
  size_t n = 0;
  int pk = 0;
  for (double t = 0; t < seconds; t += 1.0 / 60) {
    spcTime_ += kSpcHz / 60;
    apu_->runTo((uint64_t)spcTime_);
    for (int16_t v : apu_->samples) {
      pk = std::max(pk, std::abs((int)v));
      sum += (double)v * v;
      n++;
    }
    apu_->samples.clear();
  }
  audio_ = saved;
  stop();
  *peak = pk;
  *rms = n ? std::sqrt(sum / n) : 0;
  return true;
}

void MusicPlayer::stop() {
  if (apu_ && driverRunning_) apu_->inPorts[2] = 0xf0;
  if (msu_) msu_->write(0x2007, 0);
  msuOn_ = false;
  playing_ = false;
  curTrack_ = -1;
  pulses_.clear();
}

void MusicPlayer::update(double dt, bool menuOpen) {
  if (!menuOpen) {
    if (playing_) stop();
    return;
  }
  if (!playing_ || !driverRunning_) {
    for (float& l : voiceLevel_) l *= 0.8f;
    return;
  }
  // SFX pulses: port goes 0 -> value (edge triggered), then back to 0.
  for (auto& p : pulses_) {
    p.t -= dt;
    if (p.t > 0) continue;
    if (p.phase == 0) {
      apu_->inPorts[p.port] = p.value;
      p.phase = 1;
      p.t = 0.1;
    } else if (p.phase == 1) {
      apu_->inPorts[p.port] = 0;
      p.phase = 2;
    }
  }
  pulses_.erase(std::remove_if(pulses_.begin(), pulses_.end(), [](const Pulse& p) { return p.phase == 2; }),
                pulses_.end());
  runApu(dt, true);
  playTime_ += dt;
  // End-of-track handling: the driver's music echo returns to 0 when a
  // non-looping song finishes.
  bool ended = playTime_ > 0.5 && (msuOn_ ? !msu_->playing() : apu_->outPorts[2] == 0 && curId_ != 0);
  const auto& list = tracks(curBank_);
  bool looping = curTrack_ >= 0 && curTrack_ < (int)list.size() && list[curTrack_].loops;
  if (afterTrack_ == 2 && curTrack_ >= 0 && (ended || (looping && playTime_ > 120.0))) {
    int next = (curTrack_ + 1) % (int)list.size();
    playId(curBank_, list[next].id, next);
  } else if (afterTrack_ == 1 && ended) {
    playId(curBank_, curId_, curTrack_);
  }
}

// ---------------------------------------------------------------------------
// UI

void MusicPlayer::drawVisualizer(Menu& menu, Canvas* cv, RectF r, bool large) {
  float s = menu.uiScale();
  ColorF acc = menu.accent();
  cv->fillRound(r, 12 * s, colorF(0.03f, 0.035f, 0.06f, 0.9f));
  float pad = 18 * s;
  float y = r.top + pad;
  std::string title = "Nothing playing";
  if (playing_ && curTrack_ >= 0 && curTrack_ < (int)tracks(curBank_).size())
    title = tracks(curBank_)[curTrack_].name;
  else if (playing_ && curId_)
    title = "Track " + std::to_string(curId_);
  menu.text(Menu::widen(title), rectF(r.left + pad, y, r.right - pad, y + 40 * s), 28 * s, true,
            colorF(1, 1, 1));
  y += 40 * s;
  const std::string from = playing_ && msuOn_ ? "  \xE2\x80\xA2  " + msuName_ : std::string();
  menu.text(Menu::widen(bankName(curBank_) + from), rectF(r.left + pad, y, r.right - pad, y + 28 * s), 18 * s, false,
            colorF(0.7f, 0.74f, 0.85f));
  y += 30 * s;
  char tbuf[32];
  int secs = (int)playTime_;
  snprintf(tbuf, sizeof tbuf, "%d:%02d", secs / 60, secs % 60);
  menu.text(Menu::widen(playing_ ? tbuf : "--:--"), rectF(r.left + pad, y, r.right - pad, y + 26 * s), 18 * s,
            false, colorF(0.55f, 0.9f, 0.6f));
  y += 36 * s;
  // 8 voice level bars
  float bottom = r.bottom - pad - 24 * s;
  float barsH = bottom - y;
  if (barsH < 20 * s) return;
  float gap = 8 * s;
  float bw = ((r.right - r.left) - 2 * pad - 7 * gap) / 8;
  for (int v = 0; v < 8; v++) {
    float x0 = r.left + pad + v * (bw + gap);
    cv->fillRound(rectF(x0, y, x0 + bw, bottom), 4 * s, colorF(1, 1, 1, 0.06f));
    float lv = std::clamp(voiceLevel_[v], 0.0f, 1.0f);
    int segs = 16;
    float sh = barsH / segs;
    int lit = (int)std::round(lv * segs);
    for (int k = 0; k < lit; k++) {
      float t = (float)k / segs;
      float rr = t < 0.6f ? 0.35f + t : 1.0f;
      float gg = t < 0.75f ? 0.85f : 0.85f - (t - 0.75f) * 2.5f;
      float yy = bottom - (k + 1) * sh;
      cv->fillRect(rectF(x0 + 2 * s, yy + 1.5f * s, x0 + bw - 2 * s, yy + sh - 1.5f * s),
                   colorF(rr, gg, 0.3f + 0.4f * (1 - t), 0.95f));
    }
    menu.text(std::to_wstring(v + 1), rectF(x0, bottom + 2 * s, x0 + bw, bottom + 22 * s), 14 * s, false,
              colorF(0.6f, 0.62f, 0.7f), kAlignCenter);
  }
  (void)acc;
  (void)large;
}

std::shared_ptr<MenuPage> MusicPlayer::playerPage(Menu& menu) {
  auto p = std::make_shared<MenuPage>();
  p->title = "Music Player";
  p->subtitle = "Q / E or LB / RB switch collection";
  p->listFraction = 0.52f;
  Menu* mp = &menu;
  p->rebuild = [this, mp](MenuPage& pg) {
    pg.items.clear();
    std::vector<std::string> banks;
    for (int b = 0; b < kBankCount; b++) banks.push_back(shortBankName(b));
    pg.items.push_back(MenuItem::choice("Collection", banks, [this]() { return curBank_; },
                                        [this, mp](int v) {
                                          curBank_ = v;
                                          stop();
                                          mp->refresh();  // show that collection's tracks
                                        }));
    pg.items.push_back(MenuItem::choice("After a Track", {"Stop", "Repeat", "Play Next"},
                                        [this]() { return afterTrack_; }, [this](int v) { afterTrack_ = v; },
                                        "Play Next moves on when a song ends (looping songs after 2 minutes)."));
    pg.items.push_back(MenuItem::action("Stop", [this]() { stop(); }));
    if (msu_) {
      pg.items.push_back(MenuItem::choice(
          "Music", {"Original", msuName_}, [this]() { return usePack_ ? 1 : 0; },
          [this](int v) {
            usePack_ = v != 0;
            if (playing_ && curId_) playId(curBank_, curId_, curTrack_);  // (the same song, the other way)
          },
          "The music pack's songs (chosen in Options > Audio), or the originals. Songs the pack doesn't have play "
          "the original."));
    }
    pg.items.push_back(MenuItem::separator("Tracks"));
    const auto& list = tracks(curBank_);
    for (int i = 0; i < (int)list.size(); i++) {
      int bank = curBank_;
      uint8_t id = list[i].id;
      MenuItem it = MenuItem::action(list[i].name, [this, bank, id, i]() { playId(bank, id, i); },
                                     list[i].loops ? "Loops" : "Plays once");
      it.valueText = [this, bank, i]() {
        return (playing_ && curBank_ == bank && curTrack_ == i) ? std::string("♫ Playing") : std::string();
      };
      pg.items.push_back(it);
    }
  };
  p->inputCustom = [this](Menu& m, const MenuInput& in) {
    if (in.tabLeft || in.tabRight) {
      curBank_ = (curBank_ + (in.tabRight ? 1 : kBankCount - 1)) % kBankCount;
      stop();
      m.refresh();
      return true;
    }
    return false;
  };
  p->drawCustom = [this](Menu& m, Canvas* cv, RectF r) { drawVisualizer(m, cv, r, true); };
  p->onLeave = [this]() { stop(); };
  (void)menu;
  return p;
}

MenuItem MusicPlayer::sfxItem(const char* label, int* var, int maxv, int port, const char* help) {
  MenuItem it;
  it.kind = MenuItem::Choice;
  it.label = label;
  it.options = [maxv]() { return std::vector<std::string>((size_t)maxv, std::string()); };
  it.get = [var]() { return *var - 1; };
  it.set = [var](int v) { *var = v + 1; };
  it.valueText = [var]() {
    char b[8];
    snprintf(b, sizeof b, "%02X", *var);
    return std::string(b);
  };
  it.onActivate = [this, var, port]() { playSfx(port, (uint8_t)*var); };
  it.help = help;
  return it;
}

std::shared_ptr<MenuPage> MusicPlayer::soundSelectPage(Menu& menu) {
  auto p = std::make_shared<MenuPage>();
  p->title = "Sound Select";
  p->subtitle = "Classic sound test";
  p->listFraction = 0.6f;
  auto hex = [](int v) {
    char b[8];
    snprintf(b, sizeof b, "%02X", v);
    return std::string(b);
  };
  p->rebuild = [this, hex](MenuPage& pg) {
    pg.items.clear();
    std::vector<std::string> banks;
    for (int b = 0; b < kBankCount; b++) banks.push_back(shortBankName(b));
    pg.items.push_back(MenuItem::choice("Sound Bank", banks, [this]() { return ssBank_; },
                                        [this](int v) {
                                          ssBank_ = v;
                                          ssMusic_ = std::min(ssMusic_, maxMusicId(v));
                                          stop();
                                        }));
    MenuItem music = MenuItem::slider("Music", 1, 0x18, 1, [this]() { return ssMusic_; },
                                      [this](int v) { ssMusic_ = std::clamp(v, 1, maxMusicId(ssBank_)); });
    music.valueText = [this, hex]() {
      std::string name;
      for (auto& t : tracks(ssBank_))
        if (t.id == ssMusic_) name = std::string("  ") + t.name;
      return hex(ssMusic_) + name;
    };
    music.help = "Left/Right choose a number, Enter plays it.";
    music.onActivate = [this]() { playId(ssBank_, (uint8_t)ssMusic_, -1); };
    music.kind = MenuItem::Choice;
    music.options = [this]() {
      std::vector<std::string> o;
      for (int i = 1; i <= maxMusicId(ssBank_); i++) o.push_back("");
      return o;
    };
    music.get = [this]() { return ssMusic_ - 1; };
    music.set = [this](int v) { ssMusic_ = v + 1; };
    pg.items.push_back(music);

    pg.items.push_back(sfxItem("Sound Effect A", &ssSfxA_, 0x47, 0, "Effects on channel A (Enter plays)."));
    MenuItem jump;
    jump.kind = MenuItem::Choice;
    jump.label = "Sound Effect B";
    auto jopts = std::make_shared<std::vector<std::string>>(std::vector<std::string>{"Jump", "Skid"});
    jump.options = [jopts]() { return *jopts; };
    jump.get = [this]() { return ssSfxB_; };
    jump.set = [this](int v) { ssSfxB_ = v; };
    jump.onActivate = [this]() { playSfx(1, ssSfxB_ == 0 ? 0x01 : 0x04); };
    jump.help = "Jump and skid sounds (Enter plays).";
    pg.items.push_back(jump);
    pg.items.push_back(sfxItem("Sound Effect C", &ssSfxC_, 0x4d, 3, "Effects on channel C (Enter plays)."));
    pg.items.push_back(MenuItem::action("Stop", [this]() { stop(); }));
  };
  p->drawCustom = [this](Menu& m, Canvas* cv, RectF r) { drawVisualizer(m, cv, r, false); };
  p->onLeave = [this]() { stop(); };
  (void)menu;
  return p;
}
