// Our player's prediction against an AI that simulates it from the same
// inputs (server/arena/simulation.py, here with the C++ movement code):
// inputs only when they change, no corrections when nothing differs, and an
// exact catch-up after a knockback or a late input.
#include "Test.h"

#include "game/Prediction.h"

#include <cmath>
#include <deque>

using namespace arena::game;
using arena::net::InputCmd;
using arena::net::SelfState;

namespace {

ArenaLayout arena34() {
    ArenaLayout a;
    a.radius = 34.0f;
    a.pillars = {{0.0f, 0.0f, 2.6f}};
    return a;
}

// The AI's side: applies inputs at the steps they name (or the next step it
// simulates, if they come late) and sends SELF as protocol.packSelf() does,
// continuing from exactly what it sent.
struct Ai {
    Rules rules;
    ArenaLayout arena = arena34();
    MoveState s;
    int64_t step = 0;
    std::deque<InputCmd> queue;
    bool held = false;
    uint8_t dir = 0;
    uint8_t ack = 0;

    void advanceTo(int64_t target) {
        while (step < target) {
            const int64_t k = ++step;
            bool dash = false;
            while (!queue.empty() && (queue.front().step <= (k & 0xffff))) {
                const InputCmd &in = queue.front();
                held = (in.flags & arena::net::kInputHeld) != 0;
                dir = in.dir;
                dash = dash || (in.flags & arena::net::kInputDash) != 0;
                ack = in.seq;
                queue.pop_front();
            }
            stepMovement(s, MoveInput{held ? arena::net::dirX(dir) : 0.0f, held ? arena::net::dirY(dir) : 0.0f, dash},
                         rules, arena, static_cast<float>(kPhysicsStep));
        }
    }

    SelfState self(uint8_t flags = 0, uint8_t know = 12) {
        auto q = [](float v, float scale) { return std::nearbyint(v * scale) / scale; };
        SelfState m;
        m.ack = ack;
        m.step = static_cast<uint16_t>(step & 0xffff);
        s.x = m.x = q(s.x, 100.0f);
        s.y = m.y = q(s.y, 100.0f);
        s.vx = m.vx = q(s.vx, 100.0f);
        s.vy = m.vy = q(s.vy, 100.0f);
        s.dashLeft = m.dashLeft = q(s.dashLeft, 1000.0f);
        s.dashCooldown = m.dashCooldown = q(s.dashCooldown, 1000.0f);
        s.stun = m.stun = q(s.stun, 1000.0f);
        m.heading = s.heading;
        m.flags = flags;
        m.knowDelay = know;
        return m;
    }
};

struct Run {
    Ai ai;
    Prediction me;
    int sends = 0;

    Run() {
        ai.s.x = 5.0f;
        ai.s.y = -3.0f;
        ai.step = 1000;
        me.start(ai.step, ai.s, 12);
    }

    // One client frame of two steps with this input; the AI gets it now.
    void frame(bool held, uint8_t dir, bool dash, double now) {
        InputCmd cmd;
        me.advance(ai.rules, ai.arena, {});
        if (me.setInput(held, dir, dash, now, 0, cmd)) {
            ai.queue.push_back(cmd);
            ++sends;
        }
        me.advance(ai.rules, ai.arena, {});
    }
};

} // namespace

ARENA_TEST(prediction_sends_only_changes) {
    Prediction p;
    p.start(100, MoveState{}, 12);
    InputCmd cmd;
    CHECK(!p.setInput(false, 0, false, 0.0, 0, cmd));  // nothing pressed: nothing to say
    CHECK(p.setInput(true, 10, false, 0.0, 5, cmd));    // pressed
    CHECK(cmd.step == 101 && (cmd.flags & arena::net::kInputHeld) && cmd.dir == 10 && (cmd.flags >> 2) == 5);
    CHECK(!p.setInput(true, 10, false, 0.1, 5, cmd));   // held: nothing new
    CHECK(!p.setInput(true, 12, false, 0.2, 5, cmd));   // 2.8 degrees of thumb wobble: not a turn
    CHECK(p.setInput(true, 20, false, 0.3, 5, cmd));    // a turn
    CHECK(!p.setInput(true, 30, false, 0.31, 5, cmd));  // another one too soon: it waits
    CHECK(p.setInput(true, 30, false, 0.4, 5, cmd));
    CHECK(p.setInput(true, 30, true, 0.41, 5, cmd));    // a dash always goes at once
    CHECK(cmd.flags & arena::net::kInputDash);
    CHECK(p.setInput(false, 30, false, 0.42, 5, cmd));  // released
    CHECK(p.inputsSent() == 5);
}

