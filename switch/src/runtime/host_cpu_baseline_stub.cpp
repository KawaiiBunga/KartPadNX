// Switch host-CPU baseline stub.
//
// The Windows build's host_cpu_baseline.cpp runs an x86 CPUID check so an
// unsupported machine gets a readable error instead of an illegal-instruction
// crash. On Switch the CPU is fixed (Tegra X1, Cortex-A57 aarch64) and the whole
// runtime is compiled for it, so there is nothing to probe. Mirrors the Apple
// port's src/apple/host_cpu_baseline_stub.cpp.
namespace {
[[maybe_unused]] const int kKartPadSwitchAarch64Baseline = 1;
}
