#include "panels.h"
#include "panels_common.h"

#include "../../platform/shell.h"
#include "../../ui/theme.h"
#include "../price_format.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void saveSettings(LiquidationsPanel& st) {
  st.sideMode = std::clamp(st.sideMode, 0, 2);
  st.density = std::clamp(st.density, 0, 2);
  st.intensity = std::clamp(st.intensity, 0, 2);
  st.amountPrecision = std::clamp(st.amountPrecision, 0, 3);
  char saved[256];
  snprintf(saved, sizeof(saved), "1,%.12g,%.12g,%d,%d,%d,%d,%d,%d,%d,%d,%d",
           st.minUsd, st.maxUsd, st.showUsd, st.sideMode, st.density,
           st.showTime, st.showNoise, st.showGradient, st.showMarker,
           st.intensity, st.amountPrecision);
  shell_storage_set(st.settingsKey.c_str(), saved);
}

void loadSettings(LiquidationsPanel& st) {
  if (st.settingsLoaded) return;
  st.settingsLoaded = true;
  if (char* saved = shell_storage_get(st.settingsKey.c_str())) {
    double minUsd = 0, maxUsd = 0;
    int version = 0, showUsd = 1, side = 0, density = 1, showTime = 1;
    int noise = 1, gradient = 1, marker = 1, intensity = 1, precision = 2;
    int parsed = sscanf(saved, "%d,%lf,%lf,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                        &version, &minUsd, &maxUsd, &showUsd, &side, &density,
                        &showTime, &noise, &gradient, &marker, &intensity,
                        &precision);
    if (parsed == 12 && version == 1 && std::isfinite(minUsd) && minUsd >= 0 &&
        std::isfinite(maxUsd) && maxUsd >= 0 &&
        (maxUsd == 0 || maxUsd >= minUsd)) {
      st.minUsd = minUsd;
      st.maxUsd = maxUsd;
      st.showUsd = showUsd != 0;
      st.sideMode = side;
      st.density = density;
      st.showTime = showTime != 0;
      st.showNoise = noise != 0;
      st.showGradient = gradient != 0;
      st.showMarker = marker != 0;
      st.intensity = intensity;
      st.amountPrecision = precision;
      saveSettings(st);
    }
    std::free(saved);
  }
  char lo[32], hi[32];
  snprintf(lo, sizeof(lo), "%.10g", st.minUsd);
  snprintf(hi, sizeof(hi), "%.10g", st.maxUsd);
  st.minInput.text = lo;
  st.maxInput.text = hi;
}

void invalidate(LiquidationsPanel& st) {
  st.filterVersion = ~0ull;
  st.list.scroll = 0;
}

void setFilter(LiquidationsPanel& st, double minUsd, double maxUsd) {
  st.minUsd = minUsd;
  st.maxUsd = maxUsd;
  char lo[32], hi[32];
  snprintf(lo, sizeof(lo), "%.10g", minUsd);
  snprintf(hi, sizeof(hi), "%.10g", maxUsd);
  st.minInput.text = lo;
  st.maxInput.text = hi;
  st.filterError.clear();
  invalidate(st);
  saveSettings(st);
}

bool applyFilter(LiquidationsPanel& st) {
  double minUsd = 0, maxUsd = 0;
  if (!parseUsd(st.minInput.text, minUsd) || !parseUsd(st.maxInput.text, maxUsd)) {
    st.filterError = "enter a positive USD value";
    return false;
  }
  if (maxUsd > 0 && maxUsd < minUsd) {
    st.filterError = "maximum must be above minimum";
    return false;
  }
  setFilter(st, minUsd, maxUsd);
  return true;
}

void closeFields(LiquidationsPanel& st) {
  if (st.minInput.focused || st.maxInput.focused) shell_ime_blur();
  st.minInput.focused = false;
  st.maxInput.focused = false;
}

