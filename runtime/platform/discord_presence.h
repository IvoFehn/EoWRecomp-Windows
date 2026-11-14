#ifndef EOW_DISCORD_PRESENCE_H
#define EOW_DISCORD_PRESENCE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call from the host UI thread. Availability checks only the configured public application ID.
#ifdef _WIN32
bool discord_presence_available(void);
void discord_presence_update(bool enabled, bool playing);
void discord_presence_shutdown(void);
#else
static inline bool discord_presence_available(void) { return false; }
static inline void discord_presence_update(bool enabled, bool playing) { (void)enabled; (void)playing; }
static inline void discord_presence_shutdown(void) {}
#endif

#ifdef __cplusplus
}
#endif

#endif
