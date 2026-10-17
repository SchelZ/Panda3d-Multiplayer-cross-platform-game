// The phone's first screen, and its menu (the back button): pick a server
// found on the local network, or tap the address field and type an address
// or a host name on the phone's keyboard, and press Play. Drawn in aspect2d
// over the meadow; works with taps (touches).
#pragma once

#include "app/Platform.h"
#include "net/LanDiscovery.h"

#include "nodePath.h"
#include "pointerTo.h"
#include "textNode.h"

#include <string>
#include <utility>
#include <vector>

namespace arena::game {

class ConnectScreen {
public:
    ConnectScreen(NodePath aspect2d, const std::string &address, const std::string &playerName);
    ~ConnectScreen();
    ConnectScreen(const ConnectScreen &) = delete;
    ConnectScreen &operator=(const ConnectScreen &) = delete;

    void setWindow(int width, int height, float uiScale);
    void setVisible(bool visible);
    bool visible() const { return visible_; }

    void handle(const platform::Touch &touch);
    void setServers(const std::vector<net::LanDiscovery::Server> &servers);
    // A line for the player (empty: how the search for servers is going).
    void setMessage(const std::string &text);
    // The server of the game going on behind the menu ("" if none): Play
    // shows RESUME while the address is that one.
    void setCurrentServer(const std::string &address);

    // The address field: tapping it starts typing, and asks for the phone's
    // keyboard (each tap: the back button may have put it away); tapping
    // elsewhere, Play or a server ends it, and the keyboard goes. type() takes
    // what was typed: characters, '\b' to delete and '\n' for Play.
    bool editing() const { return editing_; }
    bool takeKeyboardRequest() { return std::exchange(keyboardRequest_, false); }
    void stopEditing() { editing_ = false; }
    void type(const std::string &keys);

    // After Play (or a found server) was tapped: the address to join, as
    // typed ("host" or "host:port").
    bool takeJoin(std::string &address);
    const std::string &address() const { return address_; }

    void update(double now);

    // Where the address field is (aspect2d units), e.g. for tests.
    LPoint2f fieldCenter() const { return LPoint2f(fieldX_, fieldZ_); }

    // "host[:port]" -> host and port (default 4400). False if malformed.
    static bool parseAddress(const std::string &text, std::string &host, uint16_t &port);
    // The same host (any case) and port, e.g. "10.0.0.5" and "10.0.0.5:4400".
    static bool sameServer(const std::string &a, const std::string &b);

private:
    struct Button {
        std::string id;
        float x = 0, z = 0, halfW = 0, halfH = 0;  // aspect2d units
        NodePath np, card;
        PT(TextNode) text;
        LColor color;
        bool enabled = true;
    };
    Button &addButton(const std::string &id, const std::string &label, const LColor &color, float textScale);
    void place(Button &b, float x, float z, float halfW, float halfH);
    Button *hit(float x, float z);
    bool onField(float x, float z) const;
    void press(const std::string &id);
    void layout();
    void refreshAddress();
    void refreshMessage();
    LPoint2f toAspect(float px, float py) const;

    NodePath root_, dim_, panel_;
    PT(TextNode) title_, subtitle_, field_, foundTitle_, message_, you_;
    NodePath titleNp_, subtitleNp_, fieldNp_, fieldCard_, foundTitleNp_, messageNp_, youNp_;
    std::vector<Button> buttons_;
    std::vector<net::LanDiscovery::Server> servers_;
    static constexpr int kServerRows = 3;

    std::string address_, join_, fieldText_, messageText_, current_;
    bool hasJoin_ = false;
    bool visible_ = true;
    bool editing_ = false;
    bool keyboardRequest_ = false;
    int width_ = 1280, height_ = 720;
    float aspect_ = 16.0f / 9.0f, scale_ = 1.0f;
    float fieldX_ = 0, fieldZ_ = 0, fieldHalfW_ = 0, fieldHalfH_ = 0, fieldTextWidth_ = 1;
    int pressedTouch_ = -1;
    std::string pressedId_;
    double now_ = 0.0;
};

} // namespace arena::game
