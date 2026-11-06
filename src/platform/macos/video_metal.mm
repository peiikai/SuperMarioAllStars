// Video on macOS (SDL window, Metal): see video_metal.h. The shaders and the
// canvas follow platform/android/video_gles.cpp line for line where they can.
#include "platform/macos/video_metal.h"
#include "platform/macos/slang_metal.h"
#import <CoreText/CoreText.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL.h>
#include <SDL_metal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace {

void logError(const char* msg) { fprintf(stderr, "SmasPort: %s\n", msg); }

// ---- the shaders: app/video.cpp's game picture, video_gles.cpp's UI quads ----
const char* kShaders = R"(
#include <metal_stdlib>
using namespace metal;

struct GameU {
  float4 texSize;  // w, h, 1/w, 1/h
  float4 outSize;  // viewport w, h, 1/w, 1/h
  float4 f0;       // filter, scanlines, curve, pass
  float4 f1;       // brightness, contrast, saturation, gamma
  float4 f2;       // profile, colorblind, src w, src h
  float4 f3;       // fade, pillarbox, temperature, 0
};
struct GOut {
  float4 pos [[position]];
  float2 uv;
};

vertex GOut gameVS(uint vid [[vertex_id]]) {
  float2 p = float2(float((vid << 1) & 2), float(vid & 2));
  GOut o;
  o.pos = float4(p * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
  o.uv = p;
  return o;
}

constexpr sampler sP(filter::nearest, address::clamp_to_edge);
constexpr sampler sL(filter::linear, address::clamp_to_edge);

float2 clampUv(float2 uv, constant GameU& u) { return clamp(uv, 0.5f * u.texSize.zw, float2(1.0f) - 0.5f * u.texSize.zw); }
float3 texNearest(texture2d<float> t, float2 uv, constant GameU& u) { return t.sample(sP, clampUv(uv, u)).rgb; }
float3 texLinear(texture2d<float> t, float2 uv, constant GameU& u) { return t.sample(sL, clampUv(uv, u)).rgb; }

float3 texSharp(texture2d<float> t, float2 uv, constant GameU& u) {
  float2 texel = uv * u.texSize.xy;
  float2 scale = max(floor(u.outSize.xy / u.f2.zw), float2(1.0f));
  float2 tf = floor(texel), s = fract(texel);
  float2 region = 0.5f - 0.5f / scale;
  float2 cd = s - 0.5f;
  float2 f = (cd - clamp(cd, -region, region)) * scale + 0.5f;
  return texLinear(t, (tf + f) * u.texSize.zw, u);
}

float3 texRow(texture2d<float> t, float x, float row, constant GameU& u) {
  float2 scale = max(floor(u.outSize.xy / u.f2.zw), float2(1.0f));
  float tf = floor(x), s = fract(x);
  float region = 0.5f - 0.5f / max(scale.x, 1.0f);
  float cd = s - 0.5f;
  float f = (cd - clamp(cd, -region, region)) * max(scale.x * 0.6f, 1.0f) + 0.5f;
  return texLinear(t, float2((tf + f) * u.texSize.z, (row + 0.5f) * u.texSize.w), u);
}

float3 crt(texture2d<float> t, float2 uv, float2 pos, bool aperture, constant GameU& u) {
  float2 texel = uv * u.texSize.xy;
  float y = texel.y - 0.5f, yf = floor(y), d = y - yf;
  float3 c0 = texRow(t, texel.x, yf, u), c1 = texRow(t, texel.x, yf + 1.0f, u);
  c0 = c0 * c0;
  c1 = c1 * c1;
  float strength = u.f0.y;
  float3 w0 = exp(-d * d * mix(3.0f, 12.0f, strength) / (0.6f + 0.8f * sqrt(c0)));
  float3 w1 = exp(-(1.0f - d) * (1.0f - d) * mix(3.0f, 12.0f, strength) / (0.6f + 0.8f * sqrt(c1)));
  float3 col = c0 * w0 + c1 * w1;
  col *= mix(1.0f, 1.55f, strength);
  if (aperture) {
    int m = int(fmod(pos.x, 3.0f));
    float3 mask = m == 0 ? float3(1.0f, 0.7f, 0.7f) : (m == 1 ? float3(0.7f, 1.0f, 0.7f) : float3(0.7f, 0.7f, 1.0f));
    col *= mix(float3(1.0f), mask * 1.25f, strength);
  }
  return sqrt(max(col, 0.0f));
}

float3 lcd(texture2d<float> t, float2 uv, constant GameU& u) {
  float2 texel = uv * u.texSize.xy;
  float2 f = fract(texel);
  float3 c = texNearest(t, uv, u);
  float2 scale = u.outSize.xy / u.f2.zw;
  float2 edge = smoothstep(float2(0.0f), 1.5f / scale, f) * smoothstep(float2(0.0f), 1.5f / scale, 1.0f - f);
  float g = mix(1.0f, edge.x * edge.y, 0.55f * u.f0.y + 0.2f);
  return c * g;
}

float3 applyProfile(float3 c, constant GameU& u) {
  int prof = int(u.f2.x);
  float l = dot(c, float3(0.299f, 0.587f, 0.114f));
  if (prof == 1) {
    c = mix(float3(l), c, 1.3f);
    c = (c - 0.5f) * 1.08f + 0.5f;
  } else if (prof == 2) {
    c = pow(max(c, 0.0f), float3(1.15f));
    c = c * float3x3(float3(1.02f, 0.02f, -0.04f), float3(0.0f, 1.0f, 0.0f), float3(-0.02f, 0.03f, 0.95f));
  } else if (prof == 3) {
    c = mix(float3(l), c, 0.72f);
    c = (c - 0.5f) * 0.9f + 0.52f;
  } else if (prof == 4) {
    c = float3(l);
  } else if (prof == 5) {
    c = float3(dot(c, float3(0.393f, 0.769f, 0.189f)), dot(c, float3(0.349f, 0.686f, 0.168f)),
               dot(c, float3(0.272f, 0.534f, 0.131f)));
  } else if (prof == 6) {
    float q = floor(clamp(l, 0.0f, 1.0f) * 3.999f);
    c = q < 1.0f ? float3(0.06f, 0.22f, 0.06f) : q < 2.0f ? float3(0.19f, 0.38f, 0.19f)
      : q < 3.0f ? float3(0.55f, 0.67f, 0.06f) : float3(0.61f, 0.74f, 0.06f);
  } else if (prof == 7) {
    c = float3(pow(clamp(l, 0.0f, 1.0f), 0.8f), 0.0f, 0.0f);
  } else if (prof == 8) {
    c = c * float3(1.0f, 0.86f, 0.62f);
  } else if (prof == 9) {
    c = pow(max(c, 0.0f), float3(1.18f)) * 1.04f;
    c = mix(float3(l * 1.02f), c, 0.92f);
  }
  return c;
}

float3 daltonize(float3 c, int mode) {
  float3x3 rgb2lms = float3x3(float3(17.8824f, 43.5161f, 4.11935f), float3(3.45565f, 27.1554f, 3.86714f),
                              float3(0.0299566f, 0.184309f, 1.46709f));
  float3x3 lms2rgb = float3x3(float3(0.0809444479f, -0.130504409f, 0.116721066f),
                              float3(-0.0102485335f, 0.0540193266f, -0.113614708f),
                              float3(-0.000365296938f, -0.00412161469f, 0.693511405f));
  float3 lms = c * rgb2lms;
  float3 s;
  if (mode == 1) s = float3(2.02344f * lms.y - 2.52581f * lms.z, lms.y, lms.z);
  else if (mode == 2) s = float3(lms.x, 0.494207f * lms.x + 1.24827f * lms.z, lms.z);
  else s = float3(lms.x, lms.y, -0.395913f * lms.x + 0.801109f * lms.y);
  float3 sim = s * lms2rgb;
  float3 err = c - sim;
  float3 corr = float3(0.0f, err.r * 0.7f + err.g, err.r * 0.7f + err.b);
  return clamp(c + corr, 0.0f, 1.0f);
}

float3 applyColor(float3 c, constant GameU& u) {
  c = pow(max(c, 0.0f), float3(1.0f / u.f1.w));
  c = (c - 0.5f) * u.f1.y + 0.5f;
  c *= u.f1.x;
  float l = dot(c, float3(0.299f, 0.587f, 0.114f));
  c = mix(float3(l), c, u.f1.z);
  c = applyProfile(clamp(c, 0.0f, 1.0f), u);
  float tmp = u.f3.z;
  if (tmp > 0.0f) c *= mix(float3(1.0f), float3(1.0f, 0.9f, 0.74f), tmp);
  else if (tmp < 0.0f) c *= mix(float3(1.0f), float3(0.82f, 0.92f, 1.0f), -tmp);
  int cb = int(u.f2.y);
  if (cb > 0) c = daltonize(c, cb);
  return clamp(c, 0.0f, 1.0f);
}

fragment float4 gameFS(GOut in [[stage_in]], texture2d<float> t [[texture(0)]], constant GameU& u [[buffer(0)]]) {
  float2 uv = in.uv;
  if (int(u.f0.w) == 1) {
    // a blurred, darkened backdrop for the sides
    float3 acc = float3(0.0f);
    float wsum = 0.0f;
    for (int y = -3; y <= 3; y++)
      for (int x = -3; x <= 3; x++) {
        float w = exp(-float(x * x + y * y) / 8.0f);
        acc += texLinear(t, uv + float2(float(x) * 6.0f, float(y) * 4.0f) * u.texSize.zw, u) * w;
        wsum += w;
      }
    float3 c = applyColor(acc / wsum, u) * 0.32f;
    float vign = 1.0f - 0.5f * pow(abs(uv.x * 2.0f - 1.0f), 3.0f);
    return float4(c * vign * u.f3.x, 1.0f);
  }
  float curve = u.f0.z;
  if (curve > 0.0f) {
    float2 cc = uv * 2.0f - 1.0f;
    cc *= 1.0f + (cc.yx * cc.yx) * curve * 0.12f;
    uv = cc * 0.5f + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) return float4(0.0f, 0.0f, 0.0f, 1.0f);
  }
  int filt = int(u.f0.x);
  float3 c;
  if (filt == 0) c = texNearest(t, uv, u);
  else if (filt == 1) c = texSharp(t, uv, u);
  else if (filt == 2) c = texLinear(t, uv, u);
  else if (filt == 3) c = crt(t, uv, in.pos.xy, false, u);
  else if (filt == 4) c = crt(t, uv, in.pos.xy, true, u);
  else c = lcd(t, uv, u);
  c = applyColor(c, u);
  if (curve > 0.0f) {
    float2 cc = uv * 2.0f - 1.0f;
    c *= clamp((1.0f - cc.x * cc.x) * (1.0f - cc.y * cc.y) * 12.0f, 0.0f, 1.0f);
  }
  return float4(c * u.f3.x, 1.0f);
}

