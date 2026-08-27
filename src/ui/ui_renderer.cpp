#include "ui/ui_renderer.h"

#include <vk_mem_alloc.h>

#include <cstring>

#include "util/file.h"
#include "util/log.h"

namespace looks::ui {

namespace {

// Column-major ortho: physical-pixel top-left (0,0) -> NDC (-1,-1),
// bottom-right -> (+1,+1). Vulkan NDC is Y-down, so no flip is needed.
struct Mat4 {
    float m[16];
};

Mat4 make_ortho(float width, float height) {
    Mat4 o{};
    o.m[0] = 2.0f / width;
    o.m[5] = 2.0f / height;
    o.m[10] = 1.0f;
    o.m[12] = -1.0f;
    o.m[13] = -1.0f;
    o.m[15] = 1.0f;
    return o;
}

}  // namespace

std::unique_ptr<UiRenderer> UiRenderer::create(gfx::Device& device,
                                               VkFormat color_format,
                                               const std::filesystem::path& shader_dir) {
    auto r = std::unique_ptr<UiRenderer>(new UiRenderer(device));
    if (!r->init(color_format, shader_dir)) return nullptr;
    return r;
}

UiRenderer::~UiRenderer() {
    VkDevice dev = device_.device();
    device_.wait_idle();
    for (auto& tex : textures_) {
        if (tex->external) continue;   // view/image owned by the engine
        if (tex->view) vkDestroyImageView(dev, tex->view, nullptr);
        if (tex->image) vmaDestroyImage(device_.allocator(), tex->image, tex->allocation);
    }
    for (FrameGeometry& f : frames_) {
        destroy_buffer(f.vertices);
        destroy_buffer(f.indices);
    }
    if (solid_pipeline_) vkDestroyPipeline(dev, solid_pipeline_, nullptr);
    if (text_pipeline_) vkDestroyPipeline(dev, text_pipeline_, nullptr);
    if (solid_layout_) vkDestroyPipelineLayout(dev, solid_layout_, nullptr);
    if (textured_layout_) vkDestroyPipelineLayout(dev, textured_layout_, nullptr);
    if (descriptor_pool_) vkDestroyDescriptorPool(dev, descriptor_pool_, nullptr);
    if (texture_set_layout_) vkDestroyDescriptorSetLayout(dev, texture_set_layout_, nullptr);
    if (nearest_sampler_) vkDestroySampler(dev, nearest_sampler_, nullptr);
    if (linear_sampler_) vkDestroySampler(dev, linear_sampler_, nullptr);
}

VkShaderModule UiRenderer::load_shader(const std::filesystem::path& path) {
    auto bytes = read_file_bytes(path);
    if (!bytes || bytes->size() % 4 != 0) {
        log_error("ui: failed to load shader %s", path.string().c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes->size();
    info.pCode = reinterpret_cast<const uint32_t*>(bytes->data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_.device(), &info, nullptr, &module) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return module;
}

VkPipeline UiRenderer::build_pipeline(VkShaderModule vs, VkShaderModule fs,
                                      VkPipelineLayout layout,
                                      VkFormat color_format,
                                      uint32_t attribute_count) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{0, sizeof(Vertex),
                                            VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[4] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, pos)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, color)},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, shape)},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = attribute_count;
    vertex_input.pVertexAttributeDescriptions = attrs;

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

    // Straight alpha: fragment shaders fold SDF/coverage into alpha.
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
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

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
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
    info.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(device_.device(), VK_NULL_HANDLE, 1,
                                           &info, nullptr, &pipeline);
    if (r != VK_SUCCESS) {
        log_error("ui: pipeline creation failed: %s", gfx::vk_result_name(r));
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

bool UiRenderer::init(VkFormat color_format, const std::filesystem::path& shader_dir) {
    VkDevice dev = device_.device();

    // Texture descriptor set layout: one combined image sampler in FS.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_layout_info.bindingCount = 1;
    set_layout_info.pBindings = &binding;
    gfx::vk_check(vkCreateDescriptorSetLayout(dev, &set_layout_info, nullptr,
                                              &texture_set_layout_),
                  "vkCreateDescriptorSetLayout(ui)");

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 64;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    gfx::vk_check(vkCreateDescriptorPool(dev, &pool_info, nullptr, &descriptor_pool_),
                  "vkCreateDescriptorPool(ui)");

    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.anisotropyEnable = VK_FALSE;
    sampler_info.maxAnisotropy = 1.0f;
    VkSamplerCreateInfo linear_info = sampler_info;
    linear_info.magFilter = VK_FILTER_LINEAR;
    linear_info.minFilter = VK_FILTER_LINEAR;
    gfx::vk_check(vkCreateSampler(dev, &linear_info, nullptr, &linear_sampler_),
                  "vkCreateSampler(ui linear)");
    gfx::vk_check(vkCreateSampler(dev, &sampler_info, nullptr, &nearest_sampler_),
                  "vkCreateSampler(ui)");

    // Pipeline layouts: 64-byte vertex push constant (ortho); the textured
    // layout adds the atlas set plus a 16-byte fragment word (MSDF flag).
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    gfx::vk_check(vkCreatePipelineLayout(dev, &layout_info, nullptr, &solid_layout_),
                  "vkCreatePipelineLayout(ui solid)");
    // One VS|FS range: both text shaders declare the same 80-byte block
    // (ortho + MSDF flag), pushed in a single combined update.
    VkPushConstantRange text_push{
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        sizeof(Mat4) + 16};
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &text_push;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &texture_set_layout_;
    gfx::vk_check(vkCreatePipelineLayout(dev, &layout_info, nullptr, &textured_layout_),
                  "vkCreatePipelineLayout(ui textured)");

    VkShaderModule ui_vs = load_shader(shader_dir / "ui.vert.spv");
    VkShaderModule ui_fs = load_shader(shader_dir / "ui.frag.spv");
    VkShaderModule text_vs = load_shader(shader_dir / "text.vert.spv");
    VkShaderModule text_fs = load_shader(shader_dir / "text.frag.spv");
    bool ok = ui_vs && ui_fs && text_vs && text_fs;
    if (ok) {
        solid_pipeline_ = build_pipeline(ui_vs, ui_fs, solid_layout_, color_format, 4);
        // Text consumes pos/uv/color only — its shader has no shape input.
        text_pipeline_ = build_pipeline(text_vs, text_fs, textured_layout_, color_format, 3);
        ok = solid_pipeline_ && text_pipeline_;
    }
    for (VkShaderModule m : {ui_vs, ui_fs, text_vs, text_fs})
        if (m) vkDestroyShaderModule(dev, m, nullptr);
    return ok;
}

bool UiRenderer::upload_texture(UiTexture& tex, VkFormat format,
                                const uint8_t* pixels, size_t size,
                                const UploadLabels& labels) {
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {tex.width, tex.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(device_.allocator(), &image_info, &alloc_info, &tex.image,
                       &tex.allocation, nullptr) != VK_SUCCESS) {
        log_error(labels.image_fail, tex.width, tex.height);
        return false;
    }

    // Staging buffer + one-shot upload on the graphics queue (init time).
    VkBufferCreateInfo staging_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    staging_info.size = size;
    staging_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo staging_alloc_info{};
    staging_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    staging_alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                               VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation staging_alloc = nullptr;
    VmaAllocationInfo staging_mapped{};
    if (vmaCreateBuffer(device_.allocator(), &staging_info, &staging_alloc_info,
                        &staging, &staging_alloc, &staging_mapped) != VK_SUCCESS) {
        vmaDestroyImage(device_.allocator(), tex.image, tex.allocation);
        return false;
    }
    std::memcpy(staging_mapped.pMappedData, pixels, size);
    vmaFlushAllocation(device_.allocator(), staging_alloc, 0, VK_WHOLE_SIZE);

    VkDevice dev = device_.device();
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device_.graphics_family();
    VkCommandPool pool = VK_NULL_HANDLE;
    gfx::vk_check(vkCreateCommandPool(dev, &pool_info, nullptr, &pool),
                  labels.pool);
    VkCommandBufferAllocateInfo cb_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb_info.commandPool = pool;
    cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_info.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    gfx::vk_check(vkAllocateCommandBuffers(dev, &cb_info, &cmd), labels.cmd);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = tex.image;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_dst);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {tex.width, tex.height, 1};
    vkCmdCopyBufferToImage(cmd, staging, tex.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageMemoryBarrier to_read = to_dst;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &to_read);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    {
        std::lock_guard<std::mutex> lock(device_.queue_mutex());
        gfx::vk_check(vkQueueSubmit(device_.graphics_queue(), 1, &submit,
                                    VK_NULL_HANDLE),
                      labels.submit);
        vkQueueWaitIdle(device_.graphics_queue());
    }
    vkDestroyCommandPool(dev, pool, nullptr);
    vmaDestroyBuffer(device_.allocator(), staging, staging_alloc);

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = tex.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    gfx::vk_check(vkCreateImageView(dev, &view_info, nullptr, &tex.view),
                  labels.view);
    return true;
}

