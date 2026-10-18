#include "game/GameClient.h"

#include "core/Log.h"
#include "game/Hud.h"
#include "game/World.h"
#include "net/TimeManager.h"

#include "clockObject.h"
#include "executionEnvironment.h"
#include "virtualFileSystem.h"
#include "pandaSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace arena::game {

namespace {

double realTime() { return ClockObject::get_global_clock()->get_real_time(); }

const char *phaseName(GameClient::Phase p) {
    switch (p) {
    case GameClient::Phase::Offline: return "offline";
    case GameClient::Phase::Connecting: return "connecting";
    case GameClient::Phase::SyncingTime: return "syncing time";
    case GameClient::Phase::EnteringArena: return "entering arena";
    case GameClient::Phase::Joining: return "joining";
    case GameClient::Phase::Playing: return "playing";
    case GameClient::Phase::Rejected: return "rejected";
    }
    return "?";
}

std::string formatClock(double seconds) {
    const int s = std::max(0, static_cast<int>(std::ceil(seconds)));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}

// Through the virtual file system: the files may be in the mounted pack.
bool fileExists(const Filename &f) { return VirtualFileSystem::get_global_ptr()->exists(f); }

} // namespace

GameClient::GameClient(const Options &options, World *world, Hud *hud)
    : options_(options), world_(world), hud_(hud), rng_(std::random_device{}()) {
    lastHitAt_.fill(-1e9);
}

GameClient::~GameClient() {
    shutdown();
    if (trace_ != nullptr) std::fclose(trace_);
}

bool GameClient::init(std::string &error) {
    if (!options_.trace.empty()) {
        trace_ = std::fopen(options_.trace.c_str(), "w");
        if (trace_ == nullptr) {
            error = "cannot write " + options_.trace;
            return false;
        }
        std::setvbuf(trace_, nullptr, _IOFBF, 1 << 20);
    }
    // DC files: from the resource pack the app mounted, else --dc-dir, else
    // plain files next to the executable or in the source tree (development).
    std::vector<Filename> dirs;
    if (!options_.resourceDcDir.empty()) {
        dirs.push_back(Filename(options_.resourceDcDir));
    } else if (!options_.dcDir.empty()) {
        dirs.push_back(Filename::from_os_specific(options_.dcDir));
    } else {
        const Filename exeDir =
            Filename::from_os_specific(ExecutionEnvironment::get_binary_name()).get_dirname();
        dirs.push_back(Filename(exeDir, "dc"));
        dirs.push_back(Filename(exeDir, "../common"));
        dirs.push_back(Filename(exeDir, "../../common"));
        dirs.push_back(Filename("dc"));
        dirs.push_back(Filename("common"));
    }
    std::vector<Filename> files;
    for (const Filename &dir : dirs) {
        const Filename direct(dir, "direct.dc"), arena(dir, "arena.dc");
        if (fileExists(direct) && fileExists(arena)) {
            files = {direct, arena};
            break;
        }
    }
    if (files.empty()) {
        error = "cannot find direct.dc and arena.dc (no arena.pak next to the client; use --pack or --dc-dir)";
        return false;
    }
    if (!cr_.loadDcFiles(files, error)) {
        return false;
    }
    LOG_INFO("loaded %s (DC hash %u)", files[1].get_dirname().c_str(), cr_.dcHash());

    if (!cr_.registerClass<net::TimeManager>("TimeManager",
            [this](net::ClientRepository &cr) {
                auto tm = std::make_unique<net::TimeManager>(cr);
                tm->onSynced = [this] { onTimeSynced(); };
                return tm;
            }, error) ||
        !cr_.registerClass<ArenaGame>("DistributedArenaGame",
            [this](net::ClientRepository &cr) { return std::make_unique<ArenaGame>(cr, *this); }, error) ||
        !cr_.registerClass<ArenaPlayer>("DistributedArenaPlayer",
            [this](net::ClientRepository &cr) { return std::make_unique<ArenaPlayer>(cr, *this); }, error)) {
        return false;
    }

    cr_.onReady = [this] { onRepositoryReady(); };
    cr_.onSlot = [this](uint8_t slot, uint32_t doId) { onSlot(slot, doId); };
    cr_.onMove = [this](uint8_t slot, const net::WireState &state, uint16_t sent) { onMove(slot, state, sent); };
    cr_.onEvent = [this](const net::FrameEvent &e) {
        if (game_ != nullptr) game_->applyEvent(e);
    };
    cr_.onSelf = [this](const net::SelfState &msg) { onSelf(msg); };
    cr_.onConnectFailed = [this](const std::string &why) { scheduleReconnect(nextBackoff(), why); };
    cr_.onLost = [this] {
        if (hud_) hud_->addSystem("Connection lost", now_);
        scheduleReconnect(nextBackoff(), "connection lost");
    };
    cr_.onRejected = [this](uint16_t code, const std::string &why) {
        if (net::isFatalReject(code)) {
            rejectReason_ = why;
            setPhase(Phase::Rejected);
            if (hud_) hud_->addSystem("Server refused the connection: " + why, now_);
        } else {
            const double delay = code == net::kRejectServerFull ? 5.0 : nextBackoff();
            scheduleReconnect(delay, why);
        }
    };

    if (options_.name.empty()) {
        options_.name = "Player" + std::to_string(std::uniform_int_distribution<int>(100, 999)(rng_));
    }
    if (options_.color < 0) {
        options_.color = std::uniform_int_distribution<int>(0, ArenaPlayer::kNumColors - 1)(rng_);
    }
    options_.name = ArenaPlayer::sanitizeName(options_.name);
    return true;
}

