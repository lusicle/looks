// BMFF demuxer tests over a synthetic moov fixture built box-by-box.

#include <vector>

#include "media/bmff.h"
#include "test_framework.h"

using namespace looks::media;

namespace {

struct Bytes {
    std::vector<uint8_t> v;

    void u8(uint32_t x) { v.push_back(static_cast<uint8_t>(x)); }
    void u16(uint32_t x) { u8(x >> 8); u8(x); }
    void u32(uint32_t x) { u16(x >> 16); u16(x); }
    void raw(std::initializer_list<uint8_t> bytes) {
        v.insert(v.end(), bytes.begin(), bytes.end());
    }
    void append(const Bytes& other) {
        v.insert(v.end(), other.v.begin(), other.v.end());
    }
    void zeros(size_t n) { v.insert(v.end(), n, 0); }
};

Bytes box(const char* type, const Bytes& payload) {
    Bytes b;
    b.u32(static_cast<uint32_t>(8 + payload.v.size()));
    b.u8(static_cast<uint8_t>(type[0]));
    b.u8(static_cast<uint8_t>(type[1]));
    b.u8(static_cast<uint8_t>(type[2]));
    b.u8(static_cast<uint8_t>(type[3]));
    b.append(payload);
    return b;
}

Bytes full_payload() {   // version 0, flags 0
    Bytes b;
    b.u32(0);
    return b;
}

Bytes make_video_trak() {
    Bytes tkhd = full_payload();
    tkhd.zeros(8);        // creation + modification
    tkhd.u32(1);          // track id

    Bytes mdhd = full_payload();
    mdhd.zeros(8);
    mdhd.u32(12800);      // timescale
    mdhd.u32(2048);       // duration

    Bytes hdlr = full_payload();
    hdlr.u32(0);
    hdlr.raw({'v', 'i', 'd', 'e'});

    // avc1 sample entry: 78 bytes then avcC.
    Bytes avc1;
    avc1.zeros(6);
    avc1.u16(1);          // data reference index
    avc1.zeros(16);
    avc1.u16(640);
    avc1.u16(360);
    avc1.zeros(50);
    Bytes avcc;
    avcc.raw({1, 0x64, 0x00, 0x28, 0xFF, 0xE1});
    avc1.append(box("avcC", avcc));

    Bytes stsd = full_payload();
    stsd.u32(1);
    stsd.append(box("avc1", avc1));

    Bytes stts = full_payload();
    stts.u32(1);
    stts.u32(4);          // 4 samples
    stts.u32(512);        // delta

    Bytes ctts = full_payload();
    ctts.u32(2);
    ctts.u32(2); ctts.u32(1024);
    ctts.u32(2); ctts.u32(0);

    Bytes stsc = full_payload();
    stsc.u32(1);
    stsc.u32(1); stsc.u32(2); stsc.u32(1);   // 2 samples per chunk

    Bytes stsz = full_payload();
    stsz.u32(0);          // no fixed size
    stsz.u32(4);
    stsz.u32(100); stsz.u32(200); stsz.u32(300); stsz.u32(400);

    Bytes stco = full_payload();
    stco.u32(2);
    stco.u32(1000); stco.u32(2000);

    Bytes stss = full_payload();
    stss.u32(1);
    stss.u32(1);          // only sample 1 is sync

    Bytes stbl;
    stbl.append(box("stsd", stsd));
    stbl.append(box("stts", stts));
    stbl.append(box("ctts", ctts));
    stbl.append(box("stsc", stsc));
    stbl.append(box("stsz", stsz));
    stbl.append(box("stco", stco));
    stbl.append(box("stss", stss));

    Bytes minf;
    minf.append(box("stbl", stbl));

    Bytes mdia;
    mdia.append(box("mdhd", mdhd));
    mdia.append(box("hdlr", hdlr));
    mdia.append(box("minf", minf));

    Bytes trak;
    trak.append(box("tkhd", tkhd));
    trak.append(box("mdia", mdia));
    return box("trak", trak);
}

Bytes make_audio_trak() {
    Bytes tkhd = full_payload();
    tkhd.zeros(8);
    tkhd.u32(2);

    Bytes mdhd = full_payload();
    mdhd.zeros(8);
    mdhd.u32(48000);
    mdhd.u32(3072);

    Bytes hdlr = full_payload();
    hdlr.u32(0);
    hdlr.raw({'s', 'o', 'u', 'n'});

    // mp4a v0 entry: 28 bytes then esds.
    Bytes mp4a;
    mp4a.zeros(6);
    mp4a.u16(1);
    mp4a.u16(0);          // version
    mp4a.zeros(6);
    mp4a.u16(2);          // channels
    mp4a.u16(16);         // sample size
    mp4a.zeros(4);
    mp4a.u32(48000u << 16);
    Bytes esds = full_payload();
    esds.raw({0x03, 22, 0x00, 0x02, 0x00});          // ES_Descr, len, ES_ID, flags
    esds.raw({0x04, 17, 0x40});                       // DecoderConfig, objType AAC
    esds.raw({0x15, 0x00, 0x00, 0x00});               // streamType/buffer
    esds.raw({0x00, 0x01, 0xF4, 0x00});               // max bitrate
    esds.raw({0x00, 0x01, 0xF4, 0x00});               // avg bitrate
    esds.raw({0x05, 2, 0x12, 0x10});                  // DecoderSpecificInfo: ASC
    mp4a.append(box("esds", esds));

    Bytes stsd = full_payload();
    stsd.u32(1);
    stsd.append(box("mp4a", mp4a));

    Bytes stts = full_payload();
    stts.u32(1);
    stts.u32(3);
    stts.u32(1024);

    Bytes stsc = full_payload();
    stsc.u32(1);
    stsc.u32(1); stsc.u32(3); stsc.u32(1);

    Bytes stsz = full_payload();
    stsz.u32(0);
    stsz.u32(3);
    stsz.u32(10); stsz.u32(20); stsz.u32(30);

    Bytes stco = full_payload();
    stco.u32(1);
    stco.u32(5000);

    Bytes stbl;
    stbl.append(box("stsd", stsd));
    stbl.append(box("stts", stts));
    stbl.append(box("stsc", stsc));
    stbl.append(box("stsz", stsz));
    stbl.append(box("stco", stco));

    Bytes minf;
    minf.append(box("stbl", stbl));

    Bytes mdia;
    mdia.append(box("mdhd", mdhd));
    mdia.append(box("hdlr", hdlr));
    mdia.append(box("minf", minf));

    Bytes trak;
    trak.append(box("tkhd", tkhd));
    trak.append(box("mdia", mdia));
    return box("trak", trak);
}

Bytes make_moov() {
    Bytes mvhd = full_payload();
    mvhd.zeros(8);
    mvhd.u32(1000);       // movie timescale
    mvhd.u32(160);        // duration

    Bytes moov;
    moov.append(box("mvhd", mvhd));
    moov.append(make_video_trak());
    moov.append(make_audio_trak());
    return box("moov", moov);
}

}  // namespace

