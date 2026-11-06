// Super Mario All-Stars port: macOS, as an app bundle. The SDL window and
// event loop, frame pacing, the ROM picker, as platform/linux/app_linux.cpp's;
// the sound, input and touch layers are the Android build's (platform/android:
// SDL), the video Metal (video_metal.mm), the application itself app/app.cpp.
// Settings, saves and the ROM live in ~/Library/Application Support/
// SuperMarioAllStars; the title font in the bundle's Resources.
#define SDL_MAIN_HANDLED
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <SDL.h>
#include <SDL_metal.h>
#include <SDL_syswm.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <set>
#include <string>
#include "app/app.h"
#include "app/sys.h"
#include "app/vkeys.h"
#include "core/png.h"

std::string g_fontDir;

namespace {
// Refreshes per game frame when the display rate is a whole multiple of the
// game's 60.1 fps, else 0 (60 Hz displays: 1; ProMotion at 120 Hz: 2).
int lockedRefreshes(double hz) {
  int k = (int)std::lround(hz / Snes::kFrameRate);
  return k >= 1 && std::fabs(hz / k - Snes::kFrameRate) / Snes::kFrameRate < 0.025 ? k : 0;
}

// The refresh rate of the display the window is on (0 if unknown).
double screenHz(SDL_Window* window) {
  SDL_SysWMinfo wm;
  SDL_VERSION(&wm.version);
  if (@available(macOS 12.0, *)) {  // (macOS 11: SDL's whole number below)
    if (SDL_GetWindowWMInfo(window, &wm) && wm.info.cocoa.window) {
      NSScreen* screen = wm.info.cocoa.window.screen;
      if (screen && screen.maximumFramesPerSecond > 0) return (double)screen.maximumFramesPerSecond;
    }
  }
  SDL_DisplayMode m;
  int di = SDL_GetWindowDisplayIndex(window);
  if (SDL_GetCurrentDisplayMode(di < 0 ? 0 : di, &m) == 0 && m.refresh_rate > 0) return m.refresh_rate;
  return 0;
}

// The bundle's Resources (the font), and the folder the app is in (where a
// "roms" folder may be put beside it). Run outside a bundle: the program's own folder.
std::string resourceDir() {
  NSString* r = [[NSBundle mainBundle] resourcePath];
  return r ? r.fileSystemRepresentation : ".";
}

std::string appDir() {
  NSString* b = [[NSBundle mainBundle] bundlePath];
  return b ? b.stringByDeletingLastPathComponent.fileSystemRepresentation : ".";
}

std::string dataDir() {
  NSArray<NSString*>* dirs = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
  std::string base;
  if (dirs.count > 0) base = dirs[0].fileSystemRepresentation;
  else if (const char* h = getenv("HOME"); h && *h) base = std::string(h) + "/Library/Application Support";
  else base = ".";
  return base + "/SuperMarioAllStars";
}

void makeDirs(const std::string& path) {  // (mkdir -p)
  for (size_t i = 1; i <= path.size(); i++)
    if (i == path.size() || path[i] == '/') makeDir(widenUtf8(path.substr(0, i)));
}

bool hasRomExt(const std::wstring& name) {
  std::wstring lower = name;
  for (wchar_t& c : lower) c = (wchar_t)towlower(c);
  for (const wchar_t* ext : {L".sfc", L".smc", L".swc", L".fig"})
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ext) == 0) return true;
  return false;
}

std::vector<std::wstring> romDirs(const std::wstring& data) {
  std::vector<std::wstring> dirs = {pathJoin(data, L"roms"), data};
  const std::wstring beside = widenUtf8(appDir());
  dirs.push_back(beside);
  dirs.push_back(pathJoin(beside, L"roms"));
  char cwd[4096];
  if (getcwd(cwd, sizeof cwd)) dirs.push_back(widenUtf8(cwd));
  return dirs;
}

// The ROM picker: an open panel, not modal (its answer arrives while SDL
// pumps the events, the window still drawing); collected in pollPick.
std::string g_picked;
int g_pickState = 0;  // 0 idle, 1 open, 2 answered

