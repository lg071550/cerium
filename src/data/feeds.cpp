#include "feeds.h"

#include "bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

static Feeds* g_feedsInstance = nullptr;

extern "C" void cerium_on_candles(double* data, int n, int interval, int sym) {
  if (g_feedsInstance && sym == g_feedsInstance->symbol)
    g_feedsInstance->candles.load(data, n, interval, sym);
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
  }
  tape.count = 0;
  tape.head = 0;
  candles.v.clear();
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
  }
  bridge::sendCommand(wire::CmdSetVenueEnabled, (uint32_t)i, on ? 1.0 : 0.0);
}

void Feeds::setCandleInterval(int minutes) {
  if (minutes == candles.intervalMin) return;
  candles.intervalMin = minutes;
  candles.v.clear();
  bridge::sendCommand(wire::CmdSetCandles, 0, (double)minutes);
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

int Feeds::collectHealthy(double tolBps, bool* out) const {
  if (tolBps == kAggTolBps) { // cached path (same tolerance as agg best bid/ask)
    refreshAggCache();
    int n = 0;
    for (size_t i = 0; i < venues.size(); ++i) {
      bool ok = i < 64 && m_aggHealthy[i];
      out[i] = ok;
      if (ok) n++;
    }
    return n;
  }
  double mid = aggMid();
  int n = 0;
  for (size_t i = 0; i < venues.size(); ++i) {
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
      break;
    }

    case wire::BookUpdate:
      if (m_collecting && m_collectVenue == e.venue) break; // mid-snapshot; skip
      if (e.side == wire::BidOrBuy) v.book.bids.set(e.price, e.qty);
      else v.book.asks.set(e.price, e.qty);
      v.book.version++;
      break;

    case wire::Trade:
      tape.push(e.price, e.qty, e.ts, e.venue, e.side);
      if (e.venue == 0) candles.onTrade(e.price, e.qty, e.side, e.ts); // chart venue
      break;

    case wire::FeedStatus:
      v.status = (uint8_t)e.aux;
      break;

    default:
      break;
  }
}
