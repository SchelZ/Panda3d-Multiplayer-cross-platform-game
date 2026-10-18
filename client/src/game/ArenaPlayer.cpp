#include "game/ArenaPlayer.h"

#include "game/GameEvents.h"
#include "net/ClientRepository.h"

#include <algorithm>
#include <cmath>

namespace arena::game {

namespace {
net::FieldTable<ArenaPlayer> g_fields;

constexpr float kRadToDeg = 57.29577951308232f;

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

// ------------------------------------------------------------------ ours

void ArenaPlayer::setLocalPose(float x, float y, float h) {
    x_ = x;
    y_ = y;
    h_ = h;
}

// ----------------------------------------------------------------- others

double ArenaPlayer::applyState(const net::WireState &state, uint16_t sentStep, double now, double delay,
                               const Rules &rules) {
    double sampleTime = now, lateness = -1.0;
    const net::ClockDelta &clock = cr_.clockDelta();
    if (clock.synced()) [[likely]] {
        // Never trust a sample from the future, or a stale one, blindly.
        sampleTime = std::clamp(clock.stepToLocalTime(clock.resolveStep(sentStep, now) - state.age), now - 2.0, now);
        lateness = now - sampleTime;
    }
    const MotionSample sample = DeadReckoning::fromWire(state, sampleTime, rules);
    motion_.addSample(sample, now, delay, rules);
    if (knownCount_ == kKnown) {
        std::copy(known_.begin() + 1, known_.end(), known_.begin());
        --knownCount_;
    }
    KnownSample &k = known_[knownCount_++];
    k.s = sample;
    k.s.t -= clock.delta();
    k.sentStep = clock.resolveStep(sentStep, now);
    if (state.flags & net::kFlagDash) {
        lastDash_ = now;
        if (generated()) events_.onPlayerDashed(*this);
    }
    facing_ = state.headingDeg();
    if (!hasHeading_) {
        hasHeading_ = true;
        h_ = facing_;  // the AI's: everyone shows a new player facing the same way
    }
    updateSmoothing(now, 0.0f, delay, rules);
    return lateness;
}

void ArenaPlayer::updateSmoothing(double now, float dt, double delay, const Rules &rules) {
    if (controlled_ || !motion_.hasSample()) {
        return;
    }
    const RemoteMotion::Pose p = motion_.update(now, delay, rules);
    x_ = p.x;
    y_ = p.y;
    // Bodies face where they move (Movement.cpp). The drawn velocity turns
    // smoothly, and the body follows it like ours does; standing, it turns
    // to where the AI says the player faces.
    const float target = p.vx * p.vx + p.vy * p.vy > 0.25f ? std::atan2(-p.vx, p.vy) * kRadToDeg : facing_;
    h_ = turnToward(h_, target, dt);
}

} // namespace arena::game
