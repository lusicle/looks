#include "codec/core.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace looks::codec {

const uint8_t kZigzag[kBlockCoeffs] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

const uint8_t kQuantBaseLuma[kBlockCoeffs] = {
    16, 11, 10, 16, 24,  40,  51,  61,
    12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,
    14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,
    24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99};

const uint8_t kQuantBaseChroma[kBlockCoeffs] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99};

void build_quant_table(const uint8_t* base, int quality,
                       uint16_t out[kBlockCoeffs]) {
    quality = std::clamp(quality, 1, 100);
    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for (int i = 0; i < kBlockCoeffs; ++i) {
        const int q = (base[i] * scale + 50) / 100;
        out[i] = static_cast<uint16_t>(std::clamp(q, 1, 255));
    }
}

namespace {

// Keep hardcoded: runtime cos can differ across CRTs and change frames.
constexpr int32_t kDctFwd[kBlockSize][kBlockSize] = {
    {2896, 2896, 2896, 2896, 2896, 2896, 2896, 2896},
    {4017, 3406, 2276, 799, -799, -2276, -3406, -4017},
    {3784, 1567, -1567, -3784, -3784, -1567, 1567, 3784},
    {3406, -799, -4017, -2276, 2276, 4017, 799, -3406},
    {2896, -2896, -2896, 2896, 2896, -2896, -2896, 2896},
    {2276, -4017, 799, 3406, -3406, -799, 4017, -2276},
    {1567, -3784, 3784, -1567, -1567, 3784, -3784, 1567},
    {799, -2276, 3406, -4017, 4017, -3406, 2276, -799}};

// The folded passes must equal the plain matrix product bit for bit.

inline int16_t round13(int32_t acc) {
    return static_cast<int16_t>(std::clamp((acc + 4096) >> 13, -32768, 32767));
}

inline void fwd_pass8(const int16_t* in, int in_stride, int16_t* out,
                      int out_stride) {
    int32_t s[4], d[4];
    for (int j = 0; j < 4; ++j) {
        const int32_t a = in[j * in_stride];
        const int32_t b = in[(7 - j) * in_stride];
        s[j] = a + b;
        d[j] = a - b;
    }
    out[0 * out_stride] = round13(2896 * (s[0] + s[1] + s[2] + s[3]));
    out[4 * out_stride] = round13(2896 * ((s[0] + s[3]) - (s[1] + s[2])));
    out[2 * out_stride] = round13(3784 * (s[0] - s[3]) + 1567 * (s[1] - s[2]));
    out[6 * out_stride] = round13(1567 * (s[0] - s[3]) - 3784 * (s[1] - s[2]));
    out[1 * out_stride] =
        round13(4017 * d[0] + 3406 * d[1] + 2276 * d[2] + 799 * d[3]);
    out[3 * out_stride] =
        round13(3406 * d[0] - 799 * d[1] - 4017 * d[2] - 2276 * d[3]);
    out[5 * out_stride] =
        round13(2276 * d[0] - 4017 * d[1] + 799 * d[2] + 3406 * d[3]);
    out[7 * out_stride] =
        round13(799 * d[0] - 2276 * d[1] + 3406 * d[2] - 4017 * d[3]);
}

inline void inv_pass8(const int16_t* in, int in_stride, int16_t* out,
                      int out_stride) {
    const int32_t v0 = in[0 * in_stride], v1 = in[1 * in_stride];
    const int32_t v2 = in[2 * in_stride], v3 = in[3 * in_stride];
    const int32_t v4 = in[4 * in_stride], v5 = in[5 * in_stride];
    const int32_t v6 = in[6 * in_stride], v7 = in[7 * in_stride];
    const int32_t e0 = 2896 * (v0 + v4) + 3784 * v2 + 1567 * v6;
    const int32_t e1 = 2896 * (v0 - v4) + 1567 * v2 - 3784 * v6;
    const int32_t e2 = 2896 * (v0 - v4) - 1567 * v2 + 3784 * v6;
    const int32_t e3 = 2896 * (v0 + v4) - 3784 * v2 - 1567 * v6;
    const int32_t o0 = 4017 * v1 + 3406 * v3 + 2276 * v5 + 799 * v7;
    const int32_t o1 = 3406 * v1 - 799 * v3 - 4017 * v5 - 2276 * v7;
    const int32_t o2 = 2276 * v1 - 4017 * v3 + 799 * v5 + 3406 * v7;
    const int32_t o3 = 799 * v1 - 2276 * v3 + 3406 * v5 - 4017 * v7;
    out[0 * out_stride] = round13(e0 + o0);
    out[7 * out_stride] = round13(e0 - o0);
    out[1 * out_stride] = round13(e1 + o1);
    out[6 * out_stride] = round13(e1 - o1);
    out[2 * out_stride] = round13(e2 + o2);
    out[5 * out_stride] = round13(e2 - o2);
    out[3 * out_stride] = round13(e3 + o3);
    out[4 * out_stride] = round13(e3 - o3);
}

}  // namespace

