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

// Switch: the controller mapping the port menu edits (input_switch.cpp). For each player and
// controller style, every physical input (source) sends one Classic Controller button (target;
// target 0 sends nothing). Indices count from 0; the names live as long as the program.
enum KartPadInputStyle {
  KARTPAD_INPUT_PRO = 0,
  KARTPAD_INPUT_JOYCON_PAIR = 1,
  KARTPAD_INPUT_JOYCON_SIDEWAYS = 2,  // one Joy-Con alone, held with SL and SR on top
  KARTPAD_INPUT_HANDHELD = 3,
  KARTPAD_INPUT_STYLE_COUNT = 4,
};
const char *KartPadInputStyleName(int style);
int KartPadInputSourceCount(int style);
const char *KartPadInputSourceName(int style, int source);
int KartPadInputTargetCount(void);
const char *KartPadInputTargetName(int target);     // "A"
const char *KartPadInputTargetUse(int target);      // what it does in a race ("accelerate"), or ""
unsigned int KartPadInputTargetButton(int target);  // its Classic Controller bit (gamepad_contract.h)
int KartPadInputGetTarget(unsigned int player, int style, int source);
void KartPadInputSetTarget(unsigned int player, int style, int source, int target);
// The stick that steers: 0 left, 1 right; -1 for one Joy-Con, which has one stick.
int KartPadInputGetSteeringStick(unsigned int player, int style);
void KartPadInputSetSteeringStick(unsigned int player, int style, int stick);
void KartPadInputResetMapping(unsigned int player, int style);
void KartPadInputCopyMappingToAllPlayers(unsigned int player, int style);
// The controller player N holds now: its style (-1 when none) and a name ("left Joy-Con, sideways";
// null when none).
int KartPadInputPlayerStyle(unsigned int player);
const char *KartPadInputPlayerController(unsigned int player);
// Bit N is set while source N of the style is held on player N's controller.
unsigned long long KartPadInputHeldSources(unsigned int player, int style);
// Player N's Joy-Con pair becomes two players: the left Joy-Con stays, the right one becomes the
// returned player (-1: not a pair, or no free player).
int KartPadInputSplitJoyCons(unsigned int player);
// Player N's lone Joy-Con and another player's lone Joy-Con of the other side become one pair on
// player N; returns the other player (-1: no such Joy-Con, or HID refused).
int KartPadInputJoinJoyCons(unsigned int player);

}