void rebuildFilter(LiquidationsPanel& st, const MarketSeries& market) {
  if (st.filterVersion == market.liqVersion && st.filterMin == st.minUsd &&
      st.filterMax == st.maxUsd && st.filterSide == st.sideMode)
    return;
  st.filtered.clear();
  st.filtered.reserve(market.liq.size());
  for (size_t n = market.liq.size(); n-- > 0;) {
    const LiqPrint& e = market.liq[n];
    double usd = std::fabs(e.price * e.qty);
    // side 0 is a forced buy (short liquidation), side 1 a forced sell (long liquidation)
    if (st.sideMode == 1 && e.side != 0) continue;
    if (st.sideMode == 2 && e.side != 1) continue;
    if (usd < st.minUsd) continue;
    if (st.maxUsd > 0 && usd > st.maxUsd) continue;
    st.filtered.push_back(n);
  }
  st.filterVersion = market.liqVersion;
  st.filterMin = st.minUsd;
  st.filterMax = st.maxUsd;
  st.filterSide = st.sideMode;
}

void drawNoise(DrawList& d, Rect area, const MarketSeries& market,
               const LiquidationsPanel& st) {
  if (!st.showNoise || market.liq.empty() || area.w < 8 || area.h < 8) return;
  const Theme& t = theme();
  constexpr int kBins = 96;
  float shortEnergy[kBins] = {}, longEnergy[kBins] = {};
  double newest = market.liq.back().ts;
  if (!(newest > 0)) return;
  const double window = 120000.0;
  double oldest = newest - window;
  float maxEnergy = 0;
  for (size_t index : st.filtered) {
    const LiqPrint& e = market.liq[index];
    if (e.ts < oldest || e.ts > newest) continue;
    double usd = std::fabs(e.price * e.qty);
    float energy = (float)std::clamp(std::log1p(usd) / 12.0, 0.08, 1.0);
    int bin = std::clamp((int)((e.ts - oldest) / window * kBins), 0, kBins - 1);
    if (e.side == 0) shortEnergy[bin] += energy;
    else longEnergy[bin] += energy;
    maxEnergy = std::max(maxEnergy, std::max(shortEnergy[bin], longEnergy[bin]));
  }
  if (!(maxEnergy > 0)) return;

  const float base = area.y + area.h * 0.5f;
  d.rect({area.x, base, area.w, 1}, withAlpha(t.textDim, 0.28f));
  float visual = kPanelIntensity[std::clamp(st.intensity, 0, 2)];
  for (int i = 0; i < kBins; ++i) {
    float x = area.x + area.w * ((float)i + 0.5f) / kBins;
    float sw = std::min(shortEnergy[i] / maxEnergy, 2.0f);
    float lw = std::min(longEnergy[i] / maxEnergy, 2.0f);
    if (sw > 0) {
      float h = std::max(1.0f, area.h * 0.46f * sw / 2.0f);
      d.rect({x - 1, base - h, 2, h}, withAlpha(t.green, (0.12f + 0.30f * sw) * visual));
    }
    if (lw > 0) {
      float h = std::max(1.0f, area.h * 0.46f * lw / 2.0f);
      d.rect({x - 1, base, 2, h}, withAlpha(t.red, (0.12f + 0.30f * lw) * visual));
    }
  }

  // Event-level hairlines add the requested noisy texture without using
  // nondeterministic random state, so screenshots and replay remain stable.
  for (size_t n = 0; n < st.filtered.size(); ++n) {
    const LiqPrint& e = market.liq[st.filtered[n]];
    if (e.ts < oldest || e.ts > newest) continue;
    float x = area.x + area.w * (float)((e.ts - oldest) / window);
    uint32_t hash = (uint32_t)(n * 0x9e3779b9u + 0x7f4a7c15u);
    x += (float)((hash >> 28) & 3) - 1.5f;
    double usd = std::fabs(e.price * e.qty);
    float h = std::clamp((float)(std::log10(std::max(usd, 1.0)) - 2.0) * 3.0f,
                         1.0f, area.h * 0.44f);
    Color side = e.side == 0 ? t.green : t.red;
    d.rect({std::floor(x), e.side == 0 ? base - h : base, 1, h},
           withAlpha(side, 0.22f * visual));
  }
}

