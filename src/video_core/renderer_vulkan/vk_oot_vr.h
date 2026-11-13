// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
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

/// After a BeginFrame that returned an image: a headset image the caller clears to black in the
/// same submit, in COLOR_ATTACHMENT_OPTIMAL at the end (VK_NULL_HANDLE: none).
VkImage BlackImageToClear();

/// The room backdrop behind 2D screens: one image per eye and the matrix it is drawn with.
struct RoomEyes {
    std::array<VkImage, 2> images{};
    std::array<std::array<float, 16>, 2> view_projection{}; ///< column-major, room metres to clip
    VkFormat format = VK_FORMAT_UNDEFINED;
    u32 size = 0; ///< width and height of each image
};

/// A room backdrop is loaded: screens show it behind them instead of black.
void EnableRoom();

/// After a BeginFrame that returned an image, on a 2D screen: the eye images to draw the room into in
/// the same submit, in COLOR_ATTACHMENT_OPTIMAL at the end (false: none this present).
bool RoomToDraw(RoomEyes& eyes);

/// Ends the frame BeginFrame started, after the copy was submitted.
void EndFrame(std::mutex& queue_lock, bool drawn);

} // namespace Vulkan::OotVr
