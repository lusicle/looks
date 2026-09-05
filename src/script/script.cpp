#include "script/script.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace looks::script {

std::string to_display(const Value& v) {
    switch (v.kind) {
        case Value::Kind::Nil: return "nil";
        case Value::Kind::Bool: return v.b ? "true" : "false";
        case Value::Kind::Num: {
            char buf[40];
            const double d = v.num;
            if (std::isfinite(d) && d == std::floor(d) &&
                std::fabs(d) <= 9007199254740992.0) {
                std::snprintf(buf, sizeof(buf), "%lld",
                              static_cast<long long>(d));
            } else {
                std::snprintf(buf, sizeof(buf), "%.10g", d);
            }
            return buf;
        }
        case Value::Kind::Str: return v.str ? *v.str : "";
        case Value::Kind::List: {
            std::string out = "[";
            if (v.list)
                for (size_t i = 0; i < v.list->size(); ++i) {
                    if (i) out += ", ";
                    const Value& e = (*v.list)[i];
                    if (e.kind == Value::Kind::Str)
                        out += "\"" + *e.str + "\"";
                    else
                        out += to_display(e);
                }
            out += "]";
            return out;
        }
        case Value::Kind::Map: {
            std::string out = "{";
            bool first = true;
            if (v.map)
                for (const auto& [k, e] : *v.map) {
                    if (!first) out += ", ";
                    first = false;
                    out += k + ": ";
                    if (e.kind == Value::Kind::Str)
                        out += "\"" + *e.str + "\"";
                    else
                        out += to_display(e);
                }
            out += "}";
            return out;
        }
        case Value::Kind::Fn: return "<fn>";
        case Value::Kind::Native: return "<native>";
    }
    return "";
}

void Env::add(std::string name, std::string sig, int min_args, int max_args,
              NativeFn fn) {
    by_name_[name] = static_cast<int>(defs_.size());
    defs_.push_back({std::move(name), std::move(sig), min_args, max_args,
                     std::move(fn)});
}

const NativeDef* Env::find(const std::string& name) const {
    const auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : &defs_[static_cast<size_t>(it->second)];
}

int Env::index_of(const std::string& name) const {
    const auto it = by_name_.find(name);
    return it == by_name_.end() ? -1 : it->second;
}

namespace {

enum class Tok : uint8_t {
    End, Error,
    Ident, Number, String,
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Dot, Colon, Semicolon,
    Plus, Minus, Star, Slash, Percent,
    Assign, Eq, Ne, Lt, Le, Gt, Ge, Not, AndAnd, OrOr,
    KwLet, KwIf, KwElse, KwWhile, KwFor, KwIn, KwFn, KwReturn, KwBreak,
    KwContinue, KwTrue, KwFalse, KwNil,
};

struct Token {
    Tok kind = Tok::End;
    std::string text;
    double num = 0.0;
    int line = 1;
    bool newline_before = false;
};

class Lexer {
public:
    explicit Lexer(const std::string& src) : src_(src) {}

    Token next() {
        bool nl = false;
        for (;;) {
            while (pos_ < src_.size()) {
                const char c = src_[pos_];
                if (c == '\n') {
                    nl = true;
                    ++line_;
                    ++pos_;
                } else if (c == ' ' || c == '\t' || c == '\r') {
                    ++pos_;
                } else {
                    break;
                }
            }
            if (pos_ < src_.size() && src_[pos_] == '#') {
                while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
                continue;
            }
            break;
        }
        Token t;
        t.line = line_;
        t.newline_before = nl;
        if (pos_ >= src_.size()) {
            t.kind = Tok::End;
            return t;
        }
        const char c = src_[pos_++];
        auto two = [&](char want, Tok yes, Tok no) {
            if (pos_ < src_.size() && src_[pos_] == want) {
                ++pos_;
                t.kind = yes;
            } else {
                t.kind = no;
            }
        };
        switch (c) {
            case '(': t.kind = Tok::LParen; return t;
            case ')': t.kind = Tok::RParen; return t;
            case '{': t.kind = Tok::LBrace; return t;
            case '}': t.kind = Tok::RBrace; return t;
            case '[': t.kind = Tok::LBracket; return t;
            case ']': t.kind = Tok::RBracket; return t;
            case ',': t.kind = Tok::Comma; return t;
            case '.': t.kind = Tok::Dot; return t;
            case ':': t.kind = Tok::Colon; return t;
            case ';': t.kind = Tok::Semicolon; return t;
            case '+': t.kind = Tok::Plus; return t;
            case '-': t.kind = Tok::Minus; return t;
            case '*': t.kind = Tok::Star; return t;
            case '/': t.kind = Tok::Slash; return t;
            case '%': t.kind = Tok::Percent; return t;
            case '=': two('=', Tok::Eq, Tok::Assign); return t;
            case '!': two('=', Tok::Ne, Tok::Not); return t;
            case '<': two('=', Tok::Le, Tok::Lt); return t;
            case '>': two('=', Tok::Ge, Tok::Gt); return t;
            case '&':
                if (pos_ < src_.size() && src_[pos_] == '&') {
                    ++pos_;
                    t.kind = Tok::AndAnd;
                    return t;
                }
                t.kind = Tok::Error;
                t.text = "stray '&'";
                return t;
            case '|':
                if (pos_ < src_.size() && src_[pos_] == '|') {
                    ++pos_;
                    t.kind = Tok::OrOr;
                    return t;
                }
                t.kind = Tok::Error;
                t.text = "stray '|'";
                return t;
            case '"': {
                std::string s;
                while (pos_ < src_.size() && src_[pos_] != '"') {
                    char d = src_[pos_++];
                    if (d == '\\' && pos_ < src_.size()) {
                        const char e = src_[pos_++];
                        if (e == 'n') d = '\n';
                        else if (e == 't') d = '\t';
                        else if (e == '"') d = '"';
                        else if (e == '\\') d = '\\';
                        else {
                            t.kind = Tok::Error;
                            t.text = "bad escape";
                            return t;
                        }
                    } else if (d == '\n') {
                        t.kind = Tok::Error;
                        t.text = "unterminated string";
                        return t;
                    }
                    s += d;
                }
                if (pos_ >= src_.size()) {
                    t.kind = Tok::Error;
                    t.text = "unterminated string";
                    return t;
                }
                ++pos_;
                t.kind = Tok::String;
                t.text = std::move(s);
                return t;
            }
            default: break;
        }
        if (c >= '0' && c <= '9') {
            size_t start = pos_ - 1;
            while (pos_ < src_.size() &&
                   ((src_[pos_] >= '0' && src_[pos_] <= '9') ||
                    src_[pos_] == '.' || src_[pos_] == 'e' ||
                    src_[pos_] == 'E' ||
                    ((src_[pos_] == '+' || src_[pos_] == '-') &&
                     (src_[pos_ - 1] == 'e' || src_[pos_ - 1] == 'E'))))
                ++pos_;
            t.kind = Tok::Number;
            t.num = std::strtod(src_.c_str() + start, nullptr);
            return t;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            size_t start = pos_ - 1;
            while (pos_ < src_.size() &&
                   ((src_[pos_] >= 'a' && src_[pos_] <= 'z') ||
                    (src_[pos_] >= 'A' && src_[pos_] <= 'Z') ||
                    (src_[pos_] >= '0' && src_[pos_] <= '9') ||
                    src_[pos_] == '_'))
                ++pos_;
            t.text = src_.substr(start, pos_ - start);
            if (t.text == "let") t.kind = Tok::KwLet;
            else if (t.text == "if") t.kind = Tok::KwIf;
            else if (t.text == "else") t.kind = Tok::KwElse;
            else if (t.text == "while") t.kind = Tok::KwWhile;
            else if (t.text == "for") t.kind = Tok::KwFor;
            else if (t.text == "in") t.kind = Tok::KwIn;
            else if (t.text == "fn") t.kind = Tok::KwFn;
            else if (t.text == "return") t.kind = Tok::KwReturn;
            else if (t.text == "break") t.kind = Tok::KwBreak;
            else if (t.text == "continue") t.kind = Tok::KwContinue;
            else if (t.text == "true") t.kind = Tok::KwTrue;
            else if (t.text == "false") t.kind = Tok::KwFalse;
            else if (t.text == "nil") t.kind = Tok::KwNil;
            else t.kind = Tok::Ident;
            return t;
        }
        t.kind = Tok::Error;
        t.text = std::string("unexpected character '") + c + "'";
        return t;
    }

private:
    const std::string& src_;
    size_t pos_ = 0;
    int line_ = 1;
};

enum class Op : uint8_t {
    Const,        // u16 constant index
    Nil, True, False, Pop,
    GetLocal,     // u16 slot
    SetLocal,     // u16 slot
    GetGlobal,    // u16 name constant
    SetGlobal,    // u16 name constant (must exist)
    DefGlobal,    // u16 name constant
    Eq, Ne, Lt, Le, Gt, Ge,
    Add, Sub, Mul, Div, Mod, Negate, NotOp,
    Jump,         // u16 forward offset
    Loop,         // u16 backward offset
    JumpFalse,    // u16, pops condition
    JumpFalsePeek,// u16, leaves value (and)
    JumpTruePeek, // u16, leaves value (or)
    Call,         // u8 arg count
    Index,        // a[i]
    SetIndex,     // a i v -> a[i] = v
    MakeList,     // u16 element count
    MakeMap,      // u16 pair count (key value key value...)
    Return, Halt,
    HaltValue,    // top-level return: pops the script's result
};

struct Chunk {
    std::vector<uint8_t> code;
    std::vector<int> lines;   // parallel to code
    int arity = 0;
    std::string name;
};

struct Program {
    std::vector<Chunk> chunks;   // 0 = top level
    std::vector<Value> constants;
};

class Compiler {
public:
    Compiler(const std::string& src, std::string chunk_name, const Env& env)
        : lex_(src), env_(env) {
        prog_.chunks.emplace_back();
        prog_.chunks[0].name = std::move(chunk_name);
        advance();
    }

