#include "game/World.h"

#include "core/Log.h"
#include "game/Meshes.h"
#include "game/Scenery.h"

#include "ambientLight.h"
#include "camera.h"
#include "clockObject.h"
#include "colorBlendAttrib.h"
#include "colorWriteAttrib.h"
#include "cullFaceAttrib.h"
#include "directionalLight.h"
#include "fog.h"
#include "lens.h"
#include "shaderAttrib.h"
#include "shaderInput.h"
#include "textNode.h"
#include "transparencyAttrib.h"
#include "virtualFileSystem.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace arena::game {

namespace {

// The shaders are GLSL 1.50. With OpenGL ES (Android) the same source compiles
// as GLSL ES 3.00 once it says so and declares its precisions.
std::string shaderSource(const std::string &text) {
#ifdef ARENA_GLES
    const std::string desktop = "#version 150";
    if (text.compare(0, desktop.size(), desktop) == 0) {
        return "#version 300 es\n"
               "precision highp float;\nprecision highp int;\nprecision highp sampler2DShadow;" +
               text.substr(desktop.size());
    }
#endif
    return text;
}

PT(Shader) loadShader(const Filename &dir, const std::string &name) {
    VirtualFileSystem *vfs = VirtualFileSystem::get_global_ptr();
    std::string vert, frag;
    if (!vfs->read_file(Filename(dir, name + ".vert"), vert, true) ||
        !vfs->read_file(Filename(dir, name + ".frag"), frag, true)) {
        return nullptr;
    }
    return Shader::make(Shader::SL_GLSL, shaderSource(vert), shaderSource(frag));
}

constexpr float kPi = 3.14159265f;
constexpr float kRadToDeg = 57.29577951f;

// The main camera sees bit 0; the sun's shadow camera bit 1. Thin things
// (grass, effects, labels) are hidden from the shadow camera.
const BitMask32 kMainMask = BitMask32::bit(0);
const BitMask32 kShadowMask = BitMask32::bit(1);

// The look (sRGB colours; the light intensities are linear).
const LColor kHorizon(0.74f, 0.86f, 0.95f, 1.0f);
const LVecBase3f kSkyTop(0.30f, 0.56f, 0.92f);
const LVecBase3f kSunColor(1.08f, 1.0f, 0.86f);
const LVecBase3f kSkyAmbient(0.30f, 0.37f, 0.50f);
const LVecBase3f kGroundAmbient(0.19f, 0.21f, 0.11f);
const LPoint3 kSunFrom(-30.0f, -38.0f, 56.0f);  // the sun is up there (south-west, high)

const LColor kOrbColor(0.45f, 1.0f, 0.85f, 1.0f);
const LColor kGoldColor(1.0f, 0.80f, 0.25f, 1.0f);

// Additive, unlit, no depth writes, fixed-function: glows and effects.
void makeGlow(NodePath np) {
    np.set_light_off(1);
    np.set_shader_off(1);
    np.set_depth_write(false);
    np.set_attrib(ColorBlendAttrib::make(ColorBlendAttrib::M_add, ColorBlendAttrib::O_incoming_alpha,
                                         ColorBlendAttrib::O_one));
    np.set_bin("fixed", 5);
    np.hide(kShadowMask);
}

LColor lighter(const LColor &c, float k) {
    return LColor(c[0] + (1 - c[0]) * k, c[1] + (1 - c[1]) * k, c[2] + (1 - c[2]) * k, 1.0f);
}

LColor darker(const LColor &c, float k) { return LColor(c[0] * k, c[1] * k, c[2] * k, 1.0f); }

} // namespace

LColor World::playerColor(int index) {
    static const LColor palette[12] = {
        {0.95f, 0.30f, 0.30f, 1}, {0.25f, 0.60f, 1.00f, 1}, {0.35f, 0.85f, 0.35f, 1},
        {1.00f, 0.80f, 0.20f, 1}, {0.75f, 0.40f, 1.00f, 1}, {1.00f, 0.50f, 0.15f, 1},
        {0.20f, 0.90f, 0.85f, 1}, {1.00f, 0.45f, 0.75f, 1}, {0.70f, 0.90f, 0.25f, 1},
        {0.95f, 0.95f, 0.95f, 1}, {0.45f, 0.45f, 0.95f, 1}, {0.85f, 0.65f, 0.45f, 1},
    };
    return palette[((index % 12) + 12) % 12];
}

