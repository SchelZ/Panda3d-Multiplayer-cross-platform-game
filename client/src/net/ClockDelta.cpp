#include "net/ClockDelta.h"

#include "net/Protocol.h"

#include <algorithm>
#include <cmath>

namespace arena::net {

namespace {
int32_t signExtend16(int64_t t) {
    return static_cast<int32_t>(((t + 32768) & 0xffff) - 32768);
}
} // namespace

void ClockDelta::clear() {
    delta_ = 0.0;
    uncertainty_ = -1.0;
    lastResync_ = 0.0;
}

bool ClockDelta::resynchronize(double localTime, int64_t networkTime, double uncertainty,
                               bool trustNew) {
    const double d = localTime - static_cast<double>(networkTime) / kTicksPerSec;
    return newDelta(localTime, d, uncertainty, trustNew);
}

bool ClockDelta::newDelta(double localTime, double d, double uncertainty, bool trustNew) {
    if (synced()) {
        // Intersect the old window [delta +/- u] with the new one.
        const double low = std::max(delta_ - uncertainty_, d - uncertainty);
        const double high = std::min(delta_ + uncertainty_, d + uncertainty);
        if (low <= high) {
            d = (low + high) * 0.5;
            uncertainty = (high - low) * 0.5;
        } else if (!trustNew) {
            return false;  // windows disagree and the new one is not trusted
        }
    }
    delta_ = d;
    uncertainty_ = uncertainty;
    lastResync_ = localTime;
    return true;
}

double ClockDelta::networkToLocalTime(int64_t networkTime, double now, int bits) const {
    const int64_t ntime = static_cast<int64_t>(std::floor((now - delta_) * kTicksPerSec + 0.5));
    const int64_t diff = bits == 16 ? signExtend16(networkTime - ntime) : networkTime - ntime;
    return now + static_cast<double>(diff) / kTicksPerSec;
}

int32_t ClockDelta::localToNetworkTime(double localTime, int bits) const {
    const int64_t ntime = static_cast<int64_t>(std::floor((localTime - delta_) * kTicksPerSec + 0.5));
    return bits == 16 ? signExtend16(ntime) : static_cast<int32_t>(ntime);
}

int64_t ClockDelta::localToStep(double localTime) const {
    return static_cast<int64_t>(std::floor((localTime - delta_) * kStepsPerSec));
}

double ClockDelta::stepToLocalTime(int64_t step) const { return static_cast<double>(step) / kStepsPerSec + delta_; }

int64_t ClockDelta::resolveStep(uint16_t step16, double now) const {
    const int64_t near = localToStep(now);
    return near + signExtend16(static_cast<int64_t>(step16) - near);
}

} // namespace arena::net
