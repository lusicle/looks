#include "platform/window.h"

#include <windows.h>
#include <windowsx.h>

#include <shellapi.h>

#include <cassert>

#pragma comment(lib, "shell32")

namespace looks::platform {

namespace {

constexpr wchar_t kWindowClass[] = L"looks_main_window";

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), n);
    out.resize(wcslen(out.c_str()));
    return out;
}

Key translate_key(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<Key>(static_cast<int>(Key::A) + (vk - 'A'));
    if (vk >= '0' && vk <= '9')
        return static_cast<Key>(static_cast<int>(Key::Num0) + (vk - '0'));
    if (vk >= VK_F1 && vk <= VK_F12)
        return static_cast<Key>(static_cast<int>(Key::F1) + (vk - VK_F1));
    switch (vk) {
        case VK_ESCAPE: return Key::Escape;
        case VK_TAB: return Key::Tab;
        case VK_CAPITAL: return Key::CapsLock;
        case VK_SHIFT: return Key::Shift;
        case VK_CONTROL: return Key::Ctrl;
        case VK_MENU: return Key::Alt;
        case VK_SPACE: return Key::Space;
        case VK_RETURN: return Key::Enter;
        case VK_BACK: return Key::Backspace;
        case VK_INSERT: return Key::Insert;
        case VK_DELETE: return Key::Delete;
        case VK_HOME: return Key::Home;
        case VK_END: return Key::End;
        case VK_PRIOR: return Key::PageUp;
        case VK_NEXT: return Key::PageDown;
        case VK_LEFT: return Key::Left;
        case VK_RIGHT: return Key::Right;
        case VK_UP: return Key::Up;
        case VK_DOWN: return Key::Down;
        case VK_OEM_MINUS: return Key::Minus;
        case VK_OEM_PLUS: return Key::Equals;
        case VK_OEM_4: return Key::LeftBracket;
        case VK_OEM_6: return Key::RightBracket;
        case VK_OEM_5: return Key::Backslash;
        case VK_OEM_1: return Key::Semicolon;
        case VK_OEM_7: return Key::Apostrophe;
        case VK_OEM_COMMA: return Key::Comma;
        case VK_OEM_PERIOD: return Key::Period;
        case VK_OEM_2: return Key::Slash;
        case VK_OEM_3: return Key::Grave;
        default: return Key::Unknown;
    }
}

uint32_t current_mods() {
    uint32_t mods = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= kModCtrl;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= kModShift;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= kModAlt;
    return mods;
}

class WinWindow final : public Window {
public:
    WinWindow(const WindowDesc& desc) {
        HINSTANCE hinstance = GetModuleHandleW(nullptr);

        static bool class_registered = false;
        if (!class_registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = &WinWindow::wnd_proc;
            wc.hInstance = hinstance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kWindowClass;
            RegisterClassExW(&wc);
            class_registered = true;
        }

        DWORD style = WS_OVERLAPPEDWINDOW;
        if (!desc.resizable)
            style &= ~static_cast<DWORD>(WS_THICKFRAME | WS_MAXIMIZEBOX);

        // Desired size is logical px; scale by the primary monitor DPI, then
        // convert client size -> outer size.
        UINT dpi = GetDpiForSystem();
        RECT rect{0, 0,
                  MulDiv(desc.width, static_cast<int>(dpi), 96),
                  MulDiv(desc.height, static_cast<int>(dpi), 96)};
        AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi);

        hwnd_ = CreateWindowExW(
            0, kWindowClass, widen(desc.title).c_str(), style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top,
            nullptr, nullptr, hinstance, this);
        assert(hwnd_);

        dpi_scale_ = static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f;
        update_client_size();

