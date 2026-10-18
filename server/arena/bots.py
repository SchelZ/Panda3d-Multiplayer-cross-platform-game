"""Headless Python players, for load tests and server tests.

Each bot is a stock ClientRepository that follows the connection flow from
the Panda3D manual ("Client Repositories"): connect, sync time with the
TimeManager in the global zone, enter the arena and ask the AI for a player.
Then it only sends inputs (protocol.py CLIENT_INPUT), like the C++ client,
and predicts its own player with the shared movement code (motion.py).
"""

import math
import random

from direct.directnotify import DirectNotifyGlobal
from direct.distributed.ClientRepository import ClientRepository
from direct.distributed.ClockDelta import globalClockDelta
from direct.distributed.PyDatagram import PyDatagram
from direct.task import Task
from panda3d.core import URLSpec

from . import motion
from .frame import EVENT_ORB_COLLECTED, EVENT_ORB_SPAWNED, EVENT_PLAYER_HIT, FrameError, decodeFrame
from .protocol import (
    CLIENT_HELLO_ARENA, CLIENT_INPUT, INPUT_DASH, INPUT_HELD, POS_SCALE,
    PROTOCOL_VERSION, ROLE_PLAYER, SERVER_FRAME, SERVER_REJECT_ARENA, SERVER_SLOTS,
    STEPS_PER_SECOND, VEL_SCALE, ZONE_ARENA, ZONE_GLOBAL, dirByte, dirVector, packInput,
    resolveStep, unpackSelf)


