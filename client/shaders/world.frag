#version 150
// Stylised daylight: a soft sun with filtered shadows and drifting cloud
// shadows, sky/ground ambient light, a rim light, and distance haze that
// fades into the sky. Lighting happens in linear colour.

uniform mat4 p3d_ViewMatrix;
uniform float osg_FrameTime;
uniform vec4 p3d_ColorScale;
uniform struct p3d_LightSourceParameters {  // the sun (identical in both stages)
    vec4 position;
    sampler2DShadow shadowMap;
    mat4 shadowViewMatrix;
} p3d_LightSource[1];

uniform vec3 sunColor;
uniform vec3 skyColor;
uniform vec3 groundColor;
uniform vec3 fogColor;
uniform vec2 fogRange;        // start, end (m from the camera)
uniform float shadowStrength; // 0: no shadow map
uniform vec2 shadowTexel;     // 1 / shadow map size
uniform float flash;          // white flash of a hit player, 0..1

in vec3 vViewPos;
in vec3 vViewNormal;
in vec2 vWorldXY;
in vec3 vColor;
in vec4 vShadow;

out vec4 p3d_FragColor;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x),
               mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

float sunShadow(float ndl) {
    if (shadowStrength <= 0.0 || vShadow.w <= 0.0) {
        return 1.0;
    }
    vec3 c = vShadow.xyz / vShadow.w;
    if (c.x <= 0.0 || c.y <= 0.0 || c.x >= 1.0 || c.y >= 1.0 || c.z >= 1.0) {
        return 1.0;  // outside the shadow map: lit
    }
    float bias = mix(0.0025, 0.0008, clamp(ndl, 0.0, 1.0));
    float sum = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec2 o = vec2(float(i & 1) - 0.5, float(i >> 1) - 0.5) * 1.5 * shadowTexel;
        sum += texture(p3d_LightSource[0].shadowMap, vec3(c.xy + o, c.z - bias));
    }
    return mix(1.0, sum * 0.25, shadowStrength);
}

void main() {
    vec3 n = normalize(vViewNormal);
    if (!gl_FrontFacing) {
        n = -n;  // leaves and grass are lit on both sides
    }
    vec3 l = normalize(p3d_LightSource[0].position.xyz);
    vec3 up = normalize(mat3(p3d_ViewMatrix) * vec3(0.0, 0.0, 1.0));
    vec3 albedo = pow(vColor * p3d_ColorScale.rgb, vec3(2.2));

    // A soft, slightly banded terminator gives the painted look.
    float ndl = dot(n, l);
    float diffuse = smoothstep(-0.15, 0.55, ndl);
    float t = osg_FrameTime;
    float clouds = noise(vWorldXY * 0.03 + t * vec2(0.010, 0.006)) * 0.65 +
                   noise(vWorldXY * 0.08 - t * vec2(0.004, 0.011)) * 0.35;
    float cloudLight = mix(0.7, 1.0, smoothstep(0.38, 0.62, clouds));
    float sun = diffuse * sunShadow(ndl) * cloudLight;

    vec3 ambient = mix(groundColor, skyColor, dot(n, up) * 0.5 + 0.5);
    vec3 color = albedo * (ambient + sunColor * sun);

    // Rim light: edges glow faintly with the sky, separating shapes.
    vec3 toEye = normalize(-vViewPos);
    float rim = pow(1.0 - max(dot(n, toEye), 0.0), 4.0);
    color += albedo * skyColor * rim * 0.6;
    color = mix(color, vec3(1.2), flash);

    // Haze: distant things fade into the horizon.
    float fog = smoothstep(fogRange.x, fogRange.y, length(vViewPos));
    color = mix(color, pow(fogColor, vec3(2.2)), fog * fog * (3.0 - 2.0 * fog));

    // A touch more saturation, a soft shoulder, then back to display gamma.
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = max(mix(vec3(luma), color, 0.95), 0.0);
    color = color / (1.0 + max(color - 0.85, 0.0));
    p3d_FragColor = vec4(pow(color, vec3(1.0 / 2.2)), p3d_ColorScale.a);
}
