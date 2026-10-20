#!/usr/bin/env python3
"""How well clients draw each other, from arena_client --trace files.

    python tests/trace_stats.py trace0.csv trace1.csv ...

Every client logs, on the shared network clock, where its player really was
(L lines: the AI's simulation), where it shows its own player (S lines) and
where it draws every other player (R lines). For each R line this looks up
where that player really was at the same moment (its L trace) and reports:

* error:  distance between drawn and true position (cm);
* jerk:   how hard the drawn motion accelerates from frame to frame, against
          the players' true motion. Corrections that snap or wobble show up as
          drawn acceleration far above the true one;
* jumps:  frames where a drawn player moved more than 1.5x (+2 m/s) faster
          than the real player moved at any moment in the 0.3 s around it, so
          a dash drawn at dash speed is fine but a snap or lurch is counted;
* backtracks: frames where a drawn player moved (over 1 m/s) in a direction
          the real player did not move at any moment in the 0.3 s around it,
          more than 90 degrees off: the rubber band of an overshoot pulled back;
* turn snaps: frames where a drawn heading turned more than 20 degrees at
          once (1200 degrees/s at 60 fps).

The same jumps and turn snaps are reported for each client's own player on
its own screen (S lines): a correction of its prediction shows there.
"""

import bisect
import math
import sys
from collections import defaultdict

JUMP_FACTOR, JUMP_MARGIN = 1.5, 2.0   # drawn speed > factor * real speed + margin
JUMP_WINDOW = (0.3, 0.05)             # s before / after a frame to look for the real speed
MAX_GAP = 0.1       # s: ignore truth lookups across gaps in a trace
BACKTRACK_SPEED = 1.0   # m/s of drawn motion before a wrong direction counts
TURN_SNAP = 20.0        # degrees of heading change in one frame


def load(paths):
    truth = defaultdict(list)    # doId -> [(t, x, y, h)]
    drawn = defaultdict(list)    # (file, doId) -> [(t, x, y, h)]
    shown = defaultdict(list)    # doId -> [(t, x, y, h)], own player on its screen
    for path in paths:
        with open(path) as f:
            for line in f:
                parts = line.rstrip().split(',')
                if len(parts) not in (5, 6):
                    continue
                kind, doId, t, x, y = parts[0], int(parts[1]), float(parts[2]), float(parts[3]), float(parts[4])
                h = float(parts[5]) if len(parts) == 6 else 0.0
                if kind == 'L':
                    truth[doId].append((t, x, y, h))
                elif kind == 'R':
                    drawn[(path, doId)].append((t, x, y, h))
                elif kind == 'S':
                    shown[doId].append((t, x, y, h))
    for track in truth.values():
        track.sort()
    return truth, drawn, shown


def truthAt(track, times, t):
    i = bisect.bisect_left(times, t)
    if i == 0 or i >= len(track):
        return None
    t0, x0, y0 = track[i - 1][:3]
    t1, x1, y1 = track[i][:3]
    if t1 - t0 > MAX_GAP or t1 <= t0:
        return None
    f = (t - t0) / (t1 - t0)
    return x0 + (x1 - x0) * f, y0 + (y1 - y0) * f


def trueVelocities(track, times, t0, t1):
    i = max(bisect.bisect_left(times, t0), 1)
    j = bisect.bisect_right(times, t1)
    for k in range(i, min(j + 1, len(track))):
        a, b = track[k - 1], track[k]
        dt = b[0] - a[0]
        if 1e-4 < dt <= MAX_GAP:
            yield (b[1] - a[1]) / dt, (b[2] - a[2]) / dt


def maxTrueSpeed(track, times, t0, t1):
    return max((math.hypot(vx, vy) for vx, vy in trueVelocities(track, times, t0, t1)), default=0.0)


def wentThatWay(track, times, t0, t1, dx, dy):
    """Did the real player move within 90 degrees of (dx, dy) during [t0, t1]?"""
    return any(vx * dx + vy * dy > 0.0 for vx, vy in trueVelocities(track, times, t0, t1))


def turnSnaps(points):
    """Frames whose heading changed by more than TURN_SNAP degrees."""
    snaps = 0
    for a, b in zip(points, points[1:]):
        if 1e-4 < b[0] - a[0] <= MAX_GAP and abs((b[3] - a[3] + 540.0) % 360.0 - 180.0) > TURN_SNAP:
            snaps += 1
    return snaps