    bool compile(CompileError* err) {
        while (!check(Tok::End) && !failed_) statement();
        emit(Op::Halt);
        if (failed_ && err) {
            err->message = error_;
            err->line = error_line_;
        }
        return !failed_;
    }

    Program take() { return std::move(prog_); }

private:
    void advance() {
        prev_ = cur_tok_;
        cur_tok_ = lex_.next();
        if (cur_tok_.kind == Tok::Error) fail(cur_tok_.text);
    }
    bool check(Tok k) const { return cur_tok_.kind == k; }
    bool match(Tok k) {
        if (!check(k)) return false;
        advance();
        return true;
    }
    void expect(Tok k, const char* what) {
        if (!check(k)) {
            fail(std::string("expected ") + what);
            return;
        }
        advance();
    }
    void fail(const std::string& msg) {
        if (failed_) return;
        failed_ = true;
        error_ = msg;
        error_line_ = cur_tok_.line;
    }

    void emit(Op op) {
        cur_chunk().code.push_back(static_cast<uint8_t>(op));
        cur_chunk().lines.push_back(prev_.line);
    }
    void emit_u8(uint8_t v) {
        cur_chunk().code.push_back(v);
        cur_chunk().lines.push_back(prev_.line);
    }
    void emit_u16(uint16_t v) {
        emit_u8(static_cast<uint8_t>(v & 0xFF));
        emit_u8(static_cast<uint8_t>(v >> 8));
    }
    void emit_op16(Op op, uint16_t v) {
        emit(op);
        emit_u16(v);
    }
    size_t emit_jump(Op op) {
        emit(op);
        emit_u16(0xFFFF);
        return cur_chunk().code.size() - 2;
    }
    void patch_jump(size_t at) {
        const size_t dist = cur_chunk().code.size() - (at + 2);
        if (dist > 0xFFFF) {
            fail("jump too long");
            return;
        }
        cur_chunk().code[at] = static_cast<uint8_t>(dist & 0xFF);
        cur_chunk().code[at + 1] = static_cast<uint8_t>(dist >> 8);
    }
    void emit_loop(size_t target) {
        emit(Op::Loop);
        const size_t dist = cur_chunk().code.size() + 2 - target;
        if (dist > 0xFFFF) {
            fail("loop too long");
            return;
        }
        emit_u16(static_cast<uint16_t>(dist));
    }
    uint16_t constant(Value v) {
        for (size_t i = 0; i < prog_.constants.size(); ++i) {
            const Value& c = prog_.constants[i];
            if (c.kind != v.kind) continue;
            if (c.kind == Value::Kind::Num && c.num == v.num)
                return static_cast<uint16_t>(i);
            if (c.kind == Value::Kind::Str && *c.str == *v.str)
                return static_cast<uint16_t>(i);
        }
        if (prog_.constants.size() >= 0xFFFF) {
            fail("too many constants");
            return 0;
        }
        prog_.constants.push_back(std::move(v));
        return static_cast<uint16_t>(prog_.constants.size() - 1);
    }
    uint16_t name_constant(const std::string& n) {
        return constant(Value::string(n));
    }

    struct Local {
        std::string name;
        int depth;
    };
    struct LoopCtx {
        size_t continue_target = 0;
        bool continue_patches = false;
        std::vector<size_t> breaks;
        std::vector<size_t> continues;
        size_t local_floor = 0;
    };

