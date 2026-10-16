// C++ port of direct/distributed/ClockDelta.py: the offset between this
// machine's clock and the AI's "network time" (1/100 s ticks), so that
// timestamps in setSmXYH / setRound mean the same thing everywhere.
#pragma once

#include <cstdint>

namespace arena::net {

class ClockDelta {
public:
    static constexpr double kTicksPerSec = 100.0;  // ClockDelta.NetworkTimePrecision

    void clear();

    // A server timestamp observed at localTime, +/- uncertainty seconds.
    // Returns true if the measurement was used.
    bool resynchronize(double localTime, int64_t networkTime, double uncertainty,
                       bool trustNew = true);
    bool newDelta(double localTime, double delta, double uncertainty, bool trustNew);

    // networkTime is 16 or 32 bits wide (as in the DC file). 16-bit values
    // wrap every ~11 minutes, so they are resolved relative to `now`.
    double networkToLocalTime(int64_t networkTime, double now, int bits = 16) const;
    int32_t localToNetworkTime(double localTime, int bits = 16) const;

    double delta() const { return delta_; }
    bool synced() const { return uncertainty_ >= 0.0; }
    double uncertainty() const { return uncertainty_; }  // < 0 = never synced

private:
    double delta_ = 0.0;
    double uncertainty_ = -1.0;
    double lastResync_ = 0.0;
};

} // namespace arena::net
