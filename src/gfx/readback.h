// Export readback (spec §3): offline frame-by-frame graph evaluation at
// full res with a fenced staging readback. Owns its own command pool/fence
// and NV12 conversion targets so an export worker can drive a private
// Engine instance concurrently with the preview loop (queue submissions are
// serialized by Device::queue_mutex).

#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "gfx/engine.h"

namespace looks::gfx {

class Nv12Readback {
public:
    static std::unique_ptr<Nv12Readback> create(
        Device& device, const std::filesystem::path& shader_dir);
    ~Nv12Readback();

    Nv12Readback(const Nv12Readback&) = delete;
    Nv12Readback& operator=(const Nv12Readback&) = delete;

    // Evaluates the document over `source` through `engine`, converts to
    // NV12, and blocks on the fence. `out` receives packed NV12 (stride ==
    // width; Y plane then interleaved UV). Dimensions must be even.
    // cache_ctx passes through to Engine::render (0 = bypass the frame
    // render cache — the default for export, which visits each frame once).
    bool render(Engine& engine, const SourcePlanes& source,
                const doc::Document& doc, uint32_t timeline_frame, double fps,
                std::vector<uint8_t>& out,
                const Engine::MaskSourceFrame* mask_sources = nullptr,
                size_t mask_source_count = 0, uint64_t cache_ctx = 0,
                const Engine::LayerSourceFrame* layer_sources = nullptr,
                size_t layer_source_count = 0);

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
