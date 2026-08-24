// In-app automation scripting: a small deterministic language compiled
// to bytecode and stepped by the host's frame loop. The VM suspends on
// host waits (a native calls mark_suspend) and resumes with a value, so
// one script interleaves with frames without threads. No filesystem, no
// clock, no randomness in the core - every effect on the app goes
// through natives the host registers, and those bind to the same
// commands the UI executes.
//
// Language: numbers (double), strings, bools, nil, lists, maps,
// `let`/assignment, if/else, while, for-in, top-level fn, break/
// continue/return. Newlines end statements (expressions continue inside
// parens/brackets); `#` comments to end of line. Entity ids ride as
// numbers - doubles hold integers exactly far past the document's id
// counter.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace looks::script {

class Vm;

struct Value {
    enum class Kind : uint8_t { Nil, Bool, Num, Str, List, Map, Fn, Native };
    Kind kind = Kind::Nil;
    bool b = false;
    double num = 0.0;
    std::shared_ptr<std::string> str;
    std::shared_ptr<std::vector<Value>> list;
    // Ordered map: keys() and printing stay deterministic.
    std::shared_ptr<std::map<std::string, Value>> map;
    uint32_t fn = 0;       // chunk index (Kind::Fn)
    uint32_t native = 0;   // Env def index (Kind::Native)

    static Value nil() { return Value{}; }
    static Value boolean(bool v) {
        Value x;
        x.kind = Kind::Bool;
        x.b = v;
        return x;
    }
    static Value number(double v) {
        Value x;
        x.kind = Kind::Num;
        x.num = v;
        return x;
    }
    static Value string(std::string v) {
        Value x;
        x.kind = Kind::Str;
        x.str = std::make_shared<std::string>(std::move(v));
        return x;
    }
    static Value make_list() {
        Value x;
        x.kind = Kind::List;
        x.list = std::make_shared<std::vector<Value>>();
        return x;
    }
    static Value make_map() {
        Value x;
        x.kind = Kind::Map;
        x.map = std::make_shared<std::map<std::string, Value>>();
        return x;
    }

    bool truthy() const {
        if (kind == Kind::Nil) return false;
        if (kind == Kind::Bool) return b;
        if (kind == Kind::Num) return num != 0.0;
        return true;
    }
    bool is_num() const { return kind == Kind::Num; }
    bool is_str() const { return kind == Kind::Str; }
    // Convenience for natives: id/count access with truncation.
    uint64_t as_id() const {
        return kind == Kind::Num && num > 0.0 ? static_cast<uint64_t>(num)
                                              : 0ull;
    }
    const std::string& as_str() const {
        static const std::string empty;
        return kind == Kind::Str && str ? *str : empty;
    }
};

// Display form: str() and string concatenation share it. Integers print
// without a decimal point so ids round-trip through text.
std::string to_display(const Value& v);

// Natives receive the VM (for suspend / error raising) and the argument
// list; they return the call's result. A native that must wait on the
// frame loop records its wait with the host, calls vm.mark_suspend()
// and returns nil - the host later resume()s the VM with the real
// result, or fail()s it.
using NativeFn = std::function<Value(Vm&, std::vector<Value>&)>;

struct NativeDef {
    std::string name;
    std::string sig;    // one-line doc: "set_param(look, fx, name, value)"
    int min_args = 0;
    int max_args = 0;   // -1 = unbounded
    NativeFn fn;
};

class Env {
public:
    void add(std::string name, std::string sig, int min_args, int max_args,
             NativeFn fn);
    const NativeDef* find(const std::string& name) const;
    int index_of(const std::string& name) const;   // -1 = absent
    const std::vector<NativeDef>& defs() const { return defs_; }

private:
    std::vector<NativeDef> defs_;
    std::map<std::string, int> by_name_;
};

// Deterministic core library: len/str/num/type, math, list/map/string
// helpers, range. No I/O, no clock, no randomness.
void add_core_natives(Env& env);

struct CompileError {
    std::string message;
    int line = 0;
};

class Vm {
public:
    enum class Status { Done, Suspended, Yielded, Error };

    // `env` must outlive the returned VM. Null + `err` filled on a
    // compile error.
    static std::unique_ptr<Vm> compile(const std::string& source,
                                       std::string chunk_name,
                                       const Env& env, CompileError* err);
    ~Vm();

    // Runs until done, error, suspend, or `budget` instructions executed
    // (Yielded - call run again next frame). After Suspended, continue
    // with resume()/fail() instead.
    Status run(uint64_t budget);
    Status resume(Value v, uint64_t budget);
    // Aborts a suspended script with a runtime error (wait timeout).
    Status fail(const std::string& message);

    // For natives:
    void mark_suspend() { suspend_ = true; }
    void set_error(std::string message) {
        error_ = std::move(message);
        has_error_ = true;
    }

    const std::string& error() const { return error_; }
    int current_line() const;
    // The value of a top-level `return`, nil otherwise. Valid when Done.
    const Value& result() const { return result_; }

    struct Impl;

private:
    Vm();
    Status execute(uint64_t budget);

    std::unique_ptr<Impl> impl_;
    bool suspend_ = false;
    bool has_error_ = false;
    std::string error_;
    Value result_;
};

}  // namespace looks::script
