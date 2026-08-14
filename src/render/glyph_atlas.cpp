#include "glyph_atlas.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

// Hard cap on atlas height growth — past this, unmapped/overflowing glyphs
// reuse the '?' fallback cell instead of growing toward the 16384 texture
// dimension limit.
static const int kMaxAtlasH = 4096;

bool GlyphAtlas::init(WGPUDevice device, const char* ttfPath, float fontSizeLogical,
                      float dpr) {
  FILE* f = fopen(ttfPath, "rb");
  if (!f) {
    fprintf(stderr, "font: cannot open %s\n", ttfPath);
    return false;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> bytes((size_t)n);
  bool ok = fread(bytes.data(), 1, (size_t)n, f) == (size_t)n;
  fclose(f);
  if (!ok) {
    fprintf(stderr, "font: short read on %s\n", ttfPath);
    return false;
  }
  return init(device, bytes.data(), bytes.size(), fontSizeLogical, dpr);
}

bool GlyphAtlas::init(WGPUDevice device, const uint8_t* ttfBytes, size_t size,
                      float fontSizeLogical, float dpr) {
  m_device = device;
  m_fontSize = fontSizeLogical;

  m_ttf.assign(ttfBytes, ttfBytes + size);

  auto* info = new stbtt_fontinfo;
  if (!stbtt_InitFont(info, m_ttf.data(), stbtt_GetFontOffsetForIndex(m_ttf.data(), 0))) {
    fprintf(stderr, "font: stbtt_InitFont failed\n");
    delete info;
    return false;
  }
  m_font = info;

  m_pixels.assign((size_t)m_W * m_H, 0);
  reallocateTexture();

  WGPUSamplerDescriptor sd = {};
  sd.addressModeU = WGPUAddressMode_ClampToEdge;
  sd.addressModeV = WGPUAddressMode_ClampToEdge;
  sd.addressModeW = WGPUAddressMode_ClampToEdge;
  sd.magFilter = WGPUFilterMode_Linear;
  sd.minFilter = WGPUFilterMode_Linear;
  sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
  sd.maxAnisotropy = 1;
  m_sampler = wgpuDeviceCreateSampler(device, &sd);

  setDpr(dpr); // computes scale + line metrics, resets packer
  return true;
}

void GlyphAtlas::setDpr(float dpr) {
  m_dpr = dpr;
  auto* info = static_cast<stbtt_fontinfo*>(m_font);
  m_scale = stbtt_ScaleForPixelHeight(info, m_fontSize * dpr);

  int ascent = 0, descent = 0, lineGap = 0;
  stbtt_GetFontVMetrics(info, &ascent, &descent, &lineGap);
  m_ascent = ascent * m_scale / dpr;
  m_descent = -descent * m_scale / dpr; // positive distance below baseline
  m_lineH = (ascent - descent + lineGap) * m_scale / dpr;

  // rebake from scratch: clear pixels + caches, reset packer, full re-upload
  memset(m_pixels.data(), 0, m_pixels.size());
  for (auto& g : m_ascii) g = Glyph{};
  m_extra.clear();
  m_fallback = Glyph{};
  m_penX = m_penY = m_shelfH = 0;
  m_dirtyY0 = 0;
  m_dirtyY1 = m_H;
}

void GlyphAtlas::setFontSize(float fontSizeLogical) {
  m_fontSize = fontSizeLogical;
  setDpr(m_dpr); // rebake everything at the new size
}

const Glyph& GlyphAtlas::glyph(uint32_t cp) {
  if (cp < 128) {
    Glyph& slot = m_ascii[cp];
    if (!slot.valid) slot = bake(cp);
    return slot;
  }
  auto it = m_extra.find(cp);
  if (it == m_extra.end()) it = m_extra.emplace(cp, bake(cp)).first;
  return it->second;
}

const Glyph& GlyphAtlas::fallback() {
  if (!m_fallback.valid) m_fallback = bake('?');
  return m_fallback;
}

Glyph GlyphAtlas::bake(uint32_t cp) {
  auto* info = static_cast<stbtt_fontinfo*>(m_font);
  Glyph g{};

  int gi = stbtt_FindGlyphIndex(info, (int)cp);
  if (gi == 0 && cp != ' ' && cp != '?') return fallback(); // missing glyph fallback

  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  stbtt_GetGlyphBitmapBox(info, gi, m_scale, m_scale, &x0, &y0, &x1, &y1);
  int w = x1 - x0, h = y1 - y0;

  int advance = 0;
  stbtt_GetGlyphHMetrics(info, gi, &advance, nullptr);

  g.advance = advance * m_scale / m_dpr;
  g.bearingX = x0 / m_dpr;
  g.bearingY = y0 / m_dpr;
  g.w = w / m_dpr;
  g.h = h / m_dpr;
  g.valid = true;
  if (w <= 0 || h <= 0) return g; // whitespace — advance only

  // Shelf-pack the glyph. If the current shelf no longer fits, grow the atlas
  // (height only) instead of silently dropping the glyph; at the height cap,
  // reuse the fallback cell rather than growing toward the texture-dimension
  // limit.
  if (m_penX + w + 1 > m_W) {
    m_penX = 0;
    m_penY += m_shelfH + 1;
    m_shelfH = 0;
  }
  if (m_penY + h + 1 > m_H) {
    if (m_H >= kMaxAtlasH) {
      if (cp != '?') return fallback();
      Glyph e{}; // even '?' has no room — advance-only, renders nothing
      e.valid = true;
      e.advance = g.advance;
      return e;
    }
    growAtlas();
  }

  unsigned char* dst = m_pixels.data() + m_penX + (size_t)m_penY * m_W;

  // Oversample at low DPR for crisper glyph edges, then box-downsample into the
  // same w x h cell (layout, UV rect and dirty-row tracking are unchanged, so
  // upload math is unaffected). At dpr >= 3 the native raster is dense enough
  // that os collapses to 1x (no oversampling).
  int dprInt = (int)std::lround(m_dpr);
  if (dprInt < 1) dprInt = 1;
  int os = 4 / dprInt; // clamp(4 / round(dpr), 1, 4): 1x->4, 2x->2, >=3x->1
  if (os < 1) os = 1;

  if (os <= 1) {
    stbtt_MakeGlyphBitmap(info, dst, w, h, m_W, m_scale, m_scale, gi);
  } else {
    // Rasterize at os* resolution into a scratch buffer (stb's prefilter keeps
    // the result band-limited), then average os x os blocks back into the cell.
    // The +(os-1) guard keeps the prefilter from reading past the buffer.
    int sw = w * os + (os - 1);
    int sh = h * os + (os - 1);
    m_scratch.assign((size_t)sw * sh, 0);
    float sub_x = 0.0f, sub_y = 0.0f;
    stbtt_MakeGlyphBitmapSubpixelPrefilter(info, m_scratch.data(), sw, sh, sw,
                                           m_scale * (float)os,
                                           m_scale * (float)os, 0.0f, 0.0f, os, os,
                                           &sub_x, &sub_y, gi);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        int acc = 0;
        for (int dy = 0; dy < os; ++dy)
          for (int dx = 0; dx < os; ++dx)
            acc += m_scratch[((size_t)y * os + dy) * sw + (x * os + dx)];
        dst[(size_t)y * m_W + x] = (unsigned char)(acc / (os * os));
      }
    }
  }

  g.u0 = (float)m_penX / m_W;
  g.v0 = (float)m_penY / m_H;
  g.u1 = (float)(m_penX + w) / m_W;
  g.v1 = (float)(m_penY + h) / m_H;

  if (m_penY < m_dirtyY0) m_dirtyY0 = m_penY;
  if (m_penY + h > m_dirtyY1) m_dirtyY1 = m_penY + h;

  m_penX += w + 1;
  if (h > m_shelfH) m_shelfH = h;
  return g;
}

