// SERVER_FRAME decoding (net/Frame.h) against frames written by the server's
// encoder (server/arena/frame.py), and garbage refused.
#include "Test.h"

#include "net/Frame.h"

#include <cmath>
#include <random>
#include <string>
#include <vector>

using namespace arena::net;

namespace {

std::vector<uint8_t> fromHex(const char *hex) {
    std::vector<uint8_t> out;
    for (const char *p = hex; p[0] && p[1]; p += 2) {
        out.push_back(static_cast<uint8_t>(std::stoi(std::string(p, 2), nullptr, 16)));
    }
    return out;
}

bool same(const WireState &s, int x, int y, int vx, int vy, int flags, int dir, int heading, int age) {
    return s.x == x && s.y == y && s.vx == vx && s.vy == vy && s.flags == flags && s.dir == dir &&
           s.heading == heading && s.age == age;
}

} // namespace

ARENA_TEST(frame_decodes_what_the_server_wrote) {
    // Written by frame.py: three frames to one client, in order.
    // 1: sent 65530; slot 3 (1234, -5000, 36, -4, input, dir 10, age 3) and
    //    slot 7 (-200, 300, 1, 0, standing: heading 77, age 5), in full;
    //    an orb spawned (513 at -12.3, 25.1, gold), orb 512 collected by 7
    //    (score 42), 3 hit 7 (scores 300, 2).
    // 2: sent 65533 (coded +3); both coded against frame 1 (slot 7 dashes,
    //    dir 200, age 4); SELF packSelf(9, 65530, 12.34, -7.89, 23.99, -0.01,
    //    -90, 0.212, 1.6, 0, 0, 15).
    // 3: sent 4 (coded +7, wrapping); slot 3 knocked back to a stop;
    //    SELF packSelf(10, 1000, ...) with its step in full (too old for an age).
    const char *hex[] = {
        "f4ff0f684081845f9a008ffd010a020070fe59029a2c40a0f07f1f6000040e54003a70c012200000",
        "e5c0c682fcc14380fc17006207c08401698275feaf84ff7f00e0d400810cf000",
        "ed40ad57f0b20001008450e11f7d800c0019000000000000845e014100",
    };
    FrameDecoder d;
    std::vector<uint8_t> f = fromHex(hex[0]);
    CHECK(d.decode(f.data(), f.size()));
    CHECK(d.sent() == 65530);
    CHECK(d.samples().size() == 2);
    CHECK(d.samples()[0].slot == 3 && same(d.samples()[0].state, 1234, -5000, 36, -4, kFlagInput, 10, 187, 3));
    CHECK(d.samples()[1].slot == 7 && same(d.samples()[1].state, -200, 300, 1, 0, 0, 0, 77, 5));
    CHECK(d.events().size() == 3);
    const FrameEvent &spawned = d.events()[0];
    CHECK(spawned.kind == kEventOrbSpawned && spawned.orbId == 513 && spawned.x == -123 && spawned.y == 251 &&
          spawned.orbKind == 1);
    const FrameEvent &taken = d.events()[1];
    CHECK(taken.kind == kEventOrbCollected && taken.orbId == 512 && taken.slot == 7 && taken.score == 42);
    const FrameEvent &hit = d.events()[2];
    CHECK(hit.kind == kEventPlayerHit && hit.slot == 3 && hit.victim == 7 && hit.score == 300 &&
          hit.victimScore == 2);
    CHECK(d.self() == nullptr);

    f = fromHex(hex[1]);
    CHECK(d.decode(f.data(), f.size()));
    CHECK(d.sent() == 65533);
    CHECK(d.samples().size() == 2 && d.events().empty());
    CHECK(same(d.samples()[0].state, 1260, -5004, 34, -2, kFlagInput, 10, 190, 3));
    CHECK(same(d.samples()[1].state, -200, 300, 96, 0, kFlagInput | kFlagDash, 200, 192, 4));
    const SelfState *m = d.self();
    CHECK(m != nullptr);
    if (m != nullptr) {
        CHECK(m->ack == 9 && m->step == 65530 && m->flags == 0 && m->knowDelay == 15);
        CHECK_NEAR(m->x, 12.34, 1e-5);
        CHECK_NEAR(m->y, -7.89, 1e-5);
        CHECK_NEAR(m->vx, 23.99, 1e-5);
        CHECK_NEAR(m->vy, -0.01, 1e-6);
        CHECK_NEAR(m->heading, -90.0, 1e-4);
        CHECK_NEAR(m->dashLeft, 0.212, 1e-6);
        CHECK_NEAR(m->dashCooldown, 1.6, 1e-6);
        CHECK(m->stun == 0.0f);
    }

    f = fromHex(hex[2]);
    CHECK(d.decode(f.data(), f.size()));
    CHECK(d.sent() == 4);
    CHECK(d.samples().size() == 1 && same(d.samples()[0].state, 2000, -5004, 0, 0, kFlagKnock, 0, 33, 3));
    m = d.self();
    CHECK(m != nullptr && m->ack == 10 && m->step == 1000 && m->flags == 1 && m->knowDelay == 16);
    if (m != nullptr) {
        CHECK_NEAR(m->heading, 45.0, 1e-2);
        CHECK_NEAR(m->stun, 0.35, 1e-6);
    }

    // Without frame 1, frame 2 cannot be read: it is refused, not misread.
    FrameDecoder fresh;
    f = fromHex(hex[1]);
    CHECK(!fresh.decode(f.data(), f.size()));
    // And a slot that changed hands needs a full sample again.
    FrameDecoder again;
    f = fromHex(hex[0]);
    CHECK(again.decode(f.data(), f.size()));
    again.forget(7);
    f = fromHex(hex[1]);
    CHECK(!again.decode(f.data(), f.size()));
}

ARENA_TEST(frame_prediction_matches_python) {
    CHECK(predictPos(0, 4, 4, 120) == 100);  // 1 m/s for 1 s
    CHECK(predictPos(0, -4, -4, 120) == -100);
    CHECK(predictPos(0, 1, 0, 1) == 0);
    CHECK(predictPos(0, -4, -4, 1) == -1);  // -0.83 cm rounds to -1 (floor of n + half, as Python)
    CHECK(predictPos(0, -1, 0, 1) == 0);
    CHECK(predictPos(0, 127, 127, 1000000) == predictPos(0, 127, 127, 240));
    CHECK(velocityHeading(0, 10) == 0);
    CHECK(velocityHeading(-10, 0) == 64);
    CHECK(velocityHeading(10, 0) == 192);
}

ARENA_TEST(frame_refuses_garbage) {
    std::mt19937 rng(5);
    int refused = 0;
    for (int i = 0; i < 2000; ++i) {
        std::vector<uint8_t> data(rng() % 41);
        for (uint8_t &b : data) b = static_cast<uint8_t>(rng());
        FrameDecoder d;
        refused += d.decode(data.data(), data.size()) ? 0 : 1;
    }
    CHECK(refused > 1500);
}
