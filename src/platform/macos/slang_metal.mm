// Slang presets through librashader's Metal runtime: see slang_metal.h.
#include "platform/macos/slang_metal.h"
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#define LIBRA_RUNTIME_METAL
#include "../../../third_party/librashader/librashader_ld.h"
#include <algorithm>

namespace {
libra_instance_t g_libra;

// librashader's message for an error (and the error freed).
std::string message(libra_error_t e) {
  std::string s = "unknown error";
  char* text = nullptr;
  if (g_libra.error_write && g_libra.error_write(e, &text) == 0 && text) {
    s = text;
    if (g_libra.error_free_string) g_libra.error_free_string(&text);
  }
  if (g_libra.error_free) g_libra.error_free(&e);
  return s;
}
}  // namespace

bool SlangChain::available() {
  if (!tried_) {
    tried_ = true;
    // (loaded from the app's Frameworks first: its install name is
    // @rpath/librashader.dylib, so the loader's dlopen("librashader.dylib")
    // then finds it)
    NSString* fw = [[NSBundle mainBundle] privateFrameworksPath];
    if (fw) dlopen([fw stringByAppendingPathComponent:@"librashader.dylib"].fileSystemRepresentation, RTLD_NOW);
    g_libra = librashader_load_instance();
    ok_ = g_libra.instance_loaded && g_libra.preset_create && g_libra.mtl_filter_chain_create &&
          g_libra.mtl_filter_chain_frame && g_libra.mtl_filter_chain_free;
  }
  return ok_;
}

bool SlangChain::load(id<MTLCommandQueue> queue, const std::string& path, std::string* error) {
  if (path.empty()) {
    release();
    return true;
  }
  if (!available()) {
    if (error) *error = "librashader.dylib not found";
    return false;
  }
  libra_shader_preset_t preset = nullptr;
  if (libra_error_t e = g_libra.preset_create(path.c_str(), &preset)) {
    if (error) *error = message(e);
    return false;
  }
  filter_chain_mtl_opt_t opt{};
  opt.version = LIBRASHADER_CURRENT_VERSION;
  libra_mtl_filter_chain_t chain = nullptr;
  libra_error_t e = g_libra.mtl_filter_chain_create(&preset, queue, &opt, &chain);
  if (preset && g_libra.preset_free) g_libra.preset_free(&preset);
  if (e) {
    if (error) *error = message(e);
    return false;
  }
  release();
  chain_ = chain;
  return true;
}

void SlangChain::draw(id<MTLCommandBuffer> cmd, id<MTLTexture> src, id<MTLTexture> out, float x, float y, float w,
                      float h, unsigned frame) {
  if (!chain_ || !src || !out) return;
  libra_viewport_t vp{x, y, (uint32_t)std::max(1.0f, w), (uint32_t)std::max(1.0f, h)};
  frame_mtl_opt_t fo{};
  fo.version = LIBRASHADER_CURRENT_VERSION;
  fo.total_subframes = 1;
  fo.current_subframe = 1;
  fo.frames_per_second = 60.0988f;
  auto chain = (libra_mtl_filter_chain_t)chain_;
  if (libra_error_t e = g_libra.mtl_filter_chain_frame(&chain, cmd, frame, src, out, &vp, nullptr, &fo)) message(e);
}

void SlangChain::release() {
  if (chain_) {
    auto chain = (libra_mtl_filter_chain_t)chain_;
    g_libra.mtl_filter_chain_free(&chain);
  }
  chain_ = nullptr;
}
