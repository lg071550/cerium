#pragma once

#include "../gpu/wgpu_context.h"
#include "glyph_atlas.h"
#include "line_batch.h"
#include "quad_batch.h"
#include "text_batch.h"

class DrawList;

// Frame orchestration: surface acquire → one render pass (quad + text draws
// grouped by clip rect) → present.
//
// Public API surface (all the host app touches):
//   beginInit(selector) → ready(...) each frame until true → render(...) per
//   frame; setClearColor, stats, atlas (font metrics for UI setup).
// Fatal init errors are reported via gpu_set_error_handler (wgpu_context.h).
struct Renderer {
  void beginInit(const char* canvasSelector);

  // Call each frame. Returns false until GPU + pipelines + font are ready.
  // fontSize is logical px. Handles DPR changes (rebakes the atlas).
  bool ready(const char* ttfPath, float fontSize, float dpr);

  void setClearColor(float r, float g, float b);

  // Renders the draw list. Skips the frame silently if the surface is
  // momentarily unavailable (mid-resize).
  void render(DrawList& list, int physW, int physH, float dpr);

  // Counters from the last render() call. drawCalls = actual draw() calls
  // issued (after per-cmd culling); quads/glyphs/lines = instance counts.
  struct Stats {
    int drawCalls = 0, cmds = 0, quads = 0, glyphs = 0, lines = 0;
  };
  const Stats& stats() const { return m_stats; }

  // The glyph atlas (font metrics, measure) — needed to set up text widgets.
  GlyphAtlas* atlas() { return &m_atlas; }

private:
  GpuContext m_gpu;
  WGPUBuffer m_uniformBuf = nullptr;
  WGPUBindGroupLayout m_uniformLayout = nullptr;
  WGPUBindGroup m_uniformGroup = nullptr;
  QuadBatch m_quads;
  TextBatch m_text;
  LineBatch m_lines;
  GlyphAtlas m_atlas;

  bool m_inited = false;
  bool m_failed = false; // fatal init error (reported via gpu_report_error)
  float m_dpr = 1.0f;
  float m_fontSize = 0.0f;
  float m_clearR = 0, m_clearG = 0, m_clearB = 0;

  Stats m_stats;
  uint32_t m_atlasGen = 0; // last bound atlas texture generation
};
