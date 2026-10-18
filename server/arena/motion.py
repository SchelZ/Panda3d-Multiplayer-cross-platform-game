"""Player movement, line-by-line twins of the C++ client's code, so the AI
and every client compute the same motion:

* stepMovement() and friends: client/src/game/Movement.cpp. The AI runs it
  for every player from the player's inputs; the client runs it to predict
  its own player. client/tests/MovementTests.cpp and tests/test_motion.py
  check both against the same reference trajectory.
* predictMotion(), DeadReckoning: Movement.cpp and DeadReckoning.cpp. From
  one sample (state + steering input) everyone predicts a player's motion;
  the AI sends a sample only when that prediction would go wrong.
* Drawing: RemoteMotion.cpp, what a client draws for another player, which
  the AI replays for lag-compensated hits. DeadReckoningTests.cpp checks the
  two agree to a millimetre.

A sample is a tuple (t, x, y, vx, vy, sent, ix, iy, hold[, sentStep]): the
player's state at its time t, when the AI sent it, the steering input (a
unit vector, or zero) and how long its velocity is held first (the rest of a
dash); the AI also keeps the step it sent it at.
"""

import bisect
import math

BLEND_TIME = 0.1            # s: shortest blend onto a new sample
MAX_CORRECTION_SPEED = 8.0  # m/s: longer blends for bigger errors
MAX_EXTRAPOLATION = 1.0     # s of prediction past the newest sample
SNAP_DISTANCE = 3.0         # m
RATE_LIMIT = 3.0            # offset's initial rate <= 3 x offset / blend
PHYSICS_STEP = 1.0 / 120.0  # the fixed movement step (kPhysicsStep)

# Movement.cpp constants.
BOUNCE = 0.35               # fraction of impact speed reflected
TIMER_EPS = 1e-3            # s: timers count as run out within this
FAST_EPS = 1e-2             # m2/s2: "faster than max speed" beyond this
STUN_TIME = 0.35            # s of reduced control after a knockback


class MoveState:
    """MoveState in Movement.h. heading in degrees, Panda's convention."""
    __slots__ = ('x', 'y', 'vx', 'vy', 'heading', 'dashLeft', 'dashCooldown', 'stun')

    def __init__(self, x=0.0, y=0.0, vx=0.0, vy=0.0, heading=0.0, dashLeft=0.0, dashCooldown=0.0,
                 stun=0.0):
        self.x, self.y, self.vx, self.vy = x, y, vx, vy
        self.heading = heading
        self.dashLeft, self.dashCooldown, self.stun = dashLeft, dashCooldown, stun

    def copy(self):
        return MoveState(self.x, self.y, self.vx, self.vy, self.heading, self.dashLeft,
                         self.dashCooldown, self.stun)


def stepMovement(s, ix, iy, dash, rules, pillars, arenaRadius, dt=PHYSICS_STEP):
    """Advances s by dt with steering input (ix, iy) (length <= 1) and the
    dash button. Returns True if a dash started."""
    length = math.sqrt(ix * ix + iy * iy)
    if length > 1.0:
        ix /= length
        iy /= length

    s.dashCooldown = max(0.0, s.dashCooldown - dt)
    s.stun = max(0.0, s.stun - dt)

    dashed = False
    if dash and s.dashCooldown <= TIMER_EPS and s.stun <= TIMER_EPS:
        n = math.sqrt(ix * ix + iy * iy)
        if n > 0.01:
            dx, dy = ix / n, iy / n
        else:  # no input: dash where we are facing
            h = math.radians(s.heading)
            dx, dy = -math.sin(h), math.cos(h)
        s.vx = dx * rules.dashSpeed
        s.vy = dy * rules.dashSpeed
        s.dashLeft = rules.dashTime
        s.dashCooldown = rules.dashCooldown
        dashed = True

    if s.dashLeft > TIMER_EPS:
        s.dashLeft = max(0.0, s.dashLeft - dt)  # committed: no steering
    else:
        maxSpeed = rules.maxSpeed
        control = 0.25 if s.stun > TIMER_EPS else 1.0
        dvx, dvy = ix * maxSpeed - s.vx, iy * maxSpeed - s.vy
        dlen = math.sqrt(dvx * dvx + dvy * dvy)
        maxDelta = rules.accel * control * dt
        if s.vx * s.vx + s.vy * s.vy > maxSpeed * maxSpeed + FAST_EPS:
            maxDelta = max(maxDelta, rules.accel * 2.5 * dt)
        if dlen > maxDelta and dlen > 0.0:
            dvx *= maxDelta / dlen
            dvy *= maxDelta / dlen
        s.vx += dvx
        s.vy += dvy

    s.x += s.vx * dt
    s.y += s.vy * dt
    resolveCollisions(s, rules.playerRadius, pillars, arenaRadius)

    if s.vx * s.vx + s.vy * s.vy > 0.25:
        s.heading = math.degrees(math.atan2(-s.vx, s.vy))
    return dashed


