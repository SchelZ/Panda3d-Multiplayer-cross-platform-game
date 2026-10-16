// On-screen controls for touch screens (phones, tablets; --touch on a PC,
// where the mouse is the finger). Drawn in aspect2d:
//  - bottom left: the stick, always in the same place. A thumb that lands on
//    it (or close to it) moves the knob; the ring stays put. The direction
//    counts, not how far: like the keys, the player always runs at full speed
//    (which is also what the other screens predict, see net/Protocol.h).
//  - bottom right: the dash button, on touch down (no waiting for the finger
//    to lift), only when the button itself is touched. It shows the cooldown.
//  - two fingers elsewhere on the right half: pinch to zoom.
//  - the top-left corner (the status line): network statistics on and off.
#pragma once

#include "app/Platform.h"

#include "nodePath.h"
#include "pointerTo.h"
#include "textNode.h"

#include <vector>

namespace arena::game {

class TouchControls {
public:
    explicit TouchControls(NodePath aspect2d);
    ~TouchControls();
    TouchControls(const TouchControls &) = delete;
    TouchControls &operator=(const TouchControls &) = delete;

    // Window size in pixels, and how much to enlarge the controls.
    void setWindow(int width, int height, float uiScale);
    void handle(const platform::Touch &touch);
    void releaseAll();  // e.g. the app went to the background
    void setVisible(bool visible);

    struct Output {
        float x = 0.0f, y = 0.0f;  // unit direction (screen up = +y), or zero
        bool dash = false;         // since the last take()
        float zoomSteps = 0.0f;    // like mouse wheel steps, + = closer
        bool toggleStats = false;
    };
    Output take();

    // Animates the controls; dashReady 0..1 (1 = ready).
    void update(double now, float dashReady);

    // Where the stick and the button are (aspect2d units), e.g. for tests.
    LPoint2f stickCenter() const { return stickHome_; }
    LPoint2f dashCenter() const { return dashCenter_; }

private:
    enum class Role { Stick, Dash, Pinch, Stats, Ignore };
    struct Finger {
        int id;
        Role role;
        LPoint2f pos;    // aspect2d units
        LPoint2f start;
    };
    LPoint2f toAspect(float px, float py) const;
    Finger *find(int id);
    void layout();
    float pinchDistance() const;

    NodePath root_, stick_, stickRing_, stickKnob_, dash_, dashRing_, dashFill_, dashLabel_;
    int width_ = 1280, height_ = 720;
    float aspect_ = 16.0f / 9.0f;
    float scale_ = 1.0f;
    bool visible_ = true;

    std::vector<Finger> fingers_;
    LPoint2f stickHome_{0, 0}, dashCenter_{0, 0};
    float lastPinch_ = 0.0f;

    Output pending_;
    double dashPressedAt_ = -1.0, now_ = 0.0;
};

} // namespace arena::game
