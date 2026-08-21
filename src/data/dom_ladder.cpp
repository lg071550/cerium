#include "dom_ladder.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <unordered_set>

namespace {
constexpr size_t kMaxSideBuckets = 4096;
constexpr double kConsolidatedStaleMs = 3000.0;
constexpr size_t kMaxResidualsPerLevel = 24;
constexpr size_t kMaxDisplayResiduals = 32;
// Match the DOM flash fade (~0.8s) so a late print still kills the pull
// before the amber ghost appears, and so it cannot reappear after the hit
// window expires while the fade is still running.
constexpr double kHitWindowMs = 4000.0;

void capResiduals(std::vector<DomResidual>& q, size_t cap) {
  while (q.size() > cap) {
    DomResidual extra = q.back();
    q.pop_back();
    if (q.empty()) {
      q.push_back(extra);
      break;
    }
    q.back().size += extra.size;
    if (q.back().bornAt > 0 && (extra.bornAt <= 0 || extra.bornAt < q.back().bornAt))
      q.back().bornAt = extra.bornAt;
  }
}

void applyResidualDelta(std::vector<DomResidual>& q, int venue, double newSize,
                        double oldSize, double now) {
  double delta = newSize - oldSize;
  if (delta > 1e-12) {
    q.push_back({venue, delta, now});
    capResiduals(q, kMaxResidualsPerLevel);
    return;
  }
  if (delta >= -1e-12) return;
  double need = -delta;
  size_t eaten = 0;
  while (need > 1e-12 && eaten < q.size()) {
    if (q[eaten].size <= need + 1e-12) {
      need -= q[eaten].size;
      ++eaten;
    } else {
      q[eaten].size -= need;
      need = 0;
    }
  }
  if (eaten > 0) q.erase(q.begin(), q.begin() + (std::ptrdiff_t)eaten);
}

void sortResiduals(std::vector<DomResidual>& q) {
  std::sort(q.begin(), q.end(), [](const DomResidual& a, const DomResidual& b) {
    bool au = a.bornAt <= 0, bu = b.bornAt <= 0;
    if (au != bu) return au;
    if (a.bornAt != b.bornAt) return a.bornAt < b.bornAt;
    return a.venue < b.venue;
  });
}

bool depthFresh(const VenueState& venue, double nowMs) {
  return venue.bookUpdatedAtMs <= 0 || nowMs <= 0 ||
         nowMs - venue.bookUpdatedAtMs <= kConsolidatedStaleMs;
}

bool sourceAllowed(const Feeds& feeds, int index, int selected, uint32_t mask,
                   const bool* healthy, double nowMs) {
  if (index < 0 || index >= (int)feeds.venues.size()) return false;
  const VenueState& venue = feeds.venues[(size_t)index];
  if (selected >= 0) return index == selected;
  return venue.enabled && venue.status == wire::Live && healthy[index] &&
         depthFresh(venue, nowMs) && (mask & (1u << index));
}

double priceScale(const Feeds& feeds, int venueIndex, int selected,
                  double referenceMid) {
  if (selected >= 0 || venueIndex < 0 || venueIndex >= (int)feeds.venues.size())
    return 1.0;
  double venueMid = feeds.venues[(size_t)venueIndex].book.mid();
  return venueMid > 0 && referenceMid > 0 ? referenceMid / venueMid : 1.0;
}

int64_t bidTick(double price, double step) {
  return (int64_t)std::floor(price / step + 1e-9);
}

int64_t askTick(double price, double step) {
  return (int64_t)std::ceil(price / step - 1e-9);
}

double inferBookTick(const L2Book& book) {
  std::unordered_map<int64_t, int> counts;
  auto collect = [&](const BookSide& side, bool nearestHigh) {
    if (side.prices.size() < 2) return;
    size_t n = std::min<size_t>(65, side.prices.size());
    size_t begin = nearestHigh ? side.prices.size() - n : 0;
    size_t end = nearestHigh ? side.prices.size() : n;
    for (size_t i = begin + 1; i < end; ++i) {
      double d = side.prices[i] - side.prices[i - 1];
      if (!(d > 1e-10) || !std::isfinite(d)) continue;
      int64_t key = (int64_t)std::llround(d * 1e8);
      if (key > 0) counts[key]++;
    }
  };
  collect(book.bids, true);
  collect(book.asks, false);
  int bestCount = 0;
  int64_t best = 0;
  for (const auto& [key, count] : counts) {
    if (count > bestCount || (count == bestCount && (best == 0 || key < best))) {
      bestCount = count;
      best = key;
    }
  }
  return best > 0 ? (double)best / 1e8 : 0.0;
}
} // namespace