    void begin_scope() { ++depth_; }
    void end_scope() {
        --depth_;
        while (!locals_.empty() && locals_.back().depth > depth_) {
            emit(Op::Pop);
            locals_.pop_back();
        }
    }
    int resolve_local(const std::string& name) const {
        for (int i = static_cast<int>(locals_.size()) - 1; i >= 0; --i)
            if (locals_[static_cast<size_t>(i)].name == name) return i;
        return -1;
    }
    void declare_local(const std::string& name) {
        for (int i = static_cast<int>(locals_.size()) - 1; i >= 0; --i) {
            const Local& l = locals_[static_cast<size_t>(i)];
            if (l.depth < depth_) break;
            if (l.name == name) {
                fail("'" + name + "' already declared in this scope");
                return;
            }
        }
        if (locals_.size() >= 0xFFF0) {
            fail("too many locals");
            return;
        }
        locals_.push_back({name, depth_});
    }

    void statement() {
        if (match(Tok::Semicolon)) return;
        if (match(Tok::KwLet)) return let_statement();
        if (match(Tok::KwIf)) return if_statement();
        if (match(Tok::KwWhile)) return while_statement();
        if (match(Tok::KwFor)) return for_statement();
        if (match(Tok::KwFn)) return fn_statement();
        if (match(Tok::KwReturn)) return return_statement();
        if (match(Tok::KwBreak)) return break_statement();
        if (match(Tok::KwContinue)) return continue_statement();
        if (check(Tok::LBrace)) {
            fail("blocks stand alone only after if/while/for/fn");
            return;
        }
        expr_statement();
    }

    void block() {
        expect(Tok::LBrace, "'{'");
        begin_scope();
        while (!check(Tok::RBrace) && !check(Tok::End) && !failed_)
            statement();
        expect(Tok::RBrace, "'}'");
        end_scope();
    }

    void let_statement() {
        expect(Tok::Ident, "a name after let");
        const std::string name = prev_.text;
        expect(Tok::Assign, "'=' after the name");
        expression();
        if (fn_depth_ == 0 && depth_ == 0) {
            emit_op16(Op::DefGlobal, name_constant(name));
        } else {
            declare_local(name);
        }
    }

    void if_statement() {
        expression();
        const size_t skip = emit_jump(Op::JumpFalse);
        block();
        if (match(Tok::KwElse)) {
            const size_t over = emit_jump(Op::Jump);
            patch_jump(skip);
            if (match(Tok::KwIf)) if_statement();
            else block();
            patch_jump(over);
        } else {
            patch_jump(skip);
        }
    }

    void while_statement() {
        const size_t top = cur_chunk().code.size();
        expression();
        const size_t exit = emit_jump(Op::JumpFalse);
        LoopCtx loop;
        loop.continue_target = top;
        loop.local_floor = locals_.size();
        loops_.push_back(loop);
        block();
        emit_loop(top);
        patch_jump(exit);
        for (const size_t b : loops_.back().breaks) patch_jump(b);
        loops_.pop_back();
    }

    void for_statement() {
        expect(Tok::Ident, "a loop variable");
        const std::string var = prev_.text;
        expect(Tok::KwIn, "'in'");
        const int len_native = env_.index_of("len");
        if (len_native < 0) {
            fail("for-in needs the core library (len)");
            return;
        }
        begin_scope();
        expression();
        declare_local("(it)");
        emit(Op::Nil);
        declare_local(var);
        emit_op16(Op::Const, constant(Value::number(0.0)));
        declare_local("(i)");
        const int it_slot = resolve_local("(it)");
        const int var_slot = resolve_local(var);
        const int i_slot = resolve_local("(i)");

        const size_t top = cur_chunk().code.size();
        // Call the native directly. A shadowed len global must not break it.
        emit_op16(Op::GetLocal, static_cast<uint16_t>(i_slot));
        {
            Value v;
            v.kind = Value::Kind::Native;
            v.native = static_cast<uint32_t>(len_native);
            emit_op16(Op::Const, constant(v));
        }
        emit_op16(Op::GetLocal, static_cast<uint16_t>(it_slot));
        emit(Op::Call);
        emit_u8(1);
        emit(Op::Lt);
        const size_t exit = emit_jump(Op::JumpFalse);
        emit_op16(Op::GetLocal, static_cast<uint16_t>(it_slot));
        emit_op16(Op::GetLocal, static_cast<uint16_t>(i_slot));
        emit(Op::Index);
        emit_op16(Op::SetLocal, static_cast<uint16_t>(var_slot));
        emit(Op::Pop);

        LoopCtx loop;
        loop.continue_patches = true;
        loop.local_floor = locals_.size();
        loops_.push_back(loop);
        block();
        for (const size_t c : loops_.back().continues) patch_jump(c);
        emit_op16(Op::GetLocal, static_cast<uint16_t>(i_slot));
        emit_op16(Op::Const, constant(Value::number(1.0)));
        emit(Op::Add);
        emit_op16(Op::SetLocal, static_cast<uint16_t>(i_slot));
        emit(Op::Pop);
        emit_loop(top);
        patch_jump(exit);
        for (const size_t b : loops_.back().breaks) patch_jump(b);
        loops_.pop_back();
        end_scope();
    }

    void fn_statement() {
        if (fn_depth_ > 0) {
            fail("functions are top-level only");
            return;
        }
        expect(Tok::Ident, "a function name");
        const std::string name = prev_.text;
        expect(Tok::LParen, "'('");
        prog_.chunks.emplace_back();
        const size_t saved_idx = cur_idx_;
        cur_idx_ = prog_.chunks.size() - 1;
        cur_chunk().name = name;
        const uint32_t chunk_index = static_cast<uint32_t>(cur_idx_);
        std::vector<Local> saved_locals = std::move(locals_);
        locals_.clear();
        const int saved_depth = depth_;
        depth_ = 0;
        ++fn_depth_;
        begin_scope();
        if (!check(Tok::RParen)) {
            do {
                expect(Tok::Ident, "a parameter name");
                declare_local(prev_.text);
                ++cur_chunk().arity;
            } while (match(Tok::Comma));
        }
        expect(Tok::RParen, "')'");
        block();
        emit(Op::Nil);
        emit(Op::Return);
        --fn_depth_;
        depth_ = saved_depth;
        locals_ = std::move(saved_locals);
        cur_idx_ = saved_idx;
        Value fv;
        fv.kind = Value::Kind::Fn;
        fv.fn = chunk_index;
        emit_op16(Op::Const, constant(fv));
        emit_op16(Op::DefGlobal, name_constant(name));
    }

    void return_statement() {
        if (check(Tok::RBrace) || check(Tok::End) || cur_tok_.newline_before)
            emit(Op::Nil);
        else
            expression();
        if (fn_depth_ == 0) {
            emit(Op::HaltValue);
        } else {
            emit(Op::Return);
        }
    }

