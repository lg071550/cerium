#include "text_batch.h"
#include "pipeline.h"

#include <string>

// group(0) uniforms + srgb_to_linear come from wgsl_common() (pipeline.h)
static const char* kTextWGSL = R"wgsl(
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
  let rgb = vec3f(srgb_to_linear(in.color.x), srgb_to_linear(in.color.y),
                  srgb_to_linear(in.color.z));
  return vec4f(rgb, in.color.a * a);
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

  std::string src = std::string(wgsl_common()) + kTextWGSL;
  WGPUBindGroupLayout layouts[2] = {uniformLayout, atlasLayout};
  pipeline = pipeline_instanced(dev, format, src.c_str(), "text", sizeof(GlyphInstance),
                                layouts, 2);

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

void TextBatch::draw(WGPURenderPassEncoder pass, uint32_t first, uint32_t count,
                     WGPURenderPipeline& lastPipe, WGPUBuffer& lastVB) const {
  if (count == 0) return;
  if (pipeline != lastPipe) {
    wgpuRenderPassEncoderSetPipeline(pass, pipeline);
    lastPipe = pipeline;
  }
  wgpuRenderPassEncoderSetBindGroup(pass, 1, atlasGroup, 0, nullptr);
  if (instances != lastVB) {
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, instances, 0, WGPU_WHOLE_SIZE);
    lastVB = instances;
  }
  wgpuRenderPassEncoderDraw(pass, 6, count, 0, first);
}
