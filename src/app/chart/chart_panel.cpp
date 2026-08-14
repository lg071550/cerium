#include "chart_panel.h"

#include "../../data/feeds.h"
#include "../../ui/theme.h"
#include "../../ui/ui_context.h"
#include "../../ui/widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

// canonical symbols — index matches feeds/registry.ts SYMBOLS
static const char* kSyms[] = {"ETH", "BTC", "SOL"};

// ---------------------------------------------------------------------------
// indicator computations (full series, NaN before minBars)
// ---------------------------------------------------------------------------

static void computeVol(const CandleSeries& cs, std::vector<float>& out) {
  size_t n = cs.v.size();
  out.resize(n);
  for (size_t i = 0; i < n; ++i) out[i] = (float)cs.v[i].vol;
}

static void computeCvd(const CandleSeries& cs, std::vector<float>& out) {
  size_t n = cs.v.size();
  out.resize(n);
  double acc = 0;
  for (size_t i = 0; i < n; ++i) {
    acc += cs.v[i].delta;
    out[i] = (float)acc;
  }
}

// EMA seeded with the SMA of the first `period` closes
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

static void computeEma21(const CandleSeries& cs, std::vector<float>& out) {
  emaSeries(cs, 21, out);
}

static void computeSma50(const CandleSeries& cs, std::vector<float>& out) {
  smaSeries(cs, 50, out);
}

// Bollinger basis (SMA 20); the ±2σ bands are derived at draw time
static void computeBoll(const CandleSeries& cs, std::vector<float>& out) {
  smaSeries(cs, 20, out);
}

static void computeRsi14(const CandleSeries& cs, std::vector<float>& out) {
  size_t n = cs.v.size();
  out.assign(n, NAN);
  const int p = 14;
  if (n <= (size_t)p) return;
  double gain = 0, loss = 0;
  for (int i = 1; i <= p; ++i) {
    double d = cs.v[(size_t)i].c - cs.v[(size_t)i - 1].c;
    if (d > 0) gain += d;
    else loss -= d;
  }
  gain /= p;
  loss /= p;
  auto rsi = [](double g, double l) {
    return l == 0 ? 100.0f : (float)(100.0 - 100.0 / (1.0 + g / l));
  };
  out[(size_t)p] = rsi(gain, loss);
  for (size_t i = (size_t)p + 1; i < n; ++i) { // Wilder smoothing
    double d = cs.v[i].c - cs.v[i - 1].c;
    gain = (gain * (p - 1) + (d > 0 ? d : 0)) / p;
    loss = (loss * (p - 1) + (d < 0 ? -d : 0)) / p;
    out[i] = rsi(gain, loss);
  }
}

static void computeMacd(const CandleSeries& cs, std::vector<float>& out) {
  size_t n = cs.v.size();
  out.assign(n, NAN);
  if (n < 26) return;
  double e12 = cs.v[0].c, e26 = cs.v[0].c;
  const double k12 = 2.0 / 13, k26 = 2.0 / 27;
  for (size_t i = 1; i < n; ++i) {
    e12 += (cs.v[i].c - e12) * k12;
    e26 += (cs.v[i].c - e26) * k26;
    if (i >= 25) out[i] = (float)(e12 - e26);
  }
}

// ---------------------------------------------------------------------------
// registry
// ---------------------------------------------------------------------------

// pane indicators stack below the price pane in registry order
enum { IndVol, IndCvd, IndRsi, IndMacd, IndEma, IndSma, IndBoll };

static const Indicator kRegistry[] = {
    {"VOL", false, 1, computeVol},
    {"CVD", false, 1, computeCvd},
    {"RSI 14", false, 15, computeRsi14},
    {"MACD 12 26 9", false, 26, computeMacd},
    {"EMA 21", true, 21, computeEma21},
    {"SMA 50", true, 50, computeSma50},
    {"BB 20 2", true, 20, computeBoll},
};

const Indicator* indicatorRegistry() { return kRegistry; }
int indicatorCount() { return (int)(sizeof(kRegistry) / sizeof(kRegistry[0])); }

