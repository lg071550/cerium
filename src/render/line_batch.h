#pragma once

#include <webgpu/webgpu.h>
#include <cstdint>

// One instanced line segment: endpoints in logical px, expanded to a
// screen-aligned quad in the vertex shader. Butt caps, 1px physical AA.
struct LineInstance {
  float pts[4];   // x0, y0, x1, y1
  float color[4]; // rgba 0..1
  float params[4]; // thickness (logical px), unused x3
};

struct LineBatch {
  WGPUDevice device = nullptr;
  WGPURenderPipeline pipeline = nullptr;
  WGPUBuffer instances = nullptr;
  uint32_t capacity = 0;

  void init(WGPUDevice device, WGPUTextureFormat format, WGPUBindGroupLayout uniformLayout);
  void upload(WGPUQueue queue, const LineInstance* data, uint32_t count);
  void draw(WGPURenderPassEncoder pass, uint32_t first, uint32_t count,
            WGPURenderPipeline& lastPipe, WGPUBuffer& lastVB) const;
};
