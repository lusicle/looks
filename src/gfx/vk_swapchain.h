// Swapchain wrapper: sRGB target, FIFO present, resize-driven recreation.
//
// The swapchain format is *_SRGB so UI/blit writes blend in linear and store
// encoded — matching the reference toolkit's pipeline expectations. The
// engine's working space stays linear RGBA16F offscreen; only the
// viewport blit and UI touch this surface.

#pragma once

#include <vector>

#include "gfx/vk_api.h"

namespace looks::gfx {

class Device;

class Swapchain {
public:
    Swapchain(Device& device, uint32_t width, uint32_t height);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Rebuilds at the surface's current size. Safe to call every resize
    // event; no-ops when the size is unchanged and valid.
    void recreate(uint32_t width, uint32_t height);

    VkFormat format() const { return format_; }
    VkExtent2D extent() const { return extent_; }
    uint32_t image_count() const { return static_cast<uint32_t>(images_.size()); }
    VkImage image(uint32_t i) const { return images_[i]; }
    VkImageView view(uint32_t i) const { return views_[i]; }
    VkSwapchainKHR handle() const { return swapchain_; }

    // VK_ERROR_OUT_OF_DATE_KHR is returned (not fatal) so the frame loop can
    // recreate and retry.
    VkResult acquire(VkSemaphore signal, uint32_t* image_index);
    VkResult present(VkQueue queue, VkSemaphore wait, uint32_t image_index);

private:
    void create(uint32_t width, uint32_t height);
    void destroy_views();

    Device& device_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
};

}  // namespace looks::gfx
