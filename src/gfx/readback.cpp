#include "gfx/readback.h"

#include <vk_mem_alloc.h>

#include <cstring>

#include "gfx/vk_device.h"
#include "util/log.h"

namespace looks::gfx {

std::unique_ptr<Nv12Readback> Nv12Readback::create(
    Device& device, const std::filesystem::path& shader_dir) {
    auto r = std::unique_ptr<Nv12Readback>(new Nv12Readback(device));
    if (!r->init(shader_dir)) return nullptr;
    return r;
}

Nv12Readback::~Nv12Readback() {
    device_.wait_idle();
    VkDevice dev = device_.device();
    if (fence_) vkDestroyFence(dev, fence_, nullptr);
    if (pool_) vkDestroyCommandPool(dev, pool_, nullptr);
    if (readback_)
        vmaDestroyBuffer(device_.allocator(), readback_, readback_alloc_);
}

bool Nv12Readback::init(const std::filesystem::path& shader_dir) {
    ComputePipelineDesc desc;
    desc.spv_name = "export_nv12.comp.spv";
    desc.sampled_inputs = 1;
    desc.storage_outputs = 2;
    desc.push_bytes = 2 * sizeof(uint32_t);
    to_nv12_ = ComputePipeline::create(device_, shader_dir, desc);
    if (!to_nv12_) return false;

    VkDevice dev = device_.device();
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = device_.graphics_family();
    vk_check(vkCreateCommandPool(dev, &pool_info, nullptr, &pool_),
             "vkCreateCommandPool(readback)");
    VkCommandBufferAllocateInfo cb_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb_info.commandPool = pool_;
    cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_info.commandBufferCount = 1;
    vk_check(vkAllocateCommandBuffers(dev, &cb_info, &cmd_),
             "vkAllocateCommandBuffers(readback)");
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vk_check(vkCreateFence(dev, &fence_info, nullptr, &fence_),
             "vkCreateFence(readback)");
    return true;
}

bool Nv12Readback::ensure_targets(uint32_t width, uint32_t height) {
    if (y_image_ && y_image_->width() == width && y_image_->height() == height)
        return true;
    device_.wait_idle();
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    y_image_ = GpuImage::create(device_, VK_FORMAT_R8_UNORM, width, height, usage);
    uv_image_ = GpuImage::create(device_, VK_FORMAT_R8G8_UNORM, width / 2,
                                 height / 2, usage);
    if (!y_image_ || !uv_image_) return false;

    const size_t needed = static_cast<size_t>(width) * height * 3 / 2;
    if (capacity_ < needed) {
        if (readback_)
            vmaDestroyBuffer(device_.allocator(), readback_, readback_alloc_);
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = needed;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                           VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mapped{};
        if (vmaCreateBuffer(device_.allocator(), &info, &alloc_info, &readback_,
                            &readback_alloc_, &mapped) != VK_SUCCESS) {
            log_error("gfx: readback buffer alloc failed (%zu bytes)", needed);
            readback_ = VK_NULL_HANDLE;
            capacity_ = 0;
            return false;
        }
        mapped_ = mapped.pMappedData;
        capacity_ = needed;
    }
    return true;
}

bool Nv12Readback::render(Engine& engine, const SourcePlanes& source,
                          const doc::Document& doc, uint32_t timeline_frame,
                          double fps, std::vector<uint8_t>& out,
                          uint64_t cache_ctx,
                          const Engine::LayerSourceFrame* layer_sources,
                          size_t layer_source_count) {
    // Output dims follow the engine's divisor (export scale) with the
    // exact working-target math from Engine::render, so the NV12 planes
    // match the composite instead of resampling it back up.
    const uint32_t div = engine.preview_divisor();
    const uint32_t w = std::max((source.width / div) & ~1u, 2u);
    const uint32_t h = std::max((source.height / div) & ~1u, 2u);
    if (source.width == 0 || source.height == 0 || (source.width & 1) ||
        (source.height & 1)) {
        log_error("readback: dimensions must be even (%ux%u)", source.width,
                  source.height);
        return false;
    }
    if (!ensure_targets(w, h)) return false;

    VkDevice dev = device_.device();
    vk_check(vkResetCommandPool(dev, pool_, 0), "vkResetCommandPool(readback)");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd_, &begin), "vkBeginCommandBuffer(readback)");

    GpuImage* final_image =
        engine.render(cmd_, 0, source, doc, timeline_frame, fps, cache_ctx,
                      nullptr, layer_sources, layer_source_count);
    if (!final_image) {
        vkEndCommandBuffer(cmd_);
        return false;
    }

    y_image_->transition(cmd_, VK_IMAGE_LAYOUT_GENERAL);
    uv_image_->transition(cmd_, VK_IMAGE_LAYOUT_GENERAL);
    const uint32_t push[2] = {w, h};
    const GpuImage* sampled[1] = {final_image};
    GpuImage* storage[2] = {y_image_.get(), uv_image_.get()};
    to_nv12_->dispatch(cmd_, engine.arena(), 0, sampled, 1, storage, 2, push,
                       sizeof(push), w, h, engine.linear_sampler());

    y_image_->transition(cmd_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    uv_image_->transition(cmd_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy copies[2]{};
    copies[0].bufferOffset = 0;
    copies[0].bufferRowLength = w;
    copies[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copies[0].imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd_, y_image_->image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1,
                           &copies[0]);
    copies[1].bufferOffset = static_cast<VkDeviceSize>(w) * h;
    copies[1].bufferRowLength = w / 2;   // texels; R8G8 = 2 bytes each
    copies[1].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copies[1].imageExtent = {w / 2, h / 2, 1};
    vkCmdCopyImageToBuffer(cmd_, uv_image_->image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1,
                           &copies[1]);

    VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, nullptr,
                         0, nullptr);

    vk_check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer(readback)");

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_;
    {
        std::lock_guard<std::mutex> lock(device_.queue_mutex());
        vk_check(vkQueueSubmit(device_.graphics_queue(), 1, &submit, fence_),
                 "vkQueueSubmit(readback)");
    }
    vk_check(vkWaitForFences(dev, 1, &fence_, VK_TRUE, UINT64_MAX),
             "vkWaitForFences(readback)");
    vk_check(vkResetFences(dev, 1, &fence_), "vkResetFences(readback)");

    const size_t bytes = static_cast<size_t>(w) * h * 3 / 2;
    vmaInvalidateAllocation(device_.allocator(), readback_alloc_, 0, bytes);
    out.resize(bytes);
    std::memcpy(out.data(), mapped_, bytes);
    return true;
}

}  // namespace looks::gfx
