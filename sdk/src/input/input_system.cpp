/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/input/device_assignment.h>
#include <rex/input/flags.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#if REX_PLATFORM_SWITCH
#include <rex/input/switch/switch_input_driver.h>
#else
#include <rex/input/sdl/sdl_input_driver.h>
#endif
#include <rex/input/state_merge.h>
#include <rex/input/xinput/xinput_input_driver.h>
#include <rex/logging.h>

#if REX_PLATFORM_SWITCH
REXCVAR_DEFINE_STRING(input_backend, "switch", "Input", "Input backend: switch")
    .allowed({"switch"});
#else
REXCVAR_DEFINE_STRING(input_backend, "sdl", "Input", "Input backend: sdl, xinput")
    .allowed({"sdl", "xinput"});
// fc3-switch 2026-11-12: the Switch driver's route playback on the PC build too (same file format and cvars), so a
// route recorded on the console (scenes/rec2.txt) can be replayed for a PC reference capture at the same place.
// Only the sticks are replayed (the file's button field holds Switch HID bits).
REXCVAR_DEFINE_STRING(input_play, "", "Input", "Replay a recorded route (sticks) on guest user 0");
REXCVAR_DEFINE_INT32(input_play_auto_s, 0, "Input", "Start input_play this many seconds after the first pad poll");
#endif

REXCVAR_DEFINE_BOOL(guide_button, false, "Input", "Enable guide button pass-through");
REXCVAR_DEFINE_BOOL(input_rumble, false, "Input",
                    "Controller vibration (off by default: controllers only get "
                    "zero vibration). true = the game's vibration");
// PROTOTYPE (fc3-switch, 2026-11-01): the same script as FC3_AUTOPRESS, from the config file, for the Switch,
// which has no environment variables. FC3_AUTOPRESS wins when both are set.
REXCVAR_DEFINE_STRING(input_autopress, "", "Input",
                      "Bring-up only: scripted presses on guest user 0, e.g. \"start@25,a@35:0.5\" (seconds after start)");

