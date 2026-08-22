#include "panels.h"
#include "panels_common.h"

#include "../flow_sources.h"
#include "../../platform/shell.h"
#include "../../ui/theme.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr int kLimits[] = {0, 40, 80, 160};
constexpr int kWindows[] = {5, 15, 60};

double epochMs() {
  using namespace std::chrono;
  return duration<double, std::milli>(system_clock::now().time_since_epoch()).count();
}

// Legacy aliases so existing call sites don't need a mass rename:
// mixRgb → mixColor, shape → shapeDepth, formatValue → formatAmount,
// kRows → kPanelRowHeights, kVisual → kPanelIntensity.
static inline Color mixRgb(Color a, Color b, float t) { return mixColor(a, b, t); }
static inline float shape(float v, int m) { return shapeDepth(v, m); }
static inline void formatValue(double v, bool u, int p, char* o, size_t c) { formatAmount(v, u, p, o, c); }

double niceLot(double raw) {
  if (!(raw > 0) || !std::isfinite(raw)) return 1.0;
  double base = std::pow(10.0, std::floor(std::log10(raw)));
  double f = raw / base;
  double n = f <= 1.0 ? 1.0 : f <= 2.0 ? 2.0 : f <= 5.0 ? 5.0 : 10.0;
  return n * base;
}

int queueTileCapacity(float laneW) {
  const float gap = 1.0f, pad = 1.0f, minW = 18.0f;
  float inner = std::max(0.0f, laneW - pad * 2.0f);
  int maxN = std::max(1, (int)std::floor((inner + gap) / (minW + gap)));
  return std::min(maxN, 22);
}

float residualAge01(double bornAt, float now) {
  if (bornAt <= 0) return 1.0f;
  return std::clamp((now - (float)bornAt) / 45.0f, 0.0f, 1.0f);
}

Color tileColor(bool ask, double bornAt, float now) {
  Color oldC = ask ? hexColor(0x7a2424) : hexColor(0x155652);
  Color newC = ask ? hexColor(0xf4d6d4) : hexColor(0xd4eeea);
  return mixRgb(newC, oldC, residualAge01(bornAt, now));
}

struct QueueTile {
  double size = 0;
  double bornAt = 0;
  bool merged = false;
};

void dropDustTiles(std::vector<QueueTile>& tiles, double minTile) {
  if (tiles.size() <= 1 || !(minTile > 0)) return;
  tiles.erase(std::remove_if(tiles.begin(), tiles.end(),
                             [&](const QueueTile& tile) {
                               return tile.size + 1e-12 < minTile;
                             }),
              tiles.end());
}

void capTiles(std::vector<QueueTile>& tiles, int maxN) {
  maxN = std::max(1, maxN);
  if ((int)tiles.size() <= maxN) return;
  double extra = 0;
  for (size_t i = (size_t)maxN - 1; i < tiles.size(); ++i) extra += tiles[i].size;
  tiles.resize((size_t)maxN - 1);
  tiles.push_back({extra, 0.0, true});
}

void explodeTiles(const std::vector<DomResidual>& q, double lot,
                  std::vector<QueueTile>& out) {
  out.clear();
  if (!(lot > 0)) lot = 1.0;
  for (const DomResidual& r : q) {
    if (!(r.size > 1e-9)) continue;
    if (r.size <= lot * 1.35) {
      out.push_back({r.size, r.bornAt});
      continue;
    }
    int n = std::max(1, (int)std::floor(r.size / lot + 1e-9));
    double rem = r.size - (double)n * lot;
    for (int i = 0; i < n; ++i) out.push_back({lot, r.bornAt});
    if (rem > lot * 0.2)
      out.push_back({rem, r.bornAt});
    else if (!out.empty())
      out.back().size += rem;
  }
}

void buildQueueTiles(const std::vector<DomResidual>& q, double lot, double minTile,
                     int maxN, std::vector<QueueTile>& out) {
  explodeTiles(q, lot, out);
  dropDustTiles(out, minTile);
  capTiles(out, maxN);
}

double medianPositive(std::vector<double>& values) {
  if (values.empty()) return 1.0;
  size_t mid = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + (std::ptrdiff_t)mid, values.end());
  return std::max(1.0, values[mid]);
}

float wallHighlight01(double size, double wallMin, double wallMax) {
  if (!(size + 1e-12 >= wallMin) || !(wallMin > 0)) return 0.0f;
  double span = std::max(wallMax / wallMin, 8.0);
  float t = (float)std::clamp(std::log(size / wallMin) / std::log(span), 0.0, 1.0);
  return t * t * (3.0f - 2.0f * t);
}

void drawOrderTiles(DrawList& d, Rect lane, bool ask,
                    const std::vector<DomResidual>& q, double lot, double minTile,
                    float now, double wallMin, double wallMax) {
  if (lane.w < 8.0f || q.empty()) return;
  static thread_local std::vector<QueueTile> tiles;
  buildQueueTiles(q, lot, minTile, queueTileCapacity(lane.w), tiles);
  if (tiles.empty()) return;
  const float gap = 1.0f;
  const float pad = 1.0f;
  float inner = lane.w - pad * 2.0f;
  int n = (int)tiles.size();
  float tileW = std::min(32.0f, (inner - (float)(n - 1) * gap) / (float)n);
  if (tileW < 8.0f) return;
  float y = lane.y + pad;
  float h = std::max(1.0f, lane.h - pad * 2.0f);
  float cursor = ask ? lane.x + pad : lane.x + lane.w - pad;
  const Theme& th = theme();
  for (int i = 0; i < n; ++i) {
    Rect blk = ask ? Rect{cursor, y, tileW, h} : Rect{cursor - tileW, y, tileW, h};
    float age = residualAge01(tiles[(size_t)i].bornAt, now);
    Color fill = tileColor(ask, tiles[(size_t)i].bornAt, now);
    bool wall = !tiles[(size_t)i].merged && tiles[(size_t)i].size + 1e-12 >= wallMin;
    float punch = wall ? wallHighlight01(tiles[(size_t)i].size, wallMin, wallMax) : 0.0f;
    if (wall) fill = mixRgb(fill, hexColor(0xd6a354), 0.16f + 0.52f * punch);
    d.rect(blk, fill);
    if (tileW >= 16.0f && h >= 12.0f) {
      char label[16];
      formatLot(tiles[(size_t)i].size, label, sizeof(label));
      d.textFit({blk.x + 1, blk.y, blk.w - 2, blk.h}, label,
                (wall || age > 0.45f) ? th.text : hexColor(0x161a1e),
                DrawList::Center);
    }
    if (wall) {
      Color mark = withAlpha(hexColor(0xe8c36a), 0.32f + 0.68f * punch);
      d.rect({blk.x, blk.y, blk.w, 1}, mark);
      d.rect({blk.x, blk.y + blk.h - 1, blk.w, 1}, mark);
      d.rect({blk.x, blk.y, 1, blk.h}, mark);
      d.rect({blk.x + blk.w - 1, blk.y, 1, blk.h}, mark);
    }
    cursor = ask ? cursor + tileW + gap : cursor - tileW - gap;
  }
}

// formatValue, formatLot — from panels_common.h via aliases above

int automaticPriceDecimals(double step) {
  if (!(step > 0)) return 2;
  int d = 0;
  while (d < 8 && std::fabs(step * std::pow(10.0, d) -
                            std::round(step * std::pow(10.0, d))) > 1e-7)
    ++d;
  return d;
}

