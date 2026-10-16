#include "game/Movement.h"

#include <algorithm>
#include <cmath>

namespace arena::game {

namespace {
constexpr float kRadToDeg = 57.29577951308232f;
constexpr float kDegToRad = 0.017453292519943295f;
constexpr float kBounce = 0.35f;  // fraction of impact speed reflected
} // namespace

float speedOf(const MoveState &s) { return std::sqrt(s.vx * s.vx + s.vy * s.vy); }

bool stepMovement(MoveState &s, const MoveInput &in, const Rules &rules, const ArenaLayout &arena, float dt) {
    float ix = in.x, iy = in.y;
    const float len = std::sqrt(ix * ix + iy * iy);
    if (len > 1.0f) {
        ix /= len;
        iy /= len;
    }

    s.dashCooldown = std::max(0.0f, s.dashCooldown - dt);
    s.stun = std::max(0.0f, s.stun - dt);

    bool dashed = false;
    if (in.dash && s.dashCooldown <= 0.0f && s.stun <= 0.0f) {
        float dx, dy;
        const float n = std::sqrt(ix * ix + iy * iy);
        if (n > 0.01f) {
            dx = ix / n;
            dy = iy / n;
        } else {  // no input: dash where we are facing
            dx = -std::sin(s.heading * kDegToRad);
            dy = std::cos(s.heading * kDegToRad);
        }
        s.vx = dx * rules.dashSpeed;
        s.vy = dy * rules.dashSpeed;
        s.dashLeft = rules.dashTime;
        s.dashCooldown = rules.dashCooldown;
        dashed = true;
    }

    if (s.dashLeft > 0.0f) {
        s.dashLeft = std::max(0.0f, s.dashLeft - dt);  // committed: no steering
    } else {
        const float control = s.stun > 0.0f ? 0.25f : 1.0f;
        const float tx = ix * rules.maxSpeed, ty = iy * rules.maxSpeed;
        float dvx = tx - s.vx, dvy = ty - s.vy;
        const float dlen = std::sqrt(dvx * dvx + dvy * dvy);
        // Over max speed (end of a dash/knockback) we brake harder, so the
        // burst stays short and the server's speed check stays tight.
        float maxDelta = rules.accel * control * dt;
        if (speedOf(s) > rules.maxSpeed) {
            maxDelta = std::max(maxDelta, rules.accel * 2.5f * dt);
        }
        if (dlen > maxDelta && dlen > 0.0f) {
            dvx *= maxDelta / dlen;
            dvy *= maxDelta / dlen;
        }
        s.vx += dvx;
        s.vy += dvy;
    }

    s.x += s.vx * dt;
    s.y += s.vy * dt;
    resolveCollisions(s, rules, arena);

    if (speedOf(s) > 0.5f) {
        s.heading = std::atan2(-s.vx, s.vy) * kRadToDeg;
    }
    return dashed;
}

float dashHoldTime(const Rules &rules) {
    return static_cast<float>(std::ceil(rules.dashTime / kPhysicsStep - 1e-4) * kPhysicsStep);
}

float heldFor(float dashLeft) {
    return dashLeft > 0.0f ? static_cast<float>(std::ceil(dashLeft / kPhysicsStep - 1e-4) * kPhysicsStep) : 0.0f;
}

MotionPose predictMotion(const MotionSample &s, double age, const Rules &rules) {
    double x = s.x, y = s.y, vx = s.vx, vy = s.vy;
    double left = std::max(age, 0.0);
    const double hold = std::min(left, double(s.hold));
    x += vx * hold;
    y += vy * hold;
    left -= hold;
    if (left > 0.0) {
        const double tx = s.ix * rules.maxSpeed, ty = s.iy * rules.maxSpeed;
        const double dvx = tx - vx, dvy = ty - vy;
        const double gap = std::sqrt(dvx * dvx + dvy * dvy);
        if (gap > 1e-6) {
            const bool fast = vx * vx + vy * vy > double(rules.maxSpeed) * rules.maxSpeed + 1e-3;
            const double a = rules.accel * (fast ? 2.5 : 1.0);
            const double ta = std::min(left, gap / a);
            const double ax = dvx / gap * a, ay = dvy / gap * a;
            x += vx * ta + 0.5 * ax * ta * (ta + kPhysicsStep);
            y += vy * ta + 0.5 * ay * ta * (ta + kPhysicsStep);
            left -= ta;
            if (left > 0.0) {  // reached the target velocity
                vx = tx;
                vy = ty;
            } else {
                vx += ax * ta;
                vy += ay * ta;
            }
        }
        x += vx * left;
        y += vy * left;
    }
    return MotionPose{static_cast<float>(x), static_cast<float>(y), static_cast<float>(vx),
                      static_cast<float>(vy)};
}

void applyKnockback(MoveState &s, float vx, float vy, float stunTime) {
    s.vx = vx;
    s.vy = vy;
    s.dashLeft = 0.0f;
    s.stun = std::max(s.stun, stunTime);
}

void resolveCollisions(MoveState &s, const Rules &rules, const ArenaLayout &arena) {
    for (const Pillar &p : arena.pillars) {
        float dx = s.x - p.x, dy = s.y - p.y;
        const float minDist = p.radius + rules.playerRadius;
        const float d2 = dx * dx + dy * dy;
        if (d2 >= minDist * minDist) {
            continue;
        }
        float d = std::sqrt(d2);
        if (d < 1e-4f) {
            dx = 1.0f;
            dy = 0.0f;
            d = 1.0f;
        }
        const float nx = dx / d, ny = dy / d;
        s.x = p.x + nx * minDist;
        s.y = p.y + ny * minDist;
        const float vn = s.vx * nx + s.vy * ny;
        if (vn < 0.0f) {
            s.vx -= nx * vn * (1.0f + kBounce);
            s.vy -= ny * vn * (1.0f + kBounce);
        }
    }

    const float limit = arena.radius - rules.playerRadius;
    const float d = std::sqrt(s.x * s.x + s.y * s.y);
    if (d > limit && d > 0.0f) {
        const float nx = s.x / d, ny = s.y / d;
        s.x = nx * limit;
        s.y = ny * limit;
        const float vn = s.vx * nx + s.vy * ny;
        if (vn > 0.0f) {
            s.vx -= nx * vn * (1.0f + kBounce);
            s.vy -= ny * vn * (1.0f + kBounce);
        }
    }
}

void separateFrom(MoveState &s, float ox, float oy, float minDist) {
    const float dx = s.x - ox, dy = s.y - oy;
    const float d2 = dx * dx + dy * dy;
    if (d2 >= minDist * minDist || d2 < 1e-8f) {
        return;
    }
    const float d = std::sqrt(d2);
    // Move half the overlap; the other player resolves the other half.
    const float push = (minDist - d) * 0.5f;
    s.x += dx / d * push;
    s.y += dy / d * push;
}

} // namespace arena::game