namespace rex::input {

namespace {

// Synthetic devices are parked past every physical ordinal so they cannot push
// a real pad off guest user 0. SlotAssignment routes them by their synthetic
// flag and never reads this value.
constexpr uint32_t kSyntheticOrdinal = UINT32_MAX;

// PROTOTYPE (fc3 bring-up, 2026-11-01): scripted pad input for unattended boot tests.
// Env FC3_AUTOPRESS="start@25,a@35,down@40:0.3" holds the named buttons on guest user 0 at the given seconds
// after process start (default hold 0.25 s). Names: a b x y start back up down left right lb rb ls rs.
// The buttons are ORed into XamInputGetState (packet number bumped on every change) and replayed as
// VK_PAD_* KEYDOWN/KEYUP through XamInputGetKeystroke. Unset = no effect. Not a feature: remove when the
// game is driven by a real pad / remote input.
namespace autopress {
struct Press {
  double at, until;
  uint16_t buttons;
  int axis = -1;     // 0 lx, 1 ly, 2 rx, 3 ry, 4 lt, 5 rt (sticks/triggers; -1 = button)
  int value = 0;
};
// Stick/trigger names for the script: "lfwd@50:3" pushes the left stick forward for 3 s.
bool AxisOf(const std::string& n, int* axis, int* value) {
  static const struct { const char* name; int axis, value; } kMap[] = {
      {"lfwd", 1, 32767}, {"lback", 1, -32767}, {"lleft", 0, -32767}, {"lright", 0, 32767},
      {"rup", 3, 32767},  {"rdown", 3, -32767}, {"rleft", 2, -32767}, {"rright", 2, 32767},
      {"lt", 4, 255},     {"rt", 5, 255}};
  for (const auto& m : kMap) {
    if (n == m.name) { *axis = m.axis; *value = m.value; return true; }
  }
  return false;
}
const auto kStart = std::chrono::steady_clock::now();
std::mutex mutex;
bool parsed = false;
std::vector<Press> presses;
uint32_t packet_bump = 0;
uint16_t last_state_mask = 0;
uint16_t last_key_mask = 0;

uint16_t ButtonOf(const std::string& n) {
  static const struct { const char* name; uint16_t bit; } kMap[] = {
      {"a", X_INPUT_GAMEPAD_A}, {"b", X_INPUT_GAMEPAD_B}, {"x", X_INPUT_GAMEPAD_X}, {"y", X_INPUT_GAMEPAD_Y},
      {"start", X_INPUT_GAMEPAD_START}, {"back", X_INPUT_GAMEPAD_BACK}, {"up", X_INPUT_GAMEPAD_DPAD_UP},
      {"down", X_INPUT_GAMEPAD_DPAD_DOWN}, {"left", X_INPUT_GAMEPAD_DPAD_LEFT},
      {"right", X_INPUT_GAMEPAD_DPAD_RIGHT}, {"lb", X_INPUT_GAMEPAD_LEFT_SHOULDER},
      {"rb", X_INPUT_GAMEPAD_RIGHT_SHOULDER}, {"ls", X_INPUT_GAMEPAD_LEFT_THUMB},
      {"rs", X_INPUT_GAMEPAD_RIGHT_THUMB}};
  for (const auto& m : kMap) {
    if (n == m.name) return m.bit;
  }
  return 0;
}

void ParseLocked() {
  parsed = true;
  const char* env = std::getenv("FC3_AUTOPRESS");
  std::string spec = (env && *env) ? std::string(env) : std::string(REXCVAR_GET(input_autopress));
  if (spec.empty()) return;
  size_t pos = 0;
  while (pos <= spec.size()) {
    size_t end = spec.find(',', pos);
    if (end == std::string::npos) end = spec.size();
    std::string item = spec.substr(pos, end - pos);
    pos = end + 1;
    size_t at = item.find('@');
    if (at == std::string::npos) continue;
    uint16_t bit = ButtonOf(item.substr(0, at));
    double t = std::atof(item.c_str() + at + 1);
    double hold = 0.25;
    size_t colon = item.find(':', at);
    if (colon != std::string::npos) hold = std::atof(item.c_str() + colon + 1);
    int axis = -1, value = 0;
    if (!bit && !AxisOf(item.substr(0, at), &axis, &value)) {
      REXLOG_WARN("[autopress] unknown button in '{}'", item);
      continue;
    }
    presses.push_back({t, t + hold, bit, axis, value});
    REXLOG_INFO("[autopress] {} at {:.2f} s for {:.2f} s", item.substr(0, at), t, hold);
  }
}

// Buttons held now; 0 when FC3_AUTOPRESS is unset. Caller holds the mutex.
uint16_t MaskLocked() {
  if (!parsed) ParseLocked();
  if (presses.empty()) return 0;
  double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - kStart).count();
  uint16_t mask = 0;
  for (const auto& p : presses) {
    if (now >= p.at && now < p.until) mask |= p.buttons;
  }
  return mask;
}

// Stick/trigger values held now (axis order as in Press). Caller holds the mutex.
bool AxesLocked(int out[6]) {
  if (!parsed) ParseLocked();
  bool any = false;
  double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - kStart).count();
  for (const auto& p : presses) {
    if (p.axis >= 0 && now >= p.at && now < p.until) { out[p.axis] = p.value; any = true; }
  }
  return any;
}

#if !REX_PLATFORM_SWITCH
// Route playback (PC): "ms<TAB>buttons_hex<TAB>lx ly rx ry" lines, started input_play_auto_s after the first poll.
struct Muestra {
  uint32_t ms;
  int lx, ly, rx, ry;
};
std::vector<Muestra> ruta;
bool ruta_leida = false, ruta_iniciada = false, ruta_terminada = false;
std::chrono::steady_clock::time_point primer_sondeo{}, inicio_ruta{};

// The sticks now (true while playing). Caller holds the mutex.
bool RutaLocked(int out[4]) {
  if (!ruta_leida) {
    ruta_leida = true;
    const std::string path = REXCVAR_GET(input_play);
    if (!path.empty()) {
      if (FILE* f = std::fopen(path.c_str(), "r")) {
        Muestra m{};
        unsigned long long b = 0;
        while (std::fscanf(f, "%u\t%llx\t%d %d %d %d", &m.ms, &b, &m.lx, &m.ly, &m.rx, &m.ry) == 6) ruta.push_back(m);
        std::fclose(f);
      }
      REXLOG_INFO("input_play (pc): {} samples from {}", ruta.size(), path);
    }
    primer_sondeo = std::chrono::steady_clock::now();
  }
  if (ruta.empty() || ruta_terminada) return false;
  const auto ahora = std::chrono::steady_clock::now();
  if (!ruta_iniciada) {
    const int32_t auto_s = REXCVAR_GET(input_play_auto_s);
    if (auto_s <= 0 || ahora - primer_sondeo < std::chrono::seconds(auto_s)) return false;
    ruta_iniciada = true;
    inicio_ruta = ahora;
    REXLOG_INFO("input_play (pc): playback started after {} s", auto_s);
  }
  const uint32_t ms = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(ahora - inicio_ruta).count());
  if (ms > ruta.back().ms + 500) {
    ruta_terminada = true;
    REXLOG_INFO("input_play (pc): playback finished");
    return false;
  }
  const Muestra* actual = &ruta.front();
  for (const Muestra& m : ruta) {
    if (m.ms > ms) break;
    actual = &m;
  }
  out[0] = actual->lx;
  out[1] = actual->ly;
  out[2] = actual->rx;
  out[3] = actual->ry;
  return true;
}
#endif

