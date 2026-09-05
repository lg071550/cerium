#pragma once

#include "d7_suite.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Market Cipher B (LazyBear WaveTrend + MFI area + confluence marks).
// Pine-accurate EMA: SMA seed of the first `length` valid samples, then
// k = 2/(length+1). Default channel/average/MA is 10/21/4; 9/12/3 is the
// later VuManChu retune, exposed as a settings preset.
//
// Marker language mirrors the original: a small dot rides every WaveTrend
// crossover, green "+" / red "x" crosses mark the ±53 extreme signals at the
// wave cross itself, diamonds flag money-flow divergence (accumulation into
// an oversold cross / distribution into an overbought cross), ringed diamonds
// mark regular price/WT divergences, and the gold/blood RSI confluences
// recolor the cross. Money flow hue is semantic (mint/red) and not paintable.

enum {
  CipherLayerMfi = 1,
  CipherLayerHist = 2,
  CipherLayerDots = 4,
  CipherLayerAll = CipherLayerMfi | CipherLayerHist | CipherLayerDots,
  // opt bits above the layer mask.
  CipherOptDiv = 256,   // regular-divergence diamonds
  CipherOptXover = 512  // small dots on mid-range wave crossovers
};

enum {
  CipherMarkCross = 1,
  CipherMarkUp = 2,
  CipherMarkBuy = 4,
  CipherMarkSell = 8,
  CipherMarkGold = 16,
  CipherMarkBlood = 32,
  CipherMarkDivBull = 64,
  CipherMarkDivBear = 128
};

// Market Cipher B's RSI leg is fixed at period 14 in the Pine original.
constexpr int kCipherRsiPeriod = 14;

inline int cipherLayers(int opt) { return opt ? opt : CipherLayerAll; }

inline double cipherHlc3(const Candle& c) { return (c.h + c.l + c.c) / 3.0; }

inline float cipherRsiOf(double g, double l) {
  return l == 0.0 ? 100.0f : (float)(100.0 - 100.0 / (1.0 + g / l));
}

inline int8_t cipherMark(float wt1, float wt2, float prev1, float prev2,
                         float rsi, float mfi) {
  if (std::isnan(wt1) || std::isnan(wt2) || std::isnan(prev1) ||
      std::isnan(prev2))
    return 0;
  float d0 = prev1 - prev2;
  float d1 = wt1 - wt2;
  bool crossUp = d0 <= 0.0f && d1 > 0.0f;
  bool crossDown = d0 >= 0.0f && d1 < 0.0f;
  // Original Market Cipher B paints a signal only when the wave cross lands
  // beyond the ±53 WaveTrend extremes; mid-range crossovers stay unmarked.
  if (crossUp && wt2 <= -53.0f) {
    int8_t m = (int8_t)(CipherMarkCross | CipherMarkUp | CipherMarkBuy);
    // Golden buy: buy confluence with RSI < 20 and negative money flow.
    if (!std::isnan(rsi) && !std::isnan(mfi) && rsi < 20.0f && mfi < 0.0f)
      m = (int8_t)(m | CipherMarkGold);
    return m;
  }
  if (crossDown && wt2 >= 53.0f) {
    int8_t m = (int8_t)(CipherMarkCross | CipherMarkSell);
    // Blood sell: sell confluence with RSI > 80 and positive money flow.
    if (!std::isnan(rsi) && !std::isnan(mfi) && rsi > 80.0f && mfi > 0.0f)
      m = (int8_t)(m | CipherMarkBlood);
    return m;
  }
  return 0;
}

