// GPU image + staging upload helpers. VMA-backed images with
// simple explicit layout tracking; pooled RGBA16F intermediates for the
// render graph. Uploads go through per-frame staging rings on the graphics
// queue for now — the dedicated transfer queue path arrives with async
// double-buffered player uploads.

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

    // Records a layout transition (all-commands scope — correctness first;
    // tighter stages come with the real graph scheduler).
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

// Host-visible staging buffer reused across frames; grows on demand.
class StagingBuffer {
public:
    explicit StagingBuffer(Device& device) : device_(device) {}
    ~StagingBuffer();

    // Copies `data` into the staging area at an internal offset and records
    // a buffer->image copy. Call between begin_frame's fence wait and
    // submit; offsets reset with reset().
    bool upload_image(VkCommandBuffer cmd, const void* data, size_t size,
                      size_t row_pitch, GpuImage& dst);
    // Resets the write offset and destroys buffers retired by growth.
    // MUST run after the owning slot's fence wait: growth mid-frame keeps
    // the old buffer alive (already-recorded copies reference it), and the
    // fence is what proves those copies have executed.
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

// Pooled RGBA16F render targets: acquire per pass, release when the frame's
// evaluation is done. Images are reused by (width, height); the pool never
// shrinks (a handful of 1080p targets is the steady state).
class TargetPool {
public:
    explicit TargetPool(Device& device) : device_(device) {}

    GpuImage* acquire(uint32_t width, uint32_t height);
    void release(GpuImage* image);
    void release_all();
    // Destroys every pooled image (source-size change). Caller must ensure
    // the GPU is idle.
    void clear();

private:
    struct Entry {
        std::unique_ptr<GpuImage> image;
        bool in_use = false;
    };
    Device& device_;
    std::vector<Entry> entries_;
};

}  // namespace looks::gfx
