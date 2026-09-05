#pragma once

#include "../../data/candles.h"
#include "../../data/feeds.h"
#include "../../data/heatmap.h"
#include "../../data/liq_map.h"
#include "../../ui/widgets.h"
#include "chart_panes.h"
#include "period_levels.h"
#include "drawings.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

struct Feeds;
struct Ui;

struct IndicatorInstance;

// Behavior half of the per-indicator registry (kRegistry in chart_panel.cpp):
// compute, live last-bar update, and registry name. Presentation hooks —
// pane format, y-range, body renderer — live in the file-local kPresent
// table so the two halves stay side-by-side with their implementations.
enum : uint8_t {
  CapOnce = 1 << 0, // singleton, no series polyline / last-value
  CapFlow = 1 << 1, // extra order-flow window (volume profile)
  CapMkt = 1 << 2,  // mix MarketSeries::version
  CapLiq = 1 << 3,  // mix MarketSeries::liqVersion
};

struct Indicator {
  const char* name;
  bool overlay;
  uint8_t caps;
  void (*compute)(const CandleSeries&, IndicatorInstance&, const OrderFlowSeries*,
                  const MarketSeries*);
  bool (*updateLast)(const CandleSeries&, IndicatorInstance&, const OrderFlowSeries*,
                     const MarketSeries*);
};

struct IndicatorInstance {
  int id = 0;            // stable UI identity
  int reg = 0;           // index into the chart indicator registry
  bool labelVisible = true;
  bool seriesVisible = true; // overlay legend click hides the plot, not the name
  float height = 104.0f; // pane plot height; unused for overlays
  uint8_t width = 1;     // 1 / 1.5 / 2 / 2.5px
  // Palette slot index (0-7), or 0x80000000|rgb for a custom picker color.
  uint32_t colorA = 0;   // primary series / up / %K / ADX
  uint32_t colorB = 3;   // secondary / down / %D / +DI / signal
  uint32_t colorC = 4;   // tertiary / −DI / hist
  uint32_t colorD = 3;   // vwap ±2σ band
  uint32_t colorE = 3;   // vwap ±3σ band
  int p0 = 0, p1 = 0, p2 = 0; // type-specific periods / volume mode
  int opt = 0;           // deviation / multiplier / volume intensity
  bool flag = true;      // guides / histogram / bands / DI / zero line
  PeriodLevels levels; // calendar opens, independent of oscillator series
  std::vector<float> series;
  std::vector<float> aux;
  std::vector<float> aux2;
  std::vector<int8_t> dir;
  double live0 = 0, live1 = 0, live2 = 0, live3 = 0, live4 = 0;
  double live5 = 0, live6 = 0; // cipher B: MFI RMA up/down seeds
  int liveI = 0;
  int64_t liveDay = 0;
  int64_t liveShift = 0; // OrderFlowSeries::indexShift as of the last fold
};


struct ChartPanel {
  bool panning() const { return m_panning || m_timeScaling; } // cursor hint
  bool paneResizing() const { return m_resizePane >= 0 || m_resizeHotPane >= 0; }
  bool drawing() const { return m_drawings.armed(); }
  const char* drawingCursor() const { return m_drawings.cursor(); }

  bool scaleAuto = true;   // auto-fit price range to the visible window
  bool scaleLog = false;   // logarithmic price axis

  void draw(Ui& u, Rect r, Feeds& feeds);
  void setStorageKey(const std::string& key) {
    m_settingsKey = key;
    m_drawings.setStorageKey(key);
  }

  // popovers — drawn by the host during the overlay pass
  void drawIndicatorPicker(Ui& u);
  void drawIndicatorSettings(Ui& u);
  void drawTfPicker(Ui& u, Feeds& feeds);
  void drawFlowPicker(Ui& u, Feeds& feeds);
  void drawToolPicker(Ui& u);
private:
  float scroll = 0; // bars scrolled back from the latest (fractional: drag pans sub-bar)
  std::vector<IndicatorInstance> m_panes;    // stacked below price, top → bottom
  std::vector<IndicatorInstance> m_overlays; // drawn on the price pane
  int m_nextInstId = 1;
  float m_dragX = 0;                   // last drag-pan cursor x
  float m_dragY = 0;                   // last drag-pan cursor y
  float m_panTotalX = 0;               // drag displacement used for intent lock
  float m_panTotalY = 0;
  float m_barWidth = 7.0f;             // horizontal zoom, logical px per bar
  int m_appliedChartType = -1;          // one-time spacing defaults on mode entry
  float m_tsX = 0;                     // last time-scale drag cursor x
  float m_psY = 0;                     // last price-scale drag cursor y
  bool m_panning = false;              // drag-pan active this frame
  bool m_timeScaling = false;           // time-axis scale drag active
  bool m_panPrice = false;              // drag began in the main price pane
  bool m_panMoved = false;              // distinguishes a click from a view drag
  int m_resizePane = -1;                 // separator above this pane is active
  int m_resizeHotPane = -1;              // separator hover for cursor/highlight
  float m_resizeStartY = 0.0f;
  float m_resizeUpper = 0.0f;
  float m_resizeLower = 0.0f;
  std::vector<bool> m_enabled;         // derived per-registry "any instance"
  std::vector<uint8_t> m_indicatorWidths;  // persist defaults for new instances
  uint64_t m_computedSig = ~0ull;          // full candle signature as of last compute
  uint64_t m_shapeSig = 0;                 // size/interval/symbol/last-bucket: a
                                           // change means history append/replace
  unsigned m_calcGen = 0;                  // bumped when toggles change → recompute
  unsigned m_calcToggles = 0;              // m_calcGen as of the last compute
  // Streamed-fill reconciliation: prepends fold into the live tail cheaply,
  // but their older prints belong to historical bars. Once the flow window
  // stops moving for a beat, one full recompute reconciles the history.
  uint64_t m_flowShadowVer = 0;
  uint64_t m_flowSeenPrepends = 0;
  unsigned m_flowQuiet = 0;
  bool m_flowHistoryStale = false;

