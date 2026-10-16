#include "game/DeadReckoning.h"

#include <algorithm>
#include <cmath>

namespace arena::game {

MotionSample DeadReckoning::fromWire(const net::WireState &state, double t, const Rules &rules) {
    MotionSample s;
    s.t = t;
    s.x = state.posX();
    s.y = state.posY();
    s.vx = state.velX();
    s.vy = state.velY();
    s.ix = state.inputX();
    s.iy = state.inputY();
    s.hold = (state.flags & net::kFlagDash) ? dashHoldTime(rules) : 0.0f;
    return s;
}

MotionPose DeadReckoning::predict(double t, const Rules &rules) const {
    const double age = t - sample_.t;
    if (age < s_.maxExtrapolation) {
        return predictMotion(sample_, age, rules);
    }
    MotionPose p = predictMotion(sample_, s_.maxExtrapolation, rules);
    p.vx = p.vy = 0.0f;
    return p;
}

bool DeadReckoning::due(const MotionSample &real, const Rules &rules) const {
    if (!any_) [[unlikely]] {
        return true;
    }
    const double since = real.t - last_;
    if (since < s_.minInterval) {
        return false;
    }
    const MotionPose p = predict(real.t, rules);
    if (std::hypot(real.x - p.x, real.y - p.y) > s_.maxPositionError ||
        std::hypot(real.vx - p.vx, real.vy - p.vy) > s_.maxVelocityError) {
        return true;
    }
    // Where the others will draw us a moment from now, and where we will be
    // if we keep doing what we do: a new input shows here at once.
    const MotionPose ahead = predict(real.t + s_.lookahead, rules);
    const MotionPose real2 = predictMotion(real, s_.lookahead, rules);
    if (std::hypot(real2.x - ahead.x, real2.y - ahead.y) > s_.maxPositionError) {
        return true;
    }
    const bool moving = real.vx != 0.0f || real.vy != 0.0f || real.ix != 0.0f || real.iy != 0.0f;
    return moving && since >= s_.maxInterval;
}

void DeadReckoning::sent(double now, const net::WireState &state, const Rules &rules) {
    any_ = true;
    last_ = now;
    sample_ = fromWire(state, now, rules);
}

} // namespace arena::game
