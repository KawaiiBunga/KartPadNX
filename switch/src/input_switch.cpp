// Switch controllers (libnx HID) as the game's four Classic Controllers (kartpad_mobile_runtime_host.h).
// Player 1 is HID player No1 or the handheld console, players 2-4 are No2-No4. The system controller
// applet (KartPadShowControllerApplet) assigns controllers to players: at start when player 1 has
// none, and from the port menu (Controller settings). Joy-Cons count as one controller as a pair or
// each on its own held sideways (SL and SR on top).
//
// Reference for the target bit layout and stick scaling:
//   ref/kartpad-main/runtime/src/android/controller_mapping.cpp
//
#include "kartpad_mobile_runtime_host.h"
#include "kartpad/android/gamepad_contract.h"
#include "perf_switch.h"

#include <switch.h>

#include <cstdio>

namespace {

// Consume the shared Classic Controller ABI rather than duplicating bit values.
namespace cc = kartpad::android;

constexpr unsigned kPlayers = 4;
PadState g_pads[kPlayers]{};
u32 g_styles[kPlayers]{};
bool g_padsReady = false;
bool g_configuredForGame = false;

// Four players, every controller style the applet can assign, Joy-Cons held sideways when alone.
// SDL's joystick start-up configures one player, so the game's first input read configures again.
void ConfigureHid() {
  padConfigureInput(kPlayers, HidNpadStyleSet_NpadStandard);
  hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
}

void EnsureReady() {
  if (!g_padsReady) {
    padInitializeDefault(&g_pads[0]);
    padInitialize(&g_pads[1], HidNpadIdType_No2);
    padInitialize(&g_pads[2], HidNpadIdType_No3);
    padInitialize(&g_pads[3], HidNpadIdType_No4);
    g_padsReady = true;
  }
  if (!g_configuredForGame) {
    ConfigureHid();
    g_configuredForGame = true;
  }
}

float NormalizeStick(std::int32_t raw) {
  const float v = static_cast<float>(raw) / 32767.0f;
  return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

std::uint32_t MapButtons(std::uint64_t nx) {
  std::uint32_t out = 0;
  if (nx & HidNpadButton_A) out |= cc::kClassicA;
  if (nx & HidNpadButton_B) out |= cc::kClassicB;
  if (nx & HidNpadButton_X) out |= cc::kClassicX;
  if (nx & HidNpadButton_Y) out |= cc::kClassicY;
  if (nx & HidNpadButton_L) out |= cc::kClassicL;
  if (nx & HidNpadButton_R) out |= cc::kClassicR;
  if (nx & HidNpadButton_ZL) out |= cc::kClassicZl;
  if (nx & HidNpadButton_ZR) out |= cc::kClassicZr;
  if (nx & HidNpadButton_Plus) out |= cc::kClassicPlus;
  if (nx & HidNpadButton_Minus) out |= cc::kClassicMinus;
  if (nx & HidNpadButton_Up) out |= cc::kClassicUp;
  if (nx & HidNpadButton_Down) out |= cc::kClassicDown;
  if (nx & HidNpadButton_Left) out |= cc::kClassicLeft;
  if (nx & HidNpadButton_Right) out |= cc::kClassicRight;
  return out;
}

// One Joy-Con held sideways: its four face buttons by position (right A, bottom B, top X, left Y),
// SL and SR as the shoulders (L items, R drift), its + or - as + (pause).
std::uint32_t MapSideways(std::uint64_t nx, bool left) {
  struct Face {
    std::uint64_t right, bottom, top, leftmost;
  };
  // the left Joy-Con turned with its rail up: Down is on the right; the right Joy-Con: X is
  const Face f = left ? Face{HidNpadButton_Down, HidNpadButton_Left, HidNpadButton_Right, HidNpadButton_Up}
                      : Face{HidNpadButton_X, HidNpadButton_A, HidNpadButton_Y, HidNpadButton_B};
  std::uint32_t out = 0;
  if (nx & f.right) out |= cc::kClassicA;
  if (nx & f.bottom) out |= cc::kClassicB;
  if (nx & f.top) out |= cc::kClassicX;
  if (nx & f.leftmost) out |= cc::kClassicY;
  if (nx & HidNpadButton_AnySL) out |= cc::kClassicL;
  if (nx & HidNpadButton_AnySR) out |= cc::kClassicR;
  if (nx & (HidNpadButton_Plus | HidNpadButton_Minus)) out |= cc::kClassicPlus;
  return out;
}

const char* StyleName(u32 style) {
  if (style & HidNpadStyleTag_NpadHandheld) return "handheld";
  if (style & HidNpadStyleTag_NpadFullKey) return "Pro Controller";
  if (style & HidNpadStyleTag_NpadJoyDual) return "Joy-Con pair";
  if (style & HidNpadStyleTag_NpadJoyLeft) return "left Joy-Con sideways";
  if (style & HidNpadStyleTag_NpadJoyRight) return "right Joy-Con sideways";
  return style ? "other" : "none";
}

}  // namespace

extern "C" {

bool KartPadMobileIsControllerConnected(unsigned int player) {
  EnsureReady();
  if (player >= kPlayers) return false;
  padUpdate(&g_pads[player]);
  return padIsConnected(&g_pads[player]);
}

bool KartPadMobileReadClassicInputForPlayer(
    unsigned int player, KartPadMobileClassicInputSnapshot* snapshot) {
  if (snapshot == nullptr) return false;
  EnsureReady();
  *snapshot = {};
  if (player >= kPlayers) return false;
  PadState& pad = g_pads[player];
  padUpdate(&pad);
  if (!padIsConnected(&pad)) return false;
  const u32 style = padGetStyleSet(&pad);
  if (style != g_styles[player]) {
    std::fprintf(stderr, "[input] player %u: %s\n", player + 1, StyleName(style));
    g_styles[player] = style;
  }
  const bool single = !(style & (HidNpadStyleTag_NpadHandheld | HidNpadStyleTag_NpadFullKey |
                                 HidNpadStyleTag_NpadJoyDual)) &&
                      (style & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight));
  const bool leftJoyCon = style & HidNpadStyleTag_NpadJoyLeft;
  const std::uint64_t held = padGetButtons(&pad);
  // A right Joy-Con alone reports its stick as the right stick.
  HidAnalogStickState stick = padGetStickPos(&pad, single && !leftJoyCon ? 1 : 0);
  // HID turns a sideways Joy-Con's stick with the horizontal hold type but reports its buttons as
  // they are. Not checked on hardware yet: perf.cfg `joycon_rotate_stick 1` turns the stick here as
  // well, `joycon_sideways_buttons 0` passes the buttons through unmapped.
  static const bool rotate = kartpad_perf_int("joycon_rotate_stick", 0) != 0;
  static const bool sideways = kartpad_perf_int("joycon_sideways_buttons", 1) != 0;
  if (single && rotate) {
    const std::int32_t x = stick.x, y = stick.y;
    stick.x = leftJoyCon ? -y : y;
    stick.y = leftJoyCon ? x : -x;
  }
  snapshot->buttons = single && sideways ? MapSideways(held, leftJoyCon) : MapButtons(held);
  snapshot->leftStickX = NormalizeStick(stick.x);
  snapshot->leftStickY = NormalizeStick(stick.y);
  snapshot->connected = 1;
  return true;
}

// The system controller applet for players 1-4 (pairs, sideways Joy-Cons, Pro Controllers). Blocks
// until it closes. From the port menu, or at start when player 1 has no controller.
bool KartPadShowControllerApplet(void) {
  EnsureReady();
  HidLaControllerSupportArg arg;
  hidLaCreateControllerSupportArg(&arg);
  arg.hdr.player_count_min = 1;
  arg.hdr.player_count_max = kPlayers;
  HidLaControllerSupportResultInfo info{};
  const Result rc = hidLaShowControllerSupport(&info, &arg);
  std::fprintf(stderr, "[input] controller applet: rc=0x%x, %d player(s)\n", rc, int(info.player_count));
  ConfigureHid();
  return R_SUCCEEDED(rc);
}

// At start: the applet when player 1 has no controller (Joy-Cons detached and not yet assigned).
void KartPadEnsureControllerAtStart(void) {
  EnsureReady();
  // HID reports what is connected a few frames after the configuration
  bool connected = false;
  for (int i = 0; i < 30 && !connected; ++i) {
    padUpdate(&g_pads[0]);
    connected = padIsConnected(&g_pads[0]);
    if (!connected) svcSleepThread(16'000'000);
  }
  if (!connected) KartPadShowControllerApplet();
  g_configuredForGame = false;  // SDL starts later and configures one player: configure again then
}

}  // extern "C"
