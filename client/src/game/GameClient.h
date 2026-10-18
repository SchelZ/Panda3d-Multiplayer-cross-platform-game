// Game logic of the client: connection flow, our player (asked of the AI,
// steered with inputs and predicted at the AI's 120 Hz step), other players,
// orbs, HUD state and the bot brain. World and Hud are optional, so the same
// code runs headless.
#pragma once

#include "app/Options.h"
#include "game/ArenaGame.h"
#include "game/ArenaPlayer.h"
#include "game/GameEvents.h"
#include "game/Movement.h"
#include "game/Prediction.h"
#include "net/ClientRepository.h"

#include <array>
#include <bitset>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <random>
#include <string>
#include <unordered_map>

namespace arena::game {

class World;
class Hud;

struct PlayerInput {
    float x = 0.0f, y = 0.0f;  // desired direction, world space (screen up = +Y)
    bool dash = false;         // pressed this frame
};

class GameClient final : public GameEvents {
public:
    enum class Phase { Offline, Connecting, SyncingTime, EnteringArena, Joining, Playing, Rejected };

    // What happened, for automation (exit codes) and the HUD.
    struct Counters {
        bool everPlayed = false;
        int maxOtherPlayers = 0;
        int orbsCollected = 0;
        int pongs = 0;
        int chatsReceived = 0;
        int hitsGiven = 0;
        int hitsTaken = 0;
        int corrections = 0;     // the AI put our player elsewhere than we predicted
        double correctionMaxCm = 0.0;
        int selfs = 0;           // SELF messages (our exact state) received
        int reconnects = 0;
        int arenaRebuilds = 0;   // arena/orb resets after the first generate
        double pingSumMs = 0.0;  // over `pongs` replies
        double pingMaxMs = 0.0;
        // Netcode quality, judged per dash: someone touched on this screen
        // (contact) and confirmed by the server, touched but not confirmed
        // (missed), confirmed without a touch here (phantom).
        int dashes = 0;
        int dashContacts = 0;
        int contactsConfirmed = 0;
        int contactsMissed = 0;
        int phantomHits = 0;
        int inputSends = 0;
        double playSeconds = 0.0;
        uint64_t frames = 0;
        uint64_t bytesIn = 0, bytesOut = 0;
    };

    GameClient(const Options &options, World *world, Hud *hud);
    ~GameClient() override;

    bool init(std::string &error);
    void start();
    void update(double now, double dt, const PlayerInput &input);
    void shutdown();
    void sendChat(const std::string &text);

    Phase phase() const { return phase_; }
    const Counters &counters() const { return counters_; }
    const std::string &rejectReason() const { return rejectReason_; }
    double playingSince() const { return playingSince_; }
    float dashReady() const;  // 0..1, 1 = the dash is ready

    // GameEvents
    void onGameGenerated(ArenaGame &game) override;
    void onGameDisabled(ArenaGame &game) override;
    void onArenaChanged() override;
    void onOrbsReset() override;
    void onOrbSpawned(uint16_t orbId) override;
    void onOrbCollected(uint16_t orbId, uint8_t slot) override;
    void onScoresChanged() override;
    void onRoundChanged() override;
    void onPlayerHit(uint8_t attacker, uint8_t victim) override;
    void onChat(uint8_t slot, const std::string &text) override;
    void onNotice(uint8_t kind, uint8_t slot, uint16_t value) override;
    void onJoined(uint8_t slot, uint32_t playerId) override;
    void onPong(uint16_t context) override;
    void onPlayerGenerated(ArenaPlayer &player) override;
    void onPlayerDisabled(ArenaPlayer &player) override;
    void onPlayerDashed(ArenaPlayer &player) override;

private:
    void connectNow();
    void scheduleReconnect(double delay, const std::string &reason);
    double nextBackoff();
    void onRepositoryReady();
    void onTimeSynced();
    void enterArena();
    void requestJoin();
    void startPlaying();
    void onSelf(const net::SelfState &msg);
    Prediction::Others otherPositions() const;
    void simulate(double now, double dt, PlayerInput input);
    PlayerInput botInput(double now);
    void updateHud(double now, double dt);
    void writeTrace(double now);
    void onSlot(uint8_t slot, uint32_t doId);
    void onMove(uint8_t slot, const net::WireState &state, uint16_t sentStep);
    void resetSlots();
    std::string playerName(uint8_t slot) const;
    int playerColor(uint8_t slot) const;
    void setPhase(Phase phase);
    const Rules &rules() const;  // the game's, or the defaults before it exists

