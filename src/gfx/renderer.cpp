#include "gfx/renderer.h"

#include <vk_mem_alloc.h>

#include <cstring>

#include "util/log.h"

namespace looks::gfx {

std::unique_ptr<Renderer> Renderer::create(void* hwnd, void* hinstance,
                                           uint32_t width, uint32_t height,
                                           bool enable_validation) {
    auto r = std::unique_ptr<Renderer>(new Renderer());

    DeviceDesc desc;
    desc.hwnd = hwnd;
    desc.hinstance = hinstance;
    desc.enable_validation = enable_validation;
    r->device_ = Device::create(desc);
    if (!r->device_) return nullptr;

    r->swapchain_ = std::make_unique<Swapchain>(*r->device_, width, height);
    r->create_per_image_sync();

    VkDevice dev = r->device_->device();
    for (Frame& f : r->frames_) {
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.queueFamilyIndex = r->device_->graphics_family();
        vk_check(vkCreateCommandPool(dev, &pool_info, nullptr, &f.pool),
                 "vkCreateCommandPool(frame)");

        VkCommandBufferAllocateInfo cb_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cb_info.commandPool = f.pool;
        cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb_info.commandBufferCount = 1;
        vk_check(vkAllocateCommandBuffers(dev, &cb_info, &f.cmd),
                 "vkAllocateCommandBuffers(frame)");

        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vk_check(vkCreateFence(dev, &fence_info, nullptr, &f.in_flight),
                 "vkCreateFence(frame)");

        VkSemaphoreCreateInfo sem_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vk_check(vkCreateSemaphore(dev, &sem_info, nullptr, &f.acquire),
                 "vkCreateSemaphore(acquire)");
    }
    return r;
}

Renderer::~Renderer() {
    if (!device_) return;
    device_->wait_idle();
    VkDevice dev = device_->device();
    if (capture_buf_)
        vmaDestroyBuffer(device_->allocator(), capture_buf_, capture_alloc_);
    for (Frame& f : frames_) {
        if (f.in_flight) vkDestroyFence(dev, f.in_flight, nullptr);
        if (f.acquire) vkDestroySemaphore(dev, f.acquire, nullptr);
        if (f.pool) vkDestroyCommandPool(dev, f.pool, nullptr);
    }
    for (VkSemaphore s : render_done_)
        vkDestroySemaphore(dev, s, nullptr);
    swapchain_.reset();
}

bool Renderer::ensure_capture_buffer(size_t bytes) {
    if (capture_capacity_ >= bytes) return true;
    if (capture_buf_) {
        vmaDestroyBuffer(device_->allocator(), capture_buf_, capture_alloc_);
        capture_buf_ = VK_NULL_HANDLE;
        capture_capacity_ = 0;
    }
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo mapped{};
    if (vmaCreateBuffer(device_->allocator(), &info, &alloc_info,
                        &capture_buf_, &capture_alloc_,
                        &mapped) != VK_SUCCESS) {
        log_error("gfx: capture buffer alloc failed (%zu bytes)", bytes);
        capture_buf_ = VK_NULL_HANDLE;
        return false;
    }
    capture_mapped_ = mapped.pMappedData;
    capture_capacity_ = bytes;
    return true;
}

bool Renderer::take_capture(std::vector<uint8_t>* out, uint32_t* w,
                            uint32_t* h) {
    if (!capture_ready_) return false;
    *out = std::move(capture_rgba_);
    *w = capture_w_;
    *h = capture_h_;
    capture_rgba_.clear();
    capture_ready_ = false;
    return true;
}

void Renderer::create_per_image_sync() {
    VkDevice dev = device_->device();
    for (VkSemaphore s : render_done_)
        vkDestroySemaphore(dev, s, nullptr);
    render_done_.assign(swapchain_->image_count(), VK_NULL_HANDLE);
    VkSemaphoreCreateInfo sem_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkSemaphore& s : render_done_)
        vk_check(vkCreateSemaphore(dev, &sem_info, nullptr, &s),
                 "vkCreateSemaphore(render_done)");
}

void Renderer::notify_resize(uint32_t width, uint32_t height) {
    pending_width_ = width;
    pending_height_ = height;
    needs_recreate_ = true;
}

bool Renderer::begin_frame(FrameContext& out) {
    if (needs_recreate_) {
        if (pending_width_ == 0 || pending_height_ == 0)
            return false;   // minimized — nothing to render
        // Semaphores may be pending from the destroyed images; rebuild all.
        device_->wait_idle();
        swapchain_->recreate(pending_width_, pending_height_);
        create_per_image_sync();
        needs_recreate_ = false;
    }
    if (swapchain_->extent().width == 0 || swapchain_->extent().height == 0)
        return false;

    VkDevice dev = device_->device();
    Frame& f = frames_[frame_counter_ % kFramesInFlight];

    vk_check(vkWaitForFences(dev, 1, &f.in_flight, VK_TRUE, UINT64_MAX),
             "vkWaitForFences(frame)");

    uint32_t image_index = 0;
    VkResult r = swapchain_->acquire(f.acquire, &image_index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        notify_resize(swapchain_->extent().width, swapchain_->extent().height);
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        vk_check(r, "vkAcquireNextImageKHR");

    vk_check(vkResetFences(dev, 1, &f.in_flight), "vkResetFences(frame)");
    vk_check(vkResetCommandPool(dev, f.pool, 0), "vkResetCommandPool(frame)");

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(f.cmd, &begin), "vkBeginCommandBuffer(frame)");

    out.cmd = f.cmd;
    out.frame_index = frame_counter_ % kFramesInFlight;
    out.image_index = image_index;
    out.extent = swapchain_->extent();
    return true;
}

