# Slang -> SPIR-V offline compilation.
#
# Shaders are compiled at build time by the pinned toolchain in
# third_party/slang/bin/slangc.exe — there is no runtime shader compiler.
# Entry points are declared in-source with [shader("vertex")] etc., so a
# single slangc invocation per file suffices; stage is inferred from the
# attribute. Artifacts land in ${CMAKE_BINARY_DIR}/shaders/<name>.spv where
# <name> strips the trailing .slang (ui.vert.slang -> ui.vert.spv).

set(LOOKS_SLANGC "${LOOKS_THIRD_PARTY}/slang/bin/slangc.exe")
if(NOT EXISTS "${LOOKS_SLANGC}")
    message(FATAL_ERROR "slangc not found at ${LOOKS_SLANGC}")
endif()

set(LOOKS_SHADER_BIN "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${LOOKS_SHADER_BIN}")

# add_slang_shaders(<target> SOURCES a.vert.slang b.frag.slang ...)
#
# Creates a custom target that compiles each source to SPIR-V. Paths are
# relative to the calling CMakeLists. Extra include files that shaders
# `import`/`#include` can be listed after DEPENDS to trigger rebuilds.
function(add_slang_shaders TARGET)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS" ${ARGN})
    set(_outputs "")
    foreach(_src ${ARG_SOURCES})
        get_filename_component(_abs "${_src}" ABSOLUTE)
        get_filename_component(_name "${_src}" NAME_WLE)   # strips .slang
        set(_out "${LOOKS_SHADER_BIN}/${_name}.spv")
        add_custom_command(
            OUTPUT "${_out}"
            COMMAND "${LOOKS_SLANGC}" "${_abs}"
                    -target spirv
                    -O2
                    -matrix-layout-column-major
                    -o "${_out}"
            DEPENDS "${_abs}" ${ARG_DEPENDS}
            COMMENT "slangc ${_src}"
            VERBATIM)
        list(APPEND _outputs "${_out}")
    endforeach()
    add_custom_target(${TARGET} DEPENDS ${_outputs})
endfunction()

# Copies the compiled shader directory next to an executable so the app can
# resolve them exe-relative at runtime regardless of working directory.
function(target_stage_shaders EXE SHADER_TARGET)
    add_dependencies(${EXE} ${SHADER_TARGET})
    add_custom_command(TARGET ${EXE} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${LOOKS_SHADER_BIN}" "$<TARGET_FILE_DIR:${EXE}>/shaders"
        COMMENT "Staging shaders for ${EXE}")
endfunction()
