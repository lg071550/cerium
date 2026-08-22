#pragma once

#include "../../data/candles.h"
#include "chart_panes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <limits>
#include <vector>

inline int d7BaseLength(int p0) {
  if (p0 == 14 || p0 == 28 || p0 == 56) return p0;
  return 56;
}

// EMA on the fast Donchian mid so D-Lead flows; K is a short raw 50% stair.
inline int d7DEmaLen(int nLen) { return std::max(4, nLen / 6); }
inline int d7KLen(int nLen) { return nLen; }

inline int64_t d7UtcDay(double tsMs) {
  return (int64_t)std::floor(tsMs / 86400000.0);
}

inline int64_t d7UtcWeek(int64_t day) { return (day + 3) / 7; }

inline int d7UtcMonth(double tsMs) {
  time_t t = (time_t)(tsMs / 1000.0);
  tm g{};
  if (!gmtime_r(&t, &g)) return 0;
  return g.tm_year * 12 + g.tm_mon;
}

inline float d7MidAt(const CandleSeries& cs, int i, int period) {
  if (period < 1 || i + 1 < period) return NAN;
  double hh = -1e300, ll = 1e300;
  for (int j = i + 1 - period; j <= i; ++j) {
    hh = std::max(hh, cs.v[(size_t)j].h);
    ll = std::min(ll, cs.v[(size_t)j].l);
  }
  return (float)((hh + ll) * 0.5);
}

inline void d7DonchianMid(const CandleSeries& cs, int period,
                         std::vector<float>& out) {
  size_t n = cs.v.size();
  out.assign(n, NAN);
  if (period < 1 || n < (size_t)period) return;
  for (size_t i = (size_t)period - 1; i < n; ++i)
    out[i] = d7MidAt(cs, (int)i, period);
}

inline void d7EmaClose(const CandleSeries& cs, int period,
                      std::vector<float>& out) {
  size_t n = cs.v.size();
  out.assign(n, NAN);
  if (n < (size_t)period || period < 1) return;
  double sum = 0;
  for (int i = 0; i < period; ++i) sum += cs.v[(size_t)i].c;
  double ema = sum / period;
  out[(size_t)period - 1] = (float)ema;
  double k = 2.0 / (period + 1);
  for (size_t i = (size_t)period; i < n; ++i) {
    ema += (cs.v[i].c - ema) * k;
    out[i] = (float)ema;
  }
}

inline void d7Rsi(const CandleSeries& cs, int p, std::vector<float>& out,
                  double* gainOut = nullptr, double* lossOut = nullptr) {
  // Shared Wilder implementation (chart_panes.h); the out-params are kept for
  // signature compatibility but no caller consumes them.
  wilderRsiSeries(cs, p, out);
  (void)gainOut;
  (void)lossOut;
}

inline void d7EmaOn(const std::vector<float>& src, int period,
                   std::vector<float>& out) {
  size_t n = src.size();
  out.assign(n, NAN);
  if (period < 1) return;
  double ema = 0;
  int valid = 0;
  const double k = 2.0 / (period + 1);
  for (size_t i = 0; i < n; ++i) {
    if (std::isnan(src[i])) continue;
    ema = valid == 0 ? src[i] : ema + (src[i] - ema) * k;
    if (++valid >= period) out[i] = (float)ema;
  }
}
inline void d7SuperSmootherClose(const CandleSeries& cs, int period,
                                std::vector<float>& out) {
  size_t n = cs.v.size();
  out.assign(n, NAN);
  if (period < 2 || n < 3) return;
  static constexpr double pi = 3.14159265358979323846;
  double a1 = std::exp(-1.41421356237 * pi / (double)period);
  double b1 = 2.0 * a1 * std::cos(1.41421356237 * pi / (double)period);
  double c2 = b1;
  double c3 = -a1 * a1;
  double c1 = 1.0 - c2 - c3;
  double f1 = cs.v[0].c, f2 = cs.v[0].c;
  for (size_t i = 0; i < n; ++i) {
    double x = cs.v[i].c;
    double x1 = i ? cs.v[i - 1].c : x;
    double f = c1 * (x + x1) * 0.5 + c2 * f1 + c3 * f2;
    f2 = f1;
    f1 = f;
    if (i + 1 >= (size_t)period) out[i] = (float)f;
  }
}

