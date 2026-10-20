"""LAN discovery: lets clients on the same network find this server without
typing its address (the Android client's connect screen lists what it finds).

A client broadcasts a probe to UDP port 4400 (the server's default TCP port;
a server listens for probes on the UDP port with its own TCP port number) and
every server that hears it answers the sender directly:

  PROBE  client -> broadcast, exactly 64 bytes:
      b'PARENA?', uint8 version, uint32 nonce, zero padding
  REPLY  server -> client, at most 64 bytes:
      b'PARENA!', uint8 version, uint32 nonce (echoed), uint16 tcpPort,
      uint8 players, uint8 maxPlayers, uint8 nameLength, name (UTF-8)

A reply is never larger than the probe and replies are rate-limited, so the
port cannot amplify a spoofed flood. Polled from the server loop; it never
blocks.
"""

import errno
import socket
import struct
import time

MAGIC_PROBE = b'PARENA?'
MAGIC_REPLY = b'PARENA!'
VERSION = 1
PROBE_SIZE = 64
HEADER = struct.Struct('<7sBI')         # magic, version, nonce
REPLY_INFO = struct.Struct('<HBBB')     # tcpPort, players, maxPlayers, nameLength
MAX_NAME = PROBE_SIZE - HEADER.size - REPLY_INFO.size   # 45 bytes
REPLIES_PER_SECOND = 50


def makeProbe(nonce):
    return HEADER.pack(MAGIC_PROBE, VERSION, nonce & 0xffffffff).ljust(PROBE_SIZE, b'\0')


def makeReply(nonce, port, players, maxPlayers, name):
    raw = name.encode('utf-8')[:MAX_NAME]
    while raw:
        try:
            raw.decode('utf-8')  # do not cut a character in half
            break
        except UnicodeDecodeError:
            raw = raw[:-1]
    return (HEADER.pack(MAGIC_REPLY, VERSION, nonce & 0xffffffff) +
            REPLY_INFO.pack(port, min(players, 255), min(maxPlayers, 255), len(raw)) + raw)


def parseReply(data):
    """Returns (nonce, port, players, maxPlayers, name) or None."""
    if len(data) < HEADER.size + REPLY_INFO.size:
        return None
    magic, version, nonce = HEADER.unpack_from(data)
    if magic != MAGIC_REPLY or version != VERSION:
        return None
    port, players, maxPlayers, nameLength = REPLY_INFO.unpack_from(data, HEADER.size)
    start = HEADER.size + REPLY_INFO.size
    name = data[start:start + nameLength].decode('utf-8', 'replace')
    return nonce, port, players, maxPlayers, name


class LanBeacon:
    def __init__(self, port, name, maxPlayers, numPlayers, bindAddress='0.0.0.0'):
        """numPlayers: a function returning the current player count."""
        self.port = port
        self.name = name
        self.maxPlayers = maxPlayers
        self.numPlayers = numPlayers
        self.replies = 0
        self._budget = REPLIES_PER_SECOND
        self._budgetTime = time.monotonic()
        family = socket.AF_INET6 if ':' in bindAddress else socket.AF_INET
        self.sock = socket.socket(family, socket.SOCK_DGRAM)
        try:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.sock.bind((bindAddress, port))
            self.sock.setblocking(False)
        except OSError:
            self.sock.close()
            raise

    def close(self):
        self.sock.close()

    def poll(self, maxPackets=32):
        """Answers the probes that arrived; returns how many it answered."""
        answered = 0
        for _ in range(maxPackets):
            try:
                data, sender = self.sock.recvfrom(256)
            except OSError as e:
                if e.errno in (errno.EAGAIN, errno.EWOULDBLOCK) or isinstance(e, BlockingIOError):
                    break
                # ICMP port unreachable from an earlier reply (Windows) and the
                # like: ignore and keep reading.
                continue
            if len(data) != PROBE_SIZE:
                continue
            magic, version, nonce = HEADER.unpack_from(data)
            if magic != MAGIC_PROBE or version != VERSION:
                continue
            if not self._spend():
                continue
            reply = makeReply(nonce, self.port, self.numPlayers(), self.maxPlayers, self.name)
            try:
                self.sock.sendto(reply, sender)
                answered += 1
                self.replies += 1
            except OSError:
                pass
        return answered

    def _spend(self):
        now = time.monotonic()
        self._budget = min(REPLIES_PER_SECOND,
                           self._budget + (now - self._budgetTime) * REPLIES_PER_SECOND)
        self._budgetTime = now
        if self._budget < 1.0:
            return False
        self._budget -= 1.0
        return True