void Renderer::begin_present_pass(const FrameContext& frame,
                                  const VkClearColorValue& clear) {
    // Swapchain image: undefined -> color attachment.
    VkImageMemoryBarrier to_color{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_color.srcAccessMask = 0;
    to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_color.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.image = swapchain_->image(frame.image_index);
    to_color.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(frame.cmd,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_color);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = swapchain_->view(frame.image_index);
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = clear;

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, swapchain_->extent()};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(frame.cmd, &rendering);

    VkViewport viewport{0.0f, 0.0f,
                        static_cast<float>(swapchain_->extent().width),
                        static_cast<float>(swapchain_->extent().height),
                        0.0f, 1.0f};
    vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, swapchain_->extent()};
    vkCmdSetScissor(frame.cmd, 0, 1, &scissor);
}

void Renderer::end_frame(const FrameContext& frame) {
    Frame& f = frames_[frame.frame_index];

    vkCmdEndRendering(f.cmd);

    // Screenshot capture: route the finished image through TRANSFER_SRC
    // and copy it out before the present transition.
    const size_t cap_bytes = size_t{frame.extent.width} *
                             frame.extent.height * 4;
    const bool capturing =
        capture_pending_ && ensure_capture_buffer(cap_bytes);
    capture_pending_ = false;   // a failed alloc drops the request, logged
    if (capturing) {
        VkImageMemoryBarrier to_src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.image = swapchain_->image(frame.image_index);
        to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(f.cmd,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_src);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {frame.extent.width, frame.extent.height, 1};
        vkCmdCopyImageToBuffer(f.cmd, swapchain_->image(frame.image_index),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               capture_buf_, 1, &region);
    }

    VkImageMemoryBarrier to_present{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_present.srcAccessMask = capturing ? VK_ACCESS_TRANSFER_READ_BIT
                                         : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = capturing
                               ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                               : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image = swapchain_->image(frame.image_index);
    to_present.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(f.cmd,
                         capturing ? VK_PIPELINE_STAGE_TRANSFER_BIT
                                   : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_present);

    vk_check(vkEndCommandBuffer(f.cmd), "vkEndCommandBuffer(frame)");

    VkSemaphore render_done = render_done_[frame.image_index];
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &f.acquire;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &f.cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_done;

    // Queue access is shared with export workers — serialize.
    std::lock_guard<std::mutex> lock(device_->queue_mutex());
    vk_check(vkQueueSubmit(device_->graphics_queue(), 1, &submit, f.in_flight),
             "vkQueueSubmit(frame)");

    VkResult r = swapchain_->present(device_->graphics_queue(), render_done,
                                     frame.image_index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        notify_resize(swapchain_->extent().width, swapchain_->extent().height);
    } else if (r != VK_SUCCESS) {
        vk_check(r, "vkQueuePresentKHR");
    }
    ++frame_counter_;

    if (capturing) {
        // One blocking fence wait on the capturing frame only; then the
        // bytes convert to RGBA rows (swapchain formats are BGRA-ordered
        // on this platform; sRGB-encoded bytes are exactly what the PNG
        // wants).
        vk_check(vkWaitForFences(device_->device(), 1, &f.in_flight, VK_TRUE,
                                 UINT64_MAX),
                 "vkWaitForFences(capture)");
        vmaInvalidateAllocation(device_->allocator(), capture_alloc_, 0,
                                cap_bytes);
        capture_rgba_.resize(cap_bytes);
        std::memcpy(capture_rgba_.data(), capture_mapped_, cap_bytes);
        const VkFormat fmt = swapchain_->format();
        const bool bgra = fmt == VK_FORMAT_B8G8R8A8_UNORM ||
                          fmt == VK_FORMAT_B8G8R8A8_SRGB;
        if (bgra) {
            uint8_t* p = capture_rgba_.data();
            for (size_t i = 0; i < cap_bytes; i += 4)
                std::swap(p[i], p[i + 2]);
        }
        // The UI composites opaque; force alpha so viewers ignore
        // whatever the blend left in the channel.
        for (size_t i = 3; i < cap_bytes; i += 4) capture_rgba_[i] = 255;
        capture_w_ = frame.extent.width;
        capture_h_ = frame.extent.height;
        capture_ready_ = true;
    }
}

}  // namespace looks::gfx
