"""Client view of a player, used by the Python bots (arena/bots.py).
The C++ client has its own implementation (client/src/game/ArenaPlayer.cpp).
The AI creates every player; positions arrive in binary SERVER_FRAME
messages, which the bot repository applies to these objects by slot."""

from direct.distributed.DistributedObject import DistributedObject


class DistributedArenaPlayer(DistributedObject):

    def __init__(self, cr):
        DistributedObject.__init__(self, cr)
        self.name = 'Player'
        self.colorIndex = 0
        self.dashCount = 0
        self.x = self.y = self.vx = self.vy = 0.0

    def setName(self, name):
        self.name = name

    def getName(self):
        return self.name

    def setColorIndex(self, index):
        self.colorIndex = index

    def getColorIndex(self):
        return self.colorIndex

    def applyState(self, x, y, vx, vy, flags):
        self.x, self.y, self.vx, self.vy = x, y, vx, vy
        if flags & 1:
            self.dashCount += 1

    def announceGenerate(self):
        DistributedObject.announceGenerate(self)
        self.cr.remotePlayers[self.doId] = self

    def disable(self):
        self.cr.remotePlayers.pop(self.doId, None)
        DistributedObject.disable(self)
