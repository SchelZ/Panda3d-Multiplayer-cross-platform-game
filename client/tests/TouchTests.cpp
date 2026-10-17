// Touch controls, the connect screen's address parsing, and LAN discovery
// packets: the phone-facing logic that has no window to test it in.
#include "Test.h"

#include "core/Settings.h"
#include "game/ConnectScreen.h"
#include "game/TouchControls.h"
#include "net/LanDiscovery.h"

#include "nodePath.h"

#include <cstdio>
#include <string>

using namespace arena;
using Kind = platform::Touch::Kind;

// The tests use a 2000x1000 window (aspect 2), where one aspect2d unit is
// 500 pixels: pixel (x, y) is aspect2d ((x / 1000 - 1) * 2, 1 - y / 500).
namespace {

struct Px {
    float x, y;
};

Px pixels(const LPoint2f &p) { return {(p[0] / 2.0f + 1.0f) * 1000.0f, (1.0f - p[1]) * 500.0f}; }

} // namespace

ARENA_TEST(touch_stick_gives_full_speed_directions) {
    NodePath root("aspect2d");
    game::TouchControls tc(root);
    tc.setWindow(2000, 1000, 1.0f);
    const Px c = pixels(tc.stickCenter());
    tc.handle({Kind::Down, 7, c.x, c.y});
    auto out = tc.take();
    CHECK(out.x == 0.0f && out.y == 0.0f);  // on the centre: not moving
    CHECK(!out.dash);
    tc.handle({Kind::Move, 7, c.x + 3, c.y - 2});  // inside the dead zone
    out = tc.take();
    CHECK(out.x == 0.0f && out.y == 0.0f);
    tc.handle({Kind::Move, 7, c.x + 30, c.y});  // a little to the right: full speed right
    out = tc.take();
    CHECK_NEAR(out.x, 1.0, 1e-5);
    CHECK_NEAR(out.y, 0.0, 1e-5);
    tc.handle({Kind::Move, 7, c.x, c.y - 40});  // up (pixels grow downward)
    out = tc.take();
    CHECK_NEAR(out.x, 0.0, 1e-5);
    CHECK_NEAR(out.y, 1.0, 1e-5);
    tc.handle({Kind::Up, 7, c.x, c.y - 40});
    out = tc.take();
    CHECK(out.x == 0.0f && out.y == 0.0f);
}

ARENA_TEST(touch_stick_stays_in_place) {
    NodePath root("aspect2d");
    game::TouchControls tc(root);
    tc.setWindow(2000, 1000, 1.0f);
    const Px c = pixels(tc.stickCenter());
    tc.handle({Kind::Down, 1, c.x + 60, c.y});   // near the stick: grabs it
    tc.handle({Kind::Move, 1, c.x + 600, c.y});  // far to the right: the ring does not follow
    tc.handle({Kind::Move, 1, c.x - 50, c.y});   // so just left of its centre is left
    CHECK_NEAR(tc.take().x, -1.0, 1e-5);
    CHECK(tc.stickCenter() == root.find("**/stick").get_pos().get_xz());
    tc.handle({Kind::Up, 1, c.x - 50, c.y});

    // A thumb that lands far from the stick does not move the player.
    tc.handle({Kind::Down, 2, 600, 300});
    tc.handle({Kind::Move, 2, 700, 300});
    const auto out = tc.take();
    CHECK(out.x == 0.0f && out.y == 0.0f && !out.dash);
}

ARENA_TEST(touch_dash_only_on_the_button) {
    NodePath root("aspect2d");
    game::TouchControls tc(root);
    tc.setWindow(2000, 1000, 1.0f);
    const Px d = pixels(tc.dashCenter());
    tc.handle({Kind::Down, 2, d.x, d.y});
    CHECK(tc.take().dash);
    CHECK(!tc.take().dash);  // once
    tc.handle({Kind::Up, 2, d.x, d.y});
    tc.handle({Kind::Down, 3, d.x - 150, d.y});  // beside the button (radius 80 px)
    CHECK(!tc.take().dash);
    tc.handle({Kind::Up, 3, d.x - 150, d.y});
    tc.handle({Kind::Down, 4, d.x, d.y - 230});  // above it
    CHECK(!tc.take().dash);
    tc.handle({Kind::Up, 4, d.x, d.y - 230});
    tc.handle({Kind::Down, 5, d.x + 56, d.y + 56});  // on its edge
    CHECK(tc.take().dash);
    tc.handle({Kind::Up, 5, d.x + 56, d.y + 56});

    // The stick and a dash at the same time.
    const Px c = pixels(tc.stickCenter());
    tc.handle({Kind::Down, 6, c.x, c.y});
    tc.handle({Kind::Move, 6, c.x, c.y + 50});
    tc.handle({Kind::Down, 7, d.x, d.y});
    const auto out = tc.take();
    CHECK(out.dash);
    CHECK_NEAR(out.y, -1.0, 1e-5);
}

