// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <fmt/ranges.h>

#include "common/logging.h"
#include "common/oot_vr.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/svc/svc_nx2_oot_vr.h"
#include "core/memory.h"

namespace Kernel::Svc {
namespace {

constexpr u64 OotProgramId = 0x0400486024DEC000ULL;
constexpr u32 VrSvc = 0xF4;
constexpr u64 SiteOffset = 0x33A428;
// The camera controller update (main+0x4C48C0) has just written the camera's view object (x20) at
// main+0x4C5064; its next word, `ldr x8, [x19, #0x20]`, becomes svc 0xF5 (x19 = the controller).
constexpr u32 CameraSvc = 0xF5;
constexpr u64 CameraSiteOffset = 0x4C5068;
// Unused end of main's BSS, after Dark's state at main+0x4E9FE00 (0xA0 bytes).
constexpr u64 CopyOffset = 0x4E9FF00;
// {u32 mode, f32 yaw, f32 pitch} for tests, after the two view copies.
constexpr u64 TestPoseOffset = 0x4E9FFF0;
// The camera target update (main+0x32A4C40): `mov x21, x0` becomes svc 0xF6, x0 = the followed actor.
constexpr u32 TargetSvc = 0xF6;
constexpr u64 TargetSiteOffset = 0x32A4DE4;
constexpr u64 LinkVtableOffset = 0x4B25E18;
// The skinning matrices (main+0x14E0200): `mov x19, x0` becomes svc 0xF7, x0 = a skeleton.
constexpr u32 SkeletonSvc = 0xF7;
constexpr u64 SkeletonSiteOffset = 0x14E0220;
// The HUD code (tools/nx2/oot_vr_hud.s) calls svc 0xF8 every frame: x0 = hud_params, w1 = the item menu is shown.
constexpr u32 FrozenSvc = 0xF8;
constexpr u32 HeadBone = 103;
constexpr float HeadScale = 0.001f;
// Metres from Link's head bone (the top of his neck) up to the eye.
constexpr float EyeUpM = 0.15f;
// Eye forward per metre Link's head is below standing height (rolls, crawls); StandFollowM is per camera update.
constexpr float LowForwardPerM = 0.8f;
constexpr float MaxLowForwardM = 0.5f;
constexpr float StandFollowM = 0.0002f;
// Metres in front of the head bone, along Link's facing (EDEN_NX2_OOT_VR_EYE_FWD).
constexpr float EyeForwardM = 0.1f;constexpr float Deg = 0.017453292519943295f;
// The displayed camera's set-view call returns here (main+0x33A240); other view-0 callers are probes.
constexpr u64 DisplayCallerOffset = 0x33A2D8;
// The main projection object's vtable; its 4x4 matrix is at +0xC (P00) and +0xC + 20 (P11).
constexpr u64 ProjectionVtableOffset = 0x4BF8C70;
constexpr float MaxMoveM = 0.5f;
// The widened copies of the main projection object, one per view (0x100 bytes covers one). No game code addresses
// main's BSS past page main+0x4E9A000.
constexpr u64 ProjectionCopyOffset = 0x4E9F000;
constexpr u64 ProjectionCopySize = 0x100;
// Turned copies of views 1 and 2 (0x60 each), after the three projection copies.
constexpr u64 ViewCopyOffset = 0x4E9F300;
// {f32 size, u32 on} read by the VR HUD code (patch_manager.cpp, tools/nx2/oot_vr_hud.s).
constexpr u64 HudParamsOffset = 0x4E9F800;
constexpr u64 UiManagerPtrOffset = 0x4C7E098;

struct Head {
    std::array<float, 3> fwd;
    std::array<float, 3> pos;
    bool from_headset = false;
    Common::OotVr::HeadPose pose; ///< the headset pose, when from_headset
};

float Normalize(float* v) {
    const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 1e-6f) {
        v[0] /= n, v[1] /= n, v[2] /= n;
    }
    return n;
}

// m: 3 rows of 4, world to camera, looking along -z. Port of vr_camera_view in the Ocarina recomp.
void TurnView(float* m, const Head& head, float move) {
    float R[3], U[3], B[3], pos[3];
    const float t[3]{m[3], m[7], m[11]};
    for (int k = 0; k < 3; ++k) {
        R[k] = m[k], U[k] = m[4 + k], B[k] = m[8 + k];
    }
    for (int k = 0; k < 3; ++k) {
        pos[k] = -(R[k] * t[0] + U[k] * t[1] + B[k] * t[2]);
    }
    float flat[3]{B[0], 0, B[2]};
    if (Normalize(flat) > 1e-3f) {
        B[0] = flat[0], B[1] = 0, B[2] = flat[2];
        U[0] = 0, U[1] = 1, U[2] = 0;
        R[0] = B[2], R[1] = 0, R[2] = -B[0];
    }
    float d[3]{head.pos[0], head.pos[1], head.pos[2]};
    const float dm = std::fmin(Normalize(d), MaxMoveM) * move;
    for (int k = 0; k < 3; ++k) {
        pos[k] += (d[0] * R[k] + d[1] * U[k] + d[2] * B[k]) * dm;
    }
    const float* fh = head.fwd.data();
    float hr[3]{-fh[2], 0, fh[0]};
    if (Normalize(hr) <= 1e-6f) {
        hr[0] = 1, hr[1] = 0, hr[2] = 0;
    }
    const float hu[3]{hr[1] * fh[2] - hr[2] * fh[1], hr[2] * fh[0] - hr[0] * fh[2],
                      hr[0] * fh[1] - hr[1] * fh[0]};
    const float hb[3]{-fh[0], -fh[1], -fh[2]};
    const float* hv[3]{hr, hu, hb};
    for (int i = 0; i < 3; ++i) {
        float row[3];
        for (int k = 0; k < 3; ++k) {
            row[k] = hv[i][0] * R[k] + hv[i][1] * U[k] + hv[i][2] * B[k];
        }
        for (int k = 0; k < 3; ++k) {
            m[i * 4 + k] = row[k];
        }
        m[i * 4 + 3] = -(row[0] * pos[0] + row[1] * pos[1] + row[2] * pos[2]);
    }
}

// The head: a test pose if one is set, else the headset's. Test poses: EDEN_NX2_OOT_VR_FAKE_YAW /
// _PITCH (degrees) at launch, or {u32 1, f32 yaw, f32 pitch} (degrees) written at
// main+TestPoseOffset while the game runs (u32 2 turns it off). {u32 3, f32 x, y, z} sets the test head
// position in metres (x right, y up, z back) and keeps the last yaw and pitch.
bool GetHead(Core::Memory::Memory& memory, u64 main_base, bool display, Head& head) {
    static const char* const yaw_text = std::getenv("EDEN_NX2_OOT_VR_FAKE_YAW");
    static const char* const pitch_text = std::getenv("EDEN_NX2_OOT_VR_FAKE_PITCH");
    static float yaw = yaw_text ? static_cast<float>(std::atof(yaw_text)) : 0.0f;
    static float pitch = pitch_text ? static_cast<float>(std::atof(pitch_text)) : 0.0f;
    static std::array<float, 3> test_pos{};
    bool test = yaw_text || pitch_text;
    std::array<u32, 4> written{};
    memory.ReadBlock(main_base + TestPoseOffset, written.data(), sizeof(written));
    if (written[0] == 1) {
        std::memcpy(&yaw, &written[1], 4);
        std::memcpy(&pitch, &written[2], 4);
        test = true;
    } else if (written[0] == 3) {
        std::memcpy(test_pos.data(), &written[1], sizeof(test_pos));
        test = true;
    } else if (written[0] == 2) {
        return false;
    }
    if (test) {
        head.fwd = {-std::sin(yaw * Deg) * std::cos(pitch * Deg), std::sin(pitch * Deg),
                    -std::cos(yaw * Deg) * std::cos(pitch * Deg)};
        head.pos = test_pos;
        return true;
    }
    Common::OotVr::HeadPose pose;
    if (!Common::OotVr::HeadForCamera(pose, display)) {
        return false;
    }
    // Yaw and pitch only: the game camera has no roll.
    const auto [x, y, z, w] = pose.orientation;
    const float fx = -2 * (x * z + y * w), fy = -2 * (y * z - x * w), fz = -(1 - 2 * (x * x + y * y));
    const float hyaw = std::atan2(-fx, -fz), hpitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, fy)));
    head.fwd = {-std::sin(hyaw) * std::cos(hpitch), std::sin(hpitch), -std::cos(hyaw) * std::cos(hpitch)};
    head.pos = pose.position;
    // The pose the picture is drawn with: yaw about y, then pitch about x, no roll.
    const float sy = std::sin(hyaw / 2), cy = std::cos(hyaw / 2);
    const float sp = std::sin(hpitch / 2), cp = std::cos(hpitch / 2);
    pose.orientation = {cy * sp, sy * cp, -sy * sp, cy * cp};
    head.from_headset = true;
    head.pose = pose;
    return true;
}

