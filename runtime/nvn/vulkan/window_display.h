#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WINDOW_DISPLAY_KEY_MAX 256
#define WINDOW_DISPLAY_NAME_MAX 128

typedef struct {
    char key[WINDOW_DISPLAY_KEY_MAX];
    char name[WINDOW_DISPLAY_NAME_MAX];
    bool current;
} WindowDisplay;

// Windows Vulkan. The render thread can copy up to capacity entries. Empty key means Auto.
int vk_window_displays(WindowDisplay *out, int capacity);
// Main thread. Refreshes the UI snapshot and returns true for a selectable topology change.
bool vk_refresh_window_displays(void *sdl_window);
// Main thread. A missing preference falls back to the current or primary display.
void vk_apply_window_mode(void *sdl_window, bool fullscreen, const char *monitor_key);

#ifdef __cplusplus
}
#endif