uint16_t VirtualKeyOf(uint16_t bit) {
  switch (bit) {
    case X_INPUT_GAMEPAD_A: return 0x5800;
    case X_INPUT_GAMEPAD_B: return 0x5801;
    case X_INPUT_GAMEPAD_X: return 0x5802;
    case X_INPUT_GAMEPAD_Y: return 0x5803;
    case X_INPUT_GAMEPAD_RIGHT_SHOULDER: return 0x5804;
    case X_INPUT_GAMEPAD_LEFT_SHOULDER: return 0x5805;
    case X_INPUT_GAMEPAD_DPAD_UP: return 0x5810;
    case X_INPUT_GAMEPAD_DPAD_DOWN: return 0x5811;
    case X_INPUT_GAMEPAD_DPAD_LEFT: return 0x5812;
    case X_INPUT_GAMEPAD_DPAD_RIGHT: return 0x5813;
    case X_INPUT_GAMEPAD_START: return 0x5814;
    case X_INPUT_GAMEPAD_BACK: return 0x5815;
    case X_INPUT_GAMEPAD_LEFT_THUMB: return 0x5816;
    case X_INPUT_GAMEPAD_RIGHT_THUMB: return 0x5817;
    default: return 0;
  }
}
}  // namespace autopress

}  // namespace

InputSystem::InputSystem(rex::ui::Window* window) : window_(window) {}

InputSystem::~InputSystem() = default;

X_STATUS InputSystem::Setup() {
  return X_STATUS_SUCCESS;
}

void InputSystem::Shutdown() {
  // device_owners_ holds raw driver pointers.
  devices_.clear();
  device_owners_.clear();
  drivers_.clear();
}

void InputSystem::AddDriver(std::unique_ptr<InputDriver> driver) {
  drivers_.push_back(std::move(driver));
}

void InputSystem::AttachWindow(rex::ui::Window* window) {
  window_ = window;
  for (auto& driver : drivers_) {
    driver->OnWindowAvailable(window);
  }
}

void InputSystem::SetActiveCallback(std::function<bool()> callback) {
  for (auto& driver : drivers_) {
    driver->set_is_active_callback(callback);
  }
}