void GameClient::start() { connectNow(); }

// On the shared network clock:
//   L,<doId>,<t>,<x>,<y>,<h>   where our player really was: the AI's simulation,
//                              which our history matches half a second later
//                              (any correction has arrived by then), every step
//   S,<doId>,<t>,<x>,<y>,<h>   where we show our player, every frame
//   R,<doId>,<t>,<x>,<y>,<h>   where we draw another player, every frame
// server/tests/trace_stats.py compares every R and S line with the L trace.
void GameClient::writeTrace(double now) {
    if (phase_ != Phase::Playing || local_ == nullptr || !cr_.clockDelta().synced()) return;
    const double t = now - cr_.clockDelta().delta();
    const int64_t settled = prediction_.step() - static_cast<int64_t>(net::kStepsPerSec / 2);
    for (int64_t k = std::max(tracedStep_ + 1, settled - 4); k <= settled; ++k) {
        if (const MoveState *s = prediction_.predictedAt(k)) {
            std::fprintf(trace_, "L,%u,%.4f,%.3f,%.3f,%.1f\n", local_->doId(), k / net::kStepsPerSec, s->x, s->y,
                         s->heading);
        }
    }
    tracedStep_ = std::max(tracedStep_, settled);
    std::fprintf(trace_, "S,%u,%.4f,%.3f,%.3f,%.1f\n", local_->doId(), t, local_->x(), local_->y(), local_->h());
    for (const auto &kv : players_) {
        const ArenaPlayer *p = kv.second;
        if (p != local_) std::fprintf(trace_, "R,%u,%.4f,%.3f,%.3f,%.1f\n", kv.first, t, p->x(), p->y(), p->h());
    }
}

// A dash is settled once the server's verdict has surely arrived.
void GameClient::settleDashes(double now, bool all) {
    const double wait = 0.6 + std::max(0.0, rttMs_) / 1000.0;
    while (!dashRecords_.empty() && (all || now - dashRecords_.front().start > wait)) {
        const DashRecord &d = dashRecords_.front();
        counters_.dashContacts += static_cast<int>(d.seen.count());
        counters_.contactsConfirmed += static_cast<int>((d.seen & d.confirmed).count());
        counters_.contactsMissed += static_cast<int>((d.seen & ~d.confirmed).count());
        counters_.phantomHits += static_cast<int>((d.confirmed & ~d.seen).count());
        dashRecords_.pop_front();
    }
}

void GameClient::shutdown() {
    settleDashes(now_, true);
    if (cr_.state() != net::ClientRepository::State::Idle) {
        closing_ = true;
        cr_.disconnect();  // tells the server, so others see us leave at once
        closing_ = false;
    }
}

const Rules &GameClient::rules() const {
    static const Rules defaults;
    return game_ != nullptr ? game_->rules() : defaults;
}

void GameClient::setPhase(Phase phase) {
    if (phase_ != phase) {
        LOG_DEBUG("phase: %s -> %s", phaseName(phase_), phaseName(phase));
        phase_ = phase;
        phaseSince_ = now_;
    }
}

// ----------------------------------------------------------------- connection

void GameClient::resetSlots() {
    for (Slot &s : slots_) s = Slot{};
    localSlot_ = net::kNoSlot;
    lastHitAt_.fill(-1e9);
    myDoId_ = 0;
    prediction_ = Prediction{};
    dashLatch_ = false;
    smoothX_ = smoothY_ = 0.0f;
}

void GameClient::connectNow() {
    resetSlots();
    setPhase(Phase::Connecting);
#if defined(_WIN32)
    const char *platform = "windows";
#elif defined(__APPLE__)
    const char *platform = "macos";
#else
    const char *platform = "linux";
#endif
    const std::string info = std::string("arena-client/1.0 (C++, Panda3D ") +
                             PandaSystem::get_version_string() + ", " + platform +
                             (options_.bot ? ", bot)" : ")");
    cr_.connect(options_.host, options_.port, info);
}

double GameClient::nextBackoff() {
    static const double steps[] = {1.0, 2.0, 4.0, 8.0, 10.0};
    const double base = steps[std::min(backoffStep_, 4)];
    ++backoffStep_;
    return base * std::uniform_real_distribution<double>(0.8, 1.2)(rng_);  // jitter
}

