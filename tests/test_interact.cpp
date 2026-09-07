#include "test_framework.h"
#include "ui/interact.h"

using namespace looks;
using namespace looks::ui;

TEST(picker_nearest_wins_inside_a_tier) {
    Picker p;
    p.add_point({5, 0}, {12, 0}, 9.0f, 0, 1);
    p.add_point({5, 0}, {0, 0}, 9.0f, 0, 2);
    CHECK_EQ(p.best(), 2);

    Picker q;
    q.add_point({5, 0}, {0, 0}, 9.0f, 0, 2);
    q.add_point({5, 0}, {12, 0}, 9.0f, 0, 1);
    CHECK_EQ(q.best(), 2);
}

TEST(picker_higher_tier_beats_a_nearer_target) {
    Picker p;
    p.add_point({0, 0}, {1, 0}, 9.0f, 0, 1);
    p.add_point({0, 0}, {8, 0}, 9.0f, 2, 2);
    CHECK_EQ(p.best(), 2);
    CHECK_EQ(p.tier(), 2);
}

TEST(picker_radius_is_euclidean) {
    Picker p;
    p.add_point({6, 6}, {0, 0}, 6.0f, 0, 1);
    CHECK(!p.hit());
    CHECK_EQ(p.best(), -1);

    Picker q;
    q.add_point({4, 4}, {0, 0}, 6.0f, 0, 1);
    CHECK(q.hit());
}

TEST(picker_1d_and_region_forms) {
    Picker p;
    p.add_1d(10.0f, 14.0f, 5.0f, 0, 7);
    CHECK_EQ(p.best(), 7);
    p.add_1d(10.0f, 11.0f, 5.0f, 0, 8);
    CHECK_EQ(p.best(), 8);
    p.add_1d(10.0f, 20.0f, 5.0f, 0, 9);
    CHECK_EQ(p.best(), 8);

    Picker q;
    q.add_inside(true, 0, 3);
    CHECK_EQ(q.best(), 3);
    q.add_inside(true, 1, 4);
    CHECK_EQ(q.best(), 4);
    q.add_inside(false, 5, 9);
    CHECK_EQ(q.best(), 4);
    q.add_point({0, 0}, {1, 0}, 4.0f, 2, 5);
    CHECK_EQ(q.best(), 5);
}

TEST(picker_empty_and_out_of_range) {
    Picker p;
    CHECK(!p.hit());
    CHECK_EQ(p.best(), -1);
    p.add_point({100, 100}, {0, 0}, 9.0f, 0, 1);
    CHECK(!p.hit());
    p.add(0.0f, 0.0f, 0, 1);
    CHECK(!p.hit());
}

namespace {

platform::Event char_ev(uint32_t cp) {
    platform::Event e{};
    e.type = platform::Event::Type::Char;
    e.codepoint = cp;
    return e;
}

platform::Event key_ev(platform::Key k, uint32_t mods = 0) {
    platform::Event e{};
    e.type = platform::Event::Type::KeyDown;
    e.key = k;
    e.mods = mods;
    return e;
}

void type_in(TextField& f, const char* s) {
    for (const char* p = s; *p; ++p)
        text_field_key(f, char_ev(static_cast<uint32_t>(*p)));
}

}  // namespace

TEST(text_field_backspace_uses_the_caret) {
    TextField f;
    type_in(f, "abcd");
    CHECK_EQ(f.buf, std::string("abcd"));
    CHECK_EQ(f.caret, 4);

    text_field_key(f, key_ev(platform::Key::Left));
    text_field_key(f, key_ev(platform::Key::Left));
    CHECK_EQ(f.caret, 2);
    CHECK_EQ(text_field_key(f, key_ev(platform::Key::Backspace)),
             TextResult::Edit);
    CHECK_EQ(f.buf, std::string("acd"));
    CHECK_EQ(f.caret, 1);

    CHECK_EQ(text_field_key(f, key_ev(platform::Key::Delete)),
             TextResult::Edit);
    CHECK_EQ(f.buf, std::string("ad"));
}

TEST(text_field_selection_and_home_end) {
    TextField f;
    type_in(f, "hello");
    text_field_key(f, key_ev(platform::Key::Home));
    CHECK_EQ(f.caret, 0);
    CHECK(!f.has_selection());

    text_field_key(f, key_ev(platform::Key::Right, platform::kModShift));
    text_field_key(f, key_ev(platform::Key::Right, platform::kModShift));
    CHECK(f.has_selection());
    CHECK_EQ(f.sel_lo(), 0);
    CHECK_EQ(f.sel_hi(), 2);

    type_in(f, "H");
    CHECK_EQ(f.buf, std::string("Hllo"));
    CHECK(!f.has_selection());

    text_field_key(f, key_ev(platform::Key::End));
    CHECK_EQ(f.caret, 4);
    text_field_key(f, key_ev(platform::Key::A, platform::kModCtrl));
    type_in(f, "3");
    CHECK_EQ(f.buf, std::string("3"));
    CHECK(!f.has_selection());
}