// cheap change detector: size + interval + symbol + the live (last) candle
static uint64_t candleSig(const CandleSeries& cs) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](uint64_t x) {
    h ^= x;
    h *= 1099511628211ull;
  };
  mix(cs.v.size());
  mix((uint64_t)cs.intervalMin);
  mix((uint64_t)(cs.sym + 1));
  if (!cs.v.empty()) {
    const Candle& c = cs.v.back();
    auto bits = [](double d) {
      uint64_t u;
      std::memcpy(&u, &d, 8);
      return u;
    };
    mix(bits(c.ts));
    mix(bits(c.o));
    mix(bits(c.h));
    mix(bits(c.l));
    mix(bits(c.c));
    mix(bits(c.vol));
    mix(bits(c.delta));
  }
  return h;
}

void ChartPanel::ensureComputed(const CandleSeries& cs) {
  uint64_t sig = candleSig(cs);
  if (sig == m_computedSig) return;
  m_computedSig = sig;
  int regN = indicatorCount();
  m_cache.resize((size_t)regN);
  for (int i = 0; i < regN; ++i)
    if (m_enabled[(size_t)i]) kRegistry[i].compute(cs, m_cache[(size_t)i]);

  // MACD signal line: EMA9 over the cached MACD series
  m_macdSignal.clear();
  if (m_enabled[IndMacd] && !m_cache[IndMacd].empty()) {
    const std::vector<float>& macd = m_cache[IndMacd];
    m_macdSignal.assign(macd.size(), NAN);
    const double k = 2.0 / 10;
    double ema = 0;
    int valid = 0;
    for (size_t i = 0; i < macd.size(); ++i) {
      if (std::isnan(macd[i])) continue;
      ema = valid == 0 ? macd[i] : ema + (macd[i] - ema) * k;
      if (++valid >= 9) m_macdSignal[i] = (float)ema;
    }
  }
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
                       int vis0, int vis1, int startIdx, float bw, Color c,
                       float thick = 1.0f) {
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
    xy.push_back(pane.area.x + (i - startIdx) * bw + bw * 0.5f);
    xy.push_back(pane.yOf(v));
  }
  flush();
}

// corner tag top-left of a pane: dim name + bright latest value
static void drawCornerTag(DrawList& d, const ChartPane& pane, const char* name,
                          float value, ChartFmt fmt) {
  Rect r{pane.area.x + 6, pane.area.y + 3, pane.area.w - 12, 14};
  d.textAligned(r, name, theme().textDim, DrawList::Left);
  if (std::isnan(value)) return;
  char vb[24];
  fmt(vb, sizeof(vb), value);
  float nw = d.measure(name);
  d.textAligned({r.x + nw + 10, r.y, r.w - nw - 10, r.h}, vb, theme().text,
                DrawList::Left);
}

// ---------------------------------------------------------------------------
// panel
// ---------------------------------------------------------------------------