ARENA_TEST(touch_two_fingers_on_the_right_pinch_zoom) {
    NodePath root("aspect2d");
    game::TouchControls tc(root);
    tc.setWindow(2000, 1000, 1.0f);
    tc.handle({Kind::Down, 1, 1200, 300});
    tc.handle({Kind::Down, 2, 1400, 300});
    tc.handle({Kind::Move, 2, 1600, 300});  // spread: closer
    auto out = tc.take();
    CHECK(!out.dash);
    CHECK(out.zoomSteps > 1.0f);
    tc.handle({Kind::Move, 2, 1300, 300});  // pinch: farther
    out = tc.take();
    CHECK(out.zoomSteps < -1.0f);
    tc.handle({Kind::Up, 2, 1300, 300});
    tc.handle({Kind::Move, 1, 1100, 300});  // one finger left: nothing
    tc.handle({Kind::Up, 1, 1100, 300});
    out = tc.take();
    CHECK(!out.dash && out.zoomSteps == 0.0f);
}

ARENA_TEST(touch_status_corner_toggles_stats) {
    NodePath root("aspect2d");
    game::TouchControls tc(root);
    tc.setWindow(2000, 1000, 1.0f);
    tc.handle({Kind::Down, 1, 40, 20});
    tc.handle({Kind::Up, 1, 42, 22});
    const auto out = tc.take();
    CHECK(out.toggleStats);
    CHECK(out.x == 0.0f && out.y == 0.0f && !out.dash);
}

ARENA_TEST(connect_screen_parses_addresses) {
    std::string host;
    uint16_t port = 0;
    CHECK(game::ConnectScreen::parseAddress("192.168.1.20", host, port));
    CHECK(host == "192.168.1.20" && port == 4400);
    CHECK(game::ConnectScreen::parseAddress("10.0.0.5:5000", host, port));
    CHECK(host == "10.0.0.5" && port == 5000);
    CHECK(game::ConnectScreen::parseAddress(" arena.example.com ", host, port));
    CHECK(host == "arena.example.com");
    CHECK(!game::ConnectScreen::parseAddress("", host, port));
    CHECK(!game::ConnectScreen::parseAddress("1.2.3.4:", host, port));
    CHECK(!game::ConnectScreen::parseAddress("1.2.3.4:0", host, port));
    CHECK(!game::ConnectScreen::parseAddress("1.2.3.4:70000", host, port));
    CHECK(!game::ConnectScreen::parseAddress("1.2..3", host, port));
    CHECK(!game::ConnectScreen::parseAddress(".1.2.3", host, port));
    CHECK(!game::ConnectScreen::parseAddress("1:2:3", host, port));
}

ARENA_TEST(connect_screen_types_on_the_phone_keyboard) {
    NodePath root("aspect2d");
    game::ConnectScreen screen(root, "", "Tester");
    screen.setWindow(2000, 1000, 1.0f);
    CHECK(!screen.editing());
    CHECK(!screen.takeKeyboardRequest());

    // Tapping the field starts typing and asks for the keyboard (once).
    const Px f = pixels(screen.fieldCenter());
    screen.handle({Kind::Down, 1, f.x, f.y});
    screen.handle({Kind::Up, 1, f.x, f.y});
    CHECK(screen.editing());
    CHECK(screen.takeKeyboardRequest());
    CHECK(!screen.takeKeyboardRequest());

    // Host names in any case, one port, delete; spaces and symbols dropped.
    screen.type("Arena.Example.com:50011\b\b0");
    CHECK(screen.address() == "arena.example.com:5000");
    screen.type(" /:");
    CHECK(screen.address() == "arena.example.com:5000");
    std::string address;
    CHECK(!screen.takeJoin(address));

    // Enter is Play: join, and the typing is over.
    screen.type("\n");
    CHECK(screen.takeJoin(address));
    CHECK(address == "arena.example.com:5000");
    CHECK(!screen.editing());

    // Not an address: no join. A tap away from the field ends the typing.
    screen.handle({Kind::Down, 2, f.x, f.y});
    screen.handle({Kind::Up, 2, f.x, f.y});
    CHECK(screen.takeKeyboardRequest());
    for (int i = 0; i < 30; ++i) screen.type("\b");
    CHECK(screen.address().empty());
    screen.type("a..b\n");
    CHECK(!screen.takeJoin(address));
    screen.handle({Kind::Down, 3, f.x, f.y});
    screen.handle({Kind::Up, 3, f.x, f.y});
    CHECK(screen.editing());
    screen.handle({Kind::Down, 4, 1000, 950});  // the bottom of the screen
    CHECK(!screen.editing());
}

