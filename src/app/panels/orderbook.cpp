#include "panels.h"

#include "../../data/merge.h"
#include "../../ui/theme.h"

#include <algorithm>
#include <cstdio>

void drawOrderbook(Ui& u, Rect r, OrderbookPanel& st, Feeds& feeds) {
  const Theme& t = theme();

  // header: Price | Size — cumulative is the translucent bar behind each row
  Rect header{r.x, r.y, r.w, 20};
  u.draw.textAligned(header, "Price (USDT)", t.textDim, DrawList::Left, 10);
  u.draw.textAligned(header, "Size", t.textDim, DrawList::Right, 10);

  // control row: class filter chips + bin selector + recenter
  float cx = r.x + 8;
  float cy = r.y + 22;
  if (chip(u, {cx, cy, 48, 18}, "spot", st.mask & ClassSpot)) st.mask ^= ClassSpot;
  cx += 52;
  if (chip(u, {cx, cy, 48, 18}, "perp", st.mask & ClassPerp)) st.mask ^= ClassPerp;
  cx += 52;
  if (chip(u, {cx, cy, 48, 18}, "dex", st.mask & ClassDex)) st.mask ^= ClassDex;
  cx += 58;
  static const double kBins[] = {0, 0.5, 1, 2.5, 5, 10};
  static const char* kBinLabels[] = {"raw", "0.5", "1", "2.5", "5", "10"};
  char binLabel[24];
  snprintf(binLabel, sizeof(binLabel), "bin %s", kBinLabels[st.binSel]);
  if (chip(u, {cx, cy, 64, 18}, binLabel, st.bin > 0)) {
    st.binSel = (st.binSel + 1) % 6;
    st.bin = kBins[st.binSel];
  }
  cx += 70;
  if (st.scroll != 0 && chip(u, {cx, cy, 58, 18}, "recenter", false)) st.scroll = 0;

  // live venue count at the right end of the control row
  char live[24];
  snprintf(live, sizeof(live), "%d live", feeds.liveCount());
  u.draw.textAligned({r.x, cy, r.w, 18}, live,
                     feeds.liveCount() > 0 ? t.green : t.textDim, DrawList::Right, 10);

  Rect area{r.x, r.y + 44, r.w, r.h - 44};
  double mid = feeds.aggMid();
  if (mid <= 0) {
    u.draw.textAligned(area, "syncing…", t.textDim, DrawList::Center);
    return;
  }

  // rebuild the ladder only when version/filter/bin changed
  uint64_t ver = feeds.booksVersion();
  if (ver != st.mergeVersion || st.mask != st.mergeMask || st.bin != st.mergeBin) {
    st.mergeVersion = ver;
    st.mergeMask = st.mask;
    st.mergeBin = st.bin;

    bool healthy[64];
    feeds.collectHealthy(50.0, healthy);
    std::vector<const BookSide*> askSides, bidSides;
    for (size_t i = 0; i < feeds.venues.size(); ++i) {
      if (!healthy[i] || !(feeds.venues[i].cls & st.mask)) continue;
      askSides.push_back(&feeds.venues[i].book.asks);
      bidSides.push_back(&feeds.venues[i].book.bids);
    }
    static thread_local std::vector<MergedLevel> asks, bids;
    // cap each side at the levels nearest mid — the ladder only ever shows a
    // screenful around mid (plus scroll), and 27 full-depth books otherwise
    // merge tens of thousands of levels every frame
    static constexpr size_t kMaxLadderSide = 1500;
    mergeSideWindow(askSides.data(), askSides.size(), mid, mid * 1.15, st.bin, true,
                    asks, kMaxLadderSide);
    mergeSideWindow(bidSides.data(), bidSides.size(), mid * 0.85, mid, st.bin, false,
                    bids, kMaxLadderSide);
    // cap the MERGED ladder too: 27 venues × 1500 input levels can still fuse
    // into ~10k+ distinct prices; both vectors are sorted nearest-mid-first
    if (asks.size() > kMaxLadderSide) asks.resize(kMaxLadderSide);
    if (bids.size() > kMaxLadderSide) bids.resize(kMaxLadderSide);

    st.ladder.clear();
    st.ladderMid = (int)asks.size();
    st.ladder.reserve(asks.size() + bids.size());
    double cum = 0;
    static thread_local std::vector<double> askCum;
    askCum.clear();
    for (auto& a : asks) { // asks ascend from best: cum accumulates from mid outward
      cum += a.size;
      askCum.push_back(cum);
    }
    for (int i = (int)asks.size() - 1; i >= 0; --i) // desc: deepest ask first
      st.ladder.push_back({asks[(size_t)i].price, asks[(size_t)i].size,
                           askCum[(size_t)i], true, {}, {}});
    cum = 0;
    for (auto& b : bids) {
      cum += b.size;
      st.ladder.push_back({b.price, b.size, cum, false, {}, {}});
    }
    // row labels are formatted lazily in the draw loop — only visible rows
    // ever need them, which keeps snprintf off the rebuild path
  }

  const float rowH = 18.0f;
  int rows = std::max(1, (int)(area.h / rowH));

  // wheel scrolls the ladder window away from mid
  if (u.hovered(area) && u.input.wheelY != 0)
    st.scroll += u.input.wheelY > 0 ? 3 : -3;
  int maxStart = std::max(0, (int)st.ladder.size() - rows);
  int startIdx = std::clamp(st.ladderMid - rows / 2 + st.scroll, 0, maxStart);
  st.scroll = startIdx - (st.ladderMid - rows / 2); // keep scroll bounded

  double maxCum = 1;
  for (int i = startIdx; i < std::min((int)st.ladder.size(), startIdx + rows); ++i)
    maxCum = std::max(maxCum, st.ladder[(size_t)i].cum);

  float y = area.y;
  for (int i = startIdx; i < (int)st.ladder.size() && y + rowH <= area.y + area.h; ++i) {
    if (i == st.ladderMid) { // spread band at the ask/bid boundary
      double bb = feeds.aggBestBid(), ba = feeds.aggBestAsk();
      char spread[48];
      double sp = ba - bb;
      snprintf(spread, sizeof(spread), "Spread  %.2f  (%.2f bps)", sp, sp / mid * 1e4);
      u.draw.rect({area.x, y, area.w, rowH}, t.panelAlt);
      u.draw.textAligned({area.x, y, area.w, rowH}, spread, t.textDim, DrawList::Center);
      y += rowH;
      if (y + rowH > area.y + area.h) break;
    }
    OrderbookPanel::Level& L = st.ladder[(size_t)i];
    if (L.fmtP != L.price) { // label caches survive price-stable frames
      snprintf(L.priceLbl, sizeof(L.priceLbl), "%.2f", L.price);
      L.fmtP = L.price;
    }
    if (L.fmtS != L.size) {
      snprintf(L.sizeLbl, sizeof(L.sizeLbl), "%.3f", L.size);
      L.fmtS = L.size;
    }
    Color c = L.ask ? t.red : t.green;
    float bw = (float)(L.cum / maxCum) * (area.w * 0.45f);
    u.draw.rect({area.x + area.w - bw, y + 2, bw, rowH - 4}, withAlpha(c, 0.15f));

    u.draw.textAligned({area.x, y, area.w, rowH}, L.priceLbl, c, DrawList::Left, 10);
    u.draw.textAligned({area.x, y, area.w, rowH}, L.sizeLbl, t.text, DrawList::Right,
                       10);
    y += rowH;
  }
}
