#include <cmath>
#include <cstring>
#include <filesystem>

#include "media/bmff.h"
#include "media/bmff_mux.h"
#include "media/h264_util.h"
#include "media/import.h"
#include "media/mp3.h"
#include "media/pcm.h"
#include "media/wav.h"
#include "test_framework.h"

using namespace looks::media;

TEST(wav_roundtrip) {
    const auto path = std::filesystem::temp_directory_path() / "looks_t.wav";
    std::vector<int16_t> samples(2 * 480);
    for (size_t i = 0; i < 480; ++i) {
        samples[i * 2] = static_cast<int16_t>(
            20000.0 * std::sin(2.0 * 3.14159265 * 440.0 * i / 48000.0));
        samples[i * 2 + 1] = static_cast<int16_t>(i * 13 % 32768);
    }
    std::string error;
    CHECK(write_wav(path, samples.data(), 480, 2, 48000, &error));

    WavData back;
    CHECK(read_wav(path, &back, &error));
    CHECK_EQ(back.channels, 2u);
    CHECK_EQ(back.sample_rate, 48000u);
    CHECK_EQ(back.frame_count(), uint64_t{480});
    bool identical = true;
    for (size_t i = 0; i < samples.size(); ++i)
        identical = identical && back.samples[i] == samples[i];
    CHECK(identical);
    std::filesystem::remove(path);
}

TEST(wav_imports_as_audio_only_bundle) {
    const auto dir = std::filesystem::temp_directory_path() / "looks_wavimp";
    std::filesystem::create_directories(dir);
    const auto src = dir / "tone.wav";
    std::vector<int16_t> samples(48000);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            12000.0 * std::sin(2.0 * 3.14159265 * 220.0 * i / 48000.0));
    std::string error;
    CHECK(write_wav(src, samples.data(), samples.size(), 1, 48000, &error));

    const ImportResult r = import_media(src, dir);
    CHECK(r.ok);
    CHECK(r.mez_path.empty());
    CHECK(!r.pcm_path.empty());
    CHECK_EQ(r.frame_count, 0u);
    CHECK_EQ(r.width, 0u);
    CHECK_EQ(r.audio_channels, 1u);
    CHECK_EQ(r.audio_sample_rate, 48000u);
    CHECK_EQ(r.audio_frames, uint64_t{48000});

    {
        PcmReader pcm;
        CHECK(pcm.open(r.pcm_path, &error));
        CHECK_EQ(pcm.frame_count(), uint64_t{48000});
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(mp3_cover_art_parses_id3v2_apic) {
    // The fixture is a synthetic ID3v2.3 tag: one TIT2 frame, then APIC.
    const uint8_t img[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};
    std::vector<uint8_t> apic;
    apic.push_back(0);                        // text encoding: latin-1
    const char* mime = "image/jpeg";
    apic.insert(apic.end(), mime, mime + std::strlen(mime) + 1);
    apic.push_back(3);                        // picture type: front cover
    apic.push_back(0);                        // empty description
    apic.insert(apic.end(), img, img + 5);

    std::vector<uint8_t> tag = {'I', 'D', '3', 3, 0, 0};
    auto push_be32 = [&](uint32_t v) {
        tag.push_back(static_cast<uint8_t>(v >> 24));
        tag.push_back(static_cast<uint8_t>(v >> 16));
        tag.push_back(static_cast<uint8_t>(v >> 8));
        tag.push_back(static_cast<uint8_t>(v));
    };
    auto push_syncsafe = [&](uint32_t v) {
        tag.push_back(static_cast<uint8_t>((v >> 21) & 0x7F));
        tag.push_back(static_cast<uint8_t>((v >> 14) & 0x7F));
        tag.push_back(static_cast<uint8_t>((v >> 7) & 0x7F));
        tag.push_back(static_cast<uint8_t>(v & 0x7F));
    };
    const uint32_t body =
        10 + 5 + 10 + static_cast<uint32_t>(apic.size());
    push_syncsafe(body);
    tag.insert(tag.end(), {'T', 'I', 'T', '2'});
    push_be32(5);
    tag.push_back(0);
    tag.push_back(0);
    tag.insert(tag.end(), {0, 't', 'e', 's', 't'});
    tag.insert(tag.end(), {'A', 'P', 'I', 'C'});
    push_be32(static_cast<uint32_t>(apic.size()));
    tag.push_back(0);
    tag.push_back(0);
    tag.insert(tag.end(), apic.begin(), apic.end());

    std::vector<uint8_t> out;
    CHECK(mp3_cover_art(tag.data(), tag.size(), &out));
    CHECK_EQ(out.size(), size_t{5});
    CHECK(!std::memcmp(out.data(), img, 5));

    std::vector<uint8_t> plain = {'I', 'D', '3', 3, 0, 0, 0, 0, 0, 0};
    CHECK(!mp3_cover_art(plain.data(), plain.size(), &out));
    CHECK(!mp3_cover_art(img, 5, &out));
}

TEST(mp3_decode_refuses_garbage) {
    Mp3Data data;
    std::string error;
    std::vector<uint8_t> junk(4096, 0xAB);
    CHECK(!decode_mp3(junk.data(), junk.size(), &data, &error));
    CHECK(!decode_mp3(nullptr, 0, &data, &error));
}

TEST(mux_demux_roundtrip) {
    const auto path = std::filesystem::temp_directory_path() / "looks_mux.mp4";

    MuxVideoParams video;
    video.width = 640;
    video.height = 360;
    video.timescale = 90000;
    video.avcc = {1, 0x4D, 0x40, 0x1E, 0xFF, 0xE1, 0, 2, 0x67, 0x42,
                  1, 0, 2, 0x68, 0xCE};

    MuxAudioParams audio;
    audio.channels = 2;
    audio.sample_rate = 48000;
    audio.audio_specific_config = {0x12, 0x10};

    std::vector<std::vector<uint8_t>> video_payloads;
    std::vector<std::vector<uint8_t>> audio_payloads;
    {
        BmffMuxer muxer;
        CHECK(muxer.open(path, video, &audio));
        for (uint32_t i = 0; i < 5; ++i) {
            std::vector<uint8_t> payload(40 + i * 13);
            for (size_t k = 0; k < payload.size(); ++k)
                payload[k] = static_cast<uint8_t>(i * 31 + k);
            CHECK(muxer.add_video_sample(payload.data(), payload.size(),
                                         i * 3000ull, 3000, 0, i % 3 == 0));
            video_payloads.push_back(std::move(payload));
        }
        for (uint32_t i = 0; i < 4; ++i) {
            std::vector<uint8_t> payload(20 + i * 7, static_cast<uint8_t>(0xA0 + i));
            CHECK(muxer.add_audio_sample(payload.data(), payload.size(),
                                         i * 1024ull, 1024));
            audio_payloads.push_back(std::move(payload));
        }
        CHECK(muxer.finish());
    }

    BmffFile file;
    std::string error;
    CHECK(file.open(path, &error));
    const MovieInfo& movie = file.movie();
    CHECK_EQ(movie.timescale, 1000u);
    CHECK_EQ(movie.tracks.size(), size_t{2});

    const TrackInfo* v = movie.first_video();
    CHECK(v != nullptr);
    CHECK_EQ(std::string(v->fourcc), "avc1");
    CHECK_EQ(v->width, 640u);
    CHECK_EQ(v->height, 360u);
    CHECK_EQ(v->timescale, 90000u);
    CHECK(v->avcc == video.avcc);
    CHECK_EQ(v->samples.size(), size_t{5});
    CHECK_EQ(v->samples[1].dts, uint64_t{3000});
    CHECK_EQ(v->samples[1].duration, 3000u);
    CHECK(v->samples[0].keyframe);
    CHECK(!v->samples[1].keyframe);
    CHECK(v->samples[3].keyframe);

    const TrackInfo* a = movie.first_audio();
    CHECK(a != nullptr);
    CHECK_EQ(std::string(a->fourcc), "mp4a");
    CHECK_EQ(a->channels, 2u);
    CHECK_EQ(a->sample_rate, 48000u);
    CHECK(a->audio_specific_config == audio.audio_specific_config);
    CHECK_EQ(a->samples.size(), size_t{4});

    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < video_payloads.size(); ++i) {
        CHECK(file.read_sample(v->samples[i], bytes));
        CHECK(bytes == video_payloads[i]);
    }
    for (size_t i = 0; i < audio_payloads.size(); ++i) {
        CHECK(file.read_sample(a->samples[i], bytes));
        CHECK(bytes == audio_payloads[i]);
    }

    file.close();
    std::filesystem::remove(path);
}

