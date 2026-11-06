// Windows: the trailer's recording mode (--autotest <dir> <script>, with
// --test-size WxH for the picture). The real app, its window hidden, runs one
// game frame per pass from a script: key presses, save states, controller
// recordings, settings, menus; "record" writes every picture it draws to a
// lossless video (through ffmpeg) and its sound to a raw file beside it.
//
// Script lines: "<pass> <command> [argument]", passes counted from 0.
//   down / up / tap KEY      a key (up, down, left, right, enter, esc, space, tab, a letter, or a VK number)
//   game smb1|smbll|smb2|smb3|allstars   start a game from the launcher
//   launcher                 back to the launcher
//   loadstate NAME           <dir>\NAME.sst into the running game
//   pads NAME                <dir>\NAME.pads: the two controllers, uint16 each per game frame, from the next one
//   set section.key=value    a setting, as smas_port.ini has it (display.aspect=3, graphics.filter=3,
//                            graphics.slang_preset=..., game.coop=1, cheats.mask=...), applied at once
//   hfr N                    N pictures per game frame (high frame rate: N-1 in-between), recorded each
//   menu PAGE                open the menu at a page (main, options, display, graphics, color, audio,
//                            controls, cheats, levelselect, achievements, music, credits, slang); "menu close"
//   music BANK ID            a track through the music player (ID in hex; banks: 0 All-Stars, 1 SMB1/LL,
//                            2 SMB2, 3 SMB3, 4 SMB3 maps); "music stop"
//   award ID                 an achievement's pop-up
//   notify TEXT_WITH_UNDERSCORES   a notice
//   record NAME / stoprec    <dir>\NAME.mkv (60 fps x hfr) + NAME.pcm (32 kHz stereo s16) + NAME.txt
//   shot NAME                <dir>\NAME.png
//   mark NAME                <dir>\marks.txt: the pass and an FNV-1a of the console's work RAM
//   achexport DIR            Achievements > Export to CSV, into <dir>\DIR
//   quit
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

#include "app/app.h"
#include "app/achexport.h"
#include "app/sys.h"
#include "core/png.h"

namespace {
int vkOf(const std::string& n) {
  static const struct {
    const char* n;
    int vk;
  } names[] = {{"up", VK_UP},       {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT}, {"enter", VK_RETURN},
               {"esc", VK_ESCAPE},  {"space", VK_SPACE}, {"tab", VK_TAB}, {"back", VK_BACK},   {"f1", VK_F1},
               {"f2", VK_F2},       {"f3", VK_F3},     {"f4", VK_F4},     {"f5", VK_F5},       {"f6", VK_F6},
               {"f7", VK_F7},       {"f8", VK_F8},     {"f9", VK_F9},     {"f10", VK_F10},     {"f11", VK_F11},
               {"pgup", VK_PRIOR},  {"pgdn", VK_NEXT}, {"shift", VK_LSHIFT}, {"ctrl", VK_LCONTROL}};
  for (auto& e : names)
    if (n == e.n) return e.vk;
  if (n.size() == 1) return toupper((unsigned char)n[0]);
  return (int)strtol(n.c_str(), nullptr, 0);
}
}  // namespace

bool App::atLoad(const std::wstring& path) {
  std::string text;
  if (!readText(path, text)) return false;
  std::istringstream f(text);
  std::string line;
  while (std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    AtStep s;
    ls >> s.pass >> s.cmd;
    std::getline(ls, s.arg);
    while (!s.arg.empty() && s.arg[0] == ' ') s.arg.erase(0, 1);
    if (s.cmd.empty()) continue;
    if (s.cmd == "tap") {
      at.steps.push_back({s.pass, "down", s.arg});
      at.steps.push_back({s.pass + 3, "up", s.arg});
    } else {
      at.steps.push_back(s);
    }
  }
  std::stable_sort(at.steps.begin(), at.steps.end(), [](const AtStep& a, const AtStep& b) { return a.pass < b.pass; });
  return true;
}

