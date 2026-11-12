// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

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
constexpr float Deg = 0.017453292519943295f;
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

struct Head {
    std::array<float, 3> fwd;
    std::array<float, 3> pos;
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
// main+TestPoseOffset while the game runs (u32 2 turns it off).
bool GetHead(Core::Memory::Memory& memory, u64 main_base, bool display, Head& head) {
    static const char* const yaw_text = std::getenv("EDEN_NX2_OOT_VR_FAKE_YAW");
    static const char* const pitch_text = std::getenv("EDEN_NX2_OOT_VR_FAKE_PITCH");
    float yaw = yaw_text ? static_cast<float>(std::atof(yaw_text)) : 0.0f;
    float pitch = pitch_text ? static_cast<float>(std::atof(pitch_text)) : 0.0f;
    bool test = yaw_text || pitch_text;
    std::array<u32, 3> written{};
    memory.ReadBlock(main_base + TestPoseOffset, written.data(), sizeof(written));
    if (written[0] == 1) {
        std::memcpy(&yaw, &written[1], 4);
        std::memcpy(&pitch, &written[2], 4);
        test = true;
    } else if (written[0] == 2) {
        return false;
    }
    if (test) {
        yaw *= Deg;
        pitch *= Deg;
        head.fwd = {-std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                    -std::cos(yaw) * std::cos(pitch)};
        head.pos = {};
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

// Projection +0xA0 fovy, +0xA4 sin(fovy/2), +0xA8 cos(fovy/2), +0xAC tan(fovy/2), +0xB0 aspect.
std::array<float, 5> WideFields(float tan_x, float tan_y) {
    const float half = std::atan(tan_y);
    return {2 * half, std::sin(half), std::cos(half), tan_y, tan_x / tan_y};
}

// View object: 3x4 view at +0x8, eye float3 +0x38, look-at float3 +0x44, up float3 +0x50, eye
// double3 +0x60, look-at double3 +0x78 (written by main+0x4C48C0).
void CameraSource(Core::Memory::Memory& memory, u64 main_base, u64 view_object) {
    static std::atomic<u64> turned{}, other{};
    const bool mine = view_object != 0 && IsDisplayView(view_object);
    const u64 count = (mine ? turned : other).fetch_add(1, std::memory_order_relaxed) + 1;
    if (mine && count % 300 == 0) {
        LOG_INFO(Kernel_SVC, "OoT VR camera: {} displayed camera updates turned, {} other camera updates",
                 count, other.load(std::memory_order_relaxed));
    }
    if (!mine) {
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
    if (!GetHead(memory, main_base, true, head)) {
        return;
    }
    std::array<float, 12> m;
    std::array<float, 6> eye_at;
    memory.ReadBlock(view_object + 0x8, m.data(), sizeof(m));
    memory.ReadBlock(view_object + 0x38, eye_at.data(), sizeof(eye_at));
    const float dx = eye_at[3] - eye_at[0], dy = eye_at[4] - eye_at[1], dz = eye_at[5] - eye_at[2];
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    TurnView(m.data(), head, MoveScale());
    memory.WriteBlock(view_object + 0x8, m.data(), sizeof(m));
    std::array<float, 3> eye;
    for (int k = 0; k < 3; ++k) {
        eye[k] = -(m[k] * m[3] + m[4 + k] * m[7] + m[8 + k] * m[11]);
    }
    const std::array<float, 3> at{eye[0] - m[8] * distance, eye[1] - m[9] * distance,
                                  eye[2] - m[10] * distance};
    const std::array<float, 9> floats{eye[0], eye[1], eye[2], at[0], at[1], at[2], m[4], m[5], m[6]};
    memory.WriteBlock(view_object + 0x38, floats.data(), sizeof(floats));
    const std::array<double, 6> doubles{eye[0], eye[1], eye[2], at[0], at[1], at[2]};
    memory.WriteBlock(view_object + 0x60, doubles.data(), sizeof(doubles));
}

} // Anonymous namespace

bool CallNx2OotVr(Core::System& system, u32 imm, std::span<u64, 8> args) {
    if (imm != VrSvc && imm != CameraSvc) {
        return false;
    }
    auto& kernel = system.Kernel();
    auto& process = GetCurrentProcess(kernel);
    if (!process.Is64Bit() || process.GetProgramId() != OotProgramId) {
        return false;
    }
    auto* const arm = process.GetArmInterface(kernel.CurrentPhysicalCoreIndex());
    auto& memory = process.GetMemory();
    ThreadContext ctx{};
    arm->GetContext(ctx);
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
            g_display_projection.store(args[3], std::memory_order_relaxed);
            float p00, p11;
            memory.ReadBlock(args[3] + 0xC, &p00, 4);
            memory.ReadBlock(args[3] + 0xC + 20, &p11, 4);
            static std::atomic<u64> seen{};
            if (seen.fetch_add(1, std::memory_order_relaxed) % 120 == 0) {
                float fovy;
                memory.ReadBlock(args[3] + 0xA0, &fovy, 4);
                LOG_INFO(Kernel_SVC, "OoT VR camera: game projection at set-view P00 {:.3f} P11 {:.3f} fovy {:.3f}",
                         p00, p11, fovy);
            }
            float tan_x, tan_y;
            if (Common::OotVr::HeadsetTan(tan_x, tan_y)) {
                std::array<u8, ProjectionCopySize> object;
                memory.ReadBlock(args[3], object.data(), object.size());
                p00 = 1.0f / tan_x, p11 = 1.0f / tan_y;
                for (const size_t matrix : {0xC, 0x4C}) {
                    std::memcpy(object.data() + matrix, &p00, 4);
                    std::memcpy(object.data() + matrix + 20, &p11, 4);
                }
                const auto fov = WideFields(tan_x, tan_y);
                std::memcpy(object.data() + 0xA0, fov.data(), sizeof(fov));
                memory.WriteBlock(main_base + ProjectionCopyOffset, object.data(), object.size());
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
