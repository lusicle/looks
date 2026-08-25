// EffectInstance — one node in a layer's stack.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace looks::doc {

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
    Datamosh,          // Codec-Box family: one box,
    GenerationLoss,    // three named effects (generations 1 = one JPEG)
    BitrateStarve,
    Echo,              // stateful: previous own output (one-frame delay)
    Feedback,
    Glyph,             // per-cell luma -> tile from a glyph atlas
    FilmStock,         // film-stock matrices/curves
    Kaleido,           // wave 2: warp + optics + signal
    Polar,
    Turbulence,        // liquify: curl-noise warp
    Displace,          // displacement by own luma
    LensDistort,
    Fringe,            // lens CA / digicam purple fringe
    Interlace,
    SliceShuffle,      // horizontal band displacement
    PixelStretch,      // freeze a row/col and smear past it
    Composite,         // dot crawl / rainbow shimmer / chroma delay
    Snow,              // analog snow + ghosting
    SyncFail,          // vertical roll + horizontal tear
    Timestamp,         // 7-seg camcorder date/timecode burn
    Oversharpen,       // unsharp-mask halos
    Blur,              // directional / radial / zoom / surface (denoise)
    DustScratches,     // procedural dust flecks + wobbling scratch lines
    LightLeak,         // warm edge-anchored leak blobs
    Anamorphic,        // horizontal streak flare + squeeze
    DirectFlash,       // on-camera flash look
    EdgeDetect,        // Sobel stylized lines
    Kuwahara,          // painterly variance-min filter
    CelShade,          // luma banding + dark edges
    RuttEtra,          // luma-displaced scanlines
    SlitScan,          // rows/cols sampled from a past-frames ring buffer
    CamcorderHud,      // REC dot, battery, counter, safe-frame brackets
    GateMask,          // Super 8 / 16mm / 35mm / scope gate + aspect
    CueMark,           // projector reel-change dots
    ScreenTexture,     // projection screen weave + hotspot
    Voronoi,           // cellular shatter
    ReactionDiffusion, // Gray-Scott sim seeded by the frame (stateful)
    ErrorDiffusion,    // CPU error diffusion (raster kernels + Hilbert)
    Flicker,           // exposure / projector flicker
    FrameHold,         // frame-rate sim: hold at N fps + shutter blend
    Stutter,           // beat-repeat: loop the last N frames when armed
    Contour,           // topo iso-lines from luma
    FlowParticles,     // dissolve + advect along the flow field (stateful)
    Spherize,          // frame wrapped onto a sphere / pinch
    SoftUpscale,       // soft low-res upscale
    ZoomCrunch,        // digital-zoom crunch: crop-zoom + resample
    PixelSort,         // threshold-gated luma sort streaks
    WaveWarp,          // sine / ripple displacement
    CrtSim,            // CRT tube: grille, scanlines, curvature
    Halftone,          // angled dot screens (mono / cmyk) — real print sim
    StarFilter,        // cross-screen diffraction spikes on highlights
    Streak,            // directional smear / directional glow (threshold)
    SplitTone,         // shadow hue vs highlight hue
    CornerSoft,        // radial corner blur + astigmatism (lens character)
    HeadSwitch,        // VHS head-switching noise band at frame bottom
    VhsOsd,            // tape-deck overlay: PLAY / REC / counter
    CamAuto,           // AF hunt + AE pump + AWB drift, one bad camera
    Mosquito,          // flickering DCT ringing hugging hard edges
    BitPlane,          // XOR/AND/OR bit patterns on quantized channels
    BlockShuffle,      // macroblock copy/stamp/swap glitch (GPU-side)
    BufferGlitch,      // stuck columns / accumulating row shear
    CrossHatch,        // multi-angle stroke shading by luma band
    SpliceBump,        // splice: flash + frame jump + dirt (route a trigger)
    FilmSlip,          // projector loses the loop: rolled frame + frame bar
    Emulsion,          // vinegar-syndrome warp, mottle, mold blooms
    TimeDisplace,      // per-pixel playback delay from luma/mask (ring)
    FlowPaint,         // anisotropic Kuwahara along the flow field
    FmSynth,           // video-synth: luma-FM'd scanline carrier / ring mod
    Colorizer,         // gradient map: luma -> 3-stop color ramp
    Solarize,          // Sabattier partial inversion / luma wavefold
    Invert,            // negative (plain / film orange-mask / luma-only)
    Twirl,             // swirl warp: rotation falling off from center
    Tile,              // grid repeat with mirror alternation
    Emboss,            // directional relief convolution
    LensFlare,         // aperture ghost train + halo from clipped lights
    VelocityScan,      // dwell-time rendering: luma brakes sweeping lines
    Lidar,             // point-scan sampling persisting on phosphor
    Anaglyph,          // red/cyan stereo double image (luma depth proxy)
    Photocopy,         // contrast collapse + toner speckle, N generations
    Risograph,         // tone-separated ink layers, misregistered
    WetPlate,          // collodion tintype: ortho response + chemistry
    Glass,             // architectural glass refraction (5 profiles)
    Watercolor,        // washes, edge pooling, pigment granulation
    WireTerrain,       // perspective luma-heightfield wireframe
    Ridgeline,         // stacked occluded luma waveforms (joyplot)
    SlowScan,          // SSTV: beam crawls, replacing the held image
    VectorTrace,       // beam strokes crawling image contours (phosphor)
    ScopeMonitor,      // waveform / parade / vectorscope as aesthetic
    SecurityMux,       // camera-wall grid, per-tile time offsets (ring)
    AudioScope,        // the soundtrack's waveform traced over the frame
    Engraver,          // luma-PM'd fine raster weave (FM engraving)
    BlendNode,         // graph merge: blends the B input over In
    Matte,             // matte maker: luma/key extract + levels (—
                       // masks ARE images; feeds any mask anchor)
    // The PRIMITIVES batch: single-job nodes
    // for operations previously buried inside compound effects.
    Levels,            // in/out black-white points + gamma (tone primitive)
    HueSat,            // hue rotate / saturation / lightness
    ChannelMix,        // per-output source-channel pick (swap / mono)
    Posterize,         // plain level quantization, no dither, no palette
    Threshold,         // soft-knee luma threshold to black & white
    PaletteMap,        // nearest-color palette snap (gb/cga/nes/ttx/duo)
    Dither,            // the ordered-pattern threshold engine, standalone
    Transform,         // mid-chain affine: scale/rotate/offset/flip + edges
    FrameDelay,        // plain N-frame delay from a past-frames ring
    Text,              // SDF TTF text burn-in (EffectInstance::text)
    // v5.7 primitives: the two clean tools the roster only had as
    // deliberately-degraded looks.
    WhiteBalance,      // temperature / tint, linear-light channel gains
    Sharpen,           // clean unsharp mask (Oversharpen is the artifact)
    CornerPin,         // perspective quad warp: offset the four corners
    // AUDIO MODIFIERS: nodes that transform the VOICE and pass the image
    // through untouched (the inverse of every effect above). They ride
    // the same stack/card/link machinery but never reach the image
    // graph - the compiler routes around them and the audio flatten
    // collects them into per-instance DSP op lists. New audio types
    // append INSIDE this span (is_audio_effect is a range test).
    AudioGain,         // linear level
    AudioBitcrush,     // amplitude quantize to N bits
    AudioDownsample,   // sample-and-hold rate crush
    AudioDistortion,   // normalized tanh waveshaper
    AudioDelay,        // feedback echo taps
    AudioFilter,       // windowed-sinc FIR low/high pass
    // OFFSET: a time shim, not an image pass. Wired DIRECTLY onto a
    // source node (media or nested ref) it shifts that source's read
    // by a signed frame count - video, audio, or both per its target
    // selector; wired anywhere else it passes through unchanged. The
    // shift folds into the source's stream key, so fan-out through
    // different offsets decodes separate streams.
    Offset,
    // TRACK PIN: attaches its B input onto the tracked plane of the
    // chain's media (or stabilizes the frame against it). The plane is
    // the effect's own region params solved through the media's .track
    // sidecar; the engine composes the per-frame homography CPU-side.
    TrackPin,
    Count,
};

