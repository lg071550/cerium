#include "glyph_atlas.h"

#include <cstdio>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

bool GlyphAtlas::init(WGPUDevice device, const char* ttfPath, float fontSizeLogical,
                      float dpr) {
  m_device = device;
  m_fontSize = fontSizeLogical;

  FILE* f = fopen(ttfPath, "rb");
  if (!f) {
    fprintf(stderr, "font: cannot open %s\n", ttfPath);
    return false;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  m_ttf.resize((size_t)n);
  if (fread(m_ttf.data(), 1, (size_t)n, f) != (size_t)n) {
    fprintf(stderr, "font: short read on %s\n", ttfPath);
    fclose(f);
    return false;
  }
  fclose(f);

  auto* info = new stbtt_fontinfo;
  if (!stbtt_InitFont(info, m_ttf.data(), stbtt_GetFontOffsetForIndex(m_ttf.data(), 0))) {
    fprintf(stderr, "font: stbtt_InitFont failed\n");
    delete info;
    return false;
  }
  m_font = info;

  m_pixels.assign((size_t)W * H, 0);

  WGPUTextureDescriptor td = {};
  td.size = {W, H, 1};
  td.format = WGPUTextureFormat_R8Unorm;
  td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
  td.dimension = WGPUTextureDimension_2D;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  m_tex = wgpuDeviceCreateTexture(device, &td);
  m_view = wgpuTextureCreateView(m_tex, nullptr);

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
  m_penX = m_penY = m_shelfH = 0;
  m_dirtyY0 = 0;
  m_dirtyY1 = H;
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

Glyph GlyphAtlas::bake(uint32_t cp) {
  auto* info = static_cast<stbtt_fontinfo*>(m_font);
  Glyph g{};

  int gi = stbtt_FindGlyphIndex(info, (int)cp);
  if (gi == 0 && cp != ' ' && cp != '?') return bake('?'); // missing glyph fallback

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

  if (m_penX + w + 1 > W) {
    m_penX = 0;
    m_penY += m_shelfH + 1;
    m_shelfH = 0;
  }
  if (m_penY + h + 1 > H) {
    fprintf(stderr, "font: glyph atlas full\n");
    g.valid = false;
    return g;
  }

  stbtt_MakeGlyphBitmap(info, m_pixels.data() + m_penX + (size_t)m_penY * W, w, h, W,
                        m_scale, m_scale, gi);

  g.u0 = (float)m_penX / W;
  g.v0 = (float)m_penY / H;
  g.u1 = (float)(m_penX + w) / W;
  g.v1 = (float)(m_penY + h) / H;

  if (m_penY < m_dirtyY0) m_dirtyY0 = m_penY;
  if (m_penY + h > m_dirtyY1) m_dirtyY1 = m_penY + h;

  m_penX += w + 1;
  if (h > m_shelfH) m_shelfH = h;
  return g;
}

float GlyphAtlas::measure(const char* utf8) {
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
  layout.bytesPerRow = W; // 1024 — multiple of 256 ✓
  layout.rowsPerImage = rows;

  WGPUExtent3D extent = {W, rows, 1};
  wgpuQueueWriteTexture(queue, &dst, m_pixels.data() + (size_t)m_dirtyY0 * W,
                        (size_t)rows * W, &layout, &extent);

  m_dirtyY0 = H;
  m_dirtyY1 = 0;
}
