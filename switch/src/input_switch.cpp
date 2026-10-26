// Switch controllers (libnx HID) as the game's four Classic Controllers (kartpad_mobile_runtime_host.h).
// Player N reads HID npad No.N; player 1 also reads the handheld console. The system controller
// applet (KartPadShowControllerApplet) assigns controllers to players: at start when player 1 has
// none, and from the port menu (Controls). A Pro Controller, a Joy-Con pair or the handheld console
// is one player, and so is one Joy-Con on its own, held sideways (SL and SR on top).
//
// Every physical input sends one Classic Controller button, per player and controller style. The
// port menu edits these tables (KartPadInput*); sdmc:/switch/kartpad/controls.cfg keeps what
// differs from the defaults.
//
// Reference for the target bit layout and stick scaling:
//   ref/kartpad-main/runtime/src/android/controller_mapping.cpp
//
#include "kartpad_mobile_runtime_host.h"
#include "kartpad/android/gamepad_contract.h"

#include <switch.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

// Consume the shared Classic Controller ABI rather than duplicating bit values.
namespace cc = kartpad::android;

constexpr unsigned kPlayers = 4;
constexpr const char* kMappingPath = "sdmc:/switch/kartpad/controls.cfg";

// Physical inputs. The sideways ones are places on one Joy-Con held with SL and SR on top;
// HeldInputs() finds them on a left or a right Joy-Con.
enum Input : unsigned {
  kInA, kInB, kInX, kInY, kInL, kInR, kInZl, kInZr, kInPlus, kInMinus,
  kInUp, kInDown, kInLeft, kInRight, kInStickL, kInStickR,
  kInLeftSl, kInLeftSr, kInRightSl, kInRightSr,
  kInFaceRight, kInFaceBottom, kInFaceTop, kInFaceLeft, kInSl, kInSr, kInStick, kInPlusMinus,
  kInShoulder, kInTrigger,
  kInputCount
};

struct InputInfo {
  const char* key;   // controls.cfg
  const char* name;  // port menu
};
constexpr InputInfo kInputs[kInputCount] = {
    {"a", "A"}, {"b", "B"}, {"x", "X"}, {"y", "Y"}, {"l", "L"}, {"r", "R"}, {"zl", "ZL"}, {"zr", "ZR"},
    {"plus", "+"}, {"minus", "-"}, {"up", "D-pad Up"}, {"down", "D-pad Down"}, {"left", "D-pad Left"},
    {"right", "D-pad Right"}, {"lstick", "Left stick press"}, {"rstick", "Right stick press"},
    {"left_sl", "Left Joy-Con SL"}, {"left_sr", "Left Joy-Con SR"}, {"right_sl", "Right Joy-Con SL"},
    {"right_sr", "Right Joy-Con SR"},
    {"face_right", "Right button"}, {"face_bottom", "Bottom button"}, {"face_top", "Top button"},
    {"face_left", "Left button"}, {"sl", "SL"}, {"sr", "SR"}, {"stick", "Stick press"},
    {"plus_minus", "+ or -"}, {"shoulder", "L or R (on the end)"}, {"trigger", "ZL or ZR (on the end)"},
};

// HID buttons of the inputs a Pro Controller, a Joy-Con pair or the handheld console has.
constexpr u64 kInputButtons[kInRightSr + 1] = {
    HidNpadButton_A, HidNpadButton_B, HidNpadButton_X, HidNpadButton_Y, HidNpadButton_L, HidNpadButton_R,
    HidNpadButton_ZL, HidNpadButton_ZR, HidNpadButton_Plus, HidNpadButton_Minus, HidNpadButton_Up,
    HidNpadButton_Down, HidNpadButton_Left, HidNpadButton_Right, HidNpadButton_StickL, HidNpadButton_StickR,
    HidNpadButton_LeftSL, HidNpadButton_LeftSR, HidNpadButton_RightSL, HidNpadButton_RightSR,
};

