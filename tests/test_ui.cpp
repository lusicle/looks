#include "test_framework.h"
#include "ui/canvas2d.h"
#include "ui/font.h"
#include "ui/text.h"

using namespace looks;
using namespace looks::ui;

TEST(ui_color_packing) {
    // Black and white are exact through the sRGB round trip.
    CHECK_EQ(Color::rgba(0, 0, 0, 1).to_rgba8(), 0xFF000000u);
    CHECK_EQ(Color::rgba(1, 1, 1, 1).to_rgba8(), 0xFFFFFFFFu);
    // Alpha stays linear (0.5 -> 128).
    const uint32_t half = Color::rgba(0, 0, 0, 0.5f).to_rgba8();
    CHECK_EQ(half >> 24, 0x80u);
    // hex() decodes sRGB bytes; re-encoding returns the same bytes.
    const uint32_t packed = Color::hex(0x4C9FE8).to_rgba8();
    CHECK_EQ(packed & 0xFF, 0x4Cu);
    CHECK_EQ((packed >> 8) & 0xFF, 0x9Fu);
    CHECK_EQ((packed >> 16) & 0xFF, 0xE8u);
}

TEST(ui_canvas_batching_merges_solids) {
    Canvas2D canvas;
    canvas.begin_frame(1.0f, {800, 600});
    canvas.draw_rect({0, 0, 10, 10}, Color::hex(0xFF0000));
    canvas.draw_rect({20, 0, 10, 10}, Color::hex(0x00FF00));
    canvas.draw_line({0, 0}, {50, 50}, 2.0f, Color::hex(0x0000FF));
    canvas.draw_sdf_rect({0, 100, 40, 40}, 8.0f, Color::hex(0xFFFFFF));
    CHECK_EQ(canvas.batches().size(), size_t{1});   // all Solid, same clip
    CHECK_EQ(canvas.total_index_count(), uint32_t{24});
    CHECK_EQ(canvas.vertices().size(), size_t{16});
}

TEST(ui_canvas_scale_applied_at_vertex_write) {
    Canvas2D canvas;
    canvas.begin_frame(2.0f, {400, 300});
    canvas.draw_rect({10, 20, 30, 40}, Color::hex(0xFFFFFF));
    const Vertex& tl = canvas.vertices()[0];
    CHECK_EQ(tl.pos.x, 20.0f);
    CHECK_EQ(tl.pos.y, 40.0f);
    const Vertex& br = canvas.vertices()[2];
    CHECK_EQ(br.pos.x, 80.0f);
    CHECK_EQ(br.pos.y, 120.0f);
}

TEST(ui_canvas_clip_stack) {
    Canvas2D canvas;
    canvas.begin_frame(1.0f, {800, 600});
    canvas.draw_rect({0, 0, 10, 10}, Color::hex(0xFFFFFF));
    canvas.push_clip({100, 100, 200, 200});
    canvas.draw_rect({100, 100, 10, 10}, Color::hex(0xFFFFFF));
    canvas.push_clip({150, 150, 500, 500});
    canvas.draw_rect({150, 150, 10, 10}, Color::hex(0xFFFFFF));
    const Rect clip = canvas.current_clip_physical();
    CHECK_EQ(clip.x, 150.0f);
    CHECK_EQ(clip.y, 150.0f);
    CHECK_EQ(clip.w, 150.0f);   // clamped to parent's right edge at 300
    canvas.pop_clip();
    canvas.pop_clip();
    CHECK(canvas.current_clip_physical().empty());
    CHECK_EQ(canvas.batches().size(), size_t{3});   // scissor changes split batches
}

TEST(ui_canvas_collapsed_clip_drops_draws) {
    Canvas2D canvas;
    canvas.begin_frame(1.0f, {800, 600});
    canvas.push_clip({0, 0, 100, 100});
    canvas.push_clip({200, 200, 100, 100});   // disjoint -> collapsed
    canvas.draw_rect({0, 0, 500, 500}, Color::hex(0xFFFFFF));
    CHECK_EQ(canvas.total_index_count(), uint32_t{0});
    canvas.pop_clip();
    canvas.pop_clip();
}

TEST(ui_canvas_sdf_shape_attributes) {
    Canvas2D canvas;
    canvas.begin_frame(2.0f, {400, 300});
    canvas.draw_sdf_rect({0, 0, 100, 50}, 10.0f, Color::hex(0xFFFFFF));
    const Vertex& v = canvas.vertices()[0];
    CHECK_EQ(v.shape[0], 20.0f);    // radius, physical px
    CHECK_EQ(v.shape[1], 0.0f);     // no stroke
    CHECK_EQ(v.shape[2], 100.0f);   // halfW physical
    CHECK_EQ(v.shape[3], 50.0f);    // halfH physical
    // Radius clamps to half the short side.
    canvas.draw_sdf_rect({0, 100, 100, 10}, 50.0f, Color::hex(0xFFFFFF));
    CHECK_EQ(canvas.vertices()[4].shape[0], 10.0f);   // 5 logical * 2
}

TEST(ui_utf8_decode) {
    const char* text = "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    size_t cursor = 0;
    const size_t len = 10;
    CHECK_EQ(utf8_decode(text, len, &cursor), 0x41u);
    CHECK_EQ(utf8_decode(text, len, &cursor), 0xE9u);
    CHECK_EQ(utf8_decode(text, len, &cursor), 0x20ACu);
    CHECK_EQ(utf8_decode(text, len, &cursor), 0x1F600u);
    CHECK_EQ(cursor, len);
    // 0x80 is a lone continuation byte.
    const char bad[] = "\x80x";
    cursor = 0;
    CHECK_EQ(utf8_decode(bad, 2, &cursor), 0xFFFDu);
    CHECK_EQ(cursor, size_t{1});
    // 0xC0 0xAF is an overlong encoding.
    const char overlong[] = "\xC0\xAF";
    cursor = 0;
    CHECK_EQ(utf8_decode(overlong, 2, &cursor), 0xFFFDu);
}

TEST(ui_font_and_measure) {
    Font font = Font::create_debug();
    CHECK(font.find_glyph('A') != nullptr);
    CHECK_EQ(font.find_glyph('A')->codepoint, uint32_t{'A'});
    CHECK_EQ(font.find_glyph(0x4E2D)->codepoint, uint32_t{'?'});
    CHECK(!font.find_glyph(' ')->has_geometry);
    CHECK(font.find_glyph('g')->has_geometry);
    // Monospace: N chars at size S measure N*S wide.
    const Vec2 m = measure_text(font, "hello", 16.0f);
    CHECK_EQ(m.x, 80.0f);
    CHECK_EQ(m.y, 16.0f * 1.25f);
    bool any = false;
    for (uint8_t p : font.atlas_pixels()) any |= p != 0;
    CHECK(any);
}

TEST(ui_text_emits_glyph_quads) {
    Font font = Font::create_debug();
    // draw_text emits nothing without a texture, so set a fake one.
    const UiTexture* fake = reinterpret_cast<const UiTexture*>(0x1);
    font.set_texture(fake);

    Canvas2D canvas;
    canvas.begin_frame(1.0f, {800, 600});
    ui::draw_text(canvas, font, "ok go", {10, 10}, 16.0f, Color::hex(0xFFFFFF));
    // The space makes no quad, so 5 chars give 4 quads.
    CHECK_EQ(canvas.batches().size(), size_t{1});
    CHECK_EQ(canvas.batches()[0].kind == BatchKind::Text, true);
    CHECK_EQ(canvas.batches()[0].texture, fake);
    CHECK_EQ(canvas.total_index_count(), uint32_t{4 * 6});
}