        DragAcceptFiles(hwnd_, TRUE);   // WM_DROPFILES -> Event::FileDrop
        ShowWindow(hwnd_, SW_SHOW);
    }

    ~WinWindow() override {
        if (hwnd_) DestroyWindow(hwnd_);
    }

    bool pump_events(std::vector<Event>& out) override {
        pending_ = &out;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                pending_ = nullptr;
                return false;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        pending_ = nullptr;
        return hwnd_ != nullptr;
    }

    uint32_t width() const override { return width_; }
    uint32_t height() const override { return height_; }
    float dpi_scale() const override { return dpi_scale_; }
    bool minimized() const override { return minimized_; }

    void set_title(const std::string& utf8) override {
        SetWindowTextW(hwnd_, widen(utf8).c_str());
    }

    void request_close() override {
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    void* native_window() const override { return hwnd_; }
    void* native_instance() const override {
        return GetModuleHandleW(nullptr);
    }

private:
    void update_client_size() {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        width_ = static_cast<uint32_t>(rc.right - rc.left);
        height_ = static_cast<uint32_t>(rc.bottom - rc.top);
    }

    void emit(const Event& e) {
        if (pending_) pending_->push_back(e);
    }

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        WinWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<WinWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        } else {
            self = reinterpret_cast<WinWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (!self) return DefWindowProcW(hwnd, msg, wparam, lparam);
        return self->handle(msg, wparam, lparam);
    }

    LRESULT handle(UINT msg, WPARAM wparam, LPARAM lparam) {
        Event e{};
        switch (msg) {
            case WM_CLOSE:
                e.type = Event::Type::CloseRequested;
                emit(e);
                return 0;  // app decides; request_close() actually destroys

            case WM_DESTROY:
                hwnd_ = nullptr;
                PostQuitMessage(0);
                return 0;

            case WM_SIZE: {
                minimized_ = (wparam == SIZE_MINIMIZED);
                update_client_size();
                e.type = Event::Type::Resize;
                e.width = static_cast<int32_t>(width_);
                e.height = static_cast<int32_t>(height_);
                emit(e);
                return 0;
            }

            case WM_DPICHANGED: {
                dpi_scale_ = static_cast<float>(HIWORD(wparam)) / 96.0f;
                const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(hwnd_, nullptr,
                             suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                e.type = Event::Type::DpiChanged;
                e.dpi_scale = dpi_scale_;
                emit(e);
                return 0;
            }

            case WM_SETFOCUS:
                e.type = Event::Type::FocusGained;
                emit(e);
                return 0;
            case WM_KILLFOCUS:
                e.type = Event::Type::FocusLost;
                emit(e);
                return 0;

            case WM_MOUSEMOVE:
                e.type = Event::Type::MouseMove;
                e.mouse_x = static_cast<float>(GET_X_LPARAM(lparam));
                e.mouse_y = static_cast<float>(GET_Y_LPARAM(lparam));
                e.mods = current_mods();
                emit(e);
                return 0;

            case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
            case WM_XBUTTONDOWN: {
                e.type = Event::Type::MouseDown;
                e.button = button_from_msg(msg, wparam);
                e.mouse_x = static_cast<float>(GET_X_LPARAM(lparam));
                e.mouse_y = static_cast<float>(GET_Y_LPARAM(lparam));
                e.mods = current_mods();
                if (++capture_count_ == 1) SetCapture(hwnd_);
                emit(e);
                return msg == WM_XBUTTONDOWN ? TRUE : 0;
            }

            case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
            case WM_XBUTTONUP: {
                e.type = Event::Type::MouseUp;
                e.button = button_from_msg(msg, wparam);
                e.mouse_x = static_cast<float>(GET_X_LPARAM(lparam));
                e.mouse_y = static_cast<float>(GET_Y_LPARAM(lparam));
                e.mods = current_mods();
                if (capture_count_ > 0 && --capture_count_ == 0) ReleaseCapture();
                emit(e);
                return msg == WM_XBUTTONUP ? TRUE : 0;
            }

            case WM_MOUSEWHEEL: {
                e.type = Event::Type::MouseWheel;
                e.wheel_y = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
                POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(hwnd_, &pt);   // wheel coords arrive in screen space
                e.mouse_x = static_cast<float>(pt.x);
                e.mouse_y = static_cast<float>(pt.y);
                e.mods = current_mods();
                emit(e);
                return 0;
            }
            case WM_MOUSEHWHEEL: {
                e.type = Event::Type::MouseWheel;
                e.wheel_x = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
                POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(hwnd_, &pt);
                e.mouse_x = static_cast<float>(pt.x);
                e.mouse_y = static_cast<float>(pt.y);
                e.mods = current_mods();
                emit(e);
                return 0;
            }

            case WM_DROPFILES: {
                HDROP drop = reinterpret_cast<HDROP>(wparam);
                // WHERE the files landed: dropping onto the timeline
                // means something different from dropping anywhere
                // else, and the last mouse-move is stale (dragging
                // over another window sends us nothing). Physical
                // client px, like every other event carries. A
                // multi-select drop delivers ONE event per file, all
                // at the same point - nothing is silently discarded.
                POINT drop_pt{};
                DragQueryPoint(drop, &drop_pt);
                const UINT count =
                    DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
                wchar_t path[MAX_PATH] = L"";
                for (UINT i = 0; i < count; ++i) {
                    if (!DragQueryFileW(drop, i, path, MAX_PATH)) continue;
                    Event fe{};
                    fe.type = Event::Type::FileDrop;
                    fe.mouse_x = static_cast<float>(drop_pt.x);
                    fe.mouse_y = static_cast<float>(drop_pt.y);
                    const int n = WideCharToMultiByte(CP_UTF8, 0, path, -1,
                                                      nullptr, 0, nullptr,
                                                      nullptr);
                    std::string utf8(static_cast<size_t>(n), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8.data(),
                                        n, nullptr, nullptr);
                    utf8.resize(strlen(utf8.c_str()));
                    fe.drop_path = std::move(utf8);
                    emit(fe);
                }
                DragFinish(drop);
                return 0;
            }

            case WM_KEYDOWN: case WM_SYSKEYDOWN:
                e.type = Event::Type::KeyDown;
                e.key = translate_key(wparam);
                e.repeat = (lparam & (1ll << 30)) != 0;
                e.mods = current_mods();
                emit(e);
                // Let DefWindowProc handle Alt+F4 etc. for syskeys.
                return msg == WM_SYSKEYDOWN
                    ? DefWindowProcW(hwnd_, msg, wparam, lparam) : 0;

            case WM_KEYUP: case WM_SYSKEYUP:
                e.type = Event::Type::KeyUp;
                e.key = translate_key(wparam);
                e.mods = current_mods();
                emit(e);
                return msg == WM_SYSKEYUP
                    ? DefWindowProcW(hwnd_, msg, wparam, lparam) : 0;

            case WM_CHAR: {
                // UTF-16 in; pair surrogates into one UTF-32 event.
                uint32_t c = static_cast<uint32_t>(wparam);
                if (c >= 0xD800 && c <= 0xDBFF) {
                    pending_high_surrogate_ = c;
                    return 0;
                }
                if (c >= 0xDC00 && c <= 0xDFFF) {
                    if (!pending_high_surrogate_) return 0;
                    c = 0x10000 + ((pending_high_surrogate_ - 0xD800) << 10) + (c - 0xDC00);
                    pending_high_surrogate_ = 0;
                }
                if (c >= 0x20 && c != 0x7F) {
                    e.type = Event::Type::Char;
                    e.codepoint = c;
                    emit(e);
                }
                return 0;
            }
        }
        return DefWindowProcW(hwnd_, msg, wparam, lparam);
    }

    static MouseButton button_from_msg(UINT msg, WPARAM wparam) {
        switch (msg) {
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: return MouseButton::Left;
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: return MouseButton::Right;
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: return MouseButton::Middle;
            default:
                return GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? MouseButton::X1
                                                              : MouseButton::X2;
        }
    }

    HWND hwnd_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    float dpi_scale_ = 1.0f;
    bool minimized_ = false;
    int capture_count_ = 0;
    uint32_t pending_high_surrogate_ = 0;
    std::vector<Event>* pending_ = nullptr;
};

}  // namespace

void init() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

std::unique_ptr<Window> create_window(const WindowDesc& desc) {
    return std::make_unique<WinWindow>(desc);
}

}  // namespace looks::platform
