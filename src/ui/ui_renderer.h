// UiRenderer — the single UI TU that touches Vulkan (mirrors the reference
// toolkit's renderer_frame). Turns Canvas2D batches into draw calls inside
// the caller's already-begun dynamic-rendering pass.
//
// Geometry streams through per-frame-in-flight host-visible buffers that
// grow on demand (safe to recreate at record time: the frame slot's fence
// was waited before begin_frame). Textures (font atlases, later images) are
// long-lived; each gets a descriptor set at registration — no per-frame
// descriptor churn.

#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "gfx/renderer.h"
#include "gfx/vk_api.h"
#include "ui/canvas2d.h"
#include "ui/font.h"

VK_DEFINE_HANDLE(VmaAllocation)

namespace looks::ui {

struct UiTexture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    // MSDF font atlases carry their decode parameters (multiple fonts =
    // multiple atlases with different dimensions/px ranges).
    bool msdf = false;
    // Plain RGBA image (thumbnail strips): sampled and tinted as-is.
    bool rgba_image = false;
    float unit_range[2] = {0.0f, 0.0f};   // px_range / atlas size
};

class UiRenderer {
public:
    // color_format: the render target the UI pipelines will draw into
    // (the swapchain). shader_dir: directory holding the compiled .spv.
    static std::unique_ptr<UiRenderer> create(gfx::Device& device,
                                              VkFormat color_format,
                                              const std::filesystem::path& shader_dir);
    ~UiRenderer();

    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    // Uploads the font atlas (A8, nearest-sampled) and binds it to the font.
    // Returns null on failure.
    const UiTexture* register_font(Font& font);

    // Uploads a plain RGBA8 image (sRGB-encoded bytes, linear-sampled) for
    // Canvas2D::draw_image_quad — thumbnail strips etc. Long-lived; there
    // is no unregister (textures die with the renderer).
    const UiTexture* register_image(const uint8_t* rgba, uint32_t width,
                                    uint32_t height);

    // Records the canvas into `cmd` (must be inside a rendering pass whose
    // color attachment matches color_format). frame_index selects the
    // geometry buffer slot; extent is the framebuffer size in physical px.
    void record(VkCommandBuffer cmd, uint32_t frame_index, VkExtent2D extent,
                const Canvas2D& canvas);

private:
    explicit UiRenderer(gfx::Device& device) : device_(device) {}

    bool init(VkFormat color_format, const std::filesystem::path& shader_dir);
    VkShaderModule load_shader(const std::filesystem::path& path);
    VkPipeline build_pipeline(VkShaderModule vs, VkShaderModule fs,
                              VkPipelineLayout layout, VkFormat color_format,
                              uint32_t attribute_count);

    struct GeometryBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        void* mapped = nullptr;
        VkDeviceSize capacity = 0;
    };
    bool ensure_capacity(GeometryBuffer& buf, VkDeviceSize needed,
                         VkBufferUsageFlags usage);
    void destroy_buffer(GeometryBuffer& buf);

    gfx::Device& device_;
    VkDescriptorSetLayout texture_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkSampler nearest_sampler_ = VK_NULL_HANDLE;
    VkSampler linear_sampler_ = VK_NULL_HANDLE;   // MSDF font atlases
    VkPipelineLayout solid_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout textured_layout_ = VK_NULL_HANDLE;
    VkPipeline solid_pipeline_ = VK_NULL_HANDLE;
    VkPipeline text_pipeline_ = VK_NULL_HANDLE;

    struct FrameGeometry {
        GeometryBuffer vertices;
        GeometryBuffer indices;
    };
    FrameGeometry frames_[gfx::kFramesInFlight];

    std::vector<std::unique_ptr<UiTexture>> textures_;
};

}  // namespace looks::ui
