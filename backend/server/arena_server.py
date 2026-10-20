#!/usr/bin/env python3
"""Panda Arena dedicated server.

Runs the two server-side parts of Panda3D's distributed networking in one
process, as described in the manual ("Servers"): the ServerRepository that
clients connect to, and an AI repository (a ClientRepository with the AI dc
suffix) that owns the game objects and runs the game logic.

    python arena_server.py                   # listen on 0.0.0.0:4400
    python arena_server.py --port 5000 --max-players 8 --round-time 120
"""

import argparse
import builtins
import os
import secrets
import signal
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
COMMON = os.path.normpath(os.path.join(HERE, '..', 'common'))


def parseArgs(argv=None):
    p = argparse.ArgumentParser(description='Panda Arena dedicated server')
    p.add_argument('--bind', default='0.0.0.0',
                   help='address to listen on (default 0.0.0.0 = all IPv4; use :: for IPv6)')
    p.add_argument('--port', type=int, default=4400, help='TCP port (default 4400)')
    p.add_argument('--max-players', type=int, default=16)
    p.add_argument('--round-time', type=float, default=180.0, help='seconds per round')
    p.add_argument('--orbs', type=int, default=18, help='orbs on the field')
    p.add_argument('--tick-rate', type=int, default=100,
                   help='server loop frequency in Hz (relay latency is at most 1/tick)')
    p.add_argument('--batch-rate', type=float, default=30.0,
                   help='batches of movement and events per second, each one TCP packet per '
                        'player (default 30; 50 shows others a few ms sooner for ~25%% more '
                        'traffic)')
    p.add_argument('--dc-dir', default=COMMON, help='folder with direct.dc and arena.dc')
    p.add_argument('--name', default=None,
                   help='server name shown to LAN clients (default: this computer\'s name)')
    p.add_argument('--no-lan', action='store_true',
                   help='do not answer LAN discovery probes (UDP, same port number)')
    p.add_argument('--status-interval', type=float, default=30.0,
                   help='seconds between status lines (0 = off)')
    p.add_argument('--verbose', action='store_true', help='debug logging')
    return p.parse_args(argv)


def configurePanda(args):
    from panda3d.core import loadPrcFileData
    level = 'debug' if args.verbose else 'info'
    loadPrcFileData('arena-server', '\n'.join([
        'window-type none',
        'audio-library-name null',
        # Run the loop at a fixed rate instead of spinning a core.
        'clock-mode limited',
        'clock-frame-rate %d' % max(10, args.tick_rate),
        # Sleep for the whole idle part of a tick. Panda's default (0.01 s)
        # busy-waits the last 10 ms of every frame: 100% CPU at 100 Hz.
        'sleep-precision 0',
        # Coalesce all messages of one tick into one TCP write per client;
        # ArenaServerRepository flushes at the end of every tick.
        'collect-tcp 1',
        'collect-tcp-interval 0.05',
        'notify-level warning',
        'default-directnotify-level warning',
        'notify-level-net warning',
        'notify-level-device fatal',  # headless: no /dev/input
        'notify-level-ArenaServerRepository %s' % level,
        'notify-level-ArenaAIRepository %s' % level,
        'notify-level-DistributedArenaGameAI %s' % level,
        'notify-level-DistributedArenaPlayerAI %s' % level,
        'notify-timestamp 1',
    ]))


