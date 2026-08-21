#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct Feeds;
struct CandleSeries;

// One candle's aggregated resting-book heat on the shared price lattice. Row
// `row0 + r` covers [(row0+r)*bin, (row0+r+1)*bin) — the lattice origin is 0
// for every column, so rows line up across the whole series and never move
// when the chart pans or zooms. Cells are signed USD notional: positive =
// resting bids, negative = resting asks — the TradingLite-style two-tone
// split happens naturally at the spread. The live bar max-holds samples until
// the candle closes, so a wall that rested at any point during the bar stays
// lit for that column; when mid crosses a row, the fresh sample's side takes
// it over. `heat` is dense over populated rows only.
struct HeatmapBar {
  double ts = 0;    // candle open time this column belongs to
  int64_t row0 = 0; // lattice row of heat[0]
  std::vector<float> heat;
};

// Liquidity history on a stable price lattice. Every column shares `bin`;
// the grid only changes when the bin size itself changes (manual pick, or an
// AUTO regime shift — AUTO is derived from the books and mid only, never
// from the viewport, so pan/zoom/axis interactions never re-grid history).
// Sampling captures the book in a band around mid (mid ±50%, capped at
// kMaxRows) — enough that panning back to earlier price action still finds
// heat there.
struct HeatmapSeries {
  static constexpr int kMaxRows = 32768;   // per-bar populated-row cap
  static constexpr double kNearFrac = 0.02; // ref population = mid ±2%
  static constexpr double kFence = 100.0;   // ignore prices outside mid/100..mid*100

  double bin = 0; // shared lattice step (0 = no grid yet)
  std::vector<HeatmapBar> bars; // index matches CandleSeries
  float ref = 0; // smoothed P95 cell notional — the AUTO heat normalization
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
  // boundaries. A real change max-projects every column onto the new lattice
  // (a held wall survives) and rescales the AUTO reference, since the cell
  // population changes.
  void setBin(double newBin);

  // Book-tick floor for the AUTO bin: median level gap across the mask's
  // healthy venues, so AUTO rows approximate typical aggregated level
  // spacing. Cached per books version — the gap scan + sort is too expensive
  // to run per frame.
  double nativeTickFor(const Feeds& feeds, uint32_t mask);

  // Samples the mask-filtered aggregated book into bar `bar` (the live
  // candle) over mid ±50% (capped at kMaxRows) on the shared lattice.
  void sample(int bar, const Feeds& feeds, uint32_t mask, double mid);
};

// Accumulates the mask-filtered merged book over lattice rows
// [row0, row0+rows) into the side arrays (cleared first). Values are USD
// notional per price row.
void captureHeatmapSides(const Feeds& feeds, uint32_t mask, double bin,
                         int64_t row0, int rows, float* bid, float* ask);

// Heat strength for a cell notional against the reference. Piecewise log2:
// ref/256 fades to 0 (eight octaves of visible texture so ordinary depth
// stays a continuous field), the reference itself (near-book P95) lands at
// 0.50 — ordinary depth stays dim — and 32×ref reaches full white-hot, so
// walls stand out sharply from the field.
float heatmapStrength(float size, float ref);

// Asymmetric EMA: rises quickly when liquidity appears, decays slowly when it
// drains, so the auto reference tracks regime changes without flickering.
float heatmapSmoothRef(float previous, float sample);

// P95 of the nonzero cells in a capture — robust auto reference (immune to the
// single huge wall that would own a max). Restrict to rows [r0, r1) so the
// caller can anchor the reference to the near-book instead of far junk walls.
float heatmapPercentileRef(const float* bid, const float* ask, int r0, int r1);
