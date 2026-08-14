#pragma once

#include "render/draw_list.h"

#include <memory>
#include <utility>
#include <vector>

enum class DockDir { Horizontal, Vertical }; // Horizontal = left | right
enum class DropZone { None, Center, Left, Right, Top, Bottom };

// Binary dock tree: Split nodes divide space between two children; Leaf nodes
// hold a strip of tabs (panel ids). Model follows ImGui's DockNode ideas but
// is deliberately minimal — no floating windows, no multi-viewport.
struct DockNode {
  enum Kind { Split, Leaf } kind = Leaf;
  DockNode* parent = nullptr;

  // split
  DockDir dir = DockDir::Horizontal;
  float ratio = 0.5f; // fraction of available space given to child `a`
  DockNode* a = nullptr;
  DockNode* b = nullptr;

  // leaf
  std::vector<int> tabs;
  int active = 0; // index into tabs

  Rect rect; // computed each frame by computeRects

  bool isLeaf() const { return kind == Leaf; }
};

struct DockTree {
  std::vector<std::unique_ptr<DockNode>> pool; // ownership
  DockNode* root = nullptr;
  bool changed = false; // set by mutations; app persists + clears

  DockNode* makeLeaf(std::vector<int> tabs);
  DockNode* makeSplit(DockDir dir, DockNode* first, DockNode* second, float ratio);
  void setRoot(DockNode* n) { root = n; }

  void computeRects(Rect area, float splitter);
  DockNode* leafAt(float x, float y);
  void collectSplitters(float splitter, std::vector<std::pair<Rect, DockNode*>>& out);
  void collectTabs(std::vector<int>& out) const; // all tab ids in the tree
  DockNode* firstLeaf();                         // leftmost leaf (panels land here)

  void insertTab(DockNode* leaf, int tabId, int index = -1);
  void removeTab(DockNode* leaf, int tabId);
  void collapseIfEmpty(DockNode* leaf);
  void splitLeaf(DockNode* leaf, DockNode* newLeaf, DropZone edge, float ratio = 0.5f);
  void dockTab(int tabId, DockNode* src, DockNode* dst, DropZone zone);
  void dockTabRootEdge(int tabId, DockNode* src, DropZone edge);
};