inline void d7Leads(const CandleSeries& cs, int nLen, std::vector<float>& dLead,
                   std::vector<float>& kLead) {
  std::vector<float> dRaw;
  d7DonchianMid(cs, nLen, dRaw);
  d7EmaOn(dRaw, d7DEmaLen(nLen), dLead);
  d7DonchianMid(cs, d7KLen(nLen), kLead);
}

inline int8_t d7Vote(double close, float dLead, float kLead, float rsi) {
  if (std::isnan(dLead)) return 0;
  int lead = std::isnan(kLead) ? (close >= dLead ? 1 : -1)
                               : (dLead >= kLead ? 1 : -1);
  int px = close >= dLead ? 1 : -1;
  int mom = std::isnan(rsi) ? px : (rsi >= 50.0f ? 1 : -1);
  if (px == lead && mom == lead) return (int8_t)lead;
  return 0;
}

inline void d7FillScore(const CandleSeries& cs, const std::vector<float>& dLead,
                       const std::vector<float>& kLead,
                       const std::vector<float>& rsi,
                       std::vector<int8_t>& dir, std::vector<float>* asFloat) {
  size_t n = cs.v.size();
  dir.assign(n, 0);
  if (asFloat) asFloat->assign(n, NAN);
  int run = 0;
  int8_t prev = 0;
  for (size_t i = 0; i < n; ++i) {
    float r = i < rsi.size() ? rsi[i] : NAN;
    float d = i < dLead.size() ? dLead[i] : NAN;
    float k = i < kLead.size() ? kLead[i] : NAN;
    int8_t s = d7Vote(cs.v[i].c, d, k, r);
    dir[i] = s;
    if (std::isnan(d)) continue;
    run = (s == prev) ? run + 1 : 1;
    prev = s;
    if (asFloat) (*asFloat)[i] = (float)run;
  }
}

inline void d7ComputeCloud(const CandleSeries& cs, int length,
                          std::vector<float>& dLead, std::vector<float>& kLead,
                          std::vector<float>& ma, std::vector<int8_t>& dir) {
  int nLen = d7BaseLength(length);
  d7Leads(cs, nLen, dLead, kLead);
  d7EmaClose(cs, nLen, ma);
  std::vector<float> rsi;
  d7Rsi(cs, 14, rsi, nullptr, nullptr);
  d7FillScore(cs, dLead, kLead, rsi, dir, nullptr);
}

inline bool d7UpdateCloudLast(const CandleSeries& cs, int length,
                             std::vector<float>& dLead, std::vector<float>& kLead,
                             std::vector<float>& ma, std::vector<int8_t>& dir,
                             double rsiGain, double rsiLoss) {
  size_t n = cs.v.size();
  int nLen = d7BaseLength(length);
  if (n < 2 || dLead.size() != n || kLead.size() != n || ma.size() != n ||
      dir.size() != n)
    return false;
  float dRaw = d7MidAt(cs, (int)n - 1, nLen);
  kLead[n - 1] = d7MidAt(cs, (int)n - 1, d7KLen(nLen));
  if (std::isnan(dRaw) || std::isnan(dLead[n - 2]) || std::isnan(kLead[n - 1]))
    return false;
  double dk = 2.0 / (d7DEmaLen(nLen) + 1);
  dLead[n - 1] = (float)(dLead[n - 2] + (dRaw - dLead[n - 2]) * dk);
  if (std::isnan(ma[n - 2])) return false;
  double k = 2.0 / (nLen + 1);
  ma[n - 1] = (float)(ma[n - 2] + (cs.v[n - 1].c - ma[n - 2]) * k);
  double d = cs.v[n - 1].c - cs.v[n - 2].c;
  double g = (rsiGain * 13.0 + (d > 0 ? d : 0)) / 14.0;
  double l = (rsiLoss * 13.0 + (d < 0 ? -d : 0)) / 14.0;
  float rsi =
      l == 0 ? 100.0f : (float)(100.0 - 100.0 / (1.0 + g / l));
  dir[n - 1] = d7Vote(cs.v[n - 1].c, dLead[n - 1], kLead[n - 1], rsi);
  return true;
}

