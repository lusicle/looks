#include "gfx/vk_api.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

#include "util/log.h"

#define LOOKS_VK_DEFINE_FN(name) PFN_##name name = nullptr;
LOOKS_VK_GLOBAL_FNS(LOOKS_VK_DEFINE_FN)
LOOKS_VK_INSTANCE_FNS(LOOKS_VK_DEFINE_FN)
LOOKS_VK_INSTANCE_FNS_OPTIONAL(LOOKS_VK_DEFINE_FN)
LOOKS_VK_DEVICE_FNS(LOOKS_VK_DEFINE_FN)
LOOKS_VK_DEVICE_FNS_DYNRENDER(LOOKS_VK_DEFINE_FN)
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#undef LOOKS_VK_DEFINE_FN

namespace looks::gfx {

bool vk_load_loader() {
    static HMODULE module = nullptr;
    if (module) return true;
    module = LoadLibraryW(L"vulkan-1.dll");
    if (!module) {
        log_error("vulkan-1.dll not found — no Vulkan driver installed?");
        return false;
    }
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(module, "vkGetInstanceProcAddr")));
    if (!vkGetInstanceProcAddr) {
        log_error("vkGetInstanceProcAddr missing from vulkan-1.dll");
        return false;
    }
#define LOOKS_VK_LOAD(name) \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));
    LOOKS_VK_GLOBAL_FNS(LOOKS_VK_LOAD)
#undef LOOKS_VK_LOAD
    return vkCreateInstance != nullptr;
}

void vk_load_instance(VkInstance instance) {
#define LOOKS_VK_LOAD(name) \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));
    LOOKS_VK_INSTANCE_FNS(LOOKS_VK_LOAD)
    LOOKS_VK_INSTANCE_FNS_OPTIONAL(LOOKS_VK_LOAD)
#undef LOOKS_VK_LOAD
}

void vk_load_device(VkDevice device) {
#define LOOKS_VK_LOAD(name) \
    name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
    LOOKS_VK_DEVICE_FNS(LOOKS_VK_LOAD)
    LOOKS_VK_DEVICE_FNS_DYNRENDER(LOOKS_VK_LOAD)
#undef LOOKS_VK_LOAD

    // Vulkan 1.2 devices expose dynamic rendering under the KHR suffix.
    if (!vkCmdBeginRendering) {
        vkCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(
            vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR"));
        vkCmdEndRendering = reinterpret_cast<PFN_vkCmdEndRendering>(
            vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR"));
    }
}

const char* vk_result_name(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_VALIDATION_FAILED_EXT: return "VK_ERROR_VALIDATION_FAILED_EXT";
        default: return "VK_ERROR_<unmapped>";
    }
}

void vk_check(VkResult result, const char* what) {
    if (result == VK_SUCCESS) return;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s failed: %s (%d)", what,
                  vk_result_name(result), static_cast<int>(result));
    log_error("%s", buf);
    std::abort();
}

}  // namespace looks::gfx
