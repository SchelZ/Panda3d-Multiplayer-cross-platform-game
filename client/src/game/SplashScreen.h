// The engine's logo at start-up, as games made with Unreal show theirs: the
// Panda3D head and "PANDA3D" in white on the logo's dark grey, while the game
// loads behind it (the world is built, shaders compile, the menu looks for
// servers), then it fades out.
//
// Everything is drawn here, nothing is an image: the head is polygons
// (traced from Panda3D's own icon, panda/src/configfiles/pandaIcon.ico), the
// word is the logo's typeface (Roboto Bold) rendered at the screen's own
// resolution, and the background is a plain card. So it is sharp on every
// screen size and shape, there is no picture to load and decode, and it takes
// next to no memory: a few dozen triangles and one small glyph texture.
#pragma once

#include "filename.h"
#include "luse.h"
#include "nodePath.h"

namespace arena::game {

class SplashScreen {
public:
    static const LColor kBackground;  // the logo's dark grey (#252525)
    static constexpr double kMinShow = 1.8;   // seconds, at least
    static constexpr double kFadeIn = 0.4, kFadeOut = 0.5;

    // render2d: the whole window (the background); aspect2d: the logo, which
    // keeps its shape. aspect: window width / height; windowHeight in pixels,
    // so the word is rendered at the size it is shown. font: Roboto Bold (the
    // default font if it cannot be loaded).
    SplashScreen(NodePath render2d, NodePath aspect2d, float aspect, int windowHeight, const Filename &font,
                 double now);
    ~SplashScreen();
    SplashScreen(const SplashScreen &) = delete;
    SplashScreen &operator=(const SplashScreen &) = delete;

    // Animates it; false once it has faded out (then it can go). ready: the
    // game behind it has loaded, so it may fade out (after kMinShow).
    bool update(double now, bool ready);
    // The logo, e.g. for tests: its two white shapes (the outline around the
    // head, and the face with its eyes left out).
    NodePath logo() const { return logo_; }
    NodePath word() const { return word_; }

private:
    void layout(float aspect, int windowHeight, const Filename &font);

    NodePath background_, root_, logo_, word_;
    double start_ = 0.0, fadeOutAt_ = -1.0;
};

} // namespace arena::game