UiTexture* UiRenderer::commit_texture(std::unique_ptr<UiTexture> tex,
                                      VkSampler sampler, const char* set_label) {
    VkDevice dev = device_.device();
    VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_info.descriptorPool = descriptor_pool_;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &texture_set_layout_;
    gfx::vk_check(vkAllocateDescriptorSets(dev, &set_info, &tex->set), set_label);

    VkDescriptorImageInfo image_binding{sampler, tex->view,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = tex->set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_binding;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

    textures_.push_back(std::move(tex));
    return textures_.back().get();
}

const UiTexture* UiRenderer::register_font(Font& font) {
    const uint32_t width = font.atlas_width();
    const uint32_t height = font.atlas_height();
    // MSDF fonts (fontbake): RGBA atlas, linear sampling, and the
    // text shader's median-of-RGB decode (flagged via push constant).
    const bool msdf = font.msdf();
    const std::vector<uint8_t>& pixels =
        msdf ? font.atlas_rgba() : font.atlas_pixels();
    const size_t bpp = msdf ? 4 : 1;
    if (width == 0 || height == 0 || pixels.size() < width * height * bpp)
        return nullptr;

    auto tex = std::make_unique<UiTexture>();
    tex->width = width;
    tex->height = height;
    tex->msdf = msdf;
    if (msdf) {
        tex->unit_range[0] = font.px_range() / static_cast<float>(width);
        tex->unit_range[1] = font.px_range() / static_cast<float>(height);
    }

    // View: replicate R into all channels so the shader's .r read works and
    // future RGBA atlases need no shader change.
    if (!upload_texture(*tex,
                        msdf ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8_UNORM,
                        pixels.data(), pixels.size(),
                        {.image_fail = "ui: font atlas image creation failed",
                         .pool = "vkCreateCommandPool(upload)",
                         .cmd = "vkAllocateCommandBuffers(upload)",
                         .submit = "vkQueueSubmit(upload)",
                         .view = "vkCreateImageView(font atlas)"}))
        return nullptr;

    UiTexture* result =
        commit_texture(std::move(tex), msdf ? linear_sampler_ : nearest_sampler_,
                       "vkAllocateDescriptorSets(font atlas)");
    font.set_texture(result);
    return result;
}

const UiTexture* UiRenderer::register_image(const uint8_t* rgba,
                                            uint32_t width, uint32_t height) {
    if (!rgba || width == 0 || height == 0) return nullptr;
    const size_t bytes = static_cast<size_t>(width) * height * 4;

    auto tex = std::make_unique<UiTexture>();
    tex->width = width;
    tex->height = height;
    tex->rgba_image = true;

    // UNORM, not SRGB: UI colors are sRGB-encoded pass-through values (the
    // viewport blit owns the OETF), so image bytes flow through unchanged.
    if (!upload_texture(*tex, VK_FORMAT_R8G8B8A8_UNORM, rgba, bytes,
                        {.image_fail = "ui: image texture creation failed (%ux%u)",
                         .pool = "vkCreateCommandPool(image upload)",
                         .cmd = "vkAllocateCommandBuffers(image upload)",
                         .submit = "vkQueueSubmit(image upload)",
                         .view = "vkCreateImageView(image)"}))
        return nullptr;

    return commit_texture(std::move(tex), linear_sampler_,
                          "vkAllocateDescriptorSets(image)");
}

const UiTexture* UiRenderer::register_external(VkImageView view,
                                               uint32_t width,
                                               uint32_t height) {
    if (!view || width == 0 || height == 0) return nullptr;
    auto tex = std::make_unique<UiTexture>();
    tex->view = view;
    tex->width = width;
    tex->height = height;
    tex->rgba_image = true;
    tex->external = true;

    return commit_texture(std::move(tex), linear_sampler_,
                          "vkAllocateDescriptorSets(external image)");
}

bool UiRenderer::ensure_capacity(GeometryBuffer& buf, VkDeviceSize needed,
                                 VkBufferUsageFlags usage) {
    if (buf.capacity >= needed) return true;
    destroy_buffer(buf);
    VkDeviceSize capacity = 65536;
    while (capacity < needed) capacity *= 2;

    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = capacity;
    info.usage = usage;
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo mapped{};
    if (vmaCreateBuffer(device_.allocator(), &info, &alloc_info, &buf.buffer,
                        &buf.allocation, &mapped) != VK_SUCCESS) {
        log_error("ui: geometry buffer allocation failed (%llu bytes)",
                  static_cast<unsigned long long>(capacity));
        return false;
    }
    buf.mapped = mapped.pMappedData;
    buf.capacity = capacity;
    return true;
}

void UiRenderer::destroy_buffer(GeometryBuffer& buf) {
    if (buf.buffer)
        vmaDestroyBuffer(device_.allocator(), buf.buffer, buf.allocation);
    buf = {};
}

void UiRenderer::record(VkCommandBuffer cmd, uint32_t frame_index,
                        VkExtent2D extent, const Canvas2D& canvas) {
    if (canvas.total_index_count() == 0) return;
    FrameGeometry& frame = frames_[frame_index % gfx::kFramesInFlight];

    const VkDeviceSize vbytes = canvas.vertices().size() * sizeof(Vertex);
    const VkDeviceSize ibytes = canvas.indices().size() * sizeof(uint16_t);
    if (!ensure_capacity(frame.vertices, vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !ensure_capacity(frame.indices, ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
        return;
    std::memcpy(frame.vertices.mapped, canvas.vertices().data(), vbytes);
    std::memcpy(frame.indices.mapped, canvas.indices().data(), ibytes);
    vmaFlushAllocation(device_.allocator(), frame.vertices.allocation, 0, vbytes);
    vmaFlushAllocation(device_.allocator(), frame.indices.allocation, 0, ibytes);

    const Mat4 ortho = make_ortho(static_cast<float>(extent.width),
                                  static_cast<float>(extent.height));

    VkDeviceSize zero_offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &frame.vertices.buffer, &zero_offset);
    vkCmdBindIndexBuffer(cmd, frame.indices.buffer, 0, VK_INDEX_TYPE_UINT16);

    VkPipeline bound_pipeline = VK_NULL_HANDLE;
    const UiTexture* bound_texture = nullptr;
    Rect bound_scissor{-1, -1, -1, -1};

    for (const Batch& batch : canvas.batches()) {
        if (batch.index_count == 0) continue;
        if (batch.kind == BatchKind::Image &&
            (!batch.texture || !batch.texture->rgba_image))
            continue;   // image batch without a registered image

        // Image batches ride the text pipeline: same vertex layout, and
        // the fragment mode flag (0 A8 / 1 MSDF / 2 plain RGBA) selects
        // the sampling flavor per batch.
        const bool textured = batch.kind != BatchKind::Solid;
        VkPipeline pipeline = textured ? text_pipeline_ : solid_pipeline_;
        VkPipelineLayout layout = textured ? textured_layout_ : solid_layout_;
        if (pipeline != bound_pipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdPushConstants(cmd, layout,
                               textured ? VK_SHADER_STAGE_VERTEX_BIT |
                                              VK_SHADER_STAGE_FRAGMENT_BIT
                                        : VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(Mat4), &ortho);
            bound_pipeline = pipeline;
            bound_texture = nullptr;
        }
        if (textured && batch.texture != bound_texture) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                                    0, 1, &batch.texture->set, 0, nullptr);
            // Per-atlas MSDF decode params ride the fragment tail of the
            // push range (each font atlas has its own unit range).
            uint8_t tail[16] = {};
            const uint32_t flag = batch.texture->rgba_image
                                      ? 2u
                                      : (batch.texture->msdf ? 1u : 0u);
            std::memcpy(tail, &flag, sizeof(flag));
            std::memcpy(tail + 4, batch.texture->unit_range,
                        sizeof(batch.texture->unit_range));
            vkCmdPushConstants(cmd, layout,
                               VK_SHADER_STAGE_VERTEX_BIT |
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                               sizeof(Mat4), sizeof(tail), tail);
            bound_texture = batch.texture;
        }

        if (!(batch.scissor_physical == bound_scissor)) {
            VkRect2D scissor;
            if (batch.scissor_physical.empty()) {
                scissor = {{0, 0}, extent};
            } else {
                const Rect& r = batch.scissor_physical;
                const int32_t x = std::max(0, static_cast<int32_t>(r.x));
                const int32_t y = std::max(0, static_cast<int32_t>(r.y));
                const int32_t right = std::min(static_cast<int32_t>(extent.width),
                                               static_cast<int32_t>(r.right() + 0.5f));
                const int32_t bottom = std::min(static_cast<int32_t>(extent.height),
                                                static_cast<int32_t>(r.bottom() + 0.5f));
                scissor = {{x, y},
                           {static_cast<uint32_t>(std::max(0, right - x)),
                            static_cast<uint32_t>(std::max(0, bottom - y))}};
            }
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            bound_scissor = batch.scissor_physical;
        }

        vkCmdDrawIndexed(cmd, batch.index_count, 1, batch.first_index, 0, 0);
    }

    // Restore the full-framebuffer scissor for whoever records next.
    VkRect2D full{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &full);
}

}  // namespace looks::ui
