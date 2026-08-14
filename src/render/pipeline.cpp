#include "pipeline.h"

const char* wgsl_common() {
  return R"wgsl(
struct Uniforms { screen: vec2f, scale: f32, pad: f32 };
@group(0) @binding(0) var<uniform> u: Uniforms;

// The surface is sRGB (color-managed canvas): shaders output linear light and
// the view encodes to sRGB on write — authored sRGB colors display unchanged
// and blending happens in linear space.
fn srgb_to_linear(c: f32) -> f32 {
  return select(c / 12.92, pow((c + 0.055) / 1.055, 2.4), c >= 0.04045);
}
)wgsl";
}

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

WGPURenderPipeline pipeline_instanced(WGPUDevice device, WGPUTextureFormat format,
                                      const char* wgsl, const char* label,
                                      uint64_t instanceStride,
                                      const WGPUBindGroupLayout* layouts,
                                      uint32_t layoutCount) {
  WGPUShaderModule shader = pipeline_shader(device, wgsl, label);

  WGPUVertexAttribute attrs[3] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    attrs[i].format = WGPUVertexFormat_Float32x4;
    attrs[i].offset = 16 * i;
    attrs[i].shaderLocation = i;
  }

  WGPUVertexBufferLayout vb = {};
  vb.arrayStride = instanceStride;
  vb.stepMode = WGPUVertexStepMode_Instance;
  vb.attributeCount = 3;
  vb.attributes = attrs;

  WGPUBlendState blend = pipeline_blend_state();
  WGPUColorTargetState colorTarget = {};
  colorTarget.format = format;
  colorTarget.blend = &blend;
  colorTarget.writeMask = WGPUColorWriteMask_All;

  WGPUFragmentState fragment = {};
  fragment.module = shader;
  fragment.entryPoint.data = "fs";
  fragment.entryPoint.length = WGPU_STRLEN;
  fragment.targetCount = 1;
  fragment.targets = &colorTarget;

  WGPUPipelineLayoutDescriptor plDesc = {};
  plDesc.bindGroupLayoutCount = layoutCount;
  plDesc.bindGroupLayouts = layouts;
  WGPUPipelineLayout plLayout = wgpuDeviceCreatePipelineLayout(device, &plDesc);

  WGPURenderPipelineDescriptor desc = {};
  desc.layout = plLayout;
  desc.vertex.module = shader;
  desc.vertex.entryPoint.data = "vs";
  desc.vertex.entryPoint.length = WGPU_STRLEN;
  desc.vertex.bufferCount = 1;
  desc.vertex.buffers = &vb;
  desc.fragment = &fragment;
  desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
  desc.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
  desc.primitive.frontFace = WGPUFrontFace_CCW;
  desc.primitive.cullMode = WGPUCullMode_None;
  desc.multisample.count = 1;
  desc.multisample.mask = 0xFFFFFFFF;
  desc.multisample.alphaToCoverageEnabled = false;

  WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device, &desc);

  wgpuPipelineLayoutRelease(plLayout);
  wgpuShaderModuleRelease(shader);
  return pipeline;
}
