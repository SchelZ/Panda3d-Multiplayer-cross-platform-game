#include "game/Prediction.h"

#include "game/RemoteMotion.h"

#include <cmath>
#include <cstdlib>
#include <limits>

namespace arena::game {

namespace {

constexpr int64_t kNoRemap = std::numeric_limits<int64_t>::min();

bool close(const MoveState &a, const MoveState &b) {
    return std::hypot(a.x - b.x, a.y - b.y) <= Prediction::kPositionTolerance &&
           std::hypot(a.vx - b.vx, a.vy - b.vy) <= Prediction::kVelocityTolerance &&
           std::fabs(a.dashLeft - b.dashLeft) <= Prediction::kTimerTolerance &&
           std::fabs(a.dashCooldown - b.dashCooldown) <= Prediction::kTimerTolerance &&
           std::fabs(a.stun - b.stun) <= Prediction::kTimerTolerance;
}

} // namespace

void Prediction::start(int64_t step, const MoveState &s, int knowDelay) {
    started_ = true;
    step_ = step;
    know_ = knowBefore_ = knowDelay;
    knowFrom_ = step;
    state_ = previous_ = s;
    record();
}

void Prediction::record() { history_[static_cast<size_t>(step_ % kHistory)] = state_; }

const MoveState *Prediction::predictedAt(int64_t step) const {
    if (!started_ || step > step_ || step <= step_ - kHistory) return nullptr;
    return &history_[static_cast<size_t>(step % kHistory)];
}

bool Prediction::setInput(bool held, uint8_t dir, bool dash, double now, uint8_t viewDelay, net::InputCmd &out) {
    const bool wasHeld = this->held();
    bool send = dash || held != wasHeld;
    if (!send && held) {
        const int turn = std::abs(static_cast<int8_t>(static_cast<uint8_t>(dir - this->dir())));
        send = turn > kTurnDeadband && now - lastSend_ >= kTurnInterval;
    }
    if (!send) {
        return false;
    }
    const int64_t step = step_ + 1;
    sent_.push_back(Sent{++sentCount_, step, held, held ? dir : uint8_t(0), dash});
    while (sent_.size() >= 2 && sent_[1].step <= step_ - kHistory) {
        sent_.pop_front();  // the one before stays: it is in effect at the oldest step kept
    }
    lastSend_ = now;
    out.seq = static_cast<uint8_t>(sentCount_ & 0xff);
    out.step = static_cast<uint16_t>(step & 0xffff);
    out.flags = static_cast<uint8_t>((dash ? net::kInputDash : 0) | (held ? net::kInputHeld : 0) |
                                     (viewDelay & 0x3f) << net::kViewDelayShift);
    out.dir = held ? dir : 0;
    return true;
}

bool Prediction::simulate(int64_t k, const std::deque<Sent> &sent, int64_t lateFrom, int64_t ackN,
                          const Rules &rules, const ArenaLayout &arena, const Others &others) {
    // The input in effect at step k: the newest one from k or before. Inputs
    // the AI had not received by step `lateFrom` (it simulated that step
    // without them) will be applied by the AI at the next step it simulates.
    bool held = false, dash = false, found = false;
    uint8_t dir = 0;
    for (auto it = sent.rbegin(); it != sent.rend(); ++it) {
        const int64_t from = it->n > ackN && it->step <= lateFrom ? lateFrom + 1 : it->step;
        if (from > k) continue;
        if (!found) {
            found = true;
            held = it->held;
            dir = it->dir;
        }
        if (from < k) break;
        dash = dash || it->dash;
    }
    const MoveInput in{held ? net::dirX(dir) : 0.0f, held ? net::dirY(dir) : 0.0f, dash};
    const bool dashed = stepMovement(state_, in, rules, arena, static_cast<float>(kPhysicsStep));
    // Soft collisions with the other players where they are at this step.
    bool contact = false;
    const float minDist = 2.0f * rules.playerRadius;
    const double t = static_cast<double>(k) / net::kStepsPerSec;
    const int64_t cutoff = k - knowDelayAt(k);
    // Not while dashing or knocked back: the player flies through.
    const bool free = state_.dashLeft <= kTimerEps && state_.stun <= kTimerEps;
    for (const Other &o : others) {
        if (!free) break;
        int i = o.count - 1;
        while (i >= 0 && o.samples[i].sentStep > cutoff) --i;
        if (i < 0) continue;  // too new to count yet
        const MotionPose p = RemoteMotion::predict(o.samples[i].s, t, rules);
        const float ddx = state_.x - p.x, ddy = state_.y - p.y;
        if (ddx * ddx + ddy * ddy < minDist * minDist) {
            separateFrom(state_, p.x, p.y, minDist);
            contact = true;
        }
    }
    if (contact) resolveCollisions(state_, rules, arena);
    return dashed;
}

bool Prediction::advance(const Rules &rules, const ArenaLayout &arena, const Others &others) {
    previous_ = state_;
    const int64_t k = step_ + 1;
    const bool dashed = simulate(k, sent_, kNoRemap, sentCount_, rules, arena, others);
    step_ = k;
    record();
    return dashed;
}

bool Prediction::reconcile(const net::SelfState &msg, int64_t msgStep, const Rules &rules, const ArenaLayout &arena,
                           const Others &others, float &dx, float &dy) {
    dx = dy = 0.0f;
    const MoveState theirs{msg.x, msg.y, msg.vx, msg.vy, msg.heading, msg.dashLeft, msg.dashCooldown, msg.stun};
    if (!started_ || msgStep > step_ || msgStep <= step_ - kHistory) {
        // Nothing to compare with (the spawn, or we lost track): start there.
        const float oldX = state_.x, oldY = state_.y;
        const bool was = started_;
        start(msgStep, theirs, msg.knowDelay);
        if (was) {
            dx = state_.x - oldX;
            dy = state_.y - oldY;
        }
        return true;
    }
    // Our newest input the AI had applied, from the low 8 bits it echoes.
    const int64_t ackN = sentCount_ - ((sentCount_ - msg.ack) & 0xff);
    bool stale = false;
    for (const Sent &s : sent_) {
        stale = stale || (s.n > ackN && s.step <= msgStep);
    }
    const MoveState *mine = predictedAt(msgStep);
    const bool event = (msg.flags & (net::kSelfKnock | net::kSelfSnap)) != 0;
    lastStale_ = stale;
    // A new knowDelay holds from the next step on, here as at the AI.
    const bool newRule = msg.knowDelay != know_;
    if (newRule) {
        knowBefore_ = know_;
        know_ = msg.knowDelay;
        knowFrom_ = msgStep + 1;
    }
    const bool predicted = !stale && !event && mine != nullptr && close(*mine, theirs);
    // Either way, continue from exactly the AI's state (it rounded it to what
    // it sent) and replay our inputs since, by its rule: then rounding never
    // adds up. Cheap: a few dozen steps, a couple of times a second.
    const float oldX = state_.x, oldY = state_.y;
    const int64_t now = step_;
    state_ = previous_ = theirs;
    step_ = msgStep;
    record();
    while (step_ < now) {
        previous_ = state_;
        const int64_t k = step_ + 1;
        simulate(k, sent_, msgStep, ackN, rules, arena, others);
        step_ = k;
        record();
    }
    dx = state_.x - oldX;
    dy = state_.y - oldY;
    if (!event && !predicted) ++corrections_;
    return !predicted || newRule;
}

} // namespace arena::game
