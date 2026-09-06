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
    {"sample", "samples", 0.0f, 5.0f, 0.0f, "%.0f",
     "centre|average|brightest|darkest|median|corner"},
    {"shape", "cell", 0.0f, 2.0f, 0.0f, "%.0f", "square|column|row"},
};

constexpr ParamDesc kGrainParams[] = {
    {"amount", "amount", 0.0f, 1.0f, 0.35f, "%.2f"},
    // The kernel forces cell size 1 in ccd mode.
    {"size", "size", 1.0f, 8.0f, 1.5f, "%.1f px", nullptr, false, 3, 0x1},
    {"color", "color", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"mode", "mode", 0.0f, 1.0f, 0.0f, "%.0f", "film|ccd"},
};

constexpr ParamDesc kJitterParams[] = {
    {"amount", "amount", 0.0f, 48.0f, 6.0f, "%.1f px"},
    {"speed", "speed", 0.1f, 12.0f, 2.0f, "%.1f hz"},
    {"mode", "mode", 0.0f, 3.0f, 0.0f, "%.0f",
     "gate weave|eis wobble|vhs tracking|jello"},
};

constexpr ParamDesc kQuantizeParams[] = {
    {"levels", "levels", 2.0f, 16.0f, 4.0f, "%.0f", nullptr, true, 1, 0x3},
    {"palette", "palette", 0.0f, 6.0f, 0.0f, "%.0f",
     "rgb|gray|game boy palette|cga palette|nes subset|teletext palette|duotone"},
    {"dither", "dither", 0.0f, 16.0f, 3.0f, "%.0f",
     "none|bayer 2|bayer 4|bayer 8|white noise|blue noise|stbn|moire|"
     "level cycle|rd stipple|spiral|rings|diamond|clustered dot|lines|"
     "checker|ign"},
    // Mask bits: 0x1FCFE = ordered patterns, 0x1FEFE adds rd stipple,
    // 0x1FDFE adds level cycle.
    {"dither_amt", "dither amt", 0.0f, 1.0f, 1.0f, "%.2f", nullptr, false,
     2, 0x1FEFE},
    {"boil_hz", "boil rate", 0.0f, 30.0f, 8.0f, "%.0f hz", nullptr, false,
     2, 0x1FDFE},
    {"scroll", "pattern scroll", 0.0f, 64.0f, 0.0f, "%.0f px/s", nullptr,
     false, 2, 0x1FCFE},
    {"warp", "wave warp", 0.0f, 1.0f, 0.0f, "%.2f", nullptr, false,
     2, 0x1FCFE},
    {"dissolve", "dissolve", 0.0f, 1.0f, 1.0f, "%.2f"},
    // Pattern scale floors at 1x: sub-1 strides alias the matrix.
    {"lock", "lock", 0.0f, 1.0f, 0.0f, "%.0f", "screen|motion", false,
     2, 0x1FCFE},
    {"pat_rotate", "pattern rotate", -180.0f, 180.0f, 0.0f, "%.0f deg",
     nullptr, false, 2, 0x1FCFE},
    {"pat_scale", "pattern scale", 1.0f, 4.0f, 1.0f, "%.2f x", nullptr,
     false, 2, 0x1FCFE},
    {"pal_hue_a", "custom hue a", 0.0f, 1.0f, 0.08f, "%.2f", nullptr,
     false, 1, 0x40, false, 0.5f},
    {"pal_hue_b", "custom hue b", 0.0f, 1.0f, 0.55f, "%.2f", nullptr,
     false, 1, 0x40, false, 0.5f},
};

constexpr ParamDesc kGlowParams[] = {
    {"amount", "amount", 0.0f, 3.0f, 1.0f, "%.2f"},
    {"radius", "radius", 1.0f, 64.0f, 18.0f, "%.0f px"},
    {"threshold", "threshold", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"mode", "mode", 0.0f, 2.0f, 0.0f, "%.0f",
     "pro-mist|halation|ccd"},
    {"smear", "ccd smear", 0.0f, 1.0f, 0.0f, "%.2f", nullptr, false,
     3, 0x4},
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
    {"quality", "quality", 1.0f, 100.0f, 50.0f, "%.0f", nullptr, true},
    // gop 0 = no I-frames at all.
    {"gop", "gop (0=none)", 0.0f, 120.0f, 30.0f, "%.0f", nullptr, true},
    {"mv_scale", "mv scale", -4.0f, 4.0f, 1.0f, "%.2f"},
    {"mv_random", "mv random", 0.0f, 32.0f, 0.0f, "%.1f px"},
    {"corrupt", "corrupt", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"bloom", "bloom", 0.0f, 8.0f, 0.0f, "%.0f", nullptr, true},
    {"mv_rotate", "mv rotate", -180.0f, 180.0f, 0.0f, "%.0f deg"},
    {"byte_flips", "byte flips", 0.0f, 64.0f, 0.0f, "%.0f", nullptr, true},
    {"mv_field", "field", 0.0f, 3.0f, 0.0f, "%.0f",
     "flow|pan|zoom|swirl"},
    {"field_amt", "field amt", -32.0f, 32.0f, 8.0f, "%.0f px", nullptr,
     false, 8, 0xE},
    {"drop_i", "i-frames", 0.0f, 1.0f, 1.0f, "%.0f", "accept|drop"},
};

constexpr ParamDesc kGenerationLossParams[] = {
    {"quality", "quality", 1.0f, 100.0f, 35.0f, "%.0f", nullptr, true},
    {"generations", "generations", 1.0f, 12.0f, 4.0f, "%.0f", nullptr, true},
};

constexpr ParamDesc kBitrateStarveParams[] = {
    {"budget_kb", "budget", 1.0f, 200.0f, 8.0f, "%.0f kb"},
    {"gop", "gop", 1.0f, 120.0f, 30.0f, "%.0f", nullptr, true},
};

constexpr ParamDesc kEchoParams[] = {
    {"decay", "decay", 0.0f, 0.98f, 0.85f, "%.2f"},
    {"mode", "mode", 0.0f, 1.0f, 0.0f, "%.0f", "lighten|crossfade"},
};

constexpr ParamDesc kFeedbackParams[] = {
    {"amount", "amount", 0.0f, 1.0f, 0.7f, "%.2f"},
    {"zoom", "zoom", 0.8f, 1.2f, 1.03f, "%.3f"},
    {"rotate", "rotate", -0.2f, 0.2f, 0.01f, "%.3f"},
    {"fade", "fade", 0.0f, 0.5f, 0.06f, "%.2f"},
    {"hue_shift", "hue shift", -60.0f, 60.0f, 0.0f, "%.0f deg"},
};

