#include "chart_panel.h"
#include "../symbols.h"
#include "cipher_b.h"
#include "d7_suite.h"

#include "../flow_sources.h"
#include "../../data/feeds.h"
#include "../../data/heatmap.h"
#include "../../platform/shell.h"
#include "../../ui/theme.h"
#include "../../ui/ui_context.h"
#include "../../ui/widgets.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <limits>
#include <unordered_map>

static constexpr float kChartGutters[] = {56.0f, 72.0f, 88.0f};
static constexpr float kChartLineWidths[] = {1.0f, 1.5f, 2.0f, 2.5f};
static constexpr float kLastPriceLineWidths[] = {1.0f, 1.5f, 2.0f};
static constexpr float kChartVolumeAlpha[] = {0.18f, 0.30f, 0.46f};
static constexpr float kBollDeviations[] = {1.5f, 2.0f, 2.5f, 3.0f};
static constexpr float kStMultipliers[] = {1.5f, 2.0f, 3.0f, 4.0f};
static constexpr const char* kChartTypeNames[] = {"CANDLES", "CLUSTER", "PROFILE", "TPO"};

// Mode-specific bar spacing defaults (presentation only, not a data limit).
static float barWidthDefault(int chartType) {
  if (chartType == 0) return 7.0f;
  if (chartType == 1 || chartType == 2) return 18.0f;
  return 5.0f;
}

static float barWidthMax(int chartType) {
  if (chartType == 1 || chartType == 2) return 120.0f;
  if (chartType == 3) return 16.0f;
  return 28.0f;
}
enum { CandleSolid = 0, CandleHollow = 1, CandleGhost = 2 };

static void drawOhlcCandle(DrawList& d, float x, float bw, float yO, float yC,
                           float yH, float yL, Color col, bool bull, bool wicks,
                           int body) {
  float cx = x + bw * 0.5f;
  if (wicks) d.line(cx, yH, cx, yL, withAlpha(col, 0.88f), 1.0f);

  float top = std::min(yO, yC);
  float h = std::max(std::fabs(yO - yC), 1.0f);
  Rect bodyR{x + bw * 0.15f, top, std::max(1.0f, bw * 0.7f), h};

  if (body == CandleGhost) {
    d.rect(bodyR, withAlpha(col, bodyR.w < 1.8f ? 0.20f : 0.14f));
    if (h > 2.0f) d.rectOutline(bodyR, withAlpha(col, 0.72f), 1.0f);
    return;
  }
  if (bodyR.w < 1.8f || body == CandleSolid) {
      d.rect(bodyR, withAlpha(col, 0.90f));
     return;
  }
  if (body == CandleHollow) {
    if (bull && h > 2.0f) {
      d.rectOutline(bodyR, withAlpha(col, 0.92f), 1.0f);
    } else {
      d.rect(bodyR, withAlpha(col, 0.88f));
    }
    return;
  }
  d.rect(bodyR, withAlpha(col, 0.14f));
  if (h > 2.0f) d.rectOutline(bodyR, withAlpha(col, 0.72f), 1.0f);
}

static double niceStep(double raw) {
  if (!(raw > 0)) return 1.0;
  double mag = std::pow(10.0, std::floor(std::log10(raw)));
  for (double mult : {1.0, 2.0, 2.5, 5.0, 10.0})
   if (mult * mag >= raw) return mult * mag;
  return 10.0 * mag;
}

// Heat resolution: slider % → pixel height of one display row.
// 100 = 1px (finest, ~one row per pane pixel), 0 = 8px. Purely a display
// setting — the lattice bin never depends on it or on the viewport, so
// resolution changes re-render without touching captured history.
static int heatPxPerRow(int sel) {
  int step = std::clamp(
      (int)std::lround(std::clamp(sel, 0, 100) * 3.0 / 100.0), 0, 3);
  return 8 >> step;
}
// Min clamp: hides cells under this USD notional (0 = off). Log $100..$1M.
static double heatMinUsdForSel(int sel) {
  sel = std::clamp(sel, 0, 100);
  return sel <= 0 ? 0.0 : std::pow(10.0, 2.0 + (double)sel * 0.04);
}
// Max clamp: the full-heat ceiling; 100 = AUTO (near-book P90). Log $10K..$50M.
static double heatMaxUsdForSel(int sel) {
  sel = std::clamp(sel, 0, 100);
  return sel >= 100 ? 0.0 : std::pow(10.0, 4.0 + (double)sel * 0.0375);
}
// Manual price bin (slider %, 0 = AUTO). Log sweep ~$0.055..$550, snapped to
// nice steps so the lattice and the value readout stay clean.
static double heatBinUsdForSel(int sel) {
  sel = std::clamp(sel, 1, 100);
  return niceStep(std::pow(10.0, -1.25 + (double)sel * 0.04));
}

static void sampleBookHeat(HeatmapSeries& hs, const CandleSeries& cs,
                           const Feeds& feeds, uint32_t mask, int binSel) {
  if (hs.sym != feeds.symbol || hs.tfKind != (int)cs.tf.kind ||
      hs.tfValue != cs.tf.value) {
    hs.clear();
    hs.sym = feeds.symbol;
    hs.tfKind = (int)cs.tf.kind;
    hs.tfValue = cs.tf.value;
  }
  hs.syncToCandles(cs);
  double mid = feeds.aggMid();
  if (cs.v.empty() || !(mid > 0)) return;
  double bin = 0;
  if (binSel > 0) bin = heatBinUsdForSel(binSel);
  else {
    // Full-book floor: coarse enough that the mid ±50% capture band always
    // fits kMaxRows — far-book walls stay rendered at any zoom.
    bin = niceStep(mid / (double)HeatmapSeries::kMaxRows);
    const double tick = hs.nativeTickFor(feeds, mask);
    if (tick > bin) bin = niceStep(tick);
    if (hs.bin > 0 && bin >= hs.bin / 1.5 && bin <= hs.bin * 1.5) bin = hs.bin;
  }
  hs.setBin(bin);
  // Only the live column is sampled from the current book. Walking empty
  // history here (the old stall-fill) painted the live book into every new
  // TF's columns — at native tick that's kMaxRows × N bars in one frame and
  // froze the pump so the 3m kline payload never even presented.
  if (feeds.booksVersion() != hs.sampledBooks || hs.bars.back().heat.empty()) {
    hs.sampledBooks = feeds.booksVersion();
    const size_t last = cs.v.size() - 1;
    if (last > 0 && hs.sampledTs > 0 && hs.bars[last - 1].heat.empty() &&
        hs.bars[last - 1].ts > hs.sampledTs)
      hs.sample((int)last - 1, feeds, mask, mid);
    hs.sample((int)last, feeds, mask, mid);
    hs.sampledTs = cs.v[last].ts;
  }
}

static void formatFootprint(char* out, size_t n, double value) {
  double a = std::fabs(value);
  if (a >= 1e6) snprintf(out, n, "%.1fM", value / 1e6);
  else if (a >= 1e3) snprintf(out, n, "%.1fk", value / 1e3);
  else if (a >= 100) snprintf(out, n, "%.0f", value);
  else if (a >= 10) snprintf(out, n, "%.1f", value);
  else if (a >= 1) snprintf(out, n, "%.2f", value);
  else snprintf(out, n, "%.3f", value);
}

static void drawSettingsGlyph(DrawList& draw, Rect r, Color color) {
  float x = r.x + 4.0f, w = std::max(6.0f, r.w - 8.0f);
  float y0 = r.y + r.h * 0.30f, y1 = r.y + r.h * 0.50f, y2 = r.y + r.h * 0.70f;
  draw.rect({x, y0, w, 1}, color);
  draw.rect({x, y1, w, 1}, color);
  draw.rect({x, y2, w, 1}, color);
  draw.rect({x + w * 0.62f, y0 - 1.5f, 2, 4}, color);
  draw.rect({x + w * 0.28f, y1 - 1.5f, 2, 4}, color);
  draw.rect({x + w * 0.72f, y2 - 1.5f, 2, 4}, color);
}

static void legendActionRects(float textX, float textW, float y, float h, float rightLimit,
                              Rect& settings, Rect& remove) {
  const float btnW = 18.0f;
  float x = std::min(rightLimit - 2.0f * btnW, textX + std::max(0.0f, textW));
  settings = {x, y, btnW, h};
  remove = {x + btnW, y, btnW, h};
}