void normalizeSettings(DomPanel& st) {
  st.venue = std::max(-1, st.venue);
  st.mask &= kAllVenuesMask;
  st.density = std::clamp(st.density, 0, 2);
  st.levelLimit = std::clamp(st.levelLimit, 0, 3);
  st.tradeWindow = std::clamp(st.tradeWindow, 0, 2);
  st.groupMode = std::clamp(st.groupMode, 0, 11);
  st.scaleMode = std::clamp(st.scaleMode, 0, 2);
  st.intensity = std::clamp(st.intensity, 0, 2);
  st.amountPrecision = std::clamp(st.amountPrecision, 0, 3);
  st.pricePrecision = std::clamp(st.pricePrecision, 0, 4);
  if (!(st.customStep > 0) || !std::isfinite(st.customStep)) st.customStep = 1.0;
  st.customStep = DomModel::cleanStep(st.customStep);
}

void saveSettings(DomPanel& st) {
  normalizeSettings(st);
  char buf[512];
  snprintf(buf, sizeof(buf),
           "7,%d,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.12g,%d,%d",
           st.venue, (unsigned)st.mask, st.showUsd, st.showTrades,
           st.showCumulative, st.showVenueCounts, st.showSummary,
           st.showInspector, st.showBars, st.showTexture, st.showEdges,
           st.showFlowFlashes, st.density, st.levelLimit, st.tradeWindow,
           st.groupMode, st.scaleMode, st.intensity, st.amountPrecision,
           st.pricePrecision, st.customStep, st.showQueue ? 1 : 0,
           st.showWalls ? 1 : 0);
  shell_storage_set(st.settingsKey.c_str(), buf);
  st.tickVersion = ~0ull;
  st.model.sourceVersion = ~0ull;
}

void loadSettings(DomPanel& st) {
  if (st.settingsLoaded) return;
  st.settingsLoaded = true;
  st.customStepInput.text = "1";
  char* saved = shell_storage_get(st.settingsKey.c_str());
  if (!saved) return;
  int version = 0, venue = 0, usd = 0, trades = 1, cumulative = 1;
  int venueCounts = 1, summary = 1, inspector = 1, bars = 1, texture = 0;
  int edges = 1, flashes = 1, density = 1, limit = 0, window = 1, grouping = 0;
  int scaling = 0, intensity = 1, amountPrecision = 2, pricePrecision = 2;
  int queue = 1, walls = 1;
  unsigned mask = 0;
  double custom = 1.0;
  int count = sscanf(
      saved,
      "%d,%d,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%lf,%d,%d",
      &version, &venue, &mask, &usd, &trades, &cumulative, &venueCounts,
      &summary, &inspector, &bars, &texture, &edges, &flashes, &density,
      &limit, &window, &grouping, &scaling, &intensity, &amountPrecision,
      &pricePrecision, &custom, &queue, &walls);
  if (version == 1 || version == 2) {
    edges = 1;
    count = sscanf(saved,
                   "%d,%d,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%lf",
                   &version, &venue, &mask, &usd, &trades, &cumulative,
                   &venueCounts, &summary, &inspector, &bars, &texture,
                   &flashes, &density, &limit, &window, &grouping, &scaling,
                   &intensity, &amountPrecision, &pricePrecision, &custom);
  }
  std::free(saved);
  if (!((count == 24 && version == 7) ||
        (count == 23 && version == 6) ||
        (count == 22 && (version == 5 || version == 4 || version == 3)) ||
        (count == 21 && (version == 1 || version == 2))))
    return;
  bool migrate = version < 7;
  if (version < 6) queue = 1;
  if (version < 7) walls = 1;
  if (version == 1 && venue < 0) venue = 0;
  if (version == 1) { texture = 0; scaling = 0; }
  st.venue = venue;
  st.mask = (version <= 3) ? venueMaskForClass((uint8_t)mask) : (uint32_t)mask;
  st.showUsd = usd != 0;
  st.showTrades = trades != 0;
  st.showCumulative = cumulative != 0;
  st.showVenueCounts = venueCounts != 0;
  st.showSummary = summary != 0;
  st.showInspector = inspector != 0;
  st.showBars = bars != 0;
  st.showTexture = texture != 0;
  st.showQueue = queue != 0;
  st.showWalls = walls != 0;
  st.showEdges = edges != 0;
  st.showFlowFlashes = flashes != 0;
  st.density = density;
  st.levelLimit = limit;
  st.tradeWindow = window;
  st.groupMode = grouping;
  // Version 4 used mode 5 for CUSTOM. Preserve that meaning after the new
  // large grouping presets occupy modes 5..10.
  if (version < 5 && st.groupMode == 5) st.groupMode = 11;
  st.scaleMode = scaling;
  st.intensity = intensity;
  st.amountPrecision = amountPrecision;
  st.pricePrecision = pricePrecision;
  st.customStep = DomModel::cleanStep(custom);
  normalizeSettings(st);
  char customBuf[32];
  snprintf(customBuf, sizeof(customBuf), "%.10g", st.customStep);
  st.customStepInput.text = customBuf;
  if (migrate) saveSettings(st);
}

void resetSettings(DomPanel& st) {
  st.venue = 0;
  st.mask = kAllVenuesMask;
  st.showUsd = false;
  st.showTrades = st.showCumulative = st.showVenueCounts = true;
  st.showSummary = st.showInspector = st.showBars = true;
  st.showTexture = false;
  st.showQueue = true;
  st.showWalls = true;
  st.showEdges = true;
  st.showFlowFlashes = true;
  st.density = 1;
  st.levelLimit = 0;
  st.tradeWindow = 1;
  st.groupMode = 0;
  st.scaleMode = 0;
  st.intensity = 1;
  st.amountPrecision = st.pricePrecision = 2;
  st.customStep = 1.0;
  st.customStepInput.text = "1";
  st.customStepError.clear();
  st.settingsList.scroll = 0;
  st.autoCenter = true;
  st.centerOffset = 0;
  st.hasPinned = false;
  st.pinnedPrice = 0;
}

bool applyCustomStep(DomPanel& st) {
  char* end = nullptr;
  double parsed = std::strtod(st.customStepInput.text.c_str(), &end);
  if (!end || end == st.customStepInput.text.c_str() || *end != '\0' ||
      !(parsed > 0) || !std::isfinite(parsed)) {
    st.customStepError = "Enter a positive increment";
    return false;
  }
  st.customStep = parsed;
  st.groupMode = 11;
  st.customStep = DomModel::cleanStep(parsed);
  st.customStepError.clear();
  saveSettings(st);
  return true;
}

