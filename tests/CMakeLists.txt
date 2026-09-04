add_executable(looks_tests
    test_main.cpp
    test_json.cpp
    test_command.cpp
    test_stack.cpp
    test_look.cpp
    test_graph.cpp
    test_nesting.cpp
    test_instances.cpp
    test_audio_mix.cpp
    test_interact.cpp
    test_layout.cpp
    test_ui.cpp
    test_bmff.cpp
    test_frame_index.cpp
    test_codec.cpp
    test_mux.cpp
    test_mod.cpp
    test_mosh.cpp
    test_image.cpp
    test_serialize.cpp
    test_cache.cpp
    test_script.cpp
    test_refdump.cpp
    test_truetype.cpp
    test_error_diffusion.cpp
    test_shape_sdf.cpp
    test_track.cpp
    test_numerics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/gfx/shape_sdf.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/gfx/graph.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/gfx/render_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/gfx/error_diffusion.cpp)
target_include_directories(looks_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# The dither equivalence tests walk hundreds of frames of CPU pixel loops;
# same optimized-in-Debug tradeoff as looks_gfx/looks_codec.
if(MSVC)
    target_compile_options(looks_tests PRIVATE $<$<CONFIG:Debug>:/O2 /Ob2 /Oi>)
endif()
# test_serialize validates the shipped era presets in-place.
target_compile_definitions(looks_tests PRIVATE
    LOOKS_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(looks_tests PRIVATE
    looks_util looks_doc looks_ui_core looks_media looks_codec looks_mod
    looks_script)

add_test(NAME looks_tests COMMAND looks_tests)

# GPU determinism harness: renders through the real engine twice and
# compares hashes; skips cleanly on machines without a Vulkan device.
add_test(NAME looks_determinism COMMAND looks_rendertest 12)
set_tests_properties(looks_determinism PROPERTIES SKIP_RETURN_CODE 77)

# Export smoke: the full offline pipeline — engine -> NV12 readback ->
# H.264 encoder MFT (hardware async path when available, software
# fallback) -> mux -> re-demux with our own parser.
add_test(NAME looks_export
         COMMAND looks_rendertest 12 --export rendertest_export.mp4)
set_tests_properties(looks_export PROPERTIES SKIP_RETURN_CODE 77)

# Per-effect performance table (opt-in): runs only when
# temp/bench_request.txt contains '1'; writes temp/bench.txt.
add_test(NAME looks_bench COMMAND looks_rendertest --bench)
set_tests_properties(looks_bench PROPERTIES SKIP_RETURN_CODE 77
                     TIMEOUT 600)