struct StyleInfo {
  const char* key;
  const char* name;
  unsigned first, count;  // its inputs
};
constexpr StyleInfo kStyles[KARTPAD_INPUT_STYLE_COUNT] = {
    {"pro", "Pro Controller", kInA, kInStickR + 1},
    {"pair", "Joy-Con pair", kInA, kInRightSr + 1},  // SL and SR too, for Joy-Cons held apart
    {"sideways", "One Joy-Con, sideways", kInFaceRight, kInputCount - kInFaceRight},
    {"handheld", "Handheld", kInA, kInStickR + 1},
};

struct Target {
  const char* key;
  const char* name;
  const char* use;  // in a race
  std::uint32_t button;
};
enum : std::uint8_t {
  kToNone, kToA, kToB, kToX, kToY, kToL, kToR, kToZl, kToZr, kToPlus, kToMinus,
  kToUp, kToDown, kToLeft, kToRight, kTargetCount
};
constexpr Target kTargets[kTargetCount] = {
    {"none", "Nothing", "", 0},
    {"a", "A", "accelerate", cc::kClassicA},
    {"b", "B", "brake, drift", cc::kClassicB},
    {"x", "X", "look behind", cc::kClassicX},
    {"y", "Y", "", cc::kClassicY},
    {"l", "L", "use item", cc::kClassicL},
    {"r", "R", "drift, brake", cc::kClassicR},
    {"zl", "ZL", "", cc::kClassicZl},
    {"zr", "ZR", "look behind", cc::kClassicZr},
    {"plus", "+", "pause", cc::kClassicPlus},
    {"minus", "-", "", cc::kClassicMinus},
    {"up", "D-pad Up", "trick, wheelie", cc::kClassicUp},
    {"down", "D-pad Down", "trick", cc::kClassicDown},
    {"left", "D-pad Left", "trick", cc::kClassicLeft},
    {"right", "D-pad Right", "trick", cc::kClassicRight},
};

// Each button sends the Classic Controller button of the same name. One Joy-Con sideways: its face
// buttons by place (A right, B bottom, X top, Y left), SL and SR as L and R, + or - as +; it has
// no D-pad, so its stick press does tricks and wheelies.
std::uint8_t DefaultTarget(unsigned input) {
  switch (input) {
    case kInA: case kInFaceRight: return kToA;
    case kInB: case kInFaceBottom: return kToB;
    case kInX: case kInFaceTop: return kToX;
    case kInY: case kInFaceLeft: return kToY;
    case kInL: case kInSl: return kToL;
    case kInR: case kInSr: return kToR;
    case kInZl: return kToZl;
    case kInZr: return kToZr;
    case kInPlus: case kInPlusMinus: return kToPlus;
    case kInMinus: return kToMinus;
    case kInUp: case kInStick: return kToUp;
    case kInDown: return kToDown;
    case kInLeft: return kToLeft;
    case kInRight: return kToRight;
    default: return kToNone;
  }
}

struct Mapping {
  std::uint8_t target[kInputCount];
  std::uint8_t steer;  // 0 left stick, 1 right stick
};

// Player colours and texts in the controller applet. Two players split the screen top and bottom,
// three or four into quarters.
constexpr HidLaControllerSupportArgColor kPlayerColors[kPlayers] = {
    {0xe6, 0x2e, 0x2e, 0xff}, {0x2e, 0x6b, 0xe6, 0xff}, {0xf2, 0xc4, 0x1b, 0xff}, {0x2e, 0xb8, 0x4a, 0xff}};
constexpr const char* kPlayerTexts[kPlayers] = {
    "Player 1: top screen (top left with 3-4 players)",
    "Player 2: bottom screen (top right with 3-4 players)",
    "Player 3: bottom left screen",
    "Player 4: bottom right screen",
};

Mutex g_lock;  // the game's pad reads (guest thread) and the port menu (GX HLE thread)
PadState g_pads[kPlayers]{};
u32 g_styles[kPlayers]{};
Mapping g_mapping[kPlayers][KARTPAD_INPUT_STYLE_COUNT];
bool g_padsReady = false;
bool g_configuredForGame = false;

