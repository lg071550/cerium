#pragma once

#include "../render/glyph_atlas.h"
#include "../render/quad_batch.h"
#include "../render/text_batch.h"
#include "theme.h"

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
};

// Immediate-mode draw list: widgets append quads + resolved glyph instances;
// the renderer walks cmds and draws each range under its scissor rect.
class DrawList {
public:
  enum Align { Left, Center, Right };

  void setAtlas(GlyphAtlas* atlas) { m_atlas = atlas; }
  void reset();

  void pushClip(Rect r); // intersected with current clip
  void popClip();
  Rect currentClip() const;

  void rect(Rect r, Color c, float radius = 0);
  void rectOutline(Rect r, Color c, float t = 1.0f, float radius = 0);
  void text(float x, float baselineY, const char* s, Color c);
  void textAligned(Rect r, const char* s, Color c, Align align = Left, float padX = 0);

  float measure(const char* s);
  float lineHeight() const;
  float ascent() const;

  std::vector<QuadInstance> quads;
  std::vector<GlyphInstance> glyphs;
  std::vector<DrawCmd> cmds;

private:
  void touch(); // ensure the current command matches the current clip

  GlyphAtlas* m_atlas = nullptr;
  std::vector<Rect> m_clip;
};
