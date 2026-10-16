// Finds Panda Arena servers on the local network: broadcasts a probe every
// couple of seconds and collects the answers (server/arena/discovery.py has
// the packet layout). Plain non-blocking UDP; update() never waits.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace arena::net {

inline constexpr uint16_t kDiscoveryPort = 4400;  // = the default server port

class LanDiscovery {
public:
    struct Server {
        std::string address;  // as the reply came from
        uint16_t port = 0;    // the server's TCP port
        std::string name;
        int players = 0, maxPlayers = 0;
        double seenAt = 0.0;
    };

    LanDiscovery();
    ~LanDiscovery();
    LanDiscovery(const LanDiscovery &) = delete;
    LanDiscovery &operator=(const LanDiscovery &) = delete;

    bool start();  // false if no UDP socket could be opened
    void stop();
    bool running() const { return sock_ >= 0; }

    // Probes (every kProbeInterval) and reads replies; servers not heard from
    // for a while are dropped. Returns true if the list changed.
    bool update(double now);
    const std::vector<Server> &servers() const { return servers_; }

    // Packets (exposed for the unit tests).
    static std::vector<uint8_t> makeProbe(uint32_t nonce);
    static bool parseReply(const uint8_t *data, size_t size, uint32_t nonce, Server &out);

private:
    void probe();

    long long sock_ = -1;  // SOCKET on Windows, int elsewhere
    uint32_t nonce_ = 0;
    double nextProbe_ = 0.0;
    std::vector<Server> servers_;
};

} // namespace arena::net