    void break_statement() {
        if (loops_.empty()) {
            fail("break outside a loop");
            return;
        }
        pop_to_floor(loops_.back().local_floor);
        loops_.back().breaks.push_back(emit_jump(Op::Jump));
    }

    void continue_statement() {
        if (loops_.empty()) {
            fail("continue outside a loop");
            return;
        }
        LoopCtx& loop = loops_.back();
        pop_to_floor(loop.local_floor);
        if (loop.continue_patches)
            loop.continues.push_back(emit_jump(Op::Jump));
        else
            emit_loop(loop.continue_target);
    }

    // A jump out of a loop skips end_scope, so pop the locals here.
    void pop_to_floor(size_t floor) {
        for (size_t i = locals_.size(); i > floor; --i) emit(Op::Pop);
    }

    void expr_statement() {
        expression();
        if (match(Tok::Assign)) {
            rewrite_store();
            expression();
            finish_store();
        } else {
            emit(Op::Pop);
        }
    }

    // rewrite_store() needs the load to be the last emitted bytes.
    enum class LastLoad : uint8_t { None, Local, Global, Index };
    LastLoad last_load_ = LastLoad::None;
    uint16_t last_slot_ = 0;

    void rewrite_store() {
        pending_store_ = last_load_;
        pending_slot_ = last_slot_;
        switch (last_load_) {
            case LastLoad::Local:
            case LastLoad::Global:
                cur_chunk().code.resize(cur_chunk().code.size() - 3);
                cur_chunk().lines.resize(cur_chunk().code.size());
                break;
            case LastLoad::Index:
                cur_chunk().code.resize(cur_chunk().code.size() - 1);
                cur_chunk().lines.resize(cur_chunk().code.size());
                break;
            case LastLoad::None:
                fail("cannot assign to this expression");
                break;
        }
    }
    void finish_store() {
        switch (pending_store_) {
            case LastLoad::Local:
                emit_op16(Op::SetLocal, pending_slot_);
                emit(Op::Pop);
                break;
            case LastLoad::Global:
                emit_op16(Op::SetGlobal, pending_slot_);
                break;
            case LastLoad::Index:
                emit(Op::SetIndex);
                break;
            case LastLoad::None: break;
        }
    }
    LastLoad pending_store_ = LastLoad::None;
    uint16_t pending_slot_ = 0;

    void expression() { parse_or(); }

    // A newline ends the expression unless it is in parens or brackets.
    bool op_continues() const {
        return group_depth_ > 0 || !cur_tok_.newline_before;
    }

    void parse_or() {
        parse_and();
        while (check(Tok::OrOr) && op_continues()) {
            advance();
            const size_t skip = emit_jump(Op::JumpTruePeek);
            emit(Op::Pop);
            parse_and();
            patch_jump(skip);
            last_load_ = LastLoad::None;
        }
    }
    void parse_and() {
        parse_equality();
        while (check(Tok::AndAnd) && op_continues()) {
            advance();
            const size_t skip = emit_jump(Op::JumpFalsePeek);
            emit(Op::Pop);
            parse_equality();
            patch_jump(skip);
            last_load_ = LastLoad::None;
        }
    }
    struct BinOp {
        Tok tok;
        Op op;
    };
    template <size_t N>
    void parse_binary(void (Compiler::*next)(), const BinOp (&ops)[N]) {
        (this->*next)();
        for (;;) {
            const BinOp* hit = nullptr;
            for (const BinOp& o : ops)
                if (check(o.tok)) {
                    hit = &o;
                    break;
                }
            if (!hit || !op_continues()) return;
            advance();
            (this->*next)();
            emit(hit->op);
            last_load_ = LastLoad::None;
        }
    }
    void parse_equality() {
        static const BinOp k[] = {{Tok::Eq, Op::Eq}, {Tok::Ne, Op::Ne}};
        parse_binary(&Compiler::parse_compare, k);
    }
    void parse_compare() {
        static const BinOp k[] = {{Tok::Lt, Op::Lt},
                                  {Tok::Le, Op::Le},
                                  {Tok::Gt, Op::Gt},
                                  {Tok::Ge, Op::Ge}};
        parse_binary(&Compiler::parse_term, k);
    }
    void parse_term() {
        static const BinOp k[] = {{Tok::Plus, Op::Add}, {Tok::Minus, Op::Sub}};
        parse_binary(&Compiler::parse_factor, k);
    }
    void parse_factor() {
        static const BinOp k[] = {{Tok::Star, Op::Mul},
                                  {Tok::Slash, Op::Div},
                                  {Tok::Percent, Op::Mod}};
        parse_binary(&Compiler::parse_unary, k);
    }
    void parse_unary() {
        if (match(Tok::Minus)) {
            parse_unary();
            emit(Op::Negate);
            last_load_ = LastLoad::None;
            return;
        }
        if (match(Tok::Not)) {
            parse_unary();
            emit(Op::NotOp);
            last_load_ = LastLoad::None;
            return;
        }
        parse_postfix();
    }
    void parse_postfix() {
        parse_primary();
        for (;;) {
            if (check(Tok::LParen) && op_continues()) {
                advance();
                ++group_depth_;
                uint8_t argc = 0;
                if (!check(Tok::RParen)) {
                    do {
                        expression();
                        if (argc == 255) {
                            fail("too many arguments");
                            return;
                        }
                        ++argc;
                    } while (match(Tok::Comma));
                }
                --group_depth_;
                expect(Tok::RParen, "')'");
                emit(Op::Call);
                emit_u8(argc);
                last_load_ = LastLoad::None;
            } else if (check(Tok::LBracket) && op_continues()) {
                advance();
                ++group_depth_;
                expression();
                --group_depth_;
                expect(Tok::RBracket, "']'");
                emit(Op::Index);
                last_load_ = LastLoad::Index;
            } else if (check(Tok::Dot) && op_continues()) {
                advance();
                expect(Tok::Ident, "a member name after '.'");
                emit_op16(Op::Const, name_constant(prev_.text));
                emit(Op::Index);
                last_load_ = LastLoad::Index;
            } else {
                return;
            }
        }
    }
    void parse_primary() {
        last_load_ = LastLoad::None;
        if (match(Tok::Number)) {
            emit_op16(Op::Const, constant(Value::number(prev_.num)));
            return;
        }
        if (match(Tok::String)) {
            emit_op16(Op::Const, constant(Value::string(prev_.text)));
            return;
        }
        if (match(Tok::KwTrue)) {
            emit(Op::True);
            return;
        }
        if (match(Tok::KwFalse)) {
            emit(Op::False);
            return;
        }
        if (match(Tok::KwNil)) {
            emit(Op::Nil);
            return;
        }
        if (match(Tok::LParen)) {
            ++group_depth_;
            expression();
            --group_depth_;
            expect(Tok::RParen, "')'");
            last_load_ = LastLoad::None;
            return;
        }
        if (match(Tok::LBracket)) {
            ++group_depth_;
            uint16_t count = 0;
            if (!check(Tok::RBracket)) {
                do {
                    if (check(Tok::RBracket)) break;
                    expression();
                    ++count;
                } while (match(Tok::Comma));
            }
            --group_depth_;
            expect(Tok::RBracket, "']'");
            emit_op16(Op::MakeList, count);
            return;
        }
        if (match(Tok::LBrace)) {
            ++group_depth_;
            uint16_t count = 0;
            if (!check(Tok::RBrace)) {
                do {
                    if (check(Tok::RBrace)) break;
                    if (match(Tok::Ident) || match(Tok::String)) {
                        emit_op16(Op::Const, name_constant(prev_.text));
                    } else {
                        fail("expected a key");
                        return;
                    }
                    expect(Tok::Colon, "':'");
                    expression();
                    ++count;
                } while (match(Tok::Comma));
            }
            --group_depth_;
            expect(Tok::RBrace, "'}'");
            emit_op16(Op::MakeMap, count);
            return;
        }
        if (match(Tok::Ident)) {
            const std::string name = prev_.text;
            const int slot = resolve_local(name);
            if (slot >= 0) {
                emit_op16(Op::GetLocal, static_cast<uint16_t>(slot));
                last_load_ = LastLoad::Local;
                last_slot_ = static_cast<uint16_t>(slot);
            } else {
                const uint16_t nc = name_constant(name);
                emit_op16(Op::GetGlobal, nc);
                last_load_ = LastLoad::Global;
                last_slot_ = nc;
            }
            return;
        }
        fail("expected an expression");
    }

