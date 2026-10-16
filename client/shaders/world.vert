#version 150
// Everything lit in the world: terrain, trees, rocks, players.
// Vertex colour alpha below 1 marks vertices the wind moves (grass tips,
// leaves): sway = 1 - alpha. Static scenery is flattened to world space, so
// its model space is world space.

uniform mat4 p3d_ModelViewProjectionMatrix;
uniform mat4 p3d_ModelViewMatrix;
uniform mat4 p3d_ModelMatrix;
uniform mat3 p3d_NormalMatrix;
uniform float osg_FrameTime;
uniform struct p3d_LightSourceParameters {  // the sun (identical in both stages)
    vec4 position;
    sampler2DShadow shadowMap;
    mat4 shadowViewMatrix;
} p3d_LightSource[1];

uniform float windStrength;

in vec4 p3d_Vertex;
in vec3 p3d_Normal;
in vec4 p3d_Color;

out vec3 vViewPos;
out vec3 vViewNormal;
out vec2 vWorldXY;
out vec3 vColor;
out vec4 vShadow;

void main() {
    vec4 v = p3d_Vertex;
    vec4 world = p3d_ModelMatrix * v;
    float sway = (1.0 - p3d_Color.a) * windStrength;
    if (sway > 0.0) {
        // Gusts roll across the meadow; each blade also flutters a little.
        float t = osg_FrameTime;
        float gust = 0.55 + 0.45 * sin(t * 0.45 - world.x * 0.05 - world.y * 0.03);
        float phase = dot(world.xy, vec2(0.71, 0.53));
        vec2 flutter = vec2(sin(t * 2.3 + phase), cos(t * 1.9 + phase * 1.3)) * 0.25;
        vec2 bend = (vec2(0.8, 0.45) * gust + flutter) * sway * 0.35;
        v.xy += bend;
        v.z -= dot(bend, bend) * 0.6;  // tips dip as they bend
    }
    vec4 view = p3d_ModelViewMatrix * v;
    gl_Position = p3d_ModelViewProjectionMatrix * v;
    vViewPos = view.xyz;
    vViewNormal = p3d_NormalMatrix * p3d_Normal;
    vWorldXY = world.xy;
    vColor = p3d_Color.rgb;
    vShadow = p3d_LightSource[0].shadowViewMatrix * view;
}
