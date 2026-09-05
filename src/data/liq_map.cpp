#include "liq_map.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

double liqMapNiceStep(double raw) {
  if (!(raw > 0) || !std::isfinite(raw)) return 1;
  double mag = std::pow(10.0, std::floor(std::log10(raw)));
  for (double mult : {1.0, 2.0, 2.5, 5.0, 10.0})
    if (mult * mag >= raw) return mult * mag;
  return 10.0 * mag;
}

double liqMapAutoBin(double mid, double nativeTick) {
  // ~12.5 bps of mid ≈ $100 on BTC. Do not multiply HeatmapSeries::nativeTickFor
  // — that is median aggregated book spacing ($1–$50+), and 1000× it is a
  // $1k–$50k lattice. Only trust nativeTick when it looks like a contract
  // tick (< 0.5 bps of mid), e.g. BTC $0.10.
  double raw = mid > 0 ? mid * 0.00125 : 0;
  if (nativeTick > 0 && mid > 0 && nativeTick <= mid * 5e-6)
    raw = nativeTick * 1000.0;
  if (!(raw > 0) || !std::isfinite(raw)) return 1;
  return liqMapNiceStep(raw);
}

double liqMapBinUsd(int sel) {
  sel = std::clamp(sel, 1, 100);
  return liqMapNiceStep(std::pow(10.0, -1.25 + (double)sel * 0.04));
}

void LiqMapSeries::clear() {
  bars.clear();
  bin = 0;
  ref = 0;
  sym = -1;
  tfKind = -1;
  tfValue = 0;
  builtShape = ~0ull;
  builtOiHist = 0;
  builtBands = 0;
}

static void finishBar(HeatmapBar& b) {
  if (b.rows.empty()) {
    b.heat.clear();
    b.row0 = 0;
    return;
  }
  b.row0 = b.rows.front();
}

static void capClosest(HeatmapBar& b, int64_t midRow, int cap) {
  if ((int)b.rows.size() <= cap) return;
  std::vector<size_t> idx(b.rows.size());
  for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
  std::nth_element(idx.begin(), idx.begin() + cap, idx.end(),
                   [&](size_t a, size_t bIdx) {
                     auto dist = [&](size_t k) {
                       int64_t d = b.rows[k] > midRow ? b.rows[k] - midRow
                                                      : midRow - b.rows[k];
                       return d;
                     };
                     int64_t da = dist(a), db = dist(bIdx);
                     if (da != db) return da < db;
                     return a < bIdx;
                   });
  idx.resize((size_t)cap);
  std::sort(idx.begin(), idx.end(),
            [&](size_t a, size_t bIdx) { return b.rows[a] < b.rows[bIdx]; });
  std::vector<int64_t> rows;
  std::vector<float> heat;
  rows.reserve(idx.size());
  heat.reserve(idx.size());
  for (size_t i : idx) {
    rows.push_back(b.rows[i]);
    heat.push_back(b.heat[i]);
  }
  b.rows.swap(rows);
  b.heat.swap(heat);
}

static void addHeat(HeatmapBar& b, int64_t row, float w) {
  if (!(w > 0)) return;
  auto it = std::lower_bound(b.rows.begin(), b.rows.end(), row);
  size_t i = (size_t)(it - b.rows.begin());
  if (it != b.rows.end() && *it == row) {
    b.heat[i] += w;
    return;
  }
  b.rows.insert(it, row);
  b.heat.insert(b.heat.begin() + (std::ptrdiff_t)i, w);
}


static void resampleOi(const CandleSeries& cs, const MarketSeries* mkt,
                       std::vector<double>& out) {
  const size_t n = cs.v.size();
  out.assign(n, NAN);
  if (!mkt || mkt->oi.empty()) return;
  size_t j = 0;
  double last = NAN;
  for (size_t i = 0; i < n; ++i) {
    const double t = cs.v[i].ts;
    while (j < mkt->oi.size() && mkt->oi[j].ts <= t) {
      last = mkt->oi[j].oi;
      ++j;
    }
    out[i] = last;
  }
}

static void resampleFund(const CandleSeries& cs, const MarketSeries* mkt,
                         std::vector<double>& out) {
  const size_t n = cs.v.size();
  out.assign(n, NAN);
  if (!mkt || mkt->funding.empty()) return;
  size_t j = 0;
  double last = NAN;
  for (size_t i = 0; i < n; ++i) {
    const double t = cs.v[i].ts;
    while (j < mkt->funding.size() && mkt->funding[j].ts <= t) {
      last = mkt->funding[j].rate;
      ++j;
    }
    out[i] = last;
  }
}

