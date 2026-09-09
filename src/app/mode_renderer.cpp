#include "app/mode_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <vk_mem_alloc.h>

#include "doc/effects.h"
#include "gfx/graph.h"
#include "gfx/vk_device.h"
#include "media/decode_pool.h"
#include "util/log.h"
#include "util/linear_image.h"

namespace looks::app {

namespace {

struct Submission {
    gfx::Device& device;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    gfx::DescriptorArena arena;
    explicit Submission(gfx::Device& d) : device(d), arena(d) {}
    ~Submission() {
        if (fence) vkDestroyFence(device.device(), fence, nullptr);
        if (pool) vkDestroyCommandPool(device.device(), pool, nullptr);
    }
    bool init() {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = device.graphics_family();
        if (vkCreateCommandPool(device.device(), &pi, nullptr, &pool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ci.commandPool = pool;
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ci.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device.device(), &ci, &cmd) != VK_SUCCESS) return false;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        return vkCreateFence(device.device(), &fi, nullptr, &fence) == VK_SUCCESS;
    }
    bool begin() {
        vkResetCommandPool(device.device(), pool, 0);
        arena.reset(0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS;
    }
    bool finish() {
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) return false;
        gfx::submit_and_wait(device, device.graphics_queue(), cmd, fence, "Mode analysis");
        return true;
    }
};

}

ModeRenderer::ModeRenderer(gfx::Device& device, std::filesystem::path shaders)
    : device_(device), shaders_(std::move(shaders)) {}

bool ModeRenderer::generate(gfx::Engine& engine, const doc::Document& document,
                       const std::vector<media::AssetBundle>& bundles, uint64_t look, uint64_t effect,
                       const std::filesystem::path& path, std::atomic<bool>& cancelled,
                       std::atomic<uint32_t>& done, std::atomic<uint32_t>& total,
                       const mod::AnalysisCurves* analysis, const mod::NodeAudioMap* audio,
                       const mod::NodeCameraMap* camera) {
    document_ = &document;
    bundles_ = &bundles;
    analysis_ = analysis;
    audio_ = audio;
    camera_ = camera;
    cancelled_ = [&cancelled] { return cancelled.load(); };
    done_ = &done;
    total_ = &total;
    const auto* owner = document.find_look(look);
    const auto* fx = owner ? doc::find_effect(*owner, effect) : nullptr;
    if (!fx || fx->type != doc::EffectType::Mode) return false;
    double content_width, content_height;
    doc::content_size(document, look, &content_width, &content_height);
    const uint32_t width = uint32_t(std::ceil(content_width));
    const uint32_t height = uint32_t(std::ceil(content_height));
    auto image = analyse(engine, look, *fx, width, height);
    if (!image || cancelled) return false;
    Submission submission(device_);
    if (!submission.init()) return false;
    struct Readback {
        gfx::Device& device;
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        void* mapped = nullptr;
        ~Readback() { if (buffer) vmaDestroyBuffer(device.allocator(), buffer, allocation); }
    } readback{device_};
    const size_t bytes = size_t(width) * height * 8;
    if (!gfx::create_mapped_buffer(device_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            &readback.buffer, &readback.allocation, &readback.mapped) || !submission.begin()) return false;
    image->transition(submission.cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(submission.cmd, image->image(), image->layout(), readback.buffer, 1, &copy);
    gfx::memory_barrier(submission.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    if (!submission.finish() || cancelled) return false;
    vmaInvalidateAllocation(device_.allocator(), readback.allocation, 0, bytes);
    LinearImage result;
    result.width = width;
    result.height = height;
    const auto* pixels = static_cast<const uint16_t*>(readback.mapped);
    result.pixels.assign(pixels, pixels + bytes / 2);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    return !error && write_linear_image(path, result);
}

std::unique_ptr<gfx::GpuImage> ModeRenderer::analyse(gfx::Engine& parent, uint64_t look,
                               const doc::EffectInstance& effect, uint32_t width, uint32_t height) {
    if (!document_ || !bundles_ || (cancelled_ && cancelled_())) return nullptr;
    media::DecodePool pool("Mode");
    doc::Document input_document = *document_;
    input_document.look(look).duration = 0;
    pool.set_document(input_document, look, *bundles_, 1);
    const double fps = doc::entity_fps(*document_, look);
    std::vector<gfx::Engine::LayerSourceFrame> sources;
    auto resolve_frame = [&](uint32_t frame) -> std::optional<doc::Document> {
        sources.clear();
        const auto& decoded = pool.collect(frame);
        for (const auto& source : decoded) {
            if (!source.frame) return std::nullopt;
            const auto view = source.frame->view();
            gfx::Engine::LayerSourceFrame item;
            item.key = source.key;
            item.content_stamp = source.frame->stamp;
            item.planes = {view.y.data, size_t(view.y.stride), view.u.data,
                size_t(view.u.stride), view.v.data, size_t(view.v.stride),
                uint32_t(view.width), uint32_t(view.height), source.frame->nv12,
                source.frame->rgba.empty() ? nullptr : source.frame->rgba.data()};
            sources.push_back(item);
        }
        mod::SourceFrameView video;
        if (!sources.empty()) {
            const auto& p = sources.front().planes;
            video.y = p.y; video.u = p.u; video.v = p.v;
            video.y_stride = int(p.y_stride); video.u_stride = int(p.u_stride);
            video.v_stride = int(p.v_stride); video.width = int(p.width);
            video.height = int(p.height); video.nv12 = p.nv12;
        }
        return mod::resolve(input_document, frame, fps, analysis_,
            -1.0, -1.0, &video, audio_, camera_);
    };
    const auto initial = resolve_frame(0);
    if (!initial) return nullptr;
    const auto* settings = doc::find_effect(initial->look(look), effect.id);
    if (!settings) return nullptr;
    const float tolerance = std::clamp(settings->params[0], 0.0f, 0.25f);
    const uint32_t requested = uint32_t(std::clamp(settings->params[1], 2.0f, 128.0f));
    if (!resolve_) resolve_ = gfx::ComputePipeline::create(device_, shaders_,
        {"mode_resolve.comp.spv", 1, 1, 7 * sizeof(uint32_t)});
    if (!resolve_) return nullptr;
    const uint32_t length = gfx::mode_input_length(*document_, look, effect.id);
    if (!length) return nullptr;
    const auto frames = gfx::mode_sample_frames(length, requested);
    const uint32_t count = uint32_t(frames.size());
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device_.physical(), &properties);
    const uint32_t strip = std::max(1u, std::min({height,
        properties.limits.maxImageDimension2D / count,
        uint32_t((256ull << 20) / (uint64_t(width) * count * 8))}));
    const auto usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto samples = gfx::GpuImage::create(device_, VK_FORMAT_R16G16B16A16_SFLOAT, width, strip * count, usage);
    auto output = gfx::GpuImage::create(device_, VK_FORMAT_R16G16B16A16_SFLOAT, width, height, usage);
    Submission submission(device_);
    if (!samples || !output || !submission.init()) return nullptr;
    log_info("Mode: analysing %u samples over %u frames", count, length);
    const bool history = doc::document_uses_history(input_document);
    total_->store(uint32_t(std::min(uint64_t(UINT32_MAX),
        uint64_t((height + strip - 1) / strip) * (history ? length : count))));
    auto engine = gfx::Engine::create(device_, shaders_);
    if (!engine || !parent.copy_resources_to(*engine)) return nullptr;
    for (uint32_t y = 0; y < height; y += strip) {
        if (y && history) engine->reset_effect_state();
        const uint32_t rows = std::min(strip, height - y);
        uint32_t next = 0;
        for (uint32_t sample = 0; sample < count; ++sample) {
            const uint32_t target = frames[sample];
            if (!history) next = target;
            for (; next <= target; ++next) {
                if (cancelled_ && cancelled_()) return nullptr;
                const auto resolved = resolve_frame(next);
                if (!resolved) return nullptr;
                if (!submission.begin()) return nullptr;
                auto* image = engine->render(submission.cmd, 0, *resolved, look, next, fps,
                    width, height, 0, next, nullptr, sources.data(), sources.size(),
                    0, 0, 0, false, effect.id);
                if (!image) { vkEndCommandBuffer(submission.cmd); return nullptr; }
                if (next == target) {
                    image->transition(submission.cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    samples->transition(submission.cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    VkImageCopy copy{};
                    copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    copy.srcOffset = {0, int32_t(y), 0};
                    copy.dstOffset = {0, int32_t(sample * strip), 0};
                    copy.extent = {width, rows, 1};
                    vkCmdCopyImage(submission.cmd, image->image(), image->layout(),
                        samples->image(), samples->layout(), 1, &copy);
                    image->transition(submission.cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
                if (!submission.finish()) return nullptr;
                done_->fetch_add(1);
            }
        }
        const uint32_t batch = std::max(1u, 65536u / width);
        for (uint32_t row = 0; row < rows; row += batch) {
            if (cancelled_() || !submission.begin()) return nullptr;
            samples->transition(submission.cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            output->transition(submission.cmd, VK_IMAGE_LAYOUT_GENERAL);
            uint32_t bits;
            std::memcpy(&bits, &tolerance, sizeof(bits));
            const uint32_t n = std::min(batch, rows - row);
            const uint32_t push[] = {width, n, count, strip, y + row, bits, row};
            const gfx::GpuImage* input[] = {samples.get()};
            gfx::GpuImage* dest[] = {output.get()};
            resolve_->dispatch(submission.cmd, submission.arena, 0, input, 1, dest, 1,
                push, sizeof(push), width, n, parent.linear_sampler());
            output->transition(submission.cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (!submission.finish()) return nullptr;
        }
    }
    log_info("Mode: analysis complete");
    return output;
}

}
