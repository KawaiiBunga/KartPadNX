// Tracy is disabled on Switch. Aurora still calls its thread-name helpers.
#include <common/TracySystem.hpp>
#include <switch.h>
#include <cstdlib>
#ifdef TRACY_ENABLE
#error "Switch profiling requires a full Tracy platform port"
#endif
namespace tracy {
namespace detail {
uint32_t GetThreadHandleImpl() {
  u64 id = 0;
  svcGetThreadId(&id, CUR_THREAD_HANDLE);
  return static_cast<uint32_t>(id);
}
}
void SetThreadName(const char*) {}
void SetThreadNameWithHint(const char*, int32_t) {}
const char* GetThreadName(uint32_t) { return "Switch thread"; }
const char* GetEnvVar(const char* name) { return std::getenv(name); }
const char* GetUserLogin() { return "Switch"; }
const char* GetUserFullName() { return "Switch"; }
}