  // Per-instance live-tick state lives on IndicatorInstance. This flag says
  // the last full compute filled those tails so updateLastBar may run.
  bool m_liveState = false;

  bool m_settingsOpen = false;
  bool m_settingsLoaded = false;
  std::string m_settingsKey = "cerium.chart.settings.v1";
  bool m_showGrid = true;
  bool m_showWicks = true;
  bool m_showLastPrice = true;
  bool m_showLastPriceLine = true;
  bool m_showLastPriceLabel = true;
  bool m_showBarCountdown = true;
  int m_lastPriceStyle = 1;     // solid / dashed / dotted
  int m_lastPriceWidth = 0;     // 1 / 1.5 / 2px
  int m_lastPriceColor = 0;     // accent / candle side / neutral
  int m_candleBody = 2;         // solid / hollow / ghost 
  bool m_showCrosshair = true;
  bool m_showTimeLabels = true;
  bool m_showIndicatorLabels = true;
  bool m_showCvdZero = true;
  bool m_showRsiGuides = true;
  bool m_showMacdHistogram = true;
  bool m_showVwapBands = true;
  bool m_showStochGuides = true;
  bool m_showAdxDi = true;
  int m_gutterWidth = 1;       // 56 / 72 / 88px
  int m_lineWidth = 1;         // 1 / 1.5 / 2 / 2.5px
  int m_volumeIntensity = 1;   // quiet / normal / strong
  int m_chartType = 0;         // candles / footprint cluster / profile / TPO
  uint32_t m_flowMask = kAllVenuesMask; // venues feeding CVD + footprint
  uint64_t m_flowPickerId = 0;  // per-venue flow-source popover
  Rect m_flowPickerRect{};
  ListState m_flowPickerList;
  int m_footprintGrouping = 0; // auto / fine / medium / coarse
  int m_footprintImbalance = 1; // 3x / 4x / 5x
  bool m_showFootprintText = true;
  bool m_showFootprintPoc = true;
  bool m_showFootprintImbalances = true;
  bool m_showFootprintStacked = true;
  int m_footprintHeatmap = 1; // quiet / normal / strong
  int m_footprintMinCell = 0;  // hide cells under 0/1/2/5/10% of bar POC volume
  int m_tpoBracket = 0;        // 30 / 60 minutes
  bool m_heatOn = true;        // BOOK HEAT overlay; persisted separately
  bool m_hlLiqOn = false;      // HL LIQ overlay
  bool m_hlSlOn = false;       // HL SL overlay
  bool m_liqMapOn = false;     // predicted LIQ MAP overlay
  int m_liqBinSel = 0;         // 0 = AUTO (~1000 ticks), else log dollar bin
  float m_liqGamma = 4.32f;    // crush faint bins; MMT default
  float m_liqOpacity = 1.0f;
  int m_liqBands = 15;         // bitmask 10/25/50/100x
  bool m_liqProfile = true;
  int m_heatResSel = 100;      // capture resolution: slider % → 8/4/2/1 px per row
  float m_heatIntensity = 0.85f;
  float m_heatOpacity = 1.0f;  // overlay alpha multiplier, independent of intensity
  int m_heatMinSel = 0;        // min USD clamp, slider % (log $100..$10M, 0 = off)
  int m_heatMaxSel = 100;      // max USD clamp, slider % (log $10K..$1B, 100 = auto)
  int m_heatBinSel = 0;        // price bin, slider % (0 = AUTO from resolution)
  HeatmapSeries m_bookHeat;
  LiqMapSeries m_liqMap;
  int m_emaPeriod = 21;
  int m_ema2Period = 200;
  int m_smaPeriod = 50;
  int m_rsiPeriod = 14;
  int m_macdFast = 12;
  int m_macdSlow = 26;
  int m_macdSignalPeriod = 9;
  int m_bollPeriod = 20;
  int m_bollDeviation = 1;     // option index: 1.5 / 2 / 2.5 / 3
  int m_stPeriod = 10;
  int m_stMultSel = 2;         // option index: 1.5 / 2 / 3 / 4
  int m_stochPeriod = 14;
  int m_stochSmooth = 3;
  int m_stochDPeriod = 3;
  int m_atrPeriod = 14;
  int m_adxPeriod = 14;
  ListState m_settingsList;

