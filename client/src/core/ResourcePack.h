// The client's resources (DC files, shaders) ship in arena.pak: a Panda3D
// Multifile encrypted with ChaCha20 (built by tools/make_pack.py). It is
// decrypted in memory and mounted read-only in Panda's virtual file system,
// so everything loads it like plain files (/arena/dc/arena.dc, ...) and
// nothing readable is ever written to disk.
//
// The key is compiled into the client (CMake's ARENA_PACK_KEY, masked so it
// is not one string in the executable). That keeps the files from being read
// or edited casually; it cannot stop someone who takes the client apart, and
// does not need to: the server checks the DC hash and everything players send.
#pragma once

#include "filename.h"

#include <cstdint>
#include <string>

namespace arena::core {

inline constexpr const char *kPackMount = "/arena";

// Mounts `pak` at `mountPoint` with the built-in key. False, with `error`,
// if it is missing, damaged, or encrypted with another key.
bool mountPack(const Filename &pak, const Filename &mountPoint, std::string &error);

// The same with an explicit key (tests).
bool mountPack(const Filename &pak, const Filename &mountPoint, const uint8_t key[32], std::string &error);

// The same from a pack already read into memory (Android: from the APK's
// assets); `name` is for messages.
bool mountPackData(std::string data, const std::string &name, const Filename &mountPoint, std::string &error);

} // namespace arena::core
