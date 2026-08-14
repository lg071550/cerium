#pragma once

#include "book.h"
#include "candles.h"
#include "tape.h"
#include "wire.h"

#include <string>
#include <vector>

// Venue registry + per-venue books + global tape. Drains the bridge ring once
// per frame and applies events. Venue indices are a wire-protocol contract
// with feeds/registry.ts — keep the order in sync.
// Venue class flags (orderbook filter chips)
enum VenueClass : uint8_t { ClassSpot = 1, ClassPerp = 2, ClassDex = 4 };

struct VenueState {
  std::string id;
  std::string label;
  std::string shortLabel; // tape tag
  uint8_t cls = ClassSpot;
  bool enabled = true;
  uint8_t status = wire::StatusNone; // wire::Status*
  L2Book book;
};

struct Feeds {
  std::vector<VenueState> venues;
  Tape tape;
  CandleSeries candles; // chart series (venue 0 klines + live trades)
  int symbol = 0; // 0 = ETH, 1 = BTC, 2 = SOL (feeds/registry.ts SYMBOLS)

  void init();               // registers venues + spawns the worker bridge
  int frame();               // drain + apply pending events; returns events applied
  void requestResync(int venue);

  void setSymbol(int sym);                 // switches the whole feed set
  void setVenueEnabled(int i, bool on);
  void setCandleInterval(int minutes);     // refetch klines at a new interval

  VenueState* venue(size_t i) { return i < venues.size() ? &venues[i] : nullptr; }
  int liveCount() const;

  // Sum of book versions over enabled venues — cheap merge dirty-check.
  uint64_t booksVersion() const;

  // Aggregate mid = MEDIAN of per-venue mids — robust to a single venue with a
  // corrupt book (one garbage book must never poison the aggregate view).
  double aggMid() const;

  // Venues whose own mid is within `tolBps` of the aggregate mid.
  int collectHealthy(double tolBps, bool* out) const;

  double aggBestBid() const;
  double aggBestAsk() const;

private:
  void apply(const wire::Event& e);

  // Aggregate cache: median mid + 50 bps health flags, refreshed only when the
  // books version sum changes so a frame's aggMid/collectHealthy/aggBestBid/
  // aggBestAsk calls share one mid-sorting pass instead of one per call.
  void refreshAggCache() const;
  mutable bool m_aggCacheValid = false;
  mutable uint64_t m_aggCacheVer = 0;
  mutable double m_aggMid = 0;
  mutable bool m_aggHealthy[64] = {};

  bool m_started = false;
  // snapshot assembly scratch
  bool m_collecting = false;
  uint8_t m_collectVenue = 0;
  std::vector<double> m_snapBidP, m_snapBidS, m_snapAskP, m_snapAskS;
};