class ArenaBotRepository(ClientRepository):
    notify = DirectNotifyGlobal.directNotify.newCategory('ArenaBot')

    TURN_INTERVAL = 0.1    # s between inputs that only change the direction

    def __init__(self, host, port, dcFileNames, name, seed=None, chatty=False):
        ClientRepository.__init__(self, dcFileNames=dcFileNames,
                                  connectMethod=ClientRepository.CM_NET, threadedNet=False)
        self.botName = name
        self.rng = random.Random(seed)
        self.chatty = chatty
        self.game = None
        self.joinRequested = False
        self.remotePlayers = {}
        self.slotToDoId = {}
        self.frameSent = None          # SERVER_FRAME decoding state (frame.py)
        self.baselines = [None] * 256
        self.mySlot = None
        self.myDoId = None
        self.rejected = None
        self.lost = False
        self.pongs = 0
        self.corrections = 0
        self.knockbacks = 0
        self.selfs = 0
        # Own player, predicted from our inputs; the AI's SELF resets it.
        self.state = None
        self.step = 0
        self.held = False
        self.dir = 0
        self.dashNext = False
        self.sent = []         # (step, held, dir, dash) of our inputs, for replays
        self.history = {}      # step -> (x, y) we predicted
        self.seq = 0
        self.inputs = 0
        self.lastInput = -1e9
        self.lastPing = 0.0
        self.lastDash = -10.0
        self.lastChat = 0.0
        self.pingContext = 0

        self.accept(self.uniqueName('createReady'), self._createReady)
        self.accept(self.uniqueName('gotTimeSync'), self._timeSynced)
        self.connect([URLSpec('tcp://%s:%d' % (host, port))],
                     successCallback=self._connected, failureCallback=self._failed)

    @property
    def x(self):
        return self.state.x if self.state else 0.0

    @property
    def y(self):
        return self.state.y if self.state else 0.0

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
        if not self.joinRequested:
            self.joinRequested = True
            game.sendUpdate('requestJoin', [self.botName, self.rng.randrange(12)])
            taskMgr.add(self._think, self.uniqueName('think'))

    def onJoined(self, slot, playerId):
        self.mySlot = slot
        self.myDoId = playerId

    def networkStep(self):
        return int(math.floor((globalClock.getRealTime() - globalClockDelta.getDelta())
                              * STEPS_PER_SECOND))

    def handleMessageType(self, msgType, di):
        if msgType == SERVER_FRAME:
            try:
                self.frameSent, samples, events, own = decodeFrame(
                    di.getRemainingBytes(), self.frameSent, self.baselines)
            except FrameError as e:
                self.notify.warning('bad frame (%s), disconnecting' % e)
                self.disconnect()
                self.lostConnection()
                return
            for slot, (x, y, vx, vy, flags, _, _, _) in samples:
                other = self.remotePlayers.get(self.slotToDoId.get(slot))
                if other is not None:
                    other.applyState(x / POS_SCALE, y / POS_SCALE, vx / VEL_SCALE, vy / VEL_SCALE, flags)
            if self.game is not None:
                for kind, fields in events:
                    if kind == EVENT_ORB_COLLECTED:
                        self.game.orbCollected(*fields)
                    elif kind == EVENT_ORB_SPAWNED:
                        orbId, x, y, orbKind = fields
                        self.game.orbSpawned(orbId, x / 10.0, y / 10.0, orbKind)
                    elif kind == EVENT_PLAYER_HIT:
                        self.game.playerHit(*fields)
            if own is not None:
                self._onSelf(unpackSelf(own))
        elif msgType == SERVER_SLOTS:
            while di.getRemainingSize() >= 5:
                slot, doId = di.getUint8(), di.getUint32()
                self.baselines[slot] = None  # whoever is in it now: samples start in full
                if doId:
                    self.slotToDoId[slot] = doId
                else:
                    self.slotToDoId.pop(slot, None)
        elif msgType == SERVER_REJECT_ARENA:
            code = di.getUint16()
            self.rejected = '%s (%d)' % (di.getString(), code)
        else:
            self.notify.warning('unexpected message %d' % msgType)

    def _onSelf(self, msg):
        _, step16, x, y, vx, vy, heading, dashLeft, cooldown, stun, flags, _ = msg
        self.selfs += 1
        if flags & 1:
            self.knockbacks += 1
        now = self.networkStep()
        step = resolveStep(step16, now)
        mine = self.history.get(step)
        if mine is not None and not flags & 1 and math.hypot(mine[0] - x, mine[1] - y) > 0.02:
            self.corrections += 1  # where we predicted we were then is not where we were
        self.state = motion.MoveState(x, y, vx, vy, heading, dashLeft, cooldown, stun)
        self.step = step
        self._simulate(min(now, step + 60))  # replay our inputs since

    def lostConnection(self):
        self.lost = True
        taskMgr.remove(self.uniqueName('think'))

    # ---------------------------------------------------------- AI replies

    def onPong(self, context):
        self.pongs += 1

    # -------------------------------------------------------------- brain

    def _simulate(self, target):
        """Steps our player to `target` with the inputs we sent for each step."""
        rules = _Rules(self.game.rules)
        while self.step < target:
            k = self.step = self.step + 1
            held, direction, dash = False, 0, False
            for step, h, d, dsh in self.sent:
                if step > k:
                    break
                held, direction = h, d
                dash = dsh and step == k
            ix, iy = dirVector(direction) if held else (0.0, 0.0)
            motion.stepMovement(self.state, ix, iy, dash, rules, self.game.pillars,
                                self.game.arenaRadius)
            self.history[k] = (self.state.x, self.state.y)
            self.history.pop(k - 2 * STEPS_PER_SECOND, None)
        while len(self.sent) > 1 and self.sent[1][0] < target - 2 * STEPS_PER_SECOND:
            del self.sent[0]

    def _think(self, task):
        if not self.isConnected() or self.game is None or self.game.rules is None \
                or self.state is None:
            return Task.cont
        r = self.game.rules
        now = globalClock.getFrameTime()
        target = self.networkStep()

        # Steer to the nearest orb.
        tx, ty = 0.0, 0.0
        if self.game.orbs:
            tx, ty, _ = min(self.game.orbs.values(),
                            key=lambda o: (o[0] - self.x) ** 2 + (o[1] - self.y) ** 2)
        held = math.hypot(tx - self.x, ty - self.y) > 0.3
        direction = dirByte(tx - self.x, ty - self.y) if held else 0
        for other in self.remotePlayers.values():
            if other.doId != self.myDoId and now - self.lastDash > r['dashCooldown'] + 0.2 and \
                    (other.x - self.x) ** 2 + (other.y - self.y) ** 2 < 9.0:
                self.lastDash = now
                self.dashNext = True
                break
        turned = held != self.held or (held and abs((direction - self.dir + 128) % 256 - 128) > 2)
        if self.dashNext or (turned and now - self.lastInput >= self.TURN_INTERVAL):
            self.held, self.dir = held, direction
            self.seq = (self.seq + 1) & 0xff
            flags = (INPUT_HELD if held else 0) | (INPUT_DASH if self.dashNext else 0)
            dg = PyDatagram()
            dg.addUint16(CLIENT_INPUT)
            dg.appendData(packInput(self.seq, self.step + 1, flags, direction))
            self.send(dg)
            self.inputs += 1
            self.lastInput = now
            self.sent.append((self.step + 1, held, direction, self.dashNext))
            self.dashNext = False
        self._simulate(target)

        if now - self.lastPing >= 2.0:
            self.lastPing = now
            self.pingContext = (self.pingContext + 1) & 0xffff
            self.game.sendUpdate('ping', [self.pingContext])
        if self.chatty and now - self.lastChat > 5.0:
            self.lastChat = now
            self.game.sendUpdate('requestChat', ['hello from %s' % self.botName])
        self.flush()
        return Task.cont


class _Rules:
    """The game's rules (a dict from setRules) as motion.stepMovement reads them."""
    __slots__ = ('maxSpeed', 'accel', 'dashSpeed', 'dashTime', 'dashCooldown', 'playerRadius')

    def __init__(self, rules):
        for key in self.__slots__:
            setattr(self, key, rules[key])
