#include "game/ArenaGame.h"

#include "game/GameEvents.h"
#include "net/ClientRepository.h"

#include "clockObject.h"

#include <utility>

namespace arena::game {

namespace {
net::FieldTable<ArenaGame> g_fields;
const DCField *g_requestJoin = nullptr;
const DCField *g_requestChat = nullptr;
const DCField *g_ping = nullptr;
} // namespace

bool ArenaGame::bindFields(const DCClass &dclass) {
    g_fields.bind(dclass, "setRules", &ArenaGame::setRules);
    g_fields.bind(dclass, "setArena", &ArenaGame::setArena);
    g_fields.bind(dclass, "setRound", &ArenaGame::setRound);
    g_fields.bind(dclass, "setOrbs", &ArenaGame::setOrbs);
    g_fields.bind(dclass, "setScores", &ArenaGame::setScores);
    g_fields.bind(dclass, "chatMessage", &ArenaGame::chatMessage);
    g_fields.bind(dclass, "notice", &ArenaGame::notice);
    g_fields.bind(dclass, "joined", &ArenaGame::joined);
    g_fields.bind(dclass, "pong", &ArenaGame::pong);
    g_requestJoin = g_fields.field(dclass, "requestJoin");
    g_requestChat = g_fields.field(dclass, "requestChat");
    g_ping = g_fields.field(dclass, "ping");
    return g_fields.missing().empty();
}

void ArenaGame::receiveField(const DCField &field, net::FieldReader &in) {
    g_fields.receive(*this, field, in);
}

void ArenaGame::announceGenerate() { events_.onGameGenerated(*this); }

void ArenaGame::disable() { events_.onGameDisabled(*this); }

void ArenaGame::requestJoin(const std::string &name, int colorIndex) {
    cr_.sendUpdate(*this, g_requestJoin, [&name, colorIndex](net::FieldWriter &out) {
        out.str(name).u32(static_cast<uint32_t>(colorIndex));
    });
}

void ArenaGame::requestChat(const std::string &text) {
    cr_.sendUpdate(*this, g_requestChat, [&text](net::FieldWriter &out) { out.str(text); });
}

void ArenaGame::ping(uint16_t context) {
    cr_.sendUpdate(*this, g_ping, [context](net::FieldWriter &out) { out.u32(context); });
}

double ArenaGame::roundEndLocal() const {
    const double now = ClockObject::get_global_clock()->get_frame_time();
    return cr_.clockDelta().networkToLocalTime(roundEndNetwork_, now, 32);
}

// While the initial generate is applied, generated() is false and the game is
// told about the complete object once, in announceGenerate(). Afterwards
// changes are reported as they happen. The AI re-sends the whole object
// whenever someone else enters the zone (REQUEST_GENERATES), so the
// "required" setters only report real changes; otherwise every join would
// rebuild the arena on every client.

void ArenaGame::setRules(net::FieldReader &in) {
    Rules r;
    r.maxSpeed = static_cast<float>(in.f64());
    r.accel = static_cast<float>(in.f64());
    r.dashSpeed = static_cast<float>(in.f64());
    r.dashTime = static_cast<float>(in.f64());
    r.dashCooldown = static_cast<float>(in.f64());
    r.playerRadius = static_cast<float>(in.f64());
    r.orbRadius = static_cast<float>(in.f64());
    const bool changed = !(r == rules_);
    rules_ = r;
    if (generated() && changed) events_.onArenaChanged();
}

void ArenaGame::setArena(net::FieldReader &in) {
    ArenaLayout layout;
    layout.radius = static_cast<float>(in.f64());
    in.push();
    while (in.more()) {
        in.push();
        Pillar p;
        p.x = static_cast<float>(in.f64());
        p.y = static_cast<float>(in.f64());
        p.radius = static_cast<float>(in.f64());
        in.pop();
        layout.pillars.push_back(p);
    }
    in.pop();
    const bool changed = !(layout == layout_);
    layout_ = std::move(layout);
    if (generated() && changed) events_.onArenaChanged();
}

void ArenaGame::setRound(net::FieldReader &in) {
    const RoundState state = in.u32() == 1 ? RoundState::Intermission : RoundState::Playing;
    const int number = static_cast<int>(in.u32());
    const int32_t end = in.i32();
    const bool changed = state != roundState_ || number != roundNumber_ || end != roundEndNetwork_;
    roundState_ = state;
    roundNumber_ = number;
    roundEndNetwork_ = end;
    if (generated() && changed) events_.onRoundChanged();
}

void ArenaGame::setOrbs(net::FieldReader &in) {
    std::unordered_map<uint16_t, Orb> orbs;
    in.push();
    while (in.more()) {
        in.push();
        const auto id = static_cast<uint16_t>(in.u32());
        Orb orb;
        orb.x = static_cast<float>(in.f64());
        orb.y = static_cast<float>(in.f64());
        orb.kind = static_cast<int>(in.u32());
        in.pop();
        orbs[id] = orb;
    }
    in.pop();
    const bool changed = orbs != orbs_;
    orbs_ = std::move(orbs);
    if (generated() && changed) events_.onOrbsReset();
}

void ArenaGame::setScores(net::FieldReader &in) {
    std::array<int, 256> scores{};
    in.push();
    while (in.more()) {
        in.push();
        const auto slot = static_cast<uint8_t>(in.u32());
        const int score = static_cast<int>(in.u32());
        in.pop();
        scores[slot] = score;
    }
    in.pop();
    const bool changed = scores != scores_;
    scores_ = std::move(scores);
    if (generated() && changed) events_.onScoresChanged();
}

void ArenaGame::applyEvent(const net::FrameEvent &e) {
    switch (e.kind) {
    case net::kEventOrbSpawned:
        orbs_[e.orbId] = Orb{e.x / 10.0f, e.y / 10.0f, e.orbKind};
        events_.onOrbSpawned(e.orbId);
        break;
    case net::kEventOrbCollected:
        scores_[e.slot] = e.score;
        events_.onOrbCollected(e.orbId, e.slot);  // before erasing, so the game can see where it was
        orbs_.erase(e.orbId);
        events_.onScoresChanged();
        break;
    case net::kEventPlayerHit:
        scores_[e.slot] = e.score;
        scores_[e.victim] = e.victimScore;
        events_.onPlayerHit(e.slot, e.victim);
        events_.onScoresChanged();
        break;
    default:
        break;
    }
}

void ArenaGame::chatMessage(net::FieldReader &in) {
    const auto slot = static_cast<uint8_t>(in.u32());
    const std::string text = in.str();
    events_.onChat(slot, text);
}

void ArenaGame::notice(net::FieldReader &in) {
    const auto kind = static_cast<uint8_t>(in.u32());
    const auto slot = static_cast<uint8_t>(in.u32());
    const auto value = static_cast<uint16_t>(in.u32());
    events_.onNotice(kind, slot, value);
}

void ArenaGame::joined(net::FieldReader &in) {
    const auto slot = static_cast<uint8_t>(in.u32());
    const uint32_t playerId = in.u32();
    events_.onJoined(slot, playerId);
}

void ArenaGame::pong(net::FieldReader &in) { events_.onPong(static_cast<uint16_t>(in.u32())); }

} // namespace arena::game
