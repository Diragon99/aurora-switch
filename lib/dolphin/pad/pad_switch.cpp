#include <dolphin/pad.h>

#include <aurora/aurora.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <switch.h>

// The remap table is persisted under Aurora's user path (dusk::ConfigPath on Switch). Declared
// here to avoid pulling in internal.hpp (fmt/SDL) which does not build in this libnx TU.
namespace aurora {
extern AuroraConfig g_config;
}

namespace {
PadState s_pad{};
bool s_initialized = false;
bool s_blockInput = false;
u32 s_spec = PAD_SPEC_5;
u32 s_analogMode = 0;
HidSixAxisSensorHandle s_sixAxisHandle{};
u8 s_enabledSensors = 0;
bool s_sixAxisStarted = false;
bool s_sixAxisHandleValid = false;

constexpr u8 kSensorAccelBit = 1 << 0;
constexpr u8 kSensorGyroBit = 1 << 1;

constexpr s8 scaleStick(s32 value) {
  const s32 scaled = value / 256;
  return static_cast<s8>(std::clamp(scaled, -128, 127));
}

void ensureInitialized() {
  if (s_initialized) {
    return;
  }
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&s_pad);
  s_initialized = true;
}

u8 sensorBit(PADSensorType sensor) {
  switch (sensor) {
  case PAD_SENSOR_ACCEL:
    return kSensorAccelBit;
  case PAD_SENSOR_GYRO:
    return kSensorGyroBit;
  default:
    return 0;
  }
}

bool chooseSixAxisStyle(u32 styleSet, HidNpadStyleTag& outStyle) {
  if ((styleSet & HidNpadStyleTag_NpadFullKey) != 0) {
    outStyle = HidNpadStyleTag_NpadFullKey;
    return true;
  }
  if ((styleSet & HidNpadStyleTag_NpadJoyDual) != 0) {
    outStyle = HidNpadStyleTag_NpadJoyDual;
    return true;
  }
  if ((styleSet & HidNpadStyleTag_NpadJoyRight) != 0) {
    outStyle = HidNpadStyleTag_NpadJoyRight;
    return true;
  }
  if ((styleSet & HidNpadStyleTag_NpadJoyLeft) != 0) {
    outStyle = HidNpadStyleTag_NpadJoyLeft;
    return true;
  }
  return false;
}

bool resolveSixAxisHandle(HidSixAxisSensorHandle& outHandle) {
  ensureInitialized();
  padUpdate(&s_pad);

  HidNpadIdType id = HidNpadIdType_No1;
  HidNpadStyleTag style = static_cast<HidNpadStyleTag>(0);
  if (padIsHandheld(&s_pad)) {
    id = HidNpadIdType_Handheld;
    style = HidNpadStyleTag_NpadHandheld;
  } else if (padIsNpadActive(&s_pad, HidNpadIdType_No1)) {
    if (!chooseSixAxisStyle(hidGetNpadStyleSet(HidNpadIdType_No1), style)) {
      return false;
    }
  } else {
    return false;
  }

  HidSixAxisSensorHandle handles[2]{};
  if (style == HidNpadStyleTag_NpadJoyDual) {
    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(handles, 2, id, style))) {
      // Detached dual Joy-Con has one sensor per controller. Use the right Joy-Con for aiming.
      outHandle = handles[1];
      return true;
    }
  }

  if (R_FAILED(hidGetSixAxisSensorHandles(handles, 1, id, style))) {
    return false;
  }
  outHandle = handles[0];
  return true;
}

void stopSixAxisSensor() {
  if (s_sixAxisStarted) {
    hidStopSixAxisSensor(s_sixAxisHandle);
    s_sixAxisStarted = false;
  }
  s_sixAxisHandleValid = false;
}

bool ensureSixAxisSensorStarted() {
  HidSixAxisSensorHandle handle{};
  if (!resolveSixAxisHandle(handle)) {
    return false;
  }

  if (s_sixAxisStarted && s_sixAxisHandle.type_value != handle.type_value) {
    hidStopSixAxisSensor(s_sixAxisHandle);
    s_sixAxisStarted = false;
    s_sixAxisHandleValid = false;
  }

  s_sixAxisHandle = handle;
  s_sixAxisHandleValid = true;
  if (!s_sixAxisStarted) {
    if (R_FAILED(hidStartSixAxisSensor(s_sixAxisHandle))) {
      s_sixAxisHandleValid = false;
      return false;
    }
    s_sixAxisStarted = true;
  }

  return true;
}

