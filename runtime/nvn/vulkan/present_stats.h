#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double guest_fps;       // Guest NVN presents per second.
    double submitted_fps;   // Vulkan presents accepted per second, including host re-presents.
    double present_max_ms;  // Longest accepted-present interval in the sample, or zero without a pair.
    const char *present_mode;
    bool valid;
    double display_fps;    // Rate of driver-reported first-pixel events.
    bool display_valid;
    const char *display_stage;
} PresentStats;

// Latest completed interval. Read from the render thread.
PresentStats vk_present_stats(void);

#ifdef __cplusplus
}
#endif
