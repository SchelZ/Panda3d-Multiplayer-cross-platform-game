#include "Test.h"

#include "core/ChaCha20.h"
#include "core/ResourcePack.h"

#include "virtualFileSystem.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace arena::core;

namespace {

std::string hex(const uint8_t *data, size_t size) {
    std::string out;
    char b[3];
    for (size_t i = 0; i < size; ++i) {
        std::snprintf(b, sizeof b, "%02x", data[i]);
        out += b;
    }
    return out;
}

void unhex(const char *text, uint8_t *out, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        unsigned v = 0;
        std::sscanf(text + 2 * i, "%2x", &v);
        out[i] = static_cast<uint8_t>(v);
    }
}

std::string readVfs(const Filename &f) {
    std::string data;
    VirtualFileSystem::get_global_ptr()->read_file(f, data, false);
    return data;
}

} // namespace

ARENA_TEST(chacha20_matches_rfc8439) {
    // RFC 8439 section 2.4.2: key 00..1f, nonce 000000000000004a00000000, counter 1.
    uint8_t key[32], nonce[12] = {0, 0, 0, 0, 0, 0, 0, 0x4a, 0, 0, 0, 0};
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i);
    char text[] = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for "
                  "the future, sunscreen would be it.";
    const size_t n = std::strlen(text);
    auto *data = reinterpret_cast<uint8_t *>(text);
    chacha20Xor(key, nonce, 1, data, n);
    CHECK(hex(data, 16) == "6e2e359a2568f98041ba0728dd0d6981");
    CHECK(hex(data + n - 8, 8) == "8eedf2785e42874d");
    chacha20Xor(key, nonce, 1, data, n);  // the same operation decrypts
    CHECK(std::string(text, 7) == "Ladies ");
}

ARENA_TEST(resource_pack_mounts_and_reads_like_files) {
    const Filename pak = Filename::from_os_specific(ARENA_PACK_FILE);
    if (!VirtualFileSystem::get_global_ptr()->exists(pak)) {
        std::printf("    (no arena.pak: built without Python, skipped)\n");
        return;
    }
    std::string error;
    // The key compiled into the client (masked) is the build's ARENA_PACK_KEY.
    CHECK(mountPack(pak, Filename("/pack-test"), error));
    const std::string packed = readVfs(Filename("/pack-test/dc/arena.dc"));
    const std::string plain = readVfs(Filename::from_os_specific(ARENA_COMMON_DIR "/arena.dc"));
    CHECK(!plain.empty() && packed == plain);
    CHECK(!readVfs(Filename("/pack-test/dc/direct.dc")).empty());

    uint8_t key[32];
    unhex(ARENA_PACK_KEY, key, sizeof key);
    CHECK(mountPack(pak, Filename("/pack-test-2"), key, error));
    key[0] ^= 1;  // any other key is refused, not mounted as garbage
    CHECK(!mountPack(pak, Filename("/pack-test-3"), key, error));
    CHECK(error.find("another key") != std::string::npos);
    CHECK(!VirtualFileSystem::get_global_ptr()->exists(Filename("/pack-test-3/dc/arena.dc")));
    // And the pack itself is not readable: no DC text in it.
    CHECK(readVfs(pak).find("dclass") == std::string::npos);
}
