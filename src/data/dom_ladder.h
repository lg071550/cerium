#pragma once

#include "feeds.h"

#include <cstdint>
#include <array>
#include <queue>
#include <unordered_map>
#include <vector>

struct DomBucket {
  int64_t tick = 0;
  double bid = 0, ask = 0;
  double bidCum = 0, askCum = 0;
  double bidCumUsd = 0, askCumUsd = 0;
  uint16_t bidVenues = 0, askVenues = 0;
  int16_t topBidVenue = -1, topAskVenue = -1;
  double topBidSize = 0, topAskSize = 0;
};

struct DomFlow {
  double buy = 0, sell = 0;
};

struct DomChange {
  double bid = 0, ask = 0;
  double bidAt = -1e9, askAt = -1e9;
};

struct DomSummary {
  double bestBid = 0, bestAsk = 0, mid = 0, microprice = 0;
  double bidTop5 = 0, askTop5 = 0, imbalance = 0;
  int sourceCount = 0;
  bool crossed = false;
};

struct DomContribution {
  int venue = -1;
  double bid = 0, ask = 0;
};

// One inferred residual at a price. Reconstructed per venue from L2 size
// deltas (not exchange MBO). bornAt == 0 means the block was seeded from a
// snapshot and its age is unknown.
struct DomResidual {
  int venue = -1;
  double size = 0;
  double bornAt = 0;
};

// Cached, UI-independent DOM projection over the live L2 books. venue=-1 is
// consolidated mode; otherwise only that venue contributes. Consolidated mode
// respects the per-venue mask (bit i = venue i) and the same 50bps health
// filter as Orderbook.
class DomModel {
public:
  // bandLo/bandHi: normalized ticks the panel currently displays. Scans
  // are bounded to that band (plus mid-near and margin) — display stays
  // per-frame, only off-screen computation is skipped.
  void rebuild(const Feeds& feeds, int venue, uint32_t mask, double step,
               double nowSeconds, double nowMs, int64_t bandLo,
               int64_t bandHi);
  bool bandCovers(int64_t lo, int64_t hi) const {
    return m_bandValid && lo >= m_bandLo && hi <= m_bandHi;
  }
  void updateTrades(const Feeds& feeds, int venue, uint32_t mask, double step,
                    double windowSeconds, double nowMs);

  const DomBucket* bucket(int64_t tick) const;
  DomFlow flow(int64_t tick) const;
  DomChange change(int64_t tick) const;
  void contributions(const Feeds& feeds, int venue, uint32_t mask,
                     int64_t tick, std::vector<DomContribution>& out) const;
  void residuals(int64_t tick, bool ask, std::vector<DomResidual>& out) const;

  static double inferTick(const Feeds& feeds, int venue, uint32_t mask,
                          double nowMs);
  static double cleanStep(double step);
  static double niceFallbackStep(double mid);
  static uint64_t sourceSignature(const Feeds& feeds, int venue, double nowMs);

  std::vector<DomBucket> levels; // sparse, descending tick
  DomSummary summary;
  DomFlow recentFlow;
  double lastTradePrice = 0;
  double step = 0;
  uint64_t sourceVersion = ~0ull;

private:
  std::unordered_map<int64_t, size_t> m_index;
  std::unordered_map<int64_t, DomFlow> m_flow;
  std::unordered_map<int64_t, DomChange> m_changes;
  uint64_t m_flowRevision = ~0ull;
  uint64_t m_flowSourceSignature = ~0ull;
  uint64_t m_flowResetRevision = ~0ull;
  std::array<bool, 64> m_flowAllowed{};
  std::array<double, 64> m_flowScales{};
  // Retain raw venue/price totals so normalization and grouping changes only
  // revisit distinct prices, not every print in the five-minute window.
  struct RawFlow : DomFlow { size_t buyCount = 0, sellCount = 0; };
  std::array<std::unordered_map<double, RawFlow>, 64> m_rawFlow, m_rawHits;
  struct FlowPrint {
    double expires, price, qty;
    uint8_t venue;
    bool buy;
    bool operator<(const FlowPrint& other) const { return expires > other.expires; }
  };
  std::priority_queue<FlowPrint> m_flowExpiry, m_hitExpiry;
  double m_hitClockOffset = 0, m_lastHitOrigin = 0;
  double m_futureRetry = INFINITY;
  int m_flowVenue = -2;
  uint32_t m_flowMask = 0;
  double m_flowStep = 0, m_flowWindow = 0, m_flowNextExpiry = -1;
  double m_sourceNowMs = 0;

  // Prints in the last ~4s, keyed with the same bid/ask tick rounding as
  // the ladder. Used to strip trade-consumed size out of pull flashes.
  std::unordered_map<int64_t, DomFlow> m_hits;
  double m_sweepBuy = 0, m_sweepSell = 0, m_hitNextExpiry = -1;

  void suppressExecutedPulls(double step);

  // Change events are tracked per venue in that venue's own raw price space
  // (levels bucketed by absolute tick, before mid normalization), then mapped
  // onto the normalized tick axis for display. Diffing raw space means a
  // venue's mid drifting (or the reference mid moving) re-buckets its ladder
  // without fabricating "pulled" liquidity — only actual order deltas fire.
  std::vector<std::unordered_map<int64_t, double>> m_prevBid, m_prevAsk;
  struct ResidualLevel {
    double repPrice = 0;
    std::vector<DomResidual> blocks;
  };
  std::vector<std::unordered_map<int64_t, ResidualLevel>> m_residBid, m_residAsk;
  std::unordered_map<int64_t, std::vector<DomResidual>> m_dispBid, m_dispAsk;
  int m_changeVenue = -2;
  uint32_t m_changeMask = 0;
  double m_changeStep = 0;
  int64_t m_bandLo = 0, m_bandHi = 0;
  bool m_bandValid = false;
  bool m_changeSeeded = false;
};
