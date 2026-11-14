#ifndef EOW_WIDE_CAMERA_H
#define EOW_WIDE_CAMERA_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline float vk_wide_camera_dot(const float *a, const float *b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// The verified main EoW perspective pass uses a 1408x792 NVN D24S8 target. Its vertex CB4 starts
// with four row-major world-to-clip rows; UI and square shadow passes use different matrices.
static inline bool vk_wide_camera_matches(const void *ubo, size_t size, unsigned width, unsigned height,
                                          unsigned depth_format) {
    if (!ubo || size < 16 * sizeof(float) || width != 1408 || height != 792 || depth_format != 0x35) return false;

    float m[4][4];
    memcpy(m, ubo, sizeof m);
    for (unsigned row = 0; row < 4; row++)
        for (unsigned column = 0; column < 4; column++)
            if (!isfinite(m[row][column])) return false;

    float x2 = vk_wide_camera_dot(m[0], m[0]);
    float y2 = vk_wide_camera_dot(m[1], m[1]);
    float w2 = vk_wide_camera_dot(m[3], m[3]);
    if (!isfinite(x2) || !isfinite(y2) || y2 <= 1.0f || fabsf(w2 - 1.0f) > 0.002f ||
        fabsf(sqrtf(x2 / y2) - 9.0f / 16.0f) > 0.001f) return false;
    if (fabsf(vk_wide_camera_dot(m[0], m[1])) > 0.002f * sqrtf(x2) * sqrtf(y2) ||
        fabsf(vk_wide_camera_dot(m[0], m[3])) > 0.002f * sqrtf(x2) ||
        fabsf(vk_wide_camera_dot(m[1], m[3])) > 0.002f * sqrtf(y2)) return false;

    // Perspective depth uses the same axis as W, with a distinct near-plane intercept.
    float depth_scale = vk_wide_camera_dot(m[2], m[3]) / w2;
    if (!(depth_scale > 1.0f && depth_scale < 1.1f)) return false;
    for (unsigned i = 0; i < 3; i++)
        if (fabsf(m[2][i] - m[3][i] * depth_scale) > 0.002f) return false;
    float depth_intercept = m[2][3] - m[3][3] * depth_scale;
    return depth_intercept < -0.001f && depth_intercept > -10.0f;
}

// Apply to a private upload copy only. Every rejected input is left byte-for-byte intact.
static inline bool vk_wide_camera_transform(void *ubo, size_t size, unsigned width, unsigned height,
                                            unsigned depth_format, float shape) {
    if (!(shape > 1.0f && shape <= 2.0f) || !vk_wide_camera_matches(ubo, size, width, height, depth_format)) return false;
    float x[4];
    memcpy(x, ubo, sizeof x);
    for (unsigned i = 0; i < 4; i++) x[i] /= shape;
    memcpy(ubo, x, sizeof x);
    return true;
}

#endif
