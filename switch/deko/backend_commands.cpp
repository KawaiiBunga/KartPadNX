// Native deko3d adapter, part 3: command recording and replay. Encoders and render passes
// record closures; queue submission replays them into the frame's deko3d command buffer, where
// the draw state (bindings, pipeline, split RGB/alpha blending) is applied. See
// backend_internal.h for the whole adapter.
#include "backend_internal.h"
#ifdef COS_DK_DEBUG_LIB
#include "present_vsh_dksh.h"
#include "probe_fsh_dksh.h"
#include <sys/stat.h>
#endif

namespace kd {
std::unordered_map<WGPUCommandEncoder, std::shared_ptr<Commands>> encoders;
std::unordered_map<WGPUCommandEncoder, int> encoderRefs;
std::unordered_map<WGPUCommandBuffer, std::pair<std::shared_ptr<Commands>, int>> commands;
std::unordered_map<WGPURenderPassEncoder, Pass> passes;
State state;
Binding *binding(BoundGroup &g, uint32_t b) {
  if (g.group)
    for (auto &e : g.group->entries)
      if (e.binding == b)
        return &e;
  return nullptr;
}
uint64_t offset(BoundGroup &g, Binding &b) {
  return b.offset +
         (b.dynamic >= 0 && size_t(b.dynamic) < g.offsets.size() ? g.offsets[b.dynamic] : 0);
}
void bind_stage(Stage &s, DkStage stage) {
  const uint32_t cacheStage = stage == DkStage_Vertex ? 0 : 1;
  for (auto &slot : s.slots) {
    if (slot.group >= state.groups.size())
      fatal("Invalid bind group");
    auto &g = state.groups[slot.group];
    auto *b = binding(g, slot.binding);
    if (slot.kind == SlotBinding::Kind::Sampler) {
      auto *sampler = slot.samplerGroup < state.groups.size()
                          ? binding(state.groups[slot.samplerGroup], slot.samplerBinding)
                          : nullptr;
      const DkResHandle h =
          texture_handle(b ? find_view(b->view.Get()) : nullptr,
                         sampler ? find_sampler(sampler->sampler.Get()) : nullptr);
      if (state.bindings.texture(cacheStage, slot.slot, h))
        dkCmdBufBindTextures(R.cmd, stage, slot.slot, &h, 1);
    } else if (b && b->buffer) {
      uint64_t off = offset(g, *b);
      if (off > b->buffer->size)
        fatal("Buffer binding offset out of bounds");
      uint64_t n = std::min(b->size, b->buffer->size - off);
      if (slot.kind == SlotBinding::Kind::Uniform &&
          state.bindings.uniform(cacheStage, slot.slot, b->buffer->mem.gpu + off, n))
        dkCmdBufBindUniformBuffer(R.cmd, stage, slot.slot, b->buffer->mem.gpu + off, n);
      else if (slot.kind == SlotBinding::Kind::Storage &&
               state.bindings.storage(cacheStage, slot.slot, b->buffer->mem.gpu + off, n))
        dkCmdBufBindStorageBuffer(R.cmd, stage, slot.slot, b->buffer->mem.gpu + off, n);
    } else if (slot.kind != SlotBinding::Kind::Immediates)
      fatal("Missing buffer binding %u:%u", slot.group, slot.binding);
  }
}
bool bind_pipeline() {
  auto p = state.pipeline;
  if (!p)
    fatal("Draw without pipeline");
  if (!shader_usable(p->vs.shader) || !shader_usable(p->fs.shader))
    fatal("Native shader could not be loaded");
  p->vs.shader->usedDraws.fetch_add(1, std::memory_order_relaxed);
  p->fs.shader->usedDraws.fetch_add(1, std::memory_order_relaxed);
  if (code_take_written())
    dkCmdBufBarrier(R.cmd, DkBarrier_None, DkInvalidateFlags_Shader);
  if (state.boundPipeline != p.get()) {
    // Pipeline binds and any fixed/native helper may change driver-side state.
    // A pass reset clears these too; never carry bindings across render passes.
    state.bindings.invalidate();
    const DkShader *shaders[] = {&p->vs.shader->shader, &p->fs.shader->shader};
    dkCmdBufBindShaders(R.cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
    dkCmdBufBindRasterizerState(R.cmd, &p->raster);
    dkCmdBufBindDepthStencilState(R.cmd, &p->depth);
    dkCmdBufBindColorState(R.cmd, &p->color);
    dkCmdBufBindColorWriteState(R.cmd, &p->write);
    auto blend = p->blend;
    if ((p->write.masks & 15) == DkColorMask_A)
      blend.dstColorBlendFactor = blend.dstAlphaBlendFactor;
    dkCmdBufBindBlendStates(R.cmd, 0, &blend, 1);
    dkCmdBufSetDepthBias(R.cmd, p->bias[0], p->bias[1], p->bias[2]);
    dkCmdBufBindVtxAttribState(R.cmd, p->attributes.empty() ? nullptr : p->attributes.data(),
                               p->attributes.size());
    dkCmdBufBindVtxBufferState(R.cmd, p->vertexStates.empty() ? nullptr : p->vertexStates.data(),
                               p->vertexStates.size());
    state.boundPipeline = p.get();
    state.splitAlphaDirty = false;
  } else if (state.splitAlphaDirty) {
    // The second alpha-only draw changed only these three states. Preserve
    // shaders and resource bindings instead of invalidating the full pipeline
    // after every translucent UI/material draw.
    dkCmdBufBindDepthStencilState(R.cmd, &p->depth);
    dkCmdBufBindColorWriteState(R.cmd, &p->write);
    auto blend = p->blend;
    if ((p->write.masks & 15) == DkColorMask_A)
      blend.dstColorBlendFactor = blend.dstAlphaBlendFactor;
    dkCmdBufBindBlendStates(R.cmd, 0, &blend, 1);
    state.splitAlphaDirty = false;
  }
  bind_stage(p->vs, DkStage_Vertex);
  bind_stage(p->fs, DkStage_Fragment);
  return true;
}
#ifdef COS_DK_DEBUG_LIB
#include "gx_probe.inc"
#endif
void draw(bool indexed, uint32_t count, uint32_t instances, uint32_t first, int32_t base,
          uint32_t firstInstance) {
  bind_pipeline();
#ifdef COS_DK_DEBUG_LIB
  // Probe before the real draw: the depth comparison must see the same depth
  // image the real draw will see, rather than its own already-written depth.
  probe_draw(indexed, count, instances, first, base, firstInstance);
  bind_pipeline();
#endif
  ++state.draws;
  auto p = state.pipeline;
  auto issue = [&] {
    if (indexed) {
      if (!state.index)
        fatal("Missing index buffer");
      if (state.bindings.index(state.index->mem.gpu + state.indexOffset, state.indexFormat))
        dkCmdBufBindIdxBuffer(R.cmd, state.indexFormat, state.index->mem.gpu + state.indexOffset);
      dkCmdBufDrawIndexed(R.cmd, p->topology, count, instances, first, base, firstInstance);
    } else
      dkCmdBufDraw(R.cmd, p->topology, count, instances, first, firstInstance);
  };
  // deko3d 0.5.0 uses dstColorBlendFactor for destination alpha. Preserve both
  // equations by writing RGB first and alpha second when their factors differ.
  if ((p->color.blendEnableMask & 1) &&
      p->blend.dstColorBlendFactor != p->blend.dstAlphaBlendFactor && (p->write.masks & 15) == 15) {
    auto cw = p->write;
    dkColorWriteStateSetMask(&cw, 0, DkColorMask_RGB);
    dkCmdBufBindColorWriteState(R.cmd, &cw);
    issue();
    dkColorWriteStateSetMask(&cw, 0, DkColorMask_A);
    dkCmdBufBindColorWriteState(R.cmd, &cw);
    auto b = p->blend;
    b.dstColorBlendFactor = b.dstAlphaBlendFactor;
    dkCmdBufBindBlendStates(R.cmd, 0, &b, 1);
    auto d = p->depth;
    if (d.depthWriteEnable)
      d.depthCompareOp = DkCompareOp_Equal;
    d.depthWriteEnable = false;
    dkCmdBufBindDepthStencilState(R.cmd, &d);
    issue();
    state.splitAlphaDirty = true;
  } else {
    issue();
  }
#ifdef COS_DK_DEBUG_LIB
  probe_after_draw();
#endif
}
} // namespace kd

extern "C" {
WGPUCommandEncoder __wrap_wgpuDeviceCreateCommandEncoder(WGPUDevice d,
                                                         const WGPUCommandEncoderDescriptor *desc) {
  auto h = __real_wgpuDeviceCreateCommandEncoder(d, desc);
  kd::Lock l(kd::mutex);
  kd::encoders[h] = std::make_shared<kd::Commands>();
  kd::encoderRefs[h] = 1;
  return h;
}
void __wrap_wgpuCommandEncoderAddRef(WGPUCommandEncoder h) {
  kd::Lock l(kd::mutex);
  ++kd::encoderRefs[h];
  __real_wgpuCommandEncoderAddRef(h);
}
void __wrap_wgpuCommandEncoderRelease(WGPUCommandEncoder h) {
  kd::Lock l(kd::mutex);
  if (!--kd::encoderRefs[h]) {
    kd::encoders.erase(h);
    kd::encoderRefs.erase(h);
  }
  __real_wgpuCommandEncoderRelease(h);
}
WGPUCommandBuffer __wrap_wgpuCommandEncoderFinish(WGPUCommandEncoder h,
                                                  const WGPUCommandBufferDescriptor *desc) {
  auto b = __real_wgpuCommandEncoderFinish(h, desc);
  kd::Lock l(kd::mutex);
  kd::commands[b] = {kd::required(kd::encoders, h, __func__), 1};
  return b;
}
void __wrap_wgpuCommandBufferAddRef(WGPUCommandBuffer h) {
  kd::Lock l(kd::mutex);
  ++kd::required(kd::commands, h, __func__).second;
  __real_wgpuCommandBufferAddRef(h);
}
void __wrap_wgpuCommandBufferRelease(WGPUCommandBuffer h) {
  kd::Lock l(kd::mutex);
  auto it = kd::commands.find(h);
  if (it != kd::commands.end() && !--it->second.second)
    kd::commands.erase(it);
  __real_wgpuCommandBufferRelease(h);
}
WGPURenderPassEncoder
__wrap_wgpuCommandEncoderBeginRenderPass(WGPUCommandEncoder e,
                                         const WGPURenderPassDescriptor *desc) {
  auto p = __real_wgpuCommandEncoderBeginRenderPass(e, desc);
  kd::Lock l(kd::mutex);
  auto view = kd::keep(desc->colorAttachments[0].view);
  kd::passes[p] = {kd::required(kd::encoders, e, __func__), view, kd::str(desc->label)};
  auto depth = desc->depthStencilAttachment ? kd::keep(desc->depthStencilAttachment->view)
                                            : wgpu::TextureView{};
  auto color = desc->colorAttachments[0].clearValue;
  bool clear = desc->colorAttachments[0].loadOp == WGPULoadOp_Clear;
  bool clearDepth =
      desc->depthStencilAttachment && desc->depthStencilAttachment->depthLoadOp == WGPULoadOp_Clear;
  float z = desc->depthStencilAttachment ? desc->depthStencilAttachment->depthClearValue : 1;
  kd::record(e, [view, depth, color, clear, clearDepth, z] {
    kd::open();
    kd::barrier();
    kd::state = {};
    auto *v = kd::find_view(view.Get());
    auto *d = depth ? kd::find_view(depth.Get()) : nullptr;
    if (!v || !v->tex->valid)
      swdk::fatal("Invalid native render target");
    const DkImageView *targets[] = {&v->view};
    dkCmdBufBindRenderTargets(swdk::R.cmd, targets, 1, d ? &d->view : nullptr);
    swdk::set_view(0, 0, v->tex->width >> v->baseMip, v->tex->height >> v->baseMip);
    kd::state.target = view;
    kd::state.depthTarget = depth;
    kd::state.viewport = {
        0, 0, float(v->tex->width >> v->baseMip), float(v->tex->height >> v->baseMip), 0, 1};
    kd::state.scissor = {0, 0, v->tex->width >> v->baseMip, v->tex->height >> v->baseMip};
    if (clear) {
      const float c[] = {float(color.r), float(color.g), float(color.b), float(color.a)};
      dkCmdBufClearColor(swdk::R.cmd, 0, DkColorMask_RGBA, c);
    }
    if (clearDepth && d)
      dkCmdBufClearDepthStencil(swdk::R.cmd, true, z, 0, 0);
  });
  return p;
}
void __wrap_wgpuRenderPassEncoderEnd(WGPURenderPassEncoder p) {
  kd::Lock l(kd::mutex);
  const auto &pass = kd::required(kd::passes, p, __func__);
  auto target = pass.target;
  auto label = pass.label;
  kd::record_pass(p, [target, label] {
    kd::barrier();
    if (swdk::capture_frame()) {
      auto *v = kd::find_view(target.Get());
      if (v && v->tex->valid) {
        swdk::dklog("pixel pass: %s target=%s draws=%llu", label.c_str(), v->tex->label.c_str(),
                    (unsigned long long)kd::state.draws);
        swdk::capture_image(v->view, std::max(1u, v->tex->width >> v->baseMip),
                            std::max(1u, v->tex->height >> v->baseMip), v->tex->format,
                            label.c_str());
      }
      if (auto pipeline = kd::state.pipeline) {
        for (const auto *stage : {&pipeline->vs, &pipeline->fs}) {
          for (const auto &slot : stage->slots) {
            if (slot.kind != swdk::SlotBinding::Kind::Sampler)
              continue;
            auto *b = kd::binding(kd::state.groups[slot.group], slot.binding);
            auto *source = b ? kd::find_view(b->view.Get()) : nullptr;
            swdk::dklog("pixel input: %s binding=%u:%u native_slot=%u texture=%s valid=%d",
                        label.c_str(), slot.group, slot.binding, slot.slot,
                        source ? source->tex->label.c_str() : "MISSING",
                        source && source->tex->valid);
            if (source && source->tex->valid) {
              auto sourceLabel = std::string("input ") + source->tex->label;
              swdk::capture_image(source->view, std::max(1u, source->tex->width >> source->baseMip),
                                  std::max(1u, source->tex->height >> source->baseMip),
                                  source->tex->format, sourceLabel.c_str());
            }
          }
        }
      }
    }
  });
  __real_wgpuRenderPassEncoderEnd(p);
  kd::passes.erase(p);
}
void __wrap_wgpuRenderPassEncoderSetPipeline(WGPURenderPassEncoder p, WGPURenderPipeline h) {
  kd::Lock l(kd::mutex);
  auto pipe = kd::required(kd::pipelines, h, __func__);
  kd::record_pass(p, [pipe] { kd::state.pipeline = pipe; });
}
void __wrap_wgpuRenderPassEncoderSetBindGroup(WGPURenderPassEncoder p, uint32_t i, WGPUBindGroup h,
                                              size_t n, const uint32_t *offsets) {
  kd::Lock l(kd::mutex);
  auto g = h ? kd::required(kd::groups, h, __func__) : nullptr;
  std::array<uint32_t, 8> o{};
  if (n > o.size())
    swdk::fatal("Too many dynamic offsets");
  if (n)
    std::copy_n(offsets, n, o.begin());
  kd::record_pass(p, [i, g, o] { kd::state.groups.at(i) = {g, o}; });
}
void __wrap_wgpuRenderPassEncoderSetViewport(WGPURenderPassEncoder p, float x, float y, float w,
                                             float h, float near, float far) {
  kd::record_pass(p, [=] {
    DkViewport v{x, y, w, h, near, far};
    kd::state.viewport = v;
    dkCmdBufSetViewports(swdk::R.cmd, 0, &v, 1);
  });
}
void __wrap_wgpuRenderPassEncoderSetScissorRect(WGPURenderPassEncoder p, uint32_t x, uint32_t y,
                                                uint32_t w, uint32_t h) {
  kd::record_pass(p, [=] {
    DkScissor s{x, y, w, h};
    kd::state.scissor = s;
    dkCmdBufSetScissors(swdk::R.cmd, 0, &s, 1);
  });
}
void __wrap_wgpuRenderPassEncoderSetBlendConstant(WGPURenderPassEncoder p, const WGPUColor *color) {
  auto c = *color;
  kd::record_pass(p, [=] { dkCmdBufSetBlendConst(swdk::R.cmd, c.r, c.g, c.b, c.a); });
}
void __wrap_wgpuRenderPassEncoderSetIndexBuffer(WGPURenderPassEncoder p, WGPUBuffer h,
                                                WGPUIndexFormat fmt, uint64_t off, uint64_t size) {
  kd::Lock l(kd::mutex);
  auto b = kd::required(kd::buffers, h, __func__);
  kd::record_pass(p, [=] {
    kd::state.index = b;
    kd::state.indexOffset = off;
    kd::state.indexFormat = fmt == WGPUIndexFormat_Uint32 ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16;
  });
}
void __wrap_wgpuRenderPassEncoderSetVertexBuffer(WGPURenderPassEncoder p, uint32_t slot,
                                                 WGPUBuffer h, uint64_t off, uint64_t size) {
  kd::Lock l(kd::mutex);
  auto b = kd::required(kd::buffers, h, __func__);
  if (off > b->size)
    swdk::fatal("Vertex buffer offset out of bounds");
  if (size == WGPU_WHOLE_SIZE)
    size = b->size - off;
  if (size > UINT32_MAX || size > b->size - off)
    swdk::fatal("Invalid vertex buffer size");
  kd::record_pass(p, [=] {
    DkBufExtents e{b->mem.gpu + off, uint32_t(size)};
    dkCmdBufBindVtxBuffers(swdk::R.cmd, slot, &e, 1);
  });
}
void __wrap_wgpuRenderPassEncoderDraw(WGPURenderPassEncoder p, uint32_t count, uint32_t instances,
                                      uint32_t first, uint32_t firstInstance) {
  kd::record_pass(p, [=] { kd::draw(false, count, instances, first, 0, firstInstance); });
}
void __wrap_wgpuRenderPassEncoderDrawIndexed(WGPURenderPassEncoder p, uint32_t count,
                                             uint32_t instances, uint32_t first, int32_t base,
                                             uint32_t firstInstance) {
  kd::record_pass(p, [=] { kd::draw(true, count, instances, first, base, firstInstance); });
}
void __wrap_wgpuQueueSubmit(WGPUQueue q, size_t n, const WGPUCommandBuffer *b) {
  kd::Lock l(kd::mutex);
  kd::open();
  for (size_t i = 0; i < n; ++i)
    kd::required(kd::commands, b[i], __func__).first->run();
  swdk::submit("Aurora queue submit");
  kd::retire_uploads();
  __real_wgpuQueueSubmit(q, n, b);
}

void __wrap_wgpuCommandEncoderCopyBufferToBuffer(WGPUCommandEncoder e, WGPUBuffer src, uint64_t so,
                                                 WGPUBuffer dst, uint64_t off, uint64_t n) {
  kd::Lock l(kd::mutex);
  auto s = kd::required(kd::buffers, src, __func__), d = kd::required(kd::buffers, dst, __func__);
  // Pin the mapped generation, not a CPU copy. prepare_map() cannot hand this
  // memory to the producer again before this command and its GPU fence finish.
  auto source = s->active;
  kd::record(e, [=] {
    kd::open();
    kd::barrier();
    dkCmdBufCopyBuffer(swdk::R.cmd, source->mem.gpu + so, d->mem.gpu + off, n);
    kd::barrier();
    source->used();
  });
}
void __wrap_wgpuCommandEncoderCopyTextureToTexture(WGPUCommandEncoder e,
                                                   const WGPUTexelCopyTextureInfo *src,
                                                   const WGPUTexelCopyTextureInfo *dst,
                                                   const WGPUExtent3D *size) {
  auto sh = kd::keep(src->texture), dh = kd::keep(dst->texture);
  auto s = *src, d = *dst;
  auto n = *size;
  kd::record(e, [=] {
    kd::open();
    kd::barrier();
    auto *st = kd::find_texture(sh.Get());
    auto *dt = kd::find_texture(dh.Get());
    auto sv = kd::level_view(st, s.mipLevel), dv = kd::level_view(dt, d.mipLevel);
    auto sx = s.origin.x, sy = s.origin.y, dx = d.origin.x, dy = d.origin.y, w = n.width,
         h = n.height;
    if (st->compressed) {
      w = (w + st->blockW - 1) / st->blockW;
      h = (h + st->blockH - 1) / st->blockH;
      sx /= st->blockW;
      sy /= st->blockH;
      dx /= dt->blockW;
      dy /= dt->blockH;
    }
    DkImageRect sr{sx, sy, s.origin.z, w, h, n.depthOrArrayLayers},
        dr{dx, dy, d.origin.z, w, h, n.depthOrArrayLayers};
    dkCmdBufCopyImage(swdk::R.cmd, &sv, &sr, &dv, &dr, 0);
    kd::barrier();
  });
}
void __wrap_wgpuCommandEncoderCopyBufferToTexture(WGPUCommandEncoder e,
                                                  const WGPUTexelCopyBufferInfo *src,
                                                  const WGPUTexelCopyTextureInfo *dst,
                                                  const WGPUExtent3D *size) {
  kd::Lock l(kd::mutex);
  auto s = kd::required(kd::buffers, src->buffer, __func__)->active;
  auto t = kd::keep(dst->texture);
  auto layout = src->layout;
  auto d = *dst;
  auto n = *size;
  kd::record(e, [=] {
    kd::open();
    kd::barrier();
    auto *dt = kd::find_texture(t.Get());
    auto dv = kd::level_view(dt, d.mipLevel);
    uint32_t row = layout.bytesPerRow == WGPU_COPY_STRIDE_UNDEFINED
                       ? ((n.width + dt->blockW - 1) / dt->blockW) * dt->blockBytes
                       : layout.bytesPerRow;
    uint32_t rows = layout.rowsPerImage == WGPU_COPY_STRIDE_UNDEFINED
                        ? (n.height + dt->blockH - 1) / dt->blockH
                        : layout.rowsPerImage;
    DkCopyBuf b{s->mem.gpu + layout.offset, row, row * rows};
    DkImageRect r{d.origin.x, d.origin.y, d.origin.z, n.width, n.height, n.depthOrArrayLayers};
    dkCmdBufCopyBufferToImage(swdk::R.cmd, &b, &dv, &r, 0);
    kd::barrier();
    s->used();
  });
}
void __wrap_wgpuCommandEncoderCopyTextureToBuffer(WGPUCommandEncoder e,
                                                  const WGPUTexelCopyTextureInfo *src,
                                                  const WGPUTexelCopyBufferInfo *dst,
                                                  const WGPUExtent3D *size) {
  kd::Lock l(kd::mutex);
  auto d = kd::required(kd::buffers, dst->buffer, __func__);
  auto t = kd::keep(src->texture);
  auto layout = dst->layout;
  auto s = *src;
  auto n = *size;
  kd::record(e, [=] {
    kd::open();
    kd::barrier();
    auto *st = kd::find_texture(t.Get());
    auto sv = kd::level_view(st, s.mipLevel);
    uint32_t row = layout.bytesPerRow == WGPU_COPY_STRIDE_UNDEFINED ? n.width * st->blockBytes
                                                                    : layout.bytesPerRow;
    uint32_t rows =
        layout.rowsPerImage == WGPU_COPY_STRIDE_UNDEFINED ? n.height : layout.rowsPerImage;
    DkCopyBuf b{d->mem.gpu + layout.offset, row, row * rows};
    DkImageRect r{s.origin.x, s.origin.y, s.origin.z, n.width, n.height, n.depthOrArrayLayers};
    dkCmdBufCopyImageToBuffer(swdk::R.cmd, &sv, &r, &b, 0);
    kd::barrier();
  });
}
void __wrap_wgpuCommandEncoderClearBuffer(WGPUCommandEncoder e, WGPUBuffer b, uint64_t off,
                                          uint64_t n) {
  kd::Lock l(kd::mutex);
  auto d = kd::required(kd::buffers, b, __func__);
  if (n == WGPU_WHOLE_SIZE)
    n = d->size - off;
  kd::record(e, [=] {
    kd::open();
    kd::barrier();
    std::vector<uint8_t> zero(n);
    auto temp = swdk::block_create(n, "buffer clear");
    memcpy(temp.cpu, zero.data(), n);
    dkCmdBufCopyBuffer(swdk::R.cmd, temp.gpu, d->mem.gpu + off, n);
    swdk::block_free_deferred(temp.block);
    kd::barrier();
  });
}

// Queue writes execute before subsequent command buffers, under the same lock.
void __wrap_wgpuQueueWriteBuffer(WGPUQueue q, WGPUBuffer h, uint64_t off, const void *data,
                                 size_t n) {
  kd::Lock l(kd::mutex);
  auto b = kd::required(kd::buffers, h, __func__);
  kd::open();
  kd::barrier();
  auto temp = kd::upload_allocation(n);
  memcpy(temp->mem.cpu, data, n);
  dkCmdBufCopyBuffer(swdk::R.cmd, temp->mem.gpu, b->mem.gpu + off, n);
  temp->used();
  kd::barrier();
}
void __wrap_wgpuQueueWriteTexture(WGPUQueue q, const WGPUTexelCopyTextureInfo *dst,
                                  const void *data, size_t bytes,
                                  const WGPUTexelCopyBufferLayout *layout,
                                  const WGPUExtent3D *size) {
  kd::Lock l(kd::mutex);
  kd::open();
  kd::barrier();
  auto *t = kd::find_texture(dst->texture);
  auto v = kd::level_view(t, dst->mipLevel);
  uint32_t row = ((size->width + t->blockW - 1) / t->blockW) * t->blockBytes,
           rows = (size->height + t->blockH - 1) / t->blockH, pitch = (row + 255) & ~255u;
  uint32_t inPitch = layout->bytesPerRow == WGPU_COPY_STRIDE_UNDEFINED ? row : layout->bytesPerRow;
  uint32_t inRows =
      layout->rowsPerImage == WGPU_COPY_STRIDE_UNDEFINED ? rows : layout->rowsPerImage;
  auto temp = kd::upload_allocation(uint64_t(pitch) * rows * size->depthOrArrayLayers);
  for (uint32_t z = 0; z < size->depthOrArrayLayers; ++z)
    for (uint32_t y = 0; y < rows; ++y)
      memcpy(temp->mem.cpu + (z * rows + y) * pitch,
             static_cast<const uint8_t *>(data) + layout->offset + (z * inRows + y) * inPitch, row);
  DkCopyBuf b{temp->mem.gpu, pitch, pitch * rows};
  DkImageRect r{dst->origin.x, dst->origin.y, dst->origin.z,
                size->width,   size->height,  size->depthOrArrayLayers};
  dkCmdBufCopyBufferToImage(swdk::R.cmd, &b, &v, &r, 0);
  temp->used();
  kd::barrier();
}
}
