"""Unit tests of the AI's lag-compensated dash hits
(DistributedArenaGameAI._record / _checkDashHits and arena/motion.py).

A dash counts when the attacker touched the victim *on the attacker's
screen*. Clients draw others with RemoteMotion: a view delay in the past,
predicting each player's motion from its newest sample (position, velocity
and held input) and blending each new sample in from where the drawing was.
motion.Drawing replays that. These tests build the situation by hand with a
fake clock and fake network timestamps.

    cd server && python -m unittest tests.test_lagcomp -v
"""

import math
import os
import sys
import unittest
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from direct.distributed.ClockDelta import globalClockDelta  # noqa: E402

from arena import motion  # noqa: E402
from arena.DistributedArenaGameAI import DistributedArenaGameAI, ROUND_PLAYING  # noqa: E402
from arena.DistributedArenaPlayerAI import DistributedArenaPlayerAI  # noqa: E402
from arena.rules import Rules  # noqa: E402


class FakePlayer:
    """The attributes of DistributedArenaPlayerAI the game logic uses."""
    sampleTime = staticmethod(DistributedArenaPlayerAI.sampleTime)

    def __init__(self, doId, slot):
        self.doId = doId
        self.slot = slot
        self.ownerId = doId * 10
        self.name = 'P%d' % doId
        self.validX = self.validY = 0.0
        self.heading = 0.0
        self.lastMoveTime = None
        self.moveBudget = 0.0
        self.lastCorrection = -1e9
        self.lastDash = -1e9
        self.dashHits = set()
        self.lastHitTime = -1e9
        self.track = deque()
        self.latency = None
        self.viewDelay = 0.0
        self.dashStart = self.dashEnd = -1e9


class FakeAir:
    class AnyClass(dict):
        def __missing__(self, key):
            return None

    dclassesByName = AnyClass()


class Game(DistributedArenaGameAI):
    """The real game logic with a settable clock and recorded messages."""

    def __init__(self):
        self.clock = 1000.0
        DistributedArenaGameAI.__init__(self, FakeAir(), rules=Rules(numOrbs=0), seed=1)
        self.sent = []

    def now(self):
        return self.clock

    def sendUpdate(self, field, args, *_):
        self.sent.append((field, args))

    def sendUpdateToAvatarId(self, avId, field, args):
        self.sent.append((field, args))

    def hits(self):
        return [args[:2] for field, args in self.sent if field == 'playerHit']


def ts(t):
    """16-bit network timestamp of AI time t (the AI's clock is network time)."""
    return globalClockDelta.localToNetworkTime(t, bits=16)


