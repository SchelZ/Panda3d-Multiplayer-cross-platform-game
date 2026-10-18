// Client view of DistributedArenaPlayer (arena.dc). The AI creates and owns
// every player, ours included: name and color are DC fields, movement arrives
// as compact binary samples keyed by the player's one-byte slot
// (net/Protocol.h). Other players are drawn with RemoteMotion; ours is
// predicted by GameClient (Prediction) and only shown here.
#pragma once

#include "net/DistributedObject.h"
#include "net/Protocol.h"

#include "game/DeadReckoning.h"
#include "game/RemoteMotion.h"

#include <array>
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

    // --- our player (GameClient predicts it) --------------------------------
    bool controlled() const { return controlled_; }
    void setControlled(bool controlled) { controlled_ = controlled; }
    void setLocalPose(float x, float y, float h);

    // --- other players ----------------------------------------------------
    // `sentStep`: the step the AI sent it at (SERVER_FRAME). Returns how late
    // the sample arrived (s), or -1 before the clock is synced.
    double applyState(const net::WireState &state, uint16_t sentStep, double now, double delay, const Rules &rules);
    // Pose `delay` in the past (RemoteMotion), heading turned toward the
    // motion, or toward where the AI says the player faces while it stands.
    void updateSmoothing(double now, float dt, double delay, const Rules &rules);

    // The newest samples received, on the network clock, with the steps they
    // were sent at: our player bumps into this one where they say it is
    // (Prediction).
    static constexpr int kKnown = 16;
    int knownCount() const { return knownCount_; }
    const std::array<KnownSample, kKnown> &known() const { return known_; }  // oldest first

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
    bool controlled_ = false;
    float x_ = 0.0f, y_ = 0.0f, h_ = 0.0f;
    bool hasHeading_ = false;
    float facing_ = 0.0f;  // the newest sample's heading

    // Other players.
    RemoteMotion motion_;
    std::array<KnownSample, kKnown> known_{};
    int knownCount_ = 0;
    double lastDash_ = -1e9;
};

} // namespace arena::game
