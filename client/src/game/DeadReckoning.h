// Dead reckoning for the local player's movement samples.
//
// Every sample carries position, velocity and the held input, and the other
// clients predict our motion from it exactly as the movement code would
// (predictMotion): accelerating, braking, dashing. The sender runs the same
// prediction and only sends when it would go wrong: when it is already more
// than 12 cm or 1 m/s off, or would be within the next 0.15 s (we changed the
// input: pressed, released or turned). So a tap costs two samples, straight
// runs a heartbeat every 0.5 s, standing still nothing, and every change goes
// out at once (at most `minInterval` apart).
#pragma once

#include "game/Movement.h"
#include "net/Protocol.h"

namespace arena::game {

class DeadReckoning {
public:
    struct Settings {
        double minInterval = 0.05;       // s: at most 20 samples per second
        double maxInterval = 0.5;        // s: at least this often while moving
        float maxPositionError = 0.12f;  // m the others' prediction may be off
        float maxVelocityError = 1.0f;   // m/s
        double lookahead = 0.15;         // s: changes that would show by then go now
        double maxExtrapolation = 1.0;   // RemoteMotion::kMaxExtrapolation
    };

    DeadReckoning() = default;
    explicit DeadReckoning(const Settings &settings) : s_(settings) {}

    // What the others predict for us at time t (from the last sample sent).
    MotionPose predict(double t, const Rules &rules) const;

    // True if a sample should go out now for our real state (real.t = now;
    // real.hold = the rest of a dash).
    bool due(const MotionSample &real, const Rules &rules) const;

    // Records a sample as sent, quantised as the wire carries it.
    void sent(double now, const net::WireState &state, const Rules &rules);

    // A wire sample as receivers read it (time `t` on their clock). A dash
    // sample describes the moment the dash started: its velocity is held for
    // dashHoldTime() from then.
    static MotionSample fromWire(const net::WireState &state, double t, const Rules &rules);

    double lastSent() const { return last_; }
    const Settings &settings() const { return s_; }

private:
    Settings s_;
    bool any_ = false;
    double last_ = -1e9;
    MotionSample sample_;
};

} // namespace arena::game
