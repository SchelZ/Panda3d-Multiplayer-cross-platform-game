"""Authoritative game state: players' movement, orbs, scores, rounds, dash
hits, chat.

The AI creates every player (requestJoin) and moves it: a client sends only
its inputs, and the AI runs the movement code on them (simulation.py). So
positions, speeds, pickups and hits are decided here, never by a client.
Clients predict their own player from the same inputs, so they respond at
once, and are told when the AI's result differs.
"""

import math
import random
import traceback
from collections import deque

from direct.directnotify import DirectNotifyGlobal
from direct.distributed.ClockDelta import globalClockDelta
from direct.distributed.DistributedObjectAI import DistributedObjectAI
from direct.task import Task

from .protocol import (
    MAX_SLOTS, NO_SLOT, NOTICE_JOINED, NOTICE_LEFT, NOTICE_ROUND_OVER, NOTICE_ROUND_START, STEP,
    STEPS_PER_SECOND)
from . import motion
from .DistributedArenaPlayerAI import DistributedArenaPlayerAI
from .frame import EVENT_ORB_COLLECTED, EVENT_ORB_SPAWNED, EVENT_PLAYER_HIT, packAiEvent
from .rules import Rules, defaultPillars, segmentPointDistanceSq
from .simulation import PlayerSim

ROUND_PLAYING = 0
ROUND_INTERMISSION = 1

ORB_NORMAL = 0
ORB_GOLD = 1


def sanitizeText(text, limit):
    text = ''.join(ch if ch.isprintable() else ' ' for ch in text)
    return ' '.join(text.split())[:limit]


