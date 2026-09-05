#include "heatmap.h"

#include "feeds.h"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <vector>

void HeatmapSeries::clear() {
  bars.clear();
  bin = 0;
  ref = 0;
  nativeTick = 0;
  tickBooks = ~0ull;
  sampledBooks = ~0ull;
  sampledTs = -1;
  sym = -1;
  tfKind = -1;
  tfValue = 0;
}

void HeatmapSeries::syncToCandles(const CandleSeries& cs) {
  const size_t n = cs.v.size();
  if (n == 0) {
    bars.clear();
    return;
  }
  if (bars.size() == n && !bars.empty() &&
      bars.front().ts == cs.v.front().ts && bars.back().ts == cs.v.back().ts)
    return;

  std::vector<HeatmapBar> next(n);
  size_t h = 0;
  for (size_t i = 0; i < n; ++i) {
    const double ts = cs.v[i].ts;
    next[i].ts = ts;
    while (h < bars.size() && bars[h].ts < ts) ++h;
    if (h < bars.size() && bars[h].ts == ts) next[i] = std::move(bars[h++]);
  }
  bars.swap(next);
}

static void holdCell(float& cell, float v) {
  if (v == 0) return;
  bool sameSide = cell == 0 || ((v > 0) == (cell > 0));
  if (!sameSide || std::fabs(v) > std::fabs(cell)) cell = v;
}

static void finishBar(HeatmapBar& b) {
  if (b.rows.empty()) {
    b.heat.clear();
    b.row0 = 0;
    return;
  }
  b.row0 = b.rows.front();
}

template <typename... Extra>
static void keepClosest(int64_t midRow, int cap, std::vector<int64_t>& rows,
                        Extra&... extra) {
  if ((int)rows.size() <= cap) return;
  std::vector<size_t> idx(rows.size());
  for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
  std::nth_element(idx.begin(), idx.begin() + cap, idx.end(),
                   [&](size_t a, size_t b) {
                     int64_t da = rows[a] > midRow ? rows[a] - midRow
                                                   : midRow - rows[a];
                     int64_t db = rows[b] > midRow ? rows[b] - midRow
                                                   : midRow - rows[b];
                     if (da != db) return da < db;
                     return a < b;
                   });
  idx.resize((size_t)cap);
  std::sort(idx.begin(), idx.end(),
            [&](size_t a, size_t b) { return rows[a] < rows[b]; });
  auto compact = [&](auto& vec) {
    using T = typename std::decay<decltype(vec[0])>::type;
    std::vector<T> next;
    next.reserve(idx.size());
    for (size_t i : idx) next.push_back(vec[i]);
    vec.swap(next);
  };
  compact(rows);
  (compact(extra), ...);
}

static void mergeHold(std::vector<int64_t>& rows, std::vector<float>& heat,
                      const std::vector<int64_t>& fRows,
                      const std::vector<float>& fHeat) {
  std::vector<int64_t> outR;
  std::vector<float> outH;
  outR.reserve(rows.size() + fRows.size());
  outH.reserve(rows.size() + fRows.size());
  size_t i = 0, j = 0;
  while (i < rows.size() || j < fRows.size()) {
    if (j == fRows.size() || (i < rows.size() && rows[i] < fRows[j])) {
      outR.push_back(rows[i]);
      outH.push_back(heat[i]);
      ++i;
    } else if (i == rows.size() || fRows[j] < rows[i]) {
      outR.push_back(fRows[j]);
      outH.push_back(fHeat[j]);
      ++j;
    } else {
      float v = heat[i];
      holdCell(v, fHeat[j]);
      outR.push_back(rows[i]);
      outH.push_back(v);
      ++i;
      ++j;
    }
  }
  rows.swap(outR);
  heat.swap(outH);
}