// EDEN_NX2_OOT_VR_MOVE: game metres per metre of head movement (default 1).
float MoveScale() {
    static const float move = [] {
        const char* const v = std::getenv("EDEN_NX2_OOT_VR_MOVE");
        return v && *v ? static_cast<float>(std::atof(v)) : 1.0f;
    }();
    return move;
}

// EDEN_NX2_OOT_VR_SOURCE=0: turn only the final view in set-view (culling and shadows keep the
// game's own camera). Default: turn the camera's view object where the game builds it.
bool SourceMode() {
    static const bool on = [] {
        const char* const v = std::getenv("EDEN_NX2_OOT_VR_SOURCE");
        return !(v && *v == '0');
    }();
    return on;
}

// The displayed camera's view objects (set-view x2 - 8): the game double-buffers its camera.
std::array<std::atomic<u64>, 4> g_display_views{};
std::atomic<u32> g_display_next{};

bool IsDisplayView(u64 view_object) {
    for (const auto& v : g_display_views) {
        if (v.load(std::memory_order_relaxed) == view_object) {
            return true;
        }
    }
    return false;
}

void NoteDisplayView(u64 view_object) {
    if (!IsDisplayView(view_object)) {
        g_display_views[g_display_next.fetch_add(1, std::memory_order_relaxed) % g_display_views.size()]
            .store(view_object, std::memory_order_relaxed);
        LOG_INFO(Kernel_SVC, "OoT VR camera: displayed camera view object {:#x}", view_object);
    }
}
std::atomic<u64> g_display_projection{}; ///< the displayed camera's projection object (set-view x3)

// The head pose each displayed view object was last turned with, recorded once per drawn frame.
std::mutex g_view_pose_lock;
std::array<std::pair<u64, Common::OotVr::HeadPose>, 4> g_view_pose{};

void NoteViewPose(u64 view_object, const Common::OotVr::HeadPose& pose) {
    std::scoped_lock lk{g_view_pose_lock};
    for (auto& [object, saved] : g_view_pose) {
        if (object == view_object || object == 0) {
            object = view_object, saved = pose;
            return;
        }
    }
    g_view_pose[0] = {view_object, pose};
}

// The eye the next drawn frame's camera moves to; set-view flips it after each drawn frame.
std::atomic<int> g_next_eye{};

bool ViewPose(u64 view_object, Common::OotVr::HeadPose& pose) {
    std::scoped_lock lk{g_view_pose_lock};
    for (const auto& [object, saved] : g_view_pose) {
        if (object == view_object) {
            pose = saved;
            return true;
        }
    }
    return false;
}

std::atomic<u64> g_view_scene{}; ///< the scene that owns g_display_view: views at +0x138, count +0x130
std::atomic<u64> g_display_view{}; ///< the displayed camera's view (set-view x0); +0x54dc bit 0: drawn
std::atomic<u64> g_link{}; ///< Link's actor: feet float3 +0x31C, "Player" object +0x8
std::mutex g_head_lock;
std::array<float, 3> g_head{}; ///< Link's head bone in the world, metres
std::array<float, 3> g_head_feet{}; ///< Link's feet when g_head was taken
bool g_has_head = false;
std::array<float, 9> g_head_axes{}; ///< Link's head bone: 3 axes of 3 floats, under g_head_lock

// Link's body skeleton: actor +0x8 -> +0x70 parts -> +0xB0 body model -> +0x398.
u64 LinkSkeleton(Core::Memory::Memory& memory, u64 link) {
    const u64 player = memory.Read64(link + 0x8);
    const u64 parts = player ? memory.Read64(player + 0x70) : 0;
    const u64 model = parts ? memory.Read64(parts + 0xB0) : 0;
    return model ? memory.Read64(model + 0x398) : 0;
}

// Skeleton resource: bone count u16 +0x38, bones +0x10 (0x58 each, parent u16 +0x22).
const std::vector<u16>& HeadBones(Core::Memory::Memory& memory, u64 resource) {
    static u64 cached = 0;
    static std::vector<u16> bones;
    if (cached == resource) {
        return bones;
    }
    cached = resource;
    bones.clear();
    const u32 count = memory.Read16(resource + 0x38);
    const u64 table = memory.Read64(resource + 0x10);
    const auto parent = [&](u32 b) { return memory.Read16(table + b * 0x58 + 0x22); };
    if (count <= HeadBone || count > 1024 || parent(HeadBone) != HeadBone - 1) {
        LOG_WARNING(Kernel_SVC, "OoT VR camera: Link's skeleton has {} bones and no head bone where expected", count);
        return bones;
    }
    for (u32 b = 0; b < count; ++b) {
        u32 k = b;
        for (int depth = 0; k != HeadBone && k < count && depth < 64; ++depth) {
            k = parent(k);
        }
        if (k == HeadBone) {
            bones.push_back(static_cast<u16>(b));
        }
    }
    LOG_INFO(Kernel_SVC, "OoT VR camera: Link's skeleton has {} bones, {} in the head", count, bones.size());
    return bones;
}

// Projection row 2, floats 10 and 11 (row-major): near 0.6 m, far 1000 m. Link's shield and sword are nearer.
constexpr float CaptureNearM = 0.6f, CaptureFarM = 1000.0f;
constexpr std::array<float, 2> CaptureDepthRow{-(CaptureFarM + CaptureNearM) / (CaptureFarM - CaptureNearM),
                                               -2 * CaptureFarM * CaptureNearM / (CaptureFarM - CaptureNearM)};

// EDEN_NX2_OOT_VR_CAPTURE=1: Link and the HUD hidden, for room captures.
bool CaptureMode() {
    static const bool capture = [] {
        const char* const v = std::getenv("EDEN_NX2_OOT_VR_CAPTURE");
        return v && *v && *v != '0';
    }();
    return capture;
}

// Link's body moved 100 m down in one piece, out of the capture's view.
void MoveSkeletonAway(Core::Memory::Memory& memory, u64 skeleton) {
    const u64 world = memory.Read64(skeleton + 0x20);
    const u32 count = memory.Read16(memory.Read64(skeleton) + 0x38);
    std::array<float, 16> bone;
    for (u32 b = 0; b < count && count <= 1024; ++b) {
        memory.ReadBlock(world + 0x40 * b, bone.data(), sizeof(bone));
        bone[13] -= 100.0f;
        memory.WriteBlock(world + 0x40 * b, bone.data(), sizeof(bone));
    }
}

// World matrices at skeleton +0x20: 0x40 bytes per bone, 4 columns of 4 floats, from a local origin.
void LinkSkeletonUpdate(Core::Memory::Memory& memory, u64 skeleton) {
    const u64 link = g_link.load(std::memory_order_relaxed);
    if (!link || skeleton != LinkSkeleton(memory, link)) {
        return;
    }
    const u64 world = memory.Read64(skeleton + 0x20);
    std::array<float, 16> root, head;
    std::array<float, 3> feet;
    memory.ReadBlock(world, root.data(), sizeof(root));
    memory.ReadBlock(world + 0x40 * HeadBone, head.data(), sizeof(head));
    memory.ReadBlock(link + 0x31C, feet.data(), sizeof(feet));
    {
        std::scoped_lock lk{g_head_lock};
        for (int k = 0; k < 3; ++k) {
            g_head[k] = feet[k] + head[12 + k] - root[12 + k];
        }
        g_head_feet = feet;
        g_has_head = true;
        for (int c = 0; c < 3; ++c) {
            for (int k = 0; k < 3; ++k) {
                g_head_axes[c * 3 + k] = head[c * 4 + k];
            }
        }
    }
    if (CaptureMode()) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_INFO(Kernel_SVC, "OoT VR capture: Link's body hidden");
        }
        MoveSkeletonAway(memory, skeleton);
        return;
    }
    if (!Common::OotVr::FirstPerson()) {
        return;
    }
    std::array<float, 16> hidden{};
    hidden[0] = hidden[5] = hidden[10] = HeadScale;
    hidden[12] = head[12], hidden[13] = head[13], hidden[14] = head[14], hidden[15] = head[15];
    for (const u16 bone : HeadBones(memory, memory.Read64(skeleton))) {
        memory.WriteBlock(world + 0x40 * bone, hidden.data(), sizeof(hidden));
    }
}

