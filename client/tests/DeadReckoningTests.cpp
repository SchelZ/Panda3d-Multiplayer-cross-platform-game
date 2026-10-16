#include "Test.h"

#include "game/DeadReckoning.h"
#include "game/Movement.h"
#include "game/RemoteMotion.h"
#include "net/Protocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <iterator>
#include <string>

using namespace arena::game;
using arena::net::WireState;

namespace {

constexpr double kFrame = 1.0 / 60.0;
constexpr float kStep = 1.0f / 120.0f;

struct RunStats {
    int sends = 0;
    double maxErrorWhenFree = 0.0;  // the others' prediction error once a send is allowed
    double minGap = 1e9;
    double maxDrawnError = 0.0;     // drawn vs the truth `delay` ago
    double maxDrawnStep = 0.0;      // biggest drawn move in one frame (snaps show here)
    double maxBacktrack = 0.0;      // drawn motion against every true direction (m)
};

// A receiver whose samples arrive `latency` late and who draws `delay` back.
struct Receiver {
    double latency = 0.0, delay = 0.0;
};

// Runs the real movement code for `seconds` with input(t), sending samples as
// ArenaPlayer::broadcastPose does, and drawing them as `rx` would.
RunStats run(DeadReckoning &dr, RemoteMotion &drawn, double seconds,
             const std::function<MoveInput(double)> &input, MoveState &s, double t0 = 0.0,
             Receiver rx = {}) {
    Rules rules;
    ArenaLayout arena;
    arena.radius = 1000.0f;
    RunStats st;
    double lastSend = dr.lastSent();
    struct InFlight {
        double t, sentAt;  // a dash sample's time is its start, before it was sent
        WireState w;
    };
    std::deque<InFlight> inFlight;
    std::deque<std::array<double, 3>> truth;  // (t, x, y)
    float lastX = 0, lastY = 0;
    bool haveLast = false;
    for (double t = t0; t < t0 + seconds; t += kFrame) {
        const MoveInput in = input(t);
        bool dashed = false;
        for (int i = 0; i < 2; ++i) {
            dashed = stepMovement(s, i == 0 ? in : MoveInput{in.x, in.y, false}, rules, arena, kStep) || dashed;
        }
        truth.push_back({t, s.x, s.y});
        const float len = std::hypot(in.x, in.y);
        const float ix = len > 1e-3f ? in.x / len : 0.0f, iy = len > 1e-3f ? in.y / len : 0.0f;
        const MotionSample real{t, s.x, s.y, s.vx, s.vy, ix, iy, heldFor(s.dashLeft)};
        if (dashed || dr.due(real, rules)) {
            // As ArenaPlayer::broadcastPose: a dash sample is rewound to its start.
            const float since = dashed ? dashHoldTime(rules) - heldFor(s.dashLeft) : 0.0f;
            const WireState w = WireState::make(s.x - s.vx * since, s.y - s.vy * since, s.vx, s.vy,
                                                dashed ? arena::net::kFlagDash : 0, 0, ix, iy);
            dr.sent(t - since, w, rules);
            inFlight.push_back({t - since, t, w});
            ++st.sends;
            st.minGap = std::min(st.minGap, t - lastSend);
            lastSend = t;
        }
        while (!inFlight.empty() && inFlight.front().sentAt + rx.latency <= t + 1e-9) {
            const InFlight &f = inFlight.front();
            drawn.addSample(DeadReckoning::fromWire(f.w, f.t, rules), t, rx.delay, rules);
            inFlight.pop_front();
        }
        if (t - lastSend >= dr.settings().minInterval) {
            const MotionPose p = dr.predict(t, rules);
            st.maxErrorWhenFree = std::max(st.maxErrorWhenFree, double(std::hypot(p.x - s.x, p.y - s.y)));
        }
        if (!drawn.hasSample()) continue;
        const RemoteMotion::Pose p = drawn.update(t, rx.delay, rules);
        if (t < t0 + rx.delay + 0.3) {  // until the truth covers the delay and the window
            lastX = p.x;
            lastY = p.y;
            haveLast = true;
            continue;
        }
        // The truth at r = t - delay, between two frames.
        const double r = t - rx.delay;
        size_t k = 0;
        while (k + 2 < truth.size() && truth[k + 1][0] <= r + 1e-9) ++k;
        const double f = std::clamp((r - truth[k][0]) / (truth[k + 1][0] - truth[k][0]), 0.0, 1.0);
        const double tx = truth[k][1] + (truth[k + 1][1] - truth[k][1]) * f;
        const double ty = truth[k][2] + (truth[k + 1][2] - truth[k][2]) * f;
        st.maxDrawnError = std::max(st.maxDrawnError, std::hypot(p.x - tx, p.y - ty));
        if (haveLast) {
            const double dx = p.x - lastX, dy = p.y - lastY;
            st.maxDrawnStep = std::max(st.maxDrawnStep, std::hypot(dx, dy));
            // Against every true direction in [r - 0.3 s, r + 1 frame]: a rubber band.
            bool along = std::hypot(dx, dy) < 1e-4;
            for (size_t i = 1; i < truth.size() && truth[i][0] <= r + kFrame + 1e-9 && !along; ++i) {
                along = (truth[i][1] - truth[i - 1][1]) * dx + (truth[i][2] - truth[i - 1][2]) * dy > 0.0;
            }
            if (!along) st.maxBacktrack = std::max(st.maxBacktrack, std::hypot(dx, dy));
        }
        lastX = p.x;
        lastY = p.y;
        haveLast = true;
        while (truth.size() > 1 && truth.front()[0] < t - rx.delay - 0.3) truth.pop_front();
    }
    return st;
}

MoveInput taps(double t) {  // 120 ms presses every 0.4 s, often the other way
    const double phase = std::fmod(t, 0.4);
    const bool left = int(t / 0.4) % 3 == 1;
    return phase < 0.12 ? MoveInput{left ? -1.0f : 1.0f, 0.0f, false} : MoveInput{};
}

} // namespace

