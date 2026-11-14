// Controller input: SDL keyboard/gamepad -> nn::hid Npad state.
//
// Both go through the player's bindings (runtime/hle/bindings.c, defaults there: keyboard WASD left stick, arrows
// d-pad, K/J/L/I = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter / Backspace = Plus / Minus, numpad 8/4/5/6 right stick;
// controller by position, Switch A = east).
#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../ui/touch.h"
#include "../hle.h"
#include "../nvn/nvn.h"
#include "../nvn/renderer.h"
#include "../settings.h"
#include "../input_bind.h"

enum {
    BTN_A = 0, BTN_B, BTN_X, BTN_Y, BTN_LSTICK, BTN_RSTICK, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
    BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN
};

_Static_assert(BTN_DOWN == BIND_DOWN && BTN_ZL == BIND_ZL, "bindings follow the button bits");

static _Atomic uint64_t key_buttons, pad_buttons;
static _Atomic int32_t key_lx, key_ly, key_rx, key_ry, pad_lx, pad_ly, pad_rx, pad_ry;
static uint8_t keys_down[SDL_SCANCODE_COUNT];
#ifdef _WIN32
static _Atomic bool mouse_left;
#endif

// Open every available pad to receive SDL events from it. The list and handles belong to the SDL event thread.
typedef struct InputPad {
    SDL_JoystickID id;
    SDL_Gamepad *handle;
    unsigned engaged_axes;
    bool can_rumble;
    struct InputPad *next;
} InputPad;
static InputPad *pads, *active_pad;
static _Atomic uint32_t active_pad_id;
static _Atomic int64_t sampling;

// Host keys / buttons held by a test script (EOWRECOMP_HOST_INPUT, input_frame): they go through the bindings
static uint8_t test_keys[SDL_SCANCODE_COUNT];
static _Atomic uint32_t test_pad;
static _Atomic int32_t test_lx, test_ly;  // (a resting stick's drift: "lstick" in EOWRECOMP_HOST_INPUT)
static _Atomic bool host_test;  // (then a real controller is left out: the player's own pad must not spoil the test)

static int held_key(int sc) { return sc > 0 && sc < SDL_SCANCODE_COUNT && (keys_down[sc] || test_keys[sc]); }

// Through the bindings (runtime/hle/bindings.c: g_bind_key), read on every key event, so a rebind applies at once.
static void recompute_keyboard(void) {
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (held_key(g_bind_key[i])) b |= 1ull << i;
    key_buttons = b;
    key_lx = (held_key(g_bind_key[BIND_LS_RIGHT]) - held_key(g_bind_key[BIND_LS_LEFT])) * 32767;
    key_ly = (held_key(g_bind_key[BIND_LS_UP]) - held_key(g_bind_key[BIND_LS_DOWN])) * 32767;
    key_rx = (held_key(g_bind_key[BIND_RS_RIGHT]) - held_key(g_bind_key[BIND_RS_LEFT])) * 32767;
    key_ry = (held_key(g_bind_key[BIND_RS_UP]) - held_key(g_bind_key[BIND_RS_DOWN])) * 32767;
}

// Controller buttons (and the triggers as buttons) -> Switch buttons through g_bind_pad; the sticks stay the sticks.
static void recompute_pad(SDL_Gamepad *pad) {
    uint32_t host = test_pad;
    if (pad) {
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)i)) host |= 1u << i;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) host |= 1u << PADB_LTRIGGER;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) host |= 1u << PADB_RTRIGGER;
    }
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (g_bind_pad[i] >= 0 && g_bind_pad[i] < PADB_HOST_COUNT && (host >> g_bind_pad[i] & 1)) b |= 1ull << i;
    pad_buttons = b;
    if (!pad) { pad_lx = test_lx, pad_ly = test_ly, pad_rx = pad_ry = 0; return; }
    pad_lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    pad_ly = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    pad_rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    pad_ry = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
}

// Forget held controls at menu transitions. SDL gamepad handles stay on the event thread.
void input_reset(void) {
    memset(keys_down, 0, sizeof keys_down);
    recompute_keyboard();
    recompute_pad(NULL);
#ifdef _WIN32
    mouse_left = false;
#endif
}

