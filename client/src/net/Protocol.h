// Wire constants. Must match server/arena/protocol.py and Panda3D's
// direct/distributed/MsgTypesCMU.py (the CMU ServerRepository protocol).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace arena::net {

inline constexpr uint32_t kProtocolVersion = 5;

// MsgTypesCMU.py
inline constexpr uint16_t SET_DOID_RANGE_CMU = 9001;
inline constexpr uint16_t CLIENT_OBJECT_GENERATE_CMU = 9002;
inline constexpr uint16_t OBJECT_GENERATE_CMU = 9003;
inline constexpr uint16_t OBJECT_UPDATE_FIELD_CMU = 9004;
inline constexpr uint16_t OBJECT_DISABLE_CMU = 9005;
inline constexpr uint16_t OBJECT_DELETE_CMU = 9006;
inline constexpr uint16_t REQUEST_GENERATES_CMU = 9007;
inline constexpr uint16_t CLIENT_DISCONNECT_CMU = 9008;
inline constexpr uint16_t CLIENT_SET_INTEREST_CMU = 9009;
inline constexpr uint16_t OBJECT_SET_ZONE_CMU = 9010;
inline constexpr uint16_t CLIENT_OBJECT_UPDATE_FIELD = 120;
// Note: CLIENT_HEARTBEAT_CMU shares id 9011 with the targeted-update message
// in MsgTypesCMU.py, so this client never sends heartbeats; it pings the AI.

// Panda Arena handshake (protocol.py)
inline constexpr uint16_t CLIENT_HELLO_ARENA = 9100;
inline constexpr uint16_t SERVER_REJECT_ARENA = 9101;

// The server is authoritative (protocol.py): this client sends only its
// player's inputs, when they change, and the AI tells everyone where everyone
// is. Players are named by a one-byte slot. Time is counted in 120 Hz steps
// on the network clock (ClockDelta); a step goes on the wire as 16 bits.
//   CLIENT_INPUT  client -> server: InputCmd
//   SERVER_FRAME  server -> client, at most once per batch (30 a second): the
//                 others' samples (WireState), game events and our own
//                 player's exact state (SelfState), bit-packed (net/Frame.h)
//   SERVER_SLOTS  server -> client: k x (uint8 slot, uint32 doId); doId 0 frees the slot
inline constexpr uint16_t CLIENT_INPUT = 9102;
inline constexpr uint16_t SERVER_FRAME = 9103;
inline constexpr uint16_t SERVER_SLOTS = 9104;
inline constexpr uint8_t kNoSlot = 255;
inline constexpr double kStepsPerSec = 120.0;
inline constexpr float kDirStep = 6.283185307179586f / 256.0f;  // 1.4 degrees

namespace detail {
inline void put16(uint8_t *out, uint16_t v) {  // little-endian, like Panda's Datagram
    out[0] = static_cast<uint8_t>(v);
    out[1] = static_cast<uint8_t>(v >> 8);
}
inline uint16_t get16(const uint8_t *in) { return static_cast<uint16_t>(in[0] | (in[1] << 8)); }
inline float quant(float v, float lo, float hi) { return std::clamp(std::nearbyint(v), lo, hi); }
} // namespace detail

// A steering direction in 256ths of a turn from +x, and back.
inline uint8_t dirByte(float x, float y) {
    return static_cast<uint8_t>(static_cast<long>(std::lround(std::atan2(y, x) / kDirStep)) & 0xff);
}
inline float dirX(uint8_t dir) { return std::cos(dir * kDirStep); }
inline float dirY(uint8_t dir) { return std::sin(dir * kDirStep); }

// InputCmd::flags: bit 0 = dash at this step, bit 1 = steering toward dir,
// bits 2-7 = our view delay (game/RemoteMotion.h ViewDelay) in 8 ms units.
inline constexpr uint8_t kInputDash = 0x01;
inline constexpr uint8_t kInputHeld = 0x02;
inline constexpr int kViewDelayShift = 2;

struct InputCmd {
    static constexpr size_t kSize = 6;
    uint8_t seq = 0;    // counts our inputs; the AI acknowledges them in SelfState
    uint16_t step = 0;  // first step it applies to (low 16 bits)
    uint8_t flags = 0;
    uint8_t dir = 0;
    // How many steps after their sent step we have been getting the others'
    // samples, by our clock (the second most of the last 64). The AI keeps
    // SelfState::knowDelay above it, so we have every sample we bump by in time
    // even when our clock is a little off the AI's.
    uint8_t sampleDelay = 0;

    void encode(uint8_t *out) const {
        out[0] = seq;
        detail::put16(out + 1, step);
        out[3] = flags;
        out[4] = dir;
        out[5] = sampleDelay;
    }
    static InputCmd decode(const uint8_t *in) {
        return InputCmd{in[0], detail::get16(in + 1), in[3], in[4], in[5]};
    }
};

// WireState::flags: bit 0 = a dash started at this sample, bit 1 = steering
// toward dir, bit 2 = a hit knocked the player back here.
inline constexpr uint8_t kFlagDash = 0x01;
inline constexpr uint8_t kFlagInput = 0x02;
inline constexpr uint8_t kFlagKnock = 0x04;