TEST(h264_annexb_avcc_utils) {
    // The fixture mixes 3-byte and 4-byte start codes.
    const std::vector<uint8_t> annexb = {
        0, 0, 0, 1, 0x67, 0x4D, 0x40, 0x1E,   // SPS
        0, 0, 1, 0x68, 0xCE, 0x06,            // PPS
        0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21,   // IDR slice
    };
    std::vector<uint8_t> sample, sps, pps;
    const bool idr = annexb_to_avcc_sample(annexb.data(), annexb.size(),
                                           sample, &sps, &pps);
    CHECK(idr);
    CHECK_EQ(sps.size(), size_t{4});
    CHECK_EQ(sps[0], uint8_t{0x67});
    CHECK_EQ(pps.size(), size_t{3});
    // Sample holds only the IDR NAL, 4-byte length prefixed.
    CHECK_EQ(sample.size(), size_t{4 + 4});
    CHECK_EQ(sample[3], uint8_t{4});
    CHECK_EQ(sample[4], uint8_t{0x65});

    const std::vector<uint8_t> avcc = build_avcc(sps, pps);
    CHECK(!avcc.empty());
    CHECK_EQ(avcc[0], uint8_t{1});
    CHECK_EQ(avcc[1], uint8_t{0x4D});      // profile from SPS
    CHECK_EQ(avcc[4] & 0x3, 0x3);          // 4-byte lengths
}
