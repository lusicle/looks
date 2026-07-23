#include "gfx/compute.h"

#include "util/file.h"
#include "util/log.h"

namespace looks::gfx {

DescriptorArena::DescriptorArena(Device& device) : device_(device) {
    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
    };
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.maxSets = 256;
    info.poolSizeCount = 2;
    info.pPoolSizes = sizes;
    for (VkDescriptorPool& pool : pools_)
        vk_check(vkCreateDescriptorPool(device.device(), &info, nullptr, &pool),
                 "vkCreateDescriptorPool(arena)");
}

DescriptorArena::~DescriptorArena() {
    for (VkDescriptorPool pool : pools_)
        if (pool) vkDestroyDescriptorPool(device_.device(), pool, nullptr);
}

void DescriptorArena::reset(uint32_t frame_index) {
    vkResetDescriptorPool(device_.device(),
                          pools_[frame_index % kFramesInFlight], 0);
}

VkDescriptorSet DescriptorArena::allocate(uint32_t frame_index,
                                          VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = pools_[frame_index % kFramesInFlight];
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    vk_check(vkAllocateDescriptorSets(device_.device(), &info, &set),
             "vkAllocateDescriptorSets(arena)");
    return set;
}

std::unique_ptr<ComputePipeline> ComputePipeline::create(
    Device& device, const std::filesystem::path& shader_dir,
    const ComputePipelineDesc& desc) {
    auto cp = std::unique_ptr<ComputePipeline>(new ComputePipeline());
    cp->device_ = &device;
    cp->desc_ = desc;

    auto bytes = read_file_bytes(shader_dir / desc.spv_name);
    if (!bytes || bytes->size() % 4 != 0) {
        log_error("gfx: compute shader missing: %s", desc.spv_name);
        return nullptr;
    }
    VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = bytes->size();
    module_info.pCode = reinterpret_cast<const uint32_t*>(bytes->data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device.device(), &module_info, nullptr, &module) !=
        VK_SUCCESS)
        return nullptr;

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (uint32_t i = 0; i < desc.sampled_inputs; ++i)
        bindings.push_back({i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    for (uint32_t i = 0; i < desc.storage_outputs; ++i)
        bindings.push_back({desc.sampled_inputs + i,
                            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo set_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = static_cast<uint32_t>(bindings.size());
    set_info.pBindings = bindings.data();
    vk_check(vkCreateDescriptorSetLayout(device.device(), &set_info, nullptr,
                                         &cp->set_layout_),
             "vkCreateDescriptorSetLayout(compute)");

    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, desc.push_bytes};
    VkPipelineLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &cp->set_layout_;
    if (desc.push_bytes > 0) {
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
    }
    vk_check(vkCreatePipelineLayout(device.device(), &layout_info, nullptr,
                                    &cp->layout_),
             "vkCreatePipelineLayout(compute)");

    VkComputePipelineCreateInfo pipe_info{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipe_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipe_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipe_info.stage.module = module;
    pipe_info.stage.pName = "main";
    pipe_info.layout = cp->layout_;
    const VkResult r = vkCreateComputePipelines(device.device(), VK_NULL_HANDLE,
                                                1, &pipe_info, nullptr,
                                                &cp->pipeline_);
    vkDestroyShaderModule(device.device(), module, nullptr);
    if (r != VK_SUCCESS) {
        log_error("gfx: compute pipeline failed: %s", desc.spv_name);
        return nullptr;
    }
    return cp;
}

ComputePipeline::~ComputePipeline() {
    if (!device_) return;
    if (pipeline_) vkDestroyPipeline(device_->device(), pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device_->device(), layout_, nullptr);
    if (set_layout_)
        vkDestroyDescriptorSetLayout(device_->device(), set_layout_, nullptr);
}

void ComputePipeline::dispatch(VkCommandBuffer cmd, DescriptorArena& arena,
                               uint32_t frame_index,
                               const GpuImage* const* sampled,
                               uint32_t sampled_count, GpuImage* const* storage,
                               uint32_t storage_count, const void* push,
                               uint32_t push_bytes, uint32_t width,
                               uint32_t height, VkSampler sampler) {
    VkDescriptorSet set = arena.allocate(frame_index, set_layout_);

    VkDescriptorImageInfo image_infos[8]{};
    VkWriteDescriptorSet writes[8]{};
    uint32_t w = 0;
    for (uint32_t i = 0; i < sampled_count && w < 8; ++i, ++w) {
        image_infos[w] = {sampler, sampled[i]->view(),
                          sampled[i]->layout()};
        writes[w] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[w].dstSet = set;
        writes[w].dstBinding = i;
        writes[w].descriptorCount = 1;
        writes[w].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[w].pImageInfo = &image_infos[w];
    }
    for (uint32_t i = 0; i < storage_count && w < 8; ++i, ++w) {
        image_infos[w] = {VK_NULL_HANDLE, storage[i]->view(),
                          VK_IMAGE_LAYOUT_GENERAL};
        writes[w] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[w].dstSet = set;
        writes[w].dstBinding = desc_.sampled_inputs + i;
        writes[w].descriptorCount = 1;
        writes[w].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[w].pImageInfo = &image_infos[w];
    }
    vkUpdateDescriptorSets(device_->device(), w, writes, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1,
                            &set, 0, nullptr);
    if (push && push_bytes)
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           push_bytes, push);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
}

}  // namespace looks::gfx
