#include "game/ConnectScreen.h"

#include "cardMaker.h"
#include "transparencyAttrib.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace arena::game {

namespace {

constexpr size_t kMaxAddress = 40;
const LColor kKeyColor(1.0f, 1.0f, 1.0f, 0.16f);
const LColor kPlayColor(0.32f, 0.78f, 0.42f, 0.9f);
const LColor kServerColor(0.45f, 0.75f, 1.0f, 0.22f);
const LColor kText(1.0f, 1.0f, 1.0f, 1.0f);
const LColor kDimText(0.82f, 0.88f, 0.95f, 0.85f);

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

ConnectScreen::ConnectScreen(NodePath aspect2d, const std::string &address, const std::string &playerName)
    : address_(address) {
    root_ = aspect2d.attach_new_node("connect-screen");
    dim_ = unitCard(root_, "dim", LColor(0.02f, 0.05f, 0.08f, 0.12f), 49);
    leftPanel_ = unitCard(root_, "left-panel", LColor(0.03f, 0.08f, 0.12f, 0.4f), 50);
    rightPanel_ = unitCard(root_, "right-panel", LColor(0.03f, 0.08f, 0.12f, 0.4f), 50);

    title_ = makeText(root_, "title", titleNp_, TextNode::A_left, LColor(1.0f, 0.95f, 0.75f, 1.0f));
    title_->set_text("PANDA ARENA");
    subtitle_ = makeText(root_, "subtitle", subtitleNp_, TextNode::A_left, kDimText);
    subtitle_->set_text("Server address");
    fieldCard_ = unitCard(root_, "field", LColor(1.0f, 1.0f, 1.0f, 0.12f), 51);
    field_ = makeText(root_, "field-text", fieldNp_, TextNode::A_left, kText);
    foundTitle_ = makeText(root_, "found", foundTitleNp_, TextNode::A_left, kDimText);
    foundTitle_->set_text("On this network");
    message_ = makeText(root_, "message", messageNp_, TextNode::A_left, LColor(1.0f, 0.86f, 0.45f, 1.0f));
    message_->set_wordwrap(24.0f);
    you_ = makeText(root_, "you", youNp_, TextNode::A_left, kDimText);
    you_->set_text("Playing as " + playerName);

    static const char *keys[] = {"1", "2", "3", "del", "4", "5", "6", "clr", "7", "8", "9", ":", ".", "0"};
    for (const char *k : keys) {
        const std::string id = k;
        const std::string label = id == "del" ? "DEL" : id == "clr" ? "CLEAR" : id;
        addButton(id, label, kKeyColor, id.size() > 1 ? 0.06f : 0.09f);
    }
    addButton("play", "PLAY", kPlayColor, 0.09f);
    for (int i = 0; i < kServerRows; ++i) {
        addButton("srv" + std::to_string(i), "", kServerColor, 0.05f);
    }
    for (Button &b : buttons_) {
        if (b.id.rfind("srv", 0) == 0) b.text->set_align(TextNode::A_left);
    }
    setMessage("Looking for servers on this network...");
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

    // The keypad on the right, where a right thumb reaches.
    const float key = 0.2f * s, gap = 0.035f * s;
    const float gridW = 4 * key + 3 * gap, gridH = 4 * key + 3 * gap;
    const float right = a - 0.1f * s, left = right - gridW;
    const float top = std::min(0.6f, gridH * 0.5f + 0.02f);
    auto cellX = [&](int c) { return left + key * 0.5f + c * (key + gap); };
    auto cellZ = [&](int r) { return top - key * 0.5f - r * (key + gap); };
    const char *grid[4][4] = {{"1", "2", "3", "del"}, {"4", "5", "6", "clr"}, {"7", "8", "9", ":"}, {".", "0", "", ""}};
    for (Button &b : buttons_) {
        b.np.find("label").set_scale((b.id.size() > 1 ? 0.06f : 0.09f) * s);
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            for (Button &b : buttons_) {
                if (b.id == grid[r][c]) place(b, cellX(c), cellZ(r), key * 0.5f, key * 0.5f);
            }
        }
    }
    for (Button &b : buttons_) {
        if (b.id == "play") place(b, (cellX(2) + cellX(3)) * 0.5f, cellZ(3), key + gap * 0.5f, key * 0.5f);
    }
    rightPanel_.set_pos((left + right) * 0.5f, 0.0f, top - gridH * 0.5f);
    rightPanel_.set_scale(gridW * 0.5f + 0.05f * s, 1.0f, gridH * 0.5f + 0.05f * s);

