#include "util/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace looks {

namespace {

std::mutex g_log_mutex;
void (*g_fatal_sink)(const char* message) = nullptr;

// The file is truncated for each run and flushed for each line.
FILE* log_file() {
    static FILE* file = [] {
        wchar_t path[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return static_cast<FILE*>(nullptr);
        wchar_t* slash = wcsrchr(path, L'\\');
        if (!slash) return static_cast<FILE*>(nullptr);
        *(slash + 1) = L'\0';
        std::wstring full = std::wstring(path) + L"looks.log";
        return _wfopen(full.c_str(), L"w");
    }();
    return file;
}

void log_emit(const char* level, const char* fmt, va_list args) {
    char msg[2048];
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    char line[2112];
    std::snprintf(line, sizeof(line), "[%s] %s\n", level, msg);
    // Write to the file first, in the lock, and flush each line.
    // Write the other sinks outside the lock so a stall blocks one thread.
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        if (FILE* f = log_file()) {
            std::fputs(line, f);
            std::fflush(f);
        }
    }
    std::fputs(line, stderr);
    OutputDebugStringA(line);
}

}  // namespace

void log_info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_emit("info", fmt, args);
    va_end(args);
}

void log_warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_emit("warn", fmt, args);
    va_end(args);
}

void log_error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_emit("error", fmt, args);
    va_end(args);
}

void set_fatal_sink(void (*sink)(const char* message)) {
    g_fatal_sink = sink;
}

void log_fatal(const char* fmt, ...) {
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    log_error("%s", msg);
    if (g_fatal_sink) g_fatal_sink(msg);
    std::abort();
}

}  // namespace looks