void drawSettings(Ui& u, Rect area, DomPanel& st) {
  const Theme& t = theme();
  constexpr int count = 15;
  // Rows can wrap chip lines; anything under ~430px must reserve height for a
  // second line or wrapped controls (PRECISION emits 9 chips ≈ 414px) become
  // unreachable instead of merely taller.
  float rowH = area.w < 110 ? 150.0f : area.w < 190 ? 94.0f
                                          : area.w < 430 ? 68.0f : 44.0f;
  listView(u, area, count, rowH, st.settingsList,
           [&](Ui& rowUi, DrawList& d, Rect row, int index) {
             char id[32]; snprintf(id, sizeof(id), "##dom-setting-%d", index);
             rowUi.pushId(id);
             d.rect({row.x + 10, row.y + row.h - 1, row.w - 20, 1},
                    withAlpha(t.border, 0.65f));
             static const char* titles[] = {
                 "AMOUNT UNIT", "ROW DENSITY", "VISIBLE LEVELS", "COLUMNS",
                 "CHROME", "PRICE GROUPING", "CUSTOM INCREMENT", "TRADE WINDOW",
                 "SOURCES", "DEPTH SCALING", "FLOW", "VISUAL LAYERS",
                 "INTENSITY", "PRECISION", "RESTORE"};
             d.textAligned({row.x + 10, row.y + 2, row.w - 20, 16}, titles[index],
                           t.textDim, DrawList::Left);
             float x = row.x + 10, y = row.y + 19, h = 21;
             const float x0 = x, right = row.x + row.w - 10;
             auto option = [&](const char* label, float width, bool active, auto fn) {
               width = std::min(width, std::max(20.0f, right - x0));
               if (x > x0 && x + width > right) { x = x0; y += 25; }
               if (y + h <= row.y + row.h - 2 &&
                   chip(rowUi, {x, y, width, h}, label, active)) {
                 fn(); saveSettings(st);
               }
               x += width + 4;
             };
             switch (index) {
               case 0:
                 option("COIN", 58, !st.showUsd, [&] { st.showUsd = false; });
                 option("USD", 58, st.showUsd, [&] { st.showUsd = true; });
                 break;
               case 1:
                 option("TIGHT", 64, st.density == 0, [&] { st.density = 0; });
                 option("NORMAL", 70, st.density == 1, [&] { st.density = 1; });
                 option("RELAXED", 76, st.density == 2, [&] { st.density = 2; });
                 break;
               case 2: {
                 const char* labels[] = {"AUTO", "40", "80", "160"};
                 for (int i = 0; i < 4; ++i)
                   option(labels[i], i ? 48.0f : 58.0f, st.levelLimit == i,
                          [&, i] { st.levelLimit = i; });
                 break;
               }
               case 3:
                 option("TRADES", 70, st.showTrades,
                        [&] { st.showTrades = !st.showTrades; });
                 option("CUM", 52, st.showCumulative,
                        [&] { st.showCumulative = !st.showCumulative; });
                 option("VENUES", 68, st.showVenueCounts,
                        [&] { st.showVenueCounts = !st.showVenueCounts; });
                 break;
               case 4:
                 option("SUMMARY", 78, st.showSummary,
                        [&] { st.showSummary = !st.showSummary; });
                 option("INSPECTOR", 86, st.showInspector,
                        [&] { st.showInspector = !st.showInspector; });
                 break;
               case 5: {
                 const char* labels[] = {"AUTO", "1X", "2X", "5X", "10X", "25X", "50X", "100X", "250X", "500X", "1000X", "CUSTOM"};
                 const float widths[] = {58, 46, 46, 46, 50, 54, 54, 58, 58, 58, 64, 72};
                 for (int i = 0; i < 12; ++i)
                   option(labels[i], widths[i], st.groupMode == i,
                          [&, i] { st.groupMode = i; });
                 break;
               }
               case 6: {
                 Rect field{x, y, std::min(112.0f, std::max(40.0f, row.w - 94.0f)), 23};
                 if (textField(rowUi, field, st.customStepInput, "##dom-custom", "0.5"))
                   st.customStepError.clear();
                 if (st.customStepInput.submitted) {
                   st.customStepInput.submitted = false;
                   applyCustomStep(st);
                 }
                 Rect apply{field.x + field.w + 4, y, std::min(58.0f, right - field.x - field.w - 4), 23};
                 if (apply.w > 24 && button(rowUi, apply, "APPLY")) applyCustomStep(st);
                 if (!st.customStepError.empty())
                   d.textFit({x, y + 25, row.w - 20, 18}, st.customStepError.c_str(),
                             t.red, DrawList::Left);
                 break;
               }
               case 7: {
                 const char* labels[] = {"5 SEC", "15 SEC", "60 SEC"};
                 for (int i = 0; i < 3; ++i)
                   option(labels[i], 66, st.tradeWindow == i,
                          [&, i] { st.tradeWindow = i; });
                 break;
               }
               case 8: {
                 const uint32_t spot = venueMaskForClass(ClassSpot);
                 const uint32_t perp = venueMaskForClass(ClassPerp);
                 const uint32_t dex = venueMaskForClass(ClassDex);
                 option("ALL", 48, st.mask == kAllVenuesMask,
                        [&] { st.mask = kAllVenuesMask; });
                 option("SPOT", 58, (st.mask & spot) == spot,
                        [&] { st.mask ^= spot; });
                 option("PERP", 58, (st.mask & perp) == perp,
                        [&] { st.mask ^= perp; });
                 option("DEX", 54, (st.mask & dex) == dex,
                        [&] { st.mask ^= dex; });
                 {
                   float vw = 96.0f;
                   if (x > x0 && x + vw > right) { x = x0; y += 25; }
                   if (y + h <= row.y + row.h - 2) {
                     if (chip(rowUi, {x, y, vw, h}, "VENUES…",
                              u.overlayOpen(st.flowPickerId))) {
                       if (u.overlayOpen(st.flowPickerId)) {
                         u.closeOverlay(st.flowPickerId);
                       } else {
                         if (!st.flowPickerId) st.flowPickerId = u.id("##domflowpicker");
                         float pw = std::min(300.0f, std::max(220.0f, area.w));
                         float ph = std::min(440.0f, std::max(180.0f, area.h - 8.0f));
                         st.flowPickerRect = {
                             area.x + std::max(0.0f, area.w - pw - 6.0f),
                             area.y + 4.0f, pw, ph};
                         st.flowPickerList.scroll = 0;
                         u.openOverlay(st.flowPickerId, st.flowPickerRect);
                         u.input.pressed = false;
                       }
                     }
                   }
                 }
                 break;
               }
               case 9:
                 option("LINEAR", 70, st.scaleMode == 0, [&] { st.scaleMode = 0; });
                 option("SQRT", 58, st.scaleMode == 1, [&] { st.scaleMode = 1; });
                 option("LOG", 54, st.scaleMode == 2, [&] { st.scaleMode = 2; });
                 break;
               case 10:
                 option("FLASHES", 74, st.showFlowFlashes,
                        [&] { st.showFlowFlashes = !st.showFlowFlashes; });
                 break;
               case 11:
                 option("BARS", 58, st.showBars, [&] { st.showBars = !st.showBars; });
                 option("QUEUE", 64, st.showQueue,
                        [&] { st.showQueue = !st.showQueue; });
                 option("WALLS", 64, st.showWalls,
                        [&] { st.showWalls = !st.showWalls; });
                 option("TEXTURE", 76, st.showTexture,
                        [&] { st.showTexture = !st.showTexture; });
                 option("EDGES", 62, st.showEdges,
                        [&] { st.showEdges = !st.showEdges; });
                 break;
               case 12:
                 option("QUIET", 64, st.intensity == 0, [&] { st.intensity = 0; });
                 option("NORMAL", 70, st.intensity == 1, [&] { st.intensity = 1; });
                 option("STRONG", 70, st.intensity == 2, [&] { st.intensity = 2; });
                 break;
               case 13: {
                 const char* amount[] = {"A0", "A2", "A3", "A5"};
                 for (int i = 0; i < 4; ++i)
                   option(amount[i], 42, st.amountPrecision == i,
                          [&, i] { st.amountPrecision = i; });
                 for (int i = 0; i < 5; ++i) {
                   char label[8]; snprintf(label, sizeof(label), "P%d", i);
                   option(label, 42, st.pricePrecision == i,
                          [&, i] { st.pricePrecision = i; });
                 }
                 break;
               }
               case 14:
                 if (button(rowUi, {x, y, std::min(142.0f, row.w - 20), 23},
                            "RESET TO DEFAULTS")) {
                   resetSettings(st); saveSettings(st);
                 }
                 break;
             }
             rowUi.popId();
           });
}

