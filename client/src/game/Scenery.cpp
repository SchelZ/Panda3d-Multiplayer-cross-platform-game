#include "game/Scenery.h"

#include "cullFaceAttrib.h"
#include "geom.h"
#include "geomTriangles.h"
#include "geomVertexData.h"
#include "geomVertexFormat.h"
#include "geomVertexWriter.h"
#include "internalName.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <random>
#include <utility>
#include <vector>

namespace arena::game::scenery {

namespace {

constexpr float kTwoPi = 6.28318531f;
using P3 = LPoint3f;
using V3 = LVector3f;
using C4 = LColorf;

// Flat-shaded triangles with vertex colours (the faceted low-poly look):
// every triangle has its own normal. Colour alpha = 1 - wind sway. Vertices
// are collected in one array and copied into a single, non-indexed vertex
// buffer at the end (no per-vertex Panda calls, no index buffer).
class Mesh {
public:
    explicit Mesh(const std::string &name) : name_(name) { verts_.reserve(4096); }

    // Counter-clockwise seen from the front; the normal follows the winding.
    void tri(const P3 &a, const P3 &b, const P3 &c, const C4 &col, float sa = 0, float sb = 0, float sc = 0) {
        V3 n = (b - a).cross(c - a);
        const float len = n.length();
        if (len < 1e-9f) return;
        triN(a, b, c, n / len, col, sa, sb, sc);
    }

    // With a given normal (grass and leaves are lit like the ground they grow on).
    void triN(const P3 &a, const P3 &b, const P3 &c, const V3 &n, const C4 &col, float sa = 0, float sb = 0,
              float sc = 0) {
        const P3 p[3] = {a, b, c};
        const float s[3] = {sa, sb, sc};
        auto byte = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
        for (int i = 0; i < 3; ++i) {
            verts_.push_back(Vertex{p[i][0], p[i][1], p[i][2], n[0], n[1], n[2], byte(col[0]), byte(col[1]),
                                    byte(col[2]), byte(1.0f - s[i])});
        }
    }

    // Seen from both sides with backface culling still on: both windings,
    // one normal.
    void twoSided(const P3 &a, const P3 &b, const P3 &c, const V3 &n, const C4 &col, float sa = 0, float sb = 0,
                  float sc = 0) {
        triN(a, b, c, n, col, sa, sb, sc);
        triN(a, c, b, n, col, sa, sc, sb);
    }

    void quad(const P3 &a, const P3 &b, const P3 &c, const P3 &d, const C4 &col) {
        tri(a, b, c, col);
        tri(a, c, d, col);
    }

    bool empty() const { return verts_.empty(); }
    int vertices() const { return static_cast<int>(verts_.size()); }

