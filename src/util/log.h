// The format is printf-style. These functions add the newline.

#pragma once

namespace looks {

void log_info(const char* fmt, ...);
void log_warn(const char* fmt, ...);
void log_error(const char* fmt, ...);

[[noreturn]] void log_fatal(const char* fmt, ...);
void set_fatal_sink(void (*sink)(const char* message));

}  // namespace looks
