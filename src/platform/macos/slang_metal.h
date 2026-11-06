// RetroArch slang shader presets (.slangp) on macOS, through librashader's
// Metal runtime, as app/slang.h is for Direct3D 11: librashader.dylib from the
// app's Frameworks folder (third_party/librashader, built by
// packaging/macos/build_librashader.sh), loaded at run time. Without it the
// game runs as before, on its built-in filters. (Objective-C++ only.)
#pragma once
#import <Metal/Metal.h>
#include <string>

class SlangChain {
public:
  // Whether librashader.dylib could be loaded (tried once).
  bool available();
  // Build the chain for a preset file; false (and why) on failure, the old
  // chain kept. An empty path frees the chain (built-in filters again).
  bool load(id<MTLCommandQueue> queue, const std::string& path, std::string* error);
  bool active() const { return chain_ != nullptr; }
  // Run the chain on the game picture (src: exactly its size, as the presets
  // take the source's size from it) into out's rectangle, recorded on cmd (a
  // command buffer of its own, nothing in it yet).
  void draw(id<MTLCommandBuffer> cmd, id<MTLTexture> src, id<MTLTexture> out, float x, float y, float w, float h,
            unsigned frame);
  void release();

private:
  bool tried_ = false, ok_ = false;
  void* chain_ = nullptr;  // libra_mtl_filter_chain_t
};
