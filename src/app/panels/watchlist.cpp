#include "panels.h"
#include "panels_common.h"

#include "../../ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// Canonical symbols matching feeds/registry.ts SYMBOLS + C++ feeds.cpp.
static const char* kSymbols[] = {"ETH", "BTC", "SOL"};
static constexpr int kSymCount = 3;

// Per-symbol venue count (ETH-only venues reduce the total for BTC/SOL).
static constexpr int kVenueCounts[kSymCount] = {27, 24, 24};

void drawWatchlist(Ui& u, Rect r, Feeds& feeds) {
  const Theme& t = theme();

  Rect header{r.x, r.y, r.w, 20};
  u.draw.rect(header, t.panelAlt);
  const float pad = r.w < 140.0f ? 4.0f : 10.0f;
  const float gap = r.w < 140.0f ? 2.0f : 8.0f;
  const float innerW = std::max(0.0f, r.w - pad * 2.0f);
  const float symbolW = innerW * (r.w < 180.0f ? 0.42f : 0.36f);
  const float midW = innerW * (r.w < 180.0f ? 0.34f : 0.32f);
  const float venueW = innerW - symbolW - midW - gap * 2;
  Rect symbolColumn{r.x + pad, header.y, symbolW, header.h};
  Rect midColumn{symbolColumn.x + symbolColumn.w + gap, header.y, midW, header.h};
  Rect venueColumn{midColumn.x + midColumn.w + gap, header.y, venueW, header.h};
  u.draw.textFit(symbolColumn, r.w < 150.0f ? "SYM" : "SYMBOL", t.textDim,
                 DrawList::Left);
  u.draw.textFit(midColumn, "MID", t.textDim, DrawList::Right);
  if (r.w >= 120.0f)
    u.draw.textFit(venueColumn, "VENUES", t.textDim, DrawList::Right);
  u.draw.rect({r.x, r.y + 20, r.w, 1}, t.border);

  float rowH = 24.0f;
  for (int i = 0; i < kSymCount; ++i) {
    Rect row{r.x, r.y + 21 + i * rowH, r.w, rowH};
    bool isActive = i == feeds.symbol;
    if (i % 2) u.draw.rect(row, t.panelAlt);
    if (isActive) {
      u.draw.rect({row.x + 2, row.y + 2, row.w - 4, row.h - 4}, t.accentSoft);
    }

    // Symbol name
    Color nameColor = isActive ? t.accent : t.text;
    u.draw.textFit({symbolColumn.x, row.y, symbolColumn.w, row.h},
                   kSymbols[i], nameColor, DrawList::Left);
    // Pair suffix in dim text
    if (r.w >= 150.0f && i == feeds.symbol) {
      float nameW = u.draw.measure(kSymbols[i]);
      u.draw.textFit({symbolColumn.x + nameW + 4, row.y,
                      symbolColumn.w - nameW - 4, row.h},
                     "/USDT", t.textDim, DrawList::Left);
    }

    // Aggregate mid (only for the active symbol)
    if (isActive) {
      double mid = feeds.aggMid();
      if (mid > 0) {
        char midStr[32];
        snprintf(midStr, sizeof(midStr), "%.*f", 2, mid);
        u.draw.textFit({midColumn.x, row.y, midColumn.w, row.h}, midStr,
                       t.text, DrawList::Right);
      } else {
        u.draw.textFit({midColumn.x, row.y, midColumn.w, row.h}, "—",
                       t.textDim, DrawList::Right);
      }
    } else {
      u.draw.textFit({midColumn.x, row.y, midColumn.w, row.h}, "—",
                     t.textDim, DrawList::Right);
    }

    // Venue count — only meaningful for the active symbol; idle rows would
    // render a misleading "0/N" alarm state.
    if (r.w >= 120.0f) {
      if (!isActive) {
        u.draw.textFit({venueColumn.x, row.y, venueColumn.w, row.h}, "—",
                       t.textDim, DrawList::Right);
      } else {
        int live = 0;
        for (auto& v : feeds.venues)
          if (v.enabled && v.status == wire::Live) ++live;
        char vc[16];
        snprintf(vc, sizeof(vc), "%d/%d", live, kVenueCounts[i]);
        u.draw.textFit({venueColumn.x, row.y, venueColumn.w, row.h}, vc,
                       t.green, DrawList::Right);
      }
    }
  }
}
