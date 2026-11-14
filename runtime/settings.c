#include "settings.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "hle.h"
#include "input_bind.h"
#ifdef _WIN32
#include "nvn/texpatch.h"
#endif

#define SETTINGS_FILE settings_file()

// EOWRECOMP_SETTINGS=<path>: another settings file (test runs never touch the player's eowrecomp.ini)
static const char *settings_file(void) {
    const char *e = getenv("EOWRECOMP_SETTINGS");
    return e && *e ? e : "eowrecomp.ini";
}

void settings_set_prompts(int style) {
    if (style < PROMPTS_AUTO || style > PROMPTS_KEYBOARD) style = PROMPTS_AUTO;
    g_settings.prompts = style;
#ifdef _WIN32
    texpatch_set_prompt_style(style);
#endif
}

static const char *const prompt_names[] = {"auto", "nintendo", "xbox", "playstation", "steamdeck", "keyboard"};

#ifdef _WIN32
static atomic_bool handheld_preference;
static atomic_bool mouse_action;
static atomic_bool discord_presence;
static atomic_bool vulkan_timing;
static bool guest_handheld;

bool settings_mouse_action(void) { return atomic_load_explicit(&mouse_action, memory_order_relaxed); }
void settings_set_mouse_action(bool enabled) { atomic_store_explicit(&mouse_action, enabled, memory_order_relaxed); }
bool settings_discord_presence(void) { return atomic_load_explicit(&discord_presence, memory_order_relaxed); }
void settings_set_discord_presence(bool enabled) { atomic_store_explicit(&discord_presence, enabled, memory_order_relaxed); }

bool settings_vulkan_timing(void) {
    return atomic_load_explicit(&vulkan_timing, memory_order_relaxed);
}

void settings_set_vulkan_timing(bool enabled) {
    atomic_store_explicit(&vulkan_timing, enabled, memory_order_relaxed);
}

bool settings_handheld(void) {
    return atomic_load_explicit(&handheld_preference, memory_order_relaxed);
}

void settings_set_handheld(bool enabled) {
    atomic_store_explicit(&handheld_preference, enabled, memory_order_relaxed);
}

// The main thread calls this before starting the guest, after any previous guest has stopped.
void settings_apply_guest_mode(void) {
    guest_handheld = settings_handheld();
    LOG("[settings] guest mode %s\n", guest_handheld ? "handheld" : "docked");
}

static const struct { const char *name; int mode; } fixed_resolutions[] = {
    {"1080p", RES_1080P}, {"1440p", RES_1440P}, {"1800p", RES_1800P},
    {"2160p", RES_2160P}, {"2880p", RES_2880P},
    {"4K", RES_2160P}, {"4k", RES_2160P}, {"5K", RES_2880P}, {"5k", RES_2880P},
};

static SRWLOCK monitor_lock = SRWLOCK_INIT;
static char monitor_key[SETTINGS_MONITOR_KEY_SIZE];

void settings_get_monitor_key(char key[SETTINGS_MONITOR_KEY_SIZE]) {
    AcquireSRWLockShared(&monitor_lock);
    strcpy(key, monitor_key);
    ReleaseSRWLockShared(&monitor_lock);
}

void settings_set_monitor_key(const char *key) {
    size_t len = 0;
    if (key) {
        for (; len < sizeof monitor_key && key[len]; len++) {
            unsigned char ch = (unsigned char)key[len];
            if (ch <= ' ' || ch == 127) break;
        }
        if (len == sizeof monitor_key || key[len] || (len == 4 && !memcmp(key, "auto", 4)) ||
            (len && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, key, (int)len, NULL, 0))) len = 0;
    }
    AcquireSRWLockExclusive(&monitor_lock);
    if (len) memcpy(monitor_key, key, len);
    monitor_key[len] = '\0';
    ReleaseSRWLockExclusive(&monitor_lock);
}
#endif

bool settings_guest_handheld(void) {
#ifdef _WIN32
    return guest_handheld;
#else
    return false;
#endif
}

