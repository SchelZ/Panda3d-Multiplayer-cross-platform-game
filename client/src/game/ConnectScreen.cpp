#include "game/ConnectScreen.h"

#include "cardMaker.h"
#include "transparencyAttrib.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace arena::game {

namespace {

constexpr size_t kMaxAddress = 100;
const LColor kPlayColor(0.32f, 0.78f, 0.42f, 0.9f);
const LColor kServerColor(0.45f, 0.75f, 1.0f, 0.22f);
const LColor kFieldColor(1.0f, 1.0f, 1.0f, 0.12f);
const LColor kFieldEditingColor(1.0f, 1.0f, 1.0f, 0.24f);
const LColor kText(1.0f, 1.0f, 1.0f, 1.0f);
const LColor kDimText(0.82f, 0.88f, 0.95f, 0.85f);
const char *const kPlaceholder = "Tap here to type the address";

NodePath unitCard(NodePath parent, const char *name, const LColor &color, int sort) {
    CardMaker cm(name);
    cm.set_frame(-1.0f, 1.0f, -1.0f, 1.0f);
    NodePath np = parent.attach_new_node(cm.generate());
    np.set_color(color, 1);
    np.set_transparency(TransparencyAttrib::M_alpha);
    np.set_bin("fixed", sort);
    return np;
}

PT(TextNode) makeText(NodePath parent, const char *name, NodePath &np, int align, const LColor &color) {
    PT(TextNode) t = new TextNode(name);
    t->set_align(static_cast<TextNode::Alignment>(align));
    t->set_text_color(color);
    t->set_shadow(0.05f, 0.05f);
    t->set_shadow_color(LColor(0, 0, 0, 0.6f));
    np = parent.attach_new_node(t);
    np.set_bin("fixed", 60);
    return t;
}

bool validHostChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-';
}

} // namespace