void drawSettings(Ui& u, Rect area, LiquidationsPanel& st) {
  const Theme& t = theme();
  constexpr int kRows = 10;
  const float rowH = area.w < 190 ? 94.0f : area.w < 340 ? 68.0f : 44.0f;
  listView(u, area, kRows, rowH, st.settingsList,
           [&](Ui& rowUi, DrawList& d, Rect row, int index) {
             char id[32];
             snprintf(id, sizeof(id), "##liq-setting-%d", index);
             rowUi.pushId(id);
             d.rect({row.x + 10, row.y + row.h - 1, row.w - 20, 1},
                    withAlpha(t.border, 0.65f));
             static constexpr const char* titles[] = {
                 "AMOUNT UNIT", "SIDE", "ROW DENSITY", "COLUMNS",
                 "NOISE", "VISUAL INTENSITY", "MINIMUM USD", "MAXIMUM USD",
                 "FILTER PRESETS", "ACTIONS"};
             d.textAligned({row.x + 10, row.y + 2, row.w - 20, 16}, titles[index],
                           t.textDim, DrawList::Left);
             float x = row.x + 10, y = row.y + 19;
             const float right = row.x + row.w - 10;
             auto option = [&](const char* label, float width, bool active, auto fn) {
               if (x > row.x + 10 && x + width > right) { x = row.x + 10; y += 25; }
               if (y + 22 <= row.y + row.h - 2 && chip(rowUi, {x, y, width, 21}, label, active)) {
                 fn();
                 saveSettings(st);
               }
               x += width + 4;
             };
             switch (index) {
               case 0:
                 option("USD", 58, st.showUsd, [&] { st.showUsd = true; });
                 option("COIN", 58, !st.showUsd, [&] { st.showUsd = false; });
                 break;
               case 1:
                 option("ALL", 48, st.sideMode == 0, [&] { st.sideMode = 0; });
                 option("SHORT LIQ", 82, st.sideMode == 1, [&] { st.sideMode = 1; });
                 option("LONG LIQ", 78, st.sideMode == 2, [&] { st.sideMode = 2; });
                 break;
               case 2:
                 option("TIGHT", 62, st.density == 0, [&] { st.density = 0; });
                 option("NORMAL", 70, st.density == 1, [&] { st.density = 1; });
                 option("RELAXED", 76, st.density == 2, [&] { st.density = 2; });
                 break;
               case 3:
                 option("TIME", 58, st.showTime, [&] { st.showTime = !st.showTime; });
                 break;
               case 4:
                 option("NOISE", 66, st.showNoise, [&] { st.showNoise = !st.showNoise; });
                 option("GRADIENT", 82, st.showGradient,
                        [&] { st.showGradient = !st.showGradient; });
                 option("MARKER", 70, st.showMarker,
                        [&] { st.showMarker = !st.showMarker; });
                 break;
               case 5:
                 option("QUIET", 62, st.intensity == 0, [&] { st.intensity = 0; });
                 option("NORMAL", 70, st.intensity == 1, [&] { st.intensity = 1; });
                 option("STRONG", 70, st.intensity == 2, [&] { st.intensity = 2; });
                 break;
                case 6:
                  if (textField(rowUi, {x, y - 1, std::min(190.0f, row.w - 20.0f), 24},
                                st.minInput, "##liq-min", "$0")) st.filterError.clear();
                  // Consume Enter at the field: the APPLY row may be scrolled
                  // out of view when the key lands.
                  if (st.minInput.submitted) {
                    st.minInput.submitted = false;
                    applyFilter(st);
                  }
                  break;
                case 7:
                  if (textField(rowUi, {x, y - 1, std::min(190.0f, row.w - 20.0f), 24},
                                st.maxInput, "##liq-max", "no maximum")) st.filterError.clear();
                  if (st.maxInput.submitted) {
                    st.maxInput.submitted = false;
                    applyFilter(st);
                  }
                  break;
               case 8: {
                 struct Preset { const char* label; double min; };
                 static constexpr Preset presets[] = {
                     {"ALL", 0}, {"$1K+", 1e3}, {"$10K+", 1e4},
                     {"$50K+", 5e4}, {"$100K+", 1e5}};
                 for (const Preset& p : presets)
                   option(p.label, p.min == 0 ? 48.0f : 62.0f,
                          st.minUsd == p.min && st.maxUsd == 0,
                          [&] { setFilter(st, p.min, 0); });
                 break;
               }
                case 9: {
                  float w = std::min(72.0f, row.w - 20.0f);
                  if (button(rowUi, {x, y, w, 23}, "APPLY")) applyFilter(st);
                 if (button(rowUi, {x + w + 6, y, std::min(112.0f, right - x - w - 6), 23},
                            "RESET VIEW")) {
                   st.showUsd = true;
                   st.showTime = st.showNoise = st.showGradient = st.showMarker = true;
                   st.sideMode = 0; st.density = st.intensity = 1;
                   st.amountPrecision = 2;
                   setFilter(st, 0, 0);
                   saveSettings(st);
                 }
                 break;
               }
             }
             rowUi.popId();
           }, false);
}

} // namespace