def main(argv=None):
    args = parseArgs(argv)
    sys.path.insert(0, HERE)
    configurePanda(args)

    from direct.showbase.ShowBase import ShowBase
    from panda3d.core import Filename
    from arena.ai_repository import ArenaAIRepository
    from arena.protocol import DC_FILES, NOTICE_SHUTDOWN, PROTOCOL_VERSION
    from arena.rules import Rules
    from arena.server_repository import ArenaServerRepository

    dcFiles = []
    for name in DC_FILES:
        path = os.path.join(args.dc_dir, name)
        if not os.path.isfile(path):
            sys.exit('missing DC file: %s' % path)
        dcFiles.append(Filename.fromOsSpecific(path).getFullpath())

    base = ShowBase(windowType='none')
    builtins.simbase = base

    # The AI authenticates with a secret only this process knows.
    token = secrets.token_hex(16)
    try:
        server = ArenaServerRepository(args.port, bindAddress=args.bind, dcFileNames=dcFiles,
                                       maxPlayers=args.max_players, aiToken=token)
    except RuntimeError as e:
        sys.exit('error: %s' % e)

    # LAN discovery (arena/discovery.py): clients on this network can find us.
    beacon = None
    if not args.no_lan:
        from arena.discovery import LanBeacon
        import socket as _socket
        try:
            beacon = LanBeacon(args.port, args.name or _socket.gethostname(), args.max_players,
                               server.numPlayers, bindAddress=args.bind)
        except OSError as e:
            print('warning: LAN discovery is off (UDP port %d: %s)' % (args.port, e),
                  file=sys.stderr, flush=True)

    rules = Rules(roundTime=args.round_time, numOrbs=args.orbs)
    state = {'stopping': False, 'ready': False}

    def onReady():
        state['ready'] = True
        print('Panda Arena server ready on %s:%d (protocol %d, DC hash %d, max %d players)'
              % (args.bind, args.port, PROTOCOL_VERSION, server.hashVal,
                 args.max_players), flush=True)

    def onLost():
        if not state['stopping']:
            print('fatal: the AI lost its connection, stopping', file=sys.stderr, flush=True)
            requestStop()

    air = ArenaAIRepository(args.port, token, dcFiles, rules, onReady=onReady, onLost=onLost,
                            batchRate=args.batch_rate)
    simbase.air = air

    def requestStop(*_):
        state['stopping'] = True

    signal.signal(signal.SIGINT, requestStop)
    signal.signal(signal.SIGTERM, requestStop)
    if hasattr(signal, 'SIGBREAK'):
        signal.signal(signal.SIGBREAK, requestStop)

    last = {'time': time.monotonic(), 'msgs': 0, 'bytes': 0, 'out': 0, 'inputs': 0}

    def statusTask(task):
        if args.status_interval > 0 and time.monotonic() - last['time'] >= args.status_interval:
            now = time.monotonic()
            dt = now - last['time']
            s = server.stats
            game = air.game
            print('[status] players %d | in %.0f msg/s %.1f KB/s (inputs %.0f/s) | out %.0f msg/s | '
                  'kicks %d%s'
                  % (server.numPlayers(), (s['msgsIn'] - last['msgs']) / dt,
                     (s['bytesIn'] - last['bytes']) / dt / 1024.0, (s['inputs'] - last['inputs']) / dt,
                     (s['msgsOut'] - last['out']) / dt, s['kicks'],
                     ' | round %d, orbs taken %d, hits %d'
                     % (game.roundNumber, game.stats['orbs'], game.stats['hits']) if game else ''),
                  flush=True)
            last.update(time=now, msgs=s['msgsIn'], bytes=s['bytesIn'], out=s['msgsOut'],
                        inputs=s['inputs'])
        if state['stopping']:
            return task.done
        return task.cont

    def stopWatcher(task):
        if not state['stopping']:
            return task.cont
        print('shutting down...', flush=True)
        if air.game is not None and air.isConnected():
            air.game.d_notice(NOTICE_SHUTDOWN)
            air.flush()
        # Let the announcement reach the server loop before closing clients.
        taskMgr.doMethodLater(0.2, finalStop, 'arenaFinalStop')
        return task.done

    def finalStop(task):
        server.shutdown()
        if beacon is not None:
            beacon.close()
        base.userExit()

    def pump(task):
        # End of tick: send what the AI produced and relay it immediately, so a
        # client request and the AI's reply cross the server in one tick.
        if air.isConnected():
            air.flush()
        server.readerPollUntilEmpty(task)
        server.flushPending()
        if beacon is not None:
            beacon.poll()
        return task.cont

    taskMgr.add(pump, 'arenaPump', sort=1001)
    taskMgr.add(statusTask, 'arenaStatus')
    taskMgr.add(stopWatcher, 'arenaStopWatcher')
    try:
        base.run()
    except (KeyboardInterrupt, SystemExit):
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