void GameClient::scheduleReconnect(double delay, const std::string &reason) {
    if (phase_ == Phase::Rejected) {
        return;
    }
    offlineReason_ = reason;
    setPhase(Phase::Offline);
    reconnectAt_ = now_ + delay;
    playingSince_ = -1.0;
    if (options_.reconnect) {
        LOG_INFO("%s; retrying in %.1f s", reason.c_str(), delay);
    }
}

void GameClient::onRepositoryReady() {
    backoffStep_ = 0;
    setPhase(Phase::SyncingTime);
    // As in the manual: first only the zone of the TimeManager.
    cr_.setInterestZones({net::kZoneGlobal});
}

void GameClient::onTimeSynced() {
    if (phase_ == Phase::SyncingTime) {
        enterArena();
    }
}

void GameClient::enterArena() {
    setPhase(Phase::EnteringArena);
    cr_.setInterestZones({net::kZoneGlobal, net::kZoneArena});
}

// The AI creates our player where it decides (and tells everyone which way
// it faces); we only ask, with the name and color we would like.
void GameClient::requestJoin() {
    if (game_ == nullptr || phase_ == Phase::Joining || phase_ == Phase::Playing) {
        return;
    }
    game_->requestJoin(options_.name, options_.color);
    setPhase(Phase::Joining);
}

// Playing once we know which player is ours and where the AI put it.
void GameClient::startPlaying() {
    if (phase_ != Phase::Joining || local_ == nullptr || !prediction_.started()) {
        return;
    }
    const MoveState &s = prediction_.state();
    shownHeading_ = s.heading;
    sampleDelays_.fill(0);  // this connection's
    sampleDelayNext_ = 0;
    local_->setLocalPose(s.x, s.y, s.heading);
    setPhase(Phase::Playing);
    playingSince_ = now_;
    counters_.everPlayed = true;
    LOG_INFO("playing as %s (player %u)", local_->name().c_str(), local_->doId());
}

// --------------------------------------------------------------------- frame

void GameClient::update(double now, double dt, const PlayerInput &input) {
    now_ = now;
    cr_.poll();

    switch (phase_) {
    case Phase::Offline:
        if (options_.reconnect && now >= reconnectAt_) {
            ++counters_.reconnects;
            connectNow();
        }
        break;
    case Phase::SyncingTime:
        if (now - phaseSince_ > 5.0) {
            LOG_WARN("no time sync after 5 s, entering anyway");
            enterArena();
        }
        break;
    case Phase::EnteringArena:
        if (now - phaseSince_ > 10.0) {
            cr_.disconnect();
            scheduleReconnect(nextBackoff(), "the server has no arena");
        }
        break;
    case Phase::Joining:
        if (now - phaseSince_ > 10.0) {
            cr_.disconnect();
            scheduleReconnect(nextBackoff(), "the server did not let us join");
        }
        break;
    default:
        break;
    }

    if (net::TimeManager *tm = cr_.timeManager()) {
        tm->update();
    }

    if (phase_ == Phase::Playing && game_ != nullptr && local_ != nullptr) {
        simulate(now, dt, input);

        if (now - lastPing_ >= 2.0) {
            lastPing_ = now;
            pingContext_ = pingContext_ + 1;
            pingSentAt_ = realTime();
            game_->ping(pingContext_);
        }
        if (!said_ && !options_.say.empty() && now - playingSince_ > 2.0) {
            said_ = true;
            sendChat(options_.say);
        }
    }

    // Remote players: drawn a little in the past along the path through their
    // samples, with a fading correction (RemoteMotion).
    viewDelay_.update(dt);
    int others = 0;
    for (auto &kv : players_) {
        ArenaPlayer *p = kv.second;
        if (p == local_) continue;
        ++others;
        p->updateSmoothing(now, static_cast<float>(dt), viewDelay_.value(), rules());
        if (world_) world_->setPlayerPose(p->doId(), p->x(), p->y(), p->h());
    }
    counters_.maxOtherPlayers = std::max(counters_.maxOtherPlayers, others);

    if (world_) {
        const bool focus = local_ != nullptr;
        world_->update(now, static_cast<float>(dt), focus, focus ? local_->x() : 0.0f, focus ? local_->y() : 0.0f);
    }
    updateHud(now, dt);
    cr_.flush();
    if (trace_ != nullptr) writeTrace(now);

    if (playingSince_ >= 0.0) {
        ++frames_;
        counters_.frames = frames_;
        counters_.playSeconds = now - playingSince_;
        counters_.bytesIn = cr_.stats().bytesIn;
        counters_.bytesOut = cr_.stats().bytesOut;
    }
}

// In slot order, as the AI pushes off them.
Prediction::Others GameClient::otherPositions() const {
    Prediction::Others out;
    out.reserve(players_.size());
    for (const Slot &s : slots_) {
        const ArenaPlayer *p = s.player;
        if (p != nullptr && p != local_ && p->knownCount() > 0) out.push_back({p->known().data(), p->knownCount()});
    }
    return out;
}