bool ConnectScreen::parseAddress(const std::string &text, std::string &host, uint16_t &port) {
    std::string t;
    for (char c : text) {
        if (c != ' ') t += c;
    }
    port = 4400;
    const auto colon = t.find(':');
    if (colon != std::string::npos) {
        if (t.find(':', colon + 1) != std::string::npos) return false;
        const std::string p = t.substr(colon + 1);
        if (p.empty() || p.size() > 5 || !std::all_of(p.begin(), p.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return false;
        }
        const long v = std::strtol(p.c_str(), nullptr, 10);
        if (v < 1 || v > 65535) return false;
        port = static_cast<uint16_t>(v);
        t = t.substr(0, colon);
    }
    if (t.empty() || t.front() == '.' || t.back() == '.' || t.find("..") != std::string::npos) return false;
    if (!std::all_of(t.begin(), t.end(), validHostChar)) return false;
    host = t;
    return true;
}

bool ConnectScreen::sameServer(const std::string &a, const std::string &b) {
    std::string hostA, hostB;
    uint16_t portA = 0, portB = 0;
    if (!parseAddress(a, hostA, portA) || !parseAddress(b, hostB, portB) || portA != portB) return false;
    auto lower = [](std::string t) {
        for (char &c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return t;
    };
    return lower(hostA) == lower(hostB);
}

ConnectScreen::ConnectScreen(NodePath aspect2d, const std::string &address, const std::string &playerName)
    : address_(address) {
    root_ = aspect2d.attach_new_node("connect-screen");
    dim_ = unitCard(root_, "dim", LColor(0.02f, 0.05f, 0.08f, 0.12f), 49);
    panel_ = unitCard(root_, "panel", LColor(0.03f, 0.08f, 0.12f, 0.4f), 50);

    title_ = makeText(root_, "title", titleNp_, TextNode::A_left, LColor(1.0f, 0.95f, 0.75f, 1.0f));
    title_->set_text("PANDA ARENA");
    subtitle_ = makeText(root_, "subtitle", subtitleNp_, TextNode::A_left, kDimText);
    subtitle_->set_text("Server address or name");
    fieldCard_ = unitCard(root_, "field", kFieldColor, 51);
    field_ = makeText(root_, "field-text", fieldNp_, TextNode::A_left, kText);
    foundTitle_ = makeText(root_, "found", foundTitleNp_, TextNode::A_left, kDimText);
    foundTitle_->set_text("On this network");
    message_ = makeText(root_, "message", messageNp_, TextNode::A_left, LColor(1.0f, 0.86f, 0.45f, 1.0f));
    message_->set_wordwrap(24.0f);
    you_ = makeText(root_, "you", youNp_, TextNode::A_left, kDimText);
    you_->set_text("Playing as " + playerName);

    addButton("play", "PLAY", kPlayColor, 0.09f);
    for (int i = 0; i < kServerRows; ++i) {
        addButton("srv" + std::to_string(i), "", kServerColor, 0.05f);
    }
    for (Button &b : buttons_) {
        if (b.id.rfind("srv", 0) == 0) b.text->set_align(TextNode::A_left);
    }
    refreshMessage();
    layout();
    refreshAddress();
}

ConnectScreen::~ConnectScreen() { root_.remove_node(); }

ConnectScreen::Button &ConnectScreen::addButton(const std::string &id, const std::string &label, const LColor &color,
                                                float textScale) {
    Button b;
    b.id = id;
    b.color = color;
    b.np = root_.attach_new_node("button-" + id);
    b.card = unitCard(b.np, "card", color, 52);
    NodePath textNp;
    b.text = makeText(b.np, "label", textNp, TextNode::A_center, kText);
    textNp.set_scale(textScale);
    b.text->set_text(label);
    buttons_.push_back(b);
    return buttons_.back();
}

void ConnectScreen::place(Button &b, float x, float z, float halfW, float halfH) {
    b.x = x;
    b.z = z;
    b.halfW = halfW;
    b.halfH = halfH;
    b.np.set_pos(x, 0.0f, z);
    b.card.set_scale(halfW, 1.0f, halfH);
    NodePath text = b.np.find("label");
    const float scale = text.get_sx();
    if (b.id.rfind("srv", 0) == 0) {
        text.set_pos(-halfW + 0.04f * scale_, 0.0f, -scale * 0.3f);
    } else {
        text.set_pos(0.0f, 0.0f, -scale * 0.32f);
    }
}

void ConnectScreen::setWindow(int width, int height, float uiScale) {
    if (width <= 0 || height <= 0) return;
    uiScale = std::clamp(uiScale, 0.8f, 1.5f);
    if (width == width_ && height == height_ && uiScale == scale_) return;
    width_ = width;
    height_ = height;
    aspect_ = static_cast<float>(width) / height;
    scale_ = uiScale;
    layout();
}

void ConnectScreen::layout() {
    const float s = scale_, a = aspect_;
    dim_.set_scale(a, 1.0f, 1.0f);

    // One panel in the middle, as wide as fits. The address field and Play
    // are near the top: the phone's keyboard covers the lower half.
    const float w = std::min(2.0f * a - 0.24f * s, 2.6f * s);
    const float x0 = -w * 0.5f, x1 = w * 0.5f;
    for (Button &b : buttons_) {
        b.np.find("label").set_scale((b.id == "play" ? 0.09f : 0.05f) * s);
    }
    const float titleZ = 0.92f - 0.12f * s;
    titleNp_.set_pos(x0, 0.0f, titleZ);
    titleNp_.set_scale(0.12f * s);
    const float subtitleZ = titleZ - 0.12f * s;
    subtitleNp_.set_pos(x0, 0.0f, subtitleZ);
    subtitleNp_.set_scale(0.048f * s);

    const float fieldZ = subtitleZ - 0.1f * s, fieldHalfH = 0.07f * s;
    const float playHalfW = 0.2f * s, gap = 0.04f * s;
    const float fieldRight = x1 - 2.0f * playHalfW - gap;
    fieldX_ = (x0 + fieldRight) * 0.5f;
    fieldZ_ = fieldZ;
    fieldHalfW_ = (fieldRight - x0) * 0.5f;
    fieldHalfH_ = fieldHalfH;
    fieldCard_.set_pos(fieldX_, 0.0f, fieldZ);
    fieldCard_.set_scale(fieldHalfW_, 1.0f, fieldHalfH);
    const float fieldScale = 0.068f * s;
    fieldNp_.set_pos(x0 + 0.04f * s, 0.0f, fieldZ - 0.025f * s);
    fieldNp_.set_scale(fieldScale);
    fieldTextWidth_ = std::max(1.0f, (2.0f * fieldHalfW_ - 0.08f * s) / fieldScale);
    fieldText_.clear();  // fit the shown text to the new width
    for (Button &b : buttons_) {
        if (b.id == "play") place(b, x1 - playHalfW, fieldZ, playHalfW, fieldHalfH);
    }

    const float foundZ = fieldZ - 0.15f * s;
    foundTitleNp_.set_pos(x0, 0.0f, foundZ);
    foundTitleNp_.set_scale(0.045f * s);
    for (int i = 0; i < kServerRows; ++i) {
        for (Button &b : buttons_) {
            if (b.id == "srv" + std::to_string(i)) {
                place(b, (x0 + x1) * 0.5f, foundZ - 0.1f * s - i * 0.14f * s, w * 0.5f, 0.055f * s);
            }
        }
    }
    const float messageZ = foundZ - 0.1f * s - (kServerRows - 1) * 0.14f * s - 0.12f * s;
    messageNp_.set_pos(x0, 0.0f, messageZ);
    messageNp_.set_scale(0.045f * s);
    message_->set_wordwrap(std::max(8.0f, w / (0.045f * s)));
    const float youZ = messageZ - 0.13f * s;
    youNp_.set_pos(x0, 0.0f, youZ);
    youNp_.set_scale(0.04f * s);
    // A panel just behind the text, so the meadow stays in view.
    const float panelTop = 0.95f, panelBottom = youZ - 0.06f * s;
    panel_.set_pos(0.0f, 0.0f, (panelTop + panelBottom) * 0.5f);
    panel_.set_scale(w * 0.5f + 0.06f * s, 1.0f, (panelTop - panelBottom) * 0.5f);
}

void ConnectScreen::setVisible(bool visible) {
    visible_ = visible;
    if (visible) root_.show();
    else root_.hide();
    editing_ = false;
    pressedTouch_ = -1;
    pressedId_.clear();
}

void ConnectScreen::setServers(const std::vector<net::LanDiscovery::Server> &servers) {
    servers_ = servers;
    for (int i = 0; i < kServerRows; ++i) {
        for (Button &b : buttons_) {
            if (b.id != "srv" + std::to_string(i)) continue;
            if (i < static_cast<int>(servers.size())) {
                const auto &srv = servers[i];
                std::string label = srv.name + "   " + std::to_string(srv.players) + "/" +
                                    std::to_string(srv.maxPlayers) + " players   " + srv.address;
                if (srv.port != net::kDiscoveryPort) label += ":" + std::to_string(srv.port);
                b.text->set_text(label);
                b.np.show();
                b.enabled = true;
            } else {
                b.np.hide();
                b.enabled = false;
            }
        }
    }
    foundTitle_->set_text(servers.empty() ? "On this network" : "On this network: tap to join");
    refreshMessage();
}

void ConnectScreen::setMessage(const std::string &text) {
    messageText_ = text;
    refreshMessage();
}

// The app's message if there is one, else how the search is going.
void ConnectScreen::refreshMessage() {
    if (!messageText_.empty()) {
        message_->set_text(messageText_);
    } else {
        message_->set_text(servers_.empty() ? "Looking for servers on this network..." : "");
    }
}

void ConnectScreen::setCurrentServer(const std::string &address) {
    current_ = address;
    refreshAddress();
}

bool ConnectScreen::takeJoin(std::string &address) {
    if (!hasJoin_) return false;
    hasJoin_ = false;
    address = join_;
    return true;
}

LPoint2f ConnectScreen::toAspect(float px, float py) const {
    return LPoint2f((px / width_ * 2.0f - 1.0f) * aspect_, 1.0f - py / height_ * 2.0f);
}

ConnectScreen::Button *ConnectScreen::hit(float x, float z) {
    for (Button &b : buttons_) {
        if (!b.enabled) continue;
        // A little extra margin: fingers are bigger than the drawn button.
        const float mx = b.halfW + 0.015f * scale_, mz = b.halfH + 0.015f * scale_;
        if (std::abs(x - b.x) <= mx && std::abs(z - b.z) <= mz) return &b;
    }
    return nullptr;
}

bool ConnectScreen::onField(float x, float z) const {
    const float m = 0.015f * scale_;
    return std::abs(x - fieldX_) <= fieldHalfW_ + m && std::abs(z - fieldZ_) <= fieldHalfH_ + m;
}

void ConnectScreen::handle(const platform::Touch &touch) {
    using Kind = platform::Touch::Kind;
    if (!visible_) return;
    const LPoint2f p = toAspect(touch.x, touch.y);
    if (touch.kind == Kind::Down) {
        if (Button *b = hit(p[0], p[1])) {
            pressedTouch_ = touch.id;
            pressedId_ = b->id;
        } else if (onField(p[0], p[1])) {
            pressedTouch_ = touch.id;
            pressedId_ = "field";
        } else {
            editing_ = false;  // a tap elsewhere puts the keyboard away
        }
    } else if (touch.kind == Kind::Up && touch.id == pressedTouch_) {
        const std::string id = pressedId_;
        pressedTouch_ = -1;
        pressedId_.clear();
        if (id == "field") {
            if (onField(p[0], p[1])) editing_ = keyboardRequest_ = true;
        } else if (Button *b = hit(p[0], p[1]); b != nullptr && b->id == id) {
            press(id);
        }
    } else if (touch.kind == Kind::Cancel && (touch.id == pressedTouch_ || touch.id < 0)) {
        pressedTouch_ = -1;
        pressedId_.clear();
    }
}

void ConnectScreen::type(const std::string &keys) {
    for (char c : keys) {
        if (c == '\b') {
            if (!address_.empty()) address_.pop_back();
        } else if (c == '\n') {
            press("play");
        } else if (address_.size() < kMaxAddress) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');  // host names: any case
            if (c == ':' && address_.find(':') != std::string::npos) continue;
            if (validHostChar(c) || c == ':') address_ += c;
        }
    }
    refreshAddress();
}

void ConnectScreen::press(const std::string &id) {
    if (id == "play") {
        editing_ = false;
        std::string host;
        uint16_t port = 0;
        if (parseAddress(address_, host, port)) {
            join_ = address_;
            hasJoin_ = true;
        } else {
            setMessage(address_.empty() ? "Type the server's address, or pick a server below."
                                        : "That does not look like an address (e.g. 192.168.1.20, "
                                          "arena.example.com or arena.example.com:4400).");
        }
    } else if (id.rfind("srv", 0) == 0) {
        const size_t i = static_cast<size_t>(std::atoi(id.c_str() + 3));
        if (i < servers_.size()) {
            editing_ = false;
            address_ = servers_[i].address;
            if (servers_[i].port != net::kDiscoveryPort) address_ += ":" + std::to_string(servers_[i].port);
            join_ = address_;
            hasJoin_ = true;
        }
    }
    refreshAddress();
}

void ConnectScreen::refreshAddress() {
    std::string host;
    uint16_t port = 0;
    const bool ok = parseAddress(address_, host, port);
    // The server of the game going on behind the menu: back to it, still connected.
    const bool resume = !current_.empty() && sameServer(address_, current_);
    for (Button &b : buttons_) {
        if (b.id != "play") continue;
        b.card.set_color(ok ? kPlayColor : LColor(0.4f, 0.5f, 0.45f, 0.5f), 1);
        b.text->set_text(resume ? "RESUME" : "PLAY");
    }
}

void ConnectScreen::update(double now) {
    now_ = now;
    if (!visible_) return;
    const std::string cursor = editing_ && std::fmod(now, 1.0) < 0.55 ? "_" : (editing_ ? " " : "");
    const std::string key = address_ + '\n' + cursor;
    if (key != fieldText_) {  // regenerating the glyphs is the expensive part
        fieldText_ = key;
        std::string shown;
        if (address_.empty() && !editing_) {
            shown = kPlaceholder;
        } else {
            // A long name scrolls: its end (where the typing is) stays in view.
            std::string body = address_;
            shown = body + cursor;
            if (field_->calc_width(shown) > fieldTextWidth_) {
                while (!body.empty() && field_->calc_width("..." + body + cursor) > fieldTextWidth_) {
                    body.erase(0, 1);
                }
                shown = "..." + body + cursor;
            }
        }
        field_->set_text_color(address_.empty() ? LColor(1, 1, 1, 0.45f) : kText);
        field_->set_text(shown);
    }
    fieldCard_.set_color(editing_ ? kFieldEditingColor : kFieldColor, 1);
    for (Button &b : buttons_) {
        if (b.id == "play") continue;
        LColor c = b.color;
        if (b.id == pressedId_) c = LColor(c[0], c[1], c[2], std::min(1.0f, c[3] + 0.3f));
        b.card.set_color(c, 1);
    }
}

} // namespace arena::game
