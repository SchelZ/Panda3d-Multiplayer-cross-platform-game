#include "Test.h"

#include "net/ClockDelta.h"

using arena::net::ClockDelta;

ARENA_TEST(clock_delta_round_trip) {
    ClockDelta c;
    CHECK(!c.synced());
    // Server said "1234.56 s" (123456 ticks) when our clock read 10.0 s.
    CHECK(c.resynchronize(10.0, 123456, 0.01));
    CHECK(c.synced());
    CHECK_NEAR(c.delta(), 10.0 - 1234.56, 1e-9);
    CHECK(c.localToNetworkTime(10.0, 32) == 123456);
    CHECK_NEAR(c.networkToLocalTime(123456, 10.0, 32), 10.0, 1e-9);
    CHECK_NEAR(c.networkToLocalTime(123466, 10.0, 32), 10.1, 1e-9);
}

ARENA_TEST(clock_delta_16_bit_wraps_like_python) {
    ClockDelta c;
    c.resynchronize(0.0, 32760, 0.0);  // network time just below the int16 limit
    // 0.1 s later the 16-bit timestamp has wrapped to the negative side.
    const int32_t ts = c.localToNetworkTime(0.1, 16);
    CHECK(ts == -32766);  // ClockDelta.__signExtend(32770)
    // And it still maps back to the right local time.
    CHECK_NEAR(c.networkToLocalTime(ts, 0.1, 16), 0.1, 1e-9);
    CHECK_NEAR(c.networkToLocalTime(ts, 0.0, 16), 0.1, 1e-9);
}

ARENA_TEST(clock_delta_intersects_measurements) {
    ClockDelta c;
    c.resynchronize(100.0, 0, 0.05);       // delta 100 +/- 0.05
    CHECK(c.resynchronize(100.0, -2, 0.05)); // delta 100.02 +/- 0.05
    CHECK_NEAR(c.delta(), 100.01, 1e-9);
    CHECK_NEAR(c.uncertainty(), 0.04, 1e-9);
    // A disjoint measurement is rejected unless trusted.
    CHECK(!c.resynchronize(100.0, -100, 0.01, false));
    CHECK_NEAR(c.delta(), 100.01, 1e-9);
    CHECK(c.resynchronize(100.0, -100, 0.01, true));
    CHECK_NEAR(c.delta(), 101.0, 1e-9);
}
