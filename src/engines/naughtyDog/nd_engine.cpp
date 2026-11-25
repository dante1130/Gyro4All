// Naughty Dog's engine: Uncharted 4 (Legacy of Thieves Collection, Steam u4.exe; MODLOG 2026-11-25). Native x64, D3D12,
// MSVC RTTI kept, Y up. The game reads PlayStation pads itself (HID) and Xbox pads through XInput 1.4.
//
// The camera: CameraManager objects, found through a two-entry table (exe+0x68af530 in that build: 16-byte entries,
// handle pointer then a 32-bit id; the manager is [handle+8], valid when its dword at +0xbc is the id; the code reading
// it is found by a signature). Each manager keeps a list of camera controls at +0x100 (node +8 next, +0x10 handle, +0x18
// id; the handle's byte at +0x24 must be 0; the control is [handle+8] with the same id at +0xbc). The active control is
// the first whose state (dword +0x1b0) isn't 1, 3 or 4 (the game's own lookup, exe+0x1afdaa0). Manager 0 is the
// player's view.
//
// The controls keep their own view, and the game keeps writes to it (write tests in game):
//   CameraControlStrafe (on foot): a level unit direction at +0x300 (x, 0, z) and the pitch as a 0..1 setting at +0x4cc
//   (copy at +0x4c0), 0 = up, 1 = down, mapped to the view's pitch by kStrafePitch (measured, not linear).
//   The classes derived from CameraControlStrafe (RTTI: Stick, StrafeMelee, Evade, Edge, Balance, OffBalance,
//   KnockedDown, FreeRope, StrafeSurfaceSwim, StrafeSwimUnderwater) are turned the same way.
//   CameraControlZoom (aiming, L2): a 3D unit look direction at +0x750 (y = sine of the pitch).
//   CameraControlCover (in cover): target and current yaw directions and pitch angles (kCoverYawTarget...).
// A turn to the right lowers the heading atan2(x, z) of both directions (mouse input injected, in game).
//
// The controls are made and freed with the camera modes and move in memory while active (pool compaction: the same
// control slides down 0x680 bytes every few hundred ms), so they are never kept. The game updates the active control
// through its vtable slot 62 (+0x1f0): Gyro4All patches that slot in both classes' vtables (found from their RTTI), and
// the hook adds the turn the frame thread left pending to the control's view right before the game's own update reads it.
// That update runs on the game's threads, not on the window pump: Gyro4All's frame runs on a thread of its own, woken by
// each active update (every 20 ms without one: menus, cutscenes, other cameras).
#include "naughtydog.hpp"
#include "nd_view.hpp"

#include "engine.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "rtti.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gyro4all::naughtydog {

