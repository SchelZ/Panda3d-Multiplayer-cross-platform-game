#include "Test.h"

#include "game/ArenaGame.h"
#include "game/ArenaPlayer.h"
#include "net/ClientRepository.h"
#include "net/FieldIO.h"
#include "net/TimeManager.h"

#include "dcPacker.h"

#include <cstdio>
#include <string>

using namespace arena;

namespace {

bool loadDc(net::ClientRepository &cr) {
    std::string error;
    const std::string dir = ARENA_COMMON_DIR;
    const bool ok = cr.loadDcFiles({Filename::from_os_specific(dir + "/direct.dc"),
                                    Filename::from_os_specific(dir + "/arena.dc")},
                                   error);
    if (!ok) std::printf("    %s\n", error.c_str());
    return ok;
}

std::string hex(const DCPacker &p) {
    std::string out;
    char buf[3];
    for (size_t i = 0; i < p.get_length(); ++i) {
        std::snprintf(buf, sizeof buf, "%02x", static_cast<unsigned char>(p.get_data()[i]));
        out += buf;
    }
    return out;
}

} // namespace

ARENA_TEST(protocol_dc_files_match_client_code) {
    net::ClientRepository cr;
    CHECK(loadDc(cr));
    CHECK(cr.dcHash() != 0);
    const DCFile &dc = cr.dcFile();
    // Every field the C++ classes bind must exist with the expected name.
    CHECK(net::TimeManager::bindFields(*dc.get_class_by_name("TimeManager")));
    CHECK(game::ArenaGame::bindFields(*dc.get_class_by_name("DistributedArenaGame")));
    CHECK(game::ArenaPlayer::bindFields(*dc.get_class_by_name("DistributedArenaPlayer")));
}

ARENA_TEST(protocol_packs_bytes_identical_to_python) {
    net::ClientRepository cr;
    CHECK(loadDc(cr));
    const DCField *field = cr.dcFile().get_class_by_name("DistributedArenaGame")->get_field_by_name("setArena");
    // Python: DCPacker.packObject((40.0, [(1.5, -2.25, 1.0), (3, 4, 2)]))
    DCPacker p;
    p.begin_pack(field);
    p.push();
    net::FieldWriter w(p);
    w.f64(40.0).push();
    w.push().f64(1.5).f64(-2.25).f64(1.0).pop();
    w.push().f64(3.0).f64(4.0).f64(2.0).pop();
    w.pop();
    p.pop();
    CHECK(p.end_pack());
    CHECK(hex(p) == "90010c000f00eaff64001e002800c800");
}

ARENA_TEST(protocol_field_round_trip_and_validation) {
    net::ClientRepository cr;
    CHECK(loadDc(cr));
    const DCClass *player = cr.dcFile().get_class_by_name("DistributedArenaPlayer");
    const DCField *correct =
        cr.dcFile().get_class_by_name("DistributedArenaGame")->get_field_by_name("correctPosition");

    DCPacker p;
    p.begin_pack(correct);
    p.push();
    net::FieldWriter(p).f64(12.345).f64(-7.891);
    p.pop();
    CHECK(p.end_pack());
    CHECK(p.get_length() == 4);  // two int16 / 100
    CHECK(correct->validate_ranges(p.get_bytes()));

    DCPacker u;
    u.set_unpack_data(p.get_bytes());
    u.begin_unpack(correct);
    u.push();
    net::FieldReader r(u);
    CHECK_NEAR(r.f64(), 12.35, 1e-6);  // centimetres
    CHECK_NEAR(r.f64(), -7.89, 1e-6);
    u.pop();
    CHECK(u.end_unpack());

    // Truncated or padded data is rejected.
    vector_uchar bytes = p.get_bytes();
    bytes.pop_back();
    CHECK(!correct->validate_ranges(bytes));
    bytes.push_back(0);
    bytes.push_back(0);
    CHECK(!correct->validate_ranges(bytes));

    // string(1-16): a 17-byte name cannot even be packed.
    const DCField *setName = player->get_field_by_name("setName");
    DCPacker n;
    n.begin_pack(setName);
    n.push();
    n.pack_string(std::string(17, 'x'));
    n.pop();
    CHECK(!n.end_pack());
}

ARENA_TEST(protocol_sanitizes_names) {
    using game::ArenaPlayer;
    CHECK(ArenaPlayer::sanitizeName("  Bob  ") == "Bob");
    CHECK(ArenaPlayer::sanitizeName("a\nb\tc\x7f") == "abc");
    CHECK(ArenaPlayer::sanitizeName("") == "Player");
    CHECK(ArenaPlayer::sanitizeName(std::string(40, 'z')).size() == 16);
    // 8 two-byte characters = 16 bytes fit; the 9th is cut whole, not halved.
    const std::string e = "\xc3\xa9";
    std::string nine;
    for (int i = 0; i < 9; ++i) nine += e;
    CHECK(ArenaPlayer::sanitizeName(nine).size() == 16);
}
