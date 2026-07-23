#include "util/json.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace looks::json {

const Value* Value::find(std::string_view key) const {
    if (!is_object()) return nullptr;
    for (const Member& m : obj_)
        if (m.first == key) return &m.second;
    return nullptr;
}

const Value& Value::get(std::string_view key) const {
    static const Value null_value;
    const Value* v = find(key);
    return v ? *v : null_value;
}

Value& Value::set(std::string_view key, Value v) {
    if (!is_object()) { type_ = Type::Object; obj_.clear(); }
    for (Member& m : obj_) {
        if (m.first == key) { m.second = std::move(v); return m.second; }
    }
    obj_.emplace_back(std::string(key), std::move(v));
    return obj_.back().second;
}

bool Value::operator==(const Value& rhs) const {
    if (type_ != rhs.type_) return false;
    switch (type_) {
        case Type::Null: return true;
        case Type::Bool: return bool_ == rhs.bool_;
        case Type::Number: return num_ == rhs.num_;
        case Type::String: return str_ == rhs.str_;
        case Type::Array: return arr_ == rhs.arr_;
        case Type::Object: return obj_ == rhs.obj_;
    }
    return false;
}

// ---------------------------------------------------------------- parser

namespace {

constexpr int kMaxDepth = 128;

struct Parser {
    const char* p;
    const char* end;
    const char* begin;
    std::string error;

    bool fail(const char* msg) {
        if (error.empty()) {
            int line = 1, col = 1;
            for (const char* c = begin; c < p; ++c) {
                if (*c == '\n') { ++line; col = 1; } else { ++col; }
            }
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s at line %d col %d", msg, line, col);
            error = buf;
        }
        return false;
    }

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    bool literal(const char* lit) {
        size_t n = std::strlen(lit);
        if (static_cast<size_t>(end - p) < n || std::memcmp(p, lit, n) != 0)
            return fail("invalid literal");
        p += n;
        return true;
    }

    bool parse_hex4(uint32_t& out) {
        if (end - p < 4) return fail("truncated \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("bad hex digit in \\u escape");
        }
        out = v;
        return true;
    }

