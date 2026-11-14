// Texture edits on upload (texpatch.h).
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "../input_bind.h"
#include "../settings.h"
#include "texpatch.h"

int texpatch_identify(const NvnTexture *t) {
    (void)t;
    return -1;
}

uint32_t texpatch_generation(void) { return 0; }

void texpatch_apply(int id, uint32_t lv, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t row) {
    (void)id, (void)lv, (void)blocks, (void)w, (void)h, (void)row;
}

enum { FACE_SIZE = 48, FACE_BLOCKS = FACE_SIZE / 4, FACE_ROW = FACE_BLOCKS * 16 };
enum { SYMBOL_A = 1, SYMBOL_B, SYMBOL_X, SYMBOL_Y, SYMBOL_CIRCLE, SYMBOL_CROSS, SYMBOL_SQUARE, SYMBOL_TRIANGLE };
enum { SYMBOL_LB = 0x100, SYMBOL_RB, SYMBOL_LT, SYMBOL_RT, SYMBOL_L1, SYMBOL_R1, SYMBOL_L2, SYMBOL_R2,
       SYMBOL_VIEW, SYMBOL_MENU, SYMBOL_CREATE, SYMBOL_OPTIONS, SYMBOL_LS, SYMBOL_RS, SYMBOL_L3, SYMBOL_R3,
       SYMBOL_KEYBOARD = 0x10000 };
static _Atomic int prompt_style;

static uint64_t prompt_fingerprint(const uint8_t *data, size_t size) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; i++) hash = (hash ^ data[i]) * 0x100000001b3ull;
    return hash;
}

// FNV-1a64 over unpadded level-0 ASTC data in EoW 1.0.0 GameMain.arc, ordered by guest binding slot.
static const uint64_t prompt_fingerprints[BIND_PAD_COUNT] = {
    [BIND_A] = 0x7e0c7617589b9146ull, [BIND_B] = 0xde3cf18191870b41ull,
    [BIND_X] = 0xc7f1d439481d2f37ull, [BIND_Y] = 0xf921d19590ff99efull,
    [BIND_L] = 0xfd3b46ebc0a5b7a8ull, [BIND_R] = 0x1fb4422caf821baeull,
    [BIND_ZL] = 0x78b7808427dc34a9ull, [BIND_ZR] = 0x0884ee2c600f4802ull,
    [BIND_PLUS] = 0x2e6c39f1ed7bff24ull, [BIND_MINUS] = 0x5788a58fd8fd5bb1ull,
};

static bool has_prompt(int id) { return id >= 0 && id < BIND_PAD_COUNT && prompt_fingerprints[id]; }

const char *texpatch_prompt_name(int id) { return has_prompt(id) ? bind_input_name(id) : "?"; }

void texpatch_set_prompt_style(int style) { prompt_style = style; }

bool texpatch_prompt_candidate(const NvnTexture *t) {
    static const uint8_t identity[] = {2, 3, 4, 5}, unset[4];
    const FormatInfo *f = t->layout.fmt;
    return t->format == 0x87 && t->target == 1 && t->width == FACE_SIZE && t->height == FACE_SIZE &&
           t->depth <= 1 && t->samples <= 1 && t->levels == 1 && t->layout.layers <= 1 && t->layout.levels == 1 &&
           f && f->bw == 4 && f->bh == 4 && f->bpb == 16 &&
           (!memcmp(t->swizzle, identity, 4) || !memcmp(t->swizzle, unset, 4));
}

int texpatch_prompt_id(const NvnTexture *t) {
    if (!t->addr || !texpatch_prompt_candidate(t)) return -1;
    LevelLayout l = tegra_level(&t->layout, 0);
    if (l.wb != FACE_BLOCKS || l.hb != FACE_BLOCKS || l.d != 1) return -1;
    bool linear = (t->flags & 0x10) != 0;
    if (linear && (t->stride < FACE_ROW || t->stride > UINT64_MAX / FACE_BLOCKS)) return -1;
    uint64_t size = linear ? t->stride * (FACE_BLOCKS - 1) + FACE_ROW : tegra_total_size(&t->layout);
    if (t->pool && (t->pool_offset > t->pool->size || size > t->pool->size - t->pool_offset)) return -1;
    const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr;
    uint8_t blocks[FACE_ROW * FACE_BLOCKS];
    if (linear) {
        for (unsigned y = 0; y < FACE_BLOCKS; y++) memcpy(blocks + y * FACE_ROW, src + y * t->stride, FACE_ROW);
    } else {
        tegra_deswizzle(src, blocks, FACE_BLOCKS, FACE_BLOCKS, 1, 16, l.block_height_log2, l.block_depth_log2);
    }
    uint64_t hash = prompt_fingerprint(blocks, sizeof blocks);
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (prompt_fingerprints[i] && hash == prompt_fingerprints[i]) return i;
    return -1;
}