// --- Controller remapping -------------------------------------------------------------------
// The controller-config UI drives the GC pad through the PAD*Mapping API. This backend keeps a
// GC-button -> Switch-physical-button table (s_buttonMappings) that mapButtons() consults, so the
// user can remap from the interface. Native button ids are offset past the SDL_GamepadButton
// D-pad range (11-14) so the shared native_button_name() never mislabels them and instead falls
// through to PADGetNativeButtonName(). Minus is intentionally absent: it stays the Dusk menu key
// and cannot be bound.
struct SwitchNativeButton {
  u32 id;
  u64 hidMask;
  const char* name;
};

enum : u32 {
  NBTN_A = 0x100,
  NBTN_B,
  NBTN_X,
  NBTN_Y,
  NBTN_L,
  NBTN_R,
  NBTN_ZL,
  NBTN_ZR,
  NBTN_PLUS,
  NBTN_STICK_L,
  NBTN_STICK_R,
  NBTN_UP,
  NBTN_DOWN,
  NBTN_LEFT,
  NBTN_RIGHT,
};

constexpr std::array<SwitchNativeButton, 15> kSwitchButtons = {{
    {NBTN_A, HidNpadButton_A, "A"},
    {NBTN_B, HidNpadButton_B, "B"},
    {NBTN_X, HidNpadButton_X, "X"},
    {NBTN_Y, HidNpadButton_Y, "Y"},
    {NBTN_L, HidNpadButton_L, "L"},
    {NBTN_R, HidNpadButton_R, "R"},
    {NBTN_ZL, HidNpadButton_ZL, "ZL"},
    {NBTN_ZR, HidNpadButton_ZR, "ZR"},
    {NBTN_PLUS, HidNpadButton_Plus, "+"},
    {NBTN_STICK_L, HidNpadButton_StickL, "L-Stick"},
    {NBTN_STICK_R, HidNpadButton_StickR, "R-Stick"},
    {NBTN_UP, HidNpadButton_Up, "D-Up"},
    {NBTN_DOWN, HidNpadButton_Down, "D-Down"},
    {NBTN_LEFT, HidNpadButton_Left, "D-Left"},
    {NBTN_RIGHT, HidNpadButton_Right, "D-Right"},
}};

// Default GC <- Switch mapping. Mirrors the GameCube-style shoulder layout: GC L=ZL, GC R=ZR,
// GC Z on the R bumper. (The old backend also let the L bumper trigger GC L; the single-button
// remap model drops that redundancy — remap GC L to the L bumper if you prefer it.)
constexpr std::array<PADButtonMapping, PAD_BUTTON_COUNT> kDefaultMapping = {{
    {NBTN_LEFT, PAD_BUTTON_LEFT},
    {NBTN_RIGHT, PAD_BUTTON_RIGHT},
    {NBTN_DOWN, PAD_BUTTON_DOWN},
    {NBTN_UP, PAD_BUTTON_UP},
    {NBTN_R, PAD_TRIGGER_Z},
    {NBTN_ZR, PAD_TRIGGER_R},
    {NBTN_ZL, PAD_TRIGGER_L},
    {NBTN_A, PAD_BUTTON_A},
    {NBTN_B, PAD_BUTTON_B},
    {NBTN_X, PAD_BUTTON_X},
    {NBTN_Y, PAD_BUTTON_Y},
    {NBTN_PLUS, PAD_BUTTON_START},
}};

std::array<PADButtonMapping, PAD_BUTTON_COUNT> s_buttonMappings = kDefaultMapping;
bool s_mappingsLoaded = false;

constexpr u32 kMappingMagic = 0x53504144;  // 'SPAD'
constexpr u32 kMappingVersion = 1;

u64 hidMaskForNative(u32 id) {
  for (const auto& b : kSwitchButtons) {
    if (b.id == id) {
      return b.hidMask;
    }
  }
  return 0;
}