    static void append_utf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parse_string(std::string& out) {
        ++p;  // opening quote
        while (p < end) {
            char c = *p;
            if (c == '"') { ++p; return true; }
            if (static_cast<unsigned char>(c) < 0x20) return fail("control char in string");
            if (c != '\\') { out += c; ++p; continue; }
            ++p;
            if (p >= end) return fail("truncated escape");
            char e = *p++;
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!parse_hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (end - p < 6 || p[0] != '\\' || p[1] != 'u')
                            return fail("unpaired surrogate");
                        p += 2;
                        uint32_t lo;
                        if (!parse_hex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return fail("bad low surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("unpaired surrogate");
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool parse_number(Value& out) {
        const char* start = p;
        if (p < end && *p == '-') ++p;
        while (p < end && (*p >= '0' && *p <= '9')) ++p;
        if (p < end && *p == '.') {
            ++p;
            while (p < end && (*p >= '0' && *p <= '9')) ++p;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p < end && (*p == '+' || *p == '-')) ++p;
            while (p < end && (*p >= '0' && *p <= '9')) ++p;
        }
        // Copy to a NUL-terminated buffer for strtod.
        char buf[64];
        size_t n = static_cast<size_t>(p - start);
        if (n == 0 || n >= sizeof(buf)) return fail("bad number");
        std::memcpy(buf, start, n);
        buf[n] = 0;
        char* parse_end = nullptr;
        double v = std::strtod(buf, &parse_end);
        if (parse_end != buf + n || !std::isfinite(v)) return fail("bad number");
        out = Value(v);
        return true;
    }

    bool parse_value(Value& out, int depth) {
        if (depth > kMaxDepth) return fail("nesting too deep");
        skip_ws();
        if (p >= end) return fail("unexpected end of input");
        switch (*p) {
            case '{': {
                ++p;
                Object obj;
                skip_ws();
                if (p < end && *p == '}') { ++p; out = Value(std::move(obj)); return true; }
                while (true) {
                    skip_ws();
                    if (p >= end || *p != '"') return fail("expected object key");
                    std::string key;
                    if (!parse_string(key)) return false;
                    skip_ws();
                    if (p >= end || *p != ':') return fail("expected ':'");
                    ++p;
                    Value v;
                    if (!parse_value(v, depth + 1)) return false;
                    obj.emplace_back(std::move(key), std::move(v));
                    skip_ws();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == '}') { ++p; break; }
                    return fail("expected ',' or '}'");
                }
                out = Value(std::move(obj));
                return true;
            }
            case '[': {
                ++p;
                Array arr;
                skip_ws();
                if (p < end && *p == ']') { ++p; out = Value(std::move(arr)); return true; }
                while (true) {
                    Value v;
                    if (!parse_value(v, depth + 1)) return false;
                    arr.push_back(std::move(v));
                    skip_ws();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == ']') { ++p; break; }
                    return fail("expected ',' or ']'");
                }
                out = Value(std::move(arr));
                return true;
            }
            case '"': {
                std::string s;
                if (!parse_string(s)) return false;
                out = Value(std::move(s));
                return true;
            }
            case 't': if (!literal("true")) return false; out = Value(true); return true;
            case 'f': if (!literal("false")) return false; out = Value(false); return true;
            case 'n': if (!literal("null")) return false; out = Value(nullptr); return true;
            default: return parse_number(out);
        }
    }
};

}  // namespace

ParseResult parse(std::string_view text) {
    Parser parser{text.data(), text.data() + text.size(), text.data(), {}};
    Value v;
    if (!parser.parse_value(v, 0)) return {std::nullopt, std::move(parser.error)};
    parser.skip_ws();
    if (parser.p != parser.end) {
        parser.fail("trailing garbage after document");
        return {std::nullopt, std::move(parser.error)};
    }
    return {std::move(v), {}};
}

// ---------------------------------------------------------------- writer

namespace {

void write_string(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);  // UTF-8 passes through
                }
        }
    }
    out += '"';
}

void write_number(std::string& out, double v) {
    // Integers print without a fractional part; everything else uses %.17g
    // (shortest round-trip-safe fixed precision without a Grisu).
    double integral;
    if (std::modf(v, &integral) == 0.0 && std::abs(v) < 9.0e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        out += buf;
    } else {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", v);
        out += buf;
    }
}

void write_value(std::string& out, const Value& v, bool pretty, int indent) {
    auto newline = [&](int level) {
        if (!pretty) return;
        out += '\n';
        out.append(static_cast<size_t>(level) * 2, ' ');
    };
    switch (v.type()) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += v.as_bool() ? "true" : "false"; break;
        case Type::Number: write_number(out, v.as_number()); break;
        case Type::String: write_string(out, v.as_string()); break;
        case Type::Array: {
            const Array& a = v.array();
            if (a.empty()) { out += "[]"; break; }
            out += '[';
            for (size_t i = 0; i < a.size(); ++i) {
                if (i) out += ',';
                newline(indent + 1);
                write_value(out, a[i], pretty, indent + 1);
            }
            newline(indent);
            out += ']';
            break;
        }
        case Type::Object: {
            const Object& o = v.object();
            if (o.empty()) { out += "{}"; break; }
            out += '{';
            for (size_t i = 0; i < o.size(); ++i) {
                if (i) out += ',';
                newline(indent + 1);
                write_string(out, o[i].first);
                out += pretty ? ": " : ":";
                write_value(out, o[i].second, pretty, indent + 1);
            }
            newline(indent);
            out += '}';
            break;
        }
    }
}

}  // namespace

std::string write(const Value& v, bool pretty) {
    std::string out;
    write_value(out, v, pretty, 0);
    if (pretty) out += '\n';
    return out;
}

}  // namespace looks::json