void GameClient::simulate(double now, double dt, PlayerInput input) {
    const Rules &rules = game_->rules();
    const ArenaLayout &arena = game_->layout();
    if (options_.bot) {
        input = botInput(now);
    }

    // The input as the AI gets it: a direction in 256ths of a turn (or none)
    // and the dash button. We simulate exactly what we send.
    const bool held = std::hypot(input.x, input.y) > 0.1f;
    const uint8_t dir = held ? net::dirByte(input.x, input.y) : 0;
    dashLatch_ = dashLatch_ || input.dash;

    // Our player runs on the AI's step clock: step k ends at network time k / 120.
    const net::ClockDelta &clock = cr_.clockDelta();
    const int64_t target = clock.localToStep(now);
    constexpr int64_t kMaxSteps = 240;  // 2 s; after a longer stall, wait for the AI's word
    if (target - prediction_.step() > kMaxSteps) {
        prediction_.start(target - kMaxSteps, prediction_.state(), prediction_.knowDelay());
    }
    if (target > prediction_.step()) {
        const Prediction::Others others = otherPositions();
        auto advance = [&] {
            if (prediction_.advance(rules, arena, others)) {
                if (world_) world_->playerDashed(local_->doId());
                ++counters_.dashes;
                dashRecords_.push_back(DashRecord{now, {}, {}});
            }
        };
        // Catch up with what was sent; this frame's input starts at the
        // step that ends now, not at a step already past.
        while (prediction_.step() < target - 1) advance();
        net::InputCmd cmd;
        if (prediction_.setInput(held, dir, dashLatch_, now, viewDelay_.wire(), cmd)) {
            cmd.sampleDelay = sampleDelay();
            cr_.sendInput(cmd);
        }
        dashLatch_ = false;
        advance();
    }
    counters_.inputSends = prediction_.inputsSent();
    const MoveState &me = prediction_.state();

    // What this player sees as a hit: touching someone while dashing (victims
    // are immune for the server's hitCooldown, 0.8 s, after being hit).
    if (!dashRecords_.empty() && now - dashRecords_.back().start <= rules.dashTime + 0.05) {
        DashRecord &dash = dashRecords_.back();
        const float touch = 2.0f * rules.playerRadius + 0.1f;
        for (const auto &kv : players_) {
            const ArenaPlayer *p = kv.second;
            const uint8_t slot = p->slot();
            if (p == local_ || slot == net::kNoSlot || dash.seen.test(slot)) continue;
            if (now - lastHitAt_[slot] < 0.8) continue;
            if (std::hypot(p->x() - me.x, p->y() - me.y) <= touch) {
                dash.seen.set(slot);
            }
        }
    }
    settleDashes(now, false);

    // On screen: between the last two steps, plus what is left of any
    // correction (a knockback we heard of late, mostly), fading out quickly
    // but never faster than RemoteMotion's corrections, so it is a slide, not
    // a lurch. Our body turns toward where we move as the others' bodies do.
    const float left = std::hypot(smoothX_, smoothY_);
    if (left > 1e-4f) {
        const float fdt = static_cast<float>(dt);
        const float shrink = std::min(left * (1.0f - std::exp(-fdt / 0.1f)), RemoteMotion::kMaxCorrectionSpeed * fdt);
        smoothX_ *= (left - shrink) / left;
        smoothY_ *= (left - shrink) / left;
    }
    const MoveState &prev = prediction_.previous();
    const float a = std::clamp(static_cast<float>((now - clock.delta()) * net::kStepsPerSec -
                                                  static_cast<double>(prediction_.step())),
                               0.0f, 1.0f);
    const float shownX = prev.x + (me.x - prev.x) * a + smoothX_;
    const float shownY = prev.y + (me.y - prev.y) * a + smoothY_;
    shownHeading_ = turnToward(shownHeading_, me.heading, static_cast<float>(dt));
    local_->setLocalPose(shownX, shownY, shownHeading_);
    if (world_) world_->setPlayerPose(local_->doId(), shownX, shownY, shownHeading_);

    // Predict pickups: hide the orb at once, the server confirms (or not).
    if (game_->roundState() == RoundState::Playing) {
        const float reach = rules.playerRadius + rules.orbRadius;
        for (const auto &kv : game_->orbs()) {
            if (predictedOrbs_.count(kv.first)) continue;
            if (std::hypot(kv.second.x - me.x, kv.second.y - me.y) <= reach) {
                predictedOrbs_[kv.first] = now;
                if (world_) world_->setOrbHidden(kv.first, true);
            }
        }
    }
    for (auto it = predictedOrbs_.begin(); it != predictedOrbs_.end();) {
        if (now - it->second > 1.0) {  // never confirmed: show it again
            if (world_) world_->setOrbHidden(it->first, false);
            it = predictedOrbs_.erase(it);
        } else {
            ++it;
        }
    }
}

