#pragma once

#include <cstdint>

struct KartPadMobileClassicInputSnapshot {
  std::uint32_t buttons = 0;
  float leftStickX = 0.0f;
  float leftStickY = 0.0f;
  int connected = 0;
};

extern "C" {

// Holds an Apple-mobile launch in its native setup/import flow until a complete,
// validated private RMCP01 data tree is available. Existing valid data returns
// directly.
bool KartPadMobileEnsureGameDataAvailable();

// Valid after KartPadMobileEnsureGameDataAvailable returns true. The launch
// chooser returns exactly "base" or "retro_rewind" for the linked dual runtime.
const char *KartPadMobileSelectedRuntimeProfile();

// Queries connection state without consuming latched button presses.
bool KartPadMobileIsControllerConnected(unsigned int player);

// Reads touch (where present) plus the first physical controller for player
// zero, or the independently assigned physical controller for players one
// through three.
bool KartPadMobileReadClassicInputForPlayer(
    unsigned int player, KartPadMobileClassicInputSnapshot *snapshot);

// Switch: the system controller applet for players 1-4 (pairs, sideways Joy-Cons, Pro
// Controllers); blocks until it closes. The port menu opens it.
bool KartPadShowControllerApplet(void);

// Switch, before the game starts: the controller applet when player 1 has no controller.
void KartPadEnsureControllerAtStart(void);

}