static double meanAbsDelta(const CandleSeries& cs, size_t i, int lookback) {
  if (i == 0 || lookback < 1) return 0;
  const size_t i0 = i > (size_t)lookback ? i - (size_t)lookback : 0;
  double sum = 0;
  int n = 0;
  for (size_t k = i0; k < i; ++k) {
    double d = std::fabs(cs.v[k].delta);
    if (std::isfinite(d)) {
      sum += d;
      ++n;
    }
  }
  return n > 0 ? sum / n : 0;
}

static constexpr float kLev[] = {10.0f, 25.0f, 50.0f, 100.0f};
static constexpr float kBandW[] = {0.35f, 0.35f, 0.20f, 0.10f};
static constexpr uint32_t kBandBit[] = {LiqMapSeries::kBand10, LiqMapSeries::kBand25,
                                        LiqMapSeries::kBand50, LiqMapSeries::kBand100};

static void stampBar(HeatmapBar& b, const Candle& c, double meanAbs, double oi,
                     double prevOi, double fund, uint32_t bands, double bin) {
  if (!(bin > 0) || !std::isfinite(c.c) || !(c.c > 0)) return;
  const double dlt = c.delta;
  if (!std::isfinite(dlt) || dlt == 0) return;
  const double mag = std::fabs(dlt);
  const double z = meanAbs > 1e-12 ? mag / meanAbs : 1.0;
  if (z < 0.75) return;
  if (std::isfinite(oi) && std::isfinite(prevOi) && prevOi > 0) {
    const double dOi = oi - prevOi;
    if (dOi < -0.002 * prevOi) return;
  }
  double w = mag * c.c * std::min(z, 6.0);
  if (std::isfinite(oi) && std::isfinite(prevOi) && prevOi > 0) {
    const double dOi = std::max(0.0, oi - prevOi);
    w *= 1.0 + std::min(dOi / prevOi * 8.0, 1.5);
  }
  const bool longs = dlt > 0;
  if (std::isfinite(fund)) {
    const double bias = longs ? fund : -fund;
    w *= 1.0 + std::clamp(bias * 30.0, 0.0, 0.40);
  }
  const double px = (c.h + c.l + c.c) / 3.0;
  if (!(px > 0) || !std::isfinite(px)) return;
  for (int k = 0; k < 4; ++k) {
    if ((bands & kBandBit[k]) == 0) continue;
    const double lev = (double)kLev[k];
    const double liq = longs ? px * (1.0 - 1.0 / lev) : px * (1.0 + 1.0 / lev);
    if (!(liq > 0) || !std::isfinite(liq)) continue;
    const int64_t row = (int64_t)std::floor(liq / bin);
    const float hw = (float)(w * (double)kBandW[k]);
    addHeat(b, row, hw);
    addHeat(b, row - 1, hw * 0.45f);
    addHeat(b, row + 1, hw * 0.45f);
  }
}

static void fillBar(LiqMapSeries& hs, const CandleSeries& cs, size_t i,
                    const std::vector<double>& oi, const std::vector<double>& fund,
                    uint32_t bands) {
  HeatmapBar& b = hs.bars[i];
  b.ts = cs.v[i].ts;
  if (i > 0)
    heatmapCopyPunch(hs.bars[i - 1], b, cs.v[i].l, cs.v[i].h, hs.bin);
  else {
    b.rows.clear();
    b.heat.clear();
  }
  const double meanAbs = meanAbsDelta(cs, i, LiqMapSeries::kLookback);
  const double o = i < oi.size() ? oi[i] : NAN;
  const double po = i > 0 && i - 1 < oi.size() ? oi[i - 1] : NAN;
  const double f = i < fund.size() ? fund[i] : NAN;
  stampBar(b, cs.v[i], meanAbs, o, po, f, bands, hs.bin);
  const double mid = cs.v[i].c;
  if (mid > 0 && hs.bin > 0)
    capClosest(b, (int64_t)std::floor(mid / hs.bin), LiqMapSeries::kMaxRows);
  finishBar(b);
}

static float percentileRef(const float* values, int n) {
  if (n <= 0) return 0;
  std::vector<float> v;
  v.reserve((size_t)n);
  for (int i = 0; i < n; ++i)
    if (values[i] > 0) v.push_back(values[i]);
  if (v.empty()) return 0;
  size_t k = (v.size() * 95) / 100;
  if (k >= v.size()) k = v.size() - 1;
  std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)k, v.end());
  return v[k];
}

