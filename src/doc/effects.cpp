#include "doc/effects.h"

namespace looks::doc {

namespace {

constexpr ParamDesc kRgbSplitParams[] = {
    {"shift_x", "shift x", -64.0f, 64.0f, 6.0f, "%.1f px"},
    {"shift_y", "shift y", -64.0f, 64.0f, 0.0f, "%.1f px"},
};

constexpr ParamDesc kVignetteParams[] = {
    {"amount", "amount", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"radius", "radius", 0.0f, 1.0f, 0.55f, "%.2f"},
    {"softness", "softness", 0.01f, 1.0f, 0.35f, "%.2f"},
};

constexpr ParamDesc kPixelateParams[] = {
    {"block_size", "block size", 1.0f, 128.0f, 12.0f, "%.0f px"},
};

constexpr ParamDesc kGrainParams[] = {
    {"amount", "amount", 0.0f, 1.0f, 0.35f, "%.2f"},
    {"size", "size", 1.0f, 8.0f, 1.5f, "%.1f px"},
    {"color", "color", 0.0f, 1.0f, 0.3f, "%.2f"},
    // Spec §6.5: film grain OR the CCD sensor-noise flavor (shadow-
    // weighted, per-pixel, with faint row banding).
    {"mode", "mode (0=film 1=ccd)", 0.0f, 1.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kJitterParams[] = {
    {"amount", "amount", 0.0f, 48.0f, 6.0f, "%.1f px"},
    {"speed", "speed", 0.1f, 12.0f, 2.0f, "%.1f hz"},
    // 0 gate weave, 1 EIS wobble, 2 VHS tracking, 3 rolling-shutter jello
    // (per-row skew — the phone/digicam wobble).
    {"mode", "mode (3=jello)", 0.0f, 3.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kQuantizeParams[] = {
    {"levels", "levels", 2.0f, 16.0f, 4.0f, "%.0f"},
    // 0 rgb, 1 gray, 2 game boy, 3 cga, 4 nes, 5 teletext, 6 custom
    // duotone (black -> hue a -> hue b -> white)
    {"palette", "palette", 0.0f, 6.0f, 0.0f, "%.0f"},
    // 0 none, 1-3 bayer 2/4/8, 4 white, 5 blue noise, 6 STBN, 7 moire,
    // 8 level cycle (rotate quantization boundaries on the boil clock),
    // 9 RD stipple (Gray-Scott dots as the threshold — stateful),
    // 10 spiral, 11 radial rings, 12 diamond cluster (ordered shapes).
    {"dither", "dither", 0.0f, 12.0f, 3.0f, "%.0f"},
    {"dither_amt", "dither amt", 0.0f, 1.0f, 1.0f, "%.2f"},
    {"boil_hz", "boil rate", 0.0f, 30.0f, 8.0f, "%.0f hz"},
    {"scroll", "pattern scroll", 0.0f, 64.0f, 0.0f, "%.0f px/s"},
    {"warp", "wave warp", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"dissolve", "dissolve", 0.0f, 1.0f, 1.0f, "%.2f"},
    // Shared dither controls (spec §6.2): lock mode (motion-locked pattern
    // advection along flow) + the rotate/scale legs of pattern transform.
    {"lock", "lock (1=motion)", 0.0f, 1.0f, 0.0f, "%.0f"},
    {"pat_rotate", "pattern rotate", -180.0f, 180.0f, 0.0f, "%.0f deg"},
    {"pat_scale", "pattern scale", 0.25f, 4.0f, 1.0f, "%.2f x"},
    {"pal_hue_a", "custom hue a", 0.0f, 1.0f, 0.08f, "%.2f"},
    {"pal_hue_b", "custom hue b", 0.0f, 1.0f, 0.55f, "%.2f"},
};

constexpr ParamDesc kGlowParams[] = {
    {"amount", "amount", 0.0f, 3.0f, 1.0f, "%.2f"},
    {"radius", "radius", 1.0f, 64.0f, 18.0f, "%.0f px"},
    {"threshold", "threshold", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"mode", "mode", 0.0f, 2.0f, 0.0f, "%.0f"},
    // True CCD smear (spec §6.3 CCD mode): clipped highlights bleed a
    // full-height column streak, not just a local bloom.
    {"smear", "ccd smear", 0.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kFlowSmearParams[] = {
    {"amount", "amount", 0.0f, 4.0f, 1.5f, "%.2f"},
    {"falloff", "falloff", 0.0f, 1.0f, 0.5f, "%.2f"},
};

constexpr ParamDesc kMotionExtractParams[] = {
    {"gain", "gain", 1.0f, 32.0f, 8.0f, "%.1f"},
    {"pure_diff", "pure diff", 0.0f, 1.0f, 1.0f, "%.2f"},
};

constexpr ParamDesc kDatamoshParams[] = {
    {"quality", "quality", 1.0f, 100.0f, 50.0f, "%.0f"},
    {"gop", "gop (0=hold)", 0.0f, 120.0f, 0.0f, "%.0f"},
    {"mv_scale", "mv scale", -4.0f, 4.0f, 1.0f, "%.2f"},
    {"mv_random", "mv random", 0.0f, 32.0f, 0.0f, "%.1f px"},
    {"corrupt", "corrupt", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"bloom", "bloom", 0.0f, 8.0f, 0.0f, "%.0f"},
    // Spec §6.3 user ops, previously codec-only: MV rotate, structured
    // byte corruption, and replace-with-custom-field (pan/zoom/swirl).
    {"mv_rotate", "mv rotate", -180.0f, 180.0f, 0.0f, "%.0f deg"},
    {"byte_flips", "byte flips", 0.0f, 64.0f, 0.0f, "%.0f"},
    {"mv_field", "field (0=flow)", 0.0f, 3.0f, 0.0f, "%.0f"},
    {"field_amt", "field amt", -32.0f, 32.0f, 8.0f, "%.0f px"},
};

constexpr ParamDesc kGenerationLossParams[] = {
    {"quality", "quality", 1.0f, 100.0f, 35.0f, "%.0f"},
    {"generations", "generations", 1.0f, 12.0f, 4.0f, "%.0f"},
};

constexpr ParamDesc kJpegBlockingParams[] = {
    {"quality", "quality", 1.0f, 100.0f, 8.0f, "%.0f"},
};

constexpr ParamDesc kBitrateStarveParams[] = {
    {"budget_kb", "budget", 1.0f, 200.0f, 8.0f, "%.0f kb"},
    {"gop", "gop", 1.0f, 120.0f, 30.0f, "%.0f"},
};

constexpr ParamDesc kEchoParams[] = {
    {"decay", "decay", 0.0f, 0.98f, 0.85f, "%.2f"},
    {"mode", "mode", 0.0f, 1.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kFeedbackParams[] = {
    {"amount", "amount", 0.0f, 1.0f, 0.7f, "%.2f"},
    {"zoom", "zoom", 0.8f, 1.2f, 1.03f, "%.3f"},
    {"rotate", "rotate", -0.2f, 0.2f, 0.01f, "%.3f"},
    {"fade", "fade", 0.0f, 0.5f, 0.06f, "%.2f"},
    // Spec §6.1: per-pass color-shift — hue rotation of the fed-back frame.
    {"hue_shift", "hue shift", -60.0f, 60.0f, 0.0f, "%.0f deg"},
};

constexpr ParamDesc kGlyphParams[] = {
    {"cell", "cell size", 2.0f, 32.0f, 8.0f, "%.0f px"},
    // 3 = braille: procedural 2x4 dot-cell ramp (spec §6.6);
    // 4 = teletext: procedural 2x3 block-mosaic sextants.
    {"set", "set (0=dot 1=asc 3=brl 4=ttx)", 0.0f, 4.0f, 1.0f, "%.0f"},
    {"mode", "mode", 0.0f, 2.0f, 1.0f, "%.0f"},
    {"jitter", "jitter", 0.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kColorScienceParams[] = {
    // 12 = three-strip technicolor, 13 = expired stock (spec §6.4),
    // 14 = aerochrome IR false color, 15 = b&w infrared, 16 = instant.
    {"preset", "stock", 0.0f, 16.0f, 1.0f, "%.0f"},
    {"push", "push/pull", -2.0f, 2.0f, 0.0f, "%+.1f st"},
    {"punch", "punch", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"fade", "fade", 0.0f, 1.0f, 0.0f, "%.2f"},
    // Spec §6.4 highlight clip/rolloff: negative hard-clips the top end,
    // positive soft-knee compresses it (filmic shoulder).
    {"highlight", "highlight (-clip +roll)", -1.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kKaleidoParams[] = {
    {"segments", "segments", 2.0f, 16.0f, 6.0f, "%.0f"},
    {"angle", "angle", -3.1416f, 3.1416f, 0.0f, "%.2f"},
    {"center_x", "center x", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"center_y", "center y", 0.0f, 1.0f, 0.5f, "%.2f"},
};

constexpr ParamDesc kPolarParams[] = {
    {"mode", "mode (0=to polar)", 0.0f, 1.0f, 0.0f, "%.0f"},
    {"swirl", "swirl", -3.0f, 3.0f, 0.0f, "%.2f"},
    {"zoom", "zoom", 0.25f, 4.0f, 1.0f, "%.2f"},
};

constexpr ParamDesc kTurbulenceParams[] = {
    {"amount", "amount", 0.0f, 64.0f, 12.0f, "%.0f px"},
    {"scale", "scale", 8.0f, 256.0f, 96.0f, "%.0f px"},
    {"speed", "speed", 0.0f, 4.0f, 0.5f, "%.2f hz"},
};

constexpr ParamDesc kDisplaceParams[] = {
    {"amount", "amount", -64.0f, 64.0f, 16.0f, "%.1f px"},
    {"angle", "angle", -3.1416f, 3.1416f, 0.0f, "%.2f"},
    {"mode", "mode (1=gradient)", 0.0f, 1.0f, 0.0f, "%.0f"},
    // Second-input displacement (spec §6.2): 1 = displace by the
    // referenced mask's grayscale (mask sources cover other layers,
    // external videos, and generators, each with a mini chain — spec §8).
    {"map_mode", "map (1=mask)", 0.0f, 1.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kLensDistortParams[] = {
    {"k1", "barrel", -1.0f, 1.0f, 0.15f, "%.2f"},
    {"k2", "edge", -1.0f, 1.0f, 0.0f, "%.2f"},
    {"zoom", "zoom", 0.5f, 2.0f, 1.0f, "%.2f"},
};

constexpr ParamDesc kFringeParams[] = {
    {"amount", "amount", 0.0f, 16.0f, 3.0f, "%.1f px"},
    {"mode", "mode (1=purple)", 0.0f, 1.0f, 0.0f, "%.0f"},
    {"threshold", "threshold", 0.0f, 1.0f, 0.7f, "%.2f"},
};

constexpr ParamDesc kInterlaceParams[] = {
    {"shift", "comb shift", 0.0f, 16.0f, 4.0f, "%.1f px"},
    {"darken", "line darken", 0.0f, 1.0f, 0.15f, "%.2f"},
    {"mode", "mode (1=lines only)", 0.0f, 1.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kSliceShuffleParams[] = {
    {"slices", "slices", 2.0f, 64.0f, 12.0f, "%.0f"},
    {"amount", "amount", 0.0f, 256.0f, 48.0f, "%.0f px"},
    {"rate", "rate", 0.0f, 30.0f, 4.0f, "%.0f hz"},
    {"prob", "probability", 0.0f, 1.0f, 0.5f, "%.2f"},
};

constexpr ParamDesc kPixelStretchParams[] = {
    {"position", "position", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"mode", "mode (down/up/rt/lt)", 0.0f, 3.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kCompositeParams[] = {
    {"dot_crawl", "dot crawl", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"rainbow", "rainbow", 0.0f, 1.0f, 0.4f, "%.2f"},
    {"chroma_delay", "chroma delay", 0.0f, 16.0f, 4.0f, "%.1f px"},
};

constexpr ParamDesc kSnowParams[] = {
    {"amount", "snow", 0.0f, 1.0f, 0.25f, "%.2f"},
    {"ghost_px", "ghost offset", 0.0f, 64.0f, 24.0f, "%.0f px"},
    {"ghost", "ghost strength", 0.0f, 1.0f, 0.3f, "%.2f"},
};

constexpr ParamDesc kSyncFailParams[] = {
    {"roll", "v roll", 0.0f, 4.0f, 0.6f, "%.2f hz"},
    {"tear", "h tear", 0.0f, 1.0f, 0.35f, "%.2f"},
    {"band", "tear band", 0.02f, 0.5f, 0.15f, "%.2f"},
};

constexpr ParamDesc kTimestampParams[] = {
    {"mode", "mode (0=date 1=tc)", 0.0f, 2.0f, 2.0f, "%.0f"},
    {"size", "size", 1.0f, 6.0f, 3.0f, "%.0f"},
    {"corner", "corner", 0.0f, 3.0f, 3.0f, "%.0f"},
};

constexpr ParamDesc kOversharpenParams[] = {
    {"amount", "amount", 0.0f, 4.0f, 1.5f, "%.2f"},
    {"radius", "radius", 0.5f, 8.0f, 2.0f, "%.1f px"},
};

constexpr ParamDesc kNrMushParams[] = {
    {"amount", "amount", 0.0f, 1.0f, 0.7f, "%.2f"},
    {"radius", "radius", 1.0f, 12.0f, 5.0f, "%.0f px"},
};

constexpr ParamDesc kBlurParams[] = {
    {"amount", "amount", 0.0f, 64.0f, 16.0f, "%.0f px"},
    // Spec §6.5 blur family: 0 directional, 1 spin (arc around center),
    // 2 zoom, 3 gaussian, 4 tilt-shift, 5 bokeh (flat disc aperture,
    // highlights bloom into discs), 6 surface (edge-preserving bilateral).
    {"mode", "mode (5=bokeh 6=surf)", 0.0f, 6.0f, 0.0f, "%.0f"},
    {"angle", "angle", -3.1416f, 3.1416f, 0.0f, "%.2f"},
    {"focus", "tilt focus", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"band", "tilt band", 0.02f, 0.5f, 0.15f, "%.2f"},
};

constexpr ParamDesc kDustScratchesParams[] = {
    {"dust", "dust", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"scratch", "scratches", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"size", "size", 0.5f, 3.0f, 1.0f, "%.1f"},
    // Spec §6.5: gate hairs — curved fibers that cling to the frame edge
    // and wiggle a little between reseeds.
    {"hair", "hair", 0.0f, 1.0f, 0.0f, "%.2f"},
    // Texture-driven damage (spec §6.5): tiles assets/textures/dust.png
    // (procedural grunge fallback when absent) over the frame.
    {"texture", "texture amt", 0.0f, 1.0f, 0.0f, "%.2f"},
    // 0 = fixed plate: every mark seed-locked in place (photograph /
    // print damage). >0 = projected-print boil: dust re-rolls at this
    // rate, scratches at 1/12 of it, gate hairs at 1/22 (24 = the
    // classic per-frame film-dirt flicker).
    {"boil", "boil (0=fixed plate)", 0.0f, 30.0f, 0.0f, "%.1f hz"},
};

constexpr ParamDesc kLightLeakParams[] = {
    {"amount", "amount", 0.0f, 2.0f, 0.8f, "%.2f"},
    {"hue", "hue (0=warm 1=magenta)", 0.0f, 1.0f, 0.2f, "%.2f"},
    {"drift", "drift", 0.0f, 2.0f, 0.25f, "%.2f hz"},
    // Spec §6.3: film-burn reel ends — flickering edge burn-through.
    {"burn", "film burn", 0.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kAnamorphicParams[] = {
    {"streak", "streak", 0.0f, 2.0f, 0.8f, "%.2f"},
    {"threshold", "threshold", 0.0f, 1.0f, 0.75f, "%.2f"},
    {"squeeze", "squeeze", 1.0f, 2.0f, 1.33f, "%.2f"},
    // Spec §6.3: oval-bokeh approximation — vertical elliptical bloom.
    {"bokeh", "oval bokeh", 0.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kDirectFlashParams[] = {
    {"strength", "strength", 0.0f, 2.0f, 0.9f, "%.2f"},
    {"falloff", "falloff", 0.5f, 4.0f, 1.8f, "%.2f"},
    {"cool", "cool tint", 0.0f, 1.0f, 0.35f, "%.2f"},
    // Spec §6.3: optional red-eye — red lift in dark center regions.
    {"red_eye", "red eye", 0.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kEdgeDetectParams[] = {
    {"threshold", "threshold", 0.0f, 1.0f, 0.15f, "%.2f"},
    {"mode", "mode (0=wht 1=blk 2=over)", 0.0f, 2.0f, 0.0f, "%.0f"},
    {"thickness", "thickness", 0.5f, 4.0f, 1.0f, "%.1f px"},
    {"algo", "algo (0=sobel 1=dog)", 0.0f, 1.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kKuwaharaParams[] = {
    {"radius", "radius", 1.0f, 6.0f, 3.0f, "%.0f px"},
};

constexpr ParamDesc kCelShadeParams[] = {
    {"bands", "bands", 2.0f, 8.0f, 4.0f, "%.0f"},
    {"edge", "edge", 0.0f, 1.0f, 0.6f, "%.2f"},
};

constexpr ParamDesc kRuttEtraParams[] = {
    {"spacing", "spacing", 2.0f, 16.0f, 6.0f, "%.0f px"},
    {"amount", "amount", 0.0f, 64.0f, 24.0f, "%.0f px"},
    {"line_w", "line width", 0.5f, 4.0f, 1.5f, "%.1f px"},
    // Scan-processor extras: column rasters, oscillator noise riding the
    // trace, and beam-intensity dot breakup (weak signal beads the line).
    {"mode", "mode (0=rows 1=cols)", 0.0f, 1.0f, 0.0f, "%.0f"},
    {"wiggle", "trace wiggle", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"beam", "dot breakup", 0.0f, 1.0f, 0.0f, "%.2f"},
    // The raster itself crawls across the image on the timeline clock —
    // the animated scan-render look (0 = parked grid).
    {"scroll", "raster scroll", -64.0f, 64.0f, 0.0f, "%.0f px/s"},
};

constexpr ParamDesc kSlitScanParams[] = {
    {"mode", "mode (0=rows 1=cols)", 0.0f, 1.0f, 0.0f, "%.0f"},
    {"depth", "depth", 2.0f, 16.0f, 12.0f, "%.0f frames"},
    {"reverse", "reverse", 0.0f, 1.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kCamcorderHudParams[] = {
    {"size", "size", 1.0f, 6.0f, 3.0f, "%.0f"},
    {"blink_hz", "rec blink", 0.0f, 4.0f, 1.0f, "%.1f hz"},
    {"elements", "elements (0=rec 2=full)", 0.0f, 2.0f, 2.0f, "%.0f"},
};

constexpr ParamDesc kGateMaskParams[] = {
    // 4 = instant print: warm white border, square window, fat bottom.
    {"gauge", "gauge (s8/16/185/239/inst)", 0.0f, 4.0f, 1.0f, "%.0f"},
    {"round", "corner round", 0.0f, 1.0f, 0.35f, "%.2f"},
    {"soft", "softness", 0.0f, 1.0f, 0.15f, "%.2f"},
};

constexpr ParamDesc kCueMarkParams[] = {
    {"period", "period", 2.0f, 60.0f, 12.0f, "%.0f s"},
    {"dwell", "dwell", 1.0f, 12.0f, 4.0f, "%.0f frames"},
    {"mark_size", "size", 0.02f, 0.12f, 0.05f, "%.2f"},
};

constexpr ParamDesc kScreenTextureParams[] = {
    {"amount", "weave", 0.0f, 1.0f, 0.35f, "%.2f"},
    {"weave_px", "weave scale", 2.0f, 12.0f, 4.0f, "%.0f px"},
    {"hotspot", "hotspot", 0.0f, 1.0f, 0.4f, "%.2f"},
};

constexpr ParamDesc kVoronoiParams[] = {
    {"cells", "cells", 4.0f, 128.0f, 24.0f, "%.0f"},
    {"shatter", "shatter", 0.0f, 64.0f, 10.0f, "%.0f px"},
    {"edge", "edge", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"drift", "drift", 0.0f, 2.0f, 0.3f, "%.2f hz"},
    // Spec §6.6: Voronoi cells or the Delaunay-style triangle facets.
    {"style", "style (0=cell 1=tri)", 0.0f, 1.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kReactionDiffusionParams[] = {
    {"feed", "feed", 0.010f, 0.090f, 0.037f, "%.3f"},
    {"kill", "kill", 0.040f, 0.070f, 0.060f, "%.3f"},
    {"steps", "steps/frame", 1.0f, 24.0f, 10.0f, "%.0f"},
    {"inject", "frame inject", 0.0f, 1.0f, 0.35f, "%.2f"},
    {"ink", "ink (0=dark 1=lit)", 0.0f, 1.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kErrorDiffusionParams[] = {
    {"levels", "levels", 2.0f, 8.0f, 2.0f, "%.0f"},
    // Kernel family: 0 Floyd-Steinberg, 1 Atkinson, 2 Jarvis-Judice-Ninke,
    // 3 Stucki, 4 Burkes, 5 Sierra.
    {"mode", "kernel (0=fs..5=sierra)", 0.0f, 5.0f, 0.0f, "%.0f"},
    {"serpentine", "serpentine", 0.0f, 1.0f, 1.0f, "%.0f"},
    {"carry", "temporal carry", 0.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kFlickerParams[] = {
    {"amount", "amount", 0.0f, 1.0f, 0.4f, "%.2f"},
    {"rate", "rate", 1.0f, 48.0f, 24.0f, "%.0f hz"},
    // 2 = color strobe: the flicker alternates hue instead of exposure.
    {"mode", "mode (2=color)", 0.0f, 2.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kFrameHoldParams[] = {
    {"hold_fps", "hold fps", 6.0f, 30.0f, 16.0f, "%.0f"},
    {"blend", "shutter blend", 0.0f, 1.0f, 0.3f, "%.2f"},
};

constexpr ParamDesc kStutterParams[] = {
    {"frames", "loop frames", 2.0f, 16.0f, 4.0f, "%.0f"},
    {"armed", "armed", 0.0f, 1.0f, 0.0f, "%.0f"},
    {"rate", "replay rate", 0.25f, 4.0f, 1.0f, "%.2f"},
};

constexpr ParamDesc kContourParams[] = {
    {"levels", "levels", 3.0f, 24.0f, 10.0f, "%.0f"},
    {"thickness", "thickness", 0.5f, 3.0f, 1.0f, "%.1f"},
    {"mode", "mode (0=wht 1=blk 2=over)", 0.0f, 2.0f, 2.0f, "%.0f"},
};

constexpr ParamDesc kFlowParticlesParams[] = {
    {"advect", "advect", 0.0f, 8.0f, 2.5f, "%.1f"},
    {"dissolve", "dissolve", 0.0f, 1.0f, 0.35f, "%.2f"},
    {"decay", "decay", 0.5f, 0.999f, 0.94f, "%.3f"},
    {"swirl", "swirl", 0.0f, 1.0f, 0.3f, "%.2f"},
};

constexpr ParamDesc kBallParams[] = {
    // Negative amounts extrapolate the spherize inward = pinch.
    {"amount", "amount", -1.0f, 1.0f, 1.0f, "%.2f"},
    {"radius", "radius", 0.2f, 1.5f, 0.95f, "%.2f"},
    {"center_x", "center x", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"center_y", "center y", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"shade", "rim shade", 0.0f, 1.0f, 0.35f, "%.2f"},
    {"backdrop", "keep bg", 0.0f, 1.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kSoftUpscaleParams[] = {
    {"factor", "factor", 2.0f, 16.0f, 4.0f, "%.0f x"},
    {"softness", "softness", 0.0f, 1.0f, 0.5f, "%.2f"},
};

constexpr ParamDesc kZoomCrunchParams[] = {
    {"zoom", "zoom", 1.0f, 8.0f, 2.0f, "%.1f x"},
    {"crunch", "crunch", 0.0f, 1.0f, 0.5f, "%.2f"},
};

constexpr ParamDesc kPixelSortParams[] = {
    {"threshold", "threshold", 0.0f, 1.0f, 0.4f, "%.2f"},
    {"range", "band range", 0.05f, 1.0f, 0.35f, "%.2f"},
    {"length", "length", 8.0f, 256.0f, 96.0f, "%.0f px"},
    {"direction", "dir (0=h 1=v)", 0.0f, 1.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kWaveWarpParams[] = {
    {"amount", "amount", 0.0f, 64.0f, 12.0f, "%.0f px"},
    {"freq", "frequency", 0.5f, 24.0f, 4.0f, "%.1f"},
    {"speed", "speed", 0.0f, 4.0f, 0.5f, "%.2f hz"},
    {"mode", "mode (0=h 1=v 2=rip)", 0.0f, 2.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kCrtSimParams[] = {
    {"curvature", "curvature", 0.0f, 1.0f, 0.35f, "%.2f"},
    {"scanline", "scanlines", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"mask", "grille", 0.0f, 1.0f, 0.4f, "%.2f"},
    {"triad", "triad size", 2.0f, 8.0f, 3.0f, "%.0f px"},
    // LCD panel: flat rectangular RGB stripes + row gaps, no tube
    // curvature or scanline beam — the laptop/monitor screen-door look.
    {"panel", "panel (0=crt 1=lcd)", 0.0f, 1.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kHalftoneParams[] = {
    {"dot_px", "dot size", 2.0f, 32.0f, 8.0f, "%.0f px"},
    {"angle", "screen angle", -90.0f, 90.0f, 15.0f, "%.0f deg"},
    {"mode", "mode (0=ink 1=inv 2=cmyk)", 0.0f, 2.0f, 0.0f, "%.0f"},
    {"gain", "dot gain", 0.5f, 2.0f, 1.0f, "%.2f"},
    {"soft", "softness", 0.0f, 1.0f, 0.15f, "%.2f"},
    // Screen shapes: dots, line screen (engraving/banknote — thickness
    // carries tone), or a spiral screen wound from the frame center.
    {"shape", "screen (0=dot 1=line 2=spiral)", 0.0f, 2.0f, 0.0f, "%.0f"},
    // Engraving wave: lines bow around image features (luma displaces
    // the screen coordinate before thresholding).
    {"wave", "engrave wave", 0.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kStarFilterParams[] = {
    {"points", "points", 2.0f, 8.0f, 4.0f, "%.0f"},
    {"length", "length", 8.0f, 256.0f, 90.0f, "%.0f px"},
    {"threshold", "threshold", 0.0f, 1.0f, 0.8f, "%.2f"},
    {"angle", "angle", -90.0f, 90.0f, 15.0f, "%.0f deg"},
};

constexpr ParamDesc kStreakParams[] = {
    {"length", "length", 0.0f, 256.0f, 80.0f, "%.0f px"},
    {"angle", "angle", -3.1416f, 3.1416f, 0.0f, "%.2f"},
    {"decay", "decay", 0.5f, 0.99f, 0.9f, "%.2f"},
    {"threshold", "threshold (0=smear)", 0.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kSplitToneParams[] = {
    {"sh_hue", "shadow hue", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"sh_amt", "shadow amt", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"hi_hue", "highlight hue", 0.0f, 1.0f, 0.12f, "%.2f"},
    {"hi_amt", "highlight amt", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"balance", "balance", -1.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kCornerSoftParams[] = {
    {"amount", "amount", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"start", "start radius", 0.0f, 1.0f, 0.4f, "%.2f"},
    {"astig", "astigmatism", 0.0f, 1.0f, 0.3f, "%.2f"},
};

constexpr ParamDesc kHeadSwitchParams[] = {
    {"band_px", "band height", 4.0f, 64.0f, 18.0f, "%.0f px"},
    {"shift_px", "h shift", 0.0f, 64.0f, 14.0f, "%.0f px"},
    {"noise", "noise", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"wobble", "wobble", 0.0f, 10.0f, 2.0f, "%.1f hz"},
};

constexpr ParamDesc kVhsOsdParams[] = {
    {"mode", "mode (0=play 1=rec 2=pse 3=ff)", 0.0f, 3.0f, 0.0f, "%.0f"},
    {"size", "size", 1.0f, 6.0f, 3.0f, "%.0f"},
    {"counter", "counter", 0.0f, 1.0f, 1.0f, "%.0f"},
};

constexpr ParamDesc kCamAutoParams[] = {
    {"af", "af hunt", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"ae", "ae pump", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"awb", "awb drift", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"rate", "rate", 0.1f, 4.0f, 0.6f, "%.1f hz"},
};

constexpr ParamDesc kMosquitoParams[] = {
    {"amount", "amount", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"radius", "radius", 1.0f, 6.0f, 3.0f, "%.0f px"},
    {"rate", "flicker", 0.0f, 30.0f, 15.0f, "%.0f hz"},
};

constexpr ParamDesc kBitPlaneParams[] = {
    {"op", "op (0=xor 1=and 2=or)", 0.0f, 2.0f, 0.0f, "%.0f"},
    {"value", "pattern", 1.0f, 255.0f, 32.0f, "%.0f"},
    {"channels", "ch (0=rgb 1=r 2=g 3=b)", 0.0f, 3.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kBlockShuffleParams[] = {
    {"block_px", "block", 4.0f, 128.0f, 32.0f, "%.0f px"},
    {"amount", "amount", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"spread", "spread", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"rate", "rate", 0.0f, 30.0f, 6.0f, "%.0f hz"},
};

constexpr ParamDesc kBufferGlitchParams[] = {
    {"mode", "mode (0=cols 1=shear 2=both)", 0.0f, 2.0f, 2.0f, "%.0f"},
    {"amount", "amount", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"density", "density", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"rate", "rate", 0.0f, 30.0f, 4.0f, "%.0f hz"},
};

constexpr ParamDesc kCrossHatchParams[] = {
    {"spacing", "spacing", 3.0f, 24.0f, 7.0f, "%.0f px"},
    {"layers", "hatch layers", 1.0f, 4.0f, 3.0f, "%.0f"},
    {"ink", "ink", 0.0f, 1.0f, 0.85f, "%.2f"},
    {"wobble", "line wobble", 0.0f, 1.0f, 0.3f, "%.2f"},
};

constexpr ParamDesc kSpliceBumpParams[] = {
    // Route the scene-cut (or beat) trigger onto `bump` in the mod matrix
    // and every cut becomes a splice event.
    {"bump", "bump (route a trigger)", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"jump_px", "frame jump", 0.0f, 128.0f, 48.0f, "%.0f px"},
    {"flash", "flash", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"dirt", "dirt burst", 0.0f, 1.0f, 0.7f, "%.2f"},
};

constexpr ParamDesc kFilmSlipParams[] = {
    {"slip", "slip", 0.0f, 1.0f, 0.25f, "%.2f"},
    {"speed", "roll speed", 0.0f, 4.0f, 0.0f, "%.1f hz"},
    {"blend", "double expose", 0.0f, 1.0f, 0.4f, "%.2f"},
};

constexpr ParamDesc kEmulsionParams[] = {
    {"warp", "warp", 0.0f, 1.0f, 0.4f, "%.2f"},
    {"mottle", "mottle", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"mold", "mold", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"scale", "scale", 0.5f, 4.0f, 1.5f, "%.1f"},
};

constexpr ParamDesc kTimeDisplaceParams[] = {
    {"range", "range", 2.0f, 16.0f, 12.0f, "%.0f frames"},
    {"mode", "mode (0=dark lags 2=mask)", 0.0f, 2.0f, 0.0f, "%.0f"},
    {"smooth", "band dither", 0.0f, 1.0f, 0.3f, "%.2f"},
};

constexpr ParamDesc kFlowPaintParams[] = {
    {"radius", "brush radius", 2.0f, 14.0f, 6.0f, "%.0f px"},
    {"aniso", "stroke stretch", 0.0f, 1.0f, 0.7f, "%.2f"},
    {"boost", "motion boost", 0.0f, 2.0f, 1.0f, "%.1f"},
};

constexpr ParamDesc kFmSynthParams[] = {
    {"freq", "carrier freq", 1.0f, 64.0f, 12.0f, "%.0f"},
    {"fm", "freq mod (luma)", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"pm", "phase mod (luma)", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"depth", "depth", 0.0f, 64.0f, 16.0f, "%.0f px"},
    {"speed", "speed", 0.0f, 4.0f, 0.5f, "%.2f hz"},
    // 0/1: scanline warp along x/y; 2: ring mod (multiply by carrier).
    {"mode", "mode (0=h 1=v 2=ring)", 0.0f, 2.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kColorizerParams[] = {
    {"sh_hue", "shadow hue", 0.0f, 1.0f, 0.66f, "%.2f"},
    {"mid_hue", "mid hue", 0.0f, 1.0f, 0.88f, "%.2f"},
    {"hi_hue", "highlight hue", 0.0f, 1.0f, 0.12f, "%.2f"},
    {"sat", "saturation", 0.0f, 1.0f, 0.85f, "%.2f"},
};

constexpr ParamDesc kSolarizeParams[] = {
    {"threshold", "threshold", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"folds", "folds", 1.0f, 4.0f, 1.0f, "%.0f"},
    {"smooth", "knee", 0.0f, 1.0f, 0.2f, "%.2f"},
};

constexpr ParamDesc kInvertParams[] = {
    // 1 = film negative: inversion through the orange print mask.
    {"mode", "mode (0=inv 1=neg 2=luma)", 0.0f, 2.0f, 0.0f, "%.0f"},
    {"amount", "amount", 0.0f, 1.0f, 1.0f, "%.2f"},
};

constexpr ParamDesc kTwirlParams[] = {
    {"angle", "angle", -720.0f, 720.0f, 180.0f, "%.0f deg"},
    {"radius", "radius", 0.1f, 1.5f, 0.8f, "%.2f"},
    {"center_x", "center x", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"center_y", "center y", 0.0f, 1.0f, 0.5f, "%.2f"},
};

constexpr ParamDesc kTileParams[] = {
    {"cols", "columns", 1.0f, 8.0f, 2.0f, "%.0f"},
    {"rows_n", "rows", 1.0f, 8.0f, 2.0f, "%.0f"},
    {"mirror", "mirror", 0.0f, 1.0f, 1.0f, "%.0f"},
};

constexpr ParamDesc kEmbossParams[] = {
    {"strength", "strength", 0.0f, 4.0f, 1.5f, "%.1f"},
    {"angle", "light angle", -3.1416f, 3.1416f, -2.356f, "%.2f"},
    // 0 = gray relief; 1 = colored relief added over the image.
    {"mode", "mode (0=gray 1=color)", 0.0f, 1.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kLensFlareParams[] = {
    {"threshold", "threshold", 0.0f, 1.0f, 0.8f, "%.2f"},
    {"ghosts", "ghosts", 1.0f, 6.0f, 4.0f, "%.0f"},
    {"spread", "spread", 0.2f, 2.0f, 1.0f, "%.2f"},
    {"halo", "halo", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"tint", "rainbow tint", 0.0f, 1.0f, 0.6f, "%.2f"},
};

constexpr ParamDesc kLidarParams[] = {
    // Point-scan sampling (the lidar / point-cloud look): seeded sample
    // dots stamp the source onto a persisting phosphor canvas. Scatter
    // samples everywhere; spin sweeps a rotating beam from center; sweep
    // marches a raster column across the frame.
    {"mode", "mode (0=scatter 1=spin 2=swp)", 0.0f, 2.0f, 1.0f, "%.0f"},
    {"amount", "density", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"dot_px", "dot size", 1.0f, 8.0f, 2.0f, "%.1f px"},
    {"persist", "persistence", 0.0f, 0.98f, 0.9f, "%.2f"},
    {"rate", "sample rate", 1.0f, 60.0f, 30.0f, "%.0f hz"},
    {"bias", "luma bias", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"spin", "spin/sweep rate", 0.05f, 2.0f, 0.25f, "%.2f hz"},
    {"color", "source color", 0.0f, 1.0f, 1.0f, "%.2f"},
};

constexpr ParamDesc kVelocityScanParams[] = {
    // Velocity-modulated scanning (dwell-time rendering): sweeping line
    // fronts brake over brightness, so the image emerges as accumulated
    // dwell on a phosphor canvas. Stateful (front field + canvas).
    {"mode", "mode (0=cols 1=rows)", 0.0f, 1.0f, 0.0f, "%.0f"},
    {"speed", "sweep speed", 20.0f, 600.0f, 240.0f, "%.0f px/s"},
    {"stick", "stickiness", 0.0f, 1.0f, 0.75f, "%.2f"},
    {"persist", "persistence", 0.0f, 0.98f, 0.92f, "%.2f"},
    {"rate", "spawn rate", 0.5f, 30.0f, 6.0f, "%.1f hz"},
    {"beam_w", "beam width", 0.5f, 4.0f, 1.5f, "%.1f px"},
    {"wiggle", "shear wiggle", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"color", "source color", 0.0f, 1.0f, 1.0f, "%.2f"},
};

constexpr ParamDesc kSlowScanParams[] = {
    // SSTV / slow-scan TV: a beam crawls the frame replacing the held
    // previous image line by line; one full frame takes `period` seconds.
    {"period", "scan period", 0.5f, 20.0f, 6.0f, "%.1f s"},
    {"mode", "mode (0=rows 1=cols)", 0.0f, 1.0f, 0.0f, "%.0f"},
    {"beam", "beam glow", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"noise", "sync noise", 0.0f, 1.0f, 0.4f, "%.2f"},
    {"dim", "dim unscanned", 0.0f, 1.0f, 0.15f, "%.2f"},
};

constexpr ParamDesc kVectorTraceParams[] = {
    // Beam strokes crawling along image contours over phosphor — the
    // image perpetually being hand-drawn in outline.
    {"density", "density", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"len_px", "stroke length", 6.0f, 40.0f, 22.0f, "%.0f px"},
    {"rate", "crawl rate", 0.5f, 20.0f, 5.0f, "%.1f hz"},
    {"persist", "persistence", 0.0f, 0.98f, 0.9f, "%.2f"},
    {"beam_w", "beam width", 0.5f, 3.0f, 1.2f, "%.1f px"},
    {"edge", "edge affinity", 0.0f, 1.0f, 0.7f, "%.2f"},
    {"color", "source color", 0.0f, 1.0f, 0.6f, "%.2f"},
};

constexpr ParamDesc kScopeParams[] = {
    // Broadcast scopes as aesthetic: luma waveform, RGB parade, or the
    // chroma vectorscope, dots accumulating on decaying phosphor.
    {"mode", "mode (0=wave 1=parade 2=vec)", 0.0f, 2.0f, 0.0f, "%.0f"},
    {"gain", "gain", 0.5f, 2.0f, 1.0f, "%.2f"},
    {"glow", "trace glow", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"persist", "persistence", 0.0f, 0.98f, 0.85f, "%.2f"},
    {"graticule", "graticule", 0.0f, 1.0f, 0.4f, "%.2f"},
    {"color", "source color", 0.0f, 1.0f, 0.3f, "%.2f"},
};

constexpr ParamDesc kSecurityMuxParams[] = {
    // Camera-wall multiplexer: a cols x rows monitor grid, every tile
    // replaying the frame at its own seeded delay from the history ring.
    {"cols", "columns", 1.0f, 4.0f, 3.0f, "%.0f"},
    {"rows_n", "rows", 1.0f, 4.0f, 3.0f, "%.0f"},
    {"spread", "time spread", 0.0f, 1.0f, 1.0f, "%.2f"},
    {"mono", "mono cctv", 0.0f, 1.0f, 0.7f, "%.2f"},
    {"osd", "osd", 0.0f, 1.0f, 0.8f, "%.2f"},
};

constexpr ParamDesc kAudioScopeParams[] = {
    // The soundtrack itself traced over the frame — the engine supplies a
    // per-frame min/max waveform strip sampled from the PCM sidecar.
    {"window", "window", 0.05f, 2.0f, 0.5f, "%.2f s"},
    {"mode", "mode (0=fill 1=mirror 2=line)", 0.0f, 2.0f, 0.0f, "%.0f"},
    {"thick", "edge", 1.0f, 6.0f, 2.0f, "%.0f px"},
    {"height", "height", 0.05f, 1.0f, 0.35f, "%.2f"},
    {"pos_y", "position", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"glow", "glow", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"color", "source color", 0.0f, 1.0f, 0.2f, "%.2f"},
};

constexpr ParamDesc kModulateParams[] = {
    // True frequency modulation: luma modulates the INSTANTANEOUS
    // FREQUENCY of the raster — phase accumulates across the frame at
    // omega + distortion * luma, and traces sit at integer phase
    // crossings. Bright regions pack lines dense, dark regions spread
    // them; animate speed and the lines travel, crawling slowly where
    // frequency is high (stuck in the bright areas) and zipping through
    // the dark. Lowpass is a 1-pole IIR on the modulator.
    {"omega", "base lines", 2.0f, 200.0f, 24.0f, "%.0f"},
    // EXTRA frequency multiplier at full density: 0 = off, 0.5 = braked
    // to 1.5x, continuous from zero. Negative inverts the signal.
    {"distortion", "distortion", -16.0f, 16.0f, 8.0f, "%.1f x"},
    {"lowpass", "lowpass", 0.0f, 1.0f, 0.15f, "%.2f"},
    // Sign = direction (the march integrates from the spawn edge).
    {"speed", "travel speed", -10.0f, 10.0f, 0.3f, "%.2f hz"},
    {"mode", "mode (0=h 1=v)", 0.0f, 1.0f, 0.0f, "%.0f"},
    {"line_w", "line width", 0.5f, 3.0f, 1.0f, "%.1f px"},
    {"channels", "channels", 1.0f, 4.0f, 1.0f, "%.0f"},
    {"spread", "channel spread", 0.0f, 1.0f, 0.3f, "%.2f"},
    // 0 = one luma signal, white traces; 1 = R/G/B modulated as three
    // separate signals (GenerateMe fm.pde style) — lines split chromatic
    // wherever the channels disagree.
    {"color", "rgb split", 0.0f, 1.0f, 0.0f, "%.2f"},
    // Response curve: 0 = linear (every shade slows the lines
    // proportionally — subtle darks included), higher bends the
    // response toward the brights. Never a dead zone.
    {"threshold", "response curve", 0.0f, 1.0f, 0.35f, "%.2f"},
};

constexpr ParamDesc kAnaglyphParams[] = {
    // Red/cyan stereo double image; disparity rides a luma depth proxy so
    // bright (near) subjects pop harder than the shadows behind them.
    {"depth", "disparity", 0.0f, 30.0f, 10.0f, "%.0f px"},
    {"pop", "luma pop", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"mode", "mode (0=r/c 1=r/b 2=g/m)", 0.0f, 2.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kPhotocopyParams[] = {
    // Copy-of-a-copy: each generation collapses the tones harder around
    // the drum threshold; roller streaks wobble that threshold by column.
    {"contrast", "contrast collapse", 0.0f, 1.0f, 0.65f, "%.2f"},
    {"generations", "generations", 1.0f, 8.0f, 3.0f, "%.0f"},
    {"toner", "toner speckle", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"streaks", "roller streaks", 0.0f, 1.0f, 0.4f, "%.2f"},
};

constexpr ParamDesc kRisographParams[] = {
    // Tone-separated ink layers over warm paper, each layer misregistered
    // its own seeded direction — the community-print-shop look.
    {"inks", "inks", 1.0f, 3.0f, 2.0f, "%.0f"},
    {"hue1", "ink 1 hue", 0.0f, 1.0f, 0.55f, "%.2f"},
    {"hue2", "ink 2 hue", 0.0f, 1.0f, 0.93f, "%.2f"},
    {"misreg", "misregistration", 0.0f, 12.0f, 4.0f, "%.0f px"},
    {"grain", "ink grain", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"paper", "paper warmth", 0.0f, 1.0f, 0.3f, "%.2f"},
};

constexpr ParamDesc kWetPlateParams[] = {
    // Collodion tintype: blue-sensitive emulsion (skies blow out, reds go
    // black), silver-to-sepia tone, pour marks creeping in from the edges.
    {"ortho", "ortho response", 0.0f, 1.0f, 0.8f, "%.2f"},
    {"tone", "tone (silver->sepia)", 0.0f, 1.0f, 0.25f, "%.2f"},
    {"plate", "plate chemistry", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"fog", "fog", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"vign", "plate vignette", 0.0f, 1.0f, 0.5f, "%.2f"},
};

constexpr ParamDesc kReededGlassParams[] = {
    // Fluted bathroom-window glass: each flute compresses its slice of the
    // image toward its center, blurring and darkening at the boundaries.
    {"flutes", "flute width", 8.0f, 160.0f, 48.0f, "%.0f px"},
    {"refract", "refraction", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"soften", "soften", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"mode", "mode (0=vert 1=horiz)", 0.0f, 1.0f, 0.0f, "%.0f"},
};

constexpr ParamDesc kWatercolorParams[] = {
    // Transparent washes: edge-stopped bleed, pigment pooling darkening
    // the wash boundaries, granulation settling into the paper tooth.
    {"bleed", "bleed", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"pool", "edge pooling", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"granulation", "granulation", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"paper", "paper", 0.0f, 1.0f, 0.4f, "%.2f"},
};

constexpr ParamDesc kWireTerrainParams[] = {
    // Luma heightfield as a receding perspective wireframe — the "3D
    // Rutt-Etra". Hidden-line removal via per-column silhouette tracking.
    {"rows_n", "rows", 16.0f, 120.0f, 48.0f, "%.0f"},
    {"amount", "height", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"pitch", "pitch", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"horizon", "horizon", 0.0f, 0.8f, 0.35f, "%.2f"},
    {"line_w", "line width", 0.5f, 3.0f, 1.2f, "%.1f px"},
    {"grid", "cross grid", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"color", "source color", 0.0f, 1.0f, 0.4f, "%.2f"},
};

constexpr ParamDesc kRidgelineParams[] = {
    // Stacked occluded luma waveforms — the Unknown Pleasures joyplot.
    {"rows_n", "rows", 12.0f, 80.0f, 32.0f, "%.0f"},
    {"amount", "height", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"line_w", "line width", 0.5f, 3.0f, 1.2f, "%.1f px"},
    {"soften", "soften", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"fill", "fill", 0.0f, 1.0f, 0.9f, "%.2f"},
    {"color", "source color", 0.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr EffectInfo kEffectInfos[] = {
    {"rgb_split", "RGB Split", kRgbSplitParams, 2, FxCategory::Signal},
    {"vignette", "Vignette", kVignetteParams, 3, FxCategory::Optics},
    {"pixelate", "Pixelate", kPixelateParams, 1, FxCategory::Texture},
    {"grain", "Noise", kGrainParams, 4, FxCategory::Texture},
    {"jitter", "Jitter", kJitterParams, 3, FxCategory::Time},
    {"quantize", "Quantizer", kQuantizeParams, 13, FxCategory::Color},
    {"glow", "Glow", kGlowParams, 5, FxCategory::Optics},
    {"flow_smear", "Flow Smear", kFlowSmearParams, 2, FxCategory::Time},
    {"motion_extract", "Motion Extract", kMotionExtractParams, 2,
     FxCategory::Time},
    {"datamosh", "Datamosh", kDatamoshParams, 10, FxCategory::Signal},
    {"generation_loss", "Generation Loss", kGenerationLossParams, 2,
     FxCategory::Signal},
    {"jpeg_blocking", "JPEG Blocking", kJpegBlockingParams, 1,
     FxCategory::Signal},
    {"bitrate_starve", "Bitrate Starve", kBitrateStarveParams, 2,
     FxCategory::Signal},
    {"echo", "Echo Trails", kEchoParams, 2, FxCategory::Time},
    {"feedback", "Feedback", kFeedbackParams, 5, FxCategory::Time},
    {"glyph", "Glyph", kGlyphParams, 4, FxCategory::Mosaic},
    {"colorscience", "ColorScience", kColorScienceParams, 5,
     FxCategory::Color},
    {"kaleido", "Kaleidoscope", kKaleidoParams, 4, FxCategory::Warp},
    {"polar", "Polar Coords", kPolarParams, 3, FxCategory::Warp},
    {"turbulence", "Liquify", kTurbulenceParams, 3, FxCategory::Warp},
    {"displace", "Displace", kDisplaceParams, 4, FxCategory::Warp},
    {"lens_distort", "Lens Distort", kLensDistortParams, 3,
     FxCategory::Optics},
    {"fringe", "Fringing", kFringeParams, 3, FxCategory::Optics},
    {"interlace", "Interlace", kInterlaceParams, 3, FxCategory::Signal},
    {"slice_shuffle", "Slice Shuffle", kSliceShuffleParams, 4,
     FxCategory::Signal},
    {"pixel_stretch", "Pixel Stretch", kPixelStretchParams, 2,
     FxCategory::Warp},
    {"composite_artifacts", "Composite", kCompositeParams, 3,
     FxCategory::Signal},
    {"analog_snow", "Analog Snow", kSnowParams, 3, FxCategory::Signal},
    {"sync_fail", "Sync Failure", kSyncFailParams, 3, FxCategory::Signal},
    {"timestamp", "Timestamp", kTimestampParams, 3, FxCategory::Overlay},
    {"oversharpen", "Oversharpen", kOversharpenParams, 2,
     FxCategory::Texture},
    {"nr_mush", "NR Mush", kNrMushParams, 2, FxCategory::Texture},
    {"blur", "Blur", kBlurParams, 5, FxCategory::Texture},
    {"dust_scratches", "Dust & Scratches", kDustScratchesParams, 6,
     FxCategory::Texture},
    {"light_leak", "Light Leak", kLightLeakParams, 4, FxCategory::Optics},
    {"anamorphic", "Anamorphic", kAnamorphicParams, 4, FxCategory::Optics},
    {"direct_flash", "Direct Flash", kDirectFlashParams, 4,
     FxCategory::Optics},
    {"edge_detect", "Edge Detect", kEdgeDetectParams, 4, FxCategory::Mosaic},
    {"kuwahara", "Kuwahara", kKuwaharaParams, 1, FxCategory::Mosaic},
    {"cel_shade", "Cel Shade", kCelShadeParams, 2, FxCategory::Mosaic},
    {"rutt_etra", "Rutt-Etra", kRuttEtraParams, 7, FxCategory::Mosaic},
    {"slit_scan", "Slit-Scan", kSlitScanParams, 3, FxCategory::Time},
    {"camcorder_hud", "Camcorder HUD", kCamcorderHudParams, 3,
     FxCategory::Overlay},
    {"gate_mask", "Gate Mask", kGateMaskParams, 3, FxCategory::Overlay},
    {"cue_mark", "Cue Marks", kCueMarkParams, 3, FxCategory::Overlay},
    {"screen_texture", "Screen Texture", kScreenTextureParams, 3,
     FxCategory::Overlay},
    {"voronoi", "Voronoi Shatter", kVoronoiParams, 5, FxCategory::Mosaic},
    {"reaction_diffusion", "Reaction-Diffusion", kReactionDiffusionParams, 5,
     FxCategory::Mosaic},
    {"error_diffusion", "Error Diffusion", kErrorDiffusionParams, 4,
     FxCategory::Color},
    {"flicker", "Flicker", kFlickerParams, 3, FxCategory::Time},
    {"frame_hold", "Frame Rate", kFrameHoldParams, 2, FxCategory::Time},
    {"stutter", "Stutter", kStutterParams, 3, FxCategory::Time},
    {"contour", "Contour Lines", kContourParams, 3, FxCategory::Mosaic},
    {"flow_particles", "Flow Particles", kFlowParticlesParams, 4,
     FxCategory::Mosaic},
    {"ball", "Ball", kBallParams, 6, FxCategory::Warp},
    {"soft_upscale", "Soft Upscale", kSoftUpscaleParams, 2,
     FxCategory::Texture},
    {"zoom_crunch", "Zoom Crunch", kZoomCrunchParams, 2,
     FxCategory::Texture},
    {"pixel_sort", "Pixel Sort", kPixelSortParams, 4, FxCategory::Signal},
    {"wave_warp", "Wave Warp", kWaveWarpParams, 4, FxCategory::Warp},
    {"crt_sim", "CRT Tube", kCrtSimParams, 5, FxCategory::Signal},
    {"halftone", "Halftone", kHalftoneParams, 7, FxCategory::Mosaic},
    {"star_filter", "Star Filter", kStarFilterParams, 4,
     FxCategory::Optics},
    {"streak", "Streak", kStreakParams, 4, FxCategory::Optics},
    {"split_tone", "Split Tone", kSplitToneParams, 5, FxCategory::Color},
    {"corner_soft", "Corner Soft", kCornerSoftParams, 3,
     FxCategory::Optics},
    {"head_switch", "Head Switch", kHeadSwitchParams, 4,
     FxCategory::Signal},
    {"vhs_osd", "VHS OSD", kVhsOsdParams, 3, FxCategory::Overlay},
    {"cam_auto", "Auto Camera", kCamAutoParams, 4, FxCategory::Optics},
    {"mosquito", "Mosquito Noise", kMosquitoParams, 3, FxCategory::Signal},
    {"bit_plane", "Bit Plane", kBitPlaneParams, 3, FxCategory::Signal},
    {"block_shuffle", "Block Shuffle", kBlockShuffleParams, 4,
     FxCategory::Signal},
    {"buffer_glitch", "Buffer Glitch", kBufferGlitchParams, 4,
     FxCategory::Signal},
    {"cross_hatch", "Cross-Hatch", kCrossHatchParams, 4,
     FxCategory::Mosaic},
    {"splice_bump", "Splice Bump", kSpliceBumpParams, 4, FxCategory::Time},
    {"film_slip", "Film Slip", kFilmSlipParams, 3, FxCategory::Time},
    {"emulsion", "Emulsion", kEmulsionParams, 4, FxCategory::Texture},
    {"time_displace", "Time Displace", kTimeDisplaceParams, 3,
     FxCategory::Time},
    {"flow_paint", "Flow Paint", kFlowPaintParams, 3, FxCategory::Mosaic},
    {"fm_synth", "FM Synth", kFmSynthParams, 6, FxCategory::Warp},
    {"colorizer", "Colorizer", kColorizerParams, 4, FxCategory::Color},
    {"solarize", "Solarize", kSolarizeParams, 3, FxCategory::Color},
    {"invert", "Invert", kInvertParams, 2, FxCategory::Color},
    {"twirl", "Twirl", kTwirlParams, 4, FxCategory::Warp},
    {"tile", "Tile", kTileParams, 3, FxCategory::Mosaic},
    {"emboss", "Emboss", kEmbossParams, 3, FxCategory::Texture},
    {"lens_flare", "Lens Flare", kLensFlareParams, 5, FxCategory::Optics},
    {"velocity_scan", "Velocity Scan", kVelocityScanParams, 8,
     FxCategory::Mosaic},
    {"lidar", "Lidar", kLidarParams, 8, FxCategory::Mosaic},
    {"anaglyph", "Anaglyph 3D", kAnaglyphParams, 3, FxCategory::Optics},
    {"photocopy", "Photocopy", kPhotocopyParams, 4, FxCategory::Texture},
    {"risograph", "Risograph", kRisographParams, 6, FxCategory::Color},
    {"wet_plate", "Wet Plate", kWetPlateParams, 5, FxCategory::Texture},
    {"reeded_glass", "Reeded Glass", kReededGlassParams, 4,
     FxCategory::Warp},
    {"watercolor", "Watercolor", kWatercolorParams, 4, FxCategory::Mosaic},
    {"wire_terrain", "Wire Terrain", kWireTerrainParams, 7,
     FxCategory::Mosaic},
    {"ridgeline", "Ridgeline", kRidgelineParams, 6, FxCategory::Mosaic},
    {"slow_scan", "Slow Scan", kSlowScanParams, 5, FxCategory::Time},
    {"vector_trace", "Vector Trace", kVectorTraceParams, 7,
     FxCategory::Mosaic},
    {"scope", "Scope Monitor", kScopeParams, 6, FxCategory::Signal},
    {"security_mux", "Security Mux", kSecurityMuxParams, 5,
     FxCategory::Time},
    {"audio_scope", "Audio Scope", kAudioScopeParams, 7,
     FxCategory::Overlay},
    {"modulate", "Modulation", kModulateParams, 10, FxCategory::Mosaic},
};
static_assert(sizeof(kEffectInfos) / sizeof(kEffectInfos[0]) ==
              static_cast<size_t>(EffectType::Count));

}  // namespace

const char* fx_category_label(FxCategory category) {
    static const char* kLabels[] = {
        "time & motion",  "warp & displace",    "optics & light",
        "color & tone",   "texture & detail",   "mosaic & structure",
        "signal & codec", "frame & overlay"};
    const auto i = static_cast<size_t>(category);
    return i < static_cast<size_t>(FxCategory::Count) ? kLabels[i] : "?";
}

const EffectInfo& effect_info(EffectType type) {
    return kEffectInfos[static_cast<uint32_t>(type)];
}

EffectInstance make_effect(Document& doc, EffectType type) {
    const EffectInfo& info = effect_info(type);
    EffectInstance fx;
    fx.type = type;
    fx.id = doc.next_effect_id++;
    fx.params.reserve(info.param_count);
    for (uint32_t i = 0; i < info.param_count; ++i)
        fx.params.push_back(info.params[i].default_value);
    return fx;
}

bool effect_uses_history(EffectType type) {
    switch (type) {
        case EffectType::Echo:
        case EffectType::Feedback:
        case EffectType::SlitScan:
        case EffectType::Stutter:
        case EffectType::FrameHold:
        case EffectType::ReactionDiffusion:
        case EffectType::ErrorDiffusion:
        case EffectType::Datamosh:
        case EffectType::GenerationLoss:
        case EffectType::JpegBlocking:
        case EffectType::BitrateStarve:
        case EffectType::FlowSmear:
        case EffectType::MotionExtract:
        case EffectType::FlowParticles:
        case EffectType::TimeDisplace:   // past-frames ring
        case EffectType::FlowPaint:      // flow = previous-frame luma
        case EffectType::VelocityScan:   // front field + phosphor canvas
        case EffectType::Lidar:          // phosphor canvas
        case EffectType::SlowScan:       // held-image canvas
        case EffectType::VectorTrace:    // phosphor canvas
        case EffectType::ScopeMonitor:   // phosphor canvas
        case EffectType::SecurityMux:    // past-frames ring
            return true;
        default:
            return false;
    }
}

namespace {

// Per-instance history check: type-based, plus the quantizer's RD-stipple
// dither mode (9), whose Gray-Scott state accumulates across frames.
bool instance_uses_history(const EffectInstance& fx) {
    if (effect_uses_history(fx.type)) return true;
    return fx.type == EffectType::Quantize && fx.params.size() > 2 &&
           fx.params[2] >= 8.5f;
}

}  // namespace

bool document_uses_history(const Document& doc) {
    for (const Layer& layer : doc.layers)
        for (const EffectInstance& fx : layer.stack)
            if (!fx.bypass && instance_uses_history(fx)) return true;
    for (const Mask& mask : doc.masks)
        for (const EffectInstance& fx : mask.chain)
            if (!fx.bypass && instance_uses_history(fx)) return true;
    return false;
}

}  // namespace looks::doc
