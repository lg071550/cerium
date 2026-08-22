#pragma once

#include "../data/feeds.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"

#include <vector>

// Shared per-venue flow-source picker body: ALL/SPOT/PERP/DEX quick chips that
// select or clear an entire venue class, followed by a class-grouped checklist
// (PERP / SPOT / DEX) of every venue. Toggles `mask` in place and returns true
// when it changed; the caller persists the mask and owns the popover frame.
// Drawn during the host's overlay pass.
inline bool drawFlowSources(Ui& u, Rect r, uint32_t& mask, Feeds& feeds,
                            ListState& list) {
  const Theme& t = theme();
  u.draw.shadow(r, t.radius, 14.0f, 3.0f, hexColor(0x000000, 0.45f));
  u.draw.rect(r, t.panel, t.radius);
  u.draw.rectOutline(r, t.border, 1.0f, t.radius);

  bool changed = false;
  auto classChip = [&](float& x, float y, const char* label, uint32_t cmask,
                       bool active, float w = 0.0f) {
    if (w <= 0) w = u.draw.measure(label) + 18;
    if (chip(u, {x, y, w, 20}, label, active)) {
      mask = active ? (mask & ~cmask) : (mask | cmask);
      changed = true;
    }
    x += w + 4;
  };

  float cx = r.x + 8;
  const float cy = r.y + 6;
  const uint32_t spot = venueMaskForClass(ClassSpot);
  const uint32_t perp = venueMaskForClass(ClassPerp);
  const uint32_t dex = venueMaskForClass(ClassDex);
  classChip(cx, cy, "ALL", kAllVenuesMask, mask == kAllVenuesMask);
  classChip(cx, cy, "SPOT", spot, (mask & spot) == spot);
  classChip(cx, cy, "PERP", perp, (mask & perp) == perp);
  classChip(cx, cy, "DEX", dex, (mask & dex) == dex);

  static constexpr struct {
    uint8_t cls;
    const char* label;
  } kGroups[] = {{ClassPerp, "PERP"}, {ClassSpot, "SPOT"}, {ClassDex, "DEX"}};
  struct Row {
    int venue; // -1 = header
    uint8_t cls;
  };
  // Rebuilt every frame while the popover is open; static scratch avoids the
  // per-frame alloc. Contents depend only on Feeds' venue set, which is fixed
  // after init.
  static thread_local std::vector<Row> rows;
  rows.clear();
  rows.reserve((size_t)kVenueCount + 3);
  for (const auto& g : kGroups) {
    rows.push_back({-1, g.cls});
    for (int i = 0; i < kVenueCount; ++i)
      if (feeds.venues[(size_t)i].cls & g.cls) rows.push_back({i, g.cls});
  }

  Rect listArea{r.x + 4, cy + 30, r.w - 8, r.h - 34};
  listView(u, listArea, (int)rows.size(), 22.0f, list,
           [&](Ui& rowUi, DrawList& d, Rect row, int index) {
             const Row& rr = rows[(size_t)index];
             if (rr.venue < 0) {
               const char* name = rr.cls == ClassPerp ? "PERP"
                                  : rr.cls == ClassSpot ? "SPOT" : "DEX";
               d.textAligned({row.x + 8, row.y, row.w - 16, row.h}, name,
                             withAlpha(t.text, 0.55f), DrawList::Left);
               return;
             }
             const VenueState& v = feeds.venues[(size_t)rr.venue];
             bool on = (mask >> rr.venue) & 1u;
             if (toggle(rowUi, row, v.label.c_str(), on)) {
               mask = on ? (mask | (1u << rr.venue))
                         : (mask & ~(1u << rr.venue));
               changed = true;
             }
           });
  return changed;
}
