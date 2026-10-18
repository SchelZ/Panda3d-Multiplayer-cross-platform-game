"""SERVER_FRAME: everything one client gets from the AI in one batch, in one
message, bit-packed (client/src/net/Frame.h decodes it).

The AI sends a batch every MOVES_INTERVAL (AI_FRAME, protocol.py): the
movement samples due, the game events since the last batch (orbs, hits) and
the SELF messages due. The server turns it into one SERVER_FRAME per client:
the samples of everyone but the client's own player, the events, and that
client's SELF. One message per client per batch means one TCP packet, and
on a phone link the 40-60 bytes of IP and TCP headers per packet (and the
client's acknowledgement of it) cost more than the game data.

Fields are packed with as many bits as they need, least significant bit
first, and only what changed is sent. Everything travels over one TCP
connection, in order, so both ends can code a sample against the previous
sample of the same player (its *baseline*), which the receiver always has.

Frame:
  sent       1 bit: 1 = 4 bits (sent - previous frame's sent - 1, a delta of
             1-16 steps); 0 = 16 bits (the step's low 16 bits)
  samples    each: 1 bit 1, then a sample; then 1 bit 0
  events     each: 1 bit 1, 2 bits kind, then its fields; then 1 bit 0
  self       1 bit: then a SELF
  zero bits up to a whole byte

Sample (a protocol.py STATE):
  slot 8; coded 1 (1 = against the slot's baseline, 0 = in full)
  input 1 (FLAG_INPUT); event 1, and if set: dash 1, knock 1
  age        coded: 1 bit same as the baseline's; else (or in full) 8 bits
  dir        if input: coded and the baseline had input: 1 bit same; else 8 bits
  velocity   in full: vx 8, vy 8 (1/4 m/s, signed);
             coded: 2 bits width class, then vx - base vx and vy - base vy
             in VEL_WIDTHS[class] bits each
  position   in full: x 16, y 16 (cm, signed);
             coded: 2 bits width class, then the error of predictPos() for
             x and y in POS_WIDTHS[class] bits each
  heading    8 bits only if vx^2 + vy^2 < HEADING_SPEED_SQ; above that the
             player faces where it moves (Movement.cpp), and receivers use
             velocityHeading()

Events (kind):
  0 ORB_COLLECTED  orbId 16, slot 8, score 16
  1 ORB_SPAWNED    orbId 16, x 16, y 16 (signed, 1/10 m), kind 1
  2 PLAYER_HIT     attacker 8, victim 8, attackerScore 16, victimScore 16

SELF (protocol.py SELF): ack 8; age 8 (sent - step; 255 = then the step in
16 bits); x 16, y 16, vx 16, vy 16 (signed); heading 16; dashLeft,
dashCooldown, stun: 1 bit nonzero, then 16 bits; flags 2; knowDelay 8.
"""

import math
import struct

from .protocol import FLAG_DASH, FLAG_INPUT, FLAG_KNOCK, SELF

EVENT_ORB_COLLECTED = 0
EVENT_ORB_SPAWNED = 1
EVENT_PLAYER_HIT = 2

VEL_WIDTHS = (3, 5, 7, 9)       # bits per velocity change, by class
POS_WIDTHS = (4, 6, 9, 18)      # bits per position error, by class
HEADING_SPEED_SQ = 64           # (1/4 m/s)^2: below 2 m/s the heading is sent
MAX_PREDICT_STEPS = 240         # predictPos() extrapolates at most 2 s
SENT_DELTA_BITS = 4
SELF_AGE_ESCAPE = 255

# AI_FRAME events (protocol.py): uint8 kind, then these.
AI_EVENT = {
    EVENT_ORB_COLLECTED: struct.Struct('<HBH'),
    EVENT_ORB_SPAWNED: struct.Struct('<HhhB'),
    EVENT_PLAYER_HIT: struct.Struct('<BBHH'),
}
EVENT_BITS = {
    EVENT_ORB_COLLECTED: ((16, False), (8, False), (16, False)),
    EVENT_ORB_SPAWNED: ((16, False), (16, True), (16, True), (1, False)),
    EVENT_PLAYER_HIT: ((8, False), (8, False), (16, False), (16, False)),
}


class FrameError(ValueError):
    """A frame that does not decode: the stream is out of step."""


class BitWriter:
    __slots__ = ('acc', 'n')

    def __init__(self):
        self.acc = 0
        self.n = 0

    def put(self, value, bits):
        """The low `bits` bits of `value` (two's complement if negative)."""
        self.acc |= (value & ((1 << bits) - 1)) << self.n
        self.n += bits

    def extend(self, other):
        self.acc |= other.acc << self.n
        self.n += other.n

    def getvalue(self):
        return self.acc.to_bytes((self.n + 7) >> 3, 'little')


