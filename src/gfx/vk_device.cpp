#define VK_USE_PLATFORM_WIN32_KHR
#include "gfx/vk_device.h"

#include <vk_mem_alloc.h>

#include <cstring>
#include <vector>

#include "util/log.h"

namespace looks::gfx {

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*user*/) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        log_error("vulkan: %s", data->pMessage);
    else
        log_warn("vulkan: %s", data->pMessage);
    return VK_FALSE;
}

bool has_layer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto& l : layers)
        if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}

bool has_instance_extension(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
    for (const auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

bool has_device_extension(VkPhysicalDevice pd, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, exts.data());
    for (const auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

struct QueuePick {
    uint32_t graphics = VK_QUEUE_FAMILY_IGNORED;
    uint32_t transfer = VK_QUEUE_FAMILY_IGNORED;
};

QueuePick pick_queues(VkPhysicalDevice pd, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, families.data());

    QueuePick pick;
    for (uint32_t i = 0; i < count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        const bool graphics_compute =
            (flags & VK_QUEUE_GRAPHICS_BIT) && (flags & VK_QUEUE_COMPUTE_BIT);
        if (graphics_compute && pick.graphics == VK_QUEUE_FAMILY_IGNORED) {
            // Headless (no surface): any graphics+compute family will do.
            VkBool32 present = surface == VK_NULL_HANDLE ? VK_TRUE : VK_FALSE;
            if (surface != VK_NULL_HANDLE)
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present);
            if (present) pick.graphics = i;
        }
        // Dedicated DMA family: transfer without graphics/compute.
        const bool dma = (flags & VK_QUEUE_TRANSFER_BIT) &&
                         !(flags & VK_QUEUE_GRAPHICS_BIT) &&
                         !(flags & VK_QUEUE_COMPUTE_BIT);
        if (dma && pick.transfer == VK_QUEUE_FAMILY_IGNORED) pick.transfer = i;
    }
    if (pick.transfer == VK_QUEUE_FAMILY_IGNORED) pick.transfer = pick.graphics;
    return pick;
}

}  // namespace