volatile int g_debug_frame_request;  // F9: dump the next frame's passes (build/pass_*.png) + draw log

// Events a host input test pushes carry this device ID and reach the menu first.
#define TEST_DEVICE 0x7E57u
static bool test_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: return e->key.which == TEST_DEVICE;
    case SDL_EVENT_TEXT_INPUT: return e->text.windowID == TEST_DEVICE;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: return e->gbutton.which == TEST_DEVICE;
#ifdef _WIN32
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: return e->button.which == TEST_DEVICE;
#endif
    default: return false;
    }
}

// Scripted test runs ignore the user's own input (a key or stick touched while a test drives Link spoils it):
// EOWRECOMP_NO_USER_INPUT=1, on by default whenever EOWRECOMP_INPUT / EOWRECOMP_DOORJUMP_TEST / EOWRECOMP_PLACE is set
// (or EOWRECOMP_HOST_INPUT / EOWRECOMP_BIND_TEST; EOWRECOMP_NO_USER_INPUT=0 lets it through again).
static bool user_input_disabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("EOWRECOMP_NO_USER_INPUT");
        on = v ? atoi(v) != 0 : (getenv("EOWRECOMP_INPUT") || getenv("EOWRECOMP_DOORJUMP_TEST") || getenv("EOWRECOMP_PLACE") ||
                                 getenv("EOWRECOMP_HOST_INPUT") || getenv("EOWRECOMP_BIND_TEST"));
        if (on) LOG("[input] scripted run: keyboard, mouse and controllers ignored\n");
    }
    return on;
}

bool input_user_blocked(const SDL_Event *e) {
    if (!user_input_disabled()) return false;
    if (test_event(e)) return false;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_JOYSTICK_ADDED: case SDL_EVENT_JOYSTICK_AXIS_MOTION:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN: case SDL_EVENT_JOYSTICK_BUTTON_UP:
        return true;
    default:
        return false;
    }
}

// Controller family for the button prompts (runtime/nvn/texpatch.c): 0 Nintendo / other / none, 1 Xbox, 2 PlayStation.
// EOWRECOMP_PAD_TYPE=xbox|playstation|nintendo pretends one is attached (tests).
static _Atomic int pad_family;
static void note_family(SDL_Gamepad *pad) {
    int f = 0;
    if (pad) switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: f = 1; break;
        case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: f = 2; break;
        default: break;
    }
    if (f != pad_family) LOG("[input] button prompts: %s controller\n", f == 1 ? "Xbox" : f == 2 ? "PlayStation" : "Nintendo / other");
    pad_family = f;
}
static int pad_family_override;
static void read_pad_family_override(void) {
    const char *e = getenv("EOWRECOMP_PAD_TYPE");
    pad_family_override = !e ? -1 : !strcmp(e, "xbox") ? 1 : !strcmp(e, "playstation") || !strcmp(e, "ps") ? 2 : 0;
}

int input_pad_family(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, read_pad_family_override);
    return pad_family_override >= 0 ? pad_family_override : pad_family;
}

uint32_t input_active_pad_id(void) { return active_pad_id; }

enum { RUMBLE_MS = 100 };
static pthread_mutex_t rumble_lock = PTHREAD_MUTEX_INITIALIZER;
static bool rumble_accepting;
static struct { uint16_t low, high; uint64_t expires; } rumble_levels[2];
// Only the SDL event thread reads or writes the last motor settings.
static uint16_t rumble_low, rumble_high;
static uint64_t rumble_poll_at, rumble_refresh_at;

void input_submit_rumble(unsigned sides, const uint16_t levels[2][2]) {
    if (!sides) return;
    pthread_mutex_lock(&rumble_lock);
    if (rumble_accepting) {
        uint64_t expires = plat_now_ns() + RUMBLE_MS * 1000000ull;
        for (unsigned side = 0; side < 2; side++) {
            if (!(sides & (1u << side))) continue;
            rumble_levels[side].low = levels[side][0];
            rumble_levels[side].high = levels[side][1];
            rumble_levels[side].expires = expires;
        }
    }
    pthread_mutex_unlock(&rumble_lock);
}

