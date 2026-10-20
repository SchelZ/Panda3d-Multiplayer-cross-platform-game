"""Game tuning. The server is the single source of truth: these values are
sent to every client through DistributedArenaGame.setRules/setArena, so the
C++ client never hard-codes gameplay numbers."""

import math
from dataclasses import dataclass


@dataclass
class Rules:
    maxSpeed: float = 9.0          # m/s
    accel: float = 42.0            # m/s^2
    dashSpeed: float = 24.0        # m/s, initial dash velocity
    dashTime: float = 0.22         # s
    dashCooldown: float = 1.6      # s
    playerRadius: float = 0.8      # m
    orbRadius: float = 0.45        # m

    arenaRadius: float = 34.0      # m
    roundTime: float = 180.0       # s
    intermissionTime: float = 8.0  # s
    numOrbs: int = 18
    goldChance: float = 0.12
    orbValue: int = 1
    goldValue: int = 5
    orbRespawnDelay: float = 1.5   # s
    hitSteal: int = 2              # points a dash hit steals
    knockbackSpeed: float = 16.0   # m/s
    hitCooldown: float = 0.8       # s a victim is immune after a hit
    chatInterval: float = 0.75     # s between chat messages per player

    def asRulesField(self):
        return [self.maxSpeed, self.accel, self.dashSpeed, self.dashTime,
                self.dashCooldown, self.playerRadius, self.orbRadius]


# Fixed layout: a ring of pillars plus a central one. Kept server-side and
# sent to clients, so changing it here changes it everywhere.
def defaultPillars(arenaRadius):
    pillars = [(0.0, 0.0, 2.6)]
    for i in range(6):
        a = math.radians(i * 60.0 + 30.0)
        r = arenaRadius * 0.55
        pillars.append((round(r * math.cos(a), 1), round(r * math.sin(a), 1), 1.4))
    return pillars


def segmentPointDistanceSq(ax, ay, bx, by, px, py):
    """Squared distance from point P to segment AB."""
    abx, aby = bx - ax, by - ay
    lenSq = abx * abx + aby * aby
    t = 0.0
    if lenSq > 1e-12:
        t = max(0.0, min(1.0, ((px - ax) * abx + (py - ay) * aby) / lenSq))
    dx, dy = ax + abx * t - px, ay + aby * t - py
    return dx * dx + dy * dy
