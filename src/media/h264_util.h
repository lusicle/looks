// H.264 bitstream plumbing: the avcC record and Annex B conversion,
// shared by the decoder glue (parse, AVCC->Annex B) and the export path
// (Annex B from the encoder MFT -> AVCC samples + avcC for the muxer).
// Parser and builder live together so the record layout is spelled once.

#pragma once

#include <cstdint>
#include <vector>

namespace looks::media {

// Converts one encoded access unit from Annex B to AVCC (4-byte lengths).
// SPS/PPS NALs are captured into the out-params (latest wins) and excluded
// from the sample payload; AUD/filler NALs are dropped. Returns true if the
// unit contains an IDR slice.
bool annexb_to_avcc_sample(const uint8_t* data, size_t size,
                           std::vector<uint8_t>& out,
                           std::vector<uint8_t>* sps,
                           std::vector<uint8_t>* pps);

// Builds an AVCDecoderConfigurationRecord (4-byte NAL lengths, one SPS,
// one PPS - the corner this app's encoder emits; parse_avcc accepts the
// general record).
std::vector<uint8_t> build_avcc(const std::vector<uint8_t>& sps,
                                const std::vector<uint8_t>& pps);

// A parsed AVCDecoderConfigurationRecord: the sample NAL-length width
// and every parameter set as one Annex B stream (4-byte start codes),
// ready to inject ahead of keyframes.
struct AvccInfo {
    int nal_length_size = 4;
    std::vector<uint8_t> sps_pps_annexb;
};

bool parse_avcc(const std::vector<uint8_t>& avcc, AvccInfo* out);

// Appends one NAL as Annex B: 4-byte start code + payload.
void append_annexb_nal(std::vector<uint8_t>& out, const uint8_t* data,
                       size_t size);

}  // namespace looks::media