    Options options_;
    World *world_;
    Hud *hud_;
    net::ClientRepository cr_;

    Phase phase_ = Phase::Offline;
    double phaseSince_ = 0.0;
    double now_ = 0.0;
    ArenaGame *game_ = nullptr;
    ArenaPlayer *local_ = nullptr;  // our player (the AI's object we steer)
    uint32_t myDoId_ = 0;           // its doId, from DistributedArenaGame.joined
    std::unordered_map<uint32_t, ArenaPlayer *> players_;  // every generated player

    // Movement slots (net/Protocol.h): who is behind each one-byte player id.
    struct Slot {
        uint32_t doId = 0;
        ArenaPlayer *player = nullptr;
        bool hasState = false;  // a sample that arrived before the player's generate
        net::WireState state;
        uint16_t sentStep = 0;
        std::string name;       // kept after the player left, for the "left" notice
        int color = 0;
    };
    std::array<Slot, 256> slots_;
    uint8_t localSlot_ = net::kNoSlot;

    Prediction prediction_;      // our player, from our inputs
    bool dashLatch_ = false;     // dash pressed, not sent yet
    float smoothX_ = 0.0f, smoothY_ = 0.0f;  // corrections, faded out on screen
    float shownHeading_ = 0.0f;  // our body's drawn heading (turnToward)
    ViewDelay viewDelay_;        // how far back remote players are drawn
    // Steps after their sent step the others' samples reached our prediction,
    // last 64 (net::InputCmd::sampleDelay).
    std::array<uint8_t, 64> sampleDelays_{};
    size_t sampleDelayNext_ = 0;
    uint8_t sampleDelay() const;
    double reconnectAt_ = 0.0;
    int backoffStep_ = 0;
    std::string offlineReason_;
    std::string rejectReason_;
    Counters counters_;
    double playingSince_ = -1.0;
    bool said_ = false;
    bool closing_ = false;  // disconnecting on purpose: do not ask for a new player

    uint16_t pingContext_ = 0;
    double pingSentAt_ = 0.0;
    double lastPing_ = -1e9;
    double rttMs_ = -1.0;

    std::unordered_map<uint16_t, double> predictedOrbs_;  // orb -> time we touched it

    // Dash contact measurement (see Counters).
    struct DashRecord {
        double start;
        std::bitset<256> seen, confirmed;  // by slot
    };
    void settleDashes(double now, bool all);
    std::deque<DashRecord> dashRecords_;
    std::array<double, 256> lastHitAt_;  // by victim slot

    // Bot brain.
    float wanderX_ = 0.0f, wanderY_ = 0.0f;
    double wanderUntil_ = 0.0;
    // Fidgeting like people do: short taps and quick reversals, which are the
    // hardest motion to show smoothly on other screens.
    bool fidgetInput(double now, PlayerInput &in);
    double fidgetFrom_ = 4.0, fidgetUntil_ = 0.0;  // since playing
    double tapUntil_ = 0.0, tapPauseUntil_ = 0.0;
    float tapX_ = 0.0f, tapY_ = 0.0f;

    // Rates for the stats overlay.
    double statsTime_ = 0.0;
    uint64_t statsBytesIn_ = 0, statsBytesOut_ = 0, statsMsgsIn_ = 0, statsMsgsOut_ = 0;
    double kbIn_ = 0, kbOut_ = 0, msgsInRate_ = 0, msgsOutRate_ = 0;
    double fps_ = 0.0;
    double shownFps_ = 0.0, shownFpsTime_ = -1e9;  // refreshed twice a second
    uint64_t frames_ = 0;

    std::mt19937 rng_;
    std::FILE *trace_ = nullptr;
    int64_t tracedStep_ = 0;  // newest step written as an L line
};

} // namespace arena::game
