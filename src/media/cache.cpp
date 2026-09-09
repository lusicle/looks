#include "media/cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include "codec/mez.h"
#include "media/bundle.h"
#include "media/track.h"

namespace looks::media {
namespace {

bool bundle_name(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    return name.size() == 16 && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool cache_bundle_name(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    return bundle_name(path) || (name.size() > 17 && name[16] == '-' &&
        bundle_name(name.substr(0, 16)));
}

bool staged_name(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    if (name.size() < 27 || name.rfind(".clear-", 0) != 0 || name[23] != '-' ||
        !bundle_name(name.substr(7, 16))) return false;
    const auto split = name.find('-', 24);
    if (split == std::string::npos || split == 24 || split + 1 == name.size()) return false;
    for (size_t i = 24; i < name.size(); ++i)
        if (i != split && (name[i] < '0' || name[i] > '9')) return false;
    return true;
}

bool direct_child(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code ec;
    const auto base = std::filesystem::canonical(root, ec);
    if (ec) return false;
    const auto target = std::filesystem::canonical(path, ec);
    return !ec && target.parent_path() == base &&
        !std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec)) && !ec;
}

bool cache_file(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    ext = ext.substr(0, ext.find('-'));
    const auto name = path.filename().string();
    return name == ".last_used" || name.rfind(".last_used-", 0) == 0 || ext == ".mez" || ext == ".pcm" ||
        ext == ".thumbs" || ext == ".analysis" || ext == ".track" || ext == ".lrgba" ||
        (ext == ".mp4" && path.stem().extension() == ".intra");
}

CacheEntry read_entry(const std::filesystem::path& root, const std::filesystem::path& path) {
    CacheEntry out;
    out.path = path;
    if (!direct_child(root, path)) return out;
    out.removable = cache_bundle_name(path) || staged_name(path);
    std::error_code ec;
    out.used = std::filesystem::last_write_time(path / ".last_used", ec);
    const bool has_used = !ec;
    if (!has_used) out.used = std::filesystem::file_time_type::min();
    ec.clear();
    for (auto it = std::filesystem::recursive_directory_iterator(path, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        const auto status = it->symlink_status(ec);
        if (ec) break;
        if (std::filesystem::is_symlink(status)) {
            it.disable_recursion_pending();
            out.removable = false;
            continue;
        }
        if (std::filesystem::is_directory(status)) out.removable = false;
        else if (std::filesystem::is_regular_file(status)) {
            const auto bytes = it->file_size(ec);
            if (ec) break;
            out.bytes += bytes;
            if (!has_used) out.used = std::max(out.used, it->last_write_time(ec));
            if (!cache_file(it->path())) out.removable = false;
        } else out.removable = false;
    }
    if (ec) out.removable = false;
    return out;
}

bool protected_entry(const CacheEntry& entry, const std::vector<std::filesystem::path>& paths) {
    for (const auto& path : paths) {
        std::error_code ec;
        const auto full = std::filesystem::weakly_canonical(path, ec);
        if (ec) continue;
        const auto relative = full.lexically_relative(entry.path);
        if (relative == "." || (!relative.empty() && *relative.begin() != "..")) return true;
    }
    return false;
}

}

CacheScan scan_disk_cache(const std::filesystem::path& root) {
    CacheScan out;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return out;
    const auto base = std::filesystem::canonical(root, ec);
    if (ec) { out.error = ec.message(); return out; }
    for (auto it = std::filesystem::directory_iterator(base, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const auto status = it->symlink_status(ec);
        if (ec) break;
        if (std::filesystem::is_symlink(status)) continue;
        if (std::filesystem::is_directory(status)) {
            auto entry = read_entry(base, it->path());
            out.bytes += entry.bytes;
            out.entries.push_back(std::move(entry));
        } else if (std::filesystem::is_regular_file(status)) out.bytes += it->file_size(ec);
    }
    if (ec) out.error = ec.message();
    return out;
}

void touch_disk_cache(const std::filesystem::path& entry) {
    std::error_code ec;
    if (!bundle_name(entry) || !direct_child(entry.parent_path(), entry)) return;
    const auto marker = entry / ".last_used";
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(marker, ec))) return;
    ec.clear();
    if (!std::filesystem::exists(marker, ec)) {
        std::ofstream file(marker, std::ios::binary);
        if (!file) return;
    }
    std::filesystem::last_write_time(marker, std::filesystem::file_time_type::clock::now(), ec);
}

