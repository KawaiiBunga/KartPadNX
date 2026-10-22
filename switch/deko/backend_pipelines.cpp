// Native deko3d adapter, part 2: shader modules and render pipelines. A pipeline's WGSL stages
// are translated (Tint) and compiled (uam) to native shaders, or found in the shader cache, and
// its WebGPU state is converted to deko3d state. See backend_internal.h for the whole adapter.
#include "backend_internal.h"
#ifdef COS_DK_DEBUG_LIB
#include <sys/stat.h>
#endif

namespace kd {
std::unordered_map<std::string, Stage> stageCache;
std::unordered_map<WGPURenderPipeline, std::shared_ptr<Pipeline>> pipelines;
std::unordered_map<WGPUShaderModule, std::pair<std::string, int>> modules;
Stage compile(WGPUShaderModule module, WGPUStringView entry, ShaderStage stage) {
  std::string source;
  {
    Lock l(mutex);
    auto it = modules.find(module);
    if (it == modules.end())
      fatal("Missing native shader module %p for entry %s", static_cast<void *>(module),
            str(entry).c_str());
    source = it->second.first;
  }
  // compilerMutex guards the stage memo only. Tint and the wait for uam run outside it, so several
  // pipeline workers translate at once while uam's one worker compiles (the boot preload builds on
  // every core). Two workers may translate the same stage; the first to finish fills the memo.
  const auto key = std::to_string(XXH3_64bits(source.data(), source.size())) + ":" +
                   std::to_string(static_cast<unsigned>(stage)) + ":" + str(entry);
  const NamedShader* cached = nullptr;
  {
    std::lock_guard compilerLock(compilerMutex);
    if (auto it = stageCache.find(key); it != stageCache.end())
      return it->second;
    if ((cached = shader_cache_named(key.c_str())))
      stageCache.emplace(key, Stage{cached->entry, cached->bindings});
  }
  if (cached) {
    Stage s{cached->entry, cached->bindings};
    shader_record_stage(key, stage, s.shader->glslHash, s.slots, source);
    return s;
  }
  ShaderCompileTrace trace(key, static_cast<unsigned>(stage), source);
  auto r = wgsl_to_glsl(source, str(entry), stage, BindingScheme::Sequential);
  trace.phase("tint-exit");
  if (!r.ok)
    fatal("Tint: %s", r.error.c_str());
  Stage s;
  s.slots = std::move(r.bindings);
  s.shader = shader_entry(stage, XXH3_64bits(r.glsl.data(), r.glsl.size()));
#ifdef COS_DK_DEBUG_LIB
  if (FILE *flag = fopen("sdmc:/switch/kartpad/deko-capture.flag", "rb")) {
    fclose(flag);
    mkdir("sdmc:/switch/kartpad/Logs/deko-capture", 0777);
    char path[192];
    for (const auto &item : {std::pair{"wgsl", &source}, std::pair{"glsl", &r.glsl}}) {
      snprintf(path, sizeof path, "sdmc:/switch/kartpad/Logs/deko-capture/shader-%016llx-%u.%s",
               (unsigned long long)s.shader->glslHash, unsigned(stage), item.first);
      if (FILE *out = fopen(path, "wb")) {
        fwrite(item.second->data(), 1, item.second->size(), out);
        fclose(out);
      }
    }
  }
#endif
  shader_compile(s.shader, r.glsl, true);
  if (!shader_wait(s.shader))
    fatal("uam failed compiling %s", str(entry).c_str());
  trace.phase("done");
  {
    std::lock_guard compilerLock(compilerMutex);
    if (!stageCache.emplace(key, s).second)
      return stageCache.at(key);
  }
  shader_record_stage(key, stage, s.shader->glslHash, s.slots, source);
  return s;
}
// Where pipeline creation time goes, summed over all creations and logged every 64 pipelines:
// Dawn's WGSL parse and validation (shader modules), the native stage lookup or compile, and
// Dawn's own pipeline validation.
struct BuildTimes {
  std::atomic<uint64_t> moduleUs{0}, modules{0}, stagesUs{0}, dawnPipelineUs{0}, pipelines{0};
};
BuildTimes buildTimes;
uint64_t micros_since(std::chrono::steady_clock::time_point start) {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count());
}
void count_pipeline_build(uint64_t stagesUs, uint64_t dawnPipelineUs) {
  buildTimes.stagesUs += stagesUs;
  buildTimes.dawnPipelineUs += dawnPipelineUs;
  const uint64_t n = ++buildTimes.pipelines;
  if (n % 64)
    return;
  const uint64_t modules = std::max<uint64_t>(buildTimes.modules.load(), 1);
  swdk::dklog("pipeline builds: %llu pipelines; per pipeline: native stages %.1f ms, Dawn pipeline "
              "%.1f ms; per shader module (%llu): Dawn WGSL %.1f ms",
              (unsigned long long)n, buildTimes.stagesUs.load() / 1000.0 / n,
              buildTimes.dawnPipelineUs.load() / 1000.0 / n, (unsigned long long)modules,
              buildTimes.moduleUs.load() / 1000.0 / modules);
}
DkCompareOp compare(WGPUCompareFunction f) {
  return f == WGPUCompareFunction_Undefined
             ? DkCompareOp_Always
             : DkCompareOp(DkCompareOp_Never + int(f) - int(WGPUCompareFunction_Never));
}
DkBlendFactor factor(WGPUBlendFactor f, bool alpha) {
  switch (f) {
  case WGPUBlendFactor_Zero:
    return DkBlendFactor_Zero;
  case WGPUBlendFactor_One:
    return DkBlendFactor_One;
  case WGPUBlendFactor_Src:
    return DkBlendFactor_SrcColor;
  case WGPUBlendFactor_OneMinusSrc:
    return DkBlendFactor_InvSrcColor;
  case WGPUBlendFactor_SrcAlpha:
    return DkBlendFactor_SrcAlpha;
  case WGPUBlendFactor_OneMinusSrcAlpha:
    return DkBlendFactor_InvSrcAlpha;
  case WGPUBlendFactor_Dst:
    return DkBlendFactor_DstColor;
  case WGPUBlendFactor_OneMinusDst:
    return DkBlendFactor_InvDstColor;
  case WGPUBlendFactor_DstAlpha:
    return DkBlendFactor_DstAlpha;
  case WGPUBlendFactor_OneMinusDstAlpha:
    return DkBlendFactor_InvDstAlpha;
  case WGPUBlendFactor_Constant:
    return alpha ? DkBlendFactor_ConstAlpha : DkBlendFactor_ConstColor;
  case WGPUBlendFactor_OneMinusConstant:
    return alpha ? DkBlendFactor_InvConstAlpha : DkBlendFactor_InvConstColor;
  case WGPUBlendFactor_SrcAlphaSaturated:
    return DkBlendFactor_SrcAlphaSaturate;
  case WGPUBlendFactor_Src1:
    return DkBlendFactor_Src1Color;
  case WGPUBlendFactor_OneMinusSrc1:
    return DkBlendFactor_InvSrc1Color;
  case WGPUBlendFactor_Src1Alpha:
    return DkBlendFactor_Src1Alpha;
  case WGPUBlendFactor_OneMinusSrc1Alpha:
    return DkBlendFactor_InvSrc1Alpha;
  default:
    fatal("Unsupported blend factor %d", int(f));
  }
}
DkBlendOp blendop(WGPUBlendOperation o) {
  switch (o) {
  case WGPUBlendOperation_Subtract:
    return DkBlendOp_Sub;
  case WGPUBlendOperation_ReverseSubtract:
    return DkBlendOp_RevSub;
  case WGPUBlendOperation_Min:
    return DkBlendOp_Min;
  case WGPUBlendOperation_Max:
    return DkBlendOp_Max;
  default:
    return DkBlendOp_Add;
  }
}
} // namespace kd