void HeatmapSeries::setBin(double newBin) {
  if (!(newBin > 0) || !std::isfinite(newBin)) return;
  const double oldBin = bin;
  if (bin > 0) {
    // Ignore sub-5% changes: nice-step candidates sit ≥25% apart, so anything
    // smaller is boundary float noise, not a real change.
    if (std::fabs(newBin - bin) < bin * 0.05) return;
    for (HeatmapBar& b : bars) {
      const size_t n = b.rows.size();
      if (n < 1) {
        b.heat.clear();
        b.row0 = 0;
        continue;
      }
      std::vector<int64_t> nr;
      std::vector<float> nh;
      nr.reserve(n);
      nh.reserve(n);
      for (size_t i = 0; i < n; ++i) {
        float v = b.heat[i];
        if (v == 0) continue;
        int64_t row = (int64_t)std::floor(((double)b.rows[i] * oldBin) / newBin);
        if (!nr.empty() && nr.back() == row) holdCell(nh.back(), v);
        else {
          nr.push_back(row);
          nh.push_back(v);
        }
      }
      b.rows.swap(nr);
      b.heat.swap(nh);
      finishBar(b);
      b.mut = ++mutGen;
    }
  }
  bin = newBin;
  // A different bin is a different cell population, but zeroing the AUTO
  // reference would white out the overlay until the next book mutation
  // (strength saturates against the ref=1 fallback). Rescale by the bin
  // ratio instead — uniform depth per row scales with bin width — and let
  // the EMA's 0.25×/4× snap absorb the wall-dominated residue on the next
  // sample.
  if (ref > 0 && oldBin > 0) ref *= (float)(newBin / oldBin);
}

static double scanNativeTick(const Feeds& feeds, uint32_t mask) {
  static thread_local std::vector<double> gaps;
  gaps.clear();
  bool healthy[64] = {};
  feeds.collectHealthy(50.0, healthy, std::size(healthy));
  for (size_t i = 0; i < feeds.venues.size() && i < 32; ++i) {
    if (!(mask & (1u << (uint32_t)i))) continue;
    const VenueState& v = feeds.venues[i];
    if (!v.enabled) continue;
    if (!healthy[i] && v.status != wire::Live) continue;
    auto add = [&](const BookSide& side, bool fromBack) {
      size_t n = side.prices.size();
      if (n < 2) return;
      size_t start = fromBack && n > 48 ? n - 48 : 0;
      size_t end = fromBack ? n : std::min(n, (size_t)48);
      for (size_t k = start + 1; k < end; ++k) {
        double d = side.prices[k] - side.prices[k - 1];
        if (d > 1e-12 && std::isfinite(d)) gaps.push_back(d);
      }
    };
    add(v.book.bids, true);
    add(v.book.asks, false);
  }
  if (gaps.empty()) return 0;
  std::sort(gaps.begin(), gaps.end());
  // Median gap: the AUTO bin should approximate typical aggregated level
  // spacing. Flooring at the finest venue's tick instead subdivides most
  // levels into empty sub-rows — the hairline-stripe look.
  return gaps[gaps.size() / 2];
}

double HeatmapSeries::nativeTickFor(const Feeds& feeds, uint32_t mask) {
  const uint64_t version = feeds.booksVersion();
  if (version != tickBooks) {
    tickBooks = version;
    nativeTick = scanNativeTick(feeds, mask);
  }
  return nativeTick;
}

