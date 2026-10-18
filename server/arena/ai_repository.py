"""The AI repository: a ClientRepository with dcSuffix='AI' that connects to
the ServerRepository like any client (see "AI Repositories" in the Panda3D
manual) and owns the game objects."""

from direct.directnotify import DirectNotifyGlobal
from direct.distributed.ClientRepository import ClientRepository
from direct.distributed.ClockDelta import globalClockDelta
from direct.distributed.PyDatagram import PyDatagram
from direct.distributed.TimeManagerAI import TimeManagerAI
from direct.task import Task
from panda3d.core import URLSpec
from panda3d.direct import CConnectionRepository

from .DistributedArenaGameAI import DistributedArenaGameAI
from .protocol import (
    AI_FRAME, AI_SLOT, AI_SLOT_MSG, CLIENT_HELLO_ARENA, INPUT_ENTRY_SIZE, PROTOCOL_VERSION, ROLE_AI, SERVER_CLIENT_LEFT, SERVER_INPUTS, SERVER_REJECT_ARENA, ZONE_ARENA,
    ZONE_GLOBAL, unpackInput, wrapStep)


class ArenaTimeManagerAI(TimeManagerAI):
    """The stock TimeManagerAI prints every request. Same reply, no spam."""

    def requestServerTime(self, context):
        requesterId = self.air.getAvatarIdFromSender()
        if requesterId:
            timestamp = globalClockDelta.getRealNetworkTime(bits=32)
            self.sendUpdateToAvatarId(requesterId, 'serverTime', [context, timestamp])


