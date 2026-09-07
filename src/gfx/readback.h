// Safe on an export thread; queue submits take Device::queue_mutex.

#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "gfx/engine.h"

namespace looks::gfx {

class RgbaReadback {
public:
    static std::unique_ptr<RgbaReadback> create(Device& device);
    ~RgbaReadback();
    bool read(GpuImage& image, std::vector<uint8_t>& rgba, bool alpha = true);
    bool render(Engine& engine, const doc::Document& doc, uint64_t entity,
                uint32_t frame, double fps, uint32_t width, uint32_t height,
                std::vector<uint8_t>& rgba, const Engine::LayerSourceFrame* sources,
                size_t source_count, bool alpha = true, bool read_pixels = true,
                uint64_t preview_node = 0, uint64_t preview_layer = 0,
                uint32_t clock_frame = UINT32_MAX);
private:
    bool begin(uint32_t width, uint32_t height, bool read_pixels);
    bool finish(GpuImage& image, std::vector<uint8_t>& rgba, bool alpha, bool read_pixels);
    explicit RgbaReadback(Device& device) : device_(device) {}
    Device& device_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    void* mapped_ = nullptr;
    size_t capacity_ = 0;
};

class Nv12Readback {
public:
    static std::unique_ptr<Nv12Readback> create(
        Device& device, const std::filesystem::path& shader_dir);
    ~Nv12Readback();

    Nv12Readback(const Nv12Readback&) = delete;
    Nv12Readback& operator=(const Nv12Readback&) = delete;

    // out gets packed NV12: Y plane, then interleaved UV; stride == width.
    // Canvas dimensions must be even. cache_ctx 0 bypasses the render cache.
    bool render(Engine& engine, const doc::Document& doc, uint64_t look_id,
                uint32_t timeline_frame, double fps, uint32_t canvas_w,
                uint32_t canvas_h, std::vector<uint8_t>& out,
                uint64_t cache_ctx = 0, uint32_t cache_frame = 0,
                const Engine::LayerSourceFrame* layer_sources = nullptr,
                size_t layer_source_count = 0,
                uint64_t measure_placement = 0);

private:
    explicit Nv12Readback(Device& device) : device_(device) {}
    bool init(const std::filesystem::path& shader_dir);
    bool ensure_targets(uint32_t width, uint32_t height);

    Device& device_;
    std::unique_ptr<ComputePipeline> to_nv12_;
    std::unique_ptr<GpuImage> y_image_, uv_image_;
    VkBuffer readback_ = VK_NULL_HANDLE;
    VmaAllocation readback_alloc_ = nullptr;
    void* mapped_ = nullptr;
    size_t capacity_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
};

}  // namespace looks::gfx
