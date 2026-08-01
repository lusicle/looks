// Build-time blue-noise generator: void-and-cluster over a
// toroidal grid, 2D for the classic blue-noise tile and 3D for the STBN
// volume (each temporal slice is spatially blue; threshold sequences are
// blue along time). Fully deterministic — same binary output every build.
//
//   looks_bluenoise <out.bin>
//
// Output: packed R8 "dither LUT", 512x192:
//   rows 0..63    STBN 64x64x8   (slice s occupies x in [s*64, s*64+64))
//   rows 64..191  blue noise 128x128 (x in [0,128)); x >= 128 is filler
// Header: 'D','L','T','1', u32 width, u32 height, then width*height bytes.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// Deterministic LCG (never std::rand — implementation-defined).
struct Lcg {
    uint64_t state;
    explicit Lcg(uint64_t seed) : state(seed) {}
    uint32_t next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 33);
    }
};

struct Grid {
    int w = 0, h = 0, d = 0;
    std::vector<uint8_t> on;
    std::vector<double> energy;

    int size() const { return w * h * d; }
    int index(int x, int y, int z) const { return (z * h + y) * w + x; }
};

// Toroidal gaussian splat. Spatial sigma 1.9 (the classic choice), temporal
// sigma 1.1 so neighboring slices repel points at the same site.
void splat(Grid& g, int cx, int cy, int cz, double sign) {
    constexpr int kR = 6;
    constexpr double kSigmaXy = 1.9;
    constexpr double kSigmaT = 1.1;
    const int dz_lo = g.d == 1 ? 0 : -(g.d / 2);
    const int dz_hi = g.d == 1 ? 0 : (g.d - 1) / 2;
    for (int dz = dz_lo; dz <= dz_hi; ++dz) {
        const int z = ((cz + dz) % g.d + g.d) % g.d;
        const double ez = std::exp(-(dz * dz) / (2.0 * kSigmaT * kSigmaT));
        for (int dy = -kR; dy <= kR; ++dy) {
            const int y = ((cy + dy) % g.h + g.h) % g.h;
            for (int dx = -kR; dx <= kR; ++dx) {
                const int x = ((cx + dx) % g.w + g.w) % g.w;
                const double r2 = dx * dx + dy * dy;
                g.energy[g.index(x, y, z)] +=
                    sign * ez *
                    std::exp(-r2 / (2.0 * kSigmaXy * kSigmaXy));
            }
        }
    }
}

void coords(const Grid& g, int idx, int* x, int* y, int* z) {
    *x = idx % g.w;
    *y = (idx / g.w) % g.h;
    *z = idx / (g.w * g.h);
}

int find_extreme(const Grid& g, bool want_on, bool want_max) {
    int best = -1;
    double best_e = want_max ? -1.0e300 : 1.0e300;
    const int n = g.size();
    for (int i = 0; i < n; ++i) {
        if ((g.on[i] != 0) != want_on) continue;
        const double e = g.energy[i];
        if (want_max ? (e > best_e) : (e < best_e)) {
            best_e = e;
            best = i;
        }
    }
    return best;
}