// Audio-modifier span: image-identity in the graph, DSP ops in the mix.
inline bool is_audio_effect(EffectType type) {
    return type >= EffectType::AudioGain && type <= EffectType::AudioFilter;
}

// True for effects that read their own previous output (engine keeps a
// persistent per-instance target; the one-frame-delay rule, ).
inline bool is_stateful_feedback(EffectType type) {
    return type == EffectType::Echo || type == EffectType::Feedback ||
           type == EffectType::Lidar || type == EffectType::SlowScan ||
           type == EffectType::VectorTrace ||
           type == EffectType::ScopeMonitor;
}

// Second sampled image input reachable as a canvas aux port. The string
// names the port on the card; null = no aux port. A wired aux wins over
// the matte-as-map fallback (Displace / Time Displace); Blend's B is the
// graph merge input.
inline const char* effect_aux_port(EffectType type) {
    switch (type) {
        case EffectType::BlendNode: return "b";
        case EffectType::Displace: return "map";
        case EffectType::TimeDisplace: return "map";
        case EffectType::TrackPin: return "b";
        default: return nullptr;
    }
}

// True for the Codec-Box effects (CPU roundtrip through the mosh codec).
inline bool is_codec_box(EffectType type) {
    return type == EffectType::Datamosh ||
           type == EffectType::GenerationLoss ||
           type == EffectType::BitrateStarve;
}

// Blend modes, shared by per-effect composition and layer compositing
//. All operate in the linear working space.
enum class BlendMode : uint32_t {
    Normal = 0,
    Add,
    Multiply,
    Screen,
    Difference,
    Count,
};

// Type + ordered params + wet/dry + opacity/blend + bypass + seed.
// Param metadata (ranges, labels, defaults) lives in effects.h; params here
// parallel that table.
//
// Composition (canonical, wet is INTERIOR to the blend, opacity outside):
//   final = mix(input, blend(input, mix(input, fx(input), wet)), opacity)
struct EffectInstance {
    EffectType type = EffectType::RgbSplit;
    uint64_t id = 0;               // stable identity (UI state, mod routes)
    std::vector<float> params;     // one per ParamDesc, same order
    float wet = 1.0f;
    float opacity = 1.0f;
    BlendMode blend = BlendMode::Normal;
    bool bypass = false;
    // Solo: when any effect in a stack is soloed, only soloed
    // effects run (bypass still wins for the soloed effect itself).
    bool solo = false;
    uint64_t seed = 0;
    uint64_t group_id = 0;         // 0 = ungrouped; else a Layer group
    // The one string param: only Text reads it. Serialized when
    // non-empty; edited through set_effect_text_command.
    std::string text;
    // Node-canvas position, graph units. Pure UI
    // placement — never read by the renderer. (0,0) = unplaced; the
    // canvas auto-lays-out unplaced nodes once and commits positions.
    float node_x = 0.0f;
    float node_y = 0.0f;
};

// The Offset node's shift in whole frames, and which signal it moves
// (params: 0 = offset, 1 = target selector video|audio|both).
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
