// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/stb.h"
#include "video_core/host_shaders/ocarina_vr_room_frag_spv.h"
#include "video_core/host_shaders/ocarina_vr_room_vert_spv.h"
#include "video_core/renderer_vulkan/vk_oot_vr_room.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {

constexpr std::array<char, 4> Magic{'O', 'V', 'R', '3'};
constexpr u32 VertexBytes = 5 * sizeof(float);
constexpr VkFormat DepthFormat = VK_FORMAT_D32_SFLOAT;

struct Header {
    std::array<char, 4> magic;
    u32 vertex_count, index_count, skin_count, texture_width, texture_height;
};

vk::Buffer Upload(MemoryAllocator& allocator, std::span<const u8> data, VkBufferUsageFlags usage) {
    vk::Buffer buffer = allocator.CreateBuffer(
        VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = data.size(),
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        },
        MemoryUsage::Upload);
    std::memcpy(buffer.Mapped().data(), data.data(), data.size());
    buffer.Flush();
    return buffer;
}

} // Anonymous namespace

std::unique_ptr<OotVrRoom> OotVrRoom::Load(const Device& device, MemoryAllocator& allocator) {
    const char* const env = std::getenv("EDEN_NX2_OOT_VR_ROOM");
    const std::filesystem::path path =
        env && *env ? std::filesystem::path{env}
                    : Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "oot_vr_room" / "room.bin";
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        LOG_INFO(Render_Vulkan, "OoT VR: no room backdrop at {}", path.string());
        return nullptr;
    }
    const std::vector<u8> data{std::istreambuf_iterator<char>{file}, {}};
    Header header{};
    if (data.size() < sizeof(header)) {
        return nullptr;
    }
    std::memcpy(&header, data.data(), sizeof(header));
    const size_t expected = sizeof(header) + size_t{header.vertex_count} * VertexBytes +
                            size_t{header.index_count} * sizeof(u32) +
                            size_t{header.texture_width} * header.texture_height * 4;
    if (header.magic != Magic || data.size() != expected || header.index_count == 0 ||
        header.skin_count > header.index_count) {
        LOG_WARNING(Render_Vulkan, "OoT VR: {} is not a room backdrop of this version", path.string());
        return nullptr;
    }
    LOG_INFO(Render_Vulkan, "OoT VR: room backdrop {}: {} triangles, texture {}x{}", path.string(),
             header.index_count / 3, header.texture_width, header.texture_height);
    return std::make_unique<OotVrRoom>(device, allocator, data);
}

