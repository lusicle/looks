#pragma once

#include <functional>
#include <atomic>

#include "gfx/engine.h"
#include "media/bundle.h"
#include "mod/eval.h"

namespace looks::app {

class ModeRenderer {
public:
    ModeRenderer(gfx::Device& device, std::filesystem::path shaders);
    bool generate(gfx::Engine& engine, const doc::Document& document,
              const std::vector<media::AssetBundle>& bundles, uint64_t look, uint64_t effect,
              const std::filesystem::path& path, std::atomic<bool>& cancelled,
              std::atomic<uint32_t>& done, std::atomic<uint32_t>& total,
              const mod::AnalysisCurves* analysis = nullptr,
              const mod::NodeAudioMap* audio = nullptr,
              const mod::NodeCameraMap* camera = nullptr);

private:
    std::unique_ptr<gfx::GpuImage> analyse(gfx::Engine& parent, uint64_t look,
                      const doc::EffectInstance& effect, uint32_t width, uint32_t height);
    gfx::Device& device_;
    std::filesystem::path shaders_;
    const doc::Document* document_ = nullptr;
    const std::vector<media::AssetBundle>* bundles_ = nullptr;
    const mod::AnalysisCurves* analysis_ = nullptr;
    const mod::NodeAudioMap* audio_ = nullptr;
    const mod::NodeCameraMap* camera_ = nullptr;
    std::function<bool()> cancelled_;
    std::atomic<uint32_t>* done_ = nullptr;
    std::atomic<uint32_t>* total_ = nullptr;
    std::unique_ptr<gfx::ComputePipeline> resolve_;
};

}
