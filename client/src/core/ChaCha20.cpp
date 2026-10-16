#include "core/ChaCha20.h"

#include <algorithm>
#include <cstring>

namespace arena::core {

namespace {

inline uint32_t rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

inline uint32_t load32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

inline void quarter(uint32_t *x, int a, int b, int c, int d) {
    x[a] += x[b]; x[d] = rotl(x[d] ^ x[a], 16);
    x[c] += x[d]; x[b] = rotl(x[b] ^ x[c], 12);
    x[a] += x[b]; x[d] = rotl(x[d] ^ x[a], 8);
    x[c] += x[d]; x[b] = rotl(x[b] ^ x[c], 7);
}

void block(const uint32_t state[16], uint8_t out[64]) {
    uint32_t x[16];
    std::memcpy(x, state, sizeof x);
    for (int i = 0; i < 10; ++i) {
        quarter(x, 0, 4, 8, 12);
        quarter(x, 1, 5, 9, 13);
        quarter(x, 2, 6, 10, 14);
        quarter(x, 3, 7, 11, 15);
        quarter(x, 0, 5, 10, 15);
        quarter(x, 1, 6, 11, 12);
        quarter(x, 2, 7, 8, 13);
        quarter(x, 3, 4, 9, 14);
    }
    for (int i = 0; i < 16; ++i) {
        const uint32_t v = x[i] + state[i];
        out[4 * i] = static_cast<uint8_t>(v);
        out[4 * i + 1] = static_cast<uint8_t>(v >> 8);
        out[4 * i + 2] = static_cast<uint8_t>(v >> 16);
        out[4 * i + 3] = static_cast<uint8_t>(v >> 24);
    }
}

} // namespace

void chacha20Xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t *data,
                 size_t size) {
    uint32_t state[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; ++i) state[4 + i] = load32(key + 4 * i);
    state[12] = counter;
    for (int i = 0; i < 3; ++i) state[13 + i] = load32(nonce + 4 * i);
    uint8_t stream[64];
    for (size_t offset = 0; offset < size; offset += 64) {
        block(state, stream);
        ++state[12];
        const size_t n = std::min<size_t>(64, size - offset);
        for (size_t i = 0; i < n; ++i) data[offset + i] ^= stream[i];
    }
    std::memset(stream, 0, sizeof stream);
}

} // namespace arena::core
