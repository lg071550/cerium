#include "feeds.h"

#include "bridge.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>

static Feeds* g_feedsInstance = nullptr;

extern "C" void cerium_on_candles(double* data, int n, int tfKind, double tfValue,
                                  int sym) {
  if (!g_feedsInstance || sym != g_feedsInstance->symbol) return;
  Timeframe tf{(Timeframe::Kind)(uint8_t)tfKind, tfValue};
  if (!(tf == g_feedsInstance->candles.tf)) return; // stale in-flight bootstrap
  g_feedsInstance->candles.load(data, n, tf, sym);
}

extern "C" void cerium_on_orderflow(double* data, int n, int tfKind,
                                     double tfValue, int sym, int prepend) {
  if (!g_feedsInstance || sym != g_feedsInstance->symbol) return;
  Timeframe tf{(Timeframe::Kind)(uint8_t)tfKind, tfValue};
  if (!(tf == g_feedsInstance->candles.tf)) return;
  g_feedsInstance->orderFlow.load(data, n, tf, sym, prepend != 0);
}

extern "C" void cerium_on_market(double* oi, int oiN, double* funding, int fundingN,
                                 double* liq, int liqN, int sym, double liqBase) {
  if (!g_feedsInstance || sym != g_feedsInstance->symbol) return;
  g_feedsInstance->market.loadOi(oi, oiN, sym);
  g_feedsInstance->market.loadFunding(funding, fundingN, sym);
  g_feedsInstance->market.loadLiq(liq, liqN, sym, (int64_t)liqBase);
}

// Incremental liquidation prints: [ts, price, qty, side] × n starting at the
// worker-global record index `start` — displays per print, no full re-copy.
extern "C" void cerium_on_liq(double* liq, int n, double start, int sym) {
  if (!g_feedsInstance || sym != g_feedsInstance->symbol) return;
  g_feedsInstance->market.appendLiq(liq, n, (int64_t)start, sym);
}

extern "C" void cerium_on_ht(double* liq, int liqN, double* sl, int slN, int sym,
                             int status, int used, int quota, double liqAt, double slAt,
                             double liqRef, double slRef) {
  if (!g_feedsInstance || sym != g_feedsInstance->symbol) return;
  g_feedsInstance->ht.load(liq, liqN, sl, slN, sym, status, used, quota, liqAt, slAt,
                           (float)liqRef, (float)slRef);
}

// Probe diagnostics: per-venue book shape at the moment of the call.
// [enabled, status, bidLevels, askLevels, minBid, maxBid, minAsk, maxAsk, mid]
// Probe diagnostics: live liquidation series state.
// [count, version, newestTs, newestPrice]
extern "C" double* cerium_liq_debug() {
  static double out[4] = {};
  if (!g_feedsInstance) return out;
  const MarketSeries& m = g_feedsInstance->market;
  out[0] = (double)m.liq.size();
  out[1] = (double)m.version;
  out[2] = m.liq.empty() ? 0 : m.liq.back().ts;
  out[3] = m.liq.empty() ? 0 : m.liq.back().price;
  return out;
}

extern "C" double* cerium_venue_debug(int i) {
  static double out[9] = {};
  if (!g_feedsInstance || i < 0 ||
      (size_t)i >= g_feedsInstance->venues.size())
    return out;
  const VenueState& v = g_feedsInstance->venues[(size_t)i];
  out[0] = v.enabled ? 1 : 0;
  out[1] = v.status;
  out[2] = (double)v.book.bids.prices.size();
  out[3] = (double)v.book.asks.prices.size();
  out[4] = v.book.bids.prices.empty() ? 0 : v.book.bids.prices.front();
  out[5] = v.book.bids.prices.empty() ? 0 : v.book.bids.prices.back();
  out[6] = v.book.asks.prices.empty() ? 0 : v.book.asks.prices.front();
  out[7] = v.book.asks.prices.empty() ? 0 : v.book.asks.prices.back();
  double mid = 0;
  if (!v.book.bids.prices.empty() && !v.book.asks.prices.empty())
    mid = (v.book.bids.prices.back() + v.book.asks.prices.front()) * 0.5;
  out[8] = mid;
  return out;
}

