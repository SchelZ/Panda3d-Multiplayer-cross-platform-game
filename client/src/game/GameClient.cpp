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
    cr_.onMove = [this](uint8_t slot, const net::WireState &state) { onMove(slot, state); };
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

// One line per player per frame, on the shared network clock:
//   L,<doId>,<t>,<x>,<y>,<h>   where this client really is (h: drawn heading)
//   R,<doId>,<t>,<x>,<y>,<h>   where it draws another player
// server/tests/trace_stats.py compares every R line with the owner's L trace.
void GameClient::writeTrace(double now) {
    if (phase_ != Phase::Playing || local_ == nullptr || !cr_.clockDelta().synced()) return;
    const double t = now - cr_.clockDelta().delta();
    std::fprintf(trace_, "L,%u,%.4f,%.3f,%.3f,%.1f\n", local_->doId(), t, move_.x, move_.y, local_->h());
    for (const auto &kv : players_) {
        const ArenaPlayer *p = kv.second;
        if (!p->isLocal()) std::fprintf(trace_, "R,%u,%.4f,%.3f,%.3f,%.1f\n", kv.first, t, p->x(), p->y(), p->h());
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
        cr_.disconnect();  // tells the server, so others see us leave at once
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

void GameClient::createLocalPlayer() {
    if (local_ != nullptr || game_ == nullptr) {
        return;
    }
    const Rules &r = game_->rules();
    const ArenaLayout &arena = game_->layout();
    // Spawn somewhere free: away from pillars and other players.
    std::uniform_real_distribution<float> angle(0.0f, 6.2831853f), unit(0.0f, 1.0f);
    float sx = 0.0f, sy = 0.0f;
    for (int attempt = 0; attempt < 64; ++attempt) {
        const float a = angle(rng_);
        const float d = (arena.radius - 3.0f) * std::sqrt(unit(rng_));
        sx = d * std::cos(a);
        sy = d * std::sin(a);
        bool ok = true;
        for (const Pillar &p : arena.pillars) {
            ok = ok && std::hypot(sx - p.x, sy - p.y) > p.radius + r.playerRadius + 1.0f;
        }
        for (const auto &kv : players_) {
            ok = ok && std::hypot(sx - kv.second->x(), sy - kv.second->y()) > 4.0f * r.playerRadius;
        }
        if (ok) break;
    }
    move_ = MoveState{};
    move_.x = sx;
    move_.y = sy;
    move_.heading = std::atan2(sx, -sy) * 57.29578f;  // face the centre
    prevMove_ = move_;
    shownHeading_ = move_.heading;
    accumulator_ = 0.0;

    auto player = std::make_unique<ArenaPlayer>(cr_, *this);
    player->setIdentity(options_.name, options_.color);
    player->setLocalPose(move_.x, move_.y, move_.heading, 0.0f, 0.0f);
    local_ = static_cast<ArenaPlayer *>(cr_.createLocal(std::move(player), "DistributedArenaPlayer", net::kZoneArena));
    if (local_ == nullptr) {
        LOG_ERROR("could not create the player object");
        return;
    }
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
        if (p->isLocal()) continue;
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

void GameClient::simulate(double now, double dt, PlayerInput input) {
    const Rules &rules = game_->rules();
    const ArenaLayout &arena = game_->layout();
    if (options_.bot) {
        input = botInput(now);
    }

    constexpr double kStep = kPhysicsStep;
    accumulator_ = std::min(accumulator_ + dt, 0.25);  // never spiral after a hitch
    // The input as stepMovement() uses it, for the others' prediction.
    float heldX = input.x, heldY = input.y;
    const float held = std::hypot(heldX, heldY);
    if (held > 1e-3f) {
        heldX /= held;
        heldY /= held;
    } else {
        heldX = heldY = 0.0f;
    }
    bool dash = input.dash;
    while (accumulator_ >= kStep) {
        prevMove_ = move_;
        MoveInput mi{input.x, input.y, dash};
        dash = false;  // a press dashes once
        if (stepMovement(move_, mi, rules, arena, static_cast<float>(kStep))) {
            local_->sendDash();
            if (world_) world_->playerDashed(local_->doId());
            ++counters_.dashes;
            dashRecords_.push_back(DashRecord{now, {}, {}});
        }
        // Soft collisions with the (interpolated) other players.
        for (const auto &kv : players_) {
            if (!kv.second->isLocal()) {
                separateFrom(move_, kv.second->x(), kv.second->y(), 2.0f * rules.playerRadius);
            }
        }
        resolveCollisions(move_, rules, arena);
        accumulator_ -= kStep;
    }

    // What this player sees as a hit: touching someone while dashing (victims
    // are immune for the server's hitCooldown, 0.8 s, after being hit).
    if (!dashRecords_.empty() && now - dashRecords_.back().start <= rules.dashTime + 0.05) {
        DashRecord &dash = dashRecords_.back();
        const float touch = 2.0f * rules.playerRadius + 0.1f;
        for (const auto &kv : players_) {
            const ArenaPlayer *p = kv.second;
            const uint8_t slot = p->slot();
            if (p->isLocal() || slot == net::kNoSlot || dash.seen.test(slot)) continue;
            if (now - lastHitAt_[slot] < 0.8) continue;
            if (std::hypot(p->x() - move_.x, p->y() - move_.y) <= touch) {
                dash.seen.set(slot);
            }
        }
    }
    settleDashes(now, false);

    // Network gets the newest state; the screen an interpolated one.
    // Our body turns toward where we move as the others' bodies do.
    shownHeading_ = turnToward(shownHeading_, move_.heading, static_cast<float>(dt));
    local_->setLocalPose(move_.x, move_.y, shownHeading_, move_.vx, move_.vy);
    local_->setLocalInput(heldX, heldY, move_.dashLeft);
    local_->broadcastPose(now, viewDelay_.wire(), rules);
    counters_.positionSends = local_->positionSends();
    if (world_) {
        const float a = static_cast<float>(accumulator_ / kStep);
        world_->setPlayerPose(local_->doId(), prevMove_.x + (move_.x - prevMove_.x) * a,
                              prevMove_.y + (move_.y - prevMove_.y) * a, shownHeading_);
    }

    // Predict pickups: hide the orb at once, the server confirms (or not).
    if (game_->roundState() == RoundState::Playing) {
        const float reach = rules.playerRadius + rules.orbRadius;
        for (const auto &kv : game_->orbs()) {
            if (predictedOrbs_.count(kv.first)) continue;
            if (std::hypot(kv.second.x - move_.x, kv.second.y - move_.y) <= reach) {
                predictedOrbs_[kv.first] = now;
                if (world_) world_->setOrbHidden(kv.first, true);
                local_->sendSoon();  // so the AI confirms it without waiting for a heartbeat
            }
        }
    }
    for (auto it = predictedOrbs_.begin(); it != predictedOrbs_.end();) {
        if (now - it->second > 0.75) {  // never confirmed: show it again
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
    float tx = wanderX_, ty = wanderY_;
    float best = 1e9f;
    for (const auto &kv : game_->orbs()) {
        if (predictedOrbs_.count(kv.first)) continue;
        const float d = std::hypot(kv.second.x - move_.x, kv.second.y - move_.y) - (kv.second.kind ? 6.0f : 0.0f);
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
    float dx = tx - move_.x, dy = ty - move_.y;
    const float dist = std::hypot(dx, dy);
    if (dist > 0.01f) {
        dx /= dist;
        dy /= dist;
    }
    // Steer around pillars in the way.
    for (const Pillar &p : arena.pillars) {
        const float px = p.x - move_.x, py = p.y - move_.y;
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
    if (move_.dashCooldown <= 0.0f) {
        for (const auto &kv : players_) {
            const ArenaPlayer *p = kv.second;
            if (p->isLocal()) continue;
            const float ox = p->x() - move_.x, oy = p->y() - move_.y;
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
            row.local = kv.second->isLocal();
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
    return cd > 0.0f ? 1.0f - std::clamp(move_.dashCooldown / cd, 0.0f, 1.0f) : 1.0f;
}

// --------------------------------------------------------------- GameEvents

void GameClient::onGameGenerated(ArenaGame &game) {
    game_ = &game;
    if (world_) {
        world_->setArena(game.layout(), game.rules());
        world_->setOrbs(game.orbs());
    }
    if (phase_ == Phase::EnteringArena || phase_ == Phase::SyncingTime) {
        createLocalPlayer();
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
        if (s.player->isLocal()) localSlot_ = slot;
    }
}

void GameClient::onMove(uint8_t slot, const net::WireState &state) {
    Slot &s = slots_[slot];
    if (s.player != nullptr) [[likely]] {
        if (!s.player->isLocal()) {
            const double lateness = s.player->applyState(state, now_, viewDelay_.value(), rules());
            // Live samples only; cached ones of idle players can be minutes old.
            if (lateness >= 0.0 && lateness < 0.5) viewDelay_.addLateness(lateness);
        }
    } else if (s.doId != 0) {
        s.state = state;  // the player's generate has not arrived yet
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

void GameClient::onKnockback(float vx, float vy) {
    applyKnockback(move_, vx, vy);
    if (world_) world_->shake(0.4f);
}

void GameClient::onCorrectPosition(float x, float y) {
    ++counters_.corrections;
    LOG_WARN("server corrected our position to (%.1f, %.1f)", x, y);
    move_.x = x;
    move_.y = y;
    move_.vx = move_.vy = 0.0f;
    prevMove_ = move_;
    if (local_) {
        shownHeading_ = move_.heading;
        local_->setLocalPose(move_.x, move_.y, shownHeading_, 0.0f, 0.0f);
        local_->setLocalInput(0.0f, 0.0f, 0.0f);
        local_->broadcastPose(now_, viewDelay_.wire(), rules(), true);
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
        if (s.hasState && !player.isLocal()) player.applyState(s.state, now_, viewDelay_.value(), rules());
        s.hasState = false;
        break;
    }
    if (world_) {
        world_->addPlayer(player.doId(), player.name(), player.colorIndex(), player.isLocal());
        world_->setPlayerPose(player.doId(), player.x(), player.y(), player.h());
    }
}

void GameClient::onPlayerDisabled(ArenaPlayer &player) {
    players_.erase(player.doId());
    if (player.slot() != net::kNoSlot && slots_[player.slot()].player == &player) {
        slots_[player.slot()].player = nullptr;
    }
    if (world_) world_->removePlayer(player.doId());
    if (&player == local_) {
        local_ = nullptr;
    }
}

void GameClient::onPlayerDashed(ArenaPlayer &player) {
    if (world_) world_->playerDashed(player.doId());
}

} // namespace arena::game
