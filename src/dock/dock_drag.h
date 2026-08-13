#pragma once

#include "../ui/ui_context.h"
#include "dock_tree.h"

// Tab drag state machine: armed on tab press, active past a small threshold,
// resolves a drop zone each frame, applies the mutation on release.
// Zone model follows ImGui: hovered-leaf center/edge zones + root-perimeter
// zones for full-span splits.
struct DockDrag {
  bool armed = false;  // pressed on a tab, threshold not yet crossed
  bool active = false; // actually dragging
  int tab = -1;
  DockNode* src = nullptr;
  float startX = 0, startY = 0;

  DockNode* hoverLeaf = nullptr;
  DropZone zone = DropZone::None;
  bool zoneOnRoot = false;

  void arm(DockNode* leaf, int tabId, float mx, float my) {
    armed = true;
    active = false;
    tab = tabId;
    src = leaf;
    startX = mx;
    startY = my;
  }

  // Returns true while the drag owns the frame (panel content should skip
  // its own interaction). Applies the drop and clears state on release.
  bool update(Ui& ui, DockTree& tree);

  void cancel() { *this = DockDrag{}; }
  void drawOverlay(Ui& ui, const DockTree& tree, const char* tabTitle) const;
};