// Regular divergences, price vs WaveTrend %B. Fractal pivots confirmed after
// `pad` bars on each side; the second pivot carries the mark. Bearish: price
// higher high + oscillator lower high inside momentum territory (>25);
// bullish mirrored under −25. Pivot separation capped so stale pairs die.
inline void cipherDivergences(const CandleSeries& cs,
                              const std::vector<float>& wt2,
                              std::vector<int8_t>& marks, int pad = 2,
                              int maxSpan = 60) {
  const size_t n = cs.v.size();
  if ((int)n < pad * 2 + 2 || wt2.size() != n || marks.size() != n) return;
  int lastHighPivot = -1, lastLowPivot = -1;
  for (size_t i = (size_t)pad; i + (size_t)pad < n; ++i) {
    const Candle& c = cs.v[i];
    bool ph = true, pl = true;
    for (size_t k = 1; k <= (size_t)pad; ++k) {
      if (!(c.h >= cs.v[i - k].h && c.h > cs.v[i + k].h)) ph = false;
      if (!(c.l <= cs.v[i - k].l && c.l < cs.v[i + k].l)) pl = false;
      if (!ph && !pl) break;
    }
    if (ph) {
      float wPrev = lastHighPivot >= 0 ? wt2[(size_t)lastHighPivot] : NAN;
      float wNow = wt2[i];
      if (lastHighPivot >= 0 && i - (size_t)lastHighPivot <= (size_t)maxSpan &&
          !std::isnan(wPrev) && !std::isnan(wNow) &&
          c.h > cs.v[(size_t)lastHighPivot].h && wNow < wPrev &&
          std::min(wNow, wPrev) > 25.0f)
        marks[i] |= CipherMarkDivBear;
      lastHighPivot = (int)i;
    }
    if (pl) {
      float wPrev = lastLowPivot >= 0 ? wt2[(size_t)lastLowPivot] : NAN;
      float wNow = wt2[i];
      if (lastLowPivot >= 0 && i - (size_t)lastLowPivot <= (size_t)maxSpan &&
          !std::isnan(wPrev) && !std::isnan(wNow) &&
          c.l < cs.v[(size_t)lastLowPivot].l && wNow > wPrev &&
          std::max(wNow, wPrev) < -25.0f)
        marks[i] |= CipherMarkDivBull;
      lastLowPivot = (int)i;
    }
  }
}

inline void cipherSma(const std::vector<float>& src, int period,
                     std::vector<float>& out) {
  size_t n = src.size();
  out.assign(n, NAN);
  if (period < 1) return;
  for (size_t i = (size_t)period - 1; i < n; ++i) {
    double sum = 0;
    bool ok = true;
    for (int k = 0; k < period; ++k) {
      float v = src[i - (size_t)(period - 1 - k)];
      if (std::isnan(v)) {
        ok = false;
        break;
      }
      sum += v;
    }
    if (ok) out[i] = (float)(sum / period);
  }
}

// Market Cipher B money flow (RSI-MFI): body position within the range
// (close−open)/(high−low), smoothed with SMA 60 and scaled ×190 — exactly
// the Pine default (mfi_input_mafn="SMA", period 60, scale 190). Previous
// builds averaged hlc3×volume via Wilder RSI, which produced a completely
// different shape and amplitude.
inline void cipherMfi(const CandleSeries& cs, std::vector<float>& out) {
  const int period = 60;
  const float scale = 190.0f;
  const size_t n = cs.v.size();
  out.assign(n, NAN);
  if ((int)n < period) return;
  for (size_t i = (size_t)period - 1; i < n; ++i) {
    double sum = 0;
    for (int k = 0; k < period; ++k) {
      const Candle& c = cs.v[i - (size_t)k];
      double denom = c.h - c.l;
      double v = (denom != 0.0) ? (c.c - c.o) / denom : 0.0;
      sum += v;
    }
    out[i] = (float)(sum / period * scale);
  }
}