bool GameClient::fidgetInput(double now, PlayerInput &in) {
    const double t = now - playingSince_;
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    if (t >= fidgetUntil_ && t < fidgetFrom_) return false;
    if (t >= fidgetUntil_) {  // start a 1-2.5 s fidget, the next one 5-12 s later
        fidgetUntil_ = t + 1.0 + 1.5 * u(rng_);
        fidgetFrom_ = fidgetUntil_ + 5.0 + 7.0 * u(rng_);
        tapUntil_ = tapPauseUntil_ = now;
    }
    if (now >= tapPauseUntil_) {
        // Next tap: 50-180 ms of input, often the opposite way, then a pause.
        const float a = u(rng_) * 6.2831853f;
        if (u(rng_) < 0.4f && (tapX_ != 0.0f || tapY_ != 0.0f)) {
            tapX_ = -tapX_;
            tapY_ = -tapY_;
        } else {
            tapX_ = std::cos(a);
            tapY_ = std::sin(a);
        }
        tapUntil_ = now + 0.05 + 0.13 * u(rng_);
        tapPauseUntil_ = tapUntil_ + 0.04 + 0.3 * u(rng_);
    }
    if (now < tapUntil_) {
        in.x = tapX_;
        in.y = tapY_;
    }
    return true;
}

PlayerInput GameClient::botInput(double now) {
    PlayerInput in;
    if (fidgetInput(now, in)) {
        return in;
    }
    const Rules &rules = game_->rules();
    const ArenaLayout &arena = game_->layout();
    const MoveState &me = prediction_.state();
    float tx = wanderX_, ty = wanderY_;
    float best = 1e9f;
    for (const auto &kv : game_->orbs()) {
        if (predictedOrbs_.count(kv.first)) continue;
        const float d = std::hypot(kv.second.x - me.x, kv.second.y - me.y) - (kv.second.kind ? 6.0f : 0.0f);
        if (d < best) {
            best = d;
            tx = kv.second.x;
            ty = kv.second.y;
        }
    }
    if (best > 1e8f && now > wanderUntil_) {
        std::uniform_real_distribution<float> u(-1.0f, 1.0f);
        wanderX_ = u(rng_) * arena.radius * 0.6f;
        wanderY_ = u(rng_) * arena.radius * 0.6f;
        wanderUntil_ = now + 3.0;
    }
    float dx = tx - me.x, dy = ty - me.y;
    const float dist = std::hypot(dx, dy);
    if (dist > 0.01f) {
        dx /= dist;
        dy /= dist;
    }
    // Steer around pillars in the way.
    for (const Pillar &p : arena.pillars) {
        const float px = p.x - me.x, py = p.y - me.y;
        const float along = px * dx + py * dy;
        if (along <= 0.0f || along > std::min(dist, 6.0f)) continue;
        const float side = px * -dy + py * dx;  // > 0: pillar on our left
        const float clearance = p.radius + rules.playerRadius + 0.6f;
        if (std::fabs(side) < clearance) {
            const float push = (clearance - std::fabs(side)) / clearance * 1.5f;
            // Veer to the side away from the pillar.
            const float perpX = side > 0 ? dy : -dy;
            const float perpY = side > 0 ? -dx : dx;
            dx += perpX * push;
            dy += perpY * push;
        }
    }
    in.x = dx;
    in.y = dy;
    // Dash at someone just ahead, or toward a far gold orb.
    if (me.dashCooldown <= 0.0f) {
        for (const auto &kv : players_) {
            const ArenaPlayer *p = kv.second;
            if (p == local_) continue;
            const float ox = p->x() - me.x, oy = p->y() - me.y;
            const float od = std::hypot(ox, oy);
            if (od < 5.0f && od > 0.1f && (ox * dx + oy * dy) / od > 0.7f) {
                in.dash = true;
                break;
            }
        }
        if (!in.dash && dist > 9.0f && std::uniform_real_distribution<float>(0, 1)(rng_) < 0.02f) {
            in.dash = true;
        }
    }
    return in;
}

void GameClient::sendChat(const std::string &raw) {
    std::string text;
    for (unsigned char c : raw) {
        if (c >= 0x20 && c != 0x7f) text.push_back(static_cast<char>(c));
    }
    if (text.size() > 120) {
        size_t cut = 120;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        text.resize(cut);
    }
    if (!text.empty() && game_ != nullptr && phase_ == Phase::Playing) {
        game_->requestChat(text);
    }
}

// ---------------------------------------------------------------------- HUD

std::string GameClient::playerName(uint8_t slot) const {
    const Slot &s = slots_[slot];
    if (s.player != nullptr) return s.player->name();
    return s.name.empty() ? std::string("?") : s.name;
}

