// The 3D scene: a sunny meadow arena in a forest valley (Scenery), blocky
// animated characters, glowing gems, butterflies and drifting pollen, a sky,
// and the chase camera. Pure presentation; GameClient tells it what exists
// and where.
//
// With shaders (shaders/world.*, sky.*, loaded from the resource pack) the
// world gets soft sun light with filtered shadow maps, sky/ground ambient
// light, drifting cloud shadows, a rim light, wind in the grass and leaves,
// and haze toward the horizon. Without (--low, or no GLSL), the same scene
// is lit by Panda's fixed-function lights.
#pragma once

#include "game/ArenaGame.h"
#include "game/Movement.h"

#include "nodePath.h"
#include "pointerTo.h"
#include "shader.h"
#include "texture.h"

#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace arena::game {

struct WorldSettings {
    bool shaders = true;     // GLSL lighting (otherwise fixed-function)
    bool shadows = true;     // sun shadow map (with shaders)
    int shadowSize = 2048;   // shadow map texels per side
    std::string shaderDir;   // Panda path of world.vert etc. (the mounted pack)
};

class World {
public:
    World(NodePath render, NodePath camera, const WorldSettings &settings);
    ~World();
    World(const World &) = delete;
    World &operator=(const World &) = delete;

    static LColor playerColor(int index);

    // After the first frame: falls back to fixed-function lighting if the
    // shaders did not compile on this driver. Returns false if it fell back.
    bool verifyShaders();
    bool usingShaders() const { return shadersOn_; }

    void setArena(const ArenaLayout &layout, const Rules &rules);
    void clear();  // players + orbs (e.g. after a disconnect)

    void setOrbs(const std::unordered_map<uint16_t, Orb> &orbs);
    void addOrb(uint16_t id, const Orb &orb);
    void collectOrb(uint16_t id, bool byLocalPlayer);
    void setOrbHidden(uint16_t id, bool hidden);

    void addPlayer(uint32_t id, const std::string &name, int color, bool local);
    void removePlayer(uint32_t id);
    void setPlayerPose(uint32_t id, float x, float y, float h);
    void playerDashed(uint32_t id);
    void playerHit(uint32_t id);

    void shake(float amount);
    void zoom(float steps);
    void setZoom(float zoom);
    // Animations, effects and the chase camera around (focusX, focusY).
    void update(double now, float dt, bool haveFocus, float focusX, float focusY);

private:
    struct PlayerVisual {
        NodePath root, body, head, armL, armR, legL, legR, tag;
        float x = 0, y = 0, lastX = 0, lastY = 0;
        float speed = 0.0f;      // smoothed, m/s
        float walk = 0.0f;       // walk cycle phase (radians)
        float flash = 0.0f;
        float idlePhase = 0.0f;
        bool hasPose = false;
        double lastTrail = 0.0;
        LColor color;
        double dashUntil = 0.0;
    };
    struct OrbVisual {
        NodePath root, bob, gem;
        float phase;
        int kind;
    };
    struct Effect {
        NodePath np;
        double start, life;
        float growFrom, growTo;
        float rise;  // m/s upward
    };
    struct Butterfly {
        NodePath root, wingL, wingR;
        LPoint3 pos, target;
        LVector3 vel;
        float phase;
    };
    struct Mote {
        NodePath np;
        LPoint3 base;
        float phase, speed;
    };

    void setupLights();
    void setupSky();
    void loadShaders();
    void buildStatic();
    void buildLife();
    void spawnEffect(NodePath model, float x, float y, float z, double life, float from, float to,
                     const LColor &color, float rise = 0.0f);
    void spawnRing(float x, float y, const LColor &color, double life, float from, float to);
    void spawnPuff(float x, float y, float z, float size);
    void animatePlayer(PlayerVisual &v, float dt);
    void updateLife(float dt);

    NodePath render_, camera_, root_, staticRoot_, orbRoot_, playerRoot_, fxRoot_, lifeRoot_;
    NodePath sunNp_, ambientNp_, fillNp_, sky_;
    WorldSettings settings_;
    PT(Shader) worldShader_, skyShader_;
    bool shadersOn_ = false;

    NodePath boxModel_, gemModel_, haloModel_, ringModel_, discModel_, puffModel_, wingModel_;
    PT(Texture) glowTex_;

    ArenaLayout layout_;
    Rules rules_;
    bool built_ = false;
    std::unordered_map<uint32_t, PlayerVisual> players_;
    std::unordered_map<uint16_t, OrbVisual> orbs_;
    std::vector<Effect> effects_;
    std::vector<Butterfly> butterflies_;
    std::vector<Mote> motes_;

    LPoint3 camFocus_{0, 0, 0};
    float zoom_ = 1.0f;
    float shake_ = 0.0f;
    double now_ = 0.0;
    std::mt19937 rng_{12345};
};

} // namespace arena::game