    PT(GeomNode) finish() {
        static CPT(GeomVertexFormat) format = [] {
            PT(GeomVertexArrayFormat) array = new GeomVertexArrayFormat;
            array->add_column(InternalName::get_vertex(), 3, Geom::NT_float32, Geom::C_point);
            array->add_column(InternalName::get_normal(), 3, Geom::NT_float32, Geom::C_normal);
            array->add_column(InternalName::get_color(), 4, Geom::NT_uint8, Geom::C_color);
            return GeomVertexFormat::register_format(new GeomVertexFormat(array));
        }();
        PT(GeomVertexData) data = new GeomVertexData(name_, format, Geom::UH_static);
        data->unclean_set_num_rows(static_cast<int>(verts_.size()));
        {
            PT(GeomVertexArrayDataHandle) handle = data->modify_array(0)->modify_handle();
            nassertr(handle->get_array_format()->get_stride() == sizeof(Vertex), nullptr);
            handle->copy_data_from(reinterpret_cast<const unsigned char *>(verts_.data()),
                                   verts_.size() * sizeof(Vertex));
        }
        PT(GeomTriangles) tris = new GeomTriangles(Geom::UH_static);
        tris->add_consecutive_vertices(0, static_cast<int>(verts_.size()));
        PT(Geom) geom = new Geom(data);
        geom->add_primitive(tris);
        PT(GeomNode) node = new GeomNode(name_);
        node->add_geom(geom);
        verts_.clear();
        verts_.shrink_to_fit();
        return node;
    }

private:
    struct Vertex {  // matches the format above: 28 bytes, interleaved
        float x, y, z, nx, ny, nz;
        uint8_t r, g, b, a;
    };
    static_assert(sizeof(Vertex) == 28, "vertex layout");
    std::vector<Vertex> verts_;
    std::string name_;
};

// ------------------------------------------------------------------ noise

uint32_t hashU(int x, int y, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

float hashF(int x, int y, uint32_t seed) { return (hashU(x, y, seed) & 0xffffff) / 16777215.0f; }

float valueNoise(float x, float y, uint32_t seed) {
    const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
    const float fx = x - xi, fy = y - yi;
    const float ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
    const float a = hashF(xi, yi, seed), b = hashF(xi + 1, yi, seed);
    const float c = hashF(xi, yi + 1, seed), d = hashF(xi + 1, yi + 1, seed);
    return (a + (b - a) * ux) + ((c + (d - c) * ux) - (a + (b - a) * ux)) * uy;
}

float fbm(float x, float y, uint32_t seed) {
    float sum = 0.0f, amp = 0.5f;
    for (int i = 0; i < 4; ++i) {
        sum += valueNoise(x, y, seed + i) * amp;
        x = x * 2.03f + 7.1f;
        y = y * 2.03f + 3.7f;
        amp *= 0.5f;
    }
    return sum;
}

C4 mix(const C4 &a, const C4 &b, float t) { return a + (b - a) * std::clamp(t, 0.0f, 1.0f); }
C4 shade(const C4 &c, float k) { return C4(c[0] * k, c[1] * k, c[2] * k, 1.0f); }

// Palette (sRGB): a warm, saturated storybook forest.
const C4 kGrassDark(0.26f, 0.46f, 0.18f, 1), kGrassLight(0.50f, 0.70f, 0.28f, 1);
const C4 kMeadow(0.42f, 0.63f, 0.25f, 1), kMeadowLight(0.56f, 0.73f, 0.32f, 1);
const C4 kPath(0.70f, 0.66f, 0.46f, 1);
const C4 kEarth(0.62f, 0.40f, 0.24f, 1), kRock(0.58f, 0.60f, 0.62f, 1), kMoss(0.40f, 0.62f, 0.20f, 1);
const C4 kBark(0.45f, 0.29f, 0.18f, 1), kWood(0.76f, 0.60f, 0.40f, 1);
const C4 kLeaf(0.34f, 0.66f, 0.22f, 1), kLeafLight(0.55f, 0.82f, 0.28f, 1);
const C4 kPine(0.16f, 0.45f, 0.30f, 1), kPineLight(0.24f, 0.58f, 0.34f, 1);

// ------------------------------------------------------------- polyhedra

struct Poly {
    std::vector<V3> v;
    std::vector<std::array<int, 3>> f;
};

Poly icosahedron() {
    const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
    Poly p;
    p.v = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
           {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    for (V3 &v : p.v) v.normalize();
    p.f = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
           {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
           {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    return p;
}

Poly subdivide(const Poly &in) {
    Poly out;
    out.v = in.v;
    std::map<std::pair<int, int>, int> mid;
    auto middle = [&](int a, int b) {
        const auto key = std::minmax(a, b);
        auto it = mid.find(key);
        if (it != mid.end()) return it->second;
        V3 m = out.v[a] + out.v[b];
        m.normalize();
        out.v.push_back(m);
        return mid[key] = static_cast<int>(out.v.size()) - 1;
    };
    for (const auto &f : in.f) {
        const int a = middle(f[0], f[1]), b = middle(f[1], f[2]), c = middle(f[2], f[0]);
        out.f.push_back({f[0], a, c});
        out.f.push_back({f[1], b, a});
        out.f.push_back({f[2], c, b});
        out.f.push_back({a, b, c});
    }
    return out;
}

const Poly &ico(int level) {
    static const Poly p0 = icosahedron(), p1 = subdivide(p0);
    return level == 0 ? p0 : p1;
}

// A lumpy blob: a jittered icosphere scaled by `radii` and turned by `yaw`.
// colorOf(face normal, face centre) picks each face's colour; sway(vertex
// height 0..1) how much the wind moves it.
template <class ColorOf, class SwayOf>
void blob(Mesh &m, const P3 &centre, const V3 &radii, float yaw, int level, uint32_t seed, float jitter,
          ColorOf colorOf, SwayOf swayOf) {
    const Poly &p = ico(level);
    const float c = std::cos(yaw), s = std::sin(yaw);
    std::vector<P3> pts(p.v.size());
    for (size_t i = 0; i < p.v.size(); ++i) {
        const V3 &u = p.v[i];
        const float k = 1.0f + (hashF(static_cast<int>(i), 17, seed) - 0.5f) * 2.0f * jitter;
        const float x = u[0] * radii[0] * k, y = u[1] * radii[1] * k, z = u[2] * radii[2] * k;
        pts[i] = centre + V3(x * c - y * s, x * s + y * c, z);
    }
    for (const auto &f : p.f) {
        const P3 &a = pts[f[0]], &b = pts[f[1]], &d = pts[f[2]];
        V3 n = (b - a).cross(d - a);
        if (n.length_squared() < 1e-12f) continue;
        n.normalize();
        const C4 col = colorOf(n, (a + b + d) / 3.0f);
        auto h = [&](const P3 &q) { return (q[2] - centre[2]) / std::max(radii[2], 1e-3f) * 0.5f + 0.5f; };
        m.tri(a, b, d, col, swayOf(h(a)), swayOf(h(b)), swayOf(h(d)));
    }
}

// A tapered prism (cone when r1 = 0) from `base` up by h, leaning by `lean`.
void frustum(Mesh &m, const P3 &base, float r0, float r1, float h, int sides, float yaw, const V3 &lean,
             const C4 &col, bool capTop, const C4 &capCol, float swayTop = 0.0f) {
    for (int i = 0; i < sides; ++i) {
        const float a0 = yaw + kTwoPi * i / sides, a1 = yaw + kTwoPi * (i + 1) / sides;
        const P3 b0 = base + V3(std::cos(a0) * r0, std::sin(a0) * r0, 0);
        const P3 b1 = base + V3(std::cos(a1) * r0, std::sin(a1) * r0, 0);
        const P3 top = base + lean + V3(0, 0, h);
        const P3 t0 = top + V3(std::cos(a0) * r1, std::sin(a0) * r1, 0);
        const P3 t1 = top + V3(std::cos(a1) * r1, std::sin(a1) * r1, 0);
        const C4 c = shade(col, 0.92f + 0.16f * hashF(i, sides, static_cast<uint32_t>(base[0] * 31 + base[1] * 17)));
        if (r1 <= 0.0f) {
            m.tri(b0, b1, top, c, 0, 0, swayTop);
        } else {
            m.tri(b0, b1, t1, c, 0, 0, swayTop);
            m.tri(b0, t1, t0, c, 0, swayTop, swayTop);
            if (capTop) m.tri(top, t0, t1, capCol, swayTop, swayTop, swayTop);
        }
    }
}

// --------------------------------------------------------------- things

void rock(Mesh &m, const P3 &at, const V3 &size, float yaw, uint32_t seed) {
    const float tone = 0.88f + 0.2f * hashF(1, 2, seed);
    blob(m, at + V3(0, 0, size[2] * 0.25f), size, yaw, 0, seed, 0.22f,
         [&](const V3 &n, const P3 &c) {
             const float k = 0.9f + 0.18f * hashF(static_cast<int>(c[0] * 7), static_cast<int>(c[1] * 7), seed);
             const C4 stone = shade(kRock, tone * k);
             return n[2] > 0.55f ? mix(stone, kMoss, (n[2] - 0.55f) * 3.0f) : stone;
         },
         [](float) { return 0.0f; });
}

void broadleaf(Mesh &m, const P3 &at, float height, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float trunkH = height * (0.45f + 0.1f * u(rng));
    const V3 lean((u(rng) - 0.5f) * 0.6f, (u(rng) - 0.5f) * 0.6f, 0);
    const float r = 0.18f + height * 0.03f;
    frustum(m, at - V3(0, 0, 0.3f), r * 1.25f, r * 0.75f, trunkH + 0.3f, 6, u(rng), lean, kBark, false, kBark);
    // Roots: a few wedges at the foot.
    for (int i = 0; i < 4; ++i) {
        const float a = kTwoPi * (i + u(rng) * 0.5f) / 4.0f;
        const V3 d(std::cos(a), std::sin(a), 0);
        const V3 side(-d[1] * r * 0.5f, d[0] * r * 0.5f, 0);
        const P3 tip = at + d * r * 2.6f - V3(0, 0, 0.05f);
        m.tri(at + side, tip, at + V3(0, 0, 0.45f), shade(kBark, 0.95f));
        m.tri(at - side, at + V3(0, 0, 0.45f), tip, shade(kBark, 0.85f));
    }
    // Canopy: chunky faceted domes, lighter on top.
    const P3 top = at + lean + V3(0, 0, trunkH);
    const float cr = height * (0.32f + 0.08f * u(rng));
    const C4 green = mix(kLeaf, kLeafLight, u(rng) * 0.6f);
    const int blobs = 2 + static_cast<int>(u(rng) * 2.0f);
    for (int i = 0; i < blobs; ++i) {
        const float a = u(rng) * kTwoPi, d = i == 0 ? 0.0f : cr * 0.55f;
        const P3 c = top + V3(std::cos(a) * d, std::sin(a) * d, cr * (0.55f + 0.25f * u(rng)) - (i ? cr * 0.2f : 0));
        const float s = cr * (i == 0 ? 1.0f : 0.7f + 0.2f * u(rng));
        blob(m, c, V3(s, s, s * 0.78f), u(rng) * kTwoPi, 1, seed + 101 * i, 0.12f,
             [&](const V3 &n, const P3 &) { return shade(mix(green, kLeafLight, n[2] * 0.6f), 0.95f + 0.1f * u(rng)); },
             [](float h) { return 0.04f + 0.1f * h; });
    }
}

void pine(Mesh &m, const P3 &at, float height, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    frustum(m, at - V3(0, 0, 0.2f), 0.2f, 0.12f, height * 0.3f + 0.2f, 5, u(rng), V3(0, 0, 0), kBark, false, kBark);
    const int tiers = 3 + static_cast<int>(u(rng) * 1.5f);
    const C4 col = mix(kPine, kPineLight, u(rng));
    for (int i = 0; i < tiers; ++i) {
        const float f = static_cast<float>(i) / tiers;
        const float z = height * (0.2f + 0.62f * f);
        const float r = height * (0.34f - 0.22f * f);
        frustum(m, at + V3(0, 0, z), r, 0.0f, height * 0.36f, 7, u(rng) * kTwoPi, V3(0, 0, 0),
                shade(col, 0.9f + 0.15f * f), false, col, 0.08f);
        // The tier's underside, so it is not see-through from below.
        for (int k = 0; k < 7; ++k) {
            const float a0 = kTwoPi * k / 7, a1 = kTwoPi * (k + 1) / 7;
            m.tri(at + V3(0, 0, z), at + V3(std::cos(a1) * r, std::sin(a1) * r, z),
                  at + V3(std::cos(a0) * r, std::sin(a0) * r, z), shade(col, 0.6f));
        }
    }
}

void bush(Mesh &m, const P3 &at, float size, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const C4 green = mix(kLeaf, kGrassDark, u(rng));
    for (int i = 0; i < 2; ++i) {
        const P3 c = at + V3((u(rng) - 0.5f) * size, (u(rng) - 0.5f) * size, size * 0.35f);
        blob(m, c, V3(size, size * (0.8f + 0.3f * u(rng)), size * 0.7f), u(rng) * kTwoPi, 0, seed + i, 0.18f,
             [&](const V3 &n, const P3 &) { return mix(green, kLeafLight, n[2] * 0.5f); },
             [](float h) { return 0.12f * h; });
    }
}

// Fern: leaves arching out from the middle, lit like the ground.
void fern(Mesh &m, const P3 &at, float size, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const int leaves = 6 + static_cast<int>(u(rng) * 3.0f);
    const C4 col = mix(C4(0.30f, 0.62f, 0.22f, 1), C4(0.45f, 0.76f, 0.26f, 1), u(rng));
    for (int i = 0; i < leaves; ++i) {
        const float a = kTwoPi * (i + u(rng) * 0.4f) / leaves;
        const V3 d(std::cos(a), std::sin(a), 0), side(-std::sin(a), std::cos(a), 0);
        const float len = size * (0.8f + 0.4f * u(rng)), w = len * 0.16f;
        const P3 mid = at + d * len * 0.5f + V3(0, 0, len * 0.45f);
        const P3 tip = at + d * len + V3(0, 0, len * 0.12f);
        const V3 n = (V3(0, 0, 1) + d * 0.25f).normalized();
        m.twoSided(at, mid - side * w, mid + side * w, n, shade(col, 0.85f), 0, 0.3f, 0.3f);
        m.twoSided(mid - side * w, tip, mid + side * w, n, col, 0.3f, 0.7f, 0.3f);
    }
}

// A tuft of grass blades.
void tuft(Mesh &m, const P3 &at, float height, int blades, std::mt19937 &rng, const C4 &tint) {
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const V3 up(0, 0, 1);
    for (int i = 0; i < blades; ++i) {
        const float a = u(rng) * kTwoPi, r = u(rng) * 0.18f;
        const P3 base = at + V3(std::cos(a) * r, std::sin(a) * r, -0.02f);
        const float yaw = u(rng) * kTwoPi, h = height * (0.6f + 0.6f * u(rng));
        const V3 side(std::cos(yaw) * 0.045f, std::sin(yaw) * 0.045f, 0);
        const V3 lean(std::cos(a) * h * 0.25f, std::sin(a) * h * 0.25f, 0);
        const C4 tip = mix(tint, kGrassLight, 0.4f + 0.4f * u(rng));
        // Two-sided, the base darker than the tip, which the wind moves.
        m.twoSided(base - side, base + side, base + lean + V3(0, 0, h), up, mix(shade(tint, 0.75f), tip, 0.5f), 0,
                   0, 0.9f);
    }
}

void flower(Mesh &m, const P3 &at, const C4 &petal, std::mt19937 &rng) {
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float h = 0.22f + 0.15f * u(rng);
    const V3 up(0, 0, 1), side(0.02f, 0.0f, 0.0f);
    const P3 head = at + V3(0, 0, h);
    m.twoSided(at - side, at + side, head, up, kGrassDark, 0, 0, 0.6f);
    const int petals = 5;
    const float r = 0.07f + 0.03f * u(rng), spin = u(rng) * kTwoPi;
    for (int i = 0; i < petals; ++i) {
        const float a0 = spin + kTwoPi * i / petals, a1 = a0 + kTwoPi / petals * 0.5f, a2 = a0 + kTwoPi / petals;
        const P3 p0 = head + V3(std::cos(a0) * r * 0.4f, std::sin(a0) * r * 0.4f, 0);
        const P3 p1 = head + V3(std::cos(a1) * r, std::sin(a1) * r, 0.01f);
        const P3 p2 = head + V3(std::cos(a2) * r * 0.4f, std::sin(a2) * r * 0.4f, 0);
        m.twoSided(head, p0, p1, up, petal, 0.6f, 0.6f, 0.6f);
        m.twoSided(head, p1, p2, up, shade(petal, 0.92f), 0.6f, 0.6f, 0.6f);
    }
    m.twoSided(head + V3(-0.025f, -0.02f, 0.012f), head + V3(0.025f, -0.02f, 0.012f), head + V3(0, 0.03f, 0.012f),
               up, C4(1.0f, 0.82f, 0.25f, 1), 0.6f, 0.6f, 0.6f);
}

// The old stump in the middle of the arena, with moss and mushrooms.
void stump(Mesh &m, const P3 &at, float radius, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float h = 1.1f;
    frustum(m, at - V3(0, 0, 0.2f), radius * 1.06f, radius * 0.96f, h + 0.2f, 10, u(rng), V3(0, 0, 0), kBark, true,
            kWood);
    // Growth rings on the cut.
    for (int ring = 1; ring <= 2; ++ring) {
        const float rr = radius * 0.96f * ring / 3.0f;
        for (int i = 0; i < 10; ++i) {
            const float a0 = kTwoPi * i / 10, a1 = kTwoPi * (i + 1) / 10, w = 0.05f;
            const P3 c = at + V3(0, 0, h + 0.005f);
            m.quad(c + V3(std::cos(a0) * rr, std::sin(a0) * rr, 0), c + V3(std::cos(a1) * rr, std::sin(a1) * rr, 0),
                   c + V3(std::cos(a1) * (rr + w), std::sin(a1) * (rr + w), 0),
                   c + V3(std::cos(a0) * (rr + w), std::sin(a0) * (rr + w), 0), shade(kWood, 0.82f));
        }
    }
    // Roots spreading into the ground.
    for (int i = 0; i < 6; ++i) {
        const float a = kTwoPi * (i + u(rng) * 0.5f) / 6;
        const V3 d(std::cos(a), std::sin(a), 0), side(-d[1] * 0.35f, d[0] * 0.35f, 0);
        const P3 root = at + d * radius * 0.9f;
        const P3 tip = at + d * (radius + 0.9f + u(rng) * 0.5f) - V3(0, 0, 0.1f);
        m.tri(root + side, tip, root + V3(0, 0, 0.55f), shade(kBark, 0.9f));
        m.tri(root - side, root + V3(0, 0, 0.55f), tip, shade(kBark, 0.8f));
    }
    // Moss on the rim and mushrooms at the foot.
    blob(m, at + V3(radius * 0.6f, -radius * 0.5f, h - 0.05f), V3(0.7f, 0.5f, 0.18f), 0.3f, 0, seed + 7, 0.2f,
         [](const V3 &, const P3 &) { return kMoss; }, [](float) { return 0.0f; });
    for (int i = 0; i < 4; ++i) {
        const float a = -1.2f + i * 0.45f + u(rng) * 0.2f;
        const P3 base = at + V3(std::cos(a), std::sin(a), 0) * (radius + 0.25f + u(rng) * 0.3f);
        const float s = 0.18f + 0.12f * u(rng);
        frustum(m, base, s * 0.35f, s * 0.3f, s * 1.4f, 6, 0, V3(0, 0, 0), C4(0.95f, 0.92f, 0.84f, 1), false, kWood);
        frustum(m, base + V3(0, 0, s * 1.3f), s, 0.0f, s * 0.75f, 7, u(rng), V3(0, 0, 0), C4(0.86f, 0.24f, 0.18f, 1),
                false, kWood);
        frustum(m, base + V3(0, 0, s * 1.3f), s * 0.3f, s, 0.0f, 7, 0, V3(0, 0, 0), C4(0.95f, 0.9f, 0.8f, 1), false,
                kWood);  // underside
    }
}

// ------------------------------------------------------------- terrain

void terrain(Mesh &m, float arenaRadius, uint32_t seed) {
    const float edge = arenaRadius + 0.6f;
    std::vector<float> radii;
    for (float r = 0.0f; r < edge - 0.01f; r += 1.6f) radii.push_back(r);
    radii.push_back(edge);
    for (float r = edge + 1.0f, step = 1.0f; r < 190.0f; r += step, step *= 1.1f) radii.push_back(r);
    const int segments = 112;
    // Vertices jittered (except on the centre and the arena's edge), so the
    // facets do not line up in rings.
    auto point = [&](size_t ring, int s) {
        const float r0 = radii[ring];
        float r = r0, a = kTwoPi * s / segments;
        if (ring > 0 && std::fabs(r0 - edge) > 1e-3f) {
            const float step = (ring + 1 < radii.size() ? radii[ring + 1] : r0 + 4.0f) - r0;
            r += (hashF(static_cast<int>(ring), s % segments, seed + 5) - 0.5f) * step * 0.7f;
            a += (hashF(static_cast<int>(ring), s % segments, seed + 6) - 0.5f) * kTwoPi / segments * 0.7f;
        }
        const float x = r * std::cos(a), y = r * std::sin(a);
        return P3(x, y, groundHeight(x, y, arenaRadius));
    };
    auto colour = [&](const P3 &c, const V3 &n) {
        const float r = std::sqrt(c[0] * c[0] + c[1] * c[1]);
        const float patch = fbm(c[0] * 0.08f, c[1] * 0.08f, seed);
        const float fine = valueNoise(c[0] * 0.7f, c[1] * 0.7f, seed + 9);
        if (r <= edge + 0.05f) {
            C4 g = mix(kMeadow, kMeadowLight, patch * 1.4f - 0.2f);
            g = shade(g, 0.94f + 0.1f * fine);
            // Worn, sandy paths winding across the meadow.
            const float path = fbm(c[0] * 0.045f + 3.0f, c[1] * 0.045f - 1.0f, seed + 21);
            g = mix(g, shade(kPath, 0.95f + 0.1f * fine), (std::fabs(path - 0.5f) < 0.018f) ? 0.7f : 0.0f);
            // A darker ring of taller grass marks the edge.
            const float rim = std::clamp((r - (arenaRadius - 2.0f)) / 2.0f, 0.0f, 1.0f);
            return mix(g, kGrassDark, rim * 0.7f);
        }
        if (n[2] < 0.8f) {  // steep: bare earth, like terraces
            return shade(mix(kEarth, kGrassDark, (n[2] - 0.6f) * 3.0f), 0.9f + 0.15f * fine);
        }
        C4 g = mix(kGrassDark, kGrassLight, patch * 1.3f - 0.15f);
        return shade(g, 0.9f + 0.15f * fine);
    };
    // The centre: a fan.
    for (int s = 0; s < segments; ++s) {
        const P3 a(0, 0, 0), b = point(1, s), c = point(1, s + 1);
        m.tri(a, b, c, colour((a + b + c) / 3.0f, V3(0, 0, 1)));
    }
    for (size_t i = 1; i + 1 < radii.size(); ++i) {
        for (int s = 0; s < segments; ++s) {
            const P3 a = point(i, s), b = point(i + 1, s);
            const P3 c = point(i + 1, s + 1), d = point(i, s + 1);
            const P3 mid1 = (a + b + c) / 3.0f, mid2 = (a + c + d) / 3.0f;
            V3 n1 = (b - a).cross(c - a), n2 = (c - a).cross(d - a);
            n1.normalize();
            n2.normalize();
            m.tri(a, b, c, colour(mid1, n1));
            m.tri(a, c, d, colour(mid2, n2));
        }
    }
}

// Spots for things: random, at least `spacing` apart from each other and
// from what is already there.
struct Spots {
    std::vector<LVecBase3f> taken;  // x, y, radius
    bool free(float x, float y, float r) const {
        for (const auto &t : taken) {
            const float dx = x - t[0], dy = y - t[1], d = r + t[2];
            if (dx * dx + dy * dy < d * d) return false;
        }
        return true;
    }
};

} // namespace

float groundHeight(float x, float y, float arenaRadius) {
    const float r = std::sqrt(x * x + y * y);
    const float t = r - (arenaRadius + 0.6f);
    if (t <= 0.0f) return 0.0f;
    const float bank = 1.1f * (t < 6.0f ? t * t * (18.0f - 2.0f * t) / 216.0f : 1.0f);  // smoothstep 0..6
    const float slope = std::max(t - 5.0f, 0.0f) * 0.075f;
    const float hills = (fbm(x * 0.03f, y * 0.03f, 4242) - 0.5f) * 7.0f * std::clamp((t - 6.0f) / 24.0f, 0.0f, 1.0f);
    return bank + slope + hills;
}

Parts build(NodePath parent, const ArenaLayout &layout, uint32_t seed) {
    const float R = layout.radius;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    Mesh solid("scenery"), soft("plants");
    Spots spots;

    terrain(solid, R, seed);

    // Obstacles: the old stump in the middle, mossy boulders around.
    for (const Pillar &p : layout.pillars) {
        if (std::hypot(p.x, p.y) < 0.5f) {
            stump(solid, P3(p.x, p.y, 0), p.radius, seed + 1);
        } else {
            const uint32_t s = hashU(static_cast<int>(p.x * 10), static_cast<int>(p.y * 10), seed);
            rock(solid, P3(p.x, p.y, 0), V3(p.radius * 1.12f, p.radius * 1.05f, 1.15f + 0.3f * (s % 7) / 6.0f),
                 (s % 360) * 0.0174f, s);
            fern(soft, P3(p.x + p.radius * 0.9f, p.y - p.radius * 0.6f, 0), 0.7f, s + 3);
        }
        spots.taken.push_back(LVecBase3f(p.x, p.y, p.radius + 0.6f));
    }

    // Stones and plants along the edge, outside the playing area.
    for (float a = 0.0f; a < kTwoPi; a += 0.07f + 0.08f * u(rng)) {
        const float r = R + 1.2f + u(rng) * 2.5f;
        const float x = r * std::cos(a), y = r * std::sin(a), z = groundHeight(x, y, R);
        const float pick = u(rng);
        if (pick < 0.3f) {
            const float s = 0.35f + 0.6f * u(rng);
            rock(solid, P3(x, y, z - 0.1f), V3(s, s * 0.9f, s * 0.6f), u(rng) * kTwoPi, hashU(int(x * 9), int(y * 9), seed));
        } else if (pick < 0.65f) {
            fern(soft, P3(x, y, z), 0.6f + 0.5f * u(rng), hashU(int(x * 7), int(y * 7), seed));
        } else {
            bush(solid, P3(x, y, z), 0.5f + 0.5f * u(rng), hashU(int(x * 5), int(y * 5), seed));
        }
    }

    // The forest. The side toward the camera (-Y) keeps only low plants
    // near the arena, so it never hides the players.
    auto nearCamera = [&](float x, float y) {
        const float a = std::atan2(y, x);
        return std::fabs(a + kTwoPi / 4.0f) < 1.05f && std::hypot(x, y) < R + 30.0f;
    };
    for (int placed = 0, tries = 0; placed < 230 && tries < 6000; ++tries) {
        const float r = R + 5.0f + 85.0f * std::pow(u(rng), 1.3f);
        const float a = u(rng) * kTwoPi;
        const float x = r * std::cos(a), y = r * std::sin(a);
        const float spacing = 2.2f + (r - R) * 0.05f;
        if (!spots.free(x, y, spacing)) continue;
        const float z = groundHeight(x, y, R);
        const uint32_t s = hashU(int(x * 13), int(y * 13), seed);
        if (nearCamera(x, y)) {
            if (u(rng) < 0.5f) bush(solid, P3(x, y, z), 0.7f + 0.6f * u(rng), s);
            else rock(solid, P3(x, y, z - 0.1f), V3(0.8f, 0.7f, 0.5f) * (0.8f + u(rng)), u(rng) * kTwoPi, s);
        } else if (u(rng) < 0.55f) {
            broadleaf(solid, P3(x, y, z), 5.5f + 4.5f * u(rng) + (r - R) * 0.04f, s);
        } else {
            pine(solid, P3(x, y, z), 7.0f + 6.0f * u(rng) + (r - R) * 0.05f, s);
        }
        spots.taken.push_back(LVecBase3f(x, y, spacing * 0.5f));
        ++placed;
    }
    // Big rocks and outcrops further out.
    for (int i = 0; i < 26; ++i) {
        const float r = R + 8.0f + 60.0f * u(rng), a = u(rng) * kTwoPi;
        const float x = r * std::cos(a), y = r * std::sin(a);
        if (nearCamera(x, y) || !spots.free(x, y, 2.5f)) continue;
        const float s = 1.5f + 2.5f * u(rng);
        rock(solid, P3(x, y, groundHeight(x, y, R) - 0.4f), V3(s, s * (0.7f + 0.5f * u(rng)), s * (0.6f + 0.6f * u(rng))),
             u(rng) * kTwoPi, hashU(int(x * 3), int(y * 3), seed));
        spots.taken.push_back(LVecBase3f(x, y, s));
    }

    // Pebbles on the meadow (decoration only: players walk over them).
    for (int i = 0; i < 70; ++i) {
        const float r = R * std::sqrt(u(rng)), a = u(rng) * kTwoPi;
        const float x = r * std::cos(a), y = r * std::sin(a);
        if (!spots.free(x, y, 0.3f)) continue;
        const float s = 0.07f + 0.12f * u(rng);
        rock(solid, P3(x, y, -0.03f), V3(s, s * 0.8f, s * 0.5f), u(rng) * kTwoPi, hashU(int(x * 11), int(y * 11), seed));
    }

    // Grass: short tufts on the playing field, thick grass and flowers around.
    const C4 tints[] = {kGrassDark, kMeadow, C4(0.40f, 0.66f, 0.22f, 1)};
    for (int i = 0; i < 520; ++i) {
        const float r = R * std::sqrt(u(rng)), a = u(rng) * kTwoPi;
        const float x = r * std::cos(a), y = r * std::sin(a);
        if (!spots.free(x, y, 0.1f)) continue;
        tuft(soft, P3(x, y, 0), 0.2f + 0.12f * u(rng), 3 + static_cast<int>(u(rng) * 4), rng, tints[i % 3]);
    }
    for (int i = 0; i < 2600; ++i) {
        const float r = R - 1.0f + 26.0f * std::pow(u(rng), 1.4f), a = u(rng) * kTwoPi;
        const float x = r * std::cos(a), y = r * std::sin(a);
        if (!spots.free(x, y, 0.2f)) continue;
        tuft(soft, P3(x, y, groundHeight(x, y, R)), 0.35f + 0.3f * u(rng), 4 + static_cast<int>(u(rng) * 4), rng,
             tints[i % 3]);
    }
    const C4 petals[] = {C4(1, 1, 1, 1), C4(1.0f, 0.86f, 0.3f, 1), C4(0.95f, 0.55f, 0.75f, 1), C4(0.65f, 0.55f, 0.95f, 1)};
    for (int i = 0; i < 260; ++i) {
        const float r = i < 90 ? R * std::sqrt(u(rng)) : R + 1.0f + 18.0f * u(rng);
        const float a = u(rng) * kTwoPi, x = r * std::cos(a), y = r * std::sin(a);
        if (!spots.free(x, y, 0.15f)) continue;
        // Flowers grow in little groups.
        const C4 &c = petals[hashU(int(x), int(y), seed) % 4];
        for (int k = 0; k < 3; ++k) {
            const float fx = x + (u(rng) - 0.5f) * 0.8f, fy = y + (u(rng) - 0.5f) * 0.8f;
            flower(soft, P3(fx, fy, groundHeight(fx, fy, R)), c, rng);
        }
    }

    Parts parts;
    parts.vertices = solid.vertices() + soft.vertices();
    parts.solid = parent.attach_new_node(solid.finish());
    parts.soft = parent.attach_new_node(soft.finish());
    return parts;
}

// ----------------------------------------------------------- small meshes

PT(GeomNode) box(const std::string &name) {
    Mesh m(name);
    const float h = 0.5f;
    const P3 c[8] = {{-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h}, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}};
    const C4 w(1, 1, 1, 1);
    m.quad(c[0], c[1], c[5], c[4], w);  // -y (front of a character faces +y)
    m.quad(c[1], c[2], c[6], c[5], w);  // +x
    m.quad(c[2], c[3], c[7], c[6], w);  // +y
    m.quad(c[3], c[0], c[4], c[7], w);  // -x
    m.quad(c[4], c[5], c[6], c[7], w);  // top
    m.quad(c[3], c[2], c[1], c[0], w);  // bottom
    return m.finish();
}

PT(GeomNode) gem(const std::string &name) {
    Mesh m(name);
    const C4 w(1, 1, 1, 1);
    const P3 top(0, 0, 1), bottom(0, 0, -1);
    const P3 ring[4] = {{1, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {0, -1, 0}};
    for (int i = 0; i < 4; ++i) {
        m.tri(ring[i], ring[(i + 1) % 4], top, w);
        m.tri(ring[(i + 1) % 4], ring[i], bottom, w);
    }
    return m.finish();
}

PT(GeomNode) wing(const std::string &name) {
    Mesh m(name);
    const C4 w(1, 1, 1, 1);
    const V3 up(0, 0, 1);
    const P3 hinge(0, 0, 0);
    const P3 pts[5] = {{0, 0.35f, 0}, {0.55f, 0.45f, 0}, {0.7f, 0.05f, 0}, {0.45f, -0.35f, 0}, {0.05f, -0.3f, 0}};
    for (int i = 0; i < 4; ++i) m.twoSided(hinge, pts[i + 1], pts[i], up, w);
    return m.finish();
}

} // namespace arena::game::scenery
