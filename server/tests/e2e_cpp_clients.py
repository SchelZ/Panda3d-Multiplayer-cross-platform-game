#!/usr/bin/env python3
"""End-to-end test: Python server + headless C++ clients + a Python bot.

    python tests/e2e_cpp_clients.py path/to/arena_client [--clients 3]
    python tests/e2e_cpp_clients.py build/bin/arena_client build-mingw/bin/arena_client.exe

Several client binaries are used round-robin, so a Linux build and a Windows
build (run through Wine when not on Windows) can be tested in the same game.
--netsim RTT[:JITTER] puts every client behind netsim.py (milliseconds), to
test under internet latency; the totals line then shows how the netcode copes.
Checks that every C++ client joins, sees the others, gets pongs from the AI,
receives everyone's chat, scores, exits 0, and that the server survives and
stops cleanly on SIGTERM.
"""

import argparse
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER_DIR = os.path.dirname(HERE)
WINDOWS = os.name == 'nt'


def freePort():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def clientCommand(path):
    if path.lower().endswith('.exe') and not WINDOWS:
        wine = shutil.which('wine') or shutil.which('wine64')
        if not wine:
            sys.exit('%s needs Wine to run on this system' % path)
        return [wine, path]
    return [path]


CHILDREN = []


def spawn(cmd, **kwargs):
    proc = subprocess.Popen(cmd, **kwargs)
    CHILDREN.append(proc)
    return proc


def killChildren():
    for proc in CHILDREN:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass


def traceArgs(args, i):
    if not args.trace_dir:
        return []
    os.makedirs(args.trace_dir, exist_ok=True)
    return ['--trace', os.path.join(args.trace_dir, 'client%d.csv' % i)]


def waitForLine(proc, pattern, timeout):
    deadline = time.time() + timeout
    lines = []
    while time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            if proc.poll() is not None:
                break
            continue
        lines.append(line)
        if re.search(pattern, line):
            return True, lines
    return False, lines