std::vector<std::filesystem::path> cache_cleanup_plan(
    const CacheScan& scan, CachePolicy policy,
    const std::vector<std::filesystem::path>& protected_paths, bool clear_all) {
    std::vector<const CacheEntry*> ordered;
    for (const auto& entry : scan.entries)
        if (entry.removable && !protected_entry(entry, protected_paths)) ordered.push_back(&entry);
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
        return a->used < b->used || (a->used == b->used && a->path < b->path);
    });
    const auto cutoff = std::filesystem::file_time_type::clock::now() -
        std::chrono::hours(uint64_t{std::min(policy.max_age_days, 36500u)} * 24);
    uint64_t bytes = scan.bytes;
    std::vector<std::filesystem::path> out;
    for (const auto* entry : ordered) {
        if (!clear_all && !staged_name(entry->path) &&
            !(policy.max_age_days && entry->used < cutoff) &&
            !(policy.max_bytes && bytes > policy.max_bytes)) continue;
        out.push_back(entry->path);
        bytes -= std::min(bytes, entry->bytes);
    }
    return out;
}

std::filesystem::path stage_cache_removal(const std::filesystem::path& root,
                                         const std::filesystem::path& entry) {
    if (!direct_child(root, entry) || !read_entry(root, entry).removable) return {};
    if (staged_name(entry)) return entry;
    static std::atomic<uint64_t> serial{0};
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto target = entry.parent_path() / (".clear-" + entry.filename().string().substr(0, 16) + "-" +
        std::to_string(suffix) + "-" + std::to_string(serial.fetch_add(1)));
    std::error_code ec;
    std::filesystem::rename(entry, target, ec);
    return ec ? std::filesystem::path{} : target;
}

bool remove_staged_cache(const std::filesystem::path& root, const std::filesystem::path& entry) {
    if (!staged_name(entry) || !direct_child(root, entry) || !read_entry(root, entry).removable) return false;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(entry, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::filesystem::remove(it->path(), ec);
    }
    if (ec) return false;
    return std::filesystem::remove(entry, ec) && !ec;
}

CacheClearResult force_clear_disk_cache(const std::filesystem::path& root,
    const std::vector<CacheSource>& sources, const ImportOptions& options,
    ImportProgress* progress) {
    CacheClearResult out;
    std::vector<std::filesystem::path> keep;
    std::vector<std::pair<std::filesystem::path, TrackData>> tracks;
    for (const auto& source : sources) {
        std::error_code ec;
        const auto parent = std::filesystem::weakly_canonical(source.directory.parent_path(), ec);
        if (ec || parent != std::filesystem::weakly_canonical(root, ec) || ec ||
            !bundle_name(source.directory) ||
            std::filesystem::is_symlink(std::filesystem::symlink_status(source.directory, ec))) {
            out.error = "invalid cache directory";
            return out;
        }
        ec.clear();
        if (!std::filesystem::is_regular_file(source.source, ec)) {
            out.error = "source missing: " + source.source.filename().string();
            return out;
        }
        keep.push_back(source.source);
        TrackData track;
        const auto path = sidecars_for(source.directory, source.source).track;
        if (track_load(path, &track)) tracks.emplace_back(path, std::move(track));
    }
    const auto scan = scan_disk_cache(root);
    if (!scan.error.empty()) { out.error = scan.error; return out; }
    const auto plan = cache_cleanup_plan(scan, {0, 0}, keep, true);
    out.kept = scan.entries.size() - plan.size();
    for (const auto& path : plan) {
        if (progress && progress->cancel.load()) break;
        const auto staged = stage_cache_removal(root, path);
        if (staged.empty() || !remove_staged_cache(root, staged)) ++out.failed;
        else ++out.removed;
    }
    for (const auto& source : sources) {
        if (progress && progress->cancel.load()) { out.error = "cache rebuild cancelled"; break; }
        auto ext = source.source.extension().wstring();
        for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
        if (ext == L".mez") continue;
        if (progress) {
            progress->frames_done = 0;
            progress->frames_total = 0;
        }
        const auto imported = import_media(source.source, source.directory, options, progress);
        if (!imported.ok) {
            ++out.failed;
            out.error = "cache rebuild failed: " + imported.error;
            continue;
        }
        if (source.still_frames && !imported.mez_path.empty()) {
            codec::mez_set_frame_count(imported.mez_path, source.still_frames);
            if (!imported.proxy_path.empty()) codec::mez_set_frame_count(imported.proxy_path, source.still_frames);
        }
        touch_disk_cache(source.directory);
        ++out.rebuilt;
    }
    for (const auto& [path, track] : tracks)
        if (!track_save(path, track)) ++out.failed;
    return out;
}

}
