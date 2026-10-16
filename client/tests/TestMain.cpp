#include "Test.h"

#include <chrono>
#include <cstring>

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    for (const auto& c : arenatest::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        const int before = arenatest::failures();
        const auto t0 = std::chrono::steady_clock::now();
        c.fn();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[%s] %s (%.1f ms)\n", arenatest::failures() == before ? " ok " : "FAIL", c.name, ms);
        ++run;
    }
    std::printf("%d test(s), %d failure(s)\n", run, arenatest::failures());
    return arenatest::failures() == 0 ? 0 : 1;
}
