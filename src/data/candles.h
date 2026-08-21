#pragma once

#include <cstdint>
#include <vector>

// Chart timeframe: time-based bars (value = minutes), tick bars (value =
// trades/bar), or volume bars (value = base-asset volume/bar). Time history
// comes from Binance klines (custom minutes are aggregated in the worker);
// tick/volume history is bootstrapped from aggTrades by the worker. Live bars
// form from the venue-0 trade stream either way.
struct Timeframe {
  enum Kind : uint8_t { Time = 0, Tick = 1, Volume = 2 };
  Kind kind = Time;
  double value = 1; // minutes | trades | base volume

  bool operator==(const Timeframe& o) const = default;
};

// Candle series for the chart panel: OHLC is seeded from Binance (via the
// feeds worker) and extended live from the venue-0 trade stream only. `vol`
// stays the Binance reference volume (also sizes volume bars). `delta`
// (taker-buy − taker-sell) and `aggVol` (total traded volume) are seeded from
// Binance history but, live, aggregate every venue matching Feeds::flowMask
// (see onAgg) — so the CVD and volume indicators are market-wide /
// class-filterable rather than Binance-only.
struct Candle {
  double ts; // bucket open time, ms (tick/volume bars: first trade's time)
  double o, h, l, c;
  double vol;   // Binance reference volume (OHLCV + volume-bar sizing)
  double delta; // aggregated taker-buy − taker-sell volume (CVD)
  double aggVol; // aggregated total traded volume (mask-filtered)
};

struct CandleSeries {
  std::vector<Candle> v;
  Timeframe tf;
  int sym = -1;    // canonical symbol index these candles belong to
  int barCount = 0; // trades seen in the forming bar (tick bars only)

  static constexpr size_t MAX_CANDLES = 2000;

  void load(const double* data, int n, Timeframe tf_, int symIdx);
  // side: 0=buy 1=sell. onTrade is the venue-0 (Binance) clock + OHLC
  // reference for time/tick bars; volume bars are aggregated and come from
  // onAgg (see below).
  void onTrade(double price, double qty, int side, double tsMs);
  // Fold a mask-filtered print into the forming bar. For time/tick bars it
  // only accumulates aggVol/delta; for volume bars it owns the bar boundary
  // (closes on aggregated volume) and the aggregated OHLC.
  void onAgg(double price, double qty, int side, double tsMs);

  double intervalMs() const { return tf.value * 60000.0; } // Time only
  // forming-bar progress: trades (tick) / accumulated volume (volume)
  double barProgress() const {
    if (tf.kind == Timeframe::Tick) return barCount;
    if (tf.kind == Timeframe::Volume) return v.empty() ? 0.0 : v.back().aggVol;
    return 0;
  }
};