TEST(bmff_parse_synthetic_moov) {
    const Bytes moov = make_moov();
    MovieInfo movie;
    std::string error;
    CHECK(parse_moov(moov.v.data(), moov.v.size(), &movie, &error));
    CHECK_EQ(movie.timescale, 1000u);
    CHECK_EQ(movie.duration, uint64_t{160});
    CHECK_EQ(movie.tracks.size(), size_t{2});

    const TrackInfo* video = movie.first_video();
    CHECK(video != nullptr);
    CHECK_EQ(video->track_id, 1u);
    CHECK_EQ(video->timescale, 12800u);
    CHECK_EQ(std::string(video->fourcc), "avc1");
    CHECK_EQ(video->width, 640u);
    CHECK_EQ(video->height, 360u);
    CHECK_EQ(video->avcc.size(), size_t{6});
    CHECK_EQ(video->avcc[0], uint8_t{1});
    CHECK_EQ(video->avcc[1], uint8_t{0x64});

    const TrackInfo* audio = movie.first_audio();
    CHECK(audio != nullptr);
    CHECK_EQ(audio->timescale, 48000u);
    CHECK_EQ(std::string(audio->fourcc), "mp4a");
    CHECK_EQ(audio->channels, 2u);
    CHECK_EQ(audio->sample_rate, 48000u);
    CHECK_EQ(audio->audio_specific_config.size(), size_t{2});
    CHECK_EQ(audio->audio_specific_config[0], uint8_t{0x12});
    CHECK_EQ(audio->audio_specific_config[1], uint8_t{0x10});
}

TEST(bmff_sample_table_expansion) {
    const Bytes moov = make_moov();
    MovieInfo movie;
    CHECK(parse_moov(moov.v.data(), moov.v.size(), &movie, nullptr));
    const TrackInfo* video = movie.first_video();
    CHECK_EQ(video->samples.size(), size_t{4});

    // Offsets: chunk 1 @1000 holds samples 0,1; chunk 2 @2000 holds 2,3.
    CHECK_EQ(video->samples[0].file_offset, uint64_t{1000});
    CHECK_EQ(video->samples[0].size, 100u);
    CHECK_EQ(video->samples[1].file_offset, uint64_t{1100});
    CHECK_EQ(video->samples[2].file_offset, uint64_t{2000});
    CHECK_EQ(video->samples[3].file_offset, uint64_t{2300});

    // Timing: dts advances by 512; ctts runs {2x1024, 2x0}.
    CHECK_EQ(video->samples[0].dts, uint64_t{0});
    CHECK_EQ(video->samples[3].dts, uint64_t{1536});
    CHECK_EQ(video->samples[1].duration, 512u);
    CHECK_EQ(video->samples[0].cts_offset, int64_t{1024});
    CHECK_EQ(video->samples[2].cts_offset, int64_t{0});

    // stss: only the first sample is a keyframe.
    CHECK(video->samples[0].keyframe);
    CHECK(!video->samples[1].keyframe);
    CHECK(!video->samples[3].keyframe);

    // Audio: no stss -> everything is a sync sample.
    const TrackInfo* audio = movie.first_audio();
    CHECK_EQ(audio->samples.size(), size_t{3});
    CHECK(audio->samples[2].keyframe);
    CHECK_EQ(audio->samples[0].file_offset, uint64_t{5000});
    CHECK_EQ(audio->samples[1].file_offset, uint64_t{5010});
    CHECK_EQ(audio->samples[2].file_offset, uint64_t{5030});
}

TEST(bmff_rejects_garbage) {
    MovieInfo movie;
    std::string error;
    const uint8_t junk[] = {0, 0, 0, 8, 'f', 'r', 'e', 'e'};
    CHECK(!parse_moov(junk, sizeof(junk), &movie, &error));
    CHECK(!error.empty());
    CHECK(!parse_moov(nullptr, 0, &movie, &error));
    // Truncated moov: size field larger than the buffer.
    const uint8_t truncated[] = {0, 0, 1, 0, 'm', 'o', 'o', 'v', 0, 0};
    CHECK(!parse_moov(truncated, sizeof(truncated), &movie, &error));
}
