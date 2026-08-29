#include "media/pcm.h"

#include <cstdio>
#include <cstring>

#include "util/bytes.h"

namespace looks::media {

namespace {
constexpr uint32_t kMagic = 0x314D4350;   // 'PCM1'
constexpr size_t kHeaderSize = 16;

// Header is explicit LE; samples write as native int16, LE on all targets.
using bytes::le32;
using bytes::put_le32;
}  // namespace

PcmWriter::~PcmWriter() {
    // Remove an unfinished file; a sealed short sidecar reads as valid audio.
    const bool partial = file_ && !finished_;
    if (file_) std::fclose(static_cast<FILE*>(file_));
    if (partial) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
}

bool PcmWriter::open(const std::filesystem::path& path, uint32_t channels,
                     uint32_t sample_rate) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    file_ = f;
    path_ = path;
    channels_ = channels;
    frames_ = 0;
    finished_ = false;

    uint8_t header[kHeaderSize] = {};
    put_le32(header, kMagic);
    header[4] = static_cast<uint8_t>(channels);
    header[5] = static_cast<uint8_t>(channels >> 8);
    header[6] = 16;
    put_le32(header + 8, sample_rate);
    return std::fwrite(header, 1, kHeaderSize, f) == kHeaderSize;
}

bool PcmWriter::append(const int16_t* samples, size_t count) {
    if (!file_ || finished_ || channels_ == 0) return false;
    FILE* f = static_cast<FILE*>(file_);
    if (std::fwrite(samples, 2, count, f) != count) return false;
    frames_ += count / channels_;
    return true;
}

bool PcmWriter::finish() {
    if (!file_ || finished_) return false;
    FILE* f = static_cast<FILE*>(file_);
    const uint32_t frames32 =
        frames_ > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(frames_);
    uint8_t le[4];
    put_le32(le, frames32);
    _fseeki64(f, 12, SEEK_SET);
    if (std::fwrite(le, 1, 4, f) != 4) return false;
    std::fflush(f);
    finished_ = true;
    return true;
}

PcmReader::~PcmReader() { close(); }

void PcmReader::close() {
    if (file_) {
        std::fclose(static_cast<FILE*>(file_));
        file_ = nullptr;
    }
}

bool PcmReader::open(const std::filesystem::path& path, std::string* error) {
    close();
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        if (error) *error = "cannot open pcm file";
        return false;
    }
    file_ = f;
    uint8_t header[kHeaderSize];
    if (std::fread(header, 1, kHeaderSize, f) != kHeaderSize ||
        le32(header) != kMagic) {
        if (error) *error = "not a PCM1 file";
        close();
        return false;
    }
    channels_ = header[4] | (header[5] << 8);
    sample_rate_ = le32(header + 8);
    frames_ = le32(header + 12);
    if (channels_ == 0 || channels_ > 8 || sample_rate_ == 0) {
        if (error) *error = "bad pcm header";
        close();
        return false;
    }
    return true;
}

size_t PcmReader::read(uint64_t first_frame, int16_t* out, size_t frames) {
    std::memset(out, 0, frames * channels_ * 2);
    if (!file_ || first_frame >= frames_) return 0;
    FILE* f = static_cast<FILE*>(file_);
    const size_t available = static_cast<size_t>(
        frames_ - first_frame < frames ? frames_ - first_frame : frames);
    _fseeki64(f, static_cast<int64_t>(kHeaderSize + first_frame * channels_ * 2),
              SEEK_SET);
    const size_t read_samples =
        std::fread(out, 2, available * channels_, f) / channels_;
    return read_samples;
}

}  // namespace looks::media