double DomModel::cleanStep(double step) {
  if (!(step > 0) || !std::isfinite(step)) return 0.01;
  double magnitude = std::pow(10.0, std::floor(std::log10(step)));
  // Venue-mid normalization scales each venue's native tick by its mid ratio
  // to the reference, so the inferred step carries ratio residue that varies
  // with the live mid drift (a few bps is routine, up to the 50bps health
  // bound). Snap to 1% of the step's magnitude — that absorbs the full drift
  // range while keeping genuine exchange ticks intact (0.25, 1.25, 2.5 … are
  // exact multiples of magnitude * 1e-2).
  double quantum = magnitude * 1e-2;
  if (!(quantum > 0) || !std::isfinite(quantum)) return step;
  return std::max(quantum, std::round(step / quantum) * quantum);
}

double DomModel::niceFallbackStep(double mid) {
  if (!(mid > 0) || !std::isfinite(mid)) return 0.01;
  double raw = mid * 1e-4;
  double base = std::pow(10.0, std::floor(std::log10(raw)));
  double f = raw / base;
  double nice = f <= 1.0 ? 1.0 : f <= 2.0 ? 2.0 : f <= 2.5 ? 2.5 : f <= 5.0 ? 5.0 : 10.0;
  return cleanStep(nice * base);
}

uint64_t DomModel::sourceSignature(const Feeds& feeds, int selected, double nowMs) {
  uint64_t hash = 1469598103934665603ull;
  int first = selected >= 0 ? selected : 0;
  int end = selected >= 0 ? selected + 1 : (int)feeds.venues.size();
  for (int i = first; i < end; ++i) {
    if (i < 0 || i >= (int)feeds.venues.size()) continue;
    const VenueState& venue = feeds.venues[(size_t)i];
    hash ^= venue.book.version;
    hash *= 1099511628211ull;
    hash ^= (uint64_t)venue.status | ((uint64_t)venue.enabled << 8) |
            ((uint64_t)venue.cls << 16) | ((uint64_t)i << 24) |
            ((uint64_t)depthFresh(venue, nowMs) << 56);
    hash *= 1099511628211ull;
  }
  return hash;
}

double DomModel::inferTick(const Feeds& feeds, int selected, uint32_t mask,
                           double nowMs) {
  bool healthy[64]{};
  feeds.collectHealthy(50.0, healthy);
  double referenceMid = selected >= 0 && selected < (int)feeds.venues.size()
                            ? feeds.venues[(size_t)selected].book.mid()
                            : feeds.aggMid();
  std::vector<double> ticks;
  for (int i = 0; i < (int)feeds.venues.size() && i < 64; ++i) {
    if (!sourceAllowed(feeds, i, selected, mask, healthy, nowMs)) continue;
    double tick = inferBookTick(feeds.venues[(size_t)i].book) *
                  priceScale(feeds, i, selected, referenceMid);
    if (tick > 0) ticks.push_back(tick);
  }
  if (ticks.empty()) {
    double mid = selected >= 0 && selected < (int)feeds.venues.size()
                     ? feeds.venues[(size_t)selected].book.mid()
                     : feeds.aggMid();
    return niceFallbackStep(mid);
  }
  std::sort(ticks.begin(), ticks.end());
  return cleanStep(selected >= 0 ? ticks.front() : ticks[ticks.size() / 2]);
}

