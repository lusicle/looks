# Module libraries. Everything includes from src/ root ("util/json.h").

# ---- util: JSON, logging, files, small math. No graphics deps.
add_library(looks_util STATIC
    util/json.cpp
    util/log.cpp
    util/file.cpp
    util/inflate.cpp
    util/numerics.cpp
    util/image.cpp)
target_sources(looks_util PRIVATE util/linear_image.cpp)
target_include_directories(looks_util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Pixel/byte loops (image decode, inflate, the doc-hash JSON writer) are
# hot on the import and autosave paths - same Debug-/O2 tradeoff as
# looks_codec below.
if(MSVC)
    target_compile_options(looks_util PRIVATE $<$<CONFIG:Debug>:/O2 /Ob2 /Oi>)
endif()

# ---- doc: document model + command/undo core + effect stack + mod data.
add_library(looks_doc STATIC
    doc/command.cpp
    doc/effects.cpp
    doc/stack_commands.cpp
    doc/mod_commands.cpp
    doc/layer_commands.cpp
    doc/placement_commands.cpp
    doc/group_commands.cpp
    doc/look_commands.cpp
    doc/instances.cpp
    doc/serialize.cpp
    doc/preset.cpp
    doc/randomize.cpp)
target_include_directories(looks_doc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(looks_doc PUBLIC looks_util)

# ---- script: the automation language core (lexer/compiler/VM). Pure
# std C++ - the app registers every native that touches it.
add_library(looks_script STATIC
    script/script.cpp)
target_include_directories(looks_script PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ---- mod: param table, modulation evaluation, FFT + import analysis.
# Pure CPU, deterministic — unit-tested without a GPU.
add_library(looks_mod STATIC
    mod/param_table.cpp
    mod/eval.cpp
    mod/fft.cpp
    mod/analysis.cpp)
target_include_directories(looks_mod PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(looks_mod PUBLIC looks_doc looks_util)
# FFT + per-frame analysis run over whole tracks at import and on every
# wired-chain rebuild - optimized in every config, like looks_codec.
if(MSVC)
    target_compile_options(looks_mod PRIVATE $<$<CONFIG:Debug>:/O2 /Ob2 /Oi>)
endif()

# ---- codec: codec_core + mezzanine + Codec-Box mosh codec. Pure CPU.
add_library(looks_codec STATIC
    codec/core.cpp
    codec/mez.cpp
    codec/mosh/mosh.cpp)
target_include_directories(looks_codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The DCT/quant/Huffman loops are the import bottleneck — optimize them in
# every config (/Od codegen made import ~10× slower than realtime). Last
# option wins on MSVC, so this overrides the Debug default /Od.
if(MSVC)
    target_compile_options(looks_codec PRIVATE $<$<CONFIG:Debug>:/O2 /Ob2 /Oi>)
endif()

# ---- platform: window/input/dialogs + Media Foundation MFT glue.
add_library(looks_platform STATIC
    platform/win/win_window.cpp
    platform/win/win_dialog.cpp
    platform/win/mf_codec.cpp
    platform/win/wic_image.cpp)
target_include_directories(looks_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(looks_platform PRIVATE looks_util)
# The MFT sample copies move 12+ MB per 4K frame - optimized in every
# config, like looks_codec.
if(MSVC)
    target_compile_options(looks_platform PRIVATE
        $<$<CONFIG:Debug>:/O2 /Ob2 /Oi>)
endif()

# ---- media: BMFF demux, PCM sidecar, import pipeline (muxer + player next).
add_library(looks_media STATIC
    media/bmff.cpp
    media/bmff_mux.cpp
    media/frame_index.cpp
    media/h264_util.cpp
    media/pcm.cpp
    media/thumbs.cpp
    media/wav.cpp
    media/mp3.cpp
    media/import.cpp
    media/cache.cpp
    media/export.cpp
    media/player.cpp
    media/audio_mix.cpp
    media/decode_pool.cpp
    media/track.cpp
    media/miniaudio_impl.cpp)
target_include_directories(looks_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(looks_media SYSTEM PUBLIC
    ${LOOKS_THIRD_PARTY}/miniaudio)
target_link_libraries(looks_media PUBLIC
    looks_util looks_codec looks_platform looks_mod)
# The import's per-frame pixel stages (NV12->I420, proxy downsample) and
# the audio mix DSP live here - optimized in every config, like
# looks_codec (at /Od these made 4K import ~2x slower than the codec).
if(MSVC)
    target_compile_options(looks_media PRIVATE
        $<$<CONFIG:Debug>:/O2 /Ob2 /Oi>)
endif()

# ---- gfx: Vulkan loader/device/swapchain/frame loop + VMA + the engine
# (graph compiler/evaluator, pooled targets, compute dispatch, viewport
# blit, export readback). graph.cpp is Vulkan-free so the DAG core stays
# unit-testable.
add_library(looks_gfx STATIC
    gfx/vk_api.cpp
    gfx/vk_device.cpp
    gfx/vk_swapchain.cpp
    gfx/renderer.cpp
    gfx/texture.cpp
    gfx/compute.cpp
    gfx/graph.cpp
    gfx/render_cache.cpp
    gfx/engine.cpp
    gfx/error_diffusion.cpp
    gfx/shape_sdf.cpp
    gfx/viewport_pass.cpp
    gfx/readback.cpp
    gfx/vma_impl.cpp)
target_include_directories(looks_gfx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(looks_gfx SYSTEM PUBLIC
    ${LOOKS_THIRD_PARTY}          # <vulkan/vulkan.h>
    ${LOOKS_THIRD_PARTY}/vma)     # <vk_mem_alloc.h>
target_compile_definitions(looks_gfx PUBLIC
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
# The engine carries CPU pixel loops (error-diffusion dither, staging
# conversions) — at /Od the dither alone is ~470 ms/frame at 1080p vs ~20
# optimized. Same tradeoff as looks_codec: optimized even in Debug.
if(MSVC)
    target_compile_options(looks_gfx PRIVATE $<$<CONFIG:Debug>:/O2 /Ob2 /Oi>)
endif()
# looks_ui_core supplies the baked-MSDF font loader the Text effect
# re-uses (ui/font.h is CPU-only — no Vulkan, no cycle).
target_link_libraries(looks_gfx PUBLIC looks_util looks_doc looks_ui_core)

# ---- ui core: CPU-side toolkit (canvas2d recorder, font, text). No Vulkan
# includes — unit-testable without a GPU or the vulkan headers.
add_library(looks_ui_core STATIC
    ui/canvas2d.cpp
    ui/font.cpp
    ui/input.cpp
    ui/interact.cpp
    ui/layout.cpp
    ui/probe.cpp
    ui/text.cpp
    ui/truetype.cpp
    ui/ui_context.cpp
    ui/widgets.cpp)
target_include_directories(looks_ui_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(looks_ui_core PUBLIC looks_util)

# ---- ui: the Vulkan renderer over the core.
add_library(looks_ui STATIC
    ui/ui_renderer.cpp)
target_include_directories(looks_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(looks_ui PUBLIC looks_ui_core looks_gfx looks_util)

# ---- app
add_executable(looks WIN32
    app/mode_renderer.cpp
    app/flow_canvas.cpp
    app/main.cpp)
if(MSVC)
    target_compile_options(looks PRIVATE /bigobj)
endif()
target_link_libraries(looks PRIVATE
    looks_platform looks_gfx looks_ui looks_doc looks_media looks_mod
    looks_script looks_util)
target_stage_shaders(looks looks_shaders)

# Ship assets (era presets, generated dither LUT) next to the exe so the
# app resolves them exe-relative like shaders.
add_dependencies(looks looks_gen_assets)
add_custom_command(TARGET looks POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:looks>/assets"
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:looks>/assets/noise"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/gen/dither_lut.bin"
            "$<TARGET_FILE_DIR:looks>/assets/noise/dither_lut.bin"
    COMMENT "Staging assets for looks")

# Baked MSDF fonts (present only when the msdf-atlas-gen tool exists).
if (EXISTS "${CMAKE_SOURCE_DIR}/third_party/msdfgen/msdf-atlas-gen.exe"
    AND EXISTS "${CMAKE_SOURCE_DIR}/assets/fonts/Roboto.ttf")
    add_custom_command(TARGET looks POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/gen/ui_font.png"
                "$<TARGET_FILE_DIR:looks>/assets/fonts/ui_font.png"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/gen/ui_font.json"
                "$<TARGET_FILE_DIR:looks>/assets/fonts/ui_font.json"
        COMMENT "Staging baked UI font")
endif()
if (EXISTS "${CMAKE_SOURCE_DIR}/third_party/msdfgen/msdf-atlas-gen.exe"
    AND EXISTS "${CMAKE_SOURCE_DIR}/assets/fonts/Cormorant.ttf")
    add_custom_command(TARGET looks POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/gen/header_font.png"
                "$<TARGET_FILE_DIR:looks>/assets/fonts/header_font.png"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/gen/header_font.json"
                "$<TARGET_FILE_DIR:looks>/assets/fonts/header_font.json"
        COMMENT "Staging baked header font")
endif()

# Dev CLIs: import pipeline + MP4 fixture generator (console apps).
add_executable(looks_import app/import_cli.cpp)
target_link_libraries(looks_import PRIVATE looks_media)

add_executable(looks_makefixture app/makefixture_cli.cpp)
target_link_libraries(looks_makefixture PRIVATE looks_media)

add_executable(looks_playtest app/playtest_cli.cpp)
target_link_libraries(looks_playtest PRIVATE looks_media)

# Determinism harness + headless export smoke; shaders are staged
# into the shared runtime dir by the `looks` target.
add_executable(looks_rendertest app/rendertest_cli.cpp)
target_compile_definitions(looks_rendertest PRIVATE
    LOOKS_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(looks_rendertest PRIVATE
    looks_gfx looks_ui looks_doc looks_media looks_util)
add_dependencies(looks_rendertest looks)
