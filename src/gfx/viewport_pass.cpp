#include "gfx/viewport_pass.h"

#include <algorithm>
#include <cmath>

#include "gfx/vk_device.h"
#include "util/file.h"
#include "util/log.h"

namespace looks::gfx {

namespace {

struct ViewportPush {
    float scale[2];    // fitted quad size in NDC
    float offset[2];   // fitted quad center in NDC
};

VkShaderModule load_shader(Device& device, const std::filesystem::path& path) {
    auto bytes = read_file_bytes(path);
    if (!bytes || bytes->size() % 4 != 0) {
        log_error("gfx: failed to load shader %s", path.string().c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes->size();
    info.pCode = reinterpret_cast<const uint32_t*>(bytes->data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device.device(), &info, nullptr, &module) !=
        VK_SUCCESS)
        return VK_NULL_HANDLE;
    return module;
}

}  // namespace

std::unique_ptr<ViewportPass> ViewportPass::create(
    Device& device, VkFormat color_format,
    const std::filesystem::path& shader_dir) {
    auto p = std::unique_ptr<ViewportPass>(new ViewportPass(device));
    if (!p->init(color_format, shader_dir)) return nullptr;
    return p;
}

ViewportPass::~ViewportPass() {
    VkDevice dev = device_.device();
    device_.wait_idle();
    if (pipeline_) vkDestroyPipeline(dev, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(dev, set_layout_, nullptr);
}

bool ViewportPass::init(VkFormat color_format,
                        const std::filesystem::path& shader_dir) {
    VkDevice dev = device_.device();

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 1;
    set_info.pBindings = &binding;
    vk_check(vkCreateDescriptorSetLayout(dev, &set_info, nullptr, &set_layout_),
             "vkCreateDescriptorSetLayout(viewport)");

    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ViewportPush)};
    VkPipelineLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    vk_check(vkCreatePipelineLayout(dev, &layout_info, nullptr, &layout_),
             "vkCreatePipelineLayout(viewport)");

    VkShaderModule vs = load_shader(device_, shader_dir / "viewport.vert.spv");
    VkShaderModule fs = load_shader(device_, shader_dir / "viewport.frag.spv");
    bool ok = vs && fs;
    if (ok) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";

        // Fullscreen triangle: no vertex input.
        VkPipelineVertexInputStateCreateInfo vertex_input{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

        VkPipelineInputAssemblyStateCreateInfo input_assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Opaque video: no blend.
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;

        VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                           VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;

        VkPipelineRenderingCreateInfo rendering{
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &color_format;

        VkGraphicsPipelineCreateInfo info{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &input_assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = layout_;
        VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info,
                                               nullptr, &pipeline_);
        if (r != VK_SUCCESS) {
            log_error("gfx: viewport pipeline failed: %s", vk_result_name(r));
            ok = false;
        }
    }
    if (vs) vkDestroyShaderModule(dev, vs, nullptr);
    if (fs) vkDestroyShaderModule(dev, fs, nullptr);
    return ok;
}

void ViewportPass::draw(VkCommandBuffer cmd, DescriptorArena& arena,
                        uint32_t frame_index, GpuImage& image,
                        VkSampler sampler, VkExtent2D extent, float dst_x,
                        float dst_y, float dst_w, float dst_h, float clip_x0,
                        float clip_x1) {
    if (dst_w < 1.0f || dst_h < 1.0f || extent.width == 0 ||
        extent.height == 0 || clip_x1 <= clip_x0)
        return;

    // Aspect-preserving fit, centered in the dst rect.
    const float aspect = static_cast<float>(image.width()) /
                         static_cast<float>(image.height());
    float fit_w = dst_w;
    float fit_h = fit_w / aspect;
    if (fit_h > dst_h) {
        fit_h = dst_h;
        fit_w = fit_h * aspect;
    }
    const float cx = dst_x + dst_w * 0.5f;
    const float cy = dst_y + dst_h * 0.5f;
    const float ew = static_cast<float>(extent.width);
    const float eh = static_cast<float>(extent.height);

    ViewportPush push;
    push.scale[0] = fit_w / ew;
    push.scale[1] = fit_h / eh;
    push.offset[0] = cx * 2.0f / ew - 1.0f;
    push.offset[1] = cy * 2.0f / eh - 1.0f;

    VkDescriptorSet set = arena.allocate(frame_index, set_layout_);
    VkDescriptorImageInfo image_info{sampler, image.view(),
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(device_.device(), 1, &write, 0, nullptr);

    // The cover triangle overshoots the fitted rect — scissor confines it
    // (further narrowed by the wipe clip fractions).
    const float fit_left = cx - fit_w * 0.5f;
    const int32_t sx = std::max(
        0, static_cast<int32_t>(std::floor(fit_left + fit_w * clip_x0)));
    const int32_t sy = std::max(0, static_cast<int32_t>(std::floor(cy - fit_h * 0.5f)));
    const int32_t sr = std::min(
        static_cast<int32_t>(extent.width),
        static_cast<int32_t>(std::ceil(fit_left + fit_w * clip_x1)));
    const int32_t sb = std::min(static_cast<int32_t>(extent.height),
                                static_cast<int32_t>(std::ceil(cy + fit_h * 0.5f)));
    if (sr <= sx || sb <= sy) return;
    VkRect2D scissor{{sx, sy},
                     {static_cast<uint32_t>(sr - sx),
                      static_cast<uint32_t>(sb - sy)}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1,
                            &set, 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    VkRect2D full{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &full);
}

}  // namespace looks::gfx