OotVrRoom::OotVrRoom(const Device& device_, MemoryAllocator& allocator_, std::span<const u8> file)
    : device{device_}, allocator{allocator_} {
    if (const char* const shot_env = std::getenv("EDEN_NX2_OOT_VR_ROOM_SHOT"); shot_env && *shot_env) {
        shot_path = shot_env;
    }
    Header header{};
    std::memcpy(&header, file.data(), sizeof(header));
    index_count = header.index_count;
    skin_count = header.skin_count;
    texture_width = header.texture_width;
    texture_height = header.texture_height;
    size_t at = sizeof(header);
    const size_t vertex_bytes = size_t{header.vertex_count} * VertexBytes;
    vertices = Upload(allocator, file.subspan(at, vertex_bytes), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    at += vertex_bytes;
    indices = Upload(allocator, file.subspan(at, size_t{index_count} * sizeof(u32)), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    at += size_t{index_count} * sizeof(u32);
    texels = Upload(allocator, file.subspan(at), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

    const auto& logical = device.GetLogical();
    texture = allocator.CreateImage({
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .extent = {texture_width, texture_height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    texture_view = logical.CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *texture,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    });
    sampler = logical.CreateSampler({
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0,
    });
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                               VK_SHADER_STAGE_FRAGMENT_BIT};
    set_layout = logical.CreateDescriptorSetLayout({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    });
    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    descriptor_pool = logical.CreateDescriptorPool({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    });
    const VkDescriptorSetLayout raw_layout = *set_layout;
    sets = descriptor_pool.Allocate({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = *descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &raw_layout,
    });
    const VkDescriptorImageInfo image{*sampler, *texture_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[0], 0, 0, 1,
                                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &image};
    logical.UpdateDescriptorSets(write, {});
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(OotVr::RoomEyes::view_projection[0])};
    layout = logical.CreatePipelineLayout({
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &raw_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    });
}

void OotVrRoom::Prepare(VkFormat format_, u32 size_) {
    if (format == format_ && size == size_) {
        return;
    }
    const auto& logical = device.GetLogical();
    targets.clear();
    format = format_, size = size_;
    depth = allocator.CreateImage({
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = DepthFormat,
        .extent = {size, size, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    depth_view = logical.CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *depth,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = DepthFormat,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    });
    const std::array attachments{
        VkAttachmentDescription{
            .format = format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        },
        VkAttachmentDescription{
            .format = DepthFormat,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        },
    };
    const VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass{
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
        .pDepthStencilAttachment = &depth_ref,
    };
    const VkSubpassDependency dependency{
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };
    render_pass = logical.CreateRenderPass({
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = static_cast<u32>(attachments.size()),
        .pAttachments = attachments.data(),
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    });

    const auto vertex = logical.CreateShaderModule({
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(OCARINA_VR_ROOM_VERT_SPV),
        .pCode = std::data(OCARINA_VR_ROOM_VERT_SPV),
    });
    const auto fragment = logical.CreateShaderModule({
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(OCARINA_VR_ROOM_FRAG_SPV),
        .pCode = std::data(OCARINA_VR_ROOM_FRAG_SPV),
    });
    const std::array stages{
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                        VK_SHADER_STAGE_VERTEX_BIT, *vertex, "main"},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                        VK_SHADER_STAGE_FRAGMENT_BIT, *fragment, "main"},
    };
    const VkVertexInputBindingDescription vertex_binding{0, VertexBytes, VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array vertex_attributes{
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32_SFLOAT, 3 * sizeof(float)},
    };
    const VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vertex_binding,
        .vertexAttributeDescriptionCount = static_cast<u32>(vertex_attributes.size()),
        .pVertexAttributeDescriptions = vertex_attributes.data(),
    };
    const VkPipelineInputAssemblyStateCreateInfo assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkViewport viewport{0, 0, static_cast<float>(size), static_cast<float>(size), 0, 1};
    const VkRect2D scissor{{0, 0}, {size, size}};
    const VkPipelineViewportStateCreateInfo viewport_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };
    const VkPipelineRasterizationStateCreateInfo raster{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1,
    };
    const VkPipelineMultisampleStateCreateInfo samples{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineDepthStencilStateCreateInfo depth_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };
    const VkPipelineColorBlendAttachmentState write{
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &write,
    };
    VkGraphicsPipelineCreateInfo pipeline_info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &raster,
        .pMultisampleState = &samples,
        .pDepthStencilState = &depth_state,
        .pColorBlendState = &blend,
        .layout = *layout,
        .renderPass = *render_pass,
        .basePipelineIndex = -1,
    };
    pipeline = logical.CreateGraphicsPipeline(pipeline_info);
    const VkPipelineDepthStencilStateCreateInfo skin_depth{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_FALSE,
        .depthWriteEnable = VK_FALSE,
    };
    pipeline_info.pDepthStencilState = &skin_depth;
    skin_pipeline = logical.CreateGraphicsPipeline(pipeline_info);
    LOG_INFO(Render_Vulkan, "OoT VR: room backdrop drawn at {}x{} per eye, format {}", size, size,
             static_cast<int>(format));
}

void OotVrRoom::RecordShot(vk::CommandBuffer cmdbuf, VkImage image) {
    shot = allocator.CreateBuffer(
        VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = VkDeviceSize{size} * size * 4,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        },
        MemoryUsage::Download);
    VkImageMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, barrier);
    const VkBufferImageCopy copy{
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {size, size, 1},
    };
    cmdbuf.CopyImageToBuffer(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, *shot, copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, barrier);
    shot_wait = 10;
}