static void emaSeries(const CandleSeries& cs, int period, std::vector<float>& out) {
  size_t n = cs.v.size();
  out.assign(n, NAN);
  if (n < (size_t)period) return;
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

static void smaSeries(const CandleSeries& cs, int period, std::vector<float>& out) {
  size_t n = cs.v.size();
  out.assign(n, NAN);
  if (n < (size_t)period) return;
  double sum = 0;
  for (int i = 0; i < period; ++i) sum += cs.v[(size_t)i].c;
  out[(size_t)period - 1] = (float)(sum / period);
  for (size_t i = (size_t)period; i < n; ++i) {
    sum += cs.v[i].c - cs.v[i - (size_t)period].c;
    out[i] = (float)(sum / period);
  }
}

// Bollinger basis + sigma bands in one pass, NaN-masked like the basis.
static void bollBandsSeries(const CandleSeries& cs, int period, float deviation,
                            std::vector<float>& basis, std::vector<float>& top,
                            std::vector<float>& bot) {
  smaSeries(cs, period, basis);
  size_t n = cs.v.size();
  top.assign(n, NAN);
  bot.assign(n, NAN);
  for (size_t i = 0; i < n; ++i) {
    float b = basis[i];
    if (std::isnan(b) || (int)i + 1 < period) continue;
    double var = 0;
    for (size_t j = i + 1 - (size_t)period; j <= i; ++j) {
      double d = cs.v[j].c - b;
      var += d * d;
    }
    double sd = std::sqrt(var / period);
    top[i] = (float)(b + deviation * sd);
    bot[i] = (float)(b - deviation * sd);
  }
}

static void rsiSeries(const CandleSeries& cs, int p, std::vector<float>& out) {
  wilderRsiSeries(cs, p, out);
}

static void macdSeries(const CandleSeries& cs, int fast, int slow,
                       std::vector<float>& out) {
  size_t n = cs.v.size();
  out.assign(n, NAN);
  if (n < (size_t)slow) return;
  double eFast = cs.v[0].c, eSlow = cs.v[0].c;
  const double kFast = 2.0 / (fast + 1), kSlow = 2.0 / (slow + 1);
  for (size_t i = 1; i < n; ++i) {
    eFast += (cs.v[i].c - eFast) * kFast;
    eSlow += (cs.v[i].c - eSlow) * kSlow;
    if (i + 1 >= (size_t)slow) out[i] = (float)(eFast - eSlow);
  }
}

static double barVolume(const Candle& c) {
  return c.aggVol > 0 ? c.aggVol : c.vol;
}

static double trueRange(const Candle& c, double prevC) {
  return std::max(c.h - c.l, std::max(std::fabs(c.h - prevC), std::fabs(c.l - prevC)));
}

static int64_t utcDay(double tsMs) { return (int64_t)std::floor(tsMs / 86400000.0); }

// TPO shows the latest few UTC sessions; profile column count scales to the
// pane. Shared by the price-range scan and the TPO session builder so both
// agree on how many sessions exist.
static inline int tpoMaxSessions(float paneW) {
  return paneW >= 760 ? 4 : paneW >= 460 ? 3 : 2;
}

// First bar of the visible TPO window: walk back from `vis1` across whole
// UTC days until one more session than the pane can show has been crossed.
static inline int tpoWindowFirst(const CandleSeries& cs, int vis1,
                                 int maxSessions) {
  int first = vis1;
  int64_t lastDay = -1;
  int days = 0;
  for (int i = vis1; i >= 0; --i) {
    int64_t day = utcDay(cs.v[(size_t)i].ts);
    if (day != lastDay) {
      lastDay = day;
      if (++days > maxSessions) return i + 1;
    }
    first = i;
  }
  return first;
}

static void atrSeries(const CandleSeries& cs, int period, std::vector<float>& out) {
  size_t n = cs.v.size();
  out.assign(n, NAN);
  if (n < (size_t)period || period < 1) return;
  double sum = 0;
  for (int i = 0; i < period; ++i) {
    double tr = i == 0 ? cs.v[(size_t)i].h - cs.v[(size_t)i].l
                       : trueRange(cs.v[(size_t)i], cs.v[(size_t)i - 1].c);
    sum += tr;
  }
  double atr = sum / period;
  out[(size_t)period - 1] = (float)atr;
  for (size_t i = (size_t)period; i < n; ++i) {
    atr = (atr * (period - 1) + trueRange(cs.v[i], cs.v[i - 1].c)) / period;
    out[i] = (float)atr;
  }
}

static void vwapSeries(const CandleSeries& cs, std::vector<float>& out,
                       std::vector<float>& upper, std::vector<float>& lower) {
  size_t n = cs.v.size();
  out.assign(n, NAN);
  upper.assign(n, NAN);
  lower.assign(n, NAN);
  double pv = 0, vol = 0, pv2 = 0;
  int64_t day = std::numeric_limits<int64_t>::min();
  for (size_t i = 0; i < n; ++i) {
    int64_t d = utcDay(cs.v[i].ts);
    if (d != day) {
      pv = vol = pv2 = 0;
      day = d;
    }
    double tp = (cs.v[i].h + cs.v[i].l + cs.v[i].c) / 3.0;
    double v = barVolume(cs.v[i]);
    pv += tp * v;
    pv2 += tp * tp * v;
    vol += v;
    if (!(vol > 0)) continue;
    double vwap = pv / vol;
    double sd = std::sqrt(std::max(0.0, pv2 / vol - vwap * vwap));
    out[i] = (float)vwap;
    upper[i] = (float)(vwap + sd);
    lower[i] = (float)(vwap - sd);
  }
}

static void supertrendSeries(const CandleSeries& cs, int period, float mult,
                             std::vector<float>& out, std::vector<int8_t>& dir) {
  size_t n = cs.v.size();
  out.assign(n, NAN);
  dir.assign(n, 0);
  if (n < (size_t)period || period < 1 || !(mult > 0)) return;
  std::vector<float> atr;
  atrSeries(cs, period, atr);
  double finalUp = 0, finalDn = 0;
  int trend = 1;
  for (size_t i = (size_t)period - 1; i < n; ++i) {
    if (std::isnan(atr[i])) continue;
    double hl2 = (cs.v[i].h + cs.v[i].l) * 0.5;
    double up = hl2 + mult * atr[i];
    double dn = hl2 - mult * atr[i];
    if (i == (size_t)period - 1) {
      finalUp = up;
      finalDn = dn;
      trend = cs.v[i].c <= finalUp ? -1 : 1;
    } else {
      double prevUp = finalUp, prevDn = finalDn;
      finalUp = cs.v[i - 1].c > prevUp ? std::min(up, prevUp) : up;
      finalDn = cs.v[i - 1].c < prevDn ? std::max(dn, prevDn) : dn;
      if (trend == -1 && cs.v[i].c > prevUp) trend = 1;
      else if (trend == 1 && cs.v[i].c < prevDn) trend = -1;
    }
    out[i] = (float)(trend == 1 ? finalDn : finalUp);
    dir[i] = (int8_t)trend;
  }
}

static void stochSeries(const CandleSeries& cs, int kPeriod, int smooth, int dPeriod,
                        std::vector<float>& kOut, std::vector<float>& dOut) {
  size_t n = cs.v.size();
  kOut.assign(n, NAN);
  dOut.assign(n, NAN);
  if (n < (size_t)kPeriod || kPeriod < 1 || smooth < 1 || dPeriod < 1) return;
  std::vector<float> raw(n, NAN);
  for (size_t i = (size_t)kPeriod - 1; i < n; ++i) {
    double hh = -1e300, ll = 1e300;
    for (size_t j = i + 1 - (size_t)kPeriod; j <= i; ++j) {
      hh = std::max(hh, cs.v[j].h);
      ll = std::min(ll, cs.v[j].l);
    }
    raw[i] = hh == ll ? 50.0f : (float)(100.0 * (cs.v[i].c - ll) / (hh - ll));
  }
  auto smaAt = [&](const std::vector<float>& src, int period, size_t i, float& dest) {
    if (i + 1 < (size_t)period) return false;
    double sum = 0;
    for (int j = 0; j < period; ++j) {
      float v = src[i - (size_t)j];
      if (std::isnan(v)) return false;
      sum += v;
    }
    dest = (float)(sum / period);
    return true;
  };
  for (size_t i = 0; i < n; ++i) {
    float k = NAN;
    if (!smaAt(raw, smooth, i, k)) continue;
    kOut[i] = k;
    float d = NAN;
    if (smaAt(kOut, dPeriod, i, d)) dOut[i] = d;
  }
}

static void obvSeries(const CandleSeries& cs, std::vector<float>& out) {
  size_t n = cs.v.size();
  out.assign(n, NAN);
  if (n == 0) return;
  double acc = barVolume(cs.v[0]);
  out[0] = (float)acc;
  for (size_t i = 1; i < n; ++i) {
    double v = barVolume(cs.v[i]);
    if (cs.v[i].c > cs.v[i - 1].c) acc += v;
    else if (cs.v[i].c < cs.v[i - 1].c) acc -= v;
    out[i] = (float)acc;
  }
}

static void adxSeries(const CandleSeries& cs, int period, std::vector<float>& adx,
                      std::vector<float>& plusDi, std::vector<float>& minusDi) {
  size_t n = cs.v.size();
  adx.assign(n, NAN);
  plusDi.assign(n, NAN);
  minusDi.assign(n, NAN);
  if (n < (size_t)period + 1 || period < 1) return;
  double smTr = 0, smP = 0, smM = 0;
  std::vector<float> dx(n, NAN);
  for (size_t i = 1; i < n; ++i) {
    double upMove = cs.v[i].h - cs.v[i - 1].h;
    double downMove = cs.v[i - 1].l - cs.v[i].l;
    double plusDM = upMove > downMove && upMove > 0 ? upMove : 0;
    double minusDM = downMove > upMove && downMove > 0 ? downMove : 0;
    double tr = trueRange(cs.v[i], cs.v[i - 1].c);
    if (i <= (size_t)period) {
      smTr += tr;
      smP += plusDM;
      smM += minusDM;
      if (i < (size_t)period) continue;
    } else {
      smTr = smTr - smTr / period + tr;
      smP = smP - smP / period + plusDM;
      smM = smM - smM / period + minusDM;
    }
    if (!(smTr > 0)) continue;
    double pdi = 100.0 * smP / smTr;
    double mdi = 100.0 * smM / smTr;
    plusDi[i] = (float)pdi;
    minusDi[i] = (float)mdi;
    double den = pdi + mdi;
    dx[i] = den == 0 ? 0.0f : (float)(100.0 * std::fabs(pdi - mdi) / den);
  }
  const size_t firstDx = (size_t)period;
  if (firstDx + (size_t)period - 1 >= n) return;
  double adxAcc = 0;
  int dxCount = 0;
  for (size_t i = firstDx; i < n; ++i) {
    if (std::isnan(dx[i])) continue;
    if (dxCount < period) {
      adxAcc += dx[i];
      ++dxCount;
      if (dxCount == period) adx[i] = (float)(adxAcc / period);
      continue;
    }
    adxAcc = (adxAcc * (period - 1) + dx[i]) / period;
    adx[i] = (float)adxAcc;
  }
}

enum {
  IndVol, IndCvd, IndRsi, IndMacd, IndEma, IndSma, IndBoll, IndHeat,
  IndVwap, IndSt, IndEma2, IndStoch, IndAtr, IndObv, IndAdx,
  IndD7, IndD7Rsi, IndD7Score, IndD7Lvls, IndOi, IndFund, IndCipherB
};

enum { CvdLine = 0, CvdCandles = 1 };
static constexpr double kCvdMinUsd[] = {0, 1e3, 1e4, 5e4, 1e5, 2.5e5};
static constexpr const char* kCvdMinLbl[] = {"ALL", "1K", "10K", "50K", "100K", "250K"};
static constexpr int kCvdMinN = 6;
static constexpr double kCvdMaxUsd[] = {0, 1e4, 5e4, 1e5, 1e6};
static constexpr const char* kCvdMaxLbl[] = {"OFF", "10K", "50K", "100K", "1M"};
static constexpr int kCvdMaxN = 5;

static int cvdMinSel(const IndicatorInstance& inst) {
return inst.reg == IndCvd ? std::clamp(inst.p0, 0, kCvdMinN - 1) : 0;
}
static int cvdMaxSel(const IndicatorInstance& inst) {
  return inst.reg == IndCvd ? std::clamp(inst.p1, 0, kCvdMaxN - 1) : 0;
}
static bool cvdNeedsFlow(const IndicatorInstance& inst) {
  return inst.reg == IndCvd && (inst.opt == CvdCandles || inst.p0 > 0 || inst.p1 > 0);
}
static bool cvdTradePass(const OrderFlowTrade& t, double minUsd, double maxUsd) {
  double usd = t.price * t.qty;
  if (!(usd > 0) || !std::isfinite(usd)) return false;
  if (minUsd > 0 && usd < minUsd) return false;
  if (maxUsd > 0 && usd >= maxUsd) return false;
  return true;
}

static void computeCvdSeries(const CandleSeries& cs, IndicatorInstance& inst,
                             const OrderFlowSeries* of) {
  const size_t n = cs.v.size();
  inst.series.resize(n);
  inst.aux.resize(n);
  inst.aux2.resize(n);
  const double minUsd = kCvdMinUsd[cvdMinSel(inst)];
  const double maxUsd = kCvdMaxUsd[cvdMaxSel(inst)];
  const bool useFlow = (minUsd > 0 || maxUsd > 0 || inst.opt == CvdCandles) &&
                       of && !of->v.empty();
  if (!useFlow) {
    double acc = 0;
    for (size_t i = 0; i < n; ++i) {
      double open = acc;
      acc += cs.v[i].delta;
      inst.series[i] = (float)acc;
      inst.aux[i] = (float)std::max(open, acc);
      inst.aux2[i] = (float)std::min(open, acc);
    }
    inst.liveI = of ? (int)of->v.size() : 0;
    inst.liveShift = of ? of->indexShift : 0;
    inst.liveDay = 0;
    return;
  }
  size_t ti = 0;
  double acc = 0;
  for (size_t i = 0; i < n; ++i) {
    double open = acc;
    double hi = open, lo = open;
    const double barTs = cs.v[i].ts;
    const double barEnd = i + 1 < n ? cs.v[i + 1].ts : 1e300;
    while (ti < of->v.size() && of->v[ti].ts < barTs) ++ti;
    while (ti < of->v.size() && of->v[ti].ts < barEnd) {
      const OrderFlowTrade& t = of->v[ti++];
      if (t.ts < barTs || !cvdTradePass(t, minUsd, maxUsd)) continue;
      acc += t.side == 0 ? t.qty : -t.qty;
      if (acc > hi) hi = acc;
      if (acc < lo) lo = acc;
    }
    inst.series[i] = (float)acc;
    inst.aux[i] = (float)hi;
    inst.aux2[i] = (float)lo;
  }
  inst.liveI = (int)of->v.size();
  inst.liveShift = of->indexShift;
  inst.liveDay = (int64_t)of->generation;
}

static void drawOscCandles(DrawList& d, const ChartPane& pane,
                           const IndicatorInstance& inst, int vis0, int vis1,
                           float startF, float bw, Color ca, Color cb) {
  const std::vector<float>& s = inst.series;
  for (int i = vis0; i <= vis1 && i < (int)s.size(); ++i) {
    float cl = s[(size_t)i];
    if (std::isnan(cl)) continue;
    float op = (i > 0 && !std::isnan(s[(size_t)i - 1])) ? s[(size_t)i - 1] : cl;
    float hi = i < (int)inst.aux.size() && !std::isnan(inst.aux[(size_t)i])
                   ? inst.aux[(size_t)i] : std::max(op, cl);
    float lo = i < (int)inst.aux2.size() && !std::isnan(inst.aux2[(size_t)i])
                   ? inst.aux2[(size_t)i] : std::min(op, cl);
    hi = std::max(hi, std::max(op, cl));
    lo = std::min(lo, std::min(op, cl));
    float x = pane.area.x + (i - startF) * bw;
    bool bull = cl >= op;
    drawOhlcCandle(d, x, bw, pane.yOf(op), pane.yOf(cl), pane.yOf(hi),
                   pane.yOf(lo), bull ? ca : cb, bull, true, CandleSolid);
  }
}

enum { VolTotal = 0, VolDelta = 1, VolRvol = 2, VolSplit = 3 };

static int volumeMode(const IndicatorInstance& inst) {
  return inst.reg == IndVol ? std::clamp(inst.p1, 0, 3) : VolTotal;
}

static int volumeRvolPeriod(const IndicatorInstance& inst) {
  return std::max(2, inst.p0 > 0 ? inst.p0 : 20);
}

static void computeVolume(const CandleSeries& cs, IndicatorInstance& inst) {
  const size_t n = cs.v.size();
  const int mode = volumeMode(inst);
  inst.series.resize(n);
  if (mode == VolSplit) inst.aux.assign(n, NAN);
  else inst.aux.clear();
  if (mode == VolTotal) {
    for (size_t i = 0; i < n; ++i) inst.series[i] = (float)barVolume(cs.v[i]);
    return;
  }
  if (mode == VolDelta) {
    for (size_t i = 0; i < n; ++i) inst.series[i] = (float)cs.v[i].delta;
    return;
  }
  if (mode == VolRvol) {
    const int p = volumeRvolPeriod(inst);
    double sum = 0;
    for (size_t i = 0; i < n; ++i) {
      double v = barVolume(cs.v[i]);
      sum += v;
      if (i >= (size_t)p) sum -= barVolume(cs.v[i - (size_t)p]);
      double avg = sum / (double)(i + 1 < (size_t)p ? i + 1 : p);
      inst.series[i] = avg > 0 ? (float)(v / avg) : NAN;
    }
    return;
  }
  for (size_t i = 0; i < n; ++i) {
    double v = barVolume(cs.v[i]);
    double d = cs.v[i].delta;
    inst.series[i] = (float)std::max(0.0, (v + d) * 0.5);
    inst.aux[i] = (float)std::max(0.0, (v - d) * 0.5);
  }
}

// ---------------------------------------------------------------------------
// Per-register compute / live-update implementations, driven by kRegistry
// below. Each computeX mirrors the series fill; the updateX functions are the
// O(1)/O(period) last-bar increments used between full recomputes.
// ---------------------------------------------------------------------------

// Shared resampler for timestamped market samples (OI, funding): walks the
// sample stream onto the candle axis, collapsing each bar to open/high/low/
// close. `firstPass` gates the prev-sample carry (samples before a bar),
// `innerSkip` gates in-bar samples, `anyNeedsPositive` selects the OI
// positivity requirement on the carried open.
template <typename Sample, typename Get, typename First, typename Inner>
static void walkSampleSeries(const CandleSeries& cs,
                             const std::vector<Sample>& samples,
                             IndicatorInstance& inst, uint64_t version, Get get,
                             First firstPass, Inner innerSkip,
                             bool anyNeedsPositive) {
  const size_t n = cs.v.size();
  inst.series.assign(n, NAN);
  inst.aux.assign(n, NAN);
  inst.aux2.assign(n, NAN);
  if (samples.empty() || n == 0) {
    inst.liveDay = (int64_t)version;
    inst.liveI = 0;
    inst.live0 = NAN;
    return;
  }
  size_t si = 0;
  double prev = NAN;
  for (size_t i = 0; i < n; ++i) {
    const double barTs = cs.v[i].ts;
    const double barEnd = i + 1 < n ? cs.v[i + 1].ts : 1e300;
    while (si < samples.size() && samples[si].ts < barTs) {
      double v = get(samples[si]);
      if (firstPass(v)) prev = v;
      ++si;
    }
    double open = prev;
    double hi = open, lo = open, close = open;
    bool any = std::isfinite(open) && (!anyNeedsPositive || open > 0);
    while (si < samples.size() && samples[si].ts < barEnd) {
      double v = get(samples[si]);
      if (innerSkip(v)) {
        ++si;
        continue;
      }
      if (!any) {
        open = hi = lo = close = v;
        any = true;
      } else {
        if (v > hi) hi = v;
        if (v < lo) lo = v;
        close = v;
      }
      prev = v;
      ++si;
    }
    if (!any) continue;
    inst.series[i] = (float)close;
    inst.aux[i] = (float)hi;
    inst.aux2[i] = (float)lo;
  }
  inst.liveI = (int)si;
  inst.live0 = prev;
  inst.liveDay = (int64_t)version;
}

// Incremental sibling: appends the bars grown since the last walk from the
// stored cursor (liveI/live0), bit-identical to a full walk because the loop
// body and carry state are the same. Falls back to a full walk on any version
// change, shrink, or cursor desync. Market samples are append-only within a
// version (loadOi/loadFunding replace buffers wholesale and bump version), so
// the cursor is always a prefix of the current stream when version matches.
template <typename Sample, typename Get, typename First, typename Inner>
static bool updateSampleSeriesLast(const CandleSeries& cs,
                                   const std::vector<Sample>& samples,
                                   IndicatorInstance& inst, uint64_t version,
                                   Get get, First firstPass, Inner innerSkip,
                                   bool anyNeedsPositive) {
  const size_t n = cs.v.size();
  const size_t oldN = inst.series.size();
  if ((uint64_t)inst.liveDay == version && oldN == n) return true;
  if ((uint64_t)inst.liveDay != version || oldN == 0 || oldN > n ||
      samples.empty() || (size_t)inst.liveI > samples.size()) {
    walkSampleSeries(cs, samples, inst, version, get, firstPass, innerSkip,
                     anyNeedsPositive);
    return true;
  }
  size_t si = (size_t)inst.liveI;
  double prev = inst.live0;
  inst.series.resize(n);
  inst.aux.resize(n);
  inst.aux2.resize(n);
  for (size_t i = oldN; i < n; ++i) {
    const double barTs = cs.v[i].ts;
    const double barEnd = i + 1 < n ? cs.v[i + 1].ts : 1e300;
    while (si < samples.size() && samples[si].ts < barTs) {
      double v = get(samples[si]);
      if (firstPass(v)) prev = v;
      ++si;
    }
    double open = prev;
    double hi = open, lo = open, close = open;
    bool any = std::isfinite(open) && (!anyNeedsPositive || open > 0);
    while (si < samples.size() && samples[si].ts < barEnd) {
      double v = get(samples[si]);
      if (innerSkip(v)) {
        ++si;
        continue;
      }
      if (!any) {
        open = hi = lo = close = v;
        any = true;
      } else {
        if (v > hi) hi = v;
        if (v < lo) lo = v;
        close = v;
      }
      prev = v;
      ++si;
    }
    if (!any) continue;
    inst.series[i] = (float)close;
    inst.aux[i] = (float)hi;
    inst.aux2[i] = (float)lo;
  }
  inst.liveI = (int)si;
  inst.live0 = prev;
  inst.liveDay = (int64_t)version;
  return true;
}

static void computeOi(const CandleSeries& cs, IndicatorInstance& inst,
                      const OrderFlowSeries* of, const MarketSeries* mkt) {
  if (!mkt) {
    const size_t n = cs.v.size();
    inst.series.assign(n, NAN);
    inst.aux.assign(n, NAN);
    inst.aux2.assign(n, NAN);
    inst.liveDay = 0;
    inst.liveI = 0;
    inst.live0 = NAN;
    return;
  }
  walkSampleSeries(cs, mkt->oi, inst, mkt->version,
                   [](const OiSample& s) { return s.oi; },
                   [](double v) { return v > 0; },
                   [](double v) { return !(v > 0) || !std::isfinite(v); }, true);
}

static bool updateOi(const CandleSeries& cs, IndicatorInstance& inst,
                     const OrderFlowSeries* of, const MarketSeries* mkt) {
  if (!mkt) return false;
  return updateSampleSeriesLast(
      cs, mkt->oi, inst, mkt->version,
      [](const OiSample& s) { return s.oi; },
      [](double v) { return v > 0; },
      [](double v) { return !(v > 0) || !std::isfinite(v); }, true);
}

static void computeFund(const CandleSeries& cs, IndicatorInstance& inst,
                        const OrderFlowSeries* of, const MarketSeries* mkt) {
  if (!mkt) {
    const size_t n = cs.v.size();
    inst.series.assign(n, NAN);
    inst.aux.assign(n, NAN);
    inst.aux2.assign(n, NAN);
    inst.liveDay = 0;
    inst.liveI = 0;
    inst.live0 = NAN;
    return;
  }
  walkSampleSeries(cs, mkt->funding, inst, mkt->version,
                   [](const FundingSample& s) { return s.rate; },
                   [](double) { return true; }, [](double) { return false; },
                   false);
}

static bool updateFund(const CandleSeries& cs, IndicatorInstance& inst,
                       const OrderFlowSeries* of, const MarketSeries* mkt) {
  if (!mkt) return false;
  return updateSampleSeriesLast(
      cs, mkt->funding, inst, mkt->version,
      [](const FundingSample& s) { return s.rate; },
      [](double) { return true; }, [](double) { return false; }, false);
}

static void computeHeat(const CandleSeries& cs, IndicatorInstance& inst,
                        const OrderFlowSeries* of, const MarketSeries* mkt) {
  inst.series.clear();
}

static void computeVol(const CandleSeries& cs, IndicatorInstance& inst,
                       const OrderFlowSeries* of, const MarketSeries* mkt) {
  computeVolume(cs, inst);
}

static bool updateVol(const CandleSeries& cs, IndicatorInstance& inst,
                      const OrderFlowSeries* of, const MarketSeries* mkt) {
  const size_t n = cs.v.size();
  const Candle& c = cs.v[n - 1];
  const int mode = volumeMode(inst);
  if (mode == VolTotal) inst.series[n - 1] = (float)barVolume(c);
  else if (mode == VolDelta) inst.series[n - 1] = (float)c.delta;
  else if (mode == VolSplit) {
    if (inst.aux.size() != n) inst.aux.assign(n, NAN);
    double v = barVolume(c);
    inst.series[n - 1] = (float)std::max(0.0, (v + c.delta) * 0.5);
    inst.aux[n - 1] = (float)std::max(0.0, (v - c.delta) * 0.5);
  } else {
    const int p = volumeRvolPeriod(inst);
    double sum = 0;
    int count = 0;
    for (size_t i = n - (size_t)std::min((size_t)p, n); i < n; ++i) {
      sum += barVolume(cs.v[i]);
      ++count;
    }
    double avg = count > 0 ? sum / count : 0;
    inst.series[n - 1] = avg > 0 ? (float)(barVolume(c) / avg) : NAN;
  }
  return true;
}

static void computeCvd(const CandleSeries& cs, IndicatorInstance& inst,
                       const OrderFlowSeries* of, const MarketSeries* mkt) {
  computeCvdSeries(cs, inst, of);
  const size_t n = cs.v.size();
  if (n >= 2 && inst.series.size() == n) {
    inst.live0 = (double)inst.series[n - 2];
    inst.live1 = (double)inst.series[n - 1];
    inst.live2 = inst.aux.size() == n ? (double)inst.aux[n - 1] : inst.live1;
    inst.live3 = inst.aux2.size() == n ? (double)inst.aux2[n - 1] : inst.live1;
  }
}

static bool updateCvd(const CandleSeries& cs, IndicatorInstance& inst,
                      const OrderFlowSeries* of, const MarketSeries* mkt) {
  const size_t n = cs.v.size();
  const Candle& c = cs.v[n - 1];
  if (inst.aux.size() != n) inst.aux.assign(n, NAN);
  if (inst.aux2.size() != n) inst.aux2.assign(n, NAN);
  const bool needFlow = cvdNeedsFlow(inst);
  if (needFlow && of && !of->v.empty() && inst.liveDay == 0) return false;
  if (needFlow && of && !of->v.empty()) {
    if ((uint64_t)inst.liveDay != of->generation || inst.liveI < 0)
      return false;
    // Prepend/compaction shifted the window's indices; slide the cursor by
    // the delta instead of forcing a full recompute per streamed batch.
    const int64_t cursor =
        (int64_t)inst.liveI + (of->indexShift - inst.liveShift);
    if (cursor < 0 || cursor > (int64_t)of->v.size()) return false;
    inst.liveI = (int)cursor;
    inst.liveShift = of->indexShift;
    double acc = inst.live1;
    double hi = inst.live2;
    double lo = inst.live3;
    const double minUsd = kCvdMinUsd[cvdMinSel(inst)];
    const double maxUsd = kCvdMaxUsd[cvdMaxSel(inst)];
    const double barTs = c.ts;
    size_t ti = (size_t)inst.liveI;
    while (ti < of->v.size()) {
      const OrderFlowTrade& t = of->v[ti++];
      if (t.ts < barTs || !cvdTradePass(t, minUsd, maxUsd)) continue;
      acc += t.side == 0 ? t.qty : -t.qty;
      if (acc > hi) hi = acc;
      if (acc < lo) lo = acc;
    }
    inst.liveI = (int)ti;
    inst.live1 = acc;
    inst.live2 = hi;
    inst.live3 = lo;
    inst.series[n - 1] = (float)acc;
    inst.aux[n - 1] = (float)hi;
    inst.aux2[n - 1] = (float)lo;
    return true;
  }
  double acc = inst.live0 + c.delta;
  inst.series[n - 1] = (float)acc;
  inst.aux[n - 1] = (float)std::max(inst.live0, acc);
  inst.aux2[n - 1] = (float)std::min(inst.live0, acc);
  inst.live1 = acc;
  inst.live2 = inst.aux[n - 1];
  inst.live3 = inst.aux2[n - 1];
  return true;
}

static void computeEma(const CandleSeries& cs, IndicatorInstance& inst,
                       const OrderFlowSeries* of, const MarketSeries* mkt) {
  emaSeries(cs, std::max(2, inst.p0), inst.series);
}

static bool updateEma(const CandleSeries& cs, IndicatorInstance& inst,
                      const OrderFlowSeries* of, const MarketSeries* mkt) {
  const size_t n = cs.v.size();
  const Candle& c = cs.v[n - 1];
  if (n < (size_t)inst.p0 + 1 || std::isnan(inst.series[n - 2])) return false;
  double e = inst.series[n - 2];
  inst.series[n - 1] = (float)(e + (c.c - e) * (2.0 / (inst.p0 + 1)));
  return true;
}

static void computeSma(const CandleSeries& cs, IndicatorInstance& inst,
                       const OrderFlowSeries* of, const MarketSeries* mkt) {
  smaSeries(cs, std::max(2, inst.p0), inst.series);
}

static bool updateSmaBoll(const CandleSeries& cs, IndicatorInstance& inst,
                          const OrderFlowSeries* of, const MarketSeries* mkt) {
  const size_t n = cs.v.size();
  if (n < (size_t)inst.p0) return false;
  double sum = 0;
  for (size_t i = n - (size_t)inst.p0; i < n; ++i) sum += cs.v[i].c;
  inst.series[n - 1] = (float)(sum / inst.p0);
  if (inst.reg == IndBoll && inst.aux.size() == n && inst.aux2.size() == n) {
    double var = 0;
    for (size_t i = n - (size_t)inst.p0; i < n; ++i) {
      double d = cs.v[i].c - inst.series[n - 1];
      var += d * d;
    }
    double sd = std::sqrt(var / inst.p0);
    float dev = kBollDeviations[std::clamp(inst.opt, 0, 3)];
    inst.aux[n - 1] = (float)(inst.series[n - 1] + dev * sd);
    inst.aux2[n - 1] = (float)(inst.series[n - 1] - dev * sd);
  }
  return true;
}

static void computeBoll(const CandleSeries& cs, IndicatorInstance& inst,
                        const OrderFlowSeries* of, const MarketSeries* mkt) {
  bollBandsSeries(cs, std::max(2, inst.p0),
                  kBollDeviations[std::clamp(inst.opt, 0, 3)], inst.series,
                  inst.aux, inst.aux2);
}

static void computeRsi(const CandleSeries& cs, IndicatorInstance& inst,
                       const OrderFlowSeries* of, const MarketSeries* mkt) {
  rsiSeries(cs, std::max(2, inst.p0), inst.series);
  const size_t n = cs.v.size();
  if (n >= (size_t)inst.p0 + 2 && inst.series.size() == n) {
    double gain, loss;
    if (!wilderSeed(cs, inst.p0, gain, loss)) return;
    for (size_t i = (size_t)inst.p0 + 1; i <= n - 2; ++i) { // to the last closed bar
      double d = cs.v[i].c - cs.v[i - 1].c;
      gain = (gain * (inst.p0 - 1) + (d > 0 ? d : 0)) / inst.p0;
      loss = (loss * (inst.p0 - 1) + (d < 0 ? -d : 0)) / inst.p0;
    }
    inst.live0 = gain;
    inst.live1 = loss;
  }
}

static bool updateRsi(const CandleSeries& cs, IndicatorInstance& inst,
                      const OrderFlowSeries* of, const MarketSeries* mkt) {
  const size_t n = cs.v.size();
  const Candle& c = cs.v[n - 1];
  const double d = c.c - cs.v[n - 2].c;
  double g = (inst.live0 * (inst.p0 - 1) + (d > 0 ? d : 0)) / inst.p0;
  double l = (inst.live1 * (inst.p0 - 1) + (d < 0 ? -d : 0)) / inst.p0;
  inst.series[n - 1] =
      l == 0 ? 100.0f : (float)(100.0 - 100.0 / (1.0 + g / l));
  return true;
}

static void computeMacd(const CandleSeries& cs, IndicatorInstance& inst,
                        const OrderFlowSeries* of, const MarketSeries* mkt) {
  macdSeries(cs, std::max(2, inst.p0), std::max(inst.p0 + 1, inst.p1),
             inst.series);
  if (!inst.series.empty()) {
    inst.aux.assign(inst.series.size(), NAN);
    const int sigP = std::max(2, inst.p2);
    const double k = 2.0 / (sigP + 1);
    double ema = 0;
    int valid = 0;
    for (size_t i = 0; i < inst.series.size(); ++i) {
      if (std::isnan(inst.series[i])) continue;
      ema = valid == 0 ? inst.series[i] : ema + (inst.series[i] - ema) * k;
      if (++valid >= sigP) inst.aux[i] = (float)ema;
    }
    inst.live2 = ema;
    inst.liveI = valid;
  }
  const size_t n = cs.v.size();
  if (n >= (size_t)std::max(inst.p0, inst.p1) + 1 && inst.series.size() == n) {
    double e12 = cs.v[0].c, e26 = cs.v[0].c;
    for (size_t i = 1; i <= n - 2; ++i) {
      e12 += (cs.v[i].c - e12) * (2.0 / (inst.p0 + 1));
      e26 += (cs.v[i].c - e26) * (2.0 / (inst.p1 + 1));
    }
    inst.live0 = e12;
    inst.live1 = e26;
  }
}

static bool updateMacd(const CandleSeries& cs, IndicatorInstance& inst,
                       const OrderFlowSeries* of, const MarketSeries* mkt) {
  const size_t n = cs.v.size();
  const Candle& c = cs.v[n - 1];
  if (inst.aux.size() != n) return false;
  double e12 = inst.live0 + (c.c - inst.live0) * (2.0 / (inst.p0 + 1));
  double e26 = inst.live1 + (c.c - inst.live1) * (2.0 / (inst.p1 + 1));
  double macdNow = e12 - e26;
  inst.series[n - 1] = (float)macdNow;
  double se = inst.liveI == 0
                  ? macdNow
                  : inst.live2 + (macdNow - inst.live2) * (2.0 / (inst.p2 + 1));
  if (inst.liveI + 1 >= inst.p2) inst.aux[n - 1] = (float)se;
  return true;
}

static void computeVwap(const CandleSeries& cs, IndicatorInstance& inst,
                        const OrderFlowSeries* of, const MarketSeries* mkt) {
  vwapSeries(cs, inst.series, inst.aux, inst.aux2);
  const size_t n = cs.v.size();
  if (n >= 2 && inst.aux.size() == n && inst.series.size() == n) {
    double pv = 0, vol = 0, pv2 = 0;
    int64_t day = std::numeric_limits<int64_t>::min();
    for (size_t i = 0; i <= n - 2; ++i) {
      int64_t d = utcDay(cs.v[i].ts);
      if (d != day) {
        pv = vol = pv2 = 0;
        day = d;
      }
      double tp = (cs.v[i].h + cs.v[i].l + cs.v[i].c) / 3.0;
      double v = barVolume(cs.v[i]);
      pv += tp * v;
      pv2 += tp * tp * v;
      vol += v;
    }
    inst.live0 = pv;
    inst.live1 = vol;
    inst.live2 = pv2;
    inst.liveDay = day;
  }
}

static bool updateVwap(const CandleSeries& cs, IndicatorInstance& inst,
                       const OrderFlowSeries* of, const MarketSeries* mkt) {
  const size_t n = cs.v.size();
  const Candle& c = cs.v[n - 1];
  if (inst.aux.size() != n || inst.aux2.size() != n) return false;
  double pv = inst.live0, vol = inst.live1, pv2 = inst.live2;
  int64_t day = utcDay(c.ts);
  if (day != inst.liveDay) pv = vol = pv2 = 0;
  double tp = (c.h + c.l + c.c) / 3.0;
  double v = barVolume(c);
  pv += tp * v;
  pv2 += tp * tp * v;
  vol += v;
  if (vol > 0) {
    double vwap = pv / vol;
    double sd = std::sqrt(std::max(0.0, pv2 / vol - vwap * vwap));
    inst.series[n - 1] = (float)vwap;
    inst.aux[n - 1] = (float)(vwap + sd);
    inst.aux2[n - 1] = (float)(vwap - sd);
  }
  return true;
}

static void computeSt(const CandleSeries& cs, IndicatorInstance& inst,
                      const OrderFlowSeries* of, const MarketSeries* mkt) {
  supertrendSeries(cs, std::max(2, inst.p0),
                   kStMultipliers[std::clamp(inst.opt, 0, 3)], inst.series,
                   inst.dir);
}

static void computeStoch(const CandleSeries& cs, IndicatorInstance& inst,
                         const OrderFlowSeries* of, const MarketSeries* mkt) {
  stochSeries(cs, std::max(2, inst.p0), std::max(1, inst.p1),
              std::max(1, inst.p2), inst.series, inst.aux);
}

static bool updateStoch(const CandleSeries& cs, IndicatorInstance& inst,
                        const OrderFlowSeries* of, const MarketSeries* mkt) {
  const size_t n = cs.v.size();
  if (inst.aux.size() != n) return false;
  auto rawK = [&](size_t end) -> float {
    if (end + 1 < (size_t)inst.p0) return NAN;
    double hh = -1e300, ll = 1e300;
    for (size_t j = end + 1 - (size_t)inst.p0; j <= end; ++j) {
      hh = std::max(hh, cs.v[j].h);
      ll = std::min(ll, cs.v[j].l);
    }
    return hh == ll ? 50.0f : (float)(100.0 * (cs.v[end].c - ll) / (hh - ll));
  };
  double kSum = 0;
  for (int j = 0; j < inst.p1; ++j) {
    float rk = rawK(n - 1 - (size_t)j);
    if (std::isnan(rk)) return false;
    kSum += rk;
  }
  inst.series[n - 1] = (float)(kSum / inst.p1);
  double dSum = 0;
  for (int j = 0; j < inst.p2; ++j) {
    float kv = inst.series[n - 1 - (size_t)j];
    if (std::isnan(kv)) return false;
    dSum += kv;
  }
  inst.aux[n - 1] = (float)(dSum / inst.p2);
  return true;
}

static void computeAtr(const CandleSeries& cs, IndicatorInstance& inst,
                       const OrderFlowSeries* of, const MarketSeries* mkt) {
  atrSeries(cs, std::max(2, inst.p0), inst.series);
  const size_t n = cs.v.size();
  if (n >= 2 && inst.series.size() == n && !std::isnan(inst.series[n - 2]))
    inst.live0 = inst.series[n - 2];
}

static bool updateAtr(const CandleSeries& cs, IndicatorInstance& inst,
                      const OrderFlowSeries* of, const MarketSeries* mkt) {
  const size_t n = cs.v.size();
  const Candle& c = cs.v[n - 1];
  if (n < (size_t)inst.p0 + 1) return false;
  double tr = trueRange(c, cs.v[n - 2].c);
  inst.series[n - 1] = (float)((inst.live0 * (inst.p0 - 1) + tr) / inst.p0);
  return true;
}

static void computeObv(const CandleSeries& cs, IndicatorInstance& inst,
                       const OrderFlowSeries* of, const MarketSeries* mkt) {
  obvSeries(cs, inst.series);
  const size_t n = cs.v.size();
  if (n >= 2 && inst.series.size() == n) inst.live0 = (double)inst.series[n - 2];
}

static bool updateObv(const CandleSeries& cs, IndicatorInstance& inst,
                      const OrderFlowSeries* of, const MarketSeries* mkt) {
  const size_t n = cs.v.size();
  const Candle& c = cs.v[n - 1];
  double v = barVolume(c);
  double acc = inst.live0;
  if (c.c > cs.v[n - 2].c) acc += v;
  else if (c.c < cs.v[n - 2].c) acc -= v;
  inst.series[n - 1] = (float)acc;
  return true;
}

static void computeAdx(const CandleSeries& cs, IndicatorInstance& inst,
                       const OrderFlowSeries* of, const MarketSeries* mkt) {
  adxSeries(cs, std::max(2, inst.p0), inst.series, inst.aux, inst.aux2);
  const size_t n = cs.v.size();
  if (inst.aux.size() == n && n >= (size_t)inst.p0 * 2) {
    double smTr = 0, smP = 0, smM = 0;
    for (size_t i = 1; i <= n - 2; ++i) {
      double upMove = cs.v[i].h - cs.v[i - 1].h;
      double downMove = cs.v[i - 1].l - cs.v[i].l;
      double plusDM = upMove > downMove && upMove > 0 ? upMove : 0;
      double minusDM = downMove > upMove && downMove > 0 ? downMove : 0;
      double tr = trueRange(cs.v[i], cs.v[i - 1].c);
      if (i <= (size_t)inst.p0) {
        smTr += tr;
        smP += plusDM;
        smM += minusDM;
      } else {
        smTr = smTr - smTr / inst.p0 + tr;
        smP = smP - smP / inst.p0 + plusDM;
        smM = smM - smM / inst.p0 + minusDM;
      }
    }
    inst.live0 = smTr;
    inst.live1 = smP;
    inst.live2 = smM;
    inst.live3 = inst.series[n - 2];
  }
}

static bool updateAdx(const CandleSeries& cs, IndicatorInstance& inst,
                      const OrderFlowSeries* of, const MarketSeries* mkt) {
  const size_t n = cs.v.size();
  const Candle& c = cs.v[n - 1];
  if (inst.aux.size() != n || inst.aux2.size() != n || n < (size_t)inst.p0 * 2)
    return false;
  double upMove = c.h - cs.v[n - 2].h;
  double downMove = cs.v[n - 2].l - c.l;
  double plusDM = upMove > downMove && upMove > 0 ? upMove : 0;
  double minusDM = downMove > upMove && downMove > 0 ? downMove : 0;
  double tr = trueRange(c, cs.v[n - 2].c);
  double smTr = inst.live0 - inst.live0 / inst.p0 + tr;
  double smP = inst.live1 - inst.live1 / inst.p0 + plusDM;
  double smM = inst.live2 - inst.live2 / inst.p0 + minusDM;
  if (!(smTr > 0)) return false;
  double pdi = 100.0 * smP / smTr;
  double mdi = 100.0 * smM / smTr;
  inst.aux[n - 1] = (float)pdi;
  inst.aux2[n - 1] = (float)mdi;
  double den = pdi + mdi;
  double dx = den == 0 ? 0.0 : 100.0 * std::fabs(pdi - mdi) / den;
  inst.series[n - 1] = (float)((inst.live3 * (inst.p0 - 1) + dx) / inst.p0);
  return true;
}

static void computeRsiLive(const CandleSeries& cs, IndicatorInstance& inst,
                           int rsiLiveP) {
  const size_t n = cs.v.size();
  if (n >= (size_t)rsiLiveP + 2 && inst.series.size() == n) {
    double gain, loss;
    if (!wilderSeed(cs, rsiLiveP, gain, loss)) return;
    for (size_t i = (size_t)rsiLiveP + 1; i <= n - 2; ++i) { // to the last closed bar
      double d = cs.v[i].c - cs.v[i - 1].c;
      gain = (gain * (rsiLiveP - 1) + (d > 0 ? d : 0)) / rsiLiveP;
      loss = (loss * (rsiLiveP - 1) + (d < 0 ? -d : 0)) / rsiLiveP;
    }
    inst.live0 = gain;
    inst.live1 = loss;
  }
}

static void computeD7(const CandleSeries& cs, IndicatorInstance& inst,
                      const OrderFlowSeries* of, const MarketSeries* mkt) {
  d7ComputeCloud(cs, std::max(2, inst.p0), inst.series, inst.aux, inst.aux2,
                 inst.dir);
  computeRsiLive(cs, inst, 14);
}

static bool updateD7(const CandleSeries& cs, IndicatorInstance& inst,
                     const OrderFlowSeries* of, const MarketSeries* mkt) {
  return d7UpdateCloudLast(cs, std::max(2, inst.p0), inst.series, inst.aux,
                           inst.aux2, inst.dir, inst.live0, inst.live1);
}

static void computeD7Rsi(const CandleSeries& cs, IndicatorInstance& inst,
                         const OrderFlowSeries* of, const MarketSeries* mkt) {
  d7ComputeRsiCloud(cs, std::max(2, inst.p0), std::max(2, inst.p1),
                    std::max(2, inst.p2), inst.series, inst.aux, inst.aux2);
  computeRsiLive(cs, inst, inst.p0);
}

static bool updateD7Rsi(const CandleSeries& cs, IndicatorInstance& inst,
                        const OrderFlowSeries* of, const MarketSeries* mkt) {
  return d7UpdateRsiCloudLast(cs, std::max(2, inst.p0), std::max(2, inst.p1),
                              std::max(2, inst.p2), inst.series, inst.aux,
                              inst.aux2, inst.live0, inst.live1);
}

static void computeD7Score(const CandleSeries& cs, IndicatorInstance& inst,
                           const OrderFlowSeries* of, const MarketSeries* mkt) {
  d7ComputeScore(cs, std::max(2, inst.p0), inst.series, inst.dir);
  computeRsiLive(cs, inst, 14);
}

static bool updateD7Score(const CandleSeries& cs, IndicatorInstance& inst,
                          const OrderFlowSeries* of, const MarketSeries* mkt) {
  return d7UpdateScoreLast(cs, std::max(2, inst.p0), inst.series, inst.dir,
                           inst.live0, inst.live1);
}

static void computeD7Lvls(const CandleSeries& cs, IndicatorInstance& inst,
                          const OrderFlowSeries* of, const MarketSeries* mkt) {
  d7ComputeLevels(cs, inst.series, inst.aux, inst.aux2);
}

static bool updateD7Lvls(const CandleSeries& cs, IndicatorInstance& inst,
                         const OrderFlowSeries* of, const MarketSeries* mkt) {
  return d7UpdateLevelsLast(cs, inst.series, inst.aux, inst.aux2);
}

static void computeCipherB(const CandleSeries& cs, IndicatorInstance& inst,
                           const OrderFlowSeries* of, const MarketSeries* mkt) {
  cipherCompute(cs, inst.p0, inst.p1, inst.p2, inst.series, inst.aux, inst.aux2,
                inst.dir, inst.live0, inst.live1, inst.live2, inst.live3,
                inst.live4, inst.live5, inst.live6);
}

static bool updateCipherB(const CandleSeries& cs, IndicatorInstance& inst,
                          const OrderFlowSeries* of, const MarketSeries* mkt) {
  return cipherUpdateLast(cs, inst.p0, inst.p1, inst.p2, inst.series, inst.aux,
                          inst.aux2, inst.dir, inst.live0, inst.live1,
                          inst.live2, inst.live3, inst.live4, inst.live5,
                          inst.live6);
}

static const Indicator kRegistry[] = {
    {"VOL", false, computeVol, updateVol},
    {"CVD", false, computeCvd, updateCvd},
    {"RSI 14", false, computeRsi, updateRsi},
    {"MACD 12 26 9", false, computeMacd, updateMacd},
    {"EMA 21", true, computeEma, updateEma},
    {"SMA 50", true, computeSma, updateSmaBoll},
    {"BB 20 2", true, computeBoll, updateSmaBoll},
    {"BOOK HEAT", true, computeHeat, nullptr},
    {"VWAP", true, computeVwap, updateVwap},
    {"ST 10 3", true, computeSt, nullptr},
    {"EMA 200", true, computeEma, updateEma},
    {"STOCH 14 3 3", false, computeStoch, updateStoch},
    {"ATR 14", false, computeAtr, updateAtr},
    {"OBV", false, computeObv, updateObv},
    {"ADX 14", false, computeAdx, updateAdx},
    {"D7", true, computeD7, updateD7},
    {"D7 RSI", false, computeD7Rsi, updateD7Rsi},
    {"D7 SCORE", false, computeD7Score, updateD7Score},
    {"D7 LVLS", true, computeD7Lvls, updateD7Lvls},
    {"OI", false, computeOi, updateOi},
    {"FUND", false, computeFund, updateFund},
    {"CIPHER B", false, computeCipherB, updateCipherB},
};

static_assert(sizeof(kRegistry) / sizeof(kRegistry[0]) == 22, "registry/schema drift");

static int indicatorCount() { return (int)(sizeof(kRegistry) / sizeof(kRegistry[0])); }

static bool paneKind(int ri) {
  return ri >= 0 && ri < indicatorCount() && !kRegistry[ri].overlay;
}

// Shared instance label: chart-panel headers and the indicator picker both
// format through here so the two can never drift.
static void indicatorName(int ri, int p0, int p1, int p2, int opt, char* out,
                          size_t n) {
  switch (ri) {
    case IndEma:
    case IndEma2: snprintf(out, n, "EMA %d", p0); break;
    case IndSma: snprintf(out, n, "SMA %d", p0); break;
    case IndRsi: snprintf(out, n, "RSI %d", p0); break;
    case IndMacd: snprintf(out, n, "MACD %d %d %d", p0, p1, p2); break;
    case IndBoll:
      snprintf(out, n, "BB %d %.1f", p0, kBollDeviations[std::clamp(opt, 0, 3)]);
      break;
    case IndSt:
      snprintf(out, n, "ST %d %.1f", p0, kStMultipliers[std::clamp(opt, 0, 3)]);
      break;
    case IndStoch: snprintf(out, n, "STOCH %d %d %d", p0, p1, p2); break;
    case IndAtr: snprintf(out, n, "ATR %d", p0); break;
    case IndAdx: snprintf(out, n, "ADX %d", p0); break;
    case IndD7: snprintf(out, n, "D7 %d", d7BaseLength(p0)); break;
    case IndD7Rsi: snprintf(out, n, "D7 RSI %d", p0); break;
    case IndD7Score: snprintf(out, n, "D7 SCORE %d", d7BaseLength(p0)); break;
    case IndCipherB:
      if (p0 == 9 && p1 == 12) snprintf(out, n, "CIPHER B 9/12");
      else snprintf(out, n, "CIPHER B");
      break;
    case IndVol: {
      int mode = std::clamp(p1, 0, 3);
      if (mode == VolDelta) snprintf(out, n, "DELTA");
      else if (mode == VolRvol) snprintf(out, n, "RVOL %d", std::max(2, p0 > 0 ? p0 : 20));
      else if (mode == VolSplit) snprintf(out, n, "VOL B/S");
      else snprintf(out, n, "VOL");
      break;
    }
    case IndCvd: {
      int lo = std::clamp(p0, 0, kCvdMinN - 1);
      int hi = std::clamp(p1, 0, kCvdMaxN - 1);
      if (lo == 0 && hi == 0) snprintf(out, n, "CVD");
      else if (hi == 0) snprintf(out, n, "CVD %s+", kCvdMinLbl[lo]);
      else if (lo == 0) snprintf(out, n, "CVD <%s", kCvdMaxLbl[hi]);
      else snprintf(out, n, "CVD %s-%s", kCvdMinLbl[lo], kCvdMaxLbl[hi]);
      break;
    }
    default: snprintf(out, n, "%s", kRegistry[ri].name); break;
  }
}

static constexpr int kIndPaletteN = 8;

static Color indPalette(int slot) {
  const Theme& t = theme();
  switch (std::clamp(slot, 0, kIndPaletteN - 1)) {
    case 0: return t.accent;
    case 1: return t.chartWarm;
    case 2: return t.chartPurple;
    case 3: return t.green;
    case 4: return t.red;
    case 5: return t.text;
    case 6: return hexColor(0xe6b84d);
    default: return hexColor(0x6aa8d8);
  }
}

static uint8_t defaultColorA(int ri) {
  switch (ri) {
    case IndEma: return 0;
    case IndEma2: return 1;
    case IndSma: return 1;
    case IndBoll: return 0;
    case IndVwap: return 2;
    case IndSt: return 3;
    case IndVol: return 3;
    case IndCvd: return 3;
    case IndOi: return 6;
    case IndFund: return 3;
    case IndRsi: return 2;
    case IndMacd: return 0;
    case IndStoch: return 2;
    case IndAtr: return 0;
    case IndObv: return 0;
    case IndAdx: return 2;
    case IndD7: return 3;
    case IndD7Rsi: return 2;
    case IndD7Score: return 3;
    case IndD7Lvls: return 0;
    case IndCipherB: return 0;
    default: return 5;
  }
}

int ChartPanel::instanceCount(int ri) const {
  int n = 0;
  for (const IndicatorInstance& o : m_overlays) if (o.reg == ri) ++n;
  for (const IndicatorInstance& p : m_panes) if (p.reg == ri) ++n;
  return n;
}

bool ChartPanel::indicatorOn(int ri) const { return instanceCount(ri) > 0; }

IndicatorInstance* ChartPanel::findInstance(int id) {
  for (IndicatorInstance& o : m_overlays) if (o.id == id) return &o;
  for (IndicatorInstance& p : m_panes) if (p.id == id) return &p;
  return nullptr;
}

IndicatorInstance ChartPanel::makeInstance(int ri) {
  IndicatorInstance inst;
  inst.id = m_nextInstId++;
  inst.reg = ri;
  inst.width = (ri >= 0 && (size_t)ri < m_indicatorWidths.size())
                   ? m_indicatorWidths[(size_t)ri]
                   : (uint8_t)m_lineWidth;
  int existing = instanceCount(ri);
  inst.colorA = (uint8_t)((defaultColorA(ri) + existing) % kIndPaletteN);
  inst.colorB = (uint8_t)((inst.colorA + 1) % kIndPaletteN);
  inst.colorC = (uint8_t)((inst.colorA + 2) % kIndPaletteN);
  inst.flag = true;
  inst.opt = 1;
  switch (ri) {
    case IndEma: inst.p0 = existing == 0 ? m_emaPeriod : (existing == 1 ? 50 : 100); break;
    case IndEma2: inst.p0 = existing == 0 ? m_ema2Period : 400; break;
    case IndSma: inst.p0 = existing == 0 ? m_smaPeriod : 200; break;
    case IndRsi: inst.p0 = m_rsiPeriod; inst.flag = m_showRsiGuides; break;
    case IndMacd:
      inst.p0 = m_macdFast;
      inst.p1 = m_macdSlow;
      inst.p2 = m_macdSignalPeriod;
      inst.flag = m_showMacdHistogram;
      break;
    case IndBoll:
      inst.p0 = m_bollPeriod;
      inst.opt = m_bollDeviation;
      break;
    case IndSt:
      inst.p0 = m_stPeriod;
      inst.opt = m_stMultSel;
      inst.colorA = (uint8_t)((3 + existing * 2) % kIndPaletteN);
      inst.colorB = (uint8_t)((4 + existing * 2) % kIndPaletteN);
      break;
    case IndStoch:
      inst.p0 = m_stochPeriod;
      inst.p1 = m_stochSmooth;
      inst.p2 = m_stochDPeriod;
      inst.flag = m_showStochGuides;
      break;
    case IndAtr: inst.p0 = m_atrPeriod; break;
    case IndAdx:
      inst.p0 = m_adxPeriod;
      inst.flag = m_showAdxDi;
      if (existing == 0) {
        inst.colorA = 2;
        inst.colorB = 3;
        inst.colorC = 4;
      }
      break;
    case IndVol:
      inst.opt = m_volumeIntensity;
      inst.p0 = 20;
      inst.p1 = VolTotal;
      inst.colorA = (uint8_t)((3 + existing * 2) % kIndPaletteN);
      inst.colorB = (uint8_t)((4 + existing * 2) % kIndPaletteN);
      break;
    case IndCvd:
      inst.flag = m_showCvdZero;
      inst.opt = CvdLine;
      inst.p0 = 0;
      inst.p1 = 0;
      inst.colorA = 3;
      inst.colorB = 4;
      break;
    case IndOi:
      inst.opt = CvdLine;
      inst.colorA = 6;
      inst.colorB = 1;
      break;
    case IndFund:
      inst.opt = CvdLine;
      inst.flag = true;
      inst.colorA = 3;
      inst.colorB = 4;
      break;
    case IndVwap:
      inst.flag = m_showVwapBands;
      inst.opt = 1; // ±1σ bands by default
      break;
    case IndD7:
      inst.p0 = 56;
      inst.flag = false;
      inst.colorA = 3;
      inst.colorB = 4;
      inst.colorC = 0;
      break;
    case IndD7Rsi:
      inst.p0 = 14;
      inst.p1 = 9;
      inst.p2 = 21;
      inst.flag = true;
      inst.colorA = 2;
      inst.colorB = 3;
      inst.colorC = 4;
      break;
    case IndD7Score:
      inst.p0 = 56;
      inst.height = 42.0f;
      inst.colorA = 3;
      inst.colorB = 4;
      break;
    case IndD7Lvls:
      inst.p0 = 7;
      inst.flag = true;
      inst.colorA = 0;
      inst.colorB = 1;
      inst.colorC = 6;
      break;
    case IndCipherB:
      inst.p0 = 10;
      inst.p1 = 21;
      inst.p2 = 4;
      inst.height = 128.0f;
      inst.flag = true;
      inst.opt = CipherLayerAll;
      inst.colorA = 0;
      inst.colorB = 6;
      break;
    default: break;
  }
  return inst;
}

void ChartPanel::addIndicator(int ri) {
  if (ri < 0 || ri >= indicatorCount() || (ri == IndHeat && indicatorOn(IndHeat))) return;
  if (kRegistry[ri].overlay) m_overlays.push_back(makeInstance(ri));
  else m_panes.push_back(makeInstance(ri));
  if (ri == IndHeat) m_heatOn = true;
  noteSetChanged();
  if (ri == IndHeat) saveSettings();
}

void ChartPanel::removeIndicator(Ui& u, int instId) {
  int reg = -1;
  for (size_t i = 0; i < m_overlays.size(); ++i) {
    if (m_overlays[i].id == instId) {
      reg = m_overlays[i].reg;
      m_overlays.erase(m_overlays.begin() + (int)i);
      break;
    }
  }
  if (reg < 0) {
    for (size_t i = 0; i < m_panes.size(); ++i) {
      if (m_panes[i].id == instId) {
        reg = m_panes[i].reg;
        m_panes.erase(m_panes.begin() + (int)i);
        break;
      }
    }
  }
  if (reg < 0) return;
  if (m_indicatorSettingsId && m_indicatorSettingsInst == instId && u.overlayOpen(m_indicatorSettingsId)) u.closeOverlay(m_indicatorSettingsId);
  if (reg == IndHeat) {
    m_heatOn = false;
    m_bookHeat.clear();
    saveSettings();
  }
  noteSetChanged();
}

void ChartPanel::setInstanceOnChart(int instId, bool onChart) {
  for (size_t i = 0; i < m_panes.size(); ++i) {
    if (m_panes[i].id != instId) continue;
    if (!onChart || !paneKind(m_panes[i].reg)) return;
    IndicatorInstance inst = std::move(m_panes[i]);
    m_panes.erase(m_panes.begin() + (int)i);
    m_overlays.push_back(std::move(inst));
    noteSetChanged();
    saveSettings();
    return;
  }
  for (size_t i = 0; i < m_overlays.size(); ++i) {
    if (m_overlays[i].id != instId) continue;
    if (onChart || !paneKind(m_overlays[i].reg)) return;
    IndicatorInstance inst = std::move(m_overlays[i]);
    m_overlays.erase(m_overlays.begin() + (int)i);
    m_panes.push_back(std::move(inst));
    noteSetChanged();
    saveSettings();
    return;
  }
}

void ChartPanel::refreshEnabled() {
  const int regN = indicatorCount();
  m_enabled.assign((size_t)regN, false);
  for (const IndicatorInstance& o : m_overlays) if (o.reg >= 0 && o.reg < regN) m_enabled[(size_t)o.reg] = true;
  for (const IndicatorInstance& p : m_panes) if (p.reg >= 0 && p.reg < regN) m_enabled[(size_t)p.reg] = true;
}

void ChartPanel::formatInstanceName(const IndicatorInstance& inst, char* out,
                                   size_t n) const {
  indicatorName(inst.reg, inst.p0, inst.p1, inst.p2, inst.opt, out, n);
}

static uint64_t bits_double(double d) {
  uint64_t u;
  std::memcpy(&u, &d, 8);
  return u;
}

static uint64_t candleSig(const CandleSeries& cs) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](uint64_t x) {
    h ^= x;
    h *= 1099511628211ull;
  };
  mix(cs.v.size());
  mix((uint64_t)(cs.tf.kind + 1));
  mix(bits_double(cs.tf.value));
  mix((uint64_t)(cs.sym + 1));
  if (!cs.v.empty()) {
    const Candle& c = cs.v.back();
    mix(bits_double(c.ts));
    mix(bits_double(c.o));
    mix(bits_double(c.h));
    mix(bits_double(c.l));
    mix(bits_double(c.c));
    mix(bits_double(c.vol));
    mix(bits_double(c.delta));
    mix(bits_double(c.aggVol));
  }
  return h;
}