bool containsInsensitive(const std::string& text, const std::string& query) {
  if (query.empty()) return true;
  auto lower = [](char c) { return (char)std::tolower((unsigned char)c); };
  for (size_t i = 0; i + query.size() <= text.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < query.size(); ++j)
      if (lower(text[i + j]) != lower(query[j])) { match = false; break; }
    if (match) return true;
  }
  return false;
}
} // namespace

void DomPanel::drawVenuePicker(Ui& u, Feeds& feeds) {
  if (!u.overlayOpen(venuePickerId)) {
    if (venueSearch.focused) { venueSearch.focused = false; shell_ime_blur(); }
    return;
  }
  const Theme& t = theme();
  Rect r = venuePickerRect;
  u.draw.shadow(r, 2.0f, 14.0f, 3.0f, hexColor(0x000000, 0.45f));
  u.draw.rect(r, t.panel, 2.0f);
  u.draw.rectOutline(r, t.border, 1.0f, 2.0f);
  Rect field{r.x + 8, r.y + 8, r.w - 16, 24};
  if (autoFocusVenue) {
    shell_ime_set(""); shell_ime_focus(field.x, field.y, field.w, field.h);
    venueSearch.focused = true; autoFocusVenue = false;
  }
  textField(u, field, venueSearch, "##dom-venue-search", "Filter venues…");

  filteredVenues.clear();
  filteredVenues.push_back(-1);
  for (int i = 0; i < (int)feeds.venues.size(); ++i) {
    const VenueState& v = feeds.venues[(size_t)i];
    if (containsInsensitive(v.label, venueSearch.text) ||
        containsInsensitive(v.shortLabel, venueSearch.text))
      filteredVenues.push_back(i);
  }
  Rect list{r.x + 4, r.y + 38, r.w - 8, r.h - 42};
  listView(u, list, (int)filteredVenues.size(), 24.0f, venueList,
           [&](Ui& rowUi, DrawList& d, Rect row, int index) {
             int v = filteredVenues[(size_t)index];
             bool active = venue == v;
             bool hov = rowUi.hovered(row);
             if (active) d.rect(row, t.bgRaised, 1.0f);
             else if (hov) d.rect(row, t.bgHover, 1.0f);
             const char* name = v < 0 ? "X-VENUE ANALYTICS"
                                      : feeds.venues[(size_t)v].label.c_str();
             d.textFit(row, name, active ? t.accent : t.text, DrawList::Left, 8);
             if (v < 0) {
               d.textAligned(row, "NORMALIZED", t.textDim, DrawList::Right, 8);
             } else {
               const VenueState& state = feeds.venues[(size_t)v];
               const char* status = state.status == wire::Live ? "LIVE"
                                    : state.status == wire::Syncing ? "SYNCING"
                                    : state.status == wire::Connecting ? "CONNECTING"
                                    : state.status == wire::Reconnecting ? "RECONNECT"
                                    : state.status == wire::Error ? "ERROR" : "OFFLINE";
               d.textAligned(row, status, state.status == wire::Live ? t.green : t.textDim,
                             DrawList::Right, 8);
             }
              // Press-inside guard via the canonical primitive: a drag that
              // started elsewhere must not commit when it releases over a row.
              Behavior rowB = behavior(
                  rowUi, row,
                  rowUi.id("##dom-vrow") +
                      (uint64_t)(index + 1) * 0x9E3779B97F4A7C15ull);
              if (rowB.clicked) {
                venue = v; autoCenter = true; centerOffset = 0; hasPinned = false;
                tickVersion = ~0ull; saveSettings(*this);
                rowUi.closeOverlay(venuePickerId);
                rowUi.input.released = false;
              }
           });
}

