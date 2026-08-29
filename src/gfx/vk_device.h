#pragma once

#include <memory>
#include <mutex>

#include "gfx/vk_api.h"

// Do not include vk_mem_alloc.h here; only the impl TUs see it.
VK_DEFINE_HANDLE(VmaAllocator)

namespace looks::gfx {

struct DeviceDesc {
    void* hwnd = nullptr;        // HWND; null = headless (no surface/swapchain)
    void* hinstance = nullptr;   // HINSTANCE
    bool enable_validation = false;
};

class Device {
public:
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
    // Same as graphics_queue() when the family has only one queue.
    // Cross-queue handoff: host fence wait plus the consumer's own barriers.
    VkQueue thumb_queue() const { return thumb_queue_; }

    // Take this lock for every submit, present, and wait-idle, on all queues.
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
