// Player physics: acceleration-based movement, dash, knockback and
// collisions against the arena edge and pillars. Pure functions, no Panda
// scene graph, so they are unit-tested (tests/MovementTests.cpp). The AI
// runs a line-by-line Python twin (server/arena/motion.py) for every player;
// this client runs it to predict its own player from the same inputs.
#pragma once

#include <cstdint>
#include <vector>

namespace arena::game {

// Tuning, received from the server (DistributedArenaGame.setRules).
struct Rules {
    float maxSpeed = 9.0f;
    float accel = 42.0f;
    float dashSpeed = 24.0f;
    float dashTime = 0.22f;
    float dashCooldown = 1.6f;
    float playerRadius = 0.8f;
    float orbRadius = 0.45f;

    bool operator==(const Rules &o) const {
        return maxSpeed == o.maxSpeed && accel == o.accel && dashSpeed == o.dashSpeed &&
               dashTime == o.dashTime && dashCooldown == o.dashCooldown &&
               playerRadius == o.playerRadius && orbRadius == o.orbRadius;
    }
};

struct Pillar {
    float x, y, radius;
    bool operator==(const Pillar &o) const { return x == o.x && y == o.y && radius == o.radius; }
};

struct ArenaLayout {
    float radius = 34.0f;
    std::vector<Pillar> pillars;
    bool operator==(const ArenaLayout &o) const { return radius == o.radius && pillars == o.pillars; }
};

// Desired direction in world space (length <= 1) and the dash button.
struct MoveInput {
    float x = 0.0f, y = 0.0f;
    bool dash = false;
};

struct MoveState {
    float x = 0.0f, y = 0.0f;
    float vx = 0.0f, vy = 0.0f;
    float heading = 0.0f;       // degrees, Panda convention: 0 = +Y, 90 = -X
    float dashLeft = 0.0f;      // s of dash remaining
    float dashCooldown = 0.0f;  // s until the next dash
    float stun = 0.0f;          // s of reduced control after a knockback
};

// The fixed physics step (GameClient runs stepMovement() at 120 Hz).
inline constexpr double kPhysicsStep = 1.0 / 120.0;

// Timers (dashLeft, dashCooldown, stun) count as run out within this (s).
inline constexpr float kTimerEps = 1e-3f;

// Advances the state by dt. Returns true if a dash started.
bool stepMovement(MoveState &s, const MoveInput &in, const Rules &rules, const ArenaLayout &arena, float dt);

// A hit's knockback: velocity replaced, control reduced for a moment.
inline constexpr float kStunTime = 0.35f;
void applyKnockback(MoveState &s, float vx, float vy, float stunTime = kStunTime);

// Keeps the player inside the arena and out of pillars; bounces velocity.
void resolveCollisions(MoveState &s, const Rules &rules, const ArenaLayout &arena);

// Pushes the player out of a circle (another player), without bounce.
void separateFrom(MoveState &s, float ox, float oy, float minDist);

float speedOf(const MoveState &s);

// A movement sample as other players know it: the state at time t plus the
// input being held. predictMotion() is stepMovement() in closed form with
// that input held: the velocity is kept for `hold` seconds (the rest of a
// dash), then moves straight toward input * maxSpeed at `accel` (2.5x while
// faster than maxSpeed, as after a dash), at kPhysicsStep steps (each step
// moves with its new velocity: 1/2 a t dt further than the exact parabola).
// Collisions are not predicted.
// server/arena/motion.py has the same function for the AI.
struct MotionSample {
    double t = 0.0;
    float x = 0.0f, y = 0.0f, vx = 0.0f, vy = 0.0f;
    float ix = 0.0f, iy = 0.0f;  // held input: unit length, or zero
    float hold = 0.0f;           // s the velocity is held first
};

// A sample and the step the AI sent it at: which samples our player bumps
// into at a given step (Prediction, SelfState::knowDelay).
struct KnownSample {
    MotionSample s;        // s.t on the network clock
    int64_t sentStep = 0;
};

struct MotionPose {
    float x = 0.0f, y = 0.0f, vx = 0.0f, vy = 0.0f;
};

MotionPose predictMotion(const MotionSample &s, double age, const Rules &rules);

// How long a dash holds its velocity: whole physics steps, the step it
// starts in included.
float dashHoldTime(const Rules &rules);
// How much longer the velocity is held with `dashLeft` of a dash left.
float heldFor(float dashLeft);

} // namespace arena::game