ARENA_TEST(predict_motion_matches_the_movement_code) {
    // predictMotion() is stepMovement() in closed form: accelerate, cruise,
    // brake, and hold a dash, within the 120 Hz steps' rounding.
    Rules rules;
    ArenaLayout arena;
    arena.radius = 1000.0f;
    struct Case {
        MoveInput in;
        float vx, vy;
        bool dash;
    };
    const Case cases[] = {{{1, 0, false}, 0, 0, false},   // start
                          {{0, 0, false}, 9, 0, false},   // let go
                          {{0, 1, false}, 9, 0, false},   // turn
                          {{-1, 0, false}, 9, 0, false},  // reverse
                          {{0, 1, true}, 9, 0, true}};    // dash, then run on
    for (const Case &c : cases) {
        MoveState s;
        s.vx = c.vx;
        s.vy = c.vy;
        s.dashCooldown = 0.0f;
        bool dashed = stepMovement(s, c.in, rules, arena, kStep);  // the dash starts here
        const MotionSample m{0.0, s.x, s.y, s.vx, s.vy, c.in.x, c.in.y, heldFor(s.dashLeft)};
        CHECK(dashed == c.dash);
        for (int i = 1; i <= 120; ++i) {
            stepMovement(s, MoveInput{c.in.x, c.in.y, false}, rules, arena, kStep);
            if (i % 12 != 0) continue;
            const MotionPose p = predictMotion(m, i * kStep, rules);
            CHECK_NEAR(p.x, s.x, 0.005);
            CHECK_NEAR(p.y, s.y, 0.005);
            CHECK_NEAR(p.vx, s.vx, 0.01);
            CHECK_NEAR(p.vy, s.vy, 0.01);
        }
    }
}

ARENA_TEST(dead_reckoning_straight_run_costs_only_heartbeats) {
    DeadReckoning dr;
    RemoteMotion drawn;
    MoveState s;
    run(dr, drawn, 1.0, [](double) { return MoveInput{0.0f, 1.0f, false}; }, s);  // reach full speed
    const RunStats st = run(dr, drawn, 4.0, [](double) { return MoveInput{0.0f, 1.0f, false}; }, s, 1.0);
    // Constant motion is predicted exactly: only the 0.5 s heartbeat remains.
    CHECK(st.sends <= 9);
    CHECK(st.maxErrorWhenFree <= dr.settings().maxPositionError);
}