// ---- the UI: batched quads ----
struct OVert {
  packed_float2 pos;
  packed_float2 uv;
  uint col;
  packed_float4 prm;
  float mode;
};
struct OOut {
  float4 pos [[position]];
  float2 uv;
  float4 col;
  float4 prm [[flat]];
  float mode [[flat]];
};

vertex OOut overlayVS(uint vid [[vertex_id]], const device OVert* v [[buffer(0)]],
                      constant float2& screen [[buffer(1)]]) {  // 2/W, 2/H
  OVert a = v[vid];
  OOut o;
  o.pos = float4(a.pos[0] * screen.x - 1.0f, 1.0f - a.pos[1] * screen.y, 0.0f, 1.0f);
  o.uv = float2(a.uv);
  o.col = unpack_unorm4x8_to_float(a.col);
  o.prm = float4(a.prm);
  o.mode = a.mode;
  return o;
}

fragment float4 overlayFS(OOut in [[stage_in]], texture2d<float> atlas [[texture(0)]],
                          texture2d<float> image [[texture(1)]], sampler sa [[sampler(0)]],
                          sampler si [[sampler(1)]]) {
  float a = 1.0f;
  if (in.mode > 3.5f) {  // a picture (BGRA: 0x00RRGGBB as it is; 5: its alpha too)
    float4 t = image.sample(si, in.uv);
    return float4(t.rgb, in.mode > 4.5f ? t.a * in.col.a : in.col.a);
  } else if (in.mode > 1.5f) {  // rounded rect SDF; uv = offset from the centre, prm = hw, hh, r, stroke
    float2 q = abs(in.uv) - in.prm.xy + in.prm.z;
    float d = length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f) - in.prm.z;
    a = clamp(0.5f - d, 0.0f, 1.0f);
    if (in.prm.w > 0.0f) a *= clamp(d + in.prm.w + 0.5f, 0.0f, 1.0f);
  } else if (in.mode > 0.5f) {  // glyph coverage
    a = atlas.sample(sa, in.uv).r;
  }
  return float4(in.col.rgb, in.col.a * a);
}
)";