    Lexer lex_;
    const Env& env_;
    Program prog_;
    size_t cur_idx_ = 0;
    Chunk& cur_chunk() { return prog_.chunks[cur_idx_]; }
    Token cur_tok_, prev_;
    std::vector<Local> locals_;
    std::vector<LoopCtx> loops_;
    int depth_ = 0;
    int fn_depth_ = 0;
    int group_depth_ = 0;
    bool failed_ = false;
    std::string error_;
    int error_line_ = 0;
};

}  // namespace

struct Vm::Impl {
    Program prog;
    const Env* env = nullptr;
    std::map<std::string, Value> globals;

    struct Frame {
        uint32_t chunk = 0;
        size_t ip = 0;
        size_t base = 0;   // stack index of first local
    };
    std::vector<Value> stack;
    std::vector<Frame> frames;
    bool started = false;
    bool waiting = false;
    bool done = false;
};

Vm::Vm() : impl_(std::make_unique<Impl>()) {}
Vm::~Vm() = default;

std::unique_ptr<Vm> Vm::compile(const std::string& source,
                                std::string chunk_name, const Env& env,
                                CompileError* err) {
    Compiler comp(source, std::move(chunk_name), env);
    if (!comp.compile(err)) return nullptr;
    std::unique_ptr<Vm> vm(new Vm());
    vm->impl_->prog = comp.take();
    vm->impl_->env = &env;
    const auto& defs = env.defs();
    for (size_t i = 0; i < defs.size(); ++i) {
        Value v;
        v.kind = Value::Kind::Native;
        v.native = static_cast<uint32_t>(i);
        vm->impl_->globals[defs[i].name] = v;
    }
    return vm;
}

int Vm::current_line() const {
    if (impl_->frames.empty()) return 0;
    const auto& f = impl_->frames.back();
    const Chunk& c = impl_->prog.chunks[f.chunk];
    const size_t ip = f.ip < c.lines.size() ? f.ip : c.lines.size() - 1;
    return c.lines.empty() ? 0 : c.lines[ip];
}

Vm::Status Vm::run(uint64_t budget) {
    if (impl_->done) return Status::Done;
    if (impl_->waiting) {
        set_error("run() while suspended");
        return Status::Error;
    }
    if (!impl_->started) {
        impl_->started = true;
        impl_->frames.push_back({0, 0, 0});
    }
    return execute(budget);
}

Vm::Status Vm::resume(Value v, uint64_t budget) {
    if (!impl_->waiting) {
        set_error("resume() without a suspend");
        return Status::Error;
    }
    impl_->waiting = false;
    impl_->stack.push_back(std::move(v));
    return execute(budget);
}

Vm::Status Vm::fail(const std::string& message) {
    impl_->waiting = false;
    set_error(message);
    impl_->done = true;
    return Status::Error;
}