// Index contract with feeds/registry.ts — do not reorder.
// cls: 1=spot 2=perp 4=dex
static const struct {
  const char* id;
  const char* label;
  const char* shortLabel;
  uint8_t cls;
} kVenues[] = {
    {"binance-perp", "Binance Perp", "BN-P", 2}, {"binance", "Binance", "BN", 1},
    {"bybit-perp", "Bybit Perp", "BB-P", 2},     {"bybit", "Bybit", "BB", 1},
    {"okx-perp", "OKX Perp", "OK-P", 2},         {"okx", "OKX", "OK", 1},
    {"bitget-perp", "Bitget Perp", "BG-P", 2},   {"bitget", "Bitget", "BG", 1},
    {"coinbase", "Coinbase", "CB", 1},           {"kraken", "Kraken", "KR", 1},
    {"gate-perp", "Gate Perp", "GT-P", 2},       {"gate", "Gate", "GT", 1},
    {"hyperliquid", "Hyperliquid", "HL", 4},     {"bitstamp", "Bitstamp", "BS", 1},
    {"cryptocom", "Crypto.com", "CRO", 1},       {"cryptocom-perp", "CRO Perp", "CRO-P", 2},
    {"bitfinex", "Bitfinex", "BFX", 1},          {"bitfinex-perp", "BFX Perp", "BFX-P", 2},
    {"deribit", "Deribit", "DRB", 2},            {"kraken-perp", "Kraken Perp", "KR-P", 2},
    {"mexc-perp", "MEXC Perp", "MX-P", 2},       {"coinbase-perp", "CB Perp", "CB-P", 2},
    {"coinbase-us-perp", "CB US Perp", "CBUS-P", 2}, {"aster", "Aster", "AST", 4},
    {"lighter", "Lighter", "LTR", 4},            {"dydx", "dYdX", "DYX", 4},
    {"extended", "Extended", "EXT", 4},
};

static_assert(sizeof(kVenues) / sizeof(kVenues[0]) == kVenueCount,
              "kVenueCount must match the kVenues registry");

uint32_t venueMaskForClass(uint8_t cls) {
  uint32_t mask = 0;
  for (int i = 0; i < kVenueCount; ++i)
    if (kVenues[i].cls & cls) mask |= (1u << i);
  return mask;
}

void Feeds::init() {
  for (auto& v : kVenues) {
    VenueState s{};
    s.id = v.id;
    s.label = v.label;
    s.shortLabel = v.shortLabel;
    s.cls = v.cls;
    venues.push_back(std::move(s));
  }
  g_feedsInstance = this;
  bridge::init();
  m_started = true;
}

int Feeds::frame() {
  if (!m_started) return 0;

  int applied = 0;
  static wire::Event buf[16384]; // 512 KB static drain buffer
  for (;;) {
    int n = bridge::drain(buf, 16384);
    if (n <= 0) break;
    for (int i = 0; i < n; ++i) apply(buf[i]);
    applied += n;
    if (n < 16384) break;
  }

  int dropped = bridge::takeDropped();
  if (dropped > 0) {
    fprintf(stderr, "feeds: ring overflow, %d events dropped — resyncing venues\n",
            dropped);
    for (size_t i = 0; i < venues.size(); ++i)
      if (venues[i].enabled) requestResync((int)i);
  }

  // Surgical recovery: the worker reports exactly which venues lost
  // book-affecting events to pre-ring eviction; only those books are stale.
  uint32_t lostVenues = bridge::takeLostVenues();
  while (lostVenues) {
    int v = std::countr_zero(lostVenues);
    lostVenues &= lostVenues - 1;
    if (v < (int)venues.size() && venues[(size_t)v].enabled) {
      fprintf(stderr, "feeds: book events lost pre-ring for venue %d — resyncing\n", v);
      requestResync(v);
    }
  }
  return applied;
}

void Feeds::requestResync(int venue) {
  bridge::sendCommand(wire::CmdResyncVenue, (uint32_t)venue, 0);
}

void Feeds::setSymbol(int sym) {
  if (sym == symbol) return;
  symbol = sym;
  m_aggCacheValid = false;
  for (auto& v : venues) {
    v.book.clear();
    v.status = wire::StatusNone;
    v.bookUpdatedAtMs = 0;
  }
  tape.clear();
  candles.v.clear();
  candles.barCount = 0;
  candles.awaitingHistory = true;
  candles.historyLoaded = false;
  candles.historyWaitT0 = 0;
  orderFlow.clear();
  market.clear();
  ht.clear();
  m_htSym = -1;
  bridge::sendCommand(wire::CmdSetSymbol, 0, (double)sym);
}

void Feeds::setVenueEnabled(int i, bool on) {
  VenueState* v = venue(i);
  if (!v || v->enabled == on) return;
  v->enabled = on;
  m_aggCacheValid = false;
  if (!on) {
    v->book.clear();
    v->status = wire::StatusNone;
    v->bookUpdatedAtMs = 0;
  }
  bridge::sendCommand(wire::CmdSetVenueEnabled, (uint32_t)i, on ? 1.0 : 0.0);
}