extern "C" {
WGPUShaderModule __wrap_wgpuDeviceCreateShaderModule(WGPUDevice d,
                                                     const WGPUShaderModuleDescriptor *desc) {
  kd::Lock api(kd::shaderApiMutex);
  const auto start = std::chrono::steady_clock::now();
  auto h = __real_wgpuDeviceCreateShaderModule(d, desc);
  kd::buildTimes.moduleUs += kd::micros_since(start);
  ++kd::buildTimes.modules;
  kd::Lock l(kd::mutex);
  if (auto it = kd::modules.find(h); it != kd::modules.end()) {
    ++it->second.second; // Create returns an owned reference even on a Dawn
                         // cache hit.
    return h;
  }
  for (auto c = desc->nextInChain; c; c = c->next)
    if (c->sType == WGPUSType_ShaderSourceWGSL)
      kd::modules[h] = {kd::str(reinterpret_cast<const WGPUShaderSourceWGSL *>(c)->code), 1};
  return h;
}
void __wrap_wgpuShaderModuleAddRef(WGPUShaderModule h) {
  kd::Lock api(kd::shaderApiMutex);
  {
    kd::Lock l(kd::mutex);
    if (kd::modules.count(h))
      ++kd::modules[h].second;
  }
  __real_wgpuShaderModuleAddRef(h);
}
void __wrap_wgpuShaderModuleRelease(WGPUShaderModule h) {
  kd::Lock api(kd::shaderApiMutex);
  {
    kd::Lock l(kd::mutex);
    auto it = kd::modules.find(h);
    if (it != kd::modules.end() && !--it->second.second)
      kd::modules.erase(it);
  }
  __real_wgpuShaderModuleRelease(h);
}
WGPURenderPipeline __wrap_wgpuDeviceCreateRenderPipeline(WGPUDevice d,
                                                         const WGPURenderPipelineDescriptor *desc) {
  // Build privately. A partially translated pipeline is never published, and
  // the render worker can upload, submit and present while this call compiles.
  auto p = std::make_shared<kd::Pipeline>();
  p->label = kd::str(desc->label);
  const auto stagesStart = std::chrono::steady_clock::now();
  p->vs = kd::compile(desc->vertex.module, desc->vertex.entryPoint, swdk::ShaderStage::Vertex);
  if (!desc->fragment)
    swdk::fatal("Pipeline has no fragment stage");
  p->fs =
      kd::compile(desc->fragment->module, desc->fragment->entryPoint, swdk::ShaderStage::Fragment);
  const uint64_t stagesUs = kd::micros_since(stagesStart);
  dkRasterizerStateDefaults(&p->raster);
  dkDepthStencilStateDefaults(&p->depth);
  // WebGPU omitting depthStencil disables both operations. deko3d's defaults
  // enable Less testing and writes, which must never leak into color-only
  // snapshot-copy, display-copy or ImGui passes.
  p->depth.depthTestEnable = false;
  p->depth.depthWriteEnable = false;
  dkColorStateDefaults(&p->color);
  dkColorWriteStateDefaults(&p->write);
  dkBlendStateDefaults(&p->blend);
  p->raster.cullMode = desc->primitive.cullMode == WGPUCullMode_Back    ? DkFace_Back
                       : desc->primitive.cullMode == WGPUCullMode_Front ? DkFace_Front
                                                                        : DkFace_None;
  p->raster.frontFace =
      desc->primitive.frontFace == WGPUFrontFace_CW ? DkFrontFace_CW : DkFrontFace_CCW;
  switch (desc->primitive.topology) {
  case WGPUPrimitiveTopology_PointList:
    p->topology = DkPrimitive_Points;
    break;
  case WGPUPrimitiveTopology_LineList:
    p->topology = DkPrimitive_Lines;
    break;
  case WGPUPrimitiveTopology_LineStrip:
    p->topology = DkPrimitive_LineStrip;
    break;
  case WGPUPrimitiveTopology_TriangleStrip:
    p->topology = DkPrimitive_TriangleStrip;
    break;
  default:
    break;
  }
  if (desc->depthStencil) {
    const auto &ds = *desc->depthStencil;
    p->depth.depthTestEnable = true;
    p->depth.depthWriteEnable = ds.depthWriteEnabled == WGPUOptionalBool_True;
    p->depth.depthCompareOp = kd::compare(ds.depthCompare);
    p->bias[0] = ds.depthBias;
    p->bias[1] = ds.depthBiasClamp;
    p->bias[2] = ds.depthBiasSlopeScale;
    p->raster.depthBiasEnableMask =
        (ds.depthBias || ds.depthBiasSlopeScale) ? DkPolygonFlag_All : 0;
  }
  const auto &target = desc->fragment->targets[0];
  dkColorWriteStateSetMask(&p->write, 0, target.writeMask & 15);
  if (target.blend) {
    const auto &b = *target.blend;
    dkColorStateSetBlendEnable(&p->color, 0, true);
    dkBlendStateSetOps(&p->blend, kd::blendop(b.color.operation), kd::blendop(b.alpha.operation));
    dkBlendStateSetFactors(
        &p->blend, kd::factor(b.color.srcFactor, false), kd::factor(b.color.dstFactor, false),
        kd::factor(b.alpha.srcFactor, true), kd::factor(b.alpha.dstFactor, true));
  }
  p->vertexStates.resize(desc->vertex.bufferCount);
  for (size_t i = 0; i < desc->vertex.bufferCount; ++i) {
    const auto &b = desc->vertex.buffers[i];
    p->vertexStates[i].stride = b.arrayStride;
    p->vertexStates[i].divisor = b.stepMode == WGPUVertexStepMode_Instance ? 1 : 0;
    for (size_t j = 0; j < b.attributeCount; ++j) {
      const auto &a = b.attributes[j];
      if (p->attributes.size() <= a.shaderLocation)
        p->attributes.resize(a.shaderLocation + 1);
      auto &v = p->attributes[a.shaderLocation];
      v.bufferId = i;
      v.offset = a.offset;
      switch (a.format) {
      case WGPUVertexFormat_Float32x2:
        v.size = DkVtxAttribSize_2x32;
        v.type = DkVtxAttribType_Float;
        break;
      case WGPUVertexFormat_Float32x3:
        v.size = DkVtxAttribSize_3x32;
        v.type = DkVtxAttribType_Float;
        break;
      case WGPUVertexFormat_Float32x4:
        v.size = DkVtxAttribSize_4x32;
        v.type = DkVtxAttribType_Float;
        break;
      case WGPUVertexFormat_Unorm8x4:
        v.size = DkVtxAttribSize_4x8;
        v.type = DkVtxAttribType_Unorm;
        break;
      default:
        swdk::fatal("Unsupported vertex attribute %d", int(a.format));
      }
    }
  }
  kd::Lock api(kd::pipelineApiMutex);
  const auto dawnStart = std::chrono::steady_clock::now();
  auto h = __real_wgpuDeviceCreateRenderPipeline(d, desc);
  kd::count_pipeline_build(stagesUs, kd::micros_since(dawnStart));
  kd::Lock l(kd::mutex);
  if (auto it = kd::pipelines.find(h); it != kd::pipelines.end())
    ++it->second->refs;
  else
    kd::pipelines[h] = p;
  return h;
}
void __wrap_wgpuRenderPipelineAddRef(WGPURenderPipeline h) {
  kd::Lock api(kd::pipelineApiMutex);
  {
    kd::Lock l(kd::mutex);
    if (kd::pipelines.count(h))
      ++kd::pipelines[h]->refs;
  }
  __real_wgpuRenderPipelineAddRef(h);
}
void __wrap_wgpuRenderPipelineRelease(WGPURenderPipeline h) {
  kd::Lock api(kd::pipelineApiMutex);
  {
    kd::Lock l(kd::mutex);
    auto it = kd::pipelines.find(h);
    if (it != kd::pipelines.end() && !--it->second->refs)
      kd::pipelines.erase(it);
  }
  __real_wgpuRenderPipelineRelease(h);
}
}
