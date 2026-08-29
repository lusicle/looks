#include <memory>
#include <string>
#include <vector>

#include "script/script.h"
#include "test_framework.h"

using looks::script::add_core_natives;
using looks::script::CompileError;
using looks::script::Env;
using looks::script::to_display;
using looks::script::Value;
using looks::script::Vm;

namespace {

struct Run {
    Vm::Status status = Vm::Status::Error;
    Value result;
    std::string error;
};

Run run_src(const std::string& src, Env* custom = nullptr) {
    static Env core_env = [] {
        Env e;
        add_core_natives(e);
        return e;
    }();
    Env& env = custom ? *custom : core_env;
    CompileError cerr;
    auto vm = Vm::compile(src, "test", env, &cerr);
    Run r;
    if (!vm) {
        r.error = cerr.message;
        return r;
    }
    r.status = vm->run(1'000'000);
    if (r.status == Vm::Status::Done) r.result = vm->result();
    if (r.status == Vm::Status::Error) r.error = vm->error();
    return r;
}

double num_result(const std::string& src) {
    const Run r = run_src(src);
    CHECK(r.status == Vm::Status::Done);
    CHECK(r.result.is_num());
    return r.result.num;
}

std::string str_result(const std::string& src) {
    const Run r = run_src(src);
    CHECK(r.status == Vm::Status::Done);
    CHECK(r.result.is_str());
    return r.result.as_str();
}

}  // namespace

TEST(script_arithmetic_and_precedence) {
    CHECK_EQ(num_result("return 1 + 2 * 3"), 7.0);
    CHECK_EQ(num_result("return (1 + 2) * 3"), 9.0);
    CHECK_EQ(num_result("return 10 % 4"), 2.0);
    CHECK_EQ(num_result("return -3 + 5"), 2.0);
    CHECK_EQ(num_result("return 7 / 2"), 3.5);
}

TEST(script_globals_and_locals) {
    CHECK_EQ(num_result("let x = 4\nx = x + 1\nreturn x"), 5.0);
    CHECK_EQ(num_result("let x = 1\nif true { let x = 9 }\nreturn x"), 1.0);
}

TEST(script_strings) {
    CHECK_EQ(str_result("return \"a\" + \"b\" + 3"), "ab3");
    CHECK_EQ(str_result("return str(12)"), "12");
    CHECK_EQ(str_result("return str(1.5)"), "1.5");
    // Ids stay integral through display.
    CHECK_EQ(str_result("return str(9007199254740000)"), "9007199254740000");
    CHECK_EQ(num_result("return num(\"42.5\")"), 42.5);
    const Run r = run_src("return num(\"nope\")");
    CHECK(r.status == Vm::Status::Done);
    CHECK(r.result.kind == Value::Kind::Nil);
}

TEST(script_control_flow) {
    CHECK_EQ(num_result("let n = 0\nlet i = 0\nwhile i < 10 { n = n + i\n"
                        "i = i + 1 }\nreturn n"),
             45.0);
    CHECK_EQ(num_result("let n = 0\nfor x in [1, 2, 3, 4] { n = n + x }\n"
                        "return n"),
             10.0);
    CHECK_EQ(num_result("let n = 0\nfor x in range(5) { if x == 3 { "
                        "continue }\nn = n + x }\nreturn n"),
             7.0);
    CHECK_EQ(num_result("let n = 0\nfor x in range(100) { if x == 4 { "
                        "break }\nn = n + 1 }\nreturn n"),
             4.0);
    // Falsy is exactly nil / false / 0 - an empty string stays truthy.
    CHECK_EQ(num_result("if 0 { return 1 } else if \"\" { return 2 } else "
                        "{ return 3 }"),
             2.0);
    CHECK_EQ(num_result("if nil { return 1 } else if false { return 2 } "
                        "else { return 3 }"),
             3.0);
}

TEST(script_lists_and_maps) {
    CHECK_EQ(num_result("let l = [1, 2, 3]\nl[1] = 20\nreturn l[1] + "
                        "len(l)"),
             23.0);
    CHECK_EQ(num_result("let m = { a: 1, \"b two\": 2 }\nm.a = 5\n"
                        "return m.a + m[\"b two\"]"),
             7.0);
    const Run missing = run_src("let m = { a: 1 }\nreturn m.zz");
    CHECK(missing.status == Vm::Status::Done);
    CHECK(missing.result.kind == Value::Kind::Nil);
    CHECK_EQ(str_result("let m = { b: 1, a: 2 }\nreturn join(keys(m), "
                        "\",\")"),
             "a,b");
    CHECK_EQ(num_result("let l = []\nfor i in range(3) { push(l, i * i) }\n"
                        "return l[2]"),
             4.0);
    // Nested member assignment mutates through the shared reference.
    CHECK_EQ(num_result("let m = { inner: { v: 1 } }\nm.inner.v = 8\n"
                        "return m.inner.v"),
             8.0);
}

TEST(script_functions) {
    CHECK_EQ(num_result("fn add(a, b) { return a + b }\nreturn add(2, 3)"),
             5.0);
    CHECK_EQ(num_result("fn fib(n) { if n < 2 { return n }\nreturn "
                        "fib(n - 1) + fib(n - 2) }\nreturn fib(12)"),
             144.0);
    const Run r = run_src("fn f(a) { return a }\nreturn f(1, 2)");
    CHECK(r.status == Vm::Status::Error);
}