class LagCompensationTest(unittest.TestCase):
    def setUp(self):
        self.game = Game()
        self.game.roundState = ROUND_PLAYING
        self.attacker = FakePlayer(1, slot=0)
        self.victim = FakePlayer(2, slot=1)
        for p in (self.attacker, self.victim):
            self.game.players[p.doId] = p
            self.game.scores[p.doId] = 10

    def move(self, player, t, x, y, vx, vy, latency, held=None):
        """`player` was at (x, y) moving (vx, vy) at time t, holding input
        `held` (default: toward where it moves); arrives `latency` later."""
        if held is None:
            speed = math.hypot(vx, vy)
            held = (vx / speed, vy / speed) if speed > 0 else (0.0, 0.0)
        self.game.clock = t + latency
        self.game.onPlayerMoved(player, x, y, vx, vy, ts(t), held[0], held[1])

    def dash(self, player, t, latency):
        self.game.clock = t + latency
        self.game.onPlayerDash(player, ts(t))

    def drawing(self, player, down, delay):
        r = self.game.rules
        return motion.Drawing(player.track, down, delay, r.maxSpeed, r.accel)

    def test_drawing_predicts_from_what_had_arrived(self):
        v = self.victim
        for t, x in ((10.0, 0.0), (10.1, 0.9), (10.2, 1.8)):
            self.move(v, t, x, 0.0, 9.0, 0.0, latency=0.05)
        now = self.drawing(v, 0.05, 0.0)  # viewer 50 ms from us, no view delay
        self.assertAlmostEqual(now.at(10.5)[0], 4.5, places=3)
        # The 10.2 sample reached the viewer at 10.3: before that, the 10.1 one.
        self.assertAlmostEqual(now.at(10.29)[0], 0.9 + 9.0 * 0.19, places=3)
        late = self.drawing(v, 0.05, 0.1)  # the same, drawn 0.1 s in the past
        self.assertAlmostEqual(late.at(10.4)[0], 3.6, places=3)
        # Never more than MAX_EXTRAPOLATION past the newest sample.
        self.assertAlmostEqual(now.at(30.0)[0], 1.8 + 9.0 * motion.MAX_EXTRAPOLATION, places=3)

    def test_braking_is_predicted_from_the_released_input(self):
        v = self.victim
        self.move(v, 10.0, 0.0, 0.0, 9.0, 0.0, latency=0.0, held=(0.0, 0.0))  # let go
        d = self.drawing(v, 0.0, 0.0)
        a, h = self.game.rules.accel, motion.PHYSICS_STEP
        # Braking at `accel` in 120 Hz steps (each moves with its new speed).
        self.assertAlmostEqual(d.at(10.1)[0], 0.9 - 0.5 * a * 0.1 * (0.1 + h), places=4)
        T = 9.0 / a
        self.assertAlmostEqual(d.at(11.0)[0], 9.0 * T - 0.5 * a * T * (T + h), places=4)

    def test_a_late_turn_is_blended_in_smoothly(self):
        v = self.victim
        self.move(v, 10.0, 0.0, 0.0, 9.0, 0.0, latency=0.0)    # running along +x
        # A turn to +y at 10.4 arrives 0.1 s late, at 10.5, and the viewer
        # draws without delay: it ran past the corner and blends back.
        self.move(v, 10.4, 3.6, 0.0, 0.0, 9.0, latency=0.1)
        d = self.drawing(v, 0.0, 0.0)
        x0, y0 = d.at(10.5)
        self.assertAlmostEqual(x0, 4.5, places=3)  # no jump when it arrives
        self.assertAlmostEqual(y0, 0.0, places=3)
        last, fastest = (x0, y0), 0.0
        for k in range(1, 61):
            p = d.at(10.5 + k / 120.0)
            fastest = max(fastest, math.hypot(p[0] - last[0], p[1] - last[1]) * 120.0)
            last = p
        # A curve, not a snap (one frame would be 150 m/s): the player's own
        # speed, the correction, and a quarter of the turn's velocity change
        # that a cubic adds mid-curve.
        self.assertLess(fastest, 9.0 + motion.MAX_CORRECTION_SPEED + 0.25 * 12.8)
        x2, y2 = d.at(11.2)  # on the new path after the blend
        self.assertAlmostEqual(x2, 3.6, places=3)
        self.assertAlmostEqual(y2, 7.2, places=3)

    def test_with_a_view_delay_a_tap_is_drawn_as_it_happened(self):
        v = self.victim
        a, h = self.game.rules.accel, motion.PHYSICS_STEP

        def real(t):  # the movement code's tap: 0.1 s of input, then braking
            if t <= 0.1:
                return 0.5 * a * t * (t + h)
            b = min(t - 0.1, 0.1)
            return 0.5 * a * 0.1 * (0.1 + h) + 4.2 * b - 0.5 * a * b * (b + h)

        # A short tap along +x: pressed at 10.0, let go at 10.1 at 4.2 m/s.
        self.move(v, 10.0, 0.0, 0.0, 0.0, 0.0, latency=0.04, held=(1.0, 0.0))
        self.move(v, 10.1, real(0.1), 0.0, 4.2, 0.0, latency=0.04, held=(0.0, 0.0))
        d = self.drawing(v, 0.0, 0.08)  # samples 40 ms late, drawn 80 ms back
        xs = [d.at(10.0 + k / 120.0) for k in range(60)]
        for (x, _), k in zip(xs, range(60)):
            # Exactly the real motion: accelerating, then braking to a stop.
            self.assertAlmostEqual(x, real(k / 120.0), places=4)
        self.assertTrue(all(b[0] >= a[0] - 1e-9 for a, b in zip(xs, xs[1:])), 'drawn moving back')

    def run_dash(self, victimState, latencyA, latencyV):
        """Attacker dashes along +x from x=0 at t=10; victimState(t) -> (x, y, vx, vy)."""
        for k in range(-6, 1):  # history before the dash
            t = 10.0 + k * 0.05
            self.move(self.victim, t, *victimState(t), latency=latencyV)
            self.move(self.attacker, t, 0.0, 0.0, 0.0, 0.0, latency=latencyA)
        self.dash(self.attacker, 10.0, latencyA)
        for k in range(1, 6):
            t = 10.0 + k * 0.05
            self.move(self.victim, t, *victimState(t), latency=latencyV)
            self.move(self.attacker, t, 24.0 * (t - 10.0), 0.0, 24.0, 0.0, latency=latencyA)

    def test_hit_judged_on_the_attackers_screen_despite_latency(self):
        # The victim runs toward the attacker's dash line and stops 2.25 m short
        # of it at t=9.95, out of reach. The attacker (150 ms away) cannot know
        # yet: on its screen the victim keeps coming and gets hit at x=3.
        def runThenStop(t):
            if t < 9.95:
                return 3.0, -4.5 + 9.0 * (t - 9.7), 0.0, 9.0
            return 3.0, -2.25, 0.0, 0.0  # (braking is not modelled: a sudden stop)

        self.run_dash(runThenStop, latencyA=0.15, latencyV=0.15)
        self.assertEqual(self.game.hits(), [[0, 1]])

    def test_no_hit_when_the_attacker_saw_a_miss(self):
        # The victim stands 2.5 m to the side: never within reach on screen.
        self.run_dash(lambda t: (3.0, 2.5, 0.0, 0.0), latencyA=0.1, latencyV=0.1)
        self.assertEqual(self.game.hits(), [])

    def test_old_timestamps_cannot_rewind_further_than_the_cap(self):
        g, a = self.game, self.attacker
        g.clock = 50.0
        t = a.sampleTime(ts(48.0), g.clock, g.MAX_REWIND)  # claims 2 s ago
        self.assertAlmostEqual(t, 50.0 - g.MAX_REWIND, places=2)
        t = a.sampleTime(ts(51.0), g.clock, g.MAX_REWIND)  # from the future
        self.assertAlmostEqual(t, 50.0, places=2)

    def test_absurd_velocities_are_capped(self):
        self.move(self.victim, 10.0, 0.0, 0.0, 500.0, 0.0, latency=0.0)
        vx = self.victim.track[-1][3]
        r = self.game.rules
        self.assertAlmostEqual(vx, r.dashSpeed + r.knockbackSpeed, places=3)

    def test_immunity_as_the_attacker_saw_it(self):
        def standing(t):
            return 3.0, 0.0, 0.0, 0.0

        # The victim was hit (by someone else) 0.1 s before the attacker's
        # dash reaches it; the attacker, 0.15 s away, had not heard of it yet.
        self.victim.lastHitTime = 10.0 + 3.0 / 24.0 - 0.1 + 0.15
        self.run_dash(standing, latencyA=0.15, latencyV=0.05)
        self.assertEqual(self.game.hits(), [[0, 1]])

    def test_immunity_blocks_when_the_attacker_knew(self):
        def standing(t):
            return 3.0, 0.0, 0.0, 0.0

        self.victim.lastHitTime = 9.5  # news reached the attacker well before
        self.run_dash(standing, latencyA=0.15, latencyV=0.05)
        self.assertEqual(self.game.hits(), [])

    def test_speed_check_still_uses_arrival_times(self):
        # Timestamps spaced 0.4 s apart cannot buy extra distance when the
        # messages arrive back to back.
        g, a = self.game, self.attacker
        self.move(a, 10.0, 0.0, 0.0, 0.0, 0.0, latency=0.0)
        g.clock = 10.01
        a.moveBudget = 0.0
        g.onPlayerMoved(a, 8.0, 0.0, 0.0, 0.0, ts(10.01 + 0.4))
        self.assertEqual(a.validX, 0.0)
        self.assertIn('correctPosition', [f for f, _ in g.sent])


if __name__ == '__main__':
    unittest.main()