// --shot <file.png> [seconds]: the window's picture saved after a while (2 s), then quit (for testing).
std::string g_shotPath;
double g_shotAfter = 2.0;

// A file given (picked, dropped or on the command line): copied into the
// data folder's "roms" if it is the ROM. True if it was.
bool takeRom(const std::wstring& data, const std::string& path) {
  std::vector<uint8_t> d;
  if (path.empty() || !readAll(widenUtf8(path), d) || !isSmasRom(d, nullptr)) return false;
  std::wstring dir = pathJoin(data, L"roms");
  makeDirs(narrowUtf8(dir));
  return writeAll(pathJoin(dir, L"Super Mario All-Stars (USA).sfc"), d.data(), d.size());
}
}  // namespace

void openUrl(const char* url) { SDL_OpenURL(url); }

bool httpGet(const char* url, std::string& out) {
  out.clear();
  NSURL* u = [NSURL URLWithString:@(url)];
  if (!u) return false;
  NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u
                                                     cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                 timeoutInterval:10];
  [req setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
  [req setValue:@"SuperMarioAllStars-port" forHTTPHeaderField:@"User-Agent"];
  __block NSData* body = nil;
  __block bool ok = false;
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  NSURLSessionDataTask* task = [[NSURLSession sharedSession]
      dataTaskWithRequest:req
        completionHandler:^(NSData* data, NSURLResponse* resp, NSError* err) {
          ok = !err && [resp isKindOfClass:[NSHTTPURLResponse class]] && ((NSHTTPURLResponse*)resp).statusCode == 200;
          body = data;
          dispatch_semaphore_signal(done);
        }];
  [task resume];
  dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
  if (!ok || body.length == 0 || body.length > (1u << 20)) return false;
  out.assign((const char*)body.bytes, body.length);
  return true;
}
void openFolder(const std::wstring& path) {
  [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:@(narrowUtf8(path).c_str()) isDirectory:YES]];
}

namespace {
// A slang preset's file: its path (under "shaders") in the data folder, else
// the same path in the app's Resources (the presets that come with it).
std::wstring presetFile(const std::wstring& data, const std::wstring& rel) {
  if (rel.empty() || rel[0] == L'/') return rel;
  const std::wstring own = pathJoin(data, rel);
  return fileExists(own) ? own : pathJoin(widenUtf8(resourceDir()), rel);
}
}  // namespace

void App::applySlang(bool force) {
  const std::wstring full = presetFile(exeDir, cfg.slangPreset);
  if (!force && full == slangLoaded) return;
  std::string err;
  if (video.setSlang(full, &err)) {
    slangLoaded = full;
    if (!full.empty()) logf("slang preset loaded: %s", narrow(full).c_str());
  } else {
    logf("slang preset failed: %s: %s", narrow(full).c_str(), err.c_str());
    // (librashader's own errors are long: a short reason here, the whole of it in the log)
    std::string why = err.find("NotFound") != std::string::npos           ? "file not found"
                      : err.find("librashader.dylib") != std::string::npos ? err
                                                                           : "it couldn't be compiled (see smas_port.log)";
    menu.notify("Shader " + narrow(pathName(full)) + " not loaded: " + why, 4.0f);
    cfg.slangPreset.clear();
    video.setSlang(L"", nullptr);
    slangLoaded.clear();
  }
}

