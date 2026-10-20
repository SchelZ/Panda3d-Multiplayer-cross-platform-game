"""The AI's movement code (arena/motion.py) on a reference script. The C++
client's Movement.cpp is checked against the same numbers
(client/tests/MovementTests.cpp, movement_matches_the_ai_reference_trajectory),
so the AI and every client move players the same way.

    cd server && python -m unittest tests.test_motion -v
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from arena import motion  # noqa: E402
from arena.protocol import dirVector  # noqa: E402
from arena.rules import Rules, defaultPillars  # noqa: E402

# step, x, y, vx, vy, dashCooldown, stun
EXPECTED = [
    (99, 11.572917, -3.000000, 9.000000, 0.000000, 0.0000, 0.0000),
    (199, 13.072917, 7.384375, 0.000000, 9.000000, 0.9417, 0.0000),
    (299, 13.072917, 11.311458, 0.000000, 0.000000, 0.1083, 0.0000),
    (399, 8.425163, 6.663704, -6.363961, -6.363961, 0.0000, 0.0000),
    (499, 5.232287, -1.091518, 0.495459, -5.792218, 0.7750, 0.0000),
    (599, 0.462809, -6.350324, -6.363961, -6.363961, 0.0000, 0.0000),
    (699, -7.940713, -14.753846, -6.363961, -6.363961, 0.7750, 0.0000),
    (799, -5.312043, -12.125177, 6.363961, 6.363961, 0.0000, 0.0000),
    (899, 3.091479, -3.721654, 6.363961, 6.363961, 0.9417, 0.0000),
    (999, 5.406415, -8.269672, 1.755813, -8.827068, 0.1083, 0.0000),
    (1099, 6.869592, -15.625562, 1.755813, -8.827068, 0.0000, 0.0000),
    (1199, 8.332770, -22.981451, 1.755813, -8.827068, 0.0000, 0.0000),
    (1299, 9.795947, -30.337341, 1.755813, -8.827068, 0.0000, 0.0000),
    (1399, 8.711559, -31.931379, -0.799673, 0.498415, 0.7750, 0.0000),
    (1499, 8.264507, -32.154905, -0.533894, -0.043514, 0.0000, 0.0000),
]


def scriptInput(k):
    """(held, direction, dash) at step k: running, turning, dashes (the one
    at 1000 refused by the cooldown), toward the central pillar and the wall."""
    if k < 120:
        return True, 0, False
    if k < 240:
        return True, 64, k == 120
    if k < 300:
        return False, 0, False
    if k < 700:
        return True, 160, k in (400, 600)
    if k < 900:
        return True, 32, k == 820
    return True, 200, k in (1000, 1300)


class MotionTest(unittest.TestCase):
    def test_reference_trajectory(self):
        r = Rules()
        pillars = defaultPillars(r.arenaRadius)
        s = motion.MoveState(5.0, -3.0)
        expected = iter(EXPECTED)
        e = next(expected)
        for k in range(1500):
            held, direction, dash = scriptInput(k)
            ix, iy = dirVector(direction) if held else (0.0, 0.0)
            motion.stepMovement(s, ix, iy, dash, r, pillars, r.arenaRadius)
            if k == 450:
                motion.applyKnockback(s, 10.0, -5.0)
            if k == e[0]:
                for got, want in zip((s.x, s.y, s.vx, s.vy, s.dashCooldown, s.stun), e[1:]):
                    self.assertAlmostEqual(got, want, places=4)
                e = next(expected, (None,))
        self.assertIsNone(e[0])

    def test_dead_reckoning_sends_only_changes(self):
        r = Rules()
        dr = motion.DeadReckoning()
        s = motion.MoveState()
        sends = 0
        for k in range(240):
            held = k < 120
            ix, iy = (1.0, 0.0) if held else (0.0, 0.0)
            motion.stepMovement(s, ix, iy, False, r, [], 1000.0)
            real = (k * motion.PHYSICS_STEP, s.x, s.y, s.vx, s.vy, 0.0, ix, iy, 0.0)
            if dr.due(real, r.maxSpeed, r.accel):
                dr.sent(real)
                sends += 1
        # A start, a heartbeat every 0.5 s while running, the release: not 240.
        self.assertLessEqual(sends, 6)
        self.assertGreaterEqual(sends, 2)


if __name__ == '__main__':
    unittest.main()
