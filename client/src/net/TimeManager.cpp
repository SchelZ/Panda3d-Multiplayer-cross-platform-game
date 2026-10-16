#include "net/TimeManager.h"

#include "core/Log.h"
#include "net/ClientRepository.h"

#include "clockObject.h"

namespace arena::net {

namespace {
FieldTable<TimeManager> g_fields;
const DCField *g_requestServerTime = nullptr;

double realTime() { return ClockObject::get_global_clock()->get_real_time(); }
} // namespace

bool TimeManager::bindFields(const DCClass &dclass) {
    g_requestServerTime = g_fields.field(dclass, "requestServerTime");
    g_fields.bind(dclass, "serverTime", &TimeManager::serverTime);
    return g_fields.missing().empty();
}

void TimeManager::receiveField(const DCField &field, FieldReader &in) {
    g_fields.receive(*this, field, in);
}

void TimeManager::announceGenerate() {
    cr_.setTimeManager(this);
    synchronize("TimeManager generated");
}

void TimeManager::disable() {
    if (cr_.timeManager() == this) {
        cr_.setTimeManager(nullptr);
    }
}

bool TimeManager::synchronize(const char *reason) {
    const double now = realTime();
    if (waiting_ || now - lastAttempt_ < 1.0) {
        return false;
    }
    LOG_DEBUG("clock sync: %s", reason);
    context_ = nextContext_;
    nextContext_ = (nextContext_ + 1) & 255;
    attempts_ = 0;
    lastAttempt_ = now;
    sendRequest();
    return true;
}

void TimeManager::sendRequest() {
    start_ = realTime();
    waiting_ = true;
    const unsigned ctx = static_cast<unsigned>(context_);
    cr_.sendUpdate(*this, g_requestServerTime, [ctx](FieldWriter &out) { out.u32(ctx); });
}

void TimeManager::update() {
    const double now = realTime();
    if (waiting_ && now - start_ > requestTimeout) {
        waiting_ = false;  // reply lost or server busy: try again
        lastAttempt_ = -1e9;
        synchronize("request timed out");
    } else if (!waiting_ && synced_ && now - lastSync_ > resyncInterval) {
        synchronize("periodic resync");
    }
}

void TimeManager::serverTime(FieldReader &in) {
    const int context = static_cast<int>(in.u32());
    const int32_t timestamp = in.i32();
    const double end = realTime();
    if (!waiting_ || context != context_) {
        return;  // reply to an older request
    }
    waiting_ = false;
    ++attempts_;
    lastRoundTrip_ = end - start_;

    // Same estimate as TimeManager.serverTime(): the server read its clock
    // somewhere in the round trip; assume the middle.
    ClockDelta &clock = cr_.clockDelta();
    clock.resynchronize((start_ + end) * 0.5, timestamp, (end - start_) * 0.5);

    if (clock.uncertainty() > maxUncertainty && attempts_ < maxAttempts) {
        sendRequest();  // jittery round trip: measure again, keep the intersection
        return;
    }
    lastSync_ = end;
    LOG_INFO("clock synced: round trip %.1f ms, uncertainty +/- %.1f ms", lastRoundTrip_ * 1000.0,
             clock.uncertainty() * 1000.0);
    if (!synced_) {
        synced_ = true;
        if (onSynced) onSynced();
    }
}

} // namespace arena::net