ARENA_TEST(dead_reckoning_a_tap_costs_two_samples) {
    // Press and release: the others predict the acceleration and the braking.
    DeadReckoning dr;
    RemoteMotion drawn;
    MoveState s;
    const RunStats st = run(dr, drawn, 1.0, [](double t) {
        return t < 0.12 ? MoveInput{1.0f, 0.0f, false} : MoveInput{};
    }, s);
    CHECK(st.sends == 2);
    CHECK(st.maxErrorWhenFree < 0.03);
    const RunStats idle = run(dr, drawn, 3.0, [](double) { return MoveInput{}; }, s, 1.0);
    CHECK(idle.sends == 0);  // standing still costs nothing
    const RemoteMotion::Pose p = drawn.update(4.0, 0.0, Rules{});
    CHECK_NEAR(p.x, s.x, 0.011);
    CHECK_NEAR(p.y, s.y, 0.011);
}

ARENA_TEST(dead_reckoning_bounds_the_error_on_curves_and_dashes) {
    DeadReckoning dr;
    RemoteMotion drawn;
    MoveState s;
    const Receiver rx{0.03, 0.05};
    const RunStats circle = run(dr, drawn, 3.0, [](double t) {
        return MoveInput{float(std::cos(t * 2.5)), float(std::sin(t * 2.5)), false};
    }, s, 0.0, rx);
    const RunStats zigzag = run(dr, drawn, 3.0, [](double t) {
        const bool left = int(t / 0.3) % 2 == 0;
        return MoveInput{left ? -1.0f : 1.0f, 1.0f, false};
    }, s, 3.0, rx);
    const RunStats dashes = run(dr, drawn, 4.0, [](double t) {
        return MoveInput{1.0f, 0.3f, std::fmod(t, 1.7) < kFrame};
    }, s, 6.0, rx);
    for (const RunStats *st : {&circle, &zigzag, &dashes}) {
        CHECK(st->maxErrorWhenFree <= dr.settings().maxPositionError);
        CHECK(st->minGap >= dr.settings().minInterval - 1e-9);  // never above 20 Hz
        // Samples arrive within the delay: the drawing is the true motion
        // `delay` ago, within the sender's error bound, never going backwards,
        // and its biggest step is a dash's own (24 m/s * 1/60 s = 0.4 m).
        CHECK(st->maxDrawnError < dr.settings().maxPositionError + 0.03);
        CHECK(st->maxBacktrack < 0.005);  // no sliding back (0.3 m/s at most)
        CHECK(st->maxDrawnStep < 0.42);
    }
    CHECK(zigzag.sends <= 2 * 10 + 2);  // about one per input change
}

ARENA_TEST(remote_motion_draws_taps_as_they_happened) {
    // Short taps back and forth, the motion that used to overshoot and slide
    // back: with samples inside the view delay, the drawing is the truth.
    for (const Receiver rx : {Receiver{0.02, 0.04}, Receiver{0.11, 0.13}}) {
        DeadReckoning dr;
        RemoteMotion drawn;
        MoveState s;
        const RunStats st = run(dr, drawn, 6.0, taps, s, 0.0, rx);
        CHECK(st.maxBacktrack < 0.005);  // no sliding back (0.3 m/s at most)
        CHECK(st.maxDrawnError < 0.04);  // wire quantisation (1 cm, 0.25 m/s)
        CHECK(st.sends <= 2 * 15 + 1);   // two per tap
    }
    // Samples later than the delay: the release is seen late, so the drawing
    // overshoots, but it comes back as a drift (bounded step), not a snap.
    DeadReckoning dr;
    RemoteMotion drawn;
    MoveState s;
    const RunStats late = run(dr, drawn, 6.0, taps, s, 0.0, Receiver{0.12, 0.04});
    CHECK(late.maxDrawnStep < (9.0 + 1.5 * RemoteMotion::kMaxCorrectionSpeed) * kFrame);
}