static void clear_rumble(bool accepting) {
    pthread_mutex_lock(&rumble_lock);
    rumble_accepting = accepting;
    memset(rumble_levels, 0, sizeof rumble_levels);
    pthread_mutex_unlock(&rumble_lock);
    if (active_pad && (rumble_low || rumble_high)) SDL_RumbleGamepad(active_pad->handle, 0, 0, 0);
    rumble_low = rumble_high = 0;
    rumble_poll_at = rumble_refresh_at = 0;
}

void input_stop_rumble(void) { clear_rumble(false); }

void input_pump_rumble(void) {
    if (!active_pad || !active_pad->can_rumble || user_input_disabled()) return;
    uint64_t now = plat_now_ns();
    if (now < rumble_poll_at) return;
    rumble_poll_at = now + 8000000ull;
    uint16_t low = 0, high = 0;
    uint64_t expires = now + RUMBLE_MS * 1000000ull;
    pthread_mutex_lock(&rumble_lock);
    for (unsigned side = 0; side < 2; side++) {
        if (rumble_levels[side].expires <= now || !(rumble_levels[side].low || rumble_levels[side].high)) continue;
        if (rumble_levels[side].low > low) low = rumble_levels[side].low;
        if (rumble_levels[side].high > high) high = rumble_levels[side].high;
        if (rumble_levels[side].expires < expires) expires = rumble_levels[side].expires;
    }
    pthread_mutex_unlock(&rumble_lock);
    if (low == rumble_low && high == rumble_high && (!(low || high) || now < rumble_refresh_at)) return;
    uint32_t ms = low || high ? (uint32_t)((expires - now + 999999ull) / 1000000ull) : 0;
    SDL_RumbleGamepad(active_pad->handle, low, high, ms);
    rumble_low = low;
    rumble_high = high;
    uint64_t refresh = now + 40000000ull;
    rumble_refresh_at = refresh < expires ? refresh : expires;
}

static void select_pad(InputPad *pad) {
    if (pad == active_pad) return;
    clear_rumble(pad && pad->can_rumble && !user_input_disabled());
    active_pad = pad;
    recompute_pad(NULL);  // the previous controller's held buttons/axes must not leak into the new one
    note_family(pad ? pad->handle : NULL);
    active_pad_id = pad ? pad->id : 0;
    if (pad) LOG("[input] gamepad: %s\n", SDL_GetGamepadName(pad->handle));
}

static InputPad **find_pad(SDL_JoystickID id) {
    InputPad **link = &pads;
    while (*link && (*link)->id != id) link = &(*link)->next;
    return link;
}

static void reset_pad_axes(InputPad *pad) {
    pad->engaged_axes = 0;
    for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; axis++)
        if (abs(SDL_GetGamepadAxis(pad->handle, (SDL_GamepadAxis)axis)) > 8000) pad->engaged_axes |= 1u << axis;
}

static bool pad_can_rumble(SDL_Gamepad *pad) {
    return SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
}

static void open_pad(SDL_JoystickID id) {
    InputPad **link = find_pad(id);
    if (*link) return;
    InputPad *pad = calloc(1, sizeof *pad);
    if (!pad) return;
    pad->handle = SDL_OpenGamepad(id);
    if (!pad->handle) { free(pad); return; }
    pad->id = id;
    pad->can_rumble = pad_can_rumble(pad->handle);
    reset_pad_axes(pad);
    *link = pad;
    if (!active_pad) select_pad(pad);
}

static void remove_pad(InputPad **link) {
    InputPad *pad = *link;
    *link = pad->next;
    if (active_pad == pad) select_pad(NULL);
    SDL_CloseGamepad(pad->handle);
    free(pad);
}

void input_refresh_gamepads(void) {
    // The launcher may have consumed hotplug/remap events during an in-process return to its menu.
    for (InputPad **link = &pads; *link;) {
        InputPad *pad = *link;
        if (!SDL_GamepadConnected(pad->handle)) remove_pad(link);
        else {
            pad->can_rumble = pad_can_rumble(pad->handle);
            reset_pad_axes(pad);
            link = &pad->next;
        }
    }
    if (!active_pad) select_pad(pads);
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) open_pad(ids[i]);
    SDL_free(ids);
    clear_rumble(active_pad && active_pad->can_rumble && !user_input_disabled());
    if (active_pad) note_family(active_pad->handle);
    recompute_pad(NULL);  // launcher input should not carry into the game
}

