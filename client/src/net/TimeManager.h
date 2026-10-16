// C++ port of direct/distributed/TimeManager.py (dclass TimeManager in
// direct.dc): measures the round trip to the AI and feeds ClockDelta.
#pragma once

#include "net/DistributedObject.h"

#include <functional>

namespace arena::net {

class TimeManager final : public DistributedObject {
public:
    explicit TimeManager(ClientRepository &cr) : DistributedObject(cr) {}

    static bool bindFields(const DCClass &dclass);

    void receiveField(const DCField &field, FieldReader &in) override;
    bool packField(const DCField &, FieldWriter &) const override { return false; }
    void announceGenerate() override;
    void disable() override;

    // Starts a sync (retried until the uncertainty is small enough).
    bool synchronize(const char *reason);
    // Call every frame: retries lost requests and resyncs periodically.
    void update();

    bool synced() const { return synced_; }
    double lastRoundTrip() const { return lastRoundTrip_; }

    std::function<void()> onSynced;  // first successful sync

    double resyncInterval = 300.0;  // s; TimeManager.py uses time-manager-freq (1800)
    double maxUncertainty = 0.05;   // s; TimeManager.py: time-manager-max-uncertainty (1)
    int maxAttempts = 5;
    double requestTimeout = 3.0;

private:
    void serverTime(FieldReader &in);
    void sendRequest();

    int context_ = -1;
    int nextContext_ = 0;
    int attempts_ = 0;
    double start_ = 0.0;
    double lastAttempt_ = -1e9;
    double lastSync_ = 0.0;
    double lastRoundTrip_ = 0.0;
    bool waiting_ = false;
    bool synced_ = false;
};

} // namespace arena::net
