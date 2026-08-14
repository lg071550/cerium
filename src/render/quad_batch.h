#pragma once

#include <webgpu/webgpu.h>
#include <cstdint>

// One instanced rounded rect. All geometry in logical px; the shader scales
// to physical and computes SDF corner coverage in physical px for crisp AA.
struct QuadInstance {
  float rect[4];   // x, y, w, h
  float color[4];  // rgba 0..1
  float params[4]; // corner radius, outline band (hollow), shadow softness,
                   // geometry inflate (room for the shadow falloff)
};

struct QuadBatch {
  WGPUDevice device = nullptr;
  WGPURenderPipeline pipeline = nullptr;
  WGPUBuffer instances = nullptr;
  uint32_t capacity = 0; // in instances

  void init(WGPUDevice device, WGPUTextureFormat format, WGPUBindGroupLayout uniformLayout);
  void upload(WGPUQueue queue, const QuadInstance* data, uint32_t count);
  // draws instances [first, first+count) of the uploaded buffer; skips
  // re-binding when lastPipe/lastVB already match (renderer tracks per pass)
  void draw(WGPURenderPassEncoder pass, uint32_t first, uint32_t count,
            WGPURenderPipeline& lastPipe, WGPUBuffer& lastVB) const;
};