static uint64_t shapeSig(const CandleSeries& cs) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](uint64_t x) {
    h ^= x;
    h *= 1099511628211ull;
  };
  mix(cs.v.size());
  mix((uint64_t)(cs.tf.kind + 1));
  mix(bits_double(cs.tf.value));
  mix((uint64_t)(cs.sym + 1));
  if (!cs.v.empty()) {
    uint64_t u;
    std::memcpy(&u, &cs.v.front().ts, 8);
    mix(u);
    std::memcpy(&u, &cs.v.back().ts, 8);
    mix(u);
  }
  return h;
}

// Axis identity only: symbol/TF/front-bar ts. Unchanged by bar-open growth
// (which touches size/back.ts), so footprint cells stay valid and new bars
// can be folded incrementally instead of rehashing the whole window.
static uint64_t axisSig(const CandleSeries& cs) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](uint64_t x) {
    h ^= x;
    h *= 1099511628211ull;
  };
  mix((uint64_t)(cs.tf.kind + 1));
  mix(bits_double(cs.tf.value));
  mix((uint64_t)(cs.sym + 1));
  if (!cs.v.empty()) {
    uint64_t u;
    std::memcpy(&u, &cs.v.front().ts, 8);
    mix(u);
  }
  return h;
}

void ChartPanel::ensureFootprint(const Feeds& feeds, double step) {
  const CandleSeries& cs = feeds.candles;
  uint64_t shape = shapeSig(cs);
  uint64_t axis = axisSig(cs);
  if (m_footprintVersion == feeds.orderFlow.version &&
      m_footprintGeneration == feeds.orderFlow.generation &&
      m_footprintShape == shape && m_footprintStep == step)
    return;

  auto barForTrade = [&](const OrderFlowTrade& trade) {
    if (cs.v.empty() || trade.ts < cs.v.front().ts) return -1;
    int lo = 0, hi = (int)cs.v.size() - 1;
    while (lo < hi) {
      int mid = (lo + hi + 1) / 2;
      if (cs.v[(size_t)mid].ts <= trade.ts) lo = mid;
      else hi = mid - 1;
    }
    return lo;
  };

  // Batched fold of trade range [firstTi, endTi): aggregate by (bar, tick),
  // sort once, then single-pass merge against the existing per-bar ranges.
  // The per-print insert used to memmove the tail of a tens-of-thousands-cell
  // vector on every unseen price and shift every later bar's offset entry,
  // per print, on the live render thread.
  auto addTradesBatch = [&](size_t firstTi, size_t endTi) {
    struct NewCell {
      int bar;
      int64_t tick;
      double buy, sell;
    };
    static thread_local std::vector<NewCell> fresh;
    static thread_local std::vector<FootprintCell> merged;
    static thread_local std::vector<int> offsetsNew;
    fresh.clear();
    for (size_t ti = firstTi; ti < endTi; ++ti) {
      const OrderFlowTrade& t = feeds.orderFlow.v[ti];
      int bar = barForTrade(t);
      if (bar < 0) continue;
      int64_t tick = (int64_t)std::llround(t.price / step);
      fresh.push_back({bar, tick, t.side == 0 ? t.qty : 0.0,
                       t.side == 1 ? t.qty : 0.0});
    }
    if (fresh.empty()) return;
    std::sort(fresh.begin(), fresh.end(),
              [](const NewCell& a, const NewCell& b) {
                return a.bar != b.bar ? a.bar < b.bar : a.tick < b.tick;
              });
    size_t w = 0;
    for (size_t r = 1; r < fresh.size(); ++r) {
      if (fresh[w].bar == fresh[r].bar && fresh[w].tick == fresh[r].tick) {
        fresh[w].buy += fresh[r].buy;
        fresh[w].sell += fresh[r].sell;
      } else {
        fresh[++w] = fresh[r];
      }
    }
    fresh.resize(w + 1);

    const int barN = (int)m_footprintOffsets.size() - 1;
    merged.clear();
    merged.reserve(m_footprint.size() + fresh.size());
    offsetsNew.assign((size_t)barN + 1, 0);
    size_t fi = 0;
    for (int bar = 0; bar < barN; ++bar) {
      size_t oldIdx = (size_t)m_footprintOffsets[(size_t)bar];
      size_t oldEnd = (size_t)m_footprintOffsets[(size_t)bar + 1];
      size_t fEnd = fi;
      while (fEnd < fresh.size() && fresh[fEnd].bar == bar) ++fEnd;
      while (oldIdx < oldEnd || fi < fEnd) {
        bool takeOld = fi >= fEnd ||
                       (oldIdx < oldEnd &&
                        m_footprint[oldIdx].tick <= fresh[fi].tick);
        if (takeOld && fi < fEnd &&
            m_footprint[oldIdx].tick == fresh[fi].tick) {
          FootprintCell c = m_footprint[oldIdx++];
          c.buy += fresh[fi].buy;
          c.sell += fresh[fi++].sell;
          merged.push_back(c);
        } else if (takeOld) {
          merged.push_back(m_footprint[oldIdx++]);
        } else {
          FootprintCell c{bar, fresh[fi].tick, fresh[fi].buy, fresh[fi].sell};
          merged.push_back(c);
          ++fi;
        }
      }
      fi = fEnd;
      offsetsNew[(size_t)bar + 1] = (int)merged.size();
    }
    m_footprint.swap(merged);
    m_footprintOffsets.swap(offsetsNew);
  };

  // Append path, tolerant of bar-open growth: same axis (symbol/TF/front ts)
  // with only appended buckets just extends the offsets index and folds the
  // new prints — no rehash of the whole window.
  bool canAppend = m_footprintGeneration == feeds.orderFlow.generation &&
                   m_footprintStep == step &&
                   m_footprintTradeCount <= feeds.orderFlow.v.size() &&
                   !cs.v.empty() && m_footprintAxis == axis &&
                   cs.v.size() >= m_footprintAxisBars &&
                   cs.v.back().ts >= m_footprintBackTs &&
                   m_footprintOffsets.size() >= m_footprintAxisBars + 1;
  if (canAppend && m_footprintOffsets.size() > cs.v.size() + 1)
    canAppend = false; // axis prefix matched but the bar window shrank
  if (canAppend) {
    while (m_footprintOffsets.size() < cs.v.size() + 1)
      m_footprintOffsets.push_back(m_footprintOffsets.back());
    addTradesBatch(m_footprintTradeCount, feeds.orderFlow.v.size());
    m_footprintTradeCount = feeds.orderFlow.v.size();
    m_footprintVersion = feeds.orderFlow.version;
    m_footprintShape = shape;
    m_footprintAxisBars = cs.v.size();
    m_footprintBackTs = cs.v.back().ts;
    return;
  }

  bool canPrepend = feeds.orderFlow.prepends != m_footprintPrepends &&
                    m_footprintShape == shape && m_footprintStep == step &&
                    m_footprintOffsets.size() == cs.v.size() + 1 &&
                    !feeds.orderFlow.v.empty() &&
                    feeds.orderFlow.v.front().ts < m_footprintFrontTs;
  if (canPrepend) {
    const double oldFront = m_footprintFrontTs;
    const size_t oldN = m_footprintTradeCount;
    size_t ti = 0;
    for (; ti < feeds.orderFlow.v.size() && feeds.orderFlow.v[ti].ts < oldFront; ++ti) {}
    addTradesBatch(0, ti);
    addTradesBatch(ti + oldN, feeds.orderFlow.v.size());
    m_footprintTradeCount = feeds.orderFlow.v.size();
    m_footprintFrontTs = feeds.orderFlow.v.front().ts;
    m_footprintPrepends = feeds.orderFlow.prepends;
    m_footprintGeneration = feeds.orderFlow.generation;
    m_footprintVersion = feeds.orderFlow.version;
    return;
  }

  m_footprintVersion = feeds.orderFlow.version;
  m_footprintGeneration = feeds.orderFlow.generation;
  m_footprintPrepends = feeds.orderFlow.prepends;
  m_footprintTradeCount = feeds.orderFlow.v.size();
  m_footprintShape = shape;
  m_footprintAxis = axis;
  m_footprintAxisBars = cs.v.size();
  m_footprintBackTs = cs.v.empty() ? 0 : cs.v.back().ts;
  m_footprintStep = step;
  m_footprintFrontTs = feeds.orderFlow.v.empty() ? 0 : feeds.orderFlow.v.front().ts;
  m_footprint.clear();
  m_footprintOffsets.assign(cs.v.size() + 1, 0);
  if (cs.v.empty() || !(step > 0)) return;

  struct CellKey {
    int bar;
    int64_t tick;
    bool operator==(const CellKey& o) const { return bar == o.bar && tick == o.tick; }
  };
  struct CellKeyHash {
    size_t operator()(const CellKey& k) const {
      size_t h = (size_t)k.bar;
      h ^= (size_t)k.tick + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      return h;
    }
  };
  std::unordered_map<CellKey, FootprintCell, CellKeyHash> cells;
  cells.reserve(std::min((size_t)65536, feeds.orderFlow.v.size()));
  for (const OrderFlowTrade& trade : feeds.orderFlow.v) {
    int bar = barForTrade(trade);
    if (bar < 0) continue;
    int64_t tick = (int64_t)std::llround(trade.price / step);
    FootprintCell& cell = cells[CellKey{bar, tick}];
    cell.bar = bar;
    cell.tick = tick;
    if (trade.side == 0) cell.buy += trade.qty;
    else cell.sell += trade.qty;
  }
  m_footprint.reserve(cells.size());
  for (const auto& entry : cells) m_footprint.push_back(entry.second);
  std::sort(m_footprint.begin(), m_footprint.end(),
            [](const FootprintCell& a, const FootprintCell& b) {
              return a.bar != b.bar ? a.bar < b.bar : a.tick < b.tick;
            });
  size_t pos = 0;
  for (size_t bar = 0; bar <= cs.v.size(); ++bar) {
    while (pos < m_footprint.size() && m_footprint[pos].bar < (int)bar) ++pos;
    m_footprintOffsets[bar] = (int)pos;
  }
}

bool ChartPanel::cvdWantsFlow() const {
  for (const IndicatorInstance& inst : m_overlays) if (cvdNeedsFlow(inst)) return true;
  for (const IndicatorInstance& inst : m_panes) if (cvdNeedsFlow(inst)) return true;
  return false;
}

bool ChartPanel::oiWantsMarket() const {
  for (const IndicatorInstance& inst : m_overlays) if (inst.reg == IndOi || inst.reg == IndFund) return true;
  for (const IndicatorInstance& inst : m_panes) if (inst.reg == IndOi || inst.reg == IndFund) return true;
  return false;
}

void ChartPanel::ensureComputed(const Feeds& feeds) {
  const CandleSeries& cs = feeds.candles;
  uint64_t data = candleSig(cs);
  bool forceFull = false;
  if (cvdWantsFlow()) {
    const OrderFlowSeries& of = feeds.orderFlow;
    // Destructive mix, not XOR: prepends bump version AND generation together,
    // which XOR would cancel back to the old signature and read as "fresh".
    data = data * 0x100000001b3ull ^ of.version;
    data ^= of.generation << 1;
    if (of.prepends != m_flowSeenPrepends) {
      m_flowSeenPrepends = of.prepends;
      m_flowHistoryStale = true;
    }
    if (of.version != m_flowShadowVer) {
      m_flowShadowVer = of.version;
      m_flowQuiet = 0;
    } else if (m_flowHistoryStale && ++m_flowQuiet > 24) {
      // Fill settled: reconcile the historical bars the streamed prepends
      // touched (the incremental path only folds the live tail).
      forceFull = true;
    }
  }
  if (oiWantsMarket()) data ^= feeds.market.version;
  if (!forceFull && data == m_computedSig && m_calcToggles == m_calcGen) return;
  if (!forceFull && data != m_computedSig && shapeSig(cs) == m_shapeSig &&
      m_calcToggles == m_calcGen && updateLastBar(feeds)) {
    m_computedSig = data;
    return;
  }
  computeAll(feeds);
  m_computedSig = data;
  m_shapeSig = shapeSig(cs);
  m_calcToggles = m_calcGen;
  m_flowHistoryStale = false;
  m_flowQuiet = 0;
}

void ChartPanel::computeInstance(const CandleSeries& cs, IndicatorInstance& inst,
                                 const OrderFlowSeries* of, const MarketSeries* mkt) {
  const int ri = inst.reg;
  if (ri < 0 || ri >= indicatorCount()) {
    inst.series.clear();
    return;
  }
  kRegistry[ri].compute(cs, inst, of, mkt);
}

void ChartPanel::computeAll(const Feeds& feeds) {
  const CandleSeries& cs = feeds.candles;
  const OrderFlowSeries* of = &feeds.orderFlow;
  const MarketSeries* mkt = &feeds.market;
  for (IndicatorInstance& inst : m_overlays) computeInstance(cs, inst, of, mkt);
  for (IndicatorInstance& inst : m_panes) computeInstance(cs, inst, of, mkt);
  m_liveState = cs.v.size() >= 2;
}

bool ChartPanel::updateInstanceLast(const CandleSeries& cs, IndicatorInstance& inst,
                                    const OrderFlowSeries* of, const MarketSeries* mkt) {
  const int ri = inst.reg;
  if (ri < 0 || ri >= indicatorCount()) return false;
  if (ri == IndHeat) return true;
  // OI/FUND manage their own version cursors and may run before any bar pair.
  if (ri == IndOi || ri == IndFund)
    return kRegistry[ri].updateLast(cs, inst, of, mkt);
  const size_t n = cs.v.size();
  if (n < 2 || inst.series.size() != n) return false;
  return kRegistry[ri].updateLast ? kRegistry[ri].updateLast(cs, inst, of, mkt)
                                  : false;
}

bool ChartPanel::updateLastBar(const Feeds& feeds) {
  const CandleSeries& cs = feeds.candles;
  const OrderFlowSeries* of = &feeds.orderFlow;
  const MarketSeries* mkt = &feeds.market;
  const size_t n = cs.v.size();
  if (n < 2 || !m_liveState) return false;
  for (IndicatorInstance& inst : m_overlays) if (!updateInstanceLast(cs, inst, of, mkt)) return false;
  for (IndicatorInstance& inst : m_panes) if (!updateInstanceLast(cs, inst, of, mkt)) return false;
  return true;
}

// latest non-NaN value of a cached series
static float lastValid(const std::vector<float>& s) {
  for (size_t i = s.size(); i-- > 0;)
    if (!std::isnan(s[i])) return s[i];
  return NAN;
}

// cached "HH:MM" label for a candle second — the time axis emits up to ~20
// per frame; a 64-slot direct-mapped cache keeps localtime_r off that path
static const char* hhmmLabel(time_t secs) {
  struct Slot {
    long long key;
    char text[8];
  };
  static Slot cache[64];
  long long key = (long long)secs + 1; // 0 = empty slot
  Slot& s = cache[((uint64_t)key * 0x9E3779B97F4A7C15ull) >> 58];
  if (s.key != key) {
    s.key = key;
    struct tm tmv;
    localtime_r(&secs, &tmv);
    snprintf(s.text, sizeof(s.text), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  }
  return s.text;
}

// polyline over the visible window, breaking at NaN runs
static void drawSeries(DrawList& d, const ChartPane& pane, const std::vector<float>& s,
                       int vis0, int vis1, float startF, float bw, Color c,
                       float thick = 1.5f) {
  static thread_local std::vector<float> xy;
  auto flush = [&]() {
    if (xy.size() >= 4) d.polyline(xy.data(), (int)(xy.size() / 2), c, thick);
    xy.clear();
  };
  for (int i = vis0; i <= vis1 && i < (int)s.size(); ++i) {
    float v = s[(size_t)i];
    if (std::isnan(v)) {
      flush();
      continue;
    }
    xy.push_back(pane.area.x + (i - startF) * bw + bw * 0.5f);
    xy.push_back(pane.yOf(v));
  }
  flush();
}

static double sampleBarMs(const CandleSeries& cs) {
  if (cs.tf.kind == Timeframe::Time) return std::max(1.0, cs.tf.value) * 60000.0;
  if (cs.v.size() >= 2) return std::max(1.0, cs.v.back().ts - cs.v[cs.v.size() - 2].ts);
  return 60000.0;
}

template <typename Sample>
static void drawTimedSamples(DrawList& d, const ChartPane& pane, const CandleSeries& cs,
                             int vis0, int vis1, float startF, float bw, Color c,
                             float thick, const std::vector<Sample>& samples,
                             double (*getTs)(const Sample&),
                             double (*getV)(const Sample&), bool step) {
  if (samples.empty() || vis0 > vis1 || vis0 < 0 || cs.v.empty()) return;
  const int nC = (int)cs.v.size();
  if (vis1 >= nC) vis1 = nC - 1;
  const double t0 = cs.v[(size_t)vis0].ts;
  const double dur = sampleBarMs(cs);
  const double t1 = vis1 + 1 < nC ? cs.v[(size_t)vis1 + 1].ts
                                  : cs.v[(size_t)vis1].ts + dur;
  size_t i = 0;
  while (i + 1 < samples.size() && getTs(samples[i + 1]) <= t0) ++i;
  int stride = 1;
  const size_t nSpan = samples.size() - i;
  if (nSpan > 1500) stride = (int)(nSpan / 1500);
  static thread_local std::vector<float> xy;
  xy.clear();
  int ci = vis0;
  float prevY = 0;
  bool have = false;
  for (; i < samples.size(); i += (size_t)stride) {
    const double ts = getTs(samples[i]);
    const double v = getV(samples[i]);
    if (ts > t1 + dur) break;
    if (!std::isfinite(v)) continue;
    while (ci + 1 < nC && cs.v[(size_t)ci + 1].ts <= ts) ++ci;
    while (ci > 0 && cs.v[(size_t)ci].ts > ts) --ci;
    const double tBar = cs.v[(size_t)ci].ts;
    const double tNext = (size_t)ci + 1 < (size_t)nC ? cs.v[(size_t)ci + 1].ts
                                                    : tBar + dur;
    const double spanT = tNext - tBar;
    double frac = spanT > 0.0 ? (ts - tBar) / spanT : 0.0;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.15) frac = 1.15;
    const float x = pane.area.x + ((float)ci - startF + (float)frac) * bw;
    const float y = pane.yOf(v);
    if (have && step) {
      xy.push_back(x);
      xy.push_back(prevY);
    }
    xy.push_back(x);
    xy.push_back(y);
    prevY = y;
    have = true;
  }
  if (have) {
    xy.push_back(pane.area.x + pane.area.w);
    xy.push_back(prevY);
  }
  if (xy.size() >= 4) d.polyline(xy.data(), (int)(xy.size() / 2), c, thick);
}

static double oiSampleTs(const OiSample& s) { return s.ts; }
static double oiSampleV(const OiSample& s) { return s.oi; }
static double fundSampleTs(const FundingSample& s) { return s.ts; }
static double fundSampleV(const FundingSample& s) { return s.rate; }

static const std::vector<OiSample> kNoOi;
static const std::vector<FundingSample> kNoFund;

// Shared pane body for the sample-backed indicators (CVD / OI / FUND): zero
// line, optional candle mode, live sample polyline, or the cached series.
template <typename Sample>
static void drawSamplePane(DrawList& d, const ChartPane& pane, const CandleSeries& cs,
                           const IndicatorInstance& inst, int vis0, int vis1,
                           float startF, float bw, Color ca, float seriesWidth,
                           const std::vector<Sample>& samples,
                           double (*getTs)(const Sample&),
                           double (*getV)(const Sample&), bool step,
                           bool zeroLine) {
  const Theme& th = theme();
  bool showZero = pane.remap ? (pane.srcLo < 0 && pane.srcHi > 0)
                             : (pane.lo < 0 && pane.hi > 0);
  if (inst.flag && zeroLine && showZero)
    d.rect({pane.area.x, pane.yOf(0), pane.area.w, 1}, withAlpha(th.textDim, 0.4f));
  if (inst.opt == CvdCandles) {
    drawOscCandles(d, pane, inst, vis0, vis1, startF, bw, ca,
                   indPalette(inst.colorB));
    return;
  }
  if (!samples.empty()) {
    drawTimedSamples(d, pane, cs, vis0, vis1, startF, bw, ca, seriesWidth,
                     samples, getTs, getV, step);
    return;
  }
  drawSeries(d, pane, inst.series, vis0, vis1, startF, bw, ca, seriesWidth);
}

static void drawSeriesDir(DrawList& d, const ChartPane& pane, const std::vector<float>& s,
                          const std::vector<int8_t>& dir, int vis0, int vis1,
                          float startF, float bw, Color up, Color down, float thick) {
  static thread_local std::vector<float> xy;
  int cur = 0;
  auto flush = [&]() {
    if (xy.size() >= 4 && cur != 0)
      d.polyline(xy.data(), (int)(xy.size() / 2), cur > 0 ? up : down, thick);
    xy.clear();
  };
  for (int i = vis0; i <= vis1 && i < (int)s.size() && i < (int)dir.size(); ++i) {
    float v = s[(size_t)i];
    int dirc = dir[(size_t)i];
    if (std::isnan(v) || dirc == 0) {
      flush();
      cur = 0;
      continue;
    }
    float x = pane.area.x + (i - startF) * bw + bw * 0.5f;
    float y = pane.yOf(v);
    if (cur && dirc != cur) {
      xy.push_back(x);
      xy.push_back(y);
      flush();
      xy.push_back(x);
      xy.push_back(y);
      cur = dirc;
      continue;
    }
    cur = dirc;
    xy.push_back(x);
    xy.push_back(y);
  }
  flush();
}

struct OscRange {
  bool ok = false;
  double lo = 0;
  double hi = 1;
};


static void drawVolumeBars(DrawList& d, const ChartPane& pane, const CandleSeries& cs,
                           const IndicatorInstance& inst, int vis0, int vis1,
                           float startF, float bw) {
  Color ca = indPalette(inst.colorA);
  Color cb = indPalette(inst.colorB);
  float alpha = kChartVolumeAlpha[std::clamp(inst.opt, 0, 2)];
  int mode = volumeMode(inst);
  const std::vector<float>& s = inst.series;
  float zeroY = pane.yOf(0);
  for (int i = vis0; i <= vis1 && i < (int)s.size() && i < (int)cs.v.size(); ++i) {
    float v = s[(size_t)i];
    if (std::isnan(v)) continue;
    float x = pane.area.x + (i - startF) * bw;
    if (mode == VolSplit) {
      float sell = i < (int)inst.aux.size() && !std::isnan(inst.aux[(size_t)i])
                       ? inst.aux[(size_t)i] : 0.0f;
      float hw = std::max(1.0f, bw * 0.32f);
      float mid = x + bw * 0.5f;
      float yBuy = pane.yOf(v);
      float ySell = pane.yOf(sell);
      d.rect({mid - hw - 1.0f, yBuy, hw, std::max(1.0f, zeroY - yBuy)},
             withAlpha(ca, alpha));
      d.rect({mid + 1.0f, ySell, hw, std::max(1.0f, zeroY - ySell)},
             withAlpha(cb, alpha));
    } else if (mode == VolDelta) {
      Color col = v >= 0 ? ca : cb;
      float yTop = pane.yOf(v > 0 ? v : 0.0f);
      float yBot = pane.yOf(v > 0 ? 0.0f : v);
      d.rect({x + bw * 0.15f, yTop, bw * 0.7f, std::max(1.0f, yBot - yTop)},
             withAlpha(col, alpha));
    } else {
      const Candle& c = cs.v[(size_t)i];
      Color col = c.c >= c.o ? ca : cb;
      float yTop = pane.yOf(v);
      d.rect({x + bw * 0.15f, yTop, bw * 0.7f,
              std::max(1.0f, pane.area.y + pane.area.h - yTop)},
             withAlpha(col, alpha));
    }
  }
  const Theme& th = theme();
  if (mode == VolRvol && pane.lo < 1 && pane.hi > 1)
    d.rect({pane.area.x, pane.yOf(1), pane.area.w, 1}, withAlpha(th.textDim, 0.35f));
  if (mode == VolDelta && pane.lo < 0 && pane.hi > 0)
    d.rect({pane.area.x, pane.yOf(0), pane.area.w, 1}, withAlpha(th.textDim, 0.35f));
}

// ---------------------------------------------------------------------------
// per-indicator presentation hooks, indexed like kRegistry
// ---------------------------------------------------------------------------

static float indWidth(const IndicatorInstance& inst) {
  return kChartLineWidths[std::clamp((int)inst.width, 0, 3)];
}

// Pane value / gutter formats.
static ChartFmt fmtPlain(const IndicatorInstance&) { return chartFmtPlain; }
static ChartFmt fmtInt(const IndicatorInstance&) { return chartFmtInt; }
static ChartFmt fmtPrice(const IndicatorInstance&) { return chartFmtPrice; }
static ChartFmt fmtVol(const IndicatorInstance&) { return chartFmtVol; }
static ChartFmt fmtPct(const IndicatorInstance&) { return chartFmtPct; }
static ChartFmt fmtVolMode(const IndicatorInstance& inst) {
  return volumeMode(inst) == VolRvol ? chartFmtPlain : chartFmtVol;
}

// Visible-window y-ranges: fixed scales for bounded oscillators, symmetric
// fits around zero for signed series, padded scans of the plotted buffers
// otherwise.
static OscRange rangeFixed01(const IndicatorInstance&, int, int) {
  return {true, 0, 100};
}

static OscRange rangeScore(const IndicatorInstance&, int, int) {
  return {true, -1.2, 1.2};
}

static OscRange rangePadded(double lo, double hi) {
  if (hi < lo) return {false, 0, 1};
  double p = (hi - lo) * 0.1 + 1e-9;
  return {true, lo - p, hi + p};
}

static void scanSeries(const IndicatorInstance& inst, int vis0, int vis1,
                       double& lo, double& hi) {
  const std::vector<float>& s = inst.series;
  for (int i = vis0; i <= vis1 && i < (int)s.size(); ++i) {
    float v = s[(size_t)i];
    if (!std::isnan(v)) {
      lo = std::min(lo, (double)v);
      hi = std::max(hi, (double)v);
    }
  }
}

static void scanWithSamples(const IndicatorInstance& inst, int vis0, int vis1,
                            double& lo, double& hi) {
  const std::vector<float>& s = inst.series;
  for (int i = vis0; i <= vis1 && i < (int)s.size(); ++i) {
    float v = s[(size_t)i];
    if (!std::isnan(v)) {
      lo = std::min(lo, (double)v);
      hi = std::max(hi, (double)v);
    }
    if (i < (int)inst.aux.size() && !std::isnan(inst.aux[(size_t)i])) {
      lo = std::min(lo, (double)inst.aux[(size_t)i]);
      hi = std::max(hi, (double)inst.aux[(size_t)i]);
    }
    if (i < (int)inst.aux2.size() && !std::isnan(inst.aux2[(size_t)i])) {
      lo = std::min(lo, (double)inst.aux2[(size_t)i]);
      hi = std::max(hi, (double)inst.aux2[(size_t)i]);
    }
  }
}

static OscRange rangeScan(const IndicatorInstance& inst, int vis0, int vis1) {
  double lo = 1e300, hi = -1e300;
  scanSeries(inst, vis0, vis1, lo, hi);
  return rangePadded(lo, hi);
}

static OscRange rangeSample(const IndicatorInstance& inst, int vis0, int vis1) {
  double lo = 1e300, hi = -1e300;
  scanWithSamples(inst, vis0, vis1, lo, hi);
  return rangePadded(lo, hi);
}

static OscRange rangeFund(const IndicatorInstance& inst, int vis0, int vis1) {
  double lo = 1e300, hi = -1e300;
  scanWithSamples(inst, vis0, vis1, lo, hi);
  if (hi < lo) return {false, 0, 1};
  double m = std::max(std::fabs(lo), std::fabs(hi)) * 1.15 + 1e-12;
  return {true, -m, m};
}

static OscRange rangeMacd(const IndicatorInstance& inst, int vis0, int vis1) {
  const std::vector<float>& s = inst.series;
  double lo = 1e300, hi = -1e300;
  for (int i = vis0; i <= vis1 && i < (int)s.size(); ++i) {
    float v = s[(size_t)i];
    if (!std::isnan(v)) {
      lo = std::min(lo, (double)v);
      hi = std::max(hi, (double)v);
    }
    if (i < (int)inst.aux.size() && !std::isnan(inst.aux[(size_t)i])) {
      double sv = inst.aux[(size_t)i];
      lo = std::min(lo, sv);
      hi = std::max(hi, sv);
      if (!std::isnan(v)) {
        lo = std::min(lo, (double)v - sv);
        hi = std::max(hi, (double)v - sv);
      }
    }
  }
  if (hi < lo) return {false, 0, 1};
  double m = std::max(std::fabs(lo), std::fabs(hi)) * 1.1 + 1e-9;
  return {true, -m, m};
}

static OscRange rangeCipherB(const IndicatorInstance& inst, int vis0, int vis1) {
  double lo = 1e300, hi = -1e300;
  scanSeries(inst, vis0, vis1, lo, hi);
  const std::vector<float>& wt2 = inst.aux;
  for (int i = vis0; i <= vis1 && i < (int)wt2.size(); ++i) {
    float v = wt2[(size_t)i];
    if (!std::isnan(v)) {
      lo = std::min(lo, (double)v);
      hi = std::max(hi, (double)v);
    }
  }
  const std::vector<float>& mfi = inst.aux2;
  for (int i = vis0; i <= vis1 && i < (int)mfi.size(); ++i) {
    float v = mfi[(size_t)i];
    if (!std::isnan(v)) {
      lo = std::min(lo, (double)v);
      hi = std::max(hi, (double)v);
    }
  }
  if (hi < lo) return {true, -70, 70};
  double m = std::max(std::fabs(lo), std::fabs(hi)) * 1.06 + 1e-9;
  m = std::clamp(m, 68.0, 100.0);
  return {true, -m, m};
}

static OscRange rangeVol(const IndicatorInstance& inst, int vis0, int vis1) {
  const std::vector<float>& s = inst.series;
  const bool split = volumeMode(inst) == VolSplit;
  double lo = 1e300, hi = -1e300;
  for (int i = vis0; i <= vis1 && i < (int)s.size(); ++i) {
    float v = s[(size_t)i];
    if (!std::isnan(v)) {
      lo = std::min(lo, (double)v);
      hi = std::max(hi, (double)v);
    }
    if (split && i < (int)inst.aux.size() && !std::isnan(inst.aux[(size_t)i])) {
      lo = std::min(lo, (double)inst.aux[(size_t)i]);
      hi = std::max(hi, (double)inst.aux[(size_t)i]);
    }
  }
  if (hi < lo) return {false, 0, 1};
  if (volumeMode(inst) == VolDelta) {
    double m = std::max(std::fabs(lo), std::fabs(hi)) * 1.08 + 1e-9;
    return {true, -m, m};
  }
  return {true, 0, hi * 1.05 + 1e-9};
}

// Pane-body renderers share one signature so the table can call them blind;
// the trailing market pointer feeds OI/FUND live samples.
using PaneDraw = void (*)(DrawList&, const ChartPane&, const CandleSeries&,
                          const IndicatorInstance&, int, int, float, float,
                          const MarketSeries*);

static void drawPaneVol(DrawList& d, const ChartPane& pane, const CandleSeries& cs,
                        const IndicatorInstance& inst, int vis0, int vis1,
                        float startF, float bw, const MarketSeries*) {
  drawVolumeBars(d, pane, cs, inst, vis0, vis1, startF, bw);
}

