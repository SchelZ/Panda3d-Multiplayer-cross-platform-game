"""What a player connection is allowed to do. The AI connection is trusted.

The stock ServerRepository lets any client create objects of any class in
any zone, rename itself mid-game through required fields, and so on. These
allow-lists close that down to exactly what the C++ client needs.
"""

from .protocol import PLAYER_INTEREST_ZONES, ZONE_ARENA

# Classes a player may create, and how many objects of that class it may own.
PLAYER_CREATABLE = {
    'DistributedArenaPlayer': 1,
}

# Zones a player may create objects in or move objects to.
PLAYER_OBJECT_ZONES = frozenset((ZONE_ARENA,))

# Zones a player may listen to.
PLAYER_INTEREST = PLAYER_INTEREST_ZONES
MAX_INTEREST_ZONES = len(PLAYER_INTEREST_ZONES)

# The class whose objects get a movement slot (protocol.py, SERVER_SLOTS).
# Movement and dashes travel as CLIENT_MOVE messages, not field updates.
SLOT_CLASS = 'DistributedArenaPlayer'

# Fields a player may update on objects it owns, per class. Name and color
# are required fields: set once in the generate, never changed.
PLAYER_OWN_FIELDS = {
    'DistributedArenaPlayer': frozenset(),
}

# Fields a player may send to objects owned by the AI (all are p2p requests).
PLAYER_REQUEST_FIELDS = {
    'TimeManager': frozenset(('requestServerTime',)),
    'DistributedArenaGame': frozenset(('requestChat', 'ping')),
}

# Optional (non-required) fields a player may include in a generate message.
# None: late joiners get positions from the server's SERVER_MOVES cache.
# setLocation is deliberately absent: the server owns object zones.
PLAYER_GENERATE_OPTIONAL = {
    'DistributedArenaPlayer': frozenset(),
}

# Fields the server strips from a resend before relaying it (stock Python
# clients include them; they carry nothing a player is allowed to decide).
GENERATE_FIELDS_TO_DROP = frozenset(('setLocation', 'setBarrierData'))
