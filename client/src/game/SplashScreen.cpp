#include "game/SplashScreen.h"

#include "antialiasAttrib.h"
#include "cardMaker.h"
#include "cullBinManager.h"
#include "dynamicTextFont.h"
#include "geom.h"
#include "geomNode.h"
#include "geomTriangles.h"
#include "geomVertexData.h"
#include "geomVertexFormat.h"
#include "geomVertexWriter.h"
#include "textNode.h"
#include "transparencyAttrib.h"
#include "triangulator.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace arena::game {

const LColor SplashScreen::kBackground(37.0f / 255.0f, 37.0f / 255.0f, 37.0f / 255.0f, 1.0f);

namespace {

// The logo, 1 high (outline included), centred, y up. Traced from Panda3D's
// pandaIcon.ico (256 px): mirrored and averaged, as the logo is symmetric;
// the white outline (about 5% of the width) and the slightly bolder dark
// lines are those of the logo on dark backgrounds.
const std::vector<LPoint2f> kOutline = {
    {0.0000f, 0.5000f}, {-0.2801f, 0.3385f}, {-0.3614f, 0.3786f}, {-0.5127f, 0.2911f}, {-0.5134f, 0.1085f},
    {-0.4411f, 0.0693f}, {-0.4411f, -0.2522f}, {0.0000f, -0.5000f}, {0.4411f, -0.2522f}, {0.4411f, 0.0693f},
    {0.5134f, 0.1085f}, {0.5127f, 0.2911f}, {0.3614f, 0.3786f}, {0.2801f, 0.3385f}};
const std::vector<LPoint2f> kHead = {  // head and ears, inside the outline (dark)
    {0.0000f, 0.4442f}, {-0.2784f, 0.2837f}, {-0.3596f, 0.3238f}, {-0.4644f, 0.2631f}, {-0.4649f, 0.1372f},
    {-0.3927f, 0.0981f}, {-0.3927f, -0.2239f}, {0.0000f, -0.4445f}, {0.3927f, -0.2239f}, {0.3927f, 0.0981f},
    {0.4649f, 0.1372f}, {0.4644f, 0.2631f}, {0.3596f, 0.3238f}, {0.2784f, 0.2837f}};
const std::vector<LPoint2f> kFace = {  // inside the dark ring, with the nose and mouth cut out
    {-0.3215f, 0.1787f}, {-0.3215f, -0.1800f}, {-0.0336f, -0.3415f}, {-0.0336f, -0.2951f}, {-0.1078f, -0.2479f},
    {-0.0772f, -0.1622f}, {0.0772f, -0.1622f}, {0.1078f, -0.2479f}, {0.0336f, -0.2951f}, {0.0336f, -0.3415f},
    {0.3215f, -0.1800f}, {0.3215f, 0.1787f}, {0.0000f, 0.3619f}};
const std::vector<LPoint2f> kEyeLeft = {{-0.1409f, 0.1272f}, {-0.1921f, 0.0630f}, {-0.2693f, 0.0219f},
                                        {-0.2693f, -0.1366f}, {-0.1655f, -0.1366f}, {-0.0587f, -0.0112f},
                                        {-0.0587f, 0.0781f}};
const std::vector<LPoint2f> kEyeRight = {{0.0587f, 0.0781f}, {0.0587f, -0.0112f}, {0.1655f, -0.1366f},
                                         {0.2693f, -0.1366f}, {0.2693f, 0.0219f}, {0.1921f, 0.0630f},
                                         {0.1409f, 0.1272f}};
constexpr float kLogoAspect = 1.0268f;  // width / height of kOutline

// Proportions of the logo with its word (from the logo image): the word's
// capitals are 0.562 of the head's height, 0.256 of it away; Roboto Bold set
// 4% tighter than it comes (the logo used an older, narrower Roboto).
constexpr float kCapOfLogo = 0.562f, kGapOfLogo = 0.256f, kTracking = -0.042f;
constexpr char kWord[] = "PANDA3D";

// A white shape: the first contour, minus the others (holes).
PT(Geom) shape(const std::vector<const std::vector<LPoint2f> *> &contours) {
    Triangulator t;
    for (size_t c = 0; c < contours.size(); ++c) {
        if (c > 0) t.begin_hole();
        for (const LPoint2f &p : *contours[c]) {
            const int index = t.add_vertex(p[0], p[1]);
            if (c == 0) t.add_polygon_vertex(index);
            else t.add_hole_vertex(index);
        }
    }
    t.triangulate();
    PT(GeomVertexData) data = new GeomVertexData("logo", GeomVertexFormat::get_v3(), Geom::UH_static);
    data->unclean_set_num_rows(t.get_num_vertices());
    GeomVertexWriter vertex(data, InternalName::get_vertex());
    for (int i = 0; i < t.get_num_vertices(); ++i) {
        const LPoint2d &v = t.get_vertex(i);
        vertex.set_data3(static_cast<float>(v[0]), 0.0f, static_cast<float>(v[1]));
    }
    PT(GeomTriangles) triangles = new GeomTriangles(Geom::UH_static);
    for (int n = 0; n < t.get_num_triangles(); ++n) {
        triangles->add_vertices(t.get_triangle_v0(n), t.get_triangle_v1(n), t.get_triangle_v2(n));
    }
    PT(Geom) geom = new Geom(data);
    geom->add_primitive(triangles);
    return geom;
}

PT(TextFont) loadFont(const Filename &file, float pixelsPerUnit) {
    if (!file.empty()) {
        PT(DynamicTextFont) font = new DynamicTextFont(file);
        if (font->is_valid()) {
            font->set_pixels_per_unit(pixelsPerUnit);
            // Drawn texel for pixel: no mipmaps needed.
            font->set_minfilter(SamplerState::FT_linear);
            font->set_magfilter(SamplerState::FT_linear);
            const int glyph = static_cast<int>(pixelsPerUnit) + 8;
            int page = 256;
            while (page < glyph * 2 && page < 2048) page *= 2;  // the five letters fit
            font->set_page_size(page, page);
            return font;
        }
    }
    return TextNode::get_default_font();
}

// A bin drawn after all the others (the HUD and the menu are in "unsorted").
const char *splashBin() {
    CullBinManager *bins = CullBinManager::get_global_ptr();
    if (bins->find_bin("splash") < 0) bins->add_bin("splash", CullBinManager::BT_fixed, 10000);
    return "splash";
}

float smooth(double x) {
    const float t = static_cast<float>(std::clamp(x, 0.0, 1.0));
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

SplashScreen::SplashScreen(NodePath render2d, NodePath aspect2d, float aspect, int windowHeight,
                           const Filename &font, double now)
    : start_(now) {
    // The background: the whole window, whatever its shape.
    CardMaker card("splash-background");
    card.set_frame(-1.0f, 1.0f, -1.0f, 1.0f);
    background_ = render2d.attach_new_node(card.generate());
    background_.set_color(kBackground, 1);
    background_.set_bin(splashBin(), 0);

    root_ = aspect2d.attach_new_node("splash");
    PT(GeomNode) logo = new GeomNode("splash-logo");
    logo->add_geom(shape({&kOutline, &kHead}));                  // the white outline round the head
    logo->add_geom(shape({&kFace, &kEyeLeft, &kEyeRight}));      // the white face, without the eyes
    logo_ = root_.attach_new_node(logo);
    logo_.set_color(LColor(1.0f, 1.0f, 1.0f, 1.0f), 1);
    logo_.set_two_sided(true);
    logo_.set_antialias(AntialiasAttrib::M_multisample);  // smooth edges where there is MSAA
    logo_.set_bin(splashBin(), 1);
    word_ = root_.attach_new_node("splash-word");
    word_.set_bin(splashBin(), 2);
    for (NodePath np : {background_, root_}) {
        np.set_transparency(TransparencyAttrib::M_alpha);
        np.set_depth_test(false);
        np.set_depth_write(false);
    }
    layout(aspect, windowHeight, font);
    update(now, false);
}

SplashScreen::~SplashScreen() {
    background_.remove_node();
    root_.remove_node();
}

void SplashScreen::layout(float aspect, int windowHeight, const Filename &font) {
    // Logo and word side by side, centred: half the window's width, at most
    // 28% of its height.
    const float width = 1.0f * kLogoAspect + kGapOfLogo + 5.75f * kCapOfLogo;  // per logo height
    const float h = std::min(aspect / width, 0.56f);
    const float left = -0.5f * width * h;
    logo_.set_pos(left + 0.5f * kLogoAspect * h, 0.0f, 0.0f);
    logo_.set_scale(h);

    // The word: each letter placed by hand, for the logo's tighter spacing.
    // First its size in text units, with a probe; then the font again, with
    // its glyphs rendered at exactly the size they are drawn.
    const float cap = kCapOfLogo * h;
    PT(TextFont) probeFont = loadFont(font, 64.0f);
    PT(TextNode) probe = new TextNode("probe");
    probe->set_font(probeFont);
    probe->set_text("P");
    NodePath probeNp(probe->generate());
    LPoint3 lo, hi;
    float capUnits = 0.7f, inkLeft = 0.0f, inkBottom = 0.0f;
    if (probeNp.calc_tight_bounds(lo, hi)) {
        // Glyph cards reach poly-margin pixels past the ink.
        float margin = 0.0f;
        if (probeFont->is_of_type(DynamicTextFont::get_class_type())) {
            margin = DCAST(DynamicTextFont, probeFont.p())->get_poly_margin() / 64.0f;
        }
        capUnits = std::max(0.1f, (hi[2] - lo[2]) - 2.0f * margin);
        inkLeft = lo[0] + margin;
        inkBottom = lo[2] + margin;
    }
    const float scale = cap / capUnits;  // aspect2d units per text unit
    PT(TextFont) wordFont = loadFont(font, scale * windowHeight * 0.5f);
    float x = left + (kLogoAspect + kGapOfLogo) * h - inkLeft * scale;
    const float baseline = -0.5f * cap - inkBottom * scale;
    for (const char *c = kWord; *c != '\0'; ++c) {
        PT(TextNode) letter = new TextNode(std::string("letter-") + *c);
        letter->set_font(wordFont);
        letter->set_text(std::string(1, *c));
        letter->set_text_color(LColor(1.0f, 1.0f, 1.0f, 1.0f));
        NodePath np = word_.attach_new_node(letter);
        np.set_pos(x, 0.0f, baseline);
        np.set_scale(scale);
        CPT(TextGlyph) glyph;
        const float advance = wordFont->get_glyph(*c, glyph) && glyph != nullptr ? glyph->get_advance() : capUnits;
        x += (advance + kTracking * capUnits) * scale;
    }
}

bool SplashScreen::update(double now, bool ready) {
    const double t = now - start_;
    if (fadeOutAt_ < 0.0 && ready && t >= kMinShow) fadeOutAt_ = now;
    float all = 1.0f;
    if (fadeOutAt_ >= 0.0) {
        const double f = (now - fadeOutAt_) / kFadeOut;
        if (f >= 1.0) {
            background_.hide();
            root_.hide();
            return false;
        }
        all = 1.0f - smooth(f);
    }
    // The logo fades in over the background, then all of it fades out.
    root_.set_alpha_scale(smooth(t / kFadeIn) * all);
    background_.set_alpha_scale(all);
    return true;
}

} // namespace arena::game
