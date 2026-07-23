// Minimal hand-rolled JSON reader/writer (spec §10, §12).
//
// Projects and presets are single JSON files. This is a small DOM model:
// parse to a Value tree, mutate, write back. Objects preserve insertion
// order so saved projects diff cleanly; member lookup is linear, which is
// fine at project-file scale. Numbers are doubles (exact for integers to
// 2^53 — frame indices and seeds fit comfortably).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace looks::json {

class Value;

using Array = std::vector<Value>;
using Member = std::pair<std::string, Value>;
using Object = std::vector<Member>;

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() : type_(Type::Null) {}
    Value(std::nullptr_t) : type_(Type::Null) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(double n) : type_(Type::Number), num_(n) {}
    Value(int n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    Value(int64_t n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    Value(uint32_t n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    Value(const char* s) : type_(Type::String), str_(s) {}
    Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Value(std::string_view s) : type_(Type::String), str_(s) {}
    Value(Array a) : type_(Type::Array), arr_(std::move(a)) {}
    Value(Object o) : type_(Type::Object), obj_(std::move(o)) {}

    static Value make_array() { return Value(Array{}); }
    static Value make_object() { return Value(Object{}); }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    // Typed access with defaults — never throws; wrong-type reads yield the
    // fallback so loading a slightly-off project degrades instead of dying.
    bool as_bool(bool fallback = false) const { return is_bool() ? bool_ : fallback; }
    double as_number(double fallback = 0.0) const { return is_number() ? num_ : fallback; }
    int64_t as_int(int64_t fallback = 0) const {
        return is_number() ? static_cast<int64_t>(num_) : fallback;
    }
    const std::string& as_string() const {
        static const std::string empty;
        return is_string() ? str_ : empty;
    }

    const Array& array() const { static const Array empty; return is_array() ? arr_ : empty; }
    Array& array() { return arr_; }
    const Object& object() const { static const Object empty; return is_object() ? obj_ : empty; }
    Object& object() { return obj_; }

    // Object member access. get() returns null-Value for missing keys.
    const Value* find(std::string_view key) const;
    const Value& get(std::string_view key) const;
    // Insert-or-assign, preserving first-insertion order.
    Value& set(std::string_view key, Value v);

    void push(Value v) { arr_.push_back(std::move(v)); }
    size_t size() const {
        return is_array() ? arr_.size() : (is_object() ? obj_.size() : 0);
    }

    bool operator==(const Value& rhs) const;

private:
    Type type_;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    Array arr_;
    Object obj_;
};

struct ParseResult {
    std::optional<Value> value;   // empty on failure
    std::string error;            // human-readable, with line:col
};

ParseResult parse(std::string_view text);

// pretty=true indents with 2 spaces; false emits the most compact form.
std::string write(const Value& v, bool pretty = true);

}  // namespace looks::json