bool input_track_gamepad(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        open_pad(e->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED: {
        InputPad **link = find_pad(e->gdevice.which);
        if (*link) remove_pad(link);
        if (!active_pad) select_pad(pads);
        break;
    }
    case SDL_EVENT_GAMEPAD_REMAPPED: {
        InputPad *pad = *find_pad(e->gdevice.which);
        if (!pad) break;
        reset_pad_axes(pad);
        pad->can_rumble = pad_can_rumble(pad->handle);
        if (pad == active_pad) {
            clear_rumble(pad->can_rumble && !user_input_disabled());
            note_family(pad->handle);
            recompute_pad(host_test || user_input_disabled() ? NULL : pad->handle);
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        if (test_event(e)) return true;
        InputPad *pad = *find_pad(e->gbutton.which);
        if (!pad) return false;
        if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) select_pad(pad);
        return pad == active_pad;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        InputPad *pad = *find_pad(e->gaxis.which);
        if (!pad || e->gaxis.axis >= SDL_GAMEPAD_AXIS_COUNT) return false;
        unsigned bit = 1u << e->gaxis.axis;
        int value = abs(e->gaxis.value);
        // A held or drifting axis can only select again after returning to its neutral zone.
        if (value <= 8000) pad->engaged_axes &= ~bit;
        if (value > 16000 && !(pad->engaged_axes & bit)) {
            pad->engaged_axes |= bit;
            select_pad(pad);
        }
        return pad == active_pad;
    }
    default: break;
    }
    return true;
}

void input_handle_event(const SDL_Event *e) {
    switch (e->type) {
#ifdef _WIN32
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button == SDL_BUTTON_LEFT && e->button.which != SDL_TOUCH_MOUSEID && e->button.which != SDL_PEN_MOUSEID)
            mouse_left = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && settings_mouse_action();
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        mouse_left = false;
        break;
#endif
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F9 && !e->key.repeat) g_debug_frame_request = 1;
        if (e->key.scancode < SDL_SCANCODE_COUNT) keys_down[e->key.scancode] = e->type == SDL_EVENT_KEY_DOWN;
        recompute_keyboard();
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP:
        recompute_pad(!host_test && !user_input_disabled() && active_pad ? active_pad->handle : NULL);
        break;
    }
}

// Scripted input for testing: EOWRECOMP_INPUT="frame:BUTTON:frames,..." (BUTTON = A,B,X,Y,L,R,ZL,ZR,PLUS,
// MINUS,LEFT,UP,RIGHT,DOWN, or STICK_UP/DOWN/LEFT/RIGHT, RSTICK_UP/DOWN/LEFT/RIGHT, STICK_<degrees>: full tilt at
// that angle, 0 = right, 90 = up).
static _Atomic uint64_t script_buttons;
static _Atomic int32_t script_lx, script_ly, script_rx, script_ry;