// The slang presets under the 'shaders' folders of the data folder and of the
// app's Resources (the ones that come with it), grouped by folder, and Off.
std::shared_ptr<MenuPage> App::pageSlang() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Slang Shader";
  p->wide = true;
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
    auto pick = [this](std::wstring rel) {
      cfg.slangPreset = std::move(rel);
      applySlang();
      menu.pop();
    };
    pg.items.push_back(MenuItem::action("Off (use the built-in filter)", [pick]() { pick(L""); }));
    if (!video.slangAvailable()) {
      pg.items.push_back(MenuItem::info("librashader not found",
                                        "Slang shaders need librashader.dylib in the app's Frameworks folder."));
      return;
    }
    namespace fs = std::filesystem;
    std::vector<fs::path> found;  // (relative to the folder holding "shaders")
    std::set<fs::path> seen;
    for (const fs::path& base : {fs::path(narrowUtf8(exeDir)), fs::path(resourceDir())}) {
      std::error_code ec;
      const fs::path root = base / "shaders";
      if (!fs::is_directory(root, ec)) continue;
      for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
           it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || it->path().extension() != ".slangp") continue;
        fs::path rel = it->path().lexically_relative(base);
        if (seen.insert(rel).second) found.push_back(rel);
      }
    }
    if (found.empty()) {
      pg.items.push_back(MenuItem::info(
          "No presets found", "Put .slangp presets (with their shader files) in a 'shaders' folder in " +
                                  narrowUtf8(exeDir) + ", e.g. the libretro slang-shaders collection."));
      return;
    }
    std::sort(found.begin(), found.end());
    fs::path lastDir;
    for (const fs::path& rel : found) {
      fs::path dir = rel.parent_path().lexically_relative("shaders");
      if (dir != lastDir) {
        pg.items.push_back(MenuItem::header(dir.empty() || dir == "." ? "shaders" : dir.string()));
        lastDir = dir;
      }
      std::wstring relW = widenUtf8(rel.string());
      MenuItem it = MenuItem::action(rel.stem().string(), [pick, relW]() { pick(relW); },
                                     "Some presets expect a 4:3 picture: with widescreen they can stretch or frame "
                                     "oddly.");
      if (relW == cfg.slangPreset) it.valueText = []() { return std::string("Active"); };
      pg.items.push_back(it);
    }
  };
  return p;
}