struct GameU {
  float texSize[4], outSize[4], f0[4], f1[4], f2[4], f3[4];
};

struct OVert {
  float x, y, u, v;
  uint32_t col;
  float p[4];
  float mode;
};
static_assert(sizeof(OVert) == 40, "vertex layout");
const int kMaxQuads = 16384;  // per draw call (16-bit indices)

uint32_t rgba(ColorF c) {
  auto b = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
  return b(c.r) | b(c.g) << 8 | b(c.b) << 16 | b(c.a) << 24;
}

// The text's code points: ASCII, Latin-1 and the symbols the menus use.
const uint32_t kExtraCps[] = {0x2190, 0x2191, 0x2192, 0x2193, 0x25B6, 0x25C0, 0x2713, 0x2022, 0x266A, 0x2026,
                              0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2605, 0x24B6, 0x24B7, 0x2122,
                              0x00D7, 0x2715};
// Font sizes glyphs are made at (others drawn scaled from the nearest).
const float kSizes[] = {9, 11, 13, 15, 17, 19, 22, 25, 28, 32, 36, 41, 47, 54, 62, 71, 82, 94, 108, 124, 142};

// The title font (Luckiest Guy, in the bundle's Resources) at a size; the
// system's bold UI font where it isn't found.
CTFontRef titleFont(float px) {
  static CTFontDescriptorRef desc = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    std::string path = g_fontDir + "/LuckiestGuy-Regular.ttf";
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path.c_str(), (CFIndex)path.size(),
                                                           false);
    if (url) {
      CFArrayRef list = CTFontManagerCreateFontDescriptorsFromURL(url);
      if (list && CFArrayGetCount(list) > 0)
        desc = (CTFontDescriptorRef)CFRetain(CFArrayGetValueAtIndex(list, 0));
      if (list) CFRelease(list);
      CFRelease(url);
    }
  }
  if (desc) return CTFontCreateWithFontDescriptor(desc, px, nullptr);
  return CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, px, nullptr);
}

}  // namespace

// The frame being drawn, shared by the picture and the canvas: one render
// pass on the window's drawable, begun on first use.
struct Gpu {
  id<MTLDevice> dev;
  id<MTLCommandQueue> queue;
  CAMetalLayer* layer = nil;
  id<MTLTexture> dummy;  // (bound where a shader's texture isn't used)
  id<CAMetalDrawable> drawable;
  id<MTLCommandBuffer> cmd, last;
  id<MTLRenderCommandEncoder> enc;
  bool started = false;  // (this frame's drawable cleared already: later passes keep what's on it)

  // The render pass, begun if it isn't (cleared to black with `clear`).
  id<MTLRenderCommandEncoder> encoder(bool clear = false) {
    if (enc && !clear) return enc;
    endPass();
    if (!drawable) drawable = [layer nextDrawable];  // (waits for one free: the display's refresh)
    if (!drawable) return nil;
    if (!cmd) cmd = [queue commandBuffer];
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = drawable.texture;
    rp.colorAttachments[0].loadAction = clear || !started ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    started = true;
    enc = [cmd renderCommandEncoderWithDescriptor:rp];
    return enc;
  }
  void endPass() {
    if (enc) [enc endEncoding];
    enc = nil;
  }
  // Before the CPU writes a texture or buffer the last frame may still read.
  void waitIdle() {
    if (last) [last waitUntilCompleted];
    last = nil;
  }
};

struct Video::State : Gpu {
  SDL_MetalView view = nullptr;
  id<MTLRenderPipelineState> game;
  id<MTLTexture> tex;
  SlangChain slang;
  id<MTLTexture> slangOut;  // (the preset's picture, copied into place: its last pass clears all it draws on)
};

// ---------------------------------------------------------------------------
class MetalCanvas : public Canvas {
public:
  MetalCanvas(Gpu& g, unsigned epoch) : g_(g), epoch_(epoch) {}
  bool create(id<MTLLibrary> lib);
  void begin(int w, int h);
  void end() { flush(); }

  unsigned epoch() const override { return epoch_; }
  void fillRect(RectF r, ColorF c) override {
    uint32_t k = rgba(c);
    quad(r.left, r.top, r.right, r.bottom, k, k, 0, 0, 0, 0, nullptr, 0);
  }
  void strokeRect(RectF r, float width, ColorF c) override { roundRect(grow(r, width * 0.5f), 0, width, c); }
  void fillRound(RectF r, float radius, ColorF c) override { roundRect(r, radius, 0, c); }
  void strokeRound(RectF r, float radius, float width, ColorF c) override {
    roundRect(grow(r, width * 0.5f), radius + width * 0.5f, width, c);
  }
  void strokeEllipse(float cx, float cy, float rx, float ry, float width, ColorF c) override {
    strokeRound(rectF(cx - rx, cy - ry, cx + rx, cy + ry), std::min(rx, ry), width, c);
  }
  void fillGradient(RectF r, ColorF top, ColorF bottom) override {
    quad(r.left, r.top, r.right, r.bottom, rgba(top), rgba(bottom), 0, 0, 0, 0, nullptr, 0);
  }
  void fillPolygon(const PointF* pts, int n, ColorF c) override {
    if (n < 3) return;
    float mx = 0, my = 0;
    for (int i = 0; i < n; i++) mx += pts[i].x, my += pts[i].y;
    mx /= n;
    my /= n;
    uint32_t k = rgba(c);
    for (int i = 0; i < n; i++) {
      const PointF &a = pts[i], &b = pts[(i + 1) % n];
      tri(mx, my, a.x, a.y, b.x, b.y, k);
    }
  }
  void strokePolygon(const PointF* pts, int n, float width, ColorF c) override {
    uint32_t k = rgba(c);
    for (int i = 0; i < n; i++) line(pts[i], pts[(i + 1) % n], width, k);
  }
  int makeShape(const std::vector<PointF>& pts, const std::vector<int>& counts) override;
  void fillShape(int shape, ColorF c, float scale, float tx, float ty) override;
  void freeShape(int shape) override {
    if (shape >= 1 && shape <= (int)shapes_.size()) shapes_[shape - 1].clear();
  }
  void text(const std::wstring& s, RectF r, float size, int weight, ColorF c, TextAlign align,
            ParaAlign para) override {
    drawText(s, r, size, weight, false, c, align, para);
  }
  float measure(const std::wstring& s, float size, int weight) override { return measureText(s, size, weight, false); }
  void titleText(const std::wstring& s, RectF r, float size, ColorF c, TextAlign align, ParaAlign para) override {
    drawText(s, r, size, kWeightNormal, true, c, align, para);
  }
  float measureTitle(const std::wstring& s, float size) override { return measureText(s, size, kWeightNormal, true); }
  void pushClip(RectF r) override;
  void popClip() override;
  int makeImage(int w, int h, bool alpha) override;
  void updateImage(int image, const uint32_t* px, int stridePixels) override;
  void drawImage(int image, RectF dst, float opacity, bool smooth) override;
  void freeImage(int image) override;

private:
  Gpu& g_;
  unsigned epoch_;
  int w_ = 0, h_ = 0;
  id<MTLRenderPipelineState> pipe_;
  id<MTLBuffer> ib_;
  id<MTLBuffer> vb_;  // this frame's vertices, one flush after another
  size_t vbUsed_ = 0;
  id<MTLSamplerState> point_, linear_;
  std::vector<OVert> verts_;
  id<MTLTexture> boundAtlas_, boundImage_;
  bool imageSmooth_ = false;
  std::vector<RectF> clips_;
  std::vector<std::vector<float>> shapes_;  // triangles: x, y per corner
  struct Image {
    id<MTLTexture> tex;
    int w = 0, h = 0;
    bool alpha = false;
  };
  std::vector<Image> images_;
  struct Glyph {
    float adv;
    int ox, oy, w, h;
    float u0, v0, u1, v1;
  };
  struct Font {
    id<MTLTexture> tex;
    int ascent = 0, lineH = 0;
    std::map<uint32_t, Glyph> glyphs;
    bool ok = false, tried = false;
  };
  std::map<int, Font> fonts_;  // by size index * 4 + title * 2 + bold