inline void d7ComputeRsiCloud(const CandleSeries& cs, int rsiP, int fastP,
                             int slowP, std::vector<float>& rsi,
                             std::vector<float>& fast, std::vector<float>& slow) {
  d7Rsi(cs, std::max(2, rsiP), rsi, nullptr, nullptr);
  d7EmaOn(rsi, std::max(2, fastP), fast);
  d7EmaOn(rsi, std::max(2, slowP), slow);
}

inline bool d7UpdateRsiCloudLast(const CandleSeries& cs, int rsiP, int fastP,
                                int slowP, std::vector<float>& rsi,
                                std::vector<float>& fast,
                                std::vector<float>& slow, double gain,
                                double loss) {
  size_t n = cs.v.size();
  if (n < 2 || rsi.size() != n || fast.size() != n || slow.size() != n)
    return false;
  double d = cs.v[n - 1].c - cs.v[n - 2].c;
  double g = (gain * (rsiP - 1) + (d > 0 ? d : 0)) / rsiP;
  double l = (loss * (rsiP - 1) + (d < 0 ? -d : 0)) / rsiP;
  rsi[n - 1] =
      l == 0 ? 100.0f : (float)(100.0 - 100.0 / (1.0 + g / l));
  auto step = [](float prev, float now, int p) {
    if (std::isnan(prev)) return NAN;
    return (float)(prev + (now - prev) * (2.0 / (p + 1)));
  };
  fast[n - 1] = step(fast[n - 2], rsi[n - 1], fastP);
  slow[n - 1] = step(slow[n - 2], rsi[n - 1], slowP);
  return true;
}

inline void d7ComputeScore(const CandleSeries& cs, int length,
                          std::vector<float>& series, std::vector<int8_t>& dir) {
  int nLen = d7BaseLength(length);
  std::vector<float> dLead, kLead, rsi;
  d7Leads(cs, nLen, dLead, kLead);
  d7Rsi(cs, 14, rsi, nullptr, nullptr);
  d7FillScore(cs, dLead, kLead, rsi, dir, &series);
}

inline bool d7UpdateScoreLast(const CandleSeries& cs, int length,
                             std::vector<float>& series,
                             std::vector<int8_t>& dir, double rsiGain,
                             double rsiLoss) {
  size_t n = cs.v.size();
  int nLen = d7BaseLength(length);
  if (n < 2 || series.size() != n || dir.size() != n) return false;
  float dLead = d7MidAt(cs, (int)n - 1, nLen);
  float kLead = d7MidAt(cs, (int)n - 1, d7KLen(nLen));
  double d = cs.v[n - 1].c - cs.v[n - 2].c;
  double g = (rsiGain * 13.0 + (d > 0 ? d : 0)) / 14.0;
  double l = (rsiLoss * 13.0 + (d < 0 ? -d : 0)) / 14.0;
  float r = l == 0 ? 100.0f : (float)(100.0 - 100.0 / (1.0 + g / l));
  dir[n - 1] = d7Vote(cs.v[n - 1].c, dLead, kLead, r);
  if (std::isnan(dLead)) series[n - 1] = NAN;
  else {
    float prev = series[n - 2];
    int run = (!std::isnan(prev) && dir[n - 1] == dir[n - 2])
                  ? (int)prev + 1
                  : 1;
    series[n - 1] = (float)run;
  }
  return true;
}

