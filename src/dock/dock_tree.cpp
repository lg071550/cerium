#include "dock_tree.h"

#include <algorithm>

DockNode* DockTree::makeLeaf(std::vector<int> tabs) {
  auto n = std::make_unique<DockNode>();
  n->kind = DockNode::Leaf;
  n->tabs = std::move(tabs);
  DockNode* ptr = n.get();
  pool.push_back(std::move(n));
  return ptr;
}

DockNode* DockTree::makeSplit(DockDir dir, DockNode* first, DockNode* second,
                              float ratio) {
  auto n = std::make_unique<DockNode>();
  n->kind = DockNode::Split;
  n->dir = dir;
  n->a = first;
  n->b = second;
  n->ratio = ratio;
  first->parent = n.get();
  second->parent = n.get();
  DockNode* ptr = n.get();
  pool.push_back(std::move(n));
  return ptr;
}

static void computeNodeRects(DockNode* n, Rect r, float s) {
  n->rect = r;
  if (n->isLeaf()) return;

  constexpr float minChild = 80.0f;
  if (n->dir == DockDir::Horizontal) {
    float avail = r.w - s;
    float aw = avail * n->ratio;
    if (avail > 2 * minChild) aw = std::clamp(aw, minChild, avail - minChild);
    else aw = avail * 0.5f;
    aw = std::max(aw, 0.0f);
    computeNodeRects(n->a, {r.x, r.y, aw, r.h}, s);
    computeNodeRects(n->b, {r.x + aw + s, r.y, std::max(avail - aw, 0.0f), r.h}, s);
  } else {
    float avail = r.h - s;
    float ah = avail * n->ratio;
    if (avail > 2 * minChild) ah = std::clamp(ah, minChild, avail - minChild);
    else ah = avail * 0.5f;
    ah = std::max(ah, 0.0f);
    computeNodeRects(n->a, {r.x, r.y, r.w, ah}, s);
    computeNodeRects(n->b, {r.x, r.y + ah + s, r.w, std::max(avail - ah, 0.0f)}, s);
  }
}

void DockTree::computeRects(Rect area, float splitter) {
  if (root) computeNodeRects(root, area, splitter);
}

DockNode* DockTree::leafAt(float x, float y) {
  DockNode* n = root;
  while (n && !n->isLeaf()) n = n->a->rect.contains(x, y) ? n->a : n->b;
  return (n && n->rect.contains(x, y)) ? n : nullptr;
}

static void collectSplittersRec(DockNode* n, float s,
                                std::vector<std::pair<Rect, DockNode*>>& out) {
  if (!n || n->isLeaf()) return;
  Rect sr = n->dir == DockDir::Horizontal
                ? Rect{n->a->rect.x + n->a->rect.w, n->rect.y, s, n->rect.h}
                : Rect{n->rect.x, n->a->rect.y + n->a->rect.h, n->rect.w, s};
  out.emplace_back(sr, n);
  collectSplittersRec(n->a, s, out);
  collectSplittersRec(n->b, s, out);
}

void DockTree::collectSplitters(float splitter,
                                std::vector<std::pair<Rect, DockNode*>>& out) {
  collectSplittersRec(root, splitter, out);
}

static void collectTabsRec(const DockNode* n, std::vector<int>& out) {
  if (!n) return;
  if (n->isLeaf()) {
    for (int t : n->tabs) out.push_back(t);
    return;
  }
  collectTabsRec(n->a, out);
  collectTabsRec(n->b, out);
}

void DockTree::collectTabs(std::vector<int>& out) const { collectTabsRec(root, out); }

DockNode* DockTree::firstLeaf() {
  DockNode* n = root;
  while (n && !n->isLeaf()) n = n->a;
  return n;
}

