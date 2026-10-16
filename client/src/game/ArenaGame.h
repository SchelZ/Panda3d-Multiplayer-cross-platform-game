// Client view of DistributedArenaGame (arena.dc), owned by the AI.
#pragma once

#include "game/Movement.h"
#include "net/DistributedObject.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace arena::game {

class GameEvents;

struct Orb {
    float x, y;
    int kind;  // 0 = normal, 1 = gold
    bool operator==(const Orb &o) const { return x == o.x && y == o.y && kind == o.kind; }
};

enum class RoundState { Playing = 0, Intermission = 1 };

// DistributedArenaGame.notice kinds (server/arena/protocol.py NOTICE_*).
enum NoticeKind : uint8_t {
    kNoticeJoined = 1,
    kNoticeLeft = 2,
    kNoticeRoundStart = 3,
    kNoticeRoundOver = 4,
    kNoticeShutdown = 5,
};

class ArenaGame final : public net::DistributedObject {
public:
    ArenaGame(net::ClientRepository &cr, GameEvents &events) : DistributedObject(cr), events_(events) {}

    static bool bindFields(const DCClass &dclass);

    void receiveField(const DCField &field, net::FieldReader &in) override;
    bool packField(const DCField &, net::FieldWriter &) const override { return false; }
    void announceGenerate() override;
    void disable() override;

    // Requests to the AI.
    void requestChat(const std::string &text);
    void ping(uint16_t context);

    const Rules &rules() const { return rules_; }
    const ArenaLayout &layout() const { return layout_; }
    RoundState roundState() const { return roundState_; }
    int roundNumber() const { return roundNumber_; }
    double roundEndLocal() const;  // local clock time the round phase ends
    const std::unordered_map<uint16_t, Orb> &orbs() const { return orbs_; }
    int scoreOf(uint8_t slot) const { return scores_[slot]; }

private:
    void setRules(net::FieldReader &in);
    void setArena(net::FieldReader &in);
    void setRound(net::FieldReader &in);
    void setOrbs(net::FieldReader &in);
    void setScores(net::FieldReader &in);
    void orbSpawned(net::FieldReader &in);
    void orbCollected(net::FieldReader &in);
    void playerHit(net::FieldReader &in);
    void chatMessage(net::FieldReader &in);
    void notice(net::FieldReader &in);
    void pong(net::FieldReader &in);
    void knockback(net::FieldReader &in);
    void correctPosition(net::FieldReader &in);

    GameEvents &events_;
    Rules rules_;
    ArenaLayout layout_;
    RoundState roundState_ = RoundState::Playing;
    int roundNumber_ = 0;
    int32_t roundEndNetwork_ = 0;
    std::unordered_map<uint16_t, Orb> orbs_;
    std::array<int, 256> scores_{};  // by slot
};

} // namespace arena::game
