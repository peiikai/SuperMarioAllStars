// Music Player and Sound Select. Tracks play on a private APU instance whose
// memory is built from the user's ROM (shared sound driver + sample bank + the
// chosen title's song block), so browsing music never disturbs the game.
// See docs/re/sound.md.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "app/menu.h"
#include "core/apu.h"
#include "core/msu1.h"

class Audio;
class Snes;
class SmasGame;

class MusicPlayer {
public:
  enum Bank { kBankMenu = 0, kBankSmb1, kBankSmb2, kBankSmb3Level, kBankSmb3Map, kBankCount };

  void init(Snes* snes, SmasGame* game, Audio* audio);
  void shutdown();
  // Called every frame. While the menu is open the player's audio is sent to
  // the output; closing the menu stops playback.
  void update(double dt, bool menuOpen);
  std::shared_ptr<MenuPage> soundSelectPage(Menu& menu);
  std::shared_ptr<MenuPage> playerPage(Menu& menu);
  bool playing() const { return playing_; }
  // Self-test: play a track for seconds without the audio device and report
  // the peak and RMS of the generated samples.
  bool testRender(int bank, uint8_t id, double seconds, int* peak, double* rms);
  void stop();
  void playTrack(int bank, uint8_t id) { playId(bank, id, -1); }  // (the recording mode's "music")
  // The MSU-1 music pack the game runs with (empty base: none): its tracks
  // play in place of the songs it has (core/msu1.h, app/msupack.h).
  void setMsu(const std::wstring& base, const std::string& name, float gain);
  void setMsuGain(float gain) {
    if (msu_) msu_->setGain(gain);
  }

  struct Track {
    uint8_t id;
    const char* name;
    bool loops;
  };
  static const char* bankName(int bank);
  static const std::vector<Track>& tracks(int bank);
  static int maxMusicId(int bank);

private:
  Snes* snes_ = nullptr;
  SmasGame* game_ = nullptr;
  Audio* audio_ = nullptr;
  std::unique_ptr<Apu> apu_;
  std::vector<uint8_t> aram_[kBankCount];
  int loadedBank_ = -1;
  bool playing_ = false;
  bool driverRunning_ = false;
  double spcTime_ = 0;
  double playTime_ = 0;
  int curBank_ = kBankSmb1;
  int curTrack_ = -1;      // index into tracks(curBank_) or -1 (raw id)
  uint8_t curId_ = 0;
  int afterTrack_ = 0;     // 0 stop, 1 repeat, 2 next
  float voiceLevel_[8] = {};
  std::vector<int16_t> buf_;
  std::unique_ptr<Msu1> msu_;  // (the player's own chip: the game's is left alone)
  std::string msuName_;
  bool msuOn_ = false;    // the song playing is the pack's
  bool usePack_ = true;   // the player's Music choice: the pack's songs or the originals
  // pending SFX port pulses
  struct Pulse {
    int port;
    uint8_t value;
    int phase;  // 0 = port held at 0, 1 = value posted, 2 = done
    double t;
  };
  std::vector<Pulse> pulses_;
  // sound select state
  int ssBank_ = kBankSmb1, ssMusic_ = 2, ssSfxA_ = 1, ssSfxB_ = 0, ssSfxC_ = 1;

  bool buildAram(int bank);
  bool startDriver(int bank);
  void runApu(double seconds, bool output);
  void playId(int bank, uint8_t id, int trackIndex);
  void playSfx(int port, uint8_t value);
  MenuItem sfxItem(const char* label, int* var, int maxv, int port, const char* help);
  void drawVisualizer(Menu& menu, Canvas* cv, RectF r, bool large);
};
