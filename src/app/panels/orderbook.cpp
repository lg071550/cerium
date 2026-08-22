#include "panels.h"
#include "panels_common.h"
#include "../symbols.h"

#include "../flow_sources.h"
#include "../../data/merge.h"
#include "../../platform/shell.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr double kBins[] = {0, 0.5, 1, 2.5, 5, 10};
constexpr const char* kBinLabels[] = {"raw", "0.5", "1", "2.5", "5", "10"};
constexpr float kLevelLimits[] = {0, 20, 40, 60, 100};
constexpr float kDepthWidths[] = {0.42f, 0.58f, 0.74f};
constexpr float kBarWidths[] = {0.24f, 0.34f, 0.46f};
// Depth bands for book analytics + the imbalance sparkline (near->far from mid).
constexpr double kDepthBands[OrderbookPanel::Analytics::kBands] = {
    0.005, 0.01, 0.025, 0.05, 0.10};

void invalidateOrderbookLabels(OrderbookPanel& st) {
  for (OrderbookPanel::Level& level : st.ladder) {
    level.fmtP = -1.0;
    level.fmtS = -1.0;
  }
}

void resetOrderbookSettings(OrderbookPanel& st) {
  st.showUsd = false;
  st.showCumulative = false;
  st.showDepth = true;
  st.showBars = true;
  st.showGradient = true;
  st.showTexture = true;
  st.showEdges = true;
  st.showFeedCount = true;
  st.showAnalytics = true;
  st.showDepthBands = true;
  st.showWeightedMid = true;
  st.density = 1;
  st.levelLimit = 0;
  st.scaleMode = 0;
  st.intensity = 1;
  st.priceDecimals = 2;
  st.amountPrecision = 2;
  st.depthWidth = 1;
  st.barWidth = 1;
  st.sideMode = 0;
  st.mask = kAllVenuesMask;
  st.binSel = 0;
  st.bin = kBins[0];
  st.scroll = 0;
  st.settingsList.scroll = 0;
  invalidateOrderbookLabels(st);
}

void saveOrderbookSettings(OrderbookPanel& st) {
  st.density = std::clamp(st.density, 0, 2);
  st.levelLimit = std::clamp(st.levelLimit, 0, 4);
  st.scaleMode = std::clamp(st.scaleMode, 0, 2);
  st.intensity = std::clamp(st.intensity, 0, 2);
  st.priceDecimals = std::clamp(st.priceDecimals, 0, 4);
  st.amountPrecision = std::clamp(st.amountPrecision, 0, 3);
  st.depthWidth = std::clamp(st.depthWidth, 0, 2);
  st.barWidth = std::clamp(st.barWidth, 0, 2);
  st.sideMode = std::clamp(st.sideMode, 0, 2);
  st.binSel = std::clamp(st.binSel, 0, 5);
  st.bin = kBins[st.binSel];
  st.mask &= kAllVenuesMask;

  char saved[320];
  snprintf(saved, sizeof(saved),
           "3,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%d",
           st.showUsd, st.showCumulative, st.showDepth, st.showBars,
           st.showGradient, st.showTexture, st.showEdges, st.showFeedCount,
           st.showAnalytics, st.showDepthBands, st.showWeightedMid, st.density,
           st.levelLimit, st.scaleMode, st.intensity,
           st.priceDecimals, st.amountPrecision, st.depthWidth, st.barWidth,
           st.sideMode, (unsigned)st.mask, st.binSel);
  shell_storage_set(st.settingsKey.c_str(), saved);
  invalidateOrderbookLabels(st);
}

void loadOrderbookSettings(OrderbookPanel& st) {
  if (st.settingsLoaded) return;
  st.settingsLoaded = true;
  char* saved = shell_storage_get(st.settingsKey.c_str());
  if (!saved) return;

  int version = 0, showUsd = 0, showCum = 0, showDepth = 0, showBars = 0;
  int showGradient = 0, showTexture = 0, showEdges = 0, showFeedCount = 0;
  int showAnalytics = 1, showDepthBands = 1, showWeightedMid = 1;
  int density = 0, levelLimit = 0, scaleMode = 0, intensity = 0;
  int priceDecimals = 0, amountPrecision = 0, depthWidth = 0, barWidth = 0;
  int sideMode = 0, binSel = 0;
  unsigned mask = 0;
  int count = sscanf(
      saved,
      "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%d",
      &version, &showUsd, &showCum, &showDepth, &showBars, &showGradient,
      &showTexture, &showEdges, &showFeedCount, &showAnalytics, &showDepthBands,
      &showWeightedMid, &density, &levelLimit, &scaleMode, &intensity,
      &priceDecimals, &amountPrecision, &depthWidth, &barWidth, &sideMode, &mask,
      &binSel);
  if (version == 1) {
    showAnalytics = showDepthBands = showWeightedMid = 1;
    count = sscanf(saved,
                   "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                   &version, &showUsd, &showCum, &showDepth, &showBars,
                   &showGradient, &showTexture, &showEdges, &showFeedCount,
                   &density, &levelLimit, &scaleMode, &intensity,
                   &priceDecimals, &amountPrecision, &depthWidth, &barWidth,
                   &sideMode, &mask, &binSel);
  }
  std::free(saved);
  if (!((count == 23 && version == 3) || (count == 23 && version == 2) ||
        (count == 20 && version == 1)))
    return;
  const bool legacyMask = version <= 2;

  st.showUsd = showUsd != 0;
  st.showCumulative = showCum != 0;
  st.showDepth = showDepth != 0;
  st.showBars = showBars != 0;
  st.showGradient = showGradient != 0;
  st.showTexture = showTexture != 0;
  st.showEdges = showEdges != 0;
  st.showFeedCount = showFeedCount != 0;
  st.showAnalytics = showAnalytics != 0;
  st.showDepthBands = showDepthBands != 0;
  st.showWeightedMid = showWeightedMid != 0;
  st.density = density;
  st.levelLimit = levelLimit;
  st.scaleMode = scaleMode;
  st.intensity = intensity;
  st.priceDecimals = priceDecimals;
  st.amountPrecision = amountPrecision;
  st.depthWidth = depthWidth;
  st.barWidth = barWidth;
  st.sideMode = sideMode;
  st.mask = legacyMask ? venueMaskForClass((uint8_t)mask) : (uint32_t)mask;
  st.binSel = binSel;
  saveOrderbookSettings(st); // clamps and normalizes the loaded values
}