void Feeds::setTimeframe(Timeframe tf) {
  if (tf.kind == Timeframe::Time) tf.value = std::round(tf.value);
  if (!(tf.value > 0) || tf.value > 1e6) return;
  const bool same = tf == candles.tf;
  // Same TF with a completed bootstrap is a no-op. Same TF while history is
  // missing (failed/in-flight klines, live-only tape) re-issues the fetch.
  if (same && candles.historyLoaded) return;
  candles.tf = tf;
  candles.v.clear();
  candles.barCount = 0;
  candles.awaitingHistory = true;
  candles.historyLoaded = false;
  candles.historyWaitT0 = 0;
  // orderFlow stays: raw aggressor prints are timeframe-independent, so
  // wiping them here blanked footprints/CVD on every TF switch until a full
  // REST re-walk finished. The worker's bootstrap replace stitches the
  // cached window in front of whatever live prints accumulated meanwhile.
  bridge::sendCommand(wire::CmdSetCandles, (uint32_t)tf.kind, tf.value);
}

void Feeds::requestOrderFlow() {
  if (m_orderFlowRequested) return;
  refreshOrderFlow();
}

void Feeds::refreshOrderFlow() {
  m_orderFlowRequested = true;
  bridge::sendCommand(wire::CmdRequestOrderFlow, 0, 1.0);
}

void Feeds::setFlowMask(uint32_t mask) {
  mask &= kAllVenuesMask;
  if (mask == flowMask) return;
  flowMask = mask;
  // Live prints follow the new mask from this point. Do not drop the Binance
  // bootstrap window — the next symbol/timeframe load replaces it, and a clear
  // here left footprint charts with only a few minutes of live tape.
}

void Feeds::setHt(const char* token, bool liq, bool sl) {
  const char* t = token ? token : "";
  if (m_htToken == t && m_htLiq == liq && m_htSl == sl && m_htSym == symbol) return;
  m_htToken = t;
  m_htLiq = liq;
  m_htSl = sl;
  m_htSym = symbol;
  bridge::sendHt(m_htToken.c_str(), symbol, liq, sl);
}

int Feeds::liveCount() const {
  int n = 0;
  for (auto& v : venues)
    if (v.enabled && v.status == wire::Live) n++;
  return n;
}

uint64_t Feeds::booksVersion() const {
  uint64_t sum = 0;
  for (auto& v : venues)
    if (v.enabled) sum += v.book.version;
  return sum;
}

// Aggregate mid + 50 bps venue health, computed at most once per book change:
// keyed on booksVersion() (sum of enabled book versions), so the several
// aggregate queries per frame share one mid-sorting pass and no allocations.
static const double kAggTolBps = 50.0;

void Feeds::refreshAggCache() const {
  uint64_t ver = booksVersion();
  if (m_aggCacheValid && ver == m_aggCacheVer) return;
  m_aggCacheValid = true;
  m_aggCacheVer = ver;

  double mids[64];
  int n = 0;
  for (auto& v : venues) {
    if (!v.enabled || n >= 64) continue;
    double b = v.book.bestBid(), a = v.book.bestAsk();
    if (b > 0 && a > 0) mids[n++] = (a + b) * 0.5;
  }
  if (n == 0) {
    m_aggMid = 0;
  } else {
    std::sort(mids, mids + n);
    m_aggMid = mids[n / 2];
  }

  for (size_t i = 0; i < venues.size() && i < 64; ++i) {
    const VenueState& v = venues[i];
    bool ok = false;
    if (v.enabled && m_aggMid > 0) {
      double b = v.book.bestBid(), a = v.book.bestAsk();
      if (b > 0 && a > 0) {
        double m = (a + b) * 0.5;
        ok = std::fabs(m - m_aggMid) / m_aggMid * 1e4 <= kAggTolBps;
      }
    }
    m_aggHealthy[i] = ok;
  }
}

double Feeds::aggMid() const {
  refreshAggCache();
  return m_aggMid;
}

int Feeds::collectHealthy(double tolBps, bool* out, size_t cap) const {
  if (cap > venues.size()) cap = venues.size();
  if (tolBps == kAggTolBps) { // cached path (same tolerance as agg best bid/ask)
    refreshAggCache();
    int n = 0;
    for (size_t i = 0; i < cap; ++i) {
      bool ok = i < 64 && m_aggHealthy[i];
      out[i] = ok;
      if (ok) n++;
    }
    return n;
  }
  double mid = aggMid();
  int n = 0;
  for (size_t i = 0; i < cap; ++i) {
    const VenueState& v = venues[i];
    bool ok = false;
    if (v.enabled && mid > 0) {
      double b = v.book.bestBid(), a = v.book.bestAsk();
      if (b > 0 && a > 0) {
        double m = (a + b) * 0.5;
        ok = std::fabs(m - mid) / mid * 1e4 <= tolBps;
      }
    }
    out[i] = ok;
    if (ok) n++;
  }
  return n;
}