void DomModel::rebuild(const Feeds& feeds, int selected, uint32_t mask,
                       double newStep, double nowSeconds, double nowMs,
                       int64_t bandLo, int64_t bandHi) {
  if (!(newStep > 0) || !std::isfinite(newStep)) return;

  // A change in the source set, selected venue, or step invalidates the
  // per-venue raw baselines: clear them and re-seed without emitting events.
  if (m_changeVenue != selected || m_changeMask != mask ||
      m_changeStep != newStep || m_prevBid.size() != feeds.venues.size()) {
    m_changeVenue = selected;
    m_changeMask = mask;
    m_changeStep = newStep;
    m_prevBid.assign(feeds.venues.size(), {});
    m_prevAsk.assign(feeds.venues.size(), {});
    m_residBid.assign(feeds.venues.size(), {});
    m_residAsk.assign(feeds.venues.size(), {});
    m_dispBid.clear();
    m_dispAsk.clear();
    m_changes.clear();
    m_changeSeeded = false;
  }
  if (m_residBid.size() != feeds.venues.size()) {
    m_residBid.assign(feeds.venues.size(), {});
    m_residAsk.assign(feeds.venues.size(), {});
  }

  std::unordered_map<int64_t, DomBucket> buckets;
  buckets.reserve(8192);
  bool healthy[64]{};
  feeds.collectHealthy(50.0, healthy);
  summary = {};
  const double referenceMid = selected >= 0 && selected < (int)feeds.venues.size()
                                  ? feeds.venues[(size_t)selected].book.mid()
                                  : feeds.aggMid();

  // Materialized band: the panel's visible ticks, widened by margin and
  // unioned with mid±16 so best/top-5 summaries never lose their source.
  // The previous band drives event suppression below: levels crossing the
  // band edge (scrolling) seed or drop silently instead of flashing.
  const int64_t midTickNow = (int64_t)std::llround(referenceMid / newStep);
  const int64_t newBandLo = std::min(bandLo, midTickNow - 16) - 64;
  const int64_t newBandHi = std::max(bandHi, midTickNow + 16) + 64;
  const int64_t oldBandLo = m_bandLo;
  const int64_t oldBandHi = m_bandHi;
  const double newBandLoP = (double)newBandLo * newStep;
  const double newBandHiP = (double)newBandHi * newStep;
  const double oldBandLoP = (double)oldBandLo * newStep;
  const double oldBandHiP = (double)oldBandHi * newStep;
  const bool bandWasValid = m_bandValid && newStep == m_changeStep;

  auto addSide = [&](const BookSide& side, int venueIndex, bool ask, double scale) {
    std::unordered_map<int64_t, double> local;
    const double loRaw = newBandLoP / scale;
    const double hiRaw = newBandHiP / scale;
    size_t begin = side.lowerBound(loRaw);
    size_t end = side.lowerBound(hiRaw);
    local.reserve(std::min(end - begin, kMaxSideBuckets));
    for (size_t i = begin; i < end && i < side.size(); ++i) {
      double price = side.prices[i] * scale;
      int64_t tick = ask ? askTick(price, newStep) : bidTick(price, newStep);
      local[tick] += side.sizes[i];
    }
    for (const auto& [tick, size] : local) {
      DomBucket& b = buckets[tick];
      b.tick = tick;
      if (ask) {
        b.ask += size;
        b.askVenues++;
        if (size > b.topAskSize) {
          b.topAskSize = size;
          b.topAskVenue = (int16_t)venueIndex;
        }
      } else {
        b.bid += size;
        b.bidVenues++;
        if (size > b.topBidSize) {
          b.topBidSize = size;
          b.topBidVenue = (int16_t)venueIndex;
        }
      }
    }
  };

  // Change events are tracked per venue in raw price space (see header): a
  // venue's own mid drifting (or the reference mid moving) re-buckets its
  // normalized ladder, and diffing normalized snapshots would report every
  // shifted resting level as "pulled". Diffing raw space means only genuine
  // order deltas ever fire. Cancels still flash as pulls; size taken by
  // prints is stripped in updateTrades so a sweep does not amber-wash the
  // ask column. Levels that merely translated with the mid stay silent.
  //
  // Deltas accumulate into `curDelta` for THIS rebuild only (multiple venues
  // can touch one tick in a single snapshot); the merge below then REPLACES
  // the persisted per-tick change, matching the old diff-vs-previous-frame
  // semantics where a +3 add followed by a -4 pull shows -4, not -1.
  std::unordered_map<int64_t, DomChange> curDelta;
  std::unordered_set<int64_t> bidTouched, askTouched;
  auto emit = [&](int64_t tick, double bidDelta, double askDelta, double at) {
    DomChange& c = curDelta[tick];
    if (bidDelta != 0) { c.bid += bidDelta; c.bidAt = at; bidTouched.insert(tick); }
    if (askDelta != 0) { c.ask += askDelta; c.askAt = at; askTouched.insert(tick); }
  };
  auto trackVenue = [&](int i, bool allowed) {
    auto vanishAll = [&](bool ask) {
      std::unordered_map<int64_t, double>& prev =
          (ask ? m_prevAsk : m_prevBid)[(size_t)i];
      auto& resid = (ask ? m_residAsk : m_residBid)[(size_t)i];
      if (prev.empty()) {
        resid.clear();
        return;
      }
      if (m_changeSeeded) {
        double scale = priceScale(feeds, i, selected, referenceMid);
        for (const auto& [tick, old] : prev) {
          if (std::fabs(old) <= 1e-12) continue;
          double rep = (double)tick * newStep * scale;
          int64_t norm = ask ? askTick(rep, newStep) : bidTick(rep, newStep);
          emit(norm, ask ? 0.0 : -old, ask ? -old : 0.0, nowSeconds);
        }
      }
      prev.clear();
      resid.clear();
    };
    if (!allowed) { vanishAll(false); vanishAll(true); return; }

    const VenueState& venue = feeds.venues[(size_t)i];
    double scale = priceScale(feeds, i, selected, referenceMid);
    auto trackSide = [&](const BookSide& side, bool ask) {
      std::unordered_map<int64_t, double>& prev =
          (ask ? m_prevAsk : m_prevBid)[(size_t)i];
      auto& resid = (ask ? m_residAsk : m_residBid)[(size_t)i];
      std::unordered_map<int64_t, std::pair<double, double>> cur;
      const double loRaw = newBandLoP / scale;
      const double hiRaw = newBandHiP / scale;
      size_t begin = side.lowerBound(loRaw);
      size_t end = side.lowerBound(hiRaw);
      cur.reserve(std::min(end - begin, kMaxSideBuckets));
      for (size_t j = begin; j < end && j < side.size(); ++j) {
        double price = side.prices[j];
        int64_t tick = ask ? askTick(price, newStep) : bidTick(price, newStep);
        auto it = cur.find(tick);
        if (it == cur.end()) it = cur.emplace(tick, std::make_pair(0.0, price)).first;
        it->second.first += side.sizes[j];
        // asks ascend (first insert keeps the nearest-mid price); bids ascend
        // so overwrite to keep the highest (nearest-mid) price as the rep
        if (!ask) it->second.second = price;
      }
      if (!m_changeSeeded) {
        for (const auto& [tick, entry] : cur) {
          prev.emplace(tick, entry.first);
          ResidualLevel& lvl = resid[tick];
          lvl.repPrice = entry.second;
          lvl.blocks.clear();
          lvl.blocks.push_back({i, entry.first, 0.0});
        }
        return;
      }
      for (const auto& [tick, entry] : cur) {
        auto it = prev.find(tick);
        double old = it != prev.end() ? it->second : 0.0;
        double delta = entry.first - old;
        ResidualLevel& lvl = resid[tick];
        lvl.repPrice = entry.second;
        double normP = entry.second * scale;
        bool observed = !bandWasValid ||
                        (normP >= oldBandLoP && normP <= oldBandHiP);
        if (std::fabs(delta) > 1e-12) {
          if (observed) {
            int64_t norm = ask ? askTick(normP, newStep) : bidTick(normP, newStep);
            emit(norm, ask ? 0.0 : delta, ask ? delta : 0.0, nowSeconds);
            applyResidualDelta(lvl.blocks, i, entry.first, old, nowSeconds);
          } else { // entered the band with the view, not the book — seed quietly
            lvl.blocks.clear();
            lvl.blocks.push_back({i, entry.first, 0.0});
          }
        } else if (lvl.blocks.empty() && entry.first > 1e-12) {
          lvl.blocks.push_back({i, entry.first, 0.0});
        }
      }
      for (const auto& [tick, old] : prev) {
        if (std::fabs(old) <= 1e-12 || cur.find(tick) != cur.end()) continue;
        double rep = (double)tick * newStep * scale;
        if (rep >= newBandLoP && rep <= newBandHiP) {
          int64_t norm = ask ? askTick(rep, newStep) : bidTick(rep, newStep);
          emit(norm, ask ? 0.0 : -old, ask ? -old : 0.0, nowSeconds);
        } // else: left the band with the view — drop quietly, no phantom pull
        resid.erase(tick);
      }
      prev.clear();
      prev.reserve(cur.size());
      for (const auto& [tick, entry] : cur) prev.emplace(tick, entry.first);
    };
    trackSide(venue.book.bids, false);
    trackSide(venue.book.asks, true);
  };

  for (int i = 0; i < (int)feeds.venues.size() && i < 64; ++i) {
    if (!sourceAllowed(feeds, i, selected, mask, healthy, nowMs)) {
      trackVenue(i, false);
      continue;
    }
    const VenueState& venue = feeds.venues[(size_t)i];
    if (venue.book.bids.size() == 0 || venue.book.asks.size() == 0) {
      trackVenue(i, false);
      continue;
    }
    summary.sourceCount++;
    double scale = priceScale(feeds, i, selected, referenceMid);
    double bid = venue.book.bestBid() * scale, ask = venue.book.bestAsk() * scale;
    summary.bestBid = std::max(summary.bestBid, bid);
    if (ask > 0) summary.bestAsk = summary.bestAsk == 0 ? ask : std::min(summary.bestAsk, ask);
    addSide(venue.book.bids, i, false, scale);
    addSide(venue.book.asks, i, true, scale);
    trackVenue(i, true);
  }

  m_changeSeeded = true;
  m_bandLo = newBandLo;
  m_bandHi = newBandHi;
  m_bandValid = true;

  // Project per-venue raw residuals onto the normalized tick axis. Age stays
  // with the chunk; only the display bucket moves when a mid scale shifts.
  m_dispBid.clear();
  m_dispAsk.clear();
  auto projectSide = [&](bool ask) {
    auto& disp = ask ? m_dispAsk : m_dispBid;
    const auto& rawSides = ask ? m_residAsk : m_residBid;
    for (int i = 0; i < (int)rawSides.size(); ++i) {
      if (rawSides[(size_t)i].empty()) continue;
      double scale = priceScale(feeds, i, selected, referenceMid);
      for (const auto& [tick, lvl] : rawSides[(size_t)i]) {
        if (lvl.blocks.empty()) continue;
        double rep = lvl.repPrice > 0 ? lvl.repPrice * scale
                                      : (double)tick * newStep * scale;
        int64_t norm = ask ? askTick(rep, newStep) : bidTick(rep, newStep);
        auto& dst = disp[norm];
        dst.insert(dst.end(), lvl.blocks.begin(), lvl.blocks.end());
      }
    }
    for (auto& [tick, list] : disp) {
      sortResiduals(list);
      capResiduals(list, kMaxDisplayResiduals);
    }
  };
  projectSide(false);
  projectSide(true);

  // Replace only the sides that actually changed this frame; sides untouched
  // this rebuild keep their persisted event so the fade can continue.
  for (int64_t tick : bidTouched) {
    DomChange& change = m_changes[tick];
    change.bid = curDelta[tick].bid;
    change.bidAt = curDelta[tick].bidAt;
  }
  for (int64_t tick : askTouched) {
    DomChange& change = m_changes[tick];
    change.ask = curDelta[tick].ask;
    change.askAt = curDelta[tick].askAt;
  }

  levels.clear();
  levels.reserve(std::min<size_t>(buckets.size(), kMaxSideBuckets * 2));
  for (auto& [tick, bucket] : buckets) levels.push_back(bucket);
  std::sort(levels.begin(), levels.end(),
            [](const DomBucket& a, const DomBucket& b) { return a.tick > b.tick; });

  // Retain only the nearest 4,096 price buckets on each side. The source L2
  // books remain canonical; this bounds the DOM projection without truncating
  // or duplicating feed storage.
  std::unordered_set<int64_t> keep;
  keep.reserve(kMaxSideBuckets * 2);
  size_t keptBids = 0, keptAsks = 0;
  for (const DomBucket& level : levels) {
    if (level.bid > 0 && keptBids++ < kMaxSideBuckets) keep.insert(level.tick);
  }
  for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
    if (it->ask > 0 && keptAsks++ < kMaxSideBuckets) keep.insert(it->tick);
  }
  levels.erase(std::remove_if(levels.begin(), levels.end(), [&](const DomBucket& level) {
                 return keep.find(level.tick) == keep.end();
               }), levels.end());
  buckets.clear();
  buckets.reserve(levels.size());
  for (const DomBucket& level : levels) buckets.emplace(level.tick, level);

  double bidCum = 0, bidCumUsd = 0;
  int bidSeen = 0;
  for (DomBucket& level : levels) {
    if (level.bid > 0) {
      bidCum += level.bid;
      bidCumUsd += level.bid * ((double)level.tick * newStep);
      level.bidCum = bidCum;
      level.bidCumUsd = bidCumUsd;
      if (bidSeen++ < 5) summary.bidTop5 += level.bid;
    }
  }
  double askCum = 0, askCumUsd = 0;
  int askSeen = 0;
  for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
    if (it->ask > 0) {
      askCum += it->ask;
      askCumUsd += it->ask * ((double)it->tick * newStep);
      it->askCum = askCum;
      it->askCumUsd = askCumUsd;
      if (askSeen++ < 5) summary.askTop5 += it->ask;
    }
  }

  summary.mid = referenceMid;
  summary.crossed = summary.bestBid > 0 && summary.bestAsk > 0 &&
                    summary.bestBid >= summary.bestAsk;
  double topTotal = summary.bidTop5 + summary.askTop5;
  if (topTotal > 0)
    summary.imbalance = (summary.bidTop5 - summary.askTop5) / topTotal;

  double bestBidSize = 0, bestAskSize = 0;
  auto bidIt = buckets.find(bidTick(summary.bestBid, newStep));
  auto askIt = buckets.find(askTick(summary.bestAsk, newStep));
  if (bidIt != buckets.end()) bestBidSize = bidIt->second.bid;
  if (askIt != buckets.end()) bestAskSize = askIt->second.ask;
  if (!summary.crossed && bestBidSize + bestAskSize > 0)
    summary.microprice = (summary.bestAsk * bestBidSize +
                          summary.bestBid * bestAskSize) /
                         (bestBidSize + bestAskSize);

  for (auto it = m_changes.begin(); it != m_changes.end();) {
    double last = std::max(it->second.bidAt, it->second.askAt);
    if (buckets.find(it->first) == buckets.end() && nowSeconds - last > 2.0)
      it = m_changes.erase(it);
    else
      ++it;
  }

  m_index.clear();
  m_index.reserve(levels.size());
  for (size_t i = 0; i < levels.size(); ++i) m_index[levels[i].tick] = i;
  step = newStep;
  m_sourceNowMs = nowMs;
  suppressExecutedPulls(newStep);
  sourceVersion = sourceSignature(feeds, selected, nowMs);
}

