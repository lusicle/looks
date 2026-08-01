cmake_minimum_required(VERSION 3.25)
project(looks LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "looks v1 is Windows-only; the platform layer lives in src/platform/win.")
endif()

# Warning level: high, but third_party headers (VMA, miniaudio) are consumed
# as SYSTEM includes so their noise never reaches us.
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:preprocessor /EHsc)
    # C4201: nameless struct/union — used deliberately by our math types.
    add_compile_options(/wd4201)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    # Drop /RTC1 from Debug: it is incompatible with per-target /O2, and the
    # pixel-loop libraries (mezzanine codec) must run optimized even in
    # Debug builds or import crawls (import ≥ 2× realtime).
    # Coverage lost is stack-frame/uninitialized checks; the test suites and
    # Vulkan validation are the project's real safety nets.
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()

set(LOOKS_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

include(cmake/slang.cmake)

enable_testing()

add_subdirectory(tools)
add_subdirectory(src)
add_subdirectory(shaders)
add_subdirectory(tests)
