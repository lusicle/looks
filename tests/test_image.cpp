#include "util/image.h"
#include "util/inflate.h"
#include "util/file.h"
#include "media/import.h"
#include "media/export.h"
#include "media/thumbs.h"
#include "codec/mez.h"
#include "platform/win/wic_image.h"

#include "test_framework.h"

using namespace looks;

TEST(gif_disposal_timing_and_lossless_cache) {
    std::vector<uint8_t> gif{'G','I','F','8','9','a',2,0,1,0,0x81,3,0,
        255,0,0, 0,255,0, 0,0,255, 0,0,0};
    auto frame = [&](uint8_t x, uint8_t color, uint8_t disposal, uint8_t delay, bool gce) {
        if (gce) gif.insert(gif.end(), {0x21,0xf9,4,uint8_t((disposal << 2) | 1),delay,0,3,0});
        gif.insert(gif.end(), {0x2c,x,0,0,0,1,0,1,0,0,2,2,uint8_t(0x44 + color * 8),1,0});
    };
    frame(0, 0, 1, 2, true);
    frame(1, 1, 3, 5, true);
    frame(0, 2, 2, 0, true);
    frame(1, 1, 0, 0, false);
    gif.push_back(0x3b);
    const std::filesystem::path fixture = std::filesystem::path(LOOKS_REPO_ROOT) / "temp/gif_stage_fixture.gif";
    CHECK(write_file_bytes(fixture, gif.data(), gif.size()));
    std::string error;
    std::vector<std::vector<uint8_t>> frames;
    CHECK(platform::decode_gif(gif.data(), gif.size(), [&](const platform::AnimationFrame& f) {
        CHECK_EQ(f.width, 2u);
        CHECK_EQ(f.height, 1u);
        CHECK_EQ(f.count, 4u);
        CHECK_EQ(f.delay_ms, f.index == 0 ? 20u : (f.index == 1 ? 50u : 100u));
        frames.emplace_back(f.rgba, f.rgba + 8);
        return true;
    }, &error));
    CHECK_EQ(frames.size(), size_t{4});
    if (frames.size() != 4) return;
    CHECK_EQ(frames[0][0], 255);
    CHECK_EQ(frames[0][7], 0);
    CHECK_EQ(frames[1][0], 255);
    CHECK_EQ(frames[1][5], 255);
    CHECK_EQ(frames[2][2], 255);
    CHECK_EQ(frames[2][7], 0);
    CHECK_EQ(frames[3][3], 0);
    CHECK_EQ(frames[3][5], 255);
    const auto imported = media::import_media(fixture, fixture.parent_path() / "gif_stage_cache");
    CHECK(imported.ok);
    CHECK_EQ(imported.frame_count, 27u);
    codec::MezReader reader;
    CHECK(reader.open(imported.mez_path, &error));
    CHECK(reader.rgba());
    CHECK(reader.animated());
    CHECK_EQ(reader.payload_offset(0), reader.payload_offset(1));
    const uint32_t ticks[] = {0, 2, 7, 17};
    for (size_t i = 0; i < 4; ++i) {
        codec::DecodedFrame decoded;
        CHECK(reader.decode(ticks[i], decoded));
        CHECK(decoded.rgba == frames[i]);
    }
    media::ThumbStripData thumbs;
    CHECK(media::read_thumbs(imported.thumbs_path, &thumbs));
    CHECK(!thumbs.alpha.empty());
}

TEST(native_png_and_gif_export) {
    const auto dir = std::filesystem::path(LOOKS_REPO_ROOT) / "temp";
    const uint8_t first[] = {255,0,0,255, 0,0,0,0};
    const uint8_t second[] = {0,0,255,255, 0,255,0,255};
    std::string error;
    const auto png = dir / "native_png_export.png";
    CHECK(platform::encode_png(png, first, 2, 1, &error));
    ImageRgba read;
    CHECK(load_image(png, &read, &error));
    CHECK(read.pixels == std::vector<uint8_t>(first, first + 8));
    platform::GifEncoder encoder;
    const auto gif = dir / "native_gif_export.gif";
    CHECK(encoder.open(gif, 2, 1, 0, &error));
    CHECK(encoder.add(first, 2, 16, false, true, 50, &error));
    CHECK(encoder.add(second, 5, 16, false, true, 50, &error));
    CHECK(encoder.finish(&error));
    const auto bytes = read_file_bytes(gif);
    CHECK(bytes.has_value());
    if (!bytes) return;
    uint32_t count = 0;
    CHECK(platform::decode_gif(bytes->data(), bytes->size(), [&](const platform::AnimationFrame& frame) {
        CHECK_EQ(frame.count, 2u);
        CHECK_EQ(frame.delay_ms, frame.index == 0 ? 20u : 50u);
        const auto* expected = frame.index == 0 ? first : second;
        for (size_t i = 0; i < 8; ++i) CHECK_EQ(frame.rgba[i], expected[i]);
        ++count;
        return true;
    }, &error));
    CHECK_EQ(count, 2u);
}