World::World(NodePath render, NodePath camera, const WorldSettings &settings)
    : render_(render), camera_(camera), settings_(settings) {
    root_ = render_.attach_new_node("world");
    staticRoot_ = root_.attach_new_node("static");
    orbRoot_ = root_.attach_new_node("orbs");
    playerRoot_ = root_.attach_new_node("players");
    fxRoot_ = root_.attach_new_node("effects");
    lifeRoot_ = root_.attach_new_node("life");
    fxRoot_.hide(kShadowMask);

    NodePath cam = camera_.find("**/+Camera");
    if (!cam.is_empty()) {
        DCAST(Camera, cam.node())->set_camera_mask(kMainMask);
    }

    glowTex_ = meshes::glowTexture(64);

    // Shared models; per-object state lives on wrapper nodes above instances.
    boxModel_ = NodePath(scenery::box("box"));
    gemModel_ = NodePath(scenery::gem("gem"));
    wingModel_ = NodePath(scenery::wing("wing"));
    haloModel_ = NodePath(meshes::card("halo"));
    haloModel_.set_texture(glowTex_);
    makeGlow(haloModel_);
    puffModel_ = NodePath(meshes::card("puff"));
    puffModel_.set_texture(glowTex_);
    puffModel_.set_transparency(TransparencyAttrib::M_alpha);
    puffModel_.set_depth_write(false);
    puffModel_.set_light_off(1);
    puffModel_.set_shader_off(1);
    puffModel_.set_billboard_point_eye();
    ringModel_ = NodePath(meshes::ring("ring", 0.82f, 1.0f, 48));
    ringModel_.set_two_sided(true);
    discModel_ = NodePath(meshes::card("blob"));
    discModel_.set_texture(glowTex_);
    discModel_.set_p(-90);
    discModel_.set_transparency(TransparencyAttrib::M_alpha);
    discModel_.set_depth_write(false);
    discModel_.set_light_off(1);
    discModel_.set_shader_off(1);

    setupLights();
    if (settings_.shaders) {
        loadShaders();
    }
    setupSky();

    camera_.set_pos(0.0f, -30.0f, 30.0f);
    camera_.look_at(0.0f, 0.0f, 0.0f);
    // The scenery is built when the server sends the arena (setArena).
}

World::~World() {
    root_.remove_node();
    sky_.remove_node();
}

// ----------------------------------------------------------- light & sky

void World::setupLights() {
    // The sun: the key light, with the shadow map when shaders are on. Its
    // shadow pass renders depth only, without our shaders.
    PT(DirectionalLight) sun = new DirectionalLight("sun");
    sun->set_color(LColor(1.0f, 0.93f, 0.8f, 1.0f));
    sun->set_camera_mask(kShadowMask);
    sun->set_initial_state(RenderState::make(CullFaceAttrib::make_reverse(),
                                              ColorWriteAttrib::make(ColorWriteAttrib::C_off),
                                              ShaderAttrib::make_off(), 1000));
    sunNp_ = root_.attach_new_node(sun);
    sunNp_.set_pos(kSunFrom);
    sunNp_.look_at(0.0f, 0.0f, 0.0f);
    render_.set_light(sunNp_);

    // Fixed-function only (the shader has its own sky/ground ambient).
    PT(AmbientLight) ambient = new AmbientLight("ambient");
    ambient->set_color(LColor(0.46f, 0.5f, 0.55f, 1.0f));
    ambientNp_ = root_.attach_new_node(ambient);
    render_.set_light(ambientNp_);
    PT(DirectionalLight) fill = new DirectionalLight("fill");
    fill->set_color(LColor(0.16f, 0.2f, 0.28f, 1.0f));
    fillNp_ = root_.attach_new_node(fill);
    fillNp_.set_hpr(160.0f, -30.0f, 0.0f);
    render_.set_light(fillNp_);

    PT(Fog) fog = new Fog("haze");
    fog->set_color(kHorizon);
    fog->set_linear_range(60.0f, 220.0f);
    render_.set_fog(fog);
}