  static RectF grow(RectF r, float d) { return rectF(r.left - d, r.top - d, r.right + d, r.bottom + d); }
  void flush();
  void useAtlas(id<MTLTexture> tex) {
    if (tex != boundAtlas_) {
      flush();
      boundAtlas_ = tex;
    }
  }
  void push4(const OVert v[4]);
  void quad(float x0, float y0, float x1, float y1, uint32_t ctop, uint32_t cbot, float u0, float v0, float u1,
            float v1, const float* prm, float mode);
  void tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c);
  void line(PointF a, PointF b, float width, uint32_t c);
  void roundRect(RectF r, float radius, float stroke, ColorF c);
  Font& font(float size, int weight, bool title, float* scale);
  bool buildFont(Font& f, float px, bool bold, bool title);
  void drawText(const std::wstring& s, RectF r, float size, int weight, bool title, ColorF c, TextAlign align,
                ParaAlign para);
  float measureText(const std::wstring& s, float size, int weight, bool title);
};

bool MetalCanvas::create(id<MTLLibrary> lib) {
  MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
  pd.vertexFunction = [lib newFunctionWithName:@"overlayVS"];
  pd.fragmentFunction = [lib newFunctionWithName:@"overlayFS"];
  MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[0];
  ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
  ca.blendingEnabled = YES;
  ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
  ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
  ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  NSError* err = nil;
  pipe_ = [g_.dev newRenderPipelineStateWithDescriptor:pd error:&err];
  if (!pipe_) {
    logError(err.localizedDescription.UTF8String);
    return false;
  }
  std::vector<uint16_t> idx(kMaxQuads * 6);
  for (int q = 0; q < kMaxQuads; q++) {
    uint16_t b = (uint16_t)(q * 4);
    uint16_t* o = &idx[q * 6];
    o[0] = b;
    o[1] = (uint16_t)(b + 1);
    o[2] = (uint16_t)(b + 2);
    o[3] = (uint16_t)(b + 2);
    o[4] = (uint16_t)(b + 1);
    o[5] = (uint16_t)(b + 3);
  }
  ib_ = [g_.dev newBufferWithBytes:idx.data() length:idx.size() * 2 options:MTLResourceStorageModeShared];
  MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
  sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
  point_ = [g_.dev newSamplerStateWithDescriptor:sd];
  sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
  linear_ = [g_.dev newSamplerStateWithDescriptor:sd];
  return pipe_ && ib_;
}

void MetalCanvas::begin(int w, int h) {
  w_ = w;
  h_ = h;
  verts_.clear();
  clips_.clear();
  boundAtlas_ = nil;
  boundImage_ = nil;
  g_.waitIdle();  // (the vertex buffer is used again from the start)
  vbUsed_ = 0;
}

void MetalCanvas::flush() {
  if (verts_.empty() || !pipe_) {
    verts_.clear();
    return;
  }
  id<MTLRenderCommandEncoder> enc = g_.encoder();
  if (!enc) {
    verts_.clear();
    return;
  }
  // (clipped to nothing: not drawn; Metal takes no empty scissor rectangle)
  MTLScissorRect sc = {0, 0, (NSUInteger)w_, (NSUInteger)h_};
  if (!clips_.empty()) {
    const RectF& r = clips_.back();
    int x0 = (int)std::floor(std::max(0.0f, r.left)), y0 = (int)std::floor(std::max(0.0f, r.top));
    int x1 = (int)std::ceil(std::min((float)w_, r.right)), y1 = (int)std::ceil(std::min((float)h_, r.bottom));
    if (x1 <= x0 || y1 <= y0) {
      verts_.clear();
      return;
    }
    sc = {(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)};
  }
  const size_t bytes = verts_.size() * sizeof(OVert);
  if (!vb_ || vbUsed_ + bytes > vb_.length) {
    // (a bigger one; the old stays alive while the command buffer uses it)
    size_t n = std::max<size_t>(vb_ ? vb_.length * 2 : 1u << 20, bytes);
    vb_ = [g_.dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    vbUsed_ = 0;
  }
  std::memcpy((uint8_t*)vb_.contents + vbUsed_, verts_.data(), bytes);
  [enc setRenderPipelineState:pipe_];
  [enc setViewport:(MTLViewport){0, 0, (double)w_, (double)h_, 0, 1}];
  [enc setScissorRect:sc];
  [enc setVertexBuffer:vb_ offset:vbUsed_ atIndex:0];
  float screen[2] = {2.0f / (float)w_, 2.0f / (float)h_};
  [enc setVertexBytes:screen length:sizeof screen atIndex:1];
  [enc setFragmentTexture:boundAtlas_ ? boundAtlas_ : g_.dummy atIndex:0];
  [enc setFragmentTexture:boundImage_ ? boundImage_ : g_.dummy atIndex:1];
  [enc setFragmentSamplerState:linear_ atIndex:0];
  [enc setFragmentSamplerState:imageSmooth_ ? linear_ : point_ atIndex:1];
  [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                  indexCount:verts_.size() / 4 * 6
                   indexType:MTLIndexTypeUInt16
                 indexBuffer:ib_
           indexBufferOffset:0];
  vbUsed_ = (vbUsed_ + bytes + 255) & ~(size_t)255;
  verts_.clear();
}

void MetalCanvas::push4(const OVert v[4]) {
  if (verts_.size() >= (size_t)kMaxQuads * 4) flush();
  verts_.insert(verts_.end(), v, v + 4);
}

void MetalCanvas::quad(float x0, float y0, float x1, float y1, uint32_t ctop, uint32_t cbot, float u0, float v0,
                       float u1, float v1, const float* prm, float mode) {
  if (x1 <= x0 || y1 <= y0) return;
  float p[4] = {0, 0, 0, 0};
  if (prm) std::memcpy(p, prm, sizeof(p));
  OVert v[4] = {
      {x0, y0, u0, v0, ctop, {p[0], p[1], p[2], p[3]}, mode},
      {x1, y0, u1, v0, ctop, {p[0], p[1], p[2], p[3]}, mode},
      {x0, y1, u0, v1, cbot, {p[0], p[1], p[2], p[3]}, mode},
      {x1, y1, u1, v1, cbot, {p[0], p[1], p[2], p[3]}, mode},
  };
  push4(v);
}

// (a triangle as a quad whose last corner repeats the third: its second triangle is empty)
void MetalCanvas::tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c) {
  OVert v[4] = {
      {x0, y0, 0, 0, c, {0, 0, 0, 0}, 0},
      {x1, y1, 0, 0, c, {0, 0, 0, 0}, 0},
      {x2, y2, 0, 0, c, {0, 0, 0, 0}, 0},
      {x2, y2, 0, 0, c, {0, 0, 0, 0}, 0},
  };
  push4(v);
}

