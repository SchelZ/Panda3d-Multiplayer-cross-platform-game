"""What clients draw for a remote player, replayed by the AI for
lag-compensated hits. A line-by-line twin of the C++ client's
predictMotion() (client/src/game/Movement.cpp) and RemoteMotion
(client/src/game/RemoteMotion.cpp); client/tests/DeadReckoningTests.cpp
checks the two agree to a millimetre.

A sample is a tuple (t, x, y, vx, vy, arrival, ix, iy, hold): the player's
state at its time t, when it reached the AI, the held input (a unit vector,
or zero) and how long its velocity is held first (the rest of a dash).
"""

import bisect
import math

BLEND_TIME = 0.1            # s: shortest blend onto a new sample
MAX_CORRECTION_SPEED = 8.0  # m/s: longer blends for bigger errors
MAX_EXTRAPOLATION = 1.0     # s of prediction past the newest sample
SNAP_DISTANCE = 3.0         # m
RATE_LIMIT = 3.0            # offset's initial rate <= 3 x offset / blend
PHYSICS_STEP = 1.0 / 120.0  # the client's fixed movement step (kPhysicsStep)


def dashHoldTime(dashTime):
    """Movement.cpp dashHoldTime(): whole physics steps."""
    return math.ceil(dashTime / PHYSICS_STEP - 1e-4) * PHYSICS_STEP


def predictMotion(s, age, maxSpeed, accel):
    """stepMovement() in closed form with the sample's input held, at
    PHYSICS_STEP steps (each moves with its new velocity)."""
    x, y, vx, vy = s[1], s[2], s[3], s[4]
    left = max(age, 0.0)
    hold = min(left, s[8])
    x += vx * hold
    y += vy * hold
    left -= hold
    if left > 0.0:
        tx, ty = s[6] * maxSpeed, s[7] * maxSpeed
        dvx, dvy = tx - vx, ty - vy
        gap = math.sqrt(dvx * dvx + dvy * dvy)
        if gap > 1e-6:
            a = accel * (2.5 if vx * vx + vy * vy > maxSpeed * maxSpeed + 1e-3 else 1.0)
            ta = min(left, gap / a)
            ax, ay = dvx / gap * a, dvy / gap * a
            x += vx * ta + 0.5 * ax * ta * (ta + PHYSICS_STEP)
            y += vy * ta + 0.5 * ay * ta * (ta + PHYSICS_STEP)
            left -= ta
            if left > 0.0:
                vx, vy = tx, ty
            else:
                vx += ax * ta
                vy += ay * ta
        x += vx * left
        y += vy * left
    return x, y, vx, vy


def predict(s, r, maxSpeed, accel):
    """RemoteMotion::predict(): sample s at time r, stopping after
    MAX_EXTRAPOLATION without news."""
    age = r - s[0]
    if age < MAX_EXTRAPOLATION:
        return predictMotion(s, age, maxSpeed, accel)
    x, y, _, _ = predictMotion(s, MAX_EXTRAPOLATION, maxSpeed, accel)
    return x, y, 0.0, 0.0


class Drawing:
    """RemoteMotion as one viewer ran it: each sample takes over the drawing
    at render time max(t, arrival at the viewer - view delay); the position
    error left at the switch fades out as an offset. Built once for a track,
    then asked for any time."""

    def __init__(self, track, down, delay, maxSpeed, accel):
        """`track`: the player's samples; each reached the viewer `down`
        after it reached us, and the viewer drew `delay` in the past."""
        self.maxSpeed, self.accel = maxSpeed, accel
        self.starts = []    # render time each segment takes over
        self.segments = []  # (sample, a, b, ox, oy, rx, ry)
        last = -1e18
        for s in track:
            at = max(s[0], s[5] + down - delay, last)
            last = at
            self._activate(s, at)

    def _path(self, seg, r):
        """RemoteMotion::path()."""
        s, a, b, ox, oy, rx, ry = seg
        x, y, vx, vy = predict(s, r, self.maxSpeed, self.accel)
        if a < b and r < b:
            span = b - a
            u = min(max((r - a) / span, 0.0), 1.0)
            u2 = u * u
            u3 = u2 * u
            h00, h10 = 2.0 * u3 - 3.0 * u2 + 1.0, (u3 - 2.0 * u2 + u) * span
            d00, d10 = (6.0 * u2 - 6.0 * u) / span, 3.0 * u2 - 4.0 * u + 1.0
            x += h00 * ox + h10 * rx
            y += h00 * oy + h10 * ry
            vx += d00 * ox + d10 * rx
            vy += d00 * oy + d10 * ry
        return x, y, vx, vy

    def _activate(self, s, at):
        """RemoteMotion::activate()."""
        seg = (s, at, at, 0.0, 0.0, 0.0, 0.0)
        if self.segments:
            old = self.segments[-1]
            drawn = self._path(old, at)
            before = predict(old[0], at, self.maxSpeed, self.accel)
            target = predict(s, at, self.maxSpeed, self.accel)
            ox, oy = drawn[0] - target[0], drawn[1] - target[1]
            err = math.hypot(ox, oy)
            if 1e-5 <= err <= SNAP_DISTANCE:
                b = at + max(BLEND_TIME, 1.5 * err / MAX_CORRECTION_SPEED)
                rx, ry = drawn[2] - before[2], drawn[3] - before[3]
                rate, limit = math.hypot(rx, ry), RATE_LIMIT * err / (b - at)
                if rate > limit:
                    rx, ry = rx * limit / rate, ry * limit / rate
                seg = (s, at, b, ox, oy, rx, ry)
        self.starts.append(at)
        self.segments.append(seg)

    def at(self, r):
        """(x, y) drawn at render time r."""
        i = max(0, bisect.bisect_right(self.starts, r) - 1)
        return self._path(self.segments[i], r)[:2]