def accelerations(points):
    """|second difference| / dt^2 of a frame-ordered (t, x, y) list."""
    out = []
    for a, b, c in zip(points, points[1:], points[2:]):
        dt1, dt2 = b[0] - a[0], c[0] - b[0]
        if dt1 <= 1e-4 or dt2 <= 1e-4 or dt1 > MAX_GAP or dt2 > MAX_GAP:
            continue
        vx1, vy1 = (b[1] - a[1]) / dt1, (b[2] - a[2]) / dt1
        vx2, vy2 = (c[1] - b[1]) / dt2, (c[2] - b[2]) / dt2
        out.append(math.hypot(vx2 - vx1, vy2 - vy1) / ((dt1 + dt2) * 0.5))
    return out


def pct(values, p):
    if not values:
        return float('nan')
    values = sorted(values)
    return values[min(len(values) - 1, int(p / 100.0 * len(values)))]


def countJumps(points, track, times):
    """(jumps, backtracks) of a drawn track against the real one."""
    jumps = backtracks = 0
    for a, b in zip(points, points[1:]):
        dt = b[0] - a[0]
        if not 1e-4 < dt <= MAX_GAP:
            continue
        dx, dy = b[1] - a[1], b[2] - a[2]
        speed = math.hypot(dx, dy) / dt
        window = (b[0] - JUMP_WINDOW[0], b[0] + JUMP_WINDOW[1])
        if window[0] < times[0] or window[1] > times[-1]:
            continue  # the real track does not cover this frame
        if speed > JUMP_MARGIN:
            real = maxTrueSpeed(track, times, *window)
            if speed > JUMP_FACTOR * real + JUMP_MARGIN:
                jumps += 1
        if speed > BACKTRACK_SPEED and not wentThatWay(track, times, window[0], window[1], dx, dy):
            backtracks += 1
    return jumps, backtracks


def analyse(paths):
    truth, drawn, shown = load(paths)
    times = {doId: [p[0] for p in track] for doId, track in truth.items()}
    errors, drawnAcc, jumps, backtracks, snaps, seconds = [], [], 0, 0, 0, 0.0
    trueAcc, ownSnaps, ownJumps, ownSeconds = [], 0, 0, 0.0
    for track in truth.values():
        trueAcc += accelerations(track)
    for doId, points in shown.items():
        track = truth.get(doId)
        if not track or len(points) < 3:
            continue
        ownSnaps += turnSnaps(points)
        ownJumps += countJumps(points, track, times[doId])[0]
        ownSeconds += points[-1][0] - points[0][0]
    for (path, doId), points in drawn.items():
        track = truth.get(doId)
        if not track or len(points) < 3:
            continue
        seconds += points[-1][0] - points[0][0]
        for t, x, y, _ in points:
            real = truthAt(track, times[doId], t)
            if real is not None:
                errors.append(math.hypot(x - real[0], y - real[1]))
        drawnAcc += accelerations(points)
        snaps += turnSnaps(points)
        j, b = countJumps(points, track, times[doId])
        jumps += j
        backtracks += b
    return {
        'samples': len(errors),
        'error_mean_cm': 100.0 * sum(errors) / len(errors) if errors else float('nan'),
        'error_p50_cm': 100.0 * pct(errors, 50),
        'error_p95_cm': 100.0 * pct(errors, 95),
        'drawn_acc_p50': pct(drawnAcc, 50),
        'drawn_acc_p90': pct(drawnAcc, 90),
        'drawn_acc_p99': pct(drawnAcc, 99),
        'true_acc_p50': pct(trueAcc, 50),
        'true_acc_p90': pct(trueAcc, 90),
        'true_acc_p99': pct(trueAcc, 99),
        'jumps_per_min': 60.0 * jumps / seconds if seconds else float('nan'),
        'backtracks_per_min': 60.0 * backtracks / seconds if seconds else float('nan'),
        'turn_snaps_per_min': 60.0 * snaps / seconds if seconds else float('nan'),
        'own_turn_snaps_per_min': 60.0 * ownSnaps / ownSeconds if ownSeconds else float('nan'),
        'own_jumps_per_min': 60.0 * ownJumps / ownSeconds if ownSeconds else float('nan'),
    }


def describe(s):
    return ('drawn vs true position: mean %.1f cm, median %.1f cm, p95 %.1f cm; '
            'acceleration drawn p50/p90/p99 %.0f/%.0f/%.0f m/s2 (true %.0f/%.0f/%.0f); '
            'per player-minute: jumps %.2f, backtracks %.1f, turn snaps %.1f; own player on its '
            'screen: jumps %.2f, turn snaps %.1f'
            % (s['error_mean_cm'], s['error_p50_cm'], s['error_p95_cm'],
               s['drawn_acc_p50'], s['drawn_acc_p90'], s['drawn_acc_p99'],
               s['true_acc_p50'], s['true_acc_p90'], s['true_acc_p99'], s['jumps_per_min'],
               s['backtracks_per_min'], s['turn_snaps_per_min'], s['own_jumps_per_min'],
               s['own_turn_snaps_per_min']))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    print(describe(analyse(sys.argv[1:])))