TEST(text_field_filters_and_cap) {
    TextField d;
    d.filter = TextFilter::Digits;
    type_in(d, "1a2.3.4");
    CHECK_EQ(d.buf, std::string("12.34"));

    TextField s;
    s.filter = TextFilter::Signed;
    type_in(s, "-1-2");
    CHECK_EQ(s.buf, std::string("-12"));
    TextField s2;
    s2.filter = TextFilter::Signed;
    type_in(s2, "1-2");
    CHECK_EQ(s2.buf, std::string("12"));

    TextField p;
    p.cap = 3;
    type_in(p, "abcdef");
    CHECK_EQ(p.buf, std::string("abc"));

    TextField pr;
    text_field_key(pr, char_ev(9));
    text_field_key(pr, char_ev(200));
    CHECK(pr.buf.empty());
}

TEST(text_field_uint_and_hex_filters) {
    TextField u;
    u.filter = TextFilter::Uint;
    type_in(u, "1.2-3a4");
    CHECK_EQ(u.buf, std::string("1234"));

    TextField h;
    h.filter = TextFilter::Hex;
    h.cap = 6;
    type_in(h, "ff00AAzz99");
    CHECK_EQ(h.buf, std::string("ff00AA"));
}

TEST(caret_text_marks_the_caret_position) {
    TextField f;
    f.set("612");
    CHECK_EQ(caret_text(f), std::string("612_"));
    text_field_key(f, key_ev(platform::Key::Home));
    CHECK_EQ(caret_text(f), std::string("_612"));
    text_field_key(f, key_ev(platform::Key::Right));
    CHECK_EQ(caret_text(f, '|'), std::string("6|12"));

    TextField e;
    CHECK_EQ(caret_text(e), std::string("_"));
}

TEST(caret_text_marks_both_ends_of_a_selection) {
    TextField f;
    f.set("hello");
    text_field_key(f, key_ev(platform::Key::Home));
    text_field_key(f, key_ev(platform::Key::Right, platform::kModShift));
    text_field_key(f, key_ev(platform::Key::Right, platform::kModShift));
    CHECK(f.has_selection());
    CHECK_EQ(caret_text(f), std::string("_he_llo"));

    TextField b;
    b.set("hello");
    text_field_key(b, key_ev(platform::Key::Left, platform::kModShift));
    CHECK_EQ(caret_text(b), std::string("hell_o_"));
}

TEST(text_field_filter_looks_past_the_selection) {
    TextField d;
    d.filter = TextFilter::Digits;
    type_in(d, "1.5");
    text_field_key(d, key_ev(platform::Key::Home));
    for (int i = 0; i < 3; ++i)
        text_field_key(d, key_ev(platform::Key::Right, platform::kModShift));
    type_in(d, ".");
    CHECK_EQ(d.buf, std::string("."));

    TextField s;
    s.filter = TextFilter::Signed;
    type_in(s, "-7");
    text_field_key(s, key_ev(platform::Key::Home));
    text_field_key(s, key_ev(platform::Key::Right, platform::kModShift));
    type_in(s, "-");
    CHECK_EQ(s.buf, std::string("-7"));
}

TEST(text_field_steps_whole_utf8_sequences) {
    const std::string pound = "\xc2\xa3";
    TextField f;
    f.set("a" + pound + "b");
    CHECK_EQ(static_cast<size_t>(f.caret), f.buf.size());

    text_field_key(f, key_ev(platform::Key::Left));
    CHECK_EQ(f.caret, 3);
    text_field_key(f, key_ev(platform::Key::Left));
    CHECK_EQ(f.caret, 1);
    text_field_key(f, key_ev(platform::Key::Right));
    CHECK_EQ(f.caret, 3);

    text_field_key(f, key_ev(platform::Key::Backspace));
    CHECK_EQ(f.buf, std::string("ab"));
    CHECK_EQ(f.caret, 1);

    TextField d;
    d.set("a" + pound + "b");
    text_field_key(d, key_ev(platform::Key::Home));
    text_field_key(d, key_ev(platform::Key::Right));
    CHECK_EQ(text_field_key(d, key_ev(platform::Key::Delete)),
             TextResult::Edit);
    CHECK_EQ(d.buf, std::string("ab"));
}

TEST(text_field_commit_and_cancel) {
    TextField f;
    f.set("seed");
    CHECK_EQ(f.caret, 4);
    CHECK_EQ(text_field_key(f, key_ev(platform::Key::Enter)),
             TextResult::Commit);
    CHECK_EQ(text_field_key(f, key_ev(platform::Key::Escape)),
             TextResult::Cancel);
    CHECK_EQ(text_field_key(f, key_ev(platform::Key::F1)),
             TextResult::None);
    CHECK_EQ(f.buf, std::string("seed"));
}
