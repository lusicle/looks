// These dialogs block. Call them from the UI thread only.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace looks::platform {

class Window;

struct FileFilter {
    std::string label;
    std::string pattern;   // Put a semicolon between patterns.
};

std::optional<std::filesystem::path> show_open_dialog(
    Window* parent, const std::vector<FileFilter>& filters);

std::optional<std::filesystem::path> show_save_dialog(
    Window* parent, const std::vector<FileFilter>& filters,
    const std::string& default_name);

// This box has no owner window, so any thread can call it.
void show_fatal(const char* message);

}  // namespace looks::platform
