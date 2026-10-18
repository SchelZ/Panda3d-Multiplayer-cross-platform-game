"""Wire-level constants shared with the C++ client (client/src/net/Protocol.h).

The CMU ServerRepository protocol (direct.distributed.MsgTypesCMU) has no
handshake, so a client built against different DC files would silently
misinterpret every message.  Panda Arena adds one message in each direction,
outside the CMU id range:

  CLIENT_HELLO_ARENA   client -> server, must be the first message:
      uint32 protocolVersion, uint32 dcHash, uint8 role,
      string token (AI only), string clientInfo (for logs)
  SERVER_REJECT_ARENA  server -> client, right before the server closes:
      uint16 code, string reason

Until the hello is accepted the server ignores everything else and does not
send SET_DOID_RANGE_CMU, so a client cannot create objects or join zones.

The server is authoritative. A player's client sends only what its player
wants to do (CLIENT_INPUT: which way it steers, whether it dashes), and only
when that changes. The AI simulates every player with the same movement code
as the client (motion.stepMovement, a twin of client/src/game/Movement.cpp)
and tells everybody where everyone is. Positions, speeds, pickups and hits
are never taken from a client. The client still predicts its own player from
its own inputs, so it responds at once, and the AI tells it where it really
is whenever that could differ (SELF). Clients only ever talk to the server,
which never forwards anyone's address.

Time is counted in *steps*: the 120 Hz physics step on the network clock
(ClockDelta: the AI's clock). Step k ends at network time k / 120 s. An
input names the step it starts at, so client and AI apply it at exactly the
same step; on the wire a step is its low 16 bits (wraps every 9 minutes).

Every player gets a one-byte *slot* from the AI; everything that names a
player on the wire (movement, scores, hits, chat) uses the slot.

The AI sends what players need to see in batches, every MOVES_INTERVAL (30
a second): movement samples, game events (orbs, hits) and SELF messages. The
server gives each client all of it that concerns it in one SERVER_FRAME,
bit-packed (frame.py), so each client gets at most one TCP packet per batch.

  CLIENT_INPUT  client -> server: INPUT (6 bytes)
  SERVER_INPUTS server -> AI, once per tick: uint8 n, n x (uint8 slot, INPUT)
  AI_FRAME      AI -> server, once per batch: uint16 sent (the step it was
                sent at); uint8 n, n x (uint8 slot, STATE); uint8 n, n x
                (uint8 slot, SELF); uint8 n, n x (uint8 kind, event fields,
                frame.AI_EVENT)
  SERVER_FRAME  server -> each client: the samples of everyone else, the
                events and the client's own SELF, bit-packed (frame.py)
  AI_SLOT       AI -> server: uint8 slot, uint32 clientId, uint32 doId
                (doId 0 frees the slot)
  SERVER_SLOTS  server -> zone: n x (uint8 slot, uint32 doId); doId 0 frees a slot
  SERVER_CLIENT_LEFT  server -> AI: uint32 clientId (a player connection closed)

The server forwards inputs to the AI only, and AI_* messages only from the AI.

INPUT (6 bytes), little-endian:
  uint8 seq            counts the client's inputs (the AI acknowledges them)
  uint16 step          first step it applies to
  uint8 flags          bit 0 INPUT_DASH: dash at this step; bit 1 INPUT_HELD:
                       steering toward dir; bits 2-7: the client's view delay
                       (how far in the past it draws the others, RemoteMotion.h)
                       in 8 ms units, for lag-compensated hits
  uint8 dir            steering direction in 256ths of a turn from +x
  uint8 sampleDelay    how many steps after the step they were sent at the
                       client has been getting the others' samples (by its own
                       clock: the second most of the last 64); the AI keeps
                       knowDelay (SELF) above it, so the client always has the
                       samples it bumps by in time

STATE (10 bytes): a player as everyone draws it
  int16 x, int16 y     position in cm
  int8 vx, int8 vy     velocity in 1/4 m/s
  uint8 flags          bit 0 FLAG_DASH: a dash started at this sample; bit 1
                       FLAG_INPUT: steering toward dir; bit 2 FLAG_KNOCK: a hit
                       knocked the player back here
  uint8 dir            steering direction in 256ths of a turn from +x
  uint8 heading        where the player faces, 256ths of a turn (Panda's H)
  uint8 age            the step it describes, this many before the batch's
                       `sent` step

With the steering input, receivers predict acceleration and braking exactly
(motion.predictMotion), so a sample goes out only when that prediction would
go wrong (motion.DeadReckoning).

In a SERVER_FRAME a sample is coded against the player's previous sample,
and a moving player's heading is left out (it faces where it moves).

SELF (21 bytes in AI_FRAME): the player's exact state, for its own client to
check its prediction against
  uint8 ack            seq of the newest input applied
  uint16 step
  int16 x, int16 y     cm
  int16 vx, int16 vy   cm/s
  uint16 heading       65536ths of a turn
  uint16 dashLeft, dashCooldown, stun   ms
  uint8 flags          bit 0 SELF_KNOCK: knocked back at this step; bit 1
                       SELF_SNAP: placed here (spawn), show it at once
  uint8 knowDelay      from the next step on, the player bumps into another
                       where that one's newest sample sent at least this many
                       steps before says it is (both sides follow this rule,
                       so the bumps are predicted exactly)
"""