void World::loadShaders() {
    const Filename dir(settings_.shaderDir);
    VirtualFileSystem *vfs = VirtualFileSystem::get_global_ptr();
    if (settings_.shaderDir.empty() || !vfs->exists(Filename(dir, "world.vert"))) {
        LOG_WARN("no shaders found: fixed-function lighting");
        return;
    }
    worldShader_ = loadShader(dir, "world");
    skyShader_ = loadShader(dir, "sky");
    if (worldShader_ == nullptr || skyShader_ == nullptr) {
        LOG_WARN("could not load the shaders: fixed-function lighting");
        worldShader_ = skyShader_ = nullptr;
        return;
    }
    shadersOn_ = true;
    render_.clear_fog();
    root_.set_shader(worldShader_);
    root_.set_shader_input(ShaderInput("sunColor", kSunColor));
    root_.set_shader_input(ShaderInput("skyColor", kSkyAmbient));
    root_.set_shader_input(ShaderInput("groundColor", kGroundAmbient));
    root_.set_shader_input(ShaderInput("fogColor", LVecBase3f(kHorizon[0], kHorizon[1], kHorizon[2])));
    root_.set_shader_input(ShaderInput("fogRange", LVecBase2f(45.0f, 190.0f)));
    root_.set_shader_input(ShaderInput("windStrength", LVecBase4f(1.0f, 0, 0, 0)));
    root_.set_shader_input(ShaderInput("flash", LVecBase4f(0.0f, 0, 0, 0)));
    const float texel = 1.0f / static_cast<float>(std::max(settings_.shadowSize, 256));
    root_.set_shader_input(ShaderInput("shadowTexel", LVecBase2f(texel, texel)));
    root_.set_shader_input(ShaderInput("shadowStrength", LVecBase4f(settings_.shadows ? 0.82f : 0.0f, 0, 0, 0)));
    if (settings_.shadows) {
        DCAST(DirectionalLight, sunNp_.node())->set_shadow_caster(true, settings_.shadowSize, settings_.shadowSize);
    }
}

bool World::verifyShaders() {
    if (!shadersOn_ || (!worldShader_->get_error_flag() && !skyShader_->get_error_flag())) {
        return true;
    }
    LOG_WARN("the shaders did not compile on this driver: fixed-function lighting (see --low)");
    shadersOn_ = false;
    root_.clear_shader();
    DCAST(DirectionalLight, sunNp_.node())->set_shadow_caster(false);
    sky_.clear_shader();
    sky_.hide();
    PT(Fog) fog = new Fog("haze");
    fog->set_color(kHorizon);
    fog->set_linear_range(60.0f, 220.0f);
    render_.set_fog(fog);
    return false;
}

void World::setupSky() {
    // A dome around the camera that keeps the world's orientation, drawn
    // first and behind everything.
    sky_ = camera_.attach_new_node(meshes::sphere("sky", 32, 16));
    sky_.set_compass();
    sky_.set_scale(300.0f);
    sky_.set_bin("background", 0);
    sky_.set_depth_write(false);
    sky_.set_depth_test(false);
    sky_.set_two_sided(true);
    sky_.set_light_off(1);
    sky_.set_fog_off(1);
    sky_.hide(kShadowMask);
    if (!shadersOn_) {
        sky_.hide();  // the window's clear colour is the sky
        return;
    }
    LVector3f toSun(kSunFrom[0], kSunFrom[1], kSunFrom[2]);
    toSun.normalize();
    sky_.set_shader(skyShader_);
    sky_.set_shader_input(ShaderInput("skyTop", kSkyTop));
    sky_.set_shader_input(ShaderInput("fogColor", LVecBase3f(kHorizon[0], kHorizon[1], kHorizon[2])));
    sky_.set_shader_input(ShaderInput("sunColor", LVecBase3f(1.0f, 0.9f, 0.7f)));
    sky_.set_shader_input(ShaderInput("sunDirWorld", LVecBase3f(toSun)));
}

// ---------------------------------------------------------------- scenery

void World::setArena(const ArenaLayout &layout, const Rules &rules) {
    rules_ = rules;
    if (built_ && layout == layout_) {
        return;  // the same arena (a reconnect): keep the scenery
    }
    layout_ = layout;
    built_ = true;
    buildStatic();
    buildLife();
}

void World::buildStatic() {
    staticRoot_.get_children().detach();
    const float R = layout_.radius;

    // Shadow frustum just big enough for the arena and the trees around it.
    if (auto *sun = DCAST(DirectionalLight, sunNp_.node())) {
        if (Lens *lens = sun->get_lens()) {
            lens->set_film_size(R * 2.7f, R * 2.7f);
            lens->set_near_far(20.0f, 170.0f);
        }
    }

    const double t0 = ClockObject::get_global_clock()->get_real_time();
    scenery::Parts parts = scenery::build(staticRoot_, layout_, 20240611u);
    parts.soft.hide(kShadowMask);  // grass and flowers: too thin to shadow
    LOG_INFO("scenery: %d vertices in 2 batches, built in %.0f ms", parts.vertices,
             (ClockObject::get_global_clock()->get_real_time() - t0) * 1000.0);
}