ARENA_TEST(remote_motion_blends_a_late_turn_without_jumping) {
    Rules rules;
    RemoteMotion m;
    m.addSample(MotionSample{10.0, 0.0f, 0.0f, 9.0f, 0.0f, 1.0f, 0.0f, 0.0f}, 10.0, 0.0, rules);
    // At 10.5 the player is drawn at x=4.5; a sample says it turned to +y at
    // (3.6, 0) at full speed 0.1 s ago, so it is really at (3.6, 0.9).
    const RemoteMotion::Pose before = m.update(10.5, 0.0, rules);
    m.addSample(MotionSample{10.4, 3.6f, 0.0f, 0.0f, 9.0f, 0.0f, 1.0f, 0.0f}, 10.5, 0.0, rules);
    const RemoteMotion::Pose after = m.update(10.5, 0.0, rules);
    CHECK_NEAR(after.x, before.x, 1e-4);  // no jump when the sample arrives
    CHECK_NEAR(after.y, before.y, 1e-4);
    float lastX = after.x, lastY = after.y, maxStep = 0.0f;
    for (double t = 10.5 + kFrame; t < 11.1; t += kFrame) {
        const RemoteMotion::Pose p = m.update(t, 0.0, rules);
        maxStep = std::max(maxStep, std::hypot(p.x - lastX, p.y - lastY));
        lastX = p.x;
        lastY = p.y;
    }
    // Own speed plus at most kMaxCorrectionSpeed: a drift, never a snap.
    CHECK(maxStep < (9.0f + RemoteMotion::kMaxCorrectionSpeed) * float(kFrame) + 1e-3f);
    const RemoteMotion::Pose done = m.update(11.1, 0.0, rules);
    CHECK_NEAR(done.x, 3.6, 1e-3);  // on the new path after the blend
    CHECK_NEAR(done.y, 6.3, 1e-3);
}

ARENA_TEST(remote_motion_matches_the_ai_replay) {
    // The AI judges hits on what clients draw (server/arena/motion.py).
    // Reference values from that Python code for the same samples, drawn
    // 60 ms in the past: a start, a turn, a late dash, a release, a press.
    struct Sample {
        double t;
        float x, y, vx, vy;
        double arrival;
        float ix, iy, hold;
    };
    const Sample samples[] = {{10.00, 0.0f, 0.0f, 0.0f, 0.0f, 10.03, 1.0f, 0.0f, 0.0f},
                              {10.20, 0.84f, 0.0f, 8.4f, 0.0f, 10.23, 0.0f, 1.0f, 0.0f},
                              {10.30, 1.6f, 0.4f, 0.0f, 24.0f, 10.45, 0.0f, 1.0f, 0.225f},
                              {10.55, 1.6f, 6.3f, 0.0f, 21.0f, 10.58, 0.0f, 0.0f, 0.0f},
                              {10.70, 1.6f, 8.2f, 0.0f, 5.0f, 10.72, -1.0f, 0.0f, 0.0f}};
    const double expected[][3] = {
        {10.03, 0.000000, 0.000000}, {10.08, 0.011900, 0.000000}, {10.20, 0.436100, 0.000000},
        {10.26, 0.875000, 0.000000}, {10.30, 1.170978, 0.029681}, {10.40, 1.718441, 0.318813},
        {10.45, 1.896047, 0.578521}, {10.50, 1.881709, 1.874488}, {10.55, 1.844279, 3.325013},
        {10.60, 1.792134, 4.855648}, {10.65, 1.733888, 6.193673}, {10.72, 1.658159, 7.565262},
        {10.80, 1.571006, 8.342587}, {10.90, 1.218780, 8.688211}, {11.20, -1.294397, 8.792002}};
    const double delay = 0.06;
    Rules rules;
    RemoteMotion m;
    size_t next = 0;
    for (const auto &e : expected) {
        while (next < std::size(samples) && samples[next].arrival <= e[0] + 1e-9) {
            const Sample &s = samples[next++];
            m.addSample(MotionSample{s.t, s.x, s.y, s.vx, s.vy, s.ix, s.iy, s.hold}, s.arrival, delay, rules);
        }
        const RemoteMotion::Pose p = m.update(e[0], delay, rules);
        CHECK_NEAR(p.x, e[1], 1e-3);
        CHECK_NEAR(p.y, e[2], 1e-3);
    }
}

ARENA_TEST(remote_motion_snaps_only_on_teleports_and_stops_predicting) {
    Rules rules;
    RemoteMotion m;
    m.addSample(MotionSample{0.0, 0.0f, 0.0f, 5.0f, 0.0f, 1.0f, 0.0f, 0.0f}, 0.0, 0.0, rules);
    // 19.5 m away: a respawn, not motion.
    m.addSample(MotionSample{0.1, 20.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}, 0.1, 0.0, rules);
    CHECK_NEAR(m.update(0.1, 0.0, rules).x, 20.0, 1e-4);
    m.addSample(MotionSample{1.0, 0.0f, 0.0f, 9.0f, 0.0f, 1.0f, 0.0f, 0.0f}, 1.0, 0.0, rules);
    CHECK_NEAR(m.update(10.0, 0.0, rules).x, 9.0 * RemoteMotion::kMaxExtrapolation, 1e-3);
    CHECK(m.update(10.0, 0.0, rules).vx == 0.0f);
}

