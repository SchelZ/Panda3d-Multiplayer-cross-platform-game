// The start-up logo: its shapes, its layout on different screens, and when it
// goes away.
#include "Test.h"

#include "game/SplashScreen.h"

#include "geom.h"
#include "geomNode.h"
#include "geomPrimitive.h"
#include "nodePath.h"

using namespace arena;

namespace {

const Filename kFont = Filename::from_os_specific(ARENA_FONT_FILE);

} // namespace

ARENA_TEST(splash_logo_is_two_white_shapes) {
    NodePath render2d("render2d"), aspect2d("aspect2d");
    game::SplashScreen splash(render2d, aspect2d, 16.0f / 9.0f, 1080, kFont, 0.0);
    GeomNode *logo = DCAST(GeomNode, splash.logo().node());
    CHECK(logo->get_num_geoms() == 2);  // the outline round the head; the face without the eyes
    int triangles = 0;
    for (int i = 0; i < logo->get_num_geoms(); ++i) {
        triangles += logo->get_geom(i)->get_primitive(0)->get_num_primitives();
    }
    CHECK(triangles >= 24 && triangles <= 80);
    CHECK(splash.word().get_num_children() == 7);  // P A N D A 3 D
}

ARENA_TEST(splash_is_centred_and_half_the_screen_wide) {
    for (float aspect : {4.0f / 3.0f, 16.0f / 9.0f, 2280.0f / 1080.0f}) {
        NodePath render2d("render2d"), aspect2d("aspect2d");
        game::SplashScreen splash(render2d, aspect2d, aspect, 1080, kFont, 0.0);
        LPoint3 lo, hi;
        CHECK(splash.logo().get_parent().calc_tight_bounds(lo, hi));
        CHECK_NEAR(lo[0] + hi[0], 0.0, 0.04 * aspect);  // centred
        CHECK_NEAR(lo[2] + hi[2], 0.0, 0.02);
        CHECK_NEAR(hi[0] - lo[0], aspect, 0.06 * aspect);  // half of the 2 * aspect wide window
        CHECK(hi[2] - lo[2] <= 0.57f);                     // and at most ~28% of its height
    }
}

ARENA_TEST(splash_word_is_set_like_the_logo) {
    NodePath render2d("render2d"), aspect2d("aspect2d");
    game::SplashScreen splash(render2d, aspect2d, 16.0f / 9.0f, 1080, kFont, 0.0);
    LPoint3 logoLo, logoHi, lo, hi;
    CHECK(splash.logo().calc_tight_bounds(logoLo, logoHi));
    CHECK(splash.word().calc_tight_bounds(lo, hi));
    const double logoHeight = logoHi[2] - logoLo[2];
    const double cap = hi[2] - lo[2];  // capitals only (glyph cards add a pixel or two)
    CHECK_NEAR(cap / logoHeight, 0.562, 0.03);
    CHECK_NEAR((hi[0] - lo[0]) / cap, 5.75, 0.25);  // Roboto Bold, set as tight as the logo's
    CHECK_NEAR((lo[0] - logoHi[0]) / logoHeight, 0.256, 0.03);
}

ARENA_TEST(splash_stays_until_loaded_then_fades_out) {
    NodePath render2d("render2d"), aspect2d("aspect2d");
    game::SplashScreen splash(render2d, aspect2d, 16.0f / 9.0f, 720, kFont, 10.0);
    const NodePath root = splash.logo().get_parent();
    CHECK(root.get_color_scale()[3] < 0.05f);  // fades in
    CHECK(splash.update(10.2, false));
    CHECK(splash.update(11.0, false));
    CHECK_NEAR(root.get_color_scale()[3], 1.0, 1e-4);
    CHECK(splash.update(13.0, false));  // the game has not loaded: it stays
    CHECK_NEAR(root.get_color_scale()[3], 1.0, 1e-4);
    CHECK(splash.update(13.1, true));   // loaded (and shown long enough): fades out
    CHECK(splash.update(13.35, true));
    CHECK(root.get_color_scale()[3] < 0.9f && root.get_color_scale()[3] > 0.1f);
    CHECK(!splash.update(13.65, true));  // gone
}

ARENA_TEST(splash_shows_at_least_its_minimum_time) {
    NodePath render2d("render2d"), aspect2d("aspect2d");
    game::SplashScreen splash(render2d, aspect2d, 16.0f / 9.0f, 720, kFont, 0.0);
    CHECK(splash.update(0.5, true));  // loaded at once: still shown
    CHECK(splash.update(game::SplashScreen::kMinShow - 0.01, true));
    CHECK(splash.update(game::SplashScreen::kMinShow + 0.01, true));
    CHECK(!splash.update(game::SplashScreen::kMinShow + game::SplashScreen::kFadeOut + 0.05, true));
}
