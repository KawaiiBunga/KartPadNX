// Switch (libnx) implementation of kartpad::platform host services.
// Mirrors runtime/src/platform/host_*_windows.cpp for aarch64-none-elf.
//
// Roots:
//   romfs:/                    read-only app data packed into the NRO
//   sdmc:/switch/kartpad/      writable game data, extracted assets, saves
//
// Build defines MKW_HOST_SWITCH. The generic host_files.cpp AtomicWriteFile is
// POSIX/std::filesystem based and works on newlib+libnx, so it is reused; only
// paths + clock are provided here.

#include "kartpad/platform/host_services.h"

#include <switch.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace kartpad::platform {
namespace {

constexpr const char* kSdRoot = "sdmc:/switch/kartpad";

void ValidateIdentifier(const std::string_view identifier) {
  if (identifier.empty() || identifier.find('/') != std::string_view::npos ||
      identifier.find('\\') != std::string_view::npos || identifier == "." ||
      identifier == "..") {
    throw std::invalid_argument(
        "application identifier must be one non-empty path component");
  }
}

}  // namespace

HostPaths ResolveHostPaths(const std::string_view application_identifier) {
  ValidateIdentifier(application_identifier);
  const std::filesystem::path root{kSdRoot};
  const std::filesystem::path component{application_identifier};
  return {
      .app_support = root / component / "data",
      .cache = root / component / "cache",
      .temporary = root / component / "tmp",
  };
}

void EnsureHostDirectories(const HostPaths& paths) {
  std::filesystem::create_directories(paths.app_support);
  std::filesystem::create_directories(paths.cache);
  std::filesystem::create_directories(paths.temporary);
}

std::uint64_t MonotonicNanoseconds() noexcept {
  // libnx exposes the CPU tick counter; convert to ns. steady_clock also works
  // under newlib, but armGetSystemTick avoids any libc clock ambiguity.
  return armTicksToNs(armGetSystemTick());
}

void SleepUntil(const std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (deadline <= now) return;
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now)
          .count();
  svcSleepThread(static_cast<std::int64_t>(ns));
}

bool SetCurrentThreadName(const std::string_view) noexcept {
  // libnx threads do not carry a settable POSIX name.
  return false;
}

std::string CurrentThreadName() { return {}; }

}  // namespace kartpad::platform
