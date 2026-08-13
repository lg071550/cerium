#include "draw_list.h"

void DrawList::reset() {
  quads.clear();
  glyphs.clear();
  cmds.clear();
  m_clip.clear();
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

void DrawList::touch() {
  Rect c = currentClip();
  if (cmds.empty() || cmds.back().clip.x != c.x || cmds.back().clip.y != c.y ||
      cmds.back().clip.w != c.w || cmds.back().clip.h != c.h) {
    DrawCmd cmd{};
    cmd.clip = c;
    cmd.quad0 = (uint32_t)quads.size();
    cmd.glyph0 = (uint32_t)glyphs.size();
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
  rect({r.x, r.y, r.w, t}, c, radius);
  rect({r.x, r.y + r.h - t, r.w, t}, c, radius);
  rect({r.x, r.y + t, t, r.h - 2 * t}, c, 0);
  rect({r.x + r.w - t, r.y + t, t, r.h - 2 * t}, c, 0);
}

void DrawList::text(float x, float baselineY, const char* s, Color c) {
  if (!m_atlas) return;
  const char* p = s;
  float pen = x;
  while (*p) {
    uint32_t cp = utf8_next(p);
    const Glyph& g = m_atlas->glyph(cp);
    if (g.w > 0 && g.h > 0) {
      touch();
      GlyphInstance gi{};
      gi.dst[0] = pen + g.bearingX;
      gi.dst[1] = baselineY + g.bearingY;
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
  float w = measure(s);
  float x = r.x + padX;
  if (align == Center) x = r.x + (r.w - w) * 0.5f;
  else if (align == Right) x = r.x + r.w - w - padX;
  float baseline = r.y + (r.h - lineHeight()) * 0.5f + ascent();
  text(x, baseline, s, c);
}

float DrawList::measure(const char* s) { return m_atlas ? m_atlas->measure(s) : 0; }
float DrawList::lineHeight() const { return m_atlas ? m_atlas->lineHeight() : 14; }
float DrawList::ascent() const { return m_atlas ? m_atlas->ascent() : 11; }