inline void d7ComputeLevels(const CandleSeries& cs, std::vector<float>& dayOpen,
                           std::vector<float>& weekOpen,
                           std::vector<float>& monthOpen) {
  size_t n = cs.v.size();
  dayOpen.assign(n, NAN);
  weekOpen.assign(n, NAN);
  monthOpen.assign(n, NAN);
  if (n == 0) return;
  int64_t prevDay = std::numeric_limits<int64_t>::min();
  int prevMonth = 0;
  float dOpen = 0, wOpen = 0, mOpen = 0;
  for (size_t i = 0; i < n; ++i) {
    int64_t day = d7UtcDay(cs.v[i].ts);
    if (i == 0 || day != prevDay) {
      dOpen = (float)cs.v[i].o;
      if (i == 0 || d7UtcWeek(day) != d7UtcWeek(prevDay))
        wOpen = (float)cs.v[i].o;
      int month = d7UtcMonth(cs.v[i].ts);
      if (i == 0 || month != prevMonth) {
        mOpen = (float)cs.v[i].o;
        prevMonth = month;
      }
      prevDay = day;
    }
    dayOpen[i] = dOpen;
    weekOpen[i] = wOpen;
    monthOpen[i] = mOpen;
  }
}

inline bool d7UpdateLevelsLast(const CandleSeries& cs,
                              std::vector<float>& dayOpen,
                              std::vector<float>& weekOpen,
                              std::vector<float>& monthOpen) {
  size_t n = cs.v.size();
  if (n < 2 || dayOpen.size() != n || weekOpen.size() != n ||
      monthOpen.size() != n)
    return false;
  int64_t day = d7UtcDay(cs.v[n - 1].ts);
  int64_t prev = d7UtcDay(cs.v[n - 2].ts);
  const Candle& c = cs.v[n - 1];
  dayOpen[n - 1] = day != prev ? (float)c.o : dayOpen[n - 2];
  weekOpen[n - 1] =
      d7UtcWeek(day) != d7UtcWeek(prev) ? (float)c.o : weekOpen[n - 2];
  monthOpen[n - 1] = d7UtcMonth(c.ts) != d7UtcMonth(cs.v[n - 2].ts)
                         ? (float)c.o
                         : monthOpen[n - 2];
  return true;
}

inline float d7BarX(const ChartPane& pane, float i, float startF, float bw) {
  return pane.area.x + (i - startF) * bw + bw * 0.5f;
}

inline void d7FlushBand(DrawList& d, std::vector<float>& bx,
                       std::vector<float>& bTop, std::vector<float>& bBot,
                       Color fill) {
  if (bx.size() >= 2)
    d.seriesBand(bx.data(), bTop.data(), bBot.data(), (int)bx.size(), fill);
  bx.clear();
  bTop.clear();
  bBot.clear();
}

inline void d7PushBand(std::vector<float>& bx, std::vector<float>& bTop,
                      std::vector<float>& bBot, const ChartPane& pane, float x,
                      float hi, float lo) {
  bx.push_back(x);
  bTop.push_back(pane.yOf(hi));
  bBot.push_back(pane.yOf(lo));
}

