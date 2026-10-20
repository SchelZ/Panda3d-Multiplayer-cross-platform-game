"""LAN discovery (arena/discovery.py): probes get answers, nothing else does,
and a reply is never larger than its probe."""

import os
import socket
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from arena import discovery  # noqa: E402


def freeUdpPort():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


class DiscoveryTest(unittest.TestCase):
    def setUp(self):
        self.port = freeUdpPort()
        self.players = 3
        self.beacon = discovery.LanBeacon(self.port, 'Meadow server', 16, lambda: self.players,
                                          bindAddress='127.0.0.1')
        self.client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.client.settimeout(0.5)

    def tearDown(self):
        self.client.close()
        self.beacon.close()

    def exchange(self, packet, polls=5):
        self.client.sendto(packet, ('127.0.0.1', self.port))
        for _ in range(polls):
            time.sleep(0.01)
            self.beacon.poll()
        try:
            return self.client.recvfrom(256)[0]
        except socket.timeout:
            return None

    def test_probe_gets_reply(self):
        reply = self.exchange(discovery.makeProbe(0xdeadbeef))
        self.assertIsNotNone(reply)
        self.assertLessEqual(len(reply), discovery.PROBE_SIZE)
        nonce, port, players, maxPlayers, name = discovery.parseReply(reply)
        self.assertEqual(nonce, 0xdeadbeef)
        self.assertEqual(port, self.port)
        self.assertEqual((players, maxPlayers), (3, 16))
        self.assertEqual(name, 'Meadow server')

    def test_garbage_is_ignored(self):
        self.assertIsNone(self.exchange(b'PARENA?'))  # too short: no amplification
        bad = bytearray(discovery.makeProbe(1))
        bad[0:7] = b'PARENA!'
        self.assertIsNone(self.exchange(bytes(bad)))
        old = bytearray(discovery.makeProbe(1))
        old[7] = discovery.VERSION + 1
        self.assertIsNone(self.exchange(bytes(old)))

    def test_long_names_fit(self):
        self.beacon.name = 'Ünïcödé ' * 20
        reply = self.exchange(discovery.makeProbe(7))
        self.assertLessEqual(len(reply), discovery.PROBE_SIZE)
        name = discovery.parseReply(reply)[4]
        self.assertTrue(name.startswith('Ünïcödé'))
        self.assertNotIn('�', name)  # not cut inside a character

    def test_replies_are_rate_limited(self):
        probe = discovery.makeProbe(9)
        for _ in range(200):
            self.client.sendto(probe, ('127.0.0.1', self.port))
        time.sleep(0.05)
        answered = sum(self.beacon.poll(maxPackets=1000) for _ in range(3))
        self.assertLessEqual(answered, discovery.REPLIES_PER_SECOND + 5)
        self.assertGreater(answered, 0)


if __name__ == '__main__':
    unittest.main()
