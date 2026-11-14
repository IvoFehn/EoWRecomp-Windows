// EoW 1.0.0 Vulkan scene/HUD boundary observed in the Suthorn corridor.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uintptr_t id;
    uint32_t width, height, format;
} WideSurface;

typedef struct {
    uint64_t frame;
    uintptr_t camera_targets[4];
    uintptr_t scene_image;
    uintptr_t scene_target, wide_present;
} WideFrame;

typedef struct { float sx, sy, ox, oy; } WideHudPlacement;

static inline bool wide_frame_surface_is(WideSurface surface, uint32_t width, uint32_t height, uint32_t format) {
    return surface.id && surface.width == width && surface.height == height && surface.format == format;
}

static inline void wide_frame_scene_draw(WideFrame *state, uint64_t frame, WideSurface target) {
    if (!wide_frame_surface_is(target, 1408, 792, 0x3f)) return;
    if (state->frame != frame) *state = (WideFrame){.frame = frame};
    for (unsigned i = 0; i < sizeof state->camera_targets / sizeof state->camera_targets[0]; i++) {
        if (state->camera_targets[i] && state->camera_targets[i] != target.id) continue;
        state->camera_targets[i] = target.id;
        break;
    }
}

static inline bool wide_frame_scene_image(WideFrame *state, uint64_t frame, uint64_t fs_hash, WideSurface target,
                                           WideSurface first, WideSurface second) {
    if (state->frame != frame || fs_hash != UINT64_C(0x69662da171af7380) ||
        !wide_frame_surface_is(target, 1408, 792, 0x3f)) return false;
    WideSurface scene = wide_frame_surface_is(first, 1408, 792, 0x3f) ? first : second;
    WideSurface bloom = scene.id == first.id ? second : first;
    if (target.id == scene.id || !wide_frame_surface_is(scene, 1408, 792, 0x3f) ||
        !wide_frame_surface_is(bloom, 176, 99, 0x3f)) return false;
    for (unsigned i = 0; i < sizeof state->camera_targets / sizeof state->camera_targets[0]; i++) {
        if (state->camera_targets[i] != scene.id) continue;
        state->scene_image = target.id;
        return true;
    }
    return false;
}

// Observe an emitted draw with the full guest viewport. Texture dimensions are unscaled NVN dimensions.
static inline bool wide_frame_composite(WideFrame *state, uint64_t frame, uint64_t fs_hash, bool indexed,
                                        uint32_t indices, uint32_t instances, uint32_t color_count, bool has_depth,
                                        bool full_viewport, uint32_t fragment_texture_count, WideSurface target,
                                        WideSurface source) {
    if (state->frame != frame || state->scene_image != source.id || fs_hash != UINT64_C(0xbf43bf1a0c6c80f9) ||
        !indexed || indices != 6 || instances != 1 || color_count != 1 || has_depth ||
        !full_viewport || fragment_texture_count != 1 || target.id == source.id ||
        !wide_frame_surface_is(target, 1920, 1080, 0x3f) ||
        !wide_frame_surface_is(source, 1408, 792, 0x3f)) return false;
    state->scene_target = target.id;
    state->wide_present = 0;
    return true;
}

static inline bool wide_frame_target_is_hud_active(const WideFrame *state, uint64_t frame, uintptr_t target) {
    return target && state->frame == frame && state->scene_target == target;
}

static inline bool wide_frame_hud(const WideFrame *state, uint64_t frame, uintptr_t target, uint64_t fs_hash) {
    if (!wide_frame_target_is_hud_active(state, frame, target)) return false;
    switch (fs_hash) {
    case UINT64_C(0x6fe62db96d30294a): // item background
    case UINT64_C(0x28129be4fbd0b3a6): // item glyphs
    case UINT64_C(0x82468691f98920eb): // item composition
    case UINT64_C(0x7b2f69d645b43bda): // heart gauge
        return true;
    default:
        return false;
    }
}

// An unclassified color write to a tracked target invalidates its current result.
static inline void wide_frame_overwrite(WideFrame *state, uint64_t frame, uintptr_t target) {
    if (!target || state->frame != frame) return;
    if (state->scene_image == target) state->scene_image = 0;
    if (state->scene_target == target) state->scene_target = 0;
    if (state->wide_present == target) state->wide_present = 0;
}

static inline void wide_frame_replace(WideFrame *state, uint64_t frame, uintptr_t target) {
    wide_frame_overwrite(state, frame, target);
    if (!target || state->frame != frame) return;
    for (unsigned i = 0; i < sizeof state->camera_targets / sizeof state->camera_targets[0]; i++)
        if (state->camera_targets[i] == target) state->camera_targets[i] = 0;
}

// Observe an emitted base-level copy covering the entire source and destination.
static inline bool wide_frame_copy(WideFrame *state, uint64_t frame, WideSurface source, WideSurface destination,
                                   bool full_base_copy) {
    wide_frame_replace(state, frame, destination.id);
    if (!full_base_copy || !wide_frame_target_is_hud_active(state, frame, source.id) ||
        !wide_frame_surface_is(source, 1920, 1080, 0x3f) ||
        !wide_frame_surface_is(destination, 1920, 1080, 0x38)) return false;
    state->scene_target = 0;
    state->wide_present = destination.id;
    return true;
}

static inline bool wide_frame_is_wide(const WideFrame *state, uint64_t frame, uintptr_t presented) {
    return presented && state->frame == frame && state->wide_present == presented;
}

// Use the same transform for the viewport, scissor and screen-space shader coordinates.
static inline WideHudPlacement wide_hud_placement(float pass_sx, float pass_sy, uint32_t logical_w, uint32_t logical_h,
                                                  uint32_t physical_w, uint32_t physical_h) {
    float scale = pass_sx < pass_sy ? pass_sx : pass_sy;
    return (WideHudPlacement){scale, scale, (physical_w - logical_w * scale) * 0.5f,
                             (physical_h - logical_h * scale) * 0.5f};
}