void MetalCanvas::line(PointF a, PointF b, float width, uint32_t c) {
  float dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
  if (len <= 0) return;
  float nx = -dy / len * width * 0.5f, ny = dx / len * width * 0.5f;
  OVert v[4] = {
      {a.x + nx, a.y + ny, 0, 0, c, {0, 0, 0, 0}, 0},
      {b.x + nx, b.y + ny, 0, 0, c, {0, 0, 0, 0}, 0},
      {a.x - nx, a.y - ny, 0, 0, c, {0, 0, 0, 0}, 0},
      {b.x - nx, b.y - ny, 0, 0, c, {0, 0, 0, 0}, 0},
  };
  push4(v);
}

void MetalCanvas::roundRect(RectF r, float radius, float stroke, ColorF c) {
  float w = r.right - r.left, h = r.bottom - r.top;
  if (w <= 0 || h <= 0) return;
  float hw = w * 0.5f, hh = h * 0.5f;
  float prm[4] = {hw, hh, std::max(0.0f, std::min(radius, std::min(hw, hh))), stroke};
  float cx = r.left + hw, cy = r.top + hh;
  float x0 = r.left - 1, y0 = r.top - 1, x1 = r.right + 1, y1 = r.bottom + 1;
  uint32_t k = rgba(c);
  quad(x0, y0, x1, y1, k, k, x0 - cx, y0 - cy, x1 - cx, y1 - cy, prm, 2.0f);
}

int MetalCanvas::makeShape(const std::vector<PointF>& pts, const std::vector<int>& counts) {
  std::vector<float> t;
  size_t at = 0;
  for (int n : counts) {
    if (n >= 3) {
      float mx = 0, my = 0;
      for (int i = 0; i < n; i++) mx += pts[at + i].x, my += pts[at + i].y;
      mx /= n;
      my /= n;
      for (int i = 0; i < n; i++) {
        const PointF &a = pts[at + i], &b = pts[at + (i + 1) % n];
        t.insert(t.end(), {mx, my, a.x, a.y, b.x, b.y});
      }
    }
    at += n;
  }
  for (size_t i = 0; i < shapes_.size(); i++)
    if (shapes_[i].empty()) {
      shapes_[i] = std::move(t);
      return (int)i + 1;
    }
  shapes_.push_back(std::move(t));
  return (int)shapes_.size();
}

void MetalCanvas::fillShape(int shape, ColorF c, float scale, float tx, float ty) {
  if (shape < 1 || shape > (int)shapes_.size()) return;
  const std::vector<float>& t = shapes_[shape - 1];
  uint32_t k = rgba(c);
  for (size_t i = 0; i + 6 <= t.size(); i += 6)
    tri(t[i] * scale + tx, t[i + 1] * scale + ty, t[i + 2] * scale + tx, t[i + 3] * scale + ty,
        t[i + 4] * scale + tx, t[i + 5] * scale + ty, k);
}

// ---- clipping ----------------------------------------------------------------
void MetalCanvas::pushClip(RectF r) {
  flush();
  if (!clips_.empty()) {
    const RectF& p = clips_.back();
    r = rectF(std::max(r.left, p.left), std::max(r.top, p.top), std::min(r.right, p.right),
              std::min(r.bottom, p.bottom));
  }
  clips_.push_back(r);
}

void MetalCanvas::popClip() {
  flush();
  if (!clips_.empty()) clips_.pop_back();
}

// ---- pictures ----------------------------------------------------------------
// (0x00RRGGBB / 0xAARRGGBB in memory is B G R A: the BGRA format as it is)
int MetalCanvas::makeImage(int w, int h, bool alpha) {
  Image im;
  im.w = w;
  im.h = h;
  im.alpha = alpha;
  MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                width:(NSUInteger)w
                                                                               height:(NSUInteger)h
                                                                            mipmapped:NO];
  im.tex = [g_.dev newTextureWithDescriptor:td];
  for (size_t i = 0; i < images_.size(); i++)
    if (!images_[i].tex) {
      images_[i] = im;
      return (int)i + 1;
    }
  images_.push_back(im);
  return (int)images_.size();
}

void MetalCanvas::updateImage(int image, const uint32_t* px, int stridePixels) {
  if (image < 1 || image > (int)images_.size() || !images_[image - 1].tex) return;
  const Image& im = images_[image - 1];
  g_.waitIdle();
  [im.tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)im.w, (NSUInteger)im.h)
            mipmapLevel:0
              withBytes:px
            bytesPerRow:(NSUInteger)stridePixels * 4];
}

