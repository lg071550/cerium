#pragma once

#include "../../data/candles.h"
#include "../../ui/widgets.h"
#include "chart_panes.h"

#include <cstdint>
#include <string>
#include <vector>

enum class DrawTool : uint8_t {
  Pointer = 0,
  HLine,
  Trend,
  Ray,
  Rect,
  Fib,
  Avwap
};

inline constexpr int kDrawToolN = (int)DrawTool::Avwap + 1;
inline constexpr const char* kDrawToolNames[] = {
    "POINTER", "H-LINE", "TREND", "RAY", "RECT", "FIB", "AVWAP"};
static_assert(sizeof(kDrawToolNames) / sizeof(kDrawToolNames[0]) == kDrawToolN);

inline bool drawClickCommit(DrawTool t) {
  return t == DrawTool::HLine || t == DrawTool::Avwap;
}

struct ChartDrawing {
  int id = 0;
  DrawTool kind = DrawTool::HLine;
  int symbol = 0;
  double t0 = 0, p0 = 0, t1 = 0, p1 = 0;
};

struct DrawingSet {
  DrawTool tool = DrawTool::Pointer;
  int selectedId = 0;
  bool placing = false;
  bool hovering = false;
  bool dragging = false;

  void setStorageKey(const std::string& chartSettingsKey);
  void load();
  void save() const;

  bool handle(Ui& u, const ChartPane& pane, const CandleSeries& cs, int symbol,
              float startF, float bw, int size);
  void draw(DrawList& d, const ChartPane& pane, const CandleSeries& cs,
            int symbol, float startF, float bw) const;

  bool armed() const {
    return tool != DrawTool::Pointer || placing || dragging;
  }
  const char* cursor() const;
  const char* toolLabel() const;
  void drawPicker(Ui& u);

  uint64_t pickerId = 0;
  Rect pickerRect{};

private:
  ChartDrawing* find(int id);
  void commitDraft();

  std::vector<ChartDrawing> items;
  std::string key;
  bool loaded = false;
  int nextId = 1;
  ChartDrawing draft{};
  int dragHandle = 0;
  float grabX = 0, grabY = 0;
  double grabP0 = 0, grabP1 = 0, grabT0 = 0, grabT1 = 0;
};
