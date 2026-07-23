#include "util/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace looks {

namespace {

void log_emit(const char* level, const char* fmt, va_list args) {
    char msg[2048];
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    char line[2112];
    std::snprintf(line, sizeof(line), "[%s] %s\n", level, msg);
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

}  // namespace looks