class BitReader:
    __slots__ = ('acc', 'pos', 'end')

    def __init__(self, data):
        self.acc = int.from_bytes(data, 'little')
        self.pos = 0
        self.end = len(data) * 8

    def get(self, bits):
        if self.pos + bits > self.end:
            raise FrameError('frame truncated')
        value = (self.acc >> self.pos) & ((1 << bits) - 1)
        self.pos += bits
        return value

    def signed(self, bits):
        value = self.get(bits)
        return value - (1 << bits) if value >> (bits - 1) else value


def signed16(value):
    return ((value + 32768) & 0xffff) - 32768


def predictPos(p, pv, v, dt):
    """Where a sample at `p` (cm) with velocity `pv` (1/4 m/s) is expected
    `dt` steps later, arriving with velocity `v`: the average velocity over
    the interval. Integers only, so both ends get exactly the same."""
    dt = max(-MAX_PREDICT_STEPS, min(MAX_PREDICT_STEPS, dt))
    return p + ((pv + v) * 25 * dt + 120) // 240  # 25 cm/s per unit, 2 x 120 steps/s; floor


def velocityHeading(vx, vy):
    """The heading byte (256ths of a turn, Panda's H) of a velocity."""
    return int(math.floor(math.atan2(-vx, vy) * 128.0 / math.pi + 0.5)) & 0xff


def _widthClass(widths, a, b):
    for i, w in enumerate(widths):
        limit = 1 << (w - 1)
        if -limit <= a < limit and -limit <= b < limit:
            return i, w
    raise FrameError('value out of range')


# ------------------------------------------------------------------ samples
#
# A baseline: (x, y, vx, vy, flags, dir, heading, age, step16) of the newest
# sample of a slot, as both ends decoded it.

def baselineOf(state, sent16):
    x, y, vx, vy, flags, direction, heading, age = state
    return (x, y, vx, vy, flags & FLAG_INPUT, direction, heading, age, (sent16 - age) & 0xffff)


def writeSample(w, slot, state, sent16, base):
    """One sample. `state`: the STATE fields (x, y, vx, vy, flags, dir,
    heading, age) of a batch sent at `sent16`; `base`: the slot's baseline
    or None. Returns the new baseline."""
    x, y, vx, vy, flags, direction, heading, age = state
    w.put(slot, 8)
    w.put(base is not None, 1)
    hasInput = flags & FLAG_INPUT
    w.put(1 if hasInput else 0, 1)
    if flags & (FLAG_DASH | FLAG_KNOCK):
        w.put(1, 1)
        w.put(1 if flags & FLAG_DASH else 0, 1)
        w.put(1 if flags & FLAG_KNOCK else 0, 1)
    else:
        w.put(0, 1)
    step16 = (sent16 - age) & 0xffff
    if base is None:
        w.put(age, 8)
        if hasInput:
            w.put(direction, 8)
        w.put(vx, 8)
        w.put(vy, 8)
        w.put(x, 16)
        w.put(y, 16)
    else:
        bx, by, bvx, bvy, bflags, bdir, _, bage, bstep = base
        if age == bage:
            w.put(1, 1)
        else:
            w.put(0, 1)
            w.put(age, 8)
        if hasInput:
            if bflags & FLAG_INPUT and direction == bdir:
                w.put(1, 1)
            else:
                if bflags & FLAG_INPUT:
                    w.put(0, 1)
                w.put(direction, 8)
        cls, bits = _widthClass(VEL_WIDTHS, vx - bvx, vy - bvy)
        w.put(cls, 2)
        w.put(vx - bvx, bits)
        w.put(vy - bvy, bits)
        dt = signed16(step16 - bstep)
        ex, ey = x - predictPos(bx, bvx, vx, dt), y - predictPos(by, bvy, vy, dt)
        cls, bits = _widthClass(POS_WIDTHS, ex, ey)
        w.put(cls, 2)
        w.put(ex, bits)
        w.put(ey, bits)
    if vx * vx + vy * vy < HEADING_SPEED_SQ:
        w.put(heading, 8)
    else:
        heading = velocityHeading(vx, vy)
    return (x, y, vx, vy, flags & FLAG_INPUT, direction if hasInput else 0, heading, age, step16)


