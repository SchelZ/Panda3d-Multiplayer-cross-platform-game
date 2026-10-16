#include "Test.h"

#include "game/Movement.h"

#include <algorithm>
#include <cmath>

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
ARENA_TEST(movement_never_trips_the_server_speed_check) {
    Rules r;
    ArenaLayout a = openArena();
    MoveState s;
    s.x = -25.0f;
    const double tolerance = 1.3, bankSeconds = 1.5, slack = 0.6;
    const double maxBudget = r.maxSpeed * tolerance * bankSeconds;
    double budget = maxBudget, lastSample = 0.0, t = 0.0;
    float vx = s.x, vy = s.y;
    int violations = 0;
    for (int i = 0; i < 120 * 20; ++i) {
        const bool dashed = stepMovement(s, MoveInput{std::cos(t * 0.3f) > 0 ? 1.0f : -1.0f, 0.3f, true}, r, a, kDt);
        t += kDt;
        if (dashed) budget += r.dashSpeed * r.dashTime * tolerance;
        if (t - lastSample >= 0.05) {  // 20 Hz broadcasts, int16/10 quantised
            const double qx = std::round(s.x * 10.0) / 10.0, qy = std::round(s.y * 10.0) / 10.0;
            budget = std::min(budget + r.maxSpeed * tolerance * (t - lastSample), std::max(maxBudget, budget));
            const double d = std::hypot(qx - vx, qy - vy);
            if (d > budget + slack) {
                ++violations;
            } else {
                budget = std::max(0.0, budget - d);
                vx = static_cast<float>(qx);
                vy = static_cast<float>(qy);
            }
            lastSample = t;
        }
    }
    CHECK(violations == 0);
}