void DomModel::suppressExecutedPulls(double newStep) {
  if (!(newStep > 0) || !std::isfinite(newStep) || m_changes.empty()) return;
  auto explained = [](double decrease, double prints) {
    return prints >= std::max(1e-12, decrease) * 0.5;
  };
  const double bestAsk = summary.bestAsk;
  const double bestBid = summary.bestBid;
  const double slop = newStep * 0.51;
  // Ask buckets use ceil(price/step). Reconstructing px = tick*step then
  // sits on the *top* of the bucket, which is often >= the new best ask
  // even when that row is already behind the market — so compare ticks.
  const int64_t bestAskTick = bestAsk > 0 ? askTick(bestAsk, newStep) : 0;
  const int64_t bestBidTick = bestBid > 0 ? bidTick(bestBid, newStep) : 0;
  for (auto& [tick, c] : m_changes) {
    double px = (double)tick * newStep;
    if (c.ask < -1e-12) {
      double prints = 0;
      auto it = m_hits.find(tick);
      if (it != m_hits.end()) prints = it->second.buy;
      // Strictly below the live best-ask tick: that liquidity was consumed
      // (or is a grouping leftover). Does not need a matching print — L2
      // often deletes the level before the tape catches up.
      bool behind = bestAskTick != 0 && tick < bestAskTick;
      bool swept = m_sweepBuy > 0 && px <= m_sweepBuy + slop;
      if (explained(-c.ask, prints) || behind || swept) {
        c.ask = 0;
        c.askAt = -1e9;
      }
    }
    if (c.bid < -1e-12) {
      double prints = 0;
      auto it = m_hits.find(tick);
      if (it != m_hits.end()) prints = it->second.sell;
      bool behind = bestBidTick != 0 && tick > bestBidTick;
      bool swept = m_sweepSell > 0 && px >= m_sweepSell - slop;
      if (explained(-c.bid, prints) || behind || swept) {
        c.bid = 0;
        c.bidAt = -1e9;
      }
    }
  }
}