void HeatmapSeries::sample(int bar, const Feeds& feeds, uint32_t mask,
                           double mid) {
  if (bar < 0 || !(mid > 0) || !std::isfinite(mid) || !(bin > 0)) return;
  if ((size_t)bar >= bars.size()) bars.resize((size_t)bar + 1);
  HeatmapBar& dest = bars[(size_t)bar];

  static thread_local std::vector<int64_t> freshRows;
  static thread_local std::vector<float> freshBid, freshAsk, freshHeat, nearVals;
  captureHeatmapSides(feeds, mask, bin, freshRows, freshBid, freshAsk);

  freshHeat.resize(freshRows.size());
  for (size_t i = 0; i < freshRows.size(); ++i) {
    float b = freshBid[i], a = freshAsk[i];
    freshHeat[i] = b >= a ? b : -a;
  }

  mergeHold(dest.rows, dest.heat, freshRows, freshHeat);
  keepClosest((int64_t)std::floor(mid / bin), kMaxRows, dest.rows, dest.heat);
  finishBar(dest);
  dest.mut = ++mutGen;

  const double nearHalf = mid * kNearFrac;
  const int64_t n0 = (int64_t)std::floor((mid - nearHalf) / bin);
  const int64_t n1 = (int64_t)std::ceil((mid + nearHalf) / bin);
  nearVals.clear();
  for (size_t i = 0; i < freshRows.size(); ++i) {
    if (freshRows[i] < n0 || freshRows[i] >= n1) continue;
    float v = std::max(freshBid[i], freshAsk[i]);
    if (v > 0) nearVals.push_back(v);
  }
  float fresh = heatmapPercentileRef(nearVals.data(), (int)nearVals.size());
  if (!(fresh > 0)) {
    nearVals.clear();
    for (size_t i = 0; i < freshRows.size(); ++i) {
      float v = std::max(freshBid[i], freshAsk[i]);
      if (v > 0) nearVals.push_back(v);
    }
    fresh = heatmapPercentileRef(nearVals.data(), (int)nearVals.size());
  }
  ref = heatmapSmoothRef(ref, fresh);
}

void captureHeatmapSides(const Feeds& feeds, uint32_t mask, double bin,
                         std::vector<int64_t>& rows, std::vector<float>& bid,
                         std::vector<float>& ask) {
  rows.clear();
  bid.clear();
  ask.clear();
  if (!(bin > 0)) return;
  const double mid = feeds.aggMid();
  const double fenceLo = mid > 0 ? mid / HeatmapSeries::kFence : 0;
  const double fenceHi = mid > 0 ? mid * HeatmapSeries::kFence : 0;

  struct Acc {
    int64_t row;
    float bid;
    float ask;
  };
  static thread_local std::vector<Acc> acc;
  acc.clear();
  acc.reserve(8192);

  bool healthy[64] = {};
  feeds.collectHealthy(50.0, healthy, std::size(healthy));
  // mask is a uint32_t, so bits at index >= 32 can never be set; bounding the
  // loop keeps `1u << i` well-defined regardless of how many venues exist.
  for (size_t i = 0; i < feeds.venues.size() && i < 32; ++i) {
    if (!(mask & (1u << (uint32_t)i))) continue;
    const VenueState& venue = feeds.venues[i];
    if (!venue.enabled) continue;
    // Prefer healthy books, but still take a live book if the 50bps filter
    // has not marked anyone yet (startup) so the overlay is not empty.
    if (!healthy[i] && venue.status != wire::Live) continue;
    auto add = [&](const BookSide& side, bool isBid) {
      if (side.prices.empty()) return;
      for (size_t li = 0; li < side.prices.size(); ++li) {
        double price = side.prices[li];
        if (fenceLo > 0 && (price < fenceLo || price > fenceHi)) continue;
        // Crossed quotes (a venue's ask below the aggregate mid, or a bid
        // above it) are transient arb, not resting depth — they would paint
        // the wrong side's color across the spread boundary.
        if (mid > 0 && (isBid ? price > mid : price < mid)) continue;
        double size = side.sizes[li];
        if (!(price > 0) || !(size > 0)) continue;
        int64_t row = (int64_t)std::floor(price / bin);
        double usd = price * size;
        if (!std::isfinite(usd) || usd <= 0) continue;
        acc.push_back(
            {row, isBid ? (float)usd : 0.0f, isBid ? 0.0f : (float)usd});
      }
    };
    add(venue.book.bids, true);
    add(venue.book.asks, false);
  }
  if (acc.empty()) return;
  std::sort(acc.begin(), acc.end(),
            [](const Acc& a, const Acc& b) { return a.row < b.row; });
  rows.reserve(acc.size());
  bid.reserve(acc.size());
  ask.reserve(acc.size());
  for (const Acc& a : acc) {
    if (!rows.empty() && rows.back() == a.row) {
      bid.back() += a.bid;
      ask.back() += a.ask;
    } else {
      rows.push_back(a.row);
      bid.push_back(a.bid);
      ask.push_back(a.ask);
    }
  }
  if ((int)rows.size() > HeatmapSeries::kMaxRows) {
    const int64_t midRow = mid > 0 ? (int64_t)std::floor(mid / bin) : 0;
    keepClosest(midRow, HeatmapSeries::kMaxRows, rows, bid, ask);
  }
}

