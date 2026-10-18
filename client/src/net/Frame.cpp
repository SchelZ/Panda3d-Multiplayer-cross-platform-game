#include "net/Frame.h"

#include <algorithm>
#include <cmath>

namespace arena::net {

namespace {

// frame.py
constexpr int kVelWidths[4] = {3, 5, 7, 9};
constexpr int kPosWidths[4] = {4, 6, 9, 18};
constexpr int kHeadingSpeedSq = 64;
constexpr int32_t kMaxPredictSteps = 240;
constexpr int kSentDeltaBits = 4;
constexpr uint32_t kSelfAgeEscape = 255;
constexpr size_t kMaxSamples = 256;
constexpr size_t kMaxEvents = 1024;

constexpr double kPi = 3.14159265358979323846;

} // namespace

uint32_t BitReader::get(int bits) {
    if (pos_ + static_cast<size_t>(bits) > bits_) [[unlikely]] {
        ok_ = false;
        pos_ = bits_;
        return 0;
    }
    uint32_t value = 0;
    for (int done = 0; done < bits;) {
        const size_t byte = pos_ >> 3;
        const int shift = static_cast<int>(pos_ & 7);
        const int take = std::min(8 - shift, bits - done);
        const uint32_t chunk = (static_cast<uint32_t>(data_[byte]) >> shift) & ((1u << take) - 1u);
        value |= chunk << done;
        done += take;
        pos_ += static_cast<size_t>(take);
    }
    return value;
}

int32_t BitReader::getSigned(int bits) {
    const uint32_t value = get(bits);
    const uint32_t sign = 1u << (bits - 1);
    return static_cast<int32_t>(value ^ sign) - static_cast<int32_t>(sign);
}

bool BitReader::atEnd() const {
    if (bits_ - pos_ >= 8) {
        return false;
    }
    return pos_ == bits_ || (data_[pos_ >> 3] >> (pos_ & 7)) == 0;
}

int32_t predictPos(int32_t p, int32_t pv, int32_t v, int32_t dt) {
    dt = std::clamp(dt, -kMaxPredictSteps, kMaxPredictSteps);
    const int32_t n = (pv + v) * 25 * dt + 120;  // 25 cm/s per unit, 2 x 120 steps/s
    return p + (n >= 0 ? n / 240 : -((-n + 239) / 240));  // floor, as Python's //
}

uint8_t velocityHeading(int vx, int vy) {
    const double a = std::atan2(static_cast<double>(-vx), static_cast<double>(vy));
    return static_cast<uint8_t>(static_cast<long>(std::floor(a * 128.0 / kPi + 0.5)) & 0xff);
}

void FrameDecoder::reset() {
    for (Baseline &b : base_) b.valid = false;
    hasSent_ = false;
}

bool FrameDecoder::decode(const uint8_t *data, size_t size) {
    samples_.clear();
    events_.clear();
    hasSelf_ = false;
    BitReader r(data, size);
    if (r.get(1)) {
        if (!hasSent_) return false;
        sent_ = static_cast<uint16_t>(sent_ + r.get(kSentDeltaBits) + 1);
    } else {
        sent_ = static_cast<uint16_t>(r.get(16));
    }
    while (r.get(1)) {
        if (samples_.size() >= kMaxSamples || !readSample(r)) return false;
    }
    while (r.get(1)) {
        if (events_.size() >= kMaxEvents || !readEvent(r)) return false;
    }
    if (r.get(1)) readSelf(r);
    if (!r.ok() || !r.atEnd()) return false;
    hasSent_ = true;
    return true;
}

bool FrameDecoder::readSample(BitReader &r) {
    Sample out{};
    out.slot = static_cast<uint8_t>(r.get(8));
    const bool coded = r.get(1) != 0;
    const bool input = r.get(1) != 0;
    WireState &s = out.state;
    s.flags = input ? kFlagInput : 0;
    if (r.get(1)) {
        if (r.get(1)) s.flags |= kFlagDash;
        if (r.get(1)) s.flags |= kFlagKnock;
    }
    int32_t x, y, vx, vy;
    if (!coded) {
        s.age = static_cast<uint8_t>(r.get(8));
        if (input) s.dir = static_cast<uint8_t>(r.get(8));
        vx = r.getSigned(8);
        vy = r.getSigned(8);
        x = r.getSigned(16);
        y = r.getSigned(16);
    } else {
        const Baseline &b = base_[out.slot];
        if (!b.valid) return false;
        s.age = r.get(1) ? b.s.age : static_cast<uint8_t>(r.get(8));
        if (input) s.dir = (b.s.flags & kFlagInput) && r.get(1) ? b.s.dir : static_cast<uint8_t>(r.get(8));
        int bits = kVelWidths[r.get(2)];
        vx = b.s.vx + r.getSigned(bits);
        vy = b.s.vy + r.getSigned(bits);
        if (vx < -128 || vx > 127 || vy < -128 || vy > 127) return false;
        const auto step = static_cast<uint16_t>(sent_ - s.age);
        const int32_t dt = static_cast<int16_t>(static_cast<uint16_t>(step - b.step));
        bits = kPosWidths[r.get(2)];
        x = predictPos(b.s.x, b.s.vx, vx, dt) + r.getSigned(bits);
        y = predictPos(b.s.y, b.s.vy, vy, dt) + r.getSigned(bits);
        if (x < -32768 || x > 32767 || y < -32768 || y > 32767) return false;
    }
    s.x = static_cast<int16_t>(x);
    s.y = static_cast<int16_t>(y);
    s.vx = static_cast<int8_t>(vx);
    s.vy = static_cast<int8_t>(vy);
    // A moving player faces where it moves; only a slow one's heading is sent.
    s.heading = vx * vx + vy * vy < kHeadingSpeedSq ? static_cast<uint8_t>(r.get(8)) : velocityHeading(vx, vy);
    Baseline &b = base_[out.slot];
    b.s = s;
    b.s.flags &= kFlagInput;  // events describe a moment, not a state
    b.step = static_cast<uint16_t>(sent_ - s.age);
    b.valid = true;
    samples_.push_back(out);
    return r.ok();
}

bool FrameDecoder::readEvent(BitReader &r) {
    FrameEvent e;
    e.kind = static_cast<uint8_t>(r.get(2));
    switch (e.kind) {
    case kEventOrbCollected:
        e.orbId = static_cast<uint16_t>(r.get(16));
        e.slot = static_cast<uint8_t>(r.get(8));
        e.score = static_cast<uint16_t>(r.get(16));
        break;
    case kEventOrbSpawned:
        e.orbId = static_cast<uint16_t>(r.get(16));
        e.x = static_cast<int16_t>(r.getSigned(16));
        e.y = static_cast<int16_t>(r.getSigned(16));
        e.orbKind = static_cast<uint8_t>(r.get(1));
        break;
    case kEventPlayerHit:
        e.slot = static_cast<uint8_t>(r.get(8));
        e.victim = static_cast<uint8_t>(r.get(8));
        e.score = static_cast<uint16_t>(r.get(16));
        e.victimScore = static_cast<uint16_t>(r.get(16));
        break;
    default:
        return false;
    }
    events_.push_back(e);
    return r.ok();
}

void FrameDecoder::readSelf(BitReader &r) {
    SelfState &m = self_;
    m.ack = static_cast<uint8_t>(r.get(8));
    const uint32_t age = r.get(8);
    m.step = age == kSelfAgeEscape ? static_cast<uint16_t>(r.get(16)) : static_cast<uint16_t>(sent_ - age);
    m.x = static_cast<float>(r.getSigned(16)) / 100.0f;  // cm
    m.y = static_cast<float>(r.getSigned(16)) / 100.0f;
    m.vx = static_cast<float>(r.getSigned(16)) / 100.0f;  // cm/s
    m.vy = static_cast<float>(r.getSigned(16)) / 100.0f;
    m.heading = static_cast<float>(r.getSigned(16)) * (360.0f / 65536.0f);
    float *timers[3] = {&m.dashLeft, &m.dashCooldown, &m.stun};  // ms
    for (float *t : timers) *t = r.get(1) ? static_cast<float>(r.get(16)) / 1000.0f : 0.0f;
    m.flags = static_cast<uint8_t>(r.get(2));
    m.knowDelay = static_cast<uint8_t>(r.get(8));
    hasSelf_ = true;
}

} // namespace arena::net
