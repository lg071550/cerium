#include "panels.h"
#include "panels_common.h"

#include "../../platform/shell.h"
#include "../../ui/theme.h"
#include "../symbols.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

namespace {

void saveTapeSettings(TapePanel& st) {
  st.density = std::clamp(st.density, 0, 2);
  st.intensity = std::clamp(st.intensity, 0, 2);
  st.priceDecimals = std::clamp(st.priceDecimals, 0, 4);
  st.amountPrecision = std::clamp(st.amountPrecision, 0, 3);
  char buf[256];
  snprintf(buf, sizeof(buf),
           "1,%.12g,%.12g,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
           st.minUsd, st.maxUsd, st.showUsd, st.density, st.showTime,
           st.showVenue, st.showWash, st.showGradient, st.showMarker,
           st.intensity, st.priceDecimals, st.amountPrecision);
  shell_storage_set(st.settingsKey.c_str(), buf);
}

void loadTapeSettings(TapePanel& st) {
  if (st.settingsLoaded) return;
  st.settingsLoaded = true;

  if (char* saved = shell_storage_get(st.settingsKey.c_str())) {
    double minUsd = 0.0, maxUsd = 0.0;
    int version = 0, showUsd = 1, density = 1, showTime = 1, showVenue = 1;
    int showWash = 1, showGradient = 1, showMarker = 1, intensity = 1;
    int priceDecimals = 2, amountPrecision = 2;
    int parsed = sscanf(saved,
                        "%d,%lf,%lf,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                        &version, &minUsd, &maxUsd, &showUsd, &density,
                        &showTime, &showVenue, &showWash, &showGradient,
                        &showMarker, &intensity, &priceDecimals,
                        &amountPrecision);
    if (parsed == 13 && version == 1 &&
        std::isfinite(minUsd) && minUsd >= 0.0 && std::isfinite(maxUsd) &&
        maxUsd >= 0.0 && (maxUsd == 0.0 || maxUsd >= minUsd)) {
      st.minUsd = minUsd;
      st.maxUsd = maxUsd;
      st.showUsd = showUsd != 0;
      st.density = density;
      st.showTime = showTime != 0;
      st.showVenue = showVenue != 0;
      st.showWash = showWash != 0;
      st.showGradient = showGradient != 0;
      st.showMarker = showMarker != 0;
      st.intensity = intensity;
      st.priceDecimals = priceDecimals;
      st.amountPrecision = amountPrecision;
      saveTapeSettings(st);
    } else if (sscanf(saved, "%lf,%lf,%d", &minUsd, &maxUsd, &showUsd) == 3 &&
               std::isfinite(minUsd) && minUsd >= 0.0 &&
               std::isfinite(maxUsd) && maxUsd >= 0.0 &&
               (maxUsd == 0.0 || maxUsd >= minUsd)) {
      // Migrate the original unit/filter-only payload.
      st.minUsd = minUsd;
      st.maxUsd = maxUsd;
      st.showUsd = showUsd != 0;
      saveTapeSettings(st);
    }
    std::free(saved);
  }
  formatFilterValue(st.minUsd, st.minInput.text);
  formatFilterValue(st.maxUsd, st.maxInput.text);
}

void invalidateTapeFilter(TapePanel& st) {
  st.filterHead = static_cast<size_t>(-1);
  st.list.scroll = 0.0f;
}

void setTapeFilter(TapePanel& st, double minUsd, double maxUsd) {
  st.minUsd = minUsd;
  st.maxUsd = maxUsd;
  formatFilterValue(minUsd, st.minInput.text);
  formatFilterValue(maxUsd, st.maxInput.text);
  st.filterError.clear();
  invalidateTapeFilter(st);
  saveTapeSettings(st);
}

bool applyTapeFilter(TapePanel& st) {
  double minUsd = 0.0, maxUsd = 0.0;
  if (!parseUsd(st.minInput.text, minUsd) || !parseUsd(st.maxInput.text, maxUsd)) {
    st.filterError = "Enter a positive USD value";
    return false;
  }
  if (maxUsd > 0.0 && maxUsd < minUsd) {
    st.filterError = "Maximum must be above minimum";
    return false;
  }
  setTapeFilter(st, minUsd, maxUsd);
  return true;
}

void closeTapeFields(TapePanel& st) {
  if (st.minInput.focused || st.maxInput.focused) shell_ime_blur();
  st.minInput.focused = false;
  st.maxInput.focused = false;
}

void rebuildTapeFilter(TapePanel& st, const Tape& tape) {
  // Filter edits rebuild immediately; a moved ring head does not — during
  // active trading head advances nearly every frame and the full O(ring)
  // rescan would run per print. ~10 Hz keeps new rows within 100 ms.
  static thread_local std::chrono::steady_clock::time_point lastHeadRebuild{};
  const bool filterChanged =
      st.filterMin != st.minUsd || st.filterMax != st.maxUsd;
  if (st.filterHead == tape.head && st.filterCount == tape.count &&
      st.filterMin == st.minUsd && st.filterMax == st.maxUsd)
    return;
  if (!filterChanged) {
    auto now = std::chrono::steady_clock::now();
    if (now - lastHeadRebuild < std::chrono::milliseconds(100)) return;
    lastHeadRebuild = now;
  }

  st.filtered.clear();
  if (st.filtered.capacity() < Tape::CAP) st.filtered.reserve(Tape::CAP);
  for (size_t i = 0; i < tape.count; ++i) {
    const TapeEntry* e = tape.latest(i);
    if (!e) continue;
    double usd = std::fabs(e->price * e->qty);
    if (usd < st.minUsd) continue;
    if (st.maxUsd > 0.0 && usd > st.maxUsd) continue;
    st.filtered.push_back(i);
  }
  st.filterHead = tape.head;
  st.filterCount = tape.count;
  st.filterMin = st.minUsd;
  st.filterMax = st.maxUsd;
}

void drawTapeSettings(Ui& u, Rect area, TapePanel& st) {
  const Theme& t = theme();
  constexpr int kRows = 12;
  const float settingsRowH = area.w < 110.0f ? 150.0f
                             : area.w < 190.0f ? 94.0f
                             : area.w < 340.0f ? 68.0f
                                               : 44.0f;
  listView(u, area, kRows, settingsRowH, st.settingsList,
           [&](Ui& rowUi, DrawList& d, Rect row, int index) {
             char rowId[32];
             snprintf(rowId, sizeof(rowId), "##tape-setting-%d", index);
             rowUi.pushId(rowId);
             d.rect({row.x + 10, row.y + row.h - 1, row.w - 20, 1},
                    withAlpha(t.border, 0.65f));
             static constexpr const char* titles[] = {
                 "AMOUNT UNIT", "ROW DENSITY", "COLUMNS", "COLOR LAYERS",
                 "VISUAL INTENSITY", "PRICE DECIMALS", "AMOUNT DECIMALS",
                 "MINIMUM USD", "MAXIMUM USD", "FILTER PRESETS", "ACTIONS",
                 "ACTIVE FILTER"};
             d.textAligned({row.x + 10, row.y + 2, row.w - 20, 16}, titles[index],
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
                 saveTapeSettings(st);
               }
               x += width + 4;
             };

             switch (index) {
               case 0:
                 option("USD", 58, st.showUsd, [&] { st.showUsd = true; });
                 option("COIN", 58, !st.showUsd, [&] { st.showUsd = false; });
                 break;
               case 1:
                 option("TIGHT", 64, st.density == 0, [&] { st.density = 0; });
                 option("NORMAL", 70, st.density == 1, [&] { st.density = 1; });
                 option("RELAXED", 76, st.density == 2, [&] { st.density = 2; });
                 break;
               case 2:
                 option("TIME", 58, st.showTime, [&] { st.showTime = !st.showTime; });
                 option("VENUE", 66, st.showVenue,
                        [&] { st.showVenue = !st.showVenue; });
                 break;
               case 3:
                 option("WASH", 60, st.showWash, [&] { st.showWash = !st.showWash; });
                 option("GRADIENT", 82, st.showGradient,
                        [&] { st.showGradient = !st.showGradient; });
                 option("MARKER", 70, st.showMarker,
                        [&] { st.showMarker = !st.showMarker; });
                 break;
               case 4:
                 option("QUIET", 64, st.intensity == 0, [&] { st.intensity = 0; });
                 option("NORMAL", 70, st.intensity == 1, [&] { st.intensity = 1; });
                 option("STRONG", 70, st.intensity == 2, [&] { st.intensity = 2; });
                 break;
               case 5:
                 for (int i = 0; i < 5; ++i) {
                   char label[4];
                   snprintf(label, sizeof(label), "%d", i);
                   option(label, 42, st.priceDecimals == i,
                          [&, i] { st.priceDecimals = i; });
                 }
                 break;
               case 6: {
                 static constexpr const char* labels[] = {"0", "2", "3", "5"};
                 for (int i = 0; i < 4; ++i)
                   option(labels[i], 46, st.amountPrecision == i,
                          [&, i] { st.amountPrecision = i; });
                 break;
               }
                case 7:
                  if (textField(rowUi, {x, y - 1, std::min(190.0f, row.w - 20), 24},
                                st.minInput, "##tape-min", "$0"))
                    st.filterError.clear();
                  // Consume Enter at the field itself: the APPLY row can be
                  // scrolled out of view, and a deferred flag would fire a
                  // ghost apply whenever it scrolled back in.
                  if (st.minInput.submitted) {
                    st.minInput.submitted = false;
                    applyTapeFilter(st);
                  }
                  break;
                case 8:
                  if (textField(rowUi, {x, y - 1, std::min(190.0f, row.w - 20), 24},
                                st.maxInput, "##tape-max", "no maximum"))
                    st.filterError.clear();
                  if (st.maxInput.submitted) {
                    st.maxInput.submitted = false;
                    applyTapeFilter(st);
                  }
                  break;
               case 9: {
                 struct Preset { const char* label; double min; };
                 static constexpr Preset presets[] = {
                     {"ALL", 0.0}, {"$1K+", 1.0e3}, {"$10K+", 1.0e4},
                     {"$50K+", 5.0e4}, {"$100K+", 1.0e5}};
                 for (const Preset& preset : presets)
                   option(preset.label, preset.min == 0 ? 48.0f : 62.0f,
                          st.minUsd == preset.min && st.maxUsd == 0.0,
                          [&] { setTapeFilter(st, preset.min, 0.0); });
                 break;
               }
                case 10: {
                  float actionW = std::min(72.0f, row.w - 20.0f);
                  if (button(rowUi, {x, y, actionW, 23}, "APPLY"))
                    applyTapeFilter(st);
                 float resetX = x + actionW + 6.0f;
                 float resetY = y;
                 if (resetX + 80.0f > optionRight) {
                   resetX = x;
                   resetY += 25.0f;
                 }
                 if (button(rowUi,
                            {resetX, resetY,
                             std::min(118.0f, optionRight - resetX), 23},
                            "RESET VIEW")) {
                   st.showUsd = true;
                   st.showTime = st.showVenue = st.showWash = st.showGradient =
                       st.showMarker = true;
                   st.density = st.intensity = 1;
                   st.priceDecimals = 2;
                   st.amountPrecision = 2;
                   saveTapeSettings(st);
                 }
                 break;
               }
               case 11: {
                 char active[128], lo[32], hi[32];
                 formatUsd(st.minUsd, lo, sizeof(lo));
                 if (st.minUsd == 0.0 && st.maxUsd == 0.0)
                   snprintf(active, sizeof(active), "ALL TRADES");
                 else if (st.maxUsd == 0.0)
                   snprintf(active, sizeof(active), "%s AND ABOVE", lo);
                 else {
                   formatUsd(st.maxUsd, hi, sizeof(hi));
                   snprintf(active, sizeof(active), "%s — %s", lo, hi);
                 }
                 if (st.filterError.empty())
                   d.textFit({x, y, row.w - 20, h}, active, t.accent,
                             DrawList::Left);
                 else
                   d.textFit({x, y, row.w - 20, h}, st.filterError.c_str(), t.red,
                             DrawList::Left);
                 break;
               }
             }
             rowUi.popId();
            }, false);
}

} // namespace