// WaveTrend + MFI + marks. live* are n-2 seeds for the last-bar increment:
// esa, d, tci, RSI Wilder gain/loss, and MFI RMA up/down.
inline void cipherCompute(const CandleSeries& cs, int n1, int n2, int n3,
                          std::vector<float>& wt1, std::vector<float>& wt2,
                          std::vector<float>& mfi, std::vector<int8_t>& marks,
                          double& liveEsa, double& liveDe, double& liveTci,
                          double& liveGain, double& liveLoss,
                          double& liveMfiUp, double& liveMfiDown) {
  n1 = std::max(2, n1);
  n2 = std::max(2, n2);
  n3 = std::max(2, n3);
  size_t n = cs.v.size();
  wt1.assign(n, NAN);
  marks.assign(n, 0);
  liveEsa = liveDe = liveTci = liveGain = liveLoss = NAN;
  liveMfiUp = liveMfiDown = 0;
  if (n == 0) {
    wt2.assign(0, NAN);
    mfi.assign(0, NAN);
    return;
  }

  const double k1 = 2.0 / (n1 + 1);
  const double k2 = 2.0 / (n2 + 1);
  double esa = 0, esaSum = 0;
  int esaN = 0;
  bool esaOn = false;
  double de = 0, deSum = 0;
  int deN = 0;
  bool deOn = false;
  double tci = 0, tciSum = 0;
  int tciN = 0;
  bool tciOn = false;

  for (size_t i = 0; i < n; ++i) {
    double ap = cipherHlc3(cs.v[i]);
    if (!esaOn) {
      esaSum += ap;
      if (++esaN == n1) {
        esa = esaSum / n1;
        esaOn = true;
      }
    } else {
      esa += (ap - esa) * k1;
    }
    if (esaOn) {
      double ad = std::fabs(ap - esa);
      if (!deOn) {
        deSum += ad;
        if (++deN == n1) {
          de = deSum / n1;
          deOn = true;
        }
      } else {
        de += (ad - de) * k1;
      }
      if (deOn && de != 0.0) {
        double ci = (ap - esa) / (0.015 * de);
        if (!tciOn) {
          tciSum += ci;
          if (++tciN == n2) {
            tci = tciSum / n2;
            tciOn = true;
            wt1[i] = (float)tci;
          }
        } else {
          tci += (ci - tci) * k2;
          wt1[i] = (float)tci;
        }
      }
    }
    if (i + 2 == n) {
      liveEsa = esaOn ? esa : NAN;
      liveDe = deOn ? de : NAN;
      liveTci = tciOn ? tci : NAN;
    }
  }

  cipherSma(wt1, n3, wt2);
  cipherMfi(cs, mfi);
  liveMfiUp = liveMfiDown = 0;
  cipherDivergences(cs, wt2, marks, 2, 60);

  double gain = 0, loss = 0;
  const int rp = kCipherRsiPeriod;
  // Pre-seed the Wilder averages; smoothing starts after the seed window.
  // (The old inline accumulate-then-smooth loop did both in one pass.)
  bool rsiOn = wilderSeed(cs, rp, gain, loss);
  float rsi = NAN;
  if (rsiOn) rsi = cipherRsiOf(gain, loss);
  for (size_t i = 1; i < n; ++i) {
    double d = cs.v[i].c - cs.v[i - 1].c;
    if (rsiOn && (int)i > rp) {
      gain = (gain * (rp - 1) + (d > 0 ? d : 0)) / rp;
      loss = (loss * (rp - 1) + (d < 0 ? -d : 0)) / rp;
      rsi = cipherRsiOf(gain, loss);
    }
    if (i + 2 == n) {
      liveGain = rsiOn ? gain : NAN;
      liveLoss = rsiOn ? loss : NAN;
    }
    marks[i] = cipherMark(wt1[i], wt2[i], wt1[i - 1], wt2[i - 1], rsi,
                          i < mfi.size() ? mfi[i] : NAN);
  }
}

inline bool cipherUpdateLast(const CandleSeries& cs, int n1, int n2, int n3,
                             std::vector<float>& wt1, std::vector<float>& wt2,
                             std::vector<float>& mfi, std::vector<int8_t>& marks,
                             double esa, double de, double tci, double gain,
                             double loss, double& mfiUp, double& mfiDown) {
  n1 = std::max(2, n1);
  n2 = std::max(2, n2);
  n3 = std::max(2, n3);
  size_t n = cs.v.size();
  if (n < 2 || wt1.size() != n || wt2.size() != n || mfi.size() != n ||
      marks.size() != n)
    return false;
  // Body-position MFI is SMA-based; recompute the last bar from its 60-bar
  // window (trivial cost, no persistent RMA state to drift).
  (void)mfiUp;
  (void)mfiDown;
  if (n >= 60) {
    double sum = 0;
    for (size_t k = n - 60; k < n; ++k) {
      const Candle& c = cs.v[k];
      double denom = c.h - c.l;
      double v = (denom != 0.0) ? (c.c - c.o) / denom : 0.0;
      sum += v;
    }
    mfi[n - 1] = (float)(sum / 60.0 * 190.0);
  } else {
    mfi[n - 1] = NAN;
  }
  if (!std::isfinite(esa) || !std::isfinite(de) || !std::isfinite(tci)) {
    wt1[n - 1] = NAN;
    wt2[n - 1] = NAN;
    marks[n - 1] = 0;
    return true;
  }

  const Candle& c = cs.v[n - 1];
  double ap = cipherHlc3(c);
  double k1 = 2.0 / (n1 + 1);
  double k2 = 2.0 / (n2 + 1);
  esa += (ap - esa) * k1;
  de += (std::fabs(ap - esa) - de) * k1;
  if (de == 0.0) {
    wt1[n - 1] = NAN;
    wt2[n - 1] = NAN;
    marks[n - 1] = 0;
    return true;
  }
  double ci = (ap - esa) / (0.015 * de);
  tci += (ci - tci) * k2;
  wt1[n - 1] = (float)tci;

  double sum = 0;
  bool ok = true;
  if (n < (size_t)n3) ok = false;
  else {
    for (int k = 0; k < n3; ++k) {
      float v = wt1[n - (size_t)n3 + (size_t)k];
      if (std::isnan(v)) {
        ok = false;
        break;
      }
      sum += v;
    }
  }
  wt2[n - 1] = ok ? (float)(sum / n3) : NAN;

  float rsi = NAN;
  if (std::isfinite(gain) && std::isfinite(loss)) {
    double d = c.c - cs.v[n - 2].c;
    double g = (gain * 13.0 + (d > 0 ? d : 0)) / 14.0;
    double l = (loss * 13.0 + (d < 0 ? -d : 0)) / 14.0;
    rsi = cipherRsiOf(g, l);
  }
  marks[n - 1] =
      cipherMark(wt1[n - 1], wt2[n - 1], wt1[n - 2], wt2[n - 2], rsi,
                 mfi[n - 1]);
  return true;
}