Vm::Status Vm::execute(uint64_t budget) {
    Impl& im = *impl_;
    auto& stack = im.stack;

    auto runtime_error = [&](const std::string& msg) {
        set_error(msg);
        im.done = true;
    };

    while (budget-- > 0) {
        if (im.frames.empty()) {
            im.done = true;
            return Status::Done;
        }
        Impl::Frame& fr = im.frames.back();
        const Chunk& chunk = im.prog.chunks[fr.chunk];
        if (fr.ip >= chunk.code.size()) {
            runtime_error("ran off the end of the chunk");
            return Status::Error;
        }
        const Op op = static_cast<Op>(chunk.code[fr.ip++]);
        auto read_u16 = [&]() -> uint16_t {
            const uint16_t v = static_cast<uint16_t>(
                chunk.code[fr.ip] |
                (static_cast<uint16_t>(chunk.code[fr.ip + 1]) << 8));
            fr.ip += 2;
            return v;
        };
        auto read_u8 = [&]() -> uint8_t { return chunk.code[fr.ip++]; };

        switch (op) {
            case Op::Const:
                stack.push_back(im.prog.constants[read_u16()]);
                break;
            case Op::Nil: stack.push_back(Value::nil()); break;
            case Op::True: stack.push_back(Value::boolean(true)); break;
            case Op::False: stack.push_back(Value::boolean(false)); break;
            case Op::Pop: stack.pop_back(); break;
            case Op::GetLocal:
                stack.push_back(stack[fr.base + read_u16()]);
                break;
            case Op::SetLocal:
                stack[fr.base + read_u16()] = stack.back();
                break;
            case Op::GetGlobal: {
                const Value& name = im.prog.constants[read_u16()];
                const auto it = im.globals.find(*name.str);
                if (it == im.globals.end()) {
                    runtime_error("unknown name '" + *name.str + "'");
                    return Status::Error;
                }
                stack.push_back(it->second);
                break;
            }
            case Op::SetGlobal: {
                const Value& name = im.prog.constants[read_u16()];
                const auto it = im.globals.find(*name.str);
                if (it == im.globals.end()) {
                    runtime_error("assignment to undeclared '" + *name.str +
                                  "' (use let)");
                    return Status::Error;
                }
                it->second = stack.back();
                stack.pop_back();
                break;
            }
            case Op::DefGlobal: {
                const Value& name = im.prog.constants[read_u16()];
                im.globals[*name.str] = stack.back();
                stack.pop_back();
                break;
            }
            case Op::Eq:
            case Op::Ne: {
                Value b = std::move(stack.back());
                stack.pop_back();
                Value a = std::move(stack.back());
                stack.pop_back();
                bool eq = false;
                if (a.kind == b.kind) {
                    switch (a.kind) {
                        case Value::Kind::Nil: eq = true; break;
                        case Value::Kind::Bool: eq = a.b == b.b; break;
                        case Value::Kind::Num: eq = a.num == b.num; break;
                        case Value::Kind::Str: eq = *a.str == *b.str; break;
                        case Value::Kind::List: eq = a.list == b.list; break;
                        case Value::Kind::Map: eq = a.map == b.map; break;
                        case Value::Kind::Fn: eq = a.fn == b.fn; break;
                        case Value::Kind::Native:
                            eq = a.native == b.native;
                            break;
                    }
                }
                stack.push_back(Value::boolean(op == Op::Eq ? eq : !eq));
                break;
            }
            case Op::Lt:
            case Op::Le:
            case Op::Gt:
            case Op::Ge: {
                Value b = std::move(stack.back());
                stack.pop_back();
                Value a = std::move(stack.back());
                stack.pop_back();
                bool r = false;
                if (a.is_num() && b.is_num()) {
                    r = op == Op::Lt   ? a.num < b.num
                        : op == Op::Le ? a.num <= b.num
                        : op == Op::Gt ? a.num > b.num
                                       : a.num >= b.num;
                } else if (a.is_str() && b.is_str()) {
                    const int c = a.str->compare(*b.str);
                    r = op == Op::Lt   ? c < 0
                        : op == Op::Le ? c <= 0
                        : op == Op::Gt ? c > 0
                                       : c >= 0;
                } else {
                    runtime_error("comparison needs two numbers or two "
                                  "strings");
                    return Status::Error;
                }
                stack.push_back(Value::boolean(r));
                break;
            }
            case Op::Add: {
                Value b = std::move(stack.back());
                stack.pop_back();
                Value a = std::move(stack.back());
                stack.pop_back();
                if (a.is_num() && b.is_num()) {
                    stack.push_back(Value::number(a.num + b.num));
                } else if (a.is_str() || b.is_str()) {
                    stack.push_back(
                        Value::string(to_display(a) + to_display(b)));
                } else {
                    runtime_error("'+' needs numbers or strings");
                    return Status::Error;
                }
                break;
            }
            case Op::Sub:
            case Op::Mul:
            case Op::Div:
            case Op::Mod: {
                Value b = std::move(stack.back());
                stack.pop_back();
                Value a = std::move(stack.back());
                stack.pop_back();
                if (!a.is_num() || !b.is_num()) {
                    runtime_error("arithmetic needs numbers");
                    return Status::Error;
                }
                double r = 0.0;
                switch (op) {
                    case Op::Sub: r = a.num - b.num; break;
                    case Op::Mul: r = a.num * b.num; break;
                    case Op::Div: r = a.num / b.num; break;
                    default: r = std::fmod(a.num, b.num); break;
                }
                stack.push_back(Value::number(r));
                break;
            }
            case Op::Negate: {
                if (!stack.back().is_num()) {
                    runtime_error("'-' needs a number");
                    return Status::Error;
                }
                stack.back().num = -stack.back().num;
                break;
            }
            case Op::NotOp: {
                const bool t = stack.back().truthy();
                stack.pop_back();
                stack.push_back(Value::boolean(!t));
                break;
            }
            case Op::Jump: {
                const uint16_t d = read_u16();
                fr.ip += d;
                break;
            }
            case Op::Loop: {
                const uint16_t d = read_u16();
                fr.ip -= d;
                break;
            }
            case Op::JumpFalse: {
                const uint16_t d = read_u16();
                const bool t = stack.back().truthy();
                stack.pop_back();
                if (!t) fr.ip += d;
                break;
            }
            case Op::JumpFalsePeek: {
                const uint16_t d = read_u16();
                if (!stack.back().truthy()) fr.ip += d;
                break;
            }
            case Op::JumpTruePeek: {
                const uint16_t d = read_u16();
                if (stack.back().truthy()) fr.ip += d;
                break;
            }
            case Op::Call: {
                const uint8_t argc = read_u8();
                const size_t callee_at = stack.size() - argc - 1;
                Value callee = stack[callee_at];
                if (callee.kind == Value::Kind::Native) {
                    std::vector<Value> args(
                        stack.begin() + static_cast<ptrdiff_t>(callee_at) + 1,
                        stack.end());
                    stack.resize(callee_at);
                    const NativeDef& def =
                        im.env->defs()[callee.native];
                    if (static_cast<int>(args.size()) < def.min_args ||
                        (def.max_args >= 0 &&
                         static_cast<int>(args.size()) > def.max_args)) {
                        runtime_error(def.name + " expects " + def.sig);
                        return Status::Error;
                    }
                    suspend_ = false;
                    Value r = def.fn(*this, args);
                    if (has_error_) {
                        im.done = true;
                        return Status::Error;
                    }
                    if (suspend_) {
                        im.waiting = true;
                        return Status::Suspended;
                    }
                    stack.push_back(std::move(r));
                } else if (callee.kind == Value::Kind::Fn) {
                    const Chunk& target = im.prog.chunks[callee.fn];
                    if (argc != target.arity) {
                        runtime_error(target.name + " expects " +
                                      std::to_string(target.arity) +
                                      " argument(s)");
                        return Status::Error;
                    }
                    if (im.frames.size() >= 128) {
                        runtime_error("call stack overflow");
                        return Status::Error;
                    }
                    stack.erase(stack.begin() +
                                static_cast<ptrdiff_t>(callee_at));
                    im.frames.push_back({callee.fn, 0, callee_at});
                } else {
                    runtime_error("call of a non-function");
                    return Status::Error;
                }
                break;
            }
            case Op::Index: {
                Value idx = std::move(stack.back());
                stack.pop_back();
                Value obj = std::move(stack.back());
                stack.pop_back();
                if (obj.kind == Value::Kind::List) {
                    if (!idx.is_num()) {
                        runtime_error("list index must be a number");
                        return Status::Error;
                    }
                    const double di = idx.num;
                    if (di < 0.0 ||
                        di >= static_cast<double>(obj.list->size())) {
                        runtime_error("list index " + to_display(idx) +
                                      " out of range (len " +
                                      std::to_string(obj.list->size()) +
                                      ")");
                        return Status::Error;
                    }
                    stack.push_back((*obj.list)[static_cast<size_t>(di)]);
                } else if (obj.kind == Value::Kind::Map) {
                    if (!idx.is_str()) {
                        runtime_error("map key must be a string");
                        return Status::Error;
                    }
                    const auto it = obj.map->find(*idx.str);
                    stack.push_back(it == obj.map->end() ? Value::nil()
                                                         : it->second);
                } else {
                    runtime_error("only lists and maps index");
                    return Status::Error;
                }
                break;
            }
            case Op::SetIndex: {
                Value v = std::move(stack.back());
                stack.pop_back();
                Value idx = std::move(stack.back());
                stack.pop_back();
                Value obj = std::move(stack.back());
                stack.pop_back();
                if (obj.kind == Value::Kind::List) {
                    if (!idx.is_num()) {
                        runtime_error("list index must be a number");
                        return Status::Error;
                    }
                    const double di = idx.num;
                    if (di < 0.0 ||
                        di >= static_cast<double>(obj.list->size())) {
                        runtime_error("list index out of range in "
                                      "assignment");
                        return Status::Error;
                    }
                    (*obj.list)[static_cast<size_t>(di)] = std::move(v);
                } else if (obj.kind == Value::Kind::Map) {
                    if (!idx.is_str()) {
                        runtime_error("map key must be a string");
                        return Status::Error;
                    }
                    (*obj.map)[*idx.str] = std::move(v);
                } else {
                    runtime_error("only lists and maps assign by index");
                    return Status::Error;
                }
                break;
            }
            case Op::MakeList: {
                const uint16_t n = read_u16();
                Value l = Value::make_list();
                l.list->assign(stack.end() - n, stack.end());
                stack.resize(stack.size() - n);
                stack.push_back(std::move(l));
                break;
            }
            case Op::MakeMap: {
                const uint16_t n = read_u16();
                Value m = Value::make_map();
                const size_t start = stack.size() - size_t{n} * 2;
                for (size_t i = 0; i < n; ++i)
                    (*m.map)[*stack[start + i * 2].str] =
                        stack[start + i * 2 + 1];
                stack.resize(start);
                stack.push_back(std::move(m));
                break;
            }
            case Op::Return: {
                Value r = std::move(stack.back());
                stack.pop_back();
                const size_t base = fr.base;
                im.frames.pop_back();
                stack.resize(base);
                stack.push_back(std::move(r));
                break;
            }
            case Op::Halt:
                im.done = true;
                return Status::Done;
            case Op::HaltValue: {
                result_ = std::move(stack.back());
                stack.pop_back();
                im.done = true;
                return Status::Done;
            }
        }
    }
    return Status::Yielded;
}