const char* LiquidationsPanel::timeLabel(int64_t secs) {
  return timeLabels.label(secs); // shared direct-mapped cache
}

void drawLiquidations(Ui& u, Rect r, LiquidationsPanel& st, Feeds& feeds) {
  const Theme& t = theme();
  loadSettings(st);
  rebuildFilter(st, feeds.market);

  Rect toolbar{r.x, r.y, r.w, 22};
  u.draw.rect(toolbar, t.panelAlt);
  u.draw.rect({toolbar.x, toolbar.y + toolbar.h, toolbar.w, 1}, t.border);

  char lo[32], hi[32], status[112];
  if (st.minUsd == 0 && st.maxUsd == 0) snprintf(status, sizeof(status), "AGG LIQ  ·  %zu", st.filtered.size());
  else if (st.maxUsd == 0) {
    formatUsd(st.minUsd, lo, sizeof(lo));
    snprintf(status, sizeof(status), "AGG LIQ %s+  ·  %zu", lo, st.filtered.size());
  } else {
    formatUsd(st.minUsd, lo, sizeof(lo)); formatUsd(st.maxUsd, hi, sizeof(hi));
    snprintf(status, sizeof(status), "AGG LIQ %s—%s  ·  %zu", lo, hi, st.filtered.size());
  }
  bool wide = r.w >= 300;
  float settingsW = r.w < 90 ? std::max(24.0f, r.w - 4) : wide ? 70.0f : 50.0f;
  Rect settingsButton{std::max(toolbar.x + 2.0f, toolbar.x + toolbar.w - settingsW - 6),
                      toolbar.y + 2, settingsW, 18};
  Rect statusRect{toolbar.x + 8, toolbar.y,
                  std::max(0.0f, settingsButton.x - toolbar.x - 12), toolbar.h};
  if (statusRect.w >= 30) u.draw.textFit(statusRect, status, t.textDim, DrawList::Left);

  Behavior sb = behavior(u, settingsButton, u.id("##liq-settings"));
  if (sb.hovered || sb.held) u.draw.rect(settingsButton, sb.held ? t.bgRaised : t.bgHover, 1);
  u.draw.textAligned(settingsButton, st.settingsOpen ? "DONE" : wide ? "SETTINGS" : "SET",
                     sb.hovered ? t.text : t.textDim, DrawList::Center);
  if (sb.clicked) {
    st.settingsOpen = !st.settingsOpen;
    if (!st.settingsOpen) closeFields(st);
  }

  Rect content{r.x, r.y + 23, r.w, r.h - 23};
  if (st.settingsOpen) {
    drawSettings(u, content, st);
    return;
  }

  const float noiseH = st.showNoise && content.h >= 42 ? 40.0f : 0.0f;
  Rect noise{content.x + 6, content.y + 2, std::max(0.0f, content.w - 12), noiseH};
  if (noiseH > 0) drawNoise(u.draw, noise, feeds.market, st);
  Rect rows{content.x, content.y + noiseH + (noiseH > 0 ? 4 : 0), content.w,
            content.h - noiseH - (noiseH > 0 ? 4 : 0)};

  if (feeds.market.liq.empty()) {
    u.draw.textAligned(rows, "waiting for liquidations…", t.textDim, DrawList::Center);
    return;
  }
  if (st.filtered.empty()) {
    u.draw.textAligned(rows, "no liquidations match this filter", t.textDim, DrawList::Center);
    return;
  }

  const bool showTime = st.showTime && r.w >= 190;
  const float pad = r.w < 140 ? 4.0f : 10.0f;
  const float gap = r.w < 140 ? 2.0f : 6.0f;
  float inner = std::max(0.0f, r.w - pad * 2);
  float timeW = showTime ? 72.0f : 0.0f;
  float sideW = r.w >= 220 ? 66.0f : 0.0f;
  float meta = timeW + sideW + ((timeW > 0 && sideW > 0) ? gap : 0);
  if (meta > 0) meta += gap;
  float core = std::max(0.0f, inner - meta);
  float priceW = core * 0.48f;
  const float headerH = rows.h >= 70 ? 17.0f : 0.0f;
  float x = rows.x + pad;
  Rect timeCol{x, rows.y, timeW, headerH}; if (showTime) x += timeW + gap;
  Rect sideCol{x, rows.y, sideW, headerH}; if (sideW > 0) x += sideW + gap;
  Rect priceCol{x, rows.y, priceW, headerH};
  Rect amountCol{x + priceW + gap, rows.y, std::max(0.0f, core - priceW - gap), headerH};
  if (headerH > 0) {
    if (showTime) u.draw.textFit(timeCol, "TIME", t.textDim, DrawList::Left);
    if (sideW > 0) u.draw.textFit(sideCol, "TYPE", t.textDim, DrawList::Left);
    u.draw.textFit(priceCol, "PRICE", t.textDim, DrawList::Left);
    u.draw.textFit(amountCol, st.showUsd ? "SIZE (USD)" : "SIZE", t.textDim, DrawList::Right);
    u.draw.rect({rows.x, rows.y + headerH, rows.w, 1}, t.border);
  }
  Rect listArea{rows.x, rows.y + headerH, rows.w, rows.h - headerH};
  float rowH = kPanelRowHeights[std::clamp(st.density, 0, 2)];
  float visual = kPanelIntensity[std::clamp(st.intensity, 0, 2)];
  listView(u, listArea, (int)st.filtered.size(), rowH, st.list,
           [&](DrawList& d, Rect row, int i) {
             const LiqPrint& e = feeds.market.liq[st.filtered[(size_t)i]];
             double usd = std::fabs(e.price * e.qty);
             float tier = std::clamp((float)((std::log10(std::max(usd, 100.0)) - 3.0) / 3.0), 0.0f, 1.0f);
             float shaped = tier * tier * (3.0f - 2.0f * tier);
             Color side = e.side == 0 ? t.green : t.red;
             if (st.showGradient)
               d.rectGradientHDithered(row, withAlpha(side, 0.002f),
                                       withAlpha(side, (0.02f + 0.26f * shaped) * visual));
             else
               d.rect(row, withAlpha(side, (0.014f + 0.20f * shaped) * visual));
             if (st.showMarker)
               d.rect({row.x, row.y + 2, shaped > 0.72f ? 3.0f : 2.0f, row.h - 4},
                      withAlpha(side, (0.16f + 0.76f * shaped) * visual));

             char price[32], amount[32];
             snprintf(price, sizeof(price), "%.*f", priceDecimalsForPrice(e.price),
                      e.price);
             if (st.showUsd) formatUsd(usd, amount, sizeof(amount));
             else formatCoin(e.qty, st.amountPrecision, amount, sizeof(amount));
             const char* type = e.side == 0 ? "SHORT LIQ" : "LONG LIQ";
             if (showTime)
               d.textFit({timeCol.x, row.y, timeCol.w, row.h},
                         st.timeLabel((int64_t)(e.ts / 1000.0)), t.textDim, DrawList::Left);
             if (sideW > 0) d.textFit({sideCol.x, row.y, sideCol.w, row.h}, type,
                                      mixColor(t.textDim, side, 0.45f), DrawList::Left);
             d.textFit({priceCol.x, row.y, priceCol.w, row.h}, price, side, DrawList::Left);
             d.textFit({amountCol.x, row.y, amountCol.w, row.h}, amount,
                       mixColor(t.text, side, 0.18f + 0.72f * shaped), DrawList::Right);
           }, false);
}
