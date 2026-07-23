// H.264 bitstream plumbing shared by import (AVCC -> Annex B lives in the
// MF glue) and export (Annex B from the encoder MFT -> AVCC samples +
// avcC record for the muxer).

#pragma once

#include <cstdint>
#include <vector>

namespace looks::media {

struct NalView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint8_t type() const { return size ? data[0] & 0x1F : 0; }
};

// Splits an Annex B buffer (3- or 4-byte start codes) into NAL units.
std::vector<NalView> split_annexb(const uint8_t* data, size_t size);

// Converts one encoded access unit from Annex B to AVCC (4-byte lengths).
// SPS/PPS NALs are captured into the out-params (latest wins) and excluded
// from the sample payload; AUD/filler NALs are dropped. Returns true if the
// unit contains an IDR slice.
bool annexb_to_avcc_sample(const uint8_t* data, size_t size,
                           std::vector<uint8_t>& out,
                           std::vector<uint8_t>* sps,
                           std::vector<uint8_t>* pps);

// Builds an AVCDecoderConfigurationRecord (4-byte NAL lengths).
std::vector<uint8_t> build_avcc(const std::vector<uint8_t>& sps,
                                const std::vector<uint8_t>& pps);

}  // namespace looks::media
