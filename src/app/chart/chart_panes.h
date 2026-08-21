#pragma once

#include "render/draw_list.h"
#include "../../ui/theme.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

struct ChartPane {
  Rect area;
  double lo = 0, hi = 1;

  bool grid = false;
  bool log = false;
  bool remap = false;

  double srcLo = 0, srcHi = 1;
  double dstLo = 0, dstHi = 1;

  double remapValue(double v) const {
    if (!remap) return v;
    double span = srcHi - srcLo;
    if (!(span > 0.0) || !(dstHi > dstLo)) return (dstLo + dstHi) * 0.5;
    double t = (v - srcLo) / span;
    return dstLo + t * (dstHi - dstLo);
  }

  double fracOf(double v) const {
    v = remapValue(v);
    if (log && lo > 0 && hi > 0)
      return (std::log(hi) - std::log(std::max(v, lo))) / (std::log(hi) - std::log(lo));
    return (hi - v) / (hi - lo);
  }

  float yOf(double v) const { return area.y + (float)fracOf(v) * area.h; }

  double vOf(float y) const {
    double t = (double)(y - area.y) / area.h;
    double v = (log && lo > 0 && hi > 0)
                   ? std::exp(std::log(hi) - t * (std::log(hi) - std::log(lo)))
                   : hi - t * (hi - lo);
    if (!remap) return v;
    double span = dstHi - dstLo;
    if (!(span > 0.0) || !(srcHi > srcLo)) return (srcLo + srcHi) * 0.5;
    double u = (v - dstLo) / span;
    return srcLo + u * (srcHi - srcLo);
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

inline void chartFmtUsd(char* out, size_t n, double v) { // $12.4K / $3.1B
  double a = std::fabs(v);
  if (a >= 1e12) snprintf(out, n, "$%.2fT", v / 1e12);
  else if (a >= 1e9) snprintf(out, n, "$%.2fB", v / 1e9);
  else if (a >= 1e6) snprintf(out, n, "$%.1fM", v / 1e6);
  else if (a >= 1e3) snprintf(out, n, "$%.1fK", v / 1e3);
  else snprintf(out, n, "$%.0f", v);
}

inline void chartFmtPct(char* out, size_t n, double v) { // +0.010%
  snprintf(out, n, "%+.3f%%", v * 100.0);
}

inline void niceTicks(double lo, double hi, int targetCount, std::vector<double>& out) {
  out.clear();
  double span = hi - lo;

  if (!(span > 0) || targetCount < 1) return;

  double raw = span / targetCount;
  double mag = std::pow(10.0, std::floor(std::log10(raw)));
  static constexpr double kMults[] = {1, 2, 5, 10};
  double step = 10 * mag;

  for (double m : kMults) if (m * mag >= raw) { step = m * mag; break; }

  long k0 = (long)std::ceil(lo / step - 1e-9);
  long k1 = (long)std::floor(hi / step + 1e-9);
  for (long k = k0; k <= k1; ++k) out.push_back(k * step);
}

inline void drawPaneGutter(DrawList& d, Rect gutter, const ChartPane& p, ChartFmt fmt,
                           int targetCount, float supA = -1e9f, float supB = -1e9f,
                           float supC = -1e9f) {

  static thread_local std::vector<double> ticks;
  niceTicks(p.lo, p.hi, targetCount, ticks);
  char buf[24];

  for (double tv : ticks) {
    float y = p.yOf(tv);

    if (y < p.area.y + 7 || y > p.area.y + p.area.h - 7) continue;
    if (std::fabs(y - supA) < 28.0f || std::fabs(y - supB) < 16.0f || std::fabs(y - supC) < 16.0f) continue;

    fmt(buf, sizeof(buf), tv);
    d.textAligned({gutter.x + 4, y - 8, gutter.w - 8, 16}, buf, theme().textDim, DrawList::Left);
  }
}