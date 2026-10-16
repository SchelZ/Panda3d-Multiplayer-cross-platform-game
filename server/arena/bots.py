"""Headless Python players, for load tests and server tests.

Each bot is a stock ClientRepository that follows the connection flow from
the Panda3D manual ("Client Repositories"): connect, sync time with the
TimeManager in the global zone, then enter the arena and create its avatar.
"""

import math
import random

from direct.directnotify import DirectNotifyGlobal
from direct.distributed.ClientRepository import ClientRepository
from direct.distributed.ClockDelta import globalClockDelta
from direct.distributed.PyDatagram import PyDatagram
from direct.task import Task
from panda3d.core import URLSpec

from .DistributedArenaPlayer import DistributedArenaPlayer
from .protocol import (
    CLIENT_HELLO_ARENA, CLIENT_MOVE, FLAG_DASH, MOVE_ENTRY_SIZE, PROTOCOL_VERSION,
    ROLE_PLAYER, SERVER_MOVES, SERVER_REJECT_ARENA, SERVER_SLOTS, ZONE_ARENA, ZONE_GLOBAL,
    packState, unpackState)


class ArenaBotRepository(ClientRepository):
    notify = DirectNotifyGlobal.directNotify.newCategory('ArenaBot')

    SEND_INTERVAL = 0.1    # 10 Hz binary movement samples (position + velocity)

    def __init__(self, host, port, dcFileNames, name, seed=None, chatty=False):
        ClientRepository.__init__(self, dcFileNames=dcFileNames,
                                  connectMethod=ClientRepository.CM_NET, threadedNet=False)
        self.botName = name
        self.rng = random.Random(seed)
        self.chatty = chatty
        self.game = None
        self.player = None
        self.remotePlayers = {}
        self.slotToDoId = {}
        self.mySlot = None
        self.dashNext = False
        self.rejected = None
        self.lost = False
        self.pongs = 0
        self.corrections = 0
        self.knockbacks = 0
        self.x = self.rng.uniform(-10, 10)
        self.y = self.rng.uniform(-10, 10)
        self.vx = self.vy = 0.0
        self.ix = self.iy = 0.0
        self.lastSend = 0.0
        self.lastPing = 0.0
        self.lastDash = -10.0
        self.lastChat = 0.0
        self.pingContext = 0

        self.accept(self.uniqueName('createReady'), self._createReady)
        self.accept(self.uniqueName('gotTimeSync'), self._timeSynced)
        self.connect([URLSpec('tcp://%s:%d' % (host, port))],
                     successCallback=self._connected, failureCallback=self._failed)

    # --------------------------------------------------------- connection flow

    def _connected(self):
        dg = PyDatagram()
        dg.addUint16(CLIENT_HELLO_ARENA)
        dg.addUint32(PROTOCOL_VERSION)
        dg.addUint32(self.hashVal)
        dg.addUint8(ROLE_PLAYER)
        dg.addString('')
        dg.addString('arena-bot (Python)')
        self.send(dg)

    def _failed(self, code, reason):
        self.rejected = 'connect failed: %s' % reason

    def _createReady(self):
        self.ignore(self.uniqueName('createReady'))
        self.setInterestZones([ZONE_GLOBAL])

    def _timeSynced(self):
        self.ignore(self.uniqueName('gotTimeSync'))
        self.setInterestZones([ZONE_GLOBAL, ZONE_ARENA])

    def onGameGenerated(self, game):
        self.game = game
        if self.player is None:
            player = DistributedArenaPlayer(self)
            player.setName(self.botName)
            player.setColorIndex(self.rng.randrange(12))
            self.player = self.createDistributedObject(distObj=player, zoneId=ZONE_ARENA)
            taskMgr.add(self._think, self.uniqueName('think'))

    def handleMessageType(self, msgType, di):
        if msgType == SERVER_MOVES:
            data = di.getRemainingBytes()
            if data and len(data) == 1 + data[0] * MOVE_ENTRY_SIZE:
                for offset in range(1, len(data), MOVE_ENTRY_SIZE):
                    other = self.remotePlayers.get(self.slotToDoId.get(data[offset]))
                    if other is not None:
                        x, y, vx, vy, flags = unpackState(data, offset + 1)[:5]
                        other.applyState(x, y, vx, vy, flags)
        elif msgType == SERVER_SLOTS:
            while di.getRemainingSize() >= 5:
                slot, doId = di.getUint8(), di.getUint32()
                if doId:
                    self.slotToDoId[slot] = doId
                    if self.player is not None and doId == self.player.doId:
                        self.mySlot = slot
                else:
                    self.slotToDoId.pop(slot, None)
        elif msgType == SERVER_REJECT_ARENA:
            code = di.getUint16()
            self.rejected = '%s (%d)' % (di.getString(), code)
        else:
            self.notify.warning('unexpected message %d' % msgType)

    def lostConnection(self):
        self.lost = True
        taskMgr.remove(self.uniqueName('think'))

    # ---------------------------------------------------------- AI replies

    def onPong(self, context):
        self.pongs += 1

    def onKnockback(self, vx, vy):
        self.knockbacks += 1
        self.vx, self.vy = vx, vy

    def onCorrectPosition(self, x, y):
        self.corrections += 1
        self.x, self.y = x, y

    # -------------------------------------------------------------- brain

    def _think(self, task):
        if not self.isConnected() or self.game is None or self.game.rules is None:
            return Task.cont
        r = self.game.rules
        dt = min(globalClock.getDt(), 0.1)
        now = globalClock.getFrameTime()

        # Steer to the nearest orb (or wander).
        tx, ty = 0.0, 0.0
        if self.game.orbs:
            tx, ty, _ = min(self.game.orbs.values(),
                            key=lambda o: (o[0] - self.x) ** 2 + (o[1] - self.y) ** 2)
        dx, dy = tx - self.x, ty - self.y
        d = math.hypot(dx, dy) or 1.0
        self.ix, self.iy = dx / d, dy / d   # held input, as the C++ client sends it
        # The client's movement (Movement.cpp): velocity moves toward the
        # input at a constant acceleration.
        gx, gy = self.ix * r['maxSpeed'] - self.vx, self.iy * r['maxSpeed'] - self.vy
        gap = math.hypot(gx, gy)
        step = r['accel'] * dt
        if gap > step:
            gx, gy = gx * step / gap, gy * step / gap
        self.vx += gx
        self.vy += gy
        self.x += self.vx * dt
        self.y += self.vy * dt
        limit = self.game.arenaRadius - r['playerRadius']
        dist = math.hypot(self.x, self.y)
        if dist > limit:
            self.x, self.y = self.x / dist * limit, self.y / dist * limit

        for other in self.remotePlayers.values():
            if now - self.lastDash > r['dashCooldown'] + 0.2 and \
                    (other.x - self.x) ** 2 + (other.y - self.y) ** 2 < 9.0:
                self.lastDash = now
                self.dashNext = True
                break
        if self.dashNext or now - self.lastSend >= self.SEND_INTERVAL:
            self.lastSend = now
            dg = PyDatagram()
            dg.addUint16(CLIENT_MOVE)
            dg.appendData(packState(self.x, self.y, self.vx, self.vy,
                                    FLAG_DASH if self.dashNext else 0,
                                    globalClockDelta.getFrameNetworkTime(), self.ix, self.iy))
            self.send(dg)
            self.dashNext = False
        if now - self.lastPing >= 2.0:
            self.lastPing = now
            self.pingContext = (self.pingContext + 1) & 0xffff
            self.game.sendUpdate('ping', [self.pingContext])
        if self.chatty and now - self.lastChat > 5.0:
            self.lastChat = now
            self.game.sendUpdate('requestChat', ['hello from %s' % self.botName])
        self.flush()
        return Task.cont