// Butterflies over the meadow and pollen drifting in the sun.
void World::buildLife() {
    lifeRoot_.get_children().detach();
    butterflies_.clear();
    motes_.clear();
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float R = layout_.radius;
    static const LColor wings[] = {{1.0f, 0.62f, 0.15f, 1}, {0.98f, 0.92f, 0.35f, 1}, {0.55f, 0.75f, 1.0f, 1},
                                   {1.0f, 1.0f, 1.0f, 1}, {0.9f, 0.45f, 0.8f, 1}};
    for (int i = 0; i < 9; ++i) {
        Butterfly b;
        const float a = u(rng_) * 2.0f * kPi, r = R * (0.5f + 0.7f * u(rng_));
        b.pos = LPoint3(r * std::cos(a), r * std::sin(a), 1.0f + u(rng_) * 1.5f);
        b.target = b.pos;
        b.phase = u(rng_) * 10.0f;
        b.root = lifeRoot_.attach_new_node("butterfly");
        b.root.set_scale(0.22f);
        b.root.hide(kShadowMask);
        const LColor &c = wings[i % 5];
        b.wingL = b.root.attach_new_node("wingL");
        wingModel_.instance_to(b.wingL);
        b.wingL.set_color(c);
        b.wingR = b.root.attach_new_node("wingR");
        wingModel_.instance_to(b.wingR);
        b.wingR.set_color(c);
        b.wingR.set_sx(-1.0f);  // mirrored
        NodePath bodyNp = b.root.attach_new_node("body");
        boxModel_.instance_to(bodyNp);
        bodyNp.set_scale(0.08f, 0.6f, 0.08f);
        bodyNp.set_color(LColor(0.2f, 0.15f, 0.1f, 1));
        butterflies_.push_back(b);
    }
    for (int i = 0; i < 46; ++i) {
        Mote m;
        const float a = u(rng_) * 2.0f * kPi, r = (R + 6.0f) * std::sqrt(u(rng_));
        m.base = LPoint3(r * std::cos(a), r * std::sin(a), 0.5f + u(rng_) * 3.5f);
        m.phase = u(rng_) * 20.0f;
        m.speed = 0.3f + 0.4f * u(rng_);
        m.np = lifeRoot_.attach_new_node("mote");
        haloModel_.instance_to(m.np);
        m.np.set_billboard_point_eye();
        m.np.set_scale(0.06f + 0.05f * u(rng_));
        m.np.set_color(LColor(1.0f, 0.97f, 0.8f, 0.45f));
        motes_.push_back(m);
    }
}

void World::clear() {
    playerRoot_.get_children().detach();
    orbRoot_.get_children().detach();
    fxRoot_.get_children().detach();
    players_.clear();
    orbs_.clear();
    effects_.clear();
}

// ------------------------------------------------------------------- orbs

void World::setOrbs(const std::unordered_map<uint16_t, Orb> &orbs) {
    orbRoot_.get_children().detach();
    orbs_.clear();
    for (const auto &kv : orbs) {
        addOrb(kv.first, kv.second);
    }
}

void World::addOrb(uint16_t id, const Orb &orb) {
    if (orbs_.count(id)) {
        orbs_[id].root.remove_node();
    }
    OrbVisual v;
    v.kind = orb.kind;
    v.phase = std::uniform_real_distribution<float>(0.0f, 6.28f)(rng_);
    v.root = orbRoot_.attach_new_node("orb");
    v.root.set_pos(orb.x, orb.y, 0.0f);
    v.bob = v.root.attach_new_node("bob");
    const LColor color = orb.kind ? kGoldColor : kOrbColor;
    const float size = rules_.orbRadius * (orb.kind ? 1.3f : 1.0f);
    // A glowing gem: unlit (it shines), with a soft halo.
    v.gem = v.bob.attach_new_node("gem");
    gemModel_.instance_to(v.gem);
    v.gem.set_scale(size * 0.75f, size * 0.75f, size * 1.05f);
    v.gem.set_color(lighter(color, 0.15f));
    v.gem.set_light_off(1);
    v.gem.set_shader_off(1);
    v.gem.hide(kShadowMask);  // light does not cast shadows
    NodePath halo = v.bob.attach_new_node("halo");
    haloModel_.instance_to(halo);
    halo.set_scale(size * 3.2f);
    halo.set_color(LColor(color[0], color[1], color[2], orb.kind ? 0.85f : 0.6f));
    halo.set_billboard_point_eye();
    // Its light on the grass.
    NodePath glow = v.root.attach_new_node("glow");
    discModel_.instance_to(glow);
    glow.set_z(0.03f);
    glow.set_scale(size * 2.6f);
    glow.set_color(LColor(color[0], color[1], color[2], 0.35f));
    glow.set_attrib(ColorBlendAttrib::make(ColorBlendAttrib::M_add, ColorBlendAttrib::O_incoming_alpha,
                                           ColorBlendAttrib::O_one));
    glow.hide(kShadowMask);
    spawnRing(orb.x, orb.y, color, 0.45, 0.2f, size * 3.0f);
    orbs_[id] = v;
}

