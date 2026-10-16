#version 150
// A bright summer sky: blue overhead, hazy at the horizon (the world's fog
// colour, so the ground blends into it), a warm sun and slow clouds.

uniform float osg_FrameTime;
uniform vec3 skyTop;
uniform vec3 fogColor;     // the horizon
uniform vec3 sunColor;
uniform vec3 sunDirWorld;  // toward the sun

in vec3 vDir;

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

float fbm(vec2 p) {
    float sum = 0.0, amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        sum += noise(p) * amp;
        p = p * 2.03 + vec2(1.7, 9.2);
        amp *= 0.5;
    }
    return sum;
}

void main() {
    vec3 d = normalize(vDir);
    float h = d.z;
    vec3 color = mix(fogColor, skyTop, smoothstep(-0.02, 0.55, h));

    // Clouds on a plane above: soft white puffs drifting with the wind.
    if (h > 0.0) {
        vec2 uv = d.xy / (h + 0.12) * 0.9 + osg_FrameTime * vec2(0.006, 0.0025);
        float c = smoothstep(0.52, 0.78, fbm(uv * 1.3));
        float lit = 0.88 + 0.12 * max(dot(d, normalize(sunDirWorld)), 0.0);
        color = mix(color, vec3(lit), c * smoothstep(0.0, 0.25, h) * 0.85);
    }

    float s = max(dot(d, normalize(sunDirWorld)), 0.0);
    color += sunColor * (pow(s, 900.0) * 3.0 + pow(s, 24.0) * 0.18 + pow(s, 4.0) * 0.06);
    p3d_FragColor = vec4(color, 1.0);
}
