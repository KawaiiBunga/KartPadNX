#!/usr/bin/env python3
"""Idempotent native graphics seams; preserves the staged Wii runtime and GX implementation."""
from pathlib import Path
import argparse
import re
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--aurora',type=Path,default=root/'build/wiicompiled-fpscr/aurora-main')
parser.add_argument('--runtime-main',type=Path,default=root/'build/wiicompiled-fpscr/runtime/src/main.cpp')
args=parser.parse_args()
aurora=args.aurora
def edit(name,old,new):
    p=aurora/name
    s=p.read_text()
    if new in s:return
    if old not in s:raise SystemExit(f'Missing context {name}: {old[:90]}')
    p.write_text(s.replace(old,new,1),newline='\n')
edit('lib/webgpu/gpu.cpp','#include "gpu.hpp"','#include "gpu.hpp"\n#if defined(AURORA_GFX_DEKO3D)\n#include "dk_aurora.h"\n#endif')
edit('lib/webgpu/gpu.cpp','static bool create_surface() {','static bool create_surface() {\n#if defined(AURORA_GFX_DEKO3D)\n  return true;\n#endif')
edit('lib/webgpu/gpu.cpp','  const wgpu::BackendType backend = to_wgpu_backend(auroraBackend);','#if defined(AURORA_GFX_DEKO3D)\n  const wgpu::BackendType backend = wgpu::BackendType::Null; // deko3d owns the GPU\n#else\n  const wgpu::BackendType backend = to_wgpu_backend(auroraBackend);\n#endif')
edit('lib/webgpu/gpu.cpp','  const wgpu::Status status = g_surface.GetCapabilities(g_adapter, &g_surfaceCapabilities);','''#if defined(AURORA_GFX_DEKO3D)
  aurora_switch_dk_init(19);
  const auto surfaceFormat = wgpu::TextureFormat::RGBA8Unorm;
  const auto presentMode = wgpu::PresentMode::Fifo;
#else
  const wgpu::Status status = g_surface.GetCapabilities(g_adapter, &g_surfaceCapabilities);''')
edit('lib/webgpu/gpu.cpp','  auto presentMode = best_present_mode();','  auto presentMode = best_present_mode();\n#endif')
edit('lib/webgpu/gpu.cpp','  if (!g_surface || !g_device || width == 0 || height == 0 || native_height == 0 || native_width == 0) {','''#if defined(AURORA_GFX_DEKO3D)
  if (!g_device || width == 0 || height == 0 || native_height == 0 || native_width == 0) {
#else
  if (!g_surface || !g_device || width == 0 || height == 0 || native_height == 0 || native_width == 0) {
#endif''')
edit('lib/webgpu/gpu.cpp','  g_surface.Configure(&surfaceConfiguration);','#if !defined(AURORA_GFX_DEKO3D)\n  g_surface.Configure(&surfaceConfiguration);\n#endif')
edit('lib/webgpu/gpu.cpp','void shutdown() {','void shutdown() {\n#if defined(AURORA_GFX_DEKO3D)\n  aurora_switch_dk_shutdown();\n#endif')
edit('lib/aurora.cpp','#include <aurora/aurora.h>','#include <aurora/aurora.h>\n#if defined(AURORA_GFX_DEKO3D)\n#include "kartpad_backend.h"\n#endif')
edit('lib/aurora.cpp','  if (webgpu::g_backendType == wgpu::BackendType::Null) {','''#if defined(AURORA_GFX_DEKO3D)
  if (false) { // deko3d draws Null-device frames
#else
  if (webgpu::g_backendType == wgpu::BackendType::Null) {
#endif''')
edit('lib/aurora.cpp','bool present_presentation_job(const PresentationJob& job) {','''bool present_presentation_job(const PresentationJob& job) {
#if defined(AURORA_GFX_DEKO3D)
  if (job.presentAt != PresentClock::time_point{}) wait_until_precise(job.presentAt);
  kartpad_deko_present(job.image->texture.texture.Get(),job.image->texture.size.width,job.image->texture.size.height);
  record_successful_present(job.interpolated,job.logicalFrame,{},{},{},{},{},{},job.duplicated,job.slidPeriods,{});
  return true;
#endif''')
edit('lib/aurora.cpp','  const bool surfaceReconfigurePending =','''#if defined(AURORA_GFX_DEKO3D)
  if (window::is_paused() || !window::is_presentable()) return false;
  if (window::native_resize_pending() || g_surfaceReconfigurePending.load(std::memory_order_acquire)) {
    wait_for_presenter_idle();
    window::SurfaceLock surfaceLock;
    std::lock_guard gpuLock(g_rendererGpuMutex);
    window::sync_frame_buffer_size();
  }
  g_surfaceReconfigurePending.store(false,std::memory_order_release);
  return begin_frame_render_state_impl(imguiPolicy,imguiNewFrameOwed);
#endif
  const bool surfaceReconfigurePending =''')