Settings g_settings = {
    .aspect = ASPECT_UNLOCKED,
    .fullscreen = false,
    .volume = 1.0f,
    .reverb = true,
    .movie_audio = true,
    .show_fps = false,
    .upscaler = UPSCALE_SHARP,  // (MetalFX on macOS, AMD FSR 1 on Windows)
    .fsr_sharpness = FSR_SHARPNESS_DEFAULT,
};

static atomic_bool guest_30;

bool settings_guest_30(void) {
    return atomic_load_explicit(&guest_30, memory_order_relaxed);
}

void settings_set_guest_30(bool enabled) {
    atomic_store_explicit(&guest_30, enabled, memory_order_relaxed);
}

// The game's own text languages: its RomFS carries these per region (regionUS/USen|USes|USfr|USpt,
// regionEU/EUen|EUde|EUes|EUfr|EUit|EUnl|EUru, regionJP/JPja, regionKR/KRko, regionTW/TWzh, regionCN/CNzh), and the
// game picks one from the system language it reads back (runtime/services/am.c), as on the console. Codes as the
// Switch reports them (nn::settings::LanguageCode).
const char *const g_language_codes[N_LANGUAGES] = {"auto", "ja", "en-US", "en-GB", "fr", "fr-CA", "de", "it", "es",
                                                   "es-419", "nl", "ru", "ko", "zh-CN", "zh-TW", "pt-BR"};
const char *const g_language_names[N_LANGUAGES] = {"Auto", "Japanese", "English (US)", "English (UK)", "French",
                                                   "French (Canada)", "German", "Italian", "Spanish",
                                                   "Spanish (Latin America)", "Dutch", "Russian", "Korean",
                                                   "Chinese (Simplified)", "Chinese (Traditional)",
                                                   "Portuguese (Brazil)"};

// LANG_AUTO: the host's own language (LANG=es_ES.UTF-8 -> es), en-US when it is none of the game's. Linux only:
// macOS / Windows apps get no locale variables from the system, so Auto is English there (pick one in settings).
static const char *host_language(void) {
#ifndef __linux__
    return "en-US";
#endif
    const char *e = getenv("LC_ALL");
    if (!e || !*e) e = getenv("LC_MESSAGES");
    if (!e || !*e) e = getenv("LANG");
    if (!e || !*e || !strcmp(e, "C") || !strcmp(e, "POSIX")) return "en-US";
    char l[8] = {0};
    size_t n = 0;
    for (const char *p = e; *p && *p != '.' && *p != '@' && *p != '_' && n < sizeof l - 1; p++) l[n++] = *p;
    static const struct { const char *loc, *code; } map[] = {{"ja", "ja"}, {"ko", "ko"}, {"ru", "ru"}, {"nl", "nl"},
                                                             {"de", "de"}, {"it", "it"}, {"fr", "fr"}, {"es", "es"},
                                                             {"pt", "pt-BR"}};
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(l, map[i].loc)) return map[i].code;
    if (!strcmp(l, "zh")) return e[n] == '_' && e[n + 1] == 'T' ? "zh-TW" : "zh-CN";  // (TW / HK: traditional)
    return "en-US";
}

const char *settings_language(void) {
    int i = g_settings.language;
    return i > LANG_AUTO && i < N_LANGUAGES ? g_language_codes[i] : host_language();
}

const char *settings_language_name(void) {
    const char *c = settings_language();
    for (int i = 0; i < N_LANGUAGES; i++)
        if (!strcmp(c, g_language_codes[i])) return g_language_names[i];
    return "English";
}

void settings_load(void) {
    static bool loaded;
    if (loaded) return;
    loaded = true;
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
    char line[384];  // Windows monitor identities can contain 255 bytes.
    bool tip_key = false, aspect_key = false;
    while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n') && !feof(f)) {
            int ch;
            while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
            continue;
        }
        char key[64];
        char val[128];
        int value_offset = 0;
        int fields = sscanf(line, " %63[^= ] = %n%127[^\r\n]", key, &value_offset, val);
#ifdef _WIN32
        if (fields >= 1 && value_offset > 0 && !strcmp(key, "monitor")) {
            char *value = line + value_offset;
            size_t len = strcspn(value, "\r\n");
            const char *end = value + len;
            if (end[strspn(end, "\r\n")]) settings_set_monitor_key(NULL);
            else {
                value[len] = '\0';
                settings_set_monitor_key(value);
            }
            continue;
        }