def applyKnockback(s, vx, vy, stunTime=STUN_TIME):
    s.vx, s.vy = vx, vy
    s.dashLeft = 0.0
    s.stun = max(s.stun, stunTime)


def resolveCollisions(s, playerRadius, pillars, arenaRadius):
    """Keeps the player inside the arena and out of pillars; bounces."""
    for px, py, pr in pillars:
        dx, dy = s.x - px, s.y - py
        minDist = pr + playerRadius
        d2 = dx * dx + dy * dy
        if d2 >= minDist * minDist:
            continue
        d = math.sqrt(d2)
        if d < 1e-4:
            dx, dy, d = 1.0, 0.0, 1.0
        nx, ny = dx / d, dy / d
        s.x, s.y = px + nx * minDist, py + ny * minDist
        vn = s.vx * nx + s.vy * ny
        if vn < 0.0:
            s.vx -= nx * vn * (1.0 + BOUNCE)
            s.vy -= ny * vn * (1.0 + BOUNCE)

    limit = arenaRadius - playerRadius
    d = math.sqrt(s.x * s.x + s.y * s.y)
    if d > limit and d > 0.0:
        nx, ny = s.x / d, s.y / d
        s.x, s.y = nx * limit, ny * limit
        vn = s.vx * nx + s.vy * ny
        if vn > 0.0:
            s.vx -= nx * vn * (1.0 + BOUNCE)
            s.vy -= ny * vn * (1.0 + BOUNCE)


def separateFrom(s, ox, oy, minDist):
    """Pushes s half the overlap out of a circle (another player). Returns
    True if they overlapped."""
    dx, dy = s.x - ox, s.y - oy
    d2 = dx * dx + dy * dy
    if d2 >= minDist * minDist or d2 < 1e-8:
        return False
    d = math.sqrt(d2)
    push = (minDist - d) * 0.5
    s.x += dx / d * push
    s.y += dy / d * push
    return True


def heldFor(dashLeft):
    """Movement.cpp heldFor(): how much longer a dash holds its velocity."""
    if dashLeft <= 0.0:
        return 0.0
    return math.ceil(dashLeft / PHYSICS_STEP - 1e-4) * PHYSICS_STEP


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
            a = accel * (2.5 if vx * vx + vy * vy > maxSpeed * maxSpeed + FAST_EPS else 1.0)
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


class DeadReckoning:
    """DeadReckoning.cpp: the AI sends a player's sample only when the
    others' prediction from the last one would go wrong: it is already more
    than 12 cm or 1 m/s off, or would be within the next 0.15 s (the input
    changed). So a tap costs two samples, a straight run a heartbeat every
    0.5 s, standing still nothing."""

    MIN_INTERVAL = 0.05       # s: at most 20 samples per second
    MAX_INTERVAL = 0.5        # s: at least this often while moving
    MAX_POSITION_ERROR = 0.12  # m
    MAX_VELOCITY_ERROR = 1.0   # m/s
    LOOKAHEAD = 0.15          # s: changes that would show by then go now

    def __init__(self):
        self.sample = None
        self.last = -1e9

    def predict(self, t, maxSpeed, accel):
        age = t - self.sample[0]
        if age < MAX_EXTRAPOLATION:
            return predictMotion(self.sample, age, maxSpeed, accel)
        x, y, _, _ = predictMotion(self.sample, MAX_EXTRAPOLATION, maxSpeed, accel)
        return x, y, 0.0, 0.0

    def due(self, real, maxSpeed, accel):
        """`real`: the player's sample at its time real[0]."""
        if self.sample is None:
            return True
        t = real[0]
        since = t - self.last
        if since < self.MIN_INTERVAL:
            return False
        px, py, pvx, pvy = self.predict(t, maxSpeed, accel)
        if math.hypot(real[1] - px, real[2] - py) > self.MAX_POSITION_ERROR or \
                math.hypot(real[3] - pvx, real[4] - pvy) > self.MAX_VELOCITY_ERROR:
            return True
        ax, ay, _, _ = self.predict(t + self.LOOKAHEAD, maxSpeed, accel)
        rx, ry, _, _ = predictMotion(real, self.LOOKAHEAD, maxSpeed, accel)
        if math.hypot(rx - ax, ry - ay) > self.MAX_POSITION_ERROR:
            return True
        moving = real[3] != 0.0 or real[4] != 0.0 or real[6] != 0.0 or real[7] != 0.0
        return moving and since >= self.MAX_INTERVAL

    def sent(self, sample):
        """`sample`: as receivers read it (quantised)."""
        self.sample = sample
        self.last = sample[0]


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
        last = lastT = -1e18
        for s in track:
            if s[0] <= lastT:  # strictly increasing times, as RemoteMotion keeps them
                s = (lastT + 1e-3,) + tuple(s[1:])
            lastT = s[0]
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