namespace {

constexpr double kPi = 3.14159265358979323846;

// The game's update of a camera control: vtable slot 62 (+0x1f0), called as (this, update info). We forward every
// register argument because only the first two are known.
constexpr int kUpdateSlot = 0x1f0 / 8;
using UpdateFn = void*(__fastcall*)(void*, void*, void*, void*);

// CameraManager and its control list.
constexpr uintptr_t kManagerId = 0xbc, kManagerList = 0x100;
constexpr uintptr_t kNodeNext = 0x8, kNodeHandle = 0x10, kNodeId = 0x18, kHandleDead = 0x24, kHandleObject = 0x8;
constexpr uintptr_t kControlId = 0xbc, kControlState = 0x1b0;
constexpr int kMaxNodes = 64;

// CameraControlStrafe: level yaw direction, pitch setting (and its copy).
constexpr uintptr_t kStrafeYaw = 0x300, kStrafePitch = 0x4cc, kStrafePitchCopy = 0x4c0;
// CameraControlZoom: 3D look direction. We keep its pitch within this (degrees) because the game's own limit is unknown.
constexpr uintptr_t kZoomLook = 0x750;
constexpr double kZoomMaxPitch = 75;

// CameraControlCover: the view eases toward a target. Yaw: level directions, target +0x4a0 and current +0x490. Pitch:
// degrees, target +0x484 and current +0x480, + looks down, clamped by the game to 80 (about one view degree each near
// level). We turn both so that the view follows the gyro at once.
constexpr uintptr_t kCoverYawTarget = 0x4a0, kCoverYawCurrent = 0x490, kCoverPitchTarget = 0x484, kCoverPitchCurrent = 0x480;
constexpr double kCoverMaxPitch = 80;

// The directions' heading atan2(x, z) falls when the view turns right (Strafe and Zoom; Cover: a write the same way
// turned the view the same way).
constexpr double kYawSense = -1;

// Without an active control update for this long, the player camera isn't in control (ms).
constexpr ULONGLONG kLiveMs = 100;

enum class Kind : int { None = 0, Strafe = 1, Zoom = 2, Cover = 3 };

// The camera controls the gyro turns: CameraControlStrafe and the classes derived from it (their RTTI: same layout),
// and CameraControlZoom.
struct ControlClass {
    const char* rtti;
    const char* name;
    Kind kind;
};
constexpr std::array<ControlClass, 13> kClasses = {{
    {".?AVCameraControlStrafe@@", "CameraControlStrafe", Kind::Strafe},
    {".?AVCameraControlZoom@@", "CameraControlZoom", Kind::Zoom},
    {".?AVCameraControlStick@@", "CameraControlStick", Kind::Strafe},
    {".?AVCameraControlStrafeMelee@@", "CameraControlStrafeMelee", Kind::Strafe},
    {".?AVCameraControlEvade@@", "CameraControlEvade", Kind::Strafe},
    {".?AVCameraControlEdge@@", "CameraControlEdge", Kind::Strafe},
    {".?AVCameraControlBalance@@", "CameraControlBalance", Kind::Strafe},
    {".?AVCameraControlOffBalance@@", "CameraControlOffBalance", Kind::Strafe},
    {".?AVCameraControlKnockedDown@@", "CameraControlKnockedDown", Kind::Strafe},
    {".?AVCameraControlFreeRope@@", "CameraControlFreeRope", Kind::Strafe},
    {".?AVCameraControlStrafeSurfaceSwim@@", "CameraControlStrafeSurfaceSwim", Kind::Strafe},
    {".?AVCameraControlStrafeSwimUnderwater@@", "CameraControlStrafeSwimUnderwater", Kind::Strafe},
    {".?AVCameraControlCover@@", "CameraControlCover", Kind::Cover},
}};

// --- the game's lookups -----------------------------------------------------------------------------------------

uintptr_t g_manager_table = 0;  // the two-entry manager table
uintptr_t g_gamepad_flag = 0;   // byte: 1 = the last input came from a gamepad (the camera then smooths the view)
std::array<uintptr_t, kClasses.size()> g_vtables{};  // of kClasses (0: not in this exe)

uintptr_t read_ptr(uintptr_t at) {
    uintptr_t v = 0;
    return mem::rd(at, v) ? v : 0;
}

uint32_t read_u32(uintptr_t at) {
    uint32_t v = 0;
    return mem::rd(at, v) ? v : 0;
}

// Manager 0, the player's view (exe+0x1af6f50 in the game).
uintptr_t player_manager() {
    if (!g_manager_table) return 0;
    const uintptr_t handle = read_ptr(g_manager_table);
    const uint32_t id = read_u32(g_manager_table + 8);
    const uintptr_t manager = handle && id ? read_ptr(handle + kHandleObject) : 0;
    return manager && read_u32(manager + kManagerId) == id ? manager : 0;
}

// The manager's active control (exe+0x1afdaa0 in the game, with its second argument 0).
uintptr_t active_control(uintptr_t manager) {
    const uintptr_t head = read_ptr(manager + kManagerList);
    if (!head) return 0;
    uintptr_t node = read_ptr(head + kNodeNext);
    for (int i = 0; i < kMaxNodes && node && node != head; ++i, node = read_ptr(node + kNodeNext)) {
        const uintptr_t handle = read_ptr(node + kNodeHandle);
        const uint32_t id = read_u32(node + kNodeId);
        uint8_t dead = 1;
        if (!handle || !id || !mem::rd(handle + kHandleDead, dead) || dead) continue;
        const uintptr_t control = read_ptr(handle + kHandleObject);
        if (!control || read_u32(control + kControlId) != id) continue;
        const uint32_t state = read_u32(control + kControlState);
        if (state != 1 && state != 3 && state != 4) return control;
    }
    return 0;
}

// --- turning a control's view -----------------------------------------------------------------------------------

void turn_strafe(uintptr_t control, double pitch, double yaw) {
    std::array<float, 3> dir{};
    float setting = 0;
    if (!mem::rd(control + kStrafeYaw, dir) || !mem::rd(control + kStrafePitch, setting) || !std::isfinite(dir[0]) ||
        !std::isfinite(dir[2]) || !std::isfinite(setting))
        return;
    turn_heading(dir[0], dir[2], kYawSense * yaw);
    const float new_setting = static_cast<float>(strafe_setting(strafe_degrees(setting) + pitch));
    mem::guarded_store(control + kStrafeYaw, dir.data(), sizeof(float) * dir.size());
    mem::guarded_store(control + kStrafePitch, &new_setting, sizeof new_setting);
    mem::guarded_store(control + kStrafePitchCopy, &new_setting, sizeof new_setting);
}

void turn_zoom(uintptr_t control, double pitch, double yaw) {
    std::array<float, 3> dir{};
    if (!mem::rd(control + kZoomLook, dir) || !std::isfinite(dir[0]) || !std::isfinite(dir[1]) || !std::isfinite(dir[2]))
        return;
    turn_heading(dir[0], dir[2], kYawSense * yaw);
    const double level = std::hypot(static_cast<double>(dir[0]), static_cast<double>(dir[2]));
    if (level < 1e-4) return;
    const double elevation = std::atan2(static_cast<double>(dir[1]), level) * 180 / kPi;
    const double new_elevation = std::clamp(elevation + pitch, -kZoomMaxPitch, kZoomMaxPitch) * kPi / 180;
    const double c = std::cos(new_elevation);
    const std::array<float, 3> out = {static_cast<float>(dir[0] / level * c), static_cast<float>(std::sin(new_elevation)),
                                      static_cast<float>(dir[2] / level * c)};
    mem::guarded_store(control + kZoomLook, out.data(), sizeof(float) * out.size());
}

void turn_cover(uintptr_t control, double pitch, double yaw) {
    for (const uintptr_t at : {kCoverYawTarget, kCoverYawCurrent}) {
        std::array<float, 3> dir{};
        if (!mem::rd(control + at, dir) || !std::isfinite(dir[0]) || !std::isfinite(dir[2])) return;
        turn_heading(dir[0], dir[2], kYawSense * yaw);
        mem::guarded_store(control + at, dir.data(), sizeof(float) * dir.size());
    }
    for (const uintptr_t at : {kCoverPitchTarget, kCoverPitchCurrent}) {
        float angle = 0;
        if (!mem::rd(control + at, angle) || !std::isfinite(angle)) return;
        const auto out = static_cast<float>(std::clamp(static_cast<double>(angle) - pitch, -kCoverMaxPitch, kCoverMaxPitch));
        mem::guarded_store(control + at, &out, sizeof out);
    }
}

// --- the update hook and the frame thread -----------------------------------------------------------------------

std::array<UpdateFn, kClasses.size()> g_update_orig{};
bool g_hooked = false;
Engine::FrameFn g_frame = nullptr;
HANDLE g_update_event = nullptr;            // auto-reset: an active control's update
std::atomic<ULONGLONG> g_active_seen{0};    // tick of the last update of an active Strafe or Zoom control
std::atomic<int> g_active_class{-1};        // its index in kClasses
std::atomic<bool> g_turn_live{false};       // the frame thread's verdict: turn the active control
SRWLOCK g_turn_lock = SRWLOCK_INIT;
double g_pending_pitch = 0, g_pending_yaw = 0;  // degrees, from the frame thread, for the next active update

void on_update(void* self, size_t index) {
    const Kind kind = kClasses[index].kind;
    const auto control = reinterpret_cast<uintptr_t>(self);
    // We only turn the player's active control because the game also updates the controls it blends out.
    const uintptr_t manager = player_manager();
    if (!manager || active_control(manager) != control) return;
    g_active_class.store(static_cast<int>(index), std::memory_order_relaxed);
    g_active_seen.store(GetTickCount64(), std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_turn_lock);
    const double pitch = g_pending_pitch, yaw = g_pending_yaw;
    g_pending_pitch = g_pending_yaw = 0;
    ReleaseSRWLockExclusive(&g_turn_lock);
    // We show the camera mouse mode because its gamepad mode smooths the view and pulls it back after fast turns,
    // which fights the gyro; the game sets the flag again from its own input every frame, so the stick still works.
    const bool live = g_turn_live.load(std::memory_order_relaxed);
    if (live && g_gamepad_flag) {
        const uint8_t mouse = 0;
        mem::guarded_store(g_gamepad_flag, &mouse, sizeof mouse);
    }
    if (live && (pitch != 0 || yaw != 0)) {
        if (kind == Kind::Strafe) turn_strafe(control, pitch, yaw);
        else if (kind == Kind::Cover) turn_cover(control, pitch, yaw);
        else turn_zoom(control, pitch, yaw);
    }
    SetEvent(g_update_event);
}

template <size_t K>
void* __fastcall hooked_update(void* self, void* a, void* b, void* c) {
    on_update(self, K);
    return g_update_orig[K](self, a, b, c);
}

bool hook(size_t k, void* fn) {
    if (!g_vtables[k]) return false;
    void* original = rtti::patch_slot(g_vtables[k], kUpdateSlot, fn);
    if (!original) return false;
    g_update_orig[k] = reinterpret_cast<UpdateFn>(original);
    log("naughtydog: %s update hooked (vtable %s slot %d, was %s)", kClasses[k].name, mem::module_offset(g_vtables[k]).c_str(),
        kUpdateSlot, mem::module_offset(reinterpret_cast<uintptr_t>(original)).c_str());
    return true;
}

template <size_t... K>
size_t hook_all(std::index_sequence<K...>) {
    return (size_t{hook(K, reinterpret_cast<void*>(&hooked_update<K>))} + ...);
}

DWORD WINAPI frame_thread(void*) {
    log("naughtydog: gyro frame thread %lu, woken by each active camera update (20 ms without one)", GetCurrentThreadId());
    for (;;) {
        WaitForSingleObject(g_update_event, 20);
        g_frame();
    }
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

uintptr_t exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

class NaughtyDogEngine final : public Engine {
public:
    const char* name() const override { return "NaughtyDog"; }

    // Uncharted 4: <root>\u4*.exe with <root>\Uncharted4_data.
    bool detect(const std::wstring& exe_path) const override {
        const size_t slash = exe_path.find_last_of(L"\\/");
        const std::wstring dir = exe_path.substr(0, slash), exe = exe_path.substr(slash + 1);
        return _wcsnicmp(exe.c_str(), L"u4", 2) == 0 && exists(dir + L"\\Uncharted4_data");
    }

    bool frame_source(FrameFn frame) override {
        g_frame = frame;
        if (!g_update_event) return false;
        if (HANDLE thread = CreateThread(nullptr, 0, &frame_thread, nullptr, 0, nullptr)) {
            CloseHandle(thread);
            return true;
        }
        log("naughtydog: can't start the gyro frame thread (error %lu); no gyro frame", GetLastError());
        return false;
    }

    void configure() override {
        // The code reading the manager table (exe+0x1af6fa3): mov rax,rbx; lea rcx,[table]; add rax,rax;
        // mov rdx,[rcx+rax*8]; test rdx,rdx; jz; mov ecx,[rcx+rax*8+8].
        const char* kTable = "48 8B C3 48 8D 0D ?? ?? ?? ?? 48 03 C0 48 8B 14 C1 48 85 D2 74 ?? 8B 4C C1 08";
        std::vector<uint8_t> bytes, mask;
        mem::parse_pattern(kTable, bytes, mask);
        const auto hits = mem::find_pattern(mem::code_sections(exe_base()), bytes.data(), mask.data(), bytes.size());
        // We accept several matches when they all read the same table, because the game has two copies of this lookup.
        for (const uintptr_t at : hits) {
            int32_t disp = 0;
            const uintptr_t table = mem::rd(at + 6, disp) ? at + 10 + disp : 0;
            if (at == hits.front()) g_manager_table = table;
            else if (table != g_manager_table) g_manager_table = 0;
            if (!g_manager_table) break;
        }
        // The code storing the gamepad flag (exe+0x1edddee): vmovsd [rsi+18h],xmm0; mov dword [rsi+20h],1.0;
        // mov [flag],dil.
        const char* kFlag = "C5 FB 11 46 18 C7 46 20 00 00 80 3F 40 88 3D ?? ?? ?? ??";
        std::vector<uint8_t> flag_bytes, flag_mask;  // parse_pattern appends: the table's buffers can't be reused
        mem::parse_pattern(kFlag, flag_bytes, flag_mask);
        const auto flag_hits =
            mem::find_pattern(mem::code_sections(exe_base()), flag_bytes.data(), flag_mask.data(), flag_bytes.size());
        int32_t flag_disp = 0;
        if (flag_hits.size() == 1 && mem::rd(flag_hits[0] + 15, flag_disp)) g_gamepad_flag = flag_hits[0] + 19 + flag_disp;
        log("naughtydog: gamepad camera flag %s (%zu code match(es))%s",
            g_gamepad_flag ? mem::module_offset(g_gamepad_flag).c_str() : "not found", flag_hits.size(),
            g_gamepad_flag ? ": the camera kept in mouse mode while in control" : ": the gamepad camera may smooth the gyro");
        const rtti::Image im = rtti::exe_image();
        for (size_t k = 0; k < kClasses.size(); ++k) g_vtables[k] = rtti::primary_vtable(im, kClasses[k].rtti);
        log("naughtydog: camera manager table %s (%zu code match(es)), %zu of %zu camera control classes found",
            g_manager_table ? mem::module_offset(g_manager_table).c_str() : "not found", hits.size(),
            static_cast<size_t>(std::ranges::count_if(g_vtables, [](uintptr_t vt) { return vt != 0; })), kClasses.size());
        // We make the update event here because configure runs before frame_source, and the hooks need it.
        g_update_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_manager_table || !g_update_event) {
            log("naughtydog: %s: gyro as right stick", g_manager_table ? "no update event" : "no camera manager table");
            return;
        }
        g_hooked = hook_all(std::make_index_sequence<kClasses.size()>{}) > 0;
        if (!g_hooked) log("naughtydog: no camera control update hooked: gyro as right stick");
    }

    std::vector<ViewInfo> views() const override { return {{1, "On foot", 0, false}, {2, "Aim", 1, false}}; }

    EngineFrame read_frame() override {
        EngineFrame f;
        f.in_game = true;
        if (!g_hooked) {
            f.no_camera = true;
            f.view_id = 1;
            f.target = "no camera hook (gyro as right stick)";
            return f;
        }
        const bool live = GetTickCount64() - g_active_seen.load(std::memory_order_relaxed) <= kLiveMs;
        const int index = live ? g_active_class.load(std::memory_order_relaxed) : -1;
        const Kind shown = index >= 0 ? kClasses[static_cast<size_t>(index)].kind : Kind::None;
        if (index != shown_class_) {
            if (index >= 0) log("naughtydog: %s in control (%s view)", kClasses[static_cast<size_t>(index)].name,
                                shown == Kind::Zoom ? "aim" : "on-foot");
            else log("naughtydog: player camera not in control (menu, cutscene or another camera)");
            shown_class_ = index;
        }
        shown_ = shown;
        g_turn_live.store(shown != Kind::None, std::memory_order_relaxed);
        if (shown == Kind::None) {
            AcquireSRWLockExclusive(&g_turn_lock);
            g_pending_pitch = g_pending_yaw = 0;
            ReleaseSRWLockExclusive(&g_turn_lock);
        }
        f.view_id = shown == Kind::Strafe ? 1 : shown == Kind::Zoom ? 2 : 0;
        f.target = index >= 0 ? kClasses[static_cast<size_t>(index)].name : "another camera";
        return f;
    }

    // Degrees from GyroLib (pitch up, yaw right): left for the active control's next update to add.
    bool apply(double pitch, double yaw) override {
        if (shown_ == Kind::None) return false;
        AcquireSRWLockExclusive(&g_turn_lock);
        g_pending_pitch += pitch;
        g_pending_yaw += yaw;
        ReleaseSRWLockExclusive(&g_turn_lock);
        return true;
    }

    // We can't tell menus from gameplay yet, other than by the camera updates stopping.
    bool sees_menus() const override { return false; }

    std::string status_scene() const override {
        if (!g_hooked) return "view=default (gyro as right stick)";
        return shown_ == Kind::Strafe ? "view=on foot" : shown_ == Kind::Zoom ? "view=aim" : "view=none (another camera)";
    }

private:
    Kind shown_ = Kind::None;
    int shown_class_ = -1;  // index in kClasses of the control in control (-1: none)
};

}  // namespace

Engine& engine() {
    static NaughtyDogEngine e;
    return e;
}

}  // namespace gyro4all::naughtydog
