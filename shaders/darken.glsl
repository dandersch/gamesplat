// Animated fog overlay used to dim the FPS view behind the top-down map. Uses
// the same fullscreen-triangle vertex shader as overlay.glsl (duplicated here
// so each @program is self-contained).
//
// sokol-shdc input. Compiled into shaders/darken.glsl.h.

@vs darken_vs
out vec2 v_ndc;

void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_ndc = pos * 2.0 - 1.0;
    gl_Position = vec4(v_ndc, 0.0, 1.0);
}
@end

@fs darken_fs
layout(binding = 0) uniform DarkenUniforms {
    float u_time;       // seconds since startup
    float u_pad0;
    float u_pad1;
    float u_pad2;
};

in vec2 v_ndc;
out vec4 out_color;

// --- Better hash than fract(p.x * p.y); avoids axis-aligned seams ---
float hash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0); // quintic smoothstep
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Per-octave rotation so the grid axes of the value-noise lattice don't pile
// up on top of each other (which is what produces visible seams).
const mat2 ROT = mat2(0.8, -0.6, 0.6, 0.8);

float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 6; i++) {
        v += a * vnoise(p);
        p = ROT * p * 2.03 + vec2(17.1, 31.7);
        a *= 0.5;
    }
    return v;
}

void main() {
    vec2 p = v_ndc * 2.5;

    // Domain-warp the fog with another fbm so wisps look organic.
    vec2 q = vec2(
        fbm(p + vec2(u_time * 0.42,  u_time * 0.18)),
        fbm(p + vec2(-u_time * 0.31, u_time * 0.27) + vec2(5.2, 1.3))
    );

    float n1 = fbm(p + 1.4 * q + vec2( u_time * 0.48,  u_time * 0.22));
    float n2 = fbm(p * 1.7 + 0.9 * q + vec2(-u_time * 0.34,  u_time * 0.40));
    float n  = mix(n1, n2, 0.55);

    n = smoothstep(0.30, 0.70, n);
    n = smoothstep(0.0, 1.0, n);

    float alpha = clamp(0.55 + (n - 0.5) * 0.95, 0.18, 0.90);
    vec3  color = vec3(0.18, 0.20, 0.24);

    out_color = vec4(color * alpha, alpha);
}
@end

@program darken darken_vs darken_fs
