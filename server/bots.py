#!/usr/bin/env python3
"""Connect N headless Python bots to a Panda Arena server (load testing).

    python bots.py --count 12 --duration 60
"""

import argparse
import builtins
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main(argv=None):
    p = argparse.ArgumentParser(description='Panda Arena load-test bots')
    p.add_argument('--host', default='127.0.0.1')
    p.add_argument('--port', type=int, default=4400)
    p.add_argument('--count', type=int, default=4)
    p.add_argument('--duration', type=float, default=0, help='seconds, 0 = until Ctrl+C')
    p.add_argument('--chat', action='store_true', help='bots say hello every 5 s')
    p.add_argument('--dc-dir', default=os.path.join(HERE, '..', 'common'))
    args = p.parse_args(argv)

    sys.path.insert(0, HERE)
    from panda3d.core import Filename, loadPrcFileData
    loadPrcFileData('bots', 'window-type none\naudio-library-name null\n'
                            'clock-mode limited\nclock-frame-rate 60\nsleep-precision 0\n'
                            'notify-level warning\ndefault-directnotify-level warning\n'
                            'notify-level-device fatal\n')
    from direct.showbase.ShowBase import ShowBase
    from arena.bots import ArenaBotRepository
    from arena.protocol import DC_FILES

    base = ShowBase(windowType='none')
    builtins.simbase = base
    dcFiles = [Filename.fromOsSpecific(os.path.join(args.dc_dir, n)).getFullpath()
               for n in DC_FILES]
    bots = [ArenaBotRepository(args.host, args.port, dcFiles, 'Bot%02d' % i, seed=i,
                               chatty=args.chat) for i in range(args.count)]

    def report(task):
        alive = sum(1 for b in bots if b.isConnected() and not b.lost)
        scores = {}
        for b in bots:
            if b.game and b.mySlot is not None:
                scores[b.botName] = b.game.scores.get(b.mySlot, 0)
        print('[bots] connected %d/%d, pongs %d, corrections %d, scores %s'
              % (alive, len(bots), sum(b.pongs for b in bots),
                 sum(b.corrections for b in bots), scores), flush=True)
        for b in bots:
            if b.rejected:
                print('[bots] %s rejected: %s' % (b.botName, b.rejected), flush=True)
                b.rejected = None
        return task.again

    taskMgr.doMethodLater(5.0, report, 'report')
    if args.duration > 0:
        taskMgr.doMethodLater(args.duration, lambda t: base.userExit(), 'stop')
    try:
        base.run()
    except (KeyboardInterrupt, SystemExit):
        pass


if __name__ == '__main__':
    main()