void fdct8x8(int16_t block[kBlockCoeffs]) {
    int16_t tmp[kBlockCoeffs];
    for (int u = 0; u < kBlockSize; ++u)
        fwd_pass8(block + u * kBlockSize, 1, tmp + u * kBlockSize, 1);
    for (int k = 0; k < kBlockSize; ++k)
        fwd_pass8(tmp + k, kBlockSize, block + k, kBlockSize);
}

void idct8x8(int16_t block[kBlockCoeffs]) {
    int16_t tmp[kBlockCoeffs];
    for (int u = 0; u < kBlockSize; ++u)
        inv_pass8(block + u * kBlockSize, 1, tmp + u * kBlockSize, 1);
    for (int k = 0; k < kBlockSize; ++k)
        inv_pass8(tmp + k, kBlockSize, block + k, kBlockSize);
}

void quantize(const int16_t in[kBlockCoeffs], const uint16_t qtab[kBlockCoeffs],
              int16_t out[kBlockCoeffs]) {
    for (int i = 0; i < kBlockCoeffs; ++i) {
        const int32_t v = in[i];
        const int32_t q = qtab[i];
        const int32_t mag = (std::abs(v) + q / 2) / q;
        out[i] = static_cast<int16_t>(v < 0 ? -mag : mag);
    }
}

void dequantize(const int16_t in[kBlockCoeffs], const uint16_t qtab[kBlockCoeffs],
                int16_t out[kBlockCoeffs]) {
    for (int i = 0; i < kBlockCoeffs; ++i)
        out[i] = static_cast<int16_t>(
            std::clamp(in[i] * qtab[i], -32768, 32767));
}

namespace {
constexpr uint32_t kEobRun = 63;
}

void encode_block(BitWriter& bw, const int16_t block[kBlockCoeffs],
                  int16_t* dc_pred) {
    bw.put_se(block[0] - *dc_pred);
    *dc_pred = block[0];

    int last_nonzero = 0;
    for (int i = 1; i < kBlockCoeffs; ++i)
        if (block[kZigzag[i]] != 0) last_nonzero = i;

    int run = 0;
    for (int i = 1; i <= last_nonzero; ++i) {
        const int16_t v = block[kZigzag[i]];
        if (v == 0) {
            ++run;
            continue;
        }
        bw.put_ue(static_cast<uint32_t>(run));
        const uint32_t mag = static_cast<uint32_t>(std::abs(v));
        bw.put_ue(mag - 1);
        bw.put_bit(v < 0 ? 1u : 0u);
        run = 0;
    }
    bw.put_ue(kEobRun);
}

uint32_t ue_bit_count(uint32_t v) {
    const uint32_t x = v + 1;
    int bits = 0;
    while ((x >> bits) > 1) ++bits;
    return static_cast<uint32_t>(2 * bits + 1);
}

