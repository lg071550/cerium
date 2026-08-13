#pragma once

#include <webgpu/webgpu.h>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Baked glyph: metrics in logical px (baseline-relative), uv in atlas space.
struct Glyph {
  float bearingX = 0, bearingY = 0; // offset from pen position (pen sits on baseline)
  float w = 0, h = 0;
  float advance = 0;
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
  bool valid = false;
};

// Runtime-baking glyph atlas over stb_truetype. glyph() rasterizes on first
// use and marks dirty rows; flush() uploads the dirty row range (full width,
// so bytesPerRow stays a multiple of 256). Glyphs are rasterized at
// fontSize * dpr for crispness on high-DPI displays.
class GlyphAtlas {
public:
  bool init(WGPUDevice device, const char* ttfPath, float fontSizeLogical, float dpr);
  void setDpr(float dpr); // rebakes everything when the ratio changes

  const Glyph& glyph(uint32_t codepoint); // bakes on demand
  float measure(const char* utf8);        // bakes on demand
  float lineHeight() const { return m_lineH; }
  float ascent() const { return m_ascent; }
  float descent() const { return m_descent; }

  WGPUTextureView textureView() const { return m_view; }
  WGPUSampler sampler() const { return m_sampler; }

  void flush(WGPUQueue queue);

  static constexpr int W = 1024;
  static constexpr int H = 1024;

private:
  Glyph bake(uint32_t codepoint);

  WGPUDevice m_device = nullptr;
  WGPUTexture m_tex = nullptr;
  WGPUTextureView m_view = nullptr;
  WGPUSampler m_sampler = nullptr;

  std::vector<unsigned char> m_ttf;
  void* m_font = nullptr; // stbtt_fontinfo, heap allocated
  std::vector<uint8_t> m_pixels;

  float m_fontSize = 13.0f;
  float m_dpr = 1.0f;
  float m_scale = 0.0f; // stb scale for fontSize*dpr
  float m_lineH = 0.0f, m_ascent = 0.0f, m_descent = 0.0f;

  int m_penX = 0, m_penY = 0, m_shelfH = 0;
  Glyph m_ascii[128];
  std::unordered_map<uint32_t, Glyph> m_extra;

  int m_dirtyY0 = H, m_dirtyY1 = 0;
};

// Advances past one UTF-8 codepoint and returns it. Shared by the atlas
// (measure) and the draw list (text emission).
inline uint32_t utf8_next(const char*& s) {
  unsigned char c = (unsigned char)*s++;
  if (c < 0x80) return c;
  if ((c >> 5) == 0x6) {
    uint32_t r = c & 0x1F;
    r = (r << 6) | ((unsigned char)*s++ & 0x3F);
    return r;
  }
  if ((c >> 4) == 0xE) {
    uint32_t r = c & 0x0F;
    r = (r << 6) | ((unsigned char)*s++ & 0x3F);
    r = (r << 6) | ((unsigned char)*s++ & 0x3F);
    return r;
  }
  uint32_t r = c & 0x07;
  r = (r << 6) | ((unsigned char)*s++ & 0x3F);
  r = (r << 6) | ((unsigned char)*s++ & 0x3F);
  r = (r << 6) | ((unsigned char)*s++ & 0x3F);
  return r;
}
