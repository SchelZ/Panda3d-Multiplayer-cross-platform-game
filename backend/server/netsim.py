#!/usr/bin/env python3
"""TCP proxy that simulates a wide-area network between clients and the server.

    python netsim.py --listen 4401 --server 127.0.0.1:4400 --rtt 120 --jitter 20

Clients connect to --listen instead of the server. Every chunk of data is held
back for half the round trip plus a random jitter in each direction. Order is
preserved, as on a real TCP connection, so jitter shows up the way it does in
practice: messages arrive late and in bunches. --kbps limits the bandwidth per
direction and connection, and --overhead adds the IP and TCP headers each
packet costs on a real link (about 52 bytes) to every chunk.

Used by tests/e2e_cpp_clients.py --netsim to test the game under latency.
"""

import argparse
import asyncio
import random
import signal
import socket
import sys
import time


class Link:
    def __init__(self, delayMs, jitterMs, kbps, rng, overhead=0):
        self.delay = delayMs / 1000.0
        self.jitter = jitterMs / 1000.0
        self.bytesPerSec = kbps * 1000.0 / 8.0 if kbps > 0 else 0.0
        self.overhead = overhead
        self.rng = rng

    async def pump(self, reader, writer):
        """Copies reader to writer, delaying each chunk."""
        queue = asyncio.Queue()
        sender = asyncio.ensure_future(self._send(queue, writer))
        lastDue = 0.0
        try:
            while True:
                data = await reader.read(65536)
                if not data:
                    break
                # Jitter is one-sided (networks add delay, never remove it),
                # and a chunk never overtakes the one before it.
                due = time.monotonic() + self.delay + self.rng.uniform(0.0, self.jitter)
                lastDue = max(lastDue, due)
                queue.put_nowait((lastDue, data))
        except (ConnectionError, OSError):
            pass
        queue.put_nowait((None, None))
        await sender

    async def _send(self, queue, writer):
        wireFree = 0.0
        try:
            while True:
                due, data = await queue.get()
                if data is None:
                    break
                if self.bytesPerSec:
                    # Serialisation delay of a link with this bandwidth.
                    wireFree = max(wireFree, time.monotonic()) + (len(data) + self.overhead) / self.bytesPerSec
                    due = max(due, wireFree)
                wait = due - time.monotonic()
                if wait > 0:
                    await asyncio.sleep(wait)
                writer.write(data)
                await writer.drain()
        except (ConnectionError, OSError):
            pass
        finally:
            writer.close()


async def serve(args):
    host, _, port = args.server.rpartition(':')
    rng = random.Random(args.seed)
    up = Link(args.rtt / 2.0, args.jitter, args.kbps, rng, args.overhead)
    down = Link(args.rtt / 2.0, args.jitter, args.kbps, rng, args.overhead)

    async def handle(clientReader, clientWriter):
        try:
            serverReader, serverWriter = await asyncio.open_connection(host or '127.0.0.1', int(port))
        except OSError:
            clientWriter.close()
            return
        for w in (clientWriter, serverWriter):
            sock = w.get_extra_info('socket')
            if sock is not None:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        await asyncio.gather(up.pump(clientReader, serverWriter),
                             down.pump(serverReader, clientWriter))

    server = await asyncio.start_server(handle, '127.0.0.1', args.listen)
    print('netsim ready: 127.0.0.1:%d -> %s, rtt %d ms, jitter 0-%d ms per direction%s'
          % (args.listen, args.server, args.rtt, args.jitter,
             ', %d kbit/s (+%d bytes per packet)' % (args.kbps, args.overhead) if args.kbps else ''),
          flush=True)
    stop = asyncio.get_running_loop().create_future()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            asyncio.get_running_loop().add_signal_handler(sig, stop.set_result, None)
        except (NotImplementedError, RuntimeError):
            pass  # Windows: Ctrl+C still ends the process
    async with server:
        await stop


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--listen', type=int, required=True, help='port clients connect to')
    p.add_argument('--server', default='127.0.0.1:4400', help='HOST:PORT of the real server')
    p.add_argument('--rtt', type=float, default=100.0, help='round trip time in ms')
    p.add_argument('--jitter', type=float, default=0.0, help='extra random delay, 0..N ms per direction')
    p.add_argument('--kbps', type=float, default=0.0, help='bandwidth limit per direction (kbit/s)')
    p.add_argument('--overhead', type=int, default=0,
                   help='bytes of headers each chunk (about one TCP packet) costs on the link')
    p.add_argument('--seed', type=int, default=None)
    args = p.parse_args()
    try:
        asyncio.run(serve(args))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
