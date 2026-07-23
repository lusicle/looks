#include "media/wav.h"

#include <cstdio>
#include <cstring>

namespace looks::media {

namespace {

uint32_t rd_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t rd_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

void wr_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}
void wr_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

int16_t clamp16(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return static_cast<int16_t>(v);
}

}  // namespace

bool read_wav(const std::filesystem::path& path, WavData* out,
              std::string* error) {
    auto fail = [&](const char* what) {
        if (error) *error = what;
        return false;
    };

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
        return fail("cannot open file");
    std::vector<uint8_t> bytes;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 44) {
        std::fclose(f);
        return fail("not a WAV (too small)");
    }
    bytes.resize(static_cast<size_t>(size));
    const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (got != bytes.size()) return fail("read failed");

    if (std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        return fail("not a RIFF/WAVE file");

    // Chunk walk: fmt then data (ignore everything else).
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t sample_rate = 0;
    const uint8_t* data = nullptr;
    uint32_t data_size = 0;
    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const uint8_t* hdr = bytes.data() + pos;
        const uint32_t chunk_size = rd_u32(hdr + 4);
        const size_t body = pos + 8;
        if (body + chunk_size > bytes.size()) break;
        if (std::memcmp(hdr, "fmt ", 4) == 0 && chunk_size >= 16) {
            format = rd_u16(bytes.data() + body);
            channels = rd_u16(bytes.data() + body + 2);
            sample_rate = rd_u32(bytes.data() + body + 4);
            bits = rd_u16(bytes.data() + body + 14);
            // WAVE_FORMAT_EXTENSIBLE: the real format sits in the GUID's
            // first two bytes.
            if (format == 0xFFFE && chunk_size >= 40)
                format = rd_u16(bytes.data() + body + 24);
        } else if (std::memcmp(hdr, "data", 4) == 0) {
            data = bytes.data() + body;
            data_size = chunk_size;
        }
        pos = body + chunk_size + (chunk_size & 1);   // chunks are padded
    }
    if (!channels || !sample_rate) return fail("missing fmt chunk");
    if (!data || !data_size) return fail("missing data chunk");
    const bool pcm = format == 1;
    const bool flt = format == 3;
    if (!pcm && !flt) return fail("unsupported WAV format (want PCM/float)");
    if (pcm && bits != 16 && bits != 24 && bits != 32)
        return fail("unsupported PCM bit depth");
    if (flt && bits != 32) return fail("unsupported float bit depth");

    const uint32_t bytes_per = bits / 8;
    const uint64_t values = data_size / bytes_per;
    out->channels = channels;
    out->sample_rate = sample_rate;
    out->samples.resize(static_cast<size_t>(values));
    for (uint64_t i = 0; i < values; ++i) {
        const uint8_t* s = data + i * bytes_per;
        if (flt) {
            float v;
            std::memcpy(&v, s, 4);
            out->samples[static_cast<size_t>(i)] = clamp16(v * 32767.0f);
        } else if (bits == 16) {
            out->samples[static_cast<size_t>(i)] =
                static_cast<int16_t>(rd_u16(s));
        } else if (bits == 24) {
            const int32_t v = static_cast<int32_t>(
                (static_cast<uint32_t>(s[0]) << 8) |
                (static_cast<uint32_t>(s[1]) << 16) |
                (static_cast<uint32_t>(s[2]) << 24));
            out->samples[static_cast<size_t>(i)] =
                static_cast<int16_t>(v >> 16);
        } else {   // PCM32
            const int32_t v = static_cast<int32_t>(rd_u32(s));
            out->samples[static_cast<size_t>(i)] =
                static_cast<int16_t>(v >> 16);
        }
    }
    return true;
}

bool write_wav(const std::filesystem::path& path, const int16_t* samples,
               uint64_t frame_count, uint32_t channels, uint32_t sample_rate,
               std::string* error) {
    auto fail = [&](const char* what) {
        if (error) *error = what;
        return false;
    };
    if (!channels || !sample_rate) return fail("bad WAV parameters");

    const uint64_t data_size = frame_count * channels * 2;
    if (data_size > 0xFFFFFFFFull - 44) return fail("WAV too large");

    uint8_t header[44];
    std::memcpy(header, "RIFF", 4);
    wr_u32(header + 4, static_cast<uint32_t>(36 + data_size));
    std::memcpy(header + 8, "WAVE", 4);
    std::memcpy(header + 12, "fmt ", 4);
    wr_u32(header + 16, 16);
    wr_u16(header + 20, 1);   // PCM
    wr_u16(header + 22, static_cast<uint16_t>(channels));
    wr_u32(header + 24, sample_rate);
    wr_u32(header + 28, sample_rate * channels * 2);
    wr_u16(header + 32, static_cast<uint16_t>(channels * 2));
    wr_u16(header + 34, 16);
    std::memcpy(header + 36, "data", 4);
    wr_u32(header + 40, static_cast<uint32_t>(data_size));

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
        return fail("cannot create file");
    bool ok = std::fwrite(header, 1, 44, f) == 44;
    if (ok && data_size)
        ok = std::fwrite(samples, 1, static_cast<size_t>(data_size), f) ==
             data_size;
    std::fclose(f);
    return ok || fail("write failed");
}

}  // namespace looks::media
