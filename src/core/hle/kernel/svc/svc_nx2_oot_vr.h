// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Kernel::Svc {

/// Ocarina of Time VR camera: svc 0xF4, placed by the built-in patch "VR camera" over
/// `ldr w10, [x0, #0x10]` at main+0x33A428 in the renderer's set-view function (main+0x33A410).
/// Does that load, then points x2 and x4 (the 3x4 view) at a head-turned copy for view 0.
/// Returns false (and does nothing) for any other SVC or process.
bool CallNx2OotVr(Core::System& system, u32 imm, std::span<u64, 8> args);

} // namespace Kernel::Svc
