#pragma once

#include "../render/color.h"

// Tokyo Night–flavored dark theme. Depth model: raised cards are lighter with
// soft shadows; recessed zones (tab strips, inputs) are darker. No outlines —
// separation comes from tone and shadow.
struct Theme {
  Color bg = hexColor(0x16161e);        // window — shows in card gaps
  Color panel = hexColor(0x1f2335);     // raised card
  Color panelAlt = hexColor(0x1a1b26);  // recessed zones
  Color border = hexColor(0x292e42);    // functional hairlines only (grids)
  Color tabStrip = hexColor(0x1a1b26);  // recessed

  Color text = hexColor(0xc0caf5);
  Color textDim = hexColor(0x565f89);

  Color accent = hexColor(0x7aa2f7);
  Color accentSoft = hexColor(0x7aa2f7, 0.22f);

  Color green = hexColor(0x9ece6a);
  Color red = hexColor(0xf7768e);

  Color bgRaised = hexColor(0x242a3d);  // small raised controls
  Color bgHover = hexColor(0x2a3047);
  Color splitter = hexColor(0x3b4261);

  Color chartPaneBg = hexColor(0x1a1c2b);   // recessed chart indicator panes
  Color chartWarm = hexColor(0xff9e64);     // warm series (SMA, MACD signal)
  Color chartPurple = hexColor(0xbb9af7);   // RSI series

  float scale = 1.0f;
  float fontSize = 13.0f;
  float topBarH = 44.0f;
  float tabStripH = 32.0f;
  float splitterSize = 8.0f; // gap between cards
  float pad = 8.0f;
  float radius = 8.0f;
};

inline Theme& themeMut() {
  static Theme t{};
  return t;
}

inline const Theme& theme() { return themeMut(); }

inline float themeScale() { return themeMut().scale; }

inline void themeApplyScale(float s) {
  Theme& t = themeMut();
  t.scale = s;
  t.fontSize = 13.0f * s;
  t.topBarH = 44.0f * s;
  t.tabStripH = 32.0f * s;
  t.splitterSize = 8.0f * s;
  t.pad = 8.0f * s;
  t.radius = 8.0f * s;
}
