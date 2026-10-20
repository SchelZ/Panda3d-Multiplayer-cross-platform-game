"""SERVER_FRAME coding (arena/frame.py): what goes in comes out, against
the same baselines on both ends, and garbage is refused, not misread.

    cd server && python -m unittest tests.test_frame -v
"""

import os
import random
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from arena.frame import (  # noqa: E402
    EVENT_ORB_COLLECTED, EVENT_ORB_SPAWNED, EVENT_PLAYER_HIT, HEADING_SPEED_SQ, BitWriter,
    FrameError, decodeFrame, packAiEvent, predictPos, unpackAiEvents, velocityHeading,
    writeEvent, writeHeader, writeSample, writeSelf)
from arena.protocol import FLAG_DASH, FLAG_INPUT, FLAG_KNOCK, packSelf  # noqa: E402


def randomState(rng, near=None):
    """STATE fields; near a previous state now and then, as real samples are."""
    if near is not None and rng.random() < 0.8:
        x, y, vx, vy = near[0], near[1], near[2], near[3]
        vx = max(-128, min(127, vx + rng.randint(-20, 20)))
        vy = max(-128, min(127, vy + rng.randint(-20, 20)))
        x = max(-32768, min(32767, x + rng.randint(-300, 300)))
        y = max(-32768, min(32767, y + rng.randint(-300, 300)))
    else:
        x, y = rng.randint(-32768, 32767), rng.randint(-32768, 32767)
        vx, vy = rng.randint(-128, 127), rng.randint(-128, 127)
    flags = rng.choice((0, FLAG_INPUT, FLAG_INPUT, FLAG_INPUT | FLAG_DASH, FLAG_KNOCK))
    direction = rng.randrange(256) if flags & FLAG_INPUT else 0
    age = rng.choice((3, 3, 3, rng.randrange(256)))
    return (x, y, vx, vy, flags, direction, rng.randrange(256), age)


class FrameTest(unittest.TestCase):

    def test_round_trip_against_baselines(self):
        rng = random.Random(7)
        sendBase, recvBase = {}, [None] * 256
        sent, prevOut, prevIn = 1000, None, None
        last = {}
        for _ in range(3000):
            sent = (sent + rng.choice((1, 2, 3, 2, 40, 9000))) & 0xffff
            samples = []
            for slot in rng.sample(range(256), rng.randint(0, 4)):
                state = randomState(rng, last.get(slot))
                last[slot] = state
                samples.append((slot, state))
            events = [(EVENT_ORB_COLLECTED, (rng.randrange(65536), rng.randrange(256), rng.randrange(65536))),
                      (EVENT_ORB_SPAWNED, (rng.randrange(65536), rng.randint(-32768, 32767),
                                           rng.randint(-32768, 32767), rng.randrange(2))),
                      (EVENT_PLAYER_HIT, (rng.randrange(256), rng.randrange(256), rng.randrange(65536),
                                          rng.randrange(65536)))][:rng.randint(0, 3)]
            own = None
            if rng.random() < 0.3:
                own = packSelf(rng.randrange(256), (sent - rng.choice((0, 5, 300, 70000))) & 0xffff,
                               rng.uniform(-300, 300), rng.uniform(-300, 300), rng.uniform(-30, 30),
                               rng.uniform(-30, 30), rng.uniform(-180, 180), rng.choice((0, 0.12)),
                               rng.choice((0, 1.6, 0.4)), rng.choice((0, 0.3)), rng.randrange(4),
                               rng.randrange(256))
            w = BitWriter()
            writeHeader(w, sent, prevOut)
            prevOut = sent
            for slot, state in samples:
                w.put(1, 1)
                sendBase[slot] = writeSample(w, slot, state, sent, sendBase.get(slot))
            w.put(0, 1)
            for kind, fields in events:
                w.put(1, 1)
                writeEvent(w, kind, fields)
            w.put(0, 1)
            w.put(own is not None, 1)
            if own is not None:
                writeSelf(w, own, sent)
            got = decodeFrame(w.getvalue(), prevIn, recvBase)
            prevIn = got[0]
            self.assertEqual(got[0], sent)
            for (slot, state), (slot2, state2) in zip(samples, got[1]):
                self.assertEqual(slot, slot2)
                self.assertEqual(state[:6] + state[7:], state2[:6] + state2[7:])
                vx, vy = state[2], state[3]
                self.assertEqual(state2[6], state[6] if vx * vx + vy * vy < HEADING_SPEED_SQ
                                 else velocityHeading(vx, vy))
            self.assertEqual(len(samples), len(got[1]))
            self.assertEqual(events, got[2])
            self.assertEqual(own, got[3])

    def test_a_typical_sample_is_small(self):
        base = (1000, -2000, 30, 10, FLAG_INPUT, 20, 0, 3, 100)
        w = BitWriter()
        writeSample(w, 5, (1062, -1979, 32, 9, FLAG_INPUT, 20, 0, 3), 108, base)
        self.assertLessEqual(w.n, 40)  # 11 bytes as a STATE with its slot

    def test_prediction_is_exact_integer_arithmetic(self):
        self.assertEqual(predictPos(0, 4, 4, 120), 100)    # 1 m/s for 1 s
        self.assertEqual(predictPos(0, -4, -4, 120), -100)
        self.assertEqual(predictPos(0, 1, 0, 1), 0)        # 0.1 cm rounds to 0
        self.assertEqual(predictPos(0, 127, 127, 10 ** 6), predictPos(0, 127, 127, 240))

    def test_heading_of_velocity(self):
        self.assertEqual(velocityHeading(0, 10), 0)        # +y: heading 0
        self.assertEqual(velocityHeading(-10, 0), 64)      # -x: a quarter turn left
        self.assertEqual(velocityHeading(10, 0), 192)

    def test_garbage_is_refused(self):
        rng = random.Random(3)
        refused = 0
        for _ in range(2000):
            data = bytes(rng.randrange(256) for _ in range(rng.randint(0, 40)))
            try:
                decodeFrame(data, rng.choice((None, 5)), [None] * 256)
            except FrameError:
                refused += 1
        self.assertGreater(refused, 1500)
        # Out of step: a coded sample without its baseline.
        w = BitWriter()
        writeHeader(w, 10, None)
        w.put(1, 1)
        writeSample(w, 3, (0, 0, 0, 0, 0, 0, 0, 0), 10, (0, 0, 0, 0, 0, 0, 0, 0, 10))
        w.put(0, 3)
        with self.assertRaises(FrameError):
            decodeFrame(w.getvalue(), None, [None] * 256)

    def test_ai_events_pack_and_unpack(self):
        data = packAiEvent(EVENT_ORB_SPAWNED, 7, -12, 340, 1) + packAiEvent(EVENT_PLAYER_HIT, 1, 2, 30, 4)
        events, end = unpackAiEvents(data, 0, 2)
        self.assertEqual(events, [(EVENT_ORB_SPAWNED, (7, -12, 340, 1)), (EVENT_PLAYER_HIT, (1, 2, 30, 4))])
        self.assertEqual(end, len(data))


if __name__ == '__main__':
    unittest.main()