struct Lock {
  Lock() { mutexLock(&g_lock); }
  ~Lock() { mutexUnlock(&g_lock); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

HidNpadIdType NpadOf(unsigned player) { return static_cast<HidNpadIdType>(HidNpadIdType_No1 + player); }

bool ValidStyle(int style) { return style >= 0 && style < KARTPAD_INPUT_STYLE_COUNT; }

// Four players, every controller style the applet can assign, Joy-Cons held sideways when alone.
// SDL's joystick start-up configures one player, so the game's first input read configures again.
void ConfigureHid() {
  padConfigureInput(kPlayers, HidNpadStyleSet_NpadStandard);
  hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
}

// Player N reads npad No.N, player 1 the handheld console too.
void InitializePads() {
  padInitializeDefault(&g_pads[0]);
  padInitialize(&g_pads[1], HidNpadIdType_No2);
  padInitialize(&g_pads[2], HidNpadIdType_No3);
  padInitialize(&g_pads[3], HidNpadIdType_No4);
  for (u32& style : g_styles) style = 0;
}

void ResetMapping(Mapping& mapping) {
  for (unsigned input = 0; input < kInputCount; ++input) mapping.target[input] = DefaultTarget(input);
  mapping.steer = 0;
}

int FindStyle(const char* key) {
  for (int style = 0; style < KARTPAD_INPUT_STYLE_COUNT; ++style) {
    if (std::strcmp(kStyles[style].key, key) == 0) return style;
  }
  return -1;
}

int FindInput(int style, const char* key) {
  const StyleInfo& info = kStyles[style];
  for (unsigned input = info.first; input < info.first + info.count; ++input) {
    if (std::strcmp(kInputs[input].key, key) == 0) return static_cast<int>(input);
  }
  return -1;
}

int FindTarget(const char* key) {
  for (int target = 0; target < kTargetCount; ++target) {
    if (std::strcmp(kTargets[target].key, key) == 0) return target;
  }
  return -1;
}

// controls.cfg: `p<player>.<style>.<input> = <target>` and `p<player>.<style>.steer = right`.
void LoadMapping() {
  for (auto& player : g_mapping) {
    for (Mapping& mapping : player) ResetMapping(mapping);
  }
  std::FILE* f = std::fopen(kMappingPath, "r");
  if (f == nullptr) return;
  char line[160];
  int applied = 0;
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    unsigned player = 0;
    char style[16], input[16], value[16];
    if (std::sscanf(line, " p%u.%15[a-z].%15[a-z_] = %15[a-z_]", &player, style, input, value) != 4) continue;
    const int s = FindStyle(style);
    if (player < 1 || player > kPlayers || s < 0) continue;
    Mapping& mapping = g_mapping[player - 1][s];
    if (std::strcmp(input, "steer") == 0) {
      if (s == KARTPAD_INPUT_JOYCON_SIDEWAYS) continue;
      mapping.steer = std::strcmp(value, "right") == 0 ? 1 : 0;
      ++applied;
      continue;
    }
    const int in = FindInput(s, input);
    const int target = FindTarget(value);
    if (in < 0 || target < 0) continue;
    mapping.target[in] = static_cast<std::uint8_t>(target);
    ++applied;
  }
  std::fclose(f);
  std::fprintf(stderr, "[input] %s: %d change(s) from the default mapping\n", kMappingPath, applied);
}

// What differs from the defaults, one line each.
std::string MappingText() {
  std::string text;
  char line[96];
  for (unsigned player = 0; player < kPlayers; ++player) {
    for (int s = 0; s < KARTPAD_INPUT_STYLE_COUNT; ++s) {
      const Mapping& mapping = g_mapping[player][s];
      const StyleInfo& style = kStyles[s];
      if (mapping.steer != 0) {
        std::snprintf(line, sizeof(line), "p%u.%s.steer = right\n", player + 1, style.key);
        text += line;
      }
      for (unsigned input = style.first; input < style.first + style.count; ++input) {
        if (mapping.target[input] == DefaultTarget(input)) continue;
        std::snprintf(line, sizeof(line), "p%u.%s.%s = %s\n", player + 1, style.key, kInputs[input].key,
                      kTargets[mapping.target[input]].key);
        text += line;
      }
    }
  }
  return text;
}

// Outside g_lock: an SD write must not hold up the game's pad reads.
void SaveMapping(const std::string& text) {
  if (text.empty()) {
    std::remove(kMappingPath);
    return;
  }
  std::FILE* f = std::fopen(kMappingPath, "w");
  if (f == nullptr) {
    std::fprintf(stderr, "[input] cannot write %s\n", kMappingPath);
    return;
  }
  std::fputs("# KartPad controller mapping (port menu > Controls): what differs from the defaults.\n"
             "# p<player>.<style>.<input> = <Classic Controller button>; p<player>.<style>.steer = right\n",
             f);
  std::fputs(text.c_str(), f);
  std::fclose(f);
}

// Everything below runs with g_lock held.
void EnsureReady() {
  if (!g_padsReady) {
    InitializePads();
    LoadMapping();
    g_padsReady = true;
  }
  if (!g_configuredForGame) {
    ConfigureHid();
    g_configuredForGame = true;
  }
}

// The style of a pad's controller (-1: none) and, for one Joy-Con, whether it is the left one.
int StyleOf(u32 set, bool* leftJoyCon) {
  *leftJoyCon = (set & HidNpadStyleTag_NpadJoyLeft) != 0;
  if (set & HidNpadStyleTag_NpadHandheld) return KARTPAD_INPUT_HANDHELD;
  if (set & HidNpadStyleTag_NpadFullKey) return KARTPAD_INPUT_PRO;
  if (set & HidNpadStyleTag_NpadJoyDual) return KARTPAD_INPUT_JOYCON_PAIR;
  if (set & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)) return KARTPAD_INPUT_JOYCON_SIDEWAYS;
  return -1;
}

