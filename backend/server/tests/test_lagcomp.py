"""Unit tests of the AI's simulation, lag-compensated dash hits and
collisions (DistributedArenaGameAI, simulation.PlayerSim, motion.py).

The AI moves every player from that player's inputs. A dash counts when the
attacker touched the victim *on the attacker's screen*: clients draw others
with RemoteMotion, a view delay in the past, predicting each player's motion
from its newest sample and blending each new sample in; motion.Drawing
replays that. These tests drive the real game logic with a fake clock and
inputs that arrive as late as the test says.

    cd server && python -m unittest tests.test_lagcomp -v
"""

import math
import os
import sys
import unittest
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from arena import motion  # noqa: E402
from arena.DistributedArenaGameAI import DistributedArenaGameAI, ROUND_PLAYING  # noqa: E402
from arena.frame import EVENT_ORB_COLLECTED, EVENT_PLAYER_HIT, unpackAiEvents  # noqa: E402
from arena.protocol import (  # noqa: E402
    INPUT_DASH, INPUT_HELD, STEP, VIEW_DELAY_SHIFT, unpackSelf, wrapStep)
from arena.rules import Rules  # noqa: E402
from arena.simulation import PlayerSim  # noqa: E402


class FakePlayer:
    """The attributes of DistributedArenaPlayerAI the game logic uses."""

    def __init__(self, doId, slot, x, y, clock):
        self.doId = doId
        self.slot = slot
        self.ownerId = doId * 10
        self.name = 'P%d' % doId
        self.lastHitTime = -1e9
        self.lastChat = -1e9
        self.dashStart = self.dashEnd = self.dashChecked = -1e9
        self.dashHits = set()
        self.drawings = {}
        self.sim = PlayerSim(motion.MoveState(x, y), int(clock / STEP) - PlayerSim.LAG_START, clock)
        self.seq = 0


class FakeAir:
    class AnyClass(dict):
        def __missing__(self, key):
            return None

    dclassesByName = AnyClass()

    def __init__(self):
        self.selfs = []
        self.moves = []
        self.frames = []   # (step, [(slot, STATE)], [(slot, SELF)], [(kind, fields)])
        self.log = []      # frames and field updates, in the order they were sent

    def sendFrame(self, step, samples, selfs, events):
        events = unpackAiEvents(b''.join(events), 0, len(events))[0]
        self.frames.append((step, samples, selfs, events))
        self.log.append(('frame', [kind for kind, _ in events]))
        self.selfs.extend((slot, unpackSelf(data)) for slot, data in selfs)
        if samples:
            self.moves.append((step, samples))


class Game(DistributedArenaGameAI):
    """The real game logic with a settable clock and recorded messages."""

    def __init__(self):
        self.clock = 1000.0
        DistributedArenaGameAI.__init__(self, FakeAir(), rules=Rules(numOrbs=0), seed=1)
        self.pillars = []
        self.sent = []

    def now(self):
        return self.clock

    def sendUpdate(self, field, args=[]):
        self._sendEvents()  # as the real one does
        self.sent.append((field, args))
        self.air.log.append(('update', field))

    def sendUpdateToAvatarId(self, avId, field, args):
        self.sent.append((field, args))

    def sentEvents(self, kind):
        """Fields of the events of `kind` sent so far, and of those waiting
        for the next batch."""
        waiting = unpackAiEvents(b''.join(self.events), 0, len(self.events))[0]
        return [list(fields) for frame in self.air.frames for k, fields in frame[3] if k == kind] + \
            [list(fields) for k, fields in waiting if k == kind]

    def hits(self):
        return [fields[:2] for fields in self.sentEvents(EVENT_PLAYER_HIT)]


