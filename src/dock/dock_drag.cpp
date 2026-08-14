#include "dock_drag.h"
#include "../ui/theme.h"

#include <cmath>

static Rect previewRect(Rect target, DropZone zone, bool onRoot) {
  if (onRoot) {
    switch (zone) {
      case DropZone::Left: return {target.x, target.y, target.w * 0.3f, target.h};
      case DropZone::Right:
        return {target.x + target.w * 0.7f, target.y, target.w * 0.3f, target.h};
      case DropZone::Top: return {target.x, target.y, target.w, target.h * 0.3f};
      case DropZone::Bottom:
        return {target.x, target.y + target.h * 0.7f, target.w, target.h * 0.3f};
      default: return target;
    }
  }
  switch (zone) {
    case DropZone::Left: return {target.x, target.y, target.w * 0.5f, target.h};
    case DropZone::Right:
      return {target.x + target.w * 0.5f, target.y, target.w * 0.5f, target.h};
    case DropZone::Top: return {target.x, target.y, target.w, target.h * 0.5f};
    case DropZone::Bottom:
      return {target.x, target.y + target.h * 0.5f, target.w, target.h * 0.5f};
    default: return target;
  }
}

bool DockDrag::update(Ui& ui, DockTree& tree) {
  float mx = ui.input.mouseX, my = ui.input.mouseY;

  if (armed && !active) {
    if (!ui.input.down) { // released without moving — just a click
      cancel();
      return false;
    }
    if (std::fabs(mx - startX) + std::fabs(my - startY) > 6.0f) {
      armed = false;
      active = true;
    } else {
      return true; // still within threshold, swallow the frame
    }
  }

  if (ui.input.escapePressed) {
    cancel();
    return false;
  }

  // resolve hovered leaf + zone
  hoverLeaf = tree.leafAt(mx, my);
  zone = DropZone::None;
  zoneOnRoot = false;

  Rect rr = tree.root->rect;
  const float rootEdge = 24.0f;
  if (mx < rr.x + rootEdge) { zone = DropZone::Left; zoneOnRoot = true; }
  else if (mx >= rr.x + rr.w - rootEdge) { zone = DropZone::Right; zoneOnRoot = true; }
  else if (my < rr.y + rootEdge) { zone = DropZone::Top; zoneOnRoot = true; }
  else if (my >= rr.y + rr.h - rootEdge) { zone = DropZone::Bottom; zoneOnRoot = true; }
  else if (hoverLeaf) {
    Rect r = hoverLeaf->rect;
    float cx = r.w > 0 ? (mx - r.x) / r.w : 0.5f;
    float cy = r.h > 0 ? (my - r.y) / r.h : 0.5f;
    if (cx < 0.25f) zone = DropZone::Left;
    else if (cx > 0.75f) zone = DropZone::Right;
    else if (cy < 0.25f) zone = DropZone::Top;
    else if (cy > 0.75f) zone = DropZone::Bottom;
    else zone = DropZone::Center;
  }

  // suppress would-be no-ops so neither the preview nor the drop lies
  if (zoneOnRoot && tree.root == src && src->tabs.size() < 2) zone = DropZone::None;
  if (!zoneOnRoot && hoverLeaf == src &&
      (zone == DropZone::Center || src->tabs.size() < 2))
    zone = DropZone::None;

  if (!ui.input.down) { // released → apply
    if (zone != DropZone::None && (zoneOnRoot || hoverLeaf)) {
      if (zoneOnRoot) tree.dockTabRootEdge(tab, src, zone);
      else tree.dockTab(tab, src, hoverLeaf, zone);
    }
    cancel();
    return false;
  }
  return true;
}

void DockDrag::drawOverlay(Ui& ui, const DockTree& tree, const char* tabTitle) const {
  if (!active) return;
  const Theme& t = theme();

  if (zone != DropZone::None && (zoneOnRoot || hoverLeaf)) {
    Rect target = zoneOnRoot ? tree.root->rect : hoverLeaf->rect;
    Rect p = previewRect(target, zone, zoneOnRoot);
    ui.draw.rect(p, t.accentSoft, t.radius);
  }

  // dragged tab chip following the cursor
  float w = ui.draw.measure(tabTitle) + 2 * t.pad;
  Rect chip{ui.input.mouseX + 12, ui.input.mouseY + 12, w, 24};
  ui.draw.shadow(chip, t.radius, 8.0f, 2.0f, hexColor(0x000000, 0.4f));
  ui.draw.rect(chip, t.bgRaised, t.radius);
  ui.draw.textAligned(chip, tabTitle, t.text, DrawList::Center);
}