std::unique_ptr<Device> Device::create(const DeviceDesc& desc) {
    if (!vk_load_loader()) return nullptr;

    auto dev = std::unique_ptr<Device>(new Device());

    // ---- instance
    uint32_t loader_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion)
        vkEnumerateInstanceVersion(&loader_version);
    if (loader_version < VK_API_VERSION_1_2) {
        log_error("Vulkan loader is %u.%u — need 1.2+",
                  VK_API_VERSION_MAJOR(loader_version),
                  VK_API_VERSION_MINOR(loader_version));
        return nullptr;
    }
    const uint32_t api_version =
        loader_version >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "looks";
    app.pEngineName = "looks";
    app.apiVersion = api_version;

    std::vector<const char*> instance_exts = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    std::vector<const char*> layers;
    bool debug_utils = false;
    if (desc.enable_validation && has_layer(kValidationLayer)) {
        layers.push_back(kValidationLayer);
        if (has_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            instance_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            debug_utils = true;
        }
        log_info("vulkan validation enabled");
    }

    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = static_cast<uint32_t>(instance_exts.size());
    instance_info.ppEnabledExtensionNames = instance_exts.data();
    instance_info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    instance_info.ppEnabledLayerNames = layers.data();

    VkResult r = vkCreateInstance(&instance_info, nullptr, &dev->instance_);
    if (r != VK_SUCCESS) {
        log_error("vkCreateInstance failed: %s", vk_result_name(r));
        return nullptr;
    }
    vk_load_instance(dev->instance_);

    if (debug_utils && vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT dbg{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dbg.pfnUserCallback = debug_callback;
        vkCreateDebugUtilsMessengerEXT(dev->instance_, &dbg, nullptr, &dev->messenger_);
    }

    // ---- surface (needed before device pick: present support is per-family;
    // headless callers pass no hwnd and get no surface/swapchain)
    if (desc.hwnd) {
        auto create_surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
            vkGetInstanceProcAddr(dev->instance_, "vkCreateWin32SurfaceKHR"));
        VkWin32SurfaceCreateInfoKHR surface_info{
            VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surface_info.hinstance = static_cast<HINSTANCE>(desc.hinstance);
        surface_info.hwnd = static_cast<HWND>(desc.hwnd);
        r = create_surface(dev->instance_, &surface_info, nullptr, &dev->surface_);
        if (r != VK_SUCCESS) {
            log_error("vkCreateWin32SurfaceKHR failed: %s", vk_result_name(r));
            return nullptr;
        }
    }

    // ---- physical device: discrete preferred, must do swapchain + dynamic
    // rendering (1.3 core or the KHR extension on 1.2).
    uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(dev->instance_, &pd_count, nullptr);
    std::vector<VkPhysicalDevice> physicals(pd_count);
    vkEnumeratePhysicalDevices(dev->instance_, &pd_count, physicals.data());

    VkPhysicalDevice best = VK_NULL_HANDLE;
    QueuePick best_queues;
    bool best_needs_dynrender_ext = false;
    int best_score = -1;
    for (VkPhysicalDevice pd : physicals) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_2) continue;
        if (!has_device_extension(pd, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

        bool needs_ext = props.apiVersion < VK_API_VERSION_1_3;
        if (needs_ext && !has_device_extension(pd, "VK_KHR_dynamic_rendering"))
            continue;
        if (!needs_ext) {
            VkPhysicalDeviceVulkan13Features f13{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            f2.pNext = &f13;
            vkGetPhysicalDeviceFeatures2(pd, &f2);
            if (!f13.dynamicRendering) {
                if (!has_device_extension(pd, "VK_KHR_dynamic_rendering")) continue;
                needs_ext = true;
            }
        }

        QueuePick queues = pick_queues(pd, dev->surface_);
        if (queues.graphics == VK_QUEUE_FAMILY_IGNORED) continue;

        int score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 100;
        if (queues.transfer != queues.graphics) score += 10;
        if (score > best_score) {
            best_score = score;
            best = pd;
            best_queues = queues;
            best_needs_dynrender_ext = needs_ext;
        }
    }
    if (!best) {
        log_error("no compatible Vulkan device (need 1.2+, swapchain, dynamic rendering)");
        return nullptr;
    }
    dev->physical_ = best;
    dev->graphics_family_ = best_queues.graphics;
    dev->transfer_family_ = best_queues.transfer;
    vkGetPhysicalDeviceProperties(best, &dev->properties_);
    log_info("gpu: %s", dev->properties_.deviceName);

    // ---- logical device
    // A second graphics-family queue (lower priority) carries the
    // thumbnail worker's background renders: the driver schedules them
    // around frame work instead of in line with it. Same family, so
    // every resource shares with no ownership transfers; one-queue
    // families fall back to the shared graphics queue.
    uint32_t gfx_queue_count = 1;
    {
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(best, &n, nullptr);
        std::vector<VkQueueFamilyProperties> fams(n);
        vkGetPhysicalDeviceQueueFamilyProperties(best, &n, fams.data());
        if (dev->graphics_family_ < n &&
            fams[dev->graphics_family_].queueCount >= 2)
            gfx_queue_count = 2;
    }
    float priority = 1.0f;
    const float gfx_priorities[2] = {1.0f, 0.5f};
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    {
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = dev->graphics_family_;
        qi.queueCount = gfx_queue_count;
        qi.pQueuePriorities = gfx_priorities;
        queue_infos.push_back(qi);
        if (dev->transfer_family_ != dev->graphics_family_) {
            qi.queueFamilyIndex = dev->transfer_family_;
            qi.queueCount = 1;
            qi.pQueuePriorities = &priority;
            queue_infos.push_back(qi);
        }
    }

    std::vector<const char*> device_exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (best_needs_dynrender_ext) device_exts.push_back("VK_KHR_dynamic_rendering");

    VkPhysicalDeviceDynamicRenderingFeatures dynrender{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    dynrender.dynamicRendering = VK_TRUE;

    // shaderDrawParameters (1.1 core): Slang lowers HLSL SV_VertexID to
    // gl_VertexIndex - gl_BaseVertex, which declares the DrawParameters
    // capability — required by the viewport blit's fullscreen triangle.
    VkPhysicalDeviceVulkan11Features features11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    features11.pNext = &dynrender;

    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &features11;

    // Enable samplerAnisotropy when available: we may use it for viewport
    // sampling later, and injected overlay layers (Steam/vendor OSDs) create
    // anisotropic samplers on our device — without the feature they trip
    // validation with errors that aren't ours.
    {
        VkPhysicalDeviceVulkan11Features supported11{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        supported.pNext = &supported11;
        vkGetPhysicalDeviceFeatures2(best, &supported);
        features2.features.samplerAnisotropy = supported.features.samplerAnisotropy;
        features11.shaderDrawParameters = supported11.shaderDrawParameters;
        if (!supported11.shaderDrawParameters)
            log_warn("gfx: shaderDrawParameters unsupported — viewport blit "
                     "may fail validation");
    }

    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &features2;
    device_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
    device_info.pQueueCreateInfos = queue_infos.data();
    device_info.enabledExtensionCount = static_cast<uint32_t>(device_exts.size());
    device_info.ppEnabledExtensionNames = device_exts.data();

    r = vkCreateDevice(best, &device_info, nullptr, &dev->device_);
    if (r != VK_SUCCESS) {
        log_error("vkCreateDevice failed: %s", vk_result_name(r));
        return nullptr;
    }
    vk_load_device(dev->device_);
    if (!vkCmdBeginRendering) {
        log_error("dynamic rendering entry points missing after device create");
        return nullptr;
    }

    vkGetDeviceQueue(dev->device_, dev->graphics_family_, 0, &dev->graphics_queue_);
    vkGetDeviceQueue(dev->device_, dev->transfer_family_, 0, &dev->transfer_queue_);
    vkGetDeviceQueue(dev->device_, dev->graphics_family_,
                     gfx_queue_count > 1 ? 1 : 0, &dev->thumb_queue_);

    // ---- VMA
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.physicalDevice = dev->physical_;
    allocator_info.device = dev->device_;
    allocator_info.instance = dev->instance_;
    allocator_info.vulkanApiVersion =
        dev->properties_.apiVersion >= VK_API_VERSION_1_3 ? api_version
                                                          : VK_API_VERSION_1_2;
    allocator_info.pVulkanFunctions = &functions;
    r = vmaCreateAllocator(&allocator_info, &dev->allocator_);
    if (r != VK_SUCCESS) {
        log_error("vmaCreateAllocator failed: %s", vk_result_name(r));
        return nullptr;
    }

    return dev;
}

Device::~Device() {
    if (device_) vkDeviceWaitIdle(device_);
    if (allocator_) vmaDestroyAllocator(allocator_);
    if (device_) vkDestroyDevice(device_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (messenger_ && vkDestroyDebugUtilsMessengerEXT)
        vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

}  // namespace looks::gfx
