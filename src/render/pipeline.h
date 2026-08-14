#pragma once

#include <webgpu/webgpu.h>

// Shared per-frame uniforms: logical→physical scale + physical screen size.
struct FrameUniforms {
  float screenW, screenH; // physical px
  float scale;            // devicePixelRatio
  float pad;              // uniform buffers are 16-byte aligned; matches the
                          // WGSL `pad: f32` in wgsl_common()
};

WGPUShaderModule pipeline_shader(WGPUDevice device, const char* wgsl, const char* label);

// WGSL shared by all batch shaders: the group(0) uniforms block and the
// sRGB→linear helper. Prepend to each shader source at module creation.
const char* wgsl_common();

// Bind group layout: group(0) = one uniform buffer (vertex-visible).
WGPUBindGroupLayout pipeline_uniform_layout(WGPUDevice device);
WGPUBuffer pipeline_uniform_buffer(WGPUDevice device);
WGPUBindGroup pipeline_uniform_group(WGPUDevice device, WGPUBindGroupLayout layout,
                                     WGPUBuffer buf);

// Alpha-blend state shared by all UI pipelines (src-alpha over).
WGPUBlendState pipeline_blend_state();

// Render-pipeline creation shared by the instanced batches (quad/line/text):
// one instance vertex buffer (3 × float32x4 at locations 0..2), shared blend
// state, vs/fs entry points from the given WGSL.
WGPURenderPipeline pipeline_instanced(WGPUDevice device, WGPUTextureFormat format,
                                      const char* wgsl, const char* label,
                                      uint64_t instanceStride,
                                      const WGPUBindGroupLayout* layouts,
                                      uint32_t layoutCount);