void drawDom(Ui& u, Rect r, DomPanel& st, Feeds& feeds) {
  const Theme& t = theme();
  loadSettings(st);
  if (st.venue >= (int)feeds.venues.size()) st.venue = -1;
  if (st.hasPinned && st.pinnedSymbol != feeds.symbol) st.hasPinned = false;
  if (!st.venuePickerId) st.venuePickerId = u.id("##dom-venue-picker");

  Rect toolbar{r.x, r.y, r.w, 23};
  u.draw.rect(toolbar, t.panelAlt);
  bool microToolbar = r.w < 180.0f;
  float edge = microToolbar ? 2.0f : 5.0f;
  float gap = microToolbar ? 2.0f : 4.0f;
  float settingsW = r.w >= 420 ? 70.0f : r.w >= 180 ? 48.0f : 26.0f;
  Rect settingsR{r.x + r.w - settingsW - edge, r.y + 2, settingsW, 18};
  float centerW = r.w >= 300 ? 58.0f : r.w >= 180 ? 42.0f : 26.0f;
  Rect centerR{settingsR.x - centerW - gap, r.y + 2, centerW, 18};
  float groupW = r.w >= 360 ? 68.0f : r.w >= 180 ? 46.0f : 28.0f;
  Rect groupR{centerR.x - groupW - gap, r.y + 2, groupW, 18};
  float venueW = std::min(180.0f, std::max(12.0f, groupR.x - r.x - edge * 2));
  Rect venueR{r.x + edge, r.y + 2, venueW, 18};

  const char* venueLabel = st.venue < 0 ? "X-VENUE"
                           : feeds.venues[(size_t)st.venue].shortLabel.c_str();
  if (chip(u, venueR, venueLabel, u.overlayOpen(st.venuePickerId))) {
    if (u.overlayOpen(st.venuePickerId)) u.closeOverlay(st.venuePickerId);
    else {
      float pw = std::min(280.0f, std::max(180.0f, r.w));
      float ph = std::min(430.0f, std::max(150.0f, r.h - 28.0f));
      st.venuePickerRect = {std::clamp(venueR.x, r.x, r.x + r.w - pw), r.y + 24, pw, ph};
      u.openOverlay(st.venuePickerId, st.venuePickerRect);
      u.input.pressed = false;
      st.venueSearch.text.clear(); st.venueList.scroll = 0; st.autoFocusVenue = true;
    }
  }

  double now = epochMs();
  uint64_t books = DomModel::sourceSignature(feeds, st.venue, now);
  if (books != st.tickVersion || st.venue != st.tickVenue || st.mask != st.tickMask) {
    st.inferredStep = DomModel::inferTick(feeds, st.venue, st.mask, now);
    st.tickVersion = books; st.tickVenue = st.venue; st.tickMask = st.mask;
  }
  static constexpr double factors[] = {
      1, 1, 2, 5, 10, 25, 50, 100, 250, 500, 1000};
  st.effectiveStep = st.groupMode == 11
                         ? st.customStep
                         : st.inferredStep * factors[std::clamp(st.groupMode, 0, 10)];
  st.effectiveStep = DomModel::cleanStep(st.effectiveStep);
  if (!(st.effectiveStep > 0)) st.effectiveStep = 0.01;

  char group[32];
  if (st.groupMode == 0) snprintf(group, sizeof(group), "AUTO %.6g", st.effectiveStep);
  else snprintf(group, sizeof(group), "%.6g", st.effectiveStep);
  if (chip(u, groupR, r.w >= 360 ? group : microToolbar ? "G" : "STEP",
           st.groupMode != 0)) {
    st.groupMode = (st.groupMode + 1) % 11;
    if (st.autoCenter) st.centerOffset = 0;
    saveSettings(st);
  }
  if (chip(u, centerR, r.w >= 300 ? "CENTER" : microToolbar ? "C" : "CTR",
           !st.autoCenter)) {
    st.autoCenter = true; st.centerOffset = 0;
  }
  Behavior settingsB = behavior(u, settingsR, u.id("##dom-settings"));
  if (settingsB.hovered || settingsB.held || st.settingsOpen)
    u.draw.rect(settingsR, settingsB.held || st.settingsOpen ? t.accentSoft : t.bgHover);
  u.draw.textAligned(settingsR, st.settingsOpen ? (microToolbar ? "D" : "DONE")
                                                : r.w >= 420 ? "SETTINGS"
                                                : microToolbar ? "S" : "SET",
                     st.settingsOpen ? t.accent : t.textDim, DrawList::Center);
  if (settingsB.clicked) st.settingsOpen = !st.settingsOpen;
  u.draw.rect({r.x, r.y + 22, r.w, 1}, t.border);
  if (st.settingsOpen) {
    drawSettings(u, {r.x, r.y + 23, r.w, r.h - 23}, st);
    return;
  }

  uint64_t modelKey = DomModel::sourceSignature(feeds, st.venue, now);
  // Visible tick band (stale-by-a-frame mid is fine; the model unions
  // mid-near anyway). The band rides the rebuild gate so scrolling never
  // shows unmaterialized rows.
  const int64_t bandMid = st.model.summary.mid > 0
      ? (int64_t)std::llround(st.model.summary.mid / st.effectiveStep)
      : 0;
  const int64_t bandCenter = bandMid + st.centerOffset;
  const int64_t bandHalf =
      std::max(1, (int)(r.h / kPanelRowHeights[std::clamp(st.density, 0, 2)])) / 2 + 2;
  const int64_t reqLo = bandCenter - bandHalf;
  const int64_t reqHi = bandCenter + bandHalf;
  bool domFilterChanged = st.model.step != st.effectiveStep ||
                          st.tickVenue != st.venue || st.tickMask != st.mask;
  if (domFilterChanged || st.model.sourceVersion != modelKey ||
      !st.model.bandCovers(reqLo, reqHi)) {
    st.model.rebuild(feeds, st.venue, st.mask, st.effectiveStep, u.time,
                      now, reqLo, reqHi);
    st.model.sourceVersion = modelKey;
  }
  st.model.updateTrades(feeds, st.venue, st.mask, st.effectiveStep,
                        (double)kWindows[std::clamp(st.tradeWindow, 0, 2)], now);

  float y = r.y + 23;
  float summaryH = st.showSummary && r.h >= 100 ? 20.0f : 0.0f;
  if (summaryH > 0) {
    Rect summary{r.x, y, r.w, summaryH};
    u.draw.rect(summary, t.panelAlt);
    char metrics[256];
    const DomSummary& s = st.model.summary;
    if (s.bestBid <= 0 || s.bestAsk <= 0) {
      snprintf(metrics, sizeof(metrics), "%s  /  waiting for depth",
               st.venue < 0 ? "CONSOLIDATED" : venueLabel);
    } else if (s.crossed) {
      double bps = (s.bestBid - s.bestAsk) / std::max(s.mid, 1e-9) * 1e4;
      snprintf(metrics, sizeof(metrics), "B %.2f  A %.2f  CROSS %.2f bps  IMB %+.0f%%  %d SRC",
               s.bestBid, s.bestAsk, bps, s.imbalance * 100.0, s.sourceCount);
    } else if (r.w >= 900) {
      double spreadBps = (s.bestAsk - s.bestBid) / std::max(s.mid, 1e-9) * 1e4;
      snprintf(metrics, sizeof(metrics),
               "B %.2f  A %.2f  SPR %.2f bps  MID %.2f  MICRO %.2f  LAST %.2f  IMB %+.0f%%  %d SRC",
               s.bestBid, s.bestAsk, spreadBps, s.mid, s.microprice,
               st.model.lastTradePrice, s.imbalance * 100.0, s.sourceCount);
    } else if (r.w >= 620) {
      double spreadBps = (s.bestAsk - s.bestBid) / std::max(s.mid, 1e-9) * 1e4;
      snprintf(metrics, sizeof(metrics),
               "B %.2f  A %.2f  SPR %.2f bps  MID %.2f  IMB %+.0f%%  %d SRC",
               s.bestBid, s.bestAsk, spreadBps, s.mid,
               s.imbalance * 100.0, s.sourceCount);
    } else {
      snprintf(metrics, sizeof(metrics), "MID %.2f  SPR %.2f  IMB %+.0f%%  %d SRC",
               s.mid, s.bestAsk - s.bestBid, s.imbalance * 100.0, s.sourceCount);
    }
    if (r.w >= 900 && (st.model.recentFlow.buy > 0 || st.model.recentFlow.sell > 0)) {
      char buy[24], sell[24], flow[64];
      formatValue(st.showUsd ? st.model.recentFlow.buy * s.mid : st.model.recentFlow.buy,
                  st.showUsd, st.amountPrecision, buy, sizeof(buy));
      formatValue(st.showUsd ? st.model.recentFlow.sell * s.mid : st.model.recentFlow.sell,
                  st.showUsd, st.amountPrecision, sell, sizeof(sell));
      snprintf(flow, sizeof(flow), "  FLOW +%s / -%s", buy, sell);
      strncat(metrics, flow, sizeof(metrics) - strlen(metrics) - 1);
    }
    Color statusColor = t.textDim;
    char line[320];
    if (st.venue >= 0) {
      const VenueState& venue = feeds.venues[(size_t)st.venue];
      bool healthy[64]{};
      feeds.collectHealthy(50.0, healthy);
      bool stale = venue.bookUpdatedAtMs > 0 && now - venue.bookUpdatedAtMs > 3000.0;
      const char* status = !venue.enabled ? "DISABLED"
                           : venue.status == wire::Connecting ? "CONNECTING"
                           : venue.status == wire::Syncing ? "SYNCING"
                           : venue.status == wire::Reconnecting ? "RECONNECTING"
                           : venue.status == wire::Error ? "ERROR"
                           : venue.status != wire::Live ? "OFFLINE"
                           : stale ? "STALE"
                           : !healthy[st.venue] ? "OUTLIER" : "LIVE";
      bool warning = !venue.enabled || venue.status != wire::Live || stale ||
                     !healthy[st.venue];
      statusColor = warning ? (venue.status == wire::Live ? hexColor(0xd6a354) : t.red)
                            : t.textDim;
      snprintf(line, sizeof(line), "%s  %s", status, metrics);
    } else {
      snprintf(line, sizeof(line), "ANALYTICS  %s", metrics);
    }
    u.draw.textFit(summary, line, statusColor, DrawList::Left, 8);
    u.draw.rect({summary.x, summary.y + summary.h - 1, summary.w, 1}, t.border);
    y += summaryH;
  }

  bool wide = r.w >= 760.0f;
  bool showTrades = st.showTrades && r.w >= 480.0f;
  bool showCum = st.showCumulative && r.w >= 620.0f;
  bool showVenues = st.showVenueCounts && wide;
  float headerH = r.h >= 72 ? 18.0f : 0.0f;
  float inspectorH = st.showInspector && r.h >= 140 && (st.hasPinned || st.hasHover)
                         ? 27.0f : 0.0f;
  Rect header{r.x, y, r.w, headerH};
  if (headerH > 0) u.draw.rect(header, t.panelAlt);
  y += headerH;
  Rect rowsArea{r.x, y, r.w, std::max(0.0f, r.y + r.h - y - inspectorH)};

  float priceW = std::min(84.0f, std::max(40.0f, r.w * 0.15f));
  priceW = std::min(priceW, r.w * 0.5f);
  float tradeW = showTrades ? 58.0f : 0.0f;
  float venueCountW = showVenues ? 56.0f : 0.0f;
  float cumW = showCum ? 62.0f : 0.0f;
  float leftover = std::max(0.0f, r.w - priceW - 2 * (tradeW + venueCountW + cumW));
  float side = leftover * 0.5f;
  float sizeW = side, laneW = 0.0f;
  if (st.showQueue && side > 148.0f) {
    laneW = std::min(168.0f, std::max(72.0f, side * 0.30f));
    sizeW = side - laneW;
  }
  float totalW = priceW + 2 * (tradeW + venueCountW + cumW + sizeW + laneW);
  float startX = r.x + std::max(0.0f, (r.w - totalW) * 0.5f);
  auto take = [&](float w) { Rect cell{startX, header.y, w, header.h}; startX += w; return cell; };
  Rect buyTrade = take(tradeW), bidVenues = take(venueCountW), bidCum = take(cumW);
  Rect bidLane = take(laneW);
  Rect bidSize = take(sizeW);
  Rect price = take(priceW);
  Rect askSize = take(sizeW);
  Rect askLane = take(laneW);
  Rect askCum = take(cumW), askVenues = take(venueCountW), sellTrade = take(tradeW);
  auto rowCell = [](Rect col, float rowY, float rowH) {
    return Rect{col.x, rowY, col.w, rowH};
  };
  if (headerH > 0) {
    if (showTrades) u.draw.textAligned(buyTrade, "BUY VOL", t.textDim, DrawList::Right, 4);
    if (showVenues) u.draw.textAligned(bidVenues, "VENUES", t.textDim, DrawList::Right, 3);
    if (showCum) u.draw.textAligned(bidCum, "BID CUM", t.textDim, DrawList::Right, 4);
    if (laneW > 8)
      u.draw.textAligned(bidLane, "ORDERS", t.textDim, DrawList::Right, 4);
    u.draw.textAligned(bidSize, r.w < 180 ? "BID" : "BID SIZE",
                       t.textDim, DrawList::Right, r.w < 180 ? 1 : 4);
    u.draw.textAligned(price, r.w < 180 ? "PX" : "PRICE", t.textDim,
                       DrawList::Center);
    u.draw.textAligned(askSize, r.w < 180 ? "ASK" : "ASK SIZE",
                       t.textDim, DrawList::Left, r.w < 180 ? 1 : 4);
    if (laneW > 8)
      u.draw.textAligned(askLane, "ORDERS", t.textDim, DrawList::Left, 4);
    if (showCum) u.draw.textAligned(askCum, "ASK CUM", t.textDim, DrawList::Left, 4);
    if (showVenues) u.draw.textAligned(askVenues, "VENUES", t.textDim, DrawList::Left, 3);
    if (showTrades) u.draw.textAligned(sellTrade, "SELL VOL", t.textDim, DrawList::Left, 4);
    u.draw.rect({header.x, header.y + header.h - 1, header.w, 1}, t.border);
  }

  if (st.model.summary.mid <= 0 || rowsArea.h < 10) {
    u.draw.textAligned(rowsArea, st.mask == 0 && st.venue < 0
                                     ? "select at least one source"
                                     : "syncing depth…",
                       t.textDim, DrawList::Center);
    return;
  }

  float rowH = kPanelRowHeights[std::clamp(st.density, 0, 2)];
  int capacity = std::max(1, (int)(rowsArea.h / rowH));
  int configured = kLimits[std::clamp(st.levelLimit, 0, 3)];
  int rowCount = configured > 0 ? std::min(capacity, configured) : capacity;
  if (u.hovered(rowsArea) && u.input.wheelY != 0) {
    st.centerOffset -= (int64_t)std::lround(u.input.wheelY / 40.0f);
    st.autoCenter = false;
  }
  // Escape priority: a focused text field owns the press (it blurs); the pin
  // unhooks only when no field is focused.
  if (u.input.escapePressed && u.focusedField == 0 && st.hasPinned)
    st.hasPinned = false;
  int64_t midTick = (int64_t)std::llround(st.model.summary.mid / st.effectiveStep);
  int64_t centerTick = midTick + st.centerOffset;
  int64_t topTick = centerTick + rowCount / 2;

  double maxSize = 1e-12, maxFlow = 1e-12;
  std::vector<DomResidual> bidQ, askQ;
  for (int row = 0; row < rowCount; ++row) {
    int64_t tick = topTick - row;
    const DomBucket* b = st.model.bucket(tick);
    DomFlow f = st.model.flow(tick);
    if (b) {
      maxSize = std::max({maxSize, b->bid, b->ask});
    }
    maxFlow = std::max({maxFlow, f.buy, f.sell});
  }

  st.hasHover = false;
  float visual = kPanelIntensity[std::clamp(st.intensity, 0, 2)];
  Color pull = hexColor(0xd6a354);
  int tileSlots = queueTileCapacity(laneW);
  double lot = niceLot(std::max(maxSize / 18.0, 0.1));
  double minTile = 1.0;
  std::vector<double> tileSizes;
  std::vector<QueueTile> built;
  if (st.showWalls) {
    tileSizes.reserve((size_t)rowCount * 6);
    for (int row = 0; row < rowCount; ++row) {
      int64_t tick = topTick - row;
      for (int side = 0; side < 2; ++side) {
        st.model.residuals(tick, side != 0, side ? askQ : bidQ);
        buildQueueTiles(side ? askQ : bidQ, lot, minTile, tileSlots, built);
        for (const QueueTile& tile : built)
          if (!tile.merged && tile.size >= 1.0) tileSizes.push_back(tile.size);
      }
    }
  }
  double wallMin = st.showWalls ? std::max(8.0, medianPositive(tileSizes) * 5.0)
                                : 1e300;
  double wallMax = wallMin;
  if (st.showWalls) {
    for (double size : tileSizes)
      if (size + 1e-12 >= wallMin) wallMax = std::max(wallMax, size);
  }
  int priceDecimals = st.pricePrecision == 0
                          ? automaticPriceDecimals(st.effectiveStep)
                          : st.pricePrecision;
  for (int row = 0; row < rowCount; ++row) {
    int64_t tick = topTick - row;
    float ry = rowsArea.y + row * rowH;
    Rect whole{r.x, ry, r.w, rowH};
    const DomBucket* b = st.model.bucket(tick);
    DomFlow flow = st.model.flow(tick);
    DomChange change = st.model.change(tick);
    double p = (double)tick * st.effectiveStep;
    bool bestBidRow = std::fabs(p - st.model.summary.bestBid) < st.effectiveStep * 0.51;
    bool bestAskRow = std::fabs(p - st.model.summary.bestAsk) < st.effectiveStep * 0.51;
    bool lastRow = st.model.lastTradePrice > 0 &&
                   std::fabs(p - st.model.lastTradePrice) < st.effectiveStep * 0.51;
    bool spreadRow = !st.model.summary.crossed && p > st.model.summary.bestBid &&
                     p < st.model.summary.bestAsk;
    if (st.showWalls || (st.showQueue && laneW > 8.0f)) {
      st.model.residuals(tick, false, bidQ);
      st.model.residuals(tick, true, askQ);
    }
    if (bestBidRow) u.draw.rect(whole, withAlpha(t.green, 0.035f));
    if (bestAskRow) u.draw.rect(whole, withAlpha(t.red, 0.035f));
    bool hov = u.hovered(whole);
    if (hov) {
      st.hasHover = true; st.hoverTick = tick;
      u.draw.rect(whole, t.bgHover);
      if (u.input.released) {
        if (st.hasPinned && st.pinnedTick == tick) st.hasPinned = false;
        else {
          st.hasPinned = true; st.pinnedTick = tick; st.pinnedSymbol = feeds.symbol;
          st.pinnedVenue = st.venue; st.pinnedPrice = p;
        }
        u.input.released = false;
      }
    }
    if (st.hasPinned && st.pinnedTick == tick)
      u.draw.rect(whole, withAlpha(t.accent, 0.055f * visual));

    Rect bidR = rowCell(bidSize, ry, rowH), askR = rowCell(askSize, ry, rowH);
    Rect priceR = rowCell(price, ry, rowH);
    u.draw.rect(priceR, spreadRow ? t.bgRaised : t.panelAlt);
    double bid = b ? b->bid : 0, ask = b ? b->ask : 0;
    double previousBid = std::max(0.0, bid - change.bid);
    double previousAsk = std::max(0.0, ask - change.ask);
    float bidFade = std::clamp(1.0f - (u.time - (float)change.bidAt) / 0.8f,
                               0.0f, 1.0f);
    float askFade = std::clamp(1.0f - (u.time - (float)change.askAt) / 0.8f,
                               0.0f, 1.0f);
    bool bidEvent = st.showFlowFlashes && bidFade > 0 &&
                    std::fabs(change.bid) >=
                        std::max(1e-9, std::max(bid, previousBid) * 0.005);
    bool askEvent = st.showFlowFlashes && askFade > 0 &&
                    std::fabs(change.ask) >=
                        std::max(1e-9, std::max(ask, previousAsk) * 0.005);
    float bidW = shape((float)(bid / maxSize), st.scaleMode) * bidR.w * 0.78f;
    float askW = shape((float)(ask / maxSize), st.scaleMode) * askR.w * 0.78f;
    if (st.showBars && bid > 0) {
      Rect bar{bidR.x + bidR.w - bidW, ry, bidW, rowH};
      u.draw.rectGradientHDithered(bar, withAlpha(t.green, 0.01f * visual),
                                  withAlpha(t.green, 0.12f * visual));
      if (st.showTexture && bidW > 18)
        for (float tx = bar.x + 7; tx < bar.x + bar.w; tx += 8)
          u.draw.rect({tx, ry + 3, 1, rowH - 6}, withAlpha(t.bg, 0.035f));
    }
    if (st.showBars && ask > 0) {
      Rect bar{askR.x, ry, askW, rowH};
      u.draw.rectGradientHDithered(bar, withAlpha(t.red, 0.12f * visual),
                                  withAlpha(t.red, 0.01f * visual));
      if (st.showTexture && askW > 18)
        for (float tx = bar.x + 7; tx < bar.x + bar.w; tx += 8)
          u.draw.rect({tx, ry + 3, 1, rowH - 6}, withAlpha(t.bg, 0.035f));
    }
    if (st.showQueue && laneW > 8.0f) {
      if (bid > 0 && !bidQ.empty())
        drawOrderTiles(u.draw, rowCell(bidLane, ry, rowH), false, bidQ, lot,
                       minTile, u.time, wallMin, wallMax);
      if (ask > 0 && !askQ.empty())
        drawOrderTiles(u.draw, rowCell(askLane, ry, rowH), true, askQ, lot,
                       minTile, u.time, wallMin, wallMax);
    }

    // Outline only the exposed perimeter of each connected depth silhouette.
    // Neighboring levels share their overlap, so this adds definition without
    // reintroducing a horizontal seam through every DOM row.
    if (st.showBars && st.showEdges) {
      auto neighborWidth = [&](int neighborRow, bool bidSide) {
        if (neighborRow < 0 || neighborRow >= rowCount) return 0.0f;
        int64_t neighborTick = topTick - neighborRow;
        const DomBucket* neighbor = st.model.bucket(neighborTick);
        double value = neighbor ? (bidSide ? neighbor->bid : neighbor->ask) : 0.0;
        float width = bidSide ? bidR.w : askR.w;
        return shape((float)(value / maxSize), st.scaleMode) * width * 0.78f;
      };
      float bidAbove = neighborWidth(row - 1, true);
      float bidBelow = neighborWidth(row + 1, true);
      float askAbove = neighborWidth(row - 1, false);
      float askBelow = neighborWidth(row + 1, false);

      if (bid > 0 && bidW >= 1.0f) {
        float left = bidR.x + bidR.w - bidW;
        float right = bidR.x + bidR.w;
        Color edgeColor = withAlpha(t.green, 0.30f * visual);
        u.draw.rect({left, ry, 1.0f, rowH}, edgeColor);
        u.draw.rect({right - 1.0f, ry, 1.0f, rowH}, edgeColor);
        if (bidW > bidAbove)
          u.draw.rect({left, ry, bidW - bidAbove, 1.0f}, edgeColor);
        if (bidW > bidBelow)
          u.draw.rect({left, ry + rowH - 1.0f, bidW - bidBelow, 1.0f},
                      edgeColor);
      }
      if (ask > 0 && askW >= 1.0f) {
        float left = askR.x;
        float right = askR.x + askW;
        Color edgeColor = withAlpha(t.red, 0.30f * visual);
        u.draw.rect({left, ry, 1.0f, rowH}, edgeColor);
        u.draw.rect({right - 1.0f, ry, 1.0f, rowH}, edgeColor);
        if (askW > askAbove)
          u.draw.rect({left + askAbove, ry, askW - askAbove, 1.0f},
                      edgeColor);
        if (askW > askBelow)
          u.draw.rect({left + askBelow, ry + rowH - 1.0f,
                       askW - askBelow, 1.0f},
                      edgeColor);
      }
    }

    // Show the actual changed extent. Adds illuminate the newly occupied bar
    // segment; pulls leave a fading amber ghost where liquidity disappeared.
    // Size taken by prints is not a pull — those hits are stripped in the model
    // so a sweep does not light the inside of every ask bar orange.
    if (bidEvent) {
      float oldW = shape((float)(previousBid / maxSize), st.scaleMode) *
                   bidR.w * 0.78f;
      float lo = std::min(oldW, bidW), hi = std::max(oldW, bidW);
      if (st.showBars && hi - lo > 0.5f)
        u.draw.rect({bidR.x + bidR.w - hi, ry + 2, hi - lo, rowH - 4},
                    withAlpha(change.bid > 0 ? t.green : pull,
                              bidFade * (change.bid > 0 ? 0.36f : 0.52f) * visual));
      float edgeX = bidR.x + bidR.w - bidW;
      u.draw.rect({edgeX - 1, ry + 1, 3, rowH - 2},
                  withAlpha(change.bid > 0 ? t.green : pull,
                            bidFade * 0.82f * visual));
    }
    if (askEvent) {
      float oldW = shape((float)(previousAsk / maxSize), st.scaleMode) *
                   askR.w * 0.78f;
      float lo = std::min(oldW, askW), hi = std::max(oldW, askW);
      if (st.showBars && hi - lo > 0.5f)
        u.draw.rect({askR.x + lo, ry + 2, hi - lo, rowH - 4},
                    withAlpha(change.ask > 0 ? t.red : pull,
                              askFade * (change.ask > 0 ? 0.36f : 0.52f) * visual));
      float edgeX = askR.x + askW;
      u.draw.rect({edgeX - 1, ry + 1, 3, rowH - 2},
                  withAlpha(change.ask > 0 ? t.red : pull,
                            askFade * 0.82f * visual));
    }
    if (showTrades) {
      Rect buyR = rowCell(buyTrade, ry, rowH), sellR = rowCell(sellTrade, ry, rowH);
      if (flow.buy > 0)
        u.draw.rect(buyR, withAlpha(t.green,
                                   (0.03f + 0.20f * shape((float)(flow.buy / maxFlow), 1)) * visual));
      if (flow.sell > 0)
        u.draw.rect(sellR, withAlpha(t.red,
                                    (0.03f + 0.20f * shape((float)(flow.sell / maxFlow), 1)) * visual));
    }

    char priceText[32], bidText[32] = {}, askText[32] = {};
    snprintf(priceText, sizeof(priceText), "%.*f", priceDecimals, p);
    if (bid > 0) formatValue(st.showUsd ? bid * p : bid, st.showUsd,
                             st.amountPrecision, bidText, sizeof(bidText));
    if (ask > 0) formatValue(st.showUsd ? ask * p : ask, st.showUsd,
                             st.amountPrecision, askText, sizeof(askText));
    Color bidTextColor = bidEvent ? (change.bid > 0 ? t.green : pull)
                                  : bid > 0 ? t.text : t.textDim;
    Color askTextColor = askEvent ? (change.ask > 0 ? t.red : pull)
                                  : ask > 0 ? t.text : t.textDim;
    u.draw.textFit(bidR, bidText, bidTextColor, DrawList::Right, 4);
    if (bestBidRow)
      u.draw.rect({priceR.x, priceR.y, 2, priceR.h}, withAlpha(t.green, 0.82f));
    if (bestAskRow)
      u.draw.rect({priceR.x + priceR.w - 2, priceR.y, 2, priceR.h},
                  withAlpha(t.red, 0.82f));
    if (lastRow)
      u.draw.rect({priceR.x + priceR.w * 0.5f - 1, priceR.y, 2, priceR.h},
                  withAlpha(t.accent, 0.85f));
    u.draw.textAligned(priceR, priceText,
                       bestBidRow ? t.green : bestAskRow ? t.red
                       : lastRow ? t.accent : t.text,
                       DrawList::Center);
    u.draw.textFit(askR, askText, askTextColor, DrawList::Left, 4);

    if (showCum) {
      char bc[24] = {}, ac[24] = {};
      if (b) {
        if (b->bid > 0)
          formatValue(st.showUsd ? b->bidCumUsd : b->bidCum, st.showUsd,
                      st.amountPrecision, bc, sizeof(bc));
        if (b->ask > 0)
          formatValue(st.showUsd ? b->askCumUsd : b->askCum, st.showUsd,
                      st.amountPrecision, ac, sizeof(ac));
      }
      u.draw.textFit(rowCell(bidCum, ry, rowH), bc, t.textDim, DrawList::Right, 4);
      u.draw.textFit(rowCell(askCum, ry, rowH), ac, t.textDim, DrawList::Left, 4);
    }
    if (showVenues) {
      char bv[24] = {}, av[24] = {};
      if (b && b->bidVenues) {
        const char* top = b->topBidVenue >= 0
                              ? feeds.venues[(size_t)b->topBidVenue].shortLabel.c_str() : "";
        snprintf(bv, sizeof(bv), "%s %u", top, b->bidVenues);
      }
      if (b && b->askVenues) {
        const char* top = b->topAskVenue >= 0
                              ? feeds.venues[(size_t)b->topAskVenue].shortLabel.c_str() : "";
        snprintf(av, sizeof(av), "%u %s", b->askVenues, top);
      }
      u.draw.textAligned(rowCell(bidVenues, ry, rowH), bv, t.textDim, DrawList::Right, 3);
      u.draw.textAligned(rowCell(askVenues, ry, rowH), av, t.textDim, DrawList::Left, 3);
    }
    if (showTrades) {
      char buy[24] = {}, sell[24] = {};
      if (flow.buy > 0) formatValue(st.showUsd ? flow.buy * p : flow.buy, st.showUsd,
                                    st.amountPrecision, buy, sizeof(buy));
      if (flow.sell > 0) formatValue(st.showUsd ? flow.sell * p : flow.sell, st.showUsd,
                                     st.amountPrecision, sell, sizeof(sell));
      u.draw.textFit(rowCell(buyTrade, ry, rowH), buy, t.text, DrawList::Right, 4);
      u.draw.textFit(rowCell(sellTrade, ry, rowH), sell, t.text, DrawList::Left, 4);
    }
  }

  if (inspectorH > 0) {
    int64_t tick = st.hasPinned ? st.pinnedTick : st.hoverTick;
    Rect inspector{r.x, r.y + r.h - inspectorH, r.w, inspectorH};
    u.draw.rect(inspector, t.panelAlt);
    u.draw.rect({inspector.x, inspector.y, inspector.w, 1}, t.border);
    const DomBucket* b = st.model.bucket(tick);
    DomFlow f = st.model.flow(tick);
    DomChange c = st.model.change(tick);
    double bid = b ? b->bid : 0, ask = b ? b->ask : 0;
    double total = bid + ask;
    double imbalance = total > 0 ? (bid - ask) / total * 100.0 : 0;
    std::vector<DomContribution> contributions;
    st.model.contributions(feeds, st.venue, st.mask, tick, contributions);
    std::vector<DomResidual> iq;
    st.model.residuals(tick, false, iq);
    size_t qBid = iq.size();
    st.model.residuals(tick, true, iq);
    size_t qAsk = iq.size();
    char detail[512];
    int used = snprintf(detail, sizeof(detail),
                        "%s %.6g  B %.4g  A %.4g  IMB %+.0f%%  FLOW +%.4g / -%.4g  Δ %.3g / %.3g  Q %zu/%zu",
                        st.hasPinned ? "PIN" : "LEVEL", (double)tick * st.effectiveStep,
                        bid, ask, imbalance, f.buy, f.sell, c.bid, c.ask, qBid, qAsk);
    for (size_t i = 0; i < contributions.size() && i < 3 && used < (int)sizeof(detail) - 32; ++i) {
      const DomContribution& v = contributions[i];
      used += snprintf(detail + used, sizeof(detail) - (size_t)used, "  %s %.3g/%.3g",
                       feeds.venues[(size_t)v.venue].shortLabel.c_str(), v.bid, v.ask);
    }
    u.draw.textFit(inspector, detail, st.hasPinned ? t.accent : t.textDim,
                   DrawList::Left, 8);
  }
}

// Per-venue source popover (shared body, see flow_sources.h). Only relevant in
// consolidated mode (venue == -1); the mask rebuild is picked up next frame via
// tickMask/sourceSignature drift.
void DomPanel::drawFlowPicker(Ui& u, Feeds& feeds) {
  if (!u.overlayOpen(flowPickerId)) return;
  if (drawFlowSources(u, flowPickerRect, mask, feeds, flowPickerList))
    saveSettings(*this);
}
