#include "net/LanDiscovery.h"

#include <algorithm>
#include <cstring>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace arena::net {

namespace {

// server/arena/discovery.py
constexpr char kProbeMagic[] = "PARENA?";
constexpr char kReplyMagic[] = "PARENA!";
constexpr uint8_t kVersion = 1;
constexpr size_t kProbeSize = 64;
constexpr size_t kHeaderSize = 7 + 1 + 4;
constexpr size_t kInfoSize = 2 + 1 + 1 + 1;
constexpr double kProbeInterval = 2.0;  // s
constexpr double kForgetAfter = 7.0;    // s without a reply

#ifdef _WIN32
using SocketHandle = SOCKET;
void closeSocket(SocketHandle s) { closesocket(s); }
#else
using SocketHandle = int;
void closeSocket(SocketHandle s) { close(s); }
#endif

uint32_t readU32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

} // namespace

LanDiscovery::LanDiscovery() {
    std::random_device rd;
    nonce_ = rd();
}

LanDiscovery::~LanDiscovery() { stop(); }

std::vector<uint8_t> LanDiscovery::makeProbe(uint32_t nonce) {
    std::vector<uint8_t> p(kProbeSize, 0);
    std::memcpy(p.data(), kProbeMagic, 7);
    p[7] = kVersion;
    for (int i = 0; i < 4; ++i) p[8 + i] = static_cast<uint8_t>(nonce >> (8 * i));
    return p;
}

bool LanDiscovery::parseReply(const uint8_t *data, size_t size, uint32_t nonce, Server &out) {
    if (size < kHeaderSize + kInfoSize || size > kProbeSize) return false;
    if (std::memcmp(data, kReplyMagic, 7) != 0 || data[7] != kVersion || readU32(data + 8) != nonce) {
        return false;
    }
    const uint8_t *info = data + kHeaderSize;
    out.port = static_cast<uint16_t>(info[0] | info[1] << 8);
    out.players = info[2];
    out.maxPlayers = info[3];
    const size_t nameLength = std::min<size_t>(info[4], size - kHeaderSize - kInfoSize);
    out.name.assign(reinterpret_cast<const char *>(info + kInfoSize), nameLength);
    for (char &c : out.name) {
        if (static_cast<unsigned char>(c) < 32) c = ' ';
    }
    return out.port != 0;
}

bool LanDiscovery::start() {
    if (running()) return true;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
    const SocketHandle s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    const BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&yes), sizeof(yes));
#else
    if (s < 0) return false;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    const int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
#endif
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    any.sin_port = 0;  // any port: the servers answer the sender
    if (bind(s, reinterpret_cast<sockaddr *>(&any), sizeof(any)) != 0) {
        closeSocket(s);
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }
    sock_ = static_cast<long long>(s);
    nextProbe_ = 0.0;
    return true;
}

void LanDiscovery::stop() {
    if (!running()) return;
    closeSocket(static_cast<SocketHandle>(sock_));
    sock_ = -1;
#ifdef _WIN32
    WSACleanup();
#endif
    servers_.clear();
}

void LanDiscovery::probe() {
    const SocketHandle s = static_cast<SocketHandle>(sock_);
    const std::vector<uint8_t> packet = makeProbe(nonce_);
    std::vector<uint32_t> targets{htonl(INADDR_BROADCAST)};
#ifndef _WIN32
    // 255.255.255.255 leaves through one interface only; also send to each
    // network's own broadcast address (Wi-Fi and Ethernet both, for example).
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs *i = list; i != nullptr; i = i->ifa_next) {
            if (i->ifa_addr == nullptr || i->ifa_addr->sa_family != AF_INET) continue;
            if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_BROADCAST)) continue;
            if (i->ifa_broadaddr == nullptr) continue;
            const uint32_t b = reinterpret_cast<sockaddr_in *>(i->ifa_broadaddr)->sin_addr.s_addr;
            if (std::find(targets.begin(), targets.end(), b) == targets.end()) targets.push_back(b);
        }
        freeifaddrs(list);
    }
#endif
    targets.push_back(htonl(INADDR_LOOPBACK));  // a server on this machine
    for (uint32_t addr : targets) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = addr;
        to.sin_port = htons(kDiscoveryPort);
        sendto(s, reinterpret_cast<const char *>(packet.data()), static_cast<int>(packet.size()), 0,
               reinterpret_cast<sockaddr *>(&to), sizeof(to));
    }
}

bool LanDiscovery::update(double now) {
    if (!running()) return false;
    if (now >= nextProbe_) {
        nextProbe_ = now + kProbeInterval;
        probe();
    }

    bool changed = false;
    const SocketHandle s = static_cast<SocketHandle>(sock_);
    for (int n = 0; n < 64; ++n) {
        uint8_t buf[256];
        sockaddr_in from{};
#ifdef _WIN32
        int fromLen = sizeof(from);
#else
        socklen_t fromLen = sizeof(from);
#endif
        const auto got = recvfrom(s, reinterpret_cast<char *>(buf), sizeof(buf), 0,
                                  reinterpret_cast<sockaddr *>(&from), &fromLen);
        if (got <= 0) break;  // nothing more (or an ICMP error from a probe)
        Server server;
        if (!parseReply(buf, static_cast<size_t>(got), nonce_, server)) continue;
        char text[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
        server.address = text;
        server.seenAt = now;
        // A server on this machine answers both on the LAN address and on
        // 127.0.0.1: keep one entry, preferring the LAN address.
        auto same = std::find_if(servers_.begin(), servers_.end(), [&](const Server &o) {
            return o.port == server.port && o.name == server.name &&
                   (o.address == server.address || o.address == "127.0.0.1" || server.address == "127.0.0.1");
        });
        if (same == servers_.end()) {
            servers_.push_back(server);
            changed = true;
        } else {
            const bool differs = same->players != server.players || same->maxPlayers != server.maxPlayers ||
                                 (same->address == "127.0.0.1" && server.address != "127.0.0.1");
            if (server.address == "127.0.0.1" && same->address != "127.0.0.1") server.address = same->address;
            *same = server;
            changed = changed || differs;
        }
    }

    const auto old = std::remove_if(servers_.begin(), servers_.end(),
                                    [&](const Server &o) { return now - o.seenAt > kForgetAfter; });
    if (old != servers_.end()) {
        servers_.erase(old, servers_.end());
        changed = true;
    }
    return changed;
}

} // namespace arena::net
