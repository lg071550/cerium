#pragma once

#include <webgpu/webgpu.h>

// Shared per-frame uniforms: logical→physical scale + physical screen size.
struct FrameUniforms {
  float screenW, screenH; // physical px
  float scale;            // devicePixelRatio
  float pad;
};

WGPUShaderModule pipeline_shader(WGPUDevice device, const char* wgsl, const char* label);

// Bind group layout: group(0) = one uniform buffer (vertex-visible).
WGPUBindGroupLayout pipeline_uniform_layout(WGPUDevice device);
WGPUBuffer pipeline_uniform_buffer(WGPUDevice device);
WGPUBindGroup pipeline_uniform_group(WGPUDevice device, WGPUBindGroupLayout layout,
                                     WGPUBuffer buf);

// Alpha-blend state shared by all UI pipelines (src-alpha over).
WGPUBlendState pipeline_blend_state();