inline void d7DrawBand(DrawList& d, const ChartPane& pane,
                      const std::vector<float>& a, const std::vector<float>& b,
                      int vis0, int vis1, float startF, float bw, Color up,
                      Color down) {
  static thread_local std::vector<float> bx, bTop, bBot;
  bx.clear();
  bTop.clear();
  bBot.clear();
  int cur = 0;
  auto xOf = [&](float i) { return d7BarX(pane, i, startF, bw); };
  auto flush = [&]() { d7FlushBand(d, bx, bTop, bBot, cur >= 0 ? up : down); };
  for (int i = vis0; i <= vis1 && i < (int)a.size() && i < (int)b.size(); ++i) {
    float av = a[(size_t)i], bv = b[(size_t)i];
    if (std::isnan(av) || std::isnan(bv)) {
      flush();
      cur = 0;
      continue;
    }
    int sign = av >= bv ? 1 : -1;
    if (cur && sign != cur) {
      if (i > vis0 && !std::isnan(a[(size_t)i - 1]) &&
          !std::isnan(b[(size_t)i - 1])) {
        float a0 = a[(size_t)i - 1], b0 = b[(size_t)i - 1];
        float den = (av - bv) - (a0 - b0);
        float t = std::fabs(den) < 1e-12f ? 0.5f : (b0 - a0) / den;
        t = std::clamp(t, 0.0f, 1.0f);
        float yc = 0.5f * (a0 + (av - a0) * t + b0 + (bv - b0) * t);
        float xc = xOf((float)(i - 1) + t);
        d7PushBand(bx, bTop, bBot, pane, xc, yc, yc);
        flush();
        d7PushBand(bx, bTop, bBot, pane, xc, yc, yc);
      } else {
        flush();
      }
    }
    cur = sign;
    d7PushBand(bx, bTop, bBot, pane, xOf((float)i), std::max(av, bv),
               std::min(av, bv));
  }
  flush();
}

inline void d7DrawShelves(DrawList& d, const ChartPane& pane,
                         const std::vector<float>& s, int vis0, int vis1,
                         float startF, float bw, Color c) {
  int run0 = -1;
  float yv = NAN;
  auto flush = [&](int i1) {
    if (run0 < 0 || i1 - run0 < 2) {
      run0 = -1;
      return;
    }
    float y = pane.yOf(yv);
    if (y < pane.area.y || y > pane.area.y + pane.area.h) {
      run0 = -1;
      return;
    }
    float x0 = d7BarX(pane, (float)run0, startF, bw);
    float x1 = d7BarX(pane, (float)i1, startF, bw);
    d.linePattern(x0, y, x1, y, c, 1.6f, DrawList::LineStyle::Dotted);
    d.circle(x0, y, 2.0f, c);
    d.circle(x1, y, 2.0f, c);
    run0 = -1;
  };
  for (int i = vis0; i <= vis1 && i < (int)s.size(); ++i) {
    float v = s[(size_t)i];
    if (std::isnan(v)) {
      flush(i - 1);
      continue;
    }
    if (run0 < 0) {
      run0 = i;
      yv = v;
      continue;
    }
    float eps = std::max(1e-4f, std::fabs(yv) * 1e-5f);
    if (std::fabs(v - yv) <= eps) continue;
    flush(i - 1);
    run0 = i;
    yv = v;
  }
  if (run0 >= 0) flush(std::min(vis1, (int)s.size() - 1));
}