// The skinning runs after the camera, so the head is last frame's: move it by Link's feet since then.
bool LinkHead(Core::Memory::Memory& memory, std::array<float, 3>& head) {
    const u64 link = g_link.load(std::memory_order_relaxed);
    std::array<float, 3> feet;
    if (!link || !memory.ReadBlock(link + 0x31C, feet.data(), sizeof(feet))) {
        return false;
    }
    // Link's rotation, 3x3 rows: his forward is the third column (seen while running).
    std::array<float, 9> rot;
    memory.ReadBlock(link + 0x328, rot.data(), sizeof(rot));
    static const float forward_m = [] {
        const char* const v = std::getenv("EDEN_NX2_OOT_VR_EYE_FWD");
        return v && *v ? static_cast<float>(std::atof(v)) : EyeForwardM;
    }();
    std::scoped_lock lk{g_head_lock};
    static float stand = 0;
    const float above = g_head[1] - g_head_feet[1];
    stand = std::fmax(above, stand - StandFollowM);
    const float forward = forward_m + std::fmin((stand - above) * LowForwardPerM, MaxLowForwardM);
    float dir[3]{rot[2], rot[5], rot[8]};
    float head_dir[3]{g_head_axes[3], 0, g_head_axes[5]};
    if (Common::OotVr::TalkCamera().load(std::memory_order_relaxed) &&
        Common::OotVr::TalkCameraMode().load(std::memory_order_relaxed) == 2 && Normalize(head_dir) > 0.3f) {
        dir[0] = head_dir[0], dir[1] = 0, dir[2] = head_dir[2];
    }
    for (int k = 0; k < 3; ++k) {
        head[k] = g_head[k] - g_head_feet[k] + feet[k] + dir[k] * forward;
    }
    return g_has_head;
}

// Projection +0xA0 fovy, +0xA4 sin(fovy/2), +0xA8 cos(fovy/2), +0xAC tan(fovy/2), +0xB0 aspect.
std::array<float, 5> WideFields(float tan_x, float tan_y) {
    const float half = std::atan(tan_y);
    return {2 * half, std::sin(half), std::cos(half), tan_y, tan_x / tan_y};
}

std::atomic<bool> g_menu_now{};          ///< a game menu is open now
std::atomic<int> g_turned_after_menu{99}; ///< camera updates with the head pose since the last menu
constexpr int TurnedAfterMenu = 4;       ///< the flat screen stays until this many such updates

struct UiScreen {
    u32 id;
    const char* name;
    bool menu;
};

constexpr u32 ShortCutId = 0x42fefb65; ///< the L1/R1 item menu: shown while +0x1A6 is 1, not 2
constexpr u32 TalkId = 0x2e6867e2;
constexpr u32 MessageDemoId = 0x6b8f8fdd;

std::atomic<bool> g_talk_open{}; ///< Talk_00 or MessageDemo_00 is open

constexpr std::array<UiScreen, 14> WatchedScreens{{
    {0xc0a4dec4, "PlusMenu_00", true},
    {0xf5b7168b, "MenuBG_00", true},
    {0x7ecad8bd, "ImportantMenu_00", true},
    {0x3bc00454, "ChallengeMenu_00", true},
    {0x41d368be, "SystemBG_00", true},
    {0x8f87396d, "MessageLog_01", false},
    {0x20d55c5a, "MenuOverlay_00", false},
    {0xc3332ad4, "MapMenu_00", false},
    {0x5c7f1c3c, "PlayerStatus_00", false},
    {TalkId, "Talk_00", false},
    {MessageDemoId, "MessageDemo_00", false},
    {0xf91df64d, "MessageDialog_00", false},
    {0x5528d3d9, "CtrlGuide_00", false},
    {ShortCutId, "ShortCut_00", false},
}};

// Screen open: byte +0x1A6 == 2 and signed byte +0x1A5 >= 0 (main+0xA687F0); UI manager layout in docs/vr-port-notes.md (recomp).
void UpdateMenuOpen(Core::Memory::Memory& memory, u64 main_base) {
    static u32 frame = 0;
    if (frame++ % 3 != 0) {
        return;
    }
    const u64 holder = memory.Read64(main_base + UiManagerPtrOffset);
    const u64 manager = holder ? memory.Read64(holder) : 0;
    const u64 map = manager ? memory.Read64(manager + 0x48) : 0;
    if (!map) {
        return;
    }
    const u32 capacity = std::min(memory.Read32(map + 0x24), 4096u);
    const u32 count = std::min(memory.Read32(manager + 0x8), 4096u);
    std::vector<std::array<u32, 2>> entries(capacity);
    std::vector<u64> screens(count);
    if (capacity == 0 || !memory.ReadBlock(memory.Read64(map + 0x18), entries.data(), capacity * 8) ||
        !memory.ReadBlock(memory.Read64(manager + 0x10), screens.data(), count * 8)) {
        return;
    }
    // EDEN_NX2_OOT_VR_UI_TRACE=1: log every screen of the UI manager as it opens or closes, to find a menu's layout.
    static const bool trace = [] {
        const char* const v = std::getenv("EDEN_NX2_OOT_VR_UI_TRACE");
        return v && *v && *v != '0';
    }();
    if (trace) {
        // Every change in a screen's bytes +0x1A0..+0x1B0, not only the open flag: a menu may show in another state.
        static std::vector<std::array<u8, 16>> traced;
        traced.resize(std::max<size_t>(traced.size(), count), std::array<u8, 16>{});
        for (u32 s = 0; s < count; ++s) {
            if (!screens[s]) {
                continue;
            }
            std::array<u8, 16> state;
            memory.ReadBlock(screens[s] + 0x1A0, state.data(), state.size());
            if (state == traced[s]) {
                continue;
            }
            traced[s] = state;
            const u64 name_ptr = memory.Read64(screens[s] + 0x178);
            std::string name = name_ptr ? memory.ReadCString(name_ptr, 48) : std::string{};
            LOG_INFO(Kernel_SVC, "OoT VR UI: screen {} '{}' +0x1A0: {:02x}", s, name, fmt::join(state, " "));
        }
        // The item menu's bytes +0x40..+0x200: what separates shown from hidden.
        static std::array<std::array<u8, 0x1C0>, 2> wide{};
        static std::array<bool, 2> wide_seen{};
        const std::array<u32, 2> ids{ShortCutId, 0xc5a49707};
        for (size_t n = 0; n < ids.size(); ++n) {
            u32 index = count;
            for (u32 k = 0, i = ids[n] % capacity; k < capacity && entries[i][0] != 0; ++k, i = (i + 1) % capacity) {
                if (entries[i][0] == ids[n]) {
                    index = entries[i][1];
                    break;
                }
            }
            if (index >= count || !screens[index]) {
                continue;
            }
            std::array<u8, 0x1C0> now;
            memory.ReadBlock(screens[index] + 0x40, now.data(), now.size());
            if (wide_seen[n]) {
                std::string diff;
                for (size_t b = 0; b < now.size(); ++b) {
                    if (now[b] != wide[n][b]) {
                        diff += fmt::format(" +{:#x}:{:02x}>{:02x}", b + 0x40, wide[n][b], now[b]);
                    }
                }
                if (!diff.empty()) {
                    LOG_INFO(Kernel_SVC, "OoT VR UI: {} changed:{}", n ? "ShortCutBG_00" : "ShortCut_00", diff);
                }
            }
            wide[n] = now;
            wide_seen[n] = true;
        }
    }
    static std::array<int, WatchedScreens.size()> last;
    static bool started = false;
    if (!started) {
        last.fill(-1);
        started = true;
    }
    bool menu = false, talk = false;
    for (size_t w = 0; w < WatchedScreens.size(); ++w) {
        const u32 id = WatchedScreens[w].id;
        u32 index = count;
        for (u32 k = 0, i = id % capacity; k < capacity && entries[i][0] != 0; ++k, i = (i + 1) % capacity) {
            if (entries[i][0] == id) {
                index = entries[i][1];
                break;
            }
        }
        if (index >= count || !screens[index]) {
            continue;
        }
        std::array<s8, 2> state;
        memory.ReadBlock(screens[index] + 0x1A5, state.data(), state.size());
        const int open = id == ShortCutId ? state[1] == 1 : state[1] == 2 && state[0] >= 0;
        menu |= open && WatchedScreens[w].menu;
        talk |= open && (id == TalkId || id == MessageDemoId);
        if (open != last[w]) {
            LOG_INFO(Kernel_SVC, "OoT VR HUD: {} {} (+0x1A5 {}, +0x1A6 {})", WatchedScreens[w].name,
                     open ? "open" : "closed", state[0], state[1]);
            last[w] = open;
        }
    }
    if (trace) {
        static u32 last_flag = ~0u;
        const u32 flag = memory.Read32(main_base + HudParamsOffset + 0x20);
        if (flag != last_flag) {
            LOG_INFO(Kernel_SVC, "OoT VR HUD: cave item menu flag {}", flag);
            last_flag = flag;
        }
    }
    g_menu_now.store(menu, std::memory_order_relaxed);
    g_talk_open.store(talk, std::memory_order_relaxed);
    if (menu) {
        g_turned_after_menu.store(0, std::memory_order_relaxed);
    }
    const bool show_menu = menu || g_turned_after_menu.load(std::memory_order_relaxed) < TurnedAfterMenu;
    if (Common::OotVr::MenuOpen().exchange(show_menu) != show_menu) {
        LOG_INFO(Kernel_SVC, "OoT VR HUD: menu {}", show_menu ? "open: flat screen" : "closed: world");
    }
}

