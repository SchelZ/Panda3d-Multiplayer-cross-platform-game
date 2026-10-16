#include "game/Meshes.h"

#include "geom.h"
#include "geomTriangles.h"
#include "geomVertexData.h"
#include "geomVertexFormat.h"
#include "geomVertexWriter.h"
#include "pnmImage.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace arena::game::meshes {

namespace {

constexpr float kTwoPi = 6.283185307179586f;

struct Builder {
    PT(GeomVertexData) data;
    GeomVertexWriter vertex, normal, texcoord;
    PT(GeomTriangles) tris;
    int count = 0;

    explicit Builder(const std::string &name, int reserve)
        : data(new GeomVertexData(name, GeomVertexFormat::get_v3n3t2(), Geom::UH_static)),
          tris(new GeomTriangles(Geom::UH_static)) {
        data->reserve_num_rows(reserve);
        vertex = GeomVertexWriter(data, "vertex");
        normal = GeomVertexWriter(data, "normal");
        texcoord = GeomVertexWriter(data, "texcoord");
    }

    int add(float x, float y, float z, float nx, float ny, float nz, float u, float v) {
        vertex.add_data3f(x, y, z);
        normal.add_data3f(nx, ny, nz);
        texcoord.add_data2f(u, v);
        return count++;
    }

    void tri(int a, int b, int c) { tris->add_vertices(a, b, c); }

    PT(GeomNode) finish(const std::string &name) {
        tris->close_primitive();
        PT(Geom) geom = new Geom(data);
        geom->add_primitive(tris);
        PT(GeomNode) node = new GeomNode(name);
        node->add_geom(geom);
        return node;
    }
};

} // namespace

PT(GeomNode) sphere(const std::string &name, int segments, int rings) {
    Builder b(name, (segments + 1) * (rings + 1));
    for (int r = 0; r <= rings; ++r) {
        const float v = static_cast<float>(r) / rings;
        const float phi = v * 3.14159265f;  // 0 = top
        const float z = std::cos(phi), rr = std::sin(phi);
        for (int s = 0; s <= segments; ++s) {
            const float u = static_cast<float>(s) / segments;
            const float th = u * kTwoPi;
            const float x = rr * std::cos(th), y = rr * std::sin(th);
            b.add(x, y, z, x, y, z, u, 1.0f - v);
        }
    }
    const int row = segments + 1;
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            const int a = r * row + s, c = a + row;
            if (r != 0) b.tri(a, c, a + 1);
            if (r != rings - 1) b.tri(a + 1, c, c + 1);
        }
    }
    return b.finish(name);
}

PT(GeomNode) ring(const std::string &name, float inner, float outer, int segments) {
    Builder b(name, (segments + 1) * 2);
    for (int s = 0; s <= segments; ++s) {
        const float u = static_cast<float>(s) / segments;
        const float c = std::cos(u * kTwoPi), sn = std::sin(u * kTwoPi);
        b.add(c * inner, sn * inner, 0.0f, 0.0f, 0.0f, 1.0f, u, 0.0f);
        b.add(c * outer, sn * outer, 0.0f, 0.0f, 0.0f, 1.0f, u, 1.0f);
    }
    for (int s = 0; s < segments; ++s) {
        const int a = s * 2;
        b.tri(a, a + 1, a + 2);
        b.tri(a + 1, a + 3, a + 2);
    }
    return b.finish(name);
}

PT(GeomNode) card(const std::string &name) {
    Builder b(name, 4);
    b.add(-1, 0, -1, 0, -1, 0, 0, 0);
    b.add(1, 0, -1, 0, -1, 0, 1, 0);
    b.add(1, 0, 1, 0, -1, 0, 1, 1);
    b.add(-1, 0, 1, 0, -1, 0, 0, 1);
    b.tri(0, 1, 2);
    b.tri(0, 2, 3);
    return b.finish(name);
}

PT(Texture) glowTexture(int size) {
    PNMImage img(size, size, 4);
    const float half = size * 0.5f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = (x + 0.5f - half) / half, dy = (y + 0.5f - half) / half;
            const float d = std::min(1.0f, std::sqrt(dx * dx + dy * dy));
            const float a = (1.0f - d) * (1.0f - d);
            img.set_xel_a(x, y, 1.0f, 1.0f, 1.0f, a);
        }
    }
    PT(Texture) tex = new Texture("glow");
    tex->load(img);
    tex->set_wrap_u(SamplerState::WM_clamp);
    tex->set_wrap_v(SamplerState::WM_clamp);
    tex->set_minfilter(SamplerState::FT_linear_mipmap_linear);
    tex->set_magfilter(SamplerState::FT_linear);
    // Mipmaps made here, not by the driver: OpenGL ES drivers that get it as
    // BGRA (EXT_texture_format_BGRA8888) may refuse glGenerateMipmap on it
    // (BlueStacks does), which leaves the texture incomplete and every glow
    // invisible.
    tex->generate_ram_mipmap_images();
    return tex;
}

} // namespace arena::game::meshes