inline void d7DrawStairBand(DrawList& d, const ChartPane& pane,
                           const std::vector<float>& ma,
                           const std::vector<float>& stair, int vis0, int vis1,
                           float startF, float bw, Color up, Color down) {
  static thread_local std::vector<float> bx, bTop, bBot;
  bx.clear();
  bTop.clear();
  bBot.clear();
  int cur = 0;
  float lastK = NAN;
  auto xOf = [&](float i) { return d7BarX(pane, i, startF, bw); };
  auto flush = [&]() { d7FlushBand(d, bx, bTop, bBot, cur >= 0 ? up : down); };
  for (int i = vis0; i <= vis1 && i < (int)ma.size() && i < (int)stair.size();
       ++i) {
    float av = ma[(size_t)i], kv = stair[(size_t)i];
    if (std::isnan(av) || std::isnan(kv)) {
      flush();
      cur = 0;
      lastK = NAN;
      continue;
    }
    int sign = av >= kv ? 1 : -1;
    float x = xOf((float)i);
    if (cur && sign != cur) {
      if (i > vis0 && !std::isnan(ma[(size_t)i - 1]) &&
          !std::isnan(stair[(size_t)i - 1])) {
        float a0 = ma[(size_t)i - 1], k0 = stair[(size_t)i - 1];
        float den = (av - kv) - (a0 - k0);
        float t = std::fabs(den) < 1e-12f ? 0.5f : (k0 - a0) / den;
        t = std::clamp(t, 0.0f, 1.0f);
        float yc = 0.5f * (a0 + (av - a0) * t + k0 + (kv - k0) * t);
        float xc = xOf((float)(i - 1) + t);
        d7PushBand(bx, bTop, bBot, pane, xc, yc, yc);
        flush();
        d7PushBand(bx, bTop, bBot, pane, xc, yc, yc);
      } else {
        flush();
      }
    } else if (!std::isnan(lastK) && std::fabs(kv - lastK) >
                                    std::max(1e-4f, std::fabs(lastK) * 1e-5f)) {
      // Hold the old shelf to this x, then step vertically so the wash's
      // outer edge is a stair, not a one-bar ramp.
      d7PushBand(bx, bTop, bBot, pane, x - 0.02f, std::max(av, lastK),
                 std::min(av, lastK));
    }
    cur = sign;
    lastK = kv;
    d7PushBand(bx, bTop, bBot, pane, x, std::max(av, kv), std::min(av, kv));
  }
  flush();
}

inline void d7DrawStairLine(DrawList& d, const ChartPane& pane,
                           const std::vector<float>& s, int vis0, int vis1,
                           float startF, float bw, Color c, float w) {
  static thread_local std::vector<float> xy;
  xy.clear();
  auto flush = [&]() {
    if (xy.size() >= 4) d.polyline(xy.data(), (int)(xy.size() / 2), c, w);
    xy.clear();
  };
  float last = NAN;
  for (int i = vis0; i <= vis1 && i < (int)s.size(); ++i) {
    float v = s[(size_t)i];
    if (std::isnan(v)) {
      flush();
      last = NAN;
      continue;
    }
    float x = d7BarX(pane, (float)i, startF, bw);
    float y = pane.yOf(v);
    if (!std::isnan(last) &&
        std::fabs(v - last) > std::max(1e-4f, std::fabs(last) * 1e-5f)) {
      xy.push_back(x);
      xy.push_back(pane.yOf(last));
    }
    xy.push_back(x);
    xy.push_back(y);
    last = v;
  }
  flush();
}

inline void d7DrawLead(DrawList& d, const ChartPane& pane,
                      const std::vector<float>& s, int vis0, int vis1,
                      float startF, float bw, Color c, float w) {
  static thread_local std::vector<float> xy;
  xy.clear();
  auto flush = [&]() {
    if (xy.size() >= 4) d.polyline(xy.data(), (int)(xy.size() / 2), c, w);
    xy.clear();
  };
  for (int i = vis0; i <= vis1 && i < (int)s.size(); ++i) {
    if (std::isnan(s[(size_t)i])) {
      flush();
      continue;
    }
    xy.push_back(d7BarX(pane, (float)i, startF, bw));
    xy.push_back(pane.yOf(s[(size_t)i]));
  }
  flush();
}

