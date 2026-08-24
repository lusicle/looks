#include "gfx/vk_swapchain.h"

#include <algorithm>

#include "gfx/vk_device.h"
#include "util/log.h"

namespace looks::gfx {

Swapchain::Swapchain(Device& device, uint32_t width, uint32_t height)
    : device_(device) {
    create(width, height);
}

Swapchain::~Swapchain() {
    destroy_views();
    if (swapchain_)
        vkDestroySwapchainKHR(device_.device(), swapchain_, nullptr);
}

void Swapchain::destroy_views() {
    for (VkImageView v : views_)
        vkDestroyImageView(device_.device(), v, nullptr);
    views_.clear();
    images_.clear();
}

void Swapchain::recreate(uint32_t width, uint32_t height) {
    if (swapchain_ && extent_.width == width && extent_.height == height) return;
    device_.wait_idle();
    destroy_views();
    create(width, height);
}

void Swapchain::create(uint32_t width, uint32_t height) {
    VkSurfaceKHR surface = device_.surface();
    VkPhysicalDevice pd = device_.physical();

    VkSurfaceCapabilitiesKHR caps{};
    vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps),
             "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &format_count, formats.data());

    VkSurfaceFormatKHR chosen = formats.empty()
        ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
        : formats[0];
    for (const auto& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    format_ = chosen.format;
    color_space_ = chosen.colorSpace;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(width, caps.minImageExtent.width,
                                  caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
    }
    extent_ = extent;

    uint32_t min_images = caps.minImageCount + 1;
    if (caps.maxImageCount > 0)
        min_images = std::min(min_images, caps.maxImageCount);

    VkSwapchainKHR old = swapchain_;

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = min_images;
    info.imageFormat = format_;
    info.imageColorSpace = color_space_;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    // TRANSFER_DST so the viewport blit can copy the rendered frame straight
    // onto the swapchain before the UI pass composites over it; TRANSFER_SRC
    // so the script screenshot op can read the presented frame back.
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;   // vsync; universally supported
    info.clipped = VK_TRUE;
    info.oldSwapchain = old;

    vk_check(vkCreateSwapchainKHR(device_.device(), &info, nullptr, &swapchain_),
             "vkCreateSwapchainKHR");
    if (old) vkDestroySwapchainKHR(device_.device(), old, nullptr);

    uint32_t image_count = 0;
    vkGetSwapchainImagesKHR(device_.device(), swapchain_, &image_count, nullptr);
    images_.resize(image_count);
    vkGetSwapchainImagesKHR(device_.device(), swapchain_, &image_count, images_.data());

    views_.resize(image_count);
    for (uint32_t i = 0; i < image_count; ++i) {
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = images_[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format_;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk_check(vkCreateImageView(device_.device(), &view_info, nullptr, &views_[i]),
                 "vkCreateImageView(swapchain)");
    }
}

VkResult Swapchain::acquire(VkSemaphore signal, uint32_t* image_index) {
    return vkAcquireNextImageKHR(device_.device(), swapchain_, UINT64_MAX,
                                 signal, VK_NULL_HANDLE, image_index);
}

VkResult Swapchain::present(VkQueue queue, VkSemaphore wait, uint32_t image_index) {
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &wait;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain_;
    info.pImageIndices = &image_index;
    return vkQueuePresentKHR(queue, &info);
}

}  // namespace looks::gfx
