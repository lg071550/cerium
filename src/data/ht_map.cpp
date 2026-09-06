#include "ht_map.h"

#include <algorithm>

static void loadLayer(HtLayer& layer, const double* rows, int n, double at, float ref) {
  layer.bands.clear();
  layer.totalUsd = 0;
  layer.fetchedAt = at;
  layer.ref = ref > 0 ? ref : 1;
  if (!rows || n <= 0) return;
  layer.bands.resize((size_t)n);
  for (int i = 0; i < n; ++i) {
    HtBand& b = layer.bands[(size_t)i];
    b.lo = (float)rows[i * 4];
    b.hi = (float)rows[i * 4 + 1];
    b.longUsd = (float)rows[i * 4 + 2];
    b.shortUsd = (float)rows[i * 4 + 3];
    layer.totalUsd += (double)b.longUsd + (double)b.shortUsd;
  }
}

void HtMaps::load(const double* liqRows, int liqN, const double* slRows, int slN, int symIdx,
                  int st, int usedToday, int quotaDay, double liqAt, double slAt, float liqRef,
                  float slRef) {
  if (symIdx != sym) {
    liq.bands.clear();
    sl.bands.clear();
  }
  sym = symIdx;
  status = st;
  used = std::max(0, usedToday);
  quota = quotaDay > 0 ? quotaDay : 100;
  loadLayer(liq, liqRows, liqN, liqAt, liqRef);
  loadLayer(sl, slRows, slN, slAt, slRef);
  ++version;
}

void HtMaps::clear() {
  liq = {};
  sl = {};
  sym = -1;
  status = kNoToken;
  used = 0;
  ++version;
}