ARENA_TEST(connect_screen_resumes_the_current_server) {
    CHECK(game::ConnectScreen::sameServer("10.0.0.5", "10.0.0.5:4400"));
    CHECK(game::ConnectScreen::sameServer("Arena.Example.com:4401", "arena.example.com:4401"));
    CHECK(!game::ConnectScreen::sameServer("10.0.0.5", "10.0.0.5:4401"));
    CHECK(!game::ConnectScreen::sameServer("10.0.0.5", "10.0.0.6"));
    CHECK(!game::ConnectScreen::sameServer("", ""));

    NodePath root("aspect2d");
    game::ConnectScreen screen(root, "192.168.1.20", "Tester");
    TextNode *label = DCAST(TextNode, root.find("**/button-play/label").node());
    CHECK(label->get_text() == "PLAY");
    screen.setCurrentServer("192.168.1.20:4400");  // the game behind the menu
    CHECK(label->get_text() == "RESUME");
    screen.type("\b\b1");                           // another server
    CHECK(label->get_text() == "PLAY");
    screen.type("\b20");
    CHECK(label->get_text() == "RESUME");
    std::string address;
    screen.type("\n");
    CHECK(screen.takeJoin(address));  // the app sees it is the same one and resumes
    CHECK(game::ConnectScreen::sameServer(address, "192.168.1.20"));
    screen.setCurrentServer("");
    CHECK(label->get_text() == "PLAY");

    // The app's message stays while servers come and go.
    TextNode *message = DCAST(TextNode, root.find("**/message").node());
    screen.setMessage("Still playing");
    net::LanDiscovery::Server srv;
    srv.address = "10.0.0.9";
    srv.port = 4400;
    srv.name = "Meadow";
    screen.setServers({srv});
    CHECK(message->get_text() == "Still playing");
    screen.setMessage("");
    screen.setServers({});
    CHECK(message->get_text() == "Looking for servers on this network...");
}

ARENA_TEST(lan_discovery_packets_match_the_server) {
    // server/arena/discovery.py: 64-byte probe, magic + version + nonce.
    const auto probe = net::LanDiscovery::makeProbe(0x11223344u);
    CHECK(probe.size() == 64);
    CHECK(std::string(probe.begin(), probe.begin() + 7) == "PARENA?");
    CHECK(probe[7] == 1);
    CHECK(probe[8] == 0x44 && probe[11] == 0x11);

    // A reply as discovery.makeReply(0x11223344, 4401, 3, 16, 'Meadow') builds it.
    const uint8_t reply[] = {'P', 'A', 'R', 'E', 'N', 'A', '!', 1, 0x44, 0x33, 0x22, 0x11,
                             0x31, 0x11, 3, 16, 6, 'M', 'e', 'a', 'd', 'o', 'w'};
    net::LanDiscovery::Server s;
    CHECK(net::LanDiscovery::parseReply(reply, sizeof reply, 0x11223344u, s));
    CHECK(s.port == 4401 && s.players == 3 && s.maxPlayers == 16 && s.name == "Meadow");
    CHECK(!net::LanDiscovery::parseReply(reply, sizeof reply, 0x11223345u, s));  // someone else's
    CHECK(!net::LanDiscovery::parseReply(reply, 10, 0x11223344u, s));            // short
}

ARENA_TEST(settings_survive_a_round_trip) {
    char path[] = "arena-settings-test.txt";
    {
        core::Settings s(path);
        s.set("server", "192.168.1.20:4400");
        s.set("name", "Panda\nwith a newline");
        CHECK(s.save());
    }
    core::Settings t(path);
    CHECK(t.load());
    CHECK(t.get("server") == "192.168.1.20:4400");
    CHECK(t.get("name") == "Pandawith a newline");
    CHECK(t.get("missing", "x") == "x");
    std::remove(path);
}
