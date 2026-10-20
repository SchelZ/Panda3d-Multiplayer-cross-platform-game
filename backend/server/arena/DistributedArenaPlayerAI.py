"""AI view of a player. The AI creates it when the player's client asks to
join (DistributedArenaGameAI.requestJoin) and owns it; the client only sends
inputs, which the AI applies to `sim` (simulation.PlayerSim)."""

from direct.directnotify import DirectNotifyGlobal
from direct.distributed.DistributedObjectAI import DistributedObjectAI

from .protocol import NO_SLOT


def sanitizeName(name):
    name = ''.join(ch for ch in name if ch.isprintable()).strip()
    return name[:16] or 'Player'


class DistributedArenaPlayerAI(DistributedObjectAI):
    notify = DirectNotifyGlobal.directNotify.newCategory('DistributedArenaPlayerAI')

    def __init__(self, air, name='Player', colorIndex=0, ownerId=0, slot=NO_SLOT):
        DistributedObjectAI.__init__(self, air)
        self.name = sanitizeName(name)
        self.colorIndex = max(0, min(int(colorIndex), 11))
        self.ownerId = ownerId    # doIdBase of the player's client
        self.slot = slot          # one-byte id on the wire
        self.sim = None           # simulation.PlayerSim, set by the game

        self.lastHitTime = -1e9   # AI time this player was last hit
        self.lastChat = -1e9
        self.dashStart = -1e9     # current dash (network time), for hits
        self.dashEnd = -1e9
        self.dashChecked = -1e9   # dash judged up to this time
        self.dashHits = set()
        self.drawings = {}        # victim doId -> (track length, motion.Drawing) this dash

    # Required fields.
    def setName(self, name):
        self.name = sanitizeName(name)

    def getName(self):
        return self.name

    def setColorIndex(self, index):
        self.colorIndex = index

    def getColorIndex(self):
        return self.colorIndex
