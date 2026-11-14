// User settings (eowrecomp.ini next to the executable's working directory), edited from the in-game menu.
#pragma once
#include <stdbool.h>

enum { ASPECT_UNLOCKED = 0, ASPECT_ORIGINAL = 1 };
enum {
    RES_MATCH = 0, /* 1..8 = 1x..8x */
    RES_1080P = 1, RES_2160P = 2,
    RES_DYNAMIC = 9, RES_Q75 = 10, RES_Q25 = 11, RES_Q50 = 12,
    RES_1440P = 13, RES_1800P = 14, RES_2880P = 15,
};
// (11 was 67% of the display: eowrecomp.ini "67%" now loads as RES_Q75)
enum { UPSCALE_BILINEAR = 0, UPSCALE_METALFX = 1 /* macOS */, UPSCALE_FSR1 = 2 /* Windows: AMD FSR 1 */ };
#ifdef EOW_METAL
#define UPSCALE_SHARP UPSCALE_METALFX  // each OS's non-bilinear upscaler (the default)
#else
#define UPSCALE_SHARP UPSCALE_FSR1
#endif
enum { AA_OFF = 0, AA_SMAA = 1 };
enum { FSR_SHARPNESS_DEFAULT = 80 };
enum { PROMPTS_AUTO, PROMPTS_NINTENDO, PROMPTS_XBOX, PROMPTS_PLAYSTATION, PROMPTS_STEAM_DECK, PROMPTS_KEYBOARD };
// System language served to the game (runtime/services/am.c GetDesiredLanguage, services/hid_set.c GetLanguageCode):
// the game picks its text language from it, as it would from the Switch's own system setting. LANG_AUTO follows the
// host locale ($LC_ALL / $LC_MESSAGES / $LANG); the rest are the codes of the languages the game has text for.
enum { LANG_AUTO = 0, N_LANGUAGES = 16 };
extern const char *const g_language_codes[N_LANGUAGES];  // what the game is told: "ja", "en-US", "es", ...
extern const char *const g_language_names[N_LANGUAGES];  // for the menu ("Auto", "Japanese", ...)

typedef struct {
    int aspect;          // ASPECT_*: unlocked = fill the window (wider/taller views), original = 16:9 with bars
    bool fullscreen;
    int res_mode;        // RES_MATCH, fixed height or multiplier, RES_DYNAMIC, RES_Q75/50/25 (share of the display)
    int upscaler;        // UPSCALE_*: how a lower internal resolution reaches the window (MetalFX: macOS, FSR 1: others)
    int fsr_sharpness;   // FSR 1 RCAS strength, 0 disables sharpening
    bool stretch_output; // fill the window without changing the scene's render size or camera aspect
    int antialias;       // AA_*: anti-aliasing of the game image before it is scaled to the window
    float volume;        // master volume, 1 = 100%
    bool reverb;         // room reverb effect
    bool movie_audio;
    bool show_fps;
    bool dpad_move;      // the d-pad (and arrow keys) also act as the left stick
    int language;        // LANG_AUTO or an index into g_language_codes: the game's text language
    int prompts;         // PROMPTS_*: button glyphs (auto = follow the controller SDL reports): runtime/nvn/texpatch.c
    bool aspect_asked;   // the start screen's one-time "authentic 16:9 or unlocked?" question was answered
    bool menu_tip_seen;  // the start screen's one-time "how to open the menu" note was shown (first PLAY ever)
} Settings;

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
void settings_set_prompts(int style);
// Windows guest frame rate preference, shared by the settings UI and Vulkan threads.
bool settings_guest_30(void);
void settings_set_guest_30(bool enabled);
#ifdef _WIN32
// Vulkan reads this preference when the app starts.
bool settings_vulkan_timing(void);
void settings_set_vulkan_timing(bool enabled);
// The preference can be edited while a game is running. The guest uses its boot-time snapshot.
bool settings_handheld(void);
void settings_set_handheld(bool enabled);
bool settings_mouse_action(void);
void settings_set_mouse_action(bool enabled);
bool settings_discord_presence(void);
void settings_set_discord_presence(bool enabled);
void settings_apply_guest_mode(void);
// Stable Windows display identity. An empty key selects Auto.
enum { SETTINGS_MONITOR_KEY_SIZE = 256 };
void settings_get_monitor_key(char key[SETTINGS_MONITOR_KEY_SIZE]);
void settings_set_monitor_key(const char *key);
#endif
bool settings_guest_handheld(void);
// The language code to serve the game ("es", "en-US", ...): the setting, or the host locale for LANG_AUTO
const char *settings_language(void);
// That code's name for the menu ("Spanish")
const char *settings_language_name(void);