const char* ControllerName(u32 set) {
  bool left = false;
  switch (StyleOf(set, &left)) {
    case KARTPAD_INPUT_HANDHELD: return "Handheld";
    case KARTPAD_INPUT_PRO: return "Pro Controller";
    case KARTPAD_INPUT_JOYCON_PAIR: return "Joy-Con pair";
    case KARTPAD_INPUT_JOYCON_SIDEWAYS: return left ? "Left Joy-Con, sideways" : "Right Joy-Con, sideways";
    default: return set != 0 ? "other controller" : nullptr;
  }
}

// Updates player N's pad; returns its style, -1 when nothing is connected.
int UpdatePad(unsigned player, bool* leftJoyCon) {
  PadState& pad = g_pads[player];
  padUpdate(&pad);
  const u32 set = padIsConnected(&pad) ? padGetStyleSet(&pad) : 0;
  if (set != g_styles[player]) {
    const char* name = ControllerName(set);
    std::fprintf(stderr, "[input] player %u: %s\n", player + 1, name != nullptr ? name : "none");
    g_styles[player] = set;
  }
  *leftJoyCon = false;
  if (!padIsConnected(&pad)) return -1;
  const int style = StyleOf(set, leftJoyCon);
  return style >= 0 ? style : KARTPAD_INPUT_PRO;
}

// The inputs held on a controller of this style: bit N for input N.
u64 HeldInputs(u64 nx, int style, bool leftJoyCon) {
  u64 held = 0;
  if (style == KARTPAD_INPUT_JOYCON_SIDEWAYS) {
    // Turned with SL and SR on top: the left Joy-Con anticlockwise (its Down button on the right),
    // the right one clockwise (its X on the right).
    const u64 places[kInputCount - kInFaceRight] = {
        leftJoyCon ? u64{HidNpadButton_Down} : u64{HidNpadButton_X},
        leftJoyCon ? u64{HidNpadButton_Left} : u64{HidNpadButton_A},
        leftJoyCon ? u64{HidNpadButton_Right} : u64{HidNpadButton_Y},
        leftJoyCon ? u64{HidNpadButton_Up} : u64{HidNpadButton_B},
        HidNpadButton_AnySL,
        HidNpadButton_AnySR,
        HidNpadButton_StickL | HidNpadButton_StickR,
        HidNpadButton_Plus | HidNpadButton_Minus,
        HidNpadButton_L | HidNpadButton_R,
        HidNpadButton_ZL | HidNpadButton_ZR,
    };
    for (unsigned i = 0; i < kInputCount - kInFaceRight; ++i) {
      if (nx & places[i]) held |= u64{1} << (kInFaceRight + i);
    }
    return held;
  }
  for (unsigned input = 0; input <= kInRightSr; ++input) {
    if (nx & kInputButtons[input]) held |= u64{1} << input;
  }
  return held;
}

