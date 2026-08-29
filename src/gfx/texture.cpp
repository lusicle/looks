#include "gfx/texture.h"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstring>

#include "gfx/vk_device.h"
#include "util/log.h"

namespace looks::gfx {

std::unique_ptr<GpuImage> GpuImage::create(Device& device, VkFormat format,
                                           uint32_t width, uint32_t height,
                                           VkImageUsageFlags usage) {
    auto img = std::unique_ptr<GpuImage>(new GpuImage());
    img->device_ = &device;
    img->format_ = format;
    img->width_ = width;
    img->height_ = height;

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(device.allocator(), &info, &alloc_info, &img->image_,
                       &img->allocation_, nullptr) != VK_SUCCESS) {
        log_error("gfx: image create failed (%ux%u fmt %d)", width, height,
                  static_cast<int>(format));
        return nullptr;
    }

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = img->image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device.device(), &view_info, nullptr, &img->view_) !=
        VK_SUCCESS) {
        vmaDestroyImage(device.allocator(), img->image_, img->allocation_);
        return nullptr;
    }
    return img;
}

GpuImage::~GpuImage() {
    if (!device_) return;
    if (view_) vkDestroyImageView(device_->device(), view_, nullptr);
    if (image_) vmaDestroyImage(device_->allocator(), image_, allocation_);
}

void GpuImage::transition(VkCommandBuffer cmd, VkImageLayout new_layout) {
    if (layout_ == new_layout) return;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = layout_;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    layout_ = new_layout;
}

StagingBuffer::~StagingBuffer() {
    reset();
    if (buffer_)
        vmaDestroyBuffer(device_.allocator(), buffer_, allocation_);
}

void StagingBuffer::reset() {
    for (const Retired& r : retired_)
        vmaDestroyBuffer(device_.allocator(), r.buffer, r.allocation);
    retired_.clear();
    offset_ = 0;
}

bool StagingBuffer::ensure(size_t needed) {
    if (capacity_ >= needed) return true;
    // Recorded copies use the old buffer; it must live until the slot fence.
    if (buffer_) retired_.push_back({buffer_, allocation_});
    size_t capacity = 4u << 20;
    while (capacity < needed) capacity *= 2;

    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = capacity;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo mapped{};
    if (vmaCreateBuffer(device_.allocator(), &info, &alloc_info, &buffer_,
                        &allocation_, &mapped) != VK_SUCCESS) {
        log_error("gfx: staging buffer alloc failed (%zu bytes)", capacity);
        buffer_ = VK_NULL_HANDLE;
        capacity_ = 0;
        return false;
    }
    mapped_ = mapped.pMappedData;
    capacity_ = capacity;
    return true;
}

bool StagingBuffer::upload_image(VkCommandBuffer cmd, const void* data,
                                 size_t size, size_t row_pitch, GpuImage& dst) {
    const size_t aligned = (offset_ + 15) & ~size_t{15};
    if (!ensure(aligned + size)) return false;
    std::memcpy(static_cast<uint8_t*>(mapped_) + aligned, data, size);
    vmaFlushAllocation(device_.allocator(), allocation_, aligned, size);

    dst.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy copy{};
    copy.bufferOffset = aligned;
    copy.bufferRowLength = static_cast<uint32_t>(row_pitch);   // texels
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {dst.width(), dst.height(), 1};
    vkCmdCopyBufferToImage(cmd, buffer_, dst.image(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    offset_ = aligned + size;
    return true;
}

GpuImage* TargetPool::acquire(uint32_t width, uint32_t height) {
    for (Entry& e : entries_) {
        if (!e.in_use && e.image->width() == width &&
            e.image->height() == height) {
            e.in_use = true;
            e.last_used = gen_;
            return e.image.get();
        }
    }
    // The render cache uploads into pooled targets; keep TRANSFER_DST.
    auto image = GpuImage::create(
        device_, VK_FORMAT_R16G16B16A16_SFLOAT, width, height,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!image) return nullptr;
    entries_.push_back({std::move(image), true, gen_});
    return entries_.back().image.get();
}

void TargetPool::release(GpuImage* image) {
    for (Entry& e : entries_)
        if (e.image.get() == image) e.in_use = false;
}

void TargetPool::release_all() {
    ++gen_;
    for (Entry& e : entries_) e.in_use = false;
    // The caller's slot fence wait proves free entries are GPU idle here.
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [&](const Entry& e) {
                           return gen_ - e.last_used > kRetireFrames;
                       }),
        entries_.end());
}

void TargetPool::clear() {
    entries_.clear();
}

}  // namespace looks::gfx
