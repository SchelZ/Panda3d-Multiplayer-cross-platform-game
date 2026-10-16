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

Movement is the only high-rate traffic, so it skips the generic field-update
format (2-byte message type + 4-byte sender + 4-byte doId + 2-byte field id
before the data) and uses three compact binary messages instead. Every player
gets a one-byte *slot* when its DistributedArenaPlayer is created; everything
that names a player on the wire (movement, scores, hits, chat) uses the slot.

  CLIENT_MOVE   client -> server: STATE (9 bytes)
  SERVER_MOVES  server -> zone, once per tick: uint8 count, count x (uint8 slot, STATE)
  SERVER_SLOTS  server -> zone: n x (uint8 slot, uint32 doId); doId 0 frees a slot

STATE (10 bytes), little-endian:
  int16 x, int16 y     position in cm
  int8 vx, int8 vy     velocity in 1/4 m/s
  uint8 flags          bit 0 FLAG_DASH: a dash started at this sample;
                       bit 1 FLAG_INPUT: the movement input is held, toward dir;
                       bits 2-7: the sender's view delay (how far in the past it
                       draws the others, RemoteMotion.h) in 4 ms units
  uint8 dir            held input direction in 256ths of a turn from +x
  uint16 t             16-bit network time (ClockDelta, 1/100 s) of the sample

With the held input, receivers predict acceleration and braking exactly
(motion.py). Heading is not sent: players face where they move.
"""

import math
import struct

PROTOCOL_VERSION = 3

CLIENT_HELLO_ARENA = 9100
SERVER_REJECT_ARENA = 9101
CLIENT_MOVE = 9102
SERVER_MOVES = 9103
SERVER_SLOTS = 9104

STATE = struct.Struct('<hhbbBBH')
STATE_SIZE = STATE.size                # 10
MOVE_ENTRY_SIZE = 1 + STATE_SIZE       # slot + state
POS_SCALE = 100.0                      # cm
VEL_SCALE = 4.0                        # 0.25 m/s
FLAG_DASH = 0x01
FLAG_INPUT = 0x02
VIEW_DELAY_SHIFT = 2                   # flags >> 2 = view delay ...
VIEW_DELAY_UNIT = 0.004                # ... in 4 ms units (0-252 ms)
DIR_STEP = 2.0 * math.pi / 256.0
NO_SLOT = 255                          # "nobody" in slot-valued fields
MAX_SLOTS = 255


def _clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def viewDelay(flags):
    """Seconds the sender draws other players in the past (flags bits 2-7)."""
    return (flags >> VIEW_DELAY_SHIFT) * VIEW_DELAY_UNIT


def packState(x, y, vx, vy, flags, t, ix=0.0, iy=0.0):
    """10 bytes; (ix, iy) is the held movement input, zero for none."""
    flags &= ~FLAG_INPUT
    direction = 0
    if ix or iy:
        flags |= FLAG_INPUT
        direction = int(round(math.atan2(iy, ix) / DIR_STEP)) & 0xff
    return STATE.pack(_clamp(int(round(x * POS_SCALE)), -32768, 32767),
                      _clamp(int(round(y * POS_SCALE)), -32768, 32767),
                      _clamp(int(round(vx * VEL_SCALE)), -128, 127),
                      _clamp(int(round(vy * VEL_SCALE)), -128, 127),
                      flags & 0xff, direction, t & 0xffff)


def unpackState(data, offset=0):
    """(x, y, vx, vy, flags, t, ix, iy) in metres, m/s, 16-bit network ticks,
    and the held input as a unit vector (or zero)."""
    x, y, vx, vy, flags, direction, t = STATE.unpack_from(data, offset)
    ix = iy = 0.0
    if flags & FLAG_INPUT:
        ix, iy = math.cos(direction * DIR_STEP), math.sin(direction * DIR_STEP)
    return x / POS_SCALE, y / POS_SCALE, vx / VEL_SCALE, vy / VEL_SCALE, flags, t, ix, iy


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
