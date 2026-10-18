"""Hardened ServerRepository for Panda Arena.

direct.distributed.ServerRepository is a minimal relay: it trusts every
client, crashes on several malformed messages (unknown class number, unknown
field, targeted update to an unknown client), never times anybody out and can
be stalled by a client that stops reading. This subclass keeps its protocol
(so the stock ClientRepository and our C++ port both work) and adds:

* a version/DC-hash handshake before a client gets its doId range,
* a role per connection: one trusted AI, N players behind allow-lists,
* full validation of generate/update payloads against the DC file before
  anything is relayed, so peers never see malformed data,
* per-client message/byte rate limits, handshake and idle timeouts,
* a send timeout on every socket, so a stalled client is dropped instead of
  blocking the server,
* batched writes (collect-tcp) flushed once per tick,
* an authoritative AI (protocol.py): players' inputs (CLIENT_INPUT) go to
  the AI only, batched once per tick; each batch from the AI (samples,
  events, SELF) becomes one bit-packed SERVER_FRAME per client (frame.py),
  one TCP packet; a cache of the newest sample per player so newcomers see
  everyone at once,
* an object's generate reaches each client once, not again whenever
  someone else starts listening to its zone,
* no client ever receives another client's address: everything is relayed,
  and nothing relayed contains one,
* no exception ever escapes into the task manager.
"""

import hmac
import socket
import struct
import sys
import time
import traceback

from direct.directnotify import DirectNotifyGlobal
from direct.distributed.MsgTypesCMU import (
    CLIENT_DISCONNECT_CMU, CLIENT_OBJECT_GENERATE_CMU, CLIENT_OBJECT_UPDATE_FIELD,
    CLIENT_OBJECT_UPDATE_FIELD_TARGETED_CMU, CLIENT_SET_INTEREST_CMU,
    OBJECT_DELETE_CMU, OBJECT_DISABLE_CMU, OBJECT_GENERATE_CMU,
    OBJECT_SET_ZONE_CMU, OBJECT_UPDATE_FIELD_CMU, REQUEST_GENERATES_CMU)
from direct.distributed.PyDatagram import PyDatagram
from direct.distributed.ServerRepository import ServerRepository
from direct.task import Task
from panda3d.core import (
    ConnectionWriter, DatagramIterator, NetAddress, PointerToConnection)
from panda3d.direct import DCPacker

from . import policy
from .frame import (
    BitWriter, FrameError, unpackAiEvents, writeEvent, writeHeader, writeSample, writeSelf)
from .protocol import (
    AI_FRAME, AI_SLOT, AI_SLOT_MSG, CLIENT_HELLO_ARENA, CLIENT_INPUT, INPUT_SIZE, MOVE_ENTRY_SIZE,
    NO_SLOT, PROTOCOL_VERSION, REJECT_BAD_DC_HASH, REJECT_BAD_TOKEN, REJECT_BAD_VERSION,
    REJECT_FLOOD, REJECT_PROTOCOL_ERROR, REJECT_SERVER_FULL, REJECT_SHUTDOWN, REJECT_TIMEOUT,
    ROLE_AI, ROLE_PLAYER, SELF_ENTRY_SIZE, SERVER_CLIENT_LEFT, SERVER_FRAME, SERVER_INPUTS,
    SERVER_REJECT_ARENA, SERVER_SLOTS, SLOT_ENTRY, STATE, ZONE_ARENA)


class ProtocolError(Exception):
    """A client sent something malformed or forbidden; it gets kicked."""


class TokenBucket:
    __slots__ = ('rate', 'burst', 'tokens', 'stamp')

    def __init__(self, rate, burst, now):
        self.rate = rate
        self.burst = burst
        self.tokens = burst
        self.stamp = now

    def take(self, amount, now):
        self.tokens = min(self.burst, self.tokens + (now - self.stamp) * self.rate)
        self.stamp = now
        self.tokens -= amount
        return self.tokens >= 0.0