    // Everything else on the left.
    const float x0 = -a + 0.12f * s, x1 = left - 0.14f * s;
    const float w = x1 - x0;
    // Top to bottom; at the largest UI scale this still ends above -0.8.
    const float titleZ = 0.92f - 0.12f * s;
    titleNp_.set_pos(x0, 0.0f, titleZ);
    titleNp_.set_scale(0.12f * s);
    const float subtitleZ = titleZ - 0.12f * s;
    subtitleNp_.set_pos(x0, 0.0f, subtitleZ);
    subtitleNp_.set_scale(0.048f * s);
    const float fieldZ = subtitleZ - 0.1f * s;
    fieldCard_.set_pos((x0 + x1) * 0.5f, 0.0f, fieldZ);
    fieldCard_.set_scale(w * 0.5f, 1.0f, 0.07f * s);
    fieldNp_.set_pos(x0 + 0.04f * s, 0.0f, fieldZ - 0.025f * s);
    fieldNp_.set_scale(0.068f * s);
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
    leftPanel_.set_pos((x0 + x1) * 0.5f, 0.0f, (panelTop + panelBottom) * 0.5f);
    leftPanel_.set_scale(w * 0.5f + 0.06f * s, 1.0f, (panelTop - panelBottom) * 0.5f);
}

void ConnectScreen::setVisible(bool visible) {
    visible_ = visible;
    if (visible) root_.show();
    else root_.hide();
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
    if (servers.empty()) {
        foundTitle_->set_text("On this network");
        setMessage("Looking for servers on this network...");
    } else {
        foundTitle_->set_text("On this network: tap to join");
        setMessage("");
    }
}

void ConnectScreen::setMessage(const std::string &text) { message_->set_text(text); }

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
        // A little extra margin: fingers are bigger than the drawn key.
        const float mx = b.halfW + 0.015f * scale_, mz = b.halfH + 0.015f * scale_;
        if (std::abs(x - b.x) <= mx && std::abs(z - b.z) <= mz) return &b;
    }
    return nullptr;
}

void ConnectScreen::handle(const platform::Touch &touch) {
    using Kind = platform::Touch::Kind;
    if (!visible_) return;
    const LPoint2f p = toAspect(touch.x, touch.y);
    if (touch.kind == Kind::Down) {
        if (Button *b = hit(p[0], p[1])) {
            pressedTouch_ = touch.id;
            pressedId_ = b->id;
        }
    } else if (touch.kind == Kind::Up && touch.id == pressedTouch_) {
        Button *b = hit(p[0], p[1]);
        const std::string id = pressedId_;
        pressedTouch_ = -1;
        pressedId_.clear();
        if (b != nullptr && b->id == id) press(id);
    } else if (touch.kind == Kind::Cancel && (touch.id == pressedTouch_ || touch.id < 0)) {
        pressedTouch_ = -1;
        pressedId_.clear();
    }
}

void ConnectScreen::press(const std::string &id) {
    if (id == "del") {
        if (!address_.empty()) address_.pop_back();
    } else if (id == "clr") {
        address_.clear();
    } else if (id == "play") {
        std::string host;
        uint16_t port = 0;
        if (parseAddress(address_, host, port)) {
            join_ = address_;
            hasJoin_ = true;
        } else {
            setMessage(address_.empty() ? "Type the server's address, or pick a server above."
                                        : "That does not look like an address (e.g. 192.168.1.20 or 10.0.0.5:4400).");
        }
    } else if (id.rfind("srv", 0) == 0) {
        const size_t i = static_cast<size_t>(std::atoi(id.c_str() + 3));
        if (i < servers_.size()) {
            address_ = servers_[i].address;
            if (servers_[i].port != net::kDiscoveryPort) address_ += ":" + std::to_string(servers_[i].port);
            join_ = address_;
            hasJoin_ = true;
        }
    } else if (address_.size() < kMaxAddress) {
        if (id == ":" && address_.find(':') != std::string::npos) return;
        address_ += id;
    }
    refreshAddress();
}

void ConnectScreen::refreshAddress() {
    std::string host;
    uint16_t port = 0;
    const bool ok = parseAddress(address_, host, port);
    for (Button &b : buttons_) {
        if (b.id == "play") b.card.set_color(ok ? kPlayColor : LColor(0.4f, 0.5f, 0.45f, 0.5f), 1);
    }
}

void ConnectScreen::update(double now) {
    now_ = now;
    if (!visible_) return;
    const bool cursor = std::fmod(now, 1.0) < 0.55;
    const std::string shown = address_.empty() ? (cursor ? "_  e.g. 192.168.1.20" : "   e.g. 192.168.1.20")
                                               : address_ + (cursor ? "_" : " ");
    if (shown != fieldText_) {  // regenerating the glyphs is the expensive part
        fieldText_ = shown;
        field_->set_text_color(address_.empty() ? LColor(1, 1, 1, 0.45f) : kText);
        field_->set_text(shown);
    }
    for (Button &b : buttons_) {
        if (b.id == "play") continue;
        LColor c = b.color;
        if (b.id == pressedId_) c = LColor(c[0], c[1], c[2], std::min(1.0f, c[3] + 0.3f));
        b.card.set_color(c, 1);
    }
}

} // namespace arena::game