namespace {

Value nat_err(Vm& vm, const std::string& msg) {
    vm.set_error(msg);
    return Value::nil();
}

}  // namespace

void add_core_natives(Env& env) {
    env.add("len", "len(list|map|string)", 1, 1,
            [](Vm& vm, std::vector<Value>& a) {
                const Value& v = a[0];
                if (v.kind == Value::Kind::List)
                    return Value::number(
                        static_cast<double>(v.list->size()));
                if (v.kind == Value::Kind::Map)
                    return Value::number(static_cast<double>(v.map->size()));
                if (v.kind == Value::Kind::Str)
                    return Value::number(static_cast<double>(v.str->size()));
                return nat_err(vm, "len() needs a list, map or string");
            });
    env.add("str", "str(value)", 1, 1, [](Vm&, std::vector<Value>& a) {
        return Value::string(to_display(a[0]));
    });
    env.add("num", "num(string) -> number or nil", 1, 1,
            [](Vm&, std::vector<Value>& a) {
                if (a[0].is_num()) return a[0];
                if (!a[0].is_str()) return Value::nil();
                const char* s = a[0].str->c_str();
                char* end = nullptr;
                const double v = std::strtod(s, &end);
                while (end && (*end == ' ' || *end == '\t')) ++end;
                if (end == s || (end && *end)) return Value::nil();
                return Value::number(v);
            });
    env.add("type", "type(value) -> \"nil|bool|num|str|list|map|fn\"", 1, 1,
            [](Vm&, std::vector<Value>& a) {
                switch (a[0].kind) {
                    case Value::Kind::Nil: return Value::string("nil");
                    case Value::Kind::Bool: return Value::string("bool");
                    case Value::Kind::Num: return Value::string("num");
                    case Value::Kind::Str: return Value::string("str");
                    case Value::Kind::List: return Value::string("list");
                    case Value::Kind::Map: return Value::string("map");
                    default: return Value::string("fn");
                }
            });
    auto num1 = [&env](const char* name, const char* sig,
                       double (*f)(double)) {
        env.add(name, sig, 1, 1, [f, name](Vm& vm, std::vector<Value>& a) {
            if (!a[0].is_num())
                return nat_err(vm, std::string(name) + "() needs a number");
            return Value::number(f(a[0].num));
        });
    };
    num1("floor", "floor(n)", [](double v) { return std::floor(v); });
    num1("ceil", "ceil(n)", [](double v) { return std::ceil(v); });
    num1("round", "round(n)", [](double v) { return std::round(v); });
    num1("abs", "abs(n)", [](double v) { return std::fabs(v); });
    num1("sqrt", "sqrt(n)", [](double v) { return std::sqrt(v); });
    const auto extremum = [&](const char* name, const char* sig, bool lo) {
        env.add(name, sig, 1, -1, [name, lo](Vm& vm, std::vector<Value>& a) {
            double m = 0.0;
            for (size_t i = 0; i < a.size(); ++i) {
                if (!a[i].is_num())
                    return nat_err(vm, std::string(name) + "() needs numbers");
                m = i == 0     ? a[i].num
                    : lo       ? std::min(m, a[i].num)
                               : std::max(m, a[i].num);
            }
            return Value::number(m);
        });
    };
    extremum("min", "min(a, b, ...)", true);
    extremum("max", "max(a, b, ...)", false);
    env.add("near", "near(a, b, eps?) - float-tolerant equality (1e-4)",
            2, 3, [](Vm& vm, std::vector<Value>& a) {
                if (!a[0].is_num() || !a[1].is_num())
                    return nat_err(vm, "near() needs numbers");
                const double eps =
                    a.size() > 2 && a[2].is_num() ? a[2].num : 1e-4;
                return Value::boolean(std::fabs(a[0].num - a[1].num) <=
                                      eps);
            });
    env.add("clamp", "clamp(v, lo, hi)", 3, 3,
            [](Vm& vm, std::vector<Value>& a) {
                if (!a[0].is_num() || !a[1].is_num() || !a[2].is_num())
                    return nat_err(vm, "clamp() needs numbers");
                return Value::number(
                    std::min(std::max(a[0].num, a[1].num), a[2].num));
            });
    env.add("range", "range(n) or range(a, b) or range(a, b, step)", 1, 3,
            [](Vm& vm, std::vector<Value>& a) {
                for (const Value& v : a)
                    if (!v.is_num())
                        return nat_err(vm, "range() needs numbers");
                double lo = 0.0, hi = a[0].num, step = 1.0;
                if (a.size() >= 2) {
                    lo = a[0].num;
                    hi = a[1].num;
                }
                if (a.size() == 3) step = a[2].num;
                if (step == 0.0) return nat_err(vm, "range() step is 0");
                Value out = Value::make_list();
                double count = (hi - lo) / step;
                if (count > 1.0e6)
                    return nat_err(vm, "range() longer than 1e6");
                for (double v = lo; step > 0.0 ? v < hi : v > hi; v += step)
                    out.list->push_back(Value::number(v));
                return out;
            });
    env.add("push", "push(list, value) -> list", 2, 2,
            [](Vm& vm, std::vector<Value>& a) {
                if (a[0].kind != Value::Kind::List)
                    return nat_err(vm, "push() needs a list");
                a[0].list->push_back(a[1]);
                return a[0];
            });
    env.add("pop", "pop(list) -> removed value", 1, 1,
            [](Vm& vm, std::vector<Value>& a) {
                if (a[0].kind != Value::Kind::List || a[0].list->empty())
                    return nat_err(vm, "pop() needs a non-empty list");
                Value v = a[0].list->back();
                a[0].list->pop_back();
                return v;
            });
    env.add("remove_at", "remove_at(list, index) -> list", 2, 2,
            [](Vm& vm, std::vector<Value>& a) {
                if (a[0].kind != Value::Kind::List || !a[1].is_num())
                    return nat_err(vm, "remove_at(list, index)");
                const double i = a[1].num;
                if (i < 0.0 || i >= static_cast<double>(a[0].list->size()))
                    return nat_err(vm, "remove_at() index out of range");
                a[0].list->erase(a[0].list->begin() +
                                 static_cast<ptrdiff_t>(i));
                return a[0];
            });
    env.add("keys", "keys(map) -> list of key strings", 1, 1,
            [](Vm& vm, std::vector<Value>& a) {
                if (a[0].kind != Value::Kind::Map)
                    return nat_err(vm, "keys() needs a map");
                Value out = Value::make_list();
                for (const auto& [k, v] : *a[0].map)
                    out.list->push_back(Value::string(k));
                return out;
            });
    env.add("has", "has(map, key) -> bool", 2, 2,
            [](Vm& vm, std::vector<Value>& a) {
                if (a[0].kind != Value::Kind::Map || !a[1].is_str())
                    return nat_err(vm, "has(map, key)");
                return Value::boolean(a[0].map->count(*a[1].str) > 0);
            });
    env.add("del", "del(map, key) -> map", 2, 2,
            [](Vm& vm, std::vector<Value>& a) {
                if (a[0].kind != Value::Kind::Map || !a[1].is_str())
                    return nat_err(vm, "del(map, key)");
                a[0].map->erase(*a[1].str);
                return a[0];
            });
    env.add("contains", "contains(string|list, needle) -> bool", 2, 2,
            [](Vm& vm, std::vector<Value>& a) {
                if (a[0].is_str() && a[1].is_str())
                    return Value::boolean(a[0].str->find(*a[1].str) !=
                                          std::string::npos);
                if (a[0].kind == Value::Kind::List) {
                    for (const Value& v : *a[0].list) {
                        if (v.kind != a[1].kind) continue;
                        if (v.is_num() && v.num == a[1].num)
                            return Value::boolean(true);
                        if (v.is_str() && *v.str == *a[1].str)
                            return Value::boolean(true);
                    }
                    return Value::boolean(false);
                }
                return nat_err(vm, "contains(string|list, needle)");
            });
    env.add("find", "find(string, needle) -> index or -1", 2, 2,
            [](Vm& vm, std::vector<Value>& a) {
                if (!a[0].is_str() || !a[1].is_str())
                    return nat_err(vm, "find(string, needle)");
                const size_t at = a[0].str->find(*a[1].str);
                return Value::number(at == std::string::npos
                                         ? -1.0
                                         : static_cast<double>(at));
            });
    env.add("substr", "substr(string, start, count?)", 2, 3,
            [](Vm& vm, std::vector<Value>& a) {
                if (!a[0].is_str() || !a[1].is_num())
                    return nat_err(vm, "substr(string, start, count?)");
                const std::string& s = *a[0].str;
                size_t start = a[1].num < 0.0
                                   ? 0u
                                   : static_cast<size_t>(a[1].num);
                if (start > s.size()) start = s.size();
                size_t count = std::string::npos;
                if (a.size() == 3 && a[2].is_num() && a[2].num >= 0.0)
                    count = static_cast<size_t>(a[2].num);
                return Value::string(s.substr(start, count));
            });
    env.add("split", "split(string, sep) -> list", 2, 2,
            [](Vm& vm, std::vector<Value>& a) {
                if (!a[0].is_str() || !a[1].is_str() || a[1].str->empty())
                    return nat_err(vm, "split(string, sep)");
                Value out = Value::make_list();
                const std::string& s = *a[0].str;
                const std::string& sep = *a[1].str;
                size_t pos = 0;
                for (;;) {
                    const size_t at = s.find(sep, pos);
                    if (at == std::string::npos) {
                        out.list->push_back(Value::string(s.substr(pos)));
                        break;
                    }
                    out.list->push_back(
                        Value::string(s.substr(pos, at - pos)));
                    pos = at + sep.size();
                }
                return out;
            });
    env.add("join", "join(list, sep) -> string", 2, 2,
            [](Vm& vm, std::vector<Value>& a) {
                if (a[0].kind != Value::Kind::List || !a[1].is_str())
                    return nat_err(vm, "join(list, sep)");
                std::string out;
                for (size_t i = 0; i < a[0].list->size(); ++i) {
                    if (i) out += *a[1].str;
                    out += to_display((*a[0].list)[i]);
                }
                return Value::string(out);
            });
    env.add("sort", "sort(list) -> list (numbers or strings)", 1, 1,
            [](Vm& vm, std::vector<Value>& a) {
                if (a[0].kind != Value::Kind::List)
                    return nat_err(vm, "sort() needs a list");
                auto& l = *a[0].list;
                for (const Value& v : l)
                    if (!v.is_num() && !v.is_str())
                        return nat_err(vm,
                                       "sort() needs numbers or strings");
                std::stable_sort(l.begin(), l.end(),
                                 [](const Value& x, const Value& y) {
                                     if (x.is_num() && y.is_num())
                                         return x.num < y.num;
                                     if (x.is_str() && y.is_str())
                                         return *x.str < *y.str;
                                     return x.is_num();
                                 });
                return a[0];
            });
}

}  // namespace looks::script
