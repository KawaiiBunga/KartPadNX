// Switch WUP-028 (official GameCube controller adapter) stub.
//
// The Windows build talks to the adapter over WinUSB. The Switch build does not
// implement USB host support (libnx usbHs), so this reports "no adapter
// present" for every call, exactly like the Apple port's
// src/apple/wup028_adapter_stub.cpp. Aurora's normal HID controller path is
// unaffected. Replace with a libnx usbHs implementation if native GC-adapter
// support is wanted later.
#include "wup028_adapter.h"

#include <dolphin/pad.h>

namespace Wup028Adapter {

void Initialize() {}
void Shutdown() {}
AdapterInfo GetInfo() { return {}; }

bool Read(std::array<PADStatus, 4>& statuses) {
  for (PADStatus& status : statuses) status = {};
  return false;
}

void SetPortAssignment(std::uint32_t, int) {}
int GetPortAssignment(std::uint32_t) { return -1; }
bool SetRumble(std::uint32_t, bool) { return false; }

}  // namespace Wup028Adapter