// A player as everyone draws it (protocol.py STATE: the AI's 10 bytes; in a
// SERVER_FRAME the same fields, coded against the player's previous sample).
struct WireState {
    static constexpr size_t kSize = 10;
    static constexpr float kPosScale = 100.0f;  // cm
    static constexpr float kVelScale = 4.0f;    // 1/4 m/s

    int16_t x = 0, y = 0;   // position, cm
    int8_t vx = 0, vy = 0;  // velocity, 1/4 m/s
    uint8_t flags = 0;
    uint8_t dir = 0;        // steering direction, 256ths of a turn from +x
    uint8_t heading = 0;    // where it faces, 256ths of a turn (Panda's H)
    uint8_t age = 0;        // the step it describes: this many before the batch was sent

    float posX() const { return x / kPosScale; }
    float posY() const { return y / kPosScale; }
    float velX() const { return vx / kVelScale; }
    float velY() const { return vy / kVelScale; }
    float headingDeg() const { return static_cast<float>(static_cast<int8_t>(heading)) * (360.0f / 256.0f); }
    bool hasInput() const { return (flags & kFlagInput) != 0; }
    float inputX() const { return hasInput() ? dirX(dir) : 0.0f; }
    float inputY() const { return hasInput() ? dirY(dir) : 0.0f; }

    // Quantises (rounding, clamped to the field ranges) like protocol.packState().
    // (ix, iy) is the steering input, zero for none; flags gets kFlagInput from it.
    static WireState make(float px, float py, float pvx, float pvy, uint8_t flags, uint8_t age,
                          float ix = 0.0f, float iy = 0.0f, float headingDeg = 0.0f) {
        using detail::quant;
        WireState s;
        s.x = static_cast<int16_t>(quant(px * kPosScale, -32768.0f, 32767.0f));
        s.y = static_cast<int16_t>(quant(py * kPosScale, -32768.0f, 32767.0f));
        s.vx = static_cast<int8_t>(quant(pvx * kVelScale, -128.0f, 127.0f));
        s.vy = static_cast<int8_t>(quant(pvy * kVelScale, -128.0f, 127.0f));
        s.flags = static_cast<uint8_t>(flags & ~kFlagInput);
        if (ix != 0.0f || iy != 0.0f) {
            s.flags |= kFlagInput;
            s.dir = dirByte(ix, iy);
        }
        s.heading = static_cast<uint8_t>(static_cast<long>(std::lround(headingDeg / 360.0f * 256.0f)) & 0xff);
        s.age = age;
        return s;
    }

    void encode(uint8_t *out) const {
        detail::put16(out, static_cast<uint16_t>(x));
        detail::put16(out + 2, static_cast<uint16_t>(y));
        out[4] = static_cast<uint8_t>(vx);
        out[5] = static_cast<uint8_t>(vy);
        out[6] = flags;
        out[7] = dir;
        out[8] = heading;
        out[9] = age;
    }

    static WireState decode(const uint8_t *in) {
        WireState s;
        s.x = static_cast<int16_t>(detail::get16(in));
        s.y = static_cast<int16_t>(detail::get16(in + 2));
        s.vx = static_cast<int8_t>(in[4]);
        s.vy = static_cast<int8_t>(in[5]);
        s.flags = in[6];
        s.dir = in[7];
        s.heading = in[8];
        s.age = in[9];
        return s;
    }
};

// SelfState::flags.
inline constexpr uint8_t kSelfKnock = 0x01;  // knocked back at this step
inline constexpr uint8_t kSelfSnap = 0x02;   // placed here (spawn): show it at once

// Our own player's exact state at a step, as the AI simulated it from our
// inputs up to `ack` (protocol.py SELF, in a SERVER_FRAME): what our
// prediction is checked against, and continues from when it differs.
struct SelfState {
    uint8_t ack = 0;
    uint16_t step = 0;
    float x = 0.0f, y = 0.0f, vx = 0.0f, vy = 0.0f;  // m, m/s (sent in cm, cm/s)
    float heading = 0.0f;                            // degrees (sent in 65536ths of a turn)
    float dashLeft = 0.0f, dashCooldown = 0.0f, stun = 0.0f;  // s (sent in ms)
    uint8_t flags = 0;
    // From the next step on, our player bumps into another where that one's
    // newest sample sent at least this many steps before says it is; the AI
    // follows the same rule, so the bumps are predicted exactly.
    uint8_t knowDelay = 0;
};

inline constexpr uint8_t kRolePlayer = 0;

enum RejectCode : uint16_t {
    kRejectBadVersion = 1,
    kRejectBadDcHash = 2,
    kRejectServerFull = 3,
    kRejectBadToken = 4,
    kRejectProtocolError = 5,
    kRejectFlood = 6,
    kRejectTimeout = 7,
    kRejectShutdown = 8,
};

// Retrying cannot fix these without the user changing something.
constexpr bool isFatalReject(uint16_t code) {
    return code == kRejectBadVersion || code == kRejectBadDcHash || code == kRejectBadToken;
}

inline constexpr uint32_t kZoneGlobal = 1;  // TimeManager
inline constexpr uint32_t kZoneArena = 2;   // game + players

inline constexpr uint16_t kDefaultPort = 4400;

} // namespace arena::net