  struct FootprintCell {
    int bar = 0;
    int64_t tick = 0;
    double buy = 0;
    double sell = 0;
  };
  std::vector<FootprintCell> m_footprint;
  std::vector<int> m_footprintOffsets;
  uint64_t m_footprintVersion = ~0ull;
  uint64_t m_footprintGeneration = ~0ull;
  uint64_t m_footprintPrepends = 0;
  size_t m_footprintTradeCount = 0;
  uint64_t m_footprintShape = 0;
  // Axis identity of the last build: symbol/TF/front-bar ts. Bar-open growth
  // keeps this hash (only back.ts and size change), so the append path can
  // extend offsets instead of rehashing the whole window.
  uint64_t m_footprintAxis = 0;
  size_t m_footprintAxisBars = 0;
  double m_footprintBackTs = 0;
  double m_footprintStep = 0;
  double m_footprintFrontTs = 0;
  std::vector<int64_t> m_footprintPocTicks;
  std::vector<int> m_footprintPocHits;
  // POC ray lifecycle: closed bars are finalized once (watermark) and their
  // rays join the active set; the last two bars stay provisional — their
  // wicks only extend, so provisional hits are final, and late-arriving
  // prints for the newest closed bar are still picked up.
  std::unordered_map<int64_t, std::vector<int>> m_footprintPocActive;
  int m_footprintPocBuilt = 0;
  uint64_t m_footprintPocVersion = ~0ull;
  uint64_t m_footprintPocShape = 0;
  double m_footprintPocStep = 0;


  struct PaneRange { bool ok; double lo, hi; };
  std::vector<PaneRange> m_rngPanes; // final pane ranges, top → bottom
  double m_rngPriceLo = 0, m_rngPriceHi = 1; // raw visible candle lo/hi
  double m_rngFitLo = 0, m_rngFitHi = 1;     // auto-fit of visible candles (even when scale is manual)
  uint64_t m_rngSig = 0;
  int m_rngV0 = -1, m_rngV1 = -1;
  unsigned m_rngGen = 0;     // bumped when the pane set changes the ranges
  unsigned m_rngToggles = 0; // m_rngGen as of the last range computation

  uint64_t m_pickerId = 0;
  Rect m_pickerRect{};
  ListState m_pickerList;

  uint64_t m_indicatorSettingsId = 0;
  Rect m_indicatorSettingsRect{};
  int m_indicatorSettingsInst = 0;
  void openIndicatorSettings(Ui& u, int instId, Rect anchor);

  // Custom color picker popover (opened from a palette row's custom swatch).
  uint64_t m_colorPickerId = 0;
  Rect m_colorPickerRect{};
  int m_colorPickerInst = 0;
  uint32_t IndicatorInstance::* m_colorPickerSlot = nullptr;
  uint32_t m_lastCustom = 0x80000000u | 0x9c8fe8u; // last picked custom color
  float m_cpHue = 0, m_cpSat = 0, m_cpVal = 0;
  TextFieldState m_cpHex;
  void openColorPicker(Ui& u, int instId, uint32_t IndicatorInstance::*slot,
                       Rect anchor);
  void drawColorPicker(Ui& u);

  TextFieldState m_tfInput;
  uint64_t m_tfPickerId = 0;
  Rect m_tfPickerRect{};
  bool m_autoFocusTf = false;
  bool m_tfError = false;

  TextFieldState m_htToken;
  bool m_htTokenLoaded = false;
  int m_htUsed = 0;
  int m_htQuota = 100;
  int m_htStatus = 1;
  double m_htLiqAt = 0;
  double m_htSlAt = 0;
  int m_htLiqN = 0;
  int m_htSlN = 0;
  void ensureHtToken();

  DrawingSet m_drawings;

