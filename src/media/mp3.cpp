#include "media/mp3.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "platform/win/mf_codec.h"
#include "util/bytes.h"
#include "util/file.h"
#include "util/log.h"

namespace looks::media {

namespace {

// One parsed frame header. Layer III only; everything else resyncs.
struct FrameHeader {
    int version = 0;        // 1 = MPEG-1, 2 = MPEG-2, 3 = MPEG-2.5
    uint32_t bitrate_kbps = 0;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint32_t frame_bytes = 0;      // header included
    uint32_t samples_per_frame = 0;
    uint32_t side_info_bytes = 0;  // after header (+2 when CRC present)
};

bool parse_header(const uint8_t* p, FrameHeader* out) {
    const uint32_t h = bytes::be32(p);
    if ((h & 0xFFE00000u) != 0xFFE00000u) return false;   // 11-bit sync
    const uint32_t version_bits = (h >> 19) & 3;   // 0=2.5, 2=2, 3=1
    if (version_bits == 1) return false;           // reserved
    const uint32_t layer_bits = (h >> 17) & 3;     // 01 = Layer III
    if (layer_bits != 1) return false;
    const bool crc = ((h >> 16) & 1) == 0;
    const uint32_t bitrate_index = (h >> 12) & 15;
    if (bitrate_index == 0 || bitrate_index == 15) return false;
    const uint32_t rate_index = (h >> 10) & 3;
    if (rate_index == 3) return false;
    const bool padding = ((h >> 9) & 1) != 0;
    const uint32_t mode = (h >> 6) & 3;            // 3 = mono

    static const uint32_t kRates[3] = {44100, 48000, 32000};
    static const uint16_t kBitrateV1[16] = {0,   32,  40,  48, 56,  64,
                                            80,  96,  112, 128, 160, 192,
                                            224, 256, 320, 0};
    static const uint16_t kBitrateV2[16] = {0,  8,  16, 24,  32,  40,
                                            48, 56, 64, 80,  96,  112,
                                            128, 144, 160, 0};

    FrameHeader f;
    f.version = version_bits == 3 ? 1 : (version_bits == 2 ? 2 : 3);
    f.sample_rate = kRates[rate_index];
    if (f.version == 2) f.sample_rate /= 2;
    if (f.version == 3) f.sample_rate /= 4;
    f.bitrate_kbps =
        f.version == 1 ? kBitrateV1[bitrate_index] : kBitrateV2[bitrate_index];
    f.channels = mode == 3 ? 1 : 2;
    f.samples_per_frame = f.version == 1 ? 1152 : 576;
    // Layer III frame length: floor(samples/8 * bitrate / rate) + padding.
    f.frame_bytes = (f.samples_per_frame / 8) * f.bitrate_kbps * 1000 /
                        f.sample_rate +
                    (padding ? 1 : 0);
    if (f.frame_bytes < 24) return false;
    // Side info sits right after the header (and the 2-byte CRC when
    // present); the Xing/Info tag begins after it.
    uint32_t side = 0;
    if (f.version == 1)
        side = f.channels == 2 ? 32 : 17;
    else
        side = f.channels == 2 ? 17 : 9;
    f.side_info_bytes = side + (crc ? 2 : 0);
    *out = f;
    return true;
}

// A Xing/Info (or VBRI) header frame carries stream metadata in an
// otherwise-silent frame; feeding it would prepend a frame of silence.
bool is_metadata_frame(const uint8_t* frame, uint32_t frame_bytes,
                       const FrameHeader& h) {
    const uint32_t at = 4 + h.side_info_bytes;
    if (at + 4 <= frame_bytes) {
        const uint8_t* t = frame + at;
        if (!std::memcmp(t, "Xing", 4) || !std::memcmp(t, "Info", 4))
            return true;
    }
    // VBRI sits at a fixed 32-byte offset past the header.
    if (4 + 32 + 4 <= frame_bytes && !std::memcmp(frame + 36, "VBRI", 4))
        return true;
    return false;
}

}  // namespace

bool decode_mp3(const uint8_t* bytes, size_t size, Mp3Data* out,
                std::string* error) {
    auto fail = [&](const char* what) {
        if (error) *error = what;
        return false;
    };
    if (!bytes || size < 4) return fail("empty stream");

    size_t pos = 0;
    size_t end = size;

    // ID3v2 tag at the front: syncsafe size + optional footer.
    if (size >= 10 && !std::memcmp(bytes, "ID3", 3)) {
        const uint32_t tag = (static_cast<uint32_t>(bytes[6] & 0x7F) << 21) |
                             (static_cast<uint32_t>(bytes[7] & 0x7F) << 14) |
                             (static_cast<uint32_t>(bytes[8] & 0x7F) << 7) |
                             (bytes[9] & 0x7F);
        pos = 10 + tag + ((bytes[5] & 0x10) ? 10 : 0);
        if (pos >= size) return fail("only an ID3 tag");
    }
    // ID3v1 tail.
    if (end >= 128 && !std::memcmp(bytes + end - 128, "TAG", 3)) end -= 128;

    platform::MfSession session;
    if (!session.ok()) return fail("Media Foundation unavailable");

    platform::Mp3Decoder decoder;
    bool created = false;
    uint64_t fed_samples = 0;   // source samples fed, for pts
    Mp3Data data;

    platform::AudioChunk chunk;
    auto pump = [&]() {
        while (decoder.receive(chunk)) {
            if (!data.channels) {
                data.channels = chunk.channels;
                data.sample_rate = chunk.sample_rate;
            }
            data.samples.insert(data.samples.end(), chunk.samples.begin(),
                                chunk.samples.end());
        }
    };

    FrameHeader first{};
    while (pos + 4 <= end) {
        FrameHeader h;
        if (!parse_header(bytes + pos, &h)) {
            ++pos;   // resync: scan forward
            continue;
        }
        if (pos + h.frame_bytes > end) break;   // truncated tail
        if (!created) {
            std::string mf_error;
            if (!decoder.create(h.channels, h.sample_rate, &mf_error))
                return fail(("mp3 decoder: " + mf_error).c_str());
            created = true;
            first = h;
        } else if (h.sample_rate != first.sample_rate) {
            break;   // rate change mid-stream: stop at the clean prefix
        }
        if (!is_metadata_frame(bytes + pos, h.frame_bytes, h)) {
            const int64_t pts = static_cast<int64_t>(
                fed_samples * 10000000ull / first.sample_rate);
            if (!decoder.feed(bytes + pos, h.frame_bytes, pts))
                return fail("mp3 decode failed");
            fed_samples += h.samples_per_frame;
            pump();
        }
        pos += h.frame_bytes;
    }
    if (!created) return fail("no MP3 frames found");
    decoder.drain();
    pump();

    if (data.samples.empty() || !data.channels)
        return fail("mp3 decoded empty");
    *out = std::move(data);
    return true;
}

bool mp3_cover_art(const uint8_t* b, size_t size,
                   std::vector<uint8_t>* image) {
    if (size < 10 || std::memcmp(b, "ID3", 3)) return false;
    const uint8_t ver = b[3];   // 3 = v2.3, 4 = v2.4
    if (ver < 3 || ver > 4) return false;
    const uint8_t tag_flags = b[5];
    if (tag_flags & 0x80) return false;   // unsynchronised tag: skip
    auto syncsafe = [](const uint8_t* p) {
        return (static_cast<uint32_t>(p[0] & 0x7F) << 21) |
               (static_cast<uint32_t>(p[1] & 0x7F) << 14) |
               (static_cast<uint32_t>(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
    };
    const auto& be32 = bytes::be32;
    size_t pos = 10;
    const size_t end = std::min<size_t>(size, 10 + syncsafe(b + 6));
    if (tag_flags & 0x40) {   // extended header
        if (pos + 4 > end) return false;
        pos += ver == 4 ? syncsafe(b + pos) : be32(b + pos) + 4;
    }
    while (pos + 10 <= end) {
        if (!b[pos]) break;   // padding
        const uint8_t* id = b + pos;
        const uint32_t fsz =
            ver == 4 ? syncsafe(b + pos + 4) : be32(b + pos + 4);
        const uint8_t format_flags = b[pos + 9];
        pos += 10;
        if (!fsz || pos + fsz > end) break;
        if (!std::memcmp(id, "APIC", 4)) {
            const uint8_t* d = b + pos;
            uint32_t off = 0;
            // Transformed frames (compressed/encrypted/per-frame unsync)
            // are not worth the machinery for artwork; group/data-length
            // prefixes just skip.
            if (ver == 4) {
                if (format_flags & 0x0E) return false;
                if (format_flags & 0x40) off += 1;   // grouping id
                if (format_flags & 0x01) off += 4;   // data length
            } else {
                if (format_flags & 0xC0) return false;
                if (format_flags & 0x20) off += 1;   // grouping id
            }
            if (off + 2 > fsz) return false;
            const uint8_t enc = d[off];
            off += 1;
            // MIME type: latin-1, nul-terminated.
            while (off < fsz && d[off]) ++off;
            if (off >= fsz) return false;
            off += 1;         // its terminator
            off += 1;         // picture type byte
            // Description: terminator width follows the text encoding.
            if (enc == 1 || enc == 2) {
                while (off + 1 < fsz && (d[off] || d[off + 1])) off += 2;
                off += 2;
            } else {
                while (off < fsz && d[off]) ++off;
                off += 1;
            }
            if (off >= fsz) return false;
            image->assign(d + off, d + fsz);
            return true;
        }
        pos += fsz;
    }
    return false;
}

bool read_mp3(const std::filesystem::path& path, Mp3Data* out,
              std::string* error) {
    auto fail = [&](std::string what) {
        if (error) *error = std::move(what);
        return false;
    };
    const auto bytes = read_file_bytes(path);
    if (!bytes) return fail("cannot open " + path.string());
    if (bytes->empty()) return fail("empty file");
    if (!decode_mp3(bytes->data(), bytes->size(), out, error)) return false;
    log_info("mp3: %u ch @%u Hz, %llu frames", out->channels,
             out->sample_rate,
             static_cast<unsigned long long>(out->frame_count()));
    return true;
}

}  // namespace looks::media
