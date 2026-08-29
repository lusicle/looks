// Pixels stay linear; the sRGB swapchain encodes on write.

#pragma once

#include <filesystem>
#include <memory>

#include "gfx/compute.h"
#include "gfx/texture.h"
#include "gfx/vk_api.h"

namespace looks::gfx {

class ViewportPass {
public:
    static std::unique_ptr<ViewportPass> create(
        Device& device, VkFormat color_format,
        const std::filesystem::path& shader_dir);
    ~ViewportPass();

    ViewportPass(const ViewportPass&) = delete;
    ViewportPass& operator=(const ViewportPass&) = delete;

    // image must be in SHADER_READ_ONLY. dst_* and bound_* are physical px.
    // clip_x0/x1 are fractions of the fitted rect; bound_w <= 0 = no bound.
    void draw(VkCommandBuffer cmd, DescriptorArena& arena, uint32_t frame_index,
              GpuImage& image, VkSampler sampler, VkExtent2D extent,
              float dst_x, float dst_y, float dst_w, float dst_h,
              float clip_x0 = 0.0f, float clip_x1 = 1.0f,
              uint32_t alpha_mode = 0, float bound_x = 0.0f,
              float bound_y = 0.0f, float bound_w = -1.0f,
              float bound_h = -1.0f);

private:
    explicit ViewportPass(Device& device) : device_(device) {}
    bool init(VkFormat color_format, const std::filesystem::path& shader_dir);

    Device& device_;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace looks::gfx
