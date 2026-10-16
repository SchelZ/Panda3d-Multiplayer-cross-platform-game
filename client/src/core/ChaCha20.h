// ChaCha20 stream cipher (RFC 8439), for arena.pak (tools/make_pack.py has
// the same code in Python). Encrypting and decrypting are the same XOR.
#pragma once

#include <cstddef>
#include <cstdint>

namespace arena::core {

// XORs `size` bytes at `data` with the ChaCha20 key stream of (key, nonce),
// starting at block `counter`.
void chacha20Xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t *data,
                 size_t size);

} // namespace arena::core