void DomModel::updateTrades(const Feeds& feeds, int selected, uint32_t mask,
                            double newStep, double window, double nowMs) {
  uint64_t signature = sourceSignature(feeds, selected, nowMs);
  bool stale = m_flowHead != feeds.tape.head || m_flowCount != feeds.tape.count ||
               m_flowSourceSignature != signature || m_flowVenue != selected ||
               m_flowMask != mask || m_flowStep != newStep ||
               m_flowWindow != window || nowMs >= m_flowNextExpiry ||
               nowMs >= m_hitNextExpiry;
  if (stale) {
    m_flowHead = feeds.tape.head;
    m_flowCount = feeds.tape.count;
    m_flowSourceSignature = signature;
    m_flowVenue = selected;
    m_flowMask = mask;
    m_flowStep = newStep;
    m_flowWindow = window;
    m_flowNextExpiry = INFINITY;
    m_hitNextExpiry = INFINITY;
    m_flow.clear();
    m_hits.clear();
    m_sweepBuy = 0;
    m_sweepSell = 0;
    recentFlow = {};
    lastTradePrice = 0;

    bool healthy[64]{};
    feeds.collectHealthy(50.0, healthy);
    // Hit matching must follow the tape's clock, not wall time: exchange
    // timestamps routinely sit a second or more off Date.now(), which is why
    // the previous print filter never caught live sweeps.
    double origin = nowMs;
    if (const TapeEntry* newest = feeds.tape.latest(0)) {
      if (newest->ts > 0 && std::fabs(newest->ts - nowMs) < 120000.0)
        origin = newest->ts;
    }
    double cutoff = nowMs - window * 1000.0;
    double hitCutoff = origin - kHitWindowMs;
    for (size_t i = 0; i < feeds.tape.count; ++i) {
      const TapeEntry* trade = feeds.tape.latest(i);
      if (!trade) continue;
      // The ring is arrival-ordered, but venue clocks skew a few seconds, so
      // ts is not strictly monotonic. Only break past a skew margin (mirrors the
      // +5000 future guard below); trades nearer the cutoff are scanned so a
      // behind-clock venue's prints aren't dropped at the window edge.
      if (trade->ts < cutoff - 5000.0 && trade->ts < hitCutoff) break;
      if (trade->ts > origin + 30000.0 || trade->venue >= feeds.venues.size()) continue;
      if (!sourceAllowed(feeds, (int)trade->venue, selected, mask, healthy, nowMs))
        continue;
      double normalizedPrice = trade->price *
          priceScale(feeds, (int)trade->venue, selected,
                     selected >= 0 ? feeds.venues[(size_t)selected].book.mid()
                                   : feeds.aggMid());
      if (lastTradePrice == 0 && trade->ts >= cutoff)
        lastTradePrice = normalizedPrice;
      if (trade->ts >= cutoff) {
        int64_t tick = (int64_t)std::llround(normalizedPrice / newStep);
        DomFlow& flow = m_flow[tick];
        if (trade->side == wire::BidOrBuy) {
          flow.buy += trade->qty;
          recentFlow.buy += trade->qty;
        } else {
          flow.sell += trade->qty;
          recentFlow.sell += trade->qty;
        }
        m_flowNextExpiry = std::min(m_flowNextExpiry, trade->ts + window * 1000.0);
      }
      if (trade->ts >= hitCutoff) {
        if (trade->side == wire::BidOrBuy) {
          m_hits[askTick(normalizedPrice, newStep)].buy += trade->qty;
          m_sweepBuy = std::max(m_sweepBuy, normalizedPrice);
        } else {
          m_hits[bidTick(normalizedPrice, newStep)].sell += trade->qty;
          m_sweepSell = m_sweepSell == 0
                            ? normalizedPrice
                            : std::min(m_sweepSell, normalizedPrice);
        }
        m_hitNextExpiry = std::min(m_hitNextExpiry, trade->ts + kHitWindowMs);
      }
    }
  }
  suppressExecutedPulls(newStep);
}

