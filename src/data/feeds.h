#pragma once

#include "book.h"
#include "candles.h"
#include "ht_map.h"
#include "market.h"
#include "orderflow.h"
#include "tape.h"
#include "wire.h"

#include <string>
#include <vector>

// Venue registry + per-venue books + global tape. Drains the bridge ring once
// per frame and applies events. Venue indices are a wire-protocol contract
// with feeds/registry.ts — keep the order in sync.
// Venue class flags (orderbook filter chips)
enum VenueClass : uint8_t { ClassSpot = 1, ClassPerp = 2, ClassDex = 4 };

// Number of venues (wire-protocol contract with feeds/registry.ts — see
// kVenues in feeds.cpp). A per-venue flow selection is a bitmask over these
// indices: bit i set = venue i's live prints feed the aggregated flow.
static constexpr int kVenueCount = 27;
static constexpr uint32_t kAllVenuesMask = (1u << kVenueCount) - 1;

// Per-venue bitmask for every venue whose class intersects `cls` — translates
// the SPOT/PERP/DEX quick toggles into concrete venue bits.
uint32_t venueMaskForClass(uint8_t cls);

struct VenueState {
  std::string id;
  std::string label;
  std::string shortLabel; // tape tag
  uint8_t cls = ClassSpot;
  bool enabled = true;
  uint8_t status = wire::StatusNone; // wire::Status*
  L2Book book;
  double bookUpdatedAtMs = 0; // worker receive time of the latest complete depth mutation
};

struct Feeds {
  std::vector<VenueState> venues;
  Tape tape;
  CandleSeries candles; // chart series (venue 0 klines + live trades)
  OrderFlowSeries orderFlow; // exact prints for footprint chart modes
  MarketSeries market;  // aggregated OI / funding / liquidations
  HtMaps ht;            // HyperTracker HL liq / stop maps (optional overlay)
  int symbol = 0; // 0 = ETH, 1 = BTC, 2 = SOL (feeds/registry.ts SYMBOLS)
  // Venues (bit i = venue i) whose live prints feed the aggregated volume,
  // CVD delta, and footprint orderflow series. Default = every venue.
  uint32_t flowMask = kAllVenuesMask;

  void init();               // registers venues + spawns the worker bridge
  int frame();               // drain + apply pending events; returns events applied
  void requestResync(int venue);

  void setSymbol(int sym);                 // switches the whole feed set
  void setVenueEnabled(int i, bool on);
  void setTimeframe(Timeframe tf);         // refetch/rebootstrap candle history
  void requestOrderFlow();                 // lazy exact trade history for footprints
  void refreshOrderFlow();                 // re-bootstrap even if already requested
  void setFlowMask(uint32_t mask);         // venues feeding volume/CVD/footprint
  // HyperTracker overlay: token + which live profiles to keep warm. No-ops
  // when unchanged. Empty token still notifies the worker so the overlay can
  // show the missing-key state without spending quota.
  void setHt(const char* token, bool liq, bool sl);

  VenueState* venue(size_t i) { return i < venues.size() ? &venues[i] : nullptr; }
  int liveCount() const;

  // Sum of book versions over enabled venues — cheap merge dirty-check.
  uint64_t booksVersion() const;

  // Aggregate mid = MEDIAN of per-venue mids — robust to a single venue with a
  // corrupt book (one garbage book must never poison the aggregate view).
  double aggMid() const;

  // Venues whose own mid is within `tolBps` of the aggregate mid. `cap` bounds
  // the output array (callers pass std::size(healthy)); entries beyond cap are
  // not written.
  int collectHealthy(double tolBps, bool* out, size_t cap) const;

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
  bool m_orderFlowRequested = false;
  std::string m_htToken;
  bool m_htLiq = false;
  bool m_htSl = false;
  int m_htSym = -1;
  // snapshot assembly scratch
  bool m_collecting = false;
  uint8_t m_collectVenue = 0;
  std::vector<double> m_snapBidP, m_snapBidS, m_snapAskP, m_snapAskS;
};
