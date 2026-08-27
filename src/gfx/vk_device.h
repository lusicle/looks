// Vulkan instance/device bootstrap.
//
// One graphics+compute queue for all rendering, plus a second
// graphics-family queue for the thumbnail worker when the family has
// one. VMA owns every allocation.

#pragma once

#include <memory>
#include <mutex>

#include "gfx/vk_api.h"

// Forward-declared so only vk_device.cpp/vma_impl.cpp see the VMA header.
VK_DEFINE_HANDLE(VmaAllocator)

namespace looks::gfx {

struct DeviceDesc {
    void* hwnd = nullptr;        // HWND for the presentation surface;
                                 // null = headless (no surface/swapchain —
                                 // export CLIs, determinism harness)
    void* hinstance = nullptr;   // HINSTANCE
    bool enable_validation = false;
};

class Device {
public:
    // Returns null on any failure (missing loader, no compatible GPU) after
    // logging the reason — the app shows a message and exits cleanly.
    static std::unique_ptr<Device> create(const DeviceDesc& desc);
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physical() const { return physical_; }
    VkDevice device() const { return device_; }
    VkSurfaceKHR surface() const { return surface_; }
    VmaAllocator allocator() const { return allocator_; }

    uint32_t graphics_family() const { return graphics_family_; }
    VkQueue graphics_queue() const { return graphics_queue_; }
    // Second graphics-family queue (priority 0.5) for the thumbnail
    // worker's background renders; the graphics queue itself when the
    // family exposes only one. Same family = shared resources, and a
    // host fence wait plus the consumer's own barriers carry work
    // across queues (execution ordered through the host, visibility
    // through the barrier).
    VkQueue thumb_queue() const { return thumb_queue_; }

    // Serializes vkQueueSubmit/vkQueuePresentKHR/vkDeviceWaitIdle
    // across ALL queues - preview loop, export worker, UI present and
    // the thumbnail queue take this one lock for every submit, so no
    // per-queue ownership bookkeeping exists to get wrong.
    std::mutex& queue_mutex() const { return queue_mutex_; }

    void wait_idle() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        vkDeviceWaitIdle(device_);
    }

private:
    Device() = default;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    uint32_t graphics_family_ = 0;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue thumb_queue_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    mutable std::mutex queue_mutex_;
};

}  // namespace looks::gfx
