// Our own player, predicted from our own inputs.
//
// The server is authoritative: this client sends only inputs (net::InputCmd),
// each naming the 120 Hz step it starts at, and the AI simulates our player
// from them with the same movement code (server/arena/simulation.py). This
// class runs that simulation here too, step for step, so the player responds
// the moment a key is pressed and normally ends up exactly where the AI puts
// it.
//
// When the AI's result may differ (a knockback, contact with another player,
// an input that arrived too late, the spawn) it sends our exact state at one
// step (net::SelfState). reconcile() compares it with what we had predicted
// for that step; if they differ it continues from the AI's state and replays
// our inputs since then. The caller fades the resulting jump out on screen.
#pragma once

#include "game/Movement.h"
#include "net/Protocol.h"

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace arena::game {

class Prediction {
public:
    static constexpr int kHistory = 256;              // steps of predicted states kept (2.1 s)
    static constexpr double kTurnInterval = 1.0 / 30.0;  // s between inputs that only turn
    static constexpr int kTurnDeadband = 2;           // 256ths of a turn that are not a turn
    static constexpr float kPositionTolerance = 0.02f;  // m our state may differ from the AI's
    static constexpr float kVelocityTolerance = 0.1f;   // m/s
    static constexpr float kTimerTolerance = 0.003f;    // s

    // The other players, each as its newest samples (network clock, oldest
    // first). At step k our player bumps into each where its newest sample
    // sent at least knowDelay steps before says it is (RemoteMotion::predict);
    // the AI judges it the same way (DistributedArenaGameAI._advance).
    struct Other {
        const KnownSample *samples;
        int count;
    };
    using Others = std::vector<Other>;

    bool started() const { return started_; }
    int64_t step() const { return step_; }
    const MoveState &state() const { return state_; }
    const MoveState &previous() const { return previous_; }

    // Begins from the AI's state at `step` (the spawn), with its knowDelay.
    void start(int64_t step, const MoveState &s, int knowDelay);
    void stop() { started_ = false; }

    // The input wanted from the next step (step() + 1) on. Fills `out` and
    // returns true when it must go to the server: pressed, released, a dash,
    // or turned (at most every kTurnInterval). Otherwise the input in effect
    // stays as it was sent: client and AI simulate the same thing.
    bool setInput(bool held, uint8_t dir, bool dash, double now, uint8_t viewDelay, net::InputCmd &out);

    // Simulates step() + 1. Returns true if a dash started.
    bool advance(const Rules &rules, const ArenaLayout &arena, const Others &others);

    // The AI's state at `msgStep` (from msg.step). Returns false if we had
    // predicted it (within the tolerances); (dx, dy) is how far our current
    // position moved, a few millimetres of rounding if so.
    bool reconcile(const net::SelfState &msg, int64_t msgStep, const Rules &rules, const ArenaLayout &arena,
                   const Others &others, float &dx, float &dy);

    // What we predicted for a step still in the history, or null.
    const MoveState *predictedAt(int64_t step) const;

    int knowDelay() const { return know_; }
    int knowDelayAt(int64_t k) const { return k >= knowFrom_ ? know_ : knowBefore_; }
    bool held() const { return !sent_.empty() && sent_.back().held; }
    uint8_t dir() const { return sent_.empty() ? 0 : sent_.back().dir; }
    int inputsSent() const { return static_cast<int>(sentCount_); }
    // Whether the last reconcile() had inputs the AI got too late (for logs).
    bool lastStale() const { return lastStale_; }
    int corrections() const { return corrections_; }

private:
    struct Sent {
        int64_t n;     // our input count; the wire seq is its low 8 bits
        int64_t step;  // the step it applies from (here and at the AI)
        bool held;
        uint8_t dir;
        bool dash;
    };
    // One step with the inputs as `sent`, each from step `effective(i)`.
    bool simulate(int64_t k, const std::deque<Sent> &sent, int64_t lateFrom, int64_t ackN, const Rules &rules,
                  const ArenaLayout &arena, const Others &others);
    void record();

    bool started_ = false;
    int64_t step_ = 0;
    int know_ = 24, knowBefore_ = 24;  // SelfState::knowDelay, in effect from step knowFrom_
    int64_t knowFrom_ = 0;
    MoveState state_, previous_;
    std::array<MoveState, kHistory> history_{};  // by step % kHistory
    std::deque<Sent> sent_;
    int64_t sentCount_ = 0;
    double lastSend_ = -1e9;
    int corrections_ = 0;
    bool lastStale_ = false;
};

} // namespace arena::game
