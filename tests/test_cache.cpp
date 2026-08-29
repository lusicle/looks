#include "test_framework.h"

#include "doc/effects.h"
#include "gfx/render_cache.h"

using namespace looks;

namespace {

std::vector<uint16_t> fill(uint32_t w, uint32_t h, uint16_t v) {
    return std::vector<uint16_t>(size_t{w} * h * 4, v);
}

}  // namespace

TEST(cache_insert_find_roundtrip) {
    gfx::RenderCache cache;
    cache.set_context(7);
    cache.insert(7, 3, 4, 2, fill(4, 2, 123));
    CHECK_EQ(cache.count(), 1u);
    const gfx::RenderCache::Frame* f = cache.find(3);
    CHECK(f != nullptr);
    if (f) {
        CHECK_EQ(f->width, 4u);
        CHECK_EQ(f->height, 2u);
        CHECK_EQ(f->halves[0], 123);
    }
    CHECK(cache.find(4) == nullptr);
    CHECK_EQ(cache.hits(), 1u);
}

TEST(cache_stale_context_insert_dropped) {
    gfx::RenderCache cache;
    cache.set_context(7);
    cache.insert(6, 0, 4, 2, fill(4, 2, 1));
    CHECK_EQ(cache.count(), 0u);
}

TEST(cache_context_change_flushes) {
    gfx::RenderCache cache;
    cache.set_context(7);
    cache.insert(7, 0, 4, 2, fill(4, 2, 1));
    CHECK_EQ(cache.count(), 1u);
    cache.set_context(8);
    CHECK_EQ(cache.count(), 0u);
    CHECK_EQ(cache.bytes(), 0u);
    CHECK(cache.find(0) == nullptr);
}

TEST(cache_budget_evicts_lru) {
    gfx::RenderCache cache;
    cache.set_context(1);
    const size_t frame_bytes = size_t{4} * 2 * 4 * sizeof(uint16_t);
    cache.set_budget(frame_bytes * 3);
    cache.insert(1, 0, 4, 2, fill(4, 2, 0));
    cache.insert(1, 1, 4, 2, fill(4, 2, 1));
    cache.insert(1, 2, 4, 2, fill(4, 2, 2));
    CHECK(cache.find(0) != nullptr);   // find freshens 0; frame 1 is now LRU
    cache.insert(1, 3, 4, 2, fill(4, 2, 3));
    CHECK_EQ(cache.count(), 3u);
    CHECK(cache.find(1) == nullptr);
    CHECK(cache.find(0) != nullptr);
    CHECK(cache.find(2) != nullptr);
    CHECK(cache.find(3) != nullptr);
    CHECK(cache.bytes() <= cache.budget());
}

TEST(cache_oversized_frame_rejected) {
    gfx::RenderCache cache;
    cache.set_context(1);
    cache.set_budget(16);
    cache.insert(1, 0, 4, 2, fill(4, 2, 1));   // 64 bytes > 16-byte budget
    CHECK_EQ(cache.count(), 0u);
    CHECK_EQ(cache.bytes(), 0u);
}

TEST(cache_replace_same_frame_updates_bytes) {
    gfx::RenderCache cache;
    cache.set_context(1);
    cache.insert(1, 0, 4, 2, fill(4, 2, 1));
    const size_t once = cache.bytes();
    cache.insert(1, 0, 4, 2, fill(4, 2, 9));
    CHECK_EQ(cache.bytes(), once);
    const gfx::RenderCache::Frame* f = cache.find(0);
    CHECK(f != nullptr && f->halves[0] == 9);
}

TEST(history_scan_gates_cache) {
    doc::Document doc;
    CHECK(!doc::document_uses_history(doc));
    doc.looks[0].layers[0].stack.push_back(
        doc::make_effect(doc, doc::EffectType::Vignette));
    doc.looks[0].layers[0].stack.push_back(
        doc::make_effect(doc, doc::EffectType::Quantize));
    CHECK(!doc::document_uses_history(doc));
    doc.looks[0].layers[0].stack.push_back(
        doc::make_effect(doc, doc::EffectType::Feedback));
    CHECK(doc::document_uses_history(doc));
    doc.looks[0].layers[0].stack.back().bypass = true;
    CHECK(!doc::document_uses_history(doc));
}

TEST(history_scan_quantize_rd_stipple) {
    // params[2] is the dither mode; only mode 9 (RD stipple) has state.
    doc::Document doc;
    doc.looks[0].layers[0].stack.push_back(
        doc::make_effect(doc, doc::EffectType::Quantize));
    CHECK(!doc::document_uses_history(doc));
    doc.looks[0].layers[0].stack[0].params[2] = 8.0f;
    CHECK(!doc::document_uses_history(doc));
    doc.looks[0].layers[0].stack[0].params[2] = 9.0f;
    CHECK(doc::document_uses_history(doc));
    doc.looks[0].layers[0].stack[0].params[2] = 13.0f;
    CHECK(!doc::document_uses_history(doc));
}
