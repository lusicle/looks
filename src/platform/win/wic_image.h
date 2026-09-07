#pragma once

#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace looks::platform {

struct AnimationFrame {
    uint32_t width = 0, height = 0;
    uint32_t index = 0, count = 0, delay_ms = 0;
    const uint8_t* rgba = nullptr;
};

bool decode_gif(const uint8_t* bytes, size_t size,
                const std::function<bool(const AnimationFrame&)>& accept,
                std::string* error);

bool encode_png(const std::filesystem::path& path, const uint8_t* rgba,
                 uint32_t width, uint32_t height, std::string* error);

class GifEncoder {
public:
    GifEncoder();
    ~GifEncoder();
    bool open(const std::filesystem::path& path, uint32_t width, uint32_t height,
              uint32_t loops, std::string* error);
    bool add(const uint8_t* rgba, uint32_t delay_cs, uint32_t colors,
             bool dither, bool alpha, float alpha_threshold, std::string* error);
    bool finish(std::string* error);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The output is tightly packed RGBA8.
bool decode_image_rgba(const uint8_t* bytes, size_t size, uint32_t* width,
                       uint32_t* height, std::vector<uint8_t>* rgba,
                       std::string* error);

}  // namespace looks::platform
