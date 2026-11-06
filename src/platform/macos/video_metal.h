// Video on macOS: Metal. The game picture with the same filters and colour
// settings as the Direct3D 11 (app/video.cpp) and OpenGL ES (platform/android/
// video_gles.cpp) ones, then the UI drawn on a Canvas (canvas.h) whose text
// comes from Core Text. The same interface as video_gles.h, so app.cpp and
// app_macos.mm use it as Linux uses that.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "app/canvas.h"

extern std::string g_fontDir;  // (where the title font is: the bundle's Resources; see app_macos.mm)

struct VideoParams {
  int srcW = 256, srcH = 224;  // active source size (pixels)
  IRect dest{};                // destination rect in screen pixels
  int filter = 1;
  float scanlines = 0.45f;
  float curve = 0.0f;
  int colorProfile = 0;
  float brightness = 1, contrast = 1, saturation = 1, gamma = 1;
  int colorblind = 0;
  float temperature = 0;  // -1 cool .. +1 warm
  int pillarbox = 1;      // 0 black, 1 blurred, 2 dark frame
  float fade = 1.0f;
  unsigned frame = 0;
};

struct SDL_Window;
class MetalCanvas;

class Video {
public:
  Video();
  ~Video();
  bool init(SDL_Window* window);
  void shutdown();
  void resize(int w, int h);
  int width() const { return w_; }
  int height() const { return h_; }

  void upload(const uint32_t* px, int w, int h, int stride);
  void draw(const VideoParams& p);
  void clear();  // black, no game picture (the launcher)
  Canvas* canvas();
  void beginUi();
  void endUi();
  void present();
  bool capture(std::vector<uint32_t>& out, int* w, int* h);
  // A slang shader preset in place of the built-in filters (empty: off);
  // false (and why) if it couldn't be loaded. See slang_metal.h.
  bool setSlang(const std::wstring& path, std::string* error);
  bool slangActive() const;
  bool slangAvailable();

private:
  struct State;  // (the Metal objects: video_metal.mm)
  std::unique_ptr<State> s_;
  int w_ = 0, h_ = 0;
  std::unique_ptr<MetalCanvas> canvas_;
  void pass(const VideoParams& p, int pass, int x, int y, int w, int h);
};
