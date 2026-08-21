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

enum {
  CipherLayerMfi = 1,
  CipherLayerHist = 2,
  CipherLayerDots = 4,
  CipherLayerAll = CipherLayerMfi | CipherLayerHist | CipherLayerDots
};

enum {
  CipherMarkCross = 1,
  CipherMarkUp = 2,
  CipherMarkBuy = 4,
  CipherMarkSell = 8,
  CipherMarkGold = 16,
  CipherMarkBlood = 32
};

inline int cipherLayers(int opt) { return opt ? opt : CipherLayerAll; }

inline double cipherHlc3(const Candle& c) { return (c.h + c.l + c.c) / 3.0; }

inline double cipherBody(const Candle& c) {
  double span = c.h - c.l;
  if (!(span > 0.0)) return 0.0;
  return (c.c - c.o) / span;
}

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
  bool cross = (d0 > 0.0f && d1 <= 0.0f) || (d0 < 0.0f && d1 >= 0.0f) ||
               (d0 == 0.0f && d1 != 0.0f);
  if (!cross) return 0;
  int8_t m = (int8_t)CipherMarkCross;
  bool up = d1 >= 0.0f;
  if (up) m = (int8_t)(m | CipherMarkUp);
  if (up && wt2 <= -60.0f) {
    m = (int8_t)(m | CipherMarkBuy);
    if (wt2 <= -80.0f && !std::isnan(rsi) && rsi < 30.0f)
      m = (int8_t)(m | CipherMarkGold);
  } else if (!up && wt2 >= 60.0f) {
    m = (int8_t)(m | CipherMarkSell);
    if (!(mfi > 0.0f)) m = (int8_t)(m | CipherMarkBlood);
  }
  return m;
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

inline void cipherMfi(const CandleSeries& cs, std::vector<float>& out) {
  const int period = 60;
  size_t n = cs.v.size();
  out.assign(n, NAN);
  if (n < (size_t)period) return;
  double sum = 0;
  for (int i = 0; i < period; ++i) sum += cipherBody(cs.v[(size_t)i]) * 150.0;
  out[(size_t)period - 1] = (float)(sum / period);
  for (size_t i = (size_t)period; i < n; ++i) {
    sum += cipherBody(cs.v[i]) * 150.0 -
           cipherBody(cs.v[i - (size_t)period]) * 150.0;
    out[i] = (float)(sum / period);
  }
}

inline float cipherMfiLast(const CandleSeries& cs, int period = 60) {
  size_t n = cs.v.size();
  if (n < (size_t)period || period < 1) return NAN;
  double sum = 0;
  for (size_t i = n - (size_t)period; i < n; ++i)
    sum += cipherBody(cs.v[i]) * 150.0;
  return (float)(sum / period);
}

// WaveTrend + MFI + marks. live* are n-2 seeds for the last-bar increment:
// esa, d, tci, RSI Wilder gain/loss.
inline void cipherCompute(const CandleSeries& cs, int n1, int n2, int n3,
                          std::vector<float>& wt1, std::vector<float>& wt2,
                          std::vector<float>& mfi, std::vector<int8_t>& marks,
                          double& liveEsa, double& liveDe, double& liveTci,
                          double& liveGain, double& liveLoss) {
  n1 = std::max(2, n1);
  n2 = std::max(2, n2);
  n3 = std::max(2, n3);
  size_t n = cs.v.size();
  wt1.assign(n, NAN);
  marks.assign(n, 0);
  liveEsa = liveDe = liveTci = liveGain = liveLoss = NAN;
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

  double gain = 0, loss = 0;
  bool rsiOn = false;
  const int rp = 14;
  float rsi = NAN;
  for (size_t i = 1; i < n; ++i) {
    double d = cs.v[i].c - cs.v[i - 1].c;
    if (!rsiOn) {
      if (d > 0) gain += d;
      else loss -= d;
      if (i == (size_t)rp) {
        gain /= rp;
        loss /= rp;
        rsiOn = true;
        rsi = cipherRsiOf(gain, loss);
      }
    } else {
      gain = (gain * (rp - 1) + (d > 0 ? d : 0)) / rp;
      loss = (loss * (rp - 1) + (d < 0 ? -d : 0)) / rp;
      rsi = cipherRsiOf(gain, loss);
    }
    if (i + 2 == n) {
      liveGain = rsiOn ? gain : NAN;
      liveLoss = rsiOn ? loss : NAN;
    }
    if (i > 0)
      marks[i] = cipherMark(wt1[i], wt2[i], wt1[i - 1], wt2[i - 1], rsi,
                            i < mfi.size() ? mfi[i] : NAN);
  }
}