static float smoothRef(float previous, float sample) {
  if (!(sample > 0) || !std::isfinite(sample)) return previous;
  if (!(previous > 0) || !std::isfinite(previous)) return sample;
  if (sample < previous * 0.25f || sample > previous * 4.0f) return sample;
  float rate = sample > previous ? 0.35f : 0.12f;
  return previous + (sample - previous) * rate;
}

static void refreshRef(LiqMapSeries& hs) {
  if (hs.bars.empty()) {
    hs.ref = 0;
    return;
  }
  // Last closed column — the live bar's wick/delta must not retune the
  // colormap every tick (that was the field flicker).
  const HeatmapBar& src =
      hs.bars.size() >= 2 ? hs.bars[hs.bars.size() - 2] : hs.bars.back();
  if (src.heat.empty()) return;
  float fresh = percentileRef(src.heat.data(), (int)src.heat.size());
  hs.ref = smoothRef(hs.ref, fresh);
}

static uint64_t shapeOf(const CandleSeries& cs) {
  if (cs.v.empty()) return 0;
  uint64_t s = (uint64_t)cs.v.size();
  s = s * 0x100000001b3ull ^ (uint64_t)(int64_t)cs.v.front().ts;
  s = s * 0x100000001b3ull ^ (uint64_t)(int64_t)cs.v.back().ts;
  return s;
}

static size_t oiHistCount(const CandleSeries& cs, const MarketSeries* mkt) {
  if (!mkt || mkt->oi.empty() || cs.v.empty()) return 0;
  const double lastClosed =
      cs.v.size() >= 2 ? cs.v[cs.v.size() - 2].ts : cs.v.back().ts;
  size_t n = 0;
  for (const OiSample& s : mkt->oi) {
    if (s.ts <= lastClosed) ++n;
    else break;
  }
  return n;
}

void LiqMapSeries::rebuild(const CandleSeries& cs, const MarketSeries* mkt,
                           uint32_t bands) {
  const size_t n = cs.v.size();
  bars.assign(n, {});
  if (n == 0 || !(bin > 0)) {
    ref = 0;
    builtShape = shapeOf(cs);
    builtOiHist = oiHistCount(cs, mkt);
    builtBands = bands;
    return;
  }
  bands &= kBandAll;
  if (bands == 0) bands = kBandAll;
  std::vector<double> oi, fund;
  resampleOi(cs, mkt, oi);
  resampleFund(cs, mkt, fund);
  for (size_t i = 0; i < n; ++i) fillBar(*this, cs, i, oi, fund, bands);
  refreshRef(*this);
  builtShape = shapeOf(cs);
  builtOiHist = oiHistCount(cs, mkt);
  builtBands = bands;
}

bool LiqMapSeries::updateLast(const CandleSeries& cs, const MarketSeries* mkt,
                              uint32_t bands) {
  const size_t n = cs.v.size();
  if (n < 2 || bars.size() != n) return false;
  if (builtShape != shapeOf(cs)) return false;
  bands &= kBandAll;
  if (bands == 0) bands = kBandAll;
  if (builtBands != bands) return false;
  if (!(bin > 0)) return false;
  if (bars[n - 1].ts != cs.v[n - 1].ts || bars[n - 2].ts != cs.v[n - 2].ts)
    return false;

  auto oiAt = [](const std::vector<OiSample>& s, double t) -> double {
    if (s.empty()) return NAN;
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2;
      if (s[mid].ts <= t) lo = mid + 1;
      else hi = mid;
    }
    return lo == 0 ? NAN : s[lo - 1].oi;
  };
  auto fundAt = [](const std::vector<FundingSample>& s, double t) -> double {
    if (s.empty()) return NAN;
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2;
      if (s[mid].ts <= t) lo = mid + 1;
      else hi = mid;
    }
    return lo == 0 ? NAN : s[lo - 1].rate;
  };
  const double t = cs.v[n - 1].ts;
  const double tPrev = cs.v[n - 2].ts;
  const double o = mkt ? oiAt(mkt->oi, t) : NAN;
  const double po = mkt ? oiAt(mkt->oi, tPrev) : NAN;
  const double f = mkt ? fundAt(mkt->funding, t) : NAN;

  HeatmapBar& b = bars[n - 1];
  b.ts = cs.v[n - 1].ts;
  heatmapCopyPunch(bars[n - 2], b, cs.v[n - 1].l, cs.v[n - 1].h, bin);
  stampBar(b, cs.v[n - 1], meanAbsDelta(cs, n - 1, kLookback), o, po, f, bands,
           bin);
  const double mid = cs.v[n - 1].c;
  if (mid > 0 && bin > 0)
    capClosest(b, (int64_t)std::floor(mid / bin), kMaxRows);
  finishBar(b);
  refreshRef(*this);
  return true;
}