void ChartPanel::draw(
    Ui& u, Rect r, Feeds& feeds,
    const std::function<void(float, float, std::vector<ChartMenuItem>)>& openMenu) {
  const Theme& t = theme();
  const CandleSeries& cs = feeds.candles;

  const int regN = indicatorCount();
  if ((int)m_enabled.size() != regN) { // first frame: defaults
    m_enabled.assign((size_t)regN, false);
    m_enabled[IndVol] = true;
    m_enabled[IndCvd] = true;
    m_enabled[IndEma] = true;
  }

  // header: symbol bright, venue/interval dim (segmented, no separator glyphs)
  char sym[16];
  snprintf(sym, sizeof(sym), "%sUSDT", kSyms[feeds.symbol]);
  u.draw.textAligned({r.x, r.y, r.w, 26}, sym, t.text, DrawList::Left, 12);
  float symW = u.draw.measure(sym);
  char sub[32];
  snprintf(sub, sizeof(sub), "Binance Perp  %dm", cs.intervalMin);
  u.draw.textAligned({r.x + 12 + symW + 12, r.y, r.w - symW - 36, 26}, sub, t.textDim,
                     DrawList::Left);

  // right-click → interval + indicator menu
  if (u.input.rightPressed && r.contains(u.input.mouseX, u.input.mouseY)) {
    static const int kIntervals[] = {1, 5, 15};
    std::vector<ChartMenuItem> items;
    char label[48];
    for (int m : kIntervals) {
      snprintf(label, sizeof(label), "%s %dm",
               cs.intervalMin == m ? "\xe2\x9c\x93" : " ", m);
      items.push_back({label, [&feeds, m] { feeds.setCandleInterval(m); }});
    }
    for (int i = 0; i < regN; ++i) {
      snprintf(label, sizeof(label), "%s %s", m_enabled[(size_t)i] ? "\xe2\x9c\x93" : " ",
               kRegistry[i].name);
      items.push_back({label, [this, i] {
        m_enabled[(size_t)i] = !m_enabled[(size_t)i];
        ++m_rngGen; // pane set changed: visible ranges are stale
      }});
    }
    openMenu(u.input.mouseX, u.input.mouseY, std::move(items));
  }

  if (cs.v.empty()) {
    u.draw.textAligned(r, "loading candles…", t.textDim, DrawList::Center);
    return;
  }

  ensureComputed(cs);

  // layout: header / price pane (flex) / indicator panes / time axis; the
  // gutter spans every pane; panes separated by a tonal gap, no borders
  const float gutterW = 72.0f;
  const float timeAxisH = 18.0f;
  const float indH = 56.0f;
  const float gap = 4.0f;

  int paneReg[8]; // registry index of each indicator pane, top → bottom
  int nPanes = 0;
  for (int i = 0; i < regN; ++i)
    if (m_enabled[(size_t)i] && !kRegistry[i].overlay) paneReg[nPanes++] = i;

  Rect chart{r.x, r.y + 26, r.w, r.h - 26};
  Rect gutter{chart.x + chart.w - gutterW, chart.y, gutterW, chart.h - timeAxisH};
  Rect timeAxis{chart.x, chart.y + chart.h - timeAxisH, chart.w, timeAxisH};
  float stackH = chart.h - timeAxisH;
  float priceH = stackH - nPanes * (indH + gap);
  if (priceH < 60.0f) priceH = 60.0f; // degrade gracefully in short windows

  ChartPane price;
  price.area = {chart.x, chart.y, chart.w - gutterW, priceH};
  price.grid = true;

  ChartPane ind[8];
  float py = price.area.y + price.area.h + gap;
  for (int p = 0; p < nPanes; ++p) {
    ind[p].area = {chart.x, py, chart.w - gutterW, indH};
    py += indH + gap;
  }

  int size = (int)cs.v.size();

  // wheel scrolls through history (up = back in time), anywhere on the stack
  Rect stackAll{chart.x, chart.y, chart.w - gutterW, stackH};
  if (u.hovered(stackAll) && u.input.wheelY != 0) scroll += u.input.wheelY < 0 ? 3 : -3;
  scroll = std::clamp(scroll, 0, std::max(0, size - 20));

  int slots = std::max(10, (int)(price.area.w / 7.0f));
  int freeSlots = scroll == 0 ? std::min(20, slots / 4) : 0;
  int endSlot = size - 1 + freeSlots - scroll;
  int startIdx = endSlot - slots + 1;
  float bw = price.area.w / slots;
  int vis0 = std::max(0, startIdx);
  int vis1 = std::min(size - 1, endSlot);
  auto xOf = [&](int i) { return price.area.x + (i - startIdx) * bw; };

  // visible-window ranges (price + panes): recomputed only when the window,
  // candle signature, or pane toggles change — history is append-only and the
  // live candle is covered by the signature, so cached scans stay valid
  bool rangeOk[8];
  if (m_computedSig != m_rngSig || vis0 != m_rngV0 || vis1 != m_rngV1 ||
      m_rngGen != m_rngToggles) {
    m_rngSig = m_computedSig;
    m_rngV0 = vis0;
    m_rngV1 = vis1;
    m_rngToggles = m_rngGen;
    m_rngPanes.assign((size_t)nPanes, PaneRange{false, 0, 1});

    double lo = 1e300, hi = -1e300; // price range over visible candles
    for (int i = vis0; i <= vis1; ++i) {
      lo = std::min(lo, cs.v[(size_t)i].l);
      hi = std::max(hi, cs.v[(size_t)i].h);
    }
    m_rngPriceLo = lo;
    m_rngPriceHi = hi;

    for (int p = 0; p < nPanes; ++p) { // per-pane ranges from the window
      int ri = paneReg[p];
      const std::vector<float>& s = m_cache[(size_t)ri];
      PaneRange& pr = m_rngPanes[(size_t)p];
      if (ri == IndRsi) {
        pr = {true, 0, 100};
        continue;
      }
      double pLo = 1e300, pHi = -1e300;
      for (int i = vis0; i <= vis1 && i < (int)s.size(); ++i) {
        float v = s[(size_t)i];
        if (std::isnan(v)) continue;
        pLo = std::min(pLo, (double)v);
        pHi = std::max(pHi, (double)v);
        if (ri == IndMacd && i < (int)m_macdSignal.size() &&
            !std::isnan(m_macdSignal[(size_t)i])) {
          double sv = m_macdSignal[(size_t)i];
          pLo = std::min(pLo, sv);
          pHi = std::max(pHi, sv);
          pLo = std::min(pLo, (double)v - sv); // histogram extents
          pHi = std::max(pHi, (double)v - sv);
        }
      }
      if (pHi < pLo) { // series shorter than minBars: chrome only
        pr = {false, 0, 1};
        continue;
      }
      if (ri == IndVol) {
        pr = {true, 0, pHi * 1.05 + 1e-9};
      } else if (ri == IndMacd) { // symmetric-ish around zero
        double m = std::max(std::fabs(pLo), std::fabs(pHi)) * 1.1 + 1e-9;
        pr = {true, -m, m};
      } else {
        double pp = (pHi - pLo) * 0.1 + 1e-9;
        pr = {true, pLo - pp, pHi + pp};
      }
    }
  }
  for (int p = 0; p < nPanes; ++p) {
    const PaneRange& pr = m_rngPanes[(size_t)p];
    ind[p].lo = pr.lo;
    ind[p].hi = pr.hi;
    rangeOk[p] = pr.ok;
  }
  double padv = (m_rngPriceHi - m_rngPriceLo) * 0.06 + 1e-9;
  price.lo = m_rngPriceLo - padv;
  price.hi = m_rngPriceHi + padv;

  double lastC = cs.v.back().c;
  float ly = price.yOf(lastC);
  bool showTag = ly > price.area.y && ly < price.area.y + price.area.h;
  bool cross = price.area.contains(u.input.mouseX, u.input.mouseY);
  float crossTagY = cross ? u.input.mouseY : -1e9f;

  // recessed indicator pane backgrounds (tone only — no borders)
  for (int p = 0; p < nPanes; ++p) u.draw.rect(ind[p].area, t.chartPaneBg, 4.0f);

  // price grid: nice ticks, horizontal lines confined to the price pane
  {
    static thread_local std::vector<double> ticks;
    niceTicks(price.lo, price.hi, 5, ticks);
    for (double tv : ticks) {
      float y = price.yOf(tv);
      if (y < price.area.y + 2 || y > price.area.y + price.area.h - 2) continue;
      u.draw.rect({price.area.x, y, price.area.w, 1}, withAlpha(t.border, 0.6f));
    }
  }
  drawPaneGutter(u.draw, gutter, price, chartFmtPrice, 5, showTag ? ly : -1e9f,
                 crossTagY);

  // time axis: labels + vertical gridlines, both confined to the price pane
  int labelEvery = std::max(1, (int)(80.0f / bw));
  for (int s = 0; s < slots; ++s) {
    int i = startIdx + s;
    if (i < 0 || i >= size || s % labelEvery != 0) continue;
    const char* tb = hhmmLabel((time_t)(cs.v[(size_t)i].ts / 1000.0));
    float x = price.area.x + s * bw + bw * 0.5f;
    u.draw.rect({x, price.area.y, 1, price.area.h}, withAlpha(t.border, 0.35f));
    u.draw.textAligned({x - 40, timeAxis.y, 80, timeAxis.h}, tb, t.textDim,
                       DrawList::Center);
  }

  // candles + overlays, clipped to the price pane
  u.draw.pushClip(price.area);

  // Bollinger band fill sits beneath the candles
  if (m_enabled[IndBoll]) {
    const std::vector<float>& basis = m_cache[IndBoll];
    static thread_local std::vector<float> bx, bTop, bBot;
    bx.clear();
    bTop.clear();
    bBot.clear();
    int last = -1;
    for (int i = vis0; i <= vis1 && i < (int)basis.size(); ++i) {
      float b = basis[(size_t)i];
      if (std::isnan(b)) continue;
      double var = 0;
      for (int j = i - 19; j <= i; ++j) {
        double d = cs.v[(size_t)j].c - b;
        var += d * d;
      }
      double sd = std::sqrt(var / 20);
      bx.push_back(xOf(i));
      bTop.push_back(price.yOf(b + 2 * sd));
      bBot.push_back(price.yOf(b - 2 * sd));
      last = i;
    }
    if (bx.size() >= 2) { // final right edge closes the last span
      bx.push_back(xOf(last) + bw);
      bTop.push_back(bTop.back());
      bBot.push_back(bBot.back());
      u.draw.seriesBand(bx.data(), bTop.data(), bBot.data(), (int)bx.size(),
                        withAlpha(t.accent, 0.05f));
    }
  }

  for (int i = vis0; i <= vis1; ++i) {
    const Candle& c = cs.v[(size_t)i];
    float x = xOf(i);
    bool up = c.c >= c.o;
    Color col = up ? t.green : t.red;
    float cx = x + bw * 0.5f;
    u.draw.line(cx, price.yOf(c.h), cx, price.yOf(c.l), withAlpha(col, 0.9f), 1.0f);
    float yO = price.yOf(c.o), yC = price.yOf(c.c);
    u.draw.rect({x + bw * 0.15f, std::min(yO, yC), bw * 0.7f,
                 std::max(std::fabs(yO - yC), 1.0f)},
                withAlpha(col, 0.9f));
  }

  // overlay series + legend tags (top-left, one row per overlay)
  {
    float legendY = price.area.y + 6;
    for (int i = 0; i < regN; ++i) {
      if (!m_enabled[(size_t)i] || !kRegistry[i].overlay) continue;
      Color c = i == IndEma ? t.accent : i == IndSma ? t.chartWarm
                                                     : withAlpha(t.accent, 0.55f);
      drawSeries(u.draw, price, m_cache[(size_t)i], vis0, vis1, startIdx, bw, c);
      float v = lastValid(m_cache[(size_t)i]);
      u.draw.textAligned({price.area.x + 6, legendY, price.area.w - 12, 14},
                         kRegistry[i].name, c, DrawList::Left);
      if (!std::isnan(v)) {
        char vb[24];
        chartFmtPrice(vb, sizeof(vb), v);
        float nw = u.draw.measure(kRegistry[i].name);
        u.draw.textAligned(
            {price.area.x + 6 + nw + 10, legendY, price.area.w - nw - 28, 14}, vb,
            t.text, DrawList::Left);
      }
      legendY += 16;
    }
  }

  u.draw.popClip();

  // indicator panes: content clipped to the recess, gutter labels outside it
  for (int p = 0; p < nPanes; ++p) {
    int ri = paneReg[p];
    const Indicator& indDef = kRegistry[ri];
    const std::vector<float>& s = m_cache[(size_t)ri];
    ChartPane& pane = ind[p];

    if (rangeOk[p]) {
      u.draw.pushClip(pane.area);
      if (ri == IndVol) {
        for (int i = vis0; i <= vis1; ++i) {
          const Candle& c = cs.v[(size_t)i];
          Color col = c.c >= c.o ? t.green : t.red;
          float x = xOf(i);
          float yTop = pane.yOf(s[(size_t)i]);
          u.draw.rect({x + bw * 0.15f, yTop, bw * 0.7f,
                       pane.area.y + pane.area.h - yTop},
                      withAlpha(col, 0.3f));
        }
      } else if (ri == IndCvd) {
        if (pane.lo < 0 && pane.hi > 0) // zero guide
          u.draw.rect({pane.area.x, pane.yOf(0), pane.area.w, 1},
                      withAlpha(t.textDim, 0.4f));
        drawSeries(u.draw, pane, s, vis0, vis1, startIdx, bw, t.accent);
      } else if (ri == IndRsi) {
        for (double g : {30.0, 70.0})
          u.draw.rect({pane.area.x, pane.yOf(g), pane.area.w, 1},
                      withAlpha(t.textDim, 0.3f));
        drawSeries(u.draw, pane, s, vis0, vis1, startIdx, bw, t.chartPurple);
      } else if (ri == IndMacd) {
        u.draw.rect({pane.area.x, pane.yOf(0), pane.area.w, 1},
                    withAlpha(t.textDim, 0.4f));
        for (int i = vis0; i <= vis1 && i < (int)m_macdSignal.size(); ++i) {
          float mv = s[(size_t)i], sv = m_macdSignal[(size_t)i];
          if (std::isnan(mv) || std::isnan(sv)) continue;
          float hv = mv - sv;
          float y0 = pane.yOf(hv > 0 ? hv : 0.0f);
          float y1 = pane.yOf(hv > 0 ? 0.0f : hv);
          float x = xOf(i);
          u.draw.rect({x + bw * 0.2f, y0, bw * 0.6f, std::max(y1 - y0, 1.0f)},
                      withAlpha(hv >= 0 ? t.green : t.red, 0.35f));
        }
        drawSeries(u.draw, pane, s, vis0, vis1, startIdx, bw, t.accent);
        drawSeries(u.draw, pane, m_macdSignal, vis0, vis1, startIdx, bw, t.chartWarm);
      }
      u.draw.popClip();
    }

    // chrome: corner tag + gutter labels (also when the series is too short)
    ChartFmt fmt = ri == IndRsi ? chartFmtPlain
                   : ri == IndMacd ? chartFmtPrice
                                   : chartFmtVol;
    float v = rangeOk[p] ? lastValid(s) : NAN;
    drawCornerTag(u.draw, pane, indDef.name, v, fmt);
    if (rangeOk[p])
      drawPaneGutter(u.draw, gutter, pane, ri == IndRsi ? chartFmtInt : fmt, 3);
  }

  // last-price line + solid gutter tag + bar-close countdown beneath it
  if (showTag) {
    u.draw.line(price.area.x, ly, gutter.x + 2, ly, withAlpha(t.accent, 0.85f), 1.0f);
    u.draw.breakCmd(); // tag (quad + shadow) must layer over the price line
    char lp[24];
    snprintf(lp, sizeof(lp), "%.2f", lastC);
    Rect tag{gutter.x + 2, ly - 9, gutter.w - 4, 18};
    gutterTag(u.draw, tag, lp, t.accent);

    time_t nowSec = time(nullptr);
    long intervalSec = (long)cs.intervalMin * 60;
    long remain = intervalSec - ((long)nowSec % intervalSec);
    char cd[16];
    snprintf(cd, sizeof(cd), "%ld:%02ld", remain / 60, remain % 60);
    u.draw.textAligned({gutter.x + 2, ly + 11, gutter.w - 4, 14}, cd, t.textDim,
                       DrawList::Center);
  }

  // crosshair: vertical spans the pane stack; horizontal + price tag stay on
  // the price pane; time chip on the hovered bar
  if (cross) {
    float mx = u.input.mouseX, my = u.input.mouseY;
    Color xc = withAlpha(t.textDim, 0.5f);
    u.draw.line(price.area.x, my, price.area.x + price.area.w, my, xc, 1.0f);
    u.draw.line(mx, chart.y, mx, chart.y + stackH, xc, 1.0f);
    double pr = price.hi - (double)(my - price.area.y) / price.area.h *
                               (price.hi - price.lo);
    char pb[24];
    snprintf(pb, sizeof(pb), "%.2f", pr);
    // gutter tag + time chip (quads + shadows) must layer over the crosshair
    // lines — same fix as the live-price tag above
    u.draw.breakCmd();
    Rect tag{gutter.x + 2, my - 9, gutter.w - 4, 18};
    gutterTag(u.draw, tag, pb, t.text);

    int hi_ = (int)((mx - price.area.x) / bw);
    int ci = startIdx + hi_;
    if (ci >= 0 && ci < size) {
      const char* tb = hhmmLabel((time_t)(cs.v[(size_t)ci].ts / 1000.0));
      float x = price.area.x + hi_ * bw + bw * 0.5f;
      Rect chip{x - 26, timeAxis.y - 1, 52, timeAxis.h + 2};
      gutterTag(u.draw, chip, tb, t.text);
    }
  }
}
