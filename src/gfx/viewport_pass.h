// Viewport blit: samples the engine's final linear RGBA16F target
// and draws it letterboxed into a screen rect inside the active present
// pass. The sRGB swapchain performs the OETF encode on write — this is the
// only place preview pixels leave the linear working space.

#pragma once

#include <filesystem>
#include <memory>

#include "gfx/compute.h"   // DescriptorArena
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

    // Records inside an active dynamic-rendering pass. Fits `image`
    // (SHADER_READ_ONLY layout) into the dst rect (physical px), aspect
    // preserved and centered; restores the full-extent scissor afterwards.
    // clip_x0/clip_x1 (fractions of the fitted rect) confine the draw
    // horizontally — the A/B before-after wipe is two draws with
    // complementary clips.
    void draw(VkCommandBuffer cmd, DescriptorArena& arena, uint32_t frame_index,
              GpuImage& image, VkSampler sampler, VkExtent2D extent,
              float dst_x, float dst_y, float dst_w, float dst_h,
              float clip_x0 = 0.0f, float clip_x1 = 1.0f);

private:
    explicit ViewportPass(Device& device) : device_(device) {}
    bool init(VkFormat color_format, const std::filesystem::path& shader_dir);

    Device& device_;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace looks::gfx