// Tests of the bindings, through them like a real player:
// EOWRECOMP_HOST_INPUT="frame:key:<SDL key name>:frames,frame:pad:<button>:frames,..." holds a host key / controller
//   button (names as in eowrecomp.ini's bind_* lines: "Z", "Keypad 0", "south", "ltrigger"...);
//   "frame:lstick:<x>/<y>:frames" holds the controller's left stick there (raw SDL units, y up: a drifting stick).
// "frame:type:<text>" types <text> as real key events (down, text, up per character), through the overlay menu / name
//   entry first like a player's typing; "{Escape}" (any SDL key name) presses that key. No ',' or ':' in <text>.
// "frame:sdlpad:<button>:frames" / "frame:sdlkey:<key>:frames" hold a controller button / key as real events (down
//   at frame, up `frames` later): they reach menus and the on-screen keyboard first, as a player's would.
// "frame:sdlmouse:left:frames" holds the Windows mouse action through the same event path.
// "frame:shot:<name>" saves the window (game + menus) to build/<name>.png.
// EOWRECOMP_BIND_TEST="frame:pad|key:<input>:<host>,..." starts a rebind capture of <input> (bind_* name: a, zl,
//   ls_up...) at that frame (the Controls tab shows it waiting) and presses <host> 90 frames later ("esc" cancels,
//   "wait" lets it time out); "frame:reset:pad|key" resets to the defaults.
static void push_key(SDL_Scancode sc, SDL_Keymod mod, bool down) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.which = TEST_DEVICE, e.key.scancode = sc, e.key.mod = mod, e.key.down = down;
    e.key.key = SDL_GetKeyFromScancode(sc, mod, false);
    SDL_PushEvent(&e);
}
static void test_push(uint64_t frame, unsigned long start, unsigned long len, const char *dev, const char *arg) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    if (!strcmp(dev, "shot")) {
        extern char g_window_shot_request[256];  // runtime/hle/hookshot.c
        if (frame == start) snprintf(g_window_shot_request, sizeof g_window_shot_request, "build/%s.png", arg);
        return;
    }
    if (!strcmp(dev, "sdlkey")) {
        if (frame == start || frame == start + len) push_key(SDL_GetScancodeFromName(arg), SDL_KMOD_NONE, frame == start);
        return;
    }
    if (!strcmp(dev, "sdlpad")) {
        int b = bind_pad_from_name(arg);
        if (b < 0 || b >= SDL_GAMEPAD_BUTTON_COUNT || (frame != start && frame != start + len)) return;
        e.type = frame == start ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.which = TEST_DEVICE, e.gbutton.button = (Uint8)b, e.gbutton.down = frame == start;
        SDL_PushEvent(&e);
        return;
    }
    if (!strcmp(dev, "sdlmouse")) {
#ifdef _WIN32
        if (strcmp(arg, "left") || (frame != start && frame != start + len)) return;
        e.button.type = frame == start ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.which = TEST_DEVICE;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = frame == start;
        SDL_PushEvent(&e);
#endif
        return;
    }
    if (frame != start) return;
    LOG("[input] test: typing \"%s\"\n", arg);
    static char ascii[128][2];  // (a text event keeps a pointer to its text: these live on)
    for (const char *p = arg; *p; p++) {
        if (*p == '{' && strchr(p, '}')) {  // a named key
            char nm[32];
            size_t n = (size_t)(strchr(p, '}') - p - 1);
            snprintf(nm, sizeof nm, "%.*s", (int)(n < sizeof nm ? n : sizeof nm - 1), p + 1);
            SDL_Scancode sc = SDL_GetScancodeFromName(nm);
            push_key(sc, SDL_KMOD_NONE, true), push_key(sc, SDL_KMOD_NONE, false);
            p += n + 1;
            continue;
        }
        SDL_Keymod mod = SDL_KMOD_NONE;
        SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)tolower((unsigned char)*p), &mod);
        if (isupper((unsigned char)*p)) mod |= SDL_KMOD_LSHIFT;
        push_key(sc, mod, true);
        int c = *p & 0x7f;
        ascii[c][0] = (char)c;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_TEXT_INPUT, e.text.windowID = TEST_DEVICE, e.text.text = ascii[c];
        SDL_PushEvent(&e);
        push_key(sc, mod, false);
    }
}

