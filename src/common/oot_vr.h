// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdlib>
#include <mutex>

#include "common/common_types.h"

/// Ocarina of Time VR: what the OpenXR side (video_core/renderer_vulkan/vk_oot_vr.cpp) and the
/// camera hook (core/hle/kernel/svc/svc_nx2_oot_vr.cpp) share.
namespace Common::OotVr {

struct HeadPose {
    std::array<float, 4> orientation{0, 0, 0, 1}; ///< x, y, z, w in the headset's local space
    std::array<float, 3> position{};              ///< metres from the first pose seen
};

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
    std::array<HeadPose, AppliedRing> applied{}; ///< by camera update, given to the game camera
    float p00 = 0, p11 = 0;                      ///< the main view's projection as drawn
    float tan_x = 0, tan_y = 0;                  ///< the headset's widest half-angle tangents
};

inline State& Get() {
    static State state;
    return state;
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

/// The pose the camera was given `lag` camera updates before the newest one.
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
