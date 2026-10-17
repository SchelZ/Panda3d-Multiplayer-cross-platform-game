// 2D overlay: status, round timer, scoreboard, chat, announcements, help.
// Text nodes are only regenerated when their string changes.
#pragma once

#include "nodePath.h"
#include "pgEntry.h"
#include "pointerTo.h"
#include "textNode.h"

#include <deque>
#include <string>
#include <vector>

namespace arena::game {

struct ScoreRow {
    std::string name;
    int color = 0;
    int score = 0;
    bool local = false;
};

struct HudState {
    std::string status;     // top-left
    std::string netStats;   // shown with F3
    std::string round;      // top centre
    std::string center;     // big centre message; empty = none
    std::vector<ScoreRow> scores;
    float dashReady = 1.0f; // 0..1
    bool showDash = false;
};

class Hud {
public:
    explicit Hud(NodePath aspect2d);
    ~Hud();
    Hud(const Hud &) = delete;
    Hud &operator=(const Hud &) = delete;

    void setAspect(float aspect);  // window width / height
    void setUiScale(float scale);  // 1 = desktop; larger on phones (laid out for touch there)
    void setVisible(bool visible);
    void reset();  // forget the last game: scores, chat, messages
    void update(const HudState &state, double now);
    void addChat(const std::string &who, int color, const std::string &text, double now);
    void addSystem(const std::string &text, double now);
    void announce(const std::string &text, double now);

    void toggleHelp();
    void toggleStats();

    // Chat input (a PGEntry).
    bool chatOpen() const { return chatOpen_; }
    void openChat();
    void closeChat();
    std::string takeChatText();
    std::string chatAcceptEvent() const;

private:
    struct ChatLine {
        std::string text;
        double time;
    };
    NodePath makeText(const std::string &name, float scale, int align, NodePath parent, PT(TextNode) &out);
    void setText(TextNode *node, std::string &cache, const std::string &text);
    void layout();
    void rebuildChat(double now);

    NodePath aspect2d_, root_;
    float aspect_ = 16.0f / 9.0f;
    float scale_ = 1.0f;

    PT(TextNode) status_, stats_, round_, center_, announce_, chat_, help_, hint_;
    NodePath statusNp_, statsNp_, roundNp_, centerNp_, announceNp_, chatNp_, helpNp_, hintNp_;
    std::string statusText_, statsText_, roundText_, centerText_, chatText_;

    static constexpr int kScoreRows = 8;
    PT(TextNode) scoreName_[kScoreRows], scoreValue_[kScoreRows];
    NodePath scoreNameNp_[kScoreRows], scoreValueNp_[kScoreRows], scoreTitleNp_;
    std::string scoreNameText_[kScoreRows], scoreValueText_[kScoreRows];
    int scoreColor_[kScoreRows] = {-1, -1, -1, -1, -1, -1, -1, -1};  // set_text_color rebuilds the text
    NodePath scoreRoot_;

    NodePath dashRoot_, dashFill_;
    PT(PGEntry) entry_;
    NodePath entryNp_;
    bool chatOpen_ = false;

    std::deque<ChatLine> chatLines_;
    double announceUntil_ = 0.0;
    bool showHelp_ = false;
    bool showStats_ = false;
};

} // namespace arena::game
