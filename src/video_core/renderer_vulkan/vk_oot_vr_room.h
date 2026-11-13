// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "video_core/renderer_vulkan/vk_oot_vr.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;

/// The Ocarina of Time VR room backdrop (tools/nx2/oot_vr_room.py): a mesh with one texture, drawn
/// for each eye behind 2D screens.
class OotVrRoom {
public:
    /// The room in EDEN_NX2_OOT_VR_ROOM (a room.bin), else <user dir>/oot_vr_room/room.bin; null without one.
    static std::unique_ptr<OotVrRoom> Load(const Device& device, MemoryAllocator& allocator);

    OotVrRoom(const Device& device, MemoryAllocator& allocator, std::span<const u8> file);

    void Draw(vk::CommandBuffer cmdbuf, const OotVr::RoomEyes& eyes);

private:
    void Prepare(VkFormat format, u32 size);
    void RecordShot(vk::CommandBuffer cmdbuf, VkImage image);
    void WriteShot();

    const Device& device;
    MemoryAllocator& allocator;
    u32 index_count = 0;
    u32 skin_count = 0; ///< the first indices: stretched triangles across depth edges, drawn under the rest
    u32 texture_width = 0, texture_height = 0;
    bool uploaded = false;
    vk::Buffer vertices, indices, texels;
    vk::Image texture;
    vk::ImageView texture_view;
    vk::Sampler sampler;
    vk::DescriptorSetLayout set_layout;
    vk::DescriptorPool descriptor_pool;
    vk::DescriptorSets sets;
    vk::PipelineLayout layout;
    VkFormat format = VK_FORMAT_UNDEFINED;
    u32 size = 0;
    vk::Image depth;
    vk::ImageView depth_view;
    vk::RenderPass render_pass;
    vk::Pipeline pipeline;
    vk::Pipeline skin_pipeline;
    struct Target {
        vk::ImageView view;
        vk::Framebuffer framebuffer;
    };
    std::unordered_map<VkImage, Target> targets;
    std::string shot_path; ///< EDEN_NX2_OOT_VR_ROOM_SHOT: a PNG of the first left-eye picture
    vk::Buffer shot;
    int shot_wait = -1; ///< draws until the copy is surely done
};

} // namespace Vulkan
