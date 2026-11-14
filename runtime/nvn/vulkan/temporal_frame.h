// Temporal input eligibility for the verified EoW perspective render path.
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float rows[4][4];
    float eye[3], forward[3], focal[2], depth_scale, near_intercept;
    float viewport[6]; // x, y, signed width/height, minDepth, maxDepth
} TemporalCamera;

typedef struct {
    TemporalCamera camera;
    uint64_t surface;
    uint64_t color_allocation, depth_allocation;
    uint32_t width, height, output_width, output_height;
    uint32_t scene_format, depth_format, output_format;
} TemporalSample;

typedef struct {
    uint64_t frame;
    bool available, saw_first;
    TemporalSample sample;
} TemporalHistory;

typedef enum {
    TEMPORAL_INVALID, TEMPORAL_UNSUBMITTED, TEMPORAL_FIRST, TEMPORAL_RESUME,
    TEMPORAL_GAP, TEMPORAL_SURFACE, TEMPORAL_EXTENT, TEMPORAL_CUT, TEMPORAL_REUSE, TEMPORAL_BACKING
} TemporalDecision;

static inline bool temporal_same_extent(const TemporalSample *a, const TemporalSample *b) {
    if (a->width != b->width || a->height != b->height ||
        a->output_width != b->output_width || a->output_height != b->output_height ||
        a->scene_format != b->scene_format || a->depth_format != b->depth_format || a->output_format != b->output_format)
        return false;
    for (unsigned i = 0; i < 6; i++)
        if (a->camera.viewport[i] != b->camera.viewport[i]) return false;
    return true;
}

static inline bool temporal_camera_cut(const TemporalCamera *a, const TemporalCamera *b) {
    float eye_distance_squared = 0, direction_dot = 0;
    for (unsigned i = 0; i < 3; i++) {
        float d = a->eye[i] - b->eye[i];
        eye_distance_squared += d * d;
        direction_dot += a->forward[i] * b->forward[i];
    }
    // Discontinuity thresholds for the validated perspective camera model.
    return eye_distance_squared > 64 * 64 || direction_dot < 0.8660254f ||
           b->focal[0] > 1.25f * a->focal[0] || a->focal[0] > 1.25f * b->focal[0] ||
           b->focal[1] > 1.25f * a->focal[1] || a->focal[1] > 1.25f * b->focal[1] ||
           fabsf(a->depth_scale - b->depth_scale) > 0.01f ||
           fabsf(a->near_intercept - b->near_intercept) > 0.25f;
}

static inline TemporalDecision temporal_history_decide(const TemporalHistory *history, uint64_t frame,
                                                         bool submitted, const TemporalSample *sample) {
    if (!submitted) return TEMPORAL_UNSUBMITTED;
    if (!sample || !sample->color_allocation || !sample->depth_allocation) return TEMPORAL_INVALID;
    if (!history->saw_first) return TEMPORAL_FIRST;
    if (!history->available) return TEMPORAL_RESUME;
    if (frame <= history->frame || frame - history->frame != 1) return TEMPORAL_GAP;
    if (history->sample.surface != sample->surface) return TEMPORAL_SURFACE;
    if (!temporal_same_extent(&history->sample, sample)) return TEMPORAL_EXTENT;
    if (history->sample.color_allocation != sample->color_allocation ||
        history->sample.depth_allocation != sample->depth_allocation) return TEMPORAL_BACKING;
    if (temporal_camera_cut(&history->sample.camera, &sample->camera)) return TEMPORAL_CUT;
    return TEMPORAL_REUSE;
}

static inline TemporalDecision temporal_history_commit(TemporalHistory *history, uint64_t frame,
                                                         bool submitted, const TemporalSample *sample) {
    TemporalDecision decision = temporal_history_decide(history, frame, submitted, sample);
    history->frame = frame;
    history->available = decision != TEMPORAL_UNSUBMITTED && decision != TEMPORAL_INVALID;
    if (history->available) {
        history->saw_first = true;
        history->sample = *sample;
    } else {
        history->sample = (TemporalSample){0};
    }
    return decision;
}
