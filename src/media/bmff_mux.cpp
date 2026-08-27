#include "media/bmff_mux.h"

#include <cstdio>
#include <cstring>

#include "media/bmff.h"
#include "util/bytes.h"
#include "util/log.h"

namespace looks::media {

namespace {

// Big-endian building on the shared writer; tag() is the only mux-local
// vocabulary.
struct Bytes : bytes::BeWriter {
    void tag(const char* s) { raw(s, 4); }
};

Bytes box(const char* type, const Bytes& payload) {
    Bytes b;
    b.u32(static_cast<uint32_t>(8 + payload.v.size()));
    b.tag(type);
    b.append(payload);
    return b;
}

Bytes full_box(const char* type, uint8_t version, uint32_t flags,
               const Bytes& payload) {
    Bytes p;
    p.u8(version);
    p.u8(flags >> 16);
    p.u8(flags >> 8);
    p.u8(flags);
    p.append(payload);
    return box(type, p);
}

void write_matrix_identity(Bytes& b) {
    b.u32(0x00010000); b.u32(0); b.u32(0);
    b.u32(0); b.u32(0x00010000); b.u32(0);
    b.u32(0); b.u32(0); b.u32(0x40000000);
}

// MP4 descriptor over the shared tag + varlen coding (bmff.h): minimal
// length form; parsers accept both minimal and padded.
Bytes descriptor(uint8_t desc_tag, const Bytes& payload) {
    Bytes b;
    b.u8(desc_tag);
    esds_write_len(b.v, payload.v.size());
    b.append(payload);
    return b;
}

Bytes build_esds(const MuxAudioParams& audio) {
    Bytes dsi;
    dsi.raw(audio.audio_specific_config.data(),
            audio.audio_specific_config.size());

    Bytes dcd;                        // DecoderConfigDescriptor
    dcd.u8(0x40);                     // objectTypeIndication: AAC LC
    dcd.u8(0x15);                     // streamType audio, upStream 0, reserved 1
    dcd.u8(0); dcd.u16(0);            // bufferSizeDB (24-bit)
    dcd.u32(audio.avg_bitrate);       // maxBitrate
    dcd.u32(audio.avg_bitrate);       // avgBitrate
    dcd.append(descriptor(kEsdsTagDecoderSpecific, dsi));

    Bytes es;                         // ES_Descriptor
    es.u16(0);                        // ES_ID
    es.u8(0);                         // flags
    es.append(descriptor(kEsdsTagDecoderConfig, dcd));
    Bytes sl;
    sl.u8(0x02);                      // SLConfig: MP4
    es.append(descriptor(kEsdsTagSLConfig, sl));

    Bytes payload;
    payload.append(descriptor(kEsdsTagES, es));
    return full_box("esds", 0, 0, payload);
}

}  // namespace

BmffMuxer::~BmffMuxer() {
    if (temp_file_) std::fclose(static_cast<FILE*>(temp_file_));
    if (!finished_ && !temp_path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(temp_path_, ec);
    }
}

bool BmffMuxer::open(const std::filesystem::path& path,
                     const MuxVideoParams& video,
                     const MuxAudioParams* audio) {
    path_ = path;
    temp_path_ = path;
    temp_path_ += L".mdat.tmp";
    video_ = video;
    if (audio) {
        audio_ = *audio;
        has_audio_ = true;
    }
    FILE* f = _wfopen(temp_path_.c_str(), L"w+b");
    if (!f) return false;
    temp_file_ = f;
    return true;
}

bool BmffMuxer::append_payload(const uint8_t* data, size_t size,
                               uint64_t* offset) {
    if (!temp_file_ || finished_) return false;
    *offset = mdat_bytes_;
    if (std::fwrite(data, 1, size, static_cast<FILE*>(temp_file_)) != size)
        return false;
    mdat_bytes_ += size;
    return true;
}

bool BmffMuxer::add_video_sample(const uint8_t* data, size_t size,
                                 uint64_t dts, uint32_t duration,
                                 int32_t cts_offset, bool keyframe) {
    uint64_t offset = 0;
    if (!append_payload(data, size, &offset)) return false;
    video_track_.samples.push_back({offset, static_cast<uint32_t>(size), dts,
                                    duration, cts_offset, keyframe});
    return true;
}

bool BmffMuxer::add_audio_sample(const uint8_t* data, size_t size,
                                 uint64_t dts, uint32_t duration) {
    if (!has_audio_) return false;
    uint64_t offset = 0;
    if (!append_payload(data, size, &offset)) return false;
    audio_track_.samples.push_back({offset, static_cast<uint32_t>(size), dts,
                                    duration, 0, true});
    return true;
}

bool BmffMuxer::finish() {
    if (!temp_file_ || finished_ || video_track_.samples.empty()) return false;
    std::fflush(static_cast<FILE*>(temp_file_));

    constexpr uint32_t kMovieTimescale = 1000;
    const uint64_t video_dur_ms =
        video_.timescale
            ? video_track_.total_duration() * kMovieTimescale / video_.timescale
            : 0;
    const uint64_t audio_dur_ms =
        has_audio_ && audio_.sample_rate
            ? audio_track_.total_duration() * kMovieTimescale / audio_.sample_rate
            : 0;
    const uint64_t movie_dur_ms =
        video_dur_ms > audio_dur_ms ? video_dur_ms : audio_dur_ms;

    // ---- ftyp
    Bytes ftyp_payload;
    ftyp_payload.tag("isom");
    ftyp_payload.u32(0x200);
    ftyp_payload.tag("isom");
    ftyp_payload.tag("iso2");
    ftyp_payload.tag("avc1");
    ftyp_payload.tag("mp41");
    const Bytes ftyp = box("ftyp", ftyp_payload);

    // ---- moov built with PROVISIONAL chunk offsets (relative to mdat
    // payload); every stco entry is later shifted by the final header size.
    // The moov size itself is offset-independent (fixed-width u32 entries),
    // so one build pass + one patch pass suffices.
    auto build_stbl_tables = [](const Track& track, bool with_ctts,
                                bool with_stss) {
        // Run-length compress a per-sample field into count/value pairs
        // (stts durations and ctts offsets share the coding).
        auto rle_pairs = [&track](auto field) {
            std::vector<std::pair<uint32_t, int64_t>> runs;
            for (const Sample& s : track.samples) {
                const int64_t v = field(s);
                if (!runs.empty() && runs.back().second == v)
                    ++runs.back().first;
                else
                    runs.push_back({1, v});
            }
            Bytes p;
            p.u32(static_cast<uint32_t>(runs.size()));
            for (auto [n, v] : runs) {
                p.u32(n);
                p.u32(static_cast<uint32_t>(v));
            }
            return p;
        };
        Bytes stbl;

        stbl.append(full_box("stts", 0, 0, rle_pairs([](const Sample& s) {
            return static_cast<int64_t>(s.duration);
        })));

        if (with_ctts) {
            bool any = false;
            for (const Sample& s : track.samples)
                if (s.cts_offset != 0) any = true;
            if (any)
                stbl.append(
                    full_box("ctts", 1, 0, rle_pairs([](const Sample& s) {
                        return static_cast<int64_t>(s.cts_offset);
                    })));
        }

        {
            Bytes p;   // one sample per chunk
            p.u32(1);
            p.u32(1);
            p.u32(1);
            p.u32(1);
            stbl.append(full_box("stsc", 0, 0, p));
        }
        {
            Bytes p;
            p.u32(0);
            p.u32(static_cast<uint32_t>(track.samples.size()));
            for (const Sample& s : track.samples) p.u32(s.size);
            stbl.append(full_box("stsz", 0, 0, p));
        }
        {
            Bytes p;
            p.u32(static_cast<uint32_t>(track.samples.size()));
            for (const Sample& s : track.samples)
                p.u32(static_cast<uint32_t>(s.mdat_offset));   // patched later
            stbl.append(full_box("stco", 0, 0, p));
        }
        if (with_stss) {
            uint32_t count = 0;
            for (const Sample& s : track.samples)
                if (s.keyframe) ++count;
            if (count < track.samples.size()) {
                Bytes p;
                p.u32(count);
                for (uint32_t i = 0; i < track.samples.size(); ++i)
                    if (track.samples[i].keyframe) p.u32(i + 1);
                stbl.append(full_box("stss", 0, 0, p));
            }
        }
        return stbl;
    };

    auto build_trak = [&](bool is_video, uint32_t track_id) {
        const Track& track = is_video ? video_track_ : audio_track_;
        const uint32_t timescale =
            is_video ? video_.timescale : audio_.sample_rate;
        const uint64_t dur_ms = is_video ? video_dur_ms : audio_dur_ms;

        Bytes tkhd_p;
        tkhd_p.u32(0);   // creation
        tkhd_p.u32(0);   // modification
        tkhd_p.u32(track_id);
        tkhd_p.u32(0);   // reserved
        tkhd_p.u32(static_cast<uint32_t>(dur_ms));
        tkhd_p.zeros(8);
        tkhd_p.u16(0);   // layer
        tkhd_p.u16(0);   // alternate group
        tkhd_p.u16(is_video ? 0 : 0x0100);   // volume
        tkhd_p.u16(0);
        write_matrix_identity(tkhd_p);
        tkhd_p.u32(is_video ? video_.width << 16 : 0);
        tkhd_p.u32(is_video ? video_.height << 16 : 0);
        const Bytes tkhd = full_box("tkhd", 0, 3, tkhd_p);   // enabled+in movie

        Bytes mdhd_p;
        mdhd_p.u32(0);
        mdhd_p.u32(0);
        mdhd_p.u32(timescale);
        mdhd_p.u32(static_cast<uint32_t>(track.total_duration()));
        mdhd_p.u16(0x55C4);   // language 'und'
        mdhd_p.u16(0);
        const Bytes mdhd = full_box("mdhd", 0, 0, mdhd_p);

        Bytes hdlr_p;
        hdlr_p.u32(0);
        hdlr_p.tag(is_video ? "vide" : "soun");
        hdlr_p.zeros(12);
        hdlr_p.raw(is_video ? "looksVideo\0" : "looksAudio\0", 11);
        const Bytes hdlr = full_box("hdlr", 0, 0, hdlr_p);

        Bytes stsd_entry;
        if (is_video) {
            Bytes e;
            e.zeros(6);
            e.u16(1);          // data reference index
            e.zeros(16);
            e.u16(video_.width);
            e.u16(video_.height);
            e.u32(0x00480000); // 72 dpi
            e.u32(0x00480000);
            e.u32(0);
            e.u16(1);          // frame count
            e.zeros(32);       // compressor name
            e.u16(0x0018);     // depth
            e.u16(0xFFFF);     // pre_defined
            Bytes avcc;
            avcc.raw(video_.avcc.data(), video_.avcc.size());
            e.append(box("avcC", avcc));
            stsd_entry = box("avc1", e);
        } else {
            Bytes e;
            e.zeros(6);
            e.u16(1);
            e.zeros(8);        // version/revision/vendor
            e.u16(audio_.channels);
            e.u16(16);
            e.zeros(4);
            e.u32(audio_.sample_rate << 16);
            e.append(build_esds(audio_));
            stsd_entry = box("mp4a", e);
        }
        Bytes stsd_p;
        stsd_p.u32(1);
        stsd_p.append(stsd_entry);
        const Bytes stsd = full_box("stsd", 0, 0, stsd_p);

        Bytes stbl_p;
        stbl_p.append(stsd);
        stbl_p.append(build_stbl_tables(track, is_video, is_video));
        const Bytes stbl = box("stbl", stbl_p);

        Bytes media_header;   // vmhd or smhd
        if (is_video) {
            Bytes p;
            p.u16(0);          // graphics mode
            p.zeros(6);        // opcolor
            media_header = full_box("vmhd", 0, 1, p);
        } else {
            Bytes p;
            p.u16(0);          // balance
            p.u16(0);
            media_header = full_box("smhd", 0, 0, p);
        }

        Bytes url_p;
        const Bytes url = full_box("url ", 0, 1, url_p);   // self-contained
        Bytes dref_p;
        dref_p.u32(1);
        dref_p.append(url);
        Bytes dinf_p;
        dinf_p.append(full_box("dref", 0, 0, dref_p));
        const Bytes dinf = box("dinf", dinf_p);

        Bytes minf_p;
        minf_p.append(media_header);
        minf_p.append(dinf);
        minf_p.append(stbl);
        const Bytes minf = box("minf", minf_p);

        Bytes mdia_p;
        mdia_p.append(mdhd);
        mdia_p.append(hdlr);
        mdia_p.append(minf);
        const Bytes mdia = box("mdia", mdia_p);

        Bytes trak_p;
        trak_p.append(tkhd);
        trak_p.append(mdia);
        return box("trak", trak_p);
    };

    Bytes mvhd_p;
    mvhd_p.u32(0);
    mvhd_p.u32(0);
    mvhd_p.u32(kMovieTimescale);
    mvhd_p.u32(static_cast<uint32_t>(movie_dur_ms));
    mvhd_p.u32(0x00010000);   // rate
    mvhd_p.u16(0x0100);       // volume
    mvhd_p.u16(0);
    mvhd_p.zeros(8);
    write_matrix_identity(mvhd_p);
    mvhd_p.zeros(24);         // pre_defined
    mvhd_p.u32(has_audio_ ? 3 : 2);   // next track id
    const Bytes mvhd = full_box("mvhd", 0, 0, mvhd_p);

    Bytes moov_p;
    moov_p.append(mvhd);
    moov_p.append(build_trak(true, 1));
    if (has_audio_ && !audio_track_.samples.empty())
        moov_p.append(build_trak(false, 2));
    Bytes moov = box("moov", moov_p);

    // ---- patch stco: absolute offset = header sizes + relative offset.
    const uint64_t mdat_payload_start = ftyp.v.size() + moov.v.size() + 8;
    {
        // Find every stco box inside moov and shift its entries. Boxes were
        // built by us, so a linear scan for the "stco" tag + size walk is
        // safe and simple.
        std::vector<uint8_t>& m = moov.v;
        for (size_t i = 0; i + 8 <= m.size(); ++i) {
            if (std::memcmp(&m[i], "stco", 4) != 0) continue;
            // i points at the tag; entries start at tag+4(fullbox)+4(count).
            const size_t count_pos = i + 4 + 4;
            if (count_pos + 4 > m.size()) continue;
            const uint32_t count = bytes::be32(&m[count_pos]);
            size_t entry = count_pos + 4;
            if (entry + static_cast<size_t>(count) * 4 > m.size()) continue;
            for (uint32_t k = 0; k < count; ++k, entry += 4) {
                bytes::put_be32(&m[entry],
                                static_cast<uint32_t>(bytes::be32(&m[entry]) +
                                                      mdat_payload_start));
            }
        }
    }

    // ---- final file: ftyp + moov + mdat(payload streamed from temp)
    FILE* out = _wfopen(path_.c_str(), L"wb");
    if (!out) {
        log_warn("mux: cannot open %ls for write (errno %d)", path_.c_str(),
                 errno);
        return false;
    }
    bool ok = std::fwrite(ftyp.v.data(), 1, ftyp.v.size(), out) == ftyp.v.size();
    ok = ok && std::fwrite(moov.v.data(), 1, moov.v.size(), out) == moov.v.size();
    uint8_t mdat_header[8];
    bytes::put_be32(mdat_header, static_cast<uint32_t>(mdat_bytes_ + 8));
    std::memcpy(mdat_header + 4, "mdat", 4);
    ok = ok && std::fwrite(mdat_header, 1, 8, out) == 8;

    FILE* temp = static_cast<FILE*>(temp_file_);
    _fseeki64(temp, 0, SEEK_SET);
    std::vector<uint8_t> chunk(1 << 20);
    uint64_t remaining = mdat_bytes_;
    while (ok && remaining > 0) {
        const size_t n = static_cast<size_t>(
            remaining < chunk.size() ? remaining : chunk.size());
        if (std::fread(chunk.data(), 1, n, temp) != n) { ok = false; break; }
        if (std::fwrite(chunk.data(), 1, n, out) != n) { ok = false; break; }
        remaining -= n;
    }
    if (!ok)
        log_warn("mux: final write failed with %llu bytes left (errno %d)",
                 static_cast<unsigned long long>(remaining), errno);
    std::fclose(out);
    std::fclose(temp);
    temp_file_ = nullptr;
    std::error_code ec;
    std::filesystem::remove(temp_path_, ec);
    finished_ = ok;
    return ok;
}

}  // namespace looks::media
