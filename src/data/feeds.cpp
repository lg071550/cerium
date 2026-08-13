#include "feeds.h"

#include "bridge.h"

#include <algorithm>
#include <cstdio>

void Feeds::init() {
  VenueState v{};
  v.id = "binance-perp";
  v.label = "Binance Perp";
  venues.push_back(std::move(v));
  bridge::init();
  m_started = true;
}

void Feeds::frame() {
  if (!m_started) return;

  static wire::Event buf[16384]; // 512 KB static drain buffer
  for (;;) {
    int n = bridge::drain(buf, 16384);
    if (n <= 0) break;
    for (int i = 0; i < n; ++i) apply(buf[i]);
    if (n < 16384) break;
  }

  int dropped = bridge::takeDropped();
  if (dropped > 0) {
    fprintf(stderr, "feeds: ring overflow, %d events dropped — resyncing all venues\n",
            dropped);
    for (size_t i = 0; i < venues.size(); ++i) requestResync((int)i);
  }
}

void Feeds::requestResync(int venue) {
  bridge::sendCommand(wire::CmdResyncVenue, (uint32_t)venue, 0);
}

void Feeds::apply(const wire::Event& e) {
  if (e.venue >= venues.size()) return;
  VenueState& v = venues[e.venue];

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
      // levels arrive best-first (bids desc, asks asc) — sort ascending
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
      break;

    case wire::FeedStatus:
      v.status = (uint8_t)e.aux;
      break;

    default:
      break;
  }
}