void InputSystem::SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment) {
  assignment_ = std::move(assignment);
  if (assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
}

void InputSystem::RefreshDevices() {
  /*
   * At most one enumeration every 50 ms.
   *
   * Stack sampling found the thread that prepares each game frame spending 5.8 % of its time
   * inside SetState (vibration), and every game call to the controller went through here:
   * three new vectors and a query to each driver, several times per frame. A controller is not
   * connected or disconnected 30 times per second; with 50 ms a new controller shows up before
   * anyone notices. The first call always enumerates.
   */
  {
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    const int64_t last = last_refresh_ns_.load(std::memory_order_relaxed);
    if (last != 0 && !devices_.empty() && now - last < 50000000) {
      return;
    }
    last_refresh_ns_.store(now, std::memory_order_relaxed);
  }
  std::vector<DeviceInfo> seen;
  std::vector<InputDriver*> owners;
  std::vector<DeviceInfo> enumerated;
  for (auto& driver : drivers_) {
    enumerated.clear();
    driver->EnumerateDevices(enumerated);
    for (auto& info : enumerated) {
      seen.push_back(info);
      owners.push_back(driver.get());
    }
  }

  // Carry forward ordinals already handed out, so a device keeps its guest user
  // when another pad is unplugged.
  bool changed = seen.size() != devices_.size();
  std::vector<bool> fresh(seen.size(), false);
  for (size_t i = 0; i < seen.size(); i++) {
    auto existing = std::find_if(devices_.begin(), devices_.end(),
                                 [&](const DeviceInfo& d) { return d.id == seen[i].id; });
    if (existing != devices_.end()) {
      seen[i].ordinal = existing->ordinal;
      continue;
    }
    fresh[i] = true;
    changed = true;
  }

  // Runs after the carry-forward pass so a new device cannot take an ordinal a
  // live one is still holding. Lowest free rather than a growing counter,
  // because a reconnected pad arrives as a new device and would otherwise walk
  // off the end of the guest users.
  for (size_t i = 0; i < seen.size(); i++) {
    if (!fresh[i]) {
      continue;
    }
    // Only physical devices consume an ordinal, so the first pad to connect is
    // guest user 0 however many synthetic devices enumerated ahead of it.
    if (seen[i].synthetic) {
      seen[i].ordinal = kSyntheticOrdinal;
      fresh[i] = false;
      continue;
    }
    auto taken = [&](uint32_t candidate) {
      for (size_t j = 0; j < seen.size(); j++) {
        if (!fresh[j] && !seen[j].synthetic && seen[j].ordinal == candidate) {
          return true;
        }
      }
      return false;
    };
    uint32_t ordinal = 0;
    while (taken(ordinal)) {
      ordinal++;
    }
    seen[i].ordinal = ordinal;
    fresh[i] = false;
  }

  for (const auto& old : devices_) {
    if (std::none_of(seen.begin(), seen.end(),
                     [&](const DeviceInfo& d) { return d.id == old.id; })) {
      active_devices_.Forget(old.id);
      changed = true;
    }
  }

  std::vector<size_t> order(seen.size());
  for (size_t i = 0; i < order.size(); i++) {
    order[i] = i;
  }
  // Stable: synthetic devices share one ordinal, and their relative order
  // decides which answers GetCapabilities when no pad is attached.
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return seen[a].ordinal < seen[b].ordinal; });

  devices_.clear();
  device_owners_.clear();
  for (size_t i : order) {
    devices_.push_back(seen[i]);
    device_owners_.push_back(owners[i]);
  }

  if (changed && assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
  // sa2-nx: drivers with per-player settings (the Switch's per-player button maps) learn each device's player.
  if (changed) {
    for (auto& driver : drivers_) {
      driver->OnDevicesAssigned(devices_);
    }
  }
}

InputDriver* InputSystem::DriverForDevice(DeviceId id) {
  for (size_t i = 0; i < devices_.size(); i++) {
    if (devices_[i].id == id) {
      return device_owners_[i];
    }
  }
  return nullptr;
}

const DeviceInfo* InputSystem::DeviceInfoFor(DeviceId id) const {
  for (const auto& device : devices_) {
    if (device.id == id) {
      return &device;
    }
  }
  return nullptr;
}