void OotVrRoom::WriteShot() {
    shot.Invalidate();
    std::vector<u8> rgb(size_t{size} * size * 3);
    const u8* const texels_out = shot.Mapped().data();
    const bool bgr = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
    for (size_t i = 0; i < size_t{size} * size; ++i) {
        rgb[i * 3 + 0] = texels_out[i * 4 + (bgr ? 2 : 0)];
        rgb[i * 3 + 1] = texels_out[i * 4 + 1];
        rgb[i * 3 + 2] = texels_out[i * 4 + (bgr ? 0 : 2)];
    }
    std::vector<u8> encoded;
    stbi_write_png_to_func(
        [](void* context, void* bytes, int count) {
            auto* const out = static_cast<std::vector<u8>*>(context);
            out->insert(out->end(), static_cast<const u8*>(bytes), static_cast<const u8*>(bytes) + count);
        },
        &encoded, static_cast<int>(size), static_cast<int>(size), 3, rgb.data(), static_cast<int>(size * 3));
    std::ofstream{shot_path, std::ios::binary}.write(reinterpret_cast<const char*>(encoded.data()),
                                                     static_cast<std::streamsize>(encoded.size()));
    LOG_INFO(Render_Vulkan, "OoT VR: room backdrop left eye saved to {}", shot_path);
    shot_path.clear();
    shot = {};
}

void OotVrRoom::Draw(vk::CommandBuffer cmdbuf, const OotVr::RoomEyes& eyes) {
    Prepare(eyes.format, eyes.size);
    if (shot_wait > 0 && --shot_wait == 0) {
        WriteShot();
    }
    if (!uploaded) {
        uploaded = true;
        VkImageMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = *texture,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, barrier);
        const VkBufferImageCopy copy{
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {texture_width, texture_height, 1},
        };
        cmdbuf.CopyBufferToImage(*texels, *texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, barrier);
    }
    const auto& logical = device.GetLogical();
    for (size_t eye = 0; eye < 2; ++eye) {
        const VkImage image = eyes.images[eye];
        auto [it, added] = targets.try_emplace(image);
        if (added) {
            it->second.view = logical.CreateImageView({
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = format,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            });
            const std::array views{*it->second.view, *depth_view};
            it->second.framebuffer = logical.CreateFramebuffer({
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = *render_pass,
                .attachmentCount = static_cast<u32>(views.size()),
                .pAttachments = views.data(),
                .width = size,
                .height = size,
                .layers = 1,
            });
        }
        const std::array clears{VkClearValue{.color = {}}, VkClearValue{.depthStencil = {1.0f, 0}}};
        cmdbuf.BeginRenderPass(
            {
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                .renderPass = *render_pass,
                .framebuffer = *it->second.framebuffer,
                .renderArea = {{0, 0}, {size, size}},
                .clearValueCount = static_cast<u32>(clears.size()),
                .pClearValues = clears.data(),
            },
            VK_SUBPASS_CONTENTS_INLINE);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, *layout, 0, sets[0], {});
        cmdbuf.PushConstants(*layout, VK_SHADER_STAGE_VERTEX_BIT, eyes.view_projection[eye]);
        cmdbuf.BindVertexBuffer(0, *vertices, 0);
        cmdbuf.BindIndexBuffer(*indices, 0, VK_INDEX_TYPE_UINT32);
        if (skin_count > 0) {
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *skin_pipeline);
            cmdbuf.DrawIndexed(skin_count, 1, 0, 0, 0);
        }
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline);
        cmdbuf.DrawIndexed(index_count - skin_count, 1, skin_count, 0, 0);
        cmdbuf.EndRenderPass();
        if (eye == 0 && !shot_path.empty() && shot_wait < 0) {
            RecordShot(cmdbuf, image);
        }
    }
}

} // namespace Vulkan
