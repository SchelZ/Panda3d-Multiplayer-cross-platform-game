// Wire constants. Must match server/arena/protocol.py and Panda3D's
// direct/distributed/MsgTypesCMU.py (the CMU ServerRepository protocol).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace arena::net {

inline constexpr uint32_t kProtocolVersion = 3;

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

// Compact binary movement (protocol.py): players are named by a one-byte
// slot, and a movement sample is a 10-byte WireState.
//   CLIENT_MOVE   client -> server: WireState
//   SERVER_MOVES  server -> client, once per tick: uint8 n, n x (uint8 slot, WireState)
//   SERVER_SLOTS  server -> client: k x (uint8 slot, uint32 doId); doId 0 frees the slot
inline constexpr uint16_t CLIENT_MOVE = 9102;
inline constexpr uint16_t SERVER_MOVES = 9103;
inline constexpr uint16_t SERVER_SLOTS = 9104;
inline constexpr uint8_t kNoSlot = 255;
// WireState::flags: bit 0 = a dash started at this sample, bit 1 = the
// movement input is held (WireState::dir), bits 2-7 = the sender's view delay
// (game/RemoteMotion.h ViewDelay) in 4 ms units.
inline constexpr uint8_t kFlagDash = 0x01;
inline constexpr uint8_t kFlagInput = 0x02;
inline constexpr int kViewDelayShift = 2;

struct WireState {
    static constexpr size_t kSize = 10;
    static constexpr float kPosScale = 100.0f;  // cm
    static constexpr float kVelScale = 4.0f;    // 1/4 m/s
    static constexpr float kDirStep = 6.283185307179586f / 256.0f;  // 1.4 degrees

    int16_t x = 0, y = 0;   // position, cm
    int8_t vx = 0, vy = 0;  // velocity, 1/4 m/s
    uint8_t flags = 0;
    uint8_t dir = 0;        // held input direction, 256ths of a turn from +x
    uint16_t t = 0;         // 16-bit network time (ClockDelta, 1/100 s)

    float posX() const { return x / kPosScale; }
    float posY() const { return y / kPosScale; }
    float velX() const { return vx / kVelScale; }
    float velY() const { return vy / kVelScale; }
    bool hasInput() const { return (flags & kFlagInput) != 0; }
    float inputX() const { return hasInput() ? std::cos(dir * kDirStep) : 0.0f; }
    float inputY() const { return hasInput() ? std::sin(dir * kDirStep) : 0.0f; }

    // Quantises (rounding, clamped to the field ranges) like protocol.packState().
    // (ix, iy) is the held input, zero for none; flags gets kFlagInput from it.
    static WireState make(float px, float py, float pvx, float pvy, uint8_t flags, uint16_t t,
                          float ix = 0.0f, float iy = 0.0f) {
        WireState s;
        s.x = static_cast<int16_t>(quant(px * kPosScale, -32768.0f, 32767.0f));
        s.y = static_cast<int16_t>(quant(py * kPosScale, -32768.0f, 32767.0f));
        s.vx = static_cast<int8_t>(quant(pvx * kVelScale, -128.0f, 127.0f));
        s.vy = static_cast<int8_t>(quant(pvy * kVelScale, -128.0f, 127.0f));
        s.flags = static_cast<uint8_t>(flags & ~kFlagInput);
        if (ix != 0.0f || iy != 0.0f) {
            s.flags |= kFlagInput;
            s.dir = static_cast<uint8_t>(static_cast<long>(std::lround(std::atan2(iy, ix) / kDirStep)) & 0xff);
        }
        s.t = t;
        return s;
    }

    void encode(uint8_t *out) const {  // little-endian, like Panda's Datagram
        out[0] = static_cast<uint8_t>(static_cast<uint16_t>(x));
        out[1] = static_cast<uint8_t>(static_cast<uint16_t>(x) >> 8);
        out[2] = static_cast<uint8_t>(static_cast<uint16_t>(y));
        out[3] = static_cast<uint8_t>(static_cast<uint16_t>(y) >> 8);
        out[4] = static_cast<uint8_t>(vx);
        out[5] = static_cast<uint8_t>(vy);
        out[6] = flags;
        out[7] = dir;
        out[8] = static_cast<uint8_t>(t);
        out[9] = static_cast<uint8_t>(t >> 8);
    }

    static WireState decode(const uint8_t *in) {
        WireState s;
        s.x = static_cast<int16_t>(in[0] | (in[1] << 8));
        s.y = static_cast<int16_t>(in[2] | (in[3] << 8));
        s.vx = static_cast<int8_t>(in[4]);
        s.vy = static_cast<int8_t>(in[5]);
        s.flags = in[6];
        s.dir = in[7];
        s.t = static_cast<uint16_t>(in[8] | (in[9] << 8));
        return s;
    }

private:
    static float quant(float v, float lo, float hi) { return std::clamp(std::nearbyint(v), lo, hi); }
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
