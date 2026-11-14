#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Setup-time only, before any guest rendering or Maxwell translation worker is running.
void vk_prepare_local_shaders(const char *game_root);
bool vk_prepare_shader_archive(const uint8_t *data, size_t size);

// Visits a pinned door pair. Stage bytes are borrowed for the call only.
bool vk_visit_resident_shader_pair(const char *game_root,
    bool (*visitor)(const uint8_t *vs, size_t vs_size, const uint8_t *fs, size_t fs_size, void *ctx), void *ctx);
bool vk_visit_resident_vegetation_pair(const char *game_root,
    bool (*visitor)(const uint8_t *vs, size_t vs_size, const uint8_t *fs, size_t fs_size, void *ctx), void *ctx);
bool vk_visit_resident_vegetation_depth_pair(const char *game_root,
    bool (*visitor)(const uint8_t *vs, size_t vs_size, const uint8_t *fs, size_t fs_size, void *ctx), void *ctx);
void vk_prepare_resident_program(const char *game_root);

#ifdef __cplusplus
}
#endif
