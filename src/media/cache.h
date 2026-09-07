#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "media/import.h"

namespace looks::media {

struct CachePolicy {
    uint64_t max_bytes = uint64_t{20} << 30;
    uint32_t max_age_days = 30;
};

struct CacheEntry {
    std::filesystem::path path;
    uint64_t bytes = 0;
    std::filesystem::file_time_type used{};
    bool removable = false;
};

struct CacheScan {
    uint64_t bytes = 0;
    std::vector<CacheEntry> entries;
    std::string error;
};

CacheScan scan_disk_cache(const std::filesystem::path& root);
void touch_disk_cache(const std::filesystem::path& entry);
std::vector<std::filesystem::path> cache_cleanup_plan(
    const CacheScan& scan, CachePolicy policy,
    const std::vector<std::filesystem::path>& protected_paths, bool clear_all = false);
std::filesystem::path stage_cache_removal(const std::filesystem::path& root,
                                        const std::filesystem::path& entry);
bool remove_staged_cache(const std::filesystem::path& root,
                         const std::filesystem::path& entry);

struct CacheSource {
    std::filesystem::path source, directory;
    uint32_t still_frames = 0;
};

struct CacheClearResult {
    size_t removed = 0, rebuilt = 0, failed = 0, kept = 0;
    std::string error;
};

CacheClearResult force_clear_disk_cache(const std::filesystem::path& root,
    const std::vector<CacheSource>& sources, const ImportOptions& options,
    ImportProgress* progress = nullptr);

}
