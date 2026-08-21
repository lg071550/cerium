#pragma once

#include "render/color.h"

// Neutral graphite terminal theme. Surfaces are flat and contiguous; hierarchy
// comes from restrained tone shifts and functional 1px separators. Accent is
// reserved for selection, focus, and market data rather than decoration.
struct Theme {
  Color bg = hexColor(0x090b0e);        // application shell
  Color panel = hexColor(0x101318);     // primary work surface
  Color panelAlt = hexColor(0x0c0f13);  // recessed inputs / secondary rows
  Color border = hexColor(0x20262e);    // functional separators and grids
  Color tabStrip = hexColor(0x0c0f13);  // workspace navigation strip

  Color text = hexColor(0xe3e7ec);
  Color textDim = hexColor(0x707a86);
  Color textShadow = hexColor(0x000000, 0.42f);

  // Hyperliquid-derived mint: cool and high-clarity against graphite, while
  // semantic bid/ask colors remain reserved for trade direction.
  Color accent = hexColor(0x50d2c1);
  Color accentSoft = hexColor(0x50d2c1, 0.14f);

  Color green = hexColor(0x26a69a);
  Color red = hexColor(0xef5350);

  Color bgRaised = hexColor(0x171c22);  // selected / active controls
  Color bgHover = hexColor(0x1b2129);
  Color splitter = hexColor(0x252c35);

  Color chartPaneBg = hexColor(0x0d1015);
  Color chartWarm = hexColor(0x86a4bd); // cool secondary series against mint
  Color chartPurple = hexColor(0x9c8fe8);

  float scale = 1.0f;
  float fontSize = 13.0f;
  float topBarH = 28.0f;
  float tabStripH = 28.0f;
  float splitterSize = 4.0f; // narrow resize target between work areas
  float pad = 6.0f;
  float radius = 3.0f;
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
  t.topBarH = 28.0f * s;
  t.tabStripH = 28.0f * s;
  t.splitterSize = 4.0f * s;
  t.pad = 6.0f * s;
  t.radius = 3.0f * s;
}
