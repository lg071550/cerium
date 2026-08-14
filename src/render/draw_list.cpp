#include "draw_list.h"

#include <cmath>

void DrawList::reset() {
  // high-water reservation: reserve to last frame's counts (grow-only) so
  // steady-state frames don't realloc mid-build
  m_hiQuads = quads.size();
  m_hiGlyphs = glyphs.size();
  m_hiLines = lines.size();
  m_hiCmds = cmds.size();
  quads.clear();
  glyphs.clear();
  lines.clear();
  cmds.clear();
  m_clip.clear();
  if (quads.capacity() < m_hiQuads) quads.reserve(m_hiQuads);
  if (glyphs.capacity() < m_hiGlyphs) glyphs.reserve(m_hiGlyphs);
  if (lines.capacity() < m_hiLines) lines.reserve(m_hiLines);
  if (cmds.capacity() < m_hiCmds) cmds.reserve(m_hiCmds);
}

void DrawList::pushClip(Rect r) {
  Rect c = m_clip.empty() ? r : m_clip.back().intersect(r);
  m_clip.push_back(c);
}

void DrawList::popClip() {
  if (!m_clip.empty()) m_clip.pop_back();
}

Rect DrawList::currentClip() const {
  return m_clip.empty() ? Rect{0, 0, 1e9f, 1e9f} : m_clip.back();
}

void DrawList::breakCmd() {
  DrawCmd cmd{};
  cmd.clip = currentClip();
  cmd.quad0 = (uint32_t)quads.size();
  cmd.glyph0 = (uint32_t)glyphs.size();
  cmd.line0 = (uint32_t)lines.size();
  cmds.push_back(cmd);
}

void DrawList::touch() {
  Rect c = currentClip();
  if (cmds.empty() || cmds.back().clip.x != c.x || cmds.back().clip.y != c.y ||
      cmds.back().clip.w != c.w || cmds.back().clip.h != c.h) {
    DrawCmd cmd{};
    cmd.clip = c;
    cmd.quad0 = (uint32_t)quads.size();
    cmd.glyph0 = (uint32_t)glyphs.size();
    cmd.line0 = (uint32_t)lines.size();
    cmds.push_back(cmd);
  }
}

void DrawList::rect(Rect r, Color c, float radius) {
  touch();
  QuadInstance q{};
  q.rect[0] = r.x;
  q.rect[1] = r.y;
  q.rect[2] = r.w;
  q.rect[3] = r.h;
  q.color[0] = c.r;
  q.color[1] = c.g;
  q.color[2] = c.b;
  q.color[3] = c.a;
  q.params[0] = radius;
  quads.push_back(q);
  cmds.back().quadN++;
}

void DrawList::rectOutline(Rect r, Color c, float t, float radius) {
  touch();
  QuadInstance q{};
  q.rect[0] = r.x;
  q.rect[1] = r.y;
  q.rect[2] = r.w;
  q.rect[3] = r.h;
  q.color[0] = c.r;
  q.color[1] = c.g;
  q.color[2] = c.b;
  q.color[3] = c.a;
  q.params[0] = radius;
  q.params[1] = t; // hollow border band (0 would be a solid fill)
  quads.push_back(q);
  cmds.back().quadN++;
}

void DrawList::shadow(Rect r, float radius, float blur, float dy, Color c) {
  touch();
  QuadInstance q{};
  q.rect[0] = r.x;
  q.rect[1] = r.y + dy;
  q.rect[2] = r.w;
  q.rect[3] = r.h;
  q.color[0] = c.r;
  q.color[1] = c.g;
  q.color[2] = c.b;
  q.color[3] = c.a;
  q.params[0] = radius;
  q.params[1] = 0;
  q.params[2] = blur; // softness
  q.params[3] = blur; // inflate geometry so the falloff has room
  quads.push_back(q);
  cmds.back().quadN++;
}

void DrawList::line(float x0, float y0, float x1, float y1, Color c, float thickness) {
  if (x0 == x1 && y0 == y1) {
    // zero-length segment: draw a square dot of `thickness` at the point
    float h = thickness * 0.5f;
    rect({x0 - h, y0 - h, thickness, thickness}, c);
    return;
  }
  touch();
  LineInstance l{};
  l.pts[0] = x0;
  l.pts[1] = y0;
  l.pts[2] = x1;
  l.pts[3] = y1;
  l.color[0] = c.r;
  l.color[1] = c.g;
  l.color[2] = c.b;
  l.color[3] = c.a;
  l.params[0] = thickness;
  lines.push_back(l);
  cmds.back().lineN++;
}