void MetalCanvas::drawImage(int image, RectF dst, float opacity, bool smooth) {
  if (image < 1 || image > (int)images_.size() || !images_[image - 1].tex) return;
  flush();
  boundImage_ = images_[image - 1].tex;
  imageSmooth_ = smooth;
  uint32_t k = rgba(colorF(1, 1, 1, opacity));
  quad(dst.left, dst.top, dst.right, dst.bottom, k, k, 0, 0, 1, 1, nullptr, images_[image - 1].alpha ? 5.0f : 4.0f);
  flush();
  boundImage_ = nil;
}

void MetalCanvas::freeImage(int image) {
  if (image < 1 || image > (int)images_.size() || !images_[image - 1].tex) return;
  images_[image - 1] = Image();
}

// ---- text ----------------------------------------------------------------------
// Glyphs from Core Text (the system's UI font; the title's Luckiest Guy), in
// the same form as Android's and Linux's: the font's ascent and line height,
// then per code point the advance x 64, the bitmap's left and top (from the
// pen and the baseline), its size; then the 8-bit coverage bitmaps one after
// another. A code point the font lacks comes from the font Core Text falls
// back to for it.
bool MetalCanvas::buildFont(Font& f, float px, bool bold, bool title) {
  std::vector<uint32_t> cps;
  for (uint32_t c = 0x20; c < 0x7F; c++) cps.push_back(c);
  for (uint32_t c = 0xA1; c < 0x100; c++)
    if (c != 0xAD) cps.push_back(c);
  for (uint32_t c : kExtraCps) cps.push_back(c);
  CTFontRef font = title ? titleFont(px)
                         : CTFontCreateUIFontForLanguage(bold ? kCTFontUIFontEmphasizedSystem : kCTFontUIFontSystem,
                                                         px, nullptr);
  if (!font) return false;
  std::vector<int> meta(2 + cps.size() * 5, 0);
  std::vector<uint8_t> bits;
  meta[0] = (int)std::ceil(CTFontGetAscent(font));
  meta[1] = (int)std::ceil(CTFontGetAscent(font) + CTFontGetDescent(font) + CTFontGetLeading(font));
  CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
  std::vector<uint8_t> buf;
  for (size_t i = 0; i < cps.size(); i++) {
    int* m = &meta[2 + i * 5];
    UniChar ch = (UniChar)cps[i];
    CGGlyph glyph = 0;
    CTFontRef use = (CTFontRef)CFRetain(font);
    if (!CTFontGetGlyphsForCharacters(use, &ch, &glyph, 1) || !glyph) {
      CFRelease(use);
      CFStringRef str = CFStringCreateWithCharacters(nullptr, &ch, 1);
      use = CTFontCreateForString(font, str, CFRangeMake(0, 1));
      CFRelease(str);
      if (!use || !CTFontGetGlyphsForCharacters(use, &ch, &glyph, 1) || !glyph) {
        if (use) CFRelease(use);
        m[0] = -1;
        continue;
      }
    }
    CGSize adv;
    CTFontGetAdvancesForGlyphs(use, kCTFontOrientationHorizontal, &glyph, &adv, 1);
    CGRect bb = CTFontGetBoundingRectsForGlyphs(use, kCTFontOrientationHorizontal, &glyph, nullptr, 1);
    m[0] = (int)std::lround(adv.width * 64);
    if (cps[i] == ' ' || CGRectIsEmpty(bb)) {
      CFRelease(use);
      continue;
    }
    // (a pixel of room for the filtering each side, as on Android)
    const int x0 = (int)std::floor(CGRectGetMinX(bb)) - 1, y0 = (int)std::floor(CGRectGetMinY(bb)) - 1;
    const int x1 = (int)std::ceil(CGRectGetMaxX(bb)) + 1, y1 = (int)std::ceil(CGRectGetMaxY(bb)) + 1;
    const int w = x1 - x0, h = y1 - y0;
    buf.assign((size_t)w * h, 0);
    CGContextRef ctx = CGBitmapContextCreate(buf.data(), (size_t)w, (size_t)h, 8, (size_t)w, gray, kCGImageAlphaNone);
    if (ctx) {
      CGContextSetGrayFillColor(ctx, 1, 1);
      CGContextSetAllowsAntialiasing(ctx, true);
      CGContextSetShouldSmoothFonts(ctx, false);
      CGPoint pos = CGPointMake(-x0, -y0);
      CTFontDrawGlyphs(use, &glyph, &pos, 1, ctx);
      CGContextRelease(ctx);
      m[1] = x0;
      m[2] = -y1;  // (the bitmap's first row is its top)
      m[3] = w;
      m[4] = h;
      bits.insert(bits.end(), buf.begin(), buf.end());
    }
    CFRelease(use);
  }
  CGColorSpaceRelease(gray);
  CFRelease(font);
  f.ascent = meta[0];
  f.lineH = meta[1];
  // shelf-pack the bitmaps (each has its own margin) into one R8 texture
  struct B {
    uint32_t cp;
    Glyph g;
    size_t off;
  };
  std::vector<B> list;
  size_t off = 0;
  for (size_t i = 0; i < cps.size(); i++) {
    const int* k = &meta[2 + i * 5];
    if (k[0] < 0) continue;
    Glyph g = {k[0] / 64.0f, k[1], k[2], k[3], k[4], 0, 0, 0, 0};
    if (g.w > 0 && off + (size_t)g.w * g.h <= bits.size()) {
      list.push_back({cps[i], g, off});
      off += (size_t)g.w * g.h;
    } else {
      g.w = g.h = 0;
      list.push_back({cps[i], g, 0});
    }
  }
  int aw = 256;
  long long area = 0;
  for (auto& b : list) area += (long long)(b.g.w + 1) * (b.g.h + 1);
  while ((long long)aw * aw < area * 3 / 2 && aw < 4096) aw *= 2;
  int x = 0, y = 0, row = 0;
  std::vector<std::pair<int, int>> at(list.size());
  for (size_t i = 0; i < list.size(); i++) {
    const Glyph& g = list[i].g;
    if (x + g.w + 1 > aw) {
      x = 0;
      y += row;
      row = 0;
    }
    at[i] = {x, y};
    x += g.w + 1;
    row = std::max(row, g.h + 1);
  }
  int ah = std::max(4, (y + row + 3) & ~3);
  std::vector<uint8_t> atlas((size_t)aw * ah, 0);
  for (size_t i = 0; i < list.size(); i++) {
    Glyph g = list[i].g;
    if (g.w > 0 && at[i].second + g.h <= ah) {
      for (int r = 0; r < g.h; r++)
        std::memcpy(&atlas[(size_t)(at[i].second + r) * aw + at[i].first], &bits[list[i].off + (size_t)r * g.w],
                    (size_t)g.w);
      g.u0 = (float)at[i].first / aw;
      g.v0 = (float)at[i].second / ah;
      g.u1 = (float)(at[i].first + g.w) / aw;
      g.v1 = (float)(at[i].second + g.h) / ah;
    }
    f.glyphs[list[i].cp] = g;
  }
  MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                width:(NSUInteger)aw
                                                                               height:(NSUInteger)ah
                                                                            mipmapped:NO];
  f.tex = [g_.dev newTextureWithDescriptor:td];
  if (!f.tex) return false;
  [f.tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)aw, (NSUInteger)ah)
           mipmapLevel:0
             withBytes:atlas.data()
           bytesPerRow:(NSUInteger)aw];
  return true;
}