TEST(script_logic_and_newlines) {
    CHECK_EQ(num_result("return (true && 5) + 0"), 5.0);
    CHECK_EQ(num_result("return (false || 7) + 0"), 7.0);
    // A newline ends the statement; the next line is its own statement.
    CHECK_EQ(num_result("let a = 1\nlet b = 2\na = a\n- b\nreturn a"), 1.0);
    // Inside parens/brackets expressions continue across lines.
    CHECK_EQ(num_result("return (1 +\n 2 +\n 3)"), 6.0);
    CHECK_EQ(num_result("let l = [\n 1,\n 2,\n]\nreturn len(l)"), 2.0);
}

TEST(script_core_natives) {
    CHECK_EQ(num_result("return clamp(15, 0, 10)"), 10.0);
    CHECK_EQ(num_result("return floor(2.7) + ceil(2.2) + round(2.5)"),
             8.0);
    CHECK_EQ(num_result("return min(3, 1, 2) + max(3, 1, 2)"), 4.0);
    CHECK_EQ(num_result("return len(split(\"a,b,c\", \",\"))"), 3.0);
    CHECK_EQ(str_result("return substr(\"monitor\", 0, 3)"), "mon");
    CHECK_EQ(num_result("return find(\"abcdef\", \"cd\")"), 2.0);
    const Run c = run_src("return contains([1, 2, 3], 2)");
    CHECK(c.status == Vm::Status::Done);
    CHECK(c.result.kind == Value::Kind::Bool && c.result.b);
    CHECK_EQ(str_result("return join(sort([\"b\", \"a\"]), \"\")"), "ab");
    CHECK_EQ(num_result("let l = sort([3, 1, 2])\nreturn l[0] * 100 + "
                        "l[2]"),
             103.0);
    CHECK_EQ(str_result("return type([])"), "list");
}

TEST(script_runtime_errors_carry_lines) {
    const Run r = run_src("let a = 1\nreturn a + [1]");
    CHECK(r.status == Vm::Status::Error);
    CHECK(!r.error.empty());
    const Run u = run_src("return nosuch(1)");
    CHECK(u.status == Vm::Status::Error);
    CHECK(u.error.find("nosuch") != std::string::npos);
    const Run oob = run_src("let l = [1]\nreturn l[5]");
    CHECK(oob.status == Vm::Status::Error);
}

TEST(script_compile_errors) {
    Env env;
    add_core_natives(env);
    CompileError err;
    CHECK(Vm::compile("let = 4", "t", env, &err) == nullptr);
    CHECK(Vm::compile("if x { ", "t", env, &err) == nullptr);
    CHECK(Vm::compile("break", "t", env, &err) == nullptr);
    CHECK(Vm::compile("fn a() { fn b() {} }", "t", env, &err) == nullptr);
    CHECK(Vm::compile("1 + ", "t", env, &err) == nullptr);
}

TEST(script_suspend_resume) {
    Env env;
    add_core_natives(env);
    int waits = 0;
    env.add("pause_here", "pause_here()", 0, 0,
            [&waits](Vm& vm, std::vector<Value>&) {
                ++waits;
                vm.mark_suspend();
                return Value::nil();
            });
    CompileError err;
    auto vm = Vm::compile(
        "let total = 0\nfor i in range(3) { total = total + "
        "pause_here() }\nreturn total",
        "t", env, &err);
    CHECK(vm != nullptr);
    if (!vm) return;
    Vm::Status st = vm->run(100000);
    int resumes = 0;
    while (st == Vm::Status::Suspended && resumes < 10) {
        ++resumes;
        st = vm->resume(Value::number(5.0), 100000);
    }
    CHECK(st == Vm::Status::Done);
    CHECK_EQ(waits, 3);
    CHECK_EQ(resumes, 3);
    CHECK_EQ(vm->result().num, 15.0);
}

TEST(script_budget_yields_and_continues) {
    Env env;
    add_core_natives(env);
    CompileError err;
    auto vm = Vm::compile(
        "let n = 0\nwhile n < 100000 { n = n + 1 }\nreturn n", "t", env,
        &err);
    CHECK(vm != nullptr);
    if (!vm) return;
    int yields = 0;
    Vm::Status st = vm->run(10000);
    while (st == Vm::Status::Yielded && yields < 1000) {
        ++yields;
        st = vm->run(10000);
    }
    CHECK(st == Vm::Status::Done);
    CHECK(yields > 5);
    CHECK_EQ(vm->result().num, 100000.0);
}

TEST(script_fail_aborts_suspended) {
    Env env;
    add_core_natives(env);
    env.add("wait_forever", "wait_forever()", 0, 0,
            [](Vm& vm, std::vector<Value>&) {
                vm.mark_suspend();
                return Value::nil();
            });
    CompileError err;
    auto vm = Vm::compile("wait_forever()\nreturn 1", "t", env, &err);
    CHECK(vm != nullptr);
    if (!vm) return;
    CHECK(vm->run(1000) == Vm::Status::Suspended);
    CHECK(vm->fail("timed out") == Vm::Status::Error);
    CHECK(vm->error().find("timed out") != std::string::npos);
}

TEST(script_display_forms) {
    CHECK_EQ(to_display(Value::nil()), "nil");
    CHECK_EQ(to_display(Value::boolean(true)), "true");
    Value l = Value::make_list();
    l.list->push_back(Value::number(1.0));
    l.list->push_back(Value::string("x"));
    CHECK_EQ(to_display(l), "[1, \"x\"]");
    Value m = Value::make_map();
    (*m.map)["k"] = Value::number(2.0);
    CHECK_EQ(to_display(m), "{k: 2}");
}
