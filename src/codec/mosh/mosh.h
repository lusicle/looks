// The open loop is deliberate: divergence must persist across frames.
// Output is deterministic for the same frame sequence and seed.

#pragma once

#include <cstdint>
#include <vector>

#include "codec/mez.h"

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
    // 0 = flow MVs, 1 = pan, 2 = zoom, 3 = swirl; mangling applies on top.
    int mv_field = 0;
    float mv_field_amount = 8.0f; // px at the frame edge (signed)
    float residual_corrupt = 0.0f;   // per-MB skip probability (moshed side)
    uint32_t byte_flips = 0;      // seeded bit flips in the moshed P stream
    uint32_t bitrate_budget = 0;  // bytes/frame cap; 0 = off (starvation)
    uint64_t seed = 0;
};

// Per-16x16-block motion vectors in full pixels.
struct MvField {
    const float* mx = nullptr;    // row-major, blocks_w * blocks_h
    const float* my = nullptr;
    uint32_t blocks_w = 0;
    uint32_t blocks_h = 0;
};

class MoshCodec {
public:
    void reset();
    bool has_state() const { return has_state_; }

    // frame_index drives GOP phase and all seeding. Output is I420.
    // mvs may be empty; empty means zero motion.
    void process(const FrameView& in, uint32_t frame_index,
                 const MoshParams& params, const MvField& mvs,
                 DecodedFrame& out);

private:
    void encode_decode_intra(const FrameView& in, int quality,
                             DecodedFrame& out);
    // Must match the real P stream byte count at this quality.
    size_t p_stream_bytes(int mb_count, int quality);
    // mangle = false gives the encoder view; true gives the moshed view.
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
    IntraDct intra_scratch_;
    std::vector<int16_t> p_coeffs_;    // 6 blocks/MB x 64 unquantized coeffs
    std::vector<int16_t> p_parsed_;    // quantized coeffs from flipped_
    std::vector<uint8_t> p_zero_;      // 1 = no residual (outside frame)
    DecodedFrame clean_pred_, pred_, pred_tmp_;   // per-frame prediction
};

}  // namespace looks::codec
