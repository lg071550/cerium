#include "text_batch.h"
#include "pipeline.h"

static const char* kTextWGSL = R"wgsl(
struct Uniforms { screen: vec2f, scale: f32, pad: f32 };
@group(0) @binding(0) var<uniform> u: Uniforms;
@group(1) @binding(0) var atlasTex: texture_2d<f32>;
@group(1) @binding(1) var atlasSmp: sampler;

struct VsIn {
  @builtin(vertex_index) vi: u32,
  @location(0) dst: vec4f,
  @location(1) uv: vec4f,
  @location(2) color: vec4f,
};
struct VsOut {
  @builtin(position) pos: vec4f,
  @location(0) uv: vec2f,
  @location(1) color: vec4f,
};

@vertex fn vs(in: VsIn) -> VsOut {
  var corners = array<vec2f, 6>(
    vec2f(0.0, 0.0), vec2f(1.0, 0.0), vec2f(1.0, 1.0),
    vec2f(0.0, 0.0), vec2f(1.0, 1.0), vec2f(0.0, 1.0));
  let c = corners[in.vi];
  let px = (in.dst.xy + c * in.dst.zw) * u.scale;
  let clip = vec2f(px.x / u.screen.x * 2.0 - 1.0, 1.0 - px.y / u.screen.y * 2.0);

  var out: VsOut;
  out.pos = vec4f(clip, 0.0, 1.0);
  out.uv = in.uv.xy + c * in.uv.zw;
  out.color = in.color;
  return out;
}

@fragment fn fs(in: VsOut) -> @location(0) vec4f {
  let a = textureSample(atlasTex, atlasSmp, in.uv).r;
  return vec4f(in.color.rgb, in.color.a * a);
}
)wgsl";

void TextBatch::init(WGPUDevice dev, WGPUTextureFormat format,
                     WGPUBindGroupLayout uniformLayout, WGPUTextureView atlasView,
                     WGPUSampler atlasSampler) {
  device = dev;

  WGPUBindGroupLayoutEntry texEntry = {};
  texEntry.binding = 0;
  texEntry.visibility = WGPUShaderStage_Fragment;
  texEntry.texture.sampleType = WGPUTextureSampleType_Float;
  texEntry.texture.viewDimension = WGPUTextureViewDimension_2D;

  WGPUBindGroupLayoutEntry smpEntry = {};
  smpEntry.binding = 1;
  smpEntry.visibility = WGPUShaderStage_Fragment;
  smpEntry.sampler.type = WGPUSamplerBindingType_Filtering;

  WGPUBindGroupLayoutEntry entries[2] = {texEntry, smpEntry};
  WGPUBindGroupLayoutDescriptor layoutDesc = {};
  layoutDesc.entryCount = 2;
  layoutDesc.entries = entries;
  atlasLayout = wgpuDeviceCreateBindGroupLayout(dev, &layoutDesc);

  WGPUShaderModule shader = pipeline_shader(dev, kTextWGSL, "text");

  WGPUVertexAttribute attrs[3] = {};
  attrs[0].format = WGPUVertexFormat_Float32x4;
  attrs[0].offset = 0;
  attrs[0].shaderLocation = 0;
  attrs[1].format = WGPUVertexFormat_Float32x4;
  attrs[1].offset = 16;
  attrs[1].shaderLocation = 1;
  attrs[2].format = WGPUVertexFormat_Float32x4;
  attrs[2].offset = 32;
  attrs[2].shaderLocation = 2;

  WGPUVertexBufferLayout vb = {};
  vb.arrayStride = sizeof(GlyphInstance);
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

  WGPUBindGroupLayout layouts[2] = {uniformLayout, atlasLayout};
  WGPUPipelineLayoutDescriptor plDesc = {};
  plDesc.bindGroupLayoutCount = 2;
  plDesc.bindGroupLayouts = layouts;
  WGPUPipelineLayout plLayout = wgpuDeviceCreatePipelineLayout(dev, &plDesc);

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

  pipeline = wgpuDeviceCreateRenderPipeline(dev, &desc);

  wgpuPipelineLayoutRelease(plLayout);
  wgpuShaderModuleRelease(shader);

  setAtlas(atlasView, atlasSampler);
}

void TextBatch::setAtlas(WGPUTextureView view, WGPUSampler sampler) {
  if (atlasGroup) wgpuBindGroupRelease(atlasGroup);

  WGPUBindGroupEntry entries[2] = {};
  entries[0].binding = 0;
  entries[0].textureView = view;
  entries[1].binding = 1;
  entries[1].sampler = sampler;

  WGPUBindGroupDescriptor desc = {};
  desc.layout = atlasLayout;
  desc.entryCount = 2;
  desc.entries = entries;
  atlasGroup = wgpuDeviceCreateBindGroup(device, &desc);
}

void TextBatch::upload(WGPUQueue queue, const GlyphInstance* data, uint32_t count) {
  if (count == 0) return;
  if (count > capacity) {
    if (instances) wgpuBufferRelease(instances);
    capacity = count + count / 2;
    WGPUBufferDescriptor desc = {};
    desc.size = sizeof(GlyphInstance) * capacity;
    desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    instances = wgpuDeviceCreateBuffer(device, &desc);
  }
  wgpuQueueWriteBuffer(queue, instances, 0, data, sizeof(GlyphInstance) * count);
}

void TextBatch::draw(WGPURenderPassEncoder pass, uint32_t first, uint32_t count) const {
  if (count == 0) return;
  wgpuRenderPassEncoderSetPipeline(pass, pipeline);
  wgpuRenderPassEncoderSetBindGroup(pass, 1, atlasGroup, 0, nullptr);
  wgpuRenderPassEncoderSetVertexBuffer(pass, 0, instances, 0, WGPU_WHOLE_SIZE);
  wgpuRenderPassEncoderDraw(pass, 6, count, 0, first);
}
