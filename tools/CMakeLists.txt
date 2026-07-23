# Build-time asset tools (spec §12). The dither LUT is generated once per
# build into gen/ and staged next to the exe with the other assets.

add_executable(looks_bluenoise bluenoise/bluenoise.cpp)

set(LOOKS_GEN_DIR "${CMAKE_BINARY_DIR}/gen" PARENT_SCOPE)
set(_gen "${CMAKE_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${_gen}")

add_custom_command(
    OUTPUT "${_gen}/dither_lut.bin"
    COMMAND looks_bluenoise "${_gen}/dither_lut.bin"
    DEPENDS looks_bluenoise
    COMMENT "Generating blue-noise / STBN dither LUT"
    VERBATIM)
set(_gen_outputs "${_gen}/dither_lut.bin")

# UI font bake (spec §12 fontbake): TTF -> MSDF atlas + metrics via the
# vendored msdf-atlas-gen CLI, decoded at runtime by our own PNG/JSON
# readers and drawn by the first-party MSDF text shader. Skipped (debug
# font fallback) when the tool or font is absent.
set(_msdf_tool "${CMAKE_SOURCE_DIR}/third_party/msdfgen/msdf-atlas-gen.exe")
set(_ui_ttf "${CMAKE_SOURCE_DIR}/assets/fonts/Roboto.ttf")
set(_header_ttf "${CMAKE_SOURCE_DIR}/assets/fonts/Cormorant.ttf")
if (EXISTS "${_msdf_tool}" AND EXISTS "${_ui_ttf}")
    add_custom_command(
        OUTPUT "${_gen}/ui_font.png" "${_gen}/ui_font.json"
        # Bake near the smallest rendered size: SDFs magnify cleanly but
        # degrade under heavy minification (no mips on the atlas).
        COMMAND "${_msdf_tool}" -font "${_ui_ttf}"
                -type msdf -size 24 -pxrange 3 -yorigin top -format png
                -imageout "${_gen}/ui_font.png"
                -json "${_gen}/ui_font.json"
        DEPENDS "${_ui_ttf}"
        COMMENT "Baking MSDF UI font atlas (Roboto)"
        VERBATIM)
    list(APPEND _gen_outputs "${_gen}/ui_font.png" "${_gen}/ui_font.json")
endif()
if (EXISTS "${_msdf_tool}" AND EXISTS "${_header_ttf}")
    add_custom_command(
        OUTPUT "${_gen}/header_font.png" "${_gen}/header_font.json"
        # Headers render larger and serifs carry finer detail: bake bigger.
        # Cormorant is a light face — pin the variable wght axis to
        # SemiBold so headers hold their own against the sans body.
        COMMAND "${_msdf_tool}" -varfont "${_header_ttf}?wght=600"
                -type msdf -size 32 -pxrange 3 -yorigin top -format png
                -imageout "${_gen}/header_font.png"
                -json "${_gen}/header_font.json"
        DEPENDS "${_header_ttf}"
        COMMENT "Baking MSDF header font atlas (Cormorant SemiBold)"
        VERBATIM)
    list(APPEND _gen_outputs "${_gen}/header_font.png" "${_gen}/header_font.json")
endif()

add_custom_target(looks_gen_assets DEPENDS ${_gen_outputs})