// View object: 3x4 view at +0x8, eye float3 +0x38, look-at float3 +0x44, up float3 +0x50, eye
// double3 +0x60, look-at double3 +0x78 (written by main+0x4C48C0).
// The game's own camera (view object +0x8..+0x90: matrix, eye and target), as the last camera update left it before
// the head pose turned it. The item menu freezes the camera update; set-view then turns this copy each frame.
std::array<u8, 0x88> g_game_camera{};
u64 g_game_camera_view = 0;
std::atomic<u64> g_camera_updates{}; ///< camera-source hook calls for the displayed camera

constexpr float CutJumpM = 1.0f;      ///< game camera eye moved this much more than Link's feet in one update
constexpr float CutTurnCos = 0.9063f; // cos 25 degrees
constexpr u32 CutBeforeTalk = 60;     ///< camera updates a cut may come before the talk box opens
constexpr u32 TalkCameraHold = 30;    ///< camera updates the talk camera stays after the talk box closes
constexpr float TalkNearM = 0.1f;     ///< near plane in play (projection +0x98); talk shots use about 1 m
constexpr float TalkNearThirdM = 0.02f; ///< third person: some talk shots put the camera almost on the floor

float TalkNear() {
    return Common::OotVr::FirstPerson() ? TalkNearM : TalkNearThirdM;
}

// Row-major 4x4 projection: P[10] = -(f+n)/(f-n), P[11] = -2fn/(f-n). Keeps the far plane.
void SetNearPlane(u8* matrix, float near_m) {
    float p10, p11;
    std::memcpy(&p10, matrix + 40, 4);
    std::memcpy(&p11, matrix + 44, 4);
    if (p10 + 1.0f >= 0.0f || p11 >= 0.0f) {
        return;
    }
    const float far_m = p11 / (p10 + 1.0f);
    if (2.0f * far_m * near_m / (far_m - near_m) >= -p11) {
        return;
    }
    p10 = -(far_m + near_m) / (far_m - near_m);
    p11 = -2.0f * far_m * near_m / (far_m - near_m);
    std::memcpy(matrix + 40, &p10, 4);
    std::memcpy(matrix + 44, &p11, 4);
}

// A talk camera: a talk box is open and the game's own camera cut to a shot (a jump, not the follow camera).
void TalkCameraUpdate(Core::Memory::Memory& memory) {
    std::array<float, 3> feet{};
    if (const u64 link = g_link.load(std::memory_order_relaxed)) {
        memory.ReadBlock(link + 0x31C, feet.data(), sizeof(feet));
    }
    std::array<float, 12> m;
    std::array<float, 3> eye;
    std::memcpy(m.data(), g_game_camera.data(), sizeof(m));
    std::memcpy(eye.data(), g_game_camera.data() + 0x30, sizeof(eye));
    float fwd[3]{-m[8], 0, -m[10]};
    const bool flat = Normalize(fwd) > 1e-3f;
    static std::array<float, 3> last_eye{}, last_feet{};
    static std::array<float, 3> last_fwd{};
    static bool has_last = false;
    static u32 since_cut = 1000, since_talk = 1000;
    float jump = 0, turn_cos = 1;
    if (has_last) {
        float eye_d = 0, feet_d = 0;
        for (int k = 0; k < 3; ++k) {
            eye_d += (eye[k] - last_eye[k]) * (eye[k] - last_eye[k]);
            feet_d += (feet[k] - last_feet[k]) * (feet[k] - last_feet[k]);
        }
        jump = std::sqrt(eye_d) - std::sqrt(feet_d);
        if (flat) {
            turn_cos = fwd[0] * last_fwd[0] + fwd[2] * last_fwd[2];
        }
    }
    const bool cut = has_last && (jump > CutJumpM || turn_cos < CutTurnCos);
    last_eye = eye, last_feet = feet, has_last = true;
    if (flat) {
        last_fwd = {fwd[0], 0, fwd[2]};
    }
    since_cut = cut ? 0 : std::min(since_cut + 1, 1000u);
    const bool talk = g_talk_open.load(std::memory_order_relaxed);
    since_talk = talk ? 0 : std::min(since_talk + 1, 1000u);
    if (cut && (talk || since_talk < TalkCameraHold)) {
        LOG_INFO(Kernel_SVC, "OoT VR talk camera: cut, eye moved {:.2f} m more than Link, turned {:.0f} degrees",
                 jump, std::acos(std::clamp(turn_cos, -1.0f, 1.0f)) / Deg);
    }
    auto& on = Common::OotVr::TalkCamera();
    const bool was = on.load(std::memory_order_relaxed);
    const bool now = was ? since_talk < TalkCameraHold : talk && since_cut < CutBeforeTalk;
    if (now != was) {
        on.store(now, std::memory_order_relaxed);
        LOG_INFO(Kernel_SVC, "OoT VR talk camera: {} (mode {})", now ? "on" : "off",
                 Common::OotVr::TalkCameraMode().load(std::memory_order_relaxed));
    }
}

// Link's facing on the ground plane: the third column of his rotation at actor +0x328.
bool LinkForward(Core::Memory::Memory& memory, float* fwd) {
    const u64 link = g_link.load(std::memory_order_relaxed);
    std::array<float, 9> rot;
    if (!link || !memory.ReadBlock(link + 0x328, rot.data(), sizeof(rot))) {
        return false;
    }
    fwd[0] = rot[2], fwd[1] = 0, fwd[2] = rot[8];
    return Normalize(fwd) > 1e-3f;
}

// Link's head bone, second axis, flat: talk animations turn his body but not actor +0x328.
bool LinkHeadForward(Core::Memory::Memory& memory, float* fwd) {
    {
        std::scoped_lock lk{g_head_lock};
        fwd[0] = g_head_axes[3], fwd[1] = 0, fwd[2] = g_head_axes[5];
    }
    return Normalize(fwd) > 0.3f || LinkForward(memory, fwd);
}

