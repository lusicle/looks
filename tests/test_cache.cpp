#include "doc_fixture.h"
#include "test_framework.h"

#include "doc/effects.h"
#include "gfx/render_cache.h"
#include "media/cache.h"
#include <chrono>
#include <fstream>
#include "util/image.h"
#include "codec/mez.h"

using namespace looks;

TEST(disk_cache_defaults_and_cleanup_protection) {
    namespace fs = std::filesystem;
    media::CachePolicy policy;
    CHECK_EQ(policy.max_bytes, uint64_t{20} << 30);
    CHECK_EQ(policy.max_age_days, 30u);
    const auto root = fs::path(LOOKS_REPO_ROOT) / "temp" /
        ("cache_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto now = fs::file_time_type::clock::now();
    auto bundle = [&](const char* name, size_t bytes, int days) {
        const auto path = root / name;
        fs::create_directory(path);
        std::ofstream file(path / "clip.mez", std::ios::binary);
        file << std::string(bytes, 'x');
        file.close();
        media::touch_disk_cache(path);
        fs::last_write_time(path / ".last_used", now - std::chrono::hours(days * 24));
        return path;
    };
    const auto old = bundle("aaaaaaaaaaaaaaaa", 100, 60);
    const auto middle = bundle("bbbbbbbbbbbbbbbb", 200, 20);
    const auto recent = bundle("cccccccccccccccc", 300, 1);
    const auto unknown = bundle("dddddddddddddddd", 100, 60);
    std::ofstream(unknown / "source.png") << std::string(20, 'x');
    std::ofstream(root / "untitled.autosave.json") << std::string(20, 'x');
    auto scan = media::scan_disk_cache(root);
    CHECK(scan.error.empty());
    CHECK_EQ(scan.bytes, uint64_t{740});
    auto plan = media::cache_cleanup_plan(scan, {0, 30}, {});
    CHECK_EQ(plan.size(), size_t{1});
    CHECK(plan[0] == old);
    CHECK(media::cache_cleanup_plan(scan, {0, 0}, {}).empty());
    CHECK(media::cache_cleanup_plan(scan, {0, 30}, {old / "clip.mez"}).empty());
    plan = media::cache_cleanup_plan(scan, {600, 0}, {});
    CHECK_EQ(plan.size(), size_t{2});
    CHECK(plan[0] == old);
    CHECK(plan[1] == middle);
    plan = media::cache_cleanup_plan(scan, {600, 30}, {middle});
    CHECK_EQ(plan.size(), size_t{2});
    CHECK(plan[0] == old);
    CHECK(plan[1] == recent);
    plan = media::cache_cleanup_plan(scan, {0, 0}, {recent}, true);
    CHECK_EQ(plan.size(), size_t{2});
    CHECK(media::stage_cache_removal(root, unknown).empty());
    CHECK(media::stage_cache_removal(root / "elsewhere", old).empty());
    CHECK(!media::remove_staged_cache(root, old));
    const auto staged = media::stage_cache_removal(root, old);
    CHECK(!staged.empty());
    CHECK(!fs::exists(old));
    bundle("aaaaaaaaaaaaaaaa", 50, 0);
    CHECK(media::remove_staged_cache(root, staged));
    CHECK(fs::exists(old / "clip.mez"));
    CHECK(fs::exists(unknown / "source.png"));
    CHECK(fs::exists(root / "untitled.autosave.json"));
    media::touch_disk_cache(middle);
    CHECK(fs::last_write_time(middle / ".last_used") >= now);
    const auto interrupted = media::stage_cache_removal(root, recent);
    CHECK(!interrupted.empty());
    scan = media::scan_disk_cache(root);
    plan = media::cache_cleanup_plan(scan, {0, 0}, {});
    CHECK_EQ(plan.size(), size_t{1});
    CHECK(plan[0] == interrupted);
    CHECK(media::remove_staged_cache(root, interrupted));
    fs::create_directory(root / ".clear-user-files");
    std::ofstream(root / ".clear-user-files" / "source.mez") << 'x';
    CHECK(media::stage_cache_removal(root, root / ".clear-user-files").empty());
    const auto changed = media::stage_cache_removal(root, middle);
    CHECK(!changed.empty());
    std::ofstream(changed / "source.png") << 'x';
    CHECK(!media::remove_staged_cache(root, changed));
    CHECK(fs::exists(changed / "clip.mez"));
}

TEST(disk_cache_force_clear_rebuilds_active_media) {
    namespace fs = std::filesystem;
    const auto folder = fs::path(LOOKS_REPO_ROOT) / "temp" /
        ("cache_force_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto root = folder / "cache";
    const auto active = root / "aaaaaaaaaaaaaaaa";
    const auto unused = root / "bbbbbbbbbbbbbbbb";
    fs::create_directories(active);
    fs::create_directories(unused);
    const auto source = folder / "source.png";
    std::vector<uint8_t> rgba(16 * 16 * 4, 255);
    CHECK(write_png(source, rgba.data(), 16, 16));
    std::ofstream(active / "source.mez") << "stale";
    std::ofstream(unused / "old.mez") << "stale";
    std::ofstream(root / "recovery.json") << "preserve";
    const auto missing = media::force_clear_disk_cache(root,
        {{folder / "missing.png", active, 90}}, {});
    CHECK(!missing.error.empty());
    CHECK(fs::exists(active / "source.mez"));
    const auto result = media::force_clear_disk_cache(root, {{source, active, 90}}, {});
    CHECK(result.error.empty());
    CHECK_EQ(result.failed, size_t{0});
    CHECK_EQ(result.removed, size_t{2});
    CHECK_EQ(result.rebuilt, size_t{1});
    CHECK(!fs::exists(unused));
    CHECK(fs::exists(source));
    CHECK(fs::exists(root / "recovery.json"));
    CHECK(fs::exists(active / "source.thumbs"));
    codec::MezReader reader;
    CHECK(reader.open(active / "source.mez", nullptr));
    CHECK_EQ(reader.width(), 16u);
    CHECK_EQ(reader.frame_count(), 90u);
    codec::DecodedFrame frame;
    CHECK(reader.decode(89, frame));
}

TEST(disk_cache_force_clear_removes_renamed_bundles_and_sidecars) {
    namespace fs = std::filesystem;
    const auto root = fs::path(LOOKS_REPO_ROOT) / "temp" /
        ("cache_renamed_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const char* names[] = {
        "0a90829abf9e3121-old", "0a90829abf9e3121-x", "3cd156d7529b6fc6-old",
        "3cd156d7529b6fc6-v2", "3cd156d7529b6fc6-x", "875aebd8d417135e",
        "921435a53dba0f27-old", "921435a53dba0f27-x", "c0fce1cf7f95a650-old",
        "c0fce1cf7f95a650-x", "d98d55df463f84f5-old", "fc1bda114ce8008e-old",
        "fc1bda114ce8008e-v2", "fc1bda114ce8008e-x"};
    for (const auto* name : names) {
        fs::create_directories(root / name);
        for (const auto* file : {"clip.mez", "clip.pcm", "clip.analysis-old", "clip.thumbs-old"})
            std::ofstream(root / name / file) << "cache";
    }
    const auto scan = media::scan_disk_cache(root);
    CHECK_EQ(scan.entries.size(), size_t{14});
    CHECK_EQ(media::cache_cleanup_plan(scan, {1, 0}, {}).size(), size_t{14});
    CHECK_EQ(media::cache_cleanup_plan(scan, {0, 0}, {root / names[0] / "clip.mez"}, true).size(), size_t{13});
    const auto result = media::force_clear_disk_cache(root, {}, {});
    CHECK(result.error.empty());
    CHECK_EQ(result.removed, size_t{14});
    CHECK_EQ(result.rebuilt, size_t{0});
    CHECK_EQ(result.failed, size_t{0});
    CHECK_EQ(result.kept, size_t{0});
    CHECK_EQ(media::scan_disk_cache(root).bytes, uint64_t{0});
    CHECK(media::scan_disk_cache(root).entries.empty());
    const auto source_folder = root / "aaaaaaaaaaaaaaaa-old";
    fs::create_directory(source_folder);
    std::ofstream(source_folder / "source.png-old") << "source";
    const auto kept = media::force_clear_disk_cache(root, {}, {});
    CHECK_EQ(kept.removed, size_t{0});
    CHECK_EQ(kept.kept, size_t{1});
    CHECK(fs::exists(source_folder / "source.png-old"));
}

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
    doc::Document doc = doc_with_look();
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
    doc::Document doc = doc_with_look();
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
