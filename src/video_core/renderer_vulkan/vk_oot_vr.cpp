// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "common/logging.h"
#include "common/oot_vr.h"
#include "video_core/renderer_vulkan/vk_oot_vr.h"
#include "video_core/vulkan_common/vulkan_device.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace Vulkan::OotVr {
namespace {

constexpr float ScreenDistanceM = 2.0f;
constexpr float ScreenWidthM = 3.2f;
// Camera updates between the one a picture was drawn with and the present that sends it.
constexpr u64 DefaultLag = 1;

struct Xr {
    int state = 0; ///< 0 not tried, 1 instance and system ready, -1 off
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    XrSpace head = XR_NULL_HANDLE;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::array<XrSwapchainImageVulkanKHR, 16> images{};
    u32 image_count = 0, width = 0, height = 0, index = 0;
    VkFormat source_format = VK_FORMAT_UNDEFINED;
    bool copy = false;
    std::vector<int64_t> formats;
    bool running = false, in_frame = false, shown = false;
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    std::array<XrVector3f, 2> eye{};
    std::array<XrFovf, 2> eye_fov{};
    bool has_eyes = false;
    Common::OotVr::HeadPose sent;
    bool has_sent = false;
    std::string instance_extensions, device_extensions;
    PFN_xrGetVulkanInstanceExtensionsKHR get_instance_extensions = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR get_device_extensions = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR get_graphics_device = nullptr;
    PFN_xrGetVulkanGraphicsRequirementsKHR get_graphics_requirements = nullptr;
};

Xr X;

bool Ok(XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    char text[XR_MAX_RESULT_STRING_SIZE] = "";
    if (X.instance) {
        xrResultToString(X.instance, result, text);
    }
    LOG_WARNING(Render_Vulkan, "OoT VR: {} failed: {} ({})", what, text, static_cast<int>(result));
    return false;
}

void Off() {
    X.state = -1;
}

bool InstanceUp() {
    if (X.state != 0) {
        return X.state > 0;
    }
    X.state = -1;
    if (!Common::OotVr::HeadsetWanted()) {
        return false;
    }
    u32 count = 0;
    if (!Ok(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr),
            "xrEnumerateInstanceExtensionProperties (is an OpenXR runtime active?)")) {
        return false;
    }
    std::vector<XrExtensionProperties> properties(count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, properties.data());
    const bool has_vulkan = std::ranges::any_of(properties, [](const XrExtensionProperties& p) {
        return std::strcmp(p.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0;
    });
    if (!has_vulkan) {
        LOG_WARNING(Render_Vulkan, "OoT VR: the OpenXR runtime has no {}",
                    XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
        return false;
    }
    const char* const names[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(info.applicationInfo.applicationName, "Fritchii OoT VR",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(info.applicationInfo.engineName, "Fritchii", XR_MAX_ENGINE_NAME_SIZE - 1);
    info.applicationInfo.applicationVersion = 1;
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = names;
    if (!Ok(xrCreateInstance(&info, &X.instance), "xrCreateInstance")) {
        return false;
    }
    const auto load = [](const char* name, auto& function) {
        return Ok(xrGetInstanceProcAddr(X.instance, name,
                                        reinterpret_cast<PFN_xrVoidFunction*>(&function)),
                  name);
    };
    if (!load("xrGetVulkanInstanceExtensionsKHR", X.get_instance_extensions) ||
        !load("xrGetVulkanDeviceExtensionsKHR", X.get_device_extensions) ||
        !load("xrGetVulkanGraphicsDeviceKHR", X.get_graphics_device) ||
        !load("xrGetVulkanGraphicsRequirementsKHR", X.get_graphics_requirements)) {
        xrDestroyInstance(X.instance);
        X.instance = XR_NULL_HANDLE;
        return false;
    }
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(X.instance, &ip))) {
        LOG_INFO(Render_Vulkan, "OoT VR: OpenXR runtime {} {}.{}.{}", ip.runtimeName,
                 XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
                 XR_VERSION_PATCH(ip.runtimeVersion));
    }
    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Ok(xrGetSystem(X.instance, &system_info, &X.system),
            "xrGetSystem (is the headset connected?)")) {
        xrDestroyInstance(X.instance);
        X.instance = XR_NULL_HANDLE;
        return false;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(X.instance, X.system, &sp))) {
        LOG_INFO(Render_Vulkan, "OoT VR: headset {}", sp.systemName);
    }
    X.state = 1;
    return true;
}

std::vector<std::string> Split(const std::string& list) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < list.size()) {
        const size_t end = std::min(list.find(' ', start), list.size());
        if (end > start) {
            out.emplace_back(list.substr(start, end - start));
        }
        start = end + 1;
    }
    return out;
}

