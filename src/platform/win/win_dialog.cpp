#include "platform/dialog.h"

#include <windows.h>

#include <shobjidl.h>

#include "platform/window.h"

namespace looks::platform {

namespace {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), n);
    out.resize(wcslen(out.c_str()));
    return out;
}

// One COM apartment per dialog call: cheap, and keeps the platform layer
// free of process-wide COM lifetime management.
struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                COINIT_DISABLE_OLE1DDE)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

std::optional<std::filesystem::path> run_dialog(
    Window* parent, const std::vector<FileFilter>& filters,
    const std::wstring& default_name, bool save) {
    ComScope com;
    if (!com.ok()) return std::nullopt;

    IFileDialog* dialog = nullptr;
    const CLSID clsid = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
    const IID iid = save ? IID_IFileSaveDialog : IID_IFileOpenDialog;
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, iid,
                                reinterpret_cast<void**>(&dialog))))
        return std::nullopt;

    std::vector<std::wstring> filter_strings;
    std::vector<COMDLG_FILTERSPEC> specs;
    filter_strings.reserve(filters.size() * 2);
    for (const FileFilter& f : filters) {
        filter_strings.push_back(widen(f.label));
        filter_strings.push_back(widen(f.pattern));
        specs.push_back({filter_strings[filter_strings.size() - 2].c_str(),
                         filter_strings[filter_strings.size() - 1].c_str()});
    }
    if (!specs.empty())
        dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
    if (!default_name.empty()) dialog->SetFileName(default_name.c_str());

    HWND owner = parent ? static_cast<HWND>(parent->native_window()) : nullptr;
    std::optional<std::filesystem::path> result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = std::filesystem::path(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

}  // namespace

std::optional<std::filesystem::path> show_open_dialog(
    Window* parent, const std::vector<FileFilter>& filters) {
    return run_dialog(parent, filters, {}, /*save=*/false);
}

std::optional<std::filesystem::path> show_save_dialog(
    Window* parent, const std::vector<FileFilter>& filters,
    const std::string& default_name) {
    return run_dialog(parent, filters, widen(default_name), /*save=*/true);
}

void show_fatal(const char* message) {
    MessageBoxW(nullptr, widen(message).c_str(), L"looks - fatal error",
                MB_OK | MB_ICONERROR | MB_TASKMODAL | MB_SETFOREGROUND);
}

ConfirmResult show_confirm(Window* parent, const std::string& title,
                           const std::string& text, bool with_cancel) {
    HWND owner = parent ? static_cast<HWND>(parent->native_window()) : nullptr;
    const UINT flags =
        (with_cancel ? MB_YESNOCANCEL : MB_YESNO) | MB_ICONWARNING;
    switch (MessageBoxW(owner, widen(text).c_str(), widen(title).c_str(),
                        flags)) {
        case IDYES: return ConfirmResult::Yes;
        case IDNO: return ConfirmResult::No;
        default: return ConfirmResult::Cancel;
    }
}

}  // namespace looks::platform