inline bool cipherUpdateLast(const CandleSeries& cs, int n1, int n2, int n3,
                             std::vector<float>& wt1, std::vector<float>& wt2,
                             std::vector<float>& mfi, std::vector<int8_t>& marks,
                             double esa, double de, double tci, double gain,
                             double loss) {
  n1 = std::max(2, n1);
  n2 = std::max(2, n2);
  n3 = std::max(2, n3);
  size_t n = cs.v.size();
  if (n < 2 || wt1.size() != n || wt2.size() != n || mfi.size() != n ||
      marks.size() != n)
    return false;
  mfi[n - 1] = cipherMfiLast(cs);
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

inline void cipherDraw(DrawList& d, const ChartPane& pane,
                      const std::vector<float>& wt1, const std::vector<float>& wt2,
                      const std::vector<float>& mfi, const std::vector<int8_t>& marks,
                      int vis0, int vis1, float startF, float bw, Color wt1C,
                      Color wt2C, float thick, bool guides, int opt) {
  const Theme& th = theme();
  int layers = cipherLayers(opt);
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
    d7DrawBand(d, pane, mfi, zero, vis0, vis1, startF, bw, withAlpha(up, 0.42f),
               withAlpha(down, 0.42f));
  d7DrawBand(d, pane, wt1, wt2, vis0, vis1, startF, bw, withAlpha(wt1C, 0.32f),
             withAlpha(wt2C, 0.28f));

  if (layers & CipherLayerHist) {
    size_t n = std::min(wt1.size(), wt2.size());
    vwap.resize(n);
    for (size_t i = 0; i < n; ++i) {
      float a = wt1[i], b = wt2[i];
      vwap[i] = (std::isnan(a) || std::isnan(b)) ? NAN : a - b;
    }
    Color hc = withAlpha(wt2C, 0.30f);
    d7DrawBand(d, pane, vwap, zero, vis0, vis1, startF, bw, hc, hc);
  }

  d7DrawLead(d, pane, wt2, vis0, vis1, startF, bw, wt2C,
             std::max(1.0f, thick - 0.5f));
  d7DrawLead(d, pane, wt1, vis0, vis1, startF, bw, wt1C, thick + 0.25f);

  if (!(layers & CipherLayerDots)) return;
  d.breakCmd();
  float r = std::clamp(bw * 0.72f, 6.2f, 10.5f);
  float wr = std::clamp(bw * 0.38f, 3.6f, 6.2f);
  float xs = std::clamp(bw * 0.50f, 4.6f, 8.5f);
  float yBuy = std::clamp(pane.yOf(-60), top + r + 4.0f, bot - r - 4.0f);
  float ySell = std::clamp(pane.yOf(60), top + r + 4.0f, bot - r - 4.0f);
  for (int i = vis0; i <= vis1 && i < (int)marks.size() && i < (int)wt1.size() &&
                     i < (int)wt2.size();
       ++i) {
    int8_t m = marks[(size_t)i];
    if (!m) continue;
    float a = wt1[(size_t)i], b = wt2[(size_t)i];
    if (std::isnan(a) || std::isnan(b)) continue;
    float x = d7BarX(pane, (float)i, startF, bw);
    float y = pane.yOf(0.5f * (a + b));
    bool buy = (m & CipherMarkBuy) != 0;
    bool sell = (m & CipherMarkSell) != 0;
    bool gdot = (m & CipherMarkGold) != 0;
    bool blood = (m & CipherMarkBlood) != 0;
    bool upCross = (m & CipherMarkUp) != 0;
    d.circle(x, y, wr, withAlpha(upCross ? up : down, 0.95f));
    d.circleOutline(x, y, wr, withAlpha(th.text, 0.35f), 1.0f);
    if (gdot) {
      d.circle(x, yBuy, r, withAlpha(up, 0.95f));
      d.circleOutline(x, yBuy, r + 3.0f, withAlpha(gold, 0.95f), 1.6f);
    } else if (buy) {
      d.circle(x, yBuy, r, withAlpha(up, 0.95f));
      d.circleOutline(x, yBuy, r, withAlpha(th.text, 0.40f), 1.1f);
    }
    if (sell) {
      if (blood)
        cipherDiamond(d, x, ySell, r + 0.8f, withAlpha(down, 0.95f));
      else {
        d.circle(x, ySell, r, withAlpha(down, 0.95f));
        d.circleOutline(x, ySell, r, withAlpha(th.text, 0.40f), 1.1f);
      }
    }
    cipherCross(d, x, y, xs, withAlpha(gold, 0.92f), 1.8f);
  }
}