void DockTree::insertTab(DockNode* leaf, int tabId, int index) {
  if (!leaf || !leaf->isLeaf()) return;
  if (index < 0 || index > (int)leaf->tabs.size()) {
    leaf->tabs.push_back(tabId);
    leaf->active = (int)leaf->tabs.size() - 1;
  } else {
    leaf->tabs.insert(leaf->tabs.begin() + index, tabId);
    leaf->active = index;
  }
  changed = true;
}

void DockTree::removeTab(DockNode* leaf, int tabId) {
  if (!leaf || !leaf->isLeaf()) return;
  auto it = std::find(leaf->tabs.begin(), leaf->tabs.end(), tabId);
  if (it == leaf->tabs.end()) return;
  int erased = (int)(it - leaf->tabs.begin());
  leaf->tabs.erase(it);
  // Tabs left of the selection slide it left with them.
  if (erased < leaf->active) --leaf->active;
  if (leaf->active >= (int)leaf->tabs.size())
    leaf->active = std::max(0, (int)leaf->tabs.size() - 1);
  changed = true;
  collapseIfEmpty(leaf);
}

void DockTree::collapseIfEmpty(DockNode* leaf) {
  if (!leaf || !leaf->isLeaf() || !leaf->tabs.empty()) return;
  DockNode* p = leaf->parent;
  if (!p) return; // empty root leaf = empty workspace placeholder

  DockNode* sib = (p->a == leaf) ? p->b : p->a;
  DockNode* gp = p->parent;
  sib->parent = gp;
  if (gp) {
    if (gp->a == p) gp->a = sib;
    else gp->b = sib;
  } else {
    root = sib;
  }
  // leaf and p stay in the pool (freed with the tree); they're just unlinked.
}

void DockTree::splitLeaf(DockNode* leaf, DockNode* newLeaf, DropZone edge,
                         float ratio) {
  DockDir dir = (edge == DropZone::Left || edge == DropZone::Right)
                    ? DockDir::Horizontal
                    : DockDir::Vertical;
  bool newFirst = (edge == DropZone::Left || edge == DropZone::Top);
  float r = newFirst ? ratio : 1.0f - ratio;

  DockNode* p = leaf->parent;
  DockNode* sp = makeSplit(dir, newFirst ? newLeaf : leaf, newFirst ? leaf : newLeaf, r);
  sp->parent = p;
  if (p) {
    if (p->a == leaf) p->a = sp;
    else p->b = sp;
  } else {
    root = sp;
  }
  changed = true;
}

void DockTree::dockTab(int tabId, DockNode* src, DockNode* dst, DropZone zone) {
  if (!src || !dst || zone == DropZone::None) return;
  if (src == dst) {
    if (zone == DropZone::Center || src->tabs.size() < 2) return;
    removeTab(src, tabId); // src stays valid: still has tabs
    DockNode* nl = makeLeaf({tabId});
    splitLeaf(src, nl, zone);
    return;
  }

  removeTab(src, tabId); // may collapse src; dst (a different leaf) stays valid
  if (zone == DropZone::Center) {
    insertTab(dst, tabId);
  } else {
    DockNode* nl = makeLeaf({tabId});
    splitLeaf(dst, nl, zone);
  }
}

void DockTree::dockTabRootEdge(int tabId, DockNode* src, DropZone edge) {
  if (!src || !root) return;
  if (root->isLeaf()) { // uniform path: split the only leaf
    dockTab(tabId, src, root, edge);
    return;
  }
  removeTab(src, tabId);
  DockNode* nl = makeLeaf({tabId});
  DockDir dir = (edge == DropZone::Left || edge == DropZone::Right)
                    ? DockDir::Horizontal
                    : DockDir::Vertical;
  bool newFirst = (edge == DropZone::Left || edge == DropZone::Top);
  DockNode* old = root;
  DockNode* sp = makeSplit(dir, newFirst ? nl : old, newFirst ? old : nl,
                           newFirst ? 0.3f : 0.7f);
  sp->parent = nullptr;
  root = sp;
  changed = true;
}