void GlyphAtlas::reallocateTexture() {
  if (m_view) {
    wgpuTextureViewRelease(m_view);
    m_view = nullptr;
  }
  if (m_tex) {
    wgpuTextureRelease(m_tex);
    m_tex = nullptr;
  }
  WGPUTextureDescriptor td = {};
  td.size = {(uint32_t)m_W, (uint32_t)m_H, 1};
  td.format = WGPUTextureFormat_R8Unorm;
  td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
  td.dimension = WGPUTextureDimension_2D;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  m_tex = wgpuDeviceCreateTexture(m_device, &td);
  m_view = wgpuTextureCreateView(m_tex, nullptr);
  ++m_generation; // texture identity changed — renderer must re-bind
}

void GlyphAtlas::growAtlas() {
  // Overflow is always vertical shelf exhaustion, so double the HEIGHT only
  // (W stays put, keeping WriteTexture bytesPerRow a multiple of 256). The
  // stride is unchanged, so a resize keeps the old pixels in place; recreate
  // the texture/view, rescale cached UVs and force a full re-upload so no
  // glyph is ever dropped.
  int oldH = m_H;
  int newH = m_H * 2 > kMaxAtlasH ? kMaxAtlasH : m_H * 2;

  m_pixels.resize((size_t)m_W * newH, 0);
  m_H = newH;
  reallocateTexture();

  // Glyph pixel positions are unchanged; only the atlas height grew, so
  // rescale every cached V back into [0,1] (U is unaffected).
  float sy = (float)oldH / newH;
  for (Glyph& gg : m_ascii) {
    gg.v0 *= sy; gg.v1 *= sy;
  }
  for (auto& kv : m_extra) {
    Glyph& gg = kv.second;
    gg.v0 *= sy; gg.v1 *= sy;
  }
  m_fallback.v0 *= sy;
  m_fallback.v1 *= sy;

  m_dirtyY0 = 0;
  m_dirtyY1 = newH;
}

float GlyphAtlas::measure(const char* utf8) {
  if (!utf8) return 0;
  float w = 0;
  const char* s = utf8;
  while (*s) w += glyph(utf8_next(s)).advance;
  return w;
}

void GlyphAtlas::flush(WGPUQueue queue) {
  if (m_dirtyY1 <= m_dirtyY0) return;
  uint32_t rows = (uint32_t)(m_dirtyY1 - m_dirtyY0);

  WGPUTexelCopyTextureInfo dst = {};
  dst.texture = m_tex;
  dst.mipLevel = 0;
  dst.origin = {0, (uint32_t)m_dirtyY0, 0};
  dst.aspect = WGPUTextureAspect_All;

  WGPUTexelCopyBufferLayout layout = {};
  layout.offset = 0;
  layout.bytesPerRow = m_W; // multiple of 256 (1024 -> 2048 -> ... on overflow)
  layout.rowsPerImage = rows;

  WGPUExtent3D extent = {(uint32_t)m_W, rows, 1};
  wgpuQueueWriteTexture(queue, &dst, m_pixels.data() + (size_t)m_dirtyY0 * m_W,
                        (size_t)rows * m_W, &layout, &extent);

  m_dirtyY0 = m_H;
  m_dirtyY1 = 0;
}