static void drawPaneCvd(DrawList& d, const ChartPane& pane, const CandleSeries& cs,
                        const IndicatorInstance& inst, int vis0, int vis1,
                        float startF, float bw, const MarketSeries*) {
  drawSamplePane(d, pane, cs, inst, vis0, vis1, startF, bw,
                 indPalette(inst.colorA), indWidth(inst), kNoOi, oiSampleTs,
                 oiSampleV, false, true);
}

static void drawPaneOi(DrawList& d, const ChartPane& pane, const CandleSeries& cs,
                       const IndicatorInstance& inst, int vis0, int vis1,
                       float startF, float bw, const MarketSeries* mkt) {
  drawSamplePane(d, pane, cs, inst, vis0, vis1, startF, bw,
                 indPalette(inst.colorA), indWidth(inst),
                 mkt ? mkt->oi : kNoOi, oiSampleTs, oiSampleV, false, false);
}

static void drawPaneFund(DrawList& d, const ChartPane& pane, const CandleSeries& cs,
                         const IndicatorInstance& inst, int vis0, int vis1,
                         float startF, float bw, const MarketSeries* mkt) {
  drawSamplePane(d, pane, cs, inst, vis0, vis1, startF, bw,
                 indPalette(inst.colorA), indWidth(inst),
                 mkt ? mkt->funding : kNoFund, fundSampleTs, fundSampleV, true,
                 true);
}

static void drawPaneRsi(DrawList& d, const ChartPane& pane, const CandleSeries&,
                        const IndicatorInstance& inst, int vis0, int vis1,
                        float startF, float bw, const MarketSeries*) {
  const Theme& th = theme();
  if (inst.flag)
    for (double g : {30.0, 70.0})
      d.rect({pane.area.x, pane.yOf(g), pane.area.w, 1},
             withAlpha(th.textDim, 0.3f));
  drawSeries(d, pane, inst.series, vis0, vis1, startF, bw,
             indPalette(inst.colorA), indWidth(inst));
}

static void drawPaneMacd(DrawList& d, const ChartPane& pane, const CandleSeries&,
                         const IndicatorInstance& inst, int vis0, int vis1,
                         float startF, float bw, const MarketSeries*) {
  const Theme& th = theme();
  Color ca = indPalette(inst.colorA);
  Color cb = indPalette(inst.colorB);
  d.rect({pane.area.x, pane.yOf(0), pane.area.w, 1}, withAlpha(th.textDim, 0.4f));
  if (inst.flag)
    for (int i = vis0; i <= vis1 && i < (int)inst.aux.size(); ++i) {
      float mv = inst.series[(size_t)i], sv = inst.aux[(size_t)i];
      if (std::isnan(mv) || std::isnan(sv)) continue;
      float hv = mv - sv;
      float y0 = pane.yOf(hv > 0 ? hv : 0.0f);
      float y1 = pane.yOf(hv > 0 ? 0.0f : hv);
      float x = pane.area.x + (i - startF) * bw;
      d.rect({x + bw * 0.2f, y0, bw * 0.6f, std::max(y1 - y0, 1.0f)},
             withAlpha(hv >= 0 ? ca : cb, 0.35f));
    }
  drawSeries(d, pane, inst.series, vis0, vis1, startF, bw, ca, indWidth(inst));
  drawSeries(d, pane, inst.aux, vis0, vis1, startF, bw, cb, indWidth(inst));
}

static void drawPaneStoch(DrawList& d, const ChartPane& pane, const CandleSeries&,
                          const IndicatorInstance& inst, int vis0, int vis1,
                          float startF, float bw, const MarketSeries*) {
  const Theme& th = theme();
  if (inst.flag)
    for (double g : {20.0, 80.0})
      d.rect({pane.area.x, pane.yOf(g), pane.area.w, 1},
             withAlpha(th.textDim, 0.3f));
  drawSeries(d, pane, inst.series, vis0, vis1, startF, bw,
             indPalette(inst.colorA), indWidth(inst));
  drawSeries(d, pane, inst.aux, vis0, vis1, startF, bw,
             indPalette(inst.colorB), indWidth(inst));
}

static void drawPaneAdx(DrawList& d, const ChartPane& pane, const CandleSeries&,
                        const IndicatorInstance& inst, int vis0, int vis1,
                        float startF, float bw, const MarketSeries*) {
  if (inst.flag) {
    drawSeries(d, pane, inst.aux, vis0, vis1, startF, bw,
               indPalette(inst.colorB), indWidth(inst));
    drawSeries(d, pane, inst.aux2, vis0, vis1, startF, bw,
               indPalette(inst.colorC), indWidth(inst));
  }
  drawSeries(d, pane, inst.series, vis0, vis1, startF, bw,
             indPalette(inst.colorA), indWidth(inst));
}

static void drawPaneD7Rsi(DrawList& d, const ChartPane& pane, const CandleSeries&,
                          const IndicatorInstance& inst, int vis0, int vis1,
                          float startF, float bw, const MarketSeries*) {
  d7DrawRsi(d, pane, inst.series, inst.aux, inst.aux2, vis0, vis1, startF, bw,
            indPalette(inst.colorA), indPalette(inst.colorB),
            indPalette(inst.colorC), indWidth(inst), inst.flag);
}

static void drawPaneD7Score(DrawList& d, const ChartPane& pane, const CandleSeries&,
                            const IndicatorInstance& inst, int vis0, int vis1,
                            float startF, float bw, const MarketSeries*) {
  d7DrawScore(d, pane, inst.dir, vis0, vis1, startF, bw,
              indPalette(inst.colorA), indPalette(inst.colorB));
}

static void drawPaneCipherB(DrawList& d, const ChartPane& pane, const CandleSeries&,
                            const IndicatorInstance& inst, int vis0, int vis1,
                            float startF, float bw, const MarketSeries*) {
  cipherDraw(d, pane, inst.series, inst.aux, inst.aux2, inst.dir, vis0, vis1,
             startF, bw, indPalette(inst.colorA), indPalette(inst.colorB),
             indWidth(inst), inst.flag, inst.opt);
}

struct IndPresent {
  ChartFmt (*fmt)(const IndicatorInstance&); // pane value / gutter format
  OscRange (*range)(const IndicatorInstance&, int vis0, int vis1);
  PaneDraw draw; // null → plain series line
};

// Presentation hooks per indicator register, in kRegistry order. Only
// pane-kind registers are queried (call sites guard with paneKind); overlays
// carry price defaults. The static_asserts pin both tables to 22 rows.
static const IndPresent kPresent[] = {
    {fmtVolMode, rangeVol, drawPaneVol},         // VOL
    {fmtVol, rangeSample, drawPaneCvd},          // CVD
    {fmtPlain, rangeFixed01, drawPaneRsi},       // RSI
    {fmtPrice, rangeMacd, drawPaneMacd},         // MACD
    {fmtPrice, rangeScan, nullptr},              // EMA
    {fmtPrice, rangeScan, nullptr},              // SMA
    {fmtPrice, rangeScan, nullptr},              // BB
    {fmtPrice, rangeScan, nullptr},              // BOOK HEAT
    {fmtPrice, rangeScan, nullptr},              // VWAP
    {fmtPrice, rangeScan, nullptr},              // ST
    {fmtPrice, rangeScan, nullptr},              // EMA 200
    {fmtPlain, rangeFixed01, drawPaneStoch},     // STOCH
    {fmtPrice, rangeScan, nullptr},              // ATR
    {fmtVol, rangeScan, nullptr},                // OBV
    {fmtPlain, rangeFixed01, drawPaneAdx},       // ADX
    {fmtPrice, rangeScan, nullptr},              // D7
    {fmtPlain, rangeFixed01, drawPaneD7Rsi},     // D7 RSI
    {fmtInt, rangeScore, drawPaneD7Score},       // D7 SCORE
    {fmtPrice, rangeScan, nullptr},              // D7 LVLS
    {fmtVol, rangeSample, drawPaneOi},           // OI
    {fmtPct, rangeFund, drawPaneFund},           // FUND
    {fmtPlain, rangeCipherB, drawPaneCipherB},   // CIPHER B
};
static_assert(sizeof(kPresent) / sizeof(kPresent[0]) == 22, "present/reg drift");

static void drawPaneBody(DrawList& d, const ChartPane& pane, const CandleSeries& cs,
                         const IndicatorInstance& inst, int vis0, int vis1,
                         float startF, float bw, const MarketSeries* mkt = nullptr) {
  if (PaneDraw draw = kPresent[inst.reg].draw) {
    draw(d, pane, cs, inst, vis0, vis1, startF, bw, mkt);
    return;
  }
  drawSeries(d, pane, inst.series, vis0, vis1, startF, bw,
             indPalette(inst.colorA), indWidth(inst));
}

static void drawVwapLine(DrawList& d, const ChartPane& pane, const CandleSeries& cs,
                         const std::vector<float>& s, int vis0, int vis1,
                         float startF, float bw, Color c, float thick) {
  static thread_local std::vector<float> xy;
  auto flush = [&]() {
    if (xy.size() >= 4) d.polyline(xy.data(), (int)(xy.size() / 2), c, thick);
    xy.clear();
  };
  int64_t day = std::numeric_limits<int64_t>::min();
  for (int i = vis0; i <= vis1 && i < (int)s.size() && i < (int)cs.v.size(); ++i) {
    float v = s[(size_t)i];
    int64_t d = utcDay(cs.v[(size_t)i].ts);
    if (std::isnan(v)) {
      flush();
      continue;
    }
    if (d != day) {
      flush();
      day = d;
    }
    xy.push_back(pane.area.x + (i - startF) * bw + bw * 0.5f);
    xy.push_back(pane.yOf(v));
  }
  flush();
}

// Liquidity heat overlay: one full-width column per candle, two-toned at the
// spread like TradingLite/MMT heatmaps — resting bids below mid on a green
// ramp, resting asks above on a red ramp, each with a wide sequential range
// (deep → side hue → white-hot only for walls several times the reference).
// All columns live on the series' shared price lattice, so rows line up
// across the whole field and never shift when the chart pans or zooms.
// Discrete book levels are drawn as a continuous depth profile: an empty row
// sandwiched between two same-side levels takes the smaller of the two (a gap
// never renders hotter than either bounding level; the spread and the
// beyond-book tail stay dark). Rows that share a screen-pixel band max-merge
// (brightness must not change with zoom) and draw on the quantized band grid,
// so near-pixel rows tile seamlessly instead of moiréing into stripes. The
// live bar extends through the chart's right-margin space so the overlay
// runs to the plot edge.
static void drawBookHeat(DrawList& d, const HeatmapSeries& hs, const ChartPane& pane,
                         int vis0, int vis1, float startF, float bw, float intensity,
                         double minUsd, double maxUsd, int cellY, float opacity) {
  if (hs.bars.empty() || !(hs.bin > 0) || bw < 1.0f || pane.area.h < 2.0f)
    return;
  const Theme& t = theme();
  const float gain = std::clamp(intensity, 0.20f, 1.25f);
  // Columns are contiguous — the overlay reads as one continuous heat
  // field behind the candles, not per-candle tiles.
  const float colW = std::max(1.0f, bw);
  float ref = maxUsd > 0 ? (float)maxUsd : hs.ref;
  if (!(ref > 0)) ref = 1;
  cellY = std::clamp(cellY, 1, 8);
  const float paneTop = pane.area.y;
  const float paneBot = pane.area.y + pane.area.h;
  const double bin = hs.bin;

  // Side ramps. heatmapStrength maps the near-book P95 reference itself to
  // ~0.50 (midway up the deep→base segment), so ordinary depth stays dim and
  // never whitens; intensity scales how hot a given size renders, opacity
  // scales only the overlay's alpha.
  auto rampFor = [&](Color deep, Color base, float str) {
    float a = std::clamp(str / 0.50f, 0.0f, 1.0f);
    a = a * a * (3.0f - 2.0f * a);
    float b = std::clamp((str - 0.50f) / 1.00f, 0.0f, 1.0f);
    b = b * b * (3.0f - 2.0f * b);
    const float hotMix = 0.85f;
    Color hot{base.r + (t.text.r - base.r) * hotMix,
              base.g + (t.text.g - base.g) * hotMix,
              base.b + (t.text.b - base.b) * hotMix, 1.0f};
    Color c{deep.r + (base.r - deep.r) * a + (hot.r - base.r) * b,
            deep.g + (base.g - deep.g) * a + (hot.g - base.g) * b,
            deep.b + (base.b - deep.b) * a + (hot.b - base.b) * b, 1.0f};
    return withAlpha(c, std::clamp(opacity * (0.10f + 0.62f * str), 0.0f, 0.95f));
  };
  const Color bidDeep = hexColor(0x0d3833);
  const Color askDeep = hexColor(0x3a1514);

  // The live bar's heat extends through the empty right-margin space so the
  // overlay runs to the plot edge instead of stopping at the last candle —
  // the live column IS the current book, and a floating one-bar tile reads
  // as detached from the field.
  const int lastSeries = (int)hs.bars.size() - 1;
  const float paneRight = pane.area.x + pane.area.w;
  const int last = std::min(vis1, lastSeries);
  static thread_local std::vector<float> bandVal, bandAbove, bandBelow;
  for (int bar = vis0; bar <= last; ++bar) {
    const HeatmapBar& col = hs.bars[(size_t)bar];
    const int colRows = (int)col.heat.size();
    if (colRows < 1) continue;
    float x = pane.area.x + (bar - startF) * bw;
    if (x + colW < pane.area.x || x > paneRight) continue;
    float w = colW;
    if (bar == lastSeries) w = std::max(colW, paneRight - x);

    const double colLo = (double)col.row0 * bin;
    const double colHi = colLo + bin * (double)colRows;
    if (colHi <= pane.lo || colLo >= pane.hi) continue;
    int rFirst = std::clamp(
        (int)std::floor((pane.lo - colLo) / bin), 0, colRows);
    int rVisible = std::clamp(
        (int)std::ceil((pane.hi - colLo) / bin), 0, colRows);

    // Accumulate rows into pixel bands: every row contributes its (signed)
    // max magnitude to EVERY band its price span touches, so bands between
    // row tops are owned by the row's body — drawing per band afterwards
    // tiles seamlessly with no skipped-band stripes at any row height.
    auto yAt = [&](int row) {
      return pane.yOf(colLo + (double)row * bin);
    };
    const float yVisTop = yAt(rVisible);
    const float yVisBot = yAt(rFirst);
    if (!std::isfinite(yVisTop) || !std::isfinite(yVisBot)) continue;
    const int bMin = (int)std::floor(std::min(yVisTop, yVisBot) / (float)cellY);
    const int bMax = (int)std::floor(std::max(yVisTop, yVisBot) / (float)cellY);
    const int bn = bMax - bMin + 1;
    // NaN/Inf yOf (empty/degenerate price range after a TF switch) used to
    // make bn wrap to a huge size_t and freeze the frame inside assign().
    if (bn < 1 || bn > (int)pane.area.h + 64) continue;
    bandVal.assign((size_t)bn, 0.0f);
    for (int rr = rFirst; rr < rVisible; ++rr) {
      const float v = col.heat[(size_t)rr];
      if (v == 0) continue;
      float yTop = yAt(rr + 1), yBot = yAt(rr);
      if (yBot < yTop) std::swap(yBot, yTop);
      int b0 = std::max(bMin, (int)std::floor(yTop / (float)cellY));
      int b1 = std::min(bMax,
                        (int)std::floor((yBot - 1e-3f) / (float)cellY));
      for (int b = b0; b <= b1; ++b) {
        float& m = bandVal[(size_t)(b - bMin)];
        if (std::fabs(v) > std::fabs(m)) m = v;
      }
    }

    // Depth-profile gap fill in BAND space — one render path at every zoom,
    // so dragging the price axis never crosses a mode threshold and pops the
    // brightness (the old row-space fill + sub-pixel skip did exactly that).
    // An empty band between two same-side bands takes the SMALLER bound
    // (conservative — a gap never renders hotter than either side, so big
    // far-out walls can't paint the whole field). Band index grows with y,
    // i.e. DECREASING price: bandAbove[b] is the nearest nonzero band at a
    // smaller index (higher price), bandBelow[b] at a larger one. Edge seeds
    // come from the nearest levels just outside the window (capped — a gap
    // that wide means there is no near liquidity). Costs O(pixels), not
    // O(lattice rows).
    constexpr int kEdgeScan = 2048;
    float aboveSeed = 0, belowSeed = 0;
    for (int r = rVisible, n = 0; r < colRows && n < kEdgeScan; ++r, ++n)
      if (col.heat[(size_t)r] != 0) { aboveSeed = col.heat[(size_t)r]; break; }
    for (int r = rFirst - 1, n = 0; r >= 0 && n < kEdgeScan; --r, ++n)
      if (col.heat[(size_t)r] != 0) { belowSeed = col.heat[(size_t)r]; break; }
    bandAbove.resize((size_t)bn);
    bandBelow.resize((size_t)bn);
    float cur = aboveSeed;
    for (int b = 0; b < bn; ++b) {
      bandAbove[(size_t)b] = cur;
      if (bandVal[(size_t)b] != 0) cur = bandVal[(size_t)b];
    }
    cur = belowSeed;
    for (int b = bn - 1; b >= 0; --b) {
      bandBelow[(size_t)b] = cur;
      if (bandVal[(size_t)b] != 0) cur = bandVal[(size_t)b];
    }
    for (int b = 0; b < bn; ++b) {
      if (bandVal[(size_t)b] != 0) continue;
      const float a = bandAbove[(size_t)b], dn = bandBelow[(size_t)b];
      if (a > 0 && dn > 0) bandVal[(size_t)b] = std::min(a, dn);
      else if (a < 0 && dn < 0) bandVal[(size_t)b] = std::max(a, dn);
    }
    for (int b = bMin; b <= bMax; ++b) {
      // Mild vertical bleed: a band renders at least 55% of its strongest
      // neighbor, so one-pixel wall lines read as soft cluster glows instead
      // of hairlines. The MIN$ filter applies to the band's own value only;
      // bleed from a below-filter neighbor is itself below filter (55% of
      // small) and never visible in practice.
      float val = bandVal[(size_t)(b - bMin)];
      if (minUsd > 0 && std::fabs(val) < minUsd) val = 0;
      const float up = b + 1 <= bMax ? bandVal[(size_t)(b + 1 - bMin)] : 0;
      const float dn = b - 1 >= bMin ? bandVal[(size_t)(b - 1 - bMin)] : 0;
      float bleed = 0;
      if (val > 0)
        bleed = 0.55f * std::max(up > 0 ? up : 0.0f, dn > 0 ? dn : 0.0f);
      else if (val < 0)
        bleed = 0.55f * std::min(up < 0 ? up : 0.0f, dn < 0 ? dn : 0.0f);
      else
        bleed = 0.55f * (std::fabs(up) > std::fabs(dn) ? up : dn);
      if (std::fabs(bleed) > std::fabs(val)) val = bleed;
      float str = std::min(heatmapStrength(std::fabs(val), ref) * gain, 1.5f);
      if (str <= 0) continue;
      const float yTop = std::max((float)b * (float)cellY, paneTop);
      const float yBot = std::min(yTop + (float)cellY, paneBot);
      if (yBot - yTop < 0.4f) continue;
      Color c = val >= 0 ? rampFor(bidDeep, t.green, str)
                         : rampFor(askDeep, t.red, str);
      d.rect({x, yTop, w, yBot - yTop}, c);
    }
  }
}

// ---------------------------------------------------------------------------
// panel
// ---------------------------------------------------------------------------

// "1m" / "45m" / "2h" / "3d" / "100t" / "500v"
static void tfLabel(char* out, size_t n, const Timeframe& tf) {
  if (tf.kind == Timeframe::Tick) {
    snprintf(out, n, "%dt", (int)tf.value);
    return;
  }
  if (tf.kind == Timeframe::Volume) {
    char vb[16];
    chartFmtVol(vb, sizeof(vb), tf.value);
    snprintf(out, n, "%sv", vb);
    return;
  }
  double m = tf.value;
  if (std::fmod(m, 10080.0) == 0) snprintf(out, n, "%dw", (int)(m / 10080));
  else if (std::fmod(m, 1440.0) == 0) snprintf(out, n, "%dd", (int)(m / 1440));
  else if (std::fmod(m, 60.0) == 0) snprintf(out, n, "%dh", (int)(m / 60));
  else snprintf(out, n, "%dm", (int)m);
}

// Parses "45m" / "2h" / "3d" / "100t" / "500v" (case-insensitive, optional
// spaces). Time values must resolve to whole minutes in [1, 1e6]; tick/volume
// values to [1, 1e6].
static bool parseTimeframe(const char* s, Timeframe& out) {
  while (*s == ' ') ++s;
  char* end = nullptr;
  double v = strtod(s, &end);
  if (end == s || !(v > 0)) return false;
  char unit = *end;
  if (unit >= 'A' && unit <= 'Z') unit = (char)(unit - 'A' + 'a');
  for (++end; *end; ++end)
    if (*end != ' ') return false;

  double mult = 1;
  Timeframe::Kind kind = Timeframe::Time;
  if (unit == 'm') mult = 1;
  else if (unit == 'h') mult = 60;
  else if (unit == 'd') mult = 1440;
  else if (unit == 'w') mult = 10080;
  else if (unit == 't') kind = Timeframe::Tick;
  else if (unit == 'v') kind = Timeframe::Volume;
  else return false;

  double value = v * mult;
  if (value < 1 || value > 1e6 || value != std::floor(value)) return false;
  out = Timeframe{kind, value};
  return true;
}

// preset chip row: 1m 5m 15m 1h 4h 1d 1w (minutes)
static const struct {
  const char* label;
  double minutes;
} kTfPresets[] = {{"1m", 1},  {"5m", 5},    {"15m", 15}, {"1h", 60},
                  {"4h", 240}, {"1d", 1440}, {"1w", 10080}};

void ChartPanel::resetSettings() {
  m_showGrid = m_showWicks = m_showLastPrice = m_showCrosshair = true;
  m_showLastPriceLine = m_showLastPriceLabel = m_showBarCountdown = true;
  m_lastPriceStyle = m_lastPriceWidth = m_lastPriceColor = 0;
  m_candleBody = CandleGhost;
  m_showTimeLabels = m_showIndicatorLabels = true;
  m_showCvdZero = m_showRsiGuides = m_showMacdHistogram = true;
  m_showVwapBands = m_showStochGuides = m_showAdxDi = true;
  m_gutterWidth = m_lineWidth = m_volumeIntensity = 1;
  m_chartType = 0;
  m_footprintGrouping = 0;
  m_footprintImbalance = 1;
  m_showFootprintText = true;
  m_showFootprintPoc = true;
  m_showFootprintImbalances = true;
  m_showFootprintStacked = true;
  m_footprintHeatmap = 1;
  m_footprintMinCell = 0;
  m_tpoBracket = 0;
  m_emaPeriod = 21;
  m_ema2Period = 200;
  m_smaPeriod = 50;
  m_rsiPeriod = 14;
  m_macdFast = 12;
  m_macdSlow = 26;
  m_macdSignalPeriod = 9;
  m_bollPeriod = 20;
  m_bollDeviation = 1;
  m_stPeriod = 10;
  m_stMultSel = 2;
  m_stochPeriod = 14;
  m_stochSmooth = 3;
  m_stochDPeriod = 3;
  m_atrPeriod = 14;
  m_adxPeriod = 14;
  m_flowMask = kAllVenuesMask;
  m_heatOn = true;
  m_heatIntensity = 0.85f;
  m_heatOpacity = 1.0f;
  m_heatResSel = 100;
  m_heatMinSel = 0;
  m_heatMaxSel = 100;
  m_heatBinSel = 0;
  m_indicatorWidths.assign((size_t)indicatorCount(), 1);
  m_settingsList.scroll = 0;
}

// Persisted settings schema: one entry per CSV column of the current storage
// version ("20"), in append order. The Widths sentinel stands for the
// per-indicator width vector; Mask is the venue flow bitmask. Legacy storage
// versions map their columns onto this table in loadSettings().
const ChartPanel::Setting ChartPanel::kSettings[] = {
    {.type = SettType::Bool, .b = &ChartPanel::m_showGrid},
    {.type = SettType::Bool, .b = &ChartPanel::m_showWicks},
    {.type = SettType::Bool, .b = &ChartPanel::m_showLastPrice},
    {.type = SettType::Bool, .b = &ChartPanel::m_showCrosshair},
    {.type = SettType::Bool, .b = &ChartPanel::m_showTimeLabels},
    {.type = SettType::Bool, .b = &ChartPanel::m_showIndicatorLabels},
    {.type = SettType::Bool, .b = &ChartPanel::m_showCvdZero},
    {.type = SettType::Bool, .b = &ChartPanel::m_showRsiGuides},
    {.type = SettType::Bool, .b = &ChartPanel::m_showMacdHistogram},
    {.type = SettType::Int, .lo = 0, .hi = 2, .i = &ChartPanel::m_gutterWidth},
    {.type = SettType::Int, .lo = 0, .hi = 3, .i = &ChartPanel::m_lineWidth},
    {.type = SettType::Int, .lo = 0, .hi = 2, .i = &ChartPanel::m_volumeIntensity},
    {.type = SettType::Int, .lo = 2, .hi = 400, .i = &ChartPanel::m_emaPeriod},
    {.type = SettType::Int, .lo = 2, .hi = 400, .i = &ChartPanel::m_smaPeriod},
    {.type = SettType::Int, .lo = 2, .hi = 100, .i = &ChartPanel::m_rsiPeriod},
    {.type = SettType::Int, .lo = 2, .hi = 100, .i = &ChartPanel::m_macdFast},
    // macdSlow's real lower bound is m_macdFast + 1 (applied after the clamp pass)
    {.type = SettType::Int, .lo = 0, .hi = 200, .i = &ChartPanel::m_macdSlow},
    {.type = SettType::Int, .lo = 2, .hi = 100, .i = &ChartPanel::m_macdSignalPeriod},
    {.type = SettType::Int, .lo = 2, .hi = 400, .i = &ChartPanel::m_bollPeriod},
    {.type = SettType::Int, .lo = 0, .hi = 3, .i = &ChartPanel::m_bollDeviation},
    {.type = SettType::Bool, .b = &ChartPanel::m_showLastPriceLine},
    {.type = SettType::Bool, .b = &ChartPanel::m_showLastPriceLabel},
    {.type = SettType::Bool, .b = &ChartPanel::m_showBarCountdown},
    {.type = SettType::Int, .lo = 0, .hi = 2, .i = &ChartPanel::m_lastPriceStyle},
    {.type = SettType::Int, .lo = 0, .hi = 2, .i = &ChartPanel::m_lastPriceWidth},
    {.type = SettType::Int, .lo = 0, .hi = 2, .i = &ChartPanel::m_lastPriceColor},
    {.type = SettType::Int, .lo = 0, .hi = 3, .i = &ChartPanel::m_chartType},
    {.type = SettType::Int, .lo = 0, .hi = 3, .i = &ChartPanel::m_footprintGrouping},
    {.type = SettType::Int, .lo = 0, .hi = 2, .i = &ChartPanel::m_footprintImbalance},
    {.type = SettType::Bool, .b = &ChartPanel::m_showFootprintText},
    {.type = SettType::Int, .lo = 0, .hi = 1, .i = &ChartPanel::m_tpoBracket},
    {.type = SettType::Bool, .b = &ChartPanel::m_showFootprintPoc},
    {.type = SettType::Bool, .b = &ChartPanel::m_showFootprintImbalances},
    {.type = SettType::Bool, .b = &ChartPanel::m_showFootprintStacked},
    {.type = SettType::Int, .lo = 0, .hi = 2, .i = &ChartPanel::m_footprintHeatmap},
    {.type = SettType::Widths},
    {.type = SettType::Mask, .u = &ChartPanel::m_flowMask},
    {.type = SettType::Int, .lo = 0, .hi = 4, .i = &ChartPanel::m_footprintMinCell},
    {.type = SettType::Bool, .b = &ChartPanel::m_heatOn},
    {.type = SettType::Pct100, .lo = 20, .hi = 125, .f = &ChartPanel::m_heatIntensity},
    {.type = SettType::Int, .lo = 0, .hi = 100, .i = &ChartPanel::m_heatMinSel},
    {.type = SettType::Int, .lo = 0, .hi = 100, .i = &ChartPanel::m_heatMaxSel},
    {.type = SettType::Int, .lo = 2, .hi = 400, .i = &ChartPanel::m_ema2Period},
    {.type = SettType::Bool, .b = &ChartPanel::m_showVwapBands},
    {.type = SettType::Int, .lo = 2, .hi = 100, .i = &ChartPanel::m_stPeriod},
    {.type = SettType::Int, .lo = 0, .hi = 3, .i = &ChartPanel::m_stMultSel},
    {.type = SettType::Int, .lo = 2, .hi = 100, .i = &ChartPanel::m_stochPeriod},
    {.type = SettType::Int, .lo = 1, .hi = 20, .i = &ChartPanel::m_stochSmooth},
    {.type = SettType::Int, .lo = 1, .hi = 20, .i = &ChartPanel::m_stochDPeriod},
    {.type = SettType::Int, .lo = 2, .hi = 100, .i = &ChartPanel::m_atrPeriod},
    {.type = SettType::Int, .lo = 2, .hi = 100, .i = &ChartPanel::m_adxPeriod},
    {.type = SettType::Bool, .b = &ChartPanel::m_showStochGuides},
    {.type = SettType::Bool, .b = &ChartPanel::m_showAdxDi},
    {.type = SettType::Pct100, .lo = 10, .hi = 100, .f = &ChartPanel::m_heatOpacity},
    {.type = SettType::Int, .lo = 0, .hi = 100, .i = &ChartPanel::m_heatResSel},
    {.type = SettType::Int, .lo = 0, .hi = 2, .i = &ChartPanel::m_candleBody},
    {.type = SettType::Int, .lo = 0, .hi = 100, .i = &ChartPanel::m_heatBinSel},
};

void ChartPanel::saveSettings() {
  constexpr int kN = (int)(sizeof(kSettings) / sizeof(kSettings[0]));
  // m_heatOn is the source of truth (add/removeIndicator maintain it). Do NOT
  // re-derive it from indicatorOn() here: loadSettings calls saveSettings()
  // before the first-frame defaults create the instances, which would force
  // the flag off and the overlay would never survive a reload.
  for (int i = 0; i < kN; ++i) { // clamp pass
    const Setting& s = kSettings[i];
    switch (s.type) {
      case SettType::Bool:
      case SettType::Widths:
        break;
      case SettType::Pct100:
        (this->*(s.f)) =
            std::clamp((this->*(s.f)), (float)s.lo / 100.0f, (float)s.hi / 100.0f);
        break;
      case SettType::Mask:
        (this->*(s.u)) &= kAllVenuesMask;
        break;
      default:
        (this->*(s.i)) = std::clamp((this->*(s.i)), s.lo, s.hi);
        break;
    }
  }
  m_macdSlow = std::clamp(m_macdSlow, m_macdFast + 1, 200);
  if (m_indicatorWidths.size() != (size_t)indicatorCount())
    m_indicatorWidths.assign((size_t)indicatorCount(), (uint8_t)m_lineWidth);
  for (uint8_t& width : m_indicatorWidths)
    width = (uint8_t)std::clamp((int)width, 0, 3);
  std::string saved = "20";
  auto appendSetting = [&](int value) {
    saved.push_back(',');
    saved += std::to_string(value);
  };
  for (int i = 0; i < kN; ++i) {
    const Setting& s = kSettings[i];
    if (s.type == SettType::Widths) {
      for (uint8_t width : m_indicatorWidths) appendSetting(width);
      continue;
    }
    if (s.type == SettType::Bool)
      appendSetting((this->*(s.b)) ? 1 : 0);
    else if (s.type == SettType::Pct100)
      appendSetting((int)std::lround((this->*(s.f)) * 100.0f));
    else if (s.type == SettType::Mask)
      appendSetting((int)(this->*(s.u)));
    else
      appendSetting(this->*(s.i));
  }
  shell_storage_set(m_settingsKey.c_str(), saved.c_str());
  ++m_calcGen;
  ++m_rngGen;
  m_liveState = false;
}

static int parseSettingsInts(const char* saved, int* values, int capacity) {
  if (!saved || !*saved) return 0;
  const char* p = saved;
  int count = 0;
  while (*p && count < capacity) {
    char* end = nullptr;
    long value = std::strtol(p, &end, 10);
    if (!end || end == p) return 0;
    values[count++] = (int)value;
    if (*end == '\0') return count;
    if (*end != ',') return 0;
    p = end + 1;
  }
  return *p == '\0' ? count : 0;
}

void ChartPanel::loadSettings() {
  if (m_settingsLoaded) return;
  m_settingsLoaded = true;
  char* saved = shell_storage_get(m_settingsKey.c_str());
  if (!saved) return;
  int values[96]{};
  int count = parseSettingsInts(saved, values, 88);
  std::free(saved);
  const int ver = count > 0 ? values[0] : 0;
  int widthN = 0;
  if (ver == 19 && count == 78) widthN = 21;
  else if (ver == 20 && count == 79) widthN = indicatorCount();
  else return;

  // Every column maps to one schema slot in order; the Widths sentinel
  // consumes the per-indicator width block. v19 stored 21 widths; v20
  // stores indicatorCount() and extra slots default to 1.
  constexpr int kN = (int)(sizeof(kSettings) / sizeof(kSettings[0]));
  for (int i = 0, col = 1; i < kN; ++i) {
    const Setting& s = kSettings[i];
    if (s.type == SettType::Widths) {
      m_indicatorWidths.assign((size_t)indicatorCount(), 1);
      for (int w = 0; w < widthN && col < count; ++w, ++col)
        if (w < (int)m_indicatorWidths.size())
          m_indicatorWidths[(size_t)w] = (uint8_t)values[col];
      continue;
    }
    if (col >= count) break;
    switch (s.type) {
      case SettType::Bool:
        (this->*(s.b)) = values[col] != 0;
        break;
      case SettType::Pct100:
        (this->*(s.f)) = (float)values[col] / 100.0f;
        break;
      case SettType::Mask:
        (this->*(s.u)) = (uint32_t)values[col];
        break;
      default:
        (this->*(s.i)) = values[col];
        break;
    }
    ++col;
  }
  saveSettings();
}

