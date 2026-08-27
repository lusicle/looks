// Frame orchestration: swapchain acquire/submit/present + per-frame command
// buffers and sync. Owns Device + Swapchain. Effect-graph rendering will
// record into the same per-frame command buffer between begin_frame and
// end_frame; the UI pass composites last, directly onto the swapchain image.

#pragma once

#include <memory>
#include <vector>

#include "gfx/vk_api.h"
#include "gfx/vk_device.h"
#include "gfx/vk_swapchain.h"

VK_DEFINE_HANDLE(VmaAllocation)

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

    // Script screenshots: the NEXT end_frame copies its finished swapchain
    // image (UI composited) to a host buffer and blocks on its own fence —
    // exact pixels, independent of what covers the OS screen. take_capture
    // hands the RGBA8 rows over once, false while none is ready.
    void request_capture() { capture_pending_ = true; }
    bool take_capture(std::vector<uint8_t>* out, uint32_t* w, uint32_t* h);

private:
    Renderer() = default;
    void create_per_image_sync();
    bool ensure_capture_buffer(size_t bytes);

    std::unique_ptr<Device> device_;
    std::unique_ptr<Swapchain> swapchain_;

    bool capture_pending_ = false;
    bool capture_ready_ = false;
    std::vector<uint8_t> capture_rgba_;
    uint32_t capture_w_ = 0, capture_h_ = 0;
    VkBuffer capture_buf_ = VK_NULL_HANDLE;
    VmaAllocation capture_alloc_ = nullptr;
    void* capture_mapped_ = nullptr;
    size_t capture_capacity_ = 0;

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
