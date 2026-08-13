#pragma once

#include "../gpu/wgpu_context.h"
#include "glyph_atlas.h"
#include "quad_batch.h"
#include "text_batch.h"

class DrawList;

// Frame orchestration: surface acquire → one render pass (quad + text draws
// grouped by clip rect) → present.
struct Renderer {
  GpuContext gpu;
  WGPUBuffer uniformBuf = nullptr;
  WGPUBindGroupLayout uniformLayout = nullptr;
  WGPUBindGroup uniformGroup = nullptr;
  QuadBatch quads;
  TextBatch text;
  GlyphAtlas atlas;

  bool inited = false;
  float dpr = 1.0f;
  float clearR = 0, clearG = 0, clearB = 0;

  void beginInit(const char* canvasSelector);

  // Call each frame. Returns false until GPU + pipelines + font are ready.
  // fontSize is logical px. Handles DPR changes (rebakes the atlas).
  bool ready(const char* ttfPath, float fontSize, float dpr);

  void setClearColor(float r, float g, float b);

  // Renders the draw list. Skips the frame silently if the surface is
  // momentarily unavailable (mid-resize).
  void render(DrawList& list, int physW, int physH, float dpr);
};
