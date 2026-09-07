#include "ui/interact.h"

#include <algorithm>

namespace looks::ui {

void Picker::add(float dist2, float radius, int tier, int slot) {
    if (slot < 0 || dist2 >= radius * radius) return;
    if (slot_ >= 0 && (tier < tier_ || (tier == tier_ && dist2 >= dist2_)))
        return;
    slot_ = slot;
    tier_ = tier;
    dist2_ = dist2;
}

void Picker::add_point(Vec2 mouse, Vec2 p, float radius, int tier, int slot) {
    const float dx = mouse.x - p.x;
    const float dy = mouse.y - p.y;
    add(dx * dx + dy * dy, radius, tier, slot);
}

void Picker::add_1d(float mouse_x, float x, float radius, int tier, int slot) {
    const float dx = mouse_x - x;
    add(dx * dx, radius, tier, slot);
}

void Picker::add_inside(bool inside, int tier, int slot) {
    if (!inside) return;
    add(0.0f, 1.0f, tier, slot);
}

namespace {

int caret_after_erase(const TextField& f) {
    return f.has_selection() ? f.sel_lo() : f.caret;
}

bool has_outside_selection(const TextField& f, char c) {
    const int lo = f.sel_lo(), hi = f.sel_hi();
    for (size_t i = 0; i < f.buf.size(); ++i) {
        const int at = static_cast<int>(i);
        if (f.has_selection() && at >= lo && at < hi) continue;
        if (f.buf[i] == c) return true;
    }
    return false;
}

bool filter_allows(const TextField& f, uint32_t cp) {
    const bool digit = cp >= '0' && cp <= '9';
    switch (f.filter) {
        case TextFilter::Printable:
            return cp >= 32 && cp <= 126;
        case TextFilter::Digits:
            return digit || (cp == '.' && !has_outside_selection(f, '.'));
        case TextFilter::Signed:
            if (digit) return true;
            if (cp == '.') return !has_outside_selection(f, '.');
            return cp == '-' && caret_after_erase(f) == 0 &&
                   !has_outside_selection(f, '-');
        case TextFilter::Uint:
            return digit;
        case TextFilter::Hex:
            return digit || (cp >= 'a' && cp <= 'f') ||
                   (cp >= 'A' && cp <= 'F');
    }
    return false;
}

void erase_selection(TextField& f) {
    const int lo = f.sel_lo(), hi = f.sel_hi();
    f.buf.erase(static_cast<size_t>(lo), static_cast<size_t>(hi - lo));
    f.caret = f.sel_anchor = lo;
}

bool is_continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

int step_left(const std::string& s, int at) {
    if (at <= 0) return 0;
    --at;
    while (at > 0 && is_continuation(s[static_cast<size_t>(at)])) --at;
    return at;
}

int step_right(const std::string& s, int at) {
    const int n = static_cast<int>(s.size());
    if (at >= n) return n;
    ++at;
    while (at < n && is_continuation(s[static_cast<size_t>(at)])) ++at;
    return at;
}

void move_caret(TextField& f, int to, bool select) {
    f.caret = std::clamp(to, 0, static_cast<int>(f.buf.size()));
    if (!select) f.sel_anchor = f.caret;
}

}  // namespace

void TextField::set(std::string text) {
    buf = std::move(text);
    caret = sel_anchor = static_cast<int>(buf.size());
}

std::string caret_text(const TextField& f, char marker) {
    const int len = static_cast<int>(f.buf.size());
    const int at = std::clamp(f.caret, 0, len);
    std::string out = f.buf;
    if (f.has_selection()) {
        const int other = std::clamp(f.sel_anchor, 0, len);
        out.insert(out.begin() + (at > other ? at : other), marker);
        out.insert(out.begin() + (at > other ? other : at), marker);
        return out;
    }
    out.insert(out.begin() + at, marker);
    return out;
}

TextResult text_field_key(TextField& f, const platform::Event& e) {
    const int len = static_cast<int>(f.buf.size());
    f.caret = std::clamp(f.caret, 0, len);
    f.sel_anchor = std::clamp(f.sel_anchor, 0, len);

    if (e.type == platform::Event::Type::Char) {
        if (!filter_allows(f, e.codepoint)) return TextResult::None;
        if (f.has_selection()) erase_selection(f);
        if (f.cap && f.buf.size() >= f.cap) return TextResult::None;
        f.buf.insert(f.buf.begin() + f.caret,
                     static_cast<char>(e.codepoint));
        ++f.caret;
        f.sel_anchor = f.caret;
        return TextResult::Edit;
    }
    if (e.type != platform::Event::Type::KeyDown) return TextResult::None;

    if ((e.mods & platform::kModCtrl) && e.key == platform::Key::A) {
        f.sel_anchor = 0;
        f.caret = len;
        return TextResult::Edit;
    }

    const bool shift = (e.mods & platform::kModShift) != 0;
    switch (e.key) {
        case platform::Key::Backspace:
            if (f.has_selection()) {
                erase_selection(f);
            } else if (f.caret > 0) {
                const int to = step_left(f.buf, f.caret);
                f.buf.erase(static_cast<size_t>(to),
                            static_cast<size_t>(f.caret - to));
                f.caret = to;
            } else {
                return TextResult::None;
            }
            f.sel_anchor = f.caret;
            return TextResult::Edit;
        case platform::Key::Delete:
            if (f.has_selection()) {
                erase_selection(f);
            } else if (f.caret < len) {
                const int to = step_right(f.buf, f.caret);
                f.buf.erase(static_cast<size_t>(f.caret),
                            static_cast<size_t>(to - f.caret));
            } else {
                return TextResult::None;
            }
            f.sel_anchor = f.caret;
            return TextResult::Edit;
        case platform::Key::Left:
            move_caret(f, step_left(f.buf, f.caret), shift);
            return TextResult::Edit;
        case platform::Key::Right:
            move_caret(f, step_right(f.buf, f.caret), shift);
            return TextResult::Edit;
        case platform::Key::Home:
            move_caret(f, 0, shift);
            return TextResult::Edit;
        case platform::Key::End:
            move_caret(f, len, shift);
            return TextResult::Edit;
        case platform::Key::Enter:
            return TextResult::Commit;
        case platform::Key::Escape:
            return TextResult::Cancel;
        default:
            return TextResult::None;
    }
}

}  // namespace looks::ui
