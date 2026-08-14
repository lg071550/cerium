#pragma once

#include "../../render/draw_list.h"
#include "../../ui/theme.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

// Chart pane infrastructure shared by the chart panel: a pane is a horizontal
// slice of the chart with its own value range; the shared gutter to the right
// of every pane shows that pane's y-labels.

struct ChartPane {
  Rect area;         // plot area (excludes the gutter)
  double lo = 0, hi = 1;
  bool grid = false; // price pane only: horizontal grid lines

  float yOf(double v) const {
    return area.y + (float)((hi - v) / (hi - lo)) * area.h;
  }
};

using ChartFmt = void (*)(char* out, size_t n, double v);

inline void chartFmtPrice(char* out, size_t n, double v) { snprintf(out, n, "%.2f", v); }
inline void chartFmtInt(char* out, size_t n, double v) { snprintf(out, n, "%.0f", v); }
inline void chartFmtPlain(char* out, size_t n, double v) { snprintf(out, n, "%.1f", v); }
inline void chartFmtVol(char* out, size_t n, double v) { // 12.4k / 3.1M
  double a = std::fabs(v);
  if (a >= 1e6) snprintf(out, n, "%.1fM", v / 1e6);
  else if (a >= 1e3) snprintf(out, n, "%.1fk", v / 1e3);
  else snprintf(out, n, "%.0f", v);
}

// 1/2/5-scaled "nice" ticks covering [lo, hi], roughly targetCount of them.
inline void niceTicks(double lo, double hi, int targetCount, std::vector<double>& out) {
  out.clear();
  double span = hi - lo;
  if (!(span > 0) || targetCount < 1) return;
  double raw = span / targetCount;
  double mag = std::pow(10.0, std::floor(std::log10(raw)));
  static const double kMults[] = {1, 2, 5, 10};
  double step = 10 * mag;
  for (double m : kMults)
    if (m * mag >= raw) {
      step = m * mag;
      break;
    }
  long k0 = (long)std::ceil(lo / step - 1e-9);
  long k1 = (long)std::floor(hi / step + 1e-9);
  for (long k = k0; k <= k1; ++k) out.push_back(k * step);
}

// Per-pane gutter y-labels at nice ticks. Labels within 16px of supA/supB
// (live-price / crosshair tag rows) are skipped; pass -1e9f for no tag.
inline void drawPaneGutter(DrawList& d, Rect gutter, const ChartPane& p, ChartFmt fmt,
                           int targetCount, float supA = -1e9f, float supB = -1e9f) {
  static thread_local std::vector<double> ticks;
  niceTicks(p.lo, p.hi, targetCount, ticks);
  char buf[24];
  for (double tv : ticks) {
    float y = p.yOf(tv);
    if (y < p.area.y + 7 || y > p.area.y + p.area.h - 7) continue;
    if (std::fabs(y - supA) < 16.0f || std::fabs(y - supB) < 16.0f) continue;
    fmt(buf, sizeof(buf), tv);
    d.textAligned({gutter.x + 4, y - 8, gutter.w - 8, 16}, buf, theme().textDim,
                  DrawList::Left);
  }
}
