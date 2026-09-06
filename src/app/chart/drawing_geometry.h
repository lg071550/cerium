#pragma once
#include "drawings.h"
#include <algorithm>
#include <cmath>
namespace drawing_geometry {
constexpr float kHit = 7.0f;
constexpr float kFib[] = {0.0f, 0.236f, 0.382f, 0.5f, 0.618f, 0.786f, 1.0f};
constexpr const char* kFibLbl[] = {"0", "0.236", "0.382", "0.5", "0.618", "0.786", "1"};

inline int barAtTime(const CandleSeries& cs, double ts) {
  if (cs.v.empty()) return 0;
  int lo = 0, hi = (int)cs.v.size() - 1;
  if (ts <= cs.v.front().ts) return 0;
  if (ts >= cs.v.back().ts) return hi;
  while (lo < hi) {
    int mid = lo + (hi - lo + 1) / 2;
    if (cs.v[(size_t)mid].ts <= ts) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

inline double timeAtBar(const CandleSeries& cs, int i) {
  if (cs.v.empty()) return 0;
  i = std::clamp(i, 0, (int)cs.v.size() - 1);
  return cs.v[(size_t)i].ts;
}

inline float xOfBar(const ChartPane& pane, float startF, float bw, int i) {
  return pane.area.x + ((float)i - startF) * bw + bw * 0.5f;
}

inline int barAtX(const ChartPane& pane, float startF, float bw, int size, float x) {
  if (!(bw > 0)) return 0;
  int i = (int)std::floor(startF + (x - pane.area.x) / bw);
  return std::clamp(i, 0, std::max(0, size - 1));
}

inline double barVolume(const Candle& c) { return c.aggVol > 0 ? c.aggVol : c.vol; }

inline float distPtSeg(float px, float py, float x0, float y0, float x1, float y1) {
  float dx = x1 - x0, dy = y1 - y0;
  float len2 = dx * dx + dy * dy;
  float t = 0;
  if (len2 > 1e-6f) t = std::clamp(((px - x0) * dx + (py - y0) * dy) / len2, 0.0f, 1.0f);
  float x = x0 + t * dx, y = y0 + t * dy;
  return std::hypot(px - x, py - y);
}

inline void screenOf(const ChartDrawing& g, const ChartPane& pane, const CandleSeries& cs,
              float startF, float bw, float& x0, float& y0, float& x1, float& y1) {
  int b0 = barAtTime(cs, g.t0);
  int b1 = barAtTime(cs, g.t1);
  x0 = xOfBar(pane, startF, bw, b0);
  x1 = xOfBar(pane, startF, bw, b1);
  y0 = pane.yOf(g.p0);
  y1 = pane.yOf(g.p1);
}

inline int hitHandle(const ChartDrawing& g, const ChartPane& pane, const CandleSeries& cs,
              float startF, float bw, float mx, float my) {
  if (drawClickCommit(g.kind)) return 0;
  float x0, y0, x1, y1;
  screenOf(g, pane, cs, startF, bw, x0, y0, x1, y1);
  if (std::hypot(mx - x0, my - y0) <= kHit + 2.0f) return 1;
  if (std::hypot(mx - x1, my - y1) <= kHit + 2.0f) return 2;
  return 0;
}

inline void avwapPoints(const ChartDrawing& g, const ChartPane& pane,
                         const CandleSeries& cs, float startF, float bw,
                         std::vector<float>& xy) {
  xy.clear();
  double pv = 0, vol = 0;
  for (int i = barAtTime(cs, g.t0); i < (int)cs.v.size(); ++i) {
    const Candle& bar = cs.v[(size_t)i];
    const double v = barVolume(bar), tp = (bar.h + bar.l + bar.c) / 3.0;
    if (!(v > 0) || !std::isfinite(v) || !std::isfinite(tp)) continue;
    pv += tp * v; vol += v;
    float x = xOfBar(pane, startF, bw, i);
    if (x < pane.area.x - bw) continue;
    if (x > pane.area.x + pane.area.w + bw) break;
    xy.push_back(x); xy.push_back(pane.yOf(pv / vol));
  }
}

inline float hitDist(const ChartDrawing& g, const ChartPane& pane, const CandleSeries& cs,
              float startF, float bw, float mx, float my) {
  float x0, y0, x1, y1;
  screenOf(g, pane, cs, startF, bw, x0, y0, x1, y1);
  switch (g.kind) {
    case DrawTool::HLine:
      return std::fabs(my - y0);
    case DrawTool::Avwap: {
      thread_local std::vector<float> xy;
      avwapPoints(g, pane, cs, startF, bw, xy);
      float distance = INFINITY;
      for (size_t i = 0; i + 1 < xy.size(); i += 2) {
        float d = i ? distPtSeg(mx,my,xy[i-2],xy[i-1],xy[i],xy[i+1])
                    : std::hypot(mx-xy[i],my-xy[i+1]);
        distance = std::min(distance, d);
      }
      return distance;
    }
    case DrawTool::Fib: {
      float distance = INFINITY;
      for (float ratio : kFib)
        distance = std::min(distance, distPtSeg(mx,my,x0,y0+(y1-y0)*ratio,x1,y0+(y1-y0)*ratio));
      return distance;
    }
    case DrawTool::Rect: {
      float l = std::min(x0, x1), r = std::max(x0, x1);
      float t = std::min(y0, y1), b = std::max(y0, y1);
      float dx = mx < l ? l - mx : mx > r ? mx - r : 0;
      float dy = my < t ? t - my : my > b ? my - b : 0;
      if (dx == 0 && dy == 0)
        return std::min({mx - l, r - mx, my - t, b - my});
      return std::hypot(dx, dy);
    }
    case DrawTool::Ray: {
      float ux = x1 - x0, uy = y1 - y0;
      float len = std::hypot(ux, uy);
      if (len < 1.0f) return std::hypot(mx - x0, my - y0);
      ux /= len;
      uy /= len;
      float t = (mx - x0) * ux + (my - y0) * uy;
      if (t < 0) return std::hypot(mx - x0, my - y0);
      return std::hypot(mx - (x0 + ux * t), my - (y0 + uy * t));
    }
    default:
      return distPtSeg(mx, my, x0, y0, x1, y1);
  }
}

}