import math
import struct

PROTOCOL_VERSION = 5

CLIENT_HELLO_ARENA = 9100
SERVER_REJECT_ARENA = 9101
CLIENT_INPUT = 9102
SERVER_FRAME = 9103
SERVER_SLOTS = 9104
SERVER_INPUTS = 9106
AI_FRAME = 9107
AI_SLOT = 9109
SERVER_CLIENT_LEFT = 9110

STEPS_PER_SECOND = 120
STEP = 1.0 / STEPS_PER_SECOND

INPUT = struct.Struct('<BHBBB')
INPUT_SIZE = INPUT.size                 # 6
INPUT_ENTRY_SIZE = 1 + INPUT_SIZE       # slot + input
INPUT_DASH = 0x01
INPUT_HELD = 0x02
VIEW_DELAY_SHIFT = 2                    # flags >> 2 = view delay ...
VIEW_DELAY_UNIT = 0.008                 # ... in 8 ms units (0-504 ms)

STATE = struct.Struct('<hhbbBBBB')
STATE_SIZE = STATE.size                 # 10
MOVE_ENTRY_SIZE = 1 + STATE_SIZE        # slot + state
POS_SCALE = 100.0                       # cm
VEL_SCALE = 4.0                         # 0.25 m/s
FLAG_DASH = 0x01
FLAG_INPUT = 0x02
FLAG_KNOCK = 0x04
DIR_STEP = 2.0 * math.pi / 256.0

SELF = struct.Struct('<BHhhhhHHHHBB')
SELF_SIZE = SELF.size                   # 21
SELF_ENTRY_SIZE = 1 + SELF_SIZE         # slot + SELF
SELF_KNOCK = 0x01
SELF_SNAP = 0x02

SLOT_ENTRY = struct.Struct('<BI')
AI_SLOT_MSG = struct.Struct('<BII')
NO_SLOT = 255                           # "nobody" in slot-valued fields
MAX_SLOTS = 255


def _clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def wrapStep(step):
    """The 16 bits of a step that go on the wire."""
    return step & 0xffff


def resolveStep(step16, near):
    """The full step number of a 16-bit wire step, the one nearest `near`."""
    return near + ((step16 - near + 32768) & 0xffff) - 32768


def dirByte(ix, iy):
    """256ths of a turn from +x of a steering direction."""
    return int(round(math.atan2(iy, ix) / DIR_STEP)) & 0xff


def dirVector(direction):
    """Unit vector of a 256ths-of-a-turn direction."""
    a = direction * DIR_STEP
    return math.cos(a), math.sin(a)


def headingByte(heading):
    """256ths of a turn of a heading in degrees."""
    return int(round(heading / 360.0 * 256.0)) & 0xff


def headingFromByte(value):
    h = value * 360.0 / 256.0
    return h - 360.0 if h > 180.0 else h


def viewDelay(flags):
    """Seconds the sender draws other players in the past (INPUT flags bits 2-7)."""
    return (flags >> VIEW_DELAY_SHIFT) * VIEW_DELAY_UNIT


def packInput(seq, step, flags, direction, sampleDelay=0):
    return INPUT.pack(seq & 0xff, wrapStep(step), flags & 0xff, direction & 0xff,
                      _clamp(sampleDelay, 0, 255))


def unpackInput(data, offset=0):
    """(seq, step16, flags, dir, sampleDelay)."""
    return INPUT.unpack_from(data, offset)


