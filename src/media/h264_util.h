#pragma once

#include <cstdint>
#include <vector>

namespace looks::media {

// SPS/PPS go to the out-params, not the payload; AUD/filler drop.
// The return value means the unit contains an IDR slice.
bool annexb_to_avcc_sample(const uint8_t* data, size_t size,
                           std::vector<uint8_t>& out,
                           std::vector<uint8_t>* sps,
                           std::vector<uint8_t>* pps);

// Writes one SPS and one PPS only; parse_avcc accepts the general record.
std::vector<uint8_t> build_avcc(const std::vector<uint8_t>& sps,
                                const std::vector<uint8_t>& pps);

// sps_pps_annexb: all parameter sets as Annex B, 4-byte start codes.
struct AvccInfo {
    int nal_length_size = 4;
    std::vector<uint8_t> sps_pps_annexb;
};

bool parse_avcc(const std::vector<uint8_t>& avcc, AvccInfo* out);

// The start code is always 4 bytes.
void append_annexb_nal(std::vector<uint8_t>& out, const uint8_t* data,
                       size_t size);

}  // namespace looks::media