inline void d7DrawCloud(DrawList& d, const ChartPane& pane,
                       const CandleSeries& cs, const std::vector<float>& dLead,
                       const std::vector<float>& kLead,
                       const std::vector<float>& ma, int vis0, int vis1,
                       float startF, float bw, Color up, Color down, Color maC,
                       float thick, int innerLen) {
  (void)dLead;
  (void)ma;
  int nLen = innerLen > 1 ? std::max(2, innerLen / 2) : 56;
  static thread_local std::vector<float> emaA, emaB, emaC;
  d7SuperSmootherClose(cs, nLen, emaA);
  d7SuperSmootherClose(cs, nLen * 2, emaB);
  d7SuperSmootherClose(cs, nLen * 3, emaC);
  d7DrawBand(d, pane, emaA, emaB, vis0, vis1, startF, bw, withAlpha(up, 0.22f),
             withAlpha(down, 0.22f));
  d7DrawBand(d, pane, emaB, emaC, vis0, vis1, startF, bw, withAlpha(up, 0.14f),
             withAlpha(down, 0.14f));
  d7DrawStairBand(d, pane, emaA, kLead, vis0, vis1, startF, bw,
                  withAlpha(up, 0.18f), withAlpha(down, 0.18f));
  float tw = std::max(1.0f, thick - 0.5f);
  d7DrawLead(d, pane, emaA, vis0, vis1, startF, bw, withAlpha(up, 0.80f), thick);
  d7DrawLead(d, pane, emaB, vis0, vis1, startF, bw, withAlpha(up, 0.60f), tw);
  d7DrawLead(d, pane, emaC, vis0, vis1, startF, bw, withAlpha(maC, 0.80f), tw);

  Color edge = down;
  for (int i = vis1; i >= vis0 && i < (int)kLead.size() && i < (int)emaA.size();
       --i) {
    if (std::isnan(kLead[(size_t)i]) || std::isnan(emaA[(size_t)i])) continue;
    edge = kLead[(size_t)i] >= emaA[(size_t)i] ? down : up;
    break;
  }
  d7DrawShelves(d, pane, kLead, vis0, vis1, startF, bw, withAlpha(edge, 0.75f));
  d7DrawStairLine(d, pane, kLead, vis0, vis1, startF, bw, withAlpha(edge, 0.90f),
                  thick);
}

inline void d7DrawRsi(DrawList& d, const ChartPane& pane,
                     const std::vector<float>& rsi, const std::vector<float>& fast,
                     const std::vector<float>& slow, int vis0, int vis1,
                     float startF, float bw, Color lineC, Color up, Color down,
                     float thick, bool guides) {
  const Theme& th = theme();
  d.rect({pane.area.x, pane.yOf(50), pane.area.w, 1},
         withAlpha(th.textDim, 0.28f));
  if (guides)
    for (double g : {30.0, 70.0})
      d.rect({pane.area.x, pane.yOf(g), pane.area.w, 1},
             withAlpha(th.textDim, 0.30f));
  d7DrawBand(d, pane, rsi, fast, vis0, vis1, startF, bw, withAlpha(up, 0.22f),
             withAlpha(down, 0.22f));
  d7DrawBand(d, pane, fast, slow, vis0, vis1, startF, bw, withAlpha(up, 0.16f),
             withAlpha(down, 0.16f));
  d7DrawLead(d, pane, fast, vis0, vis1, startF, bw, withAlpha(up, 0.75f),
             std::max(1.0f, thick - 0.5f));
  d7DrawLead(d, pane, slow, vis0, vis1, startF, bw, withAlpha(down, 0.75f),
             std::max(1.0f, thick - 0.5f));
  d7DrawLead(d, pane, rsi, vis0, vis1, startF, bw, lineC, thick);
  for (int i = vis0 + 1; i <= vis1 && i < (int)fast.size() && i < (int)slow.size();
       ++i) {
    float a0 = fast[(size_t)i - 1] - slow[(size_t)i - 1];
    float a1 = fast[(size_t)i] - slow[(size_t)i];
    if (std::isnan(a0) || std::isnan(a1) || a0 * a1 >= 0.0f) continue;
    float x = pane.area.x + (i - startF) * bw + bw * 0.5f;
    float y = pane.yOf(0.5f * (fast[(size_t)i] + slow[(size_t)i]));
    Color mc = a1 > 0.0f ? up : down;
    d.circle(x, y, 2.4f, withAlpha(mc, 0.92f));
    d.circleOutline(x, y, 2.4f, withAlpha(mc, 0.55f), 1.0f);
  }
}