bool isKnownNative(u32 id) {
  if (id == PAD_NATIVE_BUTTON_INVALID) {
    return true;
  }
  for (const auto& b : kSwitchButtons) {
    if (b.id == id) {
      return true;
    }
  }
  return false;
}

std::filesystem::path mappingFilePath() {
  if (aurora::g_config.userPath == nullptr) {
    return {};
  }
  return std::filesystem::path{aurora::g_config.userPath} / "switch_pad_mapping.bin";
}

void saveMappings() {
  const auto path = mappingFilePath();
  if (path.empty()) {
    return;
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return;
  }
  const u32 magic = kMappingMagic;
  const u32 version = kMappingVersion;
  file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
  file.write(reinterpret_cast<const char*>(&version), sizeof(version));
  file.write(reinterpret_cast<const char*>(s_buttonMappings.data()),
             sizeof(PADButtonMapping) * PAD_BUTTON_COUNT);
}

void loadMappings() {
  s_mappingsLoaded = true;
  s_buttonMappings = kDefaultMapping;

  const auto path = mappingFilePath();
  if (path.empty()) {
    return;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return;
  }
  u32 magic = 0;
  u32 version = 0;
  file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
  file.read(reinterpret_cast<char*>(&version), sizeof(version));
  if (!file || magic != kMappingMagic || version != kMappingVersion) {
    return;
  }
  std::array<PADButtonMapping, PAD_BUTTON_COUNT> loaded{};
  file.read(reinterpret_cast<char*>(loaded.data()), sizeof(PADButtonMapping) * PAD_BUTTON_COUNT);
  if (!file) {
    return;
  }
  for (const auto& m : loaded) {
    if (!isKnownNative(m.nativeButton)) {
      return;  // corrupt/foreign file -> keep defaults
    }
  }
  s_buttonMappings = loaded;
}

void ensureMappingsLoaded() {
  if (!s_mappingsLoaded) {
    loadMappings();
  }
}

u16 mapButtons(u64 buttons) {
  ensureMappingsLoaded();
  u16 out = 0;
  for (const auto& m : s_buttonMappings) {
    const u64 mask = hidMaskForNative(m.nativeButton);
    if (mask != 0 && (buttons & mask) != 0) {
      out |= m.padButton;
    }
  }
  return out;
}

#ifdef TARGET_PC
std::array<PADKeyButtonBinding, PAD_BUTTON_COUNT> s_keyButtons{};
std::array<PADKeyAxisBinding, PAD_AXIS_COUNT> s_keyAxes{};
PADDeadZones s_deadZones{true, true, 6000, 6000, 30, 30};
u16 s_rumbleLow = 0;
u16 s_rumbleHigh = 0;
#endif
} // namespace