void World::collectOrb(uint16_t id, bool byLocalPlayer) {
    auto it = orbs_.find(id);
    if (it == orbs_.end()) {
        return;
    }
    const LPoint3 pos = it->second.root.get_pos();
    const LColor color = it->second.kind ? kGoldColor : kOrbColor;
    spawnRing(pos[0], pos[1], color, 0.5, 0.3f, byLocalPlayer ? 2.6f : 1.6f);
    spawnEffect(gemModel_, pos[0], pos[1], 0.9f, 0.35, rules_.orbRadius, rules_.orbRadius * 2.2f, color, 2.5f);
    it->second.root.remove_node();
    orbs_.erase(it);
}

void World::setOrbHidden(uint16_t id, bool hidden) {
    auto it = orbs_.find(id);
    if (it != orbs_.end()) {
        if (hidden) it->second.root.hide();
        else it->second.root.show();
    }
}

// ----------------------------------------------------------------- players

void World::addPlayer(uint32_t id, const std::string &name, int color, bool local) {
    removePlayer(id);
    const float r = rules_.playerRadius;
    const float k = r / 0.8f;  // the character is sized for a 0.8 m player
    PlayerVisual v;
    v.color = playerColor(color);
    v.idlePhase = static_cast<float>(id % 97) * 0.37f;
    v.root = playerRoot_.attach_new_node("player");
    v.root.set_shader_input(ShaderInput("flash", LVecBase4f(0.0f, 0, 0, 0)));

    auto part = [&](NodePath parent, const LPoint3 &pos, const LVecBase3 &size, const LColor &c) {
        NodePath np = parent.attach_new_node("part");
        boxModel_.instance_to(np);
        np.set_pos(pos * k);
        np.set_scale(size * k);
        np.set_color(c);
        return np;
    };
    const LColor body = v.color, limb = darker(v.color, 0.78f);
    const LColor pants(0.22f, 0.24f, 0.32f, 1), shoe(0.16f, 0.12f, 0.1f, 1);
    const LColor face(0.17f, 0.15f, 0.2f, 1), eye(0.92f, 0.98f, 1.0f, 1);

    // Legs hang from the hips and swing.
    v.legL = v.root.attach_new_node("legL");
    v.legL.set_pos(-0.17f * k, 0.0f, 0.5f * k);
    part(v.legL, LPoint3(0, 0, -0.22f), LVecBase3(0.2f, 0.22f, 0.44f), pants);
    part(v.legL, LPoint3(0, 0.03f, -0.45f), LVecBase3(0.22f, 0.3f, 0.1f), shoe);
    v.legR = v.root.attach_new_node("legR");
    v.legR.set_pos(0.17f * k, 0.0f, 0.5f * k);
    part(v.legR, LPoint3(0, 0, -0.22f), LVecBase3(0.2f, 0.22f, 0.44f), pants);
    part(v.legR, LPoint3(0, 0.03f, -0.45f), LVecBase3(0.22f, 0.3f, 0.1f), shoe);

    // The body bobs and leans from the hips; arms swing from the shoulders.
    v.body = v.root.attach_new_node("body");
    v.body.set_z(0.5f * k);
    part(v.body, LPoint3(0, 0, 0.27f), LVecBase3(0.62f, 0.4f, 0.56f), body);
    part(v.body, LPoint3(0, 0, 0.03f), LVecBase3(0.64f, 0.42f, 0.08f), darker(v.color, 0.55f));  // belt
    v.armL = v.body.attach_new_node("armL");
    v.armL.set_pos(-0.41f * k, 0.0f, 0.5f * k);
    part(v.armL, LPoint3(0, 0, -0.2f), LVecBase3(0.17f, 0.19f, 0.42f), limb);
    part(v.armL, LPoint3(0, 0, -0.44f), LVecBase3(0.15f, 0.17f, 0.1f), lighter(v.color, 0.55f));
    v.armR = v.body.attach_new_node("armR");
    v.armR.set_pos(0.41f * k, 0.0f, 0.5f * k);
    part(v.armR, LPoint3(0, 0, -0.2f), LVecBase3(0.17f, 0.19f, 0.42f), limb);
    part(v.armR, LPoint3(0, 0, -0.44f), LVecBase3(0.15f, 0.17f, 0.1f), lighter(v.color, 0.55f));
    // A big blocky head with a dark visor and bright eyes (+Y is forward).
    v.head = v.body.attach_new_node("head");
    v.head.set_z(0.88f * k);
    part(v.head, LPoint3(0, 0, 0), LVecBase3(0.74f, 0.68f, 0.62f), lighter(v.color, 0.12f));
    part(v.head, LPoint3(0, 0.33f, -0.02f), LVecBase3(0.58f, 0.06f, 0.38f), face);
    part(v.head, LPoint3(-0.13f, 0.365f, 0.0f), LVecBase3(0.1f, 0.03f, 0.15f), eye);
    part(v.head, LPoint3(0.13f, 0.365f, 0.0f), LVecBase3(0.1f, 0.03f, 0.15f), eye);
    part(v.head, LPoint3(0, 0, 0.34f), LVecBase3(0.5f, 0.46f, 0.08f), darker(v.color, 0.8f));  // cap

    if (!shadersOn_ || !settings_.shadows) {
        NodePath blob = v.root.attach_new_node("blob");
        discModel_.instance_to(blob);
        blob.set_z(0.03f);
        blob.set_scale(r * 1.2f);
        blob.set_color(LColor(0, 0, 0, 0.45f));
        blob.hide(kShadowMask);
    }
    if (local) {
        NodePath marker = v.root.attach_new_node("marker");
        ringModel_.instance_to(marker);
        marker.set_z(0.04f);
        marker.set_scale(r * 1.35f);
        marker.set_color(LColor(1.0f, 1.0f, 1.0f, 0.85f));
        marker.set_light_off(1);
        marker.set_shader_off(1);
        marker.set_transparency(TransparencyAttrib::M_alpha);
        marker.hide(kShadowMask);
    }

    PT(TextNode) text = new TextNode("tag");
    text->set_text(name);
    text->set_align(TextNode::A_center);
    text->set_text_color(local ? LColor(1.0f, 0.92f, 0.45f, 1.0f) : LColor(1, 1, 1, 1));
    text->set_shadow(0.06f, 0.06f);
    text->set_shadow_color(LColor(0, 0, 0, 0.8f));
    v.tag = v.root.attach_new_node(text);
    v.tag.set_scale(0.5f);
    v.tag.set_z(2.05f * k + 0.25f);
    v.tag.set_billboard_point_eye();
    v.tag.set_light_off(1);
    v.tag.set_shader_off(1);
    v.tag.set_depth_test(false);
    v.tag.set_depth_write(false);
    v.tag.set_bin("fixed", 20);
    v.tag.hide(kShadowMask);

    players_[id] = v;
}

