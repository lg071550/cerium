#pragma once

#include "chart_panes.h"
#include <array>
#include <cassert>
#include <cstring>

// Retain geometry only. Labels and interactions still run every frame. Replaying
// keeps each original clip/layer and submits actual draws at the selected FPS.
struct ChartGeometryCache {
  struct Key {
    uint64_t data = 0, settings = 0;
    std::array<double, 20> view{};
    bool operator==(const Key&) const = default;
  };
  Key key{};
  Theme colors{};
  bool valid = false;
  DrawList geometry;

  template<class Build>
  void draw(DrawList& target, const Key& next, Build build) {
    if (!valid || !(key == next) || geometry.quality() != target.quality() ||
        std::memcmp(&colors, &theme(), sizeof(Theme)) != 0) {
      key = next; colors = theme(); valid = true;
      geometry.reset();
      geometry.setQuality(target.quality());
      geometry.pushClip(target.currentClip());
      build(geometry);
      geometry.popClip();
      assert(geometry.glyphs.empty() && geometry.glyphShadows.empty());
    }
    const uint32_t q0 = (uint32_t)target.quads.size();
    const uint32_t l0 = (uint32_t)target.lines.size();
    target.quads.insert(target.quads.end(), geometry.quads.begin(), geometry.quads.end());
    target.lines.insert(target.lines.end(), geometry.lines.begin(), geometry.lines.end());
    for (size_t i = 0; i < geometry.cmds.size(); ++i) {
      const DrawCmd& src = geometry.cmds[i];
      // The first run belongs to the existing painter layer, as a direct call
      // would. Explicit breaks inside the body remain separate commands.
      if (i == 0 && !target.cmds.empty()) {
        DrawCmd& dst = target.cmds.back();
        if (dst.clip.x == src.clip.x && dst.clip.y == src.clip.y &&
            dst.clip.w == src.clip.w && dst.clip.h == src.clip.h) {
          dst.quadN += src.quadN; dst.lineN += src.lineN;
          continue;
        }
      }
      DrawCmd cmd = src;
      cmd.quad0 += q0; cmd.line0 += l0;
      cmd.glyph0 = (uint32_t)target.glyphs.size();
      cmd.shadow0 = (uint32_t)target.glyphShadows.size();
      target.cmds.push_back(cmd);
    }
  }
};

inline ChartGeometryCache::Key chartGeometryKey(uint64_t data, uint64_t settings,
    const ChartPane& pane, Rect clip, int first, int last, float start, float width) {
  return {data, settings, {pane.area.x, pane.area.y, pane.area.w, pane.area.h,
      pane.lo, pane.hi, (double)pane.log, (double)first, (double)last, start, width,
      clip.x, clip.y, clip.w, clip.h, pane.srcLo, pane.srcHi,
      (double)pane.remap, pane.dstLo, pane.dstHi}};
}