bool CreateSession(const Device& device) {
    XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (!Ok(X.get_graphics_requirements(X.instance, X.system, &requirements),
            "xrGetVulkanGraphicsRequirementsKHR")) {
        return false;
    }
    VkPhysicalDevice wanted = VK_NULL_HANDLE;
    if (Ok(X.get_graphics_device(X.instance, X.system, device.GetInstance(), &wanted),
           "xrGetVulkanGraphicsDeviceKHR") &&
        wanted != static_cast<VkPhysicalDevice>(device.GetPhysical())) {
        LOG_WARNING(Render_Vulkan, "OoT VR: the headset wants another GPU than the one in use");
    }
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = device.GetInstance();
    binding.physicalDevice = static_cast<VkPhysicalDevice>(device.GetPhysical());
    binding.device = *device.GetLogical();
    binding.queueFamilyIndex = device.GetGraphicsFamily();
    binding.queueIndex = 0;
    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = &binding;
    session_info.systemId = X.system;
    if (!Ok(xrCreateSession(X.instance, &session_info, &X.session), "xrCreateSession")) {
        return false;
    }
    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_info.poseInReferenceSpace.orientation.w = 1;
    if (!Ok(xrCreateReferenceSpace(X.session, &space_info, &X.space), "xrCreateReferenceSpace")) {
        return false;
    }
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Ok(xrCreateReferenceSpace(X.session, &space_info, &X.head),
            "xrCreateReferenceSpace (view)")) {
        return false;
    }
    u32 count = 0;
    xrEnumerateSwapchainFormats(X.session, 0, &count, nullptr);
    X.formats.resize(count);
    if (!Ok(xrEnumerateSwapchainFormats(X.session, count, &count, X.formats.data()),
            "xrEnumerateSwapchainFormats") ||
        count == 0) {
        return false;
    }
    LOG_INFO(Render_Vulkan, "OoT VR: session created, {} swapchain formats", count);
    return true;
}

VkFormat SrgbTwin(VkFormat format) {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
        return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return VK_FORMAT_R8G8B8A8_SRGB;
    default:
        return format;
    }
}

