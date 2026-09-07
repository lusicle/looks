#include "gfx/readback.h"

#include "gfx/graph.h"

#include <vk_mem_alloc.h>

#include <cstring>

#include "gfx/vk_device.h"
#include "util/log.h"
#include "util/color.h"

namespace looks::gfx {

std::unique_ptr<RgbaReadback> RgbaReadback::create(Device& device) {
    auto r = std::unique_ptr<RgbaReadback>(new RgbaReadback(device));
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = device.graphics_family();
    if (vkCreateCommandPool(device.device(), &pi, nullptr, &r->pool_) != VK_SUCCESS) return nullptr;
    VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ci.commandPool = r->pool_;
    ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ci.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device.device(), &ci, &r->cmd_) != VK_SUCCESS) return nullptr;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device.device(), &fi, nullptr, &r->fence_) != VK_SUCCESS) return nullptr;
    return r;
}

RgbaReadback::~RgbaReadback() {
    if (buffer_) vmaDestroyBuffer(device_.allocator(), buffer_, allocation_);
    if (fence_) vkDestroyFence(device_.device(), fence_, nullptr);
    if (pool_) vkDestroyCommandPool(device_.device(), pool_, nullptr);
}

bool RgbaReadback::render(Engine& engine, const doc::Document& doc, uint64_t entity,
                          uint32_t frame, double fps, uint32_t width, uint32_t height,
                          std::vector<uint8_t>& rgba, const Engine::LayerSourceFrame* sources,
                          size_t source_count, bool alpha, bool read_pixels,
                          uint64_t preview_node, uint64_t preview_layer, uint32_t clock_frame) {
    if (!begin(width, height, read_pixels)) return false;
    GpuImage* result = engine.render(cmd_, 0, doc, entity, frame, fps, width, height,
        0, clock_frame == UINT32_MAX ? frame : clock_frame, nullptr, sources, source_count,
        preview_node, preview_layer);
    if (!result || result->width() != width || result->height() != height) {
        vkEndCommandBuffer(cmd_);
        return false;
    }
    return finish(*result, rgba, alpha, read_pixels);
}

bool RgbaReadback::read(GpuImage& image, std::vector<uint8_t>& rgba, bool alpha) {
    return begin(image.width(), image.height(), true) && finish(image, rgba, alpha, true);
}

bool RgbaReadback::begin(uint32_t width, uint32_t height, bool read_pixels) {
    if (!width || !height || uint64_t(width) * height > SIZE_MAX / 8) return false;
    const size_t bytes = size_t(width) * height * 8;
    if (read_pixels && capacity_ < bytes) {
        if (buffer_) vmaDestroyBuffer(device_.allocator(), buffer_, allocation_);
        buffer_ = VK_NULL_HANDLE;
        if (!create_mapped_buffer(device_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   &buffer_, &allocation_, &mapped_)) return false;
        capacity_ = bytes;
    }
    vkResetCommandPool(device_.device(), pool_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(cmd_, &bi) == VK_SUCCESS;
}

bool RgbaReadback::finish(GpuImage& image, std::vector<uint8_t>& rgba, bool alpha, bool read_pixels) {
    auto* result = &image;
    const uint32_t width = image.width(), height = image.height();
    const size_t bytes = size_t(width) * height * 8;
    if (read_pixels) {
        result->transition(cmd_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd_, result->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               buffer_, 1, &copy);
        memory_barrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        result->transition(cmd_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) return false;
    submit_and_wait(device_, device_.graphics_queue(), cmd_, fence_, "RGBA readback");
    if (!read_pixels) return true;
    vmaInvalidateAllocation(device_.allocator(), allocation_, 0, bytes);
    const auto* half = static_cast<const uint16_t*>(mapped_);
    rgba.resize(size_t(width) * height * 4);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        const float a = std::clamp(half_to_float(half[i + 3]), 0.0f, 1.0f);
        for (int c = 0; c < 3; ++c) {
            const float value = half_to_float(half[i + c]);
            const float straight = alpha ? (a > 0 ? value / a : 0) : value;
            rgba[i + c] = uint8_t(std::clamp(color::srgb_oetf(std::max(0.0f, straight)) * 255 + 0.5f, 0.0f, 255.0f));
        }
        rgba[i + 3] = alpha ? uint8_t(a * 255 + 0.5f) : 255;
    }
    return true;
}

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
    desc.push_bytes = 3 * sizeof(uint32_t);
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
        if (!create_mapped_buffer(device_, needed,
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback_,
                                  &readback_alloc_, &mapped_)) {
            log_error("gfx: readback buffer alloc failed (%zu bytes)", needed);
            capacity_ = 0;
            return false;
        }
        capacity_ = needed;
    }
    return true;
}

bool Nv12Readback::render(Engine& engine, const doc::Document& doc,
                          uint64_t look_id, uint32_t timeline_frame,
                          double fps, uint32_t canvas_w, uint32_t canvas_h,
                          std::vector<uint8_t>& out,
                          uint64_t cache_ctx, uint32_t cache_frame,
                          const Engine::LayerSourceFrame* layer_sources,
                          size_t layer_source_count,
                          uint64_t measure_placement) {
    // w and h must match the working-target math in Engine::render.
    const uint32_t div = engine.preview_divisor();
    const uint32_t w = even_down(canvas_w, div);
    const uint32_t h = even_down(canvas_h, div);
    if (canvas_w == 0 || canvas_h == 0 || (canvas_w & 1) || (canvas_h & 1)) {
        log_error("readback: dimensions must be even (%ux%u)", canvas_w,
                  canvas_h);
        return false;
    }
    if (!ensure_targets(w, h)) return false;

    VkDevice dev = device_.device();
    vk_check(vkResetCommandPool(dev, pool_, 0), "vkResetCommandPool(readback)");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd_, &begin), "vkBeginCommandBuffer(readback)");

    GpuImage* final_image =
        engine.render(cmd_, 0, doc, look_id, timeline_frame, fps, canvas_w,
                      canvas_h, cache_ctx, cache_frame, nullptr,
                      layer_sources, layer_source_count, 0, 0,
                      measure_placement);
    if (!final_image) {
        vkEndCommandBuffer(cmd_);
        return false;
    }

    y_image_->transition(cmd_, VK_IMAGE_LAYOUT_GENERAL);
    uv_image_->transition(cmd_, VK_IMAGE_LAYOUT_GENERAL);
    const uint32_t push[3] = {w, h, 0};
    const GpuImage* sampled[1] = {final_image};
    GpuImage* storage[2] = {y_image_.get(), uv_image_.get()};
    to_nv12_->dispatch(cmd_, engine.arena(), 0, sampled, 1, storage, 2, push,
                       sizeof(push), w, h, engine.linear_sampler());

    copy_nv12_to_buffer(cmd_, *y_image_, *uv_image_, readback_, w, h);

    memory_barrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_HOST_READ_BIT);

    vk_check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer(readback)");

    submit_and_wait(device_, device_.graphics_queue(), cmd_, fence_,
                    "readback submit");

    const size_t bytes = static_cast<size_t>(w) * h * 3 / 2;
    vmaInvalidateAllocation(device_.allocator(), readback_alloc_, 0, bytes);
    out.resize(bytes);
    std::memcpy(out.data(), mapped_, bytes);
    return true;
}

}  // namespace looks::gfx
