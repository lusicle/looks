// Codec-Box mosh codec (spec §6.3): an in-node lossy codec — encode ->
// optional bitstream/vector mangling -> decode — with PERSISTENT decoder
// state across timeline frames and no error resets, so smear accumulates
// until an I-frame lands. I+P frames over the shared codec_core; motion
// vectors are SUPPLIED (by the flow module) — no motion search, so encode
// is near-free and the mosh is controllable.
//
// One box, different params = the four named effects: Datamosh (MV ops +
// I-frame drop/hold), Generation Loss (N encode loops), JPEG Blocking
// (intra-only low Q), Bitrate Starvation (rate-limited Q ramp).
// Fully seeded/deterministic given the same frame sequence.

#pragma once

#include <cstdint>
#include <vector>

#include "codec/mez.h"   // FrameView / DecodedFrame

namespace looks::codec {

struct MoshParams {
    int quality = 50;             // quantizer crush (1..100)
    int gop_length = 30;          // I-frame every N frames; 0 = never
    bool drop_iframes = false;    // classic mosh: hold the stale reference
    int p_repeat = 0;             // bloom: re-apply motion N extra times
    int generations = 0;          // extra intra re-encode loops (gen loss)
    float mv_scale = 1.0f;        // MV mangling
    float mv_rotate = 0.0f;       // radians
    float mv_random = 0.0f;       // +/- px of seeded randomization
    // Replace-with-custom-field (spec §6.3): 0 = flow MVs, 1 = pan,
    // 2 = zoom (radial), 3 = swirl; scale/rotate/random apply on top.
    int mv_field = 0;
    float mv_field_amount = 8.0f; // px at the frame edge (signed)
    float residual_corrupt = 0.0f;   // per-MB corruption probability
    uint32_t byte_flips = 0;      // seeded bit flips in the P bitstream
    uint32_t bitrate_budget = 0;  // bytes/frame cap; 0 = off (starvation)
    uint64_t seed = 0;
};

// Per-16x16-block motion vectors in full pixels (from the flow module).
struct MvField {
    const float* mx = nullptr;    // row-major, blocks_w * blocks_h
    const float* my = nullptr;
    uint32_t blocks_w = 0;
    uint32_t blocks_h = 0;
};

class MoshCodec {
public:
    // Drops the persistent reference (clip change / discontinuity).
    void reset();
    bool has_state() const { return has_state_; }

    // Runs one frame through the box. frame_index drives the GOP phase and
    // all seeding. `mvs` may be empty (zero motion). Output is I420.
    void process(const FrameView& in, uint32_t frame_index,
                 const MoshParams& params, const MvField& mvs,
                 DecodedFrame& out);

private:
    void encode_decode_intra(const FrameView& in, int quality,
                             DecodedFrame& out);
    void predict_from_state(uint32_t frame_index, const MoshParams& params,
                            const MvField& mvs, DecodedFrame& out) const;

    DecodedFrame state_;
    bool has_state_ = false;
    std::vector<uint8_t> bitstream_;   // scratch
    // Two-phase scratch (rate loops re-run entropy only; DCT runs once,
    // across threads). Members so capacity persists across frames.
    IntraDct intra_scratch_;
    std::vector<int16_t> p_coeffs_;    // 6 blocks/MB × 64 coeffs
    std::vector<uint8_t> p_zero_;      // 1 = zero residual (outside/corrupt)
};

}  // namespace looks::codec