MetalCanvas::Font& MetalCanvas::font(float size, int weight, bool title, float* scale) {
  const int n = (int)(sizeof(kSizes) / sizeof(kSizes[0]));
  int i = 0;
  while (i < n - 1 && kSizes[i] < size * 0.96f) i++;
  *scale = size / kSizes[i];
  const bool bold = weight >= kWeightSemiBold;
  Font& f = fonts_[i * 4 + (title ? 2 : 0) + (bold ? 1 : 0)];
  if (!f.tried) {
    f.tried = true;
    f.ok = buildFont(f, kSizes[i], bold, title);
  }
  return f;
}

float MetalCanvas::measureText(const std::wstring& s, float size, int weight, bool title) {
  if (s.empty() || size <= 0) return 0;
  float scale;
  Font& f = font(size, weight, title, &scale);
  if (!f.ok) return 0;
  float w = 0;
  for (wchar_t wc : s) {
    auto it = f.glyphs.find((uint32_t)wc);
    if (it == f.glyphs.end()) it = f.glyphs.find('?');
    if (it != f.glyphs.end()) w += it->second.adv;
  }
  return w * scale;
}

void MetalCanvas::drawText(const std::wstring& s, RectF r, float size, int weight, bool title, ColorF c,
                           TextAlign align, ParaAlign para) {
  if (s.empty() || size <= 0 || r.right <= r.left || r.bottom <= r.top) return;
  float scale;
  Font& f = font(size, weight, title, &scale);
  if (!f.ok) return;
  useAtlas(f.tex);
  const float lw = measureText(s, size, weight, title), lh = f.lineH * scale;
  float pen = align == kAlignCenter ? (r.left + r.right - lw) * 0.5f : align == kAlignTrailing ? r.right - lw : r.left;
  float top = para == kParaCenter ? (r.top + r.bottom - lh) * 0.5f : para == kParaFar ? r.bottom - lh : r.top;
  const float base = std::floor(top + f.ascent * scale + 0.5f);
  const uint32_t k = rgba(c);
  for (wchar_t wc : s) {
    auto it = f.glyphs.find((uint32_t)wc);
    if (it == f.glyphs.end()) it = f.glyphs.find('?');
    if (it == f.glyphs.end()) continue;
    const Glyph& g = it->second;
    if (g.w > 0) {
      float x0 = pen + g.ox * scale, y0 = base + g.oy * scale, x1 = x0 + g.w * scale, y1 = y0 + g.h * scale;
      float u0 = g.u0, v0 = g.v0, u1 = g.u1, v1 = g.v1;
      // (clipped to the rectangle, as DirectWrite's text is)
      if (x0 < r.left) u0 += (u1 - u0) * (r.left - x0) / (x1 - x0), x0 = r.left;
      if (x1 > r.right) u1 -= (u1 - u0) * (x1 - r.right) / (x1 - x0), x1 = r.right;
      if (y0 < r.top) v0 += (v1 - v0) * (r.top - y0) / (y1 - y0), y0 = r.top;
      if (y1 > r.bottom) v1 -= (v1 - v0) * (y1 - r.bottom) / (y1 - y0), y1 = r.bottom;
      if (x1 > x0 && y1 > y0) quad(x0, y0, x1, y1, k, k, u0, v0, u1, v1, nullptr, 1.0f);
    }
    pen += g.adv * scale;
  }
}

// ---------------------------------------------------------------------------
Video::Video() = default;
Video::~Video() { shutdown(); }

bool Video::init(SDL_Window* window) {
  shutdown();
  s_ = std::make_unique<State>();
  s_->dev = MTLCreateSystemDefaultDevice();
  if (!s_->dev) {
    logError("no Metal device");
    return false;
  }
  s_->view = SDL_Metal_CreateView(window);
  if (!s_->view) {
    logError(SDL_GetError());
    return false;
  }
  CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(s_->view);
  layer.device = s_->dev;
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  layer.framebufferOnly = NO;  // (capture reads it back)
  layer.displaySyncEnabled = YES;
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  layer.colorspace = srgb;
  CGColorSpaceRelease(srgb);
  s_->layer = layer;
  s_->queue = [s_->dev newCommandQueue];
  SDL_Metal_GetDrawableSize(window, &w_, &h_);
  layer.drawableSize = CGSizeMake(w_, h_);

  NSError* err = nil;
  id<MTLLibrary> lib = [s_->dev newLibraryWithSource:@(kShaders) options:nil error:&err];
  if (!lib) {
    logError(err.localizedDescription.UTF8String);
    return false;
  }
  MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
  pd.vertexFunction = [lib newFunctionWithName:@"gameVS"];
  pd.fragmentFunction = [lib newFunctionWithName:@"gameFS"];
  pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  s_->game = [s_->dev newRenderPipelineStateWithDescriptor:pd error:&err];
  if (!s_->game) {
    logError(err.localizedDescription.UTF8String);
    return false;
  }
  MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                width:1
                                                                               height:1
                                                                            mipmapped:NO];
  s_->dummy = [s_->dev newTextureWithDescriptor:td];
  const uint32_t black = 0;
  [s_->dummy replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&black bytesPerRow:4];
  static unsigned epochs = 0;
  canvas_ = std::make_unique<MetalCanvas>(*s_, ++epochs);
  return canvas_->create(lib);
}

void Video::shutdown() {
  if (!s_) return;
  s_->endPass();
  if (s_->cmd) [s_->cmd commit];
  s_->last = s_->cmd;
  s_->waitIdle();
  s_->slang.release();
  canvas_.reset();
  if (s_->view) SDL_Metal_DestroyView(s_->view);
  s_.reset();
  w_ = h_ = 0;
}

