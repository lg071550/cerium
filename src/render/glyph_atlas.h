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
  // Init from TTF bytes held by the caller (copied internally). Preferred
  // entry point — no filesystem dependency.
  bool init(WGPUDevice device, const uint8_t* ttfBytes, size_t size, float fontSizeLogical,
            float dpr);
  // Convenience: reads the file and delegates to the bytes overload.
  bool init(WGPUDevice device, const char* ttfPath, float fontSizeLogical, float dpr);
  void setDpr(float dpr); // rebakes everything when the ratio changes
  void setFontSize(float fontSizeLogical); // rebakes at a new UI scale

  const Glyph& glyph(uint32_t codepoint); // bakes on demand
  float measure(const char* utf8);        // bakes on demand
  float lineHeight() const { return m_lineH; }
  float ascent() const { return m_ascent; }
  float descent() const { return m_descent; }

  WGPUTextureView textureView() const { return m_view; }
  WGPUSampler sampler() const { return m_sampler; }
  float dpr() const { return m_dpr; }
  // bumped whenever the atlas texture is recreated — the renderer re-binds
  // the text batch's texture view when this changes
  uint32_t generation() const { return m_generation; }

  void flush(WGPUQueue queue);

  int width() const { return m_W; }   // atlas texture width
  int height() const { return m_H; }  // atlas texture height (grows on overflow)

private:
  Glyph bake(uint32_t codepoint);
  const Glyph& fallback();    // the '?' glyph, baked once and shared
  void reallocateTexture();   // (re)create m_tex/m_view at the current m_W x m_H
  void growAtlas();           // double the atlas HEIGHT on shelf overflow

  WGPUDevice m_device = nullptr;
  WGPUTexture m_tex = nullptr;
  WGPUTextureView m_view = nullptr;
  WGPUSampler m_sampler = nullptr;
  uint32_t m_generation = 0;

  std::vector<unsigned char> m_ttf;
  void* m_font = nullptr; // stbtt_fontinfo, heap allocated
  std::vector<uint8_t> m_pixels;
  std::vector<uint8_t> m_scratch; // oversampled glyph raster before box-downsample

  int m_W = 1024, m_H = 1024; // current atlas size; only H grows (→ 4096 cap)

  float m_fontSize = 13.0f;
  float m_dpr = 1.0f;
  float m_scale = 0.0f; // stb scale for fontSize*dpr
  float m_lineH = 0.0f, m_ascent = 0.0f, m_descent = 0.0f;

  int m_penX = 0, m_penY = 0, m_shelfH = 0;
  Glyph m_ascii[128];
  Glyph m_fallback{}; // baked '?' shared by all unmapped codepoints
  std::unordered_map<uint32_t, Glyph> m_extra;

  int m_dirtyY0 = 0, m_dirtyY1 = 0;
};

// Advances past one UTF-8 codepoint and returns it. Shared by the atlas
// (measure) and the draw list (text emission). Feed strings are network data,
// so malformed input is handled defensively: an invalid lead byte (stray
// continuation, >= 0xF5) or a missing/truncated continuation consumes exactly
// ONE byte and yields U+FFFD (rendered via the '?' fallback glyph). The
// pointer always advances >= 1 byte and never reads past the first NUL.
inline uint32_t utf8_next(const char*& s) {
  unsigned char c = (unsigned char)*s;
  if (c < 0x80) { // ASCII (callers' loops stop at NUL before calling)
    ++s;
    return c;
  }
  int need = 0;
  uint32_t r = 0;
  if (c >= 0xC2 && c <= 0xDF) { // 2-byte (C0/C1 would be overlong)
    need = 1; r = c & 0x1F;
  } else if (c >= 0xE0 && c <= 0xEF) { // 3-byte
    need = 2; r = c & 0x0F;
  } else if (c >= 0xF0 && c <= 0xF4) { // 4-byte (>= F5 is out of range)
    need = 3; r = c & 0x07;
  } else {
    ++s;
    return 0xFFFD;
  }
  for (int i = 0; i < need; ++i) {
    unsigned char b = (unsigned char)s[i + 1];
    if ((b & 0xC0) != 0x80) { // NUL or non-continuation: bail on the lead byte
      ++s;
      return 0xFFFD;
    }
    r = (r << 6) | (b & 0x3F);
  }
  s += need + 1;
  return r;
}