uint32_t se_bit_count(int32_t v) {
    return ue_bit_count(v > 0 ? static_cast<uint32_t>(v) * 2 - 1
                              : static_cast<uint32_t>(-v) * 2);
}

uint32_t ac_bit_count(const int16_t block[kBlockCoeffs]) {
    int last_nonzero = 0;
    for (int i = 1; i < kBlockCoeffs; ++i)
        if (block[kZigzag[i]] != 0) last_nonzero = i;
    uint32_t bits = 0;
    int run = 0;
    for (int i = 1; i <= last_nonzero; ++i) {
        const int16_t v = block[kZigzag[i]];
        if (v == 0) {
            ++run;
            continue;
        }
        bits += ue_bit_count(static_cast<uint32_t>(run));
        bits += ue_bit_count(static_cast<uint32_t>(std::abs(v)) - 1) + 1;
        run = 0;
    }
    return bits + ue_bit_count(kEobRun);
}

bool decode_block(BitReader& br, int16_t block[kBlockCoeffs], int16_t* dc_pred) {
    std::memset(block, 0, sizeof(int16_t) * kBlockCoeffs);
    const int32_t dc = *dc_pred + br.get_se();
    block[0] = static_cast<int16_t>(dc);
    *dc_pred = static_cast<int16_t>(dc);

    int i = 1;
    for (;;) {
        const uint32_t run = br.get_ue();
        if (!br.ok()) return false;
        if (run == kEobRun) break;
        i += static_cast<int>(run);
        if (i >= kBlockCoeffs) return false;
        const uint32_t mag = br.get_ue() + 1;
        const uint32_t sign = br.get_bit();
        if (!br.ok() || mag > 32767) return false;
        block[kZigzag[i]] =
            static_cast<int16_t>(sign ? -static_cast<int32_t>(mag) : mag);
        ++i;
    }
    return true;
}

void encode_pixel_block(BitWriter& bw, const uint8_t* src, size_t stride,
                        int avail_w, int avail_h,
                        const uint16_t qtab[kBlockCoeffs], int16_t* dc_pred) {
    int16_t block[kBlockCoeffs];
    extract_dct_block(src, stride, avail_w, avail_h, block);
    int16_t quantized[kBlockCoeffs];
    quantize(block, qtab, quantized);
    encode_block(bw, quantized, dc_pred);
}

void extract_dct_block(const uint8_t* src, size_t stride, int avail_w,
                       int avail_h, int16_t out[kBlockCoeffs]) {
    for (int y = 0; y < kBlockSize; ++y) {
        const int sy = std::min(y, avail_h - 1);
        for (int x = 0; x < kBlockSize; ++x) {
            const int sx = std::min(x, avail_w - 1);
            out[y * kBlockSize + x] =
                static_cast<int16_t>(src[sy * stride + sx] - 128);
        }
    }
    fdct8x8(out);
}

void residual_dct_block(const uint8_t* cur, size_t cur_stride,
                        const uint8_t* pred, size_t pred_stride, int avail_w,
                        int avail_h, int16_t out[kBlockCoeffs]) {
    for (int y = 0; y < kBlockSize; ++y) {
        const int cy = std::min(y, avail_h - 1);
        for (int x = 0; x < kBlockSize; ++x) {
            const int cx = std::min(x, avail_w - 1);
            out[y * kBlockSize + x] = static_cast<int16_t>(
                static_cast<int>(cur[cy * cur_stride + cx]) -
                static_cast<int>(pred[cy * pred_stride + cx]));
        }
    }
    fdct8x8(out);
}

namespace {

// Results must not depend on which worker runs which chunk.
class BlockPool {
public:
    static BlockPool& instance() {
        static BlockPool pool;
        return pool;
    }

