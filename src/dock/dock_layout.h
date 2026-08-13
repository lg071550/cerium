#pragma once

#include "dock_tree.h"

#include <string>
#include <vector>

// Layout persistence. JSON shape (we own the format):
//   leaf:  {"t":"l","tabs":["Chart","Tape"],"a":1}
//   split: {"t":"s","d":0,"r":0.62,"a":{...},"b":{...}}   d: 0=H 1=V, r=ratio
// Tabs serialize by panel title so reordered/renumbered panel ids survive.

std::string dockSerialize(const DockTree& tree, const std::vector<std::string>& titles);

// Rebuilds the tree from json. Returns false (tree untouched apart from pool
// allocs) when the json is missing/corrupt or references no known panels —
// caller should fall back to the default layout.
bool dockDeserialize(DockTree& tree, const char* json,
                     const std::vector<std::string>& titles);