int GameClient::playerColor(uint8_t slot) const {
    const Slot &s = slots_[slot];
    return s.player != nullptr ? s.player->colorIndex() : s.color;
}

void GameClient::updateHud(double now, double dt) {
    // Rates, once per second.
    fps_ = fps_ <= 0.0 ? 1.0 / std::max(dt, 1e-3) : fps_ * 0.95 + 0.05 / std::max(dt, 1e-3);
    if (now - shownFpsTime_ >= 0.5) {  // a number that changes every frame rebuilds the text every frame
        shownFps_ = fps_;
        shownFpsTime_ = now;
    }
    if (now - statsTime_ >= 1.0) {
        const auto &s = cr_.stats();
        const double span = statsTime_ > 0.0 ? now - statsTime_ : 1.0;
        kbIn_ = (s.bytesIn - statsBytesIn_) / 1024.0 / span;
        kbOut_ = (s.bytesOut - statsBytesOut_) / 1024.0 / span;
        msgsInRate_ = (s.msgsIn - statsMsgsIn_) / span;
        msgsOutRate_ = (s.msgsOut - statsMsgsOut_) / span;
        statsBytesIn_ = s.bytesIn;
        statsBytesOut_ = s.bytesOut;
        statsMsgsIn_ = s.msgsIn;
        statsMsgsOut_ = s.msgsOut;
        statsTime_ = now;
    }
    if (!hud_) {
        return;
    }

    HudState st;
    char buf[256];
    const std::string server = options_.host + ":" + std::to_string(options_.port);
    switch (phase_) {
    case Phase::Offline:
        st.center = options_.reconnect
                        ? offlineReason_ + "\nreconnecting in " + std::to_string(std::max(0, static_cast<int>(std::ceil(reconnectAt_ - now)))) + " s"
                        : offlineReason_;
        st.status = "offline";
        break;
    case Phase::Connecting:
        st.center = "Connecting to " + server + " ...";
        st.status = "connecting";
        break;
    case Phase::SyncingTime:
        st.center = "Synchronising clocks ...";
        st.status = "connected";
        break;
    case Phase::EnteringArena:
        st.center = "Entering the arena ...";
        st.status = "connected";
        break;
    case Phase::Joining:
        st.center = "Joining the game ...";
        st.status = "connected";
        break;
    case Phase::Rejected:
        st.center = "The server refused the connection:\n" + rejectReason_ + "\n\nPress Esc to quit";
        st.status = "rejected";
        break;
    case Phase::Playing: {
        std::snprintf(buf, sizeof buf, "%s  |  ping %s  |  %d players  |  %.0f fps", server.c_str(),
                      rttMs_ < 0 ? "-" : (std::to_string(static_cast<int>(rttMs_ + 0.5)) + " ms").c_str(),
                      static_cast<int>(players_.size()), shownFps_);
        st.status = buf;
        break;
    }
    }
    std::snprintf(buf, sizeof buf,
                  "in %.1f KB/s (%.0f msg/s)  out %.1f KB/s (%.0f msg/s)\n"
                  "dropped %llu  |  clock +/- %.1f ms  |  phase %s",
                  kbIn_, msgsInRate_, kbOut_, msgsOutRate_,
                  static_cast<unsigned long long>(cr_.stats().dropped),
                  cr_.clockDelta().synced() ? cr_.clockDelta().uncertainty() * 1000.0 : -1.0, phaseName(phase_));
    st.netStats = buf;

    if (game_ != nullptr && phase_ == Phase::Playing) {
        const double left = game_->roundEndLocal() - now;
        if (game_->roundState() == RoundState::Playing) {
            st.round = "Round " + std::to_string(game_->roundNumber()) + "   " + formatClock(left);
        } else {
            st.round = "Next round in " + formatClock(left);
        }
        for (const auto &kv : players_) {
            ScoreRow row;
            row.name = kv.second->name();
            row.color = kv.second->colorIndex();
            row.score = kv.second->slot() == net::kNoSlot ? 0 : game_->scoreOf(kv.second->slot());
            row.local = kv.second == local_;
            st.scores.push_back(row);
        }
        std::sort(st.scores.begin(), st.scores.end(), [](const ScoreRow &a, const ScoreRow &b) {
            return a.score != b.score ? a.score > b.score : a.name < b.name;
        });
        st.showDash = true;
        st.dashReady = dashReady();
    }
    hud_->update(st, now);
}

float GameClient::dashReady() const {
    const float cd = rules().dashCooldown;
    return cd > 0.0f ? 1.0f - std::clamp(prediction_.state().dashCooldown / cd, 0.0f, 1.0f) : 1.0f;
}

// --------------------------------------------------------------- GameEvents

void GameClient::onGameGenerated(ArenaGame &game) {
    game_ = &game;
    if (world_) {
        world_->setArena(game.layout(), game.rules());
        world_->setOrbs(game.orbs());
    }
    if (phase_ == Phase::EnteringArena || phase_ == Phase::SyncingTime) {
        requestJoin();
    }
}

