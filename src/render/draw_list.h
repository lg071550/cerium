#pragma once

#include "color.h"
#include "glyph_atlas.h"
#include "line_batch.h"
#include "quad_batch.h"
#include "text_batch.h"

#include <vector>

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;

  bool contains(float px, float py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
  Rect inset(float p) const { return {x + p, y + p, w - 2 * p, h - 2 * p}; }
  Rect intersect(Rect o) const {
    float nx = x > o.x ? x : o.x;
    float ny = y > o.y ? y : o.y;
    float nr = (x + w < o.x + o.w ? x + w : o.x + o.w);
    float nb = (y + h < o.y + o.h ? y + h : o.y + o.h);
    return {nx, ny, nr > nx ? nr - nx : 0, nb > ny ? nb - ny : 0};
  }
};

// One render run: a clip rect plus the instance ranges drawn under it.
struct DrawCmd {
  Rect clip;
  uint32_t quad0 = 0, quadN = 0;
  uint32_t glyph0 = 0, glyphN = 0;
  uint32_t line0 = 0, lineN = 0;
};

// Immediate-mode draw list: widgets append quads + resolved glyph instances;
// the renderer walks cmds and draws each range under its scissor rect.
//
// Ordering rule: within one DrawCmd the renderer always draws
// quads → lines → text, regardless of submission order (one pipeline bind per
// type per cmd). Painter's order only applies ACROSS cmds: use breakCmd() to
// force a layer boundary when a later primitive must sit on top of an
// earlier primitive of a "later" type (e.g. a quad over text).
class DrawList {
public:
  enum Align { Left, Center, Right };

  void setAtlas(GlyphAtlas* atlas) { m_atlas = atlas; }
  void reset();

  void pushClip(Rect r); // intersected with current clip
  void popClip();
  Rect currentClip() const;
  // Starts a fresh DrawCmd under the SAME current clip, forcing a layer
  // boundary: everything submitted after the break draws on top of everything
  // before it (see ordering rule above).
  void breakCmd();

  void rect(Rect r, Color c, float radius = 0);
  void rectOutline(Rect r, Color c, float t = 1.0f, float radius = 0); // single hollow quad
  // soft shadow beneath a card shape (drawn before the card). dy drops it
  // downward; color is typically black at 0.2–0.5 alpha.
  void shadow(Rect r, float radius, float blur, float dy, Color c);
  void line(float x0, float y0, float x1, float y1, Color c, float thickness = 1.0f);
  // connected segments through `count` points; xy is interleaved x0,y0,x1,y1,…
  void polyline(const float* xy, int count, Color c, float thickness = 1.0f);
  // per-column fill between two y series (e.g. Bollinger band); one rect per
  // [x[i], x[i+1]) span — caller uses low alpha. n<2 → no-op
  void seriesBand(const float* x, const float* yTop, const float* yBot, int n, Color c);
  void text(float x, float baselineY, const char* s, Color c);
  void textAligned(Rect r, const char* s, Color c, Align align = Left, float padX = 0);
  // textAligned that truncates with "…" when the string exceeds the rect
  void textFit(Rect r, const char* s, Color c, Align align = Left, float padX = 0);

  float measure(const char* s);
  float lineHeight() const;
  float ascent() const;

  std::vector<QuadInstance> quads;
  std::vector<GlyphInstance> glyphs;
  std::vector<LineInstance> lines;
  std::vector<DrawCmd> cmds;

private:
  void touch(); // ensure the current command matches the current clip

  // Fills m_glyphCache with one glyph() lookup per codepoint and returns the
  // total advance width (== measure(s)). Null/no-atlas → empty cache, 0.
  float collectGlyphs(const char* s);
  // Emits m_glyphCache as a run of text starting at x (== text(), one pass).
  void emitCachedText(float x, float baselineY, Color c);
  // textAligned() body for an already-collected string of width w.
  void emitAlignedText(Rect r, float w, Color c, Align align, float padX);

  GlyphAtlas* m_atlas = nullptr;
  std::vector<Rect> m_clip;
  // single-pass measure+emit scratch: glyph() is looked up once per
  // codepoint while accumulating width, then emission reuses the cache —
  // no second string walk and no per-call allocations. Glyph references
  // stay valid across later lookups (baking inserts don't invalidate them).
  std::vector<const Glyph*> m_glyphCache;
  // previous frame's counts — reset() reserves to these (grow-only) so
  // steady-state frames don't realloc
  size_t m_hiQuads = 0, m_hiGlyphs = 0, m_hiLines = 0, m_hiCmds = 0;
};
