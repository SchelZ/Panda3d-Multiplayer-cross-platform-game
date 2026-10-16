#include "game/TouchControls.h"

#include "game/Meshes.h"

#include "transparencyAttrib.h"

#include <algorithm>
#include <cmath>

namespace arena::game {

namespace {

// Sizes in aspect2d units (half the screen height = 1) before the UI scale.
constexpr float kStickRadius = 0.18f;
constexpr float kKnobRadius = 0.08f;
constexpr float kDeadZone = 0.2f;     // of the stick radius
constexpr float kStickReach = 1.9f;   // a thumb this close (in radii) grabs the stick
constexpr float kDashRadius = 0.16f;
constexpr float kDashReach = 1.08f;   // the button, and a sliver for the fingertip
constexpr double kPressPulse = 0.18;  // s

NodePath disc(NodePath parent, const char *name, float inner, const LColor &color, int sort) {
    NodePath np = parent.attach_new_node(meshes::ring(name, inner, 1.0f, 48));
    np.set_p(90.0f);  // the ring lies in XY; aspect2d is the XZ plane
    np.set_color(color, 1);
    np.set_transparency(TransparencyAttrib::M_alpha);
    np.set_bin("fixed", sort);
    return np;
}

} // namespace

TouchControls::TouchControls(NodePath aspect2d) {
    root_ = aspect2d.attach_new_node("touch-controls");

    stick_ = root_.attach_new_node("stick");
    stickRing_ = disc(stick_, "stick-ring", 0.86f, LColor(1, 1, 1, 0.9f), 40);
    stickRing_.set_scale(kStickRadius);
    NodePath back = disc(stick_, "stick-back", 0.0f, LColor(0.1f, 0.15f, 0.2f, 0.25f), 39);
    back.set_scale(kStickRadius * 0.86f);
    stickKnob_ = disc(stick_, "stick-knob", 0.0f, LColor(1, 1, 1, 0.85f), 41);
    stickKnob_.set_scale(kKnobRadius);

    dash_ = root_.attach_new_node("dash");
    dashRing_ = disc(dash_, "dash-ring", 0.88f, LColor(1, 1, 1, 0.85f), 40);
    dashRing_.set_scale(kDashRadius);
    dashFill_ = disc(dash_, "dash-fill", 0.0f, LColor(0.3f, 0.9f, 1.0f, 0.5f), 41);
    dashFill_.set_scale(kDashRadius * 0.86f);
    PT(TextNode) label = new TextNode("dash-label");
    label->set_text("DASH");
    label->set_align(TextNode::A_center);
    label->set_text_color(LColor(1, 1, 1, 0.95f));
    label->set_shadow(0.05f, 0.05f);
    label->set_shadow_color(LColor(0, 0, 0, 0.6f));
    dashLabel_ = dash_.attach_new_node(label);
    dashLabel_.set_scale(0.055f);
    dashLabel_.set_z(-0.018f);
    dashLabel_.set_bin("fixed", 42);

    layout();
}

TouchControls::~TouchControls() { root_.remove_node(); }

void TouchControls::setWindow(int width, int height, float uiScale) {
    if (width <= 0 || height <= 0) return;
    const float aspect = static_cast<float>(width) / height;
    if (width == width_ && height == height_ && uiScale == scale_) return;
    width_ = width;
    height_ = height;
    aspect_ = aspect;
    scale_ = uiScale;
    layout();
}

void TouchControls::layout() {
    const float s = scale_, a = aspect_;
    stickHome_ = LPoint2f(-a + 0.42f * s, -1.0f + 0.44f * s);
    dashCenter_ = LPoint2f(a - 0.36f * s, -1.0f + 0.4f * s);
    stick_.set_scale(s);
    stick_.set_pos(stickHome_[0], 0.0f, stickHome_[1]);
    dash_.set_scale(s);
    dash_.set_pos(dashCenter_[0], 0.0f, dashCenter_[1]);
}

LPoint2f TouchControls::toAspect(float px, float py) const {
    return LPoint2f((px / width_ * 2.0f - 1.0f) * aspect_, 1.0f - py / height_ * 2.0f);
}

TouchControls::Finger *TouchControls::find(int id) {
    for (Finger &f : fingers_) {
        if (f.id == id) return &f;
    }
    return nullptr;
}

float TouchControls::pinchDistance() const {
    const Finger *a = nullptr, *b = nullptr;
    for (const Finger &f : fingers_) {
        if (f.role != Role::Pinch) continue;
        if (a == nullptr) a = &f;
        else if (b == nullptr) b = &f;
    }
    return (a && b) ? (a->pos - b->pos).length() : 0.0f;
}

void TouchControls::handle(const platform::Touch &touch) {
    using Kind = platform::Touch::Kind;
    if (touch.kind == Kind::Cancel && touch.id < 0) {
        releaseAll();
        return;
    }
    const LPoint2f p = toAspect(touch.x, touch.y);
    const float s = scale_, a = aspect_;
    const float R = kStickRadius * s;

    if (touch.kind == Kind::Down) {
        if (find(touch.id) != nullptr) return;  // a repeated down
        Finger f{touch.id, Role::Ignore, p, p};
        auto has = [&](Role r) {
            return std::any_of(fingers_.begin(), fingers_.end(), [&](const Finger &o) { return o.role == r; });
        };
        if (p[0] < -a + 0.8f * s && p[1] > 1.0f - 0.16f * s) {
            f.role = Role::Stats;
        } else if ((p - stickHome_).length() <= R * kStickReach) {
            if (!has(Role::Stick)) f.role = Role::Stick;
        } else if ((p - dashCenter_).length() <= kDashRadius * s * kDashReach) {
            f.role = Role::Dash;
            pending_.dash = true;
            dashPressedAt_ = now_;
        } else if (p[0] >= 0.0f) {
            f.role = Role::Pinch;  // two of these zoom
        }
        fingers_.push_back(f);
        if (f.role == Role::Pinch) lastPinch_ = pinchDistance();
        return;
    }

    Finger *f = find(touch.id);
    if (f == nullptr) return;
    if (touch.kind == Kind::Move) {
        f->pos = p;
        if (f->role == Role::Pinch) {
            const float d = pinchDistance();
            if (d > 0.0f && lastPinch_ > 0.0f) {
                pending_.zoomSteps += std::log(d / lastPinch_) * 6.0f;
            }
            lastPinch_ = d;
        }
        return;
    }

    // Up or cancel.
    const Role role = f->role;
    if (role == Role::Stats && touch.kind == Kind::Up && (p - f->start).length() < 0.06f * s) {
        pending_.toggleStats = true;
    }
    fingers_.erase(fingers_.begin() + (f - fingers_.data()));
    if (role == Role::Pinch) lastPinch_ = 0.0f;  // a new second finger starts a new pinch
}

void TouchControls::releaseAll() {
    fingers_.clear();
    lastPinch_ = 0.0f;
}

void TouchControls::setVisible(bool visible) {
    visible_ = visible;
    if (visible) root_.show();
    else root_.hide();
    if (!visible) releaseAll();
}

TouchControls::Output TouchControls::take() {
    Output out = pending_;
    pending_ = Output();
    for (const Finger &f : fingers_) {
        if (f.role != Role::Stick) continue;
        const LVector2f offset = f.pos - stickHome_;
        const float len = offset.length();
        if (len > kDeadZone * kStickRadius * scale_) {
            out.x = offset[0] / len;
            out.y = offset[1] / len;
        }
    }
    return out;
}

void TouchControls::update(double now, float dashReady) {
    now_ = now;
    if (!visible_) return;

    const Finger *stick = nullptr;
    for (const Finger &f : fingers_) {
        if (f.role == Role::Stick) stick = &f;
    }
    // The stick stays where it is; only the knob follows the thumb.
    if (stick != nullptr) {
        stick_.set_alpha_scale(0.8f);
        LVector2f offset = stick->pos - stickHome_;
        const float max = kStickRadius * scale_;
        if (offset.length() > max) offset *= max / offset.length();
        stickKnob_.set_pos(offset[0] / scale_, 0.0f, offset[1] / scale_);
    } else {
        stick_.set_alpha_scale(0.45f);
        stickKnob_.set_pos(0.0f, 0.0f, 0.0f);
    }

    const float ready = std::clamp(dashReady, 0.0f, 1.0f);
    dashFill_.set_scale(kDashRadius * 0.86f * std::max(ready, 0.04f));
    dashFill_.set_color(ready >= 1.0f ? LColor(0.3f, 0.9f, 1.0f, 0.55f) : LColor(0.6f, 0.68f, 0.75f, 0.4f), 1);
    float press = 1.0f;
    if (dashPressedAt_ >= 0.0 && now - dashPressedAt_ < kPressPulse) {
        press = 0.88f + 0.12f * static_cast<float>((now - dashPressedAt_) / kPressPulse);
    }
    dash_.set_scale(scale_ * press);
    dash_.set_alpha_scale(ready >= 1.0f ? 0.95f : 0.7f);
}

} // namespace arena::game