double Feeds::aggBestBid() const {
  refreshAggCache();
  double b = 0;
  for (size_t i = 0; i < venues.size(); ++i)
    if (i < 64 && m_aggHealthy[i]) b = std::max(b, venues[i].book.bestBid());
  return b;
}

double Feeds::aggBestAsk() const {
  refreshAggCache();
  double a = 0;
  for (size_t i = 0; i < venues.size(); ++i)
    if (i < 64 && m_aggHealthy[i]) {
      double x = venues[i].book.bestAsk();
      if (x > 0) a = (a == 0) ? x : std::min(a, x);
    }
  return a;
}

void Feeds::apply(const wire::Event& e) {
  if (e.venue >= venues.size()) return;
  VenueState& v = venues[e.venue];
  if (!v.enabled && e.type != wire::FeedStatus) return;

  switch (e.type) {
    case wire::SnapshotBegin:
      m_collecting = true;
      m_collectVenue = e.venue;
      m_snapBidP.clear();
      m_snapBidS.clear();
      m_snapAskP.clear();
      m_snapAskS.clear();
      break;

    case wire::SnapshotLevel:
      if (!m_collecting || m_collectVenue != e.venue) break;
      if (e.side == wire::BidOrBuy) {
        m_snapBidP.push_back(e.price);
        m_snapBidS.push_back(e.qty);
      } else {
        m_snapAskP.push_back(e.price);
        m_snapAskS.push_back(e.qty);
      }
      break;

    case wire::SnapshotEnd: {
      if (!m_collecting || m_collectVenue != e.venue) break;
      m_collecting = false;
      auto sortAsc = [](std::vector<double>& p, std::vector<double>& s) {
        std::vector<size_t> idx(p.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::sort(idx.begin(), idx.end(),
                  [&](size_t a, size_t b) { return p[a] < p[b]; });
        std::vector<double> tp(p.size()), ts(p.size());
        for (size_t i = 0; i < idx.size(); ++i) {
          tp[i] = p[idx[i]];
          ts[i] = s[idx[i]];
        }
        p.swap(tp);
        s.swap(ts);
      };
      sortAsc(m_snapBidP, m_snapBidS);
      sortAsc(m_snapAskP, m_snapAskS);
      v.book.bids.loadSorted(m_snapBidP.data(), m_snapBidS.data(), m_snapBidP.size());
      v.book.asks.loadSorted(m_snapAskP.data(), m_snapAskS.data(), m_snapAskP.size());
      v.book.version++;
      v.bookUpdatedAtMs = e.ts;
      break;
    }

    case wire::BookUpdate:
      if (m_collecting && m_collectVenue == e.venue) break; // mid-snapshot; skip
      if (e.side == wire::BidOrBuy) v.book.bids.set(e.price, e.qty);
      else v.book.asks.set(e.price, e.qty);
      v.book.version++;
      v.bookUpdatedAtMs = e.ts;
      break;

    case wire::Trade:
      tape.push(e.price, e.qty, e.ts, e.venue, e.side);
      if (e.venue == 0) candles.onTrade(e.price, e.qty, e.side, e.ts); // Binance OHLC
      if (flowMask & (1u << e.venue)) {
        candles.onAgg(e.price, e.qty, e.side, e.ts); // aggregated volume + CVD (mask-filtered)
        if (m_orderFlowRequested) {
          // Each venue quotes its own mid; mixed-venue footprints otherwise
          // scatter the "same" price level across the inter-venue drift. Rebase
          // every print onto the Binance (venue 0) reference axis: print -
          // venueMid + refMid. Venue 0 prints pass through unchanged (its mid
          // *is* the reference), which also preserves the bootstrap overlap
          // dedup. If the reference book is unavailable, fall back to the
          // aggregate mid, then to the raw print.
          double ref = 0;
          if (venues[0].book.bestBid() > 0 && venues[0].book.bestAsk() > 0)
            ref = (venues[0].book.bestBid() + venues[0].book.bestAsk()) * 0.5;
          if (!(ref > 0)) ref = aggMid();
          double vm = 0;
          if (v.book.bestBid() > 0 && v.book.bestAsk() > 0)
            vm = (v.book.bestBid() + v.book.bestAsk()) * 0.5;
          double px = e.price;
          if (vm > 0 && ref > 0) px = e.price - vm + ref;
          orderFlow.onTrade(px, e.qty, e.side, e.ts);
        }
      }
      break;

    case wire::FeedStatus:
      v.status = (uint8_t)e.aux;
      break;

    default:
      break;
  }
}