def main():
    p = argparse.ArgumentParser()
    p.add_argument('client', nargs='+', help='arena_client binaries, used round-robin')
    p.add_argument('--clients', type=int, default=3)
    p.add_argument('--duration', type=float, default=14.0)
    p.add_argument('--netsim', help='RTT[:JITTER] in ms: run the clients behind netsim.py')
    p.add_argument('--trace-dir', help='write --trace files there and report how smoothly clients '
                   'draw each other (tests/trace_stats.py)')
    p.add_argument('--max-corrections', type=int, default=0,
                   help='position corrections an honest client may get (0 on localhost)')
    args = p.parse_args()

    port = freePort()
    server = spawn(
        [sys.executable, os.path.join(SERVER_DIR, 'arena_server.py'), '--port', str(port),
         '--status-interval', '0', '--round-time', '60'],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
        # Windows has no SIGTERM between processes; the server stops on CTRL_BREAK.
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if WINDOWS else 0)
    ok, lines = waitForLine(server, r'server ready', 20)
    if not ok:
        print(''.join(lines))
        server.kill()
        sys.exit('server did not start')

    netsim = None
    clientPort = port
    if args.netsim:
        rtt, _, jitter = args.netsim.partition(':')
        clientPort = freePort()
        netsim = spawn(
            [sys.executable, os.path.join(SERVER_DIR, 'netsim.py'), '--listen', str(clientPort),
             '--server', '127.0.0.1:%d' % port, '--rtt', rtt, '--jitter', jitter or '0', '--seed', '1'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        ok, lines = waitForLine(netsim, r'netsim ready', 10)
        print(''.join(lines).strip())
        if not ok:
            server.kill()
            netsim.kill()
            sys.exit('netsim did not start')

    clients = []
    for i in range(args.clients):
        clients.append(spawn(
            clientCommand(args.client[i % len(args.client)]) + ['--headless', '--port', str(clientPort), '--name', 'Cpp%d' % i,
             '--duration', str(args.duration), '--expect-players', str(args.clients),
             '--say', 'hello from cpp %d' % i] + traceArgs(args, i),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
    pybot = spawn(
        [sys.executable, os.path.join(SERVER_DIR, 'bots.py'), '--port', str(clientPort), '--count', '1',
         '--duration', str(args.duration - 2)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    failures = []
    totals = {'orbs': 0, 'hits': 0, 'dashes': 0, 'contacts': 0, 'confirmed': 0, 'missed': 0,
              'phantom': 0, 'pos_sends_s': 0.0,
              'up_Bps': 0.0, 'down_Bps': 0.0, 'ping_avg_ms': 0.0, 'clients': 0}
    for i, c in enumerate(clients):
        try:
            out, _ = c.communicate(timeout=args.duration + 30)
        except subprocess.TimeoutExpired:
            c.kill()
            out, _ = c.communicate()
            failures.append('client %d hung' % i)
        m = re.search(r'summary: played=(\d+) others=(\d+) orbs=(\d+) pongs=(\d+) chats=(\d+) '
                      r'hits=(\d+)/(\d+) corrections=(\d+) reconnects=(\d+)', out)
        line = next((l for l in out.splitlines() if l.startswith('summary:')), 'no summary')
        name = os.path.basename(args.client[i % len(args.client)])
        print('client %d (%s) exit %s: %s' % (i, name, c.returncode, line))
        if c.returncode != 0 or not m:
            failures.append('client %d failed (exit %s)\n%s' % (i, c.returncode, out[-3000:]))
            continue
        played, others, orbs, pongs, chats, hitsGiven, _, corrections, reconnects = map(int, m.groups())
        totals['orbs'] += orbs
        totals['hits'] += hitsGiven
        totals['clients'] += 1
        for key in ('dashes', 'contacts', 'confirmed', 'missed', 'phantom', 'pos_sends_s',
                    'up_Bps', 'down_Bps', 'ping_avg_ms'):
            v = re.search(r'\b%s=([\d.]+)' % key, out)
            if v:
                totals[key] += float(v.group(1))
        if others < args.clients:  # the other C++ clients + the Python bot
            failures.append('client %d saw only %d other players' % (i, others))
        if pongs < 3:
            failures.append('client %d got only %d pongs' % (i, pongs))
        if chats < args.clients:
            failures.append('client %d received %d chat lines, expected %d' % (i, chats, args.clients))
        if corrections > args.max_corrections:
            failures.append('client %d was corrected %d times (honest client!)' % (i, corrections))
        if reconnects:
            failures.append('client %d reconnected %d times' % (i, reconnects))
        rebuilds = re.search(r'arena_rebuilds=(\d+)', out)
        if not rebuilds or int(rebuilds.group(1)) != 0:
            # Repeat generates (someone else joined) must not rebuild the arena.
            failures.append('client %d rebuilt the arena: %s' % (i, rebuilds and rebuilds.group(0)))
    pybot.communicate(timeout=30)
    if totals['orbs'] == 0:
        failures.append('no C++ client collected an orb')

    if server.poll() is not None:
        failures.append('server died (exit %s)' % server.returncode)
    else:
        server.send_signal(signal.CTRL_BREAK_EVENT if WINDOWS else signal.SIGTERM)
        try:
            out, _ = server.communicate(timeout=10)
            if server.returncode != 0:
                failures.append('server exited with %s when asked to stop' % server.returncode)
            if 'Traceback' in out or 'Assertion' in out:
                failures.append('server log has errors:\n' + out[-3000:])
        except subprocess.TimeoutExpired:
            server.kill()
            failures.append('server did not stop when asked to')

    if netsim is not None:
        netsim.terminate()
        netsim.communicate(timeout=10)
    n = max(1, totals['clients'])
    print('orbs collected by C++ clients: %d, dash hits: %d' % (totals['orbs'], totals['hits']))
    print('netcode: %d dashes, %d on-screen contacts: %d confirmed (%s), %d missed, %d phantom hits; '
          'per client: ping %.0f ms, %.1f position updates/s, up %.0f B/s, down %.0f B/s'
          % (totals['dashes'], totals['contacts'], totals['confirmed'],
             '%.0f%%' % (100.0 * totals['confirmed'] / totals['contacts']) if totals['contacts'] else 'n/a',
             totals['missed'], totals['phantom'], totals['ping_avg_ms'] / n, totals['pos_sends_s'] / n, totals['up_Bps'] / n,
             totals['down_Bps'] / n))
    if args.trace_dir:
        sys.path.insert(0, HERE)
        import trace_stats
        files = [os.path.join(args.trace_dir, 'client%d.csv' % i) for i in range(args.clients)]
        print('smoothness: ' + trace_stats.describe(trace_stats.analyse(
            [f for f in files if os.path.exists(f)])))
    if failures:
        print('FAILED:\n- ' + '\n- '.join(failures))
        sys.exit(1)
    print('end-to-end OK')


if __name__ == '__main__':
    try:
        main()
    finally:
        killChildren()
