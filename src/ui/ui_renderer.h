// Geometry buffers can grow at record time: the frame fence already waited.

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
    bool msdf = false;
    bool rgba_image = false;
    // External view: the renderer owns only the descriptor set.
    bool external = false;
    float unit_range[2] = {0.0f, 0.0f};   // px_range / atlas size
};

class UiRenderer {
public:
    static std::unique_ptr<UiRenderer> create(gfx::Device& device,
                                              VkFormat color_format,
                                              const std::filesystem::path& shader_dir);
    ~UiRenderer();

    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    const UiTexture* register_font(Font& font);

    // Textures live until the renderer dies; there is no unregister.
    const UiTexture* register_image(const uint8_t* rgba, uint32_t width,
                                    uint32_t height);

    // The caller keeps the view alive and in SHADER_READ_ONLY layout.
    const UiTexture* register_external(VkImageView view, uint32_t width,
                                       uint32_t height);

    // Call inside a rendering pass whose color format matches color_format.
    // extent is the framebuffer size in physical px.
    void record(VkCommandBuffer cmd, uint32_t frame_index, VkExtent2D extent,
                const Canvas2D& canvas);

private:
    explicit UiRenderer(gfx::Device& device) : device_(device) {}

    bool init(VkFormat color_format, const std::filesystem::path& shader_dir);

    struct GeometryBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        void* mapped = nullptr;
        VkDeviceSize capacity = 0;
    };
    bool ensure_capacity(GeometryBuffer& buf, VkDeviceSize needed,
                         VkBufferUsageFlags usage);
    void destroy_buffer(GeometryBuffer& buf);

    // image_fail is a log_error format that takes width and height.
    struct UploadLabels {
        const char* image_fail;
        const char* pool;
        const char* cmd;
        const char* submit;
        const char* view;
    };
    // Leaves the image in SHADER_READ_ONLY; on failure the caller drops tex.
    bool upload_texture(UiTexture& tex, VkFormat format, const uint8_t* pixels,
                        size_t size, const UploadLabels& labels);
    UiTexture* commit_texture(std::unique_ptr<UiTexture> tex, VkSampler sampler,
                              const char* set_label);

    gfx::Device& device_;
    VkDescriptorSetLayout texture_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkSampler nearest_sampler_ = VK_NULL_HANDLE;
    VkSampler linear_sampler_ = VK_NULL_HANDLE;
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