// Full void-and-cluster ranking; returns rank per cell (0 = first point).
std::vector<int> rank_grid(int w, int h, int d, uint64_t seed) {
    Grid g;
    g.w = w;
    g.h = h;
    g.d = d;
    const int n = g.size();
    g.on.assign(static_cast<size_t>(n), 0);
    g.energy.assign(static_cast<size_t>(n), 0.0);

    // Seed pattern: ~10% of cells, then relax until stable.
    const int n0 = n / 10;
    Lcg rng(seed);
    {
        std::vector<int> order(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) order[static_cast<size_t>(i)] = i;
        for (int i = n - 1; i > 0; --i) {
            const int j = static_cast<int>(rng.next() %
                                           static_cast<uint32_t>(i + 1));
            const int t = order[static_cast<size_t>(i)];
            order[static_cast<size_t>(i)] = order[static_cast<size_t>(j)];
            order[static_cast<size_t>(j)] = t;
        }
        for (int i = 0; i < n0; ++i) {
            const int idx = order[static_cast<size_t>(i)];
            g.on[static_cast<size_t>(idx)] = 1;
            int x, y, z;
            coords(g, idx, &x, &y, &z);
            splat(g, x, y, z, 1.0);
        }
    }
    for (int iter = 0; iter < n; ++iter) {
        const int cluster = find_extreme(g, true, true);
        int x, y, z;
        coords(g, cluster, &x, &y, &z);
        g.on[static_cast<size_t>(cluster)] = 0;
        splat(g, x, y, z, -1.0);
        const int voidc = find_extreme(g, false, false);
        coords(g, voidc, &x, &y, &z);
        g.on[static_cast<size_t>(voidc)] = 1;
        splat(g, x, y, z, 1.0);
        if (voidc == cluster) break;   // stable
    }

    std::vector<int> rank(static_cast<size_t>(n), 0);
    const std::vector<uint8_t> initial = g.on;
    const std::vector<double> initial_e = g.energy;

    // Phase 1: rank the seed points by peeling tightest clusters.
    for (int r = n0 - 1; r >= 0; --r) {
        const int c = find_extreme(g, true, true);
        int x, y, z;
        coords(g, c, &x, &y, &z);
        g.on[static_cast<size_t>(c)] = 0;
        splat(g, x, y, z, -1.0);
        rank[static_cast<size_t>(c)] = r;
    }

    // Phase 2: refill the largest voids up to half.
    g.on = initial;
    g.energy = initial_e;
    for (int r = n0; r < n / 2; ++r) {
        const int v = find_extreme(g, false, false);
        int x, y, z;
        coords(g, v, &x, &y, &z);
        g.on[static_cast<size_t>(v)] = 1;
        splat(g, x, y, z, 1.0);
        rank[static_cast<size_t>(v)] = r;
    }

    // Phase 3: past half full the MINORITY is the zeros — build the energy
    // of the remaining holes and always fill the tightest hole-cluster.
    Grid inv;
    inv.w = w;
    inv.h = h;
    inv.d = d;
    inv.on.assign(static_cast<size_t>(n), 0);
    inv.energy.assign(static_cast<size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) {
        if (g.on[static_cast<size_t>(i)]) continue;
        inv.on[static_cast<size_t>(i)] = 1;
        int x, y, z;
        coords(inv, i, &x, &y, &z);
        splat(inv, x, y, z, 1.0);
    }
    for (int r = n / 2; r < n; ++r) {
        const int c = find_extreme(inv, true, true);
        int x, y, z;
        coords(inv, c, &x, &y, &z);
        inv.on[static_cast<size_t>(c)] = 0;
        splat(inv, x, y, z, -1.0);
        rank[static_cast<size_t>(c)] = r;
    }
    return rank;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: looks_bluenoise <out.bin>\n");
        return 2;
    }

    constexpr int kLutW = 512, kLutH = 192;
    std::vector<uint8_t> lut(static_cast<size_t>(kLutW) * kLutH, 0);

    // Filler for unused area (deterministic hash noise).
    {
        Lcg rng(0xF111Full);
        for (uint8_t& b : lut) b = static_cast<uint8_t>(rng.next() & 0xFF);
    }

    // STBN 64x64x8 -> rows 0..63, slice s at x offset s*64.
    std::printf("stbn 64x64x8...\n");
    {
        const std::vector<int> rank = rank_grid(64, 64, 8, 0x57B7ull);
        const int n = 64 * 64 * 8;
        for (int z = 0; z < 8; ++z)
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x) {
                    const int r = rank[static_cast<size_t>((z * 64 + y) * 64 + x)];
                    lut[static_cast<size_t>(y) * kLutW + z * 64 + x] =
                        static_cast<uint8_t>(
                            (static_cast<int64_t>(r) * 255) / (n - 1));
                }
    }

    // Blue noise 128x128 -> rows 64..191, x 0..127.
    std::printf("blue noise 128x128...\n");
    {
        const std::vector<int> rank = rank_grid(128, 128, 1, 0xB111Eull);
        const int n = 128 * 128;
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x) {
                const int r = rank[static_cast<size_t>(y * 128 + x)];
                lut[static_cast<size_t>(64 + y) * kLutW + x] =
                    static_cast<uint8_t>(
                        (static_cast<int64_t>(r) * 255) / (n - 1));
            }
    }

    std::FILE* f = std::fopen(argv[1], "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    const uint8_t magic[4] = {'D', 'L', 'T', '1'};
    const uint32_t w32 = kLutW, h32 = kLutH;
    std::fwrite(magic, 1, 4, f);
    std::fwrite(&w32, 4, 1, f);
    std::fwrite(&h32, 4, 1, f);
    std::fwrite(lut.data(), 1, lut.size(), f);
    std::fclose(f);
    std::printf("wrote %s\n", argv[1]);
    return 0;
}
