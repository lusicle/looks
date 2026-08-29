#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace looks::ui {

class LayoutArena {
public:
    explicit LayoutArena(size_t block_size = 256 * 1024)
        : block_size_(block_size) {}

    ~LayoutArena() {
        for (Block& b : blocks_) ::operator delete(b.data);
    }

    LayoutArena(const LayoutArena&) = delete;
    LayoutArena& operator=(const LayoutArena&) = delete;

    template <typename T>
    T* alloc(size_t count = 1) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "arena types are never destructed");
        if (count == 0) return nullptr;
        void* p = alloc_bytes(sizeof(T) * count, alignof(T));
        std::memset(p, 0, sizeof(T) * count);
        return static_cast<T*>(p);
    }

    const char* dup(const char* text, size_t length) {
        char* p = static_cast<char*>(alloc_bytes(length + 1, 1));
        std::memcpy(p, text, length);
        p[length] = 0;
        return p;
    }

    void reset() {
        for (Block& b : blocks_) b.used = 0;
        current_ = 0;
    }

private:
    struct Block {
        void* data = nullptr;
        size_t size = 0;
        size_t used = 0;
    };

    void* alloc_bytes(size_t size, size_t align) {
        for (;;) {
            if (current_ < blocks_.size()) {
                Block& b = blocks_[current_];
                size_t offset = (b.used + align - 1) & ~(align - 1);
                if (offset + size <= b.size) {
                    b.used = offset + size;
                    return static_cast<uint8_t*>(b.data) + offset;
                }
                ++current_;
                continue;
            }
            size_t block_size = block_size_;
            while (block_size < size + align) block_size *= 2;
            blocks_.push_back({::operator new(block_size), block_size, 0});
        }
    }

    std::vector<Block> blocks_;
    size_t block_size_;
    size_t current_ = 0;
};

}  // namespace looks::ui