#endif
        if (fields != 2) continue;
        if (!strcmp(key, "aspect")) g_settings.aspect = !strcmp(val, "original") ? ASPECT_ORIGINAL : ASPECT_UNLOCKED;
        else if (!strcmp(key, "fullscreen")) g_settings.fullscreen = atoi(val) != 0;
#ifdef _WIN32
        else if (!strcmp(key, "frame_rate")) settings_set_guest_30(!strcmp(val, "30"));
        else if (!strcmp(key, "vulkan_timing")) settings_set_vulkan_timing(!strcmp(val, "1"));
        else if (!strcmp(key, "guest_mode")) settings_set_handheld(!strcmp(val, "handheld"));
        else if (!strcmp(key, "mouse_action")) settings_set_mouse_action(atoi(val) != 0);
        else if (!strcmp(key, "discord_presence")) settings_set_discord_presence(atoi(val) != 0);
#endif
        else if (!strcmp(key, "volume")) g_settings.volume = (float)atof(val);
        else if (!strcmp(key, "reverb")) g_settings.reverb = atoi(val) != 0;
        else if (!strcmp(key, "movie_audio")) g_settings.movie_audio = atoi(val) != 0;
        else if (!strcmp(key, "show_fps")) g_settings.show_fps = atoi(val) != 0;
        else if (!strcmp(key, "resolution")) {
#ifdef _WIN32
            bool fixed = false;
            for (size_t i = 0; i < sizeof fixed_resolutions / sizeof fixed_resolutions[0]; i++) {
                if (!strcmp(val, fixed_resolutions[i].name)) {
                    g_settings.res_mode = fixed_resolutions[i].mode;
                    fixed = true;
                    break;
                }
            }
            if (fixed) continue;
#endif
            int n = atoi(val);
            g_settings.res_mode = !strcmp(val, "dynamic") ? RES_DYNAMIC : (!strcmp(val, "75%") || !strcmp(val, "67%")) ? RES_Q75 :
                                  !strcmp(val, "50%") ? RES_Q50 : !strcmp(val, "25%") ? RES_Q25 :
                                  (n >= 1 && n <= 4) ? n : n > 4 ? 4 : RES_MATCH;  // (5x-8x were removed)
        }
        else if (!strcmp(key, "upscaler")) g_settings.upscaler = !strcmp(val, "bilinear") ? UPSCALE_BILINEAR : UPSCALE_SHARP;  // (metalfx / fsr1: this OS's)
#ifdef _WIN32
        else if (!strcmp(key, "fsr_sharpness")) {
            char *end;
            long n = strtol(val, &end, 10);
            g_settings.fsr_sharpness = end == val || *end ? FSR_SHARPNESS_DEFAULT :
                                       n < 0 ? 0 : n > 100 ? 100 : (int)n;
        }
        else if (!strcmp(key, "window_fit")) g_settings.stretch_output = !strcmp(val, "stretch");
#endif
        else if (!strcmp(key, "antialias")) g_settings.antialias = !strcmp(val, "off") ? AA_OFF : AA_SMAA;  // ("fxaa": SMAA now)
        else if (!strcmp(key, "dpad_move")) g_settings.dpad_move = atoi(val) != 0;
        else if (!strcmp(key, "prompts")) {
            int style = PROMPTS_AUTO;
            for (int i = PROMPTS_NINTENDO; i <= PROMPTS_KEYBOARD; i++)
                if (!strcmp(val, prompt_names[i])) style = i;
            settings_set_prompts(style);
        }
        else if (!strcmp(key, "language")) {  // ("auto" or one of the game's codes; anything else: auto)
            g_settings.language = LANG_AUTO;
            for (int i = 1; i < N_LANGUAGES; i++)
                if (!strcmp(val, g_language_codes[i])) g_settings.language = i;
        }
        else if (!strcmp(key, "menu_tip_seen")) g_settings.menu_tip_seen = atoi(val) != 0, tip_key = true;
        else if (!strcmp(key, "aspect_asked")) g_settings.aspect_asked = atoi(val) != 0, aspect_key = true;
        else bind_settings_load(key, val);  // bind_pad_* / bind_key_*: runtime/hle/bindings.c
    }
    fclose(f);
    if (!tip_key) g_settings.menu_tip_seen = true;  // (a settings file from before the note: not a first launch)
    if (!aspect_key) g_settings.aspect_asked = true;  // (likewise: the aspect ratio was chosen already)
    if (g_settings.volume < 0) g_settings.volume = 0;
    if (g_settings.volume > 2) g_settings.volume = 2;
    LOG("[settings] loaded %s\n", SETTINGS_FILE);
}