inline void cipherDiamond(DrawList& d, float x, float y, float r, Color c) {
  if (r <= 0.5f) return;
  d.trapezoid(x - r, y, y, x, y - r, y + r, c);
  d.trapezoid(x, y - r, y + r, x + r, y, y, c);
}

inline void cipherCross(DrawList& d, float x, float y, float s, Color c,
                       float t) {
  if (s <= 0.5f) return;
  d.line(x - s, y - s, x + s, y + s, c, t);
  d.line(x - s, y + s, x + s, y - s, c, t);
}

inline void cipherPlus(DrawList& d, float x, float y, float s, Color c,
                       float t) {
  if (s <= 0.5f) return;
  d.line(x - s, y, x + s, y, c, t);
  d.line(x, y - s, x, y + s, c, t);
}

// Stroke along a series, split at the zero crossing so each side carries its
// own hue — used as the crisp boundary of the money-flow area fill.
inline void cipherEdgeLine(DrawList& d, const ChartPane& pane,
                           const std::vector<float>& s, int vis0, int vis1,
                           float startF, float bw, Color up, Color down,
                           float w) {
  static thread_local std::vector<float> xy;
  xy.clear();
  int cur = 0;
  auto flush = [&]() {
    if (xy.size() >= 4)
      d.polyline(xy.data(), (int)(xy.size() / 2), cur >= 0 ? up : down, w);
    xy.clear();
  };
  for (int i = vis0; i <= vis1 && i < (int)s.size(); ++i) {
    float v = s[(size_t)i];
    if (std::isnan(v)) {
      flush();
      cur = 0;
      continue;
    }
    int sign = v >= 0.0f ? 1 : -1;
    if (cur && sign != cur) {
      float p = s[(size_t)i - 1];
      float cx = NAN;
      if (!std::isnan(p)) {
        float t = std::fabs(v - p) < 1e-12f ? 0.5f : (0.0f - p) / (v - p);
        t = std::clamp(t, 0.0f, 1.0f);
        cx = d7BarX(pane, (float)(i - 1) + t, startF, bw);
      }
      flush();
      if (!std::isnan(cx)) {
        xy.push_back(cx);
        xy.push_back(pane.yOf(0.0f));
      }
    }
    cur = sign;
    xy.push_back(d7BarX(pane, (float)i, startF, bw));
    xy.push_back(pane.yOf(v));
  }
  flush();
}