def readSample(r, sent16, baselines):
    """(slot, state) of the next sample; updates `baselines` (a dict or list
    by slot). `state` is the STATE fields as the sender had them, but for
    the heading of a moving player, which is its velocity's."""
    slot = r.get(8)
    coded = r.get(1)
    hasInput = r.get(1)
    flags = FLAG_INPUT if hasInput else 0
    if r.get(1):
        flags |= (FLAG_DASH if r.get(1) else 0)
        flags |= (FLAG_KNOCK if r.get(1) else 0)
    direction = 0
    if not coded:
        age = r.get(8)
        if hasInput:
            direction = r.get(8)
        vx = r.signed(8)
        vy = r.signed(8)
        x = r.signed(16)
        y = r.signed(16)
    else:
        base = baselines[slot]
        if base is None:
            raise FrameError('sample of slot %d without a baseline' % slot)
        bx, by, bvx, bvy, bflags, bdir, _, bage, bstep = base
        age = bage if r.get(1) else r.get(8)
        if hasInput:
            direction = bdir if bflags & FLAG_INPUT and r.get(1) else r.get(8)
        bits = VEL_WIDTHS[r.get(2)]
        vx = bvx + r.signed(bits)
        vy = bvy + r.signed(bits)
        if not (-128 <= vx < 128 and -128 <= vy < 128):
            raise FrameError('velocity out of range')
        step16 = (sent16 - age) & 0xffff
        dt = signed16(step16 - bstep)
        bits = POS_WIDTHS[r.get(2)]
        x = predictPos(bx, bvx, vx, dt) + r.signed(bits)
        y = predictPos(by, bvy, vy, dt) + r.signed(bits)
        if not (-32768 <= x < 32768 and -32768 <= y < 32768):
            raise FrameError('position out of range')
    heading = r.get(8) if vx * vx + vy * vy < HEADING_SPEED_SQ else velocityHeading(vx, vy)
    step16 = (sent16 - age) & 0xffff
    baselines[slot] = (x, y, vx, vy, flags & FLAG_INPUT, direction, heading, age, step16)
    return slot, (x, y, vx, vy, flags, direction, heading, age)


# ------------------------------------------------------------------- events

def writeEvent(w, kind, fields):
    w.put(kind, 2)
    for (bits, _), value in zip(EVENT_BITS[kind], fields):
        w.put(value, bits)


def readEvent(r):
    kind = r.get(2)
    spec = EVENT_BITS.get(kind)
    if spec is None:
        raise FrameError('unknown event %d' % kind)
    return kind, tuple(r.signed(bits) if sign else r.get(bits) for bits, sign in spec)


def unpackAiEvents(data, offset, count):
    """`count` AI_FRAME events from `data`: [(kind, fields)], end offset."""
    events = []
    for _ in range(count):
        kind = data[offset]
        spec = AI_EVENT.get(kind)
        if spec is None:
            raise FrameError('unknown AI event %d' % kind)
        events.append((kind, spec.unpack_from(data, offset + 1)))
        offset += 1 + spec.size
    return events, offset


def packAiEvent(kind, *fields):
    return bytes((kind,)) + AI_EVENT[kind].pack(*fields)


# --------------------------------------------------------------------- SELF

def writeSelf(w, selfBytes, sent16):
    """A protocol.py SELF (21 bytes)."""
    ack, step, x, y, vx, vy, heading, dashLeft, cooldown, stun, flags, know = SELF.unpack(selfBytes)
    w.put(ack, 8)
    age = (sent16 - step) & 0xffff
    if age < SELF_AGE_ESCAPE:
        w.put(age, 8)
    else:
        w.put(SELF_AGE_ESCAPE, 8)
        w.put(step, 16)
    for v in (x, y, vx, vy, heading):
        w.put(v, 16)
    for v in (dashLeft, cooldown, stun):
        if v:
            w.put(1, 1)
            w.put(v, 16)
        else:
            w.put(0, 1)
    w.put(flags, 2)
    w.put(know, 8)


def readSelf(r, sent16):
    """The SELF bytes (protocol.unpackSelf() reads them)."""
    ack = r.get(8)
    age = r.get(8)
    step = r.get(16) if age == SELF_AGE_ESCAPE else (sent16 - age) & 0xffff
    x, y, vx, vy = r.signed(16), r.signed(16), r.signed(16), r.signed(16)
    heading = r.get(16)
    timers = [r.get(16) if r.get(1) else 0 for _ in range(3)]
    flags = r.get(2)
    know = r.get(8)
    return SELF.pack(ack, step, x, y, vx, vy, heading, timers[0], timers[1], timers[2], flags, know)


# -------------------------------------------------------------------- frames

def writeHeader(w, sent16, previous):
    """`previous`: the sent step of the frame before to this client, or None."""
    delta = (sent16 - previous) & 0xffff if previous is not None else 0
    if 1 <= delta <= 1 << SENT_DELTA_BITS:
        w.put(1, 1)
        w.put(delta - 1, SENT_DELTA_BITS)
    else:
        w.put(0, 1)
        w.put(sent16, 16)


def decodeFrame(data, previous, baselines):
    """(sent16, samples [(slot, state)], events [(kind, fields)], selfBytes
    or None) of one SERVER_FRAME. `previous`: the sent step of the frame
    before; `baselines`: by slot, updated. Raises FrameError."""
    r = BitReader(data)
    if r.get(1):
        if previous is None:
            raise FrameError('first frame is coded')
        sent16 = (previous + r.get(SENT_DELTA_BITS) + 1) & 0xffff
    else:
        sent16 = r.get(16)
    samples = []
    while r.get(1):
        samples.append(readSample(r, sent16, baselines))
    events = []
    while r.get(1):
        events.append(readEvent(r))
    selfBytes = readSelf(r, sent16) if r.get(1) else None
    if r.end - r.pos >= 8 or (r.acc >> r.pos):
        raise FrameError('trailing data in frame')
    return sent16, samples, events, selfBytes
