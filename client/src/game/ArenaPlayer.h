// Client view of DistributedArenaPlayer (arena.dc): name and color are DC
// fields; movement travels as compact binary samples keyed by the player's
// one-byte slot (net/Protocol.h). The local player's object is owned by this
// client and sends its predicted motion (DeadReckoning); remote players are
// drawn with RemoteMotion.
#pragma once

#include "net/DistributedObject.h"
#include "net/Protocol.h"

#include "game/DeadReckoning.h"
#include "game/RemoteMotion.h"

#include <cstdint>
#include <string>

namespace arena::game {

class GameEvents;

class ArenaPlayer final : public net::DistributedObject {
public:
    ArenaPlayer(net::ClientRepository &cr, GameEvents &events);

    static bool bindFields(const DCClass &dclass);
    static constexpr int kNumColors = 12;
    static std::string sanitizeName(const std::string &name);

    void receiveField(const DCField &field, net::FieldReader &in) override;
    bool packField(const DCField &field, net::FieldWriter &out) const override;
    void announceGenerate() override;
    void disable() override;

    const std::string &name() const { return name_; }
    int colorIndex() const { return colorIndex_; }
    uint8_t slot() const { return slot_; }
    void setSlot(uint8_t slot) { slot_ = slot; }

    // --- local player -----------------------------------------------------
    void setIdentity(const std::string &name, int colorIndex);
    void setLocalPose(float x, float y, float h, float vx, float vy);
    // The movement input held (unit length or zero) and the rest of a dash
    // (MoveState::dashLeft).
    void setLocalInput(float ix, float iy, float dashLeft);
    // Sends a sample when the others' prediction would go wrong
    // (DeadReckoning), or at once with `force` / after sendDash() / sendSoon().
    // `viewDelay` is ViewDelay::wire(): how far back we draw the others.
    void broadcastPose(double now, uint8_t viewDelay, const Rules &rules, bool force = false);
    void sendDash();   // the next sample carries kFlagDash
    void sendSoon() { sendNow_ = true; }
    int positionSends() const { return positionSends_; }

    // --- remote players ---------------------------------------------------
    // Returns how late the sample arrived (s), or -1 before the clock is synced.
    double applyState(const net::WireState &state, double now, double delay, const Rules &rules);
    // Pose `delay` in the past (RemoteMotion), heading turned toward the motion.
    void updateSmoothing(double now, float dt, double delay, const Rules &rules);

    float x() const { return x_; }
    float y() const { return y_; }
    float h() const { return h_; }
    double lastDash() const { return lastDash_; }

private:
    void setName(net::FieldReader &in);
    void setColorIndex(net::FieldReader &in);
    void packName(net::FieldWriter &out) const;
    void packColorIndex(net::FieldWriter &out) const;

    GameEvents &events_;
    std::string name_ = "Player";
    int colorIndex_ = 0;
    uint8_t slot_ = net::kNoSlot;
    float x_ = 0.0f, y_ = 0.0f, h_ = 0.0f;
    float vx_ = 0.0f, vy_ = 0.0f;
    float ix_ = 0.0f, iy_ = 0.0f, dashLeft_ = 0.0f;

    // Local player.
    DeadReckoning reckoning_;
    int positionSends_ = 0;
    bool sendNow_ = false;
    bool dashPending_ = false;

    // Remote player.
    RemoteMotion motion_;
    double lastDash_ = -1e9;
};

} // namespace arena::game
