#pragma once

#include <webgpu/webgpu.h>
#include <cstdint>

// One instanced glyph quad. dst in logical px; uv = (u0, v0, du, dv).
struct GlyphInstance {
  float dst[4];
  float uv[4];
  float color[4];
};

struct TextBatch {
  WGPUDevice device = nullptr;
  WGPURenderPipeline pipeline = nullptr;
  WGPUBindGroupLayout atlasLayout = nullptr;
  WGPUBindGroup atlasGroup = nullptr;
  WGPUBuffer instances = nullptr;
  uint32_t capacity = 0;

  void init(WGPUDevice device, WGPUTextureFormat format, WGPUBindGroupLayout uniformLayout,
            WGPUTextureView atlasView, WGPUSampler atlasSampler);
  void setAtlas(WGPUTextureView view, WGPUSampler sampler); // after atlas rebake
  void upload(WGPUQueue queue, const GlyphInstance* data, uint32_t count);
  void draw(WGPURenderPassEncoder pass, uint32_t first, uint32_t count) const;
};
