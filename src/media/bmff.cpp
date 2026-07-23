#include "media/bmff.h"

#include <cstdio>
#include <cstring>

namespace looks::media {

namespace {

constexpr uint32_t fourcc(const char (&s)[5]) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(s[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(s[3]));
}

struct Reader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    bool ok = true;

    bool has(size_t n) const { return ok && pos + n <= size; }
    void fail() { ok = false; }

    uint8_t u8() {
        if (!has(1)) { fail(); return 0; }
        return data[pos++];
    }
    uint16_t u16() {
        if (!has(2)) { fail(); return 0; }
        uint16_t v = (static_cast<uint16_t>(data[pos]) << 8) | data[pos + 1];
        pos += 2;
        return v;
    }
    uint32_t u32() {
        if (!has(4)) { fail(); return 0; }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | data[pos + i];
        pos += 4;
        return v;
    }
    uint64_t u64() {
        if (!has(8)) { fail(); return 0; }
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | data[pos + i];
        pos += 8;
        return v;
    }
    void skip(size_t n) {
        if (!has(n)) { fail(); return; }
        pos += n;
    }
    void bytes(void* out, size_t n) {
        if (!has(n)) { fail(); std::memset(out, 0, n); return; }
        std::memcpy(out, data + pos, n);
        pos += n;
    }
};

struct Box {
    uint32_t type = 0;
    Reader payload{nullptr, 0};
};

// Reads the next child box from `r`. Returns false at end-of-payload or on
// malformation (r.ok distinguishes).
bool next_box(Reader& r, Box* out) {
    if (!r.ok || r.pos >= r.size) return false;
    if (!r.has(8)) { r.fail(); return false; }
    uint64_t box_size = r.u32();
    const uint32_t type = r.u32();
    size_t header = 8;
    if (box_size == 1) {
        box_size = r.u64();
        header = 16;
    } else if (box_size == 0) {
        box_size = (r.size - r.pos) + header;   // to end of enclosing payload
    }
    if (!r.ok || box_size < header || box_size - header > r.size - r.pos) {
        r.fail();
        return false;
    }
    out->type = type;
    out->payload = Reader{r.data + r.pos, static_cast<size_t>(box_size - header)};
    r.pos += static_cast<size_t>(box_size - header);
    return true;
}

// FullBox header: version + 24-bit flags.
struct FullBox {
    uint8_t version;
    uint32_t flags;
};

FullBox full_box(Reader& r) {
    FullBox fb{};
    fb.version = r.u8();
    fb.flags = (static_cast<uint32_t>(r.u8()) << 16);
    fb.flags |= (static_cast<uint32_t>(r.u8()) << 8);
    fb.flags |= r.u8();
    return fb;
}

// ---- per-track sample table accumulators (raw box contents, expanded at
// the end of the trak walk)

struct SttsRun { uint32_t count, delta; };
struct CttsRun { uint32_t count; int64_t offset; };
struct StscRun { uint32_t first_chunk, samples_per_chunk; };

struct SampleTables {
    std::vector<SttsRun> stts;
    std::vector<CttsRun> ctts;
    std::vector<StscRun> stsc;
    std::vector<uint32_t> sizes;      // empty if fixed_size != 0
    uint32_t fixed_size = 0;
    uint32_t sample_count = 0;
    std::vector<uint64_t> chunk_offsets;
    std::vector<uint32_t> sync;       // 1-based sample numbers; empty = all
    bool has_stss = false;
};

void parse_stsd(Reader& r, TrackInfo* track) {
    full_box(r);
    const uint32_t entry_count = r.u32();
    if (entry_count == 0) return;

    Box entry;
    if (!next_box(r, &entry)) return;
    const uint32_t t = entry.type;
    track->fourcc[0] = static_cast<char>(t >> 24);
    track->fourcc[1] = static_cast<char>(t >> 16);
    track->fourcc[2] = static_cast<char>(t >> 8);
    track->fourcc[3] = static_cast<char>(t);
    track->fourcc[4] = 0;
    Reader& e = entry.payload;

    if (track->kind == TrackInfo::Kind::Video) {
        // VisualSampleEntry: 6 reserved + data_ref(2) + predef/reserved(16)
        // + width(2) height(2) + resolutions(8) + reserved(4) +
        // frame_count(2) + compressor(32) + depth(2) + predef(2), then boxes.
        e.skip(6 + 2 + 16);
        track->width = e.u16();
        track->height = e.u16();
        e.skip(4 + 4 + 4 + 2 + 32 + 2 + 2);
        Box child;
        while (next_box(e, &child)) {
            if (child.type == fourcc("avcC")) {
                track->avcc.assign(child.payload.data,
                                   child.payload.data + child.payload.size);
            }
        }
    } else if (track->kind == TrackInfo::Kind::Audio) {
        // AudioSampleEntry: 6 reserved + data_ref(2) + version(2) +
        // revision(2) + vendor(4) + channels(2) + samplesize(2) +
        // predef(2) + reserved(2) + samplerate 16.16(4) [+ v1: 16 bytes,
        // v2: 36 bytes], then boxes.
        e.skip(6 + 2);
        const uint16_t version = e.u16();
        e.skip(2 + 4);
        track->channels = e.u16();
        track->sample_size_bits = e.u16();
        e.skip(2 + 2);
        track->sample_rate = e.u32() >> 16;
        if (version == 1) e.skip(16);
        else if (version == 2) e.skip(36);
        Box child;
        while (next_box(e, &child)) {
            if (child.type == fourcc("esds")) {
                Reader& es = child.payload;
                full_box(es);
                // Descriptor walk: tag byte + 7-bit varlen length.
                auto desc_len = [](Reader& d) -> size_t {
                    size_t len = 0;
                    for (int i = 0; i < 4; ++i) {
                        const uint8_t b = d.u8();
                        len = (len << 7) | (b & 0x7F);
                        if (!(b & 0x80)) break;
                    }
                    return len;
                };
                if (es.u8() != 0x03) break;   // ES_Descriptor
                desc_len(es);
                es.skip(2);                    // ES_ID
                const uint8_t es_flags = es.u8();
                if (es_flags & 0x80) es.skip(2);            // streamDependence
                if (es_flags & 0x40) es.skip(es.u8());      // URL
                if (es_flags & 0x20) es.skip(2);            // OCR
                if (es.u8() != 0x04) break;   // DecoderConfigDescriptor
                desc_len(es);
                es.skip(1 + 4 + 4 + 4);       // objType + stream/buffer + rates
                if (es.u8() != 0x05) break;   // DecoderSpecificInfo
                const size_t asc_len = desc_len(es);
                if (es.has(asc_len)) {
                    track->audio_specific_config.assign(
                        es.data + es.pos, es.data + es.pos + asc_len);
                }
            }
        }
    }
}

void parse_stbl(Reader& r, TrackInfo* track, SampleTables* tables) {
    Box box;
    while (next_box(r, &box)) {
        Reader& p = box.payload;
        switch (box.type) {
            case fourcc("stsd"):
                parse_stsd(p, track);
                break;
            case fourcc("stts"): {
                full_box(p);
                const uint32_t count = p.u32();
                tables->stts.reserve(count);
                for (uint32_t i = 0; i < count && p.ok; ++i) {
                    const uint32_t n = p.u32();
                    const uint32_t delta = p.u32();
                    tables->stts.push_back({n, delta});
                }
                break;
            }
            case fourcc("ctts"): {
                const FullBox fb = full_box(p);
                const uint32_t count = p.u32();
                tables->ctts.reserve(count);
                for (uint32_t i = 0; i < count && p.ok; ++i) {
                    const uint32_t n = p.u32();
                    const uint32_t raw = p.u32();
                    const int64_t offset = fb.version == 1
                        ? static_cast<int32_t>(raw)
                        : static_cast<int64_t>(raw);
                    tables->ctts.push_back({n, offset});
                }
                break;
            }
            case fourcc("stsc"): {
                full_box(p);
                const uint32_t count = p.u32();
                tables->stsc.reserve(count);
                for (uint32_t i = 0; i < count && p.ok; ++i) {
                    const uint32_t first = p.u32();
                    const uint32_t per = p.u32();
                    p.u32();   // sample description index
                    tables->stsc.push_back({first, per});
                }
                break;
            }
            case fourcc("stsz"): {
                full_box(p);
                tables->fixed_size = p.u32();
                tables->sample_count = p.u32();
                if (tables->fixed_size == 0) {
                    tables->sizes.reserve(tables->sample_count);
                    for (uint32_t i = 0; i < tables->sample_count && p.ok; ++i)
                        tables->sizes.push_back(p.u32());
                }
                break;
            }
            case fourcc("stco"): {
                full_box(p);
                const uint32_t count = p.u32();
                tables->chunk_offsets.reserve(count);
                for (uint32_t i = 0; i < count && p.ok; ++i)
                    tables->chunk_offsets.push_back(p.u32());
                break;
            }
            case fourcc("co64"): {
                full_box(p);
                const uint32_t count = p.u32();
                tables->chunk_offsets.reserve(count);
                for (uint32_t i = 0; i < count && p.ok; ++i)
                    tables->chunk_offsets.push_back(p.u64());
                break;
            }
            case fourcc("stss"): {
                full_box(p);
                tables->has_stss = true;
                const uint32_t count = p.u32();
                tables->sync.reserve(count);
                for (uint32_t i = 0; i < count && p.ok; ++i)
                    tables->sync.push_back(p.u32());
                break;
            }
            default:
                break;
        }
        if (!p.ok) { r.fail(); return; }
    }
}

// Expands the accumulated tables into flat per-sample records.
bool expand_samples(const SampleTables& t, TrackInfo* track) {
    const uint32_t count = t.sample_count;
    track->samples.resize(count);
    if (count == 0) return true;

    // Sizes.
    for (uint32_t i = 0; i < count; ++i) {
        track->samples[i].size =
            t.fixed_size ? t.fixed_size
                         : (i < t.sizes.size() ? t.sizes[i] : 0);
    }

    // Offsets: walk chunks through the stsc runs.
    if (t.chunk_offsets.empty() || t.stsc.empty()) return false;
    uint32_t sample = 0;
    const size_t chunk_count = t.chunk_offsets.size();
    for (size_t chunk = 0; chunk < chunk_count && sample < count; ++chunk) {
        // Applicable stsc run: last with first_chunk <= chunk+1.
        uint32_t per_chunk = t.stsc[0].samples_per_chunk;
        for (const StscRun& run : t.stsc) {
            if (run.first_chunk <= chunk + 1) per_chunk = run.samples_per_chunk;
            else break;
        }
        uint64_t offset = t.chunk_offsets[chunk];
        for (uint32_t s = 0; s < per_chunk && sample < count; ++s, ++sample) {
            track->samples[sample].file_offset = offset;
            offset += track->samples[sample].size;
        }
    }
    if (sample != count) return false;

    // Timing.
    uint64_t dts = 0;
    uint32_t i = 0;
    for (const SttsRun& run : t.stts) {
        for (uint32_t k = 0; k < run.count && i < count; ++k, ++i) {
            track->samples[i].dts = dts;
            track->samples[i].duration = run.delta;
            dts += run.delta;
        }
    }
    i = 0;
    for (const CttsRun& run : t.ctts) {
        for (uint32_t k = 0; k < run.count && i < count; ++k, ++i)
            track->samples[i].cts_offset = run.offset;
    }

    // Keyframes.
    if (t.has_stss) {
        for (SampleInfo& s : track->samples) s.keyframe = false;
        for (uint32_t sync : t.sync)
            if (sync >= 1 && sync <= count) track->samples[sync - 1].keyframe = true;
    }
    return true;
}

void parse_trak(Reader& r, MovieInfo* movie) {
    TrackInfo track;
    SampleTables tables;

    Box box;
    while (next_box(r, &box)) {
        Reader& p = box.payload;
        if (box.type == fourcc("tkhd")) {
            const FullBox fb = full_box(p);
            if (fb.version == 1) p.skip(8 + 8);
            else p.skip(4 + 4);
            track.track_id = p.u32();
        } else if (box.type == fourcc("mdia")) {
            Box mbox;
            while (next_box(p, &mbox)) {
                Reader& mp = mbox.payload;
                if (mbox.type == fourcc("mdhd")) {
                    const FullBox fb = full_box(mp);
                    if (fb.version == 1) {
                        mp.skip(8 + 8);
                        track.timescale = mp.u32();
                        track.duration = mp.u64();
                    } else {
                        mp.skip(4 + 4);
                        track.timescale = mp.u32();
                        track.duration = mp.u32();
                    }
                } else if (mbox.type == fourcc("hdlr")) {
                    full_box(mp);
                    mp.skip(4);   // pre_defined
                    const uint32_t handler = mp.u32();
                    if (handler == fourcc("vide")) track.kind = TrackInfo::Kind::Video;
                    else if (handler == fourcc("soun")) track.kind = TrackInfo::Kind::Audio;
                } else if (mbox.type == fourcc("minf")) {
                    Box ibox;
                    while (next_box(mp, &ibox)) {
                        if (ibox.type == fourcc("stbl"))
                            parse_stbl(ibox.payload, &track, &tables);
                    }
                }
            }
        }
    }
    if (expand_samples(tables, &track))
        movie->tracks.push_back(std::move(track));
}

}  // namespace

