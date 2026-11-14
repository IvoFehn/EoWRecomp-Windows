// Runtime edits of the game's UI textures, made on upload (runtime/nvn/texpatch.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "nvn.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TEXPATCH_UNKNOWN (-2)  // (backend's cached id before the first identification)

// Which of the known textures `t` is (by its contents), or -1.
int texpatch_identify(const NvnTexture *t);
// Changes whenever the wanted look of the known textures changes (prompt style, controller): backends re-upload
// identified textures when it differs from the value they uploaded with.
uint32_t texpatch_generation(void);
// Rewrites level `lv` of known texture `id` in place: `blocks` = its linear data in the texture's own format
// (rows of 4x4 blocks, `row` bytes apart; RGBA8 rows for uncompressed). No-op for the Nintendo look.
void texpatch_apply(int id, uint32_t lv, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t row);

// EoW's standalone button sprites use the Vulkan backend's decoded RGBA staging pixels.
void texpatch_set_prompt_style(int style);
bool texpatch_prompt_candidate(const NvnTexture *t);
int texpatch_prompt_id(const NvnTexture *t);
const char *texpatch_prompt_name(int id);
uint32_t texpatch_prompt_symbol(int id);  // 0 keeps the game's sprite
void texpatch_prompt_rgba(uint32_t symbol, uint8_t *rgba);

// Match the EoW 1.0.0 Pictogram texture and its unmodified layer 2 before editing canonical 1024x1024 BC7 data.
#define TEXPATCH_INLINE_COUNT 10
bool texpatch_inline_candidate(const NvnTexture *t);
bool texpatch_inline_matches(const uint8_t *blocks);
// Snapshot mapped symbols in A, B, X, Y, L, R, ZL, ZR, Plus, Minus order. A zero digest means all use original art.
uint32_t texpatch_inline_symbols(uint32_t symbols[TEXPATCH_INLINE_COUNT]);
void texpatch_inline_apply(const uint32_t symbols[TEXPATCH_INLINE_COUNT], uint8_t *blocks);

#ifdef __cplusplus
}
#endif
