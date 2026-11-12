// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>

#include "common/common_types.h"

/// Ocarina of Time VR: what the OpenXR side (video_core/renderer_vulkan/vk_oot_vr.cpp) and the
/// camera hook (core/hle/kernel/svc/svc_nx2_oot_vr.cpp) share.
namespace Common::OotVr {

struct HeadPose {
    std::array<float, 4> orientation{0, 0, 0, 1}; ///< x, y, z, w in the headset's local space
    std::array<float, 3> position{};              ///< metres from the first pose seen
    int eye = -1;                                 ///< the eye the camera was moved to: 0 left, 1 right, -1 none
};

/// EDEN_NX2_OOT_VR_STEREO=0: no alternate-eye frames (both eyes get every picture).
inline bool StereoWanted() {
    static const bool on = [] {
        const char* const v = std::getenv("EDEN_NX2_OOT_VR_STEREO");
        return !(v && *v == '0');
    }();
    return on;
}

/// EDEN_NX2_OOT_VR=1: the VR camera patch and, unless EDEN_NX2_OOT_VR_XR=0, the headset.
inline bool Wanted() {
    static const bool on = [] {
        const char* const v = std::getenv("EDEN_NX2_OOT_VR");
        return v && *v && *v != '0';
    }();
    return on;
}

inline bool HeadsetWanted() {
    static const bool on = [] {
        const char* const v = std::getenv("EDEN_NX2_OOT_VR_XR");
        return Wanted() && !(v && *v == '0');
    }();
    return on;
}

inline constexpr size_t AppliedRing = 8;

struct State {
    std::mutex lock;
    bool has_head = false;
    HeadPose head;
    u64 applied_count = 0;
    std::array<HeadPose, AppliedRing> applied{}; ///< by drawn frame, the pose its camera was turned with
    float p00 = 0, p11 = 0;                      ///< the main view's projection as drawn
    float tan_x = 0, tan_y = 0;                  ///< the headset's widest half-angle tangents
    bool has_eyes = false;
    std::array<std::array<float, 3>, 2> eyes{};  ///< eye positions in head space, metres
};

inline State& Get() {
    static State state;
    return state;
}

/// First person: the eye at Link's head and his head hidden. EDEN_NX2_OOT_VR_FIRST=0 starts it off.
inline std::atomic<bool>& FirstPersonFlag() {
    static std::atomic<bool> on = [] {
        const char* const v = std::getenv("EDEN_NX2_OOT_VR_FIRST");
        return !(v && *v == '0');
    }();
    return on;
}

inline bool FirstPerson() {
    return FirstPersonFlag().load(std::memory_order_relaxed);
}

inline float EnvFloat(const char* name, float fallback) {
    const char* const v = std::getenv(name);
    return v && *v ? static_cast<float>(std::atof(v)) : fallback;
}

/// The HUD's size (1 = as the game has it). EDEN_NX2_OOT_VR_HUD sets the start value.
inline std::atomic<float>& HudSize() {
    static std::atomic<float> size = EnvFloat("EDEN_NX2_OOT_VR_HUD", 0.35f);
    return size;
}

/// How far the HUD moves from the screen edges toward the centre (0 = at the edges). EDEN_NX2_OOT_VR_HUD_IN sets the start value.
inline std::atomic<float>& HudPull() {
    static std::atomic<float> pull = EnvFloat("EDEN_NX2_OOT_VR_HUD_IN", 0.75f);
    return pull;
}

/// The minimap's extra height in layout units (the screen is 2156 high; negative = down). EDEN_NX2_OOT_VR_MAP_Y sets the start value.
inline std::atomic<float>& MapY() {
    static std::atomic<float> y = EnvFloat("EDEN_NX2_OOT_VR_MAP_Y", -125.0f);
    return y;
}

/// Eye distance as a multiple of the headset's: more = more depth far away, and the world looks smaller. EDEN_NX2_OOT_VR_EYE_SCALE sets the start value.
inline std::atomic<float>& EyeScale() {
    static std::atomic<float> scale = EnvFloat("EDEN_NX2_OOT_VR_EYE_SCALE", 1.5f);
    return scale;
}

/// Set while a game menu is open: the headset then shows the game picture as a flat screen.
inline std::atomic<bool>& MenuOpen() {
    static std::atomic<bool> open{false};
    return open;
}

/// Distance in metres from the head to the flat menu screen. EDEN_NX2_OOT_VR_MENU_DIST sets the start value.
inline std::atomic<float>& MenuDistance() {
    static std::atomic<float> distance = EnvFloat("EDEN_NX2_OOT_VR_MENU_DIST", 1.0f);
    return distance;
}

/// Extra height of the action prompt and button guide, in layout units (negative = down).
inline std::atomic<float>& PromptY() {
    static std::atomic<float> y = EnvFloat("EDEN_NX2_OOT_VR_PROMPT_Y", -100.0f);
    return y;
}

/// Extra height of the talk box and cutscene text, in layout units (negative = down).
inline std::atomic<float>& TalkY() {
    static std::atomic<float> y = EnvFloat("EDEN_NX2_OOT_VR_TALK_Y", 0.0f);
    return y;
}

/// The HUD's apparent distance in metres: each eye's HUD moves sideways by that eye's view of it.
constexpr float HudDistanceM = 1.5f;

/// Swaps the HUD's sideways move between the eyes (key 0).
inline std::atomic<bool>& HudSwap() {
    static std::atomic<bool> swap{false};
    return swap;
}

/// Width of the flat menu screen per metre of distance from the head.
constexpr float MenuWidthPerM = 0.93f;

/// The size of the talk box and cutscene text (1 = as big as on the flat menu screen). EDEN_NX2_OOT_VR_TEXT sets the start value.
inline std::atomic<float>& TextSize() {
    static std::atomic<float> size = EnvFloat("EDEN_NX2_OOT_VR_TEXT", 1.0f);
    return size;
}

/// The talk box scale in the game picture that makes it look as big as on the flat menu screen.
inline float TextScale(float tan_x) {
    return TextSize().load(std::memory_order_relaxed) * MenuWidthPerM / 2 / tan_x;
}

inline void SetHead(const HeadPose& pose) {
    State& s = Get();
    std::scoped_lock lk{s.lock};
    s.head = pose;
    s.has_head = true;
}

/// The newest head pose for the camera; record = the displayed camera uses it this frame.
inline bool HeadForCamera(HeadPose& out, bool record) {
    State& s = Get();
    std::scoped_lock lk{s.lock};
    if (!s.has_head) {
        return false;
    }
    out = s.head;
    if (record) {
        s.applied[s.applied_count++ % AppliedRing] = s.head;
    }
    return true;
}

inline void RecordApplied(const HeadPose& pose) {
    State& s = Get();
    std::scoped_lock lk{s.lock};
    s.applied[s.applied_count++ % AppliedRing] = pose;
}

inline u64 AppliedCount() {
    State& s = Get();
    std::scoped_lock lk{s.lock};
    return s.applied_count;
}

/// The pose the camera was given `lag` recorded frames before the newest one.
inline bool AppliedPose(u64 lag, HeadPose& out) {
    State& s = Get();
    std::scoped_lock lk{s.lock};
    if (s.applied_count <= lag) {
        return false;
    }
    out = s.applied[(s.applied_count - 1 - lag) % AppliedRing];
    return true;
}

inline void SetHeadsetTan(float tan_x, float tan_y) {
    State& s = Get();
    std::scoped_lock lk{s.lock};
    s.tan_x = tan_x, s.tan_y = tan_y;
}

inline bool HeadsetTan(float& tan_x, float& tan_y) {
    State& s = Get();
    std::scoped_lock lk{s.lock};
    tan_x = s.tan_x, tan_y = s.tan_y;
    // EDEN_NX2_OOT_VR_FAKE_TAN=x,y: a headset field of view for runs without a headset.
    static const char* const fake = std::getenv("EDEN_NX2_OOT_VR_FAKE_TAN");
    if (fake && !(tan_x > 0)) {
        char* end = nullptr;
        tan_x = std::strtof(fake, &end);
        tan_y = *end == ',' ? std::strtof(end + 1, nullptr) : 0.0f;
    }
    return tan_x > 0 && tan_y > 0;
}

inline void SetEyes(const std::array<std::array<float, 3>, 2>& eyes) {
    State& s = Get();
    std::scoped_lock lk{s.lock};
    s.eyes = eyes;
    s.has_eyes = true;
}

inline std::array<float, 3> Rotate(const std::array<float, 4>& q, const std::array<float, 3>& v) {
    const std::array<float, 3> a{q[1] * v[2] - q[2] * v[1], q[2] * v[0] - q[0] * v[2], q[0] * v[1] - q[1] * v[0]};
    const std::array<float, 3> b{q[1] * a[2] - q[2] * a[1], q[2] * a[0] - q[0] * a[2], q[0] * a[1] - q[1] * a[0]};
    return {v[0] + 2 * (q[3] * a[0] + b[0]), v[1] + 2 * (q[3] * a[1] + b[1]), v[2] + 2 * (q[3] * a[2] + b[2])};
}

/// The HUD's sideways move for `eye` in layout units (the screen is 3840 wide): it then looks HudDistanceM away.
inline float HudShift(int eye, float tan_x) {
    State& s = Get();
    std::scoped_lock lk{s.lock};
    if (!s.has_eyes || eye < 0 || eye > 1 || !(tan_x > 0)) {
        return 0.0f;
    }
    const float x = s.eyes[eye][0] * EyeScale().load(std::memory_order_relaxed);
    const float shift = -x / HudDistanceM / tan_x * 1920.0f;
    return HudSwap().load(std::memory_order_relaxed) ? -shift : shift;
}

/// Head position plus the offset of `eye` turned by the head, in the headset's local space.
inline bool EyePosition(const HeadPose& pose, int eye, std::array<float, 3>& out) {
    State& s = Get();
    std::scoped_lock lk{s.lock};
    // EDEN_NX2_OOT_VR_FAKE_IPD=metres: eye positions for runs without a headset.
    static const char* const fake = std::getenv("EDEN_NX2_OOT_VR_FAKE_IPD");
    if (!s.has_eyes && fake) {
        const float half = std::strtof(fake, nullptr) / 2;
        s.eyes = {{{-half, 0, 0}, {half, 0, 0}}};
        s.has_eyes = half > 0;
    }
    if (!s.has_eyes || eye < 0 || eye > 1) {
        return false;
    }
    const float k = EyeScale().load(std::memory_order_relaxed);
    const auto e = Rotate(pose.orientation, {s.eyes[eye][0] * k, s.eyes[eye][1] * k, s.eyes[eye][2] * k});
    out = {pose.position[0] + e[0], pose.position[1] + e[1], pose.position[2] + e[2]};
    return true;
}

inline void SetGameProjection(float p00, float p11) {
    State& s = Get();
    std::scoped_lock lk{s.lock};
    s.p00 = p00, s.p11 = p11;
}

inline bool GameProjection(float& p00, float& p11) {
    State& s = Get();
    std::scoped_lock lk{s.lock};
    p00 = s.p00, p11 = s.p11;
    return p00 > 0 && p11 > 0;
}

} // namespace Common::OotVr
