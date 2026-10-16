"""Server hardening tests: a raw TCP client sends good, bad and hostile
messages and checks what the server answers, and that it stays up.

    cd server && python -m unittest discover -s tests -v
"""

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

from arena.protocol import (  # noqa: E402
    CLIENT_HELLO_ARENA, CLIENT_MOVE, FLAG_DASH, MOVE_ENTRY_SIZE, PROTOCOL_VERSION, REJECT_BAD_DC_HASH,
    REJECT_BAD_TOKEN, REJECT_BAD_VERSION, REJECT_FLOOD, REJECT_PROTOCOL_ERROR, REJECT_TIMEOUT,
    ROLE_AI, ROLE_PLAYER, SERVER_MOVES, SERVER_REJECT_ARENA, SERVER_SLOTS, ZONE_ARENA, ZONE_GLOBAL,
    VIEW_DELAY_SHIFT, packState)

SET_DOID_RANGE_CMU = 9001
CLIENT_OBJECT_GENERATE_CMU = 9002
OBJECT_GENERATE_CMU = 9003
OBJECT_UPDATE_FIELD_CMU = 9004
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


class RawClient:
    """Speaks Panda's TCP framing: uint16 length + datagram."""

    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b''

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
                return struct.unpack('<I', dg[12:16])[0]
        return None

    def move(self, state):
        self.send(u16(CLIENT_MOVE) + state)

    def slots(self, timeout=2.0):
        """slot -> doId from the next SERVER_SLOTS message."""
        dg = self.recvType(SERVER_SLOTS, timeout)
        if dg is None:
            return None
        return {dg[i]: struct.unpack('<I', dg[i + 1:i + 5])[0] for i in range(2, len(dg), 5)}

    def moves(self, timeout=2.0):
        """[(slot, state bytes)] from the next SERVER_MOVES message."""
        dg = self.recvType(SERVER_MOVES, timeout)
        if dg is None:
            return None
        n, size = dg[2], MOVE_ENTRY_SIZE
        assert len(dg) == 3 + n * size, 'bad SERVER_MOVES size'
        return [(dg[3 + i * size], dg[4 + i * size:3 + (i + 1) * size]) for i in range(n)]

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

    # ---------------------------------------------------------- objects

    def test_player_can_join_and_sees_the_game(self):
        c = self.client()
        base = c.join()
        self.assertIsNotNone(c.enterArena())
        c.generatePlayer(base)
        slots = c.slots()
        self.assertIsNotNone(slots, 'no slot for our player')
        self.assertIn(base, slots.values())
        c.move(packState(1.0, 2.0, 3.0, -4.0, 0, 123))
        self.assertIsNone(c.rejectCode(timeout=0.5))

    def test_moves_are_relayed_compactly_to_others_only(self):
        a, b = self.client(), self.client()
        abase = a.join()
        b.join()
        a.enterArena()
        b.enterArena()
        a.generatePlayer(abase, name='A')
        aslot = {v: k for k, v in a.slots().items()}[abase]
        flags = FLAG_DASH | 30 << VIEW_DELAY_SHIFT  # dash, drawing others 120 ms back
        state = packState(-3.25, 7.5, 8.75, -2.0, flags, 4321, 0.6, -0.8)  # input held
        flags = state[6]
        a.move(state)
        got = b.moves()
        self.assertIsNotNone(got, 'B saw no movement')
        self.assertIn((aslot, state), got)
        self.assertIsNone(a.moves(timeout=0.3), 'A got its own move back')
        # A newcomer gets everyone's slot and last state at once (dash cleared).
        c = self.client()
        c.join()
        c.send(u16(CLIENT_SET_INTEREST_CMU) + u32(ZONE_GLOBAL) + u32(ZONE_ARENA))
        self.assertEqual(c.slots().get(aslot), abase)
        last = dict(c.moves())
        self.assertEqual(last[aslot], state[:6] + bytes((flags & ~FLAG_DASH,)) + state[7:])

    def test_bad_moves_are_rejected(self):
        for bad in (packState(0, 0, 0, 0, 0, 0)[:-1],          # truncated
                    packState(0, 0, 0, 0, 0, 0) + b'\x00'):    # padded
            c = self.client()
            base = c.join()
            c.enterArena()
            c.generatePlayer(base)
            c.move(bad)
            self.assertEqual(c.rejectCode(), REJECT_PROTOCOL_ERROR)

    def test_moves_without_a_player_go_nowhere(self):
        watcher = self.client()
        watcher.join()
        watcher.enterArena()
        c = self.client()
        c.join()
        c.enterArena()
        c.move(packState(1, 1, 0, 0, 0, 0))  # no DistributedArenaPlayer: ignored
        self.assertIsNone(watcher.moves(timeout=0.5))
        self.assertIsNone(c.rejectCode(timeout=0.3))

    def test_creating_ai_classes_is_rejected(self):
        c = self.client()
        base = c.join()
        payload = b''.join(packField(GAME.getFieldByName(n), args) for n, args in [
            ('setRules', [1, 1, 1, 1, 1, 1, 1]), ('setArena', [10, []]), ('setRound', [0, 1, 0]),
            ('setOrbs', [[]]), ('setScores', [[]])]) + u16(0)
        c.send(u16(CLIENT_OBJECT_GENERATE_CMU) + u32(ZONE_ARENA) + u16(GAME.getNumber()) +
               u32(base) + payload)
        self.assertEqual(c.rejectCode(), REJECT_PROTOCOL_ERROR)

    def test_doid_outside_range_is_rejected(self):
        c = self.client()
        base = c.join()
        c.generatePlayer(base - 1)
        self.assertEqual(c.rejectCode(), REJECT_PROTOCOL_ERROR)

    def test_second_avatar_is_rejected(self):
        c = self.client()
        base = c.join()
        c.generatePlayer(base)
        c.generatePlayer(base + 1)
        self.assertEqual(c.rejectCode(), REJECT_PROTOCOL_ERROR)

    def test_overlong_name_is_rejected(self):
        c = self.client()
        base = c.join()
        bad = u16(17) + b'x' * 17  # string(1-16)
        payload = bad + packField(PLAYER.getFieldByName('setColorIndex'), [1]) + u16(0)
        c.send(u16(CLIENT_OBJECT_GENERATE_CMU) + u32(ZONE_ARENA) + u16(PLAYER.getNumber()) +
               u32(base) + payload)
        self.assertEqual(c.rejectCode(), REJECT_PROTOCOL_ERROR)

    def test_truncated_update_is_rejected(self):
        c = self.client()
        c.join()
        game = c.enterArena()
        ping = GAME.getFieldByName('ping')
        c.update(game, ping, packField(ping, [7])[:-1])
        self.assertEqual(c.rejectCode(), REJECT_PROTOCOL_ERROR)

    def test_renaming_after_generate_is_rejected(self):
        c = self.client()
        base = c.join()
        c.generatePlayer(base)
        c.update(base, PLAYER.getFieldByName('setName'), packField(PLAYER.getFieldByName('setName'), ['Evil']))
        self.assertEqual(c.rejectCode(), REJECT_PROTOCOL_ERROR)

    def test_updating_someone_else_is_rejected(self):
        victim = self.client()
        vbase = victim.join()
        victim.generatePlayer(vbase)
        attacker = self.client()
        attacker.join()
        time.sleep(0.2)
        field = PLAYER.getFieldByName('setColorIndex')
        attacker.update(vbase, field, packField(field, [3]))
        self.assertEqual(attacker.rejectCode(), REJECT_PROTOCOL_ERROR)
        self.assertIsNone(victim.rejectCode(timeout=0.3))

    def test_targeted_update_from_player_is_rejected(self):
        c = self.client()
        base = c.join()
        c.generatePlayer(base)
        field = PLAYER.getFieldByName('setColorIndex')
        c.send(u16(CLIENT_OBJECT_UPDATE_FIELD_TARGETED_CMU) + u32(1) + u32(base) +
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
