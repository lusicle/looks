// MSB-first bit order. Output is bit-exact on all platforms.

#pragma once

#include <cstdint>
#include <vector>

namespace looks::codec {

// out is valid only after finish().
class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}

    void put_bit(uint32_t bit) {
        acc_ = (acc_ << 1) | (bit & 1u);
        if (++filled_ == 8) {
            if (chunk_len_ == kChunk) spill();
            chunk_[chunk_len_++] = static_cast<uint8_t>(acc_);
            acc_ = 0;
            filled_ = 0;
        }
    }

    void put_bits(uint32_t value, int count) {   // count <= 32
        for (int i = count - 1; i >= 0; --i) put_bit((value >> i) & 1u);
    }

    void put_ue(uint32_t v) {
        const uint32_t x = v + 1;
        int bits = 0;
        while ((x >> bits) > 1) ++bits;
        for (int i = 0; i < bits; ++i) put_bit(0);
        put_bits(x, bits + 1);
    }

    void put_se(int32_t v) {
        put_ue(v > 0 ? static_cast<uint32_t>(v) * 2 - 1
                     : static_cast<uint32_t>(-v) * 2);
    }

    void finish() {
        while (filled_ != 0) put_bit(0);
        spill();
    }

private:
    static constexpr size_t kChunk = 4096;

    void spill() {
        if (chunk_len_) out_.insert(out_.end(), chunk_, chunk_ + chunk_len_);
        chunk_len_ = 0;
    }

    std::vector<uint8_t>& out_;
    uint8_t chunk_[kChunk];
    size_t chunk_len_ = 0;
    uint32_t acc_ = 0;
    int filled_ = 0;
};

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool ok() const { return ok_; }

    uint32_t get_bit() {
        const size_t byte = pos_ >> 3;
        if (byte >= size_) {
            ok_ = false;
            return 0;
        }
        const uint32_t bit = (data_[byte] >> (7 - (pos_ & 7))) & 1u;
        ++pos_;
        return bit;
    }

    uint32_t get_bits(int count) {
        uint32_t v = 0;
        for (int i = 0; i < count; ++i) v = (v << 1) | get_bit();
        return v;
    }

    uint32_t get_ue() {
        int zeros = 0;
        while (ok_ && get_bit() == 0) {
            if (++zeros > 32) {
                ok_ = false;
                return 0;
            }
        }
        uint32_t x = 1;
        for (int i = 0; i < zeros; ++i) x = (x << 1) | get_bit();
        return x - 1;
    }

    int32_t get_se() {
        const uint32_t u = get_ue();
        return (u & 1) ? static_cast<int32_t>((u + 1) / 2)
                       : -static_cast<int32_t>(u / 2);
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace looks::codec
