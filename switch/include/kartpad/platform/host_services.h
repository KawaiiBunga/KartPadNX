#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace kartpad::platform {

struct HostPaths {
  std::filesystem::path app_support;
  std::filesystem::path cache;
  std::filesystem::path temporary;
};

[[nodiscard]] std::uint64_t MonotonicNanoseconds() noexcept;
void SleepUntil(std::chrono::steady_clock::time_point deadline);
[[nodiscard]] bool SetCurrentThreadName(std::string_view name) noexcept;
[[nodiscard]] std::string CurrentThreadName();

[[nodiscard]] HostPaths ResolveHostPaths(std::string_view application_identifier);
void EnsureHostDirectories(const HostPaths& paths);

// Replaces the destination atomically on success. A failed write must leave an
// existing destination unchanged and remove its temporary sibling.
void AtomicWriteFile(const std::filesystem::path& destination, std::string_view contents);

}  // namespace kartpad::platform
