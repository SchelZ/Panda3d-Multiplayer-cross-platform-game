"""Server hardening tests: a raw TCP client sends good, bad and hostile
messages and checks what the server answers, and that it stays up.

    cd server && python -m unittest discover -s tests -v
"""

import math
import os
import random
import socket
import struct
import subprocess
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER_DIR = os.path.dirname(HERE)
COMMON_DIR = os.path.join(os.path.dirname(SERVER_DIR), 'common')
sys.path.insert(0, SERVER_DIR)

from panda3d.core import Filename  # noqa: E402
from panda3d.direct import DCFile, DCPacker  # noqa: E402

from arena.frame import FrameError, decodeFrame  # noqa: E402
from arena.protocol import (  # noqa: E402
    AI_FRAME, AI_SLOT, CLIENT_HELLO_ARENA, CLIENT_INPUT, INPUT_DASH, INPUT_HELD, PROTOCOL_VERSION,
    REJECT_BAD_DC_HASH, REJECT_BAD_TOKEN, REJECT_BAD_VERSION, REJECT_FLOOD, REJECT_PROTOCOL_ERROR,
    REJECT_TIMEOUT, ROLE_AI, ROLE_PLAYER, SELF_SIZE, SELF_SNAP, SERVER_FRAME, SERVER_INPUTS,
    SERVER_REJECT_ARENA, SERVER_SLOTS, STATE, STATE_SIZE, ZONE_ARENA, ZONE_GLOBAL, packInput,
    packState, resolveStep, unpackSelf, unpackState)
from arena.rules import Rules  # noqa: E402
from arena.simulation import PlayerSim  # noqa: E402

SET_DOID_RANGE_CMU = 9001
CLIENT_OBJECT_GENERATE_CMU = 9002
OBJECT_GENERATE_CMU = 9003
OBJECT_UPDATE_FIELD_CMU = 9004
REQUEST_GENERATES_CMU = 9007
CLIENT_SET_INTEREST_CMU = 9009
CLIENT_OBJECT_UPDATE_FIELD_TARGETED_CMU = 9011
CLIENT_OBJECT_UPDATE_FIELD = 120


def loadDc():
    dc = DCFile()
    for name in ('direct.dc', 'arena.dc'):
        assert dc.read(Filename.fromOsSpecific(os.path.join(COMMON_DIR, name)))
    return dc


DC = loadDc()
PLAYER = DC.getClassByName('DistributedArenaPlayer')
GAME = DC.getClassByName('DistributedArenaGame')


def packField(field, args):
    p = DCPacker()
    p.beginPack(field)
    p.packObject(tuple(args))
    assert p.endPack(), field.getName()
    return p.getBytes()


def u16(v):
    return struct.pack('<H', v)


def u32(v):
    return struct.pack('<I', v)


def pstr(s):
    b = s.encode()
    return u16(len(b)) + b


def _datagrams(stream):
    """The datagrams in a recorded TCP stream (uint16 length + datagram)."""
    out, i = [], 0
    while i + 2 <= len(stream):
        n = struct.unpack('<H', stream[i:i + 2])[0]
        out.append(bytes(stream[i + 2:i + 2 + n]))
        i += 2 + n
    return out


