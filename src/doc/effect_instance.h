#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace looks::doc {

inline constexpr int kWetParam = -1;
inline constexpr int kOpacityParam = -2;

enum class EffectType : uint32_t {
    RgbSplit = 0,
    Vignette,
    Pixelate,
    Grain,
    Jitter,
    Quantize,
    Glow,
    FlowSmear,
    MotionExtract,
    Datamosh,
    GenerationLoss,
    BitrateStarve,
    Echo,
    Feedback,
    Glyph,
    FilmStock,
    Kaleido,
    Polar,
    Turbulence,
    Displace,
    LensDistort,
    Fringe,
    Interlace,
    SliceShuffle,
    PixelStretch,
    Composite,
    Snow,
    SyncFail,
    Timestamp,
    Oversharpen,
    Blur,
    DustScratches,
    LightLeak,
    Anamorphic,
    DirectFlash,
    EdgeDetect,
    Kuwahara,
    CelShade,
    RuttEtra,
    SlitScan,
    CamcorderHud,
    GateMask,
    CueMark,
    ScreenTexture,
    Voronoi,
    ReactionDiffusion,
    ErrorDiffusion,
    Flicker,
    FrameHold,
    Stutter,
    Contour,
    FlowParticles,
    Spherize,
    SoftUpscale,
    ZoomCrunch,
    PixelSort,
    WaveWarp,
    CrtSim,
    Halftone,
    StarFilter,
    Streak,
    SplitTone,
    CornerSoft,
    HeadSwitch,
    VhsOsd,
    CamAuto,
    Mosquito,
    BitPlane,
    BlockShuffle,
    BufferGlitch,
    CrossHatch,
    SpliceBump,
    FilmSlip,
    Emulsion,
    TimeDisplace,
    FlowPaint,
    FmSynth,
    Colorizer,
    Solarize,
    Invert,
    Twirl,
    Tile,
    Emboss,
    LensFlare,
    VelocityScan,
    Lidar,
    Anaglyph,
    Photocopy,
    Risograph,
    WetPlate,
    Glass,
    Watercolor,
    WireTerrain,
    Ridgeline,
    SlowScan,
    VectorTrace,
    ScopeMonitor,
    SecurityMux,
    AudioScope,
    Engraver,
    BlendNode,
    Matte,
    Levels,
    HueSat,
    ChannelMix,
    Posterize,
    Threshold,
    PaletteMap,
    Dither,
    Transform,
    FrameDelay,
    Text,
    WhiteBalance,
    Sharpen,
    CornerPin,
    // is_audio_effect is a range test: new audio types append inside it.
    AudioGain,
    AudioBitcrush,
    AudioDownsample,
    AudioDistortion,
    AudioDelay,
    AudioFilter,
    // Offset shifts a source only when it wires directly onto one.
    // The shift folds into the stream key, so each offset decodes alone.
    Offset,
    TrackPin,
    Vhs,
    Morphology,
    Drip,
    BurnIn,
    Aperture,
    Parallax,
    PatchWeave,
    Halation,
    RollingShutter,
    Normalise,
    Mode,
    Count,
};

inline bool is_audio_effect(EffectType type) {
    return type >= EffectType::AudioGain && type <= EffectType::AudioFilter;
}

// Register here when an effect reads its own previous output.
inline bool is_stateful_feedback(EffectType type) {
    return type == EffectType::Echo || type == EffectType::Feedback ||
           type == EffectType::Lidar || type == EffectType::SlowScan ||
           type == EffectType::VectorTrace ||
           type == EffectType::ScopeMonitor || type == EffectType::Drip ||
           type == EffectType::BurnIn;
}

// null = no aux port. A wired aux input wins over the matte-as-map.
inline const char* effect_aux_port(EffectType type) {
    switch (type) {
        case EffectType::BlendNode: return "b";
        case EffectType::Displace: return "map";
        case EffectType::TimeDisplace: return "map";
        case EffectType::TrackPin: return "b";
        case EffectType::Aperture: return "depth";
        case EffectType::Anaglyph: return "right";
        case EffectType::Parallax: return "depth";
        default: return nullptr;
    }
}

// These effects do a CPU roundtrip through the mosh codec.
inline bool is_codec_box(EffectType type) {
    return type == EffectType::Datamosh ||
           type == EffectType::GenerationLoss ||
           type == EffectType::BitrateStarve;
}

// All blend modes work in the linear working space.
enum class BlendMode : uint32_t {
    Normal = 0,
    Add,
    Multiply,
    Screen,
    Difference,
    Count,
};

// final = mix(input, blend(input, mix(input, fx(input), wet)), opacity)
struct EffectInstance {
    std::string generated_path;
    std::string generated_signature;
    EffectType type = EffectType::RgbSplit;
    uint64_t id = 0;               // stable identity (UI state, mod routes)
    std::vector<float> params;     // one per ParamDesc, same order
    float wet = 1.0f;
    float opacity = 1.0f;
    BlendMode blend = BlendMode::Normal;
    bool bypass = false;
    // Active solo effects bypass other effects in the look.
    bool solo = false;
    uint64_t seed = 0;
    uint64_t group_id = 0;         // 0 = ungrouped; else a look-owned group
    // Only the Text effect reads this string.
    std::string text;
    // Node-canvas position. (0,0) = unplaced. The renderer ignores it.
    float node_x = 0.0f;
    float node_y = 0.0f;
};

// params: 0 = offset in whole frames, 1 = target video|audio|both.
inline int64_t offset_frames(const EffectInstance& fx) {
    return fx.params.empty()
               ? 0
               : static_cast<int64_t>(
                     fx.params[0] < 0.0f ? fx.params[0] - 0.5f
                                         : fx.params[0] + 0.5f);
}
inline bool offset_targets_video(const EffectInstance& fx) {
    const float t = fx.params.size() > 1 ? fx.params[1] : 2.0f;
    return t < 0.5f || t >= 1.5f;
}
inline bool offset_targets_audio(const EffectInstance& fx) {
    const float t = fx.params.size() > 1 ? fx.params[1] : 2.0f;
    return t >= 0.5f;
}

}  // namespace looks::doc