extern "C" {

BOOL PADInit() {
  ensureInitialized();
  return TRUE;
}

u32 PADReadRaw(PADStatus* status) {
  ensureInitialized();
  if (status == nullptr) {
    return 0;
  }

  padUpdate(&s_pad);
  std::memset(status, 0, sizeof(PADStatus) * PAD_MAX_CONTROLLERS);

  const u64 buttons = padGetButtons(&s_pad);
  const HidAnalogStickState left = padGetStickPos(&s_pad, 0);
  const HidAnalogStickState right = padGetStickPos(&s_pad, 1);

  const u16 padButtons = mapButtons(buttons);
  status[0].button = padButtons;
  status[0].stickX = scaleStick(left.x);
  status[0].stickY = scaleStick(left.y);
  status[0].substickX = scaleStick(right.x);
  status[0].substickY = scaleStick(right.y);
  // Digital triggers follow whatever the remap routed to GC L/R/A/B (default: ZL/ZR/A/B).
  status[0].triggerLeft = (padButtons & PAD_TRIGGER_L) != 0 ? 255 : 0;
  status[0].triggerRight = (padButtons & PAD_TRIGGER_R) != 0 ? 255 : 0;
  status[0].analogA = (padButtons & PAD_BUTTON_A) != 0 ? 255 : 0;
  status[0].analogB = (padButtons & PAD_BUTTON_B) != 0 ? 255 : 0;
  status[0].err = PAD_ERR_NONE;
#ifdef TARGET_PC
  status[0].extButton = 0;
#endif

  for (int i = 1; i < PAD_MAX_CONTROLLERS; ++i) {
    status[i].err = PAD_ERR_NO_CONTROLLER;
  }

  return PAD_CHAN0_BIT;
}

u32 PADRead(PADStatus* status) {
  const u32 result = PADReadRaw(status);
  if (!s_blockInput || status == nullptr || result == 0) {
    return result;
  }

  for (int i = 0; i < PAD_MAX_CONTROLLERS; ++i) {
    if (status[i].err != PAD_ERR_NONE) {
      continue;
    }
    status[i].button = 0;
    status[i].stickX = 0;
    status[i].stickY = 0;
    status[i].substickX = 0;
    status[i].substickY = 0;
    status[i].triggerLeft = 0;
    status[i].triggerRight = 0;
    status[i].analogA = 0;
    status[i].analogB = 0;
  }
  return result;
}

BOOL PADReset(u32 mask) {
  (void)mask;
  return TRUE;
}

BOOL PADRecalibrate(u32 mask) {
  (void)mask;
  return TRUE;
}

void PADClamp(PADStatus* status) { (void)status; }
void PADClampCircle(PADStatus* status) { (void)status; }
void PADControlMotor(u32 chan, u32 cmd) {
  (void)chan;
  (void)cmd;
}
void PADSetSpec(u32 spec) { s_spec = spec; }
void PADControlAllMotors(const u32* cmdArr) { (void)cmdArr; }
void PADSetAnalogMode(u32 mode) { s_analogMode = mode; }

// v1.4.1-new PAD entry points used by dusk's touch controls (virtual status), gamepad LED, and
// controller-config force-rumble UI. The Switch backend doesn't drive HID through these paths, so
// they are inert stubs (matching the standalone-backend convention of the rest of this file).
void PADSetVirtualStatus(u32 port, const PADStatus* status) {
  (void)port;
  (void)status;
}
void PADClearVirtualStatus(u32 port) { (void)port; }
BOOL PADHasLED(u32 port) {
  (void)port;
  return FALSE;
}
BOOL PADCanForceDeviceRumble(u32 port) {
  (void)port;
  return FALSE;
}
BOOL PADGetForceDeviceRumble(u32 port) {
  (void)port;
  return FALSE;
}
BOOL PADSetForceDeviceRumble(u32 port, BOOL force) {
  (void)port;
  (void)force;
  return FALSE;
}

#ifdef TARGET_PC
u32 PADCount() { return 1; }
const char* PADGetNameForControllerIndex(u32 idx) { return idx == 0 ? "Switch Controller" : nullptr; }
void PADSetPortForIndex(u32 index, u32 port) {
  (void)index;
  (void)port;
}
s32 PADGetIndexForPort(u32 port) { return port == 0 ? 0 : -1; }
void PADGetVidPid(u32 port, u32* vid, u32* pid) {
  (void)port;
  if (vid != nullptr) {
    *vid = 0;
  }
  if (pid != nullptr) {
    *pid = 0;
  }
}
void PADClearPort(u32 port) { (void)port; }
const char* PADGetName(u32 port) { return port == 0 ? "Switch Controller" : nullptr; }
void PADSetButtonMapping(u32 port, PADButtonMapping mapping) {
  if (port != PAD_CHAN0) {
    return;
  }
  ensureMappingsLoaded();
  for (auto& m : s_buttonMappings) {
    if (m.padButton == mapping.padButton) {
      m.nativeButton = mapping.nativeButton;
      break;
    }
  }
}
void PADSetAllButtonMappings(u32 port, PADButtonMapping buttons[PAD_BUTTON_COUNT]) {
  if (port != PAD_CHAN0 || buttons == nullptr) {
    return;
  }
  ensureMappingsLoaded();
  std::copy(buttons, buttons + PAD_BUTTON_COUNT, s_buttonMappings.begin());
}
PADButtonMapping* PADGetButtonMappings(u32 port, u32* buttonCount) {
  if (port != PAD_CHAN0) {
    if (buttonCount != nullptr) {
      *buttonCount = 0;
    }
    return nullptr;
  }
  ensureMappingsLoaded();
  if (buttonCount != nullptr) {
    *buttonCount = PAD_BUTTON_COUNT;
  }
  return s_buttonMappings.data();
}
void PADSetAxisMapping(u32 port, PADAxisMapping mapping) {
  (void)port;
  (void)mapping;
}
void PADSetAllAxisMappings(u32 port, PADAxisMapping axes[PAD_AXIS_COUNT]) {
  (void)port;
  (void)axes;
}
PADAxisMapping* PADGetAxisMappings(u32 port, u32* axisCount) {
  (void)port;
  if (axisCount != nullptr) {
    *axisCount = 0;
  }
  return nullptr;
}
void PADSerializeMappings() {
  ensureMappingsLoaded();
  saveMappings();
}

BOOL PADSetKeyButtonBinding(u32 port, PADKeyButtonBinding binding) {
  (void)port;
  (void)binding;
  return FALSE;
}
BOOL PADSetKeyButtonBindings(u32 port, PADKeyButtonBinding bindings[PAD_BUTTON_COUNT]) {
  (void)port;
  (void)bindings;
  return FALSE;
}
PADKeyButtonBinding* PADGetKeyButtonBindings(u32 port, u32* buttonCount) {
  // No keyboard on Switch. Returning null keeps the controller-config UI out of keyboard-binding
  // mode (keyboard_active()), which otherwise waited forever for a keypress that never arrives.
  (void)port;
  if (buttonCount != nullptr) {
    *buttonCount = 0;
  }
  return nullptr;
}
BOOL PADSetKeyAxisBinding(u32 port, PADKeyAxisBinding binding) {
  (void)port;
  (void)binding;
  return FALSE;
}
BOOL PADSetKeyAxisBindings(u32 port, PADKeyAxisBinding bindings[PAD_AXIS_COUNT]) {
  (void)port;
  (void)bindings;
  return FALSE;
}
PADKeyAxisBinding* PADGetKeyAxisBindings(u32 port, u32* axisCount) {
  (void)port;
  if (axisCount != nullptr) {
    *axisCount = PAD_AXIS_COUNT;
  }
  return s_keyAxes.data();
}
void PADClearKeyBindings(u32 port) { (void)port; }
void PADSetKeyboardActive(u32 port, BOOL active) {
  (void)port;
  (void)active;
}

PADDeadZones* PADGetDeadZones(u32 port) {
  (void)port;
  return &s_deadZones;
}
const char* PADGetButtonName(PADButton button) {
  switch (button) {
  case PAD_BUTTON_A:
    return "A";
  case PAD_BUTTON_B:
    return "B";
  case PAD_BUTTON_X:
    return "X";
  case PAD_BUTTON_Y:
    return "Y";
  case PAD_BUTTON_START:
    return "Start";
  case PAD_TRIGGER_Z:
    return "Z";
  case PAD_TRIGGER_L:
    return "L";
  case PAD_TRIGGER_R:
    return "R";
  case PAD_BUTTON_UP:
    return "Up";
  case PAD_BUTTON_DOWN:
    return "Down";
  case PAD_BUTTON_LEFT:
    return "Left";
  case PAD_BUTTON_RIGHT:
    return "Right";
  default:
    return "";
  }
}
const char* PADGetNativeButtonName(u32 button) {
  for (const auto& b : kSwitchButtons) {
    if (b.id == button) {
      return b.name;
    }
  }
  return "";
}
const char* PADGetAxisName(PADAxis axis) {
  (void)axis;
  return "";
}
const char* PADGetAxisDirectionLabel(PADAxis axis) {
  (void)axis;
  return "";
}
const char* PADGetNativeAxisName(PADSignedNativeAxis axis) {
  (void)axis;
  return "";
}

BOOL PADIsGCAdapter(u32 port) {
  (void)port;
  return FALSE;
}
struct SDL_Gamepad* PADGetSDLGamepadForIndex(u32 index) {
  (void)index;
  return nullptr;
}
s32 PADGetNativeButtonPressed(u32 port) {
  if (port != PAD_CHAN0) {
    return -1;
  }
  ensureInitialized();
  padUpdate(&s_pad);
  const u64 held = padGetButtons(&s_pad);
  for (const auto& b : kSwitchButtons) {
    if ((held & b.hidMask) != 0) {
      return static_cast<s32>(b.id);
    }
  }
  return -1;
}
PADSignedNativeAxis PADGetNativeAxisPulled(u32 port) {
  (void)port;
  return {-1, AXIS_SIGN_POSITIVE};
}
void PADRestoreDefaultMapping(u32 port) {
  if (port != PAD_CHAN0) {
    return;
  }
  s_buttonMappings = kDefaultMapping;
  s_mappingsLoaded = true;
  saveMappings();
}
void PADBlockInput(bool block) { s_blockInput = block; }
void PADSetDefaultMapping(const PADDefaultMapping* mapping, PADControllerType type) {
  (void)mapping;
  (void)type;
}
BOOL PADSetColor(u32 port, u8 red, u8 green, u8 blue) {
  (void)port;
  (void)red;
  (void)green;
  (void)blue;
  return FALSE;
}
BOOL PADGetColor(u32 port, u8* red, u8* green, u8* blue) {
  (void)port;
  if (red != nullptr) {
    *red = 0;
  }
  if (green != nullptr) {
    *green = 0;
  }
  if (blue != nullptr) {
    *blue = 0;
  }
  return FALSE;
}
BOOL PADSetSensorEnabled(u32 port, PADSensorType sensor, BOOL enabled) {
  if (port != PAD_CHAN0) {
    return FALSE;
  }

  const u8 bit = sensorBit(sensor);
  if (bit == 0) {
    return FALSE;
  }

  if (!enabled) {
    s_enabledSensors &= ~bit;
    if (s_enabledSensors == 0) {
      stopSixAxisSensor();
    }
    return TRUE;
  }

  if (!ensureSixAxisSensorStarted()) {
    return FALSE;
  }

  s_enabledSensors |= bit;
  return TRUE;
}
BOOL PADHasSensor(u32 port, PADSensorType sensor) {
  if (port != PAD_CHAN0 || sensorBit(sensor) == 0) {
    return FALSE;
  }

  HidSixAxisSensorHandle handle{};
  return resolveSixAxisHandle(handle) ? TRUE : FALSE;
}
BOOL PADGetSensorData(u32 port, PADSensorType sensor, f32* data, int nValues) {
  if (data != nullptr && nValues > 0) {
    std::fill(data, data + nValues, 0.0f);
  }

  if (port != PAD_CHAN0 || data == nullptr || nValues <= 0) {
    return FALSE;
  }

  const u8 bit = sensorBit(sensor);
  if (bit == 0 || (s_enabledSensors & bit) == 0) {
    return FALSE;
  }

  if (!ensureSixAxisSensorStarted()) {
    return FALSE;
  }

  HidSixAxisSensorState state{};
  if (hidGetSixAxisSensorStates(s_sixAxisHandle, &state, 1) == 0 ||
      (state.attributes & HidSixAxisSensorAttribute_IsConnected) == 0)
  {
    return FALSE;
  }

  const HidVector& src = sensor == PAD_SENSOR_GYRO ? state.angular_velocity : state.acceleration;
  if (nValues > 0) {
    data[0] = src.x;
  }
  if (nValues > 1) {
    data[1] = src.y;
  }
  if (nValues > 2) {
    data[2] = src.z;
  }
  return TRUE;
}
BOOL PADSetRumbleIntensity(u32 port, u16 low, u16 high) {
  (void)port;
  s_rumbleLow = low;
  s_rumbleHigh = high;
  return FALSE;
}
BOOL PADGetRumbleIntensity(u32 port, u16* low, u16* high) {
  (void)port;
  if (low != nullptr) {
    *low = s_rumbleLow;
  }
  if (high != nullptr) {
    *high = s_rumbleHigh;
  }
  return FALSE;
}
BOOL PADSupportsRumbleIntensity(u32 port) {
  (void)port;
  return FALSE;
}
PADBatteryState PADGetBatteryState(u32 port, f32* perc) {
  (void)port;
  if (perc != nullptr) {
    *perc = 0.0f;
  }
  return PAD_BATTERYSTATE_UNKNOWN;
}
PADControllerType PADGetControllerType(u32 port) {
  (void)port;
  return PAD_TYPE_SWITCH_PROCON;
}
PADControllerType PADGetControllerTypeForIndex(u32 index) {
  (void)index;
  return PAD_TYPE_SWITCH_PROCON;
}
#endif

} // extern "C"