class RawClient:
    """Speaks Panda's TCP framing: uint16 length + datagram. Keeps every byte
    it received in `received`, and every SERVER_FRAME, decoded in order as a
    client must, in `frames`: (sent, [(slot, STATE bytes)], events, SELF)."""

    def __init__(self, port, source=None):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=5,
                                             source_address=(source, 0) if source else None)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b''
        self.received = bytearray()
        self.game = None
        self.frames = []
        self.frameSent = None
        self.baselines = [None] * 256

    def send(self, payload):
        self.sock.sendall(u16(len(payload)) + payload)

    def recv(self, timeout=3.0):
        """Next datagram, or None if the connection closed/timed out."""
        deadline = time.time() + timeout
        while True:
            if len(self.buf) >= 2:
                n = struct.unpack('<H', self.buf[:2])[0]
                if len(self.buf) >= 2 + n:
                    dg, self.buf = self.buf[2:2 + n], self.buf[2 + n:]
                    self._track(dg)
                    return dg
            left = deadline - time.time()
            if left <= 0:
                return None
            self.sock.settimeout(left)
            try:
                chunk = self.sock.recv(65536)
            except (socket.timeout, ConnectionError):
                return None
            if not chunk:
                return None
            self.buf += chunk
            self.received += chunk

    def _track(self, dg):
        kind = struct.unpack('<H', dg[:2])[0]
        if kind == SERVER_FRAME:
            try:
                self.frameSent, samples, events, own = decodeFrame(dg[2:], self.frameSent, self.baselines)
            except FrameError as e:
                raise AssertionError('undecodable SERVER_FRAME: %s' % e)
            self.frames.append((self.frameSent, [(slot, STATE.pack(*st)) for slot, st in samples],
                                events, own))
        elif kind == SERVER_SLOTS:
            for i in range(2, len(dg), 5):
                self.baselines[dg[i]] = None

    def recvType(self, msgType, timeout=3.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            dg = self.recv(deadline - time.time())
            if dg is None:
                return None
            if struct.unpack('<H', dg[:2])[0] == msgType:
                return dg
        return None

    def hello(self, version=PROTOCOL_VERSION, dcHash=None, role=ROLE_PLAYER, token=''):
        self.send(u16(CLIENT_HELLO_ARENA) + u32(version) +
                  u32(DC.getHash() if dcHash is None else dcHash) +
                  bytes([role]) + pstr(token) + pstr('test-client'))

    def join(self):
        """Hello and wait for the doId range; returns doIdBase."""
        self.hello()
        dg = self.recvType(SET_DOID_RANGE_CMU)
        assert dg is not None, 'no doId range'
        base, rng = struct.unpack('<II', dg[2:10])
        self.base = base
        return base

    def rejectCode(self, timeout=3.0):
        dg = self.recvType(SERVER_REJECT_ARENA, timeout)
        if dg is None:
            return None
        return struct.unpack('<H', dg[2:4])[0]

    def generatePlayer(self, doId, name='Tester', color=1, extra=b'\x00\x00'):
        payload = packField(PLAYER.getFieldByName('setName'), [name]) + \
            packField(PLAYER.getFieldByName('setColorIndex'), [color]) + extra
        self.send(u16(CLIENT_OBJECT_GENERATE_CMU) + u32(ZONE_ARENA) + u16(PLAYER.getNumber()) +
                  u32(doId) + payload)

    def update(self, doId, field, payload):
        self.send(u16(CLIENT_OBJECT_UPDATE_FIELD) + u32(doId) + u16(field.getNumber()) + payload)

    def enterArena(self):
        """Interest in the arena; returns the game object's doId."""
        self.send(u16(CLIENT_SET_INTEREST_CMU) + u32(ZONE_GLOBAL) + u32(ZONE_ARENA))
        deadline = time.time() + 3
        while time.time() < deadline:
            dg = self.recvType(OBJECT_GENERATE_CMU, deadline - time.time())
            if dg is None:
                break
            if struct.unpack('<H', dg[10:12])[0] == GAME.getNumber():
                self.game = struct.unpack('<I', dg[12:16])[0]
                return self.game
        return None

    def requestJoin(self, name='Tester', color=1):
        field = GAME.getFieldByName('requestJoin')
        self.update(self.game, field, packField(field, [name, color]))

    def play(self, name='Tester', color=1):
        """Joins, enters the arena and asks the AI for a player. Returns
        (slot, playerDoId, SELF) from the AI's replies."""
        self.join()
        self.enterArena()
        self.requestJoin(name, color)
        joined = GAME.getFieldByName('joined')
        slot = doId = msg = None
        deadline = time.time() + 3
        while time.time() < deadline and (slot is None or msg is None):
            dg = self.recv(deadline - time.time())
            if dg is None:
                break
            kind = struct.unpack('<H', dg[:2])[0]
            if kind == OBJECT_UPDATE_FIELD_CMU and struct.unpack('<H', dg[10:12])[0] == joined.getNumber():
                slot, doId = dg[12], struct.unpack('<I', dg[13:17])[0]
            elif kind == SERVER_FRAME and self.frames[-1][3] is not None:
                msg = unpackSelf(self.frames[-1][3])
        assert slot is not None and msg is not None, 'the AI did not let us join'
        self.slot, self.playerDoId = slot, doId
        # The spawn's step is the AI's simulation of us, LAG_START behind its clock.
        self.spawnStep, self.spawnTime = resolveStep(msg[1], 0) + PlayerSim.LAG_START, time.time()
        return slot, doId, msg

    def serverStep(self):
        """About the AI's current step (its clock), from our spawn."""
        return self.spawnStep + int((time.time() - self.spawnTime) * 120)

    def input(self, seq, step, flags, direction=0):
        self.send(u16(CLIENT_INPUT) + packInput(seq, step, flags, direction))

    def slots(self, timeout=2.0):
        """slot -> doId from the next SERVER_SLOTS message."""
        dg = self.recvType(SERVER_SLOTS, timeout)
        if dg is None:
            return None
        return {dg[i]: struct.unpack('<I', dg[i + 1:i + 5])[0] for i in range(2, len(dg), 5)}

    def moves(self, timeout=2.0):
        """[(slot, STATE bytes)] from the next SERVER_FRAME with samples."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.recvType(SERVER_FRAME, deadline - time.time()) is None:
                return None
            if self.frames[-1][1]:
                return self.frames[-1][1]
        return None

    def collect(self, seconds):
        """Every datagram for `seconds`, as (type, bytes)."""
        out = []
        deadline = time.time() + seconds
        while time.time() < deadline:
            dg = self.recv(deadline - time.time())
            if dg is None:
                break
            out.append((struct.unpack('<H', dg[:2])[0], dg))
        return out

    def close(self):
        self.sock.close()


class ServerHardeningTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0))
            cls.port = s.getsockname()[1]
        cls.server = subprocess.Popen(
            [sys.executable, os.path.join(SERVER_DIR, 'arena_server.py'), '--port', str(cls.port),
             '--status-interval', '0'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        deadline = time.time() + 20
        while time.time() < deadline:
            line = cls.server.stdout.readline()
            if 'server ready' in line:
                break
        else:
            cls.server.kill()
            raise RuntimeError('server did not start')

    @classmethod
    def tearDownClass(cls):
        cls.server.terminate()
        out, _ = cls.server.communicate(timeout=10)
        cls.serverLog = out

    def setUp(self):
        self.clients = []

    def tearDown(self):
        for c in self.clients:
            c.close()
        self.assertIsNone(self.server.poll(), 'server died')

    def client(self):
        c = RawClient(self.port)
        self.clients.append(c)
        return c

    # ---------------------------------------------------------- handshake

    def test_valid_hello_gets_doid_range(self):
        c = self.client()
        base = c.join()
        self.assertGreater(base, 1)

    def test_wrong_dc_hash_is_rejected(self):
        c = self.client()
        c.hello(dcHash=DC.getHash() ^ 1)
        self.assertEqual(c.rejectCode(), REJECT_BAD_DC_HASH)

    def test_wrong_version_is_rejected(self):
        c = self.client()
        c.hello(version=PROTOCOL_VERSION + 1)
        self.assertEqual(c.rejectCode(), REJECT_BAD_VERSION)

    def test_fake_ai_is_rejected(self):
        c = self.client()
        c.hello(role=ROLE_AI, token='guess')
        self.assertEqual(c.rejectCode(), REJECT_BAD_TOKEN)

    def test_message_before_hello_is_rejected(self):
        c = self.client()
        c.send(u16(CLIENT_SET_INTEREST_CMU) + u32(ZONE_ARENA))
        self.assertEqual(c.rejectCode(), REJECT_PROTOCOL_ERROR)

    def test_silent_connection_times_out(self):
        c = self.client()
        self.assertEqual(c.rejectCode(timeout=10), REJECT_TIMEOUT)

    def test_silent_player_is_dropped_after_15_seconds(self):
        # A phone frozen in the background, or gone: its player stays 15 s
        # without a message (clients ping every 2 s), then the server drops it.
        c = self.client()
        c.join()
        start = time.time()
        self.assertEqual(c.rejectCode(timeout=25), REJECT_TIMEOUT)
        self.assertGreaterEqual(time.time() - start, 14.5)

    # ---------------------------------------------------------- objects

    def test_the_ai_creates_the_player_where_it_decides(self):
        watcher = self.client()
        watcher.join()
        watcher.enterArena()
        c = self.client()
        slot, doId, msg = c.play(name='Te\x07ster', color=4)
        self.assertTrue(msg[10] & SELF_SNAP, 'the spawn is not marked as a placement')
        x, y, heading = msg[2], msg[3], msg[6]
        self.assertLess(math.hypot(x, y), Rules().arenaRadius)
        # Facing the centre, as the AI decided: everybody gets the same heading.
        self.assertAlmostEqual(math.remainder(heading - math.degrees(math.atan2(x, -y)), 360.0), 0.0,
                               delta=0.1)
        seen = None
        deadline = time.time() + 2
        while time.time() < deadline and seen is None:
            for s_, state in watcher.moves(deadline - time.time()) or []:
                if s_ == slot:
                    seen = unpackState(state)
        self.assertIsNotNone(seen, 'others never saw the new player')
        self.assertAlmostEqual(seen[0], x, delta=0.01)
        self.assertAlmostEqual(seen[1], y, delta=0.01)
        self.assertAlmostEqual(math.remainder(seen[8] - heading, 360.0), 0.0, delta=0.8)
        # The player object is the AI's (its doId is in the AI's range), with
        # the name the AI accepted (control characters dropped).
        self.assertLess(doId, 1000000)
        generate = next(dg for dg in _datagrams(watcher.received)
                        if struct.unpack('<H', dg[:2])[0] == OBJECT_GENERATE_CMU and
                        struct.unpack('<I', dg[12:16])[0] == doId)
        p = DCPacker()
        p.setUnpackData(generate[16:])
        p.beginUnpack(PLAYER.getFieldByName('setName'))
        self.assertEqual(p.unpackObject(), ('Tester',))
        # A second request does not make a second player.
        c.requestJoin(name='Again')
        joined = GAME.getFieldByName('joined')
        again = [dg for kind, dg in c.collect(0.5) if kind == OBJECT_UPDATE_FIELD_CMU and
                 struct.unpack('<H', dg[10:12])[0] == joined.getNumber()]
        self.assertEqual(again, [])
        # A newcomer gets everyone's slot and newest state at once.
        late = self.client()
        late.join()
        late.send(u16(CLIENT_SET_INTEREST_CMU) + u32(ZONE_GLOBAL) + u32(ZONE_ARENA))
        self.assertEqual(late.slots().get(slot), doId)
        cached = unpackState(dict(late.moves())[slot])
        self.assertAlmostEqual(math.remainder(cached[8] - heading, 360.0), 0.0, delta=0.8)

    def test_a_newcomer_does_not_make_the_server_resend_everything(self):
        """Whoever already plays gets the newcomer's player and nothing else:
        not every object again (the stock server asks the owner to resend
        its objects to the whole zone whenever someone starts listening)."""
        a = self.client()
        a.play(name='A')
        a.collect(0.3)
        b = self.client()
        b.play(name='B')
        kinds = [(kind, dg) for kind, dg in a.collect(0.8)]
        generates = [struct.unpack('<I', dg[12:16])[0] for kind, dg in kinds if kind == OBJECT_GENERATE_CMU]
        self.assertEqual(generates, [b.playerDoId])
        self.assertNotIn(REQUEST_GENERATES_CMU, [kind for kind, _ in kinds])
        # The newcomer itself got everything: the game and A's player.
        got = {struct.unpack('<I', dg[12:16])[0] for dg in _datagrams(b.received)
               if struct.unpack('<H', dg[:2])[0] == OBJECT_GENERATE_CMU}
        self.assertIn(b.game, got)
        self.assertIn(a.playerDoId, got)

    def test_players_cannot_create_objects(self):
        c = self.client()
        base = c.join()
        c.enterArena()
        c.generatePlayer(base)  # the old way: the client made its own player
        self.assertEqual(c.rejectCode(), REJECT_PROTOCOL_ERROR)

    def test_inputs_move_the_player_for_everyone_else(self):
        a, b = self.client(), self.client()
        slot, _, msg = a.play(name='A')
        b.play(name='B')
        x0, y0 = msg[2], msg[3]
        a.input(1, a.serverStep(), INPUT_HELD, 0)  # run toward +x, now
        time.sleep(0.3)
        a.input(2, a.serverStep(), 0, 0)           # let go 0.3 s later
        far = None
        deadline = time.time() + 3
        while time.time() < deadline:
            for s_, state in b.moves(deadline - time.time()) or []:
                if s_ == slot:
                    st = unpackState(state)
                    far = st if far is None or st[0] > far[0] else far
        self.assertIsNotNone(far, 'B never saw A move')
        r = Rules()
        # Where the movement code puts it: 0.3 s of input, then braking.
        self.assertGreater(far[0] - x0, 1.5)
        self.assertLess(far[0] - x0, r.maxSpeed * 0.35 + r.maxSpeed ** 2 / (2 * r.accel) + 0.2)
        self.assertAlmostEqual(far[1], y0, delta=0.05)
        # A gets its own state as SELF, never as a sample.
        a.collect(0.5)
        self.assertEqual([s_ for f in a.frames for s_, _ in f[1] if s_ == slot], [])
        self.assertTrue(any(f[3] for f in a.frames))

    def test_inputs_go_to_the_ai_only(self):
        a, b = self.client(), self.client()
        a.play(name='A')
        b.play(name='B')
        step = a.serverStep() + 6
        for i in range(10):
            a.input(i + 1, step + 6 * i, INPUT_HELD if i % 2 == 0 else 0, i * 20)
        kinds = {kind for kind, _ in b.collect(1.0)}
        self.assertNotIn(CLIENT_INPUT, kinds)
        self.assertNotIn(SERVER_INPUTS, kinds)
        self.assertTrue(kinds <= {SERVER_FRAME, SERVER_SLOTS, OBJECT_UPDATE_FIELD_CMU,
                                  OBJECT_GENERATE_CMU}, kinds)

    def test_speed_and_teleport_hacks_get_nowhere(self):
        """A hacked client can only send inputs, and the AI moves its player
        by the rules: spamming inputs, dashes and timestamps from the past or
        the future never beats max speed and one dash per cooldown."""
        cheat, watcher = self.client(), self.client()
        slot, _, msg = cheat.play(name='Cheat')
        watcher.play(name='W')
        x0, y0 = msg[2], msg[3]
        step = resolveStep(msg[1], 0)
        start = time.time()
        for i in range(90):
            claimed = step + (-600 if i % 3 == 0 else 2000 if i % 3 == 1 else 12 + i)
            cheat.input(i + 1, claimed, INPUT_HELD | INPUT_DASH, (i * 37) & 0xff)
            time.sleep(0.01)
        positions = []
        deadline = time.time() + 1.0
        while time.time() < deadline:
            for s_, state in watcher.moves(deadline - time.time()) or []:
                if s_ == slot:
                    positions.append(unpackState(state)[:2])
        elapsed = time.time() - start
        self.assertTrue(positions, 'the cheater never moved')
        r = Rules()
        dashes = 1 + int(elapsed / r.dashCooldown)
        limit = r.maxSpeed * elapsed + dashes * r.dashSpeed * r.dashTime + 0.5
        self.assertLess(max(math.hypot(x - x0, y - y0) for x, y in positions), limit)
        # The old way of moving (a position) is not even a valid message any more.
        cheat.send(u16(CLIENT_INPUT) + packState(0, 0, 0, 0, 0, 0, 0))
        self.assertEqual(cheat.rejectCode(), REJECT_PROTOCOL_ERROR)

    def test_bad_inputs_are_rejected(self):
        for bad in (packInput(1, 0, 0, 0)[:-1],          # truncated
                    packInput(1, 0, 0, 0) + b'\x00'):    # padded
            c = self.client()
            c.play()
            c.send(u16(CLIENT_INPUT) + bad)
            self.assertEqual(c.rejectCode(), REJECT_PROTOCOL_ERROR)

    def test_inputs_without_a_player_go_nowhere(self):
        watcher = self.client()
        watcher.join()
        watcher.enterArena()
        c = self.client()
        c.join()
        c.enterArena()
        c.input(1, 0, INPUT_HELD, 0)  # no player asked for: ignored
        self.assertIsNone(watcher.moves(timeout=0.5))
        self.assertIsNone(c.rejectCode(timeout=0.3))

    def test_players_cannot_speak_for_the_ai(self):
        for kind, payload in ((AI_FRAME, bytes((0, 0, 1, 0)) + bytes(STATE_SIZE) + bytes((1, 0)) +
                               bytes(SELF_SIZE) + bytes((0,))),
                              (AI_SLOT, struct.pack('<BII', 0, 1, 1))):
            c = self.client()
            c.play()
            c.send(u16(kind) + payload)
            self.assertEqual(c.rejectCode(), REJECT_PROTOCOL_ERROR, kind)

    def test_no_client_sees_another_clients_address(self):
        """Clients only ever talk to the server, and nothing it relays carries
        an address: not in the movement, the slots, the objects or the chat."""
        try:
            a = RawClient(self.port, source='127.0.0.2')
            b = RawClient(self.port, source='127.0.0.3')
        except OSError:
            self.skipTest('this system cannot use 127.0.0.2/3 as source addresses')
        self.clients += [a, b]
        a.play(name='A')
        b.play(name='B')
        chat = GAME.getFieldByName('requestChat')
        for c in (a, b):
            step = c.serverStep() + 6
            c.input(1, step, INPUT_HELD | INPUT_DASH, 64)
            c.input(2, step + 30, 0)
            c.update(c.game, chat, packField(chat, ['hello']))
        for c in (a, b):
            c.collect(1.5)
        for me, other in ((a, b), (b, a)):
            host, port = other.sock.getsockname()
            for needle in (socket.inet_aton(host), socket.inet_aton(host)[::-1], host.encode(),
                           ('%s:%d' % (host, port)).encode(), struct.pack('<H', port) + b'\x00\x00'):
                self.assertNotIn(needle, bytes(me.received), 'found %r' % needle)

    def test_overlong_name_is_rejected(self):
        c = self.client()
        c.join()
        c.enterArena()
        field = GAME.getFieldByName('requestJoin')
        c.update(c.game, field, u16(17) + b'x' * 17 + b'\x01')  # string(1-16)
        self.assertEqual(c.rejectCode(), REJECT_PROTOCOL_ERROR)

    def test_truncated_update_is_rejected(self):
        c = self.client()
        c.join()
        game = c.enterArena()
        ping = GAME.getFieldByName('ping')
        c.update(game, ping, packField(ping, [7])[:-1])
        self.assertEqual(c.rejectCode(), REJECT_PROTOCOL_ERROR)

    def test_renaming_after_joining_is_rejected(self):
        c = self.client()
        _, doId, _ = c.play()
        c.update(doId, PLAYER.getFieldByName('setName'), packField(PLAYER.getFieldByName('setName'), ['Evil']))
        self.assertEqual(c.rejectCode(), REJECT_PROTOCOL_ERROR)

    def test_updating_someone_else_is_rejected(self):
        victim = self.client()
        _, vdoId, _ = victim.play()
        attacker = self.client()
        attacker.join()
        field = PLAYER.getFieldByName('setColorIndex')
        attacker.update(vdoId, field, packField(field, [3]))
        self.assertEqual(attacker.rejectCode(), REJECT_PROTOCOL_ERROR)
        self.assertIsNone(victim.rejectCode(timeout=0.3))

    def test_targeted_update_from_player_is_rejected(self):
        c = self.client()
        _, doId, _ = c.play()
        field = GAME.getFieldByName('pong')
        c.send(u16(CLIENT_OBJECT_UPDATE_FIELD_TARGETED_CMU) + u32(1) + u32(c.game) +
               u16(field.getNumber()) + packField(field, [0]))
        self.assertEqual(c.rejectCode(), REJECT_PROTOCOL_ERROR)

    def test_forbidden_interest_zone_is_rejected(self):
        c = self.client()
        c.join()
        c.send(u16(CLIENT_SET_INTEREST_CMU) + u32(77))
        self.assertEqual(c.rejectCode(), REJECT_PROTOCOL_ERROR)

    # ---------------------------------------------------------- abuse

    def test_flood_is_kicked(self):
        c = self.client()
        c.join()
        msg = u16(CLIENT_SET_INTEREST_CMU) + u32(ZONE_GLOBAL)
        try:
            for _ in range(2000):
                c.send(msg)
        except OSError:
            pass  # the server may already have closed us
        self.assertEqual(c.rejectCode(timeout=5), REJECT_FLOOD)

    def test_random_garbage_never_crashes_the_server(self):
        rng = random.Random(1234)
        for i in range(40):
            c = RawClient(self.port)
            try:
                if i % 2:
                    c.join()
                for _ in range(5):
                    c.send(bytes(rng.randrange(256) for _ in range(rng.randrange(0, 64))))
                c.recv(timeout=0.2)
            except OSError:
                pass
            finally:
                c.close()
        # Still serving.
        good = self.client()
        self.assertGreater(good.join(), 1)


if __name__ == '__main__':
    unittest.main()
