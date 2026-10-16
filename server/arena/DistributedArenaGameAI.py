"""Authoritative game state: orbs, scores, rounds, dash hits, chat.

Players own their movement (client-side prediction gives instant response),
but everything that scores is decided here from the positions the players
broadcast, after checking those positions are physically possible.
"""

import math
import random
import traceback

from direct.directnotify import DirectNotifyGlobal
from direct.distributed.ClockDelta import globalClockDelta
from direct.distributed.DistributedObjectAI import DistributedObjectAI
from direct.task import Task

from .protocol import (
    FLAG_DASH, NO_SLOT, NOTICE_JOINED, NOTICE_LEFT, NOTICE_ROUND_OVER, NOTICE_ROUND_START,
    viewDelay)
from . import motion
from .rules import Rules, defaultPillars, segmentPointDistanceSq

ROUND_PLAYING = 0
ROUND_INTERMISSION = 1

ORB_NORMAL = 0
ORB_GOLD = 1


def sanitizeText(text, limit):
    text = ''.join(ch if ch.isprintable() else ' ' for ch in text)
    return ' '.join(text.split())[:limit]


class DistributedArenaGameAI(DistributedObjectAI):
    notify = DirectNotifyGlobal.directNotify.newCategory('DistributedArenaGameAI')

    TICK = 1.0 / 20.0
    POSITION_SLACK = 0.6       # m: int16/10 quantisation + jitter
    SPEED_TOLERANCE = 1.3      # allowed speed = maxSpeed * this
    BUDGET_SECONDS = 1.5       # how much unused movement may be banked
    CORRECTION_INTERVAL = 0.5  # s between position corrections per player

    # Lag compensation of dash hits: judged on what the attacker drew, which
    # is the C++ client's RemoteMotion (the newest sample dead-reckoned, plus
    # a fading correction offset). Same constants as there.
    MAX_REWIND = 0.4           # s: oldest timestamp accepted (caps the advantage)
    HIT_SLACK = 0.15           # m: sample quantisation and timing
    MIN_HIT_GAP = 0.1          # s: simultaneous hits on one victim count once
    HIT_STEP = 0.01            # s between hit tests along a dash
    TRACK_SECONDS = 1.0

    def __init__(self, air, rules=None, seed=None):
        DistributedObjectAI.__init__(self, air)
        self.rules = rules or Rules()
        self.pillars = defaultPillars(self.rules.arenaRadius)
        self.rng = random.Random(seed)

        self.players = {}         # player doId -> DistributedArenaPlayerAI
        self.playersByOwner = {}  # client doIdBase -> DistributedArenaPlayerAI
        self.scores = {}          # player doId -> int
        self.orbs = {}            # orbId -> (x, y, kind)
        self.nextOrbId = 1
        self.spawnQueue = []      # times at which to spawn one orb

        self.roundState = ROUND_PLAYING
        self.roundNumber = 1
        self.roundEnd = self.now() + self.rules.roundTime
        self.stats = {'orbs': 0, 'hits': 0, 'corrections': 0, 'chat': 0}

        for _ in range(self.rules.numOrbs):
            self._spawnOrb(broadcast=False)

    @staticmethod
    def now():
        return globalClock.getRealTime()

    # ------------------------------------------------------- required fields

    def getRules(self):
        return self.rules.asRulesField()

    def getArena(self):
        return [self.rules.arenaRadius, [list(p) for p in self.pillars]]

    def getRound(self):
        endTime = globalClockDelta.localToNetworkTime(self.roundEnd, bits=32)
        return [self.roundState, self.roundNumber, endTime]

    def getOrbs(self):
        return [[orbId, x, y, kind] for orbId, (x, y, kind) in self.orbs.items()]

    def getScores(self):
        return [[self.players[avId].slot, min(score, 65535)] for avId, score in self.scores.items()
                if avId in self.players and self.players[avId].slot != NO_SLOT]

    # ------------------------------------------------------------ lifecycle

    def announceGenerate(self):
        DistributedObjectAI.announceGenerate(self)
        taskMgr.doMethodLater(self.TICK, self._tick, self.uniqueName('tick'))

    def delete(self):
        taskMgr.remove(self.uniqueName('tick'))
        DistributedObjectAI.delete(self)

    def addPlayer(self, player):
        if not self.players and self.roundState == ROUND_PLAYING:
            # First player on an empty server: give them a full round.
            self._startRound(self.roundNumber)
        self.players[player.doId] = player
        self.playersByOwner[player.ownerId] = player
        self.scores.setdefault(player.doId, 0)
        self.notify.info('%s joined (player %d, client %d, slot %d); %d playing'
                         % (player.name, player.doId, player.ownerId, player.slot, len(self.players)))
        self.d_notice(NOTICE_JOINED, player.slot)

    def removePlayer(self, player):
        self.players.pop(player.doId, None)
        if self.playersByOwner.get(player.ownerId) is player:
            del self.playersByOwner[player.ownerId]
        self.scores.pop(player.doId, None)
        self.notify.info('%s left; %d playing' % (player.name, len(self.players)))
        self.d_notice(NOTICE_LEFT, player.slot)

    # ----------------------------------------------------------- rounds

    def _startRound(self, number):
        self.roundState = ROUND_PLAYING
        self.roundNumber = number & 0xffff
        self.roundEnd = self.now() + self.rules.roundTime
        for avId in self.scores:
            self.scores[avId] = 0
        self.orbs.clear()
        self.spawnQueue = []
        for _ in range(self.rules.numOrbs):
            self._spawnOrb(broadcast=False)
        self.sendUpdate('setScores', [self.getScores()])
        self.sendUpdate('setOrbs', [self.getOrbs()])
        self.sendUpdate('setRound', self.getRound())

    def _endRound(self):
        self.roundState = ROUND_INTERMISSION
        self.roundEnd = self.now() + self.rules.intermissionTime
        self.sendUpdate('setRound', self.getRound())
        if self.scores:
            best = max(self.scores.values())
            winners = [self.players[a] for a, s in self.scores.items()
                       if s == best and a in self.players]
            winner = winners[0].slot if len(winners) == 1 and best > 0 else NO_SLOT
            self.d_notice(NOTICE_ROUND_OVER, winner, min(best, 65535))
            self.notify.info('round %d over: best %d by %s'
                             % (self.roundNumber, best, ', '.join(w.name for w in winners) or '-'))

    def _tick(self, task):
        try:
            now = self.now()
            if now >= self.roundEnd:
                if self.roundState == ROUND_PLAYING:
                    self._endRound()
                else:
                    self._startRound(self.roundNumber + 1)
                    self.d_notice(NOTICE_ROUND_START, NO_SLOT, self.roundNumber)
            if self.roundState == ROUND_PLAYING:
                while self.spawnQueue and self.spawnQueue[0] <= now:
                    self.spawnQueue.pop(0)
                    self._spawnOrb(broadcast=True)
        except Exception:
            # Keep the game running; a bug in one tick must not stop rounds.
            self.notify.warning('tick failed:\n' + traceback.format_exc())
        return Task.again

    # ------------------------------------------------------------- orbs

    def _spawnOrb(self, broadcast):
        r = self.rules
        limit = r.arenaRadius - 2.0
        for _ in range(40):
            a = self.rng.uniform(0.0, 2.0 * math.pi)
            d = limit * math.sqrt(self.rng.random())
            x, y = round(d * math.cos(a), 1), round(d * math.sin(a), 1)
            if any((x - px) ** 2 + (y - py) ** 2 < (pr + r.orbRadius + 0.6) ** 2
                   for px, py, pr in self.pillars):
                continue
            if any((x - ox) ** 2 + (y - oy) ** 2 < 2.0 ** 2 for ox, oy, _ in self.orbs.values()):
                continue
            break
        else:
            return
        kind = ORB_GOLD if self.rng.random() < r.goldChance else ORB_NORMAL
        orbId = self.nextOrbId
        self.nextOrbId = self.nextOrbId % 65535 + 1
        self.orbs[orbId] = (x, y, kind)
        if broadcast:
            self.sendUpdate('orbSpawned', [[orbId, x, y, kind]])

    def _collectOrbs(self, player, ax, ay, bx, by):
        reach = (self.rules.playerRadius + self.rules.orbRadius + 0.15) ** 2
        for orbId, (ox, oy, kind) in list(self.orbs.items()):
            if segmentPointDistanceSq(ax, ay, bx, by, ox, oy) <= reach:
                del self.orbs[orbId]
                value = self.rules.goldValue if kind == ORB_GOLD else self.rules.orbValue
                score = min(self.scores.get(player.doId, 0) + value, 65535)
                self.scores[player.doId] = score
                self.stats['orbs'] += 1
                self.spawnQueue.append(self.now() + self.rules.orbRespawnDelay)
                self.sendUpdate('orbCollected', [orbId, player.slot, score])

    # ------------------------------------------------------------ movement

    def onPlayerState(self, player, x, y, vx, vy, flags, timestamp, ix=0.0, iy=0.0):
        """One movement sample from SERVER_MOVES (protocol.py)."""
        player.viewDelay = viewDelay(flags)
        hold = 0.0
        if flags & FLAG_DASH:
            self.onPlayerDash(player, timestamp)
            hold = motion.dashHoldTime(self.rules.dashTime)
        self.onPlayerMoved(player, x, y, vx, vy, timestamp, ix, iy, hold)

    def onPlayerMoved(self, player, x, y, vx=0.0, vy=0.0, timestamp=None, ix=0.0, iy=0.0, hold=0.0):
        """Accept a broadcast position if it is reachable from the last
        accepted one. Unreachable positions (speed hacks, teleports) are
        ignored for scoring and the owner is told where it really is.
        The speed check uses arrival times, which a client cannot forge;
        the sender's timestamp only places the position on its timeline
        for lag-compensated hits."""
        r = self.rules
        now = self.now()
        maxBudget = r.maxSpeed * self.SPEED_TOLERANCE * self.BUDGET_SECONDS
        inBounds = x * x + y * y <= (r.arenaRadius + 1.0) ** 2

        if player.lastMoveTime is None:
            player.lastMoveTime = now
            if inBounds:
                player.validX, player.validY = x, y
                player.moveBudget = maxBudget
                self._record(player, x, y, vx, vy, timestamp, now, ix, iy, hold)
            return

        dt = max(0.0, now - player.lastMoveTime)
        player.lastMoveTime = now
        player.moveBudget = min(player.moveBudget + r.maxSpeed * self.SPEED_TOLERANCE * dt,
                                max(maxBudget, player.moveBudget))
        dist = math.hypot(x - player.validX, y - player.validY)

        if not inBounds or dist > player.moveBudget + self.POSITION_SLACK:
            if now - player.lastCorrection >= self.CORRECTION_INTERVAL:
                player.lastCorrection = now
                self.stats['corrections'] += 1
                self.notify.info('correcting %s: moved %.1f m, budget %.1f m'
                                 % (player.name, dist, player.moveBudget))
                self.sendUpdateToAvatarId(player.ownerId, 'correctPosition',
                                          [player.validX, player.validY])
            return

        player.moveBudget = max(0.0, player.moveBudget - dist)
        ax, ay = player.validX, player.validY
        player.validX, player.validY = x, y
        if self.roundState == ROUND_PLAYING and self.orbs:
            self._collectOrbs(player, ax, ay, x, y)
        self._record(player, x, y, vx, vy, timestamp, now, ix, iy, hold)

    def onPlayerDash(self, player, timestamp=None):
        r = self.rules
        now = self.now()
        if now - player.lastDash < r.dashCooldown * 0.75:
            return  # faster than the cooldown allows: ignore
        player.lastDash = now
        player.dashStart = player.sampleTime(timestamp, now, self.MAX_REWIND)
        player.dashEnd = player.dashStart + r.dashTime + 0.05
        player.dashHits = set()
        player.moveBudget += r.dashSpeed * r.dashTime * self.SPEED_TOLERANCE

    # --------------------------------------------------- lag compensation

    def _record(self, player, x, y, vx, vy, timestamp, now, ix=0.0, iy=0.0, hold=0.0):
        """Adds an accepted sample to the player's track and judges any dash
        it completes. Velocity and input are only used to replay what clients
        drew (motion.py), and velocity is capped at the fastest anyone moves."""
        r = self.rules
        speed = math.hypot(vx, vy)
        limit = r.dashSpeed + r.knockbackSpeed
        if speed > limit:
            vx, vy = vx * limit / speed, vy * limit / speed
        if speed > 0.5:
            player.heading = math.atan2(-vx, vy)
        t = player.sampleTime(timestamp, now, self.MAX_REWIND)
        track = player.track
        prev = track[-1] if track else None
        if prev is not None:
            t = max(t, prev[0] + 1e-3)  # keep the timeline strictly increasing
        late = now - t
        player.latency = late if player.latency is None else player.latency * 0.9 + late * 0.1
        sample = (t, x, y, vx, vy, now, ix, iy, hold)
        track.append(sample)
        while len(track) > 2 and track[0][5] < now - self.TRACK_SECONDS:
            track.popleft()
        if prev is not None and self.roundState == ROUND_PLAYING and \
                prev[0] <= player.dashEnd and t >= player.dashStart:
            self._checkDashHits(player, prev, sample, now)

    def _checkDashHits(self, attacker, a0, a1, now):
        """Judges the part of a dash between two attacker samples against
        every other player as the attacker saw them at each moment."""
        r = self.rules
        lo, hi = max(a0[0], attacker.dashStart), min(a1[0], attacker.dashEnd)
        if hi < lo:
            return
        span = a1[0] - a0[0]
        reach = 2.0 * r.playerRadius + self.HIT_SLACK
        reachSq = reach * reach
        down = attacker.latency or 0.0  # AI -> attacker, taken as symmetric
        delay = attacker.viewDelay      # how far back the attacker draws others
        steps = max(1, int(math.ceil((hi - lo) / self.HIT_STEP)))
        # How far apart the two can start and still meet in this segment: the
        # attacker's own travel plus the most a drawn player can move.
        fastest = 1.5 * (r.dashSpeed + r.knockbackSpeed) + motion.MAX_CORRECTION_SPEED
        slack = reach + math.hypot(a1[1] - a0[1], a1[2] - a0[2]) + fastest * (hi - lo)
        f = (lo - a0[0]) / span if span > 1e-6 else 1.0
        startX = a0[1] + (a1[1] - a0[1]) * f
        startY = a0[2] + (a1[2] - a0[2]) * f
        for victim in list(self.players.values()):
            if victim is attacker or not victim.track or victim.doId in attacker.dashHits or \
                    now - victim.lastHitTime < self.MIN_HIT_GAP:
                continue
            # The victim as the attacker drew it (motion.Drawing).
            drawing = motion.Drawing(victim.track, down, delay, r.maxSpeed, r.accel)
            vx, vy = drawing.at(lo - delay)
            if math.hypot(vx - startX, vy - startY) > slack:
                continue  # cannot get within reach in this segment
            # After a hit the victim is immune for hitCooldown, as the attacker
            # saw it: from when the news of that hit reached the attacker.
            immuneFrom = victim.lastHitTime + down
            immuneTo = immuneFrom + r.hitCooldown
            for k in range(steps + 1):
                tau = lo + (hi - lo) * k / steps
                if immuneFrom <= tau < immuneTo:
                    continue
                f = (tau - a0[0]) / span if span > 1e-6 else 1.0
                ax = a0[1] + (a1[1] - a0[1]) * f
                ay = a0[2] + (a1[2] - a0[2]) * f
                vx, vy = drawing.at(tau - delay)
                dx, dy = vx - ax, vy - ay
                if dx * dx + dy * dy <= reachSq:
                    self._hit(attacker, victim, dx, dy, now)
                    break

    def _hit(self, attacker, victim, dx, dy, now):
        r = self.rules
        attacker.dashHits.add(victim.doId)
        victim.lastHitTime = now
        steal = min(r.hitSteal, self.scores.get(victim.doId, 0))
        self.scores[victim.doId] = self.scores.get(victim.doId, 0) - steal
        self.scores[attacker.doId] = min(self.scores.get(attacker.doId, 0) + steal, 65535)
        dist = math.hypot(dx, dy)
        if dist < 1e-3:
            nx, ny = -math.sin(attacker.heading), math.cos(attacker.heading)
        else:
            nx, ny = dx / dist, dy / dist
        kv = r.knockbackSpeed
        victim.moveBudget += kv * 0.6 * self.SPEED_TOLERANCE
        self.stats['hits'] += 1
        self.sendUpdateToAvatarId(victim.ownerId, 'knockback', [nx * kv, ny * kv])
        self.sendUpdate('playerHit', [attacker.slot, victim.slot,
                                      self.scores[attacker.doId],
                                      self.scores[victim.doId]])

    # ---------------------------------------------------------- requests

    def requestChat(self, text):
        sender = self.air.getAvatarIdFromSender()
        player = self.playersByOwner.get(sender)
        if player is None:
            return
        now = self.now()
        if now - player.lastChat < self.rules.chatInterval:
            return
        text = sanitizeText(text, 120)
        if not text:
            return
        player.lastChat = now
        self.stats['chat'] += 1
        self.notify.info('chat <%s> %s' % (player.name, text))
        self.sendUpdate('chatMessage', [player.slot, text])

    def ping(self, context):
        sender = self.air.getAvatarIdFromSender()
        if sender:
            self.sendUpdateToAvatarId(sender, 'pong', [context])

    def d_notice(self, kind, slot=NO_SLOT, value=0):
        """Game messages go out as codes (protocol.py NOTICE_*), not text."""
        self.sendUpdate('notice', [kind, slot, value])