class DistributedArenaGameAI(DistributedObjectAI):
    notify = DirectNotifyGlobal.directNotify.newCategory('DistributedArenaGameAI')

    TICK = 1.0 / 20.0          # rounds and orb spawns
    MAX_STEPS_PER_FRAME = 60   # catching up after a hitch: at most 0.5 s per frame
    KNOWN_SLACK = 4.0          # m a client's idea of a player may lag behind it
    MOVES_INTERVAL = 1.0 / 30  # s between batches: samples, events, SELF (one packet each)

    # Lag compensation of dash hits: judged on what the attacker drew, which
    # is the C++ client's RemoteMotion (the newest sample dead-reckoned, plus
    # a fading correction offset). Same constants as there.
    HIT_SLACK = 0.15           # m: sample quantisation and timing
    MIN_HIT_GAP = 0.1          # s: simultaneous hits on one victim count once
    HIT_STEP = 0.01            # s between hit tests along a dash

    def __init__(self, air, rules=None, seed=None, batchRate=None):
        DistributedObjectAI.__init__(self, air)
        if batchRate:
            self.MOVES_INTERVAL = 1.0 / max(5.0, min(batchRate, 120.0))
        self.rules = rules or Rules()
        self.pillars = defaultPillars(self.rules.arenaRadius)
        self.rng = random.Random(seed)

        self.players = {}         # player doId -> DistributedArenaPlayerAI
        self.playersByOwner = {}  # client doIdBase -> DistributedArenaPlayerAI
        self.scores = {}          # player doId -> int
        self.orbs = {}            # orbId -> (x, y, kind)
        self.nextOrbId = 1
        self.spawnQueue = []      # times at which to spawn one orb
        # Freed slots go to the back of the queue, so a slot is not reused
        # while messages naming its previous player are in flight.
        self.freeSlots = deque(range(MAX_SLOTS))
        self.lastMoves = -1e9
        self.events = []          # AI_FRAME events for the next batch (frame.packAiEvent)

        self.roundState = ROUND_PLAYING
        self.roundNumber = 1
        self.roundEnd = self.now() + self.rules.roundTime
        self.stats = {'orbs': 0, 'hits': 0, 'chat': 0, 'joins': 0}

        for _ in range(self.rules.numOrbs):
            self._spawnOrb(broadcast=False)

    @staticmethod
    def now():
        return globalClock.getRealTime()

    def nowStep(self, now):
        return int(math.floor(now * STEPS_PER_SECOND))

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
        # Every server frame, after the AI read this frame's inputs (sort 5)
        # and before it flushes (sort 999).
        taskMgr.add(self._frame, self.uniqueName('frame'), sort=500)

    def sendUpdate(self, fieldName, args=[]):
        # Field updates are relayed at once; the events of the batch being
        # gathered go first, so every client sees everything in order.
        self._sendEvents()
        DistributedObjectAI.sendUpdate(self, fieldName, args)

    def delete(self):
        taskMgr.remove(self.uniqueName('tick'))
        taskMgr.remove(self.uniqueName('frame'))
        DistributedObjectAI.delete(self)

    # ------------------------------------------------------------- players

    def requestJoin(self, name, colorIndex):
        """A client asks for a player. The AI creates it, places it and
        tells everyone; the client only ever steers it."""
        sender = self.air.getAvatarIdFromSender()
        if not sender or sender in self.playersByOwner or not self.freeSlots:
            return
        self._sendEvents()  # all about this slot's last player first
        slot = self.freeSlots.popleft()
        player = DistributedArenaPlayerAI(self.air, name, colorIndex, sender, slot)
        doId = self.air.allocateDoId()
        # The server learns whose player it is (inputs, SELF) before it relays
        # the player to anyone, so everyone can map the slot right away.
        self.air.sendSlot(slot, sender, doId)
        self.air.createDistributedObject(distObj=player, zoneId=self.zoneId, doId=doId)
        now = self.now()
        player.sim = PlayerSim(self._spawnState(), self.nowStep(now) - PlayerSim.LAG_START, now)
        self.addPlayer(player)
        self.sendUpdateToAvatarId(sender, 'joined', [slot, doId])

    def onClientLeft(self, clientId):
        """The player's connection closed (SERVER_CLIENT_LEFT)."""
        player = self.playersByOwner.get(clientId)
        if player is None:
            return
        self.removePlayer(player)
        slot = player.slot
        self.air.deletePlayer(player)
        if slot != NO_SLOT:
            self.air.sendSlot(slot, 0, 0)
            self.freeSlots.append(slot)

    def _spawnState(self):
        """Somewhere free, away from pillars and players, facing the centre."""
        r = self.rules
        x = y = 0.0
        for _ in range(64):
            a = self.rng.uniform(0.0, 2.0 * math.pi)
            d = (r.arenaRadius - 3.0) * math.sqrt(self.rng.random())
            x, y = d * math.cos(a), d * math.sin(a)
            if all(math.hypot(x - px, y - py) > pr + r.playerRadius + 1.0 for px, py, pr in self.pillars) \
                    and all(math.hypot(x - p.sim.state.x, y - p.sim.state.y) > 4.0 * r.playerRadius
                            for p in self.players.values()):
                break
        heading = math.degrees(math.atan2(x, -y)) if x or y else 0.0
        return motion.MoveState(x, y, heading=heading)

    def addPlayer(self, player):
        if not self.players and self.roundState == ROUND_PLAYING:
            # First player on an empty server: give them a full round.
            self._startRound(self.roundNumber)
        self.players[player.doId] = player
        self.playersByOwner[player.ownerId] = player
        self.scores.setdefault(player.doId, 0)
        self.stats['joins'] += 1
        self.notify.info('%s joined (player %d, client %d, slot %d); %d playing'
                         % (player.name, player.doId, player.ownerId, player.slot, len(self.players)))
        self.d_notice(NOTICE_JOINED, player.slot)

    def removePlayer(self, player):
        self.players.pop(player.doId, None)
        if self.playersByOwner.get(player.ownerId) is player:
            del self.playersByOwner[player.ownerId]
        self.scores.pop(player.doId, None)
        st = player.sim.stats if player.sim else {}
        self.notify.info('%s left; %d playing (inputs %d, %d late, samples %d, SELF %d, contact steps %d, lag %d steps)'
                         % (player.name, len(self.players), st.get('inputs', 0), st.get('late', 0),
                            st.get('samples', 0), st.get('selfs', 0), st.get('contacts', 0), player.sim.lag if player.sim else 0))
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
            self.events.append(packAiEvent(EVENT_ORB_SPAWNED, orbId, int(round(x * 10)),
                                           int(round(y * 10)), kind))

    def _collectOrbs(self, player, ax, ay, bx, by):
        r = self.rules
        reach = r.playerRadius + r.orbRadius + 0.15
        reachSq = reach * reach
        lox, hix = min(ax, bx) - reach, max(ax, bx) + reach
        loy, hiy = min(ay, by) - reach, max(ay, by) + reach
        for orbId, (ox, oy, kind) in list(self.orbs.items()):
            if ox < lox or ox > hix or oy < loy or oy > hiy:
                continue
            if segmentPointDistanceSq(ax, ay, bx, by, ox, oy) <= reachSq:
                del self.orbs[orbId]
                value = r.goldValue if kind == ORB_GOLD else r.orbValue
                score = min(self.scores.get(player.doId, 0) + value, 65535)
                self.scores[player.doId] = score
                self.stats['orbs'] += 1
                self.spawnQueue.append(self.now() + r.orbRespawnDelay)
                self.events.append(packAiEvent(EVENT_ORB_COLLECTED, orbId, player.slot, score))

    # ------------------------------------------------------------ movement

    def _frame(self, task):
        """Simulates every player up to its target step and, once per batch,
        sends what changed: samples for everyone, the events, SELF for owners
        whose prediction may be off."""
        try:
            now = self.now()
            nowStep = self.nowStep(now)
            players = list(self.players.values())
            for player in players:
                self._advance(player, players, nowStep, now)
            for player in players:
                player.sim.chooseKnowDelay()
            # Players hear from us in batches every MOVES_INTERVAL, one
            # AI_FRAME each, which the server turns into one message (one
            # packet) per client. Samples say which step they describe, so
            # this delays nothing on screen but the news.
            if now - self.lastMoves >= self.MOVES_INTERVAL - 1e-4:
                # Keeps the rate when the interval is not a whole number of ticks.
                self.lastMoves = max(self.lastMoves + self.MOVES_INTERVAL,
                                     now - self.MOVES_INTERVAL)
                samples, selfs = [], []
                for player in players:
                    sim = player.sim
                    data = sim.sampleDue(self.rules, now, nowStep)
                    if data is not None:
                        samples.append((player.slot, data))
                    data = sim.selfDue(now)
                    if data is not None:
                        selfs.append((player.slot, data))
                if samples or selfs or self.events:
                    events, self.events = self.events, []
                    self.air.sendFrame(nowStep, samples, selfs, events)
        except Exception:
            self.notify.warning('frame failed:\n' + traceback.format_exc())
        return Task.cont

    def _sendEvents(self):
        """The events gathered so far, now, without waiting for the batch."""
        if self.events:
            events, self.events = self.events, []
            self.air.sendFrame(self.nowStep(self.now()), [], [], events)

    def _advance(self, player, players, nowStep, now):
        sim = player.sim
        steps = min(sim.targetStep(nowStep, now) - sim.step, self.MAX_STEPS_PER_FRAME)
        if steps <= 0:
            return
        r = self.rules
        pillars, radius = self.pillars, r.arenaRadius
        minDist = 2.0 * r.playerRadius
        playing = self.roundState == ROUND_PLAYING
        # Who could touch us in this frame, where the client has them (its
        # samples may lag behind fast moves), in slot order as the client does.
        near = minDist + self.KNOWN_SLACK + steps * STEP * 2.0 * (r.dashSpeed + r.knockbackSpeed)
        s = sim.state
        others = [p.sim for p in sorted(players, key=lambda p: p.slot) if p is not player and
                  abs(p.sim.state.x - s.x) < near and abs(p.sim.state.y - s.y) < near]
        late = sim.stats['late']
        for _ in range(steps):
            k, dashed, x0, y0 = sim.stepOnce(r, pillars, radius)
            if sim.stats['late'] != late:
                late = sim.stats['late']
                self.notify.debug('%s: input %d for step %d applied at %d (lag %d, now %d)'
                                  % ((player.name,) + sim.lastLate + (nowStep,)))
            # Soft collisions with the other players as this player's client
            # has them at this step, by the rule in SELF (protocol.py), so its
            # prediction matches (Prediction.cpp does the same).
            contact = False
            know = sim.knowDelayAt(k)
            # Not while dashing or knocked back: the player flies through.
            free = s.dashLeft <= motion.TIMER_EPS and s.stun <= motion.TIMER_EPS
            for o in (others if free else ()):
                at = o.knownAt(k, know, r.maxSpeed, r.accel)
                if at is not None and abs(s.x - at[0]) < minDist and abs(s.y - at[1]) < minDist:
                    contact = motion.separateFrom(s, at[0], at[1], minDist) or contact
            if contact:
                motion.resolveCollisions(s, r.playerRadius, pillars, radius)
                sim.stats['contacts'] += 1
                sim.wantSelf()  # in case the client knew the others a little differently
            sim.record()
            if dashed:
                player.dashStart = (k - 1) * STEP
                player.dashEnd = player.dashStart + r.dashTime + 0.05
                player.dashChecked = player.dashStart
                player.dashHits = set()
                player.drawings = {}
            if playing and self.orbs:
                self._collectOrbs(player, x0, y0, s.x, s.y)
        if playing and player.dashChecked < player.dashEnd:
            self._checkDashHits(player, now)

    # --------------------------------------------------- lag compensation

    def _checkDashHits(self, attacker, now):
        """Judges the attacker's dash, as far as it is simulated, against
        every other player as the attacker saw them at each moment: drawn
        `viewDelay` in the past from the samples that had reached it."""
        r = self.rules
        sim = attacker.sim
        lo = attacker.dashChecked
        hi = min(attacker.dashEnd, sim.step * STEP)
        if hi <= lo:
            return
        attacker.dashChecked = hi
        reach = 2.0 * r.playerRadius + self.HIT_SLACK
        reachSq = reach * reach
        down = sim.latency or 0.0  # AI -> attacker, taken as symmetric
        delay = sim.viewDelay      # how far back the attacker draws others
        steps = max(1, int(math.ceil((hi - lo) / self.HIT_STEP)))
        path = [(lo + (hi - lo) * k / steps,) + sim.positionAt(lo + (hi - lo) * k / steps)
                for k in range(steps + 1)]
        # How far apart the two can start and still meet: the attacker's own
        # travel plus the most a drawn player can move.
        fastest = 1.5 * (r.dashSpeed + r.knockbackSpeed) + motion.MAX_CORRECTION_SPEED
        travel = math.hypot(path[-1][1] - path[0][1], path[-1][2] - path[0][2])
        slack = reach + travel + fastest * (hi - lo)
        for victim in list(self.players.values()):
            vsim = victim.sim
            if victim is attacker or not vsim.track or victim.doId in attacker.dashHits or \
                    now - victim.lastHitTime < self.MIN_HIT_GAP:
                continue
            # The victim as the attacker drew it (motion.Drawing), kept for
            # the dash while no new sample of the victim went out.
            version = vsim.stats['samples']
            cached = attacker.drawings.get(victim.doId)
            if cached is None or cached[0] != version:
                cached = (version, motion.Drawing(vsim.track, down, delay, r.maxSpeed, r.accel))
                attacker.drawings[victim.doId] = cached
            drawing = cached[1]
            vx, vy = drawing.at(lo - delay)
            if math.hypot(vx - path[0][1], vy - path[0][2]) > slack:
                continue  # cannot get within reach in this part of the dash
            # After a hit the victim is immune for hitCooldown, as the attacker
            # saw it: from when the news of that hit reached the attacker.
            immuneFrom = victim.lastHitTime + down
            immuneTo = immuneFrom + r.hitCooldown
            for tau, ax, ay in path:
                if immuneFrom <= tau < immuneTo:
                    continue
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
            h = math.radians(attacker.sim.state.heading)
            nx, ny = -math.sin(h), math.cos(h)
        else:
            nx, ny = dx / dist, dy / dist
        kv = r.knockbackSpeed
        victim.sim.knockedBack(nx * kv, ny * kv)
        self.stats['hits'] += 1
        self.events.append(packAiEvent(EVENT_PLAYER_HIT, attacker.slot, victim.slot,
                                       self.scores[attacker.doId], self.scores[victim.doId]))

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