    void run(const std::function<void(int, int)>& fn, int count, int chunk) {
        std::lock_guard<std::mutex> outer(run_mutex_);
        ensure_workers();
        {
            std::lock_guard<std::mutex> lock(m_);
            fn_ = &fn;
            count_ = count;
            chunk_ = chunk;
            next_.store(0, std::memory_order_relaxed);
            active_ = static_cast<int>(workers_.size());
            ++generation_;
        }
        cv_.notify_all();
        work(fn, count, chunk);   // The calling thread also does work.
        std::unique_lock<std::mutex> lock(m_);
        done_cv_.wait(lock, [this] { return active_ == 0; });
        fn_ = nullptr;
    }

private:
    ~BlockPool() {
        {
            std::lock_guard<std::mutex> lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (std::thread& t : workers_) t.join();
    }

    void ensure_workers() {
        if (!workers_.empty()) return;
        const int n = static_cast<int>(std::min<unsigned>(
            std::max(1u, std::thread::hardware_concurrency()), 16u));
        for (int t = 1; t < n; ++t)
            workers_.emplace_back([this] { worker_loop(); });
    }

    void work(const std::function<void(int, int)>& fn, int count, int chunk) {
        for (;;) {
            const int c = next_.fetch_add(1, std::memory_order_relaxed);
            const int begin = c * chunk;
            if (begin >= count) break;
            fn(begin, std::min(count, begin + chunk));
        }
    }

    void worker_loop() {
        uint64_t seen = 0;
        for (;;) {
            const std::function<void(int, int)>* fn = nullptr;
            int count = 0, chunk = 0;
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait(lock, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
                fn = fn_;
                count = count_;
                chunk = chunk_;
            }
            work(*fn, count, chunk);
            {
                std::lock_guard<std::mutex> lock(m_);
                if (--active_ == 0) done_cv_.notify_all();
            }
        }
    }

    std::mutex run_mutex_;   // serializes concurrent run() callers
    std::mutex m_;
    std::condition_variable cv_, done_cv_;
    std::vector<std::thread> workers_;
    const std::function<void(int, int)>* fn_ = nullptr;
    std::atomic<int> next_{0};
    int count_ = 0;
    int chunk_ = 0;
    int active_ = 0;
    uint64_t generation_ = 0;
    bool stop_ = false;
};

}  // namespace

void parallel_blocks(int count, bool parallel,
                     const std::function<void(int, int)>& fn) {
    if (count <= 0) return;
    const int threads = parallel
        ? static_cast<int>(std::min<unsigned>(
              std::max(1u, std::thread::hardware_concurrency()), 16u))
        : 1;
    if (threads <= 1 || count < threads * 4) {
        fn(0, count);
        return;
    }
    const int chunk = (count + threads - 1) / threads;
    BlockPool::instance().run(fn, count, chunk);
}

void parallel_tasks(int count, const std::function<void(int)>& fn) {
    if (count <= 0) return;
    if (count == 1) {
        fn(0);
        return;
    }
    const std::function<void(int, int)> body = [&fn](int begin, int end) {
        for (int t = begin; t < end; ++t) fn(t);
    };
    BlockPool::instance().run(body, count, 1);
}

bool decode_pixel_block(BitReader& br, uint8_t* dst, size_t stride,
                        int avail_w, int avail_h,
                        const uint16_t qtab[kBlockCoeffs], int16_t* dc_pred) {
    int16_t quantized[kBlockCoeffs];
    if (!decode_block(br, quantized, dc_pred)) return false;
    int16_t block[kBlockCoeffs];
    dequantize(quantized, qtab, block);
    idct8x8(block);
    const int copy_w = std::min(kBlockSize, avail_w);
    const int copy_h = std::min(kBlockSize, avail_h);
    for (int y = 0; y < copy_h; ++y) {
        for (int x = 0; x < copy_w; ++x) {
            const int v = block[y * kBlockSize + x] + 128;
            dst[y * stride + x] = static_cast<uint8_t>(std::clamp(v, 0, 255));
        }
    }
    return true;
}

}  // namespace looks::codec