void Video::resize(int w, int h) {
  if (w <= 0 || h <= 0) return;
  w_ = w, h_ = h;
  if (s_ && s_->layer) s_->layer.drawableSize = CGSizeMake(w, h);
}

Canvas* Video::canvas() { return canvas_.get(); }

void Video::upload(const uint32_t* px, int w, int h, int stride) {
  if (!s_ || !px || w <= 0 || h <= 0) return;
  if (!s_->tex || (int)s_->tex.width != w || (int)s_->tex.height != h) {
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:(NSUInteger)w
                                                                                 height:(NSUInteger)h
                                                                              mipmapped:NO];
    s_->tex = [s_->dev newTextureWithDescriptor:td];
  } else {
    s_->waitIdle();
  }
  [s_->tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
             mipmapLevel:0
               withBytes:px
             bytesPerRow:(NSUInteger)stride * 4];
}

void Video::clear() {
  if (s_) s_->encoder(true);
}

void Video::pass(const VideoParams& p, int which, int x, int y, int w, int h) {
  id<MTLRenderCommandEncoder> enc = s_->encoder();
  if (!enc || w <= 0 || h <= 0) return;
  const float tw = (float)s_->tex.width, th = (float)s_->tex.height;
  GameU u = {{tw, th, 1.0f / tw, 1.0f / th},
             {(float)w, (float)h, 1.0f / std::max(w, 1), 1.0f / std::max(h, 1)},
             {(float)p.filter, p.scanlines, p.curve, (float)which},
             {p.brightness, p.contrast, p.saturation, p.gamma},
             {(float)p.colorProfile, (float)p.colorblind, (float)p.srcW, (float)p.srcH},
             {p.fade, (float)p.pillarbox, p.temperature, 0}};
  [enc setRenderPipelineState:s_->game];
  [enc setViewport:(MTLViewport){(double)x, (double)y, (double)w, (double)h, 0, 1}];
  [enc setScissorRect:(MTLScissorRect){0, 0, (NSUInteger)w_, (NSUInteger)h_}];
  [enc setFragmentTexture:s_->tex atIndex:0];
  [enc setFragmentBytes:&u length:sizeof u atIndex:0];
  [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

void Video::draw(const VideoParams& p) {
  clear();
  if (!s_ || !s_->tex) return;
  bool fullCover = p.dest.left <= 0 && p.dest.top <= 0 && p.dest.right >= w_ && p.dest.bottom >= h_;
  if (p.pillarbox == 1 && !fullCover) pass(p, 1, 0, 0, w_, h_);
  if (s_->slang.active() && s_->drawable) {
    // (a slang preset: its own passes from the game picture into the same
    // rectangle, on a command buffer of their own between the backdrop and
    // the UI; the colour settings are the preset's)
    s_->endPass();
    [s_->cmd commit];
    const int dw = p.dest.right - p.dest.left, dh = p.dest.bottom - p.dest.top;
    // (the part of the picture on the screen)
    const int sx = std::max(0, -p.dest.left), sy = std::max(0, -p.dest.top);
    const int cw = std::min(dw - sx, w_ - std::max(0, p.dest.left)), ch = std::min(dh - sy, h_ - std::max(0, p.dest.top));
    id<MTLCommandBuffer> cmd = [s_->queue commandBuffer];
    if (dw > 0 && dh > 0 && cw > 0 && ch > 0) {
      if (!s_->slangOut || (int)s_->slangOut.width != dw || (int)s_->slangOut.height != dh) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:(NSUInteger)dw
                                                                                     height:(NSUInteger)dh
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModePrivate;
        s_->slangOut = [s_->dev newTextureWithDescriptor:td];
      }
      s_->slang.draw(cmd, s_->tex, s_->slangOut, 0, 0, (float)dw, (float)dh, p.frame);
      id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
      [blit copyFromTexture:s_->slangOut
                sourceSlice:0
                sourceLevel:0
               sourceOrigin:MTLOriginMake((NSUInteger)sx, (NSUInteger)sy, 0)
                 sourceSize:MTLSizeMake((NSUInteger)cw, (NSUInteger)ch, 1)
                  toTexture:s_->drawable.texture
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake((NSUInteger)std::max(0, p.dest.left), (NSUInteger)std::max(0, p.dest.top), 0)];
      [blit endEncoding];
    }
    [cmd commit];
    s_->cmd = [s_->queue commandBuffer];
    return;
  }
  pass(p, 0, p.dest.left, p.dest.top, p.dest.right - p.dest.left, p.dest.bottom - p.dest.top);
}

bool Video::setSlang(const std::wstring& path, std::string* error) {
  return s_ && s_->slang.load(s_->queue, narrowUtf8(path), error);
}

bool Video::slangActive() const { return s_ && s_->slang.active(); }

bool Video::slangAvailable() { return s_ && s_->slang.available(); }

void Video::beginUi() {
  if (canvas_) canvas_->begin(w_, h_);
}

void Video::endUi() {
  if (canvas_) canvas_->end();
}

void Video::present() {
  if (!s_) return;
  s_->endPass();
  if (s_->cmd) {
    if (s_->drawable) [s_->cmd presentDrawable:s_->drawable];
    [s_->cmd commit];
    s_->last = s_->cmd;
  }
  s_->cmd = nil;
  s_->drawable = nil;
  s_->started = false;
}

bool Video::capture(std::vector<uint32_t>& out, int* w, int* h) {
  if (!s_ || !s_->encoder()) return false;
  s_->endPass();
  id<MTLTexture> t = s_->drawable.texture;
  const NSUInteger tw = t.width, th = t.height;
  id<MTLBuffer> buf = [s_->dev newBufferWithLength:tw * th * 4 options:MTLResourceStorageModeShared];
  id<MTLBlitCommandEncoder> blit = [s_->cmd blitCommandEncoder];
  [blit copyFromTexture:t
                   sourceSlice:0
                   sourceLevel:0
                  sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(tw, th, 1)
                      toBuffer:buf
             destinationOffset:0
        destinationBytesPerRow:tw * 4
      destinationBytesPerImage:tw * th * 4];
  [blit endEncoding];
  [s_->cmd commit];
  [s_->cmd waitUntilCompleted];
  s_->cmd = [s_->queue commandBuffer];  // (the drawable is presented with the next one)
  const uint32_t* px = (const uint32_t*)buf.contents;  // (B G R A: 0xAARRGGBB)
  out.resize((size_t)tw * th);
  for (size_t i = 0; i < out.size(); i++) out[i] = px[i] & 0xFFFFFF;
  *w = (int)tw;
  *h = (int)th;
  return true;
}
