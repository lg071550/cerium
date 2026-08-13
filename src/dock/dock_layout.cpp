#include "dock_layout.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// writer
// ---------------------------------------------------------------------------

static void writeNode(std::string& out, const DockNode* n,
                      const std::vector<std::string>& titles) {
  if (n->isLeaf()) {
    out += "{\"t\":\"l\",\"tabs\":[";
    for (size_t i = 0; i < n->tabs.size(); ++i) {
      int tab = n->tabs[i];
      const std::string& title = titles[(size_t)tab];
      out += '"';
      for (char ch : title) {
        if (ch == '"' || ch == '\\') out += '\\';
        out += ch;
      }
      out += '"';
      if (i + 1 < n->tabs.size()) out += ',';
    }
    out += "],\"a\":";
    out += std::to_string(n->active);
    out += '}';
    return;
  }
  out += "{\"t\":\"s\",\"d\":";
  out += n->dir == DockDir::Horizontal ? '0' : '1';
  out += ",\"r\":";
  char buf[24];
  snprintf(buf, sizeof(buf), "%.4f", (double)n->ratio);
  out += buf;
  out += ",\"a\":";
  writeNode(out, n->a, titles);
  out += ",\"b\":";
  writeNode(out, n->b, titles);
  out += '}';
}

std::string dockSerialize(const DockTree& tree, const std::vector<std::string>& titles) {
  if (!tree.root) return {};
  std::string out;
  out.reserve(256);
  writeNode(out, tree.root, titles);
  return out;
}

// ---------------------------------------------------------------------------
// parser (minimal, strict to our own format; no exceptions)
// ---------------------------------------------------------------------------

struct Cur {
  const char* p;
};

static void ws(Cur& c) {
  while (*c.p == ' ' || *c.p == '\t' || *c.p == '\n' || *c.p == '\r') c.p++;
}

static bool eat(Cur& c, char ch) {
  ws(c);
  if (*c.p == ch) {
    c.p++;
    return true;
  }
  return false;
}

static bool parseString(Cur& c, std::string& out) {
  ws(c);
  if (*c.p != '"') return false;
  c.p++;
  out.clear();
  while (*c.p && *c.p != '"') {
    if (*c.p == '\\' && c.p[1]) {
      c.p++;
      out += *c.p++;
    } else {
      out += *c.p++;
    }
  }
  if (*c.p != '"') return false;
  c.p++;
  return true;
}

static bool parseKey(Cur& c, const char* key) {
  std::string k;
  if (!parseString(c, k) || k != key) return false;
  return eat(c, ':');
}

static bool parseNum(Cur& c, double& v) {
  ws(c);
  char* end = nullptr;
  v = strtod(c.p, &end);
  if (end == c.p) return false;
  c.p = end;
  return true;
}

static int findTitle(const std::vector<std::string>& titles, const std::string& name) {
  for (size_t i = 0; i < titles.size(); ++i)
    if (titles[i] == name) return (int)i;
  return -1;
}

static DockNode* parseNode(DockTree& tree, Cur& c,
                           const std::vector<std::string>& titles) {
  if (!eat(c, '{')) return nullptr;

  std::string t;
  if (!parseKey(c, "t") || !parseString(c, t)) return nullptr;

  if (t == "l") {
    std::vector<int> tabs;
    if (!eat(c, ',') || !parseKey(c, "tabs") || !eat(c, '[')) return nullptr;
    if (!eat(c, ']')) {
      for (;;) {
        std::string name;
        if (!parseString(c, name)) return nullptr;
        int id = findTitle(titles, name);
        if (id >= 0) tabs.push_back(id);
        if (eat(c, ',')) continue;
        if (!eat(c, ']')) return nullptr;
        break;
      }
    }
    double a = 0;
    if (!eat(c, ',') || !parseKey(c, "a") || !parseNum(c, a) || !eat(c, '}'))
      return nullptr;
    if (tabs.empty()) return nullptr; // all panels unknown → drop the leaf
    DockNode* n = tree.makeLeaf(std::move(tabs));
    n->active = std::clamp((int)a, 0, (int)n->tabs.size() - 1);
    return n;
  }

  if (t == "s") {
    double d = 0, r = 0.5;
    if (!eat(c, ',') || !parseKey(c, "d") || !parseNum(c, d)) return nullptr;
    if (!eat(c, ',') || !parseKey(c, "r") || !parseNum(c, r)) return nullptr;
    if (!eat(c, ',') || !parseKey(c, "a")) return nullptr;
    DockNode* a = parseNode(tree, c, titles);
    if (!eat(c, ',') || !parseKey(c, "b")) return nullptr;
    DockNode* b = parseNode(tree, c, titles);
    if (!eat(c, '}')) return nullptr;
    if (!a) return b; // collapse half-valid splits
    if (!b) return a;
    float ratio = (float)std::clamp(r, 0.05, 0.95);
    return tree.makeSplit(d < 0.5 ? DockDir::Horizontal : DockDir::Vertical, a, b, ratio);
  }

  return nullptr;
}

bool dockDeserialize(DockTree& tree, const char* json,
                     const std::vector<std::string>& titles) {
  if (!json || !*json) return false;
  Cur c{json};
  DockNode* root = parseNode(tree, c, titles);
  ws(c);
  if (!root || *c.p != '\0') return false;
  root->parent = nullptr;
  tree.root = root;
  return true;
}
