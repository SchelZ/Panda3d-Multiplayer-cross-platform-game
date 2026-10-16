// Tiny self-contained test harness (no external framework to fetch/build).
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace arenatest {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

} // namespace arenatest

#define ARENA_TEST(name)                                                    \
    static void name();                                                  \
    static ::arenatest::Registrar name##_registrar(#name, &name);           \
    static void name()

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("    CHECK failed: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); \
            ++::arenatest::failures();                                                    \
        }                                                                              \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                        \
    do {                                                                                             \
        const double va_ = double(a), vb_ = double(b);                                               \
        if (!(std::fabs(va_ - vb_) <= double(eps))) {                                                \
            std::printf("    CHECK_NEAR failed: %s=%g vs %s=%g (%s:%d)\n", #a, va_, #b, vb_, __FILE__, \
                        __LINE__);                                                                   \
            ++::arenatest::failures();                                                                  \
        }                                                                                            \
    } while (0)