class LagCompensationTest(unittest.TestCase):
    def setUp(self):
        self.game = Game()
        self.game.roundState = ROUND_PLAYING
        self.pending = []  # (arrival, player, input args)

    def add(self, doId, slot, x, y):
        p = FakePlayer(doId, slot, x, y, self.game.clock)
        self.game.players[p.doId] = p
        self.game.scores[p.doId] = 10
        return p

    def send(self, player, t, latency, held=False, direction=0, dash=False, viewDelay=0.0,
             sampleDelay=0):
        """`player` presses at time t (its step there); it reaches the AI
        `latency` later."""
        player.seq += 1
        flags = (INPUT_HELD if held else 0) | (INPUT_DASH if dash else 0) | \
            int(round(viewDelay / 0.004)) << VIEW_DELAY_SHIFT
        self.pending.append((t + latency, player,
                             (player.seq, wrapStep(int(math.floor(t / STEP))), flags, direction, sampleDelay)))

    def runUntil(self, until, frame=0.01):
        g = self.game
        while g.clock < until - 1e-9:
            g.clock = round(g.clock + frame, 6)
            nowStep = g.nowStep(g.clock)
            for item in [i for i in self.pending if i[0] <= g.clock]:
                self.pending.remove(item)
                item[1].sim.onInput(*item[2], nowStep=nowStep, now=g.clock)
            g._frame(None)

    def warmUp(self, player, latency, viewDelay=0.0):
        """A few inputs that do nothing, so the AI learns the client's
        latency and view delay (as it does from real play)."""
        for k in range(8):
            self.send(player, self.game.clock + 0.05 * k, latency, viewDelay=viewDelay)

    # ----------------------------------------------------- what clients draw

    def drawing(self, samples, down, delay):
        r = self.game.rules
        return motion.Drawing(deque(samples), down, delay, r.maxSpeed, r.accel)

    def test_drawing_predicts_from_what_had_arrived(self):
        track = [(t, x, 0.0, 9.0, 0.0, t + 0.05, 1.0, 0.0, 0.0) for t, x in
                 ((10.0, 0.0), (10.1, 0.9), (10.2, 1.8))]
        now = self.drawing(track, 0.05, 0.0)  # viewer 50 ms from us, no view delay
        self.assertAlmostEqual(now.at(10.5)[0], 4.5, places=3)
        # The 10.2 sample reached the viewer at 10.3: before that, the 10.1 one.
        self.assertAlmostEqual(now.at(10.29)[0], 0.9 + 9.0 * 0.19, places=3)
        late = self.drawing(track, 0.05, 0.1)  # the same, drawn 0.1 s in the past
        self.assertAlmostEqual(late.at(10.4)[0], 3.6, places=3)
        # Never more than MAX_EXTRAPOLATION past the newest sample.
        self.assertAlmostEqual(now.at(30.0)[0], 1.8 + 9.0 * motion.MAX_EXTRAPOLATION, places=3)

    def test_braking_is_predicted_from_the_released_input(self):
        d = self.drawing([(10.0, 0.0, 0.0, 9.0, 0.0, 10.0, 0.0, 0.0, 0.0)], 0.0, 0.0)  # let go
        a, h = self.game.rules.accel, motion.PHYSICS_STEP
        # Braking at `accel` in 120 Hz steps (each moves with its new speed).
        self.assertAlmostEqual(d.at(10.1)[0], 0.9 - 0.5 * a * 0.1 * (0.1 + h), places=4)
        T = 9.0 / a
        self.assertAlmostEqual(d.at(11.0)[0], 9.0 * T - 0.5 * a * T * (T + h), places=4)

    def test_a_late_turn_is_blended_in_smoothly(self):
        # Running along +x; a turn to +y at 10.4 arrives 0.1 s late, at 10.5,
        # and the viewer draws without delay: it ran past the corner and
        # blends back.
        d = self.drawing([(10.0, 0.0, 0.0, 9.0, 0.0, 10.0, 1.0, 0.0, 0.0),
                          (10.4, 3.6, 0.0, 0.0, 9.0, 10.5, 0.0, 1.0, 0.0)], 0.0, 0.0)
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

    # ------------------------------------------------- the AI's simulation

    def test_inputs_apply_at_the_step_the_client_named(self):
        p = self.add(1, 0, 0.0, 0.0)
        self.warmUp(p, latency=0.08)
        self.runUntil(1001.0)
        start = self.game.clock
        self.send(p, start, latency=0.08, held=True, direction=0)       # run +x ...
        self.send(p, start + 0.25, latency=0.08, held=False)            # ... for 0.25 s
        self.runUntil(start + 1.0)
        # The same as the movement code with that input from exactly that step.
        s = motion.MoveState()
        k0 = int(math.floor(start / STEP))
        for k in range(k0 - 200, p.sim.step + 1):
            held = k0 <= k < int(math.floor((start + 0.25) / STEP))
            if k > k0 - 200:
                motion.stepMovement(s, 1.0 if held else 0.0, 0.0, False, self.game.rules, [], 34.0)
        self.assertAlmostEqual(p.sim.state.x, s.x, delta=0.006)  # SELF rounds to cm
        self.assertEqual(p.sim.stats['late'], 0)

    def test_a_late_input_is_applied_at_the_next_step_and_reported(self):
        p = self.add(1, 0, 0.0, 0.0)
        self.warmUp(p, latency=0.05)
        self.runUntil(1001.0)
        self.game.air.selfs.clear()
        self.send(p, self.game.clock, latency=0.5, held=True)  # far later than the others
        self.runUntil(self.game.clock + 0.7)
        self.assertEqual(p.sim.stats['late'], 1)
        self.assertTrue(self.game.air.selfs, 'the client was not told where it really is')

    def test_spam_cannot_beat_the_rules(self):
        p = self.add(1, 0, 0.0, 0.0)
        t = self.game.clock
        for k in range(200):  # an input every 5 ms, all dashing, claiming any time
            self.send(p, t + k * 0.005 - (0.5 if k % 2 else -0.3), latency=0.0, held=True,
                      direction=(k * 41) & 0xff, dash=True)
        self.runUntil(t + 1.0)
        r = self.game.rules
        dist = math.hypot(p.sim.state.x, p.sim.state.y)
        self.assertLess(dist, r.maxSpeed * 1.3 + r.dashSpeed * r.dashTime + 0.5)

    # ----------------------------------------------------- lag compensation

    def run_dash(self, victimPlan, latencyA, latencyV, delayA=0.0):
        """The whole way, from inputs: the attacker at (0, 0) dashes along +x
        at t0 + 1; victimPlan(victim, tDash) sends the victim's inputs."""
        g = self.game
        attacker = self.add(1, 0, 0.0, 0.0)
        victim = self.add(2, 1, 3.0, 0.0)
        self.warmUp(attacker, latencyA, delayA)
        self.warmUp(victim, latencyV)
        t0 = g.clock
        victimPlan(victim, t0 + 1.0)
        self.send(attacker, t0 + 1.0, latencyA, held=True, direction=0, dash=True, viewDelay=delayA)
        self.runUntil(t0 + 2.0)
        return attacker, victim

    def judge(self, victimSamples, latencyA, delayA=0.0):
        """The attacker dashes from (0, 0) along +x at 24 m/s from t=10 (its
        simulated path); the victim's samples as the AI sent them. Returns
        the hits."""
        g = self.game
        g.clock = 10.6
        attacker = self.add(1, 0, 0.0, 0.0)
        victim = self.add(2, 1, 3.0, 0.0)
        sim = attacker.sim
        sim.history = deque((k, max(0.0, 24.0 * (k * STEP - 10.0)), 0.0)
                            for k in range(int(9.5 / STEP), int(10.5 / STEP)))
        sim.step = sim.history[-1][0]
        sim.latency, sim.viewDelay = latencyA, delayA
        attacker.dashStart = attacker.dashChecked = 10.0
        attacker.dashEnd = 10.0 + g.rules.dashTime + 0.05
        victim.sim.track = deque(victimSamples)
        g._checkDashHits(attacker, g.clock)
        return g.hits()

    def test_hit_judged_on_the_attackers_screen_despite_latency(self):
        # The victim runs toward the attacker's dash line and stops 2.25 m short
        # of it at t=9.95, out of reach. The attacker (150 ms away) cannot know
        # yet: on its screen the victim keeps coming and gets hit at x=3.
        late = 0.15
        run = [(t, 3.0, -4.5 + 9.0 * (t - 9.7), 0.0, 9.0, t + 0.01, 0.0, 1.0, 0.0)
               for t in (9.7, 9.75, 9.8, 9.85, 9.9)]
        stop = (9.95, 3.0, -2.25, 0.0, 0.0, 9.95 + late, 0.0, 0.0, 0.0)  # (a sudden stop)
        self.assertEqual(self.judge(run + [stop], latencyA=late), [[0, 1]])

    def test_no_hit_when_the_attacker_saw_the_stop(self):
        # The same stop, but news of it reached the attacker before its dash.
        stop = (9.6, 3.0, -2.25, 0.0, 0.0, 9.61, 0.0, 0.0, 0.0)
        self.assertEqual(self.judge([stop], latencyA=0.15), [])

    def test_no_hit_when_the_attacker_saw_a_miss(self):
        # The victim stands 2.5 m to the side: never within reach on screen.
        side = (9.0, 3.0, 2.5, 0.0, 0.0, 9.0, 0.0, 0.0, 0.0)
        self.assertEqual(self.judge([side], latencyA=0.1), [])

    def test_a_hit_knocks_the_victim_back_and_tells_it(self):
        attacker, victim = self.run_dash(lambda v, t: None, latencyA=0.05, latencyV=0.05)
        self.assertEqual(self.game.hits(), [[0, 1]])
        knocks = [m for slot, m in self.game.air.selfs if slot == victim.slot and m[10] & 1]
        self.assertEqual(len(knocks), 1, 'the victim got no knockback SELF')
        self.assertGreater(victim.sim.state.x, 3.0)  # pushed along the dash

    def test_immunity_blocks_when_the_attacker_knew(self):
        def standing(victim, tDash):
            victim.lastHitTime = tDash - 0.5  # news reached the attacker well before
        self.run_dash(standing, latencyA=0.15, latencyV=0.05)
        self.assertEqual(self.game.hits(), [])

    # ----------------------------------------------------------- collisions

    def test_bumps_use_what_the_client_knew_by_the_rule(self):
        g = self.game
        a = self.add(1, 0, 0.0, 0.0)
        b = self.add(2, 1, 2.5, 0.0)
        self.warmUp(a, 0.05)
        self.warmUp(b, 0.05)
        self.runUntil(g.clock + 1.0)
        know = a.sim.knowDelay
        k = a.sim.step + 1
        # b's newest sample counts for a only `know` steps after it was sent.
        newest = b.sim.track[-1]
        self.assertLessEqual(newest[9] + know, k + know)
        self.assertIsNotNone(b.sim.knownAt(newest[9] + know, know, g.rules.maxSpeed, g.rules.accel))
        self.assertIsNone(b.sim.knownAt(b.sim.track[0][9] + know - 1, know, g.rules.maxSpeed, g.rules.accel))
        # Running into b: they push each other off where each one's client
        # has the other, and never overlap much.
        self.send(a, g.clock, 0.05, held=True, direction=0)
        closest = 1e9
        for _ in range(100):
            self.runUntil(g.clock + 0.01)
            closest = min(closest, math.hypot(a.sim.state.x - b.sim.state.x, a.sim.state.y - b.sim.state.y))
        self.assertGreater(a.sim.stats['contacts'], 0)
        self.assertGreater(b.sim.state.x, 2.6)  # b was pushed along
        self.assertGreater(closest, 2.0 * g.rules.playerRadius - 0.4)

    def test_know_delay_covers_what_the_client_sees(self):
        """Inputs arriving quickly say little about the way back when the
        client's clock is ahead of ours; the client says how late samples
        reach it, and knowDelay (announced in SELF) stays above that."""
        g = self.game
        a = self.add(1, 0, 0.0, 0.0)
        self.warmUp(a, 0.01)
        self.runUntil(g.clock + 0.5)
        quick = a.sim.knowDelay
        self.assertLess(quick, 12)
        g.air.selfs.clear()
        self.send(a, g.clock, 0.01, sampleDelay=12)
        self.runUntil(g.clock + 0.3)
        self.assertGreaterEqual(a.sim.knowDelay, 12 + 1 + a.sim.SAMPLE_DELAY_MARGIN)
        self.assertIn(a.sim.knowDelay, [m[11] for slot, m in g.air.selfs if slot == a.slot])
        # A client claiming anything is believed only up to a second.
        self.send(a, g.clock, 0.01, sampleDelay=255)
        self.runUntil(g.clock + 0.3)
        self.assertLessEqual(a.sim.knowDelay, a.sim.MAX_SAMPLE_DELAY + 1 + a.sim.SAMPLE_DELAY_MARGIN)

    # ----------------------------------------------------------- batches

    def test_everything_goes_out_in_batches_and_in_order(self):
        g = self.game
        a = self.add(1, 0, 0.0, 0.0)
        self.runUntil(g.clock + 0.05)
        batches = len(g.air.frames)
        # An orb under the player, collected in a frame between batches: it
        # waits for the next batch...
        g.orbs[7] = (0.3, 0.0, 0)
        self.runUntil(g.clock + 0.01)
        self.assertEqual(g.events, [g.events[0]])
        self.assertEqual(len(g.air.frames), batches)
        # ... unless a field update goes out first: then the event goes before it.
        g.d_notice(3, 0, 1)
        self.assertEqual(g.air.log[-2:], [('frame', [EVENT_ORB_COLLECTED]), ('update', 'notice')])
        self.assertEqual(g.sentEvents(EVENT_ORB_COLLECTED), [[7, a.slot, 11]])  # 10 + 1
        # Samples, events and SELF all leave in the batches, MOVES_INTERVAL
        # apart (give or take one 10 ms frame), not every frame.
        steps = [f[0] for f in g.air.frames if f[1] or f[2]]
        gap = int((g.MOVES_INTERVAL - 0.01) / STEP)
        self.assertTrue(all(b - a_ >= gap for a_, b in zip(steps, steps[1:])), steps)


if __name__ == '__main__':
    unittest.main()
