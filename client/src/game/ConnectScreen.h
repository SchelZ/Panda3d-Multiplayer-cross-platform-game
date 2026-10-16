// The first screen where there is no command line (Android, or --touch on a
// PC): pick a server found on the local network, or type an address on the
// on-screen keypad, and press Play. Drawn in aspect2d over the meadow; works
// with taps (touches) and with the mouse acting as a finger.
#pragma once

#include "app/Platform.h"
#include "net/LanDiscovery.h"

#include "nodePath.h"
#include "pointerTo.h"
#include "textNode.h"

#include <string>
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
    void setMessage(const std::string &text);

    // After Play (or a found server) was tapped: the address to join, as
    // typed ("host" or "host:port").
    bool takeJoin(std::string &address);
    const std::string &address() const { return address_; }

    void update(double now);

    // "host[:port]" -> host and port (default 4400). False if malformed.
    static bool parseAddress(const std::string &text, std::string &host, uint16_t &port);

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
    void press(const std::string &id);
    void layout();
    void refreshAddress();
    LPoint2f toAspect(float px, float py) const;

    NodePath root_, dim_, leftPanel_, rightPanel_;
    PT(TextNode) title_, subtitle_, field_, foundTitle_, message_, you_;
    NodePath titleNp_, subtitleNp_, fieldNp_, fieldCard_, foundTitleNp_, messageNp_, youNp_;
    std::vector<Button> buttons_;
    std::vector<net::LanDiscovery::Server> servers_;
    static constexpr int kServerRows = 3;

    std::string address_, join_, fieldText_;
    bool hasJoin_ = false;
    bool visible_ = true;
    int width_ = 1280, height_ = 720;
    float aspect_ = 16.0f / 9.0f, scale_ = 1.0f;
    int pressedTouch_ = -1;
    std::string pressedId_;
    double now_ = 0.0;
};

} // namespace arena::game
