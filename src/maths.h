#pragma once

#include <cmath>
#include <cstring>

// Gamesplat math conventions:
// - Angles are radians.
// - float[16] matrices are column-major.
// - Vectors are treated as column vectors.
// - Matrix multiplication is out = a * b.
// - Point transform is m * vec4(p, 1).
// - Object Euler transforms use T * Rz * Ry * Rx * S.

// TODO: use 3rd party math library
//
// This file is intentionally a small project-convention layer for now. If the
// math surface keeps growing, replace these implementations with one of the
// single-header libraries in vendor/ while keeping the math_* API as the seam.
//
// Findings from the raymath.h vs HandmadeMath.h convention probe/discussion:
//
// Shared positives:
// - Both are simple single-header C/C++ options with optional C++ conveniences.
// - Both provide the basics we need: vec2/3/4, mat4, quaternions, dot/cross,
//   normalize, lerp, LookAt, perspective/ortho, axis-angle, and quat helpers.
// - Both can match our camera view matrix directly with their RH LookAt helper.
// - Both can match our projection matrices after applying our intentional X/Y
//   sign flips. Keep camera/projection wrappers project-owned either way.
// - Neither raw quaternion-to-matrix helper replaces colmap_pose_to_camera();
//   COLMAP import has project-specific world-to-camera, center, and Y-flip
//   logic that should stay explicit or be wrapped with focused tests.
//
// Handmade Math pros/cons:
// - Pro: HMM_Mat4 raw memory/layout matches our current float[16] column-major
//   layout; HMM_MulM4(a, b) matches our current out = a * b semantics.
// - Pro: explicit RH/LH and NO/ZO projection variants make graphics convention
//   choices visible.
// - Pro: HMM_Mat3 maps well to examine-mode rotation helpers.
// - Con: HMM_* API is terse/less application-level; Euler compose/decompose
//   still needs project wrappers.
//
// raymath.h pros/cons:
// - Pro: nice cheatsheet w/ full api https://www.raylib.com/cheatsheet/raymath_cheatsheet.html
// - Pro: friendlier app/game API names and useful higher-level helpers such as
//   MatrixCompose, MatrixDecompose, MatrixRotateZYX, QuaternionFromEuler, and
//   Vector3Transform.
// - Pro: MatrixCompose(translation, QuaternionFromEuler(...), scale) matched
//   our object transform convention in the probe.
//-  Con: No Mat3 type
// - Con: MatrixToFloatV() must be used for shader-style float[16] output; raw
//   memcpy of Matrix does not match our layout.
// - Con: MatrixMultiply call order is opposite our current math_mat4_mul mental
//   model for composition; use a wrapper if adopting raymath.
//
// Current leaning: Handmade Math is the safer backend for matching existing
// conventions directly; raymath.h remains viable if all matrix serialization
// and multiplication goes through math_* wrappers.

static inline float math_clamp(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float math_lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

static inline float math_sigmoid(float x) {
    return 1.0f / (1.0f + expf(-x));
}

static inline float math_smoothstep01(float t) {
    t = math_clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static inline void math_vec3_cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}

static inline float math_vec3_dot(const float a[3], const float b[3]) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static inline float math_vec3_len2(const float v[3]) {
    return math_vec3_dot(v, v);
}

static inline float math_vec3_len(const float v[3]) {
    return sqrtf(math_vec3_len2(v));
}

static inline bool math_vec3_normalize(float v[3]) {
    float len = math_vec3_len(v);
    if (len <= 1e-8f) return false;
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
    return true;
}

static inline void math_mat3_from_euler_zyx(const float e[3], float m[9]) {
    float cx = cosf(e[0]), sx = sinf(e[0]);
    float cy = cosf(e[1]), sy = sinf(e[1]);
    float cz = cosf(e[2]), sz = sinf(e[2]);
    // column 0
    m[0] = cy * cz;
    m[1] = cy * sz;
    m[2] = -sy;
    // column 1
    m[3] = sx * sy * cz - cx * sz;
    m[4] = sx * sy * sz + cx * cz;
    m[5] = sx * cy;
    // column 2
    m[6] = cx * sy * cz + sx * sz;
    m[7] = cx * sy * sz - sx * cz;
    m[8] = cx * cy;
}

static inline void math_mat3_mul(const float a[9], const float b[9], float out[9]) {
    for (int c = 0; c < 3; c++) {
        for (int r = 0; r < 3; r++) {
            out[c*3+r] = a[0*3+r]*b[c*3+0] + a[1*3+r]*b[c*3+1] + a[2*3+r]*b[c*3+2];
        }
    }
}

// Rodrigues: rotation by `theta` (rad) about unit axis `a`. Column-major.
static inline void math_mat3_axis_angle(const float a[3], float theta, float m[9]) {
    float c = cosf(theta), s = sinf(theta), C = 1.0f - c;
    float x = a[0], y = a[1], z = a[2];
    m[0] = c + x*x*C;
    m[1] = y*x*C + z*s;
    m[2] = z*x*C - y*s;
    m[3] = x*y*C - z*s;
    m[4] = c + y*y*C;
    m[5] = z*y*C + x*s;
    m[6] = x*z*C + y*s;
    m[7] = y*z*C - x*s;
    m[8] = c + z*z*C;
}

// Inverse of math_mat3_from_euler_zyx.
static inline void math_mat3_decompose_zyx(const float m[9], float e[3]) {
    float sy = math_clamp(-m[2], -1.0f, 1.0f);
    e[1] = asinf(sy);
    if (fabsf(sy) < 0.99995f) {
        e[0] = atan2f(m[5], m[8]);
        e[2] = atan2f(m[1], m[0]);
    } else {
        e[0] = 0.0f;
        e[2] = atan2f(-m[3], m[4]);
    }
}

static inline void math_mat4_mul(const float a[16], const float b[16], float out[16]) {
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            out[c*4+r] = a[0*4+r]*b[c*4+0] + a[1*4+r]*b[c*4+1] + a[2*4+r]*b[c*4+2] + a[3*4+r]*b[c*4+3];
        }
    }
}

static inline void math_mat4_transform_point(const float m[16], const float p[3], float out[3]) {
    out[0] = m[0]*p[0] + m[4]*p[1] + m[8] *p[2] + m[12];
    out[1] = m[1]*p[0] + m[5]*p[1] + m[9] *p[2] + m[13];
    out[2] = m[2]*p[0] + m[6]*p[1] + m[10]*p[2] + m[14];
}

static inline void math_mat4_translate_scale(float tx, float ty, float tz, float s, float out[16]) {
    memset(out, 0, 16 * sizeof(float));
    out[0]  = s;
    out[5]  = s;
    out[10] = s;
    out[12] = tx;
    out[13] = ty;
    out[14] = tz;
    out[15] = 1.0f;
}
