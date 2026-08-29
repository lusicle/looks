#include "media/h264_util.h"

#include "util/bytes.h"

namespace looks::media {

namespace {

struct NalView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint8_t type() const { return size ? data[0] & 0x1F : 0; }
};

std::vector<NalView> split_annexb(const uint8_t* data, size_t size) {
    std::vector<NalView> nals;
    size_t i = 0;
    size_t nal_start = SIZE_MAX;
    while (i + 2 < size) {
        if (data[i] == 0 && data[i + 1] == 0 &&
            (data[i + 2] == 1 ||
             (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1))) {
            const size_t start_len = data[i + 2] == 1 ? 3 : 4;
            if (nal_start != SIZE_MAX && i > nal_start)
                nals.push_back({data + nal_start, i - nal_start});
            i += start_len;
            nal_start = i;
        } else {
            ++i;
        }
    }
    if (nal_start != SIZE_MAX && size > nal_start)
        nals.push_back({data + nal_start, size - nal_start});
    return nals;
}

}  // namespace

bool annexb_to_avcc_sample(const uint8_t* data, size_t size,
                           std::vector<uint8_t>& out,
                           std::vector<uint8_t>* sps,
                           std::vector<uint8_t>* pps) {
    out.clear();
    bool idr = false;
    for (const NalView& nal : split_annexb(data, size)) {
        switch (nal.type()) {
            case 7:   // SPS
                if (sps) sps->assign(nal.data, nal.data + nal.size);
                continue;
            case 8:   // PPS
                if (pps) pps->assign(nal.data, nal.data + nal.size);
                continue;
            case 9:   // AUD
            case 12:  // filler
                continue;
            case 5:
                idr = true;
                break;
            default:
                break;
        }
        bytes::app_be32(out, static_cast<uint32_t>(nal.size));
        out.insert(out.end(), nal.data, nal.data + nal.size);
    }
    return idr;
}

std::vector<uint8_t> build_avcc(const std::vector<uint8_t>& sps,
                                const std::vector<uint8_t>& pps) {
    std::vector<uint8_t> avcc;
    if (sps.size() < 4 || pps.empty()) return avcc;
    avcc.push_back(1);                     // configurationVersion
    avcc.push_back(sps[1]);                // AVCProfileIndication
    avcc.push_back(sps[2]);                // profile_compatibility
    avcc.push_back(sps[3]);                // AVCLevelIndication
    avcc.push_back(0xFF);                  // lengthSizeMinusOne = 3
    avcc.push_back(0xE1);                  // 1 SPS
    bytes::app_be16(avcc, static_cast<uint16_t>(sps.size()));
    avcc.insert(avcc.end(), sps.begin(), sps.end());
    avcc.push_back(1);                     // 1 PPS
    bytes::app_be16(avcc, static_cast<uint16_t>(pps.size()));
    avcc.insert(avcc.end(), pps.begin(), pps.end());
    return avcc;
}

bool parse_avcc(const std::vector<uint8_t>& avcc, AvccInfo* out) {
    // avcC: ver(1) profile(1) compat(1) level(1) lengthSizeMinusOne(1)
    // numSPS(1) [len(2) sps]... numPPS(1) [len(2) pps]...
    if (avcc.size() < 7 || avcc[0] != 1) return false;
    out->nal_length_size = (avcc[4] & 0x3) + 1;
    out->sps_pps_annexb.clear();
    size_t pos = 5;
    const int num_sps = avcc[pos++] & 0x1F;
    auto read_sets = [&](int count) -> bool {
        for (int i = 0; i < count; ++i) {
            if (pos + 2 > avcc.size()) return false;
            const size_t len = bytes::be16(avcc.data() + pos);
            pos += 2;
            if (pos + len > avcc.size()) return false;
            append_annexb_nal(out->sps_pps_annexb, avcc.data() + pos, len);
            pos += len;
        }
        return true;
    };
    if (!read_sets(num_sps)) return false;
    if (pos >= avcc.size()) return false;
    const int num_pps = avcc[pos++];
    return read_sets(num_pps);
}

void append_annexb_nal(std::vector<uint8_t>& out, const uint8_t* data,
                       size_t size) {
    static constexpr uint8_t kStart[4] = {0, 0, 0, 1};
    out.insert(out.end(), kStart, kStart + 4);
    out.insert(out.end(), data, data + size);
}

}  // namespace looks::media