ARENA_TEST(prediction_matches_the_ai_step_for_step) {
    Run run;
    double now = 0.0;
    for (int f = 0; f < 600; ++f, now += 1.0 / 60.0) {
        const bool held = (f / 40) % 3 != 2;
        const uint8_t dir = static_cast<uint8_t>((f / 25) * 37);
        run.frame(held, dir, f % 150 == 75, now);
        if (f % 30 == 29) {
            // The AI is a few steps behind (its lag) when it reports.
            run.ai.advanceTo(run.me.step() - 6);
            float dx = 0.0f, dy = 0.0f;
            const SelfState m = run.ai.self();
            CHECK(!run.me.reconcile(m, run.ai.step, run.ai.rules, run.ai.arena, {}, dx, dy));
            CHECK(std::hypot(dx, dy) < 0.01f);  // rounding to centimetres, nothing more
        }
    }
    CHECK(run.me.corrections() == 0);
    CHECK(run.sends < 120);  // changes only, not 600 frames' worth
}

ARENA_TEST(prediction_catches_up_exactly_after_a_knockback) {
    Run run;
    double now = 0.0;
    for (int f = 0; f < 60; ++f, now += 1.0 / 60.0) run.frame(true, 64, false, now);
    run.ai.advanceTo(run.me.step() - 8);
    applyKnockback(run.ai.s, 12.0f, 3.0f);  // a hit the client could not know of
    float dx = 0.0f, dy = 0.0f;
    CHECK(run.me.reconcile(run.ai.self(arena::net::kSelfKnock), run.ai.step, run.ai.rules, run.ai.arena, {}, dx,
                           dy));
    CHECK(run.me.corrections() == 0);  // a knockback is news, not a misprediction
    run.ai.advanceTo(run.me.step());
    CHECK_NEAR(run.me.state().x, run.ai.s.x, 1e-3);
    CHECK_NEAR(run.me.state().y, run.ai.s.y, 1e-3);
    CHECK_NEAR(run.me.state().stun, run.ai.s.stun, 1e-4);
    CHECK(std::hypot(dx, dy) > 0.05f);  // the caller fades this out on screen
}

ARENA_TEST(prediction_follows_an_input_the_ai_got_late) {
    Run run;
    double now = 0.0;
    for (int f = 0; f < 30; ++f, now += 1.0 / 60.0) run.frame(true, 0, false, now);
    run.ai.advanceTo(run.me.step());
    // Turn now, but the AI has already simulated past it when it arrives.
    InputCmd cmd;
    run.me.advance(run.ai.rules, run.ai.arena, {});
    CHECK(run.me.setInput(true, 64, false, now, 0, cmd));
    run.me.advance(run.ai.rules, run.ai.arena, {});
    for (int i = 0; i < 10; ++i) run.me.advance(run.ai.rules, run.ai.arena, {});
    run.ai.advanceTo(run.me.step() - 4);  // without it
    float dx = 0.0f, dy = 0.0f;
    // Its SELF does not include the turn (ack is older): the client takes the
    // turn as starting at the AI's next step, which is what the AI will do.
    CHECK(run.me.reconcile(run.ai.self(), run.ai.step, run.ai.rules, run.ai.arena, {}, dx, dy));
    CHECK(run.me.lastStale());
    run.ai.queue.push_back(cmd);  // arrives now: applied at the next step
    run.ai.advanceTo(run.me.step());
    CHECK_NEAR(run.me.state().x, run.ai.s.x, 1e-3);
    CHECK_NEAR(run.me.state().y, run.ai.s.y, 1e-3);
}

ARENA_TEST(prediction_bumps_into_others_by_the_ais_rule) {
    // Another player standing at (3, 0): its sample was sent at step 990.
    KnownSample other;
    other.s.t = 990.0 / arena::net::kStepsPerSec;
    other.s.x = 3.0f;
    other.sentStep = 990;
    const Prediction::Others others{{&other, 1}};
    Rules rules;
    ArenaLayout open;
    open.radius = 1000.0f;

    for (int know : {5, 200}) {  // the sample old enough to count / too new all along
        Prediction p;
        p.start(1000, MoveState{}, know);
        InputCmd cmd;
        p.setInput(true, 0, false, 0.0, 0, cmd);  // run into it
        float closest = 1e9f;
        for (int i = 0; i < 60; ++i) {
            p.advance(rules, open, others);
            closest = std::min(closest, std::hypot(p.state().x - 3.0f, p.state().y));
        }
        if (know == 5) {
            CHECK(closest > 2.0f * rules.playerRadius - 0.6f);  // pushed off it (half the overlap per step)
            CHECK(p.state().x < 3.0f);
        } else {
            CHECK(p.state().x > 3.0f);  // it did not count yet: straight through
        }
    }
}