const TrackInfo* MovieInfo::first_video() const {
    for (const TrackInfo& t : tracks)
        if (t.kind == TrackInfo::Kind::Video) return &t;
    return nullptr;
}

const TrackInfo* MovieInfo::first_audio() const {
    for (const TrackInfo& t : tracks)
        if (t.kind == TrackInfo::Kind::Audio) return &t;
    return nullptr;
}

bool parse_moov(const uint8_t* data, size_t size, MovieInfo* out,
                std::string* error) {
    *out = MovieInfo{};
    Reader top{data, size};
    Box moov;
    if (!next_box(top, &moov) || moov.type != fourcc("moov")) {
        if (error) *error = "not a moov box";
        return false;
    }
    Reader& r = moov.payload;
    Box box;
    while (next_box(r, &box)) {
        if (box.type == fourcc("mvhd")) {
            Reader& p = box.payload;
            const FullBox fb = full_box(p);
            if (fb.version == 1) {
                p.skip(8 + 8);
                out->timescale = p.u32();
                out->duration = p.u64();
            } else {
                p.skip(4 + 4);
                out->timescale = p.u32();
                out->duration = p.u32();
            }
        } else if (box.type == fourcc("trak")) {
            parse_trak(box.payload, out);
        }
    }
    if (!r.ok) {
        if (error) *error = "malformed moov";
        return false;
    }
    if (out->tracks.empty()) {
        if (error) *error = "no usable tracks";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- file IO

BmffFile::~BmffFile() { close(); }

void BmffFile::close() {
    if (file_) {
        std::fclose(static_cast<FILE*>(file_));
        file_ = nullptr;
    }
}

bool BmffFile::open(const std::filesystem::path& path, std::string* error) {
    close();
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        if (error) *error = "cannot open file";
        return false;
    }
    file_ = f;

    // Walk top-level boxes to find moov.
    std::vector<uint8_t> moov;
    for (;;) {
        uint8_t header[16];
        const int64_t box_start = _ftelli64(f);
        if (std::fread(header, 1, 8, f) != 8) break;
        uint64_t box_size = (static_cast<uint64_t>(header[0]) << 24) |
                            (header[1] << 16) | (header[2] << 8) | header[3];
        const uint32_t type = (static_cast<uint32_t>(header[4]) << 24) |
                              (header[5] << 16) | (header[6] << 8) | header[7];
        if (box_size == 1) {
            if (std::fread(header + 8, 1, 8, f) != 8) break;
            box_size = 0;
            for (int i = 8; i < 16; ++i)
                box_size = (box_size << 8) | header[i];
        } else if (box_size == 0) {
            _fseeki64(f, 0, SEEK_END);
            box_size = static_cast<uint64_t>(_ftelli64(f) - box_start);
            _fseeki64(f, box_start + 8, SEEK_SET);
        }
        if (box_size < 8) break;

        if (type == fourcc("moov")) {
            moov.resize(static_cast<size_t>(box_size));
            _fseeki64(f, box_start, SEEK_SET);
            if (std::fread(moov.data(), 1, moov.size(), f) != moov.size()) {
                if (error) *error = "truncated moov";
                close();
                return false;
            }
            break;
        }
        if (_fseeki64(f, box_start + static_cast<int64_t>(box_size), SEEK_SET) != 0)
            break;
    }

    if (moov.empty()) {
        if (error) *error = "no moov box (fragmented or truncated file?)";
        close();
        return false;
    }
    if (!parse_moov(moov.data(), moov.size(), &movie_, error)) {
        close();
        return false;
    }
    return true;
}

bool BmffFile::read_sample(const SampleInfo& sample, std::vector<uint8_t>& out) {
    if (!file_) return false;
    FILE* f = static_cast<FILE*>(file_);
    if (_fseeki64(f, static_cast<int64_t>(sample.file_offset), SEEK_SET) != 0)
        return false;
    out.resize(sample.size);
    return std::fread(out.data(), 1, out.size(), f) == out.size();
}

}  // namespace looks::media
