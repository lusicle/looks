#include "gfx/viewport_pass.h"

#include <algorithm>
#include <cmath>

#include "gfx/graph.h"
#include "gfx/vk_device.h"

namespace looks::gfx {

namespace {

struct ViewportPush {
    float scale[2];       // fitted quad size in NDC
    float offset[2];      // fitted quad center in NDC
    uint32_t alpha_mode;  // 0 flatten over black, 1 checkerboard
};

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

    // Each stage's declared push block must sit inside that stage's range.
    VkPushConstantRange push{
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        sizeof(ViewportPush)};
    VkPipelineLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    vk_check(vkCreatePipelineLayout(dev, &layout_info, nullptr, &layout_),
             "vkCreatePipelineLayout(viewport)");

    VkShaderModule vs =
        load_shader_module(device_, shader_dir / "viewport.vert.spv");
    VkShaderModule fs =
        load_shader_module(device_, shader_dir / "viewport.frag.spv");
    if (vs && fs) {
        GraphicsPipelineDesc desc;
        desc.vs = vs;
        desc.fs = fs;
        desc.layout = layout_;
        desc.color_format = color_format;
        pipeline_ = create_graphics_pipeline(device_, desc, "viewport");
    }
    if (vs) vkDestroyShaderModule(dev, vs, nullptr);
    if (fs) vkDestroyShaderModule(dev, fs, nullptr);
    return pipeline_ != VK_NULL_HANDLE;
}

void ViewportPass::draw(VkCommandBuffer cmd, DescriptorArena& arena,
                        uint32_t frame_index, GpuImage& image,
                        VkSampler sampler, VkExtent2D extent, float dst_x,
                        float dst_y, float dst_w, float dst_h, float clip_x0,
                        float clip_x1, uint32_t alpha_mode, float bound_x,
                        float bound_y, float bound_w, float bound_h) {
    if (dst_w < 1.0f || dst_h < 1.0f || extent.width == 0 ||
        extent.height == 0 || clip_x1 <= clip_x0)
        return;

    // Use the same fit formula as graph.h so equal aspects fill exactly.
    float fit[4];
    source_fit_rect(static_cast<float>(image.width()),
                    static_cast<float>(image.height()), dst_w, dst_h, fit);
    const float fit_w = fit[2];
    const float fit_h = fit[3];
    const float cx = dst_x + dst_w * 0.5f;
    const float cy = dst_y + dst_h * 0.5f;
    const float ew = static_cast<float>(extent.width);
    const float eh = static_cast<float>(extent.height);

    ViewportPush push;
    push.scale[0] = fit_w / ew;
    push.scale[1] = fit_h / eh;
    push.offset[0] = cx * 2.0f / ew - 1.0f;
    push.offset[1] = cy * 2.0f / eh - 1.0f;
    push.alpha_mode = alpha_mode;

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

    // The cover triangle overshoots the fitted rect; the scissor confines it.
    const float fit_left = cx - fit_w * 0.5f;
    int32_t sx = std::max(
        0, static_cast<int32_t>(std::floor(fit_left + fit_w * clip_x0)));
    int32_t sy = std::max(0, static_cast<int32_t>(std::floor(cy - fit_h * 0.5f)));
    int32_t sr = std::min(
        static_cast<int32_t>(extent.width),
        static_cast<int32_t>(std::ceil(fit_left + fit_w * clip_x1)));
    int32_t sb = std::min(static_cast<int32_t>(extent.height),
                          static_cast<int32_t>(std::ceil(cy + fit_h * 0.5f)));
    if (bound_w > 0.0f && bound_h > 0.0f) {
        sx = std::max(sx, static_cast<int32_t>(std::floor(bound_x)));
        sy = std::max(sy, static_cast<int32_t>(std::floor(bound_y)));
        sr = std::min(sr,
                      static_cast<int32_t>(std::ceil(bound_x + bound_w)));
        sb = std::min(sb,
                      static_cast<int32_t>(std::ceil(bound_y + bound_h)));
    }
    if (sr <= sx || sb <= sy) return;
    VkRect2D scissor{{sx, sy},
                     {static_cast<uint32_t>(sr - sx),
                      static_cast<uint32_t>(sb - sy)}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1,
                            &set, 0, nullptr);
    vkCmdPushConstants(cmd, layout_,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    VkRect2D full{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &full);
}

}  // namespace looks::gfx