void drawOrderbookSettings(Ui& u, Rect area, OrderbookPanel& st) {
  const Theme& t = theme();
  constexpr int kRows = 17;
  const float settingsRowH = area.w < 110.0f ? 170.0f
                             : area.w < 180.0f ? 92.0f
                             : area.w < 270.0f ? 68.0f
                                               : 44.0f;

  listView(u, area, kRows, settingsRowH, st.settingsList,
           [&](Ui& rowUi, DrawList& d, Rect row, int index) {
             char rowId[32];
             snprintf(rowId, sizeof(rowId), "##book-setting-%d", index);
             rowUi.pushId(rowId);
             d.rect({row.x + 10, row.y + row.h - 1, row.w - 20, 1},
                    withAlpha(t.border, 0.65f));

             const char* title = "";
             switch (index) {
               case 0: title = "AMOUNT UNIT"; break;
               case 1: title = "AMOUNT VALUE"; break;
               case 2: title = "VISIBLE SIDE"; break;
               case 3: title = "ROW DENSITY"; break;
               case 4: title = "VISIBLE LEVELS"; break;
               case 5: title = "PRICE DECIMALS"; break;
               case 6: title = "AMOUNT DECIMALS"; break;
               case 7: title = "BAR SCALING"; break;
               case 8: title = "VISUAL INTENSITY"; break;
               case 9: title = "DEPTH WIDTH"; break;
               case 10: title = "LEVEL-BAR WIDTH"; break;
               case 11: title = "LAYERS"; break;
               case 12: title = "DETAIL"; break;
               case 13: title = "BOOK ANALYTICS"; break;
               case 14: title = "SOURCES"; break;
               case 15: title = "PRICE GROUPING"; break;
               case 16: title = "RESTORE"; break;
             }
             d.textAligned({row.x + 10, row.y + 2, row.w - 20, 16}, title,
                           t.textDim, DrawList::Left);

             float x = row.x + 10, y = row.y + 19, h = 21;
             const float optionStart = x;
             const float optionRight = row.x + row.w - 10.0f;
             auto option = [&](const char* label, float width, bool active,
                               auto apply) {
               width = std::min(width, std::max(20.0f, optionRight - optionStart));
               if (x > optionStart && x + width > optionRight) {
                 x = optionStart;
                 y += 25.0f;
               }
               if (y + h <= row.y + row.h - 2.0f &&
                   chip(rowUi, {x, y, width, h}, label, active)) {
                 apply();
                 saveOrderbookSettings(st);
               }
               x += width + 4;
             };

             switch (index) {
               case 0:
                 option("COIN", 58, !st.showUsd, [&] { st.showUsd = false; });
                 option("USD", 58, st.showUsd, [&] { st.showUsd = true; });
                 break;
               case 1:
                 option("LEVEL", 68, !st.showCumulative,
                        [&] { st.showCumulative = false; });
                 option("CUMULATIVE", 94, st.showCumulative,
                        [&] { st.showCumulative = true; });
                 break;
               case 2:
                 option("BOTH", 58, st.sideMode == 0, [&] { st.sideMode = 0; });
                 option("ASKS", 58, st.sideMode == 1, [&] { st.sideMode = 1; });
                 option("BIDS", 58, st.sideMode == 2, [&] { st.sideMode = 2; });
                 break;
               case 3:
                 option("TIGHT", 64, st.density == 0, [&] { st.density = 0; });
                 option("NORMAL", 70, st.density == 1, [&] { st.density = 1; });
                 option("RELAXED", 76, st.density == 2, [&] { st.density = 2; });
                 break;
               case 4: {
                 static constexpr const char* labels[] = {"AUTO", "20", "40", "60", "100"};
                 for (int i = 0; i < 5; ++i)
                   option(labels[i], i == 0 ? 56.0f : 44.0f, st.levelLimit == i,
                          [&, i] { st.levelLimit = i; });
                 break;
               }
               case 5: {
                 static constexpr const char* labels[] = {"0", "1", "2", "3", "4"};
                 for (int i = 0; i < 5; ++i)
                   option(labels[i], 42, st.priceDecimals == i,
                          [&, i] { st.priceDecimals = i; });
                 break;
               }
               case 6: {
                 static constexpr const char* labels[] = {"0", "2", "3", "5"};
                 for (int i = 0; i < 4; ++i)
                   option(labels[i], 46, st.amountPrecision == i,
                          [&, i] { st.amountPrecision = i; });
                 break;
               }
               case 7:
                 option("LINEAR", 70, st.scaleMode == 0, [&] { st.scaleMode = 0; });
                 option("SQRT", 58, st.scaleMode == 1, [&] { st.scaleMode = 1; });
                 option("LOG", 54, st.scaleMode == 2, [&] { st.scaleMode = 2; });
                 break;
               case 8:
                 option("QUIET", 64, st.intensity == 0, [&] { st.intensity = 0; });
                 option("NORMAL", 70, st.intensity == 1, [&] { st.intensity = 1; });
                 option("STRONG", 70, st.intensity == 2, [&] { st.intensity = 2; });
                 break;
               case 9: {
                 static constexpr const char* labels[] = {"42%", "58%", "74%"};
                 for (int i = 0; i < 3; ++i)
                   option(labels[i], 58, st.depthWidth == i,
                          [&, i] { st.depthWidth = i; });
                 break;
               }
               case 10: {
                 static constexpr const char* labels[] = {"24%", "34%", "46%"};
                 for (int i = 0; i < 3; ++i)
                   option(labels[i], 58, st.barWidth == i,
                          [&, i] { st.barWidth = i; });
                 break;
               }
               case 11:
                 option("DEPTH", 64, st.showDepth, [&] { st.showDepth = !st.showDepth; });
                 option("BARS", 58, st.showBars, [&] { st.showBars = !st.showBars; });
                 option("GRADIENT", 82, st.showGradient,
                        [&] { st.showGradient = !st.showGradient; });
                 break;
               case 12:
                 option("TEXTURE", 76, st.showTexture,
                        [&] { st.showTexture = !st.showTexture; });
                 option("EDGES", 62, st.showEdges, [&] { st.showEdges = !st.showEdges; });
                 option("FEED COUNT", 94, st.showFeedCount,
                        [&] { st.showFeedCount = !st.showFeedCount; });
                 break;
              case 13:
                 option("STRIP", 62, st.showAnalytics,
                        [&] { st.showAnalytics = !st.showAnalytics; });
                 option("DEPTH BANDS", 102, st.showDepthBands,
                        [&] { st.showDepthBands = !st.showDepthBands; });
                 option("W-MID", 64, st.showWeightedMid,
                        [&] { st.showWeightedMid = !st.showWeightedMid; });
                 break;
               case 14: {
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
                   if (x > optionStart && x + vw > optionRight) {
                     x = optionStart;
                     y += 25.0f;
                   }
                   if (y + h <= row.y + row.h - 2.0f) {
                     if (chip(rowUi, {x, y, vw, h}, "VENUES…",
                              u.overlayOpen(st.flowPickerId))) {
                       if (u.overlayOpen(st.flowPickerId)) {
                         u.closeOverlay(st.flowPickerId);
                       } else {
                         if (!st.flowPickerId) st.flowPickerId = u.id("##obflowpicker");
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
               case 15:
                 for (int i = 0; i < 6; ++i)
                   option(i == 0 ? "RAW" : kBinLabels[i], i == 0 ? 50.0f : 43.0f,
                          st.binSel == i, [&, i] { st.binSel = i; });
                 break;
               case 16:
                 if (button(rowUi,
                            {x, y, std::min(142.0f, row.w - 20.0f), 23},
                            "RESET TO DEFAULTS")) {
                   resetOrderbookSettings(st);
                   saveOrderbookSettings(st);
                 }
                 break;
             }
             rowUi.popId();
           });
}

// Tiny depth-imbalance profile: one vertical spike per depth band (near→far
// from mid), drawn live from the current analytics. The (bid−ask)/(bid+ask)
// ratio is normally well under 1%, so it is sqrt-compressed against a 1%
// reference before mapping to height — a linear scale would keep the spikes
// sub-pixel and read as one flat line.
void drawImbalanceProfile(Ui& u, Rect plot, OrderbookPanel& st) {
  const Theme& t = theme();
  const int bands = OrderbookPanel::Analytics::kBands;
  static constexpr const char* kLabels[] = {"0.5%", "1%", "2.5%", "5%", "10%"};
  static constexpr float kRef = 0.01f; // 1% imbalance → full height

  const float labelH = 13.0f;
  const float barArea = plot.h - labelH;
  const float cy = plot.y + barArea * 0.5f;
  const float amp = barArea * 0.5f - 1.5f;
  u.draw.rect({plot.x, cy, plot.w, 1}, withAlpha(t.text, 0.18f));

  if (!st.analytics.valid) {
    u.draw.textAligned(plot, "syncing…", t.textDim, DrawList::Center);
    return;
  }
  const float slot = plot.w / (float)bands;
  const float barW = std::min(slot * 0.42f, 12.0f);
  for (int band = 0; band < bands; ++band) {
    const float cx = plot.x + slot * ((float)band + 0.5f);
    const float imb = (float)st.analytics.imbalance[band];
    float v = std::sqrt(std::fabs(imb) / kRef);
    v = std::min(v, 1.0f);
    if (imb < 0) v = -v;
    const float h = std::max(1.0f, std::fabs(v) * amp);
    const Color c = v >= 0 ? t.green : t.red;
    if (v >= 0) u.draw.rect({cx - barW * 0.5f, cy - h, barW, h}, withAlpha(c, 0.62f));
    else u.draw.rect({cx - barW * 0.5f, cy, barW, h}, withAlpha(c, 0.62f));
    u.draw.textAligned(
        {plot.x + slot * (float)band, plot.y + barArea, slot, labelH},
        kLabels[band], t.textDim, DrawList::Center);
  }
}

void drawOrderbookAnalytics(Ui& u, Rect area, OrderbookPanel& st,
                            bool fullDepthBands) {
  const Theme& t = theme();
  u.draw.rect(area, mixColor(t.panelAlt, t.bg, 0.32f));
  u.draw.rect({area.x, area.y + area.h - 1, area.w, 1}, t.border);

  double imbalance = st.analytics.imbalance[1]; // 1% band (compact pressure bar)
  Color pressureColor = imbalance >= 0 ? t.green : t.red;
  char imbalanceText[32];
  snprintf(imbalanceText, sizeof(imbalanceText), "%+.1f%%", imbalance * 100.0);

  if (!st.analytics.valid) {
    u.draw.textAligned(area, "depth analytics syncing", t.textDim,
                       DrawList::Center);
    return;
  }

  if (!fullDepthBands) {
    const float labelW = area.w >= 230.0f ? 30.0f : 24.0f;
    const float valueW = std::min(55.0f, area.w * 0.28f);
    Rect label{area.x + 6, area.y, labelW, area.h};
    Rect value{area.x + area.w - valueW - 5, area.y, valueW, area.h};
    Rect track{label.x + label.w + 5, area.y + area.h * 0.5f - 3,
               std::max(8.0f, value.x - label.x - label.w - 10), 6};
    float bidShare = std::clamp((float)((imbalance + 1.0) * 0.5), 0.0f, 1.0f);
    u.draw.rect(track, withAlpha(t.textDim, 0.08f));
    u.draw.rect({track.x, track.y, track.w * bidShare, track.h},
                withAlpha(t.green, 0.34f));
    u.draw.rect({track.x + track.w * bidShare, track.y,
                 track.w * (1.0f - bidShare), track.h},
                withAlpha(t.red, 0.34f));
    u.draw.rect({track.x + track.w * 0.5f, track.y - 2, 1, track.h + 4},
                withAlpha(t.text, 0.26f));
    u.draw.textAligned(label, "IMB", t.textDim, DrawList::Left);
    u.draw.textAligned(value, imbalanceText, pressureColor, DrawList::Right);
  } else {
    // Tiny depth-imbalance sparkline (left) + compact readout (right).
    const float chartW = std::clamp(area.w * 0.50f, 150.0f, 260.0f);
    Rect chart{area.x + 6, area.y + 3, chartW - 10, area.h - 6};
    drawImbalanceProfile(u, chart, st);

    Rect summary{area.x + chartW + 4, area.y + 2, area.w - chartW - 10, area.h - 4};
    const float half = summary.w * 0.55f;
    char nearText[24];
    snprintf(nearText, sizeof(nearText), "%+.1f%%",
             st.analytics.imbalance[0] * 100.0);
    Color nearColor = st.analytics.imbalance[0] >= 0 ? t.green : t.red;
    char line[48];
    snprintf(line, sizeof(line), "0.5%% %s", nearText);
    u.draw.textFit({summary.x, summary.y, half, summary.h * 0.5f}, line,
                   nearColor, DrawList::Left, 2);
    snprintf(line, sizeof(line), "SPR %.2fbp", st.analytics.spreadBps);
    u.draw.textFit({summary.x + half, summary.y, summary.w - half,
                    summary.h * 0.5f},
                   line, t.textDim, DrawList::Right, 2);

    snprintf(line, sizeof(line), "10%% %+.1f",
             st.analytics.imbalance[4] * 100.0);
    u.draw.textFit({summary.x, summary.y + summary.h * 0.5f, half,
                    summary.h * 0.5f},
                   line, t.textDim, DrawList::Left, 2);
    if (st.showWeightedMid) {
      snprintf(line, sizeof(line), "WM %.*f", st.priceDecimals,
               st.analytics.weightedMid);
      u.draw.textFit({summary.x + half, summary.y + summary.h * 0.5f,
                      summary.w - half, summary.h * 0.5f},
                     line, t.textDim, DrawList::Right, 2);
    }
  }

  // Tooltip text only matters while hovered; values change on merge rebuilds,
  // so cache against mergeVersion instead of formatting 9 conversions per
  // frame for a tooltip nobody is reading.
  static thread_local uint64_t tipVersion = ~0ull;
  if (u.hovered(area) || st.mergeVersion != tipVersion) {
    tipVersion = st.mergeVersion;
    char bid1[24], ask1[24], bid5[24], ask5[24];
    formatAmount(st.analytics.bidUsd[1], true, 2, bid1, sizeof(bid1));
    formatAmount(st.analytics.askUsd[1], true, 2, ask1, sizeof(ask1));
    formatAmount(st.analytics.bidUsd[3], true, 2, bid5, sizeof(bid5));
    formatAmount(st.analytics.askUsd[3], true, 2, ask5, sizeof(ask5));
    snprintf(st.analyticsTip, sizeof(st.analyticsTip),
             "Imbalance by depth 0.5/1/2.5/5/10%%: %+.1f / %+.1f / %+.1f / %+.1f / %+.1f · L2 depth: 1%% bid %s / ask %s · 5%% bid %s / ask %s",
             st.analytics.imbalance[0] * 100.0, st.analytics.imbalance[1] * 100.0,
             st.analytics.imbalance[2] * 100.0, st.analytics.imbalance[3] * 100.0,
             st.analytics.imbalance[4] * 100.0, bid1, ask1, bid5, ask5);
  }
  u.tip(u.id("##orderbook-analytics"), area, st.analyticsTip);
}
} // namespace

void drawOrderbook(Ui& u, Rect r, OrderbookPanel& st, Feeds& feeds) {
  const Theme& t = theme();
  loadOrderbookSettings(st);

  // Responsive control rail. Low-priority source toggles collapse before they
  // can collide with the essential grouping/settings controls; the complete
  // source matrix remains available on the settings page at every width.
  Rect toolbar{r.x, r.y, r.w, 22};
  u.draw.rect(toolbar, t.panelAlt);
  float cx = r.x + 8;
  float cy = r.y + 2;
  const bool wideToolbar = r.w >= 430.0f;
  const bool compactToolbar = r.w >= 300.0f;
  const float settingsW = r.w < 100.0f ? std::max(24.0f, r.w - 4.0f)
                                        : wideToolbar ? 70.0f : 54.0f;
  Rect settingsButton{std::max(r.x + 2.0f, r.x + r.w - settingsW - 6.0f), cy,
                      settingsW, 18};
  const bool showCenter = r.w >= 170.0f;
  const float centerW = wideToolbar ? 58.0f : 48.0f;
  Rect centerButton{settingsButton.x - centerW - 4.0f, cy, centerW, 18};
  const float leftLimit = (showCenter ? centerButton.x : settingsButton.x) - 4.0f;

  if (compactToolbar) {
    const float sourceW = wideToolbar ? 48.0f : 30.0f;
    const float sourceGap = 4.0f;
    const char* labels[] = {wideToolbar ? "SPOT" : "S",
                            wideToolbar ? "PERP" : "P",
                            wideToolbar ? "DEX" : "D"};
    const uint32_t bits[] = {venueMaskForClass(ClassSpot),
                             venueMaskForClass(ClassPerp),
                             venueMaskForClass(ClassDex)};
    for (int i = 0; i < 3; ++i) {
      if (cx + sourceW > leftLimit) break;
      if (chip(u, {cx, cy, sourceW, 18}, labels[i],
               (st.mask & bits[i]) == bits[i])) {
        st.mask ^= bits[i];
        saveOrderbookSettings(st);
      }
      cx += sourceW + sourceGap;
    }
    cx += wideToolbar ? 6.0f : 2.0f;
  }

  char binLabel[24];
  snprintf(binLabel, sizeof(binLabel), wideToolbar ? "BIN %s" : "%s",
           kBinLabels[st.binSel]);
  const float binW = wideToolbar ? 64.0f : 54.0f;
  const bool showBin = cx + binW <= leftLimit;
  if (showBin && chip(u, {cx, cy, binW, 18}, binLabel, st.bin > 0)) {
    st.binSel = (st.binSel + 1) % 6;
    st.bin = kBins[st.binSel];
    saveOrderbookSettings(st);
  }
  if (showBin) cx += binW;

  if (showCenter && chip(u, centerButton, wideToolbar ? "CENTER" : "CTR",
                         st.scroll != 0))
    st.scroll = 0;

  if (wideToolbar && st.showFeedCount && showBin && centerButton.x > cx + 8.0f) {
    float feedX = cx + 4.0f;
    float feedW = centerButton.x - feedX - 4.0f;
    char live[24];
    snprintf(live, sizeof(live), "%d FEEDS", feeds.liveCount());
    // textAligned does not clip an over-wide label, so only render telemetry
    // when the measured string actually fits the responsive middle gap.
    if (feedW >= u.draw.measure(live) + 4.0f)
      u.draw.textFit({feedX, cy, feedW, 18}, live,
                     feeds.liveCount() > 0 ? t.green : t.textDim,
                     DrawList::Right);
  }

  Behavior settingsBehavior =
      behavior(u, settingsButton, u.id("##orderbook-settings"));
  if (settingsBehavior.hovered || settingsBehavior.held || st.settingsOpen)
    u.draw.rect(settingsButton,
                settingsBehavior.held || st.settingsOpen ? t.accentSoft : t.bgHover);
  const char* settingsLabel = st.settingsOpen ? "DONE"
                              : wideToolbar   ? "SETTINGS"
                                              : "SET";
  u.draw.textAligned(settingsButton, settingsLabel,
                     st.settingsOpen ? t.accent
                                     : settingsBehavior.hovered ? t.text : t.textDim,
                     DrawList::Center);
  if (settingsBehavior.clicked) st.settingsOpen = !st.settingsOpen;
  u.draw.rect({r.x, r.y + 22, r.w, 1}, t.border);

  if (st.settingsOpen) {
    drawOrderbookSettings(u, {r.x, r.y + 23, r.w, r.h - 23}, st);
    return;
  }

  // The analytics strip progressively yields to the ladder: full three-band
  // depth telemetry on normal panes, a single pressure bar on medium panes,
  // and no reserved height on genuinely constrained panes.
  const bool showAnalyticsStrip =
      st.showAnalytics && r.w >= 150.0f && r.h >= 120.0f;
  const bool showFullDepthBands = showAnalyticsStrip && st.showDepthBands &&
                                  r.w >= 320.0f && r.h >= 190.0f;
  const float analyticsH = showAnalyticsStrip ? (showFullDepthBands ? 48.0f : 20.0f)
                                               : 0.0f;
  Rect analyticsArea{r.x, r.y + 23, r.w, analyticsH};

  // Column header is a separate 17px table row beneath the command rail and
  // optional analytics strip.
  const bool showColumnHeader = r.h >= 70.0f;
  const float headerBlockH = showColumnHeader ? 18.0f : 0.0f;
  Rect header{r.x, r.y + 23 + analyticsH, r.w,
              showColumnHeader ? 17.0f : 0.0f};
  if (showColumnHeader) u.draw.rect(header, t.panelAlt);
  const float columnPad = r.w < 140.0f ? 4.0f : 10.0f;
  const float columnGap = r.w < 120.0f ? 2.0f : 8.0f;
  const float columnW = std::max(0.0f, r.w - columnPad * 2.0f - columnGap);
  const float priceW = columnW * (r.w < 180.0f ? 0.52f : 0.48f);
  Rect priceColumn{r.x + columnPad, header.y, priceW, header.h};
  Rect amountColumn{priceColumn.x + priceColumn.w + columnGap, header.y,
                    std::max(0.0f, columnW - priceW), header.h};
  if (showColumnHeader)
    u.draw.textFit(priceColumn, r.w < 180.0f ? "PRICE" : "PRICE (USDT)",
                   t.textDim, DrawList::Left);
  char amountHeader[32];
  snprintf(amountHeader, sizeof(amountHeader), "%s (%s)",
           st.showCumulative ? "CUM" : "SIZE",
           st.showUsd ? "USD" : symbols::kNames[std::clamp(feeds.symbol, 0, 2)]);
  if (showColumnHeader) {
    u.draw.textFit(amountColumn,
                   r.w < 180.0f ? (st.showCumulative ? "CUM" : "SIZE")
                                 : amountHeader,
                   t.textDim, DrawList::Right);
    u.draw.rect({r.x, header.y + 17, r.w, 1}, t.border);
  }

  Rect area{r.x, r.y + 23 + analyticsH + headerBlockH, r.w,
            std::max(0.0f, r.h - 23 - analyticsH - headerBlockH)};
  double mid = feeds.aggMid();
  if (mid <= 0) {
    st.analytics.valid = false;
    if (showAnalyticsStrip)
      drawOrderbookAnalytics(u, analyticsArea, st, showFullDepthBands);
    u.draw.textAligned(area, "syncing…", t.textDim, DrawList::Center);
    return;
  }

    // The ladder rebuilds every frame the books move — no data throttling —
    // but the merge is capped so a 27-venue gather stays finite. Floor the
    // cap well past the visible window: viewable+64 made the book look empty
    // after one scroll page even when Coinbase/Bitstamp held thousands of
    // levels. 1024 nearest-mid bins is still cheap and matches a deep ladder.
    uint64_t ver = feeds.booksVersion();
    const float rowH0 = kPanelRowHeights[std::clamp(st.density, 0, 2)];
    int capRows0 = std::max(1, (int)(area.h / rowH0));
    int configured0 = kLevelLimits[std::clamp(st.levelLimit, 0, 4)];
    if (configured0 > 0) capRows0 = std::min(capRows0, configured0);
    // Quantize the scroll-driven cap to 256-step blocks: unquantized, every
    // wheel notch past 1024 produced a new cap and forced a full rebuild.
    const int mergeCap =
        std::max(1024, (capRows0 + std::abs(st.scroll) + 255) & ~255);
    bool filterChanged = st.mask != st.mergeMask || st.bin != st.mergeBin;
    bool verChanged = ver != st.mergeVersion;
    if (filterChanged || st.mergeCap != mergeCap || verChanged) {
      st.mergeVersion = ver;
      st.mergeCap = mergeCap;
      st.mergeMask = st.mask;
      st.mergeBin = st.bin;

      bool healthy[64];
      feeds.collectHealthy(50.0, healthy, std::size(healthy));
    std::vector<const BookSide*> askSides, bidSides;
    for (size_t i = 0; i < feeds.venues.size(); ++i) {
      if (!healthy[i] || !(st.mask & (1u << i))) continue;
      askSides.push_back(&feeds.venues[i].book.asks);
      bidSides.push_back(&feeds.venues[i].book.bids);
    }

    // Analytics deliberately use the raw selected books rather than display
    // bins. Ask bins round down for the visual ladder, which can make a wide
    // grouping appear to cross the midpoint and would corrupt touch metrics.
    // Keyed on version/mask only: a cap-only drift re-merges the ladder but
    // must not rescan the ±10% bands.
    if (verChanged || filterChanged) {
    st.analytics = {};
    double bestBidSize = 0, bestAskSize = 0;
    for (const BookSide* side : askSides) {
      size_t start = side->lowerBound(mid);
      size_t end = side->lowerBound(mid * 1.10 + 1e-12);
      if (start < side->prices.size()) {
        double p = side->prices[start], q = side->sizes[start];
        if (st.analytics.bestAsk <= 0 || p < st.analytics.bestAsk - 1e-9) {
          st.analytics.bestAsk = p;
          bestAskSize = q;
        } else if (std::fabs(p - st.analytics.bestAsk) <= 1e-9) {
          bestAskSize += q;
        }
      }
      for (size_t i = start; i < end; ++i) {
        double distance = side->prices[i] / mid - 1.0;
        double usd = side->prices[i] * side->sizes[i];
        for (int band = 0; band < OrderbookPanel::Analytics::kBands; ++band)
          if (distance <= kDepthBands[band] + 1e-12)
            st.analytics.askUsd[band] += usd;
      }
    }
    for (const BookSide* side : bidSides) {
      size_t start = side->lowerBound(mid * 0.90);
      size_t end = side->lowerBound(mid + 1e-12);
      if (end > 0) {
        size_t touch = end - 1;
        double p = side->prices[touch], q = side->sizes[touch];
        if (p <= mid + 1e-9 &&
            (st.analytics.bestBid <= 0 || p > st.analytics.bestBid + 1e-9)) {
          st.analytics.bestBid = p;
          bestBidSize = q;
        } else if (std::fabs(p - st.analytics.bestBid) <= 1e-9) {
          bestBidSize += q;
        }
      }
      for (size_t i = start; i < end; ++i) {
        double distance = 1.0 - side->prices[i] / mid;
        double usd = side->prices[i] * side->sizes[i];
        for (int band = 0; band < OrderbookPanel::Analytics::kBands; ++band)
          if (distance <= kDepthBands[band] + 1e-12)
            st.analytics.bidUsd[band] += usd;
      }
    }
    for (int band = 0; band < OrderbookPanel::Analytics::kBands; ++band) {
      double total = st.analytics.bidUsd[band] + st.analytics.askUsd[band];
      st.analytics.imbalance[band] =
          total > 0 ? (st.analytics.bidUsd[band] - st.analytics.askUsd[band]) / total
                    : 0;
    }
    if (st.analytics.bestBid > 0 && st.analytics.bestAsk > 0 &&
        st.analytics.bestAsk >= st.analytics.bestBid) {
      double touchTotal = bestBidSize + bestAskSize;
      st.analytics.weightedMid =
          touchTotal > 0
              ? (st.analytics.bestAsk * bestBidSize +
                 st.analytics.bestBid * bestAskSize) /
                    touchTotal
              : (st.analytics.bestBid + st.analytics.bestAsk) * 0.5;
      double touchMid = (st.analytics.bestBid + st.analytics.bestAsk) * 0.5;
      st.analytics.spreadBps =
          touchMid > 0 ? (st.analytics.bestAsk - st.analytics.bestBid) / touchMid * 1e4
                       : 0;
      st.analytics.valid = true;
    }
    } // analytics keyed on version/mask

    static thread_local std::vector<MergedLevel> asks, bids;
    // merge each side over the venue books' FULL extent (aggbook-style): the
    // ladder is scrollable through all held depth; the cap only bounds the
    // fused output (nearest-mid first) so a 27-venue merge stays finite
    static constexpr size_t kMaxLadderSide = 8192;
    const size_t ladderCap = std::min((size_t)mergeCap, kMaxLadderSide);
    mergeSideWindow(askSides.data(), askSides.size(), mid, INFINITY, st.bin, true,
                    asks, ladderCap);
    mergeSideWindow(bidSides.data(), bidSides.size(), 0.0, mid, st.bin, false,
                    bids, ladderCap);
    if (asks.size() > ladderCap) asks.resize(ladderCap);
    if (bids.size() > ladderCap) bids.resize(ladderCap);

    st.ladder.clear();
    st.ladderMid = (int)asks.size();
    st.ladder.reserve(asks.size() + bids.size());
    double cum = 0, cumUsd = 0;
    static thread_local std::vector<double> askCum;
    static thread_local std::vector<double> askCumUsd;
    askCum.clear();
    askCumUsd.clear();
    for (auto& a : asks) { // asks ascend from best: cum accumulates from mid outward
      cum += a.size;
      cumUsd += a.size * a.price;
      askCum.push_back(cum);
      askCumUsd.push_back(cumUsd);
    }
    for (int i = (int)asks.size() - 1; i >= 0; --i) // desc: deepest ask first
      st.ladder.push_back({asks[(size_t)i].price, asks[(size_t)i].size,
                           askCum[(size_t)i], askCumUsd[(size_t)i], true, {}, {}});
    cum = 0;
    cumUsd = 0;
    for (auto& b : bids) {
      cum += b.size;
      cumUsd += b.size * b.price;
      st.ladder.push_back({b.price, b.size, cum, cumUsd, false, {}, {}});
    }
    // row labels are formatted lazily in the draw loop — only visible rows
    // ever need them, which keeps snprintf off the rebuild path
  }

  if (showAnalyticsStrip)
    drawOrderbookAnalytics(u, analyticsArea, st, showFullDepthBands);

  const float rowH = kPanelRowHeights[std::clamp(st.density, 0, 2)];
  int capacityRows = std::max(1, (int)(area.h / rowH));
  int rows = capacityRows;
  int configuredLimit = kLevelLimits[std::clamp(st.levelLimit, 0, 4)];
  if (configuredLimit > 0) rows = std::min(rows, configuredLimit);

  int rangeFirst = 0;
  int rangeEnd = (int)st.ladder.size();
  if (st.sideMode == 1) rangeEnd = st.ladderMid;
  if (st.sideMode == 2) rangeFirst = st.ladderMid;
  if (rangeEnd <= rangeFirst) {
    u.draw.textAligned(area,
                       st.mask == 0 ? "select at least one source"
                                    : "no depth for this side",
                       t.textDim, DrawList::Center);
    return;
  }
  rows = std::max(1, std::min(rows, std::max(1, rangeEnd - rangeFirst)));

  // wheel scrolls the ladder window away from mid; proportional to the
  // accumulated delta so bursts that land in one frame still scroll fully
  if (u.hovered(area) && u.input.wheelY != 0)
    st.scroll += (int)std::lround(u.input.wheelY / 40.0);
  int maxStart = std::max(rangeFirst, rangeEnd - rows);
  int baseStart = st.sideMode == 1 ? maxStart
                  : st.sideMode == 2 ? rangeFirst
                                     : st.ladderMid - rows / 2;
  baseStart = std::clamp(baseStart, rangeFirst, maxStart);
  int startIdx = std::clamp(baseStart + st.scroll, rangeFirst, maxStart);
  int endIdx = std::min(rangeEnd, startIdx + rows);
  st.scroll = startIdx - baseStart; // keep scroll bounded for this view mode

  auto amountValue = [&](const OrderbookPanel::Level& level) {
    if (st.showCumulative) return st.showUsd ? level.cumUsd : level.cum;
    return st.showUsd ? level.size * level.price : level.size;
  };
  auto cumulativeValue = [&](const OrderbookPanel::Level& level) {
    return st.showUsd ? level.cumUsd : level.cum;
  };

  double maxCum = 1, maxAmt = 1;
  for (int i = startIdx; i < endIdx; ++i) {
    maxCum = std::max(maxCum, cumulativeValue(st.ladder[(size_t)i]));
    maxAmt = std::max(maxAmt, amountValue(st.ladder[(size_t)i]));
  }

  float y = area.y + (capacityRows - rows) * rowH * 0.5f;
  const float depthWidth = kDepthWidths[std::clamp(st.depthWidth, 0, 2)];
  const float barWidth = kBarWidths[std::clamp(st.barWidth, 0, 2)];
  const float visual = kPanelIntensity[std::clamp(st.intensity, 0, 2)];
  for (int i = startIdx; i < endIdx && y + rowH <= area.y + area.h; ++i) {
    // (no spread band: an aggregated book crosses venues, so bestAsk can sit
    // below bestBid and the "spread" is routinely negative — meaningless)
    OrderbookPanel::Level& L = st.ladder[(size_t)i];
    if (L.fmtP != L.price) { // label caches survive price-stable frames
      snprintf(L.priceLbl, sizeof(L.priceLbl), "%.*f", st.priceDecimals, L.price);
      L.fmtP = L.price;
    }
    double amount = amountValue(L);
    if (L.fmtS != amount) {
      formatAmount(amount, st.showUsd, st.amountPrecision, L.sizeLbl,
                   sizeof(L.sizeLbl));
      L.fmtS = amount;
    }

    Color c = L.ask ? t.red : t.green;
    float rel = shapeDepth((float)(amount / maxAmt), st.scaleMode);
    float relC = shapeDepth((float)(cumulativeValue(L) / maxCum), st.scaleMode);
    // Interpolate cumulative depth through each row in small vertical slices.
    // This preserves the stepped nature of a book while removing the coarse
    // row-height staircase from the depth silhouette. Never interpolate across
    // the ask/bid boundary because cumulative size resets there.
    float relC2 = relC;
    if (i + 1 < endIdx) {
      const OrderbookPanel::Level& next = st.ladder[(size_t)i + 1];
      if (next.ask == L.ask)
        relC2 = shapeDepth((float)(cumulativeValue(next) / maxCum), st.scaleMode);
    }
    if (st.showDepth) {
      int depthSlices = std::max(1, (int)std::lround(rowH / 2.0f));
      for (int slice = 0; slice < depthSlices; ++slice) {
        float f0 = (float)slice / (float)depthSlices;
        float f1 = (float)(slice + 1) / (float)depthSlices;
        float depth = std::clamp(relC + (relC2 - relC) * ((f0 + f1) * 0.5f),
                                 0.0f, 1.0f);
        float cw = depth * (area.w * depthWidth);
        Rect depthSlice{area.x + area.w - cw, y + rowH * f0, cw,
                        rowH * (f1 - f0)};
        if (st.showGradient)
          u.draw.rectGradientHDithered(
              depthSlice, withAlpha(c, 0.004f * visual),
              withAlpha(c, (0.045f + 0.105f * depth) * visual));
        else
          u.draw.rect(depthSlice, withAlpha(c, (0.025f + 0.075f * depth) * visual));
      }
    }

    // The amount bar is intentionally darker than the depth field's old
    // treatment. Size, row cadence, and side each affect its tone so adjacent
    // levels remain individually legible instead of becoming one bright slab.
    float aw = rel * (area.w * barWidth);
    Rect amountBar{area.x + area.w - aw, y, aw, rowH};
    float cadence = (float)((i - startIdx) % 3) * 0.025f;
    Color levelColor = mixColor(t.panelAlt, c, 0.58f + 0.16f * rel + cadence);
    if (st.showBars) {
      if (st.showGradient) {
        u.draw.rectGradientHDithered(
            amountBar, withAlpha(levelColor, (0.025f + 0.035f * rel) * visual),
            withAlpha(levelColor, (0.16f + 0.16f * rel) * visual));
        u.draw.rectGradientV(
            amountBar, withAlpha(levelColor, (0.035f + cadence * 0.3f) * visual),
            withAlpha(levelColor, 0.0f));
      } else {
        u.draw.rect(amountBar,
                    withAlpha(levelColor, (0.11f + 0.14f * rel) * visual));
      }
    }

    // Outline only the exposed perimeter of the connected bar silhouette.
    // Adjacent rows share the overlap between their widths, so drawing a full
    // rectangle per level would create horizontal seams through the field.
    if (st.showBars && st.showEdges && aw >= 2.0f) {
      Color edge = withAlpha(levelColor, (0.34f + 0.16f * rel) * visual);
      float right = area.x + area.w;
      auto connectedWidth = [&](int neighbor) {
        if (neighbor < startIdx || neighbor >= endIdx) return 0.0f;
        const OrderbookPanel::Level& N = st.ladder[(size_t)neighbor];
        if (N.ask != L.ask) return 0.0f;
        return shapeDepth((float)(amountValue(N) / maxAmt), st.scaleMode) *
               (area.w * barWidth);
      };
      float prevW = connectedWidth(i - 1);
      float nextW = connectedWidth(i + 1);
      float topExposed = std::max(0.0f, aw - prevW);
      float bottomExposed = std::max(0.0f, aw - nextW);

      u.draw.line(amountBar.x, y, amountBar.x, y + rowH, edge, 1.0f);
      u.draw.line(right, y, right, y + rowH, edge, 1.0f);
      if (topExposed > 0.0f)
        u.draw.line(amountBar.x, y, amountBar.x + topExposed, y, edge, 1.0f);
      if (bottomExposed > 0.0f)
        u.draw.line(amountBar.x, y + rowH, amountBar.x + bottomExposed, y + rowH,
                    edge, 1.0f);
    }

    // Staggered micro-ribs give wide levels a quiet material texture. Their
    // spacing and inset vary deterministically by row, avoiding animated noise.
    if (st.showBars && st.showTexture && aw > 18.0f) {
      int cadenceIndex = (i - startIdx) % 3;
      float inset = 2.0f + (float)(cadenceIndex & 1);
      float spacing = 7.0f + (float)cadenceIndex;
      float left = amountBar.x + spacing * 0.65f;
      for (float tx = area.x + area.w - spacing; tx > left; tx -= spacing)
        u.draw.rect({tx, y + inset, 1.0f, rowH - inset * 2.0f},
                    withAlpha(t.bg, 0.055f + 0.04f * rel));
    }

    Rect priceCell{priceColumn.x, y, priceColumn.w, rowH};
    Rect amountCell{amountColumn.x, y, amountColumn.w, rowH};
    u.draw.textFit(priceCell, L.priceLbl, c, DrawList::Left);
    u.draw.textFit(amountCell, L.sizeLbl, t.text, DrawList::Right);
    y += rowH;
  }
}

// Per-venue book-source popover (shared body, see flow_sources.h). The merged
// ladder + analytics rebuild on the next frame because mergeMask drifts from
// the new mask.
void OrderbookPanel::drawFlowPicker(Ui& u, Feeds& feeds) {
  if (!u.overlayOpen(flowPickerId)) return;
  if (drawFlowSources(u, flowPickerRect, mask, feeds, flowPickerList))
    saveOrderbookSettings(*this);
}
