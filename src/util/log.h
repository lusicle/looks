// Tiny logging shim. GUI app — stderr may be invisible, so everything also
// goes to the debugger via OutputDebugString. printf-style, newline appended.

#pragma once

namespace looks {

void log_info(const char* fmt, ...);
void log_warn(const char* fmt, ...);
void log_error(const char* fmt, ...);

}  // namespace looks