def packState(x, y, vx, vy, flags, step, sent, ix=0.0, iy=0.0, heading=0.0):
    """10 bytes, for a batch sent at step `sent`; (ix, iy) is the steering
    input, zero for none."""
    flags &= ~FLAG_INPUT
    direction = 0
    if ix or iy:
        flags |= FLAG_INPUT
        direction = dirByte(ix, iy)
    return STATE.pack(_clamp(int(round(x * POS_SCALE)), -32768, 32767),
                      _clamp(int(round(y * POS_SCALE)), -32768, 32767),
                      _clamp(int(round(vx * VEL_SCALE)), -128, 127),
                      _clamp(int(round(vy * VEL_SCALE)), -128, 127),
                      flags & 0xff, direction, headingByte(heading), _clamp(sent - step, 0, 255))


def unpackState(data, offset=0, sent=0):
    """(x, y, vx, vy, flags, step, ix, iy, heading) in metres, m/s, the step
    (of a batch sent at step `sent`), the steering input as a unit vector (or
    zero) and degrees."""
    x, y, vx, vy, flags, direction, heading, age = STATE.unpack_from(data, offset)
    step = sent - age
    ix = iy = 0.0
    if flags & FLAG_INPUT:
        ix, iy = dirVector(direction)
    return (x / POS_SCALE, y / POS_SCALE, vx / VEL_SCALE, vy / VEL_SCALE, flags, step, ix, iy,
            headingFromByte(heading))


def _ms(seconds):
    return _clamp(int(round(seconds * 1000.0)), 0, 65535)


def packSelf(ack, step, x, y, vx, vy, heading, dashLeft, dashCooldown, stun, flags, knowDelay):
    return SELF.pack(ack & 0xff, wrapStep(step),
                     _clamp(int(round(x * 100.0)), -32768, 32767),
                     _clamp(int(round(y * 100.0)), -32768, 32767),
                     _clamp(int(round(vx * 100.0)), -32768, 32767),
                     _clamp(int(round(vy * 100.0)), -32768, 32767),
                     int(round(heading / 360.0 * 65536.0)) & 0xffff,
                     _ms(dashLeft), _ms(dashCooldown), _ms(stun), flags & 0xff,
                     _clamp(knowDelay, 0, 255))


def unpackSelf(data, offset=0):
    """(ack, step16, x, y, vx, vy, heading, dashLeft, dashCooldown, stun, flags,
    knowDelay) in metres, m/s, degrees, seconds and steps: exactly what the
    client continues from."""
    ack, step, x, y, vx, vy, heading, dashLeft, cooldown, stun, flags, know = \
        SELF.unpack_from(data, offset)
    h = heading * 360.0 / 65536.0
    return (ack, step, x / 100.0, y / 100.0, vx / 100.0, vy / 100.0, h - 360.0 if h > 180.0 else h,
            dashLeft / 1000.0, cooldown / 1000.0, stun / 1000.0, flags, know)


# DistributedArenaGame.notice(kind, slot, value): game messages as codes, the
# client turns them into text.
NOTICE_JOINED = 1        # slot joined
NOTICE_LEFT = 2          # slot left
NOTICE_ROUND_START = 3   # value = round number
NOTICE_ROUND_OVER = 4    # value = best score; slot = winner, NO_SLOT for a draw
NOTICE_SHUTDOWN = 5      # the server is stopping

ROLE_PLAYER = 0
ROLE_AI = 1

# Reject codes. The client stops auto-reconnecting on the "fatal" ones,
# because trying again cannot succeed without the user changing something.
REJECT_BAD_VERSION = 1       # fatal: different protocol version
REJECT_BAD_DC_HASH = 2       # fatal: different arena.dc / direct.dc
REJECT_SERVER_FULL = 3
REJECT_BAD_TOKEN = 4         # fatal
REJECT_PROTOCOL_ERROR = 5    # malformed or forbidden message
REJECT_FLOOD = 6             # exceeded the message rate limit
REJECT_TIMEOUT = 7           # no hello / no traffic for too long
REJECT_SHUTDOWN = 8          # server is stopping

# Zones. Zone 1 holds the TimeManager (clients sync time there first, as in
# the Panda3D manual), zone 2 is the arena.
ZONE_GLOBAL = 1
ZONE_ARENA = 2
PLAYER_INTEREST_ZONES = frozenset((ZONE_GLOBAL, ZONE_ARENA))

# Order matters: class numbers and the hash depend on it.
DC_FILES = ('direct.dc', 'arena.dc')

DEFAULT_PORT = 4400
