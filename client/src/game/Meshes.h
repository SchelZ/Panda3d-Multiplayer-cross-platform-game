// Basic procedural shapes and textures (the scenery is in Scenery.cpp).
#pragma once

#include "geomNode.h"
#include "pointerTo.h"
#include "texture.h"

#include <string>

namespace arena::game::meshes {

// Unit sphere (radius 1) with normals and UVs.
PT(GeomNode) sphere(const std::string &name, int segments, int rings);
// Flat ring at z = 0 facing +Z, UV.x = around, UV.y = inner(0)..outer(1).
PT(GeomNode) ring(const std::string &name, float inner, float outer, int segments);
// Square card in the XZ plane, -1..1, facing -Y (for billboards).
PT(GeomNode) card(const std::string &name);

// Soft round glow (white, alpha falls off to the edge).
PT(Texture) glowTexture(int size);

} // namespace arena::game::meshes