inline void d7DrawScore(DrawList& d, const ChartPane& pane,
                       const std::vector<int8_t>& dir, int vis0, int vis1,
                       float startF, float bw, Color up, Color down) {
  const Theme& th = theme();
  for (int i = vis0; i <= vis1 && i < (int)dir.size(); ++i) {
    int8_t s = dir[(size_t)i];
    Color c = s > 0 ? up : s < 0 ? down : th.textDim;
    float a = s == 0 ? 0.16f : 0.62f;
    float x = pane.area.x + (i - startF) * bw;
    float gap = std::max(0.0f, std::min(2.0f, bw * 0.12f));
    d.rect({x + gap, pane.area.y + 3.0f, std::max(1.0f, bw - gap * 2.0f),
            std::max(2.0f, pane.area.h - 6.0f)},
           withAlpha(c, a));
  }
}

inline void d7DrawLevel(DrawList& d, const ChartPane& pane, float y, Color c,
                       const char* label, const char* value) {
  if (y < pane.area.y || y > pane.area.y + pane.area.h) return;
  d.linePattern(pane.area.x, y, pane.area.x + pane.area.w, y, c, 1.0f,
                DrawList::LineStyle::Dashed);
  d.circle(pane.area.x + pane.area.w - 5.0f, y, 2.2f, c);
  char buf[40];
  snprintf(buf, sizeof(buf), "%s %s", label, value);
  d.textAligned({pane.area.x + pane.area.w - 92.0f, y - 8.0f, 88.0f, 16.0f},
                buf, c, DrawList::Right, 0, true);
}

inline void d7DrawLevels(DrawList& d, const ChartPane& pane,
                        const std::vector<float>& dayOpen,
                        const std::vector<float>& weekOpen,
                        const std::vector<float>& monthOpen, int mask,
                        bool prev, Color dayC, Color weekC, Color monthC) {
  auto last = [](const std::vector<float>& s) -> float {
    for (size_t i = s.size(); i-- > 0;)
      if (!std::isnan(s[i])) return s[i];
    return NAN;
  };
  auto prevVal = [](const std::vector<float>& s, float cur) -> float {
    for (size_t i = s.size(); i-- > 0;) {
      if (std::isnan(s[i])) continue;
      if (s[i] != cur) return s[i];
    }
    return NAN;
  };
  char vb[24];
  if (mask & 1) {
    float v = last(dayOpen);
    if (!std::isnan(v)) {
      chartFmtPrice(vb, sizeof(vb), v);
      d7DrawLevel(d, pane, pane.yOf(v), dayC, "D-O", vb);
    }
    if (prev) {
      float p = prevVal(dayOpen, v);
      if (!std::isnan(p)) {
        chartFmtPrice(vb, sizeof(vb), p);
        d7DrawLevel(d, pane, pane.yOf(p), withAlpha(dayC, 0.45f), "PD-O", vb);
      }
    }
  }
  if (mask & 2) {
    float v = last(weekOpen);
    if (!std::isnan(v)) {
      chartFmtPrice(vb, sizeof(vb), v);
      d7DrawLevel(d, pane, pane.yOf(v), weekC, "W-O", vb);
    }
    if (prev) {
      float p = prevVal(weekOpen, v);
      if (!std::isnan(p)) {
        chartFmtPrice(vb, sizeof(vb), p);
        d7DrawLevel(d, pane, pane.yOf(p), withAlpha(weekC, 0.45f), "PW-O", vb);
      }
    }
  }
  if (mask & 4) {
    float v = last(monthOpen);
    if (!std::isnan(v)) {
      chartFmtPrice(vb, sizeof(vb), v);
      d7DrawLevel(d, pane, pane.yOf(v), monthC, "M-O", vb);
    }
    if (prev) {
      float p = prevVal(monthOpen, v);
      if (!std::isnan(p)) {
        chartFmtPrice(vb, sizeof(vb), p);
        d7DrawLevel(d, pane, pane.yOf(p), withAlpha(monthC, 0.45f), "PM-O", vb);
      }
    }
  }
}