void World::removePlayer(uint32_t id) {
    auto it = players_.find(id);
    if (it != players_.end()) {
        it->second.root.remove_node();
        players_.erase(it);
    }
}

void World::setPlayerPose(uint32_t id, float x, float y, float h) {
    auto it = players_.find(id);
    if (it == players_.end()) {
        return;
    }
    PlayerVisual &v = it->second;
    if (!v.hasPose) {
        v.lastX = x;
        v.lastY = y;
        v.hasPose = true;
    }
    v.x = x;
    v.y = y;
    v.root.set_pos(x, y, 0.0f);
    v.root.set_h(h);
}

void World::playerDashed(uint32_t id) {
    auto it = players_.find(id);
    if (it == players_.end()) {
        return;
    }
    it->second.dashUntil = now_ + 0.28;
    spawnRing(it->second.x, it->second.y, lighter(it->second.color, 0.4f), 0.35, 0.6f, 2.2f);
    spawnPuff(it->second.x, it->second.y, 0.2f, 0.9f);
}

void World::playerHit(uint32_t id) {
    auto it = players_.find(id);
    if (it == players_.end()) {
        return;
    }
    it->second.flash = 1.0f;
    spawnRing(it->second.x, it->second.y, LColor(1.0f, 0.95f, 0.6f, 1.0f), 0.5, 0.8f, 3.2f);
    for (int i = 0; i < 3; ++i) spawnPuff(it->second.x, it->second.y, 0.8f + 0.3f * i, 0.7f);
}

