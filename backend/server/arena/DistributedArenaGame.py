"""Client view of the game object, used by the Python bots (arena/bots.py).
The C++ client has its own implementation (client/src/game/ArenaGame.cpp)."""

from direct.distributed.ClockDelta import globalClockDelta
from direct.distributed.DistributedObject import DistributedObject


class DistributedArenaGame(DistributedObject):

    def __init__(self, cr):
        DistributedObject.__init__(self, cr)
        self.rules = None
        self.arenaRadius = 30.0
        self.pillars = []
        self.roundState = 0
        self.roundNumber = 0
        self.roundEndLocal = 0.0
        self.orbs = {}
        self.scores = {}
        self.events = []      # (name, args) log, inspected by tests

    def _event(self, name, *args):
        self.events.append((name, args))
        if len(self.events) > 500:
            del self.events[:250]

    def setRules(self, *rules):
        self.rules = dict(zip(('maxSpeed', 'accel', 'dashSpeed', 'dashTime', 'dashCooldown',
                               'playerRadius', 'orbRadius'), rules))

    def setArena(self, radius, pillars):
        self.arenaRadius = radius
        self.pillars = [tuple(p) for p in pillars]

    def setRound(self, state, number, endTime):
        self.roundState, self.roundNumber = state, number
        self.roundEndLocal = globalClockDelta.networkToLocalTime(endTime, bits=32)
        self._event('setRound', state, number)

    def setOrbs(self, orbs):
        self.orbs = {o[0]: (o[1], o[2], o[3]) for o in orbs}

    def setScores(self, scores):
        self.scores = {s[0]: s[1] for s in scores}

    # Events from SERVER_FRAME (frame.py), not DC fields.
    def orbSpawned(self, orbId, x, y, kind):
        self.orbs[orbId] = (x, y, kind)

    # Players are named by slot (protocol.py); scores are kept by slot too.
    def orbCollected(self, orbId, slot, score):
        self.orbs.pop(orbId, None)
        self.scores[slot] = score
        self._event('orbCollected', orbId, slot, score)

    def playerHit(self, attacker, victim, attackerScore, victimScore):
        self.scores[attacker] = attackerScore
        self.scores[victim] = victimScore
        self._event('playerHit', attacker, victim)

    def chatMessage(self, slot, text):
        self._event('chat', slot, text)

    def notice(self, kind, slot, value):
        self._event('notice', kind, slot, value)

    # Targeted replies from the AI.
    def joined(self, slot, playerId):
        self.cr.onJoined(slot, playerId)

    def pong(self, context):
        self.cr.onPong(context)

    def announceGenerate(self):
        DistributedObject.announceGenerate(self)
        self.cr.onGameGenerated(self)
