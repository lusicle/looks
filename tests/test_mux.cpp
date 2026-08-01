// Muxer <-> demuxer roundtrip: our BmffMuxer writes a file, our BmffFile
// parses it back — sample tables, avcC/esds, offsets, and payload bytes
// must survive. No codecs involved (payloads are arbitrary bytes).
// Also home to the WAV codec roundtrip.

#include <cmath>
#include <filesystem>

#include "media/bmff.h"
#include "media/bmff_mux.h"
#include "media/h264_util.h"
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

    // Payload bytes survive the trip (faststart offsets are correct).
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
    // Annex B: SPS + PPS + IDR with mixed 3/4-byte start codes.
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
