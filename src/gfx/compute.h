// Compute pipeline + per-frame descriptor arena (compute-first).
//
// Every effect kernel shares one shape: N sampled inputs (one shared
// sampler), M storage outputs, one push-constant block. Descriptor sets are
// allocated per dispatch from a per-frame-in-flight arena pool (reset when
// the frame slot recycles) — no persistent set management.

#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "gfx/renderer.h"
#include "gfx/texture.h"
#include "gfx/vk_api.h"

namespace looks::gfx {

class DescriptorArena {
public:
    explicit DescriptorArena(Device& device);
    ~DescriptorArena();

    void reset(uint32_t frame_index);
    VkDescriptorSet allocate(uint32_t frame_index, VkDescriptorSetLayout layout);

private:
    Device& device_;
    VkDescriptorPool pools_[kFramesInFlight] = {};
};

struct ComputePipelineDesc {
    const char* spv_name = nullptr;   // file in the staged shader dir
    uint32_t sampled_inputs = 1;
    uint32_t storage_outputs = 1;
    uint32_t push_bytes = 0;
};

class ComputePipeline {
public:
    static std::unique_ptr<ComputePipeline> create(
        Device& device, const std::filesystem::path& shader_dir,
        const ComputePipelineDesc& desc);
    ~ComputePipeline();

    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    // Binds, writes a fresh descriptor set, pushes constants, dispatches
    // ceil(w/8) x ceil(h/8) groups. Inputs must be in SHADER_READ_ONLY (or
    // GENERAL), outputs in GENERAL — caller transitions.
    void dispatch(VkCommandBuffer cmd, DescriptorArena& arena,
                  uint32_t frame_index, const GpuImage* const* sampled,
                  uint32_t sampled_count, GpuImage* const* storage,
                  uint32_t storage_count, const void* push, uint32_t push_bytes,
                  uint32_t width, uint32_t height, VkSampler sampler);

private:
    ComputePipeline() = default;

    Device* device_ = nullptr;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    ComputePipelineDesc desc_{};
};

}  // namespace looks::gfx