// Global settings page: one row per entry, in display order. Rows render
// through the generic chip loop below; Restore/Venues carry their own logic.
const ChartPanel::SettingRow ChartPanel::kRows[] = {
    {"CHART TYPE",
     {{.label = "CANDLES", .width = 72, .kind = RowKind::ChartType, .v0 = 0,
       .i = &ChartPanel::m_chartType},
      {.label = "CLUSTER", .width = 72, .kind = RowKind::ChartType, .v0 = 1,
       .i = &ChartPanel::m_chartType},
      {.label = "PROFILE", .width = 72, .kind = RowKind::ChartType, .v0 = 2,
       .i = &ChartPanel::m_chartType},
      {.label = "TPO", .width = 52, .kind = RowKind::ChartType, .v0 = 3,
       .i = &ChartPanel::m_chartType}},
     4},
    {"FLOW SOURCES",
     {{.label = "ALL", .width = 48, .kind = RowKind::MaskAll},
      {.label = "SPOT", .width = 58, .kind = RowKind::MaskClass, .v0 = ClassSpot},
      {.label = "PERP", .width = 58, .kind = RowKind::MaskClass, .v0 = ClassPerp},
      {.label = "DEX", .width = 54, .kind = RowKind::MaskClass, .v0 = ClassDex},
      {.label = "VENUES…", .width = 96, .kind = RowKind::Venues}},
     5},
    {"CHART LAYERS",
     {{.label = "GRID", .width = 54, .kind = RowKind::Bool, .b = &ChartPanel::m_showGrid},
      {.label = "WICKS", .width = 62, .kind = RowKind::Bool, .b = &ChartPanel::m_showWicks},
      {.label = "PRICE", .width = 62, .kind = RowKind::Bool, .b = &ChartPanel::m_showLastPrice},
      {.label = "CROSSHAIR", .width = 86, .kind = RowKind::Bool, .b = &ChartPanel::m_showCrosshair}},
     4},
    {"CANDLE BODY",
     {{.label = "SOLID", .width = 70, .kind = RowKind::Int, .v0 = CandleSolid,
       .i = &ChartPanel::m_candleBody},
      {.label = "HOLLOW", .width = 70, .kind = RowKind::Int, .v0 = CandleHollow,
       .i = &ChartPanel::m_candleBody},
      {.label = "GHOST", .width = 70, .kind = RowKind::Int, .v0 = CandleGhost,
       .i = &ChartPanel::m_candleBody}},
     3},
    {"LAST PRICE",
     {{.label = "LINE", .width = 58, .kind = RowKind::Bool, .b = &ChartPanel::m_showLastPriceLine},
      {.label = "LABEL", .width = 64, .kind = RowKind::Bool, .b = &ChartPanel::m_showLastPriceLabel},
      {.label = "TIMER", .width = 62, .kind = RowKind::Bool, .b = &ChartPanel::m_showBarCountdown}},
     3},
    {"PRICE LINE STYLE",
     {{.label = "SOLID", .width = 72, .kind = RowKind::Int, .v0 = 0,
       .i = &ChartPanel::m_lastPriceStyle},
      {.label = "DASHED", .width = 72, .kind = RowKind::Int, .v0 = 1,
       .i = &ChartPanel::m_lastPriceStyle},
      {.label = "DOTTED", .width = 72, .kind = RowKind::Int, .v0 = 2,
       .i = &ChartPanel::m_lastPriceStyle}},
     3},
    {"PRICE LINE WIDTH",
     {{.label = "1.0", .width = 52, .kind = RowKind::Int, .v0 = 0,
       .i = &ChartPanel::m_lastPriceWidth},
      {.label = "1.5", .width = 52, .kind = RowKind::Int, .v0 = 1,
       .i = &ChartPanel::m_lastPriceWidth},
      {.label = "2.0", .width = 52, .kind = RowKind::Int, .v0 = 2,
       .i = &ChartPanel::m_lastPriceWidth}},
     3},
    {"PRICE COLOR",
     {{.label = "ACCENT", .width = 70, .kind = RowKind::Int, .v0 = 0,
       .i = &ChartPanel::m_lastPriceColor},
      {.label = "SIDE", .width = 58, .kind = RowKind::Int, .v0 = 1,
       .i = &ChartPanel::m_lastPriceColor},
      {.label = "NEUTRAL", .width = 76, .kind = RowKind::Int, .v0 = 2,
       .i = &ChartPanel::m_lastPriceColor}},
     3},
    {"LABELS",
     {{.label = "TIME", .width = 58, .kind = RowKind::Bool, .b = &ChartPanel::m_showTimeLabels},
      {.label = "INDICATORS", .width = 96, .kind = RowKind::Bool,
       .b = &ChartPanel::m_showIndicatorLabels}},
     2},
    {"PRICE GUTTER",
     {{.label = "NARROW", .width = 70, .kind = RowKind::Int, .v0 = 0,
       .i = &ChartPanel::m_gutterWidth},
      {.label = "NORMAL", .width = 70, .kind = RowKind::Int, .v0 = 1,
       .i = &ChartPanel::m_gutterWidth},
      {.label = "WIDE", .width = 70, .kind = RowKind::Int, .v0 = 2,
       .i = &ChartPanel::m_gutterWidth}},
     3},
    {"FOOTPRINT GROUPING",
     {{.label = "AUTO", .width = 66, .kind = RowKind::Int, .v0 = 0,
       .i = &ChartPanel::m_footprintGrouping},
      {.label = "FINE", .width = 66, .kind = RowKind::Int, .v0 = 1,
       .i = &ChartPanel::m_footprintGrouping},
      {.label = "MEDIUM", .width = 72, .kind = RowKind::Int, .v0 = 2,
       .i = &ChartPanel::m_footprintGrouping},
      {.label = "COARSE", .width = 66, .kind = RowKind::Int, .v0 = 3,
       .i = &ChartPanel::m_footprintGrouping}},
     4},
    {"IMBALANCE RATIO",
     {{.label = "3x", .width = 52, .kind = RowKind::Int, .v0 = 0,
       .i = &ChartPanel::m_footprintImbalance},
      {.label = "4x", .width = 52, .kind = RowKind::Int, .v0 = 1,
       .i = &ChartPanel::m_footprintImbalance},
      {.label = "5x", .width = 52, .kind = RowKind::Int, .v0 = 2,
       .i = &ChartPanel::m_footprintImbalance}},
     3},
    {"FOOTPRINT LAYERS",
     {{.label = "BID x ASK", .width = 92, .kind = RowKind::Bool,
       .b = &ChartPanel::m_showFootprintText},
      {.label = "POC", .width = 52, .kind = RowKind::Bool, .b = &ChartPanel::m_showFootprintPoc},
      {.label = "IMBALANCE", .width = 88, .kind = RowKind::Bool,
       .b = &ChartPanel::m_showFootprintImbalances},
      {.label = "STACKED", .width = 72, .kind = RowKind::Bool,
       .b = &ChartPanel::m_showFootprintStacked}},
     4},
    {"MIN CELL VOL",
     {{.label = "OFF", .width = 48, .kind = RowKind::Int, .v0 = 0,
       .i = &ChartPanel::m_footprintMinCell},
      {.label = "1%", .width = 48, .kind = RowKind::Int, .v0 = 1,
       .i = &ChartPanel::m_footprintMinCell},
      {.label = "2%", .width = 48, .kind = RowKind::Int, .v0 = 2,
       .i = &ChartPanel::m_footprintMinCell},
      {.label = "5%", .width = 48, .kind = RowKind::Int, .v0 = 3,
       .i = &ChartPanel::m_footprintMinCell},
      {.label = "10%", .width = 52, .kind = RowKind::Int, .v0 = 4,
       .i = &ChartPanel::m_footprintMinCell}},
     5},
    {"CLUSTER HEATMAP",
     {{.label = "QUIET", .width = 62, .kind = RowKind::Int, .v0 = 0,
       .i = &ChartPanel::m_footprintHeatmap},
      {.label = "NORMAL", .width = 70, .kind = RowKind::Int, .v0 = 1,
       .i = &ChartPanel::m_footprintHeatmap},
      {.label = "STRONG", .width = 68, .kind = RowKind::Int, .v0 = 2,
       .i = &ChartPanel::m_footprintHeatmap}},
     3},
    {"TPO BRACKET",
     {{.label = "30 MIN", .width = 70, .kind = RowKind::Int, .v0 = 0,
       .i = &ChartPanel::m_tpoBracket},
      {.label = "60 MIN", .width = 70, .kind = RowKind::Int, .v0 = 1,
       .i = &ChartPanel::m_tpoBracket}},
     2},
    {"RESTORE", {}, 0},
};

void ChartPanel::drawSettings(Ui& u, Rect area) {
  const Theme& t = theme();
  // Indicator periods/guides live beside their labels now. The global page is
  // limited to chart-wide presentation and active chart-type controls.
  constexpr int kRowCount = (int)(sizeof(kRows) / sizeof(kRows[0]));
  const float settingsRowH = area.w < 110.0f ? 125.0f
                             : area.w < 190.0f ? 94.0f
                             : area.w < 340.0f ? 68.0f
                                               : 44.0f;
  listView(u, area, kRowCount, settingsRowH, m_settingsList,
           [&](Ui& rowUi, DrawList& d, Rect row, int index) {
             const SettingRow& sr = kRows[index];
             char id[32];
             snprintf(id, sizeof(id), "##chart-setting-%d", index);
             rowUi.pushId(id);
             d.rect({row.x + 10, row.y + row.h - 1, row.w - 20, 1},
                    withAlpha(t.border, 0.65f));
             d.textAligned({row.x + 10, row.y + 2, row.w - 20, 16}, sr.title,
                           t.textDim, DrawList::Left);
             float x = row.x + 10, y = row.y + 19, h = 21;
             const float optionStart = x;
             const float optionRight = row.x + row.w - 10.0f;
             for (int oi = 0; oi < sr.optCount; ++oi) {
               const RowOpt& o = sr.opts[oi];
               if (o.kind == RowKind::Restore) {
                 if (button(rowUi, {x, y, std::min(142.0f, row.w - 20.0f), 23},
                            "RESET TO DEFAULTS")) {
                   resetSettings();
                   saveSettings();
                 }
                 continue;
               }
               float width =
                   std::min(o.width, std::max(20.0f, optionRight - optionStart));
               if (x > optionStart && x + width > optionRight) {
                 x = optionStart;
                 y += 25.0f;
               }
               bool on = false;
               switch (o.kind) {
                 case RowKind::Bool: on = this->*(o.b); break;
                 case RowKind::Int:
                 case RowKind::ChartType: on = (this->*(o.i)) == o.v0; break;
                 case RowKind::MaskAll: on = m_flowMask == kAllVenuesMask; break;
                 case RowKind::MaskClass: {
                   const uint32_t mask = venueMaskForClass((uint8_t)o.v0);
                   on = (m_flowMask & mask) == mask;
                   break;
                 }
                 case RowKind::Venues: on = u.overlayOpen(m_flowPickerId); break;
                 default: break;
               }
               bool clicked = y + h <= row.y + row.h - 2.0f &&
                              chip(rowUi, {x, y, width, h}, o.label, on);
               if (clicked && o.kind == RowKind::Venues) {
                 if (u.overlayOpen(m_flowPickerId)) {
                   u.closeOverlay(m_flowPickerId);
                 } else {
                   if (!m_flowPickerId) m_flowPickerId = u.id("##flowpicker");
                   float pw = std::min(300.0f, std::max(220.0f, area.w));
                   float ph = std::min(440.0f, std::max(180.0f, area.h - 8.0f));
                   m_flowPickerRect = {
                       area.x + std::max(0.0f, area.w - pw - 6.0f),
                       area.y + 4.0f, pw, ph};
                   m_flowPickerList.scroll = 0;
                   u.openOverlay(m_flowPickerId, m_flowPickerRect);
                   u.input.pressed = false;
                 }
               } else if (clicked) {
                 switch (o.kind) {
                   case RowKind::Bool: (this->*(o.b)) = !(this->*(o.b)); break;
                   case RowKind::Int: (this->*(o.i)) = o.v0; break;
                   case RowKind::ChartType:
                     (this->*(o.i)) = o.v0;
                     m_barWidth = barWidthDefault(o.v0);
                     break;
                   case RowKind::MaskAll: m_flowMask = kAllVenuesMask; break;
                   case RowKind::MaskClass:
                     m_flowMask ^= venueMaskForClass((uint8_t)o.v0);
                     break;
                   default: break;
                 }
                 saveSettings();
               }
               x += width + 4;
             }
             rowUi.popId();
           });
}

// ---------------------------------------------------------------------------
// per-frame plot context
// ---------------------------------------------------------------------------

void ChartPanel::PlotCtx::updateView(float barWidth) {
  slots = std::max(10, (int)std::floor(price.area.w / barWidth));
  bw = price.area.w / slots;
  freeMax = (float)std::min(20, slots / 4);
  endSlot = size - 1 + freeMax - scroll;
  startF = endSlot - (slots - 1);
}

void ChartPanel::PlotCtx::clampScroll() {
  scroll = std::clamp(scroll, 0.0f, (float)std::max(0, size - 20));
}

float ChartPanel::PlotCtx::clampBarWidth(int chartType, float value) const {
  return std::clamp(value, 2.0f, barWidthMax(chartType));
}

void ChartPanel::draw(Ui& u, Rect r, Feeds& feeds) {
  const Theme& t = theme();
  const CandleSeries& cs = feeds.candles;
  loadSettings();
  feeds.setFlowMask(m_flowMask);
  if (m_appliedChartType != m_chartType) {
    // Mode-specific spacing is a presentation default, not a history limit.
    // Reset it on mode entry so footprint/profile's wide text layout cannot
    // leave the chart showing only a handful of bars after a mode switch.
    m_barWidth = barWidthDefault(m_chartType);
    m_appliedChartType = m_chartType;
    if ((m_chartType == 1 || m_chartType == 2) && feeds.orderFlow.v.empty())
      feeds.refreshOrderFlow();
  }

  const int regN = indicatorCount();
  if (!m_pickerId) { // first frame: defaults
    if (m_indicatorWidths.size() != (size_t)regN)
      m_indicatorWidths.assign((size_t)regN, (uint8_t)m_lineWidth);
    addIndicator(IndEma);
    if (m_heatOn) addIndicator(IndHeat);
    addIndicator(IndVol);
    addIndicator(IndCvd);
    m_pickerId = u.id("##indpicker");
    m_tfPickerId = u.id("##tfpicker");
  }
  refreshEnabled();
  if (m_chartType == 1 || m_chartType == 2 || cvdWantsFlow())
    feeds.requestOrderFlow();

  float chartTop = drawHeader(u, r, feeds);

  if (m_settingsOpen) {
    drawSettings(u, {r.x, r.y + chartTop, r.w, r.h - chartTop});
    return;
  }

  if (cs.v.empty()) {
    u.draw.textAligned(r, "loading candles…", t.textDim, DrawList::Center);
    return;
  }

  ensureComputed(feeds);

  PlotCtx ctx;
  layoutPlot(u, feeds, r, chartTop, ctx);
  navAndRanges(u, feeds, ctx);
  drawPlotChrome(u, feeds, ctx);
  drawTimeAxisRow(u, feeds, ctx);
  if (drawPricePane(u, feeds, ctx)) return;
  if (drawIndicatorPanes(u, feeds, ctx)) return;
  drawLastPriceRow(u, feeds, ctx);
  drawCrosshairRow(u, feeds, ctx);
}

// ---------------------------------------------------------------------------
// header chrome: identity, chart-type / settings / add chips, timeframe row
// ---------------------------------------------------------------------------

// Responsive header: controls stay pinned to the right while descriptive
// venue text yields first. The symbol remains readable in narrow panes.
float ChartPanel::drawHeader(Ui& u, Rect r, Feeds& feeds) {
  const Theme& t = theme();
  const CandleSeries& cs = feeds.candles;
  const bool wideHeader = r.w >= 300.0f;
  const float settingsW = wideHeader ? 72.0f : 48.0f;
  const float typeW = wideHeader ? 72.0f : 48.0f;
  Rect addBtn{r.x + r.w - 26, r.y + 3, 22, 20};
  Rect settingsBtn{addBtn.x - settingsW - 6.0f, r.y + 3, settingsW, 20};
  Rect typeBtn{settingsBtn.x - typeW - 6.0f, r.y + 3, typeW, 20};
  Rect identity{r.x + 8.0f, r.y,
                std::max(0.0f, typeBtn.x - r.x - 12.0f), 26};
  char sym[16];
  snprintf(sym, sizeof(sym), "%sUSDT", symbols::kNames[feeds.symbol]);
  u.draw.setFont(FontMonoSemibold);
  u.draw.textFit(identity, sym, t.text, DrawList::Left);
  float symW = u.draw.measure(sym);
  u.draw.setFont(FontMono);
  char tfbuf[16];
  tfLabel(tfbuf, sizeof(tfbuf), cs.tf);
  char marketSuffix[48] = "";
  if (!feeds.market.oi.empty() || !feeds.market.funding.empty()) {
    char oiBuf[20] = "", fundBuf[20] = "";
    if (!feeds.market.oi.empty())
      formatFootprint(oiBuf, sizeof(oiBuf), feeds.market.oi.back().oi);
    if (!feeds.market.funding.empty())
      snprintf(fundBuf, sizeof(fundBuf), "%+.4f%%",
               feeds.market.funding.back().rate * 100.0);
    snprintf(marketSuffix, sizeof(marketSuffix), "  OI %s  FUND %s", oiBuf, fundBuf);
  }
  char sub[112];
  if ((m_chartType == 1 || m_chartType == 2) && feeds.orderFlow.v.empty())
    snprintf(sub, sizeof(sub), "Binance Perp  %s  FLOW LOADING%s", tfbuf, marketSuffix);
  else if (m_chartType == 1 || m_chartType == 2)
    snprintf(sub, sizeof(sub), "Binance Perp  %s  %zu PRINTS%s", tfbuf,
             feeds.orderFlow.v.size(), marketSuffix);
  else
    snprintf(sub, sizeof(sub), "Binance Perp  %s%s", tfbuf, marketSuffix);
  if (wideHeader) {
    Rect subRect{identity.x + symW + 12.0f, identity.y,
                 std::max(0.0f, identity.w - symW - 12.0f), identity.h};
    u.draw.textFit(subRect, sub, t.textDim, DrawList::Left);
  }

  // Chart settings and add-indicator controls occupy stable header cells.
  const char* typeLabel = wideHeader ? kChartTypeNames[m_chartType]
                                     : m_chartType == 0 ? "CNDL"
                                       : m_chartType == 1 ? "CLST"
                                       : m_chartType == 2 ? "PROF" : "TPO";
  if (chip(u, typeBtn, typeLabel, m_chartType != 0)) {
    m_chartType = (m_chartType + 1) % 4;
    m_barWidth = barWidthDefault(m_chartType);
    saveSettings();
  }
  u.tip(u.id("##chart-type"), typeBtn,
        "chart type: candles / footprint cluster / footprint profile / TPO");

  if (chip(u, settingsBtn,
           m_settingsOpen ? "DONE" : wideHeader ? "SETTINGS" : "SET",
           m_settingsOpen)) {
    m_settingsOpen = !m_settingsOpen;
    if (m_settingsOpen) {
      if (u.overlayOpen(m_pickerId)) u.closeOverlay(m_pickerId);
      if (u.overlayOpen(m_tfPickerId)) u.closeOverlay(m_tfPickerId);
      if (m_indicatorSettingsId && u.overlayOpen(m_indicatorSettingsId))
        u.closeOverlay(m_indicatorSettingsId);
    }
  }

  // add-indicator chip → picker popover (drawn in the overlay pass)
  if (chip(u, addBtn, "+", u.overlayOpen(m_pickerId))) {
    if (u.overlayOpen(m_pickerId)) {
      u.closeOverlay(m_pickerId);
    } else {
      if (m_indicatorSettingsId && u.overlayOpen(m_indicatorSettingsId))
        u.closeOverlay(m_indicatorSettingsId);
      const float pw = 196.0f;
      const float rowH = 24.0f;
      float ph = (float)indicatorCount() * rowH + 8.0f;
      ph = std::min(ph, std::max(rowH * 8.0f + 8.0f, r.h - 36.0f));
      m_pickerList.scroll = 0;
      m_pickerRect = {std::max(r.x, addBtn.x + addBtn.w - pw), r.y + 28, pw, ph};
      u.openOverlay(m_pickerId, m_pickerRect);
    }
  }
  u.tip(u.id("addind"), addBtn, "add indicator");

  // The timeframe row yields on very short docks so the price pane keeps a
  // usable plotting surface instead of being crushed beneath fixed chrome.
  const bool showTfRow = r.h >= 90.0f;
  const float chartTop = showTfRow ? 50.0f : 27.0f;
  if (showTfRow) {
    float cx = r.x;
    const float cy = r.y + 27;
    bool presetActive = false;
    for (const auto& p : kTfPresets) {
      float w = u.draw.measure(p.label) + 16;
      if (cx + w + 6.0f + 22.0f > r.x + r.w - 2.0f) break;
      bool on = cs.tf.kind == Timeframe::Time && cs.tf.value == p.minutes;
      if (on) presetActive = true;
      if (chip(u, {cx, cy, w, 20}, p.label, on))
        feeds.setTimeframe({Timeframe::Time, p.minutes});
      cx += w + 6;
    }
    bool tfOpen = u.overlayOpen(m_tfPickerId);
    if (chip(u, {cx, cy, 22, 20}, "+", tfOpen || !presetActive)) {
      if (tfOpen) {
        u.closeOverlay(m_tfPickerId);
      } else {
        m_tfPickerRect = {cx, cy + 24, 236, 74};
        u.openOverlay(m_tfPickerId, m_tfPickerRect);
        u.input.pressed = false; // keep the opening click from blurring the field
        m_tfInput.text.clear();
        m_tfError = false;
        m_autoFocusTf = true;
      }
    }
    u.tip(u.id("customtf"), {cx, cy, 22, 20}, "custom timeframe (45m, 2h, 100t, 500v)");
  }
  u.draw.rect({r.x, r.y + chartTop - 1.0f, r.w, 1}, t.border);
  return chartTop;
}

// ---------------------------------------------------------------------------
// layout + pane resizing
// ---------------------------------------------------------------------------

// Layout: header + TF row / price pane (flex) / resizable indicator panes /
// time axis. Indicator legends live inside their plots, so no height is
// consumed by separate header strips.
void ChartPanel::layoutPlot(Ui& u, Feeds& feeds, Rect r, float chartTop, PlotCtx& ctx) {
  const CandleSeries& cs = feeds.candles;
  const float desiredGutter = kChartGutters[std::clamp(m_gutterWidth, 0, 2)];
  ctx.gutterW = std::min(desiredGutter, std::max(28.0f, r.w * 0.40f));
  ctx.chart = {r.x, r.y + chartTop, r.w, r.h - chartTop};
  const float timeAxisH = ctx.chart.h >= 80.0f ? 18.0f : 0.0f;
  const float gap = 1.0f;
  const float minPaneH = 56.0f;
  const float minPriceH = ctx.chart.h >= 80.0f ? 80.0f : std::max(20.0f, ctx.chart.h);

  ctx.nPanes = ctx.chart.h >= 150.0f ? std::min((int)m_panes.size(), 8) : 0;

  ctx.gutter = {ctx.chart.x + ctx.chart.w - ctx.gutterW, ctx.chart.y, ctx.gutterW,
                ctx.chart.h - timeAxisH};
  ctx.timeAxis = {ctx.chart.x, ctx.chart.y + ctx.chart.h - timeAxisH, ctx.chart.w,
                  timeAxisH};
  ctx.stackH = ctx.chart.h - timeAxisH;
  ctx.priceH = ctx.stackH;

  auto layoutPanes = [&]() {
    float available = std::max(0.0f, ctx.stackH - minPriceH - ctx.nPanes * gap);
    float wanted = 0.0f;
    for (int p = 0; p < ctx.nPanes; ++p)
      wanted += std::max(minPaneH, m_panes[(size_t)p].height);
    float scale = wanted > available && wanted > 0.0f ? available / wanted : 1.0f;
    float sum = 0.0f;
    for (int p = 0; p < ctx.nPanes; ++p) {
      ctx.paneHeights[p] = std::max(
          std::min(minPaneH, available / std::max(1, ctx.nPanes)),
          std::max(minPaneH, m_panes[(size_t)p].height) * scale);
      sum += ctx.paneHeights[p];
    }
    ctx.priceH = std::max(minPriceH, ctx.stackH - sum - ctx.nPanes * gap);
    ctx.price.area = {ctx.chart.x, ctx.chart.y, ctx.chart.w - ctx.gutterW, ctx.priceH};
    ctx.price.grid = true;
    ctx.price.log = scaleLog;

    float py = ctx.price.area.y + ctx.price.area.h + gap;
    for (int p = 0; p < ctx.nPanes; ++p) {
      ctx.indBg[p] = {ctx.chart.x, py, ctx.chart.w - ctx.gutterW, ctx.paneHeights[p]};
      ctx.ind[p].area = ctx.indBg[p];
      py += ctx.paneHeights[p] + gap;
    }
  };
  layoutPanes();

  // Separators are 8px interaction targets around a 1px visual hairline.
  // Dragging the first resizes price vs pane 0; later separators redistribute
  // height between their two adjacent indicator panes.
  m_resizeHotPane = -1;
  bool paneSizeChanged = false;
  for (int p = 0; p < ctx.nPanes; ++p) {
    float sepY = ctx.indBg[p].y - gap;
    Rect hit{ctx.chart.x, sepY - 3.0f, ctx.chart.w, 7.0f};
    char id[32];
    snprintf(id, sizeof(id), "##pane-resize-%d", p);
    uint64_t resizeId = u.id(id);
    Behavior b = behavior(u, hit, resizeId);
    if (b.hovered) m_resizeHotPane = p;
    if (u.input.pressed && b.held) {
      m_resizePane = p;
      m_resizeStartY = u.input.mouseY;
      m_resizeUpper = p == 0 ? ctx.priceH : ctx.paneHeights[p - 1];
      m_resizeLower = ctx.paneHeights[p];
    }
    if (m_resizePane == p && u.input.down) {
      float dy = u.input.mouseY - m_resizeStartY;
      if (p == 0) {
        float other = ctx.nPanes * gap;
        for (int q = 1; q < ctx.nPanes; ++q) other += ctx.paneHeights[q];
        float maxLower = std::max(minPaneH, ctx.stackH - minPriceH - other);
        m_panes[0].height = std::clamp(m_resizeLower - dy, minPaneH, maxLower);
      } else {
        float pair = m_resizeUpper + m_resizeLower;
        float upper = std::clamp(m_resizeUpper + dy, minPaneH,
                                 std::max(minPaneH, pair - minPaneH));
        m_panes[(size_t)p - 1].height = upper;
        m_panes[(size_t)p].height = pair - upper;
      }
      paneSizeChanged = true;
    }
    if (u.input.released && m_resizePane == p) {
      m_resizePane = -1;
      if (u.active == resizeId) u.active = 0;
    }
  }
  if (!u.input.down && !u.input.released) m_resizePane = -1;
  if (paneSizeChanged) layoutPanes();

  ctx.size = (int)cs.v.size();
  ctx.scroll = scroll;
  ctx.clampScroll();
  scroll = ctx.scroll;
  ctx.updateView(m_barWidth);
}

// ---------------------------------------------------------------------------
// navigation input + visible-window ranges
// ---------------------------------------------------------------------------

// TradingView-style navigation: wheel zooms the time scale around the bar
// under the cursor; Shift+wheel and horizontal-wheel input scroll history.
// Press-drag moves time and, when begun in the price pane, price together.
void ChartPanel::navAndRanges(Ui& u, Feeds& feeds, PlotCtx& ctx) {
  const CandleSeries& cs = feeds.candles;
  const int size = ctx.size;

  Rect stackAll{ctx.chart.x, ctx.chart.y, ctx.chart.w - ctx.gutterW, ctx.stackH};
  Rect navArea{ctx.price.area.x, ctx.chart.y, ctx.price.area.w, ctx.chart.h};
  if (u.hovered(navArea) && (u.input.wheelY != 0.0f || u.input.wheelX != 0.0f)) {
    if (u.input.shift || u.input.wheelX != 0.0f) {
      float delta = u.input.wheelX != 0.0f ? u.input.wheelX : u.input.wheelY;
      ctx.scroll += delta / std::max(ctx.bw, 1.0f);
      ctx.clampScroll();
      ctx.updateView(m_barWidth);
    } else {
      float anchorX = std::clamp(u.input.mouseX, ctx.price.area.x,
                                 ctx.price.area.x + ctx.price.area.w);
      float anchorBar = ctx.startF + (anchorX - ctx.price.area.x) / ctx.bw;
      float factor = std::exp(-u.input.wheelY * 0.0018f);
      m_barWidth = ctx.clampBarWidth(m_chartType, m_barWidth * factor);
      ctx.updateView(m_barWidth);
      float wantedStart = anchorBar - (anchorX - ctx.price.area.x) / ctx.bw;
      ctx.scroll = size - 1 + ctx.freeMax - (ctx.slots - 1) - wantedStart;
      ctx.clampScroll();
      ctx.updateView(m_barWidth);
    }
  }

  // Press-dragging the time axis changes bar spacing around the cursor.
  Rect timeScale{ctx.price.area.x, ctx.timeAxis.y, ctx.price.area.w, ctx.timeAxis.h};
  uint64_t timeScaleId = u.id("##timescale");
  if (u.input.dblClick && u.hovered(timeScale)) {
    m_barWidth = barWidthDefault(m_chartType);
    ctx.scroll = 0.0f;
    ctx.updateView(m_barWidth);
  }
  if (u.input.pressed && u.hovered(timeScale)) {
    u.active = timeScaleId;
    m_tsX = u.input.mouseX;
  }
  m_timeScaling = u.active == timeScaleId && u.input.down;
  if (m_timeScaling) {
    float dx = u.input.mouseX - m_tsX;
    if (dx != 0.0f) {
      float anchorX = std::clamp(u.input.mouseX, ctx.price.area.x,
                                 ctx.price.area.x + ctx.price.area.w);
      float anchorBar = ctx.startF + (anchorX - ctx.price.area.x) / ctx.bw;
      m_barWidth =
          ctx.clampBarWidth(m_chartType, m_barWidth * std::exp(dx * 0.010f));
      ctx.updateView(m_barWidth);
      float wantedStart = anchorBar - (anchorX - ctx.price.area.x) / ctx.bw;
      ctx.scroll = size - 1 + ctx.freeMax - (ctx.slots - 1) - wantedStart;
      ctx.clampScroll();
      ctx.updateView(m_barWidth);
    }
    m_tsX = u.input.mouseX;
  }
  if (u.input.released && u.active == timeScaleId) u.active = 0;

  float overlayLegendH = (float)m_overlays.size() * 16.0f;
  Rect overlayLegend{ctx.price.area.x, ctx.price.area.y,
                     std::min(260.0f, ctx.price.area.w), overlayLegendH + 8.0f};

  uint64_t panId = u.id("##chartpan");
  if (u.input.pressed && u.hovered(stackAll) && !u.hovered(overlayLegend) &&
      m_resizePane < 0 &&
      m_resizeHotPane < 0) {
    u.active = panId;
    m_dragX = u.input.mouseX;
    m_dragY = u.input.mouseY;
    m_panTotalX = 0.0f;
    m_panTotalY = 0.0f;
    m_panPrice = ctx.price.area.contains(u.input.mouseX, u.input.mouseY);
    m_panMoved = false;
  }
  m_panning = u.active == panId && u.input.down;
  if (m_panning) {
    float dx = u.input.mouseX - m_dragX;
    float dy = u.input.mouseY - m_dragY;
    m_panTotalX += dx;
    m_panTotalY += dy;
    if (!m_panMoved && std::fabs(dx) + std::fabs(dy) > 0.5f) {
      m_panMoved = true;
    }
    if (!m_panMoved) dx = dy = 0.0f;
    ctx.scroll += dx / ctx.bw;
    // Horizontal history panning keeps TradingView-style Auto enabled so the
    // newly visible candle window continues to fit. Only a clearly vertical
    // price-pane gesture takes ownership of the price range; the direction
    // threshold absorbs normal mouse jitter during a left/right drag.
    if (scaleAuto && m_panPrice && std::fabs(m_panTotalY) > 6.0f &&
        std::fabs(m_panTotalY) > std::fabs(m_panTotalX) * 0.75f)
      scaleAuto = false;
    if (!scaleAuto && m_panPrice && dy != 0.0f) {
      double span = m_rngPriceHi - m_rngPriceLo;
      double shift = (double)dy / ctx.price.area.h * span;
      m_rngPriceLo += shift;
      m_rngPriceHi += shift;
    }
    m_dragX = u.input.mouseX;
    m_dragY = u.input.mouseY;
    ctx.clampScroll();
    ctx.updateView(m_barWidth);
  }
  if (u.input.released && u.active == panId) u.active = 0;

  ctx.vis0 = std::max(0, (int)std::ceil(ctx.startF));
  ctx.vis1 = std::min(size - 1, (int)std::floor(ctx.endSlot));

  // visible-window ranges (price + panes): recomputed only when the window,
  // candle signature, or pane toggles change — history is append-only and the
  // live candle is covered by the signature, so cached scans stay valid
  if (m_computedSig != m_rngSig || ctx.vis0 != m_rngV0 || ctx.vis1 != m_rngV1 ||
      m_rngGen != m_rngToggles) {
    m_rngSig = m_computedSig;
    m_rngV0 = ctx.vis0;
    m_rngV1 = ctx.vis1;
    m_rngToggles = m_rngGen;
    m_rngPanes.assign((size_t)ctx.nPanes, PaneRange{false, 0, 1});

    double lo = 1e300, hi = -1e300; // price range over visible candles
    int rangeStart = ctx.vis0;
    if (m_chartType == 3)
      rangeStart = tpoWindowFirst(cs, ctx.vis1,
                                  tpoMaxSessions(ctx.price.area.w));
    for (int i = rangeStart; i <= ctx.vis1; ++i) {
      lo = std::min(lo, cs.v[(size_t)i].l);
      hi = std::max(hi, cs.v[(size_t)i].h);
    }
    for (const IndicatorInstance& inst : m_overlays) {
      if (!inst.seriesVisible || (inst.reg != IndD7 && inst.reg != IndD7Lvls)) continue;
      auto grow = [&](const std::vector<float>& s) {
        for (int i = rangeStart; i <= ctx.vis1 && i < (int)s.size(); ++i) {
          if (std::isnan(s[(size_t)i])) continue;
          lo = std::min(lo, (double)s[(size_t)i]);
          hi = std::max(hi, (double)s[(size_t)i]);
        }
      };
      grow(inst.series);
      grow(inst.aux);
      grow(inst.aux2);
    }
    if (hi >= lo) {
      m_rngFitLo = lo;
      m_rngFitHi = hi;
      if (scaleAuto) { // auto off: keep the last-fitted price range frozen
        m_rngPriceLo = lo;
        m_rngPriceHi = hi;
      }
    }

    for (int p = 0; p < ctx.nPanes; ++p) { // per-pane ranges from the window
      const IndicatorInstance& pi = m_panes[(size_t)p];
      OscRange rng = kPresent[pi.reg].range(pi, ctx.vis0, ctx.vis1);
      m_rngPanes[(size_t)p] = {rng.ok, rng.lo, rng.hi};
    }
  }
  for (int p = 0; p < ctx.nPanes; ++p) {
    const PaneRange& pr = m_rngPanes[(size_t)p];
    ctx.ind[p].lo = pr.lo;
    ctx.ind[p].hi = pr.hi;
    ctx.rangeOk[p] = pr.ok;
  }
  double padv = (m_rngPriceHi - m_rngPriceLo) * 0.06 + 1e-9;
  ctx.price.lo = m_rngPriceLo - padv;
  ctx.price.hi = m_rngPriceHi + padv;

  ctx.lastC = cs.v.back().c;
  ctx.ly = ctx.price.yOf(ctx.lastC);
  ctx.showTag =
      ctx.ly > ctx.price.area.y && ctx.ly < ctx.price.area.y + ctx.price.area.h;
  ctx.cross = m_showCrosshair &&
              ctx.price.area.contains(u.input.mouseX, u.input.mouseY);
  ctx.crossTagY = ctx.cross ? u.input.mouseY : -1e9f;

  scroll = ctx.scroll;
}

