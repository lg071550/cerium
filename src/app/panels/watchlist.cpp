#include "panels.h"

#include "../../ui/theme.h"

#include <cstdio>

// deterministic pseudo-random per (i, tick) — drives the fake market data
static float frand(uint32_t i, uint32_t tick) {
  uint32_t h = i * 2654435761u ^ tick * 2246822519u;
  h ^= h >> 13;
  h *= 3266489917u;
  h ^= h >> 16;
  return (float)(h & 0xffff) / 65535.0f;
}

void drawWatchlist(Ui& u, Rect r) {
  const Theme& t = theme();
  static const char* syms[] = {"BTCUSDT", "ETHUSDT", "SOLUSDT", "BNBUSDT", "XRPUSDT",
                               "DOGEUSDT", "ARBUSDT", "LINKUSDT", "AVAXUSDT", "NEARUSDT"};

  Rect header{r.x, r.y, r.w, 20};
  u.draw.textAligned(header, "Symbol", t.textDim, DrawList::Left, 10);
  u.draw.textAligned(header, "Last", t.textDim, DrawList::Right, r.w * 0.35f);
  u.draw.textAligned(header, "24h %", t.textDim, DrawList::Right, 10);
  u.draw.rect({r.x, r.y + 20, r.w, 1}, t.border);

  float rowH = 24.0f;
  for (int i = 0; i < 10; ++i) {
    Rect row{r.x, r.y + 21 + i * rowH, r.w, rowH};
    if (i % 2) u.draw.rect(row, t.panelAlt);

    bool up = frand((uint32_t)i, 11u) > 0.45f;
    Color c = up ? t.green : t.red;

    char price[24], pct[24];
    snprintf(price, sizeof(price), "%.2f",
             100.0 + (double)frand((uint32_t)i, 5u) * 60000.0);
    snprintf(pct, sizeof(pct), "%s%.2f%%", up ? "+" : "-",
             (double)(frand((uint32_t)i, 9u) * 8.0f));

    u.draw.textAligned(row, syms[i], t.text, DrawList::Left, 10);
    u.draw.textAligned(row, price, t.text, DrawList::Right, r.w * 0.35f);
    u.draw.textAligned(row, pct, c, DrawList::Right, 10);
  }
}
