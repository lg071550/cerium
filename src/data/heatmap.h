#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

struct Feeds;
struct CandleSeries;

// One candle's aggregated resting-book heat on the shared price lattice. Row
// `row` covers [row*bin, (row+1)*bin) — the lattice origin is 0 for every
// column, so rows line up across the whole series and never move when the
// chart pans or zooms. Cells are signed USD notional: positive = resting
// bids, negative = resting asks — the TradingLite-style two-tone split
// happens naturally at the spread. The live bar max-holds samples until the
// candle closes, so a wall that rested at any point during the bar stays lit
// for that column; when mid crosses a row, the fresh sample's side takes it
// over. `rows` / `heat` are parallel sparse arrays of occupied lattice rows
// only (strictly increasing); empty air between far walls is not stored.
struct HeatmapBar {
  double ts = 0;    // candle open time this column belongs to
  int64_t row0 = 0; // first occupied lattice row (0 if empty)
  std::vector<int64_t> rows; // occupied lattice rows, sorted
  std::vector<float> heat;   // signed USD notional, parallel to rows
  // Draw-side caches key on (ts, mut): mut is drawn from the series' mutGen
  // counter, which never resets, so a cleared-and-resampled series can never
  // collide with a stale cached fill for the same candle open time.
  uint64_t mut = 0;
};

// Copy `src` onto `out`, dropping any lattice row whose [row*bin, (row+1)*bin)
// overlaps [lo, hi]. That is the candle strike-through: those rows become
// empty air in this column. `hi == lo` still punches the bin that contains it.
inline void heatmapCopyPunch(const HeatmapBar& src, HeatmapBar& out, double lo,
                             double hi, double bin) {
  out.rows.clear();
  out.heat.clear();
  if (!(bin > 0) || src.rows.empty()) {
    out.row0 = 0;
    return;
  }
  out.rows.reserve(src.rows.size());
  out.heat.reserve(src.heat.size());
  const bool sweep = std::isfinite(lo) && std::isfinite(hi) && hi >= lo;
  for (size_t i = 0; i < src.rows.size(); ++i) {
    const int64_t r = src.rows[i];
    if (sweep) {
      const double a = (double)r * bin;
      const double b = a + bin;
      // Lattice rows are half-open [a, b). Touching the next row's edge
      // (candle.low == b) does not count as a trade-through.
      if (a <= hi && b > lo) continue;
    }
    out.rows.push_back(r);
    out.heat.push_back(src.heat[i]);
  }
  out.row0 = out.rows.empty() ? 0 : out.rows.front();
}

// Liquidity history on a stable price lattice. Every column shares `bin`;
// the grid only changes when the bin size itself changes (manual pick, or an
// AUTO regime shift — AUTO is derived from the books and mid only, never
// from the viewport, so pan/zoom/axis interactions never re-grid history).
// Sampling captures occupied book levels inside the fence (mid/100..mid*100),
// capped at kMaxRows populated rows closest to mid — far walls stay in the
// column without allocating the empty air between them.
struct HeatmapSeries {
  static constexpr int kMaxRows = 32768;   // per-bar populated-row cap
  static constexpr double kNearFrac = 0.02; // ref population = mid ±2%
  static constexpr double kFence = 100.0;   // ignore prices outside mid/100..mid*100
  static constexpr int kGapBins = 8;        // draw: fill same-side holes of at most this many lattice rows

  double bin = 0; // shared lattice step (0 = no grid yet)
  std::vector<HeatmapBar> bars; // index matches CandleSeries
  float ref = 0; // smoothed P95 cell notional — the AUTO heat normalization
  // Monotonic mutation counter stamped into every rewritten HeatmapBar::mut.
  // Never reset (not even by clear()) so (ts, mut) uniquely identifies column
  // contents for the draw-side tick-hole fill cache.
  uint64_t mutGen = 0;
  double nativeTick = 0; // cached book-tick floor for the AUTO bin
  uint64_t tickBooks = ~0ull; // books version nativeTick was computed at
  // Sampling is gated on the books version (any resting-book mutation
  // triggers the next sample).
  uint64_t sampledBooks = ~0ull;
  double sampledTs = -1; // candle open time of the last sampled bar (stall fill bound)
  int sym = -1;
  int tfKind = -1;
  double tfValue = 0;

  void clear();
  // Keep columns whose candle open time still exists; append empty slots for
  // new bars; drop the prefix when the candle series slides (MAX_CANDLES
  // trim). A new empty slot is what prompts the first sample of a fresh bar.
  void syncToCandles(const CandleSeries& cs);

  // Switch the lattice step. Legitimate bin changes (nice steps, manual
  // picks) are ≥25% apart, so changes under 5% are treated as float noise and
  // ignored — AUTO recomputes every frame and must not flap at step
  // boundaries. A real change max-projects every occupied cell onto the new
  // lattice (a held wall survives) and rescales the AUTO reference, since
  // the cell population changes.
  void setBin(double newBin);

  // Book-tick floor for the AUTO bin: median level gap across the mask's
  // healthy venues, so AUTO rows approximate typical aggregated level
  // spacing. Cached per books version — the gap scan + sort is too expensive
  // to run per frame.
  double nativeTickFor(const Feeds& feeds, uint32_t mask);

  // Samples the mask-filtered aggregated book into bar `bar` (the live
  // candle). Occupied lattice rows inside the fence are stored sparsely and
  // capped at kMaxRows closest to mid.
  void sample(int bar, const Feeds& feeds, uint32_t mask, double mid);
};

// Accumulates the mask-filtered merged book into sorted occupied lattice
// rows (cleared first). Parallel `bid` / `ask` are USD notional per row.
// Crossed quotes are skipped; prices outside the fence are ignored. If more
// than HeatmapSeries::kMaxRows rows populate, the closest to mid are kept.
void captureHeatmapSides(const Feeds& feeds, uint32_t mask, double bin,
                         std::vector<int64_t>& rows, std::vector<float>& bid,
                         std::vector<float>& ask);

// Inserts same-side holes of at most `maxGap` lattice rows, filled with the
// smaller (in magnitude, conservative) of the two bounding occupied cells.
// Wider gaps — the air between a far wall and the near book — stay empty.
void heatmapFillTickHoles(const std::vector<int64_t>& rows,
                          const std::vector<float>& heat, int maxGap,
                          std::vector<int64_t>& outRows,
                          std::vector<float>& outHeat);

// Heat strength for a cell notional against the reference. Piecewise log2:
// ref/256 fades to 0 (eight octaves of visible texture so ordinary depth
// stays a continuous field), the reference itself (near-book P95) lands at
// 0.50 — ordinary depth stays dim — and 256×ref reaches full white-hot, so
// aggregated walls ($10M–$200M against a ~$1M near-book) stay on the ramp
// instead of collapsing to the same white.
float heatmapStrength(float size, float ref);

// Manual MAX$: `ceil` is white-hot (1.5); eight octaves below fade to 0.
float heatmapStrengthCeil(float size, float ceil);

// Asymmetric EMA: rises quickly when liquidity appears, decays slowly when it
// drains, so the auto reference tracks regime changes without flickering.
float heatmapSmoothRef(float previous, float sample);

// P95 of the nonzero cells in a capture — robust auto reference (immune to the
// single huge wall that would own a max). Restrict to rows [r0, r1) so the
// caller can anchor the reference to the near-book instead of far junk walls.
float heatmapPercentileRef(const float* bid, const float* ask, int r0, int r1);

// P95 of an explicit value list (already filtered to the near book).
float heatmapPercentileRef(const float* values, int n);