void GameClient::onGameDisabled(ArenaGame &game) {
    if (game_ == &game) {
        game_ = nullptr;
    }
    predictedOrbs_.clear();
    if (world_) world_->clear();
}

void GameClient::onArenaChanged() {
    ++counters_.arenaRebuilds;
    if (world_ && game_) world_->setArena(game_->layout(), game_->rules());
}

void GameClient::onOrbsReset() {
    predictedOrbs_.clear();
    if (world_ && game_) world_->setOrbs(game_->orbs());
}

void GameClient::onOrbSpawned(uint16_t orbId) {
    if (!world_ || !game_) return;
    auto it = game_->orbs().find(orbId);
    if (it != game_->orbs().end()) world_->addOrb(orbId, it->second);
}

void GameClient::onOrbCollected(uint16_t orbId, uint8_t slot) {
    const bool mine = slot == localSlot_ && slot != net::kNoSlot;
    if (mine) ++counters_.orbsCollected;
    predictedOrbs_.erase(orbId);
    if (world_) world_->collectOrb(orbId, mine);
}

void GameClient::onScoresChanged() {}

void GameClient::onRoundChanged() {
    predictedOrbs_.clear();
}

void GameClient::onPlayerHit(uint8_t attacker, uint8_t victim) {
    lastHitAt_[victim] = now_;
    const bool mine = localSlot_ != net::kNoSlot;
    if (mine && attacker == localSlot_) {
        ++counters_.hitsGiven;
        if (!dashRecords_.empty()) dashRecords_.back().confirmed.set(victim);
    }
    if (mine && victim == localSlot_) {
        ++counters_.hitsTaken;
        if (world_) world_->shake(0.6f);
    }
    if (world_ && slots_[victim].doId != 0) world_->playerHit(slots_[victim].doId);
    const bool involved = mine && (attacker == localSlot_ || victim == localSlot_);
    if (hud_ && involved) hud_->addSystem(playerName(attacker) + " smashed " + playerName(victim), now_);
}

void GameClient::onChat(uint8_t slot, const std::string &text) {
    ++counters_.chatsReceived;
    if (hud_) hud_->addChat(playerName(slot), playerColor(slot), text, now_);
    LOG_INFO("<%s> %s", playerName(slot).c_str(), text.c_str());
}

// Game messages arrive as codes; the text is made here.
void GameClient::onNotice(uint8_t kind, uint8_t slot, uint16_t value) {
    std::string text;
    switch (kind) {
    case kNoticeJoined: text = playerName(slot) + " joined the arena"; break;
    case kNoticeLeft: text = playerName(slot) + " left the arena"; break;
    case kNoticeRoundStart: text = "Round " + std::to_string(value) + " - go!"; break;
    case kNoticeRoundOver:
        if (value == 0) {
            text = "Round over: nobody scored";
        } else if (slot != net::kNoSlot) {
            text = playerName(slot) + " wins the round with " + std::to_string(value) + " points!";
        } else {
            text = "Round over: a draw at " + std::to_string(value) + " points";
        }
        break;
    case kNoticeShutdown: text = "The server is shutting down"; break;
    default: return;
    }
    if (hud_) hud_->announce(text, now_);
    LOG_INFO("* %s", text.c_str());
}

// --------------------------------------------------------------- movement

void GameClient::onSlot(uint8_t slot, uint32_t doId) {
    Slot &s = slots_[slot];
    if (s.player != nullptr && s.player->doId() != doId) s.player->setSlot(net::kNoSlot);
    s.player = nullptr;
    s.hasState = false;
    s.doId = doId;
    if (doId == 0) {
        if (slot == localSlot_) localSlot_ = net::kNoSlot;
        return;  // freed; the name stays for a late "left" notice
    }
    auto it = players_.find(doId);
    if (it != players_.end()) {
        s.player = it->second;
        s.name = s.player->name();
        s.color = s.player->colorIndex();
        s.player->setSlot(slot);
        if (s.player == local_) localSlot_ = slot;
    }
}

// The second most of the recent ones: one hitch does not hold bumps back.
uint8_t GameClient::sampleDelay() const {
    uint8_t most = 0, next = 0;
    for (uint8_t d : sampleDelays_) {
        if (d > most) {
            next = most;
            most = d;
        } else if (d > next) {
            next = d;
        }
    }
    return next;
}

void GameClient::onMove(uint8_t slot, const net::WireState &state, uint16_t sentStep) {
    Slot &s = slots_[slot];
    if (s.player != nullptr) [[likely]] {
        if (s.player != local_) {
            const double lateness = s.player->applyState(state, sentStep, now_, viewDelay_.value(), rules());
            if (prediction_.started()) {
                // How late it is for bumping into (Prediction: by the rule
                // in SelfState::knowDelay), by our clock; the AI hears of it.
                const int64_t late = prediction_.step() - cr_.clockDelta().resolveStep(sentStep, now_);
                sampleDelays_[sampleDelayNext_++ % sampleDelays_.size()] =
                    static_cast<uint8_t>(std::clamp<int64_t>(late, 0, 255));
            }
            // Live samples only; cached ones of idle players can be minutes old.
            if (lateness >= 0.0 && lateness < 0.5) viewDelay_.addLateness(lateness);
        }
    } else if (s.doId != 0) {
        s.state = state;  // the player's generate has not arrived yet
        s.sentStep = sentStep;
        s.hasState = true;
    }
}

