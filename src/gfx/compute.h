// Arena descriptor sets live only until reset() runs for the same frame slot.

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
    const char* spv_name = nullptr;
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

    // Inputs: SHADER_READ_ONLY or GENERAL. Outputs: GENERAL. Caller sets both.
    // Group counts assume an 8x8 kernel local size.
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

VkShaderModule load_shader_module(Device& device,
                                  const std::filesystem::path& path);

struct GraphicsPipelineDesc {
    VkShaderModule vs = VK_NULL_HANDLE;
    VkShaderModule fs = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    const VkPipelineVertexInputStateCreateInfo* vertex_input = nullptr;
    const VkPipelineColorBlendAttachmentState* blend = nullptr;
};

VkPipeline create_graphics_pipeline(Device& device,
                                    const GraphicsPipelineDesc& desc,
                                    const char* label);

}  // namespace looks::gfx
