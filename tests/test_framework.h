// Micro test harness — no third-party test framework. Each test
// file defines TESTs and links test_main.cpp. CHECK failures report and mark
// the run failed but keep going; the exe exits non-zero for CTest.

#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

namespace testfw {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int failures = 0;
inline const char* current_test = "";

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline int run_all() {
#ifdef _MSC_VER
    // Headless: debug CRT asserts report to stderr and abort instead of
    // opening a dialog no harness can click. The trace line below names
    // the test that died - stderr is unbuffered, stdout is not.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    int ran = 0;
    for (const TestCase& test : registry()) {
        current_test = test.name;
        std::fprintf(stderr, "[run] %s\n", test.name);
        std::fflush(stderr);
        const int failures_before = failures;
        test.fn();
        ++ran;
        if (failures > failures_before)
            std::printf("[FAIL] %s\n", test.name);
    }
    std::printf("%d tests, %d failures\n", ran, failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace testfw

#define TEST(name)                                                     \
    static void test_fn_##name();                                      \
    static ::testfw::Registrar test_reg_##name(#name, test_fn_##name); \
    static void test_fn_##name()

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("  CHECK failed: %s (%s:%d) in %s\n", #cond,       \
                        __FILE__, __LINE__, ::testfw::current_test);       \
            ++::testfw::failures;                                          \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))
