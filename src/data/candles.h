#pragma once

#include <vector>

// Candle series for the chart panel: seeded from Binance klines (via the
// feeds worker), then extended/updated live from the trade stream.
// `delta` = taker-buy volume minus taker-sell volume in the bucket (CVD feed).
struct Candle {
  double ts; // bucket open time, ms
  double o, h, l, c;
  double vol;
  double delta;
};

struct CandleSeries {
  std::vector<Candle> v;
  int intervalMin = 1;
  int sym = -1; // canonical symbol index these candles belong to

  static constexpr size_t MAX_CANDLES = 1000;

  void load(const double* data, int n, int interval, int symIdx);
  void onTrade(double price, double qty, int side, double tsMs); // side: 0=buy 1=sell
};