X_RESULT InputSystem::GetCapabilities(uint32_t user_index, uint32_t flags,
                                      X_INPUT_CAPABILITIES* out_caps) {
  SCOPE_profile_cpu_f("hid");
  if (!out_caps || !assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);
  if (ids.empty()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Prefer the pad in hand, so button glyphs follow it rather than whichever
  // device enumerated first.
  DeviceId chosen = active_devices_.Active(user_index);
  if (std::find(ids.begin(), ids.end(), chosen) == ids.end()) {
    chosen = ids.front();
  }

  auto* driver = DriverForDevice(chosen);
  if (!driver) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return driver->GetDeviceCapabilities(chosen, flags, out_caps);
}

X_RESULT InputSystem::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  SCOPE_profile_cpu_f("hid");
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  X_INPUT_STATE merged = {};
  bool any = false;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    if (!driver) {
      continue;
    }
    X_INPUT_STATE state = {};
    if (driver->GetDeviceState(id, &state) != X_ERROR_SUCCESS) {
      continue;
    }
    active_devices_.Observe(user_index, id, state.gamepad);
    if (!any) {
      merged = state;
      any = true;
    } else {
      MergeInto(merged, state);
    }
  }

  if (!any) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (user_index == 0) {  // PROTOTYPE autopress (see namespace autopress above)
    std::lock_guard<std::mutex> lock(autopress::mutex);
    uint16_t mask = autopress::MaskLocked();
    if (mask != autopress::last_state_mask) {
      autopress::packet_bump++;
      autopress::last_state_mask = mask;
      REXLOG_INFO("[autopress] XamInputGetState buttons 0x{:04X}", mask);
    }
    int axes[6] = {0, 0, 0, 0, 0, 0};
    const bool any_axis = autopress::AxesLocked(axes);
    if (any_axis) autopress::packet_bump++;  // a moving stick is a new state every poll
    if (mask || any_axis || autopress::packet_bump) {
      merged.gamepad.buttons = static_cast<uint16_t>(static_cast<uint16_t>(merged.gamepad.buttons) | mask);
      merged.packet_number = static_cast<uint32_t>(merged.packet_number) + autopress::packet_bump;
    }
    if (any_axis) {
      if (axes[0]) merged.gamepad.thumb_lx = static_cast<int16_t>(axes[0]);
      if (axes[1]) merged.gamepad.thumb_ly = static_cast<int16_t>(axes[1]);
      if (axes[2]) merged.gamepad.thumb_rx = static_cast<int16_t>(axes[2]);
      if (axes[3]) merged.gamepad.thumb_ry = static_cast<int16_t>(axes[3]);
      if (axes[4]) merged.gamepad.left_trigger = static_cast<uint8_t>(axes[4]);
      if (axes[5]) merged.gamepad.right_trigger = static_cast<uint8_t>(axes[5]);
    }
#if !REX_PLATFORM_SWITCH
    int ruta_ejes[4];
    if (autopress::RutaLocked(ruta_ejes)) {  // the recorded route replaces the sticks completely
      merged.gamepad.thumb_lx = static_cast<int16_t>(ruta_ejes[0]);
      merged.gamepad.thumb_ly = static_cast<int16_t>(ruta_ejes[1]);
      merged.gamepad.thumb_rx = static_cast<int16_t>(ruta_ejes[2]);
      merged.gamepad.thumb_ry = static_cast<int16_t>(ruta_ejes[3]);
      merged.packet_number = static_cast<uint32_t>(merged.packet_number) + (++autopress::packet_bump);
    }
#endif
  }
  if (out_state) {
    *out_state = merged;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT InputSystem::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  SCOPE_profile_cpu_f("hid");
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  // Every pad on this user belongs to the same player, so all of them buzz.
  // Only pads decide the result: synthetic devices accept any vibration and
  // would otherwise report success for a pad that never rumbled.
  bool any_pad = false;
  bool any_synthetic = false;
  bool pad_rumbled = false;
  X_RESULT pad_error = X_ERROR_DEVICE_NOT_CONNECTED;
  // input_rumble = false: the game still sees its call serviced, but the controllers only
  // receive zero vibration.
  X_INPUT_VIBRATION no_rumble{};
  if (!REXCVAR_GET(input_rumble)) {
    vibration = &no_rumble;
  }
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    const DeviceInfo* info = DeviceInfoFor(id);
    if (!driver || !info) {
      continue;
    }
    X_RESULT result = driver->SetDeviceVibration(id, vibration);
    if (info->synthetic) {
      any_synthetic = true;
      continue;
    }
    any_pad = true;
    if (result == X_ERROR_SUCCESS) {
      pad_rumbled = true;
    } else {
      pad_error = result;
    }
  }
  if (any_pad) {
    return pad_rumbled ? X_ERROR_SUCCESS : pad_error;
  }
  return any_synthetic ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT InputSystem::GetKeystroke(uint32_t user_index, uint32_t flags,
                                   X_INPUT_KEYSTROKE* out_keystroke) {
  SCOPE_profile_cpu_f("hid");
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  if (user_index == 0 && out_keystroke) {  // PROTOTYPE autopress: one KEYDOWN/KEYUP per changed button
    std::lock_guard<std::mutex> lock(autopress::mutex);
    uint16_t mask = autopress::MaskLocked();
    uint16_t diff = static_cast<uint16_t>(mask ^ autopress::last_key_mask);
    if (diff) {
      uint16_t bit = static_cast<uint16_t>(diff & (~diff + 1));
      bool down = (mask & bit) != 0;
      autopress::last_key_mask = static_cast<uint16_t>(autopress::last_key_mask ^ bit);
      std::memset(out_keystroke, 0, sizeof(*out_keystroke));
      out_keystroke->virtual_key = autopress::VirtualKeyOf(bit);
      out_keystroke->flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
      out_keystroke->user_index = 0;
      REXLOG_INFO("[autopress] XamInputGetKeystroke vk 0x{:04X} {}", autopress::VirtualKeyOf(bit),
                  down ? "down" : "up");
      return X_ERROR_SUCCESS;
    }
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  bool any_connected = false;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    if (!driver) {
      continue;
    }
    X_RESULT result = driver->GetDeviceKeystroke(id, flags, out_keystroke);
    if (result == X_ERROR_SUCCESS) {
      out_keystroke->user_index = static_cast<uint8_t>(user_index);
      return result;
    }
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode) {
  auto input = std::make_unique<InputSystem>(nullptr);

  if (!tool_mode) {
#if REX_PLATFORM_WIN32
    if (REXCVAR_GET(input_backend) == "xinput") {
      auto xinput_driver = std::make_unique<xinput::XinputInputDriver>(nullptr, 0);
      if (xinput_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(xinput_driver));
      }
    }
#endif

#if REX_PLATFORM_SWITCH
    if (REXCVAR_GET(input_backend) == "switch") {
      auto switch_driver = std::make_unique<nx::SwitchInputDriver>(nullptr, 0);
      if (switch_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(switch_driver));
      }
    }
#else
    if (REXCVAR_GET(input_backend) == "sdl") {
      auto sdl_driver = std::make_unique<sdl::SDLInputDriver>(nullptr, 0);
      if (sdl_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(sdl_driver));
      }
    }
#endif

    // MnK driver (keyboard/mouse -> controller emulation)
    auto mnk_driver = std::make_unique<mnk::MnkInputDriver>(nullptr, 0);
    if (mnk_driver->Setup() == X_STATUS_SUCCESS) {
      input->AddDriver(std::move(mnk_driver));
    }
  }

  // NOP driver (primary in tool mode, fallback otherwise)
  uint8_t nop_index = tool_mode ? 0 : 1;
  input->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr, nop_index));
  input->SetDeviceAssignment(std::make_unique<SlotAssignment>());
  return input;
}

}  // namespace rex::input