ARENA_TEST(view_delay_follows_lateness_slowly) {
    ViewDelay d;
    for (int i = 0; i < 40; ++i) d.addLateness(0.08);
    for (int i = 0; i < 8; ++i) d.addLateness(0.4);  // 1 in 6 very late: covered, up to kMax
    CHECK_NEAR(d.target(), ViewDelay::kMax, 1e-6);
    for (int i = 0; i < 44; ++i) d.addLateness(0.08);  // 1 in 12: an outlier, ignored
    CHECK_NEAR(d.target(), 0.09, 1e-6);
    const double before = d.value();
    d.update(0.5);
    CHECK_NEAR(d.value(), before + 0.5 * ViewDelay::kRate, 1e-9);  // never faster than kRate
    for (int i = 0; i < 100; ++i) d.update(0.1);
    CHECK_NEAR(d.value(), 0.09, 1e-6);
    CHECK(d.wire() == 22);  // 4 ms units
    for (int i = 0; i < 100; ++i) d.addLateness(0.0);
    CHECK_NEAR(d.target(), ViewDelay::kMin, 1e-9);
}

ARENA_TEST(turn_toward_spins_a_reversal_smoothly) {
    float h = 0.0f, maxStep = 0.0f;
    double t = 0.0;
    for (; t < 1.0 && std::fabs(std::remainder(h - 180.0f, 360.0f)) > 10.0f; t += kFrame) {
        const float next = turnToward(h, 180.0f, float(kFrame));
        maxStep = std::max(maxStep, std::fabs(std::remainder(next - h, 360.0f)));
        h = next;
    }
    CHECK(maxStep <= kMaxTurnRate * float(kFrame) + 1e-3f);
    CHECK(t > 0.15 && t < 0.35);  // a quick spin, not a snap
    CHECK_NEAR(turnToward(170.0f, -170.0f, 1.0f), -170.0, 1e-3);  // the short way round
}

ARENA_TEST(protocol_wire_state_matches_python) {
    // Bytes from server/arena/protocol.py packState() with the same arguments.
    struct Case {
        float x, y, vx, vy;
        uint8_t flags;
        uint16_t t;
        float ix, iy;
        const char *hex;
    };
    const Case cases[] = {
        {1.234f, -5.0f, 8.9f, -24.1f, 1, 40000, 0, 0, "7b000cfe24a00100409c"},
        {-327.68f, 327.67f, -32.0f, 31.75f, 0, 65535, 0, 0, "0080ff7f807f0000ffff"},
        {0.005f, -0.005f, 0.125f, -0.125f, 0, 0, 0, 0, "00000000000000000000"},  // halves to even
        {999.0f, -999.0f, 99.0f, -99.0f, 1, 1, 0, 0, "ff7f00807f8001000100"},    // clamped
        {2.5f, 1.0f, 9.0f, 0.0f, 1 | 30 << 2, 1234, 0.6f, -0.8f, "fa00640024007bdad204"},  // input
        {0.0f, 0.0f, 0.0f, 0.0f, 63 << 2, 7, -1.0f, 0.0f, "000000000000fe800700"},
    };
    for (const Case &c : cases) {
        uint8_t b[WireState::kSize];
        const WireState w = WireState::make(c.x, c.y, c.vx, c.vy, c.flags, c.t, c.ix, c.iy);
        w.encode(b);
        char hex[2 * WireState::kSize + 1];
        for (size_t i = 0; i < WireState::kSize; ++i) std::snprintf(hex + 2 * i, 3, "%02x", b[i]);
        CHECK(std::string(hex) == c.hex);
        const WireState d = WireState::decode(b);
        CHECK(d.x == w.x && d.y == w.y && d.vx == w.vx && d.vy == w.vy && d.flags == w.flags &&
              d.dir == w.dir && d.t == w.t);
        CHECK_NEAR(d.inputX(), c.ix, 0.02);
        CHECK_NEAR(d.inputY(), c.iy, 0.02);
    }
}
