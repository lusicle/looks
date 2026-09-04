#pragma once

#include <cstddef>
#include <string>

#include "platform/window.h"
#include "ui/types.h"

namespace looks::ui {

class Picker {
public:
    void add(float dist2, float radius, int tier, int slot);
    void add_point(Vec2 mouse, Vec2 p, float radius, int tier, int slot);
    void add_1d(float mouse_x, float x, float radius, int tier, int slot);
    void add_inside(bool inside, int tier, int slot);

    int best() const { return slot_; }
    bool hit() const { return slot_ >= 0; }
    int tier() const { return tier_; }

private:
    int slot_ = -1;
    int tier_ = 0;
    float dist2_ = 0.0f;
};

enum class TextFilter : uint8_t { Printable, Digits, Signed, Uint, Hex };
enum class TextResult : uint8_t { None, Edit, Commit, Cancel };

struct TextField {
    std::string buf;
    int caret = 0;
    int sel_anchor = 0;
    TextFilter filter = TextFilter::Printable;
    size_t cap = 0;

    void set(std::string text);
    bool has_selection() const { return caret != sel_anchor; }
    int sel_lo() const { return caret < sel_anchor ? caret : sel_anchor; }
    int sel_hi() const { return caret < sel_anchor ? sel_anchor : caret; }
};

TextResult text_field_key(TextField& f, const platform::Event& e);

std::string caret_text(const TextField& f, char marker = '_');

}  // namespace looks::ui
