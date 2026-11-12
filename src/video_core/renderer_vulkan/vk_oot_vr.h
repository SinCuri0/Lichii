// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
class Device;
}

/// Ocarina of Time VR through OpenXR (EDEN_NX2_OOT_VR=1): the game picture goes to the headset as a
/// projection layer at the head pose the camera hook (svc_nx2_oot_vr.cpp) drew it with.
namespace Vulkan::OotVr {

/// The Vulkan instance extensions the OpenXR runtime needs that the instance offers.
void AddInstanceExtensions(std::vector<const char*>& extensions,
                           const std::vector<VkExtensionProperties>& available);

/// The Vulkan device extensions the OpenXR runtime needs that the device offers.
void AddDeviceExtensions(std::set<std::string, std::less<>>& extensions,
                         const std::set<std::string, std::less<>>& supported);

/// Starts a headset frame for a present of a width x height picture of `format`. Returns the
/// headset image to copy the picture into (VK_NULL_HANDLE: nothing to draw this present).
/// `copy` is set when a plain copy fits (same size and bits), else the caller blits.
/// `queue_lock` guards the graphics queue the runtime may use.
VkImage BeginFrame(const Device& device, std::mutex& queue_lock, u32 width, u32 height,
                   VkFormat format, bool& copy);

/// Ends the frame BeginFrame started, after the copy was submitted.
void EndFrame(std::mutex& queue_lock, bool drawn);

} // namespace Vulkan::OotVr
