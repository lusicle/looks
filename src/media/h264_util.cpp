#include "media/h264_util.h"

namespace looks::media {

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
        const uint32_t len = static_cast<uint32_t>(nal.size);
        out.push_back(static_cast<uint8_t>(len >> 24));
        out.push_back(static_cast<uint8_t>(len >> 16));
        out.push_back(static_cast<uint8_t>(len >> 8));
        out.push_back(static_cast<uint8_t>(len));
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
    avcc.push_back(static_cast<uint8_t>(sps.size() >> 8));
    avcc.push_back(static_cast<uint8_t>(sps.size()));
    avcc.insert(avcc.end(), sps.begin(), sps.end());
    avcc.push_back(1);                     // 1 PPS
    avcc.push_back(static_cast<uint8_t>(pps.size() >> 8));
    avcc.push_back(static_cast<uint8_t>(pps.size()));
    avcc.insert(avcc.end(), pps.begin(), pps.end());
    return avcc;
}

}  // namespace looks::media