bool MakeSwapchain(VkFormat source, u32 width, u32 height) {
    if (X.swapchain) {
        xrDestroySwapchain(X.swapchain);
        X.swapchain = XR_NULL_HANDLE;
    }
    // The picture holds sRGB-encoded values in a UNORM image: copied into the sRGB twin, the runtime
    // reads them as sRGB, as the window shows them.
    int64_t format = X.formats[0];
    X.copy = false;
    for (const int64_t f : X.formats) {
        if (f == static_cast<int64_t>(SrgbTwin(source))) {
            format = f;
            X.copy = true;
        }
    }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!Ok(xrCreateSwapchain(X.session, &info, &X.swapchain), "xrCreateSwapchain")) {
        return false;
    }
    X.images.fill({XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    if (!Ok(xrEnumerateSwapchainImages(X.swapchain, static_cast<u32>(X.images.size()),
                                       &X.image_count,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(X.images.data())),
            "xrEnumerateSwapchainImages")) {
        return false;
    }
    X.width = width, X.height = height, X.source_format = source;
    LOG_INFO(Render_Vulkan, "OoT VR: headset swapchain {}x{}, format {}, {} images, {}", width,
             height, format, X.image_count, X.copy ? "copied as it is" : "blit");
    return true;
}

void PollEvents() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(X.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrSessionState s = reinterpret_cast<XrEventDataSessionStateChanged*>(&event)->state;
            LOG_INFO(Render_Vulkan, "OoT VR: session state {}", static_cast<int>(s));
            if (s == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                X.running = Ok(xrBeginSession(X.session, &begin), "xrBeginSession");
            } else if (s == XR_SESSION_STATE_STOPPING) {
                xrEndSession(X.session);
                X.running = false;
            } else if (s == XR_SESSION_STATE_EXITING || s == XR_SESSION_STATE_LOSS_PENDING) {
                X.running = false;
                Off();
            }
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

XrVector3f Rotate(const XrQuaternionf& o, const XrVector3f& v) {
    const XrVector3f a{o.y * v.z - o.z * v.y, o.z * v.x - o.x * v.z, o.x * v.y - o.y * v.x};
    const XrVector3f b{o.y * a.z - o.z * a.y, o.z * a.x - o.x * a.z, o.x * a.y - o.y * a.x};
    return {v.x + 2 * (o.w * a.x + b.x), v.y + 2 * (o.w * a.y + b.y), v.z + 2 * (o.w * a.z + b.z)};
}

void SubmitFrame(bool with_picture) {
    const XrRect2Di rect{{0, 0}, {static_cast<int32_t>(X.width), static_cast<int32_t>(X.height)}};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.space = X.space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage = {X.swapchain, rect, 0};
    quad.pose = {{0, 0, 0, 1}, {0, 0, -ScreenDistanceM}};
    quad.size = {ScreenWidthM, X.width ? ScreenWidthM * static_cast<float>(X.height) / X.width : 1.0f};

    std::array<XrCompositionLayerProjectionView, 2> views{};
    XrCompositionLayerProjection world{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    world.space = X.space;
    world.viewCount = 2;
    world.views = views.data();
    float p00 = 0, p11 = 0;
    const bool has_projection = Common::OotVr::GameProjection(p00, p11);
    const bool in_world = X.has_sent && has_projection;
    static int last_world = -1;
    if (with_picture && last_world != static_cast<int>(in_world)) {
        last_world = in_world;
        LOG_INFO(Render_Vulkan, "OoT VR: now {} (head pose from the camera {}, game projection {} {:.3f} {:.3f})",
                 in_world ? "the world around the head" : "a screen", X.has_sent ? "yes" : "no",
                 has_projection ? "yes" : "no", p00, p11);
    }
    if (in_world) {
        const float ax = std::atan(1.0f / p00), ay = std::atan(1.0f / p11);
        const auto& q = X.sent.orientation;
        const XrQuaternionf o{q[0], q[1], q[2], q[3]};
        for (int i = 0; i < 2; ++i) {
            const XrVector3f e = Rotate(o, X.eye[i]);
            views[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            views[i].pose.orientation = o;
            views[i].pose.position = {X.sent.position[0] + e.x, X.sent.position[1] + e.y,
                                      X.sent.position[2] + e.z};
            views[i].fov = {-ax, ax, ay, -ay};
            views[i].subImage = {X.swapchain, rect, 0};
        }
    }
    const XrCompositionLayerBaseHeader* layer =
        in_world ? reinterpret_cast<const XrCompositionLayerBaseHeader*>(&world)
                 : reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = X.frame_state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = with_picture ? 1 : 0;
    end.layers = with_picture ? &layer : nullptr;
    if (Ok(xrEndFrame(X.session, &end), "xrEndFrame") && with_picture && !X.shown) {
        X.shown = true;
        LOG_INFO(Render_Vulkan, "OoT VR: the first picture went to the headset ({}x{}, {})", X.width,
                 X.height, in_world ? "the world around the head" : "a screen");
    }
}

} // Anonymous namespace

void AddInstanceExtensions(std::vector<const char*>& extensions,
                           const std::vector<VkExtensionProperties>& available) {
    if (!InstanceUp()) {
        return;
    }
    u32 size = 0;
    X.get_instance_extensions(X.instance, X.system, 0, &size, nullptr);
    X.instance_extensions.assign(size, '\0');
    if (!Ok(X.get_instance_extensions(X.instance, X.system, size, &size, X.instance_extensions.data()),
            "xrGetVulkanInstanceExtensionsKHR")) {
        return;
    }
    X.instance_extensions.resize(std::strlen(X.instance_extensions.c_str()));
    LOG_INFO(Render_Vulkan, "OoT VR: instance extensions for the headset: {}", X.instance_extensions);
    static std::vector<std::string> names;
    names = Split(X.instance_extensions);
    for (const std::string& name : names) {
        const bool offered = std::ranges::any_of(
            available, [&](const VkExtensionProperties& p) { return name == p.extensionName; });
        const bool listed =
            std::ranges::any_of(extensions, [&](const char* e) { return name == e; });
        if (offered && !listed) {
            extensions.push_back(name.c_str());
        } else if (!offered) {
            LOG_WARNING(Render_Vulkan, "OoT VR: the Vulkan instance has no {}", name);
        }
    }
}

void AddDeviceExtensions(std::set<std::string, std::less<>>& extensions,
                         const std::set<std::string, std::less<>>& supported) {
    if (!InstanceUp()) {
        return;
    }
    u32 size = 0;
    X.get_device_extensions(X.instance, X.system, 0, &size, nullptr);
    X.device_extensions.assign(size, '\0');
    if (!Ok(X.get_device_extensions(X.instance, X.system, size, &size, X.device_extensions.data()),
            "xrGetVulkanDeviceExtensionsKHR")) {
        return;
    }
    X.device_extensions.resize(std::strlen(X.device_extensions.c_str()));
    LOG_INFO(Render_Vulkan, "OoT VR: device extensions for the headset: {}", X.device_extensions);
    for (const std::string& name : Split(X.device_extensions)) {
        if (supported.contains(name)) {
            extensions.insert(name);
        } else {
            LOG_WARNING(Render_Vulkan, "OoT VR: the Vulkan device has no {}", name);
        }
    }
}

VkImage BeginFrame(const Device& device, std::mutex& queue_lock, u32 width, u32 height,
                   VkFormat format, bool& copy) {
    if (X.state <= 0) {
        return VK_NULL_HANDLE;
    }
    if (X.in_frame) {
        EndFrame(queue_lock, false);
    }
    if (!X.session && !CreateSession(device)) {
        Off();
        return VK_NULL_HANDLE;
    }
    PollEvents();
    if (!X.running) {
        return VK_NULL_HANDLE;
    }
    static const u64 lag = [] {
        const char* const v = std::getenv("EDEN_NX2_OOT_VR_LAG");
        return v && *v ? std::strtoull(v, nullptr, 10) % Common::OotVr::AppliedRing : DefaultLag;
    }();
    X.has_sent = Common::OotVr::AppliedPose(lag, X.sent);
    X.frame_state = {XR_TYPE_FRAME_STATE};
    if (!Ok(xrWaitFrame(X.session, nullptr, &X.frame_state), "xrWaitFrame")) {
        return VK_NULL_HANDLE;
    }
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    constexpr XrSpaceLocationFlags both =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if (XR_SUCCEEDED(xrLocateSpace(X.head, X.space, X.frame_state.predictedDisplayTime, &location)) &&
        (location.locationFlags & both) == both) {
        static XrVector3f origin;
        static bool has_origin = false;
        const XrPosef& p = location.pose;
        if (!has_origin) {
            origin = p.position, has_origin = true;
            LOG_INFO(Render_Vulkan, "OoT VR: head zero at {:.2f} {:.2f} {:.2f} m", origin.x,
                     origin.y, origin.z);
        }
        Common::OotVr::HeadPose pose;
        pose.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
        pose.position = {p.position.x - origin.x, p.position.y - origin.y, p.position.z - origin.z};
        Common::OotVr::SetHead(pose);
    }
    XrViewLocateInfo view_info{XR_TYPE_VIEW_LOCATE_INFO};
    view_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    view_info.displayTime = X.frame_state.predictedDisplayTime;
    view_info.space = X.head;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    u32 view_count = 0;
    if (XR_SUCCEEDED(xrLocateViews(X.session, &view_info, &view_state, 2, &view_count, views.data())) &&
        view_count == 2 && (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
        X.eye = {views[0].pose.position, views[1].pose.position};
        X.eye_fov = {views[0].fov, views[1].fov};
        float tan_x = 0, tan_y = 0;
        for (const XrView& v : views) {
            tan_x = std::max({tan_x, std::abs(std::tan(v.fov.angleLeft)), std::abs(std::tan(v.fov.angleRight))});
            tan_y = std::max({tan_y, std::abs(std::tan(v.fov.angleUp)), std::abs(std::tan(v.fov.angleDown))});
        }
        Common::OotVr::SetHeadsetTan(tan_x, tan_y);
        if (!X.has_eyes) {
            X.has_eyes = true;
            LOG_INFO(Render_Vulkan, "OoT VR: left eye view {:.1f} to {:.1f} degrees across",
                     views[0].fov.angleLeft / 0.0174533f, views[0].fov.angleRight / 0.0174533f);
        }
    }
    std::unique_lock lock{queue_lock};
    if (!Ok(xrBeginFrame(X.session, nullptr), "xrBeginFrame")) {
        return VK_NULL_HANDLE;
    }
    if (!X.frame_state.shouldRender) {
        SubmitFrame(false);
        return VK_NULL_HANDLE;
    }
    const bool fits = X.swapchain && X.width == width && X.height == height &&
                      X.source_format == format;
    if (!(fits || MakeSwapchain(format, width, height)) ||
        !Ok(xrAcquireSwapchainImage(X.swapchain, nullptr, &X.index), "xrAcquireSwapchainImage")) {
        SubmitFrame(false);
        return VK_NULL_HANDLE;
    }
    lock.unlock();
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (!Ok(xrWaitSwapchainImage(X.swapchain, &wait), "xrWaitSwapchainImage")) {
        lock.lock();
        SubmitFrame(false);
        return VK_NULL_HANDLE;
    }
    X.in_frame = true;
    copy = X.copy;
    return X.images[X.index].image;
}

void EndFrame(std::mutex& queue_lock, bool drawn) {
    if (!X.in_frame) {
        return;
    }
    X.in_frame = false;
    std::scoped_lock lock{queue_lock};
    const bool released = Ok(xrReleaseSwapchainImage(X.swapchain, nullptr), "xrReleaseSwapchainImage");
    SubmitFrame(released && drawn);
}

} // namespace Vulkan::OotVr
