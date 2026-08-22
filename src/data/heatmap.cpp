#include "heatmap.h"

#include "feeds.h"

#include <algorithm>
#include <cmath>
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

void HeatmapSeries::setBin(double newBin) {
  if (!(newBin > 0) || !std::isfinite(newBin)) return;
  const double oldBin = bin;
  if (bin > 0) {
    // Ignore sub-5% changes: nice-step candidates sit ≥25% apart, so anything
    // smaller is boundary float noise, not a real change.
    if (std::fabs(newBin - bin) < bin * 0.05) return;
    for (HeatmapBar& b : bars) {
      const int64_t n = (int64_t)b.heat.size();
      if (n < 1) continue;
      int64_t lo = INT64_MAX, hi = INT64_MIN;
      for (int64_t r = 0; r < n; ++r) {
        if (b.heat[(size_t)r] == 0) continue;
        int64_t nr = (int64_t)std::floor(((double)(b.row0 + r) * oldBin) /
                                         newBin);
        lo = std::min(lo, nr);
        hi = std::max(hi, nr);
      }
      if (lo > hi) {
        b.heat.clear();
        continue;
      }
      std::vector<float> next((size_t)(hi - lo + 1), 0.0f);
      for (int64_t r = 0; r < n; ++r) {
        float v = b.heat[(size_t)r];
        if (v == 0) continue;
        int64_t nr = (int64_t)std::floor(((double)(b.row0 + r) * oldBin) /
                                         newBin);
        holdCell(next[(size_t)(nr - lo)], v);
      }
      b.heat.swap(next);
      b.row0 = lo;
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

  // Capture band: mid ±50%, capped so the row count stays bounded at very
  // fine bins. Wide enough that panning back to earlier price action still
  // finds the heat that was sampled while this bar was live.
  double half = mid * 0.5;
  const double capHalf = bin * (double)(kMaxRows / 2);
  if (half > capHalf) half = capHalf;
  const int64_t row0 = (int64_t)std::floor((mid - half) / bin);
  const int rows =
      (int)std::min<int64_t>(kMaxRows,
                             (int64_t)std::ceil((mid + half) / bin) - row0);
  if (rows < 1) return;

  static thread_local std::vector<float> freshBid, freshAsk;
  freshBid.assign((size_t)rows, 0.0f);
  freshAsk.assign((size_t)rows, 0.0f);
  captureHeatmapSides(feeds, mask, bin, row0, rows, freshBid.data(),
                      freshAsk.data());

  // Fold the fresh sample into the bar with max-hold, growing the bar's
  // populated range to the union if the band moved since the last sample.
  if (dest.heat.empty()) {
    dest.row0 = row0;
    dest.heat.assign((size_t)rows, 0.0f);
  } else {
    const int64_t destHi = dest.row0 + (int64_t)dest.heat.size();
    const int64_t u0 = std::min(dest.row0, row0);
    const int64_t u1 = std::max(destHi, row0 + (int64_t)rows);
    if (u0 < dest.row0 || u1 > destHi) {
      std::vector<float> next((size_t)(u1 - u0), 0.0f);
      for (int64_t r = 0; r < (int64_t)dest.heat.size(); ++r)
        next[(size_t)(dest.row0 + r - u0)] = dest.heat[(size_t)r];
      dest.heat.swap(next);
      dest.row0 = u0;
    }
  }
  const int64_t off = row0 - dest.row0;
  for (int r = 0; r < rows; ++r) {
    float b = freshBid[(size_t)r], a = freshAsk[(size_t)r];
    float v = b >= a ? b : -a;
    holdCell(dest.heat[(size_t)(off + r)], v);
  }

  const double nearHalf = mid * kNearFrac;
  const int r0 = std::clamp(
      (int)((int64_t)std::floor((mid - nearHalf) / bin) - row0), 0, rows);
  const int r1 = std::clamp(
      (int)((int64_t)std::ceil((mid + nearHalf) / bin) - row0), 0, rows);
  float fresh = heatmapPercentileRef(freshBid.data(), freshAsk.data(), r0, r1);
  if (!(fresh > 0))
    fresh = heatmapPercentileRef(freshBid.data(), freshAsk.data(), 0, rows);
  ref = heatmapSmoothRef(ref, fresh);
}

void captureHeatmapSides(const Feeds& feeds, uint32_t mask, double bin,
                         int64_t row0, int rows, float* bid, float* ask) {
  if (!bid || !ask || rows < 1 || !(bin > 0)) return;
  std::fill(bid, bid + rows, 0.0f);
  std::fill(ask, ask + rows, 0.0f);
  const double lo = (double)row0 * bin;
  const double hi = (double)(row0 + rows) * bin;
  const double mid = feeds.aggMid();
  const double fenceLo = mid > 0 ? mid / HeatmapSeries::kFence : 0;
  const double fenceHi = mid > 0 ? mid * HeatmapSeries::kFence : 0;

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
    auto add = [&](const BookSide& side, float* dest, bool isBid) {
      if (side.prices.empty()) return;
      size_t start = side.lowerBound(lo);
      for (size_t li = start; li < side.prices.size(); ++li) {
        double price = side.prices[li];
        if (price >= hi) break;
        if (fenceLo > 0 && (price < fenceLo || price > fenceHi)) continue;
        // Crossed quotes (a venue's ask below the aggregate mid, or a bid
        // above it) are transient arb, not resting depth — they would paint
        // the wrong side's color across the spread boundary.
        if (mid > 0 && (isBid ? price > mid : price < mid)) continue;
        double size = side.sizes[li];
        if (!(price > 0) || !(size > 0)) continue;
        int64_t row = (int64_t)std::floor(price / bin) - row0;
        if (row < 0 || row >= rows) continue;
        double usd = price * size;
        if (std::isfinite(usd) && usd > 0) dest[row] += (float)usd;
      }
    };
    add(venue.book.bids, bid, true);
    add(venue.book.asks, ask, false);
  }
}

float heatmapStrength(float size, float ref) {
  if (!(size > 0) || !(ref > 0)) return 0;
  // Piecewise log2 ramp. Below the reference, eight octaves of visible
  // texture (ref/256 → 0) so ordinary depth reads as a continuous faint
  // field instead of cutting to black; above it, five octaves to white-hot
  // (32×ref → 1.5). The reference itself (near-book P95) sits at 0.50 — low
  // enough that ordinary depth stays dim and real walls stand out.
  const float l = std::log2(size / ref);
  float v = l < 0 ? 0.50f + l * (0.50f / 8.0f)
                  : 0.50f + l * (1.00f / 5.0f);
  return std::clamp(v, 0.0f, 1.5f);
}

float heatmapSmoothRef(float previous, float sample) {
  if (!(sample > 0) || !std::isfinite(sample)) return previous;
  if (!(previous > 0) || !std::isfinite(previous)) return sample;
  if (sample < previous * 0.25f || sample > previous * 4.0f) return sample;
  float rate = sample > previous ? 0.35f : 0.12f;
  return previous + (sample - previous) * rate;
}

float heatmapPercentileRef(const float* bid, const float* ask, int r0, int r1) {
  static thread_local std::vector<float> values;
  values.clear();
  values.reserve((size_t)std::max(0, r1 - r0));
  for (int r = r0; r < r1; ++r) {
    float v = std::max(bid[r], ask[r]);
    if (v > 0) values.push_back(v);
  }
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  size_t i = std::min(values.size() - 1, (size_t)(values.size() * 0.95));
  return values[i];
}