float NormalizeStick(std::int32_t raw) {
  const float v = static_cast<float>(raw) / 32767.0f;
  return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

// After the applet or a Joy-Con split or join: fresh pad state for players 1-4, and the log says
// who now holds what.
void ReassignPads(const char* when) {
  ConfigureHid();
  InitializePads();
  char line[256];
  int length = std::snprintf(line, sizeof(line), "[input] %s:", when);
  for (unsigned player = 0; player < kPlayers; ++player) {
    PadState& pad = g_pads[player];
    padUpdate(&pad);
    g_styles[player] = padIsConnected(&pad) ? padGetStyleSet(&pad) : 0;
    const char* name = ControllerName(g_styles[player]);
    if (length > 0 && length < static_cast<int>(sizeof(line))) {
      length += std::snprintf(line + length, sizeof(line) - length, " player %u %s;", player + 1,
                              name != nullptr ? name : "none");
    }
  }
  std::fprintf(stderr, "%s\n", line);
}

bool LoneJoyCon(u32 set) {
  return !(set & HidNpadStyleTag_NpadJoyDual) &&
         (set & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight));
}

}  // namespace

extern "C" {

bool KartPadMobileIsControllerConnected(unsigned int player) {
  if (player >= kPlayers) return false;
  Lock lock;
  EnsureReady();
  bool left = false;
  return UpdatePad(player, &left) >= 0;
}

bool KartPadMobileReadClassicInputForPlayer(
    unsigned int player, KartPadMobileClassicInputSnapshot* snapshot) {
  if (snapshot == nullptr) return false;
  *snapshot = {};
  if (player >= kPlayers) return false;
  Lock lock;
  EnsureReady();
  bool left = false;
  const int style = UpdatePad(player, &left);
  if (style < 0) return false;
  const PadState& pad = g_pads[player];
  const Mapping& mapping = g_mapping[player][style];
  const StyleInfo& info = kStyles[style];
  const u64 held = HeldInputs(padGetButtons(&pad), style, left);
  std::uint32_t buttons = 0;
  for (unsigned input = info.first; input < info.first + info.count; ++input) {
    if (held & (u64{1} << input)) buttons |= kTargets[mapping.target[input]].button;
  }
  HidAnalogStickState stick;
  if (style == KARTPAD_INPUT_JOYCON_SIDEWAYS) {
    // libnx reports a right Joy-Con's stick as the right stick. HID gives it as the Joy-Con is
    // built (up = towards its top end) whatever the hold type, so it turns with the Joy-Con here:
    // anticlockwise for the left one, clockwise for the right one.
    const HidAnalogStickState raw = padGetStickPos(&pad, left ? 0 : 1);
    stick.x = left ? -raw.y : raw.y;
    stick.y = left ? raw.x : -raw.x;
  } else {
    stick = padGetStickPos(&pad, mapping.steer);
  }
  snapshot->buttons = buttons;
  snapshot->leftStickX = NormalizeStick(stick.x);
  snapshot->leftStickY = NormalizeStick(stick.y);
  snapshot->connected = 1;
  return true;
}

// The system controller applet for players 1-4 (pairs, sideways Joy-Cons, Pro Controllers). Blocks
// until it closes. From the port menu, or at start when player 1 has no controller.
bool KartPadShowControllerApplet(void) {
  {
    Lock lock;
    EnsureReady();
    // Four players and the horizontal hold type before the applet starts: on the vertical one it
    // fails for two or more players in handheld mode (0x5D59).
    ConfigureHid();
  }
  HidLaControllerSupportArg arg;
  hidLaCreateControllerSupportArg(&arg);
  arg.hdr.player_count_min = 1;
  arg.hdr.player_count_max = kPlayers;
  arg.hdr.enable_permit_joy_dual = 1;
  arg.hdr.enable_identification_color = 1;
  arg.enable_explain_text = 1;
  for (unsigned player = 0; player < kPlayers; ++player) {
    arg.identification_color[player] = kPlayerColors[player];
    hidLaSetExplainText(&arg, kPlayerTexts[player], NpadOf(player));
  }
  HidLaControllerSupportResultInfo info{};
  const Result rc = hidLaShowControllerSupport(&info, &arg);
  std::fprintf(stderr, "[input] controller applet: rc=0x%x, %d player(s)\n", rc, int(info.player_count));
  Lock lock;
  ReassignPads("after the controller applet");
  return R_SUCCEEDED(rc);
}

// At start: the applet when player 1 has no controller (Joy-Cons detached and not yet assigned).
void KartPadEnsureControllerAtStart(void) {
  // HID reports what is connected a few frames after the configuration
  bool connected = false;
  for (int i = 0; i < 30 && !connected; ++i) {
    {
      Lock lock;
      EnsureReady();
      padUpdate(&g_pads[0]);
      connected = padIsConnected(&g_pads[0]);
    }
    if (!connected) svcSleepThread(16'000'000);
  }
  if (!connected) KartPadShowControllerApplet();
  Lock lock;
  g_configuredForGame = false;  // SDL starts later and configures one player: configure again then
}

const char* KartPadInputStyleName(int style) { return ValidStyle(style) ? kStyles[style].name : ""; }

int KartPadInputSourceCount(int style) { return ValidStyle(style) ? static_cast<int>(kStyles[style].count) : 0; }

const char* KartPadInputSourceName(int style, int source) {
  if (source < 0 || source >= KartPadInputSourceCount(style)) return "";
  return kInputs[kStyles[style].first + source].name;
}

int KartPadInputTargetCount(void) { return kTargetCount; }

const char* KartPadInputTargetName(int target) {
  return target >= 0 && target < kTargetCount ? kTargets[target].name : "";
}

const char* KartPadInputTargetUse(int target) {
  return target >= 0 && target < kTargetCount ? kTargets[target].use : "";
}

unsigned int KartPadInputTargetButton(int target) {
  return target >= 0 && target < kTargetCount ? kTargets[target].button : 0;
}

int KartPadInputGetTarget(unsigned int player, int style, int source) {
  if (player >= kPlayers || source < 0 || source >= KartPadInputSourceCount(style)) return kToNone;
  Lock lock;
  EnsureReady();
  return g_mapping[player][style].target[kStyles[style].first + source];
}

void KartPadInputSetTarget(unsigned int player, int style, int source, int target) {
  if (player >= kPlayers || source < 0 || source >= KartPadInputSourceCount(style) || target < 0 ||
      target >= kTargetCount) {
    return;
  }
  std::string text;
  {
    Lock lock;
    EnsureReady();
    g_mapping[player][style].target[kStyles[style].first + source] = static_cast<std::uint8_t>(target);
    text = MappingText();
  }
  SaveMapping(text);
}

int KartPadInputGetSteeringStick(unsigned int player, int style) {
  if (player >= kPlayers || !ValidStyle(style) || style == KARTPAD_INPUT_JOYCON_SIDEWAYS) return -1;
  Lock lock;
  EnsureReady();
  return g_mapping[player][style].steer;
}

void KartPadInputSetSteeringStick(unsigned int player, int style, int stick) {
  if (player >= kPlayers || !ValidStyle(style) || style == KARTPAD_INPUT_JOYCON_SIDEWAYS || stick < 0 || stick > 1) {
    return;
  }
  std::string text;
  {
    Lock lock;
    EnsureReady();
    g_mapping[player][style].steer = static_cast<std::uint8_t>(stick);
    text = MappingText();
  }
  SaveMapping(text);
}

void KartPadInputResetMapping(unsigned int player, int style) {
  if (player >= kPlayers || !ValidStyle(style)) return;
  std::string text;
  {
    Lock lock;
    EnsureReady();
    ResetMapping(g_mapping[player][style]);
    text = MappingText();
  }
  SaveMapping(text);
}

void KartPadInputCopyMappingToAllPlayers(unsigned int player, int style) {
  if (player >= kPlayers || !ValidStyle(style)) return;
  std::string text;
  {
    Lock lock;
    EnsureReady();
    for (unsigned other = 0; other < kPlayers; ++other) g_mapping[other][style] = g_mapping[player][style];
    text = MappingText();
  }
  SaveMapping(text);
}

int KartPadInputPlayerStyle(unsigned int player) {
  if (player >= kPlayers) return -1;
  Lock lock;
  EnsureReady();
  bool left = false;
  return UpdatePad(player, &left);
}

const char* KartPadInputPlayerController(unsigned int player) {
  if (player >= kPlayers) return nullptr;
  Lock lock;
  EnsureReady();
  bool left = false;
  UpdatePad(player, &left);
  return ControllerName(g_styles[player]);
}

unsigned long long KartPadInputHeldSources(unsigned int player, int style) {
  if (player >= kPlayers || !ValidStyle(style)) return 0;
  Lock lock;
  EnsureReady();
  bool left = false;
  if (UpdatePad(player, &left) != style) return 0;
  const u64 held = HeldInputs(padGetButtons(&g_pads[player]), style, left);
  return (held >> kStyles[style].first) & ((u64{1} << kStyles[style].count) - 1);
}

int KartPadInputSplitJoyCons(unsigned int player) {
  if (player >= kPlayers) return -1;
  Lock lock;
  EnsureReady();
  const HidNpadIdType id = NpadOf(player);
  if (!(hidGetNpadStyleSet(id) & HidNpadStyleTag_NpadJoyDual)) return -1;
  // Single assignment: the left Joy-Con keeps this npad, HID moves the right one to a free one.
  bool moved = false;
  HidNpadIdType dest = id;
  Result rc = hidSetNpadJoyAssignmentModeSingleWithDestination(id, HidNpadJoyDeviceType_Left, &moved, &dest);
  if (R_FAILED(rc)) {
    moved = false;
    rc = hidSetNpadJoyAssignmentModeSingle(id, HidNpadJoyDeviceType_Left);  // before 5.0.0
  }
  std::fprintf(stderr, "[input] split player %u's Joy-Cons: rc=0x%x, right Joy-Con to npad %d\n", player + 1, rc,
               moved ? int(dest) : -1);
  ReassignPads("after a Joy-Con split");
  if (R_FAILED(rc)) return -1;
  if (moved) return dest <= HidNpadIdType_No4 ? int(dest - HidNpadIdType_No1) : -1;
  // Without a destination: the first player that now has a right Joy-Con alone.
  for (unsigned other = 0; other < kPlayers; ++other) {
    const u32 set = hidGetNpadStyleSet(NpadOf(other));
    if (other != player && LoneJoyCon(set) && (set & HidNpadStyleTag_NpadJoyRight)) return int(other);
  }
  return -1;
}

int KartPadInputJoinJoyCons(unsigned int player) {
  if (player >= kPlayers) return -1;
  Lock lock;
  EnsureReady();
  const u32 mine = hidGetNpadStyleSet(NpadOf(player));
  if (!LoneJoyCon(mine)) return -1;
  const u32 partner = (mine & HidNpadStyleTag_NpadJoyLeft) ? HidNpadStyleTag_NpadJoyRight : HidNpadStyleTag_NpadJoyLeft;
  for (unsigned other = 0; other < kPlayers; ++other) {
    const u32 set = hidGetNpadStyleSet(NpadOf(other));
    if (other == player || !LoneJoyCon(set) || !(set & partner)) continue;
    Result rc = hidMergeSingleJoyAsDualJoy(NpadOf(player), NpadOf(other));
    if (R_FAILED(rc)) {
      // Joy-Cons in single assignment merge after both npads are back in dual assignment.
      hidSetNpadJoyAssignmentModeDual(NpadOf(player));
      hidSetNpadJoyAssignmentModeDual(NpadOf(other));
      rc = hidMergeSingleJoyAsDualJoy(NpadOf(player), NpadOf(other));
    }
    std::fprintf(stderr, "[input] join players %u and %u's Joy-Cons: rc=0x%x\n", player + 1, other + 1, rc);
    ReassignPads("after a Joy-Con join");
    return R_SUCCEEDED(rc) ? int(other) : -1;
  }
  return -1;
}

}  // extern "C"
