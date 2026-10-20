"""The AI's simulation of one player, from that player's inputs.

Inputs name the 120 Hz step they start at, on the network clock, and the
client predicts its own player with exactly those inputs at exactly those
steps. The AI runs each player a little behind real time (`lag` steps: as
late as that client's recent inputs arrived, but for the latest one, plus a
step), so nearly every input is there before its step is simulated and the AI computes the same motion as the client did,
step for step. An input later than that is applied at the next step to
simulate instead, and the client is told where it really is (SELF).

What the AI sends about a player (motion.py, protocol.py):
* samples (STATE) for everyone else, only when their prediction from the
  last one would go wrong (motion.DeadReckoning);
* SELF for the player's own client, when the client's prediction may differ:
  a knockback, contact with another player, an input applied late, the
  spawn, and once a second while moving to wash out rounding.
"""

from collections import deque

from . import motion
from .protocol import (
    FLAG_DASH, FLAG_KNOCK, INPUT_DASH, INPUT_HELD, SELF_KNOCK, SELF_SNAP, STEP, STEPS_PER_SECOND,
    dirVector, packSelf, packState, resolveStep, unpackSelf, unpackState, viewDelay)


class PlayerSim:
    LAG_START = 24          # steps behind real time before any input arrived (200 ms)
    LAG_MIN, LAG_MAX = 3, 36
    LAG_MARGIN = 1          # steps above the latest but one of the recent inputs
    LAG_DECAY = 0.05        # s per step the lag may shrink (it grows at once)
    MAX_AHEAD = 24          # steps an input may claim to be in the future (clock error)
    MAX_QUEUE = 64
    SELF_INTERVAL = 0.1     # s between corrections while they keep coming
    SELF_HEARTBEAT = 1.0    # s between checks while moving
    TRACK_SECONDS = 1.0     # broadcast samples kept for lag compensation
    KNOW_MARGIN = 4         # steps of slack in knowDelay (a busy server or client)
    SAMPLE_DELAY_MARGIN = 2  # steps knowDelay stays above the samples' delay the client sees
    MAX_SAMPLE_DELAY = 120   # steps of it believed (a client could claim anything)
    CLIENT_FRAME_WAIT = 1.0 / 120.0  # s a sample waits for the client's next frame, on average
    KNOW_START = 24         # steps, until we know how late this client's data is
    HISTORY_STEPS = 150     # simulated positions kept for hit tests

    def __init__(self, state, step, now):
        self.state = state
        self.step = step              # the state is at the end of this step
        self.ix = self.iy = 0.0       # steering input in effect
        self.dashPressed = False
        self.queue = deque()          # (seq, step, flags, dir) not yet simulated
        self.lastInputStep = step
        self.ack = 0                  # seq of the newest input applied
        self.lateness = deque(maxlen=32)  # steps, per input
        self.lag = self.LAG_START
        self.lagChanged = now
        self.latency = None           # s: one-way client -> AI, smoothed
        self.viewDelay = 0.06         # s the client draws others in the past
        self.sampleDelay = 0          # steps the client sees samples arrive after their sent step

        self.knowDelay = self.knowDelayBefore = self.KNOW_START
        self.knowDelayFrom = step
        self.pendingKnowDelay = None

        self.dr = motion.DeadReckoning()
        self.track = deque()          # samples as the others got them (motion.Drawing)
        self.history = deque()        # (step, x, y) after each simulated step
        self.history.append((step, state.x, state.y))
        self.forced = 0               # event flags (FLAG_DASH/KNOCK) for the next sample
        self.dashStep = -1            # step the newest dash started in

        self.selfFlags = SELF_SNAP    # SELF wanted: flags, or None
        self.selfCorrect = True
        self.lastSelf = -1e9
        self.movedSinceSelf = False
        self.lastLate = None
        self.stats = {'inputs': 0, 'late': 0, 'samples': 0, 'selfs': 0, 'contacts': 0}

    # ------------------------------------------------------------ inputs

    def onInput(self, seq, step16, flags, direction, sampleDelay, nowStep, now):
        """One INPUT as it arrived."""
        self.sampleDelay = min(sampleDelay, self.MAX_SAMPLE_DELAY)
        step = resolveStep(step16, nowStep)
        late = nowStep - step
        self.lateness.append(max(0, min(late, 2 * self.LAG_MAX)))
        self.latency = max(0, late) * STEP if self.latency is None else \
            self.latency * 0.9 + max(0, late) * STEP * 0.1
        if step > nowStep + self.MAX_AHEAD:
            step = nowStep  # a client clock far ahead: not believed
        step = max(step, self.lastInputStep)  # inputs never go back in time
        self.lastInputStep = step
        self.viewDelay = viewDelay(flags)
        if len(self.queue) >= self.MAX_QUEUE:
            self.queue.popleft()
        self.queue.append((seq, step, flags, direction))
        self.stats['inputs'] += 1
        self._adaptLag(now, nowStep)

    def _adaptLag(self, now, nowStep):
        # The latest but one: a lone spike is applied late (and corrected)
        # rather than holding everyone's view of this player back for long.
        late = sorted(self.lateness)[-2] if len(self.lateness) > 1 else self.lateness[0]
        target = min(self.LAG_MAX, max(self.LAG_MIN, late + self.LAG_MARGIN))
        if target > self.lag:
            self.lag = target
            self.lagChanged = now
        elif target < self.lag and now - self.lagChanged >= self.LAG_DECAY:
            self.lag -= 1
            self.lagChanged = now

    def targetStep(self, nowStep, now):
        if self.lateness:
            self._adaptLag(now, nowStep)
        return nowStep - self.lag

    def _takeInputs(self, k):
        """Inputs that start at step k (or should have started earlier)."""
        queue = self.queue
        while queue and queue[0][1] <= k:
            seq, step, flags, direction = queue.popleft()
            if step < k:
                self.stats['late'] += 1
                self.lastLate = (seq, step, k, self.lag)
                self.wantSelf()  # the client applied it earlier than we can
            if flags & INPUT_HELD:
                self.ix, self.iy = dirVector(direction)
            else:
                self.ix = self.iy = 0.0
            if flags & INPUT_DASH:
                self.dashPressed = True
            self.ack = seq

    # --------------------------------------------------------- simulation

    def stepOnce(self, rules, pillars, arenaRadius):
        """Simulates the next step. Returns (k, dashed, x0, y0)."""
        k = self.step + 1
        self._takeInputs(k)
        s = self.state
        x0, y0 = s.x, s.y
        dashed = motion.stepMovement(s, self.ix, self.iy, self.dashPressed, rules, pillars, arenaRadius)
        self.dashPressed = False
        self.step = k
        if dashed:
            self.forced |= FLAG_DASH
            self.dashStep = k
        if s.vx or s.vy:
            self.movedSinceSelf = True
        return k, dashed, x0, y0

    def record(self):
        self.history.append((self.step, self.state.x, self.state.y))
        while len(self.history) > self.HISTORY_STEPS:
            self.history.popleft()

    def knockedBack(self, vx, vy):
        """A hit: the knockback applies from the next step, here and in the
        client's prediction (SELF), and everyone sees it start (FLAG_KNOCK)."""
        motion.applyKnockback(self.state, vx, vy)
        self.forced |= FLAG_KNOCK
        self.selfFlags = (self.selfFlags or 0) | SELF_KNOCK

    def wantSelf(self):
        self.selfCorrect = True

    def knownAt(self, k, knowDelay, maxSpeed, accel):
        """Where a client following the rule `knowDelay` (protocol.py SELF)
        has us at step k: our newest sample sent at least knowDelay steps
        before k, predicted to k (RemoteMotion::predict). None before that."""
        cutoff = k - knowDelay
        for sample in reversed(self.track):
            if sample[9] <= cutoff:
                return motion.predict(sample, k * STEP, maxSpeed, accel)[:2]
        return None

    def knowDelayAt(self, k):
        """The knowDelay this player's client follows at step k."""
        return self.knowDelay if k >= self.knowDelayFrom else self.knowDelayBefore

    def chooseKnowDelay(self):
        """Steps the client waits before it bumps into what a sample says: how
        late samples reach it, plus a margin, so it has them by then. Changed
        rarely, and through SELF, so both sides switch at the same step.

        Our inputs' latency says how long the way back takes only if the client's
        clock is exactly ours; it is off by up to half a round trip, so the
        client also says how late (by its clock) the samples really arrive."""
        down = (0.1 if self.latency is None else self.latency) + self.CLIENT_FRAME_WAIT
        needed = min(255, max(int(down * STEPS_PER_SECOND) + 1 + self.KNOW_MARGIN,
                              self.sampleDelay + 1 + self.SAMPLE_DELAY_MARGIN))
        target = self.pendingKnowDelay if self.pendingKnowDelay is not None else self.knowDelay
        if needed > target or needed < target - 2 * self.KNOW_MARGIN:
            self.pendingKnowDelay = needed
            self.wantSelf()

    def positionAt(self, t):
        """Simulated position at time t (s, network clock), between steps."""
        history = self.history
        k = t / STEP
        if k <= history[0][0]:
            return history[0][1], history[0][2]
        if k >= history[-1][0]:
            return history[-1][1], history[-1][2]
        i = int(k) - history[0][0]
        i = max(0, min(i, len(history) - 2))
        a, b = history[i], history[i + 1]
        f = (k - a[0]) / (b[0] - a[0])
        return a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f

    # ------------------------------------------------------------ messages

    def sampleDue(self, rules, now, sentStep):
        """STATE bytes to send to everyone else now (step `sentStep`), or None."""
        s = self.state
        forced, self.forced = self.forced, 0
        step, x, y = self.step, s.x, s.y
        if forced & FLAG_KNOCK:
            forced = FLAG_KNOCK  # a knockback ends any dash
        elif forced & FLAG_DASH:
            # Rewound to the dash's start: its velocity is held from there.
            since = self.step - self.dashStep + 1
            step -= since
            x -= s.vx * since * STEP
            y -= s.vy * since * STEP
        else:
            real = (self.step * STEP, s.x, s.y, s.vx, s.vy, now, self.ix, self.iy,
                    motion.heldFor(s.dashLeft))
            if not self.dr.due(real, rules.maxSpeed, rules.accel):
                return None
        data = packState(x, y, s.vx, s.vy, forced, step, sentStep, self.ix, self.iy, s.heading)
        x, y, vx, vy, flags, step, ix, iy, _ = unpackState(data, 0, sentStep)
        t = step * STEP
        hold = motion.dashHoldTime(rules.dashTime) if flags & FLAG_DASH else 0.0
        sample = (t, x, y, vx, vy, now, ix, iy, hold, sentStep)
        self.dr.sent(sample)
        self.track.append(sample)
        # Keep the newest sample older than that too: it is what a client
        # still goes by until a newer one counts (knownAt, motion.Drawing).
        while len(self.track) > 2 and self.track[1][5] < now - self.TRACK_SECONDS:
            self.track.popleft()
        self.stats['samples'] += 1
        return data

    def selfDue(self, now):
        """SELF bytes for the player's own client now, or None. The AI then
        continues from exactly what it sent, as the client will."""
        flags = self.selfFlags
        if flags is None:
            if self.selfCorrect:
                if now - self.lastSelf < self.SELF_INTERVAL:
                    return None
            elif not (self.movedSinceSelf and now - self.lastSelf >= self.SELF_HEARTBEAT):
                return None
            flags = 0
        s = self.state
        if self.pendingKnowDelay is not None:
            # The client follows the new rule from the next step on; so do we.
            self.knowDelayBefore = self.knowDelay
            self.knowDelay = self.pendingKnowDelay
            self.knowDelayFrom = self.step + 1
            self.pendingKnowDelay = None
        data = packSelf(self.ack, self.step, s.x, s.y, s.vx, s.vy, s.heading, s.dashLeft,
                        s.dashCooldown, s.stun, flags, self.knowDelay)
        _, _, s.x, s.y, s.vx, s.vy, s.heading, s.dashLeft, s.dashCooldown, s.stun, _, _ = \
            unpackSelf(data)
        self.selfFlags = None
        self.selfCorrect = False
        self.movedSinceSelf = False
        self.lastSelf = now
        self.stats['selfs'] += 1
        return data