void heatmapFillTickHoles(const std::vector<int64_t>& rows,
                          const std::vector<float>& heat, int maxGap,
                          std::vector<int64_t>& outRows,
                          std::vector<float>& outHeat) {
  outRows.clear();
  outHeat.clear();
  if (rows.empty() || heat.size() != rows.size()) return;
  outRows.reserve(rows.size());
  outHeat.reserve(heat.size());
  outRows.push_back(rows[0]);
  outHeat.push_back(heat[0]);
  for (size_t i = 1; i < rows.size(); ++i) {
    const int64_t gap = rows[i] - rows[i - 1] - 1;
    const float a = heat[i - 1], b = heat[i];
    const bool same = (a > 0 && b > 0) || (a < 0 && b < 0);
    if (same && gap > 0 && gap <= (int64_t)maxGap) {
      const float fill = a > 0 ? std::min(a, b) : std::max(a, b);
      for (int64_t r = rows[i - 1] + 1; r < rows[i]; ++r) {
        outRows.push_back(r);
        outHeat.push_back(fill);
      }
    }
    outRows.push_back(rows[i]);
    outHeat.push_back(heat[i]);
  }
}

float heatmapStrength(float size, float ref) {
  if (!(size > 0) || !(ref > 0)) return 0;
  // Piecewise log2 ramp. Below the reference, eight octaves of visible
  // texture (ref/256 → 0) so ordinary depth reads as a continuous faint
  // field instead of cutting to black; above it, eight octaves to white-hot
  // (256×ref → 1.5). The reference itself (near-book P95) sits at 0.50 — low
  // enough that ordinary depth stays dim and aggregated walls stay distinct.
  const float l = std::log2(size / ref);
  float v = l < 0 ? 0.50f + l * (0.50f / 8.0f)
                  : 0.50f + l * (1.00f / 8.0f);
  return std::clamp(v, 0.0f, 1.5f);
}

float heatmapStrengthCeil(float size, float ceil) {
  if (!(size > 0) || !(ceil > 0)) return 0;
  const float l = std::log2(size / ceil);
  return std::clamp(1.5f + l * (1.5f / 8.0f), 0.0f, 1.5f);
}

float heatmapSmoothRef(float previous, float sample) {
  if (!(sample > 0) || !std::isfinite(sample)) return previous;
  if (!(previous > 0) || !std::isfinite(previous)) return sample;
  if (sample < previous * 0.25f || sample > previous * 4.0f) return sample;
  float rate = sample > previous ? 0.35f : 0.12f;
  return previous + (sample - previous) * rate;
}

float heatmapPercentileRef(const float* values, int n) {
  if (!values || n < 1) return 0;
  static thread_local std::vector<float> tmp;
  tmp.assign(values, values + n);
  std::sort(tmp.begin(), tmp.end());
  size_t i = std::min(tmp.size() - 1, (size_t)(tmp.size() * 0.95));
  return tmp[i];
}

float heatmapPercentileRef(const float* bid, const float* ask, int r0, int r1) {
  static thread_local std::vector<float> values;
  values.clear();
  values.reserve((size_t)std::max(0, r1 - r0));
  for (int r = r0; r < r1; ++r) {
    float v = std::max(bid[r], ask[r]);
    if (v > 0) values.push_back(v);
  }
  return heatmapPercentileRef(values.data(), (int)values.size());
}