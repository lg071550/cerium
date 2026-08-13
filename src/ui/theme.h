#pragma once

struct Color {
  float r, g, b, a;
};

constexpr Color hexColor(unsigned rgb, float a = 1.0f) {
  return {((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f,
          (rgb & 0xff) / 255.0f, a};
}

constexpr Color withAlpha(Color c, float a) { return {c.r, c.g, c.b, a}; }

// Single dark theme for the terminal. Metrics in logical px.
struct Theme {
  Color bg = hexColor(0x0b0e11);
  Color panel = hexColor(0x12161c);
  Color panelAlt = hexColor(0x0e1216);
  Color border = hexColor(0x1e242c);
  Color tabStrip = hexColor(0x0e1216);

  Color text = hexColor(0xd6dde6);
  Color textDim = hexColor(0x7d8794);

  Color accent = hexColor(0xf0b90b);
  Color accentSoft = hexColor(0xf0b90b, 0.25f);

  Color green = hexColor(0x2ebd85);
  Color red = hexColor(0xf6465d);

  Color bgRaised = hexColor(0x1c232d);
  Color bgHover = hexColor(0x232c38);
  Color splitter = hexColor(0x2a323d);

  float fontSize = 13.0f;
  float topBarH = 40.0f;
  float tabStripH = 30.0f;
  float splitterSize = 6.0f;
  float pad = 8.0f;
  float radius = 6.0f;
};

inline const Theme& theme() {
  static const Theme t{};
  return t;
}
