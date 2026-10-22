#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum kartpad_prof_state {
  KARTPAD_PROF_UNAVAILABLE,
  KARTPAD_PROF_IDLE,
  KARTPAD_PROF_STARTING,
  KARTPAD_PROF_RECORDING,
  KARTPAD_PROF_STOPPING,
  KARTPAD_PROF_SAVED,
  KARTPAD_PROF_ERROR
};

struct kartpad_prof_status {
  int state;
  uint32_t hz;
  uint32_t elapsed_seconds;
  uint64_t samples;
  uint64_t dropped;
};

// Requests are asynchronous; SAVED means the final file flush has completed.
int kartpad_prof_set_enabled(int enabled);
void kartpad_prof_get_status(struct kartpad_prof_status* status);

#ifdef __cplusplus
}
#endif
