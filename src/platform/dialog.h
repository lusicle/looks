// Native file dialogs (Win32 platform layer, IFileDialog
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

// Native yes/no(/cancel) prompt — the unsaved-changes and autosave-restore
// guards. Blocking; UI thread only.
enum class ConfirmResult { Yes, No, Cancel };

ConfirmResult show_confirm(Window* parent, const std::string& title,
                           const std::string& text, bool with_cancel);

// Fatal-error box: ownerless so it works from any thread on the way
// down — installed as the log_fatal sink at startup.
void show_fatal(const char* message);

}  // namespace looks::platform
