#include "game/ArenaPlayer.h"

#include "game/GameEvents.h"
#include "net/ClientRepository.h"

#include "clockObject.h"

#include <algorithm>
#include <cmath>

namespace arena::game {

namespace {
net::FieldTable<ArenaPlayer> g_fields;

constexpr float kRadToDeg = 57.29577951308232f;

double frameTime() { return ClockObject::get_global_clock()->get_frame_time(); }
} // namespace

ArenaPlayer::ArenaPlayer(net::ClientRepository &cr, GameEvents &events) : DistributedObject(cr), events_(events) {}

bool ArenaPlayer::bindFields(const DCClass &dclass) {
    g_fields.bind(dclass, "setName", &ArenaPlayer::setName, &ArenaPlayer::packName);
    g_fields.bind(dclass, "setColorIndex", &ArenaPlayer::setColorIndex, &ArenaPlayer::packColorIndex);
    return g_fields.missing().empty();
}

std::string ArenaPlayer::sanitizeName(const std::string &raw) {
    std::string out;
    for (unsigned char c : raw) {
        if (c >= 0x20 && c != 0x7f) {
            out.push_back(static_cast<char>(c));
        }
    }
    const size_t first = out.find_first_not_of(' ');
    const size_t last = out.find_last_not_of(' ');
    out = first == std::string::npos ? std::string() : out.substr(first, last - first + 1);
    // string(1-16) in arena.dc counts bytes; cut on a UTF-8 boundary.
    if (out.size() > 16) {
        size_t cut = 16;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        out.resize(cut);
    }
    return out.empty() ? std::string("Player") : out;
}

void ArenaPlayer::receiveField(const DCField &field, net::FieldReader &in) {
    g_fields.receive(*this, field, in);
}

bool ArenaPlayer::packField(const DCField &field, net::FieldWriter &out) const {
    return g_fields.pack(*this, field, out);
}

void ArenaPlayer::announceGenerate() { events_.onPlayerGenerated(*this); }

void ArenaPlayer::disable() { events_.onPlayerDisabled(*this); }

void ArenaPlayer::setName(net::FieldReader &in) { name_ = sanitizeName(in.str()); }

void ArenaPlayer::setColorIndex(net::FieldReader &in) {
    colorIndex_ = std::clamp(static_cast<int>(in.u32()), 0, kNumColors - 1);
}

void ArenaPlayer::packName(net::FieldWriter &out) const { out.str(name_); }
void ArenaPlayer::packColorIndex(net::FieldWriter &out) const { out.u32(static_cast<uint32_t>(colorIndex_)); }

// ------------------------------------------------------------------ local

void ArenaPlayer::setIdentity(const std::string &name, int colorIndex) {
    name_ = sanitizeName(name);
    colorIndex_ = std::clamp(colorIndex, 0, kNumColors - 1);
}

void ArenaPlayer::setLocalPose(float x, float y, float h, float vx, float vy) {
    x_ = x;
    y_ = y;
    h_ = h;
    vx_ = vx;
    vy_ = vy;
}

void ArenaPlayer::setLocalInput(float ix, float iy, float dashLeft) {
    ix_ = ix;
    iy_ = iy;
    dashLeft_ = dashLeft;
}

void ArenaPlayer::broadcastPose(double now, uint8_t viewDelay, const Rules &rules, bool force) {
    if (!isLocal() || !generated()) [[unlikely]] {
        return;
    }
    const MotionSample real{now, x_, y_, vx_, vy_, ix_, iy_, heldFor(dashLeft_)};
    if (!force && !sendNow_ && !reckoning_.due(real, rules)) [[likely]] {
        return;
    }
    // A dash sample is rewound to the dash's start (the velocity was held
    // since), so receivers know exactly when the dash ends.
    double when = now;
    float x = x_, y = y_;
    if (dashPending_ && dashLeft_ > 0.0f) {
        const float since = std::max(0.0f, dashHoldTime(rules) - heldFor(dashLeft_));
        when -= since;
        x -= vx_ * since;
        y -= vy_ * since;
    }
    const auto t = static_cast<uint16_t>(cr_.clockDelta().localToNetworkTime(when, 16));
    const auto flags = static_cast<uint8_t>((dashPending_ ? net::kFlagDash : 0) |
                                            (viewDelay & 0x3f) << net::kViewDelayShift);
    const net::WireState s = net::WireState::make(x, y, vx_, vy_, flags, t, ix_, iy_);
    if (cr_.sendMove(s)) {
        reckoning_.sent(when, s, rules);
        ++positionSends_;
        sendNow_ = false;
        dashPending_ = false;
    }
}

void ArenaPlayer::sendDash() {
    lastDash_ = frameTime();
    dashPending_ = true;
    sendNow_ = true;
}

// ----------------------------------------------------------------- remote

double ArenaPlayer::applyState(const net::WireState &state, double now, double delay, const Rules &rules) {
    double sampleTime = now, lateness = -1.0;
    const net::ClockDelta &clock = cr_.clockDelta();
    if (clock.synced()) [[likely]] {
        // Never trust a sample from the future, or a stale one, blindly.
        sampleTime = std::clamp(clock.networkToLocalTime(state.t, now, 16), now - 2.0, now);
        lateness = now - sampleTime;
    }
    const bool first = !motion_.hasSample();
    motion_.addSample(DeadReckoning::fromWire(state, sampleTime, rules), now, delay, rules);
    if (state.flags & net::kFlagDash) {
        lastDash_ = now;
        if (generated()) events_.onPlayerDashed(*this);
    }
    if (first) {
        const float fx = state.hasInput() ? state.inputX() : state.velX();
        const float fy = state.hasInput() ? state.inputY() : state.velY();
        if (fx != 0.0f || fy != 0.0f) h_ = std::atan2(-fx, fy) * kRadToDeg;
    }
    updateSmoothing(now, 0.0f, delay, rules);
    return lateness;
}

void ArenaPlayer::updateSmoothing(double now, float dt, double delay, const Rules &rules) {
    if (isLocal() || !motion_.hasSample()) {
        return;
    }
    const RemoteMotion::Pose p = motion_.update(now, delay, rules);
    x_ = p.x;
    y_ = p.y;
    vx_ = p.vx;
    vy_ = p.vy;
    // Players face where they move (Movement.cpp); heading is not sent. The
    // drawn velocity turns smoothly, and the body follows it like ours does.
    if (p.vx * p.vx + p.vy * p.vy > 0.25f) {
        h_ = turnToward(h_, std::atan2(-p.vx, p.vy) * kRadToDeg, dt);
    }
}

} // namespace arena::game
