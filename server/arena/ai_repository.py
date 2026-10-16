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
    CLIENT_HELLO_ARENA, MOVE_ENTRY_SIZE, PROTOCOL_VERSION, ROLE_AI, SERVER_MOVES,
    SERVER_REJECT_ARENA, SERVER_SLOTS, ZONE_ARENA, ZONE_GLOBAL, unpackState)


class ArenaTimeManagerAI(TimeManagerAI):
    """The stock TimeManagerAI prints every request. Same reply, no spam."""

    def requestServerTime(self, context):
        requesterId = self.air.getAvatarIdFromSender()
        if requesterId:
            timestamp = globalClockDelta.getRealNetworkTime(bits=32)
            self.sendUpdateToAvatarId(requesterId, 'serverTime', [context, timestamp])


class ArenaAIRepository(ClientRepository):
    notify = DirectNotifyGlobal.directNotify.newCategory('ArenaAIRepository')

    def __init__(self, port, token, dcFileNames, rules, onReady=None, onLost=None):
        ClientRepository.__init__(self, dcFileNames=dcFileNames, dcSuffix='AI',
                                  connectMethod=ClientRepository.CM_NET,
                                  threadedNet=False)
        # DistributedObjectAI finds its dclass by Python class name.
        self.dclassesByName['ArenaTimeManagerAI'] = self.dclassesByName['TimeManagerAI']
        self.token = token
        self.rules = rules
        self.onReady = onReady
        self.onLost = onLost
        self.game = None
        self.timeManager = None
        self.slotToDoId = {}      # movement slots (SERVER_SLOTS)
        self.doIdToSlot = {}
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
            distObj=DistributedArenaGameAI(self, self.rules), zoneId=ZONE_ARENA)
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
        if msgType == SERVER_MOVES:
            self._handleMoves(di)
        elif msgType == SERVER_SLOTS:
            self._handleSlots(di)
        elif msgType == SERVER_REJECT_ARENA:
            code = di.getUint16()
            reason = di.getString()
            self.notify.warning('server rejected the AI: %s (%d)' % (reason, code))
        else:
            self.notify.warning('unexpected message type %d' % msgType)

    def _handleSlots(self, di):
        while di.getRemainingSize() >= 5:
            slot = di.getUint8()
            doId = di.getUint32()
            old = self.slotToDoId.pop(slot, None)
            if old is not None and self.doIdToSlot.get(old) == slot:
                del self.doIdToSlot[old]
            if doId:
                self.slotToDoId[slot] = doId
                self.doIdToSlot[doId] = slot

    def _handleMoves(self, di):
        data = di.getRemainingBytes()
        if not data or len(data) != 1 + data[0] * MOVE_ENTRY_SIZE or self.game is None:
            return
        for offset in range(1, len(data), MOVE_ENTRY_SIZE):
            player = self.doId2do.get(self.slotToDoId.get(data[offset]))
            if player is not None and getattr(player, 'registered', False):
                self.game.onPlayerState(player, *unpackState(data, offset + 1))

    def slotOf(self, doId):
        return self.doIdToSlot.get(doId)

    def lostConnection(self):
        self.notify.warning('AI lost its connection to the server')
        if self.onLost:
            self.onLost()

    def _flush(self, task):
        if self.isConnected():
            self.flush()
        return Task.cont