TEST(image_export_jobs_preserve_pixels_timing_and_cancel) {
    const auto dir = std::filesystem::path(LOOKS_REPO_ROOT) / "temp";
    media::RenderSettings settings;
    settings.format = media::RenderFormat::PngSequence;
    media::ExportProgress progress;
    auto producer = [](uint32_t frame, std::vector<uint8_t>& pixels) {
        pixels.resize(65 * 37 * 4);
        for (size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i] = frame == 0 ? 255 : 0;
            pixels[i + 1] = frame == 1 ? 255 : 0;
            pixels[i + 2] = frame == 2 ? 255 : 0;
            pixels[i + 3] = i % 12 == 0 ? 0 : 255;
        }
        return true;
    };
    const auto path = dir / "image_job.png";
    CHECK(media::export_images(65, 37, 30, 1, 3, producer, path, settings, true, &progress).ok);
    CHECK_EQ(progress.frames_done.load(), 3u);
    for (uint32_t f = 0; f < 3; ++f) {
        ImageRgba image;
        CHECK(load_image(media::image_output_path(path, settings.format, f), &image));
        CHECK_EQ(image.width, 65u);
        CHECK_EQ(image.height, 37u);
        std::vector<uint8_t> expected;
        producer(f, expected);
        CHECK(image.pixels == expected);
    }
    settings.format = media::RenderFormat::Gif;
    settings.gif_colors = 4;
    settings.gif_dither = false;
    const auto gif_path = dir / "image_job.gif";
    CHECK(media::export_images(65, 37, 30, 1, 3, producer, gif_path, settings, true, &progress).ok);
    const auto bytes = read_file_bytes(gif_path);
    CHECK(bytes.has_value());
    if (bytes) {
        uint32_t duration = 0, count = 0;
        CHECK(platform::decode_gif(bytes->data(), bytes->size(), [&](const platform::AnimationFrame& frame) {
            ++count;
            duration += frame.delay_ms;
            CHECK_EQ(frame.width, 65u);
            CHECK_EQ(frame.height, 37u);
            CHECK_EQ(frame.rgba[3], 0u);
            CHECK_EQ(frame.rgba[4 + frame.index], 255u);
            return true;
        }, nullptr));
        CHECK_EQ(duration, 100u);
        CHECK_EQ(count, 3u);
    }
    progress.cancel.store(true);
    uint32_t produced = 0;
    const auto cancelled = media::export_images(65, 37, 30, 1, 3,
        [&](uint32_t frame, std::vector<uint8_t>& pixels) { ++produced; return producer(frame, pixels); },
        dir / "cancelled_image_job.gif", settings, true, &progress);
    CHECK(!cancelled.ok);
    CHECK_EQ(produced, 0u);
    settings.width = 65;
    settings.height = 37;
    settings.fps = 24;
    const auto preset = media::render_settings_json(settings);
    CHECK(!preset.find("source"));
    CHECK(!preset.find("first"));
    CHECK(!preset.find("path"));
    media::RenderSettings loaded;
    std::string error;
    CHECK(media::read_render_settings(preset, loaded, error));
    CHECK(media::render_settings_json(loaded) == preset);
}