static void host_test_frame(uint64_t frame) {
    static const char *hspec, *bspec;
    static int init;
    if (!init) {
        init = 1;
        hspec = getenv("EOWRECOMP_HOST_INPUT");
        bspec = getenv("EOWRECOMP_BIND_TEST");
        if (hspec) { host_test = true; recompute_pad(NULL); }
    }
    char buf[2048];
    if (hspec) {
        uint8_t keys[SDL_SCANCODE_COUNT] = {0};
        uint32_t padm = 0;
        int32_t sx = 0, sy = 0;
        snprintf(buf, sizeof buf, "%s", hspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0, len = 10;
            char dev[9] = "", name[400] = "";
            if (sscanf(tok, "%lu:%8[a-z]:%399[^:]:%lu", &start, dev, name, &len) < 3) continue;
            if (!strcmp(dev, "type") || !strcmp(dev, "sdlpad") || !strcmp(dev, "sdlkey") ||
                !strcmp(dev, "sdlmouse") || !strcmp(dev, "shot")) {
                test_push(frame, start, len, dev, name);
                continue;
            }
            if (frame < start || frame >= start + len) continue;
            if (!strcmp(dev, "key")) { SDL_Scancode sc = SDL_GetScancodeFromName(name); if (sc) keys[sc] = 1; }
            else if (!strcmp(dev, "lstick")) sscanf(name, "%d/%d", &sx, &sy);
            else { int h = bind_pad_from_name(name); if (h >= 0) padm |= 1u << h; }
        }
        bool kch = memcmp(keys, test_keys, sizeof keys) != 0, pch = padm != test_pad || sx != test_lx || sy != test_ly;
        memcpy(test_keys, keys, sizeof keys);
        test_pad = padm;
        test_lx = sx, test_ly = sy;
        if (kch) recompute_keyboard();
        if (pch) recompute_pad(NULL);
    }
    if (bspec) {
        snprintf(buf, sizeof buf, "%s", bspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0;
            char dev[8] = "", in[16] = "", host[40] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%15[a-z_]:%39[^:]", &start, dev, in, host) < 3) continue;
            if (!strcmp(dev, "reset")) { if (frame == start) bind_reset(!strcmp(in, "key") ? BIND_DEV_KEY : BIND_DEV_PAD); continue; }
            int d = !strcmp(dev, "key") ? BIND_DEV_KEY : BIND_DEV_PAD;
            if (frame == start) {
                LOG("[input] test: capture %s %s\n", dev, in);
                bind_capture_begin(d, bind_slot_from_name(in));
            }
            if (frame == start + 90 && strcmp(host, "wait")) {
                SDL_Event e;
                memset(&e, 0, sizeof e);
                if (!strcmp(host, "esc") || d == BIND_DEV_KEY) {
                    e.type = SDL_EVENT_KEY_DOWN;
                    e.key.scancode = !strcmp(host, "esc") ? SDL_SCANCODE_ESCAPE : SDL_GetScancodeFromName(host);
                } else {
                    int h = bind_pad_from_name(host);
                    if (h >= PADB_LTRIGGER) {
                        e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
                        e.gaxis.axis = h == PADB_LTRIGGER ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                        e.gaxis.value = 32767;
                    } else {
                        e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                        e.gbutton.button = (Uint8)h;
                    }
                }
                bind_capture_event(&e);
            }
        }
    }
}

static uint64_t g_pad_obj;
volatile int g_inject_plus;  // frames to hold Plus for the game (movie.c: A/B skip a skippable movie)
RT_ON_RESET(input_reboot_reset) {
    g_pad_obj = 0;  // the controller object was guest memory
    g_inject_plus = 0;
#ifdef _WIN32
    mouse_left = false;
#endif
}
void input_frame(uint64_t frame) {
    extern void watch_arm(void);
    watch_arm();
    host_test_frame(frame);
    const char *spec = getenv("EOWRECOMP_INPUT");
    if (!spec) return;
    static const char *const names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS",
                                        "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
    uint64_t b = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    static char buf[16384];  // (long scripted sessions: the perf walk is ~4 KB)
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        unsigned long start = 0, len = 10;
        char name[32] = "";
        if (sscanf(tok, "%lu:%31[A-Z_0-9]:%lu", &start, name, &len) < 2) continue;
        if (frame < start || frame >= start + len) continue;
        if (!strcmp(name, "STICK_UP")) ly = 32767;
        else if (!strcmp(name, "STICK_DOWN")) ly = -32767;
        else if (!strcmp(name, "STICK_LEFT")) lx = -32767;
        else if (!strcmp(name, "STICK_RIGHT")) lx = 32767;
        else if (!strcmp(name, "RSTICK_UP")) ry = 32767;
        else if (!strcmp(name, "RSTICK_DOWN")) ry = -32767;
        else if (!strcmp(name, "RSTICK_LEFT")) rx = -32767;
        else if (!strcmp(name, "RSTICK_RIGHT")) rx = 32767;
        else if (!strncmp(name, "STICK_", 6) && name[6] >= '0' && name[6] <= '9') {
            double r = atoi(name + 6) * 3.14159265358979 / 180;
            lx = (int32_t)lround(cos(r) * 32767), ly = (int32_t)lround(sin(r) * 32767);
        }
        for (int i = 0; i < 16; i++) if (!strcmp(name, names[i])) b |= 1ull << i;
    }
    script_buttons = b;
    script_lx = lx;
    script_ly = ly;
    script_rx = rx;
    script_ry = ry;
}