// Walk cycle from how fast the drawn player moves: legs and arms swing, the
// body bobs and leans into the run, dashes stretch it, hits flash it.
void World::animatePlayer(PlayerVisual &v, float dt) {
    const float k = rules_.playerRadius / 0.8f;
    if (dt > 0.0f) {
        const float moved = std::hypot(v.x - v.lastX, v.y - v.lastY);
        const float measured = std::min(moved / dt, 40.0f);
        v.speed += (measured - v.speed) * (1.0f - std::exp(-dt * 12.0f));
    }
    v.lastX = v.x;
    v.lastY = v.y;
    const float run = std::clamp(v.speed / rules_.maxSpeed, 0.0f, 1.0f);
    v.walk = std::fmod(v.walk + v.speed * dt * 3.6f / k, 2.0f * kPi);
    const bool dashing = now_ < v.dashUntil;
    const float swing = std::sin(v.walk) * 38.0f * run;
    const float breathe = std::sin(static_cast<float>(now_) * 2.4f + v.idlePhase);
    v.legL.set_p(swing);
    v.legR.set_p(-swing);
    if (dashing) {
        v.armL.set_p(70.0f);
        v.armR.set_p(70.0f);
        v.body.set_p(-24.0f);
        v.body.set_scale(1.0f, 1.12f, 0.92f);
    } else {
        v.armL.set_p(-swing * 0.85f + breathe * 3.0f * (1.0f - run));
        v.armR.set_p(swing * 0.85f + breathe * 3.0f * (1.0f - run));
        v.armL.set_r(-4.0f - 3.0f * (1.0f - run));
        v.armR.set_r(4.0f + 3.0f * (1.0f - run));
        v.body.set_p(-9.0f * run);
        v.body.set_scale(1.0f);
    }
    v.body.set_z((0.5f + std::fabs(std::cos(v.walk)) * 0.06f * run + breathe * 0.012f * (1.0f - run)) * k);
    v.head.set_r(std::sin(v.walk) * 3.0f * run);
    if (dashing && now_ - v.lastTrail > 0.05) {
        v.lastTrail = now_;
        spawnPuff(v.x, v.y, 0.15f, 0.55f);
    }
    if (v.flash > 0.0f) {
        v.flash = std::max(0.0f, v.flash - dt * 4.0f);
        if (shadersOn_) {
            v.root.set_shader_input(ShaderInput("flash", LVecBase4f(v.flash * 0.85f, 0, 0, 0)));
        } else {
            const float f = 1.0f + v.flash * 1.5f;
            v.root.set_color_scale(f, f, f, 1.0f);
        }
    }
}

// ----------------------------------------------------------------- effects

void World::spawnEffect(NodePath model, float x, float y, float z, double life, float from, float to,
                        const LColor &color, float rise) {
    Effect e;
    e.np = fxRoot_.attach_new_node("fx");
    model.instance_to(e.np);
    e.np.set_pos(x, y, z);
    e.np.set_scale(from);
    e.np.set_color(color);
    makeGlow(e.np);
    e.start = now_;
    e.life = life;
    e.growFrom = from;
    e.growTo = to;
    e.rise = rise;
    effects_.push_back(e);
}

void World::spawnRing(float x, float y, const LColor &color, double life, float from, float to) {
    spawnEffect(ringModel_, x, y, 0.06f, life, from, to, color);
}

// A soft dust puff (alpha blended, not additive: it is daylight).
void World::spawnPuff(float x, float y, float z, float size) {
    std::uniform_real_distribution<float> u(-0.3f, 0.3f);
    Effect e;
    e.np = fxRoot_.attach_new_node("puff");
    puffModel_.instance_to(e.np);
    e.np.set_pos(x + u(rng_), y + u(rng_), z);
    e.np.set_scale(size * 0.5f);
    e.np.set_color(LColor(0.93f, 0.88f, 0.74f, 0.7f));
    e.start = now_;
    e.life = 0.5;
    e.growFrom = size * 0.5f;
    e.growTo = size * 1.4f;
    e.rise = 0.6f;
    effects_.push_back(e);
}