namespace {

// 6x4 RGBA PNG from GDI+: dynamic Huffman plus ancillary chunks.
const uint8_t kFixturePng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x04,
    0x08, 0x06, 0x00, 0x00, 0x00, 0xAD, 0x04, 0x4E, 0x43, 0x00, 0x00, 0x00,
    0x01, 0x73, 0x52, 0x47, 0x42, 0x00, 0xAE, 0xCE, 0x1C, 0xE9, 0x00, 0x00,
    0x00, 0x04, 0x67, 0x41, 0x4D, 0x41, 0x00, 0x00, 0xB1, 0x8F, 0x0B, 0xFC,
    0x61, 0x05, 0x00, 0x00, 0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00,
    0x0E, 0xC3, 0x00, 0x00, 0x0E, 0xC3, 0x01, 0xC7, 0x6F, 0xA8, 0x64, 0x00,
    0x00, 0x00, 0x4F, 0x49, 0x44, 0x41, 0x54, 0x18, 0x57, 0x0D, 0xC9, 0xC1,
    0x00, 0x00, 0x31, 0x0C, 0x45, 0xC1, 0x40, 0x14, 0x22, 0x10, 0x1F, 0x22,
    0x10, 0x85, 0xF8, 0xC7, 0x05, 0x28, 0x44, 0x20, 0x02, 0x51, 0xAB, 0xB7,
    0x9D, 0xEB, 0xC4, 0x43, 0xC6, 0xA2, 0x22, 0x71, 0x88, 0x8E, 0xE2, 0xC6,
    0x26, 0x42, 0x8B, 0x54, 0x52, 0x12, 0x56, 0xD1, 0xDA, 0x5C, 0xF9, 0x85,
    0x93, 0xB4, 0x28, 0x17, 0xF6, 0xA6, 0x6D, 0xAE, 0xCF, 0x8B, 0x11, 0x39,
    0x45, 0xCD, 0xC6, 0x63, 0x7A, 0x0E, 0x77, 0xFA, 0xFB, 0x01, 0x5B, 0xF9,
    0x30, 0xBA, 0xE4, 0x17, 0xEA, 0xFA, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
    0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

}  // namespace

TEST(inflate_stored_block) {
    const uint8_t stream[] = {0x01, 0x05, 0x00, 0xFA, 0xFF,
                              'h', 'e', 'l', 'l', 'o'};
    std::vector<uint8_t> out;
    CHECK(inflate(stream, sizeof(stream), out));
    CHECK_EQ(out.size(), size_t{5});
    CHECK(std::memcmp(out.data(), "hello", 5) == 0);
}

TEST(inflate_fixed_huffman_literal) {
    // BFINAL=1, BTYPE=01, literal 'A' (code 0x71), EOB (7 zero bits).
    const uint8_t stream[] = {0x73, 0x04, 0x00};
    std::vector<uint8_t> out;
    CHECK(inflate(stream, sizeof(stream), out));
    CHECK_EQ(out.size(), size_t{1});
    CHECK_EQ(out[0], uint8_t{'A'});
}

TEST(inflate_checksums) {
    const uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
    CHECK_EQ(adler32(hello, 5), 0x062C0215u);
    const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK_EQ(crc32(digits, 9), 0xCBF43926u);   // standard check value
}

TEST(png_decode_fixture) {
    ImageRgba image;
    std::string error;
    CHECK(decode_png(kFixturePng, sizeof(kFixturePng), &image, &error));
    CHECK_EQ(image.width, 6u);
    CHECK_EQ(image.height, 4u);
    CHECK_EQ(image.pixels.size(), size_t{6 * 4 * 4});
    // Fixture pixel (x,y) is {x*40, y*60, (x+y)*20, 255}.
    auto px = [&](uint32_t x, uint32_t y) {
        return image.pixels.data() + (y * 6 + x) * 4;
    };
    CHECK_EQ(px(0, 0)[0], 0);
    CHECK_EQ(px(0, 0)[3], 255);
    CHECK_EQ(px(3, 2)[0], 120);
    CHECK_EQ(px(3, 2)[1], 120);
    CHECK_EQ(px(3, 2)[2], 100);
    CHECK_EQ(px(4, 1)[0], 160);
    CHECK_EQ(px(4, 1)[1], 60);
    CHECK_EQ(px(4, 1)[2], 100);
    CHECK_EQ(px(5, 3)[3], 128);   // the one translucent pixel
}

TEST(png_rejects_garbage) {
    ImageRgba image;
    const uint8_t junk[32] = {1, 2, 3};
    CHECK(!decode_png(junk, sizeof(junk), &image));
    // Corrupt a byte inside IDAT: CRC must catch it.
    std::vector<uint8_t> bad(kFixturePng, kFixturePng + sizeof(kFixturePng));
    bad[100] ^= 0xFF;
    CHECK(!decode_png(bad.data(), bad.size(), &image));
}