void settings_save(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) { LOG("[settings] cannot write %s\n", SETTINGS_FILE); return; }
    fprintf(f, "# EoWRecomp settings (edited from the in-game menu: F1, Esc, or Minus+Plus on a controller)\n");
    fprintf(f, "aspect = %s\n", g_settings.aspect == ASPECT_ORIGINAL ? "original" : "unlocked");
    fprintf(f, "fullscreen = %d\n", g_settings.fullscreen);
#ifdef _WIN32
    fprintf(f, "frame_rate = %s\n", settings_guest_30() ? "30" : "auto");
    fprintf(f, "vulkan_timing = %d\n", settings_vulkan_timing());
    fprintf(f, "guest_mode = %s\n", settings_handheld() ? "handheld" : "docked");
    fprintf(f, "mouse_action = %d\n", settings_mouse_action());
    fprintf(f, "discord_presence = %d\n", settings_discord_presence());
    char key[SETTINGS_MONITOR_KEY_SIZE];
    settings_get_monitor_key(key);
    fprintf(f, "monitor = %s\n", key[0] ? key : "auto");
#endif
    fprintf(f, "volume = %.2f\n", g_settings.volume);
    fprintf(f, "reverb = %d\n", g_settings.reverb);
    fprintf(f, "movie_audio = %d\n", g_settings.movie_audio);
    fprintf(f, "show_fps = %d\n", g_settings.show_fps);
    fprintf(f, "upscaler = %s\n", g_settings.upscaler == UPSCALE_METALFX ? "metalfx" : g_settings.upscaler == UPSCALE_FSR1 ? "fsr1" : "bilinear");
#ifdef _WIN32
    fprintf(f, "fsr_sharpness = %d\n", g_settings.fsr_sharpness);
    fprintf(f, "window_fit = %s\n", g_settings.stretch_output ? "stretch" : "bars");
#endif
    fprintf(f, "antialias = %s\n", g_settings.antialias == AA_SMAA ? "smaa" : "off");
    if (g_settings.res_mode == RES_DYNAMIC) fprintf(f, "resolution = dynamic\n");
    else if (g_settings.res_mode == RES_Q75) fprintf(f, "resolution = 75%%\n");
    else if (g_settings.res_mode == RES_Q25) fprintf(f, "resolution = 25%%\n");
    else if (g_settings.res_mode == RES_Q50) fprintf(f, "resolution = 50%%\n");
#ifdef _WIN32
    else if (g_settings.res_mode == RES_1440P) fprintf(f, "resolution = 1440p\n");
    else if (g_settings.res_mode == RES_1800P) fprintf(f, "resolution = 1800p\n");
    else if (g_settings.res_mode == RES_2880P) fprintf(f, "resolution = 2880p\n");
#endif
    else if (g_settings.res_mode >= 1 && g_settings.res_mode <= 8) fprintf(f, "resolution = %dx\n", g_settings.res_mode);
    else fprintf(f, "resolution = match\n");
    fprintf(f, "dpad_move = %d\n", g_settings.dpad_move);
    fprintf(f, "prompts = %s\n", prompt_names[g_settings.prompts]);
    fprintf(f, "language = %s\n", g_language_codes[g_settings.language > LANG_AUTO && g_settings.language < N_LANGUAGES
                                                     ? g_settings.language : LANG_AUTO]);
    fprintf(f, "aspect_asked = %d\n", g_settings.aspect_asked);
    fprintf(f, "menu_tip_seen = %d\n", g_settings.menu_tip_seen);
    bind_settings_save(f);
    fclose(f);
}