void App::applyDisplayMode() {
  if (!window) return;
  const bool full = cfg.displayMode != DisplayMode::Windowed;
  const bool now = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
  if (full == now) return;
  SDL_SetWindowFullscreen(window, full ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  if (!full) {
    int h = 224 * std::clamp(cfg.windowScale, 2, 4), w = h * 16 / 9;
    SDL_SetWindowSize(window, w, h);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  }
}

bool App::findRom(std::wstring* out) {
  for (const std::wstring& dir : romDirs(exeDir)) {
    std::vector<std::wstring> names;
    if (!listFiles(dir, names)) continue;
    for (const std::wstring& n : names) {
      if (!hasRomExt(n)) continue;
      std::wstring p = pathJoin(dir, n);
      std::vector<uint8_t> data;
      if (readAll(p, data) && isSmasRom(data, nullptr)) {
        *out = p;
        return true;
      }
    }
  }
  return false;
}

void App::pickRom() {
  if (pickingRom || g_pickState != 0) return;
  pickingRom = true;
  g_pickState = 1;
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.title = @"Choose the Super Mario All-Stars (USA) ROM";
  panel.canChooseDirectories = NO;
  panel.allowsMultipleSelection = NO;
  NSMutableArray<UTType*>* types = [NSMutableArray array];
  for (NSString* ext in @[ @"sfc", @"smc" ])
    if (UTType* t = [UTType typeWithFilenameExtension:ext]) [types addObject:t];
  if (types.count > 0) panel.allowedContentTypes = types;
  [panel beginWithCompletionHandler:^(NSModalResponse r) {
    g_picked = r == NSModalResponseOK && panel.URL ? panel.URL.fileSystemRepresentation : "";
    g_pickState = 2;
  }];
}

void App::pollPick() {
  if (!pickingRom || g_pickState != 2) return;
  std::string picked;
  picked.swap(g_picked);
  g_pickState = 0;
  pickingRom = false;
  if (window) SDL_RaiseWindow(window);
  if (picked.empty()) {
    menu.notify("No file chosen - or put the ROM in " + narrowUtf8(pathJoin(exeDir, L"roms")), 5.0f);
    return;
  }
  if (!takeRom(exeDir, picked)) {
    menu.notify("That isn't the Super Mario All-Stars (USA) ROM", 4.0f);
    return;
  }
  romPoll = 0;
  pollRom(0);
}

// (The covers and background go in the covers folder: Options > Launcher >
// Open Covers Folder; no picker here.)
void App::pickArt(int) {}
void App::pollArtPick() {}

// Clicks and drags (a mouse acts as one finger; a trackpad too): on the
// launcher's cards and buttons, the menus' rows.
void App::touchFrame() {
  touch.layout((float)video.width(), (float)video.height());
  touch.inGame = !inLauncher && snes && !menu.open() && !(game && game->levelLoading());
  touch.visible = touchActive ? cfg.touchMode != 2 : cfg.touchMode == 1;
  touch.showFF = cfg.fastForward && !achLocked();
  touch.showRewind = cfg.rewind && !achLocked();
  if (touch.takePressed(TouchControls::BtnMenu) && touch.inGame) openMenu();
  float dy = touch.takeDrag(), x, y;
  bool tapped = touch.takeTap(&x, &y);
  if (menu.open()) {
    if (dy != 0) menu.drag(dy);
    if (tapped) menu.tap(x, y);
  } else if (inLauncher && tapped) {
    launcherTap = launcher.tap(x, y);
  }
}

bool App::init() {
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
  SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");  // (the mouse taps the launcher and menus)
  SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
  SDL_SetHint(SDL_HINT_APP_NAME, "Super Mario All-Stars");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return false;
  }
  const std::string data = dataDir();
  makeDirs(data);
  exeDir = widenUtf8(data);
  g_fontDir = resourceDir();
  g_log = openFile(pathJoin(exeDir, L"smas_port.log"), "w");
  logf("starting, data dir %s, resources %s", data.c_str(), g_fontDir.c_str());
  configPath = pathJoin(exeDir, L"smas_port.ini");
  saveDir = pathJoin(exeDir, L"saves");
  shotDir = pathJoin(exeDir, L"screenshots");
  makeDir(saveDir);
  makeDir(pathJoin(exeDir, L"roms"));
  if (!cfg.load(configPath)) cfg.save(configPath);
  cfg.vsync = true;

  const bool full = cfg.displayMode != DisplayMode::Windowed;
  int wh = 224 * std::clamp(cfg.windowScale, 2, 4);
  window = SDL_CreateWindow("Super Mario All-Stars", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, wh * 16 / 9, wh,
                            SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                (full ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
  if (!window) {
    logf("window failed: %s", SDL_GetError());
    return false;
  }
  if (!video.init(window)) {
    logf("video init failed: %s", SDL_GetError());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Super Mario All-Stars", "Metal could not be started.", window);
    return false;
  }
  logf("video ok %dx%d", video.width(), video.height());
  if (!audio.init(cfg.audioLatency)) menu.notify("Audio device unavailable");
  else
    logf("audio: %s, %d Hz, %d-frame chunks, latency %d ms", SDL_GetCurrentAudioDriver(), audio.rate(),
         audio.deviceFrames(), cfg.audioLatency);
  input.init();
  menu.setCanvas(video.canvas());
  menu.touchUi = true;
  touchActive = false;

  const bool haveRom = loadRom();
  logf(haveRom ? "rom loaded: %s" : "rom not found%s", narrow(romPath).c_str());
  achStore.load(pathJoin(exeDir, L"achievements.ini"));
  launcher.setVersion(kAppVersion);
  launcher.setAllStarsOnly(cfg.launcherAllStars);
  launcher.selectGame(Launcher::cardGame(cfg.lastGame));
  playing = Launcher::cardGame(cfg.lastGame);
  if (haveRom) romReady();
  applyConfig();
  applySlang(true);
  inLauncher = true;
  // (--game smb1|smbll|smb2|smb3|allstars: straight into it, as on Windows)
  if (haveRom && !startGame.empty()) {
    static const wchar_t* kIds[Launcher::kCards + 1] = {L"smb1", L"smbll", L"smb2", L"smb3", L"allstars"};
    for (int i = 0; i <= Launcher::kCards; i++)
      if (startGame == kIds[i]) launchGame(Launcher::cardGame(i));
  }
  if (!achStore.noticeShown) {
    achStore.noticeShown = true;
    achStore.save();
    if (cfg.achievements) achWelcome();
  }
  return true;
}

void App::shutdown() {
  saveSram();
  cfg.save(configPath);
  music.shutdown();
  launcher.releaseGpu();
  menu.setCanvas(nullptr);
  audio.shutdown();
  input.shutdown();
  video.shutdown();
  if (window) SDL_DestroyWindow(window);
  window = nullptr;
  if (g_log) fclose(g_log);
  g_log = nullptr;
  SDL_Quit();
}

// One pass per display refresh (the next drawable waits for it, VSync). The
// refresh rate is the median of recent intervals; on a whole multiple of the
// game's 60.1 fps each game frame gets a steady number of refreshes, else the
// game time owed decides.
void App::run() {
  const Uint64 freq = SDL_GetPerformanceFrequency();
  Uint64 last = SDL_GetPerformanceCounter();
  const double period = 1.0 / Snes::kFrameRate;
  float intervals[48] = {};
  int nIntervals = 0;
  Uint64 statsAt = last;
  const Uint64 started = last;
  double vbCarry = 0;
  while (running) @autoreleasepool {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      const float W = (float)video.width(), H = (float)video.height();
      switch (e.type) {
        case SDL_QUIT: running = false; break;
        case SDL_WINDOWEVENT:
          if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED) {
            int w = 0, h = 0;
            SDL_Metal_GetDrawableSize(window, &w, &h);
            video.resize(w, h);
          } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            focused = false;
            input.clearKeys();
          } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            focused = true;
          }
          break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
          if (e.key.repeat) break;
          // (Option+Return as Alt+Enter on Windows, and the Mac's own Ctrl+Cmd+F:
          // windowed or fullscreen)
          const SDL_Keysym& k = e.key.keysym;
          if (e.type == SDL_KEYDOWN && ((k.sym == SDLK_RETURN && (k.mod & KMOD_ALT)) ||
                                        (k.sym == SDLK_f && (k.mod & KMOD_GUI) && (k.mod & KMOD_CTRL)))) {
            cfg.displayMode = cfg.displayMode == DisplayMode::Windowed ? DisplayMode::Borderless : DisplayMode::Windowed;
            applyDisplayMode();
            break;
          }
          if (k.mod & KMOD_GUI) break;  // (Cmd+Q and the other menu keys aren't the game's)
          int vk = vkFromScancode(k.scancode);
          if (vk) input.onKey(vk, e.type == SDL_KEYDOWN);
          if (e.type == SDL_KEYDOWN) touchActive = false;
          break;
        }
        case SDL_DROPFILE:
          if (e.drop.file) {
            if (takeRom(exeDir, e.drop.file)) {
              romPoll = 0;
              pollRom(0);
            } else {
              menu.notify("That isn't the Super Mario All-Stars (USA) ROM", 4.0f);
            }
            SDL_free(e.drop.file);
          }
          break;
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED: input.rescanDevices(); break;
        case SDL_CONTROLLERBUTTONDOWN: touchActive = false; break;
        case SDL_CONTROLLERAXISMOTION:
          if (std::abs(e.caxis.value) > 20000) touchActive = false;
          break;
        case SDL_FINGERDOWN:
          // (only the mouse, acting as a finger: a trackpad's own touches aren't taps)
          if (e.tfinger.touchId != SDL_MOUSE_TOUCHID) break;
          touch.onDown((int64_t)e.tfinger.fingerId, e.tfinger.x * W, e.tfinger.y * H);
          break;
        case SDL_FINGERMOTION:
          if (e.tfinger.touchId != SDL_MOUSE_TOUCHID) break;
          touch.onMove((int64_t)e.tfinger.fingerId, e.tfinger.x * W, e.tfinger.y * H);
          break;
        case SDL_FINGERUP:
          if (e.tfinger.touchId != SDL_MOUSE_TOUCHID) break;
          touch.onUp((int64_t)e.tfinger.fingerId, e.tfinger.x * W, e.tfinger.y * H);
          break;
        default: break;
      }
    }
    if (!running) break;
    pollPick();
    Uint64 now = SDL_GetPerformanceCounter();
    double dt = (double)(now - last) / freq;
    last = now;
    intervals[nIntervals++ % 48] = (float)dt;
    // (the display's own rate: Metal hands out drawables unevenly, so the
    // passes' spacing only averages to it; measured where it isn't known)
    double hz = screenHz(window);
    if (hz <= 0 && nIntervals >= 48) {
      float d[48];
      std::copy(intervals, intervals + 48, d);
      std::nth_element(d, d + 24, d + 48);
      if (d[24] > 0.002f) hz = 1.0 / d[24];
    }
    if (hz <= 0) hz = 60.0;
    if (pacing.mode != kPaceVsync || lockedRefreshes(hz) != pacing.perFrame) {
      pacing = Pacing();
      pacing.mode = kPaceVsync;
      pacing.perFrame = lockedRefreshes(hz);
      logf("pacing: %.2f Hz display, %d refreshes per frame (0 = uneven)", hz, pacing.perFrame);
    }
    pacing.hz = hz;
    int frames = 0;
    // refreshes since the last pass (the fraction left kept for the next: a
    // late pass and an early one after it count one each, not two and one)
    vbCarry += dt * hz;
    int vb = std::max(1, (int)std::lround(vbCarry));
    vbCarry = std::clamp(vbCarry - vb, -0.5, 0.5);
    if (vb > 6) {  // stalled (the window dragged, a dialog): resume without fast-forwarding
      vb = 0;
      vbCarry = 0;
      pacing.vblanks = 0;
      pacing.accum = 0;
      frames = 1;
    }
    if (pacing.perFrame) {
      pacing.vblanks += vb;
      while (pacing.vblanks >= pacing.perFrame) {
        pacing.vblanks -= pacing.perFrame;
        frames++;
      }
    } else {
      pacing.accum += vb / hz;
      while (pacing.accum >= period) {
        pacing.accum -= period;
        frames++;
      }
    }
    frames = std::min(frames, 3);
    frame(dt, frames);
    render();
    if (!g_shotPath.empty() && (double)(now - started) / freq >= g_shotAfter) {
      std::vector<uint32_t> px;
      int w = 0, h = 0;
      bool ok = video.capture(px, &w, &h) && writePng(g_shotPath, px.data(), w, h, w);
      logf(ok ? "shot saved: %s" : "shot failed: %s", g_shotPath.c_str());
      running = false;
    }
    video.present();
    // (sound running dry, for reports: logged every 10 s when it happened)
    if (now - statsAt > freq * 10) {
      statsAt = now;
      int dry = 0, low = 0;
      audio.takeStats(&dry, &low);
      if (dry > 0)
        logf("audio: ran dry %d times in 10 s (lowest queue %d ms; latency %d ms)", dry, low * 1000 / audio.rate(),
             cfg.audioLatency);
    }
  }
}

int main(int argc, char** argv) {
  SDL_SetMainReady();
  std::unique_ptr<App> app = std::make_unique<App>();
  g_app = app.get();
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--game") && i + 1 < argc) {
      app->startGame = widenUtf8(argv[++i]);
    } else if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
      g_shotPath = argv[++i];
      if (i + 1 < argc && atof(argv[i + 1]) > 0) g_shotAfter = atof(argv[++i]);
    } else {
      takeRom(widenUtf8(dataDir()), argv[i]);  // (a ROM given on the command line: kept for next time)
    }
  }
  int rc = 0;
  if (!app->init()) rc = 1;
  else app->run();
  app->shutdown();
  g_app = nullptr;
  return rc;
}