// cached "HH:MM:SS" — shared direct-mapped implementation on TimeLabels
const char* TapePanel::timeLabel(int64_t secs) {
  return timeLabels.label(secs);
}

void drawTape(Ui& u, Rect r, TapePanel& st, Feeds& feeds) {
  const Theme& t = theme();
  loadTapeSettings(st);

  const Tape& tape = feeds.tape;
  rebuildTapeFilter(st, tape);

  // Widget-local toolbar: current filter state remains visible while the tape
  // is running, and SETTINGS swaps the data table for this widget's controls.
  Rect toolbar{r.x, r.y, r.w, 22};
  u.draw.rect(toolbar, t.panelAlt);
  u.draw.rect({toolbar.x, toolbar.y + toolbar.h, toolbar.w, 1}, t.border);

  char status[112], lo[32], hi[32];
  if (st.minUsd == 0.0 && st.maxUsd == 0.0) {
    snprintf(status, sizeof(status), "ALL TRADES  ·  %zu", st.filtered.size());
  } else if (st.maxUsd == 0.0) {
    formatUsd(st.minUsd, lo, sizeof(lo));
    snprintf(status, sizeof(status), "%s+  ·  %zu", lo, st.filtered.size());
  } else {
    formatUsd(st.minUsd, lo, sizeof(lo));
    formatUsd(st.maxUsd, hi, sizeof(hi));
    snprintf(status, sizeof(status), "%s — %s  ·  %zu", lo, hi,
             st.filtered.size());
  }
  const bool wideToolbar = r.w >= 300.0f;
  const float settingsW = r.w < 90.0f ? std::max(24.0f, r.w - 4.0f)
                                       : wideToolbar ? 70.0f : 50.0f;
  Rect settingsButton{std::max(toolbar.x + 2.0f,
                               toolbar.x + toolbar.w - settingsW - 6.0f),
                      toolbar.y + 2.0f, settingsW, 18.0f};
  const bool showClear = r.w >= 118.0f;
  const float clearW = wideToolbar ? 52.0f : 42.0f;
  Rect clearButton{settingsButton.x - clearW - 6.0f, settingsButton.y, clearW,
                   settingsButton.h};
  Rect statusRect{toolbar.x + 8.0f, toolbar.y,
                  std::max(0.0f, (showClear ? clearButton.x : settingsButton.x) -
                                     toolbar.x - 12.0f),
                  toolbar.h};
  if (statusRect.w >= 26.0f)
    u.draw.textFit(statusRect, status, t.textDim, DrawList::Left);

  if (showClear) {
    Behavior clearBehavior = behavior(u, clearButton, u.id("##tape-clear"));
    if (clearBehavior.hovered || clearBehavior.held)
      u.draw.rect(clearButton, clearBehavior.held ? t.bgRaised : t.bgHover, 1.0f);
    u.draw.textAligned(clearButton, wideToolbar ? "CLEAR" : "CLR",
                       clearBehavior.hovered ? t.text : t.textDim,
                       DrawList::Center);
    u.tip(u.id("##tape-clear-tip"), clearButton, "clear retained trades");
    if (clearBehavior.clicked) {
      feeds.tape.clear();
      st.filtered.clear();
      invalidateTapeFilter(st);
    }
  }

  Behavior settingsBehavior = behavior(u, settingsButton, u.id("##tape-settings"));
  if (settingsBehavior.hovered || settingsBehavior.held)
    u.draw.rect(settingsButton, settingsBehavior.held ? t.bgRaised : t.bgHover,
                1.0f);
  u.draw.textAligned(settingsButton,
                     st.settingsOpen ? "DONE" : wideToolbar ? "SETTINGS" : "SET",
                     settingsBehavior.hovered ? t.text : t.textDim,
                     DrawList::Center);
  if (settingsBehavior.clicked) {
    st.settingsOpen = !st.settingsOpen;
    if (!st.settingsOpen) closeTapeFields(st);
  }

  Rect content{r.x, r.y + 23, r.w, r.h - 23};
  if (st.settingsOpen) {
    drawTapeSettings(u, content, st);
    return;
  }

  const bool showColumnHeader = r.h >= 70.0f;
  const float headerBlockH = showColumnHeader ? 18.0f : 0.0f;
  Rect header{content.x, content.y, content.w, showColumnHeader ? 17.0f : 0.0f};
  if (showColumnHeader) u.draw.rect(header, t.panelAlt);
  const float columnPad = r.w < 140.0f ? 4.0f : 10.0f;
  const float columnGap = r.w < 140.0f ? 2.0f : 6.0f;
  const float innerW = std::max(0.0f, r.w - columnPad * 2.0f);
  bool drawTime = false, drawVenue = false;
  if (st.showTime && st.showVenue) {
    drawTime = innerW >= 300.0f;
    drawVenue = innerW >= 190.0f;
  } else if (st.showTime) {
    drawTime = innerW >= 190.0f;
  } else if (st.showVenue) {
    drawVenue = innerW >= 150.0f;
  }
  const float timeW = drawTime ? 74.0f : 0.0f;
  const float venueW = drawVenue ? 56.0f : 0.0f;
  float metaW = timeW + venueW;
  if (drawTime && drawVenue) metaW += columnGap;
  if (metaW > 0.0f) metaW += columnGap;
  const float coreW = std::max(0.0f, innerW - metaW);
  const float priceW = coreW * (r.w < 180.0f ? 0.52f : 0.46f);
  float columnX = header.x + columnPad;
  Rect timeColumn{columnX, header.y, timeW, header.h};
  if (drawTime) columnX += timeW + columnGap;
  Rect venueColumn{columnX, header.y, venueW, header.h};
  if (drawVenue) columnX += venueW + columnGap;
  Rect priceColumn{columnX, header.y, priceW, header.h};
  Rect amountColumn{columnX + priceW + columnGap, header.y,
                    std::max(0.0f, coreW - priceW - columnGap), header.h};
  if (showColumnHeader) {
    if (drawTime) u.draw.textFit(timeColumn, "TIME", t.textDim, DrawList::Left);
    if (drawVenue) u.draw.textFit(venueColumn, "VENUE", t.textDim, DrawList::Left);
    u.draw.textFit(priceColumn, "PRICE", t.textDim, DrawList::Left);
  }
  char amountHeader[32];
  snprintf(amountHeader, sizeof(amountHeader), "AMOUNT (%s)",
           st.showUsd ? "USD" : symbols::kNames[std::clamp(feeds.symbol, 0, 2)]);
  if (showColumnHeader) {
    u.draw.textFit(amountColumn, r.w < 180.0f ? "AMT" : amountHeader, t.textDim,
                   DrawList::Right);
    u.draw.rect({header.x, header.y + header.h, header.w, 1}, t.border);
  }

  Rect area{content.x, content.y + headerBlockH, content.w,
            content.h - headerBlockH};
  if (tape.count == 0) {
    u.draw.textAligned(area, "waiting for trades…", t.textDim, DrawList::Center);
    return;
  }
  if (st.filtered.empty()) {
    u.draw.textAligned(area, "no trades match this filter", t.textDim,
                       DrawList::Center);
    return;
  }

  // Newest first. Keep ordinary prints quiet, then ramp decisively through
  // institutional-sized tiers: $1k / $10k / $100k. A separate whale boost
  // prevents a busy stream of small fills from visually competing with size.
  float rowH = kPanelRowHeights[std::clamp(st.density, 0, 2)];
  float visual = kPanelIntensity[std::clamp(st.intensity, 0, 2)];
  listView(u, area, (int)st.filtered.size(), rowH, st.list,
           [&](DrawList& d, Rect row, int i) {
             const TapeEntry* e = tape.latest(st.filtered[(size_t)i]);
             if (!e) return;

             double usd = std::fabs(e->price * e->qty);
             float tier = std::clamp(
                 (float)((std::log10(std::max(usd, 100.0)) - 3.0) / 2.0),
                 0.0f, 1.0f);
             float shaped = tier * tier * (3.0f - 2.0f * tier);
             float whale = std::clamp(
                 (float)((std::log10(std::max(usd, 10000.0)) - 4.0) / 2.0),
                 0.0f, 1.0f);
             Color side = e->side == 0 ? t.green : t.red;
             Color valueColor = mixColor(t.text, side, 0.14f + 0.86f * shaped);

             // A restrained side wash makes large executions immediately
             // scannable while preserving the terminal's compact table rhythm.
             if (st.showWash) {
               if (st.showGradient)
                 d.rectGradientHDithered(
                      row, withAlpha(side, 0.002f * visual),
                      withAlpha(side, (0.018f + 0.31f * shaped + 0.08f * whale) * visual));
                else
                  d.rect(row, withAlpha(side,
                                        (0.012f + 0.23f * shaped + 0.07f * whale) * visual));
              }
             if (st.showMarker) {
               float markerW = shaped > 0.72f ? 3.0f : 2.0f;
               d.rect({row.x, row.y + 2.0f, markerW, row.h - 4.0f},
                      withAlpha(side, (0.12f + 0.78f * shaped) * visual));
             }

             const char* timeBuf = st.timeLabel((int64_t)(e->ts / 1000.0));
             char price[32], amount[32];
             snprintf(price, sizeof(price), "%.*f", st.priceDecimals, e->price);
             if (st.showUsd)
               formatUsd(usd, amount, sizeof(amount));
             else
               formatCoin(e->qty, st.amountPrecision, amount, sizeof(amount));

             const char* vtag = "?";
             if (e->venue < feeds.venues.size())
               vtag = feeds.venues[e->venue].shortLabel.c_str();

             if (drawTime)
               d.textFit({timeColumn.x, row.y, timeColumn.w, row.h}, timeBuf,
                         t.textDim, DrawList::Left);
             if (drawVenue)
               d.textFit({venueColumn.x, row.y, venueColumn.w, row.h}, vtag,
                         mixColor(t.textDim, side, shaped * 0.32f),
                         DrawList::Left);
             d.textFit({priceColumn.x, row.y, priceColumn.w, row.h}, price, side,
                       DrawList::Left);
             d.textFit({amountColumn.x, row.y, amountColumn.w, row.h}, amount,
                       valueColor, DrawList::Right);
           }, false);
}
