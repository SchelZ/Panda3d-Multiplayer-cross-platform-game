// How a remote player is drawn.
//
// Every movement sample carries the player's position, velocity and held
// input at the sender's time t, so from a sample on the player's motion is
// predicted exactly (predictMotion: accelerating, braking, dashing) until the
// input changes, which makes the sender send the next sample.
//
// Remote players are drawn a little in the past, at r = now - delay, where
// the delay (ViewDelay) is about how late samples arrive. A sample takes over
// the drawing at its own time t when it arrived in time, so a stop, a turn or
// a dash is drawn exactly when and where it happened: no overshoot, no
// sliding back. A sample that arrives later than that takes over at once.
// Either way the drawing never jumps: the position error left at the switch
// (small when in time) becomes an offset that fades out smoothly (a cubic
// with no kick at either end) within kBlendTime, or longer for big errors so
// corrections stay under kMaxCorrectionSpeed. Heading follows the drawn
// velocity through turnToward(). Errors over kSnapDistance (respawns) snap.
//
// The AI replays exactly this (server/arena/motion.py) to judge hits on the
// attacker's screen; each client's delay travels in its own samples.
#pragma once

#include "game/Movement.h"

#include <array>
#include <cstdint>

namespace arena::game {

class RemoteMotion {
public:
    static constexpr double kBlendTime = 0.1;          // s: shortest blend onto a new sample
    static constexpr float kMaxCorrectionSpeed = 8.0f; // m/s: longer blends for bigger errors
    static constexpr double kMaxExtrapolation = 1.0;   // s of prediction past the newest sample
    static constexpr float kSnapDistance = 3.0f;       // m
    static constexpr float kRateLimit = 3.0f;          // offset's initial rate <= 3 x offset / blend
    static constexpr int kMaxPending = 8;              // samples waiting for their time

    using Pose = MotionPose;

    bool hasSample() const { return has_; }

    // A sample that arrived at `now` while drawing `delay` in the past (its
    // time s.t already converted to our clock).
    void addSample(MotionSample s, double now, double delay, const Rules &rules);

    // Lets samples whose time has come take over; returns what to draw at `now`.
    Pose update(double now, double delay, const Rules &rules);

    // The drawn path at render time r, from the samples that took over.
    Pose path(double r, const Rules &rules) const;

    // Sample s predicted to time r (at most kMaxExtrapolation; then it stops).
    static Pose predict(const MotionSample &s, double r, const Rules &rules);

private:
    void activate(const MotionSample &s, double at, const Rules &rules);

    struct Pending {
        MotionSample s;
        double at;  // render time it takes over: max(s.t, arrival - delay)
    };
    std::array<Pending, kMaxPending> pending_{};
    int pendingCount_ = 0;

    bool has_ = false;
    MotionSample to_;              // the sample in charge
    double a_ = 0.0, b_ = 0.0;     // its offset fades over render times [a_, b_]
    float ox_ = 0.0f, oy_ = 0.0f;  // offset at a_: drawn - prediction
    float rx_ = 0.0f, ry_ = 0.0f;  // the offset's rate at a_ (m/s), kept from before
    double lastT_ = -1e18, lastAt_ = -1e18;
};

// How far in the past remote players are drawn: about the 90th percentile of
// how late their samples arrive, so most turns and stops are known before
// they are drawn. It changes slowly (kRate), which is invisible, and goes out
// in every movement sample (4 ms units) so the AI can replay what we saw.
class ViewDelay {
public:
    static constexpr double kMin = 0.02, kMax = 0.25;  // s
    static constexpr double kMargin = 0.01;           // s above the percentile
    static constexpr double kRate = 0.05;             // s of change per second
    static constexpr double kUnit = 0.004;            // s per wire unit
    static constexpr int kWindow = 48;                // lateness samples kept

    void addLateness(double lateness);
    void update(double dt);
    double value() const { return value_; }
    double target() const { return target_; }
    uint8_t wire() const;  // 0..63, for WireState::flags bits 2-7

private:
    std::array<float, kWindow> window_{};
    int count_ = 0, next_ = 0;
    double value_ = 0.06, target_ = 0.06;
};

// Turns a drawn heading toward `target` (degrees) like a body would: quickly,
// but never faster than kMaxTurnRate, so reversals are a short spin.
float turnToward(float heading, float target, float dt);
constexpr float kTurnTime = 0.06f;       // s: exponential approach
constexpr float kMaxTurnRate = 900.0f;   // degrees per second

} // namespace arena::game
