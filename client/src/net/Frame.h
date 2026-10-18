// SERVER_FRAME: everything the AI sends us in one batch, in one message,
// bit-packed. server/arena/frame.py describes the format and writes it;
// this reads it. Each player's sample is coded against that player's
// previous sample (its baseline), which we always have: everything comes
// over one TCP connection, in order.
#pragma once

#include "net/Protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace arena::net {

// frame.py EVENT_*: game events that travel in frames.
inline constexpr uint8_t kEventOrbCollected = 0;
inline constexpr uint8_t kEventOrbSpawned = 1;
inline constexpr uint8_t kEventPlayerHit = 2;

struct FrameEvent {
    uint8_t kind = 0;
    uint16_t orbId = 0;        // orb events
    int16_t x = 0, y = 0;      // orb spawned: where, in 1/10 m
    uint8_t orbKind = 0;       // orb spawned: 0 = normal, 1 = gold
    uint8_t slot = 0;          // orb collected: by whom; hit: the attacker
    uint8_t victim = 0;        // hit
    uint16_t score = 0;        // orb collected: the collector's new score; hit: the attacker's
    uint16_t victimScore = 0;  // hit
};

// Reads bits least significant first, like frame.BitWriter writes them.
// Reading past the end gives zeros and clears ok().
class BitReader {
public:
    BitReader(const uint8_t *data, size_t size) : data_(data), bits_(size * 8) {}
    uint32_t get(int bits);  // at most 32
    int32_t getSigned(int bits);
    bool ok() const { return ok_; }
    // Whether all that is left is zero padding to the next byte.
    bool atEnd() const;

private:
    const uint8_t *data_;
    size_t bits_;
    size_t pos_ = 0;
    bool ok_ = true;
};

// frame.predictPos(): where a sample at `p` (cm) with velocity `pv` (1/4
// m/s) is expected `dt` steps later, arriving with velocity `v`. Integer
// arithmetic, so it is the same here as at the server.
int32_t predictPos(int32_t p, int32_t pv, int32_t v, int32_t dt);
// frame.velocityHeading(): the heading byte of a velocity (1/4 m/s units).
uint8_t velocityHeading(int vx, int vy);

class FrameDecoder {
public:
    struct Sample {
        uint8_t slot;
        WireState state;
    };

    // One frame. False if it does not decode: we are out of step with the
    // server and the connection has to start over (reset()).
    bool decode(const uint8_t *data, size_t size);
    void reset();  // a new connection: no baselines
    // The slot changed hands: its next sample comes in full.
    void forget(uint8_t slot) { base_[slot].valid = false; }

    // The last frame decoded.
    uint16_t sent() const { return sent_; }  // the step the AI sent it at
    const std::vector<Sample> &samples() const { return samples_; }
    const std::vector<FrameEvent> &events() const { return events_; }
    const SelfState *self() const { return hasSelf_ ? &self_ : nullptr; }

private:
    struct Baseline {
        WireState s;
        uint16_t step = 0;
        bool valid = false;
    };
    bool readSample(BitReader &r);
    bool readEvent(BitReader &r);
    void readSelf(BitReader &r);

    std::array<Baseline, 256> base_{};
    uint16_t sent_ = 0;
    bool hasSent_ = false;
    std::vector<Sample> samples_;
    std::vector<FrameEvent> events_;
    SelfState self_;
    bool hasSelf_ = false;
};

} // namespace arena::net
