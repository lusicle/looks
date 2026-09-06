#pragma once

#include <memory>
#include <vector>

#include "gfx/vk_api.h"

VK_DEFINE_HANDLE(VmaAllocation)

namespace looks::gfx {

class Device;

class GpuImage {
public:
    static std::unique_ptr<GpuImage> create(Device& device, VkFormat format,
                                            uint32_t width, uint32_t height,
                                            VkImageUsageFlags usage);
    ~GpuImage();

    GpuImage(const GpuImage&) = delete;
    GpuImage& operator=(const GpuImage&) = delete;

    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    void transition(VkCommandBuffer cmd, VkImageLayout new_layout);
    VkImageLayout layout() const { return layout_; }

private:
    GpuImage() = default;

    Device* device_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    VkImageView view_ = VK_NULL_HANDLE;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
};

class StagingBuffer {
public:
    explicit StagingBuffer(Device& device) : device_(device) {}
    ~StagingBuffer();

    // Call between the frame slot's fence wait and its submit.
    bool upload_image(VkCommandBuffer cmd, const void* data, size_t size,
                      size_t row_pitch, GpuImage& dst);
    // Run only after the owning slot's fence wait; it frees retired buffers.
    void reset();

private:
    bool ensure(size_t needed);

    Device& device_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    void* mapped_ = nullptr;
    size_t capacity_ = 0;
    size_t offset_ = 0;
    struct Retired {
        VkBuffer buffer;
        VmaAllocation allocation;
    };
    std::vector<Retired> retired_;
};

// release_all must run after the owning slot's fence wait.
class TargetPool {
public:
    explicit TargetPool(Device& device) : device_(device) {}

    GpuImage* acquire(uint32_t width, uint32_t height,
                      VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT);
    void release(GpuImage* image);
    void release_all();

private:
    static constexpr uint64_t kRetireFrames = 60;

    struct Entry {
        std::unique_ptr<GpuImage> image;
        bool in_use = false;
        uint64_t last_used = 0;
        VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    };
    Device& device_;
    std::vector<Entry> entries_;
    uint64_t gen_ = 0;
};

inline void clear_color(VkCommandBuffer cmd, GpuImage& img,
                        VkImageLayout layout, const VkClearColorValue& value) {
    img.transition(cmd, layout);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, img.image(), layout, &value, 1, &range);
}

inline void copy_full(VkCommandBuffer cmd, GpuImage& src, GpuImage& dst,
                      uint32_t w, uint32_t h) {
    src.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    dst.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {w, h, 1};
    vkCmdCopyImage(cmd, src.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

inline void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src_stage,
                           VkPipelineStageFlags dst_stage,
                           VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = src_access;
    mb.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
}

bool create_mapped_buffer(Device& device, VkDeviceSize bytes,
                          VkBufferUsageFlags usage, VkBuffer* buffer,
                          VmaAllocation* allocation, void** mapped);

void submit_and_wait(Device& device, VkQueue queue, VkCommandBuffer cmd,
                     VkFence fence, const char* label);

inline void copy_nv12_to_buffer(VkCommandBuffer cmd, GpuImage& y, GpuImage& uv,
                                VkBuffer buffer, uint32_t w, uint32_t h) {
    y.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    uv.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copies[2]{};
    copies[0].bufferOffset = 0;
    copies[0].bufferRowLength = w;
    copies[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copies[0].imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, y.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buffer, 1, &copies[0]);
    copies[1].bufferOffset = static_cast<VkDeviceSize>(w) * h;
    copies[1].bufferRowLength = w / 2;
    copies[1].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copies[1].imageExtent = {w / 2, h / 2, 1};
    vkCmdCopyImageToBuffer(cmd, uv.image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                           &copies[1]);
}

}  // namespace looks::gfx
