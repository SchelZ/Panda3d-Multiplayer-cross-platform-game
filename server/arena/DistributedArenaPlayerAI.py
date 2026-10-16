"""AI view of a player. The player's client owns the object; its movement
arrives as compact binary samples (protocol.py, SERVER_MOVES), which the AI
repository hands to DistributedArenaGameAI.onPlayerState to be judged."""

from collections import deque

from direct.directnotify import DirectNotifyGlobal
from direct.distributed.ClockDelta import globalClockDelta
from direct.distributed.DistributedObjectAI import DistributedObjectAI

from .protocol import NO_SLOT


def sanitizeName(name):
    name = ''.join(ch for ch in name if ch.isprintable()).strip()
    return name[:16] or 'Player'


class DistributedArenaPlayerAI(DistributedObjectAI):
    notify = DirectNotifyGlobal.directNotify.newCategory('DistributedArenaPlayerAI')

    def __init__(self, air):
        DistributedObjectAI.__init__(self, air)
        self.name = 'Player'
        self.colorIndex = 0
        self.ownerId = 0          # doIdBase of the owning client
        self.slot = NO_SLOT       # one-byte id on the wire
        self.registered = False

        # Server-side movement state (see DistributedArenaGameAI.onPlayerMoved).
        self.validX = 0.0
        self.validY = 0.0
        self.heading = 0.0
        self.lastMoveTime = None
        self.moveBudget = 0.0
        self.lastCorrection = -1e9
        self.lastDash = -1e9
        self.dashHits = set()
        self.lastHitTime = -1e9   # AI time this player was last hit
        self.lastChat = -1e9

        # Lag compensation (see DistributedArenaGameAI._checkDashHits): accepted
        # samples on the player's own timeline, as (time, x, y, vx, vy, arrival).
        self.track = deque()
        self.latency = None       # estimated one-way delay client -> AI (s)
        self.viewDelay = 0.0      # how far in the past it draws others (s)
        self.dashStart = -1e9     # current dash, on the player's timeline
        self.dashEnd = -1e9

    @staticmethod
    def sampleTime(timestamp, now, maxRewind):
        """AI time of a 16-bit network timestamp, trusted only within
        [now - maxRewind, now] so nobody can claim an arbitrary past."""
        if timestamp is None:
            return now
        t = globalClockDelta.networkToLocalTime(timestamp, now, bits=16)
        return min(now, max(now - maxRewind, t))

    # Required fields.
    def setName(self, name):
        self.name = sanitizeName(name)

    def getName(self):
        return self.name

    def setColorIndex(self, index):
        self.colorIndex = index

    def getColorIndex(self):
        return self.colorIndex

    def announceGenerate(self):
        DistributedObjectAI.announceGenerate(self)
        # Only valid while the generate message is being processed.
        self.ownerId = self.air.getAvatarIdFromSender() or 0
        slot = self.air.slotOf(self.doId)
        self.slot = NO_SLOT if slot is None else slot
        game = getattr(self.air, 'game', None)
        if game is not None:
            game.addPlayer(self)
            self.registered = True

    def delete(self):
        game = getattr(self.air, 'game', None) if self.air else None
        if self.registered and game is not None:
            game.removePlayer(self)
        self.registered = False
        DistributedObjectAI.delete(self)
