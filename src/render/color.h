#pragma once

struct Color {
  float r, g, b, a;
};

constexpr Color hexColor(unsigned rgb, float a = 1.0f) {
  return {((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f,
          (rgb & 0xff) / 255.0f, a};
}

constexpr Color withAlpha(Color c, float a) { return {c.r, c.g, c.b, a}; }