static int effective_prompt_style(void) {
    int style = prompt_style;
    if (style == PROMPTS_AUTO) {
        extern int input_pad_family(void);
        style = input_pad_family() + PROMPTS_NINTENDO;
    }
    return style;
}

static uint32_t prompt_symbol(int id, int style) {
    static const uint8_t xbox[] = {SYMBOL_A, SYMBOL_B, SYMBOL_X, SYMBOL_Y};
    static const uint8_t playstation[] = {SYMBOL_CROSS, SYMBOL_CIRCLE, SYMBOL_SQUARE, SYMBOL_TRIANGLE};
    static const uint16_t xbox_text[PADB_HOST_COUNT] = {
        [SDL_GAMEPAD_BUTTON_BACK] = SYMBOL_VIEW, [SDL_GAMEPAD_BUTTON_START] = SYMBOL_MENU,
        [SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = SYMBOL_LB, [SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = SYMBOL_RB,
        [PADB_LTRIGGER] = SYMBOL_LT, [PADB_RTRIGGER] = SYMBOL_RT,
    };
    static const uint16_t ps_text[PADB_HOST_COUNT] = {
        [SDL_GAMEPAD_BUTTON_BACK] = SYMBOL_CREATE, [SDL_GAMEPAD_BUTTON_START] = SYMBOL_OPTIONS,
        [SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = SYMBOL_L1, [SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = SYMBOL_R1,
        [PADB_LTRIGGER] = SYMBOL_L2, [PADB_RTRIGGER] = SYMBOL_R2,
    };
    static const uint16_t deck_text[PADB_HOST_COUNT] = {
        [SDL_GAMEPAD_BUTTON_BACK] = SYMBOL_VIEW, [SDL_GAMEPAD_BUTTON_START] = SYMBOL_MENU,
        [SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = SYMBOL_L1, [SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = SYMBOL_R1,
        [PADB_LTRIGGER] = SYMBOL_L2, [PADB_RTRIGGER] = SYMBOL_R2,
    };
    if (style == PROMPTS_KEYBOARD) return SYMBOL_KEYBOARD + (uint32_t)bind_prompt_key(id);
    if (style != PROMPTS_XBOX && style != PROMPTS_PLAYSTATION && style != PROMPTS_STEAM_DECK) return 0;
    int host = bind_prompt_host(id);
    if (host < 0 || host >= PADB_HOST_COUNT) return 0;
    if (host <= SDL_GAMEPAD_BUTTON_NORTH) return style == PROMPTS_PLAYSTATION ? playstation[host] : xbox[host];
    if (host == SDL_GAMEPAD_BUTTON_LEFT_STICK || host == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
        uint32_t left = style == PROMPTS_XBOX ? SYMBOL_LS : SYMBOL_L3;
        return left + (host == SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    }
    return (style == PROMPTS_PLAYSTATION ? ps_text : style == PROMPTS_STEAM_DECK ? deck_text : xbox_text)[host];
}

uint32_t texpatch_prompt_symbol(int id) {
    return has_prompt(id) ? prompt_symbol(id, effective_prompt_style()) : 0;
}

static bool near_segment(float x, float y, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay;
    float t = ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy);
    t = fmaxf(0.0f, fminf(t, 1.0f));
    x -= ax + t * dx;
    y -= ay + t * dy;
    return x * x + y * y <= 1.75f * 1.75f;
}

static bool glyph_inside(uint32_t symbol, float x, float y) {
    static const int8_t line[][9][4] = {
        [SYMBOL_A] = {{-7, 9, 0, -9}, {0, -9, 7, 9}, {-4, 3, 4, 3}},
        [SYMBOL_B] = {{-6, -9, -6, 9}, {-6, -9, 2, -9}, {2, -9, 6, -6}, {6, -6, 6, -3}, {6, -3, 2, 0},
                      {-6, 0, 2, 0}, {2, 0, 7, 5}, {7, 5, 2, 9}, {2, 9, -6, 9}},
        [SYMBOL_X] = {{-7, -9, 7, 9}, {7, -9, -7, 9}},
        [SYMBOL_Y] = {{-7, -9, 0, 0}, {7, -9, 0, 0}, {0, 0, 0, 9}},
        [SYMBOL_CROSS] = {{-7, -7, 7, 7}, {7, -7, -7, 7}},
        [SYMBOL_SQUARE] = {{-7, -7, 7, -7}, {7, -7, 7, 7}, {7, 7, -7, 7}, {-7, 7, -7, -7}},
        [SYMBOL_TRIANGLE] = {{-9, 7, 0, -9}, {0, -9, 9, 7}, {9, 7, -9, 7}},
    };
    if (symbol == SYMBOL_CIRCLE) {
        float r2 = x * x + y * y;
        return r2 >= 6.25f * 6.25f && r2 <= 9.75f * 9.75f;
    }
    for (unsigned i = 0; i < sizeof line[0] / sizeof line[0][0]; i++) {
        const int8_t *s = line[symbol][i];
        if (s[0] == s[2] && s[1] == s[3]) break;
        if (near_segment(x, y, s[0], s[1], s[2], s[3])) return true;
    }
    return false;
}

// Compact labels for the small in-game button art. Unbound is a dash; unnamed keys use K plus their SDL scancode.
static void key_prompt_label(SDL_Scancode sc, char label[5]) {
    static const char *const short_names[SDL_SCANCODE_COUNT] = {
        [SDL_SCANCODE_UNKNOWN] = "-", [SDL_SCANCODE_RETURN] = "ENT", [SDL_SCANCODE_ESCAPE] = "ESC",
        [SDL_SCANCODE_BACKSPACE] = "BSP", [SDL_SCANCODE_SPACE] = "SPC", [SDL_SCANCODE_CAPSLOCK] = "CAP",
        [SDL_SCANCODE_TAB] = "TAB", [SDL_SCANCODE_HOME] = "HOME", [SDL_SCANCODE_END] = "END",
        [SDL_SCANCODE_MINUS] = "-", [SDL_SCANCODE_EQUALS] = "=", [SDL_SCANCODE_PERIOD] = ".",
        [SDL_SCANCODE_SLASH] = "/",
        [SDL_SCANCODE_PRINTSCREEN] = "PRT", [SDL_SCANCODE_SCROLLLOCK] = "SCR", [SDL_SCANCODE_PAUSE] = "PAU",
        [SDL_SCANCODE_INSERT] = "INS", [SDL_SCANCODE_DELETE] = "DEL", [SDL_SCANCODE_PAGEUP] = "PGU",
        [SDL_SCANCODE_PAGEDOWN] = "PGD", [SDL_SCANCODE_LEFT] = "LT", [SDL_SCANCODE_RIGHT] = "RT",
        [SDL_SCANCODE_UP] = "UP", [SDL_SCANCODE_DOWN] = "DN", [SDL_SCANCODE_NUMLOCKCLEAR] = "NUM",
        [SDL_SCANCODE_KP_DIVIDE] = "N/", [SDL_SCANCODE_KP_MULTIPLY] = "N*", [SDL_SCANCODE_KP_MINUS] = "N-",
        [SDL_SCANCODE_KP_PLUS] = "N+", [SDL_SCANCODE_KP_ENTER] = "NENT", [SDL_SCANCODE_KP_PERIOD] = "N.",
        [SDL_SCANCODE_KP_EQUALS] = "N=", [SDL_SCANCODE_LCTRL] = "LCTL", [SDL_SCANCODE_RCTRL] = "RCTL",
        [SDL_SCANCODE_LSHIFT] = "LSFT", [SDL_SCANCODE_RSHIFT] = "RSFT", [SDL_SCANCODE_LALT] = "LALT",
        [SDL_SCANCODE_RALT] = "RALT", [SDL_SCANCODE_LGUI] = "LGUI", [SDL_SCANCODE_RGUI] = "RGUI",
    };
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        label[0] = (char)('A' + sc - SDL_SCANCODE_A);
        label[1] = 0;
        return;
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) {
        label[0] = sc == SDL_SCANCODE_0 ? '0' : (char)('1' + sc - SDL_SCANCODE_1);
        label[1] = 0;
        return;
    }
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) {
        snprintf(label, 5, "F%d", sc - SDL_SCANCODE_F1 + 1);
        return;
    }
    if (sc >= SDL_SCANCODE_F13 && sc <= SDL_SCANCODE_F24) {
        snprintf(label, 5, "F%d", sc - SDL_SCANCODE_F13 + 13);
        return;
    }
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_0) {
        label[0] = 'N';
        label[1] = sc == SDL_SCANCODE_KP_0 ? '0' : (char)('1' + sc - SDL_SCANCODE_KP_1);
        label[2] = 0;
        return;
    }
    const char *name = short_names[sc];
    if (name) { strcpy(label, name); return; }
    snprintf(label, 5, "K%u", (unsigned)sc);
}

static bool key_glyph_inside(const char *label, unsigned n, float x, float y) {
    static const uint8_t rows[128][7] = {
        ['A'] = {14,17,17,31,17,17,17}, ['B'] = {30,17,17,30,17,17,30},
        ['C'] = {14,17,16,16,16,17,14}, ['D'] = {30,17,17,17,17,17,30},
        ['E'] = {31,16,16,30,16,16,31}, ['F'] = {31,16,16,30,16,16,16},
        ['G'] = {14,17,16,23,17,17,14}, ['H'] = {17,17,17,31,17,17,17},
        ['I'] = {14,4,4,4,4,4,14},      ['J'] = {7,2,2,2,18,18,12},
        ['K'] = {17,18,20,24,20,18,17}, ['L'] = {16,16,16,16,16,16,31},
        ['M'] = {17,27,21,21,17,17,17}, ['N'] = {17,25,21,19,17,17,17},
        ['O'] = {14,17,17,17,17,17,14}, ['P'] = {30,17,17,30,16,16,16},
        ['Q'] = {14,17,17,17,21,18,13}, ['R'] = {30,17,17,30,20,18,17},
        ['S'] = {15,16,16,14,1,1,30},   ['T'] = {31,4,4,4,4,4,4},
        ['U'] = {17,17,17,17,17,17,14}, ['V'] = {17,17,17,17,17,10,4},
        ['W'] = {17,17,17,21,21,27,17}, ['X'] = {17,17,10,4,10,17,17},
        ['Y'] = {17,17,10,4,4,4,4},     ['Z'] = {31,1,2,4,8,16,31},
        ['0'] = {14,17,19,21,25,17,14}, ['1'] = {4,12,4,4,4,4,14},
        ['2'] = {14,17,1,2,4,8,31},     ['3'] = {30,1,1,14,1,1,30},
        ['4'] = {2,6,10,18,31,2,2},     ['5'] = {31,16,16,30,1,1,30},
        ['6'] = {14,16,16,30,17,17,14}, ['7'] = {31,1,2,4,8,8,8},
        ['8'] = {14,17,17,14,17,17,14}, ['9'] = {14,17,17,15,1,1,14},
        ['-'] = {0,0,0,31,0,0,0},       ['+'] = {0,4,4,31,4,4,0},
        ['/'] = {1,1,2,4,8,16,16},      ['*'] = {0,21,14,31,14,21,0},
        ['.'] = {0,0,0,0,0,6,6},        ['='] = {0,0,31,0,31,0,0},
        ['?'] = {14,17,1,2,4,0,4},      ['!'] = {4,4,4,4,4,0,4},
    };
    float cells = 6 * n - 1;
    float sx = n == 1 ? 3.0f : fminf(2.0f, 24.0f / cells), sy = n == 1 ? 3.0f : 2.0f;
    x = x / sx + cells * 0.5f;
    y = y / sy + 3.5f;
    if (x < 0 || x >= cells || y < 0 || y >= 7) return false;
    unsigned col = (unsigned)x % 6;
    return col < 5 && (rows[(unsigned char)label[(unsigned)x / 6]][(unsigned)y] & (16u >> col));
}

static bool prompt_label(uint32_t symbol, char label[5]) {
    static const char *const pad_labels[] = {
        "LB", "RB", "LT", "RT", "L1", "R1", "L2", "R2", "VIEW", "MENU", "CRE", "OPT", "LS", "RS", "L3", "R3",
    };
    bool keyboard = symbol >= SYMBOL_KEYBOARD && symbol < SYMBOL_KEYBOARD + SDL_SCANCODE_COUNT;
    bool pad_label = symbol >= SYMBOL_LB && symbol <= SYMBOL_R3;
    label[0] = '\0';
    if (keyboard) key_prompt_label((SDL_Scancode)(symbol - SYMBOL_KEYBOARD), label);
    else if (pad_label) strcpy(label, pad_labels[symbol - SYMBOL_LB]);
    else if (!symbol || symbol > SYMBOL_TRIANGLE) return false;
    return true;
}

void texpatch_prompt_rgba(uint32_t symbol, uint8_t *rgba) {
    char label[5];
    if (!prompt_label(symbol, label)) return;
    unsigned label_len = (unsigned)strlen(label);
    // Only the middle of the sprite contains label pixels. Keep the local button's shape and alpha.
    for (int y = 10; y < 38; y++) for (int x = 10; x < 38; x++) {
        int covered = 0;
        for (int sy = 0; sy < 2; sy++) for (int sx = 0; sx < 2; sx++) {
            float px = x - 24 + 0.25f + 0.5f * sx, py = y - 24 + 0.25f + 0.5f * sy;
            covered += label_len ? key_glyph_inside(label, label_len, px, py) : glyph_inside(symbol, px, py);
        }
        uint8_t *pixel = rgba + (y * FACE_SIZE + x) * 4;
        pixel[0] = pixel[1] = pixel[2] = (uint8_t)(covered * 255 / 4);
    }
}

enum {
    INLINE_SIZE = 1024, BC7_BLOCK = 4, BC7_BYTES = 16,
    INLINE_BLOCKS = INLINE_SIZE / BC7_BLOCK,
};

typedef struct {
    uint16_t x0, y0, x1, y1;  // block-aligned replacement region
    float cx, cy, rx, ry;     // artwork center and half-extents in the stored layer
    int8_t shoulder;         // -1 left shoulder, 1 right shoulder, 0 circular
    int bind;
} InlineGlyph;

static const InlineGlyph inline_glyphs[TEXPATCH_INLINE_COUNT] = {
    {552, 740, 608, 796, 581.0f, 768.0f, 26.75f, 26.75f,  0, BIND_A},      // U+E0E0
    {656, 740, 712, 796, 685.0f, 768.0f, 26.75f, 26.75f,  0, BIND_B},      // U+E0E1
    {764, 740, 820, 796, 792.0f, 768.0f, 26.75f, 26.75f,  0, BIND_X},      // U+E0E2
    {868, 740, 924, 796, 896.0f, 768.0f, 26.75f, 26.75f,  0, BIND_Y},      // U+E0E3
    { 24, 644,  80, 696,  51.5f, 669.0f, 27.25f, 23.75f, -1, BIND_L},      // U+E0E4
    {128, 644, 184, 696, 156.0f, 669.0f, 26.75f, 23.75f,  1, BIND_R},      // U+E0E5
    {236, 640, 292, 700, 263.0f, 669.5f, 26.75f, 27.25f, -1, BIND_ZL},     // U+E0E6
    {340, 640, 396, 700, 367.0f, 669.5f, 26.75f, 27.25f,  1, BIND_ZR},     // U+E0E7
    {232, 544, 288, 600, 261.0f, 570.5f, 26.00f, 26.25f,  0, BIND_PLUS},   // U+E0EF
    {340, 544, 396, 600, 368.0f, 570.5f, 26.00f, 26.25f,  0, BIND_MINUS},  // U+E0F0
};

bool texpatch_inline_candidate(const NvnTexture *t) {
    static const uint8_t identity[] = {2, 3, 4, 5}, unset[4];
    const TexLayout *l = &t->layout;
    const FormatInfo *f = l->fmt;
    return t->format == 0x4e && t->target == 4 && t->width == INLINE_SIZE && t->height == INLINE_SIZE &&
           t->depth == 4 && t->levels == 1 && t->samples <= 1 &&
           l->width == INLINE_SIZE && l->height == INLINE_SIZE && l->depth == 1 &&
           l->layers == 4 && l->levels == 1 &&
           f && f->bw == BC7_BLOCK && f->bh == BC7_BLOCK && f->bpb == BC7_BYTES &&
           (!memcmp(t->swizzle, identity, 4) || !memcmp(t->swizzle, unset, 4));
}

bool texpatch_inline_matches(const uint8_t *blocks) {
    return prompt_fingerprint(blocks, INLINE_BLOCKS * INLINE_BLOCKS * BC7_BYTES) == 0x92839fc9caaa384dull;
}

uint32_t texpatch_inline_symbols(uint32_t symbols[TEXPATCH_INLINE_COUNT]) {
    int style = effective_prompt_style();
    uint32_t digest = 2166136261u, any = 0;
    for (unsigned i = 0; i < TEXPATCH_INLINE_COUNT; i++) {
        uint32_t symbol = prompt_symbol(inline_glyphs[i].bind, style);
        symbols[i] = symbol;
        any |= symbol;
        digest = (digest ^ symbol) * 16777619u;
    }
    return any ? (digest ? digest : 1) : 0;
}

static void bc7_put(uint8_t block[BC7_BYTES], unsigned *pos, unsigned value, unsigned bits) {
    for (unsigned i = 0; i < bits; i++, (*pos)++)
        block[*pos / 8] |= ((value >> i) & 1u) << (*pos % 8);
}

// Mode 6, one subset. Each endpoint's P-bit is the low bit of all four components.
// RGB stays within one code value of the atlas gold. The endpoints give an alpha ramp from 0 to 255.
static void bc7_gold_block(uint8_t block[BC7_BYTES], const uint8_t index[16]) {
    static const unsigned gold[3] = {127, 115, 90};
    bool reverse = index[0] >= 8;  // texel 0 stores only the low three bits of its index
    unsigned pos = 0;
    memset(block, 0, BC7_BYTES);
    bc7_put(block, &pos, 1u << 6, 7);
    for (unsigned c = 0; c < 3; c++) {
        bc7_put(block, &pos, gold[c], 7);
        bc7_put(block, &pos, gold[c], 7);
    }
    bc7_put(block, &pos, reverse ? 127 : 0, 7);
    bc7_put(block, &pos, reverse ? 0 : 127, 7);
    bc7_put(block, &pos, reverse, 1);
    bc7_put(block, &pos, !reverse, 1);
    for (unsigned i = 0; i < 16; i++)
        bc7_put(block, &pos, reverse ? 15 - index[i] : index[i], i == 0 ? 3 : 4);
}

static bool inline_background(const InlineGlyph *glyph, float x, float y) {
    if (!glyph->shoulder) {
        x /= glyph->rx;
        y /= glyph->ry;
        return x * x + y * y <= 1.0f;
    }
    if (fabsf(x) > glyph->rx || fabsf(y) > glyph->ry) return false;
    // The upper outside shoulder corner is rounded. The opposite and lower corners stay square.
    const float radius = 17.0f;
    x = glyph->shoulder * x - glyph->rx + radius;
    y = -y - glyph->ry + radius;
    return x <= 0 || y <= 0 || x * x + y * y <= radius * radius;
}

static void inline_glyph_apply(const InlineGlyph *glyph, uint32_t symbol, uint8_t *blocks) {
    char label[5];
    if (!prompt_label(symbol, label)) return;
    unsigned label_len = (unsigned)strlen(label);
    // Atlas rows run in the opposite direction from the rendered glyph. Keep a transparent border in the
    // enclosing BC7 blocks; their verified footprint contains no neighboring glyph pixels.
    const float glyph_scale = 1.45f;
    for (unsigned y = glyph->y0; y < glyph->y1; y += BC7_BLOCK) {
        for (unsigned x = glyph->x0; x < glyph->x1; x += BC7_BLOCK) {
            uint8_t index[16];
            for (unsigned ty = 0; ty < BC7_BLOCK; ty++) for (unsigned tx = 0; tx < BC7_BLOCK; tx++) {
                unsigned covered = 0;
                for (unsigned sy = 0; sy < 4; sy++) for (unsigned sx = 0; sx < 4; sx++) {
                    float px = (float)(x + tx) + (sx + 0.5f) * 0.25f - glyph->cx;
                    float py = glyph->cy - (float)(y + ty) - (sy + 0.5f) * 0.25f;
                    if (!inline_background(glyph, px, py)) continue;
                    float gx = px / glyph_scale, gy = py / glyph_scale;
                    bool cutout = label_len ? key_glyph_inside(label, label_len, gx, gy) : glyph_inside(symbol, gx, gy);
                    covered += !cutout;
                }
                index[ty * BC7_BLOCK + tx] = (uint8_t)((covered * 15 + 8) / 16);
            }
            size_t offset = (y / BC7_BLOCK * INLINE_BLOCKS + x / BC7_BLOCK) * BC7_BYTES;
            bc7_gold_block(blocks + offset, index);
        }
    }
}

void texpatch_inline_apply(const uint32_t symbols[TEXPATCH_INLINE_COUNT], uint8_t *blocks) {
    for (unsigned i = 0; i < TEXPATCH_INLINE_COUNT; i++)
        inline_glyph_apply(&inline_glyphs[i], symbols[i], blocks);
}