void CameraSource(Core::Memory::Memory& memory, u64 main_base, u64 view_object, bool from_hook = true) {
    static std::atomic<u64> turned{}, other{};
    const bool mine = view_object != 0 && IsDisplayView(view_object);
    if (mine && from_hook) {
        memory.ReadBlock(view_object + 0x8, g_game_camera.data(), g_game_camera.size());
        g_game_camera_view = view_object;
        TalkCameraUpdate(memory);
        g_camera_updates.fetch_add(1, std::memory_order_relaxed);
    }
    const u64 count = (mine ? turned : other).fetch_add(1, std::memory_order_relaxed) + 1;
    if (mine && count % 300 == 0) {
        LOG_INFO(Kernel_SVC, "OoT VR camera: {} displayed camera updates turned, {} other camera updates",
                 count, other.load(std::memory_order_relaxed));
    }
    if (!mine) {
        if (Common::OotVr::ItemMenu().load(std::memory_order_relaxed) && count % 60 == 1) {
            LOG_INFO(Kernel_SVC, "OoT VR camera: item menu: camera update for view {:#x} (not displayed)", view_object);
        }
        return;
    }
    const u64 projection = g_display_projection.load(std::memory_order_relaxed);
    float tan_x, tan_y;
    if (projection && memory.Read64(projection) == main_base + ProjectionVtableOffset &&
        Common::OotVr::HeadsetTan(tan_x, tan_y)) {
        std::array<float, 9> fields;
        memory.ReadBlock(projection + 0x98, fields.data(), sizeof(fields));
        static std::atomic<bool> logged{};
        if (!logged.exchange(true)) {
            LOG_INFO(Kernel_SVC, "OoT VR camera: projection +0x98..+0xB8 {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} "
                     "{:.4f} {:.4f} {:.4f} {:.4f}", fields[0], fields[1], fields[2], fields[3], fields[4],
                     fields[5], fields[6], fields[7], fields[8]);
        }
        const auto fov = WideFields(tan_x, tan_y);
        memory.WriteBlock(projection + 0xA0, fov.data(), sizeof(fov));
        memory.Write8(projection + 0x8, 1);
    }
    Head head;
    if (g_menu_now.load(std::memory_order_relaxed) || !GetHead(memory, main_base, false, head)) {
        return;
    }
    g_turned_after_menu.fetch_add(1, std::memory_order_relaxed);
    // A stereo pair: the right-eye frame reuses the left-eye frame's game camera and head position.
    static std::array<float, 12> pair_m{};
    static std::array<float, 3> pair_head{};
    static bool has_pair = false;
    const int frame_eye = g_next_eye.load(std::memory_order_relaxed);
    const bool second = frame_eye == 1 && has_pair;
    if (second) {
        head.pose.position = pair_head;
    }
    std::array<float, 3> at_eye;
    bool stereo = false;
    if (Common::OotVr::StereoWanted() && Common::OotVr::EyePosition(head.pose, frame_eye, at_eye)) {
        head.pos = at_eye;
        head.pose.eye = frame_eye;
        stereo = true;
    }
    NoteViewPose(view_object, head.pose);
    std::array<float, 12> m;
    std::array<float, 6> eye_at;
    memory.ReadBlock(view_object + 0x8, m.data(), sizeof(m));
    memory.ReadBlock(view_object + 0x38, eye_at.data(), sizeof(eye_at));
    const float dx = eye_at[3] - eye_at[0], dy = eye_at[4] - eye_at[1], dz = eye_at[5] - eye_at[2];
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    std::array<float, 3> head_at;
    if (Common::OotVr::FirstPerson() && LinkHead(memory, head_at)) {
        head_at[1] += EyeUpM;
        float fwd[3];
        if (Common::OotVr::TalkCamera().load(std::memory_order_relaxed) &&
            Common::OotVr::TalkCameraMode().load(std::memory_order_relaxed) == 2 && LinkHeadForward(memory, fwd)) {
            const std::array<float, 9> rows{-fwd[2], 0, fwd[0], 0, 1, 0, -fwd[0], 0, -fwd[2]};
            for (int i = 0; i < 3; ++i) {
                for (int k = 0; k < 3; ++k) {
                    m[i * 4 + k] = rows[i * 3 + k];
                }
            }
        }
        for (int i = 0; i < 3; ++i) {
            m[i * 4 + 3] = -(m[i * 4] * head_at[0] + m[i * 4 + 1] * head_at[1] + m[i * 4 + 2] * head_at[2]);
        }
    }
    if (stereo && second) {
        m = pair_m;
    } else if (stereo && frame_eye == 0) {
        pair_m = m, pair_head = head.pose.position, has_pair = true;
    }
    TurnView(m.data(), head, MoveScale());
    memory.WriteBlock(view_object + 0x8, m.data(), sizeof(m));
    std::array<float, 3> eye;
    for (int k = 0; k < 3; ++k) {
        eye[k] = -(m[k] * m[3] + m[4 + k] * m[7] + m[8 + k] * m[11]);
    }
    if (CaptureMode()) {
        static std::array<float, 6> logged{};
        const std::array<float, 6> pose{head.pos[0], head.pos[1], head.pos[2], head.fwd[0], head.fwd[1], head.fwd[2]};
        if (pose != logged) {
            logged = pose;
            LOG_INFO(Kernel_SVC, "OoT VR capture: test position {:.2f} {:.2f} {:.2f}, eye {:.3f} {:.3f} {:.3f}, looking {:.3f} {:.3f} {:.3f}",
                     pose[0], pose[1], pose[2], eye[0], eye[1], eye[2], -m[8], -m[9], -m[10]);
        }
    }
    const std::array<float, 3> at{eye[0] - m[8] * distance, eye[1] - m[9] * distance,
                                  eye[2] - m[10] * distance};
    const std::array<float, 9> floats{eye[0], eye[1], eye[2], at[0], at[1], at[2], m[4], m[5], m[6]};
    memory.WriteBlock(view_object + 0x38, floats.data(), sizeof(floats));
    const std::array<double, 6> doubles{eye[0], eye[1], eye[2], at[0], at[1], at[2]};
    memory.WriteBlock(view_object + 0x60, doubles.data(), sizeof(doubles));
}

// EDEN_NX2_OOT_VR_MENU_DIFF=1: the words of main's data and BSS that hold one value whenever the item menu is gone and
// another whenever it is shown, narrowed over each opening (to find what stops the world being drawn).
void ItemMenuDiff(Core::Memory::Memory& memory, const Kernel::KProcess& process, u64 main_base, bool shown) {
    static const bool on = [] {
        const char* const v = std::getenv("EDEN_NX2_OOT_VR_MENU_DIFF");
        return v && *v && *v != '0';
    }();
    if (!on) {
        return;
    }
    // Words in main's data and BSS and in the heap that hold one value while the item menu is gone and another 90
    // frames after it opens, at every opening. A snapshot of the gone state is taken 40 frames after each close.
    constexpr u64 Chunk = 0x200000;
    struct Candidate {
        u64 address;
        u32 gone, shown;
    };
    static std::vector<std::pair<u64, std::vector<u32>>> gone_copy;
    static std::vector<Candidate> candidates;
    static std::vector<u32> buffer(Chunk / 4);
    static u32 gone_frames = 0, shown_frames = 0, openings = 0;
    static bool have_gone = false;
    if (!shown) {
        shown_frames = 0;
        if (++gone_frames != 40) {
            return;
        }
        gone_copy.clear();
        // ReadBlockUnsafe: no flush of GPU-written memory. At most 1 GiB in all (the guest commits about 9 GiB).
        u64 total = 0;
        const auto take = [&](u64 base, u64 size) {
            for (u64 a = base; a < base + size && total < 0x40000000; a += Chunk) {
                const u64 n = std::min(Chunk, base + size - a);
                if (!memory.IsValidVirtualAddressRange(a, n)) {
                    continue;
                }
                auto& [address, words] = gone_copy.emplace_back(a, std::vector<u32>(n / 4));
                memory.ReadBlockUnsafe(a, words.data(), n);
                total += n;
            }
        };
        take(main_base + 0x484D000, 0x653000);
        take(GetInteger(process.GetPageTable().GetHeapRegionStart()), process.GetPageTable().GetHeapRegionSize());
        have_gone = true;
        if (openings == 0) {
            u64 bytes = 0;
            for (const auto& [a, w] : gone_copy) {
                bytes += w.size() * 4;
            }
            LOG_INFO(Kernel_SVC, "OoT VR menu diff: gone snapshot, {} MiB in {} chunks; heap region {:#x} size {:#x}",
                     bytes >> 20, gone_copy.size(), GetInteger(process.GetPageTable().GetHeapRegionStart()),
                     process.GetPageTable().GetHeapRegionSize());
        }
        return;
    }
    gone_frames = 0;
    if (++shown_frames != 90 || !have_gone) {
        return;
    }
    have_gone = false;
    std::vector<Candidate> found;
    const bool first = openings++ == 0;
    if (first) {
        for (const auto& [a, words] : gone_copy) {
            memory.ReadBlockUnsafe(a, buffer.data(), words.size() * 4);
            for (size_t i = 0; i < words.size(); ++i) {
                if (words[i] != buffer[i] && found.size() < 16'000'000) {
                    found.push_back({a + i * 4, words[i], buffer[i]});
                }
            }
        }
    } else {
        for (const Candidate& c : candidates) {
            const auto it = std::upper_bound(gone_copy.begin(), gone_copy.end(), c.address,
                                             [](u64 v, const auto& e) { return v < e.first; });
            if (it == gone_copy.begin()) {
                continue;
            }
            const auto& [a, words] = *std::prev(it);
            const u64 i = (c.address - a) / 4;
            u32 now = 0;
            memory.ReadBlockUnsafe(c.address, &now, 4);
            if (i < words.size() && words[i] == c.gone && now == c.shown) {
                found.push_back(c);
            }
        }
    }
    candidates = std::move(found);
    LOG_INFO(Kernel_SVC, "OoT VR menu diff: opening {}, {} words differ every time", openings, candidates.size());
    if (candidates.size() <= 200000) {
        if (std::FILE* f = std::fopen("D:/lxc/vrdiff/menu_diff.txt", "w")) {
            for (const Candidate& c : candidates) {
                // The object the word is in: the nearest pointer into main below it (a vtable), as main offset and distance.
                u64 vtable = 0, distance = 0;
                std::array<u64, 0x100> before{};
                if (c.address - main_base >= 0x5000000 && c.address >= 0x800) {
                    memory.ReadBlockUnsafe((c.address & ~u64{7}) - 0x800, before.data(), 0x800);
                    for (size_t k = before.size(); k-- > 0;) {
                        if (before[k] - main_base < 0x5000000) {
                            vtable = before[k] - main_base;
                            distance = (c.address & ~u64{7}) - 0x800 + k * 8;
                            distance = c.address - distance;
                            break;
                        }
                    }
                }
                std::fprintf(f, "%llx %08x %08x %llx %llx\n", static_cast<unsigned long long>(c.address - main_base),
                             c.gone, c.shown, static_cast<unsigned long long>(vtable),
                             static_cast<unsigned long long>(distance));
            }
            std::fclose(f);
        }
    }
    if (candidates.size() <= 200) {
        for (const Candidate& c : candidates) {
            const bool in_main = c.address - main_base < 0x5000000;
            LOG_INFO(Kernel_SVC, "OoT VR menu diff: {}{:#x}: gone {:#010x}, shown {:#010x}", in_main ? "main+" : "",
                     in_main ? c.address - main_base : c.address, c.gone, c.shown);
        }
    }
}

} // Anonymous namespace