// ---------------------------------------------------------------------------
// plot chrome: surfaces, separators, grid, scale controls, price-scale drag
// ---------------------------------------------------------------------------

// The plot and its scale belong to one chart canvas, but they are distinct
// working surfaces. A quieter scale background and a functional hairline at
// the plot edge keep the y axis from visually bleeding into the data.
void ChartPanel::drawPlotChrome(Ui& u, Feeds& feeds, PlotCtx& ctx) {
  (void)feeds;
  const Theme& t = theme();
  Rect plotStack{ctx.chart.x, ctx.chart.y, ctx.chart.w - ctx.gutterW, ctx.stackH};
  Rect scaleStack{ctx.gutter.x, ctx.chart.y, ctx.gutter.w, ctx.stackH};
  u.draw.rect(plotStack, t.chartPaneBg);
  u.draw.rect(scaleStack, t.panelAlt);
  for (int p = 0; p < ctx.nPanes; ++p) {
    u.draw.rect({ctx.chart.x, ctx.indBg[p].y - 1, ctx.chart.w, 1}, t.border);
  }
  int resizeVisual = m_resizePane >= 0 ? m_resizePane : m_resizeHotPane;
  if (resizeVisual >= 0 && resizeVisual < ctx.nPanes) {
    float y = ctx.indBg[resizeVisual].y - 1.0f;
    u.draw.rect({ctx.chart.x, y, ctx.chart.w, 2.0f}, withAlpha(t.accent, 0.65f));
  }
  u.draw.rect(ctx.timeAxis, t.panelAlt);
  u.draw.rect({ctx.timeAxis.x, ctx.timeAxis.y, ctx.timeAxis.w, 1}, t.border);
  u.draw.rect({ctx.gutter.x, ctx.chart.y, 1, ctx.chart.h}, t.border);
  u.draw.rectOutline(ctx.chart, t.border, 1.0f);

  // price grid: nice ticks, horizontal lines confined to the price pane
  {
    static thread_local std::vector<double> ticks;
    niceTicks(ctx.price.lo, ctx.price.hi, 5, ticks);
    for (double tv : ticks) {
      float y = ctx.price.yOf(tv);
      if (y < ctx.price.area.y + 2 || y > ctx.price.area.y + ctx.price.area.h - 2)
        continue;
      if (m_showGrid)
        u.draw.rect({ctx.price.area.x, y, ctx.price.area.w, 1},
                    withAlpha(t.border, 0.6f));
    }
  }
  const float scaleControlY = ctx.price.area.y + ctx.price.area.h - 19.0f;
  const float scaleControlMidY = scaleControlY + 9.0f;
  drawPaneGutter(u.draw, ctx.gutter, ctx.price, chartFmtPrice, 5,
                 m_showLastPrice && ctx.showTag &&
                         (m_showLastPriceLabel || m_showBarCountdown)
                     ? ctx.ly
                     : -1e9f,
                 ctx.crossTagY, scaleControlMidY);

  // Quiet, flat scale controls. They live in the price gutter rather than
  // competing with chart content; active state is communicated by text tone,
  // not a pill or accent decoration.
  {
    auto scaleControl = [&](Rect rc, const char* id, const char* label, bool on) {
      Behavior b = behavior(u, rc, u.id(id));
      if (b.hovered || b.held)
        u.draw.rect(rc, b.held ? t.bgRaised : t.panelAlt, 1.0f);
      u.draw.textAligned(rc, label, on ? t.text : t.textDim, DrawList::Center);
      return b.clicked;
    };

    const bool showLogControl = ctx.gutter.w >= 58.0f;
    Rect autoRect{ctx.gutter.x + 2.0f, scaleControlY,
                  showLogControl ? 36.0f : ctx.gutter.w - 4.0f, 18.0f};
    Rect logRect{ctx.gutter.x + 39.0f, scaleControlY,
                 std::max(0.0f, ctx.gutter.w - 41.0f), 18.0f};
    if (scaleControl(autoRect, "##scale-auto", "AUTO", scaleAuto)) {
      scaleAuto = !scaleAuto;
      ++m_rngGen; // re-fit (or freeze) the price range this frame
    }
    if (showLogControl && scaleControl(logRect, "##scale-log", "LOG", scaleLog)) {
      scaleLog = !scaleLog;
      ++m_rngGen;
    }
  }

  // Price-scale control: dragging the axis changes scale, wheel zooms around
  // the hovered price, and double-click restores Auto. Plot dragging above is
  // responsible for translation, matching the separation used by TV charts.
  {
    Rect ps{ctx.gutter.x, ctx.price.area.y, ctx.gutter.w, ctx.price.area.h};
    float controlTop = scaleControlY; // scale toggles own the bottom row
    uint64_t psId = u.id("##pricescale");
    auto zoomPrice = [&](double factor, double anchorFrac) {
      double lo = m_rngPriceLo, hi = m_rngPriceHi;
      double span = hi - lo;
      if (!(span > 0.0)) return;
      anchorFrac = std::clamp(anchorFrac, 0.0, 1.0);
      double anchor = hi - anchorFrac * span;
      double nextLo = anchor - (anchor - lo) * factor;
      double nextHi = anchor + (hi - anchor) * factor;
      if (nextHi <= nextLo) return;
      if (nextLo <= 0.0) {
        double push = 1e-9 - nextLo;
        nextLo += push;
        nextHi += push;
      }
      m_rngPriceLo = nextLo;
      m_rngPriceHi = nextHi;
    };

    if (u.input.dblClick && u.hovered(ps) && u.input.mouseY < controlTop) {
      scaleAuto = true;
      ++m_rngGen;
    }
    if (u.hovered(ps) && u.input.mouseY < controlTop && u.input.wheelY != 0.0f) {
      scaleAuto = false;
      double frac = (double)(u.input.mouseY - ctx.price.area.y) / ctx.price.area.h;
      zoomPrice(std::exp((double)u.input.wheelY * 0.0018), frac);
    }
    if (u.input.pressed && u.hovered(ps) && u.input.mouseY < controlTop) {
      u.active = psId;
      m_psY = u.input.mouseY;
      scaleAuto = false;
    }
    if (u.active == psId && u.input.down) {
      float dy = u.input.mouseY - m_psY;
      m_psY = u.input.mouseY;
      if (dy != 0.0f) zoomPrice(std::exp((double)dy * 0.010), 0.5);
    }
    if (u.input.released && u.active == psId) u.active = 0;
  }
}

// time axis: labels + vertical gridlines, both confined to the price pane;
// anchored to absolute bar index so labels stay put while panning
void ChartPanel::drawTimeAxisRow(Ui& u, Feeds& feeds, PlotCtx& ctx) {
  const Theme& t = theme();
  const CandleSeries& cs = feeds.candles;
  int labelEvery = std::max(1, (int)(80.0f / ctx.bw));
  if (m_chartType != 3) {
    int i0 = (int)std::floor(ctx.startF);
    for (int s = 0; s < ctx.slots; ++s) {
      int i = i0 + s;
      if (i < 0 || i >= ctx.size || i % labelEvery != 0) continue;
      const char* tb = hhmmLabel((time_t)(cs.v[(size_t)i].ts / 1000.0));
      float x = ctx.price.area.x + (i - ctx.startF) * ctx.bw + ctx.bw * 0.5f;
      if (m_showGrid)
        u.draw.rect({x, ctx.price.area.y, 1, ctx.price.area.h},
                    withAlpha(t.border, 0.35f));
      if (m_showTimeLabels)
        u.draw.textAligned({x - 40, ctx.timeAxis.y, 80, ctx.timeAxis.h}, tb,
                           t.textDim, DrawList::Center);
    }
  }
}

// ---------------------------------------------------------------------------
// price pane: heat, kumo/boll/vwap fills, candles / footprint / TPO, legend
// ---------------------------------------------------------------------------

bool ChartPanel::drawPricePane(Ui& u, Feeds& feeds, PlotCtx& ctx) {
  const Theme& t = theme();
  const CandleSeries& cs = feeds.candles;

  // candles + overlays, clipped to the price pane
  u.draw.pushClip(ctx.price.area);

  bool heatVisible = false;
  for (const IndicatorInstance& o : m_overlays)
    if (o.reg == IndHeat && o.seriesVisible) {
      heatVisible = true;
      break;
    }
  if (m_enabled[IndHeat] && heatVisible) {
    sampleBookHeat(m_bookHeat, cs, feeds, m_flowMask, m_heatBinSel);
    drawBookHeat(u.draw, m_bookHeat, ctx.price, ctx.vis0, ctx.vis1, ctx.startF,
                 ctx.bw, m_heatIntensity,
                 heatMinUsdForSel(m_heatMinSel),
                 heatMaxUsdForSel(m_heatMaxSel),
                 heatPxPerRow(m_heatResSel), m_heatOpacity);
  }

  // D7 kumo sits with Bollinger: fill behind the candles.
  if (m_chartType != 3) {
    for (const IndicatorInstance& inst : m_overlays) {
      if (inst.reg != IndD7 || !inst.seriesVisible) continue;
      d7DrawCloud(u.draw, ctx.price, cs, inst.series, inst.aux, inst.aux2,
                  ctx.vis0, ctx.vis1, ctx.startF, ctx.bw,
                  indPalette(inst.colorA), indPalette(inst.colorB),
                  indPalette(inst.colorC),
                  kChartLineWidths[std::clamp((int)inst.width, 0, 3)],
                  d7BaseLength(inst.p0) * 2);
    }
  }

  // Bollinger band fill sits beneath the candles (bands cached at compute
  // time, not recomputed per frame)
  if (m_chartType != 3) {
    for (const IndicatorInstance& inst : m_overlays) {
      if (inst.reg != IndBoll || !inst.seriesVisible) continue;
      const std::vector<float>& basis = inst.series;
      static thread_local std::vector<float> bx, bTop, bBot;
      bx.clear();
      bTop.clear();
      bBot.clear();
      int last = -1;
      for (int i = ctx.vis0; i <= ctx.vis1 && i < (int)basis.size(); ++i) {
        if (i >= (int)inst.aux.size() || std::isnan(inst.aux[(size_t)i])) continue;
        bx.push_back(ctx.xOf(i));
        bTop.push_back(ctx.price.yOf(inst.aux[(size_t)i]));
        bBot.push_back(ctx.price.yOf(inst.aux2[(size_t)i]));
        last = i;
      }
      if (bx.size() >= 2) {
        bx.push_back(ctx.xOf(last) + ctx.bw);
        bTop.push_back(bTop.back());
        bBot.push_back(bBot.back());
        Color band = indPalette(inst.colorB);
        u.draw.seriesBand(bx.data(), bTop.data(), bBot.data(), (int)bx.size(),
                          withAlpha(band, 0.12f));
        static thread_local std::vector<float> topXy, botXy;
        topXy.clear();
        botXy.clear();
        for (size_t k = 0; k < bx.size(); ++k) {
          topXy.push_back(bx[k]);
          topXy.push_back(bTop[k]);
          botXy.push_back(bx[k]);
          botXy.push_back(bBot[k]);
        }
        float edge = kChartLineWidths[std::clamp((int)inst.width, 0, 3)];
        Color edgeC = withAlpha(band, 0.55f);
        if (topXy.size() >= 4)
          u.draw.polyline(topXy.data(), (int)(topXy.size() / 2), edgeC, edge);
        if (botXy.size() >= 4)
          u.draw.polyline(botXy.data(), (int)(botXy.size() / 2), edgeC, edge);
      }
    }
  }

  if (m_chartType != 3) {
    for (const IndicatorInstance& inst : m_overlays) {
      if (inst.reg != IndVwap || !inst.seriesVisible || !inst.flag ||
          inst.aux.size() != inst.series.size())
        continue;
      // σ bands are linear in k: aux/aux2 hold ±1σ, selected multiples scale
      // the distance from the line. Fills fade with k; each drawn band gets a
      // 1px edge like Bollinger's.
      int sigMask = (inst.opt & 7) ? (inst.opt & 7) : 1;
      int maxK = (sigMask & 4) ? 3 : (sigMask & 2) ? 2 : 1;
      Color band = indPalette(inst.colorB);
      static thread_local std::vector<float> bx, bTop, bBot;
      for (int k = 1; k <= maxK; ++k) {
        if (!(sigMask & (1 << (k - 1)))) continue;
        float scale = (float)k;
        bx.clear();
        bTop.clear();
        bBot.clear();
        for (int i = ctx.vis0; i <= ctx.vis1 && i < (int)inst.series.size() &&
                               i < (int)cs.v.size();
             ++i) {
          float mid = inst.series[(size_t)i];
          float up = inst.aux[(size_t)i], dn = inst.aux2[(size_t)i];
          if (std::isnan(mid) || std::isnan(up) || std::isnan(dn)) continue;
          float dist = up - mid;
          bx.push_back(ctx.xOf(i));
          bTop.push_back(ctx.price.yOf(mid + dist * scale));
          bBot.push_back(ctx.price.yOf(mid - dist * scale));
        }
        if (bx.size() < 2) continue;
        u.draw.seriesBand(bx.data(), bTop.data(), bBot.data(), (int)bx.size(),
                          withAlpha(band, k == 1 ? 0.10f : k == 2 ? 0.06f
                                                                 : 0.045f));
        static thread_local std::vector<float> exy;
        for (int side = 0; side < 2; ++side) {
          exy.clear();
          for (size_t j = 0; j < bx.size(); ++j) {
            exy.push_back(bx[j]);
            exy.push_back(side ? bBot[j] : bTop[j]);
          }
          u.draw.polyline(exy.data(), (int)(exy.size() / 2),
                          withAlpha(band, 0.5f), 1.0f);
        }
      }

      // Session-end ticks: a 1px hairline where each UTC segment ends,
      // spanning the widest drawn band on both sides of the line.
      static thread_local std::vector<float> tickYs;
      tickYs.clear();
      int64_t prevDay = std::numeric_limits<int64_t>::min();
      for (int i = std::max(ctx.vis0, 1);
           i <= ctx.vis1 && i < (int)inst.series.size() && i < (int)cs.v.size();
           ++i) {
        float mid = inst.series[(size_t)i];
        float up = inst.aux[(size_t)i], dn = inst.aux2[(size_t)i];
        if (std::isnan(mid) || std::isnan(up) || std::isnan(dn)) continue;
        int64_t dday = utcDay(cs.v[(size_t)i].ts);
        if (prevDay != std::numeric_limits<int64_t>::min() && dday != prevDay &&
            tickYs.size() >= 2) {
          float x = ctx.xOf(i) - ctx.bw * 0.5f;
          u.draw.rect({x, tickYs[0], 1.0f, tickYs[1] - tickYs[0]},
                      withAlpha(t.border, 0.55f));
        }
        prevDay = dday;
        float dist = up - mid;
        tickYs.clear();
        tickYs.push_back(ctx.price.yOf(mid + dist * (float)maxK));
        tickYs.push_back(ctx.price.yOf(mid - dist * (float)maxK));
      }
    }
  }

  // Pane indicators merged onto the price plot sit under the candles so volume
  // and delta remain a floor, not a cover.
  if (m_chartType != 3) {
    for (const IndicatorInstance& inst : m_overlays) {
      if (!inst.seriesVisible || !paneKind(inst.reg) || inst.reg != IndVol) continue;
      OscRange rng = kPresent[inst.reg].range(inst, ctx.vis0, ctx.vis1);
      if (!rng.ok) continue;
      ChartPane band = ctx.price;
      float h = ctx.price.area.h * 0.26f;
      band.area.y = ctx.price.area.y + ctx.price.area.h - h;
      band.area.h = h;
      band.lo = rng.lo;
      band.hi = rng.hi;
      u.draw.pushClip(band.area);
      drawPaneBody(u.draw, band, cs, inst, ctx.vis0, ctx.vis1, ctx.startF, ctx.bw);
      u.draw.popClip();
    }
  }

  if (m_chartType == 0) {
    const std::vector<int8_t>* d7Paint = nullptr;
    for (const IndicatorInstance& inst : m_overlays) {
      if (inst.reg == IndD7 && inst.seriesVisible && inst.flag &&
          inst.dir.size() == cs.v.size()) {
        d7Paint = &inst.dir;
        break;
      }
    }
    for (int i = ctx.vis0; i <= ctx.vis1; ++i) {
      const Candle& c = cs.v[(size_t)i];
      float x = ctx.xOf(i);
      bool up = c.c >= c.o;
      Color col = up ? t.green : t.red;
      if (d7Paint) {
        int8_t s = (*d7Paint)[(size_t)i];
        if (s > 0) col = t.green;
        else if (s < 0) col = t.red;
        else col = withAlpha(t.textDim, 0.82f);
      }
      drawOhlcCandle(u.draw, x, ctx.bw, ctx.price.yOf(c.o), ctx.price.yOf(c.c),
                     ctx.price.yOf(c.h), ctx.price.yOf(c.l), col, up, m_showWicks,
                     m_candleBody);
    }
  } else if (m_chartType == 1 || m_chartType == 2) {
    // Exact aggressor volume at price. Grouping is a display step: FINE tracks
    // the heatmap's dense rows; AUTO keeps labels readable when bid×ask text
    // is on. Volume is never dropped — only merged into the visible row.
    static constexpr float targetRowPx[] = {8.0f, 3.0f, 11.0f, 18.0f};
    float target = targetRowPx[std::clamp(m_footprintGrouping, 0, 3)];
    if (m_footprintGrouping == 0 && !m_showFootprintText) target = 4.0f;
    double rawStep = (ctx.price.hi - ctx.price.lo) /
                     std::max(1.0f, ctx.price.area.h / target);
    // Row-height clamp. On a linear axis one price step maps to a uniform
    // pixel height, but on a log axis rows compress toward the top of the
    // pane and can collapse into each other (visible as cells overlapping at
    // fine grouping). Enlarge the step so the tightest (top) row still keeps
    // the target height; linear axes are unaffected.
    if (ctx.price.log && ctx.price.lo > 0 && ctx.price.hi > 0) {
      double topStep = ctx.price.hi *
          (1.0 - std::exp(-target * std::log(ctx.price.hi / ctx.price.lo) /
                          ctx.price.area.h));
      rawStep = std::max(rawStep, topStep);
    }
    double groupStep = niceStep(rawStep);
    ensureFootprint(feeds, groupStep);
    const bool cluster = m_chartType == 1;
    const double imbalanceRatio = 3.0 + m_footprintImbalance;

    // Keep the OHLC candle in a dedicated gutter to the left of the volume
    // cells. It remains associated with its bar without covering bid/ask text,
    // heat, POC, or imbalance markers.
    auto footprintGeometry = [&](int bar, float& clusterX, float& clusterW,
                                 float& candleX, float& candleBodyW) {
      float slotX = ctx.xOf(bar);
      float pad = std::clamp(ctx.bw * 0.035f, 1.0f, 3.0f);
      float gutter = std::clamp(ctx.bw * 0.13f, 7.0f, 12.0f);
      gutter = std::min(gutter, std::max(3.0f, ctx.bw * 0.28f));
      clusterX = slotX + pad + gutter;
      clusterW = std::max(1.0f, ctx.bw - pad * 2.0f - gutter);
      candleBodyW = std::clamp(gutter * 0.48f, 3.0f, 5.0f);
      candleX = slotX + pad + std::max(candleBodyW * 0.5f, gutter * 0.38f);
    };

    // Build and cache the naked-POC lifecycle in one forward pass. Active POCs
    // are indexed by their exact grouped tick; a later candle terminates every
    // ray at that tick when its high/low trades through the level. Rebuilding
    // is event-driven, so rays cover all loaded history without a per-frame
    // scan over the retained prints.
    if (m_showFootprintPoc && ctx.bw >= 14.0f) {
      // Ray lifecycle is incremental: closed bars finalize exactly once into
      // m_footprintPocActive (watermark), and the last two bars stay
      // provisional — refreshed every frame without joining the active set.
      // Within one shape generation the series is append-only and only the
      // forming bar's cells mutate, so this matches the old rebuild-on-every-
      // print pass without re-walking all history per feed tick. Candle wicks
      // only extend while forming, so a provisional hit is final and applying
      // it early is safe; the two-bar trailing window also absorbs prints
      // that drain after their bar has already closed.
      bool pocStructural =
          m_footprintPocVersion != m_footprintVersion ||
          m_footprintPocShape != m_footprintShape ||
          m_footprintPocStep != groupStep ||
          (int)cs.v.size() < (int)m_footprintPocTicks.size();
      if (pocStructural) {
        m_footprintPocVersion = m_footprintVersion;
        m_footprintPocShape = m_footprintShape;
        m_footprintPocStep = groupStep;
        m_footprintPocTicks.assign(cs.v.size(),
                                   std::numeric_limits<int64_t>::min());
        m_footprintPocHits.assign(cs.v.size(), -1);
        m_footprintPocActive.clear();
        m_footprintPocActive.reserve((size_t)(ctx.vis1 + 8) * 2);
        m_footprintPocBuilt = 0;
      } else if ((int)cs.v.size() > (int)m_footprintPocTicks.size()) {
        m_footprintPocTicks.resize(cs.v.size(),
                                   std::numeric_limits<int64_t>::min());
        m_footprintPocHits.resize(cs.v.size(), -1);
      }

      auto pocScan = [&](int bar) -> int64_t {
        int a = bar < (int)m_footprintOffsets.size()
                    ? m_footprintOffsets[(size_t)bar]
                    : 0;
        int b = bar + 1 < (int)m_footprintOffsets.size()
                    ? m_footprintOffsets[(size_t)bar + 1]
                    : a;
        double maxVolume = 0;
        int64_t tick = std::numeric_limits<int64_t>::min();
        for (int ci = a; ci < b; ++ci) {
          const FootprintCell& cell = m_footprint[(size_t)ci];
          double volume = cell.buy + cell.sell;
          if (volume > maxVolume) {
            maxVolume = volume;
            tick = cell.tick;
          }
        }
        return tick;
      };
      auto pocHitCheck = [&](int bar) {
        const Candle& touch = cs.v[(size_t)bar];
        int64_t loTick = (int64_t)std::ceil(touch.l / groupStep - 1e-9);
        int64_t hiTick = (int64_t)std::floor(touch.h / groupStep + 1e-9);
        if (hiTick < loTick) return;
        // Normal candles cover only a handful of footprint rows. For a flash
        // wick, iterate the active set instead of a huge tick range.
        if (hiTick - loTick <= 512) {
          for (int64_t tick = loTick; tick <= hiTick; ++tick) {
            auto it = m_footprintPocActive.find(tick);
            if (it == m_footprintPocActive.end()) continue;
            for (int source : it->second)
              m_footprintPocHits[(size_t)source] = bar;
            m_footprintPocActive.erase(it);
          }
        } else {
          for (auto it = m_footprintPocActive.begin();
               it != m_footprintPocActive.end();) {
            if (it->first >= loTick && it->first <= hiTick) {
              for (int source : it->second)
                m_footprintPocHits[(size_t)source] = bar;
              it = m_footprintPocActive.erase(it);
            } else {
              ++it;
            }
          }
        }
      };

      const int nBars = (int)cs.v.size();
      int finalizeTarget = std::max(0, nBars - 2);
      for (int bar = m_footprintPocBuilt; bar < finalizeTarget; ++bar) {
        m_footprintPocTicks[(size_t)bar] = pocScan(bar);
        pocHitCheck(bar);
        int64_t pocTick = m_footprintPocTicks[(size_t)bar];
        if (pocTick != std::numeric_limits<int64_t>::min())
          m_footprintPocActive[pocTick].push_back(bar); // own candle can't hit it
      }
      m_footprintPocBuilt = finalizeTarget;

      // Provisional tail: refresh ticks and take hits, but never seed rays
      // for future bars until the bar closes into the watermark pass.
      for (int bar = std::max(0, nBars - 2); bar < nBars; ++bar) {
        m_footprintPocTicks[(size_t)bar] = pocScan(bar);
        pocHitCheck(bar);
      }

      for (int source = 0; source <= ctx.vis1; ++source) {
        int64_t tick = m_footprintPocTicks[(size_t)source];
        if (tick == std::numeric_limits<int64_t>::min()) continue;
        int hit = m_footprintPocHits[(size_t)source];
        if (source < ctx.vis0 && hit >= 0 && hit < ctx.vis0) continue;

        float clusterX, clusterW, candleX, candleBodyW;
        footprintGeometry(source, clusterX, clusterW, candleX, candleBodyW);
        float startX = clusterX + clusterW + 1.5f;
        float endX = ctx.price.area.x + ctx.price.area.w;
        if (hit >= 0) {
          float hitClusterX, hitClusterW, hitCandleX, hitCandleBodyW;
          footprintGeometry(hit, hitClusterX, hitClusterW, hitCandleX,
                            hitCandleBodyW);
          endX = hitCandleX;
        }
        startX = std::max(startX, ctx.price.area.x);
        endX = std::min(endX, ctx.price.area.x + ctx.price.area.w);
        float py = ctx.price.yOf((double)tick * groupStep);
        if (endX > startX + 0.5f && py >= ctx.price.area.y &&
            py <= ctx.price.area.y + ctx.price.area.h)
          u.draw.rect({startX, std::floor(py) + 0.5f, endX - startX, 1.0f},
                      withAlpha(t.accent, hit >= 0 ? 0.32f : 0.52f));
      }
    }

    for (int i = ctx.vis0; i <= ctx.vis1; ++i) {
      const Candle& candle = cs.v[(size_t)i];
      float innerX, innerW, cx, candleBodyW;
      footprintGeometry(i, innerX, innerW, cx, candleBodyW);
      Color side = candle.c >= candle.o ? t.green : t.red;

      int a = i < (int)m_footprintOffsets.size() ? m_footprintOffsets[(size_t)i] : 0;
      int b = i + 1 < (int)m_footprintOffsets.size()
                  ? m_footprintOffsets[(size_t)i + 1] : a;
      double maxCell = 0, maxBuy = 0, maxSell = 0;
      int pocCell = -1;
      for (int ci = a; ci < b; ++ci) {
        const FootprintCell& cell = m_footprint[(size_t)ci];
        double total = cell.buy + cell.sell;
        if (total > maxCell) { maxCell = total; pocCell = ci; }
        maxBuy = std::max(maxBuy, cell.buy);
        maxSell = std::max(maxSell, cell.sell);
      }
      if (!(maxCell > 0)) {
        if (m_showWicks)
          u.draw.rect({cx - 1.0f, ctx.price.yOf(candle.h), 2.0f,
                       std::max(1.0f, ctx.price.yOf(candle.l) - ctx.price.yOf(candle.h))},
                      withAlpha(side, 0.88f));
        float bodyTop = std::min(ctx.price.yOf(candle.o), ctx.price.yOf(candle.c));
        float bodyH = std::max(2.0f, std::fabs(ctx.price.yOf(candle.o) - ctx.price.yOf(candle.c)));
        u.draw.rect({cx - (candleBodyW + 1.0f) * 0.5f, bodyTop,
                     candleBodyW + 1.0f, bodyH},
                    withAlpha(t.chartPaneBg, 0.96f));
        u.draw.rect({cx - candleBodyW * 0.5f, bodyTop, candleBodyW, bodyH},
                    withAlpha(side, 0.98f));
        continue;
      }

      // Declutter: with every venue aggregated a single bar can hold prints at
      // dozens of distinct prices. Drop cells below a fraction of the bar's
      // busiest cell so low-volume tails don't smear the footprint.
      static constexpr double kMinCellPct[] = {0.0, 0.01, 0.02, 0.05, 0.10};
      double minCell = maxCell * kMinCellPct[std::clamp(m_footprintMinCell, 0, 4)];

      // Per-bar scratch reused across frames and bars — two fresh heap
      // allocations per visible bar per frame was pure churn (spans repeat
      // frame-to-frame; the running max only grows).
      static thread_local std::vector<uint8_t> imbalance, stacked;
      if ((size_t)(b - a) > imbalance.size()) imbalance.resize((size_t)(b - a), 0);
      std::fill_n(imbalance.begin(), b - a, 0);
      for (int ci = a; ci < b; ++ci) {
        const FootprintCell& cell = m_footprint[(size_t)ci];
        double lowerSell = 0, higherBuy = 0;
        if (ci > a && m_footprint[(size_t)ci - 1].tick == cell.tick - 1)
          lowerSell = m_footprint[(size_t)ci - 1].sell;
        if (ci + 1 < b && m_footprint[(size_t)ci + 1].tick == cell.tick + 1)
          higherBuy = m_footprint[(size_t)ci + 1].buy;
        if (cell.buy > 0 && lowerSell > 0 && cell.buy >= lowerSell * imbalanceRatio)
          imbalance[(size_t)(ci - a)] |= 2;
        if (cell.sell > 0 && higherBuy > 0 && cell.sell >= higherBuy * imbalanceRatio)
          imbalance[(size_t)(ci - a)] |= 1;
      }

      // Runs of three or more consecutive diagonal imbalances form a stacked
      // rail. Instead of drawing a bar hugging the footprint edge, mark the
      // member cells so the highlight stays on the cells themselves.
      if ((size_t)(b - a) > stacked.size()) stacked.resize((size_t)(b - a), 0);
      std::fill_n(stacked.begin(), b - a, 0);
      if (m_showFootprintStacked) {
        for (uint8_t sideBit : {uint8_t(1), uint8_t(2)}) {
          int run = -1;
          for (int j = 0; j <= b - a; ++j) {
            bool visible =
                j < b - a &&
                m_footprint[(size_t)a + j].buy + m_footprint[(size_t)a + j].sell >=
                    minCell;
            bool on = visible && (imbalance[(size_t)j] & sideBit) != 0;
            if (on && run < 0) run = j;
            if ((!on || j == b - a) && run >= 0) {
              if (j - run >= 3)
                for (int k = run; k < j; ++k) stacked[(size_t)k] |= sideBit;
              run = -1;
            }
          }
        }
      }

      for (int ci = a; ci < b; ++ci) {
        const FootprintCell& cell = m_footprint[(size_t)ci];
        double p = (double)cell.tick * groupStep;
        float top = ctx.price.yOf(p + groupStep * 0.5);
        float bottom = ctx.price.yOf(p - groupStep * 0.5);
        float rowY = std::min(top, bottom), rowH = std::max(1.0f, std::fabs(bottom - top));
        if (rowY + rowH < ctx.price.area.y || rowY > ctx.price.area.y + ctx.price.area.h) continue;
        float rowInset = rowH >= 2.0f ? std::min(0.65f, (rowH - 1.0f) * 0.5f)
                                       : 0.0f;
        Rect cellRect{innerX, rowY + rowInset, innerW,
                      std::max(1.0f, rowH - rowInset * 2.0f)};
        double total = cell.buy + cell.sell;
        if (total < minCell) continue;
        float intensity = (float)std::sqrt(total / maxCell);
        bool buyImb = (imbalance[(size_t)(ci - a)] & 2) != 0;
        bool sellImb = (imbalance[(size_t)(ci - a)] & 1) != 0;

        if (cluster) {
          Rect left{cellRect.x, cellRect.y, cellRect.w * 0.5f, cellRect.h};
          Rect right{innerX + innerW * 0.5f, left.y, innerW * 0.5f, left.h};
          float heatGain[] = {0.16f, 0.26f, 0.38f};
          float gain = heatGain[std::clamp(m_footprintHeatmap, 0, 2)];
          float sellHeat = maxSell > 0 ? (float)std::sqrt(cell.sell / maxSell) : 0;
          float buyHeat = maxBuy > 0 ? (float)std::sqrt(cell.buy / maxBuy) : 0;
          // Floor is occupancy: wick prints are tiny vs the bar POC, and
          // sqrt(vol/POC)*0.025 painted them at ~3% alpha — graphite ate them
          // while the kline wick still ran the full high/low.
          if (cell.sell > 0)
            u.draw.rectGradientHDithered(left, withAlpha(t.red, 0.14f + gain * sellHeat),
                                         withAlpha(t.red, 0.22f + gain * sellHeat));
          if (cell.buy > 0)
            u.draw.rectGradientHDithered(right, withAlpha(t.green, 0.22f + gain * buyHeat),
                                         withAlpha(t.green, 0.14f + gain * buyHeat));
          if (m_showFootprintStacked && (stacked[(size_t)(ci - a)] & 2))
            u.draw.rect(right, withAlpha(t.green, 0.18f));
          if (m_showFootprintStacked && (stacked[(size_t)(ci - a)] & 1))
            u.draw.rect(left, withAlpha(t.red, 0.18f));
          if (m_showFootprintImbalances && sellImb)
            u.draw.rect({left.x, left.y, 2, left.h}, withAlpha(t.red, 0.95f));
          if (m_showFootprintImbalances && buyImb)
            u.draw.rect({right.x + right.w - 2, right.y, 2, right.h},
                        withAlpha(t.green, 0.95f));
          if (m_showFootprintText && innerW >= 48.0f && cellRect.h >= 7.0f) {
            char sell[20], buy[20];
            formatFootprint(sell, sizeof(sell), cell.sell);
            formatFootprint(buy, sizeof(buy), cell.buy);
            u.draw.textFit(left, sell, sellImb ? t.red : t.text, DrawList::Right, 3);
            u.draw.textFit(right, buy, buyImb ? t.green : t.text, DrawList::Left, 3);
          }
        } else {
          if (m_showFootprintStacked && (stacked[(size_t)(ci - a)] & 2))
            u.draw.rect(cellRect, withAlpha(t.green, 0.16f));
          if (m_showFootprintStacked && (stacked[(size_t)(ci - a)] & 1))
            u.draw.rect(cellRect, withAlpha(t.red, 0.16f));
          float fillW = innerW * (float)(total / maxCell);
          double delta = total > 0 ? (cell.buy - cell.sell) / total : 0;
          Color col = delta >= 0 ? t.green : t.red;
          Rect bar{innerX, cellRect.y, std::max(1.0f, fillW), cellRect.h};
          u.draw.rectGradientHDithered(bar, withAlpha(col, 0.16f + 0.22f * intensity),
                                       withAlpha(col, 0.08f + 0.10f * intensity));
          if (m_showFootprintImbalances && (buyImb || sellImb))
            u.draw.rect({bar.x, bar.y, 2, bar.h},
                        withAlpha(buyImb ? t.green : t.red, 0.86f));
          if (m_showFootprintText && innerW >= 52.0f && cellRect.h >= 7.0f) {
            char vol[20];
            formatFootprint(vol, sizeof(vol), total);
            u.draw.textFit({innerX + 3, cellRect.y, innerW - 6, cellRect.h}, vol, t.text,
                           DrawList::Left);
          }
        }

        // POC is a contained row marker, not a full-tick overlay. Keeping its
        // two edges inside the cell prevents the highlight from bleeding into
        // adjacent price rows at fractional scaling.
        if (m_showFootprintPoc && ci == pocCell) {
          u.draw.rect(cellRect, withAlpha(t.accent, 0.065f));
          u.draw.rect({cellRect.x, cellRect.y, cellRect.w, 1.0f},
                      withAlpha(t.accent, 0.72f));
          if (cellRect.h >= 3.0f)
            u.draw.rect({cellRect.x, cellRect.y + cellRect.h - 1.0f,
                         cellRect.w, 1.0f},
                        withAlpha(t.accent, 0.46f));
        }
      }

      // The conventional candle lives in the left gutter, preserving OHLC
      // direction while keeping the footprint itself unobstructed.
      if (m_showWicks)
        u.draw.rect({cx - 1.0f, ctx.price.yOf(candle.h), 2.0f,
                     std::max(1.0f, ctx.price.yOf(candle.l) - ctx.price.yOf(candle.h))},
                    withAlpha(side, 0.88f));
      float bodyTop = std::min(ctx.price.yOf(candle.o), ctx.price.yOf(candle.c));
      float bodyH = std::max(2.0f, std::fabs(ctx.price.yOf(candle.o) - ctx.price.yOf(candle.c)));
      u.draw.rect({cx - (candleBodyW + 1.0f) * 0.5f, bodyTop,
                   candleBodyW + 1.0f, bodyH},
                  withAlpha(t.chartPaneBg, 0.96f));
      u.draw.rect({cx - candleBodyW * 0.5f, bodyTop, candleBodyW, bodyH},
                  withAlpha(side, 0.98f));
    }
  } else {
    // Classic crypto TPO: UTC-day profiles, one letter per 30/60-minute
    // bracket and one occurrence per price row. POC and 70% value area use
    // TPO counts, not volume.
    struct TpoRow { int64_t tick = 0; uint64_t letters = 0; };
    struct TpoSession {
      int64_t day = 0;
      int last = 0;
      double open = 0, close = 0;
      double ibHigh = -1e300, ibLow = 1e300;
      std::unordered_map<int64_t, uint64_t> rows;
    };
    double tpoStep = niceStep((ctx.price.hi - ctx.price.lo) /
                              std::max(1.0f, ctx.price.area.h / 11.0f));
    const double dayMs = 86400000.0;
    const double bracketMs = (m_tpoBracket == 0 ? 30.0 : 60.0) * 60000.0;
    // TPO is session-native rather than candle-native. Show the latest few UTC
    // sessions ending at the current horizontal viewport, allocating a stable
    // profile column to each instead of stretching letters over bar spacing.
    int maxSessions = tpoMaxSessions(ctx.price.area.w);
    int tpoFirst = tpoWindowFirst(cs, ctx.vis1, maxSessions);

    // Cached profile build: session construction, per-row letter maps and the
    // POC/value-area extraction previously ran every frame (thousands of hash
    // operations plus a sort per session) even though historical sessions are
    // immutable. tpoStep is lattice-quantized (niceStep) so it is stable
    // frame-to-frame; rebuild immediately on any window/step/bracket change,
    // otherwise refresh the forming bar's letters at most every 250 ms.
    struct TpoDraw {
      int sessionIndex = 0;
      std::vector<TpoRow> rows;
      int64_t poc = 0;
      int maxCount = 0, totalCount = 0;
      int pocIndex = 0, vaLo = 0, vaHi = 0;
    };
    static thread_local std::vector<TpoSession> tpoSessions;
    static thread_local std::vector<TpoDraw> tpoDraws;
    static thread_local uint64_t tpoSig = 0;
    static thread_local std::chrono::steady_clock::time_point tpoBuiltAt{};
    const auto nowTp = std::chrono::steady_clock::now();
    uint64_t tpSig = 1469598103934665603ull;
    auto mixTp = [&tpSig](uint64_t x) { tpSig ^= x; tpSig *= 1099511628211ull; };
    mixTp(bits_double(tpoStep));
    mixTp((uint64_t)m_tpoBracket);
    mixTp((uint64_t)maxSessions);
    mixTp((uint64_t)tpoFirst);
    mixTp((uint64_t)(ctx.vis1 + 1));
    mixTp((uint64_t)cs.v.size());
    if (!cs.v.empty()) {
      uint64_t u;
      std::memcpy(&u, &cs.v.front().ts, 8);
      mixTp(u);
    }
    const bool tpRebuild =
        tpSig != tpoSig || tpoDraws.empty() ||
        std::chrono::duration_cast<std::chrono::milliseconds>(nowTp - tpoBuiltAt)
                .count() > 250;

    if (tpRebuild) {
      tpoSig = tpSig;
      tpoBuiltAt = nowTp;
      tpoSessions.clear();
      for (int i = tpoFirst; i <= ctx.vis1; ++i) {
        const Candle& candle = cs.v[(size_t)i];
        int64_t day = utcDay(candle.ts);
        if (tpoSessions.empty() || tpoSessions.back().day != day) {
          TpoSession fresh;
          fresh.day = day;
          fresh.open = fresh.close = candle.o;
          tpoSessions.push_back(std::move(fresh));
        }
        TpoSession& session = tpoSessions.back();
        session.last = i;
        session.close = candle.c;
        int period = (int)std::floor((candle.ts - day * dayMs) / bracketMs);
        period = std::clamp(period, 0, 63);
        int ibPeriods = m_tpoBracket == 0 ? 2 : 1;
        if (period < ibPeriods) {
          session.ibHigh = std::max(session.ibHigh, candle.h);
          session.ibLow = std::min(session.ibLow, candle.l);
        }
        int64_t loTick = (int64_t)std::floor(candle.l / tpoStep);
        int64_t hiTick = (int64_t)std::ceil(candle.h / tpoStep);
        for (int64_t tick = loTick; tick <= hiTick && tick - loTick < 4096; ++tick)
          session.rows[tick] |= 1ull << period;
      }

      tpoDraws.clear();
      tpoDraws.resize(tpoSessions.size());
      for (size_t sessionIndex = 0; sessionIndex < tpoSessions.size(); ++sessionIndex) {
        TpoSession& session = tpoSessions[sessionIndex];
        TpoDraw& draw = tpoDraws[sessionIndex];
        draw.sessionIndex = (int)sessionIndex;
        draw.rows.reserve(session.rows.size());
        for (const auto& entry : session.rows) {
          int count = __builtin_popcountll(entry.second);
          draw.totalCount += count;
          if (count > draw.maxCount) { draw.maxCount = count; draw.poc = entry.first; }
          draw.rows.push_back({entry.first, entry.second});
        }
        std::sort(draw.rows.begin(), draw.rows.end(),
                  [](const TpoRow& a, const TpoRow& b) { return a.tick < b.tick; });
        for (int i = 0; i < (int)draw.rows.size(); ++i)
          if (draw.rows[(size_t)i].tick == draw.poc) { draw.pocIndex = i; break; }
        draw.vaLo = draw.vaHi = draw.pocIndex;
        int covered =
            draw.rows.empty()
                ? 0
                : __builtin_popcountll(draw.rows[(size_t)draw.pocIndex].letters);
        int targetCount = (int)std::ceil(draw.totalCount * 0.70);
        while (covered < targetCount &&
               (draw.vaLo > 0 || draw.vaHi + 1 < (int)draw.rows.size())) {
          int below = draw.vaLo > 0
                          ? __builtin_popcountll(draw.rows[(size_t)draw.vaLo - 1].letters)
                          : -1;
          int above = draw.vaHi + 1 < (int)draw.rows.size()
                          ? __builtin_popcountll(draw.rows[(size_t)draw.vaHi + 1].letters)
                          : -1;
          if (above >= below)
            covered += __builtin_popcountll(draw.rows[(size_t)++draw.vaHi].letters);
          else
            covered += __builtin_popcountll(draw.rows[(size_t)--draw.vaLo].letters);
        }
      }
    }

    for (size_t sessionIndex = 0; sessionIndex < tpoDraws.size(); ++sessionIndex) {
      TpoSession& session = tpoSessions[(size_t)tpoDraws[sessionIndex].sessionIndex];
      const std::vector<TpoRow>& rows = tpoDraws[sessionIndex].rows;
      const int64_t poc = tpoDraws[sessionIndex].poc;
      const int maxCount = tpoDraws[sessionIndex].maxCount;
      const int vaLo = tpoDraws[sessionIndex].vaLo;
      const int vaHi = tpoDraws[sessionIndex].vaHi;
      float slotW = ctx.price.area.w / std::max<size_t>(1, tpoDraws.size());
      float sx = ctx.price.area.x + sessionIndex * slotW;
      float available = std::max(2.0f, slotW - 8.0f);

      u.draw.rect({sx, ctx.price.area.y, 1, ctx.price.area.h}, withAlpha(t.border, 0.72f));
      if (!rows.empty() && available > 90.0f) {
        char dayLabel[16] = "UTC";
        time_t sessionTime = (time_t)(session.day * 86400);
        tm utcBuf{};
        if (gmtime_r(&sessionTime, &utcBuf))
          strftime(dayLabel, sizeof(dayLabel), "%m-%d", &utcBuf);
        char summary[112];
        snprintf(summary, sizeof(summary), "%s  %s  POC %.2f  VA %.2f-%.2f",
                 dayLabel, m_tpoBracket == 0 ? "30M" : "60M", poc * tpoStep,
                 rows[(size_t)vaLo].tick * tpoStep,
                 rows[(size_t)vaHi].tick * tpoStep);
        u.draw.textFit({sx + 5, ctx.price.area.y + 4, available - 8, 14}, summary,
                       t.textDim, DrawList::Left);
      }
      for (int ri = 0; ri < (int)rows.size(); ++ri) {
        const TpoRow& row = rows[(size_t)ri];
        int count = __builtin_popcountll(row.letters);
        double p = row.tick * tpoStep;
        float top = ctx.price.yOf(p + tpoStep * 0.5);
        float bottom = ctx.price.yOf(p - tpoStep * 0.5);
        float rowY = std::min(top, bottom), rowH = std::max(1.0f, std::fabs(bottom - top));
        bool inValue = ri >= vaLo && ri <= vaHi;
        bool isPoc = row.tick == poc;
        bool singlePrint = count == 1;
        float charWidth = 7.0f;
        float profileW = std::min(available, std::max(charWidth, count * charWidth));
        if (inValue)
          u.draw.rect({sx + 1, rowY, profileW + 3.0f, rowH},
                      withAlpha(t.accent, isPoc ? 0.18f : 0.045f));
        if (inValue)
          u.draw.rect({sx + 1, rowY, isPoc ? 2.0f : 1.0f, rowH},
                      withAlpha(t.accent, isPoc ? 0.95f : 0.52f));
        if (rowH >= 8.0f && count * charWidth <= available) {
          char letters[65];
          int n = 0;
          for (int bit = 0; bit < 64; ++bit)
            if (row.letters & (1ull << bit))
              letters[n++] = bit < 26 ? (char)('A' + bit)
                           : bit < 52 ? (char)('a' + bit - 26) : (char)('0' + (bit - 52) % 10);
          letters[n] = '\0';
          u.draw.textAligned({sx + 3, rowY, available - 4, rowH}, letters,
                             isPoc ? t.accent : singlePrint ? t.chartWarm : t.text,
                             DrawList::Left);
        } else if (maxCount > 0) {
          float fill = available * count / maxCount;
          u.draw.rect({sx + 2, rowY + 1, fill, std::max(1.0f, rowH - 2)},
                      withAlpha(isPoc ? t.accent : t.textDim, isPoc ? 0.72f : 0.28f));
        }
        if (isPoc)
          u.draw.rect({sx + 1, ctx.price.yOf(p), profileW + 3.0f, 1.0f},
                      withAlpha(t.accent, 0.92f));
      }

      // Initial-balance bracket and session open/close markers are kept at the
      // profile edge so they remain visible in both letters and compact modes.
      if (session.ibHigh > -1e200 && session.ibLow < 1e200) {
        float iy0 = ctx.price.yOf(session.ibHigh), iy1 = ctx.price.yOf(session.ibLow);
        float ix = sx + available - 2.0f;
        u.draw.rect({ix, iy0, 1.0f, std::max(1.0f, iy1 - iy0)},
                    withAlpha(t.chartWarm, 0.78f));
        u.draw.rect({ix - 4, iy0, 5, 1}, withAlpha(t.chartWarm, 0.78f));
        u.draw.rect({ix - 4, iy1, 5, 1}, withAlpha(t.chartWarm, 0.78f));
      }
      u.draw.rect({sx + 1, ctx.price.yOf(session.open), 6, 2}, withAlpha(t.text, 0.72f));
      Color closeColor = session.close >= session.open ? t.green : t.red;
      u.draw.rect({sx + available - 6, ctx.price.yOf(session.close), 6, 2},
                  withAlpha(closeColor, 0.90f));
    }
  }

  // Overlay series stay off TPO (that pane is letters, not a price overlay).
  // Legend chrome still draws so every instance can be removed or hidden.
  // Clicking the name (the same hit as a pane label) hides the plot.
  float legendY = ctx.price.area.y + 6;
  for (size_t ov = 0; ov < m_overlays.size(); ++ov) {
    IndicatorInstance& inst = m_overlays[ov];
    int i = inst.reg;
    const float seriesWidth =
        kChartLineWidths[std::clamp((int)inst.width, 0, 3)];
    Color c = inst.reg == IndHeat ? t.text : indPalette(inst.colorA);
    if (m_chartType != 3 && inst.seriesVisible) {
      if (paneKind(i)) {
        OscRange rng = kPresent[i].range(inst, ctx.vis0, ctx.vis1);
        if (rng.ok && i != IndVol) {
          // Affine-map the oscillator into the auto-fit candle range, then
          // project with the current (possibly zoomed) price axis so the line
          // pans and zooms with the candles instead of the pane box.
          ChartPane band = ctx.price;
          double fpad = (m_rngFitHi - m_rngFitLo) * 0.06 + 1e-9;
          band.remap = true;
          band.srcLo = rng.lo;
          band.srcHi = rng.hi;
          band.dstLo = m_rngFitLo - fpad;
          band.dstHi = m_rngFitHi + fpad;
          drawPaneBody(u.draw, band, cs, inst, ctx.vis0, ctx.vis1, ctx.startF,
                       ctx.bw, &feeds.market);
        }
      } else if (i == IndSt)
        drawSeriesDir(u.draw, ctx.price, inst.series, inst.dir, ctx.vis0,
                      ctx.vis1, ctx.startF, ctx.bw, indPalette(inst.colorA),
                      indPalette(inst.colorB), seriesWidth);
      else if (i == IndVwap)
        drawVwapLine(u.draw, ctx.price, cs, inst.series, ctx.vis0, ctx.vis1,
                     ctx.startF, ctx.bw, c, seriesWidth);
      else if (i == IndD7Lvls)
        d7DrawLevels(u.draw, ctx.price, inst.series, inst.aux, inst.aux2,
                     inst.p0 == 0 ? 7 : inst.p0, inst.flag,
                     indPalette(inst.colorA), indPalette(inst.colorB),
                     indPalette(inst.colorC));
      else if (i != IndHeat && i != IndD7)
        drawSeries(u.draw, ctx.price, inst.series, ctx.vis0, ctx.vis1,
                   ctx.startF, ctx.bw, c, seriesWidth);
    }
    char name[48];
    formatInstanceName(inst, name, sizeof(name));
    if ((i == IndSt || i == IndD7) && !inst.dir.empty()) {
      for (size_t k = inst.dir.size(); k-- > 0;) {
        if (inst.dir[k] != 0) {
          c = inst.dir[k] > 0 ? indPalette(inst.colorA) : indPalette(inst.colorB);
          break;
        }
      }
    }
    if (!inst.seriesVisible) c = withAlpha(c, 0.45f);
    float v = i == IndHeat ? NAN : lastValid(inst.series);
    char vb[24] = {};
    u.draw.textAligned({ctx.price.area.x + 6, legendY, ctx.price.area.w - 12, 14},
                       name, c, DrawList::Left, 0, true);
    float nw = u.draw.measure(name);
    float valueW = 0;
    if (m_showIndicatorLabels && !std::isnan(v)) {
      if (paneKind(i)) kPresent[i].fmt(inst)(vb, sizeof(vb), v);
      else chartFmtPrice(vb, sizeof(vb), v);
      valueW = u.draw.measure(vb);
      u.draw.textAligned(
          {ctx.price.area.x + 6 + nw + 10, legendY, ctx.price.area.w - nw - 46, 14}, vb,
          inst.seriesVisible ? t.text : withAlpha(t.text, 0.45f), DrawList::Left,
          0, true);
    }
    float textW = nw + (valueW > 0 ? valueW + 15.0f : 6.0f);
    Rect settingsIcon, removeIcon;
    legendActionRects(ctx.price.area.x + 6, textW, legendY - 1, 16,
                      ctx.price.area.x + ctx.price.area.w - 4.0f, settingsIcon,
                      removeIcon);
    char overlayId[32];
    snprintf(overlayId, sizeof(overlayId), "overlay-legend-%d", inst.id);
    u.pushId(overlayId);
    bool removed = drawLegendActions(u, inst.id, settingsIcon, removeIcon);
    Rect labelClick{ctx.price.area.x + 6, legendY,
                    std::max(1.0f, settingsIcon.x - (ctx.price.area.x + 6)), 16};
    Behavior lb = behavior(u, labelClick, u.id("label"));
    u.tip(u.id("labeltip"), labelClick, inst.seriesVisible ? "hide" : "show");
    if (lb.clicked) {
      inst.seriesVisible = !inst.seriesVisible;
      ++m_rngGen;
    }
    u.popId();
    if (removed) {
      u.draw.popClip();
      return true;
    }
    legendY += 16;
  }

  u.draw.popClip();
  return false;
}

