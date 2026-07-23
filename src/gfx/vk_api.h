// Hand-rolled Vulkan function loader (volk-style, minimal).
//
// We link nothing at build time: the system loader vulkan-1.dll is opened at
// runtime and every entry point we use is resolved through
// vkGetInstanceProcAddr / vkGetDeviceProcAddr. Device-level dispatch skips
// the loader trampoline entirely. Add new functions to the X-macro tables
// below — nowhere else.
//
// VK_NO_PROTOTYPES is forced here so nobody accidentally calls a linked
// prototype; every TU that touches Vulkan includes this header, never
// <vulkan/vulkan.h> directly.

#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#define LOOKS_VK_GLOBAL_FNS(X) \
    X(vkCreateInstance) \
    X(vkEnumerateInstanceVersion) \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties)

#define LOOKS_VK_INSTANCE_FNS(X) \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) \
    X(vkDestroySurfaceKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)

// Optional instance functions: present only when VK_EXT_debug_utils is
// enabled; call sites must null-check.
#define LOOKS_VK_INSTANCE_FNS_OPTIONAL(X) \
    X(vkCreateDebugUtilsMessengerEXT) \
    X(vkDestroyDebugUtilsMessengerEXT)

#define LOOKS_VK_DEVICE_FNS(X) \
    X(vkDestroyDevice) \
    X(vkGetDeviceQueue) \
    X(vkDeviceWaitIdle) \
    X(vkQueueWaitIdle) \
    X(vkQueueSubmit) \
    X(vkQueuePresentKHR) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) \
    X(vkCreateImageView) \
    X(vkDestroyImageView) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkResetCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkFreeCommandBuffers) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkCreateFence) \
    X(vkDestroyFence) \
    X(vkWaitForFences) \
    X(vkResetFences) \
    X(vkCreateSemaphore) \
    X(vkDestroySemaphore) \
    X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) \
    X(vkCreateComputePipelines) \
    X(vkDestroyPipeline) \
    X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) \
    X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) \
    X(vkCreateSampler) \
    X(vkDestroySampler) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdClearColorImage) \
    X(vkCmdBindPipeline) \
    X(vkCmdSetViewport) \
    X(vkCmdSetScissor) \
    X(vkCmdBindVertexBuffers) \
    X(vkCmdBindIndexBuffer) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdPushConstants) \
    X(vkCmdDraw) \
    X(vkCmdDrawIndexed) \
    X(vkCmdDispatch) \
    X(vkCmdCopyBuffer) \
    X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdCopyImage) \
    X(vkCmdBlitImage)

// Dynamic rendering: core in 1.3, VK_KHR_dynamic_rendering on 1.2. The
// loader aliases the KHR entry points onto these names when core is absent.
#define LOOKS_VK_DEVICE_FNS_DYNRENDER(X) \
    X(vkCmdBeginRendering) \
    X(vkCmdEndRendering)

#define LOOKS_VK_DECLARE_FN(name) extern PFN_##name name;
LOOKS_VK_GLOBAL_FNS(LOOKS_VK_DECLARE_FN)
LOOKS_VK_INSTANCE_FNS(LOOKS_VK_DECLARE_FN)
LOOKS_VK_INSTANCE_FNS_OPTIONAL(LOOKS_VK_DECLARE_FN)
LOOKS_VK_DEVICE_FNS(LOOKS_VK_DECLARE_FN)
LOOKS_VK_DEVICE_FNS_DYNRENDER(LOOKS_VK_DECLARE_FN)
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#undef LOOKS_VK_DECLARE_FN

namespace looks::gfx {

// Opens vulkan-1.dll and resolves global-level functions. False if the
// system has no Vulkan loader (no GPU driver).
bool vk_load_loader();

void vk_load_instance(VkInstance instance);
void vk_load_device(VkDevice device);

// Fatal-error check: prints the failing call + VkResult and aborts.
void vk_check(VkResult result, const char* what);

const char* vk_result_name(VkResult result);

}  // namespace looks::gfx
