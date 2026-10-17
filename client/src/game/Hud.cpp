#include "game/Hud.h"

#include "game/World.h"

#include "cardMaker.h"
#include "keyboardButton.h"
#include "transparencyAttrib.h"

#include <algorithm>
#include <cmath>

namespace arena::game {

namespace {

#ifdef __ANDROID__
// Touch screens: other hints, chat higher up (the stick is bottom left), no
// dash bar (the dash button shows it).
constexpr bool kTouch = true;
const char *const kHint = "Stick: move  |  DASH button: dash  |  Two fingers: zoom  |  Back: menu";
#else
constexpr bool kTouch = false;
const char *const kHint = "WASD move  |  Space dash  |  Enter chat  |  F1 help";
#endif

constexpr double kChatLife = 14.0;
constexpr size_t kChatLines = 7;
constexpr double kAnnounceLife = 4.0;

const char *kHelpText =
    "PANDA ARENA\n"
    "\n"
    "Collect glowing orbs: blue = 1 point, gold = 5 points.\n"
    "Dash into other players to steal 2 of their points.\n"
    "Most points when the round timer runs out wins.\n"
    "\n"
    "WASD / arrows   move\n"
    "Space           dash (short cooldown)\n"
    "Enter           chat\n"
    "Mouse wheel     zoom\n"
    "F3              network statistics\n"
    "F11             fullscreen\n"
    "Esc             quit\n"
    "\n"
    "F1 closes this help";

NodePath makeCard(NodePath parent, float l, float r, float b, float t, const LColor &color) {
    CardMaker cm("card");
    cm.set_frame(l, r, b, t);
    NodePath card = parent.attach_new_node(cm.generate());
    card.set_color(color);
    card.set_transparency(TransparencyAttrib::M_alpha);
    return card;
}

} // namespace

Hud::Hud(NodePath aspect2d) : aspect2d_(aspect2d) {
    root_ = aspect2d_.attach_new_node("hud");

    statusNp_ = makeText("status", 0.042f, TextNode::A_left, root_, status_);
    statsNp_ = makeText("stats", 0.036f, TextNode::A_left, root_, stats_);
    statsNp_.hide();
    roundNp_ = makeText("round", 0.07f, TextNode::A_center, root_, round_);
    centerNp_ = makeText("center", 0.075f, TextNode::A_center, root_, center_);
    center_->set_wordwrap(22.0f);
    announceNp_ = makeText("announce", 0.062f, TextNode::A_center, root_, announce_);
    announce_->set_text_color(LColor(1.0f, 0.88f, 0.4f, 1.0f));
    announce_->set_wordwrap(26.0f);
    chatNp_ = makeText("chat", 0.04f, TextNode::A_left, root_, chat_);
    chat_->set_wordwrap(34.0f);
    hintNp_ = makeText("hint", 0.036f, TextNode::A_center, root_, hint_);
    hint_->set_text(kHint);
    hint_->set_text_color(LColor(0.8f, 0.85f, 0.95f, 0.75f));
    helpNp_ = makeText("help", 0.05f, TextNode::A_left, root_, help_);
    help_->set_text(kHelpText);
    help_->set_card_color(LColor(0.0f, 0.0f, 0.0f, 0.75f));
    help_->set_card_as_margin(0.8f, 0.8f, 0.6f, 0.6f);
    helpNp_.hide();

    scoreRoot_ = root_.attach_new_node("scores");
    PT(TextNode) title;
    scoreTitleNp_ = makeText("score-title", 0.045f, TextNode::A_right, scoreRoot_, title);
    title->set_text("SCORE");
    title->set_text_color(LColor(0.7f, 0.85f, 1.0f, 1.0f));
    for (int i = 0; i < kScoreRows; ++i) {
        scoreNameNp_[i] = makeText("score-name", 0.042f, TextNode::A_left, scoreRoot_, scoreName_[i]);
        scoreValueNp_[i] = makeText("score-value", 0.042f, TextNode::A_right, scoreRoot_, scoreValue_[i]);
    }

    dashRoot_ = root_.attach_new_node("dash");
    makeCard(dashRoot_, -0.2f, 0.2f, -0.012f, 0.012f, LColor(1, 1, 1, 0.15f));
    dashFill_ = makeCard(dashRoot_, 0.0f, 0.4f, -0.012f, 0.012f, LColor(0.3f, 0.9f, 1.0f, 0.9f));
    dashFill_.set_x(-0.2f);
    dashRoot_.hide();

    // Chat input: a PGEntry, focused while typing.
    entry_ = new PGEntry("chat-entry");
    entry_->setup(30.0f, 1);
    entry_->set_max_chars(120);
    entryNp_ = root_.attach_new_node(entry_);
    entryNp_.set_scale(0.042f);
    entryNp_.hide();

    layout();
}

Hud::~Hud() {
    entry_->set_focus(false);
    root_.remove_node();
}

NodePath Hud::makeText(const std::string &name, float scale, int align, NodePath parent, PT(TextNode) &out) {
    out = new TextNode(name);
    out->set_align(static_cast<TextNode::Alignment>(align));
    out->set_text_color(LColor(1, 1, 1, 1));
    out->set_shadow(0.05f, 0.05f);
    out->set_shadow_color(LColor(0, 0, 0, 0.8f));
    NodePath np = parent.attach_new_node(out);
    np.set_scale(scale);
    return np;
}

void Hud::setText(TextNode *node, std::string &cache, const std::string &text) {
    if (cache != text) {  // regenerating glyph geometry is the expensive part
        cache = text;
        node->set_text(text);
    }
}

void Hud::setAspect(float aspect) {
    if (std::abs(aspect - aspect_) > 1e-3f) {
        aspect_ = aspect;
        layout();
    }
}

void Hud::setUiScale(float scale) {
    if (scale > 0.0f && std::abs(scale - scale_) > 1e-3f) {
        scale_ = scale;
        root_.set_scale(scale_);
        layout();
    }
}

void Hud::setVisible(bool visible) {
    if (visible) root_.show();
    else root_.hide();
}

void Hud::reset() {
    closeChat();
    chatLines_.clear();
    announce_->set_text("");
    announceUntil_ = 0.0;
    update(HudState(), 0.0);
}

void Hud::layout() {
    // Positions are in screen units (aspect2d); everything under root_ is
    // enlarged by the UI scale, so divide by it, and scale the margins.
    const float a = aspect_, s = scale_;
    auto at = [s](NodePath np, float x, float z) { np.set_pos(x / s, 0.0f, z / s); };
    at(statusNp_, -a + 0.04f * s, 1.0f - 0.06f * s);
    at(statsNp_, -a + 0.04f * s, 1.0f - 0.12f * s);
    at(roundNp_, 0.0f, 1.0f - 0.11f * s);
    at(centerNp_, 0.0f, 0.15f);
    at(announceNp_, 0.0f, 1.0f - 0.34f * s);
    // On touch screens the stick is bottom left: chat goes higher.
    at(chatNp_, -a + 0.04f * s, kTouch ? 0.42f : -0.5f);
    at(hintNp_, 0.0f, -1.0f + 0.04f * s);
    at(helpNp_, -0.62f * s, 0.55f);
    at(scoreRoot_, a - 0.05f * s, 1.0f - 0.08f * s);
    scoreTitleNp_.set_pos(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < kScoreRows; ++i) {
        const float z = -0.065f * (i + 1);
        scoreNameNp_[i].set_pos(-0.62f, 0.0f, z);
        scoreValueNp_[i].set_pos(0.0f, 0.0f, z);
    }
    at(dashRoot_, 0.0f, -1.0f + 0.12f * s);
    at(entryNp_, -a + 0.04f * s, -1.0f + 0.12f * s);
}

void Hud::update(const HudState &state, double now) {
    setText(status_, statusText_, state.status);
    setText(stats_, statsText_, state.netStats);
    setText(round_, roundText_, state.round);
    setText(center_, centerText_, state.center);

    for (int i = 0; i < kScoreRows; ++i) {
        std::string name, value;
        if (i < static_cast<int>(state.scores.size())) {
            const ScoreRow &row = state.scores[i];
            name = std::to_string(i + 1) + ". " + row.name + (row.local ? "  <" : "");
            value = std::to_string(row.score);
            if (scoreColor_[i] != row.color) {
                scoreColor_[i] = row.color;
                scoreName_[i]->set_text_color(World::playerColor(row.color));
            }
        }
        setText(scoreName_[i], scoreNameText_[i], name);
        setText(scoreValue_[i], scoreValueText_[i], value);
    }
    if (state.scores.empty()) scoreRoot_.hide();
    else scoreRoot_.show();

    if (state.showDash && !kTouch) {
        dashRoot_.show();
        dashFill_.set_sx(std::max(0.001f, state.dashReady));
        dashFill_.set_color(state.dashReady >= 1.0f ? LColor(0.3f, 0.95f, 1.0f, 0.95f)
                                                    : LColor(0.5f, 0.6f, 0.7f, 0.8f));
    } else {
        dashRoot_.hide();
    }

    if (announceUntil_ > 0.0 && now > announceUntil_) {
        announce_->set_text("");
        announceUntil_ = 0.0;
    } else if (announceUntil_ > 0.0) {
        const float fade = static_cast<float>(std::min(1.0, (announceUntil_ - now) / 0.6));
        announceNp_.set_alpha_scale(fade);
    }
    rebuildChat(now);
}

void Hud::addChat(const std::string &who, int color, const std::string &text, double now) {
    (void)color;
    chatLines_.push_back({who + ": " + text, now});
    while (chatLines_.size() > kChatLines) chatLines_.pop_front();
    rebuildChat(now);
}

void Hud::addSystem(const std::string &text, double now) {
    chatLines_.push_back({"* " + text, now});
    while (chatLines_.size() > kChatLines) chatLines_.pop_front();
    rebuildChat(now);
}

void Hud::announce(const std::string &text, double now) {
    announce_->set_text(text);
    announceNp_.set_alpha_scale(1.0f);
    announceUntil_ = now + kAnnounceLife;
    addSystem(text, now);
}

void Hud::rebuildChat(double now) {
    while (!chatLines_.empty() && !chatOpen_ && now - chatLines_.front().time > kChatLife) {
        chatLines_.pop_front();
    }
    std::string text;
    for (const ChatLine &line : chatLines_) {
        if (!text.empty()) text += '\n';
        text += line.text;
    }
    setText(chat_, chatText_, text);
}

void Hud::toggleHelp() {
    showHelp_ = !showHelp_;
    if (showHelp_) helpNp_.show();
    else helpNp_.hide();
}

void Hud::toggleStats() {
    showStats_ = !showStats_;
    if (showStats_) statsNp_.show();
    else statsNp_.hide();
}

void Hud::openChat() {
    if (chatOpen_) return;
    chatOpen_ = true;
    entry_->set_text("");
    entryNp_.show();
    entry_->set_focus(true);
    hintNp_.hide();
}

void Hud::closeChat() {
    if (!chatOpen_) return;
    chatOpen_ = false;
    entry_->set_focus(false);
    entryNp_.hide();
    hintNp_.show();
}

std::string Hud::takeChatText() {
    const std::string text = entry_->get_text();
    entry_->set_text("");
    return text;
}

std::string Hud::chatAcceptEvent() const { return entry_->get_accept_event(KeyboardButton::enter()); }

} // namespace arena::game
