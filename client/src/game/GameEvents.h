// Callbacks from the distributed objects to the game. Keeps the DO classes
// free of rendering and the game logic free of wire formats.
#pragma once

#include <cstdint>
#include <string>

namespace arena::game {

class ArenaGame;
class ArenaPlayer;

class GameEvents {
public:
    virtual ~GameEvents() = default;

    virtual void onGameGenerated(ArenaGame &game) = 0;
    virtual void onGameDisabled(ArenaGame &game) = 0;
    virtual void onArenaChanged() = 0;
    virtual void onOrbsReset() = 0;
    virtual void onOrbSpawned(uint16_t orbId) = 0;
    // Players are named by their one-byte movement slot (net/Protocol.h).
    virtual void onOrbCollected(uint16_t orbId, uint8_t slot) = 0;
    virtual void onScoresChanged() = 0;
    virtual void onRoundChanged() = 0;
    virtual void onPlayerHit(uint8_t attacker, uint8_t victim) = 0;
    virtual void onChat(uint8_t slot, const std::string &text) = 0;
    virtual void onNotice(uint8_t kind, uint8_t slot, uint16_t value) = 0;

    // Targeted replies from the AI to this client.
    virtual void onJoined(uint8_t slot, uint32_t playerId) = 0;  // our player
    virtual void onPong(uint16_t context) = 0;

    virtual void onPlayerGenerated(ArenaPlayer &player) = 0;
    virtual void onPlayerDisabled(ArenaPlayer &player) = 0;
    virtual void onPlayerDashed(ArenaPlayer &player) = 0;
};

} // namespace arena::game