constexpr ParamDesc kGlyphParams[] = {
    {"cell", "cell size", 2.0f, 32.0f, 8.0f, "%.0f px"},
    {"set", "set", 0.0f, 4.0f, 1.0f, "%.0f",
     "halftone|ascii|custom|braille|teletext"},
    {"mode", "mode", 0.0f, 2.0f, 1.0f, "%.0f",
     "mono|colored|glyph x pixel"},
    {"jitter", "jitter", 0.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kFilmStockParams[] = {
    {"preset", "stock", 0.0f, 16.0f, 1.0f, "%.0f",
     "neutral|kodachrome|portra|vision3|ektachrome|fuji|bleach bypass|"
     "cross-process|technicolor 2|ccd video|wb warm|wb cool|technicolor 3|"
     "expired|aerochrome|b&w infrared|instant"},
    {"push", "push/pull", -2.0f, 2.0f, 0.0f, "%+.1f st"},
    {"punch", "punch", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"fade", "fade", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"highlight", "highlight (-clip +roll)", -1.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kKaleidoParams[] = {
    {"segments", "segments", 2.0f, 16.0f, 6.0f, "%.0f", nullptr, true},
    {"angle", "angle", -3.1416f, 3.1416f, 0.0f, "%.2f", nullptr, false,
     -1, 0, true},
    {"center_x", "center x", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"center_y", "center y", 0.0f, 1.0f, 0.5f, "%.2f"},
};

constexpr ParamDesc kPolarParams[] = {
    {"mode", "mode", 0.0f, 1.0f, 0.0f, "%.0f", "to polar|from polar"},
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
    {"angle", "angle", -3.1416f, 3.1416f, 0.0f, "%.2f", nullptr, false,
     -1, 0, true},
    {"mode", "mode", 0.0f, 1.0f, 0.0f, "%.0f", "luma|gradient"},
    {"map_mode", "map", 0.0f, 1.0f, 0.0f, "%.0f", "self|map input"},
};

constexpr ParamDesc kLensDistortParams[] = {
    {"k1", "barrel", -1.0f, 1.0f, 0.15f, "%.2f"},
    {"k2", "edge", -1.0f, 1.0f, 0.0f, "%.2f"},
    {"zoom", "zoom", 0.5f, 2.0f, 1.0f, "%.2f"},
};

constexpr ParamDesc kFringeParams[] = {
    {"amount", "amount", 0.0f, 16.0f, 3.0f, "%.1f px"},
    {"mode", "mode", 0.0f, 1.0f, 0.0f, "%.0f", "lens ca|purple fringe"},
    {"threshold", "threshold", 0.0f, 1.0f, 0.7f, "%.2f", nullptr, false,
     1, 0x2},
};

constexpr ParamDesc kInterlaceParams[] = {
    {"shift", "comb shift", 0.0f, 16.0f, 4.0f, "%.1f px", nullptr, false,
     2, 0x2},
    {"darken", "line darken", 0.0f, 1.0f, 0.15f, "%.2f"},
    {"mode", "mode", 0.0f, 2.0f, 0.0f, "%.0f",
     "field weave|comb|lines only"},
    {"standard", "raster", 0.0f, 1.0f, 0.0f, "%.0f", "480 lines|576 lines"},
    {"field_order", "first field", 0.0f, 1.0f, 0.0f, "%.0f", "top|bottom", false, 2, 0x1},
};

constexpr ParamDesc kSliceShuffleParams[] = {
    {"slices", "slices", 2.0f, 64.0f, 12.0f, "%.0f", nullptr, true},
    {"amount", "amount", 0.0f, 256.0f, 48.0f, "%.0f px"},
    {"rate", "rate", 0.0f, 30.0f, 4.0f, "%.0f hz"},
    {"prob", "probability", 0.0f, 1.0f, 0.5f, "%.2f"},
};

constexpr ParamDesc kPixelStretchParams[] = {
    {"position", "position", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"mode", "mode", 0.0f, 3.0f, 0.0f, "%.0f", "down|up|right|left"},
};

constexpr ParamDesc kCompositeParams[] = {
    {"mode", "signal", 0.0f, 1.0f, 0.0f, "%.0f", "composite|s-video"},
    {"res", "luma res", 200.0f, 1400.0f, 640.0f, "%.0f smp"},
    {"chroma_res", "chroma res", 20.0f, 400.0f, 120.0f, "%.0f smp"},
    {"dot_crawl", "dot crawl", 0.0f, 1.0f, 0.5f, "%.2f", nullptr, false,
     0, 0x1},
    {"rainbow", "rainbow", 0.0f, 1.0f, 0.4f, "%.2f", nullptr, false,
     0, 0x1},
    {"chroma_delay", "chroma delay", 0.0f, 16.0f, 4.0f, "%.1f px"},
};

constexpr ParamDesc kVhsParams[] = {
    {"mode", "speed", 0.0f, 2.0f, 0.0f, "%.0f", "sp|lp|ep"},
    {"luma_res", "luma res", 120.0f, 420.0f, 240.0f, "%.0f lines"},
    {"chroma_res", "chroma res", 10.0f, 120.0f, 40.0f, "%.0f lines"},
    {"ringing", "edge ringing", 0.0f, 1.0f, 0.35f, "%.2f"},
    {"noise", "luma noise", 0.0f, 1.0f, 0.25f, "%.2f"},
    {"chroma_noise", "chroma noise", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"dropouts", "dropouts", 0.0f, 1.0f, 0.12f, "%.2f"},
    {"tbe", "line jitter", 0.0f, 8.0f, 1.2f, "%.1f px"},
    {"skew", "top flagging", 0.0f, 1.0f, 0.15f, "%.2f"},
};

constexpr ParamDesc kMorphologyParams[] = {
    {"op", "operation", 0.0f, 1.0f, 0.0f, "%.0f", "dilate|erode"},
    {"radius", "radius", 0.0f, 6.0f, 2.0f, "%.1f px"},
    {"shape", "kernel", 0.0f, 3.0f, 0.0f, "%.0f",
     "disc|square|cross|line"},
    {"angle", "line angle", 0.0f, 3.14159265f, 0.0f, "%.0f", nullptr, false,
     2, 1u << 3, true},
    {"channels", "channels", 0.0f, 2.0f, 1.0f, "%.0f", "luma|rgb|alpha"},
};

constexpr ParamDesc kDripParams[] = {
    {"rate", "fall rate", 0.0f, 24.0f, 4.0f, "%.1f px"},
    {"threshold", "threshold", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"pick", "picks", 0.0f, 1.0f, 0.0f, "%.0f", "bright|dark"},
    {"viscosity", "retention", 0.0f, 1.0f, 0.88f, "%.2f"},
    {"angle", "fall angle", -3.14159265f, 3.14159265f, 1.5707963f, "%.0f",
     nullptr, false, -1, 0, true},
    {"spread", "spread", 0.0f, 1.0f, 0.25f, "%.2f"},
};

constexpr ParamDesc kBurnInParams[] = {
    {"mode", "mode", 0.0f, 3.0f, 3.0f, "%.0f", "brightest|darkest|first|wear"},
    {"threshold", "threshold", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"bleed", "bleed back", 0.0f, 1.0f, 0.0f, "%.3f"},
    {"tint", "scar tint", 0.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kApertureParams[] = {
    {"radius", "radius", 0.0f, 48.0f, 10.0f, "%.1f px"},
    {"blades", "blades", 3.0f, 9.0f, 6.0f, "%.0f", nullptr, true},
    {"rotation", "iris angle", 0.0f, 3.14159265f, 0.0f, "%.0f", nullptr,
     false, -1, 0, true},
    {"cat_eye", "cat's eye", 0.0f, 1.0f, 0.35f, "%.2f"},
    {"depth", "depth from", 0.0f, 2.0f, 0.0f, "%.0f", "luma|radius|input b"},
    {"focus", "focus at", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"highlight", "highlight gain", 1.0f, 4.0f, 1.6f, "%.2f"},
};

constexpr ParamDesc kParallaxParams[] = {
    {"depth", "depth from", 0.0f, 1.0f, 0.0f, "%.0f", "luma|input b"},
    {"pan", "pan", -1.0f, 1.0f, 0.0f, "%.2f"},
    {"tilt", "tilt", -1.0f, 1.0f, 0.0f, "%.2f"},
    {"skew", "skew", -1.0f, 1.0f, 0.0f, "%.2f"},
    {"scale", "dolly", -1.0f, 1.0f, 0.0f, "%.2f"},
    {"strength", "depth scale", 0.0f, 1.0f, 0.25f, "%.2f"},
    {"gamma", "depth gamma", 0.25f, 4.0f, 1.0f, "%.2f"},
    {"fill", "uncovered", 0.0f, 1.0f, 0.0f, "%.0f", "stretch|clear"},
};

constexpr ParamDesc kPatchWeaveParams[] = {
    {"block", "block", 4.0f, 64.0f, 16.0f, "%.0f px", nullptr, true},
    {"candidates", "candidates", 1.0f, 8.0f, 4.0f, "%.0f", nullptr, true},
    {"reach", "search reach", 0.0f, 0.5f, 0.15f, "%.2f"},
    {"bias", "match bias", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"hold", "hold frames", 0.0f, 30.0f, 0.0f, "%.0f", nullptr, true},
    {"blend", "seam blend", 0.0f, 1.0f, 0.2f, "%.2f"},
};

constexpr ParamDesc kHalationParams[] = {
    {"threshold", "threshold", 0.0f, 1.0f, 0.72f, "%.2f"},
    {"radius", "radius", 0.0f, 64.0f, 16.0f, "%.1f px"},
    {"hue", "tint hue", 0.0f, 1.0f, 0.03f, "%.2f", nullptr,
     false, -1, 0, false, 0.5f},
    {"sat", "tint depth", 0.0f, 1.0f, 0.7f, "%.2f"},
    {"strength", "strength", 0.0f, 2.0f, 0.8f, "%.2f"},
    {"absorb", "backing absorb", 0.0f, 1.0f, 0.25f, "%.2f"},
};

constexpr ParamDesc kRollingShutterParams[] = {
    {"readout", "readout", 0.0f, 2.0f, 0.6f, "%.2f"},
    {"axis", "scan axis", 0.0f, 1.0f, 0.0f, "%.0f", "rows|columns"},
    {"gain", "motion gain", 0.0f, 8.0f, 2.0f, "%.2f"},
    {"anchor", "anchor line", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"wobble", "jelly", 0.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kSnowParams[] = {
    {"amount", "snow", 0.0f, 1.0f, 0.25f, "%.2f"},
    {"ghost_px", "ghost offset", 0.0f, 64.0f, 24.0f, "%.0f px"},
    {"ghost", "ghost strength", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"floor_amt", "noise floor", 0.0f, 1.0f, 0.15f, "%.2f"},
};

constexpr ParamDesc kSyncFailParams[] = {
    {"roll", "v roll", 0.0f, 4.0f, 0.6f, "%.2f hz"},
    {"tear", "h tear", 0.0f, 1.0f, 0.35f, "%.2f"},
    {"band", "tear band", 0.02f, 0.5f, 0.15f, "%.2f"},
};

constexpr ParamDesc kTimestampParams[] = {
    {"mode", "mode", 0.0f, 2.0f, 2.0f, "%.0f", "date|time|both"},
    {"size", "size", 1.0f, 6.0f, 3.0f, "%.0f"},
    {"corner", "corner", 0.0f, 3.0f, 3.0f, "%.0f",
     "top left|top right|bottom left|bottom right"},
    {"year", "year", 1970.0f, 2099.0f, 2003.0f, "%.0f", nullptr, true, 0, 0x5},
    {"month", "month", 1.0f, 12.0f, 12.0f, "%.0f", nullptr, true, 0, 0x5},
    {"day", "day", 1.0f, 31.0f, 24.0f, "%.0f", nullptr, true, 0, 0x5},
    {"start_time", "start time", 0.0f, 86399.0f, 0.0f, "%.0f s", nullptr, true},
};

constexpr ParamDesc kOversharpenParams[] = {
    {"amount", "amount", 0.0f, 4.0f, 1.5f, "%.2f"},
    {"radius", "radius", 0.5f, 8.0f, 2.0f, "%.1f px"},
};

constexpr ParamDesc kBlurParams[] = {
    {"amount", "amount", 0.0f, 64.0f, 16.0f, "%.0f px"},
    {"mode", "mode", 0.0f, 6.0f, 0.0f, "%.0f",
     "directional|spin|zoom|gaussian|tilt-shift|bokeh|surface"},
    // angle steers directional and tilt; focus doubles as surface range.
    {"angle", "angle", -3.1416f, 3.1416f, 0.0f, "%.2f", nullptr, false,
     1, 0x11, true},
    {"focus", "tilt focus", 0.0f, 1.0f, 0.5f, "%.2f", nullptr, false,
     1, 0x50},
    {"band", "tilt band", 0.02f, 0.5f, 0.15f, "%.2f", nullptr, false,
     1, 0x10},
};

constexpr ParamDesc kDustScratchesParams[] = {
    {"dust", "dust", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"scratch", "scratches", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"size", "size", 0.5f, 3.0f, 1.0f, "%.1f"},
    {"hair", "hair", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"texture", "texture amt", 0.0f, 1.0f, 0.0f, "%.2f"},
    // 0 = fixed plate. Above 0, scratches boil at 1/12 and hairs at 1/22.
    {"boil", "boil (0=fixed plate)", 0.0f, 30.0f, 0.0f, "%.1f hz"},
};

constexpr ParamDesc kLightLeakParams[] = {
    {"amount", "amount", 0.0f, 2.0f, 0.8f, "%.2f"},
    {"hue", "hue (0=warm 1=magenta)", 0.0f, 1.0f, 0.2f, "%.2f",
     nullptr, false, -1, 0, false, 0.65f},
    {"drift", "drift", 0.0f, 2.0f, 0.25f, "%.2f hz"},
    {"burn", "film burn", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"entry", "entry edge", 0.0f, 3.0f, 2.0f, "%.0f", "left|top|right|bottom"},
};

constexpr ParamDesc kAnamorphicParams[] = {
    {"streak", "streak", 0.0f, 2.0f, 0.8f, "%.2f"},
    {"threshold", "threshold", 0.0f, 1.0f, 0.75f, "%.2f"},
    {"squeeze", "squeeze", 1.0f, 2.0f, 1.33f, "%.2f"},
    {"bokeh", "oval bokeh", 0.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kDirectFlashParams[] = {
    {"strength", "strength", 0.0f, 2.0f, 0.9f, "%.2f"},
    {"falloff", "falloff", 0.5f, 4.0f, 1.8f, "%.2f"},
    {"cool", "cool tint", 0.0f, 1.0f, 0.35f, "%.2f"},
    {"red_eye", "red eye", 0.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kEdgeDetectParams[] = {
    {"threshold", "threshold", 0.0f, 1.0f, 0.15f, "%.2f"},
    {"mode", "mode", 0.0f, 2.0f, 0.0f, "%.0f",
     "white on black|black on white|overlay"},
    {"thickness", "thickness", 0.5f, 4.0f, 1.0f, "%.1f px"},
    {"algo", "algo", 0.0f, 1.0f, 0.0f, "%.0f", "sobel|dog"},
};

constexpr ParamDesc kKuwaharaParams[] = {
    {"radius", "radius", 1.0f, 6.0f, 3.0f, "%.0f px"},
};

constexpr ParamDesc kCelShadeParams[] = {
    {"bands", "bands", 2.0f, 8.0f, 4.0f, "%.0f", nullptr, true},
    {"edge", "edge", 0.0f, 1.0f, 0.6f, "%.2f"},
};

constexpr ParamDesc kRuttEtraParams[] = {
    {"spacing", "spacing", 2.0f, 16.0f, 6.0f, "%.0f px"},
    {"amount", "amount", 0.0f, 64.0f, 24.0f, "%.0f px"},
    {"line_w", "line width", 0.5f, 4.0f, 1.5f, "%.1f px"},
    {"mode", "mode", 0.0f, 1.0f, 0.0f, "%.0f", "rows|cols"},
    {"wiggle", "trace wiggle", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"beam", "dot breakup", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"scroll", "raster scroll", -64.0f, 64.0f, 0.0f, "%.0f px/s"},
};

constexpr ParamDesc kSlitScanParams[] = {
    {"mode", "mode", 0.0f, 1.0f, 0.0f, "%.0f", "rows|cols"},
    {"depth", "depth", 2.0f, 16.0f, 12.0f, "%.0f frames", nullptr, true},
    {"reverse", "reverse", 0.0f, 1.0f, 0.0f, "%.0f", "forward|reverse"},
};

constexpr ParamDesc kCamcorderHudParams[] = {
    {"size", "size", 1.0f, 6.0f, 3.0f, "%.0f"},
    {"blink_hz", "rec blink", 0.0f, 4.0f, 1.0f, "%.1f hz"},
    {"elements", "elements", 0.0f, 2.0f, 2.0f, "%.0f",
     "rec|rec + counter|full hud"},
    {"battery", "battery", 0.0f, 1.0f, 0.67f, "%.2f", nullptr, false, 2, 0x4},
};

constexpr ParamDesc kGateMaskParams[] = {
    {"gauge", "gauge", 0.0f, 4.0f, 1.0f, "%.0f",
     "super 8|16mm|1.85|2.39|instant"},
    {"round", "corner round", 0.0f, 1.0f, 0.35f, "%.2f"},
    {"soft", "softness", 0.0f, 1.0f, 0.15f, "%.2f"},
};

constexpr ParamDesc kCueMarkParams[] = {
    {"period", "period", 2.0f, 60.0f, 12.0f, "%.0f s"},
    {"dwell", "dwell", 1.0f, 12.0f, 4.0f, "%.0f film frames", nullptr, true},
    {"mark_size", "size", 0.02f, 0.12f, 0.05f, "%.2f"},
};

constexpr ParamDesc kScreenTextureParams[] = {
    {"amount", "weave", 0.0f, 1.0f, 0.35f, "%.2f"},
    {"weave_px", "weave scale", 2.0f, 12.0f, 4.0f, "%.0f px"},
    {"hotspot", "hotspot", 0.0f, 1.0f, 0.4f, "%.2f"},
};

constexpr ParamDesc kVoronoiParams[] = {
    {"cells", "cells", 4.0f, 128.0f, 24.0f, "%.0f", nullptr, true},
    {"shatter", "shatter", 0.0f, 64.0f, 10.0f, "%.0f px"},
    {"edge", "edge", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"drift", "drift", 0.0f, 2.0f, 0.3f, "%.2f hz"},
    {"style", "style", 0.0f, 1.0f, 0.0f, "%.0f", "cells|triangles"},
};

constexpr ParamDesc kReactionDiffusionParams[] = {
    {"feed", "feed", 0.010f, 0.090f, 0.037f, "%.3f"},
    {"kill", "kill", 0.040f, 0.070f, 0.060f, "%.3f"},
    {"steps", "steps/frame", 1.0f, 24.0f, 10.0f, "%.0f", nullptr, true},
    {"inject", "frame inject", 0.0f, 1.0f, 0.35f, "%.2f"},
    {"ink", "ink", 0.0f, 1.0f, 0.0f, "%.0f", "dark|lit"},
};

constexpr ParamDesc kErrorDiffusionParams[] = {
    {"levels", "levels", 2.0f, 8.0f, 2.0f, "%.0f", nullptr, true},
    {"mode", "kernel", 0.0f, 7.0f, 0.0f, "%.0f",
     "floyd-steinberg|atkinson|jarvis|stucki|burkes|sierra|ostromoukhov|"
     "riemersma"},
    // Riemersma has no raster to serpentine.
    {"serpentine", "serpentine", 0.0f, 1.0f, 1.0f, "%.0f", "off|on", false,
     1, 0x7F},
    {"carry", "temporal carry", 0.0f, 1.0f, 0.0f, "%.2f"},
    // fast runs 16 bands: different pixels from exact, still deterministic.
    {"speed_mode", "speed", 0.0f, 1.0f, 1.0f, "%.0f", "exact|fast"},
};

constexpr ParamDesc kFlickerParams[] = {
    {"amount", "amount", 0.0f, 1.0f, 0.4f, "%.2f"},
    {"rate", "rate", 1.0f, 48.0f, 24.0f, "%.0f hz"},
    {"mode", "mode", 0.0f, 2.0f, 0.0f, "%.0f",
     "projector beat|random gate|color strobe"},
};

constexpr ParamDesc kFrameHoldParams[] = {
    {"hold_fps", "hold fps", 6.0f, 30.0f, 16.0f, "%.0f"},
    {"blend", "shutter blend", 0.0f, 1.0f, 0.3f, "%.2f"},
};

constexpr ParamDesc kStutterParams[] = {
    {"frames", "loop frames", 2.0f, 16.0f, 4.0f, "%.0f", nullptr, true},
    {"armed", "armed", 0.0f, 1.0f, 0.0f, "%.0f", "off|armed"},
    {"rate", "replay rate", 0.25f, 4.0f, 1.0f, "%.2f"},
};

constexpr ParamDesc kContourParams[] = {
    {"levels", "levels", 3.0f, 24.0f, 10.0f, "%.0f", nullptr, true},
    {"thickness", "thickness", 0.5f, 3.0f, 1.0f, "%.1f"},
    {"mode", "mode", 0.0f, 2.0f, 2.0f, "%.0f",
     "white on black|black on white|overlay"},
};

constexpr ParamDesc kFlowParticlesParams[] = {
    {"advect", "advect", 0.0f, 8.0f, 2.5f, "%.1f"},
    {"dissolve", "dissolve", 0.0f, 1.0f, 0.35f, "%.2f"},
    {"decay", "decay", 0.5f, 0.999f, 0.94f, "%.3f"},
    {"swirl", "swirl", 0.0f, 1.0f, 0.3f, "%.2f"},
};

constexpr ParamDesc kSpherizeParams[] = {
    // Negative amounts pinch inward.
    {"amount", "amount", -1.0f, 1.0f, 1.0f, "%.2f"},
    {"radius", "radius", 0.2f, 1.5f, 0.95f, "%.2f"},
    {"center_x", "center x", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"center_y", "center y", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"shade", "rim shade", 0.0f, 1.0f, 0.35f, "%.2f"},
    {"backdrop", "backdrop", 0.0f, 1.0f, 0.0f, "%.0f", "black|keep bg"},
};

constexpr ParamDesc kSoftUpscaleParams[] = {
    {"factor", "factor", 2.0f, 16.0f, 4.0f, "%.0f x", nullptr, true},
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
    {"direction", "direction", 0.0f, 1.0f, 0.0f, "%.0f",
     "horizontal|vertical"},
};

constexpr ParamDesc kWaveWarpParams[] = {
    {"amount", "amount", 0.0f, 64.0f, 12.0f, "%.0f px"},
    {"freq", "frequency", 0.5f, 24.0f, 4.0f, "%.1f"},
    {"speed", "speed", 0.0f, 4.0f, 0.5f, "%.2f hz"},
    {"mode", "mode", 0.0f, 2.0f, 0.0f, "%.0f",
     "horizontal|vertical|ripple"},
};

constexpr int kCrtSimControlOrder[] = {
    4, 0, 5, 6, 12, 2, 1, 10, 9, 3, 7, 11, 8, kWetParam, kOpacityParam,
};

constexpr ParamDesc kCrtSimParams[] = {
    // LCD mode has no tube terms; scanline drives its row gaps.
    {"curvature", "curvature", 0.0f, 1.0f, 0.35f, "%.2f", nullptr, false,
     4, 0x7},
    {"scanline", "scanlines", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"lines", "scan lines", 160.0f, 1080.0f, 480.0f, "%.0f", nullptr,
     false, 4, 0x7},
    {"bloom", "beam bloom", 0.0f, 1.0f, 0.35f, "%.2f", nullptr, false,
     4, 0x7},
    {"mask_mode", "screen", 0.0f, 3.0f, 0.0f, "%.0f",
     "aperture grille|slot mask|shadow mask|lcd"},
    {"mask", "mask", 0.0f, 1.0f, 0.4f, "%.2f"},
    {"triad", "phosphor pitch", 2.0f, 8.0f, 3.0f, "%.1f px"},
    {"converge", "convergence", 0.0f, 3.0f, 0.6f, "%.1f px", nullptr,
     false, 4, 0x7},
    {"halation", "halation", 0.0f, 1.0f, 0.25f, "%.2f", nullptr, false,
     4, 0x7},
    {"beam_width", "beam width", 0.15f, 0.45f, 0.3f, "%.2f", nullptr,
     false, 4, 0x7},
    {"bandwidth", "signal blur", 0.0f, 3.0f, 0.5f, "%.1f px", nullptr,
     false, 4, 0x7},
    {"persistence", "phosphor decay", 0.0f, 100.0f, 0.0f, "%.0f ms", nullptr,
     false, 4, 0x7},
    {"interlaced", "scan mode", 0.0f, 1.0f, 0.0f, "%.0f",
     "progressive|interlaced", false, 4, 0x7},
};

constexpr ParamDesc kHalftoneParams[] = {
    {"dot_px", "dot size", 2.0f, 32.0f, 8.0f, "%.0f px"},
    {"angle", "screen angle", -90.0f, 90.0f, 15.0f, "%.0f deg"},
    {"mode", "mode", 0.0f, 2.0f, 0.0f, "%.0f", "ink|inverse|cmyk"},
    {"gain", "dot gain", 0.5f, 2.0f, 1.0f, "%.2f"},
    {"soft", "softness", 0.0f, 1.0f, 0.15f, "%.2f"},
    {"shape", "screen", 0.0f, 2.0f, 0.0f, "%.0f", "dots|lines|spiral"},
    {"wave", "engrave wave", 0.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kStarFilterParams[] = {
    {"points", "points", 2.0f, 8.0f, 4.0f, "%.0f", nullptr, true},
    {"length", "length", 8.0f, 256.0f, 90.0f, "%.0f px"},
    {"threshold", "threshold", 0.0f, 1.0f, 0.8f, "%.2f"},
    {"angle", "angle", -90.0f, 90.0f, 15.0f, "%.0f deg"},
};

constexpr ParamDesc kStreakParams[] = {
    {"length", "length", 0.0f, 256.0f, 80.0f, "%.0f px"},
    {"angle", "angle", -3.1416f, 3.1416f, 0.0f, "%.2f", nullptr, false,
     -1, 0, true},
    {"decay", "decay", 0.5f, 0.99f, 0.9f, "%.2f"},
    {"threshold", "threshold (0=smear)", 0.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kSplitToneParams[] = {
    {"sh_hue", "shadow hue", 0.0f, 1.0f, 0.6f, "%.2f", nullptr,
     false, -1, 0, false, 0.28f},
    {"sh_amt", "shadow amt", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"hi_hue", "highlight hue", 0.0f, 1.0f, 0.12f, "%.2f", nullptr,
     false, -1, 0, false, 0.72f},
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
    {"mode", "mode", 0.0f, 3.0f, 0.0f, "%.0f", "play|rec|pause|ff"},
    {"size", "size", 1.0f, 6.0f, 3.0f, "%.0f"},
    {"counter", "counter", 0.0f, 1.0f, 1.0f, "%.0f", "off|on"},
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
    {"op", "op", 0.0f, 2.0f, 0.0f, "%.0f", "xor|and|or"},
    {"value", "pattern", 1.0f, 255.0f, 32.0f, "%.0f", nullptr, true},
    {"channels", "channels", 0.0f, 3.0f, 0.0f, "%.0f",
     "rgb|red|green|blue"},
};

constexpr ParamDesc kBlockShuffleParams[] = {
    {"block_px", "block", 4.0f, 128.0f, 32.0f, "%.0f px"},
    {"amount", "amount", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"spread", "spread", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"rate", "rate", 0.0f, 30.0f, 6.0f, "%.0f hz"},
};

constexpr ParamDesc kBufferGlitchParams[] = {
    {"mode", "mode", 0.0f, 2.0f, 2.0f, "%.0f",
     "stuck columns|row shear|both"},
    {"amount", "amount", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"density", "density", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"rate", "rate", 0.0f, 30.0f, 4.0f, "%.0f hz"},
};

constexpr ParamDesc kCrossHatchParams[] = {
    {"spacing", "spacing", 3.0f, 24.0f, 7.0f, "%.0f px"},
    {"layers", "hatch layers", 1.0f, 4.0f, 3.0f, "%.0f", nullptr, true},
    {"ink", "ink", 0.0f, 1.0f, 0.85f, "%.2f"},
    {"wobble", "line wobble", 0.0f, 1.0f, 0.3f, "%.2f"},
};

constexpr ParamDesc kSpliceBumpParams[] = {
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
    {"range", "range", 2.0f, 16.0f, 12.0f, "%.0f frames", nullptr, true},
    {"mode", "mode", 0.0f, 2.0f, 0.0f, "%.0f",
     "dark lags|bright lags|map input"},
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
    {"mode", "mode", 0.0f, 2.0f, 0.0f, "%.0f",
     "h scan|v scan|ring mod"},
};

constexpr ParamDesc kColorizerParams[] = {
    {"sh_hue", "shadow hue", 0.0f, 1.0f, 0.66f, "%.2f", nullptr,
     false, -1, 0, false, 0.28f},
    {"mid_hue", "mid hue", 0.0f, 1.0f, 0.88f, "%.2f", nullptr,
     false, -1, 0, false, 0.5f},
    {"hi_hue", "highlight hue", 0.0f, 1.0f, 0.12f, "%.2f", nullptr,
     false, -1, 0, false, 0.72f},
    {"sat", "saturation", 0.0f, 1.0f, 0.85f, "%.2f"},
};

constexpr ParamDesc kSolarizeParams[] = {
    {"threshold", "threshold", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"folds", "folds", 1.0f, 4.0f, 1.0f, "%.0f", nullptr, true},
    {"smooth", "knee", 0.0f, 1.0f, 0.2f, "%.2f"},
};

constexpr ParamDesc kInvertParams[] = {
    {"mode", "mode", 0.0f, 2.0f, 0.0f, "%.0f",
     "invert|film negative|luma only"},
    {"amount", "amount", 0.0f, 1.0f, 1.0f, "%.2f"},
};

constexpr ParamDesc kTwirlParams[] = {
    {"angle", "angle", -720.0f, 720.0f, 180.0f, "%.0f deg"},
    {"radius", "radius", 0.1f, 1.5f, 0.8f, "%.2f"},
    {"center_x", "center x", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"center_y", "center y", 0.0f, 1.0f, 0.5f, "%.2f"},
};

constexpr ParamDesc kTileParams[] = {
    {"cols", "columns", 1.0f, 8.0f, 2.0f, "%.0f", nullptr, true},
    {"rows_n", "rows", 1.0f, 8.0f, 2.0f, "%.0f", nullptr, true},
    {"mirror", "mirror", 0.0f, 1.0f, 1.0f, "%.0f", "repeat|mirror"},
};

constexpr ParamDesc kEmbossParams[] = {
    {"strength", "strength", 0.0f, 4.0f, 1.5f, "%.1f"},
    {"angle", "light angle", -3.1416f, 3.1416f, -2.356f, "%.2f", nullptr,
     false, -1, 0, true},
    {"mode", "mode", 0.0f, 1.0f, 0.0f, "%.0f",
     "gray relief|color relief"},
};

constexpr ParamDesc kLensFlareParams[] = {
    {"threshold", "threshold", 0.0f, 1.0f, 0.8f, "%.2f"},
    {"ghosts", "ghosts", 1.0f, 6.0f, 4.0f, "%.0f", nullptr, true},
    {"spread", "spread", 0.2f, 2.0f, 1.0f, "%.2f"},
    {"halo", "halo", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"tint", "rainbow tint", 0.0f, 1.0f, 0.6f, "%.2f"},
};

constexpr ParamDesc kLidarParams[] = {
    {"mode", "mode", 0.0f, 2.0f, 1.0f, "%.0f", "scatter|spin|sweep"},
    {"amount", "density", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"dot_px", "dot size", 1.0f, 8.0f, 2.0f, "%.1f px"},
    {"persist", "persistence", 0.0f, 0.98f, 0.9f, "%.2f"},
    {"rate", "sample rate", 1.0f, 60.0f, 30.0f, "%.0f hz"},
    {"bias", "luma bias", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"spin", "spin/sweep rate", 0.05f, 2.0f, 0.25f, "%.2f hz", nullptr,
     false, 0, 0x6},
    {"color", "source color", 0.0f, 1.0f, 1.0f, "%.2f"},
};

constexpr ParamDesc kVelocityScanParams[] = {
    {"mode", "mode", 0.0f, 1.0f, 0.0f, "%.0f", "cols|rows"},
    {"speed", "sweep speed", 20.0f, 600.0f, 240.0f, "%.0f px/s"},
    {"stick", "stickiness", 0.0f, 1.0f, 0.75f, "%.2f"},
    {"persist", "persistence", 0.0f, 0.98f, 0.92f, "%.2f"},
    {"rate", "spawn rate", 0.5f, 30.0f, 6.0f, "%.1f hz"},
    {"beam_w", "beam width", 0.5f, 4.0f, 1.5f, "%.1f px"},
    {"wiggle", "shear wiggle", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"color", "source color", 0.0f, 1.0f, 1.0f, "%.2f"},
};

constexpr ParamDesc kSlowScanParams[] = {
    {"period", "scan period", 0.5f, 20.0f, 6.0f, "%.1f s"},
    {"mode", "mode", 0.0f, 1.0f, 0.0f, "%.0f", "rows|cols"},
    {"beam", "beam glow", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"noise", "sync noise", 0.0f, 1.0f, 0.4f, "%.2f"},
    {"dim", "dim unscanned", 0.0f, 1.0f, 0.15f, "%.2f"},
};

constexpr ParamDesc kVectorTraceParams[] = {
    {"density", "density", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"len_px", "stroke length", 6.0f, 40.0f, 22.0f, "%.0f px"},
    {"rate", "crawl rate", 0.5f, 20.0f, 5.0f, "%.1f hz"},
    {"persist", "persistence", 0.0f, 0.98f, 0.9f, "%.2f"},
    {"beam_w", "beam width", 0.5f, 3.0f, 1.2f, "%.1f px"},
    {"edge", "edge affinity", 0.0f, 1.0f, 0.7f, "%.2f"},
    {"color", "source color", 0.0f, 1.0f, 0.6f, "%.2f"},
};

constexpr ParamDesc kScopeParams[] = {
    {"mode", "mode", 0.0f, 2.0f, 0.0f, "%.0f",
     "waveform|parade|vectorscope"},
    {"gain", "gain", 0.5f, 2.0f, 1.0f, "%.2f"},
    {"glow", "trace glow", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"persist", "persistence", 0.0f, 0.98f, 0.85f, "%.2f"},
    {"graticule", "graticule", 0.0f, 1.0f, 0.4f, "%.2f"},
    {"color", "source color", 0.0f, 1.0f, 0.3f, "%.2f"},
};

constexpr ParamDesc kSecurityMuxParams[] = {
    {"cols", "columns", 1.0f, 4.0f, 3.0f, "%.0f", nullptr, true},
    {"rows_n", "rows", 1.0f, 4.0f, 3.0f, "%.0f", nullptr, true},
    {"spread", "time spread", 0.0f, 1.0f, 1.0f, "%.2f"},
    {"mono", "mono cctv", 0.0f, 1.0f, 0.7f, "%.2f"},
    {"osd", "osd", 0.0f, 1.0f, 0.8f, "%.2f"},
};

constexpr ParamDesc kAudioScopeParams[] = {
    {"window", "window", 0.05f, 2.0f, 0.5f, "%.2f s"},
    {"mode", "mode", 0.0f, 2.0f, 0.0f, "%.0f", "fill|mirror|line"},
    {"thick", "edge", 1.0f, 6.0f, 2.0f, "%.0f px"},
    {"height", "height", 0.05f, 1.0f, 0.35f, "%.2f"},
    {"pos_y", "position", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"glow", "glow", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"color", "source color", 0.0f, 1.0f, 0.2f, "%.2f"},
    {"trigger", "trigger", 0.0f, 1.0f, 0.0f, "%.0f", "free|rising"},
};

constexpr ParamDesc kEngraverParams[] = {
    {"omega", "base lines", 2.0f, 200.0f, 24.0f, "%.0f"},
    // 0 = off. Negative values invert the signal.
    {"distortion", "distortion", -16.0f, 16.0f, 8.0f, "%.1f x"},
    {"lowpass", "lowpass", 0.0f, 1.0f, 0.15f, "%.2f"},
    // The sign sets the direction.
    {"speed", "travel speed", -10.0f, 10.0f, 0.3f, "%.2f hz"},
    {"mode", "mode", 0.0f, 1.0f, 0.0f, "%.0f", "horizontal|vertical"},
    {"line_w", "line width", 0.5f, 3.0f, 1.0f, "%.1f px"},
    {"channels", "channels", 1.0f, 4.0f, 1.0f, "%.0f", "1|2|3|4"},
    {"spread", "channel spread", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"color", "rgb split", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"threshold", "response curve", 0.0f, 1.0f, 0.35f, "%.2f"},
};

constexpr ParamDesc kAnaglyphParams[] = {
    {"depth", "disparity", 0.0f, 30.0f, 10.0f, "%.0f px"},
    {"pop", "luma pop", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"mode", "mode", 0.0f, 2.0f, 0.0f, "%.0f",
     "red/cyan|red/blue|green/magenta"},
};

constexpr ParamDesc kPhotocopyParams[] = {
    {"contrast", "contrast collapse", 0.0f, 1.0f, 0.65f, "%.2f"},
    {"generations", "generations", 1.0f, 8.0f, 3.0f, "%.0f", nullptr, true},
    {"toner", "toner speckle", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"streaks", "roller streaks", 0.0f, 1.0f, 0.4f, "%.2f"},
};

constexpr ParamDesc kRisographParams[] = {
    {"inks", "inks", 1.0f, 3.0f, 2.0f, "%.0f", nullptr, true},
    {"hue1", "ink 1 hue", 0.0f, 1.0f, 0.55f, "%.2f", nullptr,
     false, -1, 0, false, 0.5f},
    // Ink 3 is always yellow.
    {"hue2", "ink 2 hue", 0.0f, 1.0f, 0.93f, "%.2f", nullptr, false,
     0, 0xC, false, 0.5f},
    {"misreg", "misregistration", 0.0f, 12.0f, 4.0f, "%.0f px"},
    {"grain", "ink grain", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"paper", "paper warmth", 0.0f, 1.0f, 0.3f, "%.2f"},
};

constexpr ParamDesc kWetPlateParams[] = {
    {"ortho", "ortho response", 0.0f, 1.0f, 0.8f, "%.2f"},
    {"tone", "tone (silver->sepia)", 0.0f, 1.0f, 0.25f, "%.2f"},
    {"plate", "plate chemistry", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"fog", "fog", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"vign", "plate vignette", 0.0f, 1.0f, 0.5f, "%.2f"},
};

constexpr ParamDesc kGlassParams[] = {
    // cell_h under 6 px means full-height cells.
    {"type", "type", 0.0f, 4.0f, 0.0f, "%.0f",
     "lens|prism|wave|hammered|frosted"},
    {"cell_w", "cell width", 6.0f, 400.0f, 48.0f, "%.0f px"},
    {"cell_h", "cell height (0=full)", 0.0f, 400.0f, 0.0f, "%.0f px",
     nullptr, false, 0, 0xB},
    {"refract", "refraction", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"dispersion", "dispersion", 0.0f, 1.0f, 0.15f, "%.2f"},
    {"soften", "soften", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"edge", "edge shade", 0.0f, 1.0f, 0.4f, "%.2f", nullptr, false,
     0, 0xB},
    {"glint", "glint", 0.0f, 1.0f, 0.3f, "%.2f", nullptr, false, 0, 0xB},
    {"wobble", "wobble", 0.0f, 1.0f, 0.15f, "%.2f"},
};

constexpr ParamDesc kWatercolorParams[] = {
    {"bleed", "bleed", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"pool", "edge pooling", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"granulation", "granulation", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"paper", "paper", 0.0f, 1.0f, 0.4f, "%.2f"},
};

constexpr ParamDesc kWireTerrainParams[] = {
    {"rows_n", "rows", 16.0f, 120.0f, 48.0f, "%.0f", nullptr, true},
    {"amount", "height", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"pitch", "pitch", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"horizon", "horizon", 0.0f, 0.8f, 0.35f, "%.2f"},
    {"line_w", "line width", 0.5f, 3.0f, 1.2f, "%.1f px"},
    {"grid", "cross grid", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"color", "source color", 0.0f, 1.0f, 0.4f, "%.2f"},
};

constexpr ParamDesc kRidgelineParams[] = {
    {"rows_n", "rows", 12.0f, 80.0f, 32.0f, "%.0f", nullptr, true},
    {"amount", "height", 0.0f, 1.0f, 0.6f, "%.2f"},
    {"line_w", "line width", 0.5f, 3.0f, 1.2f, "%.1f px"},
    {"soften", "soften", 0.0f, 1.0f, 0.3f, "%.2f"},
    {"fill", "fill", 0.0f, 1.0f, 0.9f, "%.2f"},
    {"color", "source color", 0.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kBlendNodeParams[] = {
    {"mode", "mode", 0.0f, 8.0f, 0.0f, "%.0f",
     "normal|add|multiply|screen|difference|subtract|darken|lighten|"
     "overlay"},
};

constexpr ParamDesc kMatteParams[] = {
    {"mode", "mode", 0.0f, 2.0f, 0.0f, "%.0f",
     "luma|bright key|chroma key"},
    {"key", "key center", 0.0f, 1.0f, 0.5f, "%.2f", nullptr, false,
     0, 0x6},
    {"range", "key range", 0.01f, 1.0f, 0.25f, "%.2f", nullptr, false,
     0, 0x6},
    {"black", "black point", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"white", "white point", 0.0f, 1.0f, 1.0f, "%.2f"},
    {"gamma", "gamma", 0.2f, 5.0f, 1.0f, "%.2f"},
    {"invert", "invert", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"output", "output", 0.0f, 1.0f, 0.0f, "%.0f", "matte|cutout"},
};

constexpr ParamDesc kLevelsParams[] = {
    {"black", "in black", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"white", "in white", 0.0f, 1.0f, 1.0f, "%.2f"},
    {"gamma", "gamma", 0.2f, 5.0f, 1.0f, "%.2f"},
    {"out_black", "out black", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"out_white", "out white", 0.0f, 1.0f, 1.0f, "%.2f"},
};

constexpr ParamDesc kHueSatParams[] = {
    {"hue", "hue shift", -180.0f, 180.0f, 0.0f, "%.0f deg"},
    {"sat", "saturation", 0.0f, 2.0f, 1.0f, "%.2f"},
    {"lite", "lightness", -1.0f, 1.0f, 0.0f, "%+.2f"},
};

constexpr ParamDesc kChannelMixParams[] = {
    {"r_from", "red from", 0.0f, 3.0f, 0.0f, "%.0f",
     "red|green|blue|luma"},
    {"g_from", "green from", 0.0f, 3.0f, 1.0f, "%.0f",
     "red|green|blue|luma"},
    {"b_from", "blue from", 0.0f, 3.0f, 2.0f, "%.0f",
     "red|green|blue|luma"},
    {"amount", "amount", 0.0f, 1.0f, 1.0f, "%.2f"},
};

constexpr ParamDesc kPosterizeParams[] = {
    {"levels", "levels", 2.0f, 16.0f, 4.0f, "%.0f", nullptr, true},
    {"mode", "mode", 0.0f, 1.0f, 0.0f, "%.0f", "rgb|tone"},
};

constexpr ParamDesc kThresholdParams[] = {
    {"threshold", "threshold", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"soft", "softness", 0.0f, 1.0f, 0.05f, "%.2f"},
    {"invert", "invert", 0.0f, 1.0f, 0.0f, "%.2f"},
};

constexpr ParamDesc kPaletteMapParams[] = {
    {"palette", "palette", 0.0f, 4.0f, 4.0f, "%.0f",
     "game boy|cga|nes|teletext|duotone"},
    {"pal_hue_a", "duo hue a", 0.0f, 1.0f, 0.08f, "%.2f", nullptr, false,
     0, 0x10, false, 0.5f},
    {"pal_hue_b", "duo hue b", 0.0f, 1.0f, 0.55f, "%.2f", nullptr, false,
     0, 0x10, false, 0.5f},
};

constexpr ParamDesc kDitherParams[] = {
    {"levels", "levels", 2.0f, 16.0f, 2.0f, "%.0f", nullptr, true},
    {"pattern", "pattern", 0.0f, 13.0f, 2.0f, "%.0f",
     "bayer 2|bayer 4|bayer 8|white noise|blue noise|stbn|moire|spiral|"
     "rings|diamond|clustered dot|lines|checker|ign"},
    {"amount", "amount", 0.0f, 1.0f, 1.0f, "%.2f"},
    {"boil_hz", "boil rate", 0.0f, 30.0f, 0.0f, "%.0f hz"},
    {"scroll", "pattern scroll", 0.0f, 64.0f, 0.0f, "%.0f px/s"},
    {"pat_rotate", "pattern rotate", -180.0f, 180.0f, 0.0f, "%.0f deg"},
    {"pat_scale", "pattern scale", 1.0f, 4.0f, 1.0f, "%.2f x"},
    {"lock", "lock", 0.0f, 1.0f, 0.0f, "%.0f", "screen|motion"},
    {"mode", "mode", 0.0f, 1.0f, 0.0f, "%.0f", "rgb|gray"},
};

constexpr ParamDesc kTransformParams[] = {
    // Offsets are frame fractions.
    {"scale", "scale", 0.1f, 8.0f, 1.0f, "%.2f x"},
    {"angle", "rotate", -180.0f, 180.0f, 0.0f, "%.0f deg"},
    {"pos_x", "offset x", -1.0f, 1.0f, 0.0f, "%+.2f"},
    {"pos_y", "offset y", -1.0f, 1.0f, 0.0f, "%+.2f"},
    {"edge_mode", "edges", 0.0f, 3.0f, 1.0f, "%.0f",
     "black|clamp|wrap|mirror"},
    {"flip", "flip", 0.0f, 3.0f, 0.0f, "%.0f",
     "none|horizontal|vertical|both"},
};

constexpr ParamDesc kFrameDelayParams[] = {
    {"frames", "delay", 0.0f, 15.0f, 4.0f, "%.0f frames", nullptr, true},
};

constexpr ParamDesc kTextParams[] = {
    // font indexes assets/fonts/*.ttf sorted by filename.
    // size is px at frame height, so proxy renders match.
    {"font", "font (file # a-z)", 0.0f, 7.0f, 0.0f, "%.0f", nullptr, true},
    {"size", "size", 12.0f, 400.0f, 90.0f, "%.0f px"},
    {"pos_x", "position x", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"pos_y", "position y", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"angle", "rotate", -180.0f, 180.0f, 0.0f, "%.0f deg"},
    {"hue", "hue", 0.0f, 1.0f, 0.0f, "%.2f", nullptr,
     false, -1, 0, false, 0.5f},
    {"sat", "saturation", 0.0f, 1.0f, 0.0f, "%.2f"},
    {"lite", "brightness", 0.0f, 1.0f, 1.0f, "%.2f"},
    {"outline", "outline", 0.0f, 1.0f, 0.25f, "%.2f"},
};

constexpr ParamDesc kWhiteBalanceParams[] = {
    // Linear-light channel gains.
    {"temperature", "temperature", -1.0f, 1.0f, 0.0f, "%+.2f"},
    {"tint", "tint", -1.0f, 1.0f, 0.0f, "%+.2f"},
};

constexpr ParamDesc kSharpenParams[] = {
    {"amount", "amount", 0.0f, 2.0f, 0.5f, "%.2f"},
    {"radius", "radius", 0.5f, 6.0f, 1.5f, "%.1f px"},
};

constexpr ParamDesc kCornerPinParams[] = {
    // Each corner offset is a frame fraction; all zero is identity.
    {"tl_x", "top-left x", -1.0f, 1.0f, 0.0f, "%+.2f"},
    {"tl_y", "top-left y", -1.0f, 1.0f, 0.0f, "%+.2f"},
    {"tr_x", "top-right x", -1.0f, 1.0f, 0.0f, "%+.2f"},
    {"tr_y", "top-right y", -1.0f, 1.0f, 0.0f, "%+.2f"},
    {"bl_x", "bottom-left x", -1.0f, 1.0f, 0.0f, "%+.2f"},
    {"bl_y", "bottom-left y", -1.0f, 1.0f, 0.0f, "%+.2f"},
    {"br_x", "bottom-right x", -1.0f, 1.0f, 0.0f, "%+.2f"},
    {"br_y", "bottom-right y", -1.0f, 1.0f, 0.0f, "%+.2f"},
    {"edge_mode", "edges", 0.0f, 3.0f, 0.0f, "%.0f",
     "black|clamp|wrap|mirror"},
};

// AudioOp carries at most 4 params. Time params ride the look clock.
constexpr ParamDesc kAudioGainParams[] = {
    {"gain", "gain", 0.0f, 4.0f, 1.0f, "%.2fx"},
};

constexpr ParamDesc kAudioBitcrushParams[] = {
    {"bits", "bits", 1.0f, 16.0f, 8.0f, "%.0f", nullptr, true},
};

constexpr ParamDesc kAudioDownsampleParams[] = {
    // Hold length in source samples.
    {"hold", "hold (samples)", 1.0f, 64.0f, 4.0f, "%.0f", nullptr, true},
};

constexpr ParamDesc kAudioDistortionParams[] = {
    {"drive", "drive", 0.0f, 1.0f, 0.35f, "%.2f"},
};

constexpr ParamDesc kAudioDelayParams[] = {
    {"time", "time", 1.0f, 1000.0f, 250.0f, "%.0f ms"},
    {"feedback", "feedback", 0.0f, 0.95f, 0.4f, "%.2f"},
};

constexpr ParamDesc kAudioFilterParams[] = {
    {"cutoff", "cutoff", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"mode", "mode", 0.0f, 1.0f, 0.0f, "%.0f", "lowpass|highpass"},
};

constexpr ParamDesc kTrackPinParams[] = {
    // The region is the plane seed rect at the solve start frame.
    {"mode", "mode", 0.0f, 1.0f, 0.0f, "%.0f", "attach|stabilize"},
    {"region_x", "region x", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"region_y", "region y", 0.0f, 1.0f, 0.5f, "%.2f"},
    {"region_w", "region w", 0.05f, 1.0f, 0.25f, "%.2f"},
    {"region_h", "region h", 0.05f, 1.0f, 0.25f, "%.2f"},
    {"offset_x", "offset x", -1.0f, 1.0f, 0.0f, "%+.2f", nullptr, false,
     0, 0x1},
    {"offset_y", "offset y", -1.0f, 1.0f, 0.0f, "%+.2f", nullptr, false,
     0, 0x1},
    {"pin_scale", "scale", 0.1f, 4.0f, 1.0f, "%.2f", nullptr, false, 0,
     0x1},
    {"angle", "rotate", -3.1416f, 3.1416f, 0.0f, "%.2f", nullptr, false,
     0, 0x1, true},
};

constexpr ParamDesc kOffsetParams[] = {
    {"offset", "offset", -600.0f, 600.0f, 0.0f, "%.0f fr"},
    {"target", "target", 0.0f, 2.0f, 2.0f, "%.0f", "video|audio|both"},
};

constexpr EffectInfo kEffectInfos[] = {
    {"rgb_split", "RGB Split", kRgbSplitParams, 2, FxCategory::Signal},
    {"vignette", "Vignette", kVignetteParams, 3, FxCategory::Optics},
    {"pixelate", "Pixelate", kPixelateParams, 3, FxCategory::Texture},
    {"grain", "Grain", kGrainParams, 4, FxCategory::Texture},
    {"jitter", "Jitter", kJitterParams, 3, FxCategory::Time},
    {"quantize", "Quantizer", kQuantizeParams, 13, FxCategory::Color},
    {"glow", "Glow", kGlowParams, 5, FxCategory::Optics},
    {"flow_smear", "Flow Smear", kFlowSmearParams, 2, FxCategory::Time},
    {"motion_extract", "Motion Extract", kMotionExtractParams, 2,
     FxCategory::Time},
    {"datamosh", "Datamosh", kDatamoshParams, 11, FxCategory::Signal},
    {"generation_loss", "Generation Loss", kGenerationLossParams, 2,
     FxCategory::Signal},
    {"bitrate_starve", "Bitrate Starve", kBitrateStarveParams, 2,
     FxCategory::Signal},
    {"echo", "Echo Trails", kEchoParams, 2, FxCategory::Time},
    {"feedback", "Feedback", kFeedbackParams, 5, FxCategory::Time},
    {"glyph", "Glyph", kGlyphParams, 4, FxCategory::PaintPrint},
    {"film_stock", "Film Stock", kFilmStockParams, 5, FxCategory::Color},
    {"kaleido", "Kaleidoscope", kKaleidoParams, 4, FxCategory::Warp},
    {"polar", "Polar Coords", kPolarParams, 3, FxCategory::Warp},
    {"turbulence", "Turbulence", kTurbulenceParams, 3, FxCategory::Warp},
    {"displace", "Displace", kDisplaceParams, 4, FxCategory::Warp},
    {"lens_distort", "Lens Distort", kLensDistortParams, 3,
     FxCategory::Optics},
    {"fringe", "Fringing", kFringeParams, 3, FxCategory::Optics},
    {"interlace", "Interlace", kInterlaceParams, 5, FxCategory::Signal},
    {"slice_shuffle", "Slice Shuffle", kSliceShuffleParams, 4,
     FxCategory::Signal},
    {"pixel_stretch", "Pixel Stretch", kPixelStretchParams, 2,
     FxCategory::Warp},
    {"composite_artifacts", "Composite Video", kCompositeParams, 6,
     FxCategory::Signal},
    {"analog_snow", "Analog Snow", kSnowParams, 4, FxCategory::Signal},
    {"sync_fail", "Sync Failure", kSyncFailParams, 3, FxCategory::Signal},
    {"timestamp", "Timestamp", kTimestampParams, 7, FxCategory::Overlay},
    {"oversharpen", "Oversharpen", kOversharpenParams, 2,
     FxCategory::Texture},
    {"blur", "Blur", kBlurParams, 5, FxCategory::Texture},
    {"dust_scratches", "Dust & Scratches", kDustScratchesParams, 6,
     FxCategory::Texture},
    {"light_leak", "Light Leak", kLightLeakParams, 5, FxCategory::Optics},
    {"anamorphic", "Anamorphic", kAnamorphicParams, 4, FxCategory::Optics},
    {"direct_flash", "Direct Flash", kDirectFlashParams, 4,
     FxCategory::Optics},
    {"edge_detect", "Edge Detect", kEdgeDetectParams, 4, FxCategory::Mosaic},
    {"kuwahara", "Kuwahara", kKuwaharaParams, 1, FxCategory::PaintPrint},
    {"cel_shade", "Cel Shade", kCelShadeParams, 2, FxCategory::PaintPrint},
    {"rutt_etra", "Rutt-Etra", kRuttEtraParams, 7, FxCategory::Mosaic},
    {"slit_scan", "Slit-Scan", kSlitScanParams, 3, FxCategory::Time},
    {"camcorder_hud", "Camcorder HUD", kCamcorderHudParams, 4,
     FxCategory::Overlay},
    {"gate_mask", "Gate Mask", kGateMaskParams, 3, FxCategory::Overlay},
    {"cue_mark", "Cue Marks", kCueMarkParams, 3, FxCategory::Overlay},
    {"screen_texture", "Screen Texture", kScreenTextureParams, 3,
     FxCategory::Overlay},
    {"voronoi", "Voronoi Shatter", kVoronoiParams, 5, FxCategory::Mosaic},
    {"reaction_diffusion", "Reaction-Diffusion", kReactionDiffusionParams, 5,
     FxCategory::Mosaic},
    {"error_diffusion", "Error Diffusion", kErrorDiffusionParams, 5,
     FxCategory::Color},
    {"flicker", "Flicker", kFlickerParams, 3, FxCategory::Time},
    {"frame_hold", "Frame Hold", kFrameHoldParams, 2, FxCategory::Time},
    {"stutter", "Stutter", kStutterParams, 3, FxCategory::Time},
    {"contour", "Contour Lines", kContourParams, 3, FxCategory::Mosaic},
    {"flow_particles", "Flow Particles", kFlowParticlesParams, 4,
     FxCategory::Mosaic},
    {"spherize", "Spherize", kSpherizeParams, 6, FxCategory::Warp},
    {"soft_upscale", "Soft Upscale", kSoftUpscaleParams, 2,
     FxCategory::Texture},
    {"zoom_crunch", "Zoom Crunch", kZoomCrunchParams, 2,
     FxCategory::Texture},
    {"pixel_sort", "Pixel Sort", kPixelSortParams, 4, FxCategory::Signal},
    {"wave_warp", "Wave Warp", kWaveWarpParams, 4, FxCategory::Warp},
    {"crt_sim", "CRT Tube", kCrtSimParams, 13, FxCategory::Signal,
     kCrtSimControlOrder},
    {"halftone", "Halftone", kHalftoneParams, 7, FxCategory::PaintPrint},
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
     FxCategory::PaintPrint},
    {"splice_bump", "Splice Bump", kSpliceBumpParams, 4, FxCategory::Time},
    {"film_slip", "Film Slip", kFilmSlipParams, 3, FxCategory::Time},
    {"emulsion", "Emulsion", kEmulsionParams, 4, FxCategory::Texture},
    {"time_displace", "Time Displace", kTimeDisplaceParams, 3,
     FxCategory::Time},
    {"flow_paint", "Flow Paint", kFlowPaintParams, 3,
     FxCategory::PaintPrint},
    {"fm_synth", "FM Synth", kFmSynthParams, 6, FxCategory::Mosaic},
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
    {"photocopy", "Photocopy", kPhotocopyParams, 4,
     FxCategory::PaintPrint},
    {"risograph", "Risograph", kRisographParams, 6,
     FxCategory::PaintPrint},
    {"wet_plate", "Wet Plate", kWetPlateParams, 5,
     FxCategory::PaintPrint},
    {"glass", "Glass", kGlassParams, 9, FxCategory::Warp},
    {"watercolor", "Watercolor", kWatercolorParams, 4,
     FxCategory::PaintPrint},
    {"wire_terrain", "Wire Terrain", kWireTerrainParams, 7,
     FxCategory::Mosaic},
    {"ridgeline", "Ridgeline", kRidgelineParams, 6, FxCategory::Mosaic},
    {"slow_scan", "Slow Scan", kSlowScanParams, 5, FxCategory::Time},
    {"vector_trace", "Vector Trace", kVectorTraceParams, 7,
     FxCategory::Mosaic},
    {"scope", "Scope Monitor", kScopeParams, 6, FxCategory::Signal},
    {"security_mux", "Security Mux", kSecurityMuxParams, 5,
     FxCategory::Time},
    {"audio_scope", "Audio Scope", kAudioScopeParams, 8,
     FxCategory::Signal},
    {"engraver", "Engraver", kEngraverParams, 10, FxCategory::PaintPrint},
    {"blend_node", "Blend", kBlendNodeParams, 1, FxCategory::Overlay},
    {"matte", "Matte", kMatteParams, 8, FxCategory::Color},
    {"levels", "Levels", kLevelsParams, 5, FxCategory::Color},
    {"hue_sat", "Hue/Sat", kHueSatParams, 3, FxCategory::Color},
    {"channel_mix", "Channels", kChannelMixParams, 4, FxCategory::Color},
    {"posterize", "Posterize", kPosterizeParams, 2, FxCategory::Color},
    {"threshold", "Threshold", kThresholdParams, 3, FxCategory::Color},
    {"palette_map", "Palette Map", kPaletteMapParams, 3, FxCategory::Color},
    {"dither", "Dither", kDitherParams, 9, FxCategory::Color},
    {"transform", "Transform", kTransformParams, 6, FxCategory::Warp},
    {"frame_delay", "Frame Delay", kFrameDelayParams, 1, FxCategory::Time},
    {"text", "Text", kTextParams, 9, FxCategory::Overlay},
    {"white_balance", "White Balance", kWhiteBalanceParams, 2,
     FxCategory::Color},
    {"sharpen", "Sharpen", kSharpenParams, 2, FxCategory::Texture},
    {"corner_pin", "Corner Pin", kCornerPinParams, 9, FxCategory::Warp},
    {"audio_gain", "Gain", kAudioGainParams, 1, FxCategory::Audio},
    {"audio_bitcrush", "Bitcrush", kAudioBitcrushParams, 1,
     FxCategory::Audio},
    {"audio_downsample", "Downsample", kAudioDownsampleParams, 1,
     FxCategory::Audio},
    {"audio_distortion", "Distortion", kAudioDistortionParams, 1,
     FxCategory::Audio},
    {"audio_delay", "Delay", kAudioDelayParams, 2, FxCategory::Audio},
    {"audio_filter", "Filter", kAudioFilterParams, 2, FxCategory::Audio},
    {"offset", "Offset", kOffsetParams, 2, FxCategory::Time},
    {"track_pin", "Track Pin", kTrackPinParams, 9, FxCategory::Warp},
    {"vhs", "VHS Tape", kVhsParams, 9, FxCategory::Signal},
    {"morphology", "Morphology", kMorphologyParams, 5, FxCategory::Texture},
    {"drip", "Drip", kDripParams, 6, FxCategory::Texture},
    {"burn_in", "Burn In", kBurnInParams, 4, FxCategory::Time},
    {"aperture", "Aperture", kApertureParams, 7, FxCategory::Optics},
    {"parallax", "Parallax", kParallaxParams, 8, FxCategory::Warp},
    {"patch_weave", "Patch Weave", kPatchWeaveParams, 6, FxCategory::Mosaic},
    {"halation", "Halation", kHalationParams, 6, FxCategory::Optics},
    {"rolling_shutter", "Rolling Shutter", kRollingShutterParams, 5,
     FxCategory::Warp},
};
static_assert(sizeof(kEffectInfos) / sizeof(kEffectInfos[0]) ==
              static_cast<size_t>(EffectType::Count));

}  // namespace

const char* fx_category_label(FxCategory category) {
    static const char* kLabels[] = {
        "time & motion",  "warp & displace",  "optics & light",
        "color & tone",   "texture & detail", "structure & scan",
        "paint & print",  "signal & codec",   "frame & overlay",
        "audio & dsp"};
    static_assert(sizeof(kLabels) / sizeof(kLabels[0]) ==
                  static_cast<size_t>(FxCategory::Count));
    const auto i = static_cast<size_t>(category);
    return i < static_cast<size_t>(FxCategory::Count) ? kLabels[i] : "?";
}

const EffectInfo& effect_info(EffectType type) {
    struct Controls {
        EffectInfo info[static_cast<size_t>(EffectType::Count)];
        int order[static_cast<size_t>(EffectType::Count)][32] = {};
        Controls() {
            for (size_t effect = 0; effect < static_cast<size_t>(EffectType::Count); ++effect) {
                info[effect] = kEffectInfos[effect];
                if (info[effect].control_order) continue;
                bool emitted[32] = {};
                uint32_t row = 0;
                for (int phase = 0; phase < 2; ++phase) {
                    for (uint32_t pass = 0; pass < info[effect].param_count; ++pass) {
                        for (uint32_t p = 0; p < info[effect].param_count; ++p) {
                            const auto& param = info[effect].params[p];
                            if (emitted[p] || (phase == 0 && !param.options)) continue;
                            if (param.vis_param >= 0 && !emitted[static_cast<uint32_t>(param.vis_param)]) continue;
                            order[effect][row++] = static_cast<int>(p);
                            emitted[p] = true;
                        }
                    }
                }
                order[effect][row++] = kWetParam;
                order[effect][row] = kOpacityParam;
                info[effect].control_order = order[effect];
            }
        }
    };
    static const Controls controls;
    return controls.info[static_cast<uint32_t>(type)];
}

EffectInstance make_effect(Document& doc, EffectType type) {
    const EffectInfo& info = effect_info(type);
    EffectInstance fx;
    fx.type = type;
    fx.id = doc.next_effect_id++;
    fx.params.reserve(info.param_count);
    for (uint32_t i = 0; i < info.param_count; ++i)
        fx.params.push_back(info.params[i].default_value);
    if (type == EffectType::Text) fx.text = "TEXT";
    return fx;
}

bool effect_uses_history(EffectType type) {
    switch (type) {
        case EffectType::FilmSlip:
        case EffectType::CamAuto:
        case EffectType::Echo:
        case EffectType::Feedback:
        case EffectType::SlitScan:
        case EffectType::Stutter:
        case EffectType::FrameHold:
        case EffectType::ReactionDiffusion:
        case EffectType::ErrorDiffusion:
        case EffectType::Datamosh:
        case EffectType::GenerationLoss:
        case EffectType::BitrateStarve:
        case EffectType::FlowSmear:
        case EffectType::MotionExtract:
        case EffectType::FlowParticles:
        case EffectType::TimeDisplace:
        case EffectType::FlowPaint:
        case EffectType::VelocityScan:
        case EffectType::Lidar:
        case EffectType::SlowScan:
        case EffectType::VectorTrace:
        case EffectType::ScopeMonitor:
        case EffectType::SecurityMux:
        case EffectType::FrameDelay:
        case EffectType::Drip:
        case EffectType::BurnIn:
        case EffectType::RollingShutter:
            return true;
        default:
            return false;
    }
}

namespace {

// Quantize dither mode 9 (rd stipple) accumulates state across frames.
bool instance_uses_history(const EffectInstance& fx) {
    if (effect_uses_history(fx.type)) return true;
    if (fx.type == EffectType::CrtSim && fx.params.size() > 11 &&
        fx.params[4] < 2.5f && fx.params[11] > 0.0f)
        return true;
    // Motion lock reads the previous frame: history in that mode only.
    if (fx.type == EffectType::Quantize && fx.params.size() > 8 &&
        fx.params[8] >= 0.5f)
        return true;
    if (fx.type == EffectType::Dither && fx.params.size() > 7 &&
        fx.params[7] >= 0.5f)
        return true;
    // Field weave reads the previous frame; the other modes stay pure.
    if (fx.type == EffectType::Interlace && fx.params.size() > 2 &&
        fx.params[2] < 0.5f)
        return true;
    return fx.type == EffectType::Quantize && fx.params.size() > 2 &&
           fx.params[2] >= 8.5f && fx.params[2] < 9.5f;
}

}  // namespace

bool look_uses_history(const Look& look) {
    for (const Layer& layer : look.layers)
        for (const EffectInstance& fx : layer.stack) {
            if (fx.bypass) continue;
            if (instance_uses_history(fx)) return true;
            if (fx.type != EffectType::CrtSim || fx.params.size() <= 11) continue;
            auto changes_decay = [&](const ParamKey& key) {
                return key.effect_id == fx.id &&
                    (key.param_index == 11 ||
                     (key.param_index == 4 && fx.params[11] > 0.0f));
            };
            for (const auto& lane : look.lanes)
                if (!lane.muted && !lane.keys.empty() && changes_decay(lane.target))
                    return true;
            for (const auto& route : look.mod_routes)
                if (changes_decay(route.target)) return true;
        }
    return false;
}

bool document_uses_history(const Document& doc) {
    for (const Look& look : doc.looks)
        if (look_uses_history(look)) return true;
    return false;
}

}  // namespace looks::doc
