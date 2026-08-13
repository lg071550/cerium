#include "quad_batch.h"
#include "pipeline.h"

static const char* kQuadWGSL = R"wgsl(
struct Uniforms { screen: vec2f, scale: f32, pad: f32 };
@group(0) @binding(0) var<uniform> u: Uniforms;

struct VsIn {
  @builtin(vertex_index) vi: u32,
  @location(0) rect: vec4f,
  @location(1) color: vec4f,
  @location(2) params: vec4f,
};
struct VsOut {
  @builtin(position) pos: vec4f,
  @location(0) color: vec4f,
  @location(1) local: vec2f, // physical px within rect
  @location(2) size: vec2f,  // physical px
  @location(3) radius: f32,  // physical px
};

@vertex fn vs(in: VsIn) -> VsOut {
  var corners = array<vec2f, 6>(
    vec2f(0.0, 0.0), vec2f(1.0, 0.0), vec2f(1.0, 1.0),
    vec2f(0.0, 0.0), vec2f(1.0, 1.0), vec2f(0.0, 1.0));
  let c = corners[in.vi];
  let px = (in.rect.xy + c * in.rect.zw) * u.scale;
  let clip = vec2f(px.x / u.screen.x * 2.0 - 1.0, 1.0 - px.y / u.screen.y * 2.0);

  var out: VsOut;
  out.pos = vec4f(clip, 0.0, 1.0);
  out.color = in.color;
  out.local = c * in.rect.zw * u.scale;
  out.size = in.rect.zw * u.scale;
  out.radius = in.params.x * u.scale;
  return out;
}

@fragment fn fs(in: VsOut) -> @location(0) vec4f {
  let r = min(in.radius, min(in.size.x, in.size.y) * 0.5);
  let b = in.size * 0.5 - vec2f(r);
  let q = abs(in.local - in.size * 0.5) - b;
  let d = length(max(q, vec2f(0.0))) + min(max(q.x, q.y), 0.0) - r;
  let a = clamp(0.5 - d, 0.0, 1.0); // 1px physical AA band
  return vec4f(in.color.rgb, in.color.a * a);
}
)wgsl";

void QuadBatch::init(WGPUDevice dev, WGPUTextureFormat format,
                     WGPUBindGroupLayout uniformLayout) {
  device = dev;
  WGPUShaderModule shader = pipeline_shader(dev, kQuadWGSL, "quad");

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
  vb.arrayStride = sizeof(QuadInstance);
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
  plDesc.bindGroupLayoutCount = 1;
  plDesc.bindGroupLayouts = &uniformLayout;
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
}

void QuadBatch::upload(WGPUQueue queue, const QuadInstance* data, uint32_t count) {
  if (count == 0) return;
  if (count > capacity) {
    if (instances) wgpuBufferRelease(instances);
    capacity = count + count / 2;
    WGPUBufferDescriptor desc = {};
    desc.size = sizeof(QuadInstance) * capacity;
    desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    instances = wgpuDeviceCreateBuffer(device, &desc);
  }
  wgpuQueueWriteBuffer(queue, instances, 0, data, sizeof(QuadInstance) * count);
}

void QuadBatch::draw(WGPURenderPassEncoder pass, uint32_t first, uint32_t count) const {
  if (count == 0) return;
  wgpuRenderPassEncoderSetPipeline(pass, pipeline);
  wgpuRenderPassEncoderSetVertexBuffer(pass, 0, instances, 0, WGPU_WHOLE_SIZE);
  wgpuRenderPassEncoderDraw(pass, 6, count, 0, first);
}