extern bool menu_input_captured(void);  // runtime/ui/menu.mm: overlay menu or name entry up (it has the keys / pad)

static uint64_t host_buttons(void) {
    uint64_t buttons = key_buttons | pad_buttons;
#ifdef _WIN32
    if (mouse_left && settings_mouse_action()) buttons |= 1ull << BTN_A;
#endif
    return buttons;
}

// Buttons held right now, independent of whether the game polls (movies).
uint64_t input_buttons_now(void) { return (menu_input_captured() ? 0 : host_buttons()) | script_buttons; }

static int32_t clamp_axis(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }

enum { NPAD_PLAYER_ONE = 0, NPAD_HANDHELD = 0x20, NPAD_FULLKEY_STYLE = 1, NPAD_HANDHELD_STYLE = 2 };

// NpadFullKeyState / NpadHandheldState / NpadJoy*State share this layout (0x28 bytes):
// {int64 sampling, u64 buttons, AnalogStick L {x,y}, AnalogStick R {x,y}, u32 attributes}
static void fill_state(Cpu *c, bool handheld_state) {
    uint8_t *s = (uint8_t *)(uintptr_t)c->x[0];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[1];
    memset(s, 0, 0x28);
    *(int64_t *)s = ++sampling;
    static int nlog;
    if (nlog < 6 && (sampling % 600) == 1) { nlog++; LOG("[input] GetNpadState id %#x lr %llx\n", id, c->x[30]); }
    bool handheld = settings_guest_handheld();
    if (handheld ? id != NPAD_HANDHELD || !handheld_state : id != NPAD_PLAYER_ONE) return;
    extern uint64_t g_watch_lo, g_watch_hi;
    if (id == 0 && !g_pad_obj) g_pad_obj = c->x[1] - 8;
    if (id == 0 && !g_watch_lo && getenv("EOWRECOMP_WATCH_PAD")) {
        unsigned lo = 0x98 + 0x10, hi = 0x98 + 0x48;  // offsets from the controller manager
        sscanf(getenv("EOWRECOMP_WATCH_PAD"), "%x,%x", &lo, &hi);
        g_watch_lo = c->x[1] - 8 - 0x98 + lo;
        g_watch_hi = c->x[1] - 8 - 0x98 + hi;
        LOG("[input] watching controller object %llx\n", c->x[1] - 8);
    }
    bool menu = menu_input_captured();  // the overlay menu / name entry has the controls (typing there isn't for the game)
    int32_t tlx = 0, tly = 0;  // (phones: the on-screen controls, runtime/ui/touch.h)
    if (!menu) touch_stick(&tlx, &tly);
    uint64_t b = menu ? script_buttons : host_buttons() | script_buttons | touch_buttons();
    {   // injected Plus: held for ~10 presented frames (the game polls several times per frame)
        extern uint64_t renderer_present_count(void);
        static uint64_t until;
        if (g_inject_plus > 0) { until = renderer_present_count() + 10; g_inject_plus = 0; }
        if (renderer_present_count() < until) b |= 1ull << BTN_PLUS;
    }
    int32_t lx = clamp_axis((menu ? 0 : key_lx + pad_lx + tlx) + script_lx), ly = clamp_axis((menu ? 0 : key_ly + pad_ly + tly) + script_ly);
    int32_t rx = clamp_axis((menu ? 0 : key_rx + pad_rx) + script_rx), ry = clamp_axis((menu ? 0 : key_ry + pad_ry) + script_ry);
    // d-pad / arrow keys walk (setting) while the stick is at rest. "At rest" has a dead zone: a controller's stick
    // never reads exactly 0 (a DualSense rests at a few hundred), which used to turn the d-pad off altogether.
    if (!menu && g_settings.dpad_move && (int64_t)lx * lx + (int64_t)ly * ly < 8000ll * 8000) {
        uint64_t d = key_buttons | pad_buttons | script_buttons | touch_buttons();
        int dx = (int)((d >> BTN_RIGHT) & 1) - (int)((d >> BTN_LEFT) & 1);
        int dy = (int)((d >> BTN_UP) & 1) - (int)((d >> BTN_DOWN) & 1);
        int32_t m = dx && dy ? 23170 : 32767;  // diagonals: same speed (32767 / sqrt 2)
        if (dx || dy) lx = dx * m, ly = dy * m;
    }
    // stick directions also appear as buttons (bits 16-23)
    if (lx < -16000) b |= 1ull << 16;
    if (ly > 16000) b |= 1ull << 17;
    if (lx > 16000) b |= 1ull << 18;
    if (ly < -16000) b |= 1ull << 19;
    if (rx < -16000) b |= 1ull << 20;
    if (ry > 16000) b |= 1ull << 21;
    if (rx > 16000) b |= 1ull << 22;
    if (ry < -16000) b |= 1ull << 23;
    *(uint64_t *)(s + 8) = b;
    static uint64_t last_b;
    if (b != last_b && getenv("EOWRECOMP_INPUT")) LOG("[input] id %#x buttons %#llx (sampling %lld, lr %llx)\n", id, (unsigned long long)b, (long long)sampling, c->x[30]);
    last_b = b;
    *(int32_t *)(s + 16) = lx;
    *(int32_t *)(s + 20) = ly;
    *(int32_t *)(s + 24) = rx;
    *(int32_t *)(s + 28) = ry;
    *(uint32_t *)(s + 32) = handheld ? 0x15 : 1;  // IsConnected, and both attached Joy-Cons for Handheld
}

HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_state_fullkey) { fill_state(c, false); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_state_dual) { fill_state(c, false); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", hid_state_left) { fill_state(c, false); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_state_handheld) { fill_state(c, true); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", hid_state_right) { fill_state(c, false); }

HLE_FUNC("_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set) {
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[0];
    bool handheld = settings_guest_handheld();
    uint32_t style = handheld ? (id == NPAD_HANDHELD ? NPAD_HANDHELD_STYLE : 0) :
                                (id == NPAD_PLAYER_ONE ? NPAD_FULLKEY_STYLE : 0);
    static int nlog;
    static bool logged_handheld;
    if (nlog < 4 || (style == NPAD_HANDHELD_STYLE && !logged_handheld)) {
        nlog++;
        LOG("[input] GetNpadStyleSet id %#x -> %#x\n", id, style);
    }
    if (style == NPAD_HANDHELD_STYLE) logged_handheld = true;
    c->x[0] = style;
}

// BindNpadStyleSetUpdateEvent(id, SystemEventType*, clear mode): signaled once so the game sees a
// controller connected.
#include "../kernel.h"
HLE_FUNC("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", hid_bind_style_event) {
    uint32_t w, r;
    KEvent *ev = kernel_create_event_pair(&w, &r);
    pthread_mutex_lock(&g_klock);
    kernel_signal(ev);
    pthread_mutex_unlock(&g_klock);
    uint64_t clear_mode = c->x[2];
    c->x[0] = c->x[1];
    c->x[1] = r;
    c->x[2] = 0;
    c->x[3] = clear_mode;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    LOG("[input] style set update event bound\n");
}

// The SDK's vibration graph stays recompiled and sends these handles to the HID IPC service.
HLE_FUNC("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", hid_init_vibration) { (void)c; }
HLE_FUNC("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_vibration_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int n = (int)c->x[1] < 2 ? (int)c->x[1] : 2;
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    if (settings_guest_handheld() && (id != NPAD_HANDHELD || !(c->x[3] & NPAD_HANDHELD_STYLE))) n = 0;
    for (int i = 0; i < n; i++) out[i] = (id & 0xFF) << 8 | (uint32_t)i | 0x03000000u;  // type, player, position
    c->x[0] = (uint64_t)n;
}
HLE_FUNC("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", hid_vibration_info) {
    uint32_t *info = (uint32_t *)(uintptr_t)c->x[0];
    uint32_t h = *(uint32_t *)(uintptr_t)c->x[1];
    info[0] = 1;        // VibrationDeviceType_LinearResonantActuator
    info[1] = h & 1;    // position: left / right
}