  bool indicatorOn(int ri) const;
  int instanceCount(int ri) const;
  IndicatorInstance* findInstance(int id);
  IndicatorInstance makeInstance(int ri);
  void addIndicator(int ri);
  void removeIndicator(Ui& u, int instId);
  void setMapFlag(int ri, bool on);
  void setInstanceOnChart(int instId, bool onChart);
  void refreshEnabled();
  void noteSetChanged() { ++m_rngGen; ++m_calcGen; m_liveState = false; }
  void computeInstance(const CandleSeries& cs, IndicatorInstance& inst,
                       const OrderFlowSeries* of, const MarketSeries* mkt);
  bool updateInstanceLast(const CandleSeries& cs, IndicatorInstance& inst,
                          const OrderFlowSeries* of, const MarketSeries* mkt);
  void formatInstanceName(const IndicatorInstance& inst, char* out, size_t n) const;
  bool drawPaneOverlay(Ui& u, int p, int nPanes, Rect pane, const char* name,
                       float value, ChartFmt fmt);
  bool drawLegendActions(Ui& u, int instId, Rect settings, Rect remove);

  bool cvdWantsFlow() const;
  bool oiWantsMarket() const;
  void ensureComputed(const Feeds& feeds);
  void computeAll(const Feeds& feeds); // full-series recompute (rare)
  bool updateLastBar(const Feeds& feeds); // O(1)/O(period) live tick; false → full
  void ensureFootprint(const Feeds& feeds, double step);
  void loadSettings();
  void saveSettings();
  void resetSettings();
  void drawSettings(Ui& u, Rect area);

  // --- persisted settings schema -------------------------------------------
  // One entry per CSV column in the current (v21) storage order. Bools store
  // 1/0, Pct100 fields store round(value * 100), Mask stores the venue bitmask.
  enum class SettType : uint8_t { Bool, Int, Pct100, Mask, Widths };
  struct Setting {
    SettType type;
    int lo = 0, hi = 0; // clamp bounds (Widths/Bool ignore them)
    bool ChartPanel::*b = nullptr;
    int ChartPanel::*i = nullptr;
    uint32_t ChartPanel::*u = nullptr;
    float ChartPanel::*f = nullptr;
  };
  static const Setting kSettings[];

  // --- global settings page -------------------------------------------------
  enum class RowKind : uint8_t {
    Bool,     // toggle a bool member
    Int,      // select: member == v0
    ChartType,// select + bar-width default reset
    MaskAll,  // flow mask = all venues
    MaskClass,// flow mask ^= class mask in v0
    Venues,   // opens the per-venue picker popover
    Restore,  // reset-to-defaults button
  };
  struct RowOpt {
    const char* label;
    float width;
    RowKind kind;
    int v0 = 0, v1 = 0, v2 = 0;
    bool ChartPanel::*b = nullptr;
    int ChartPanel::*i = nullptr;
    int ChartPanel::*i2 = nullptr;
    int ChartPanel::*i3 = nullptr;
  };
  struct SettingRow {
    const char* title;
    RowOpt opts[5];
    int optCount = 0;
  };
  static const SettingRow kRows[];

  // --- draw() internals -----------------------------------------------------
  // State shared by the draw helpers for one frame. `scroll` is a working
  // copy of ChartPanel::scroll, written back after input handling.
  struct PlotCtx {
    Rect chart{}, gutter{}, timeAxis{};
    ChartPane price;
    ChartPane ind[8];
    Rect indBg[8]{};
    float paneHeights[8] = {};
    float stackH = 0, gutterW = 0, priceH = 0;
    int nPanes = 0;
    int size = 0;
    int slots = 10;
    float bw = 7.0f, freeMax = 0.0f, endSlot = 0.0f, startF = 0.0f;
    float scroll = 0;
    int vis0 = 0, vis1 = 0;
    bool rangeOk[8] = {};
    double lastC = 0;
    float ly = 0;
    bool showTag = false;
    bool cross = false;
    float crossTagY = -1e9f;

    float xOf(int i) const { return price.area.x + (i - startF) * bw; }
    void updateView(float barWidth);
    void clampScroll();
    float clampBarWidth(int chartType, float value) const;
  };

  float drawHeader(Ui& u, Rect r, Feeds& feeds); // returns chartTop
  void layoutPlot(Ui& u, Feeds& feeds, Rect r, float chartTop, PlotCtx& ctx);
  void navAndRanges(Ui& u, Feeds& feeds, PlotCtx& ctx);
  void drawPlotChrome(Ui& u, Feeds& feeds, PlotCtx& ctx);
  void drawTimeAxisRow(Ui& u, Feeds& feeds, PlotCtx& ctx);
  bool drawPricePane(Ui& u, Feeds& feeds, PlotCtx& ctx); // true → frame done
  bool drawIndicatorPanes(Ui& u, Feeds& feeds, PlotCtx& ctx);
  void drawLastPriceRow(Ui& u, Feeds& feeds, PlotCtx& ctx);
  void drawCrosshairRow(Ui& u, Feeds& feeds, PlotCtx& ctx);
  void drawHeatHover(Ui& u, Feeds& feeds, PlotCtx& ctx);
};