void GameClient::onPong(uint16_t context) {
    if (context != pingContext_) return;
    ++counters_.pongs;
    const double ms = (realTime() - pingSentAt_) * 1000.0;
    rttMs_ = rttMs_ < 0.0 ? ms : rttMs_ * 0.7 + ms * 0.3;
    counters_.pingSumMs += ms;
    counters_.pingMaxMs = std::max(counters_.pingMaxMs, ms);
}

void GameClient::onJoined(uint8_t slot, uint32_t playerId) {
    myDoId_ = playerId;
    localSlot_ = slot;
    auto it = players_.find(playerId);
    if (it != players_.end()) {
        local_ = it->second;
        local_->setControlled(true);
        if (world_) world_->addPlayer(playerId, local_->name(), local_->colorIndex(), true);  // as ours
    }
    startPlaying();
}

// Our player's exact state from the AI: the spawn, or a check of our
// prediction (Prediction::reconcile), whose correction fades out on screen.
void GameClient::onSelf(const net::SelfState &msg) {
    if (game_ == nullptr) return;
    ++counters_.selfs;
    const int64_t step = cr_.clockDelta().resolveStep(msg.step, now_);
    const bool first = !prediction_.started();
    float dx = 0.0f, dy = 0.0f;
    const MoveState *mine = prediction_.predictedAt(step);
    const float errorCm = mine ? std::hypot(mine->x - msg.x, mine->y - msg.y) * 100.0f : 0.0f;
    const bool changed = prediction_.reconcile(msg, step, game_->rules(), game_->layout(), otherPositions(), dx, dy);
    smoothX_ -= dx;  // keep showing where we were, then fade to the truth
    smoothY_ -= dy;
    if (changed) {
        const bool event = (msg.flags & (net::kSelfKnock | net::kSelfSnap)) != 0;
        if (!event && !first && errorCm > 5.0f) {
            ++counters_.corrections;
            counters_.correctionMaxCm = std::max(counters_.correctionMaxCm, static_cast<double>(errorCm));
            LOG_DEBUG("our prediction was %.1f cm off at step %lld (%s)", errorCm, static_cast<long long>(step),
                      prediction_.lastStale() ? "an input reached the server late" : "bumped differently");
        }
        if ((msg.flags & net::kSelfSnap) || std::hypot(smoothX_, smoothY_) > 8.0f) {
            smoothX_ = smoothY_ = 0.0f;  // placed there: show it at once
        }
    }
    if (msg.flags & net::kSelfKnock) {
        if (world_) world_->shake(0.4f);
    }
    if (first) {
        shownHeading_ = prediction_.state().heading;
        startPlaying();
    }
}

void GameClient::onPlayerGenerated(ArenaPlayer &player) {
    players_[player.doId()] = &player;
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
        Slot &s = slots_[i];
        if (s.doId != player.doId()) continue;
        s.player = &player;
        s.name = player.name();
        s.color = player.colorIndex();
        player.setSlot(static_cast<uint8_t>(i));
        if (s.hasState && player.doId() != myDoId_) {
            player.applyState(s.state, s.sentStep, now_, viewDelay_.value(), rules());
        }
        s.hasState = false;
        break;
    }
    if (player.doId() == myDoId_) {
        local_ = &player;
        player.setControlled(true);
    }
    if (world_) {
        world_->addPlayer(player.doId(), player.name(), player.colorIndex(), &player == local_);
        world_->setPlayerPose(player.doId(), player.x(), player.y(), player.h());
    }
    if (&player == local_) startPlaying();
}

void GameClient::onPlayerDisabled(ArenaPlayer &player) {
    players_.erase(player.doId());
    if (player.slot() != net::kNoSlot && slots_[player.slot()].player == &player) {
        slots_[player.slot()].player = nullptr;
    }
    if (world_) world_->removePlayer(player.doId());
    if (&player == local_) {
        local_ = nullptr;
        if (!closing_ && cr_.isReady() && (phase_ == Phase::Playing || phase_ == Phase::Joining)) {
            // The AI removed our player: ask for a new one.
            myDoId_ = 0;
            prediction_.stop();
            playingSince_ = -1.0;
            setPhase(Phase::EnteringArena);
            requestJoin();
        }
    }
}

void GameClient::onPlayerDashed(ArenaPlayer &player) {
    if (world_) world_->playerDashed(player.doId());
}

} // namespace arena::game