class ArenaServerRepository(ServerRepository):
    notify = DirectNotifyGlobal.directNotify.newCategory('ArenaServerRepository')

    # Limits for player connections. The AI connection is exempt.
    HELLO_TIMEOUT = 5.0           # seconds to send a valid hello
    IDLE_TIMEOUT = 15.0           # clients ping every 2 s: silent this long (a frozen or lost phone), gone
    MAX_DATAGRAM = 4096           # biggest legal player message is ~300 bytes
    MSG_RATE, MSG_BURST = 120.0, 360.0
    BYTE_RATE, BYTE_BURST = 32768.0, 98304.0
    SEND_TIMEOUT = 0.25           # a client that does not drain its socket is dropped
    MAX_PENDING = 32              # connections that have not said hello yet
    HAVE_TCP_INFO = sys.platform.startswith('linux') and hasattr(socket, 'TCP_INFO')

    class Client(ServerRepository.Client):
        def __init__(self, connection, netAddress, doIdBase):
            ServerRepository.Client.__init__(self, connection, netAddress, doIdBase)
            now = time.monotonic()
            self.accepted = False
            self.role = None
            self.info = ''
            self.connectTime = now
            self.lastRecv = now
            self.msgBucket = None
            self.byteBucket = None
            self.slot = None          # slot of this client's player (bound by the AI)
            self.frameSent = None     # sent step of the last SERVER_FRAME to it
            self.knownSlots = set()   # slots whose newest sample it has (a frame baseline)
            self.bytesOut = 0         # everything sent to it, with the 2-byte length
            self.packetsOut = 0       # TCP writes (one per tick with something to send)
            self.tcpInfo = None       # (bytes out, bytes in, segments out, segments in)

        def __repr__(self):
            role = {ROLE_AI: 'ai', ROLE_PLAYER: 'player'}.get(self.role, 'pending')
            return '<%s %s %s>' % (role, self.doIdBase, self.netAddress)

    def __init__(self, tcpPort, bindAddress='0.0.0.0', dcFileNames=None, maxPlayers=16,
                 aiToken='', threadedNet=False):
        # threadedNet stays off: with a reader thread, closing a connection
        # (kick, timeout) races the thread's poll and Panda aborts the process
        # (Socket_fdset::isSetForNative assertion). Polling once per tick is
        # plenty at this scale and keeps everything on one thread.
        self.maxPlayers = maxPlayers
        self.aiToken = aiToken
        self.aiClient = None
        self.stats = {'msgsIn': 0, 'bytesIn': 0, 'msgsOut': 0, 'kicks': 0, 'inputs': 0, 'moves': 0}
        # Player slots, bound by the AI (AI_SLOT).
        self.slotOwners = {}      # slot -> the player's client
        self.slotDoIds = {}       # slot -> the player's object (owned by the AI)
        self.pendingInputs = []   # INPUT entries (slot + 5 bytes) for the AI, this tick
        self.lastStates = {}      # slot -> (sent, frame baseline) of its newest sample
        try:
            ServerRepository.__init__(self, tcpPort, serverAddress=bindAddress or '0.0.0.0',
                                      dcFileNames=dcFileNames, threadedNet=threadedNet)
        except Exception as e:
            raise RuntimeError('cannot listen on %s:%s (%s); is the port already in use?'
                               % (bindAddress or '*', tcpPort, e))
        if not self.tcpRendezvous:
            raise RuntimeError('cannot listen on %s:%s; is the port already in use?'
                               % (bindAddress or '*', tcpPort))

        # Synchronous writer: with collect-tcp a send only appends to the
        # connection's buffer, and kick() needs the reject message queued
        # before the socket is closed (a threaded writer could reorder that).
        self.cw = ConnectionWriter(self.qcm, 0)

        # Flush every connection once per tick on the main thread, instead of
        # the stock periodic flush task on its own task chain.
        taskMgr.remove('flushTask')
        # Early flush right after the reader (sort 0), so the in-process AI
        # (reader at sort 5) sees this tick's messages in this same tick.
        taskMgr.add(self.flushPending, 'arenaServerEarlyFlush', sort=1)
        taskMgr.add(self.flushPending, 'arenaServerFlush', sort=1000)
        taskMgr.remove('clientHardDisconnect')
        taskMgr.doMethodLater(0.25, self.housekeeping, 'arenaServerHousekeeping')

        self._classInfo = {}
        self._checkDcFile()

    # ------------------------------------------------------------------ setup

    def _checkDcFile(self):
        """The CMU generate format packs every *required* field, but receivers
        only unpack required fields that are also *broadcast*. Make sure the
        two sets agree for every class, or messages would desynchronise."""
        for dclass in self.dclassesByNumber.values():
            info = self._getClassInfo(dclass)
            for field in info['allRequired']:
                if not field.isBroadcast():
                    raise RuntimeError('%s.%s is required but not broadcast'
                                       % (dclass.getName(), field.getName()))

    def _getClassInfo(self, dclass):
        info = self._classInfo.get(dclass.getNumber())
        if info is None:
            required = []
            allRequired = []
            for i in range(dclass.getNumInheritedFields()):
                field = dclass.getInheritedField(i)
                if field.asMolecularField() is not None or not field.isRequired():
                    continue
                allRequired.append(field)
                if field.isBroadcast():
                    required.append(field)
            info = {'required': required, 'allRequired': allRequired}
            self._classInfo[dclass.getNumber()] = info
        return info

    # ------------------------------------------------------------ connections

    def listenerPoll(self, task):
        while self.qcl.newConnectionAvailable():
            rendezvous = PointerToConnection()
            netAddress = NetAddress()
            newConnection = PointerToConnection()
            if not self.qcl.getNewConnection(rendezvous, netAddress, newConnection):
                break
            connection = newConnection.p()

            pending = sum(1 for c in self.clientsByConnection.values() if not c.accepted)
            if pending >= self.MAX_PENDING:
                self.notify.warning('too many pending connections, refusing %s' % netAddress)
                self.qcm.closeConnection(connection)
                continue

            connection.setNoDelay(True)
            self._setSendTimeout(connection)

            id = self.idAllocator.allocate()
            client = self.Client(connection, netAddress, id * self.doIdRange + 1)
            self.clientsByConnection[connection] = client
            self.clientsByDoIdBase[client.doIdBase] = client
            self.qcr.addConnection(connection)
            self.lastConnection = connection
            self.notify.info('connection from %s (id %s)' % (netAddress, client.doIdBase))
            # sendDoIdRange() is deferred until the hello is accepted.
        return Task.cont

    def _setSendTimeout(self, connection):
        try:
            fd = connection.getSocket().GetSocket()
            sock = socket.socket(fileno=fd)
            try:
                if sys.platform == 'win32':
                    value = struct.pack('I', int(self.SEND_TIMEOUT * 1000))
                else:
                    sec = int(self.SEND_TIMEOUT)
                    value = struct.pack('ll', sec, int((self.SEND_TIMEOUT - sec) * 1e6))
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDTIMEO, value)
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            finally:
                sock.detach()  # the socket belongs to Panda; do not close it
        except (OSError, ValueError, AttributeError) as e:
            self.notify.warning('could not set socket options: %s' % e)

    def sendDoIdRange(self, client):
        if client.accepted:
            ServerRepository.sendDoIdRange(self, client)

    def getDoIdBase(self, doId):
        # The stock version maps the last doId of every range to the next
        # client (doIdBase is 1-based); this one is exact.
        return ((doId - 1) // self.doIdRange) * self.doIdRange + 1

    def numPlayers(self):
        return sum(1 for c in self.clientsByConnection.values()
                   if c.accepted and c.role == ROLE_PLAYER)

    def kick(self, client, code, reason):
        if self.clientsByConnection.get(client.connection) is not client:
            return
        self.stats['kicks'] += 1
        self.notify.info('dropping %s: %s' % (client, reason))
        dg = PyDatagram()
        dg.addUint16(SERVER_REJECT_ARENA)
        dg.addUint16(code)
        dg.addString(reason[:200])
        self.cw.send(dg, client.connection)
        client.connection.flush()
        self.handleClientDisconnect(client)

    def handleClientDisconnect(self, client):
        if self.clientsByConnection.get(client.connection) is not client:
            return
        if client is self.aiClient:
            self.notify.warning('the AI repository disconnected')
            self.aiClient = None
        self.notify.info('%s left (%s)%s'
                         % (client, client.info or 'no hello', self._traffic(client)))
        for zoneId in client.currentInterestZoneIds:
            for obj in self.objectsByZoneId.get(zoneId, ()):
                self._seenBy(obj).discard(client)
        ServerRepository.handleClientDisconnect(self, client)
        if client.slot is not None and self.slotOwners.get(client.slot) is client:
            del self.slotOwners[client.slot]  # its player stays until the AI removes it
        client.slot = None
        if client.accepted and client.role == ROLE_PLAYER and self.aiClient is not None:
            dg = PyDatagram()
            dg.addUint16(SERVER_CLIENT_LEFT)
            dg.addUint32(client.doIdBase)
            self._send(self.aiClient, dg)

    def _traffic(self, client):
        """What we sent a player and what it sent us, for the log."""
        if client.role != ROLE_PLAYER:
            return ''
        secs = max(1e-3, time.monotonic() - client.connectTime)
        text = '; sent it %.0f B/s in %.1f packets/s' % (client.bytesOut / secs,
                                                           client.packetsOut / secs)
        if client.tcpInfo is not None:
            # The kernel's count, with what the game does not see: TCP
            # segments (each with 40-60 bytes of headers) and acknowledgements.
            out, received, segsOut, segsIn = client.tcpInfo
            text += '; TCP: out %.0f B/s in %.1f segments/s, in %.0f B/s in %.1f segments/s' % (
                out / secs, segsOut / secs, received / secs, segsIn / secs)
        return text

    def _sampleTcpInfo(self, client):
        """Linux only: the connection's byte and segment counters."""
        try:
            fd = client.connection.getSocket().GetSocket()
            sock = socket.socket(fileno=fd)
            try:
                raw = sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 144)
            finally:
                sock.detach()
            if len(raw) >= 144:
                client.tcpInfo = struct.unpack_from('<QQII', raw, 120)
        except (OSError, ValueError, AttributeError, struct.error):
            pass

    def shutdown(self, reason='server shutting down'):
        for client in list(self.clientsByConnection.values()):
            if client is not self.aiClient:
                self.kick(client, REJECT_SHUTDOWN, reason)
        self.flushPending()

    # ------------------------------------------------------------ main loop

    def flushPending(self, task=None):
        self._emitInputs()
        if self.needsFlush:
            flush, self.needsFlush = self.needsFlush, set()
            for client in flush:
                if client.connection in self.clientsByConnection:
                    client.connection.flush()
                    client.packetsOut += 1
        return Task.cont

    def housekeeping(self, task):
        try:
            now = time.monotonic()
            # Connections the OS reported as reset (including send timeouts).
            while self.qcm.resetConnectionAvailable():
                conn = PointerToConnection()
                if not self.qcm.getResetConnection(conn):
                    break
                client = self.clientsByConnection.get(conn.p())
                if client:
                    self.handleClientDisconnect(client)
            for client in list(self.clientsByConnection.values()):
                if not self.qcr.isConnectionOk(client.connection):
                    self.handleClientDisconnect(client)
                elif not client.accepted:
                    if now - client.connectTime > self.HELLO_TIMEOUT:
                        self.kick(client, REJECT_TIMEOUT, 'no hello received')
                elif client.role == ROLE_PLAYER and now - client.lastRecv > self.IDLE_TIMEOUT:
                    self.kick(client, REJECT_TIMEOUT, 'connection timed out')
                elif client.role == ROLE_PLAYER and self.HAVE_TCP_INFO:
                    self._sampleTcpInfo(client)
        except Exception:
            self.notify.warning('housekeeping failed:\n' + traceback.format_exc())
        return Task.again

    def handleDatagram(self, datagram):
        client = self.clientsByConnection.get(datagram.getConnection())
        if client is None:
            return  # queued before the client was dropped
        try:
            self._handleClientDatagram(client, datagram)
        except ProtocolError as e:
            self.kick(client, REJECT_PROTOCOL_ERROR, str(e))
        except Exception:
            # Never let one bad message take the whole server down.
            self.notify.warning('error handling message from %s:\n%s'
                                % (client, traceback.format_exc()))
            self.kick(client, REJECT_PROTOCOL_ERROR, 'malformed message')

    def _handleClientDatagram(self, client, datagram):
        now = time.monotonic()
        size = datagram.getLength()
        self.stats['msgsIn'] += 1
        self.stats['bytesIn'] += size
        client.lastRecv = now

        if client.role != ROLE_AI:
            if size > self.MAX_DATAGRAM:
                raise ProtocolError('message too large (%d bytes)' % size)
            if client.msgBucket is None:
                client.msgBucket = TokenBucket(self.MSG_RATE, self.MSG_BURST, now)
                client.byteBucket = TokenBucket(self.BYTE_RATE, self.BYTE_BURST, now)
            if not (client.msgBucket.take(1, now) and client.byteBucket.take(size, now)):
                self.kick(client, REJECT_FLOOD, 'too many messages')
                return

        dgi = DatagramIterator(datagram)
        if dgi.getRemainingSize() < 2:
            raise ProtocolError('empty message')
        msgType = dgi.getUint16()

        if msgType == CLIENT_INPUT and client.accepted:
            self._handleInput(client, dgi)  # the most frequent player message
            return
        if msgType in (AI_FRAME, AI_SLOT):
            if client is not self.aiClient:
                raise ProtocolError('message %d is for the AI only' % msgType)
            if msgType == AI_FRAME:
                self._relayFrame(dgi)
            else:
                self._bindSlot(dgi)
            return
        if not client.accepted:
            if msgType == CLIENT_HELLO_ARENA:
                self._handleHello(client, dgi)
            elif msgType == CLIENT_DISCONNECT_CMU:
                self.handleClientDisconnect(client)
            else:
                raise ProtocolError('expected hello, got message %d' % msgType)
            return

        if msgType == CLIENT_OBJECT_UPDATE_FIELD:
            self._handleUpdate(client, dgi, targeted=False)
        elif msgType == CLIENT_OBJECT_UPDATE_FIELD_TARGETED_CMU:
            self._handleUpdate(client, dgi, targeted=True)
        elif msgType == CLIENT_OBJECT_GENERATE_CMU:
            self._handleGenerate(client, dgi)
        elif msgType == CLIENT_SET_INTEREST_CMU:
            self._handleSetInterest(client, dgi)
        elif msgType == OBJECT_DELETE_CMU:
            self._handleDelete(client, dgi)
        elif msgType == OBJECT_SET_ZONE_CMU:
            self._handleSetZone(client, dgi)
        elif msgType == CLIENT_DISCONNECT_CMU:
            self.handleClientDisconnect(client)
        else:
            raise ProtocolError('unknown message type %d' % msgType)

    # ------------------------------------------------------------ handshake

    def _handleHello(self, client, dgi):
        if dgi.getRemainingSize() < 13:
            raise ProtocolError('short hello')
        version = dgi.getUint32()
        dcHash = dgi.getUint32()
        role = dgi.getUint8()
        token = dgi.getString()
        info = dgi.getString()
        if dgi.getRemainingSize():
            raise ProtocolError('trailing bytes in hello')
        client.info = ''.join(ch for ch in info[:80] if ch.isprintable())

        if version != PROTOCOL_VERSION:
            self.kick(client, REJECT_BAD_VERSION,
                      'protocol version %d, server speaks %d' % (version, PROTOCOL_VERSION))
            return
        if dcHash != self.hashVal:
            self.kick(client, REJECT_BAD_DC_HASH,
                      'DC files differ from the server (hash %d, expected %d)'
                      % (dcHash, self.hashVal))
            return
        if role == ROLE_AI:
            if self.aiClient is not None or not self.aiToken or \
                    not hmac.compare_digest(token.encode(), self.aiToken.encode()):
                self.kick(client, REJECT_BAD_TOKEN, 'not authorised')
                return
            self.aiClient = client
        elif role == ROLE_PLAYER:
            if self.numPlayers() >= self.maxPlayers:
                self.kick(client, REJECT_SERVER_FULL,
                          'server is full (%d players)' % self.maxPlayers)
                return
        else:
            raise ProtocolError('unknown role %d' % role)

        client.accepted = True
        client.role = role
        self.notify.info('%s joined: %s' % (client, client.info))
        self.sendDoIdRange(client)

    # ------------------------------------------------------------ objects

    def _handleGenerate(self, client, dgi):
        if dgi.getRemainingSize() < 10:
            raise ProtocolError('short generate')
        zoneId = dgi.getUint32()
        classId = dgi.getUint16()
        doId = dgi.getUint32()
        payload = dgi.getRemainingBytes()

        if self.getDoIdBase(doId) != client.doIdBase:
            raise ProtocolError('doId %d outside the assigned range' % doId)
        dclass = self.dclassesByNumber.get(classId)
        if dclass is None or dclass.isStruct() or dclass.isBogusClass():
            raise ProtocolError('unknown class %d' % classId)

        obj = client.objectsByDoId.get(doId)
        isAI = client is self.aiClient
        if not isAI:
            limit = policy.PLAYER_CREATABLE.get(dclass.getName())
            if limit is None:
                raise ProtocolError('may not create %s' % dclass.getName())
            if zoneId not in policy.PLAYER_OBJECT_ZONES:
                raise ProtocolError('may not create objects in zone %d' % zoneId)
            if obj is None:
                owned = sum(1 for o in client.objectsByDoId.values() if o.dclass is dclass)
                if owned >= limit:
                    raise ProtocolError('too many %s objects' % dclass.getName())

        payload = self._validateGenerate(dclass, payload, isAI)

        if obj is not None:
            if obj.dclass is not dclass:
                raise ProtocolError('doId %d is already a %s' % (doId, obj.dclass.getName()))
            self.setObjectZone(client, obj, zoneId)
        else:
            obj = self.Object(doId, zoneId, dclass)
            client.objectsByDoId[doId] = obj
            client.objectsByZoneId.setdefault(zoneId, set()).add(obj)
            self.objectsByZoneId.setdefault(zoneId, set()).add(obj)
            self.updateClientInterestZones(client)

        dg = PyDatagram()
        dg.addUint16(OBJECT_GENERATE_CMU)
        dg.addUint32(client.doIdBase)
        dg.addUint32(zoneId)
        dg.addUint16(classId)
        dg.addUint32(doId)
        dg.appendData(payload)
        # The owner sends an object again whenever someone starts listening to
        # its zone (REQUEST_GENERATES_CMU); only those who lack it get it.
        seenBy = self._seenBy(obj)
        for other in self.zonesToClients.get(zoneId, ()):
            if other is not client and other not in seenBy:
                seenBy.add(other)
                self._send(other, dg)

    @staticmethod
    def _seenBy(obj):
        """The clients that have the object's generate (and no disable since)."""
        seenBy = getattr(obj, 'seenBy', None)
        if seenBy is None:
            seenBy = obj.seenBy = set()
        return seenBy

    def _validateGenerate(self, dclass, payload, isAI):
        """Checks the required + optional field blob of a generate message.
        Returns the payload to relay (optional fields players may not set
        are stripped, anything malformed raises ProtocolError)."""
        packer = DCPacker()
        packer.setUnpackData(payload)
        for field in self._getClassInfo(dclass)['required']:
            packer.beginUnpack(field)
            packer.unpackValidate()
            if not packer.endUnpack():
                raise ProtocolError('bad %s.%s in generate' % (dclass.getName(), field.getName()))
        requiredEnd = packer.getNumUnpackedBytes()

        if len(payload) - requiredEnd < 2:
            raise ProtocolError('generate is missing the optional field count')
        count = packer.rawUnpackUint16()
        if count > 64:
            raise ProtocolError('too many optional fields')

        allowed = policy.PLAYER_GENERATE_OPTIONAL.get(dclass.getName(), ())
        kept = []
        dropped = False
        for _ in range(count):
            if len(payload) - packer.getNumUnpackedBytes() < 2:
                raise ProtocolError('truncated generate')
            fieldId = packer.rawUnpackUint16()
            field = dclass.getFieldByIndex(fieldId)
            if field is None or field.asMolecularField() is not None:
                raise ProtocolError('bad optional field %d' % fieldId)
            start = packer.getNumUnpackedBytes()
            packer.beginUnpack(field)
            packer.unpackValidate()
            if not packer.endUnpack():
                raise ProtocolError('bad %s.%s in generate' % (dclass.getName(), field.getName()))
            name = field.getName()
            if not isAI:
                if name in policy.GENERATE_FIELDS_TO_DROP:
                    dropped = True
                    continue
                if name not in allowed:
                    raise ProtocolError('may not set %s.%s' % (dclass.getName(), name))
            kept.append((fieldId, payload[start:packer.getNumUnpackedBytes()]))

        if packer.getNumUnpackedBytes() != len(payload):
            raise ProtocolError('trailing bytes in generate')
        if not dropped:
            return payload
        out = bytearray(payload[:requiredEnd])
        out += struct.pack('<H', len(kept))
        for fieldId, blob in kept:
            out += struct.pack('<H', fieldId)
            out += blob
        return bytes(out)

    def _handleUpdate(self, client, dgi, targeted):
        if dgi.getRemainingSize() < (10 if targeted else 6):
            raise ProtocolError('short update')
        targetId = dgi.getUint32() if targeted else None
        doId = dgi.getUint32()
        fieldId = dgi.getUint16()
        payload = dgi.getRemainingBytes()

        owner = self.clientsByDoIdBase.get(self.getDoIdBase(doId))
        obj = owner.objectsByDoId.get(doId) if owner else None
        if obj is None:
            # Normal right after an object was deleted; nothing to relay.
            return
        field = obj.dclass.getFieldByIndex(fieldId)
        if field is None:
            raise ProtocolError('no field %d in %s' % (fieldId, obj.dclass.getName()))

        isOwner = client is owner
        if client is not self.aiClient:
            className = obj.dclass.getName()
            name = field.getName()
            if targeted:
                raise ProtocolError('players may not send targeted updates')
            if isOwner:
                ok = name in policy.PLAYER_OWN_FIELDS.get(className, ())
            else:
                ok = (name in policy.PLAYER_REQUEST_FIELDS.get(className, ())
                      and field.hasKeyword('p2p'))
            if not ok:
                raise ProtocolError('may not send %s.%s' % (className, name))
        elif not isOwner and not (field.hasKeyword('clsend') or field.hasKeyword('p2p')):
            self.notify.warning('AI tried to send %s on object %d it does not own'
                                % (field.getName(), doId))
            return

        if not field.validateRanges(payload):
            raise ProtocolError('bad data for %s.%s' % (obj.dclass.getName(), field.getName()))

        dg = PyDatagram()
        dg.addUint16(OBJECT_UPDATE_FIELD_CMU)
        dg.addUint32(client.doIdBase)
        dg.addUint32(doId)
        dg.addUint16(fieldId)
        dg.appendData(payload)

        if targeted:
            target = self.clientsByDoIdBase.get(targetId)
            if target is not None and target.accepted:
                self._send(target, dg)
        elif field.hasKeyword('p2p'):
            self._send(owner, dg)
        elif field.hasKeyword('broadcast'):
            self.sendToZoneExcept(obj.zoneId, dg, [client])
        elif field.hasKeyword('reflect'):
            self.sendToZoneExcept(obj.zoneId, dg, [])
        else:
            self.notify.warning('%s.%s is neither broadcast nor p2p; dropped'
                                % (obj.dclass.getName(), field.getName()))

    def _handleDelete(self, client, dgi):
        if dgi.getRemainingSize() != 4:
            raise ProtocolError('bad delete')
        doId = dgi.getUint32()
        obj = client.objectsByDoId.get(doId)
        if obj is None:
            return
        dg = PyDatagram()
        dg.addUint16(OBJECT_DELETE_CMU)
        dg.addUint32(doId)
        self.sendToZoneExcept(obj.zoneId, dg, [client])  # the owner deleted it already

        self._unlinkObject(client, obj)
        del client.objectsByDoId[doId]
        self.updateClientInterestZones(client)

    def _unlinkObject(self, client, obj):
        zoneSet = self.objectsByZoneId.get(obj.zoneId)
        if zoneSet is not None:
            zoneSet.discard(obj)
            if not zoneSet:
                del self.objectsByZoneId[obj.zoneId]
        ownSet = client.objectsByZoneId.get(obj.zoneId)
        if ownSet is not None:
            ownSet.discard(obj)
            if not ownSet:
                del client.objectsByZoneId[obj.zoneId]

    def _handleSetZone(self, client, dgi):
        if dgi.getRemainingSize() != 8:
            raise ProtocolError('bad set-zone')
        doId = dgi.getUint32()
        zoneId = dgi.getUint32()
        if client is not self.aiClient and zoneId not in policy.PLAYER_OBJECT_ZONES:
            raise ProtocolError('may not move objects to zone %d' % zoneId)
        obj = client.objectsByDoId.get(doId)
        if obj is not None:
            self.setObjectZone(client, obj, zoneId)

    def setObjectZone(self, owner, obj, zoneId):
        if obj.zoneId == zoneId:
            return
        oldZoneId = obj.zoneId
        self._unlinkObject(owner, obj)
        obj.zoneId = zoneId
        self.objectsByZoneId.setdefault(zoneId, set()).add(obj)
        owner.objectsByZoneId.setdefault(zoneId, set()).add(obj)
        self.updateClientInterestZones(owner)

        dg = PyDatagram()
        dg.addUint16(OBJECT_DISABLE_CMU)
        dg.addUint32(obj.doId)
        seenBy = self._seenBy(obj)
        for client in self.zonesToClients.get(oldZoneId, ()):
            if client is not owner and zoneId not in client.currentInterestZoneIds:
                seenBy.discard(client)
                self._send(client, dg)

    def _handleSetInterest(self, client, dgi):
        remaining = dgi.getRemainingSize()
        if remaining % 4:
            raise ProtocolError('bad interest message')
        zoneIds = set()
        for _ in range(remaining // 4):
            zoneIds.add(dgi.getUint32())
        if client is not self.aiClient:
            if len(zoneIds) > policy.MAX_INTEREST_ZONES or not zoneIds <= policy.PLAYER_INTEREST:
                raise ProtocolError('may not listen to zones %s' % sorted(zoneIds))
        client.explicitInterestZoneIds = zoneIds
        self.updateClientInterestZones(client)

    def updateClientInterestZones(self, client):
        """The stock version, except that only the clients owning objects in a
        newly heard zone (the AI) are asked to send them again, and nothing is
        sent when no zone was dropped."""
        before = client.currentInterestZoneIds
        after = client.explicitInterestZoneIds | set(client.objectsByZoneId.keys())
        if before == after:
            return
        client.currentInterestZoneIds = after
        added, removed = after - before, before - after
        for zoneId in added:
            self.zonesToClients.setdefault(zoneId, set()).add(client)
            dg = PyDatagram()
            dg.addUint16(REQUEST_GENERATES_CMU)
            dg.addUint32(zoneId)
            for owner in list(self.clientsByConnection.values()):
                if owner is not client and owner.objectsByZoneId.get(zoneId):
                    self._send(owner, dg)
        if removed:
            dg = PyDatagram()
            dg.addUint16(OBJECT_DISABLE_CMU)
            for zoneId in removed:
                self.zonesToClients.get(zoneId, set()).discard(client)
                for obj in self.objectsByZoneId.get(zoneId, ()):
                    dg.addUint32(obj.doId)
                    self._seenBy(obj).discard(client)
            self._send(client, dg)
            client.knownSlots.clear()  # samples resume in full when it is back
        if added and client.accepted:
            self._sendZoneMovement(client, added)

    # ------------------------------------------------------------ movement

    def _handleInput(self, client, dgi):
        """A player's input goes to the AI, and to nobody else."""
        if dgi.getRemainingSize() != INPUT_SIZE:
            raise ProtocolError('bad input')
        if client.slot is None:
            return  # no player (yet, or any more)
        self.pendingInputs.append(bytes((client.slot,)) + dgi.getRemainingBytes())
        self.stats['inputs'] += 1

    def _emitInputs(self):
        if not self.pendingInputs or self.aiClient is None:
            self.pendingInputs = []
            return
        pending, self.pendingInputs = self.pendingInputs, []
        for first in range(0, len(pending), 255):
            chunk = pending[first:first + 255]
            dg = PyDatagram()
            dg.addUint16(SERVER_INPUTS)
            dg.appendData(bytes((len(chunk),)) + b''.join(chunk))
            self._send(self.aiClient, dg)

    def _playerObject(self, slot):
        doId = self.slotDoIds.get(slot)
        return self.aiClient.objectsByDoId.get(doId) if doId and self.aiClient else None

    def _relayFrame(self, dgi):
        """One batch from the AI (AI_FRAME): one SERVER_FRAME for each client
        it concerns (frame.py), with the samples of everyone but the client's
        own player, the events, and the client's SELF."""
        data = dgi.getRemainingBytes()
        try:
            sent, samples, selfs, events = self._parseAiFrame(data)
        except (IndexError, struct.error, FrameError):
            self.notify.warning('malformed AI_FRAME')
            return
        # Each sample is coded once against its player's previous sample (for
        # the clients that have it) and once in full (for the others).
        byZone = {}
        for slot, state in samples:
            obj = self._playerObject(slot)
            if obj is None:
                continue
            last = self.lastStates.get(slot)
            full = BitWriter()
            base = writeSample(full, slot, state, sent, None)
            coded = None
            if last is not None:
                coded = BitWriter()
                writeSample(coded, slot, state, sent, last[1])
            self.lastStates[slot] = (sent, base)
            byZone.setdefault(obj.zoneId, []).append((slot, coded, full))
            self.stats['moves'] += 1
        news = None
        if events:
            news = BitWriter()
            for kind, fields in events:
                news.put(1, 1)
                writeEvent(news, kind, fields)
            news.put(0, 1)

        recipients = set(self.zonesToClients.get(ZONE_ARENA, ()))
        for zoneId in byZone:
            recipients.update(self.zonesToClients.get(zoneId, ()))
        recipients.update(self.slotOwners[slot] for slot in selfs if slot in self.slotOwners)
        recipients.discard(self.aiClient)
        for client in recipients:
            w = BitWriter()
            writeHeader(w, sent, client.frameSent)
            useful = False
            for zoneId, entries in byZone.items():
                if zoneId not in client.currentInterestZoneIds:
                    continue
                for slot, coded, full in entries:
                    if slot == client.slot:
                        continue  # its own player: SELF
                    w.put(1, 1)
                    if coded is not None and slot in client.knownSlots:
                        w.extend(coded)
                    else:
                        w.extend(full)
                        client.knownSlots.add(slot)
                    useful = True
            w.put(0, 1)
            if news is not None and ZONE_ARENA in client.currentInterestZoneIds:
                w.extend(news)
                useful = True
            else:
                w.put(0, 1)
            own = selfs.get(client.slot) if client.slot is not None else None
            if own is not None and self.slotOwners.get(client.slot) is client:
                w.put(1, 1)
                writeSelf(w, own, sent)
                useful = True
            else:
                w.put(0, 1)
            if useful:
                self._sendFrame(client, sent, w)

    @staticmethod
    def _parseAiFrame(data):
        """(sent, [(slot, STATE fields)], {slot: SELF bytes}, [(kind, fields)])."""
        sent = data[0] | data[1] << 8
        offset = 3
        samples = []
        for _ in range(data[2]):
            samples.append((data[offset], STATE.unpack_from(data, offset + 1)))
            offset += MOVE_ENTRY_SIZE
        count = data[offset]
        offset += 1
        selfs = {}
        for _ in range(count):
            selfs[data[offset]] = data[offset + 1:offset + SELF_ENTRY_SIZE]
            offset += SELF_ENTRY_SIZE
        events, offset = unpackAiEvents(data, offset + 1, data[offset])
        if offset != len(data):
            raise FrameError('AI_FRAME size')
        return sent, samples, selfs, events

    def _sendFrame(self, client, sent, writer):
        client.frameSent = sent
        dg = PyDatagram()
        dg.addUint16(SERVER_FRAME)
        dg.appendData(writer.getvalue())
        self._send(client, dg)

    def _bindSlot(self, dgi):
        """AI_SLOT: the AI created (or removed) a player for a client."""
        if dgi.getRemainingSize() != AI_SLOT_MSG.size:
            self.notify.warning('malformed AI_SLOT')
            return
        slot, clientId, doId = AI_SLOT_MSG.unpack(dgi.getRemainingBytes())
        if slot == NO_SLOT:
            return
        old = self.slotOwners.pop(slot, None)
        if old is not None and old.slot == slot:
            old.slot = None
        # A new player (or nobody) in this slot: its next sample goes in full.
        self.lastStates.pop(slot, None)
        for client in self.clientsByConnection.values():
            client.knownSlots.discard(slot)
        if doId == 0:
            self.slotDoIds.pop(slot, None)
            self._sendSlots([c for c in self.clientsByConnection.values()
                             if c.accepted and c is not self.aiClient], [(slot, 0)])
            return
        client = self.clientsByDoIdBase.get(clientId)
        self.slotDoIds[slot] = doId
        if client is not None and client.accepted and client.role == ROLE_PLAYER:
            client.slot = slot
            self.slotOwners[slot] = client
        # The AI creates players in the arena zone; whoever listens there
        # learns the slot before the player's generate arrives.
        self._sendSlots([c for c in self.zonesToClients.get(ZONE_ARENA, ()) if c is not self.aiClient],
                        [(slot, doId)])

    def _sendSlots(self, recipients, pairs):
        dg = PyDatagram()
        dg.addUint16(SERVER_SLOTS)
        dg.appendData(b''.join(SLOT_ENTRY.pack(slot, doId) for slot, doId in pairs))
        for client in recipients:
            self._send(client, dg)

    def _sendZoneMovement(self, client, zoneIds):
        """A client started listening to zones: tell it the slots and newest
        samples of the players there before their generates arrive."""
        if client is self.aiClient:
            return
        pairs, bySent = [], {}
        for slot, doId in self.slotDoIds.items():
            obj = self._playerObject(slot)
            if obj is None or obj.zoneId not in zoneIds:
                continue
            pairs.append((slot, doId))
            if slot != client.slot and slot in self.lastStates:
                sent, base = self.lastStates[slot]
                bySent.setdefault(sent, []).append((slot, base))
        if pairs:
            self._sendSlots([client], pairs)
        # In full, each with the step it was sent at (collisions go by it).
        for sent, entries in bySent.items():
            w = BitWriter()
            writeHeader(w, sent, client.frameSent)
            for slot, base in entries:
                w.put(1, 1)
                writeSample(w, slot, base[:8], sent, None)
                client.knownSlots.add(slot)
            w.put(0, 3)  # no more samples, no events, no SELF
            self._sendFrame(client, sent, w)

    # ------------------------------------------------------------ sending

    def _send(self, client, datagram):
        self.cw.send(datagram, client.connection)
        self.needsFlush.add(client)
        self.stats['msgsOut'] += 1
        client.bytesOut += datagram.getLength() + 2

    def sendToZoneExcept(self, zoneId, datagram, exceptionList):
        for client in self.zonesToClients.get(zoneId, ()):
            if client not in exceptionList:
                self._send(client, datagram)