class ArenaAIRepository(ClientRepository):
    notify = DirectNotifyGlobal.directNotify.newCategory('ArenaAIRepository')

    def __init__(self, port, token, dcFileNames, rules, onReady=None, onLost=None, batchRate=None):
        ClientRepository.__init__(self, dcFileNames=dcFileNames, dcSuffix='AI',
                                  connectMethod=ClientRepository.CM_NET,
                                  threadedNet=False)
        # DistributedObjectAI finds its dclass by Python class name.
        self.dclassesByName['ArenaTimeManagerAI'] = self.dclassesByName['TimeManagerAI']
        self.token = token
        self.rules = rules
        self.batchRate = batchRate
        self.onReady = onReady
        self.onLost = onLost
        self.game = None
        self.timeManager = None
        self.accept(self.uniqueName('createReady'), self._createReady)
        self.connect([URLSpec('tcp://127.0.0.1:%d' % port)],
                     successCallback=self._connected,
                     failureCallback=self._connectFailed)
        taskMgr.add(self._flush, 'aiFlush', sort=999)

    def _connected(self):
        dg = PyDatagram()
        dg.addUint16(CLIENT_HELLO_ARENA)
        dg.addUint32(PROTOCOL_VERSION)
        dg.addUint32(self.hashVal)
        dg.addUint8(ROLE_AI)
        dg.addString(self.token)
        dg.addString('arena-ai')
        self.send(dg)

    def _connectFailed(self, statusCode, statusString):
        self.notify.error('AI could not reach the server: %s %s' % (statusCode, statusString))

    def _createReady(self):
        self.ignore(self.uniqueName('createReady'))
        # The TimeManager lives in the global zone, as in the manual.
        self.timeManager = self.createDistributedObject(
            className='TimeManagerAI', distObj=ArenaTimeManagerAI(self), zoneId=ZONE_GLOBAL)
        self.game = self.createDistributedObject(
            className='DistributedArenaGameAI',
            distObj=DistributedArenaGameAI(self, self.rules, batchRate=self.batchRate),
            zoneId=ZONE_ARENA)
        # Listen to the arena so player objects and their updates reach us.
        self.setInterestZones([ZONE_GLOBAL, ZONE_ARENA])
        self.notify.info('AI ready: TimeManager %d, game %d'
                         % (self.timeManager.doId, self.game.doId))
        if self.onReady:
            self.onReady()

    def startReaderPollTask(self):
        # Same as ConnectionRepository's, but at sort 5: after the server
        # repository has read and flushed this tick's client messages.
        self.stopReaderPollTask()
        self.accept(CConnectionRepository.getOverflowEventName(), self.handleReaderOverflow)
        self.readerPollTaskObj = taskMgr.add(
            self.readerPollUntilEmpty, self.uniqueName('readerPollTask'), sort=5)

    def deallocateChannel(self, doId):
        """Called by DistributedObjectAI.delete() when an object goes away,
        e.g. a player's avatar after the player disconnected (see "AI
        Repositories" in the manual). The game already cleaned up in
        DistributedArenaPlayerAI.delete(); nothing else is allocated per
        channel by the CMU server."""
        self.notify.debug('object %d deleted' % doId)

    def handleDatagram(self, di):
        try:
            ClientRepository.handleDatagram(self, di)
        except Exception:
            # The server validates every payload, so this is an AI bug; log it
            # and keep serving everyone else.
            import traceback
            self.notify.warning('error handling message:\n' + traceback.format_exc())

    def handleMessageType(self, msgType, di):
        if msgType == SERVER_INPUTS:
            self._handleInputs(di)
        elif msgType == SERVER_CLIENT_LEFT:
            clientId = di.getUint32()
            if self.game is not None:
                self.game.onClientLeft(clientId)
        elif msgType == SERVER_REJECT_ARENA:
            code = di.getUint16()
            reason = di.getString()
            self.notify.warning('server rejected the AI: %s (%d)' % (reason, code))
        else:
            self.notify.warning('unexpected message type %d' % msgType)

    def _handleInputs(self, di):
        """Players' inputs as they arrived this tick (protocol.py)."""
        data = di.getRemainingBytes()
        game = self.game
        if not data or len(data) != 1 + data[0] * INPUT_ENTRY_SIZE or game is None:
            return
        now = game.now()
        nowStep = game.nowStep(now)
        bySlot = {p.slot: p for p in game.players.values()}
        for offset in range(1, len(data), INPUT_ENTRY_SIZE):
            player = bySlot.get(data[offset])
            if player is not None:
                player.sim.onInput(*unpackInput(data, offset + 1), nowStep=nowStep, now=now)

    # --------------------------------------------------- AI -> server

    def sendSlot(self, slot, clientId, doId):
        """Binds a slot to a player's client and object (doId 0 frees it)."""
        dg = PyDatagram()
        dg.addUint16(AI_SLOT)
        dg.appendData(AI_SLOT_MSG.pack(slot, clientId, doId))
        self.send(dg)

    def sendFrame(self, step, samples, selfs, events):
        """One batch, sent at `step` (protocol.py AI_FRAME): [(slot, STATE
        bytes)] for everyone in the arena, [(slot, SELF bytes)] for the
        players' own clients, [event bytes] (frame.packAiEvent) for everyone."""
        first = True
        while first or samples or selfs or events:
            first = False
            parts = [wrapStep(step).to_bytes(2, 'little')]
            for items in (samples, selfs):
                parts.append(bytes((min(len(items), 255),)))
                parts.extend(bytes((slot,)) + data for slot, data in items[:255])
            parts.append(bytes((min(len(events), 255),)))
            parts.extend(events[:255])
            samples, selfs, events = samples[255:], selfs[255:], events[255:]
            dg = PyDatagram()
            dg.addUint16(AI_FRAME)
            dg.appendData(b''.join(parts))
            self.send(dg)

    def deletePlayer(self, player):
        """Removes a player object we created, here and everywhere."""
        doId = player.doId
        if doId in self.doId2do:
            self.sendDeleteMsg(doId)
            self.deleteObject(doId)

    def lostConnection(self):
        self.notify.warning('AI lost its connection to the server')
        if self.onLost:
            self.onLost()

    def _flush(self, task):
        if self.isConnected():
            self.flush()
        return Task.cont
