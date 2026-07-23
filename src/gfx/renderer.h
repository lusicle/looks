// Frame orchestration: swapchain acquire/submit/present + per-frame command
// buffers and sync. Owns Device + Swapchain. Effect-graph rendering will
// record into the same per-frame command buffer between begin_frame and
// end_frame; the UI pass composites last, directly onto the swapchain image.

#pragma once

#include <memory>

#include "gfx/vk_api.h"
#include "gfx/vk_device.h"
#include "gfx/vk_swapchain.h"

namespace looks::gfx {

inline constexpr uint32_t kFramesInFlight = 2;

struct FrameContext {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint32_t frame_index = 0;      // 0..kFramesInFlight-1, for per-frame pools
    uint32_t image_index = 0;      // swapchain image
    VkExtent2D extent{};
};

class Renderer {
public:
    static std::unique_ptr<Renderer> create(void* hwnd, void* hinstance,
                                            uint32_t width, uint32_t height,
                                            bool enable_validation);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    Device& device() { return *device_; }
    VkFormat swapchain_format() const { return swapchain_->format(); }
    VkExtent2D swapchain_extent() const { return swapchain_->extent(); }

    // Call on every window resize; recreation happens lazily at frame start.
    void notify_resize(uint32_t width, uint32_t height);

    // False = skip this frame (minimized, zero-sized, or swapchain rebuild).
    // On success the command buffer is recording but NO rendering pass is
    // active — engine compute/upload work records here. Call
    // begin_present_pass before any draw, then end_frame.
    bool begin_frame(FrameContext& out);
    // Transitions the swapchain image and begins the dynamic-rendering pass
    // (cleared to `clear`), with viewport/scissor set to the full extent.
    void begin_present_pass(const FrameContext& frame,
                            const VkClearColorValue& clear);
    void end_frame(const FrameContext& frame);

private:
    Renderer() = default;
    void create_per_image_sync();

    std::unique_ptr<Device> device_;
    std::unique_ptr<Swapchain> swapchain_;

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence in_flight = VK_NULL_HANDLE;
        VkSemaphore acquire = VK_NULL_HANDLE;
    };
    Frame frames_[kFramesInFlight];
    std::vector<VkSemaphore> render_done_;   // one per swapchain image
    uint32_t frame_counter_ = 0;
    uint32_t pending_width_ = 0;
    uint32_t pending_height_ = 0;
    bool needs_recreate_ = false;
};

}  // namespace looks::gfx