edit('lib/gfx/depth_peek.cpp','#include "depth_peek.hpp"','#include "depth_peek.hpp"\n#include <cstring>\n#if defined(AURORA_GFX_DEKO3D)\n#include "kartpad_backend.h"\n#endif')
edit('lib/gfx/depth_peek.cpp','void initialize() {','void initialize() {\n#if defined(AURORA_GFX_DEKO3D)\n  return;\n#endif')
edit('lib/gfx/depth_peek.cpp','bool read_latest(uint16_t x, uint16_t y, uint32_t& z) noexcept {','''bool read_latest(uint16_t x, uint16_t y, uint32_t& z) noexcept {
#if defined(AURORA_GFX_DEKO3D)
  return kartpad_deko_depth_read(x,y,z);
#endif''')
edit('lib/gfx/depth_peek.cpp','  const Params params = make_params(sourceSize, dstSize);','''  const Params params = make_params(sourceSize, dstSize);
#if defined(AURORA_GFX_DEKO3D)
  static_assert(sizeof(params)==sizeof(KartPadDepthParams));
  KartPadDepthParams nativeParams; std::memcpy(&nativeParams,&params,sizeof(params));
  kartpad_deko_depth_snapshot(cmd.Get(),depthView.Get(),nativeParams,gx::UseReversedZ);
  return;
#endif''')
# Native caches are a separate namespace. The recipe seed and the boot prewarm stay on
# (docs/SHADER-PRECOMPILE-2026-10-19.md); perf.cfg `prewarm_max 0` turns prewarm off.
edit('lib/gfx/pipeline_cache.cpp',
     ' / "pipeline_cache.db");',
     ''' /
#if defined(AURORA_GFX_DEKO3D)
      "pipeline_cache_deko.db"
#else
      "pipeline_cache.db"
#endif
  );''')
edit('lib/gfx/pipeline_cache.cpp',
     '''                                          size_t maxEntries = std::numeric_limits<size_t>::max()) {
  if (!prepare_pipeline_cache_db()) {''',
     '''                                          size_t maxEntries = std::numeric_limits<size_t>::max()) {
  // A zero warmup budget performs no sqlite3_step. Binding a statement returns
  // SQLITE_OK, which must not be mistaken for a failed SQLITE_ROW/SQLITE_DONE.
  if (maxEntries == 0) return 0;
  if (!prepare_pipeline_cache_db()) {''')
edit('lib/webgpu/gpu_cache.cpp', ' / "dawn_cache.db";', ''' /
#if defined(AURORA_GFX_DEKO3D)
      "dawn_null_deko_cache.db";
#else
      "dawn_cache.db";
#endif''')
edit('lib/webgpu/gpu_cache_switch.cpp', ' / "dawn_cache.db");', ''' /
#if defined(AURORA_GFX_DEKO3D)
      "dawn_null_deko_cache.db"
#else
      "dawn_cache.db"
#endif
  );''')
# GX's performance-packed uniform header uses twelve contiguous u32 offsets.
# std140 scalar arrays would expand them to 192 bytes and shift every matrix.
edit('lib/gx/shader.cpp', '#include "shader_info.hpp"', '''#include "shader_info.hpp"
#if defined(AURORA_GFX_DEKO3D)
#include "gx_uniform_layout.hpp"
#endif''')
edit('lib/gx/shader.cpp',
     'auto normal_group_load(const ShaderConfig& config, u8 group, std::string_view vidx) -> std::string {',
     '''static constexpr std::string_view kArrayStartUniformType =
#if defined(AURORA_GFX_DEKO3D)
    kartpad::deko::GxArrayStartType;
#else
    "u32, 12";
#endif

static std::string uniform_array_start(u32 index) {
#if defined(AURORA_GFX_DEKO3D)
  return kartpad::deko::gx_array_start_access(index);
#else
  return fmt::format("ubuf.array_start[{}]", index);
#endif
}

auto normal_group_load(const ShaderConfig& config, u8 group, std::string_view vidx) -> std::string {''')
for old,new in [
    ('"ubuf.array_start[{}] + raw_fetch_u8_1(&{}, {}) * {}u + {}u", GX_VA_NRM - GX_VA_POS, buf,',
     '"{} + raw_fetch_u8_1(&{}, {}) * {}u + {}u", uniform_array_start(GX_VA_NRM - GX_VA_POS), buf,'),
    ('"ubuf.array_start[{}] + raw_fetch_u16_1(&{}, {}, {}) * {}u + {}u", GX_VA_NRM - GX_VA_POS,',
     '"{} + raw_fetch_u16_1(&{}, {}, {}) * {}u + {}u", uniform_array_start(GX_VA_NRM - GX_VA_POS),'),
    ('"ubuf.array_start[{}] + raw_fetch_u8_1(&{}, {}) * {}u", attr - GX_VA_POS, buf, offs,',
     '"{} + raw_fetch_u8_1(&{}, {}) * {}u", uniform_array_start(attr - GX_VA_POS), buf, offs,'),
    ('"ubuf.array_start[{}] + raw_fetch_u16_1(&{}, {}, {}) * {}u", attr - GX_VA_POS, buf, offs, le,',
     '"{} + raw_fetch_u16_1(&{}, {}, {}) * {}u", uniform_array_start(attr - GX_VA_POS), buf, offs, le,'),
    ('array_start: array<u32, 12>,{0}', 'array_start: array<{11}>,{0}'),
    ('fragmentReturn);', 'fragmentReturn, kArrayStartUniformType);'),
]:
    edit('lib/gx/shader.cpp',old,new)

symbols=set()
for p in sorted((root/'switch/deko').glob('backend_*.cpp'))+[root/'switch/deko/aurora/dk_objects.cpp']:
    symbols.update(re.findall(r'__wrap_(wgpu\w+)\(',p.read_text()))
(root/'switch/deko/wrap-symbols.txt').write_text('\n'.join(sorted(symbols))+'\n')
runtime=args.runtime_main
s=runtime.read_text()
old='const std::string backend = "vulkan";\n        auroraConfig.desiredBackend = BACKEND_VULKAN;'
new='const std::string backend = "deko3d";\n        auroraConfig.desiredBackend = BACKEND_NULL; // native deko3d with Dawn object handles'
if new not in s:
    if old not in s:raise SystemExit('Missing Switch backend selection in runtime main.cpp')
    runtime.write_text(s.replace(old,new,1),newline='\n')