void App::atSet(const std::string& kv) {
  // (the settings as they stand, the one changed after them: the last of a key wins)
  size_t eq = kv.find('='), dot = kv.find('.');
  if (eq == std::string::npos || dot == std::string::npos || dot > eq) return;
  const std::wstring tmp = pathJoin(at.dir, L"_set.ini");
  cfg.save(tmp);
  std::string text;
  readText(tmp, text);
  text += "\n[" + kv.substr(0, dot) + "]\n" + kv.substr(dot + 1) + "\n";
  if (FILE* f = _wfopen(tmp.c_str(), L"wb")) {
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
  }
  cfg.load(tmp);
  cfg.displayMode = DisplayMode::Windowed;
  cfg.vsync = false;
  applyConfig();
  audio.setVolume(0);
  applySlang(true);
}

void App::atStartRec(const std::string& name) {
  atStopRec();
  const int w = video.width(), h = video.height();
  const std::wstring base = pathJoin(at.dir, widenUtf8(name));
  const int fps = 60 * std::max(1, at.subs);
  // (the console's 60.1 Hz shown as 60: a clip's sound is stretched to match by the trailer script)
  std::wstring cmd = L"ffmpeg -y -v error -f rawvideo -pix_fmt bgr0 -s " + std::to_wstring(w) + L"x" + std::to_wstring(h) +
                     L" -r " + std::to_wstring(fps) + L" -i - -c:v libx264 -preset ultrafast -qp 0 \"" + base + L".mkv\"";
  at.vid = _wpopen(cmd.c_str(), L"wb");
  at.pcm = _wfopen((base + L".pcm").c_str(), L"wb");
  at.recName = base;
  at.recFrames = 0;
  at.recSamples = 0;
  at.recW = w;
  at.recH = h;
}

void App::atStopRec() {
  if (at.vid) {
    _pclose(at.vid);
    at.vid = nullptr;
  }
  if (at.pcm) {
    fclose(at.pcm);
    at.pcm = nullptr;
    if (FILE* f = _wfopen((at.recName + L".txt").c_str(), L"wb")) {
      fprintf(f, "%d %d %d %d\n", at.recW, at.recH, at.recFrames, std::max(1, at.subs));
      fclose(f);
    }
  }
}

void App::atRecordAudio(const int16_t* s, size_t frames) {
  if (!at.pcm) return;
  fwrite(s, 4, frames, at.pcm);
  at.recSamples += (int64_t)frames;
}

void App::atRecordPicture() {
  if (!at.vid) return;
  std::vector<uint32_t> px;
  int w, h;
  if (!video.capture(px, &w, &h) || w != at.recW || h != at.recH) return;
  fwrite(px.data(), 4, px.size(), at.vid);
  at.recFrames++;
  // (sound kept level with the pictures: silence where none was made, the menu open)
  const int64_t due = (int64_t)std::llround(at.recFrames / (60.0 * std::max(1, at.subs)) * Snes::kSampleRate);
  if (at.pcm && at.recSamples + 600 < due) {
    std::vector<int16_t> z((size_t)(due - at.recSamples) * 2, 0);
    fwrite(z.data(), 4, z.size() / 2, at.pcm);
    at.recSamples = due;
  }
}