void DrawList::polyline(const float* xy, int count, Color c, float thickness) {
  for (int i = 0; i + 1 < count; ++i)
    line(xy[i * 2], xy[i * 2 + 1], xy[i * 2 + 2], xy[i * 2 + 3], c, thickness);
}

void DrawList::seriesBand(const float* x, const float* yTop, const float* yBot, int n,
                          Color c) {
  for (int i = 0; i + 1 < n; ++i) {
    float top = yTop[i], bot = yBot[i];
    float y = top < bot ? top : bot;
    float h = top < bot ? bot - top : top - bot;
    rect({x[i], y, x[i + 1] - x[i], h}, c);
  }
}

void DrawList::text(float x, float baselineY, const char* s, Color c) {
  if (!m_atlas || !s) return;
  collectGlyphs(s);
  emitCachedText(x, baselineY, c);
}

float DrawList::collectGlyphs(const char* s) {
  m_glyphCache.clear();
  if (!m_atlas || !s) return 0;
  float w = 0;
  const char* p = s;
  while (*p) {
    const Glyph& g = m_atlas->glyph(utf8_next(p));
    m_glyphCache.push_back(&g);
    w += g.advance;
  }
  return w;
}

void DrawList::emitCachedText(float x, float baselineY, Color c) {
  if (!m_atlas) return;
  const float dpr = m_atlas->dpr();
  float pen = x;
  for (const Glyph* cached : m_glyphCache) {
    const Glyph& g = *cached;
    if (g.w > 0 && g.h > 0) {
      touch();
      // snap to the physical pixel grid — fractional positions are what makes
      // text render blurry
      float dx = std::round((pen + g.bearingX) * dpr) / dpr;
      float dy = std::round((baselineY + g.bearingY) * dpr) / dpr;
      GlyphInstance gi{};
      gi.dst[0] = dx;
      gi.dst[1] = dy;
      gi.dst[2] = g.w;
      gi.dst[3] = g.h;
      gi.uv[0] = g.u0;
      gi.uv[1] = g.v0;
      gi.uv[2] = g.u1 - g.u0;
      gi.uv[3] = g.v1 - g.v0;
      gi.color[0] = c.r;
      gi.color[1] = c.g;
      gi.color[2] = c.b;
      gi.color[3] = c.a;
      glyphs.push_back(gi);
      cmds.back().glyphN++;
    }
    pen += g.advance;
  }
}

void DrawList::textAligned(Rect r, const char* s, Color c, Align align, float padX) {
  float w = collectGlyphs(s);
  emitAlignedText(r, w, c, align, padX);
}

void DrawList::emitAlignedText(Rect r, float w, Color c, Align align, float padX) {
  float x = r.x + padX;
  if (align == Center) x = r.x + (r.w - w) * 0.5f;
  else if (align == Right) x = r.x + r.w - w - padX;
  float baseline = r.y + (r.h - lineHeight()) * 0.5f + ascent();
  emitCachedText(x, baseline, c);
}

void DrawList::textFit(Rect r, const char* s, Color c, Align align, float padX) {
  float avail = r.w - 2 * padX;
  float w = collectGlyphs(s);
  if (w <= avail) {
    emitAlignedText(r, w, c, align, padX);
    return;
  }
  float limit = avail - measure("\xe2\x80\xa6"); // "…"
  if (limit <= 0) return;
  // truncate the cached glyph run at the first codepoint that no longer fits
  // (same accumulation order as a plain measure walk) and append the ellipsis
  float acc = 0;
  size_t cut = 0;
  for (; cut < m_glyphCache.size(); ++cut) {
    float adv = m_glyphCache[cut]->advance;
    if (acc + adv > limit) break;
    acc += adv;
  }
  m_glyphCache.resize(cut);
  const Glyph& ell = m_atlas->glyph(0x2026);
  m_glyphCache.push_back(&ell);
  emitAlignedText(r, acc + ell.advance, c, align, padX);
}

float DrawList::measure(const char* s) { return (m_atlas && s) ? m_atlas->measure(s) : 0; }
float DrawList::lineHeight() const { return m_atlas ? m_atlas->lineHeight() : 14; }
float DrawList::ascent() const { return m_atlas ? m_atlas->ascent() : 11; }
