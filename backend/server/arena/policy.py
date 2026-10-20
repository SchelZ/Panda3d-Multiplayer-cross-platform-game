"""What a player connection is allowed to do. The AI connection is trusted.

The stock ServerRepository lets any client create objects of any class in
any zone, rename itself mid-game through required fields, and so on. These
allow-lists close that down to exactly what the C++ client needs: the AI
creates every object, players included, and a player's client may only ask
for things (the AI's p2p request fields) and send its inputs (CLIENT_INPUT).
"""

from .protocol import PLAYER_INTEREST_ZONES, ZONE_ARENA

# Classes a player may create, and how many objects of that class it may own:
# none. Its player is created by the AI (DistributedArenaGame.requestJoin).
PLAYER_CREATABLE = {}

# Zones a player may create objects in or move objects to.
PLAYER_OBJECT_ZONES = frozenset((ZONE_ARENA,))

# Zones a player may listen to.
PLAYER_INTEREST = PLAYER_INTEREST_ZONES
MAX_INTEREST_ZONES = len(PLAYER_INTEREST_ZONES)

# Fields a player may update on objects it owns, per class: it owns none.
PLAYER_OWN_FIELDS = {}

# Fields a player may send to objects owned by the AI (all are p2p requests).
PLAYER_REQUEST_FIELDS = {
    'TimeManager': frozenset(('requestServerTime',)),
    'DistributedArenaGame': frozenset(('requestJoin', 'requestChat', 'ping')),
}

# Optional (non-required) fields a player may include in a generate message
# (it may create nothing, so none).
PLAYER_GENERATE_OPTIONAL = {}

# Fields the server strips from a resend before relaying it (stock Python
# clients include them; they carry nothing a player is allowed to decide).
GENERATE_FIELDS_TO_DROP = frozenset(('setLocation', 'setBarrierData'))
