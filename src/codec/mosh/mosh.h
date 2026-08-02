// Codec-Box mosh codec: encode -> mangle -> decode in one node, with
// PERSISTENT decoder state across timeline frames and no error resets.
// I+P frames over the shared codec_core; motion vectors are SUPPLIED (by
// the flow module) - no motion search, so encode is near-free and the
// mosh is controllable.
//
// The box is OPEN-LOOP, which is what makes divergence persist: residuals
// are encoded against a clean reference chain (what the encoder believes
// the decoder holds - unmangled MVs, no corruption), then applied to the
// moshed chain (mangled MVs, flips, dropped I-frames). A closed loop
// would repair every artifact within a frame; here dropped I-frames,
// MV mangling, and corruption scars ride the motion until the moshed
// chain accepts an I-frame. drop_iframes = true never accepts one.
//
// One box, different params = the four named effects: Datamosh (MV ops +
// I-frame drop), Generation Loss (N encode loops), JPEG Blocking
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
    bool drop_iframes = false;    // moshed chain never accepts an I-frame
    int p_repeat = 0;             // bloom: re-apply motion N extra times
    int generations = 0;          // extra intra re-encode loops (gen loss)
    float mv_scale = 1.0f;        // MV mangling (moshed chain only)
    float mv_rotate = 0.0f;       // radians
    float mv_random = 0.0f;       // +/- px of seeded randomization
    // Replace-with-custom-field: 0 = flow MVs, 1 = pan,
    // 2 = zoom (radial), 3 = swirl; scale/rotate/random apply on top.
    int mv_field = 0;
    float mv_field_amount = 8.0f; // px at the frame edge (signed)
    float residual_corrupt = 0.0f;   // per-MB skip probability (moshed side)
    uint32_t byte_flips = 0;      // seeded bit flips in the moshed P stream
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
    // Drops both persistent chains (clip change / discontinuity).
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
    // Exact byte size of the P residual stream at this quality, without
    // writing it (parallel AC bits + serial DC chain over p_coeffs_).
    // The rate loop probes with this; actual bytes only materialize when
    // byte flips need something to corrupt.
    size_t p_stream_bytes(int mb_count, int quality);
    // Motion-compensated prediction from `ref`. mangle = false is the
    // encoder's view: raw flow MVs only. mangle = true applies the field
    // replacement plus scale/rotate/random - the moshed decoder's view.
    void predict(const DecodedFrame& ref, uint32_t frame_index,
                 const MoshParams& params, const MvField& mvs, bool mangle,
                 DecodedFrame& out) const;

    DecodedFrame state_;         // moshed chain: what the node outputs
    DecodedFrame clean_state_;   // encoder reference: faithful decode
    bool has_state_ = false;
    std::vector<uint8_t> bitstream_;   // P stream; built only for byte flips
    std::vector<uint8_t> flipped_;     // byte-flipped copy (moshed parse)
    std::vector<int16_t> p_dc_;        // rate probe: quantized DC per block
    std::vector<uint32_t> p_acbits_;   // rate probe: AC bits per block
    // Two-phase scratch (rate loops re-run entropy only; DCT runs once,
    // across threads). Members so capacity persists across frames.
    IntraDct intra_scratch_;
    std::vector<int16_t> p_coeffs_;    // 6 blocks/MB x 64 unquantized coeffs
    std::vector<int16_t> p_parsed_;    // quantized coeffs from flipped_
    std::vector<uint8_t> p_zero_;      // 1 = no residual (outside frame)
    DecodedFrame clean_pred_, pred_, pred_tmp_;   // per-frame prediction
};

}  // namespace looks::codec