void World::shake(float amount) { shake_ = std::max(shake_, amount); }

void World::zoom(float steps) { zoom_ = std::clamp(zoom_ * std::pow(0.9f, steps), 0.55f, 1.6f); }

void World::setZoom(float zoom) { zoom_ = std::clamp(zoom, 0.55f, 1.6f); }

void World::updateLife(float dt) {
    const float t = static_cast<float>(now_);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float R = layout_.radius;
    for (Butterfly &b : butterflies_) {
        // Flutter toward a wandering target near the grass.
        if ((b.target - b.pos).length() < 0.8f) {
            const float a = u(rng_) * 2.0f * kPi, r = R * (0.35f + 0.8f * u(rng_));
            b.target = LPoint3(r * std::cos(a), r * std::sin(a), 0.6f + 1.8f * u(rng_));
        }
        LVector3 want = b.target - b.pos;
        want.normalize();
        want *= 1.8f;
        b.vel += (want - b.vel) * std::min(1.0f, dt * 1.5f);
        const LVector3 wobble(std::sin(t * 3.1f + b.phase), std::cos(t * 2.7f + b.phase), std::sin(t * 4.3f + b.phase));
        b.pos += (b.vel + wobble * 0.7f) * dt;
        b.root.set_pos(b.pos);
        if (b.vel.length_squared() > 1e-4f) {
            b.root.set_h(std::atan2(-b.vel[0], b.vel[1]) * kRadToDeg);
        }
        const float flap = 15.0f + 60.0f * std::fabs(std::sin(t * 13.0f + b.phase));
        b.wingL.set_r(-flap);
        b.wingR.set_r(flap);
    }
    // Pollen drifts in the sun, around the camera's focus.
    for (Mote &m : motes_) {
        const float s = t * m.speed + m.phase;
        LPoint3 p = m.base + LVector3(std::sin(s * 0.7f) * 1.2f + s * 0.25f, std::cos(s * 0.5f) * 1.2f,
                                      std::sin(s * 1.3f) * 0.4f);
        // Wrapped into a box around the focus, so there is always some nearby.
        for (int i = 0; i < 2; ++i) {
            p[i] = camFocus_[i] + std::remainder(p[i] - camFocus_[i], 44.0f);
        }
        m.np.set_pos(p);
    }
}

void World::update(double now, float dt, bool haveFocus, float focusX, float focusY) {
    now_ = now;

    for (auto &kv : orbs_) {
        OrbVisual &v = kv.second;
        const float t = static_cast<float>(now) * 2.2f + v.phase;
        v.bob.set_z(0.9f + 0.16f * std::sin(t));
        v.gem.set_h(static_cast<float>(now) * 70.0f + v.phase * 57.0f);
    }

    for (auto &kv : players_) {
        animatePlayer(kv.second, dt);
    }
    updateLife(dt);

    for (size_t i = 0; i < effects_.size();) {
        Effect &e = effects_[i];
        const float t = static_cast<float>((now - e.start) / e.life);
        if (t >= 1.0f) {
            e.np.remove_node();
            effects_[i] = effects_.back();
            effects_.pop_back();
            continue;
        }
        e.np.set_scale(e.growFrom + (e.growTo - e.growFrom) * t);
        e.np.set_alpha_scale(1.0f - t);
        if (e.rise != 0.0f) e.np.set_z(e.np.get_z() + e.rise * dt);
        ++i;
    }

    // Chase camera: fixed angle (so WASD = screen directions), smooth follow.
    const LPoint3 target = haveFocus ? LPoint3(focusX, focusY, 0.0f) : LPoint3(0, 0, 0);
    const float k = 1.0f - std::exp(-dt * 7.0f);
    camFocus_ += (target - camFocus_) * k;
    // Closer is lower, like over a shoulder; further out is more top-down.
    const float dist = 27.0f * zoom_;
    const float pitch = 0.36f + 0.26f * zoom_;  // ~36 degrees by default
    LPoint3 eye = camFocus_ + LVector3(0.0f, -std::cos(pitch) * dist, std::sin(pitch) * dist);
    if (shake_ > 0.001f) {
        std::uniform_real_distribution<float> u(-1.0f, 1.0f);
        eye += LVector3(u(rng_), u(rng_), u(rng_)) * shake_;
        shake_ *= std::exp(-dt * 7.0f);
    }
    camera_.set_pos(eye);
    camera_.look_at(camFocus_ + LVector3(0, 0, 1.6f));
}

} // namespace arena::game
