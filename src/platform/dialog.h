// Native file dialogs (spec §12: Win32 platform layer, IFileDialog
// open/save). Blocking; call from the UI thread only.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace looks::platform {

class Window;

struct FileFilter {
    std::string label;     // "MP4 video"
    std::string pattern;   // "*.mp4;*.mov"
};

// Returns the chosen path, or nullopt on cancel/error.
std::optional<std::filesystem::path> show_open_dialog(
    Window* parent, const std::vector<FileFilter>& filters);

std::optional<std::filesystem::path> show_save_dialog(
    Window* parent, const std::vector<FileFilter>& filters,
    const std::string& default_name);

}  // namespace looks::platform