// ---------------------------------------------------------------------------
// indicator panes, last-price annotations, crosshair
// ---------------------------------------------------------------------------

// indicator panes: content clipped to the recess, gutter labels outside it
bool ChartPanel::drawIndicatorPanes(Ui& u, Feeds& feeds, PlotCtx& ctx) {
  const CandleSeries& cs = feeds.candles;
  for (int p = 0; p < ctx.nPanes; ++p) {
    const IndicatorInstance& inst = m_panes[(size_t)p];
    int ri = inst.reg;
    if (ri < 0 || ri >= indicatorCount()) continue;
    ChartPane& pane = ctx.ind[p];

    if (ctx.rangeOk[p]) {
      u.draw.pushClip(pane.area);
      drawPaneBody(u.draw, pane, cs, inst, ctx.vis0, ctx.vis1, ctx.startF,
                   ctx.bw, &feeds.market);
      u.draw.popClip();
    }

    ChartFmt fmt = kPresent[ri].fmt(inst);
    float v = ctx.rangeOk[p] ? lastValid(inst.series) : NAN;
    char paneName[56];
    formatInstanceName(inst, paneName, sizeof(paneName));
    if (drawPaneOverlay(u, p, ctx.nPanes, ctx.indBg[p], paneName, v, fmt))
      return true; // pane vector mutated — indices are stale, redraw next frame
    if (ctx.rangeOk[p])
      drawPaneGutter(u.draw, ctx.gutter, pane,
                     (ri == IndRsi || ri == IndStoch || ri == IndAdx ||
                      ri == IndD7Rsi || ri == IndD7Score || ri == IndCipherB)
                         ? chartFmtInt
                         : fmt,
                     3);
  }
  return false;
}

// Independently configurable last-price line, gutter label, and forming-bar
// countdown/progress. Pattern geometry is emitted by DrawList so the same
// solid/dashed/dotted behavior is reusable by other chart annotations.
void ChartPanel::drawLastPriceRow(Ui& u, Feeds& feeds, PlotCtx& ctx) {
  const Theme& t = theme();
  const CandleSeries& cs = feeds.candles;
  if (!m_showLastPrice || !ctx.showTag) return;
  Color priceColor = t.accent;
  if (m_lastPriceColor == 1)
    priceColor = ctx.lastC >= cs.v.back().o ? t.green : t.red;
  else if (m_lastPriceColor == 2)
    priceColor = t.textDim;

  if (m_showLastPriceLine) {
    auto style = static_cast<DrawList::LineStyle>(
        std::clamp(m_lastPriceStyle, 0, 2));
    float width = kLastPriceLineWidths[std::clamp(m_lastPriceWidth, 0, 2)];
    u.draw.linePattern(ctx.price.area.x, ctx.ly, ctx.gutter.x + 2, ctx.ly,
                       withAlpha(priceColor, 0.86f), width, style);
  }
  if (m_showLastPriceLabel) {
    u.draw.breakCmd(); // tag quad must layer over every chart line
    char lp[24];
    snprintf(lp, sizeof(lp), "%.2f", ctx.lastC);
    Rect tag{ctx.gutter.x + 2, ctx.ly - 9, ctx.gutter.w - 4, 18};
    gutterTag(u.draw, tag, lp, priceColor);
  }

  if (m_showBarCountdown) {
    char cd[24];
    if (cs.tf.kind == Timeframe::Time) {
      time_t nowSec = time(nullptr);
      long intervalSec = (long)cs.tf.value * 60;
      long remain = intervalSec - ((long)nowSec % intervalSec);
      snprintf(cd, sizeof(cd), "%ld:%02ld", remain / 60, remain % 60);
    } else if (cs.tf.kind == Timeframe::Tick) {
      snprintf(cd, sizeof(cd), "%d/%d", (int)cs.barProgress(), (int)cs.tf.value);
    } else {
      char pv[16], tv[16];
      chartFmtVol(pv, sizeof(pv), cs.barProgress());
      chartFmtVol(tv, sizeof(tv), cs.tf.value);
      snprintf(cd, sizeof(cd), "%s/%s", pv, tv);
    }
    u.draw.textAligned({ctx.gutter.x + 2, ctx.ly + 11, ctx.gutter.w - 4, 14}, cd,
                       t.textDim, DrawList::Center);
  }
}

// crosshair: vertical spans the pane stack; horizontal + price tag stay on
// the price pane; time chip on the hovered bar
void ChartPanel::drawCrosshairRow(Ui& u, Feeds& feeds, PlotCtx& ctx) {
  const Theme& t = theme();
  const CandleSeries& cs = feeds.candles;
  if (!ctx.cross) return;
  float mx = u.input.mouseX, my = u.input.mouseY;
  Color xc = withAlpha(t.textDim, 0.5f);
  u.draw.line(ctx.price.area.x, my, ctx.price.area.x + ctx.price.area.w, my, xc, 1.0f);
  u.draw.line(mx, ctx.chart.y, mx, ctx.chart.y + ctx.stackH, xc, 1.0f);
  double pr = ctx.price.vOf(my);
  char pb[24];
  snprintf(pb, sizeof(pb), "%.2f", pr);
  // gutter tag + time chip (quads + shadows) must layer over the crosshair
  // lines — same fix as the live-price tag above
  u.draw.breakCmd();
  Rect tag{ctx.gutter.x + 2, my - 9, ctx.gutter.w - 4, 18};
  gutterTag(u.draw, tag, pb, t.text);

  int ci = (int)std::floor(ctx.startF + (mx - ctx.price.area.x) / ctx.bw);
  if (ci >= 0 && ci < ctx.size) {
    const char* tb = hhmmLabel((time_t)(cs.v[(size_t)ci].ts / 1000.0));
    float x = ctx.price.area.x + (ci - ctx.startF) * ctx.bw + ctx.bw * 0.5f;
    Rect chip{x - 26, ctx.timeAxis.y - 1, 52, ctx.timeAxis.h + 2};
    gutterTag(u.draw, chip, tb, t.text);
  }
}

// ---------------------------------------------------------------------------
// in-plot pane controls + indicator picker
// ---------------------------------------------------------------------------

// Row layout of the per-indicator settings popover, shared by
// openIndicatorSettings (popup height) and drawIndicatorSettings (rendering)
// so the two can never drift apart.
struct IndSettingsLayout {
  bool widthRow; // leading WIDTH chip row (absent on VOL)
  int colorRows; // palette swatch rows
  int optRows;   // option rows below the swatches (PLACE sits after them)
};

// Per-register popover geometry, in kRegistry order.
static constexpr IndSettingsLayout kIndLayouts[] = {
    {false, 2, 3}, // VOL
    {true, 2, 4},  // CVD
    {true, 1, 2},  // RSI
    {true, 2, 2},  // MACD
    {true, 1, 1},  // EMA
    {true, 1, 1},  // SMA
    {true, 2, 2},  // BB
    {false, 0, 0}, // BOOK HEAT (slider page, fixed height)
    {true, 2, 1},  // VWAP
    {true, 2, 2},  // ST
    {true, 1, 1},  // EMA 200
    {true, 2, 2},  // STOCH
    {true, 1, 1},  // ATR
    {true, 1, 0},  // OBV
    {true, 3, 2},  // ADX
    {true, 3, 2},  // D7
    {true, 3, 2},  // D7 RSI
    {true, 2, 2},  // D7 SCORE (second opt row reserved for pane PLACE)
    {true, 3, 2},  // D7 LVLS
    {true, 2, 1},  // OI
    {true, 2, 2},  // FUND
    {true, 2, 3},  // CIPHER B
};

// Palette rows per register, in kRegistry order; the first
// kIndLayouts[reg].colorRows entries are active.
struct IndColorRow {
  const char* label;
  uint8_t IndicatorInstance::*slot;
};

static constexpr IndColorRow kIndColors[][3] = {
    {{"UP", &IndicatorInstance::colorA}, {"DOWN", &IndicatorInstance::colorB}},                                       // VOL
    {{"UP", &IndicatorInstance::colorA}, {"DOWN", &IndicatorInstance::colorB}},                                       // CVD
    {{"LINE", &IndicatorInstance::colorA}},                                                                           // RSI
    {{"LINE", &IndicatorInstance::colorA}, {"SIGNAL", &IndicatorInstance::colorB}},                                   // MACD
    {{"LINE", &IndicatorInstance::colorA}},                                                                           // EMA
    {{"LINE", &IndicatorInstance::colorA}},                                                                           // SMA
    {{"LINE", &IndicatorInstance::colorA}, {"BAND", &IndicatorInstance::colorB}},                                     // BB
    {},                                                                                                               // BOOK HEAT
    {{"LINE", &IndicatorInstance::colorA}, {"BAND", &IndicatorInstance::colorB}},                                     // VWAP
    {{"UP", &IndicatorInstance::colorA}, {"DOWN", &IndicatorInstance::colorB}},                                       // ST
    {{"LINE", &IndicatorInstance::colorA}},                                                                           // EMA 200
    {{"%K", &IndicatorInstance::colorA}, {"%D", &IndicatorInstance::colorB}},                                         // STOCH
    {{"LINE", &IndicatorInstance::colorA}},                                                                           // ATR
    {{"LINE", &IndicatorInstance::colorA}},                                                                           // OBV
    {{"ADX", &IndicatorInstance::colorA}, {"+DI", &IndicatorInstance::colorB}, {"-DI", &IndicatorInstance::colorC}},  // ADX
    {{"UP", &IndicatorInstance::colorA}, {"DOWN", &IndicatorInstance::colorB}, {"MA", &IndicatorInstance::colorC}},   // D7
    {{"LINE", &IndicatorInstance::colorA}, {"BAND", &IndicatorInstance::colorB}, {"SLOW", &IndicatorInstance::colorC}}, // D7 RSI
    {{"UP", &IndicatorInstance::colorA}, {"DOWN", &IndicatorInstance::colorB}},                                       // D7 SCORE
    {{"DAY", &IndicatorInstance::colorA}, {"WEEK", &IndicatorInstance::colorB}, {"MONTH", &IndicatorInstance::colorC}}, // D7 LVLS
    {{"UP", &IndicatorInstance::colorA}, {"DOWN", &IndicatorInstance::colorB}},                                       // OI
    {{"UP", &IndicatorInstance::colorA}, {"DOWN", &IndicatorInstance::colorB}},                                       // FUND
    {{"WT1", &IndicatorInstance::colorA}, {"WT2", &IndicatorInstance::colorB}},                                       // CIPHER B
};

static_assert(sizeof(kIndLayouts) / sizeof(kIndLayouts[0]) == 22, "layout/reg drift");
static_assert(sizeof(kIndColors) / sizeof(kIndColors[0]) == 22, "colors/reg drift");

// Option-chip rows per register, in kRegistry order. Chips are declarative:
// Set assigns one member (or a three-member preset), Toggle flips a bool,
// Bits xor-tests a bitmask with a never-zero guard, Custom calls apply().
enum class ChipKind : uint8_t { Set, Toggle, Bits, Custom };

struct IndChip {
  const char* label;
  float width;
  ChipKind kind;
  int v0 = 0, v1 = 0, v2 = 0; // compare / assign values
  int IndicatorInstance::*i = nullptr;
  int IndicatorInstance::*i2 = nullptr; // preset members (Set only)
  int IndicatorInstance::*i3 = nullptr;
  bool IndicatorInstance::*b = nullptr; // Toggle target
  void (*apply)(IndicatorInstance&, const IndChip&) = nullptr; // Custom hook
};

struct IndOptRow {
  const char* title;
  const char* idScope; // widget-id scope (interlocked rows), null if none
  IndChip chips[6];
};

static void applyVolRvol(IndicatorInstance& inst, const IndChip&) {
  inst.p1 = VolRvol;
  if (inst.p0 < 2) inst.p0 = 20;
}

static void applyCvdMin(IndicatorInstance& inst, const IndChip& chip) {
  inst.p0 = chip.v0;
  int mx = cvdMaxSel(inst);
  if (mx > 0 && kCvdMinUsd[chip.v0] >= kCvdMaxUsd[mx]) inst.p1 = 0;
}

static void applyCvdMax(IndicatorInstance& inst, const IndChip& chip) {
  inst.p1 = chip.v0;
  int mn = cvdMinSel(inst);
  if (chip.v0 > 0 && kCvdMinUsd[mn] >= kCvdMaxUsd[chip.v0]) inst.p0 = 0;
}

static constexpr IndChip chipSet(const char* label, float width, int v,
                                 int IndicatorInstance::*i) {
  return {label, width, ChipKind::Set, v, 0, 0, i};
}

static constexpr IndChip chipToggle(const char* label, float width,
                                    bool IndicatorInstance::*b) {
  return {label, width, ChipKind::Toggle, 0, 0, 0, nullptr, nullptr, nullptr, b};
}

