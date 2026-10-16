// The scenery: a sunny meadow arena in a forest valley, generated at start-up
// as low-poly, flat-shaded geometry with vertex colours (no model files).
// Vertex colour alpha below 1 marks what the wind moves (shaders/world.vert).
#pragma once

#include "game/Movement.h"

#include "geomNode.h"
#include "nodePath.h"
#include "pointerTo.h"

#include <cstdint>
#include <string>

namespace arena::game::scenery {

// Height of the ground: flat inside the arena, rising into wooded hills.
float groundHeight(float x, float y, float arenaRadius);

struct Parts {
    NodePath solid;    // terrain, trees, rocks: cast and receive shadows
    NodePath soft;     // grass, ferns, flowers: thin and swaying, no shadows cast
    int vertices = 0;  // in both
};

// Builds the scenery for `layout` under `parent`, flattened into a few
// batches (one draw call per part). The same seed gives the same forest.
Parts build(NodePath parent, const ArenaLayout &layout, uint32_t seed);

// A unit cube (-0.5..0.5) with flat normals, for the characters.
PT(GeomNode) box(const std::string &name);
// A faceted gem (octahedron, radius 1), for orbs.
PT(GeomNode) gem(const std::string &name);
// A butterfly wing: a flat, rounded triangle fan on the XY plane, hinged on +Y.
PT(GeomNode) wing(const std::string &name);

} // namespace arena::game::scenery