bool CallNx2OotVr(Core::System& system, u32 imm, std::span<u64, 8> args) {
    if (imm != VrSvc && imm != CameraSvc && imm != TargetSvc && imm != SkeletonSvc && imm != FrozenSvc) {
        return false;
    }
    auto& kernel = system.Kernel();
    auto& process = GetCurrentProcess(kernel);
    if (!process.Is64Bit() || process.GetProgramId() != OotProgramId) {
        return false;
    }
    auto* const arm = process.GetArmInterface(kernel.CurrentPhysicalCoreIndex());
    auto& memory = process.GetMemory();
    if (imm == FrozenSvc) {
        // Every frame: w1 = the item menu is shown.
        const bool shown = (args[1] & 1) != 0;
        if (Common::OotVr::ItemMenu().exchange(shown, std::memory_order_relaxed) != shown) {
            LOG_INFO(Kernel_SVC, "OoT VR HUD: item menu {}", shown ? "shown" : "gone");
        }
        ItemMenuDiff(memory, process, args[0] - HudParamsOffset, shown);
        // Test: EDEN_NX2_OOT_VR_MENU_TEST_YAW=<degrees> turns the test head pose that far 120 frames into each item
        // menu opening and back when it closes, to see whether the world behind the menu turns.
        {
            static const float test_yaw = [] {
                const char* const v = std::getenv("EDEN_NX2_OOT_VR_MENU_TEST_YAW");
                return v && *v ? static_cast<float>(std::atof(v)) : 0.0f;
            }();
            static u32 menu_frames = 0;
            static bool turned = false;
            menu_frames = shown ? menu_frames + 1 : 0;
            if (test_yaw != 0.0f && (menu_frames == 120 || (!shown && turned))) {
                turned = shown;
                struct {
                    u32 mode;
                    float yaw, pitch;
                } pose{1, shown ? test_yaw : 0.0f, 0.0f};
                memory.WriteBlock(args[0] - HudParamsOffset + TestPoseOffset, &pose, sizeof(pose));
                LOG_INFO(Kernel_SVC, "OoT VR test: item menu head yaw {}", pose.yaw);
            }
        }
        // The item menu pauses the camera (clears camera +0x88 bit 0), and with it the world draw. Keep the camera
        // running while the menu is shown, so the world stays live behind it (EDEN_NX2_OOT_VR_MENU_VIEW=0 turns this
        // off). EDEN_NX2_OOT_VR_VIEW_TRACE=1 logs the view's draw inputs.
        if (const u64 view = g_display_view.load(std::memory_order_relaxed)) {
            static const bool force = [] {
                const char* const v = std::getenv("EDEN_NX2_OOT_VR_MENU_VIEW");
                return !(v && *v == '0');
            }();
            static const bool trace = [] {
                const char* const v = std::getenv("EDEN_NX2_OOT_VR_VIEW_TRACE");
                return v && *v && *v != '0';
            }();
            // EDEN_NX2_OOT_VR_WATCH_VIEW=1: log every guest write to the camera's +0x91 once the item menu is shown
            // (arm_dynarmic_64.cpp).
            static const bool watch = [] {
                const char* const v = std::getenv("EDEN_NX2_OOT_VR_WATCH_VIEW");
                return v && *v && *v != '0';
            }();
            static u64 display = 0;
            if (const u64 cam = memory.Read64(view + 0x5f0); watch && shown && cam && display == 0 &&
                Common::OotVr::WriteWatch().exchange(cam + 0x91) != cam + 0x91) {
                memory.MarkRegionDebug((cam + 0x91) & ~u64{0xFFF}, 0x1000, true);
                LOG_INFO(Kernel_SVC, "OoT VR view: watching writes to the camera's +0x91 at {:#x}", cam + 0x91);
            }
            // The camera manager (main+0x6531C0 x0) is in a saved register of that writer: cameras at +0x28 (count
            // +0x20), displays at +0x18 (count +0x10). The camera's display: index camera+0x90, else camera+0x8f.
            if (const u64 cam = memory.Read64(view + 0x5f0); watch && cam && display == 0 &&
                Common::OotVr::WriteWatch().load() == cam + 0x91) {
                for (auto& reg : Common::OotVr::WriteWatchRegs()) {
                    const u64 m = reg.load(std::memory_order_relaxed);
                    if (m < 0x1000 || !memory.IsValidVirtualAddressRange(m + 0x10, 0x20)) {
                        continue;
                    }
                    const u32 cams = memory.Read32(m + 0x20), displays = memory.Read32(m + 0x10);
                    const u64 list = memory.Read64(m + 0x28);
                    if (cams == 0 || cams > 64 || displays == 0 || displays > 16 ||
                        !memory.IsValidVirtualAddressRange(list, cams * 8)) {
                        continue;
                    }
                    for (u32 i = 0; i < cams; ++i) {
                        if (memory.Read64(list + i * 8) == cam) {
                            const u8 a = memory.Read8(cam + 0x90);
                            const u8 index = a != 0xff ? a : memory.Read8(cam + 0x8f);
                            if (index < displays) {
                                display = memory.Read64(memory.Read64(m + 0x18) + index * 8);
                            }
                            LOG_INFO(Kernel_SVC, "OoT VR view: camera manager {:#x}, display {} at {:#x}", m, index,
                                     display);
                            break;
                        }
                    }
                    if (display) {
                        Common::OotVr::WriteWatch().store(display + 0x7d0);
                        memory.MarkRegionDebug((display + 0x7d0) & ~u64{0xFFF}, 0x1000, true);
                        break;
                    }
                }
            }
            if (display) {
                static u32 last_display = 0xFFFFFFFF;
                const u32 d = memory.Read32(display + 0x7d0);
                if (d != last_display) {
                    last_display = d;
                    LOG_INFO(Kernel_SVC, "OoT VR view: display +0x7d0 = {:#010x}, item menu {}", d, shown);
                }
            }
            const u16 flags = memory.Read16(view + 0x54dc);
            static u32 last = 0x10000;
            if (trace && flags != last) {
                last = flags;
                LOG_INFO(Kernel_SVC, "OoT VR view {:#x}: +0x54dc = {:#06x}, item menu {}", view, flags, shown);
            }
            // The inputs of main+0x3437C0, which sets bit 0: camera +0x91, +0x884 bit 4, +0x228; scene +0x3b18 bit 6
            // unless the view's layer entry +0x270 is 1.
            if (const u64 scene = g_view_scene.load(std::memory_order_relaxed); trace && scene) {
                const u64 camera = memory.Read64(view + 0x5f0);
                const u64 layer = memory.Read64(scene + 0x1b88) + memory.Read8(view + 8) * 0x278ULL;
                const auto inputs = fmt::format(
                    "camera {:#x} +0x88 {:#06x} +0x8a {:#06x} +0x8c {:#06x} +0x91 {} +0x884 {:#06x} +0x228 {}; scene +0x3b14 {:#010x} +0x3b18 {:#010x}; "
                    "layer +0x270 {}", camera, camera ? memory.Read16(camera + 0x88) : 0, camera ? memory.Read16(camera + 0x8a) : 0,
                    camera ? memory.Read16(camera + 0x8c) : 0, camera ? memory.Read8(camera + 0x91) : 0,
                    camera ? memory.Read16(camera + 0x884) : 0, camera ? memory.Read64(camera + 0x228) != 0 : false,
                    memory.Read32(scene + 0x3b14), memory.Read32(scene + 0x3b18),
                    memory.Read8(layer + 0x270));
                static std::string last_inputs;
                if (inputs != last_inputs) {
                    last_inputs = inputs;
                    LOG_INFO(Kernel_SVC, "OoT VR view inputs: {}, item menu {}", inputs, shown);
                }
            }
            // The camera manager (main+0x6531C0) updates a camera, and sets its +0x91 that main+0x3437C0 needs to mark
            // the view drawn, only while camera +0x88 has bits 0 and 1. The menu clears bit 0 once when it opens.
            const u64 camera = memory.Read64(view + 0x5f0);
            if (force && shown && camera && (memory.Read16(camera + 0x88) & 3) != 3) {
                memory.Write16(camera + 0x88, static_cast<u16>(memory.Read16(camera + 0x88) | 3));
                static u32 forced = 0;
                LOG_INFO(Kernel_SVC, "OoT VR view: the item menu paused the camera; kept it running ({} times)",
                         ++forced);
            }
        }
        return true;
    }
    ThreadContext ctx{};
    arm->GetContext(ctx);
    if (imm == TargetSvc) {
        ctx.r[21] = ctx.r[0];
        arm->SetContext(ctx);
        const u64 site = ((ctx.pc - TargetSiteOffset) & 0xFFF) == 0 ? ctx.pc : ctx.pc - 4;
        const u64 main_base = site - TargetSiteOffset;
        if (ctx.r[0] && memory.Read64(ctx.r[0]) == main_base + LinkVtableOffset &&
            g_link.exchange(ctx.r[0], std::memory_order_relaxed) != ctx.r[0]) {
            LOG_INFO(Kernel_SVC, "OoT VR camera: Link's actor {:#x}", ctx.r[0]);
        }
        return true;
    }
    if (imm == SkeletonSvc) {
        ctx.r[19] = ctx.r[0];
        arm->SetContext(ctx);
        LinkSkeletonUpdate(memory, ctx.r[0]);
        return true;
    }
    if (imm == CameraSvc) {
        ctx.r[8] = memory.Read64(ctx.r[19] + 0x20);
        arm->SetContext(ctx);
        const u64 site = ((ctx.pc - CameraSiteOffset) & 0xFFF) == 0 ? ctx.pc : ctx.pc - 4;
        static std::atomic<bool> logged{};
        if (!logged.exchange(true)) {
            LOG_INFO(Kernel_SVC, "OoT VR camera: camera-source hook live, view object {:#x}, "
                     "controller+0x18 -> {:#x}", ctx.r[20], memory.Read64(ctx.r[19] + 0x18));
        }
        if (SourceMode()) {
            CameraSource(memory, site - CameraSiteOffset, ctx.r[20]);
        }
        return true;
    }
    ctx.r[10] = memory.Read32(args[0] + 0x10);
    arm->SetContext(ctx);

    static std::atomic<u64> calls{};
    const u64 n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const u64 at = ((ctx.pc - SiteOffset) & 0xFFF) == 0 ? ctx.pc : ctx.pc - 4;
    const u64 main_base = at - SiteOffset;
    if (n == 1) {
        LOG_INFO(Kernel_SVC, "OoT VR camera: set-view hook live, pc {:#x}, main at {:#x}", ctx.pc,
                 main_base);
    }
    const u32 view_index = static_cast<u32>(args[1]);
    const u64 caller = ctx.lr - main_base;
    // The camera's set-view function (main+0x33A240) sets views 0, 1 and 2 from these calls.
    const bool from_camera =
        caller == DisplayCallerOffset || caller == 0x33A320 || caller == 0x33A368;
    const bool display = view_index == 0 && caller == DisplayCallerOffset;
    if (display) {
        g_display_view.store(args[0], std::memory_order_relaxed);
        // The scene is in a callee-saved register of the caller (main+0x798A0): views at +0x138, 0x5548 bytes each.
        if (!g_view_scene.load(std::memory_order_relaxed)) {
            // Registers x0..x28, then the caller's stack frame.
            std::array<u64, 29 + 256> cands{};
            for (int r = 0; r <= 28; ++r) {
                cands[r] = ctx.r[r];
            }
            if (memory.IsValidVirtualAddressRange(ctx.sp, 256 * 8)) {
                memory.ReadBlock(ctx.sp, cands.data() + 29, 256 * 8);
            }
            for (int r = 0; r < static_cast<int>(cands.size()); ++r) {
                const u64 s = cands[r];
                if (s < 0x1000 || !memory.IsValidVirtualAddressRange(s + 0x130, 16)) {
                    continue;
                }
                const u64 views = memory.Read64(s + 0x138);
                if (args[0] >= views && (args[0] - views) % 0x5548 == 0 &&
                    (args[0] - views) / 0x5548 < memory.Read32(s + 0x130)) {
                    g_view_scene.store(s, std::memory_order_relaxed);
                    LOG_INFO(Kernel_SVC, "OoT VR view: scene {:#x} (x{}), view {}", s, r, (args[0] - views) / 0x5548);
                    break;
                }
            }
        }
    }
    {
        static std::array<u64, 16> seen{};
        const u64 key = (caller << 4 | view_index) + 1;
        const u64 vtable = memory.Read64(args[3]) - main_base;
        for (u64& s : seen) {
            if (s == key) {
                break;
            }
            if (s == 0) {
                s = key;
                LOG_INFO(Kernel_SVC, "OoT VR camera: view {} from main+{:#x}{}, projection vtable main+{:#x}{}",
                         view_index, caller, display ? " (displayed camera)" : "", vtable,
                         vtable == ProjectionVtableOffset ? " (main projection)" : "");
                break;
            }
        }
    }
    if (SourceMode()) {
        if (display && memory.Read64(args[3]) == main_base + ProjectionVtableOffset) {
            NoteDisplayView(args[2] - 8);
            // While the item menu freezes the camera update, no camera-source call turns the view: turn the game's
            // last camera here, once per drawn frame, so the eyes alternate and the head pose is followed.
            static u64 updates_at_last_view = 0;
            const u64 updates = g_camera_updates.load(std::memory_order_relaxed);
            // The camera is double-buffered: the saved copy may come from the other view object of the pair.
            if (Common::OotVr::ItemMenu().load(std::memory_order_relaxed) && updates == updates_at_last_view &&
                g_game_camera_view != 0) {
                memory.WriteBlock(args[2] - 8 + 0x8, g_game_camera.data(), g_game_camera.size());
                CameraSource(memory, main_base, args[2] - 8, false);
                // Set-view takes the matrix from x2 but the inverse and the culling planes from x4, another view
                // object (main+0x33a2c4): give it the turned camera too.
                if (args[4] != args[2] && args[4] > 8) {
                    std::array<u8, 0x88> turned;
                    memory.ReadBlock(args[2], turned.data(), turned.size());
                    memory.WriteBlock(args[4], turned.data(), turned.size());
                }
                static u64 menu_turns = 0;
                if (++menu_turns % 120 == 1) {
                    LOG_INFO(Kernel_SVC, "OoT VR camera: item menu: view turned at set-view ({} frames), x2 {:#x} x4 "
                             "{:#x}", menu_turns, args[2], args[4]);
                }
            }
            if (Common::OotVr::ItemMenu().load(std::memory_order_relaxed)) {
                static u64 menu_views = 0;
                if (++menu_views % 60 == 1) {
                    LOG_INFO(Kernel_SVC, "OoT VR camera: item menu set-view: view {:#x}, saved camera of {:#x}, camera "
                             "updates {} (last view {}), menu flag now {}", args[2] - 8, g_game_camera_view, updates,
                             updates_at_last_view, g_menu_now.load(std::memory_order_relaxed));
                }
            }
            updates_at_last_view = updates;
            Common::OotVr::HeadPose drawn;
            if (ViewPose(args[2] - 8, drawn)) {
                Common::OotVr::RecordApplied(drawn);
                static int last_eye = -1;
                static u64 frames = 0, flips = 0;
                flips += drawn.eye >= 0 && drawn.eye != last_eye;
                last_eye = drawn.eye;
                if (++frames % 60 == 0) {
                    LOG_INFO(Kernel_SVC, "OoT VR camera: {} eye changes in the last 60 drawn frames (eye now {})",
                             flips, drawn.eye);
                    flips = 0;
                }
                if (drawn.eye >= 0) {
                    g_next_eye.store(1 - drawn.eye, std::memory_order_relaxed);
                }
            }
            g_display_projection.store(args[3], std::memory_order_relaxed);
            float p00, p11;
            memory.ReadBlock(args[3] + 0xC, &p00, 4);
            memory.ReadBlock(args[3] + 0xC + 20, &p11, 4);
            static std::atomic<u64> seen{};
            if (seen.fetch_add(1, std::memory_order_relaxed) % 120 == 0) {
                std::array<float, 16> m;
                std::array<float, 8> fields;
                memory.ReadBlock(args[3] + 0xC, m.data(), sizeof(m));
                memory.ReadBlock(args[3] + 0xA0, fields.data(), sizeof(fields));
                LOG_INFO(Kernel_SVC, "OoT VR camera: game projection at set-view P00 {:.3f} P11 {:.3f} fovy {:.3f}; "
                         "matrix {}; fields from +0xA0 {}",
                         p00, p11, fields[0], fmt::join(m, " "), fmt::join(fields, " "));
            }
            float tan_x, tan_y;
            if (Common::OotVr::HeadsetTan(tan_x, tan_y)) {
                const int hud_eye = g_next_eye.load(std::memory_order_relaxed);
                const float hud_shift = Common::OotVr::HudShift(hud_eye, tan_x);
                static u64 hud_frames = 0;
                if (hud_frames++ % 120 < 2) {
                    LOG_INFO(Kernel_SVC, "OoT VR HUD: eye {} moves the HUD {:+.1f} sideways", hud_eye, hud_shift);
                }
                const std::array<u32, 8> hud_params{std::bit_cast<u32>(CaptureMode() ? 0.0f : Common::OotVr::HudSize().load()), 1,
                                                    std::bit_cast<u32>(Common::OotVr::HudPull().load()),
                                                    std::bit_cast<u32>(Common::OotVr::MapY().load()),
                                                    std::bit_cast<u32>(CaptureMode() ? 0.0f : Common::OotVr::TextScale(tan_x)),
                                                    std::bit_cast<u32>(Common::OotVr::PromptY().load()),
                                                    std::bit_cast<u32>(Common::OotVr::TalkY().load()),
                                                    std::bit_cast<u32>(hud_shift)};
                memory.WriteBlock(main_base + HudParamsOffset, hud_params.data(), sizeof(hud_params));
                UpdateMenuOpen(memory, main_base);
                std::array<u8, ProjectionCopySize> object;
                memory.ReadBlock(args[3], object.data(), object.size());
                p00 = 1.0f / tan_x, p11 = 1.0f / tan_y;
                for (const size_t matrix : {0xC, 0x4C}) {
                    std::memcpy(object.data() + matrix, &p00, 4);
                    std::memcpy(object.data() + matrix + 20, &p11, 4);
                    if (CaptureMode()) {
                        std::memcpy(object.data() + matrix + 40, CaptureDepthRow.data(), sizeof(CaptureDepthRow));
                    } else if (Common::OotVr::TalkCamera().load(std::memory_order_relaxed) &&
                               Common::OotVr::TalkCameraMode().load(std::memory_order_relaxed) != 0) {
                        SetNearPlane(object.data() + matrix, TalkNear());
                    }
                }
                const auto fov = WideFields(tan_x, tan_y);
                std::memcpy(object.data() + 0xA0, fov.data(), sizeof(fov));
                float near_m;
                std::memcpy(&near_m, object.data() + 0x98, 4);
                if (!CaptureMode() && Common::OotVr::TalkCamera().load(std::memory_order_relaxed) &&
                    Common::OotVr::TalkCameraMode().load(std::memory_order_relaxed) != 0 && near_m > TalkNear()) {
                    near_m = TalkNear();
                    std::memcpy(object.data() + 0x98, &near_m, 4);
                }
                memory.WriteBlock(main_base + ProjectionCopyOffset, object.data(), object.size());
                // Talk cameras narrow the game's own projection after the camera update; the clouds read it.
                memory.WriteBlock(args[3], object.data(), object.size());
                args[3] = main_base + ProjectionCopyOffset;
            }
            Common::OotVr::SetGameProjection(p00, p11);
        }
        return true;
    }
    // EDEN_NX2_OOT_VR_VIEWS: bit n turns view n (default 1: view 0 only).
    static const u32 views = [] {
        const char* const v = std::getenv("EDEN_NX2_OOT_VR_VIEWS");
        return v && *v ? static_cast<u32>(std::strtoul(v, nullptr, 0)) : 1u;
    }();
    if (view_index > 2 || !(views >> view_index & 1) || (view_index != 0 && !from_camera)) {
        return true;
    }
    float tan_x, tan_y;
    if (from_camera && memory.Read64(args[3]) == main_base + ProjectionVtableOffset) {
        float p00, p11;
        memory.ReadBlock(args[3] + 0xC, &p00, 4);
        memory.ReadBlock(args[3] + 0xC + 20, &p11, 4);
        // The headset's field of view: the game draws the whole headset view, not a window in it.
        if (Common::OotVr::HeadsetTan(tan_x, tan_y)) {
            std::array<u8, ProjectionCopySize> object;
            memory.ReadBlock(args[3], object.data(), object.size());
            p00 = 1.0f / tan_x, p11 = 1.0f / tan_y;
            for (const size_t matrix : {0xC, 0x4C}) {
                std::memcpy(object.data() + matrix, &p00, 4);
                std::memcpy(object.data() + matrix + 20, &p11, 4);
            }
            const u64 projection = main_base + ProjectionCopyOffset + view_index * ProjectionCopySize;
            memory.WriteBlock(projection, object.data(), object.size());
            args[3] = projection;
        }
        if (display) {
            Common::OotVr::SetGameProjection(p00, p11);
        }
    }
    Head head;
    if (!GetHead(memory, main_base, display, head)) {
        return true;
    }
    const float move = MoveScale();
    const u64 copy = view_index == 0 ? main_base + CopyOffset
                                     : main_base + ViewCopyOffset + (view_index - 1) * 0x60;
    std::array<float, 12> view;
    if (!memory.ReadBlock(args[2], view.data(), sizeof(view))) {
        return true;
    }
    TurnView(view.data(), head, move);
    memory.WriteBlock(copy, view.data(), sizeof(view));
    if (args[4] == args[2]) {
        args[4] = copy;
    } else if (memory.ReadBlock(args[4], view.data(), sizeof(view))) {
        TurnView(view.data(), head, move);
        memory.WriteBlock(copy + sizeof(view), view.data(), sizeof(view));
        args[4] = copy + sizeof(view);
    }
    args[2] = copy;
    return true;
}

} // namespace Kernel::Svc
