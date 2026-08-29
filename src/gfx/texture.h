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
    VkFormat format() const { return format_; }
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
    VkFormat format_ = VK_FORMAT_UNDEFINED;
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

    GpuImage* acquire(uint32_t width, uint32_t height);
    void release(GpuImage* image);
    void release_all();
    // Caller must make sure the GPU is idle.
    void clear();

private:
    static constexpr uint64_t kRetireFrames = 60;

    struct Entry {
        std::unique_ptr<GpuImage> image;
        bool in_use = false;
        uint64_t last_used = 0;
    };
    Device& device_;
    std::vector<Entry> entries_;
    uint64_t gen_ = 0;
};

}  // namespace looks::gfx