inline void cipherDraw(DrawList& d, const ChartPane& pane,
                       const std::vector<float>& wt1, const std::vector<float>& wt2,
                       const std::vector<float>& mfi, const std::vector<int8_t>& marks,
                       int vis0, int vis1, float startF, float bw, Color wt1C,
                       Color wt2C, float thick, bool guides, int opt) {
  const Theme& th = theme();
  int layers = cipherLayers(opt);
  bool divOn = (opt & CipherOptDiv) != 0;
  Color up = th.green;
  Color down = th.red;
  Color gold = hexColor(0xe6b84d);
  static thread_local std::vector<float> zero, vwap;
  size_t zN = std::max(wt1.size(), mfi.size());
  if (zero.size() != zN) zero.assign(zN, 0.0f);

  float y0 = pane.yOf(0);
  float yOb = pane.yOf(60);
  float yOs = pane.yOf(-60);
  float top = pane.area.y;
  float bot = pane.area.y + pane.area.h;
  Color well = withAlpha(th.textDim, 0.07f);
  if (yOb > top)
    d.rect({pane.area.x, top, pane.area.w, std::min(yOb, bot) - top}, well);
  if (yOs < bot) {
    float y = std::max(yOs, top);
    d.rect({pane.area.x, y, pane.area.w, bot - y}, well);
  }

  d.rect({pane.area.x, y0, pane.area.w, 1}, withAlpha(th.textDim, 0.42f));
  if (guides) {
    for (double g : {53.0, -53.0})
      d.rect({pane.area.x, pane.yOf(g), pane.area.w, 1},
             withAlpha(th.textDim, 0.16f));
    for (double g : {60.0, -60.0})
      d.rect({pane.area.x, pane.yOf(g), pane.area.w, 1},
             withAlpha(th.textDim, 0.38f));
  }

  Color wave = withAlpha(wt1C, 0.16f);
  d7DrawBand(d, pane, wt1, zero, vis0, vis1, startF, bw, wave, wave);
  if (layers & CipherLayerMfi)
    // Money flow hue is semantic and deliberately not user-paintable: mint
    // while positive, red while negative, so the area reads directionally.
    d7DrawBand(d, pane, mfi, zero, vis0, vis1, startF, bw,
               withAlpha(up, 0.40f), withAlpha(down, 0.42f));
  d7DrawBand(d, pane, wt1, wt2, vis0, vis1, startF, bw, withAlpha(wt1C, 0.32f),
             withAlpha(wt2C, 0.28f));

  if (layers & CipherLayerHist) {
    // d7DrawBand only reads [vis0..vis1] (its crossover probe at i>vis0 reads
    // i-1, still inside the window), so fill just the visible slice instead
    // of re-differencing the full series every frame. Stale entries beyond
    // the window are never read.
    const size_t n = std::min(wt1.size(), wt2.size());
    if (vwap.size() < n) vwap.resize(n);
    for (int i = std::max(0, vis0); i <= vis1 && i < (int)n; ++i) {
      float a = wt1[(size_t)i], b = wt2[(size_t)i];
      vwap[(size_t)i] = (std::isnan(a) || std::isnan(b)) ? NAN : a - b;
    }
    Color hc = withAlpha(wt2C, 0.30f);
    d7DrawBand(d, pane, vwap, zero, vis0, vis1, startF, bw, hc, hc);
  }

  if (layers & CipherLayerMfi)
    // Crisp boundary on top of the fills, under the wave leads.
    cipherEdgeLine(d, pane, mfi, vis0, vis1, startF, bw, withAlpha(up, 0.90f),
                   withAlpha(down, 0.90f), 1.25f);

  d7DrawLead(d, pane, wt2, vis0, vis1, startF, bw, wt2C,
             std::max(1.0f, thick - 0.5f));
  d7DrawLead(d, pane, wt1, vis0, vis1, startF, bw, wt1C, thick + 0.25f);

  if (!(layers & CipherLayerDots)) return;
  d.breakCmd();
  // Marker geometry rides the waves, not fixed rails: crosses and diamonds
  // sit at the WaveTrend cross point, sized from the bar width.
  const float xs = std::clamp(bw * 0.95f, 4.6f, 7.6f);  // signal cross arm
  const float dr = std::clamp(bw * 0.60f, 4.2f, 6.4f);  // diamond radius
  const float xr = std::clamp(bw * 0.32f, 2.2f, 3.2f);  // crossover dot radius
  const float ct = 1.6f;                                // cross stroke
  const Color casing = withAlpha(th.chartPaneBg, 0.88f);
  const float margin = std::max(xs, dr) + 2.0f;
  const float yLo = std::min(top + margin, 0.5f * (top + bot));
  const float yHi = std::max(bot - margin, 0.5f * (top + bot));
  float prevD = NAN;
  for (int i = vis0; i <= vis1 && i < (int)wt1.size() && i < (int)wt2.size();
       ++i) {
    const float a = wt1[(size_t)i], b = wt2[(size_t)i];
    const float d1 = (std::isnan(a) || std::isnan(b)) ? NAN : a - b;
    bool xup = false, xdn = false;
    if (!std::isnan(d1) && !std::isnan(prevD)) {
      xup = prevD <= 0.0f && d1 > 0.0f;
      xdn = prevD >= 0.0f && d1 < 0.0f;
    }
    if (!std::isnan(d1)) prevD = d1;

    const int8_t m =
        i < (int)marks.size() ? marks[(size_t)i] : (int8_t)0;
    const bool buy = (m & CipherMarkBuy) != 0;
    const bool sell = (m & CipherMarkSell) != 0;

    // Mid-range wave crossovers: small direction dot at the intersection.
    if ((xup || xdn) && !buy && !sell && (opt & CipherOptXover) &&
        !std::isnan(d1)) {
      float x = d7BarX(pane, (float)i, startF, bw);
      float y = std::clamp(pane.yOf(0.5f * (a + b)), yLo, yHi);
      d.circle(x, y, xr + 1.1f, casing);
      d.circle(x, y, xr, withAlpha(xdn ? down : up, 0.95f));
    }
    if (!buy && !sell) continue;

    const float x = d7BarX(pane, (float)i, startF, bw);
    if (std::isnan(a) || std::isnan(b)) continue;
    const float yc = std::clamp(pane.yOf(0.5f * (a + b)), yLo, yHi);
    const bool golden = (m & CipherMarkGold) != 0;
    const bool blood = (m & CipherMarkBlood) != 0;

    // Extreme WaveTrend signals: green "+" buys, red "x" sells, drawn at the
    // cross itself with a dark casing so they read over the waves. The RSI
    // confluences recolor them (gold buy / blood diamond sell).
    if (buy) {
      cipherPlus(d, x, yc, xs, casing, ct + 1.4f);
      cipherPlus(d, x, yc, xs, withAlpha(golden ? gold : up, 1.0f), ct);
    } else if (blood) {
      cipherDiamond(d, x, yc, xs * 1.15f + 1.2f, casing);
      cipherDiamond(d, x, yc, xs * 1.15f, withAlpha(down, 0.95f));
      cipherCross(d, x, yc, xs * 0.55f, withAlpha(hexColor(0xffffff), 0.90f),
                  1.4f);
    } else {
      cipherCross(d, x, yc, xs, casing, ct + 1.4f);
      cipherCross(d, x, yc, xs, withAlpha(down, 1.0f), ct);
    }

    // Money-flow diamonds: the wave extreme disagrees with flow — mint
    // diamond under a buy when money flow is already positive (accumulation),
    // red diamond over a sell when it is already negative (distribution).
    const float mf = i < (int)mfi.size() ? mfi[(size_t)i] : NAN;
    if (!std::isnan(mf) && (buy ? mf > 0.0f : mf < 0.0f)) {
      const float yd =
          buy ? std::clamp(yc + dr + 5.5f, yLo, yHi)
              : std::clamp(yc - dr - 5.5f, yLo, yHi);
      cipherDiamond(d, x, yd, dr + 1.3f, casing);
      cipherDiamond(d, x, yd, dr, withAlpha(buy ? up : down, 0.95f));
    }

    // Regular divergences ride the wave point itself (opt-gated).
    if (!divOn) continue;
    const bool divBull = (m & CipherMarkDivBull) != 0;
    const bool divBear = (m & CipherMarkDivBear) != 0;
    if ((divBull || divBear) && !std::isnan(b)) {
      const float rr = std::max(3.0f, dr * 0.80f);
      float yDiv =
          std::clamp(pane.yOf(b) + (divBear ? -(dr + 4.0f) : (dr + 4.0f)),
                     yLo, yHi);
      cipherDiamond(d, x, yDiv, rr, withAlpha(divBear ? down : up, 0.95f));
      cipherDiamond(d, x, yDiv, std::max(1.6f, rr - 2.0f),
                    withAlpha(th.panel, 0.85f));
      cipherDiamond(d, x, yDiv, std::max(0.8f, rr - 3.6f),
                    withAlpha(divBear ? down : up, 0.95f));
    }
  }
}
