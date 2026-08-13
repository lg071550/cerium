#include "pipeline.h"

WGPUShaderModule pipeline_shader(WGPUDevice device, const char* wgsl, const char* label) {
  WGPUShaderSourceWGSL src = {};
  src.chain.sType = WGPUSType_ShaderSourceWGSL;
  src.code.data = wgsl;
  src.code.length = WGPU_STRLEN;

  WGPUShaderModuleDescriptor desc = {};
  desc.label.data = label;
  desc.label.length = WGPU_STRLEN;
  desc.nextInChain = &src.chain;
  return wgpuDeviceCreateShaderModule(device, &desc);
}

WGPUBindGroupLayout pipeline_uniform_layout(WGPUDevice device) {
  WGPUBindGroupLayoutEntry entry = {};
  entry.binding = 0;
  entry.visibility = WGPUShaderStage_Vertex;
  entry.buffer.type = WGPUBufferBindingType_Uniform;
  entry.buffer.minBindingSize = sizeof(FrameUniforms);

  WGPUBindGroupLayoutDescriptor desc = {};
  desc.entryCount = 1;
  desc.entries = &entry;
  return wgpuDeviceCreateBindGroupLayout(device, &desc);
}

WGPUBuffer pipeline_uniform_buffer(WGPUDevice device) {
  WGPUBufferDescriptor desc = {};
  desc.size = sizeof(FrameUniforms);
  desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
  return wgpuDeviceCreateBuffer(device, &desc);
}

WGPUBindGroup pipeline_uniform_group(WGPUDevice device, WGPUBindGroupLayout layout,
                                     WGPUBuffer buf) {
  WGPUBindGroupEntry entry = {};
  entry.binding = 0;
  entry.buffer = buf;
  entry.size = sizeof(FrameUniforms);

  WGPUBindGroupDescriptor desc = {};
  desc.layout = layout;
  desc.entryCount = 1;
  desc.entries = &entry;
  return wgpuDeviceCreateBindGroup(device, &desc);
}

WGPUBlendState pipeline_blend_state() {
  WGPUBlendState blend = {};
  blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
  blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
  blend.color.operation = WGPUBlendOperation_Add;
  blend.alpha.srcFactor = WGPUBlendFactor_One;
  blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
  blend.alpha.operation = WGPUBlendOperation_Add;
  return blend;
}