const DomBucket* DomModel::bucket(int64_t tick) const {
  auto it = m_index.find(tick);
  return it == m_index.end() ? nullptr : &levels[it->second];
}

DomFlow DomModel::flow(int64_t tick) const {
  auto it = m_flow.find(tick);
  return it == m_flow.end() ? DomFlow{} : it->second;
}

DomChange DomModel::change(int64_t tick) const {
  auto it = m_changes.find(tick);
  return it == m_changes.end() ? DomChange{} : it->second;
}

void DomModel::residuals(int64_t tick, bool ask, std::vector<DomResidual>& out) const {
  out.clear();
  const auto& m = ask ? m_dispAsk : m_dispBid;
  auto it = m.find(tick);
  if (it != m.end()) out = it->second;
}

void DomModel::contributions(const Feeds& feeds, int selected, uint32_t mask,
                             int64_t targetTick,
                             std::vector<DomContribution>& out) const {
  out.clear();
  bool healthy[64]{};
  feeds.collectHealthy(50.0, healthy);
  double referenceMid = selected >= 0 && selected < (int)feeds.venues.size()
                            ? feeds.venues[(size_t)selected].book.mid()
                            : feeds.aggMid();
  for (int i = 0; i < (int)feeds.venues.size() && i < 64; ++i) {
    if (!sourceAllowed(feeds, i, selected, mask, healthy, m_sourceNowMs)) continue;
    DomContribution c;
    c.venue = i;
    const L2Book& book = feeds.venues[(size_t)i].book;
    double scale = priceScale(feeds, i, selected, referenceMid);
    for (size_t j = 0; j < book.bids.size(); ++j)
      if (bidTick(book.bids.prices[j] * scale, step) == targetTick)
        c.bid += book.bids.sizes[j];
    for (size_t j = 0; j < book.asks.size(); ++j)
      if (askTick(book.asks.prices[j] * scale, step) == targetTick)
        c.ask += book.asks.sizes[j];
    if (c.bid > 0 || c.ask > 0) out.push_back(c);
  }
  std::sort(out.begin(), out.end(), [](const DomContribution& a, const DomContribution& b) {
    return std::max(a.bid, a.ask) > std::max(b.bid, b.ask);
  });
}