void App::atRunSteps() {
  while (at.pos < at.steps.size() && at.steps[at.pos].pass <= at.pass) {
    const AtStep& s = at.steps[at.pos++];
    const std::string& a = s.arg;
    if (s.cmd == "down" || s.cmd == "up") {
      input.onKey(vkOf(a), s.cmd == "down");
    } else if (s.cmd == "game") {
      static const char* kIds[Launcher::kCards + 1] = {"smb1", "smbll", "smb2", "smb3", "allstars"};
      for (int i = 0; i <= Launcher::kCards; i++)
        if (a == kIds[i]) launchGame(Launcher::cardGame(i));
    } else if (s.cmd == "launcher") {
      returnToLauncher();
    } else if (s.cmd == "loadstate") {
      std::vector<uint8_t> d;
      if (snes && readAll(pathJoin(at.dir, widenUtf8(a) + L".sst"), d) && snes->loadState(d)) {
        game->onStateLoaded();
        rewind.clear();
        audio.flush();
      } else {
        logf("autotest: loadstate %s failed", a.c_str());
      }
    } else if (s.cmd == "pads") {
      std::vector<uint8_t> d;
      at.pads.clear();
      at.padPos = 0;
      if (readAll(pathJoin(at.dir, widenUtf8(a) + L".pads"), d)) {
        at.pads.resize(d.size() / 2);
        std::memcpy(at.pads.data(), d.data(), at.pads.size() * 2);
      }
    } else if (s.cmd == "set") {
      atSet(a);
      launcher.setAllStarsOnly(cfg.launcherAllStars);  // (launcher.allstars_only, read at start otherwise)
    } else if (s.cmd == "hfr") {
      at.subs = std::clamp(atoi(a.c_str()), 1, (int)Ppu::kMaxSubFrames);
    } else if (s.cmd == "menu") {
      if (a == "close") {
        menu.closeAll();
      } else {
        if (!menu.open()) openMenu();
        menuAnim = 1.0f;
        if (a == "options") menu.push(pageOptions());
        else if (a == "display") menu.push(pageDisplay());
        else if (a == "graphics") menu.push(pageGraphics());
        else if (a == "color") menu.push(pageColor());
        else if (a == "audio") menu.push(pageAudio());
        else if (a == "controls") menu.push(pageControls());
        else if (a == "cheats") menu.push(pageCheats());
        else if (a == "levelselect") menu.push(pageLevelSelect());
        else if (a == "achievements") menu.push(pageAchievements());
        else if (a == "music") menu.push(pageMusicPlayer());
        else if (a == "credits") menu.push(pageCredits());
        else if (a == "slang") menu.push(pageSlang());
      }
    } else if (s.cmd == "music") {
      // music BANK ID (hex): a track through the music player; music stop
      int bank = 0;
      unsigned id = 0;
      if (a == "stop") music.stop();
      else if (sscanf(a.c_str(), "%d %x", &bank, &id) == 2) music.playTrack(bank, (uint8_t)id);
    } else if (s.cmd == "award") {
      if (const AchievementDef* d = findAchievement(a)) menu.achievement(d->name, d->desc);
    } else if (s.cmd == "notify") {
      std::string t = a;
      std::replace(t.begin(), t.end(), '_', ' ');
      menu.notify(t);
    } else if (s.cmd == "record") {
      atStartRec(a);
    } else if (s.cmd == "stoprec") {
      atStopRec();
    } else if (s.cmd == "shot") {
      at.shot = a;
    } else if (s.cmd == "mark") {
      uint32_t hsh = 2166136261u;
      if (snes)
        for (uint8_t b : snes->wram) hsh = (hsh ^ b) * 16777619u;
      if (FILE* f = _wfopen(pathJoin(at.dir, L"marks.txt").c_str(), L"ab")) {
        fprintf(f, "%s pass %d ram %08x\n", a.c_str(), at.pass, hsh);
        fclose(f);
      }
    } else if (s.cmd == "achexport") {  // achexport DIR: Achievements > Export to CSV, into <dir>/DIR
      std::string err;
      if (!exportAchievements(pathJoin(at.dir, widenUtf8(a)), achStore, &err)) logf("autotest: achexport: %s", err.c_str());
    } else if (s.cmd == "quit") {
      running = false;
    }
  }
}

// One pass per game frame, as fast as it goes; pictures recorded as drawn.
void App::runAutotest() {
  const double period = 1.0 / Snes::kFrameRate;
  // (all the sound the app plays, the game's and the music player's, recorded; none heard)
  audio.tap = [](void* ctx, const int16_t* s, size_t n) { static_cast<App*>(ctx)->atRecordAudio(s, n); };
  audio.tapCtx = this;
  audio.setVolume(0);
  while (running) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) running = false;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    atRunSteps();
    if (!running) break;
    const int subs = std::max(1, at.subs);
    if (snes) snes->ppu.subFrames = subs, snes->ppu.subTimed = false;
    frame(period, 1);
    if (!video.ready()) break;
    for (int k = 1; k <= subs; k++) {
      hfrT = subs > 1 ? (double)k / subs : -1;
      render();
      if (!at.shot.empty() && k == subs) {
        std::vector<uint32_t> px;
        int w, h;
        if (video.capture(px, &w, &h)) writePng(narrow(pathJoin(at.dir, widenUtf8(at.shot) + L".png")), px.data(), w, h, w);
        at.shot.clear();
      }
      atRecordPicture();
      video.present(0);
    }
    at.pass++;
  }
  atStopRec();
}
