#include "Test.h"

#include "game/Movement.h"
#include "net/Protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>

using namespace arena::game;

namespace {
ArenaLayout openArena() {
    ArenaLayout a;
    a.radius = 34.0f;
    return a;
}
constexpr float kDt = 1.0f / 120.0f;
} // namespace

ARENA_TEST(movement_reaches_but_never_exceeds_max_speed) {
    Rules r;
    ArenaLayout a = openArena();
    MoveState s;
    float peak = 0.0f;
    for (int i = 0; i < 120; ++i) {
        stepMovement(s, MoveInput{0.0f, 1.0f, false}, r, a, kDt);
        peak = std::max(peak, speedOf(s));
    }
    CHECK_NEAR(speedOf(s), r.maxSpeed, 1e-3);
    CHECK(peak <= r.maxSpeed + 1e-3f);
    CHECK_NEAR(s.heading, 0.0, 1e-3);  // moving +Y faces heading 0
}

ARENA_TEST(movement_dash_and_cooldown) {
    Rules r;
    ArenaLayout a = openArena();
    MoveState s;
    CHECK(stepMovement(s, MoveInput{1.0f, 0.0f, true}, r, a, kDt));
    CHECK_NEAR(speedOf(s), r.dashSpeed, 1e-3);
    CHECK(!stepMovement(s, MoveInput{1.0f, 0.0f, true}, r, a, kDt));  // cooling down
    for (int i = 0; i < static_cast<int>(r.dashCooldown / kDt) + 2; ++i) {
        stepMovement(s, MoveInput{1.0f, 0.0f, false}, r, a, kDt);
    }
    CHECK(speedOf(s) <= r.maxSpeed + 1e-3f);  // braked back down after the dash
    CHECK(stepMovement(s, MoveInput{1.0f, 0.0f, true}, r, a, kDt));
}

ARENA_TEST(movement_stays_inside_arena_and_out_of_pillars) {
    Rules r;
    ArenaLayout a = openArena();
    a.pillars.push_back({10.0f, 0.0f, 2.0f});
    MoveState s;
    for (int i = 0; i < 1200; ++i) {  // run (and dash) straight into the pillar, then the rim
        stepMovement(s, MoveInput{1.0f, 0.02f, i % 200 == 0}, r, a, kDt);
        const float dp = std::hypot(s.x - 10.0f, s.y);
        CHECK(dp >= 2.0f + r.playerRadius - 1e-3f);
        CHECK(std::hypot(s.x, s.y) <= a.radius - r.playerRadius + 1e-3f);
    }
}

ARENA_TEST(movement_knockback_and_separation) {
    Rules r;
    ArenaLayout a = openArena();
    MoveState s;
    applyKnockback(s, 16.0f, 0.0f);
    CHECK(s.stun > 0.0f);
    stepMovement(s, MoveInput{-1.0f, 0.0f, true}, r, a, kDt);
    CHECK(s.vx > 15.0f);  // stunned: no dash, barely any control

    MoveState p;
    separateFrom(p, 1.0f, 0.0f, 1.6f);  // overlap of 0.6: we move half of it
    CHECK_NEAR(p.x, -0.3, 1e-5);
}

// The AI rejects movement faster than its speed budget (see
// DistributedArenaGameAI.onPlayerMoved). An honest client running and dashing
// as often as allowed must never trip it, or players would get corrected.
ARENA_TEST(movement_matches_the_ai_reference_trajectory) {
    // The AI moves every player with server/arena/motion.py and this client
    // predicts its own with Movement.cpp: they must agree. Reference values
    // from the Python code for the same script (server/tests/test_motion.py
    // checks the Python side against the same numbers): running, turning,
    // dashes (one refused by the cooldown), a knockback, the central pillar
    // and the arena wall.
    Rules r;
    ArenaLayout a;
    a.radius = 34.0f;
    a.pillars = {{0.0f, 0.0f, 2.6f},    {16.2f, 9.3f, 1.4f},   {0.0f, 18.7f, 1.4f}, {-16.2f, 9.3f, 1.4f},
                 {-16.2f, -9.4f, 1.4f}, {-0.0f, -18.7f, 1.4f}, {16.2f, -9.4f, 1.4f}};
    auto input = [](int k) {
        struct In { bool held; uint8_t dir; bool dash; };
        if (k < 120) return In{true, 0, false};
        if (k < 240) return In{true, 64, k == 120};
        if (k < 300) return In{false, 0, false};
        if (k < 700) return In{true, 160, k == 400 || k == 600};
        if (k < 900) return In{true, 32, k == 820};
        return In{true, 200, k == 1000 || k == 1300};
    };
    const double expected[][7] = {  // step, x, y, vx, vy, dashCooldown, stun
        {99, 11.572917, -3.000000, 9.000000, 0.000000, 0.0000, 0.0000},
        {199, 13.072917, 7.384375, 0.000000, 9.000000, 0.9417, 0.0000},
        {299, 13.072917, 11.311458, 0.000000, 0.000000, 0.1083, 0.0000},
        {399, 8.425163, 6.663704, -6.363961, -6.363961, 0.0000, 0.0000},
        {499, 5.232287, -1.091518, 0.495459, -5.792218, 0.7750, 0.0000},
        {599, 0.462809, -6.350324, -6.363961, -6.363961, 0.0000, 0.0000},
        {699, -7.940713, -14.753846, -6.363961, -6.363961, 0.7750, 0.0000},
        {799, -5.312043, -12.125177, 6.363961, 6.363961, 0.0000, 0.0000},
        {899, 3.091479, -3.721654, 6.363961, 6.363961, 0.9417, 0.0000},
        {999, 5.406415, -8.269672, 1.755813, -8.827068, 0.1083, 0.0000},
        {1099, 6.869592, -15.625562, 1.755813, -8.827068, 0.0000, 0.0000},
        {1199, 8.332770, -22.981451, 1.755813, -8.827068, 0.0000, 0.0000},
        {1299, 9.795947, -30.337341, 1.755813, -8.827068, 0.0000, 0.0000},
        {1399, 8.711559, -31.931379, -0.799673, 0.498415, 0.7750, 0.0000},
        {1499, 8.264507, -32.154905, -0.533894, -0.043514, 0.0000, 0.0000},
    };
    MoveState s;
    s.x = 5.0f;
    s.y = -3.0f;
    size_t next = 0;
    for (int k = 0; k < 1500; ++k) {
        const auto in = input(k);
        const MoveInput mi{in.held ? arena::net::dirX(in.dir) : 0.0f, in.held ? arena::net::dirY(in.dir) : 0.0f,
                           in.dash};
        stepMovement(s, mi, r, a, kDt);
        if (k == 450) applyKnockback(s, 10.0f, -5.0f);
        if (next < std::size(expected) && k == static_cast<int>(expected[next][0])) {
            const double *e = expected[next++];
            CHECK_NEAR(s.x, e[1], 2e-3);  // 15 s of floats against doubles
            CHECK_NEAR(s.y, e[2], 2e-3);
            CHECK_NEAR(s.vx, e[3], 1e-3);
            CHECK_NEAR(s.vy, e[4], 1e-3);
            CHECK_NEAR(s.dashCooldown, e[5], 1e-3);
            CHECK_NEAR(s.stun, e[6], 1e-3);
        }
    }
    CHECK(next == std::size(expected));
}
