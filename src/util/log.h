// Tiny logging shim. GUI app — stderr may be invisible, so everything also
// goes to the debugger via OutputDebugString AND a per-session looks.log
// beside the exe (flushed per line so it survives abort()).
// printf-style, newline appended.

#pragma once

namespace looks {

void log_info(const char* fmt, ...);
void log_warn(const char* fmt, ...);
void log_error(const char* fmt, ...);

// Unrecoverable failure (device loss, allocation failure): logs, tells the
// user through the installed sink, then aborts. The app installs a sink at
// startup that raises a native message box; library code just calls
// log_fatal and stays platform-clean.
[[noreturn]] void log_fatal(const char* fmt, ...);
void set_fatal_sink(void (*sink)(const char* message));

}  // namespace looks
