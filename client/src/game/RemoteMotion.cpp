#include "game/RemoteMotion.h"

#include <algorithm>
#include <cmath>

namespace arena::game {

RemoteMotion::Pose RemoteMotion::predict(const MotionSample &s, double r, const Rules &rules) {
    const double age = r - s.t;
    if (age < kMaxExtrapolation) [[likely]] {
        return predictMotion(s, age, rules);
    }
    Pose p = predictMotion(s, kMaxExtrapolation, rules);  // no news for too long: stop
    p.vx = p.vy = 0.0f;
    return p;
}

RemoteMotion::Pose RemoteMotion::path(double r, const Rules &rules) const {
    Pose p = predict(to_, r, rules);
    if (r < b_ && b_ > a_) {
        // Cubic Hermite fade of the offset: from (o, rate) at a_ to (0, 0) at b_.
        const double span = b_ - a_;
        const double u = std::clamp((r - a_) / span, 0.0, 1.0);
        const double u2 = u * u, u3 = u2 * u;
        const double h00 = 2.0 * u3 - 3.0 * u2 + 1.0, h10 = (u3 - 2.0 * u2 + u) * span;
        const double d00 = (6.0 * u2 - 6.0 * u) / span, d10 = 3.0 * u2 - 4.0 * u + 1.0;
        p.x += static_cast<float>(h00 * ox_ + h10 * rx_);
        p.y += static_cast<float>(h00 * oy_ + h10 * ry_);
        p.vx += static_cast<float>(d00 * ox_ + d10 * rx_);
        p.vy += static_cast<float>(d00 * oy_ + d10 * ry_);
    }
    return p;
}

void RemoteMotion::activate(const MotionSample &s, double at, const Rules &rules) {
    if (!has_) [[unlikely]] {
        has_ = true;
        to_ = s;
        a_ = b_ = at;
        return;
    }
    // Where we draw the player at that moment, and where the sample says.
    const Pose drawn = path(at, rules);
    const Pose before = predict(to_, at, rules);
    const Pose target = predict(s, at, rules);
    to_ = s;
    const float ox = drawn.x - target.x, oy = drawn.y - target.y;
    const float err = std::hypot(ox, oy);
    if (err > kSnapDistance || err < 1e-5f) [[unlikely]] {
        a_ = b_ = at;  // a teleport: show it as one (or nothing to blend)
        return;
    }
    a_ = at;
    // A smoothstep fade moves at most 1.5x its average speed (mid-way).
    b_ = at + std::max(kBlendTime, 1.5 * err / kMaxCorrectionSpeed);
    ox_ = ox;
    oy_ = oy;
    // Keep the offset's motion so far (not the player's: a dash or a stop is
    // shown at once), within limits so the fade never swings out.
    float rx = drawn.vx - before.vx, ry = drawn.vy - before.vy;
    const float rate = std::hypot(rx, ry), limit = kRateLimit * err / static_cast<float>(b_ - a_);
    if (rate > limit) {
        rx *= limit / rate;
        ry *= limit / rate;
    }
    rx_ = rx;
    ry_ = ry;
}

void RemoteMotion::addSample(MotionSample s, double now, double delay, const Rules &rules) {
    s.t = std::max(s.t, lastT_ + 1e-3);  // strictly increasing, as the AI keeps them
    lastT_ = s.t;
    const double at = std::max({s.t, now - delay, lastAt_});
    lastAt_ = at;
    if (!has_) [[unlikely]] {
        activate(s, at, rules);  // the first one is drawn at once
        return;
    }
    if (pendingCount_ == kMaxPending) [[unlikely]] {
        activate(pending_[0].s, pending_[0].at, rules);
        std::copy(pending_.begin() + 1, pending_.end(), pending_.begin());
        --pendingCount_;
    }
    pending_[pendingCount_++] = Pending{s, at};
}

RemoteMotion::Pose RemoteMotion::update(double now, double delay, const Rules &rules) {
    const double r = now - delay;
    int done = 0;
    while (done < pendingCount_ && pending_[done].at <= r) {
        activate(pending_[done].s, pending_[done].at, rules);
        ++done;
    }
    if (done > 0) {
        std::copy(pending_.begin() + done, pending_.begin() + pendingCount_, pending_.begin());
        pendingCount_ -= done;
    }
    return path(r, rules);
}

// ------------------------------------------------------------- ViewDelay

void ViewDelay::addLateness(double lateness) {
    window_[next_] = static_cast<float>(lateness);
    next_ = (next_ + 1) % kWindow;
    count_ = std::min(count_ + 1, kWindow);
    std::array<float, kWindow> sorted;
    std::copy_n(window_.begin(), count_, sorted.begin());
    const int i = (count_ - 1) * 9 / 10;
    std::nth_element(sorted.begin(), sorted.begin() + i, sorted.begin() + count_);
    target_ = std::clamp(double(sorted[i]) + kMargin, kMin, kMax);
}

void ViewDelay::update(double dt) {
    const double step = kRate * dt;
    value_ += std::clamp(target_ - value_, -step, step);
}

uint8_t ViewDelay::wire() const {
    return static_cast<uint8_t>(std::clamp<long>(std::lround(value_ / kUnit), 0, 63));
}

float turnToward(float heading, float target, float dt) {
    const float diff = std::remainder(target - heading, 360.0f);
    const float maxStep = kMaxTurnRate * dt;
    const float step = std::clamp(diff * (1.0f - std::exp(-dt / kTurnTime)), -maxStep, maxStep);
    return std::remainder(heading + step, 360.0f);
}

} // namespace arena::game