// Option rows per register, in kRegistry order. Row counts must stay within
// Option rows per register, in kRegistry order: kIndOpts[reg][row]. Row
// counts must stay within kIndLayouts[reg].optRows (asserted below); D7
// SCORE reserves its second row for pane PLACE alignment.
static constexpr IndOptRow kIndOpts[][4] = {
    {// VOL
     {"MODE", nullptr,
      {{"VOL", 44, ChipKind::Set, VolTotal, 0, 0, &IndicatorInstance::p1},
       {"DELTA", 56, ChipKind::Set, VolDelta, 0, 0, &IndicatorInstance::p1},
       {"RVOL", 50, ChipKind::Custom, VolRvol, 0, 0, &IndicatorInstance::p1,
        nullptr, nullptr, nullptr, applyVolRvol},
       {"SPLIT", 52, ChipKind::Set, VolSplit, 0, 0, &IndicatorInstance::p1}}},
     {"LOOKBACK", nullptr,
      {chipSet("10", 42, 10, &IndicatorInstance::p0),
       chipSet("20", 42, 20, &IndicatorInstance::p0),
       chipSet("50", 42, 50, &IndicatorInstance::p0),
       chipSet("100", 42, 100, &IndicatorInstance::p0)}},
     {"INTENSITY", nullptr,
      {chipSet("QUIET", 56, 0, &IndicatorInstance::opt),
       chipSet("NORMAL", 62, 1, &IndicatorInstance::opt),
       chipSet("STRONG", 62, 2, &IndicatorInstance::opt)}}},
    {// CVD
     {"STYLE", nullptr,
      {{"LINE", 48, ChipKind::Set, CvdLine, 0, 0, &IndicatorInstance::opt},
       {"CANDLES", 72, ChipKind::Set, CvdCandles, 0, 0,
        &IndicatorInstance::opt}}},
     {"MIN", "cvdmin",
      {{kCvdMinLbl[0], 30, ChipKind::Custom, 0, 0, 0, &IndicatorInstance::p0,
        nullptr, nullptr, nullptr, applyCvdMin},
       {kCvdMinLbl[1], 26, ChipKind::Custom, 1, 0, 0, &IndicatorInstance::p0,
        nullptr, nullptr, nullptr, applyCvdMin},
       {kCvdMinLbl[2], 32, ChipKind::Custom, 2, 0, 0, &IndicatorInstance::p0,
        nullptr, nullptr, nullptr, applyCvdMin},
       {kCvdMinLbl[3], 32, ChipKind::Custom, 3, 0, 0, &IndicatorInstance::p0,
        nullptr, nullptr, nullptr, applyCvdMin},
       {kCvdMinLbl[4], 36, ChipKind::Custom, 4, 0, 0, &IndicatorInstance::p0,
        nullptr, nullptr, nullptr, applyCvdMin},
       {kCvdMinLbl[5], 36, ChipKind::Custom, 5, 0, 0, &IndicatorInstance::p0,
        nullptr, nullptr, nullptr, applyCvdMin}}},
     {"MAX", "cvdmax",
      {{kCvdMaxLbl[0], 36, ChipKind::Custom, 0, 0, 0, &IndicatorInstance::p1,
        nullptr, nullptr, nullptr, applyCvdMax},
       {kCvdMaxLbl[1], 36, ChipKind::Custom, 1, 0, 0, &IndicatorInstance::p1,
        nullptr, nullptr, nullptr, applyCvdMax},
       {kCvdMaxLbl[2], 36, ChipKind::Custom, 2, 0, 0, &IndicatorInstance::p1,
        nullptr, nullptr, nullptr, applyCvdMax},
       {kCvdMaxLbl[3], 42, ChipKind::Custom, 3, 0, 0, &IndicatorInstance::p1,
        nullptr, nullptr, nullptr, applyCvdMax},
       {kCvdMaxLbl[4], 32, ChipKind::Custom, 4, 0, 0, &IndicatorInstance::p1,
        nullptr, nullptr, nullptr, applyCvdMax}}},
     {"GUIDE", nullptr,
      {chipToggle("ZERO LINE", 82, &IndicatorInstance::flag)}}},
    {// RSI
     {"PERIOD", nullptr,
      {chipSet("7", 42, 7, &IndicatorInstance::p0),
       chipSet("14", 42, 14, &IndicatorInstance::p0),
       chipSet("21", 42, 21, &IndicatorInstance::p0),
       chipSet("28", 42, 28, &IndicatorInstance::p0)}},
     {"GUIDES", nullptr,
      {chipToggle("30 / 70", 72, &IndicatorInstance::flag)}}},
    {// MACD
     {"PRESET", nullptr,
      {{"8/21/5", 58, ChipKind::Set, 8, 21, 5, &IndicatorInstance::p0,
        &IndicatorInstance::p1, &IndicatorInstance::p2},
       {"12/26/9", 66, ChipKind::Set, 12, 26, 9, &IndicatorInstance::p0,
        &IndicatorInstance::p1, &IndicatorInstance::p2},
       {"19/39/9", 66, ChipKind::Set, 19, 39, 9, &IndicatorInstance::p0,
        &IndicatorInstance::p1, &IndicatorInstance::p2}}},
     {"LAYER", nullptr,
      {chipToggle("HISTOGRAM", 86, &IndicatorInstance::flag)}}},
    {// EMA
     {"PERIOD", nullptr,
      {chipSet("9", 42, 9, &IndicatorInstance::p0),
       chipSet("21", 42, 21, &IndicatorInstance::p0),
       chipSet("50", 42, 50, &IndicatorInstance::p0),
       chipSet("100", 42, 100, &IndicatorInstance::p0)}}},
    {// SMA
     {"PERIOD", nullptr,
      {chipSet("20", 42, 20, &IndicatorInstance::p0),
       chipSet("50", 42, 50, &IndicatorInstance::p0),
       chipSet("100", 42, 100, &IndicatorInstance::p0),
       chipSet("200", 42, 200, &IndicatorInstance::p0)}}},
    {// BB
     {"PERIOD", nullptr,
      {chipSet("10", 42, 10, &IndicatorInstance::p0),
       chipSet("20", 42, 20, &IndicatorInstance::p0),
       chipSet("50", 42, 50, &IndicatorInstance::p0),
       chipSet("100", 42, 100, &IndicatorInstance::p0)}},
     {"DEVIATION", nullptr,
      {chipSet("1.5", 42, 0, &IndicatorInstance::opt),
       chipSet("2", 42, 1, &IndicatorInstance::opt),
       chipSet("2.5", 42, 2, &IndicatorInstance::opt),
       chipSet("3", 42, 3, &IndicatorInstance::opt)}}},
    {}, // BOOK HEAT (slider page)
    {// VWAP
      {"SIGMA", nullptr,
       {{"1\xcf\x83", 44, ChipKind::Bits, 1, 0, 0, &IndicatorInstance::opt},
        {"2\xcf\x83", 44, ChipKind::Bits, 2, 0, 0, &IndicatorInstance::opt},
        {"3\xcf\x83", 44, ChipKind::Bits, 4, 0, 0, &IndicatorInstance::opt}}}},
    {// ST
     {"PERIOD", nullptr,
      {chipSet("7", 42, 7, &IndicatorInstance::p0),
       chipSet("10", 42, 10, &IndicatorInstance::p0),
       chipSet("14", 42, 14, &IndicatorInstance::p0),
       chipSet("21", 42, 21, &IndicatorInstance::p0)}},
     {"MULT", nullptr,
      {chipSet("1.5", 42, 0, &IndicatorInstance::opt),
       chipSet("2", 42, 1, &IndicatorInstance::opt),
       chipSet("3", 42, 2, &IndicatorInstance::opt),
       chipSet("4", 42, 3, &IndicatorInstance::opt)}}},
    {// EMA 200
     {"PERIOD", nullptr,
      {chipSet("50", 42, 50, &IndicatorInstance::p0),
       chipSet("100", 42, 100, &IndicatorInstance::p0),
       chipSet("200", 42, 200, &IndicatorInstance::p0),
       chipSet("400", 42, 400, &IndicatorInstance::p0)}}},
    {// STOCH
     {"PRESET", nullptr,
      {{"5/3/3", 58, ChipKind::Set, 5, 3, 3, &IndicatorInstance::p0,
        &IndicatorInstance::p1, &IndicatorInstance::p2},
       {"14/3/3", 62, ChipKind::Set, 14, 3, 3, &IndicatorInstance::p0,
        &IndicatorInstance::p1, &IndicatorInstance::p2},
       {"21/7/7", 62, ChipKind::Set, 21, 7, 7, &IndicatorInstance::p0,
        &IndicatorInstance::p1, &IndicatorInstance::p2}}},
     {"GUIDES", nullptr,
      {chipToggle("20 / 80", 72, &IndicatorInstance::flag)}}},
    {// ATR
     {"PERIOD", nullptr,
      {chipSet("7", 42, 7, &IndicatorInstance::p0),
       chipSet("14", 42, 14, &IndicatorInstance::p0),
       chipSet("21", 42, 21, &IndicatorInstance::p0),
       chipSet("28", 42, 28, &IndicatorInstance::p0)}}},
    {}, // OBV (no option rows)
    {// ADX
     {"PERIOD", nullptr,
      {chipSet("7", 42, 7, &IndicatorInstance::p0),
       chipSet("14", 42, 14, &IndicatorInstance::p0),
       chipSet("21", 42, 21, &IndicatorInstance::p0),
       chipSet("28", 42, 28, &IndicatorInstance::p0)}},
     {"DI", nullptr,
      {chipToggle("+DI / \xe2\x88\x92" "DI", 86, &IndicatorInstance::flag)}}},
    {// D7
     {"BASE", nullptr,
      {chipSet("14", 42, 14, &IndicatorInstance::p0),
       chipSet("28", 42, 28, &IndicatorInstance::p0),
       chipSet("56", 42, 56, &IndicatorInstance::p0)}},
     {"PAINT", nullptr,
      {chipToggle("CANDLES", 78, &IndicatorInstance::flag)}}},
    {// D7 RSI
     {"PERIOD", nullptr,
      {chipSet("7", 42, 7, &IndicatorInstance::p0),
       chipSet("14", 42, 14, &IndicatorInstance::p0),
       chipSet("21", 42, 21, &IndicatorInstance::p0),
       chipSet("28", 42, 28, &IndicatorInstance::p0)}},
     {"GUIDES", nullptr,
      {chipToggle("30 / 70", 72, &IndicatorInstance::flag)}}},
    {// D7 SCORE (second opt row reserved for pane PLACE)
     {"BASE", nullptr,
      {chipSet("14", 42, 14, &IndicatorInstance::p0),
       chipSet("28", 42, 28, &IndicatorInstance::p0),
       chipSet("56", 42, 56, &IndicatorInstance::p0)}}},
    {// D7 LVLS
     {"OPENS", nullptr,
      {{"DAY", 48, ChipKind::Bits, 1, 0, 0, &IndicatorInstance::p0},
       {"WEEK", 52, ChipKind::Bits, 2, 0, 0, &IndicatorInstance::p0},
       {"MONTH", 56, ChipKind::Bits, 4, 0, 0, &IndicatorInstance::p0}}},
     {"PRIOR", nullptr,
      {chipToggle("PREV", 52, &IndicatorInstance::flag)}}},
    {// OI
     {"STYLE", nullptr,
      {{"LINE", 48, ChipKind::Set, CvdLine, 0, 0, &IndicatorInstance::opt},
       {"CANDLES", 72, ChipKind::Set, CvdCandles, 0, 0,
        &IndicatorInstance::opt}}}},
    {// FUND
     {"STYLE", nullptr,
      {{"LINE", 48, ChipKind::Set, CvdLine, 0, 0, &IndicatorInstance::opt},
       {"CANDLES", 72, ChipKind::Set, CvdCandles, 0, 0,
        &IndicatorInstance::opt}}},
     {"GUIDE", nullptr,
      {chipToggle("ZERO LINE", 82, &IndicatorInstance::flag)}}},
    {// CIPHER B
     {"PRESET", nullptr,
      {{"10/21/4", 66, ChipKind::Set, 10, 21, 4, &IndicatorInstance::p0,
        &IndicatorInstance::p1, &IndicatorInstance::p2},
       {"9/12/3", 62, ChipKind::Set, 9, 12, 3, &IndicatorInstance::p0,
        &IndicatorInstance::p1, &IndicatorInstance::p2}}},
     {"GUIDES", nullptr,
      {chipToggle("\xc2\xb1" "60", 52, &IndicatorInstance::flag)}},
     {"LAYER", nullptr,
      {{"MFI", 42, ChipKind::Bits, CipherLayerMfi, 0, 0, &IndicatorInstance::opt},
       {"HIST", 50, ChipKind::Bits, CipherLayerHist, 0, 0, &IndicatorInstance::opt},
       {"DOTS", 50, ChipKind::Bits, CipherLayerDots, 0, 0, &IndicatorInstance::opt}}}},
};

static_assert(sizeof(kIndOpts) / sizeof(kIndOpts[0]) == 22, "opts/reg drift");

constexpr bool indOptRowsFit() {
  for (int reg = 0; reg < 22; ++reg) {
    int n = 0;
    while (n < 4 && kIndOpts[reg][n].title) ++n;
    if (n > kIndLayouts[reg].optRows) return false;
  }
  return true;
}
static_assert(indOptRowsFit(), "option rows exceed reserved popover geometry");


void ChartPanel::openIndicatorSettings(Ui& u, int instId, Rect anchor) {
  IndicatorInstance* inst = findInstance(instId);
  if (!inst) return;
  if (m_indicatorSettingsId && u.overlayOpen(m_indicatorSettingsId)) {
    if (m_indicatorSettingsInst == instId) {
      u.closeOverlay(m_indicatorSettingsId);
      return;
    }
    u.closeOverlay(m_indicatorSettingsId);
  }
  m_indicatorSettingsInst = instId;
  m_indicatorSettingsId = u.id("##indicator-local-settings");
  int reg = inst->reg;
  float height = 200.0f; // BOOK HEAT slider page
  if (reg != IndHeat) {
    const IndSettingsLayout& lay = kIndLayouts[reg];
    height = 42.0f + 26.0f * ((float)lay.colorRows + (float)lay.optRows +
                              (paneKind(reg) ? 1.0f : 0.0f) +
                              (lay.widthRow ? 1.0f : 0.0f));
  }
  float width = 300.0f;
  float y = anchor.y > 520.0f ? anchor.y - height - 4.0f
                              : anchor.y + anchor.h + 4.0f;
  m_indicatorSettingsRect = {anchor.x, y, width, height};
  u.openOverlay(m_indicatorSettingsId, m_indicatorSettingsRect);
  u.input.pressed = false;
}

void ChartPanel::drawIndicatorSettings(Ui& u) {
  IndicatorInstance* inst = findInstance(m_indicatorSettingsInst);
  if (!m_indicatorSettingsId || !u.overlayOpen(m_indicatorSettingsId) || !inst)
    return;
  const Theme& t = theme();
  int reg = inst->reg;
  Rect r = m_indicatorSettingsRect;
  u.draw.shadow(r, t.radius, 14.0f, 3.0f, hexColor(0x000000, 0.45f));
  u.draw.rect(r, t.panel, t.radius);
  u.draw.rectOutline(r, t.border, 1.0f, t.radius);
  char title[56];
  formatInstanceName(*inst, title, sizeof(title));
  u.draw.setFont(FontMonoSemibold);
  u.draw.textAligned({r.x + 10, r.y + 5, r.w - 20, 18}, title, t.text,
                     DrawList::Left);
  u.draw.setFont(FontMono);

  auto optionRow = [&](float y, const char* title, auto options) {
    u.draw.textAligned({r.x + 10, y, 62, 20}, title, t.textDim, DrawList::Left);
    float x = r.x + 76;
    options(x, y);
  };
  auto option = [&](float& x, float y, const char* label, float width, bool active,
                    auto apply) {
    if (chip(u, {x, y, width, 20}, label, active)) {
      apply();
      noteSetChanged();
      saveSettings();
    }
    x += width + 4;
  };
  auto colorRow = [&](float y, const char* title, uint8_t& slot) {
    u.draw.textAligned({r.x + 10, y, 62, 20}, title, t.textDim, DrawList::Left);
    float x = r.x + 76;
    for (int i = 0; i < kIndPaletteN; ++i) {
      Rect sw{x, y + 2, 18, 16};
      Behavior b = behavior(u, sw, u.id("col") + (uint64_t)i * 17 + (uint64_t)y);
      u.draw.rect(sw, indPalette(i), 1.0f);
      if (slot == (uint8_t)i)
        u.draw.rectOutline(sw, t.text, 1.0f, 1.0f);
      else if (b.hovered)
        u.draw.rectOutline(sw, t.border, 1.0f, 1.0f);
      if (b.clicked) {
        slot = (uint8_t)i;
        saveSettings();
      }
      x += 22;
    }
  };

  float y = r.y + 28;
  if (reg != IndHeat) {
    if (reg != IndVol) {
      optionRow(y, "WIDTH", [&](float& x, float rowY) {
        static constexpr const char* labels[] = {"1", "1.5", "2", "2.5"};
        for (int i = 0; i < 4; ++i)
          option(x, rowY, labels[i], 42, inst->width == i,
                 [&, i] { inst->width = (uint8_t)i; });
      });
      y += 26;
    }
    const IndColorRow* colors = kIndColors[reg];
    for (int ci = 0, colorCount = kIndLayouts[reg].colorRows; ci < colorCount;
         ++ci) {
      colorRow(y, colors[ci].label, inst->*(colors[ci].slot));
      y += 26;
    }
  }
  const float optTop = y;

  if (reg == IndHeat) {
    // Slider rows: label left, live value right, track between.
    auto sliderRow = [&](float y, const char* title, const char* id,
                         float& value, float lo, float hi, const char* text) {
      u.draw.textAligned({r.x + 10, y, 86, 20}, title, t.textDim,
                         DrawList::Left);
      u.draw.textAligned({r.x + r.w - 56, y, 46, 20}, text, t.text,
                         DrawList::Right);
      return slider(u, {r.x + 98, y + 2, r.w - 160, 16}, id, value, lo, hi);
    };
    char vbuf[16];
    snprintf(vbuf, sizeof(vbuf), "%dpx", heatPxPerRow(m_heatResSel));
    u.draw.textAligned({r.x + 10, r.y + 28, 86, 20}, "RESOLUTION", t.textDim,
                       DrawList::Left);
    u.draw.textAligned({r.x + r.w - 56, r.y + 28, 46, 20}, vbuf, t.text,
                       DrawList::Right);
    float res = (float)m_heatResSel;
    if (slider(u, {r.x + 98, r.y + 30, r.w - 160, 16}, "##heat-res", res,
               0.0f, 100.0f)) {
      int step = (int)std::lround(res * 3.0f / 100.0f); // snap to grid steps
      m_heatResSel = (int)std::lround(step * 100.0f / 3.0f);
      saveSettings();
    }
    if (m_heatBinSel > 0)
      snprintf(vbuf, sizeof(vbuf), "$%g", heatBinUsdForSel(m_heatBinSel));
    else
      snprintf(vbuf, sizeof(vbuf), "AUTO");
    // Commit into the member before saveSettings() serializes state, or the
    // persisted CSV lags one drag behind (see RESOLUTION above).
    float binSel = (float)m_heatBinSel;
    if (sliderRow(r.y + 54, "BIN", "##heat-bin", binSel, 0.0f, 100.0f, vbuf)) {
      m_heatBinSel = (int)std::lround(binSel);
      saveSettings();
    }
    float intensity = m_heatIntensity;
    snprintf(vbuf, sizeof(vbuf), "%.2f", m_heatIntensity);
    if (sliderRow(r.y + 80, "INTENSITY", "##heat-intensity", intensity, 0.20f,
                  1.25f, vbuf)) {
      m_heatIntensity = intensity;
      saveSettings();
    }
    float opacity = m_heatOpacity;
    snprintf(vbuf, sizeof(vbuf), "%d%%", (int)std::lround(m_heatOpacity * 100));
    if (sliderRow(r.y + 106, "OPACITY", "##heat-opacity", opacity, 0.10f, 1.0f,
                  vbuf)) {
      m_heatOpacity = opacity;
      saveSettings();
    }
    double minUsd = heatMinUsdForSel(m_heatMinSel);
    if (minUsd > 0) chartFmtUsd(vbuf, sizeof(vbuf), minUsd);
    else snprintf(vbuf, sizeof(vbuf), "OFF");
    float minSel = (float)m_heatMinSel;
    if (sliderRow(r.y + 132, "MIN $", "##heat-min", minSel, 0.0f, 100.0f, vbuf)) {
      m_heatMinSel = (int)std::lround(minSel);
      saveSettings();
    }
    double maxUsd = heatMaxUsdForSel(m_heatMaxSel);
    if (maxUsd > 0) chartFmtUsd(vbuf, sizeof(vbuf), maxUsd);
    else snprintf(vbuf, sizeof(vbuf), "AUTO");
    float maxSel = (float)m_heatMaxSel;
    if (sliderRow(r.y + 158, "MAX $", "##heat-max", maxSel, 0.0f, 100.0f, vbuf)) {
      m_heatMaxSel = (int)std::lround(maxSel);
      saveSettings();
    }

  } else {
    // Declarative option rows: geometry from kIndLayouts, content from
    // kIndOpts. The bespoke per-register chains this replaces could drift
    // from the popup height; both now read the same tables.
    const IndOptRow* rows = kIndOpts[reg];
    for (int r = 0; r < kIndLayouts[reg].optRows && rows[r].title; ++r) {
      if (rows[r].idScope) u.pushId(rows[r].idScope);
      optionRow(y, rows[r].title, [&](float& x, float rowY) {
        for (int ci = 0; ci < 6; ++ci) {
          const IndChip& c = rows[r].chips[ci];
          if (!c.label) break;
          bool on = false;
          switch (c.kind) {
            case ChipKind::Set:
              on = inst->*(c.i) == c.v0 &&
                   (!c.i2 || (inst->*(c.i2) == c.v1 && inst->*(c.i3) == c.v2));
              break;
            case ChipKind::Toggle: on = inst->*(c.b); break;
            case ChipKind::Bits: on = (inst->*(c.i) & c.v0) != 0; break;
            case ChipKind::Custom: on = inst->*(c.i) == c.v0; break;
          }
          if (chip(u, {x, rowY, c.width, 20}, c.label, on)) {
            switch (c.kind) {
              case ChipKind::Set:
                inst->*(c.i) = c.v0;
                if (c.i2) inst->*(c.i2) = c.v1;
                if (c.i3) inst->*(c.i3) = c.v2;
                break;
              case ChipKind::Toggle:
                inst->*(c.b) = !(inst->*(c.b));
                break;
              case ChipKind::Bits: {
                int& v = inst->*(c.i);
                v ^= c.v0;
                if (v == 0) v = c.v0; // at least one open stays selected
                break;
              }
              case ChipKind::Custom: c.apply(*inst, c); break;
            }
            noteSetChanged();
            saveSettings();
          }
          x += c.width + 4;
        }
      });
      if (rows[r].idScope) u.popId();
      y += 26;
    }
  }
  if (paneKind(reg)) {
    float placeY = optTop + 26.0f * (float)kIndLayouts[reg].optRows;
    bool onChart = false;
    for (const IndicatorInstance& o : m_overlays)
      if (o.id == inst->id) {
        onChart = true;
        break;
      }
    int keepId = inst->id;
    // setInstanceOnChart moves the instance between containers, invalidating
    // inst — skip further rows for this frame after a move; state redraws next
    // frame from the new container.
    bool moved = false;
    optionRow(placeY, "PLACE", [&](float& x, float rowY) {
      option(x, rowY, "PANE", 52, !onChart,
             [&, keepId] { setInstanceOnChart(keepId, false); moved = true; });
      if (moved) return;
      option(x, rowY, "CHART", 58, onChart,
             [&, keepId] { setInstanceOnChart(keepId, true); moved = true; });
    });
  }
}

bool ChartPanel::drawLegendActions(Ui& u, int instId, Rect settings, Rect remove) {
  const Theme& t = theme();
  Behavior sb = behavior(u, settings, u.id("settings"));
  if (sb.hovered) u.draw.rect(settings, t.bgHover, 1.0f);
  drawSettingsGlyph(u.draw, settings, sb.hovered ? t.text : t.textDim);
  if (!(m_indicatorSettingsId && m_indicatorSettingsInst == instId &&
        u.overlayOpen(m_indicatorSettingsId)))
    u.tip(u.id("settingstip"), settings, "indicator settings");
  if (sb.clicked) openIndicatorSettings(u, instId, settings);

  Behavior rb = behavior(u, remove, u.id("remove"));
  if (rb.hovered) u.draw.rect(remove, t.bgHover, 1.0f);
  u.draw.textAligned(remove, "\xc3\x97", rb.hovered ? t.red : t.textDim,
                     DrawList::Center);
  u.tip(u.id("removetip"), remove, "remove");
  if (rb.clicked) {
    removeIndicator(u, instId);
    return true;
  }
  return false;
}

// Compact legend over the plot: name + latest value + settings/remove on the
// left. Hover reveals move controls on the right. No reserved strip.
bool ChartPanel::drawPaneOverlay(Ui& u, int p, int nPanes, Rect pane,
                                 const char* name, float value, ChartFmt fmt) {
  const Theme& t = theme();
  bool hov = u.hovered(pane);

  char idbuf[24];
  snprintf(idbuf, sizeof(idbuf), "pane%d", p);
  u.pushId(idbuf);

  const float btnW = 18.0f;
  Rect labelR{pane.x + 6, pane.y + 2, pane.w - 12 - 2 * btnW, 17.0f};
  u.draw.setFont(FontMonoSemibold);
  float nw = u.draw.measure(name);
  u.draw.setFont(FontMono);
  char vb[24] = {};
  float valueW = 0;
  if (!std::isnan(value)) {
    fmt(vb, sizeof(vb), value);
    valueW = u.draw.measure(vb);
  }
  float textW = nw + (valueW > 0 ? valueW + 14.0f : 5.0f);
  Rect localSettings, localRemove;
  legendActionRects(labelR.x, textW, labelR.y, labelR.h, labelR.x + labelR.w,
                    localSettings, localRemove);

  if ((m_showIndicatorLabels && m_panes[(size_t)p].labelVisible) || hov) {
    Color nameC = m_showIndicatorLabels && m_panes[(size_t)p].labelVisible
                      ? t.textDim
                      : withAlpha(t.textDim, 0.6f);
    u.draw.setFont(FontMonoSemibold);
    u.draw.textAligned(labelR, name, nameC, DrawList::Left);
    u.draw.setFont(FontMono);
    if (!std::isnan(value)) {
      u.draw.textAligned({labelR.x + nw + 10, labelR.y, labelR.w - nw - 10, labelR.h},
                         vb, t.text, DrawList::Left);
    }
    if (drawLegendActions(u, m_panes[(size_t)p].id, localSettings, localRemove)) {
      u.popId();
      return true;
    }
  }

  Rect labelClick{labelR.x, labelR.y, std::max(1.0f, localSettings.x - labelR.x),
                  labelR.h};
  Behavior lb = behavior(u, labelClick, u.id("label"));
  u.tip(u.id("labeltip"), labelClick,
        m_panes[(size_t)p].labelVisible ? "hide label" : "show label");
  if (lb.clicked) {
    m_panes[(size_t)p].labelVisible = !m_panes[(size_t)p].labelVisible;
    u.popId();
    return false;
  }

  if (hov) {
    struct Ctl {
      const char* glyph;
      const char* id;
      const char* tip;
      bool enabled;
    };
    const Ctl ctls[] = {
        {"\xe2\x86\x91", "up", "move up", p > 0},              // ↑
        {"\xe2\x86\x93", "down", "move down", p < nPanes - 1}, // ↓
    };
    float bx = pane.x + pane.w - 2 * btnW - 4.0f;
    for (int c = 0; c < 2; ++c) {
      Rect btn{bx, pane.y + 2.0f, btnW, 17.0f};
      Behavior b = ctls[c].enabled ? behavior(u, btn, u.id(ctls[c].id)) : Behavior{};
      if (ctls[c].enabled) u.tip(u.id(ctls[c].id), btn, ctls[c].tip);
      if (b.hovered) u.draw.rect(btn, t.bgHover, 1.0f);
      Color gc = !ctls[c].enabled ? withAlpha(t.textDim, 0.35f)
                 : b.hovered      ? t.text
                                  : t.textDim;
      u.draw.textAligned(btn, ctls[c].glyph, gc, DrawList::Center);
      if (b.clicked) {
        if (c == 0) std::swap(m_panes[(size_t)p], m_panes[(size_t)p - 1]);
        else std::swap(m_panes[(size_t)p], m_panes[(size_t)p + 1]);
        noteSetChanged();
        u.popId();
        return true;
      }
      bx += btnW;
    }
  }

  u.popId();
  return false;
}

// Indicator catalog: add-only. Checkmarks mean at least one instance exists;
// click still adds another (BOOK HEAT is the only singleton).
void ChartPanel::drawIndicatorPicker(Ui& u) {
  if (!u.overlayOpen(m_pickerId)) return;
  const Theme& t = theme();
  const int regN = indicatorCount();
  const float rowH = 24.0f;
  Rect r = m_pickerRect;
  u.updateOverlayRect(m_pickerId, r);
  u.draw.shadow(r, t.radius, 14.0f, 3.0f, hexColor(0x000000, 0.45f));
  u.draw.rect(r, t.panel, t.radius);
  u.draw.rectOutline(r, t.border, 1.0f, t.radius);

  Rect list{r.x + 4.0f, r.y + 4.0f, r.w - 8.0f, r.h - 8.0f};
  bool added = false;
  listView(u, list, regN, rowH, m_pickerList,
           [&](Ui& rowUi, DrawList& d, Rect row, int i) {
             if (added) return;
             const Indicator& def = kRegistry[i];
             const int existing = instanceCount(i);
             const bool on = existing > 0;
             const bool locked = i == IndHeat && on;
             bool hov = rowUi.hovered(row);
             if (hov) {
               rowUi.hot = m_pickerId + (uint64_t)i;
               if (!locked) d.rect(row, t.bgHover, 3.0f);
             }
             if (on)
               d.textAligned({row.x + 4, row.y, 14, row.h}, "\xe2\x9c\x93",
                             t.accent, DrawList::Left);
             char name[56];
             int p0 = 0, p1 = 0, p2 = 0, opt = 0;
             switch (i) {
               case IndEma: p0 = m_emaPeriod; break;
               case IndEma2: p0 = m_ema2Period; break;
               case IndSma: p0 = m_smaPeriod; break;
               case IndRsi: p0 = m_rsiPeriod; break;
               case IndMacd: p0 = m_macdFast; p1 = m_macdSlow; p2 = m_macdSignalPeriod; break;
               case IndBoll: p0 = m_bollPeriod; opt = m_bollDeviation; break;
               case IndSt: p0 = m_stPeriod; opt = m_stMultSel; break;
               case IndStoch: p0 = m_stochPeriod; p1 = m_stochSmooth; p2 = m_stochDPeriod; break;
               case IndAtr: p0 = m_atrPeriod; break;
               case IndAdx: p0 = m_adxPeriod; break;
               default: break;
             }
             indicatorName(i, p0, p1, p2, opt, name, sizeof(name));
             d.textAligned(row, name, locked ? t.textDim : t.text, DrawList::Left,
                           22);
             d.textAligned(row, def.overlay ? "overlay" : "pane", t.textDim,
                           DrawList::Right, 8);
             if (hov && !locked && rowUi.input.released) {
               addIndicator(i);
               added = true;
               u.closeOverlay(m_pickerId);
               rowUi.input.released = false;
             }
           });
}

// ---------------------------------------------------------------------------
// custom-timeframe popup
// ---------------------------------------------------------------------------

// Small popover with a text field: "45m", "2h", "3d", "100t", "500v". Enter
// applies; invalid input turns the hint red and keeps the popup open. Opened
// from the "+" chip in the timeframe row; the host draws this during the
// overlay pass.
void ChartPanel::drawTfPicker(Ui& u, Feeds& feeds) {
  if (!u.overlayOpen(m_tfPickerId)) {
    if (m_tfInput.focused) { // popup closed while the field was focused
      m_tfInput.focused = false;
      shell_ime_blur();
    }
    return;
  }
  const Theme& t = theme();
  Rect r = m_tfPickerRect;
  u.draw.shadow(r, t.radius, 14.0f, 3.0f, hexColor(0x000000, 0.45f));
  u.draw.rect(r, t.panel, t.radius);
  u.draw.rectOutline(r, t.border, 1.0f, t.radius);

  Rect field{r.x + 8, r.y + 8, r.w - 16, 26};
  if (m_autoFocusTf) {
    shell_ime_set("");
    shell_ime_focus(field.x, field.y, field.w, field.h);
    m_tfInput.focused = true;
    m_autoFocusTf = false;
  }
  if (textField(u, field, m_tfInput, "##tffield", "45m, 2h, 100t, 500v…"))
    m_tfError = false; // typing clears the error state

  if (m_tfInput.submitted) {
    m_tfInput.submitted = false;
    Timeframe tf;
    if (parseTimeframe(m_tfInput.text.c_str(), tf)) {
      feeds.setTimeframe(tf);
      u.closeOverlay(m_tfPickerId);
      return;
    }
    m_tfError = true;
  }

  u.draw.textAligned({r.x + 8, field.y + field.h + 6, r.w - 16, 16},
                     m_tfError ? "not a timeframe - try 45m, 2h, 100t, 500v"
                               : "m h d w time, t ticks, v volume",
                     m_tfError ? t.red : t.textDim, DrawList::Left);
}

// Per-venue flow-source popover: shared with the orderbook/DOM widgets (see
// flow_sources.h). The mask rewrite also feeds Feeds::setFlowMask via draw();
// live prints follow the new mask, historical bootstrap stays until the next
// symbol/timeframe load.
void ChartPanel::drawFlowPicker(Ui& u, Feeds& feeds) {
  if (!u.overlayOpen(m_flowPickerId)) return;
  if (drawFlowSources(u, m_flowPickerRect, m_flowMask, feeds, m_flowPickerList))
    saveSettings();
}