TEST(tga_decode_uncompressed_and_rle) {
    // 2x2 24-bit uncompressed, bottom-left origin. Rows bottom-up:
    // row0(bottom): blue, green; row1(top): red, white  (BGR order).
    const uint8_t tga[] = {
        0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 24, 0,
        255, 0, 0,  0, 255, 0,
        0, 0, 255,  255, 255, 255
    };
    ImageRgba image;
    CHECK(decode_tga(tga, sizeof(tga), &image));
    CHECK_EQ(image.width, 2u);
    CHECK_EQ(image.height, 2u);
    // Output is top-down: (0,0) = red.
    CHECK_EQ(image.pixels[0], 255);
    CHECK_EQ(image.pixels[1], 0);
    CHECK_EQ(image.pixels[2], 0);
    const uint8_t* p11 = image.pixels.data() + (1 * 2 + 1) * 4;
    CHECK_EQ(p11[0], 0);
    CHECK_EQ(p11[1], 255);
    CHECK_EQ(p11[2], 0);

    // RLE: 4 identical magenta pixels (32-bit, top origin).
    const uint8_t rle[] = {
        0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 32, 0x20,
        0x83, 255, 0, 255, 200
    };
    ImageRgba rle_image;
    CHECK(decode_tga(rle, sizeof(rle), &rle_image));
    CHECK_EQ(rle_image.width, 2u);
    for (int i = 0; i < 4; ++i) {
        const uint8_t* p = rle_image.pixels.data() + i * 4;
        CHECK_EQ(p[0], 255);
        CHECK_EQ(p[1], 0);
        CHECK_EQ(p[2], 255);
        CHECK_EQ(p[3], 200);
    }
}

TEST(png_encode_decode_roundtrip) {
    // The gradient and alpha ramp send every byte value to the encoder.
    // The odd size makes the stored-block split and the row filters uneven.
    const uint32_t w = 131, h = 67;
    std::vector<uint8_t> src(size_t{w} * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = src.data() + (size_t{y} * w + x) * 4;
            p[0] = static_cast<uint8_t>(x * 2);
            p[1] = static_cast<uint8_t>(y * 3);
            p[2] = static_cast<uint8_t>(x + y);
            p[3] = static_cast<uint8_t>(255 - (x % 255));
        }
    const std::vector<uint8_t> png =
        looks::encode_png_rgba(src.data(), w, h);
    CHECK(!png.empty());
    ImageRgba back;
    std::string err;
    CHECK(decode_png(png.data(), png.size(), &back, &err));
    CHECK_EQ(back.width, w);
    CHECK_EQ(back.height, h);
    CHECK(back.pixels == src);
}

TEST(thumbnails_preserve_linear_mean) {
    const auto dir = std::filesystem::path(LOOKS_REPO_ROOT) / "temp";
    const auto path = dir / "thumbnail_linear_check.png";
    std::vector<uint8_t> rgba(320 * 180 * 4, 255);
    for (uint32_t y = 0; y < 180; ++y)
        for (uint32_t x = 0; x < 320; ++x)
            for (int c = 0; c < 3; ++c)
                rgba[(y * 320 + x) * 4 + c] = (x + y) % 2 ? 255 : 0;
    const auto png = encode_png_rgba(rgba.data(), 320, 180);
    CHECK(write_file_bytes(path, png.data(), png.size()));
    media::ImportOptions options;
    options.quality = 0;
    options.proxy = false;
    const auto result = media::import_media(path, dir, options);
    CHECK(result.ok);
    media::ThumbStripData strip;
    CHECK(media::read_thumbs(result.thumbs_path, &strip));
    CHECK(strip.linear_filtered);
    CHECK_EQ(strip.count, 1u);
    CHECK_EQ(strip.w, 160u);
    CHECK_EQ(strip.h, 90u);
    for (uint8_t value : strip.rgb) CHECK(value >= 187 && value <= 189);
    media::ThumbStripData head;
    CHECK(media::read_thumbs_header(result.thumbs_path, &head));
    CHECK(head.linear_filtered);
    CHECK(media::rebuild_still_thumbs(result.mez_path, result.thumbs_path));
    media::ThumbStripData rebuilt;
    CHECK(media::read_thumbs(result.thumbs_path, &rebuilt));
    CHECK(rebuilt.linear_filtered);
    CHECK(rebuilt.rgb == strip.rgb);
}
