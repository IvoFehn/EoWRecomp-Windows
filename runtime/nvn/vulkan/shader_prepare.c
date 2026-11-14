// Prepare a small set of EoW 1.0.0 Vulkan shaders directly from the player's local RomFS.
#include <libavutil/mem.h>
#include <libavutil/sha.h>
#include <stdlib.h>
#include <string.h>

#include "../../romfs.h"
#include "shader_prepare.h"
#include "shaders.h"

void vk_lock_shader_translation(void);
void vk_unlock_shader_translation(void);

typedef struct {
    uint32_t offset, variant;  // BNSH location in the archive and program variation
    uint8_t attr7_kind;
    uint32_t vs_out;
    uint64_t vs_key, fs_key;
    uint32_t vs_size, fs_size;
} ShaderRecipe;
typedef struct {
    const char *path, *label;
    size_t size;
    uint8_t sha256[32];
    const ShaderRecipe *recipes;
    unsigned count;
} ShaderArchive;

static const ShaderRecipe postprocess_pairs[] = {
    {.offset = 0x37000}, {.offset = 0x48000, .variant = 1}, {.offset = 0x4f000},
    {.offset = 0x6f000}, {.offset = 0x73000}, {.offset = 0x254000},
};
static const ShaderArchive postprocess = {
    "region_common/shader/postprocess.bfsha", "postprocess", 2987936,
    {0x00, 0xd1, 0xa1, 0x28, 0x62, 0xfc, 0x0a, 0x7c, 0xe4, 0xf7, 0xa8, 0x71, 0xfe, 0xd8, 0x2e, 0xda,
     0xb6, 0x33, 0x28, 0x2f, 0x49, 0x0f, 0x13, 0x1b, 0xba, 0xb3, 0xd7, 0x58, 0x21, 0xf9, 0x51, 0xe3},
    postprocess_pairs, sizeof postprocess_pairs / sizeof postprocess_pairs[0],
};

// The exact Windows release policy and both stage keys witnessed at the room entrance.
// A different shader compiler or cache policy still uses the normal runtime translation path.
static const uint64_t door_policy = UINT64_C(0xab699b2b38d4e55d);
enum { RESIDENT_DOOR_BACKGROUND_RECIPE = 2, RESIDENT_DOOR_VEGETATION_RECIPE = 2 };
static const ShaderRecipe door_bg_pairs[] = {
    {0xbb000, 2, 0, 0x003, UINT64_C(0x07ec3c0dded0f000), UINT64_C(0xb79cff78b6631a8a), 4608, 768},
    {0xbb000, 2, 2, 0x003, UINT64_C(0x45a8bbba7c7d6771), UINT64_C(0xb79cff78b6631a8a), 4608, 768},
    {0x1f000, 54, 0, 0x7ff, UINT64_C(0x1edb5ae831683611), UINT64_C(0x7928729ea662a712), 10752, 8960},
    {0x1f000, 54, 2, 0x7ff, UINT64_C(0x00b9749b94a9b57a), UINT64_C(0x7928729ea662a712), 10752, 8960},
    {0x1f000, 38, 2, 0x3ff, UINT64_C(0x1d368bead956fd7d), UINT64_C(0x98000da1c05dfb13), 10752, 8704},
};
static const ShaderRecipe door_char_pairs[] = {
    {0xf2000, 2, 2, 0x003, UINT64_C(0x45a8bbba7c7d6771), UINT64_C(0x2a078c7823019024), 4608, 768},
    {0x2a000, 54, 2, 0xfff, UINT64_C(0xce489c45d890507e), UINT64_C(0x751c27a37e5e2b41), 11008, 14080},
    {0x2a000, 38, 2, 0x7ff, UINT64_C(0x0f6940c00897ba65), UINT64_C(0x469a8135c8fcc83a), 10752, 13824},
};
static const ShaderRecipe door_vertex_color_pairs[] = {
    {0xbc000, 2, 0, 0x003, UINT64_C(0x07ec3c0dded0f000), UINT64_C(0xde82533938b7dda3), 4608, 768},
    {0x1f000, 54, 0, 0xfff, UINT64_C(0x211326bbc732fc19), UINT64_C(0x3d9a3b393110da09), 11008, 8960},
};
static const ShaderRecipe door_rgbe_pairs[] = {
    {0xbd000, 2, 0, 0x003, UINT64_C(0x07ec3c0dded0f000), UINT64_C(0x25453b17dc907877), 4608, 768},
    {0x20000, 54, 0, 0x7ff, UINT64_C(0x1edb5ae831683611), UINT64_C(0xc9e0f16fc463aa66), 10752, 8960},
};
static const ShaderRecipe door_vegetation_pairs[] = {
    {0xbf000, 2, 2, 0x003, UINT64_C(0x0fee335d9e4d89ed), UINT64_C(0x25453b17dc907877), 5376, 768},
    {0x1f000, 54, 2, 0x7ff, UINT64_C(0x71417dff0bb99e45), UINT64_C(0x72020b7b5d1972c9), 11776, 8960},
    {0x1f000, 38, 2, 0x3ff, UINT64_C(0x3ea13ff0864cc541), UINT64_C(0xf8da90a1f95128dd), 11776, 8704},
};
static const ShaderRecipe resident_vegetation_depth_recipe = {
    0xbf000, 0, 2, 0x001, UINT64_C(0xeb4cfef08062daf4), UINT64_C(0xf34f2c4793871d9d), 5376, 512,
};
static const ShaderArchive door_archives[] = {
    {
        "region_common/shader/M_PBR_BG_RGB.bfsha", "door background", 819840,
        {0x52, 0xbf, 0xce, 0xc3, 0xdc, 0xb9, 0xc4, 0x3c, 0x7b, 0xe4, 0xa7, 0x46, 0x7a, 0xbd, 0x21, 0x2e,
         0x58, 0x1e, 0x78, 0x3a, 0x91, 0x6b, 0xcb, 0xf7, 0xd2, 0x26, 0xb8, 0x05, 0x70, 0xfa, 0xbb, 0xc3},
        door_bg_pairs, sizeof door_bg_pairs / sizeof door_bg_pairs[0],
    }, {
        "region_common/shader/M_PBR_Char00.bfsha", "door characters", 1053328,
        {0x16, 0x10, 0x11, 0x96, 0x2c, 0xc2, 0xb0, 0x75, 0x02, 0x25, 0x3f, 0xe3, 0x1f, 0x83, 0x27, 0x92,
         0x2e, 0x2f, 0x3e, 0x30, 0x28, 0x84, 0x70, 0xd9, 0xe7, 0xef, 0x9f, 0x72, 0xca, 0xc0, 0x08, 0x2e},
        door_char_pairs, sizeof door_char_pairs / sizeof door_char_pairs[0],
    }, {
        "region_common/shader/M_PBR_BG_RGB_VC.bfsha", "door background vertex color", 823976,
        {0xab, 0x14, 0x5c, 0xf7, 0x02, 0xc7, 0x3a, 0xd9, 0x03, 0x69, 0x59, 0x7d, 0x53, 0x8f, 0x05, 0x70,
         0x45, 0xac, 0xa4, 0x89, 0x41, 0x70, 0x23, 0x5e, 0xd5, 0xa5, 0x1a, 0x4d, 0x29, 0x0c, 0x45, 0xaa},
        door_vertex_color_pairs, sizeof door_vertex_color_pairs / sizeof door_vertex_color_pairs[0],
    }, {
        "region_common/shader/M_PBR_BG_RGBE.bfsha", "door background RGBE", 828032,
        {0x30, 0xb6, 0x35, 0x03, 0x5a, 0x1d, 0x4d, 0x3f, 0x9c, 0xe5, 0x39, 0x5e, 0xd3, 0x58, 0xef, 0xdf,
         0x60, 0x6a, 0xfc, 0x73, 0x63, 0xee, 0x84, 0x71, 0xbe, 0x85, 0x26, 0x19, 0x2c, 0xa7, 0x7f, 0xc8},
        door_rgbe_pairs, sizeof door_rgbe_pairs / sizeof door_rgbe_pairs[0],
    }, {
        "region_common/shader/M_PBR_BG_Vegetation.bfsha", "door vegetation", 844456,
        {0xb5, 0xe5, 0x51, 0x87, 0x9b, 0xa5, 0x54, 0x36, 0xbb, 0xbf, 0xf3, 0x2a, 0x50, 0x21, 0xad, 0xc8,
         0x34, 0x04, 0x3f, 0x55, 0x2f, 0x5b, 0xdd, 0x68, 0xe3, 0x66, 0x58, 0x16, 0x88, 0xaa, 0x96, 0x30},
        door_vegetation_pairs, sizeof door_vegetation_pairs / sizeof door_vegetation_pairs[0],
    },
};
enum { MAX_ARCHIVE_RECIPES = 6 };
_Static_assert(sizeof postprocess_pairs / sizeof postprocess_pairs[0] <= MAX_ARCHIVE_RECIPES, "postprocess recipes");
_Static_assert(sizeof door_bg_pairs / sizeof door_bg_pairs[0] <= MAX_ARCHIVE_RECIPES, "door background recipes");
_Static_assert(sizeof door_char_pairs / sizeof door_char_pairs[0] <= MAX_ARCHIVE_RECIPES, "door character recipes");
_Static_assert(sizeof door_vertex_color_pairs / sizeof door_vertex_color_pairs[0] <= MAX_ARCHIVE_RECIPES, "door vertex-color recipes");
_Static_assert(sizeof door_rgbe_pairs / sizeof door_rgbe_pairs[0] <= MAX_ARCHIVE_RECIPES, "door RGBE recipes");
_Static_assert(sizeof door_vegetation_pairs / sizeof door_vegetation_pairs[0] <= MAX_ARCHIVE_RECIPES, "door vegetation recipes");
typedef struct { const uint8_t *code; size_t size; } ShaderCode;
typedef struct { ShaderCode vertex, fragment; } ShaderPair;

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t read_u64(const uint8_t *p) {
    return (uint64_t)read_u32(p) | (uint64_t)read_u32(p + 4) << 32;
}

static bool within(uint64_t offset, uint64_t bytes, size_t size) {
    return offset <= size && bytes <= size - offset;
}

static bool read_stage(const uint8_t *bnsh, size_t size, uint64_t desc, unsigned stage, ShaderCode *out) {
    if (!within(desc, 0x20, size) || desc < 0x60) return false;
    uint64_t control = read_u64(bnsh + desc + 8), code = read_u64(bnsh + desc + 0x10);
    uint32_t code_size = read_u32(bnsh + desc + 0x18), control_size = read_u32(bnsh + desc + 0x1c);
    if (control < 0x60 || code < 0x60 || control_size < 0x704 || code_size < 0x88 ||
        !within(control, control_size, size) || !within(code, code_size, size)) return false;
    uint32_t constants = read_u32(bnsh + control + 0x6fc), end = read_u32(bnsh + control + 0x700);
    if (end < 0x88 || !within(end, constants, code_size) ||
        (code & 7) || read_u32(bnsh + code) != 0x12345678 ||
        ((read_u32(bnsh + code + 0x30) >> 10) & 0xf) != stage) return false;
    *out = (ShaderCode){bnsh + code, end};
    return true;
}

static bool read_pair(const uint8_t *archive, size_t size, const ShaderRecipe *recipe, ShaderPair *out) {
    uint32_t offset = recipe->offset, variant = recipe->variant;
    if ((offset & 7) || !within(offset, 0x90, size)) return false;
    const uint8_t *bnsh = archive + offset;
    uint32_t bnsh_size = read_u32(bnsh + 0x1c);
    if (memcmp(bnsh, "BNSH\0\0\0\0", 8) || memcmp(bnsh + 0x60, "grsc", 4) ||
        bnsh_size < 0x90 || !within(offset, bnsh_size, size)) return false;
    uint32_t count = read_u32(bnsh + 0x7c);
    uint64_t table = read_u64(bnsh + 0x80);
    if (variant >= count || table < 0x90 || !within(table, (uint64_t)count * 0x40, bnsh_size)) return false;
    uint64_t program = read_u64(bnsh + table + variant * 0x40 + 0x10);
    if (program < 0x60 || !within(program, 0x38, bnsh_size)) return false;
    return read_stage(bnsh, bnsh_size, read_u64(bnsh + program + 8), 1, &out->vertex) &&
           read_stage(bnsh, bnsh_size, read_u64(bnsh + program + 0x28), 5, &out->fragment);
}

static bool archive_matches(const ShaderArchive *archive, const uint8_t *data, size_t size) {
    if (!data || size != archive->size || memcmp(data, "FSHA", 4) || read_u32(data + 0x1c) != size) return false;
    struct AVSHA *sha = av_sha_alloc();
    if (!sha) return false;
    uint8_t digest[sizeof archive->sha256];
    int result = av_sha_init(sha, 256);
    if (result == 0) {
        av_sha_update(sha, data, size);
        av_sha_final(sha, digest);
    }
    av_free(sha);
    return result == 0 && !memcmp(digest, archive->sha256, sizeof digest);
}

static bool prepare_pair(const ShaderPair *pair, const ShaderRecipe *recipe) {
    if (recipe->vs_key && (pair->vertex.size != recipe->vs_size || pair->fragment.size != recipe->fs_size ||
                          (recipe->attr7_kind != 0 && recipe->attr7_kind != 2))) return false;
    // Observed runtime profile: lower-left origin, zero-to-one depth, and float outputs.
    // All vertex inputs are float except slot 7 in the recipes using an unsigned input.
    ShaderKey v = {.glsl = 1, .flip_y = true};
    ShaderKey f = {.glsl = 1, .flip_y = true};
    v.attr_kind[7] = recipe->attr7_kind;
    vk_lock_shader_translation();
    uint64_t saved_base = maxwell_gmem_base, saved_size = maxwell_gmem_size;
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    ShaderInfo *vi = maxwell_translate(pair->vertex.code, pair->vertex.size, STAGE_VERTEX, &v);
    ShaderInfo *fi = NULL;
    if (vi && vi->msl && (!recipe->vs_key || vi->out_mask == recipe->vs_out)) {
        f.vs_out_mask = vi->out_mask;
        fi = maxwell_translate(pair->fragment.code, pair->fragment.size, STAGE_FRAGMENT, &f);
    }
    maxwell_gmem_base = saved_base;
    maxwell_gmem_size = saved_size;
    vk_unlock_shader_translation();
    bool ok = false;
    if (fi && fi->msl &&
        (!recipe->vs_key || (vk_spirv_cache_key(vi->msl, STAGE_VERTEX) == recipe->vs_key &&
                            vk_spirv_cache_key(fi->msl, STAGE_FRAGMENT) == recipe->fs_key))) {
        size_t words;
        uint32_t *vs = vk_glsl_to_spirv(vi->msl, STAGE_VERTEX, "vs", &words);
        uint32_t *fs = vs ? vk_glsl_to_spirv(fi->msl, STAGE_FRAGMENT, "fs", &words) : NULL;
        ok = fs != NULL;
        free(vs);
        free(fs);
    }
    if (vi) maxwell_free(vi);
    if (fi) maxwell_free(fi);
    return ok;
}

static unsigned prepare_archive(const ShaderArchive *archive, const uint8_t *data, size_t size) {
    if (!archive_matches(archive, data, size)) {
        LOG("[shader-prep] unsupported %s archive\n", archive->label);
        return 0;
    }
    ShaderPair pairs[MAX_ARCHIVE_RECIPES];
    if (archive->count > MAX_ARCHIVE_RECIPES) return 0;
    for (unsigned i = 0; i < archive->count; i++) {
        if (!read_pair(data, size, &archive->recipes[i], &pairs[i])) {
            LOG("[shader-prep] invalid %s variation %u\n", archive->label, i);
            return 0;
        }
    }
    unsigned ready = 0;
    for (unsigned i = 0; i < archive->count; i++)
        ready += prepare_pair(&pairs[i], &archive->recipes[i]);
    if (archive == &postprocess)
        LOG("[shader-prep] translated %u/%u known postprocess pairs\n", ready, archive->count);
    return ready;
}

bool vk_prepare_shader_archive(const uint8_t *data, size_t size) {
    if (getenv("EOWRECOMP_VK_NO_SPIRV_CACHE") || getenv("EOWRECOMP_SHADER_GUARD")) return false;
    return prepare_archive(&postprocess, data, size) == postprocess.count;
}

static bool read_loose_archive(const char *game_root, const ShaderArchive *archive, uint8_t *data) {
    char path[1200];
    int n = snprintf(path, sizeof path, "%s/romfs/%s", game_root, archive->path);
    if (n < 0 || (size_t)n >= sizeof path) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool ok = fread(data, 1, archive->size, file) == archive->size && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    return ok;
}

static bool read_local_archive(const char *game_root, const ShaderArchive *archive, uint8_t *data) {
    return romfs_read_local_file(game_root, archive->path, data, archive->size) ||
           read_loose_archive(game_root, archive, data);
}

static bool visit_resident_pair(const char *game_root, const ShaderArchive *archive, const ShaderRecipe *recipe,
        bool (*visitor)(const uint8_t *vs, size_t vs_size, const uint8_t *fs, size_t fs_size, void *ctx), void *ctx) {
    if (!game_root || !*game_root || !visitor || getenv("EOWRECOMP_VK_NO_SPIRV_CACHE") ||
        getenv("EOWRECOMP_SHADER_GUARD") || vk_spirv_cache_key("", STAGE_VERTEX) != door_policy) return false;
    uint8_t *data = malloc(archive->size);
    if (!data) return false;
    ShaderPair pair;
    bool ok = read_local_archive(game_root, archive, data) && archive_matches(archive, data, archive->size) &&
              read_pair(data, archive->size, recipe, &pair) &&
              pair.vertex.size == recipe->vs_size && pair.fragment.size == recipe->fs_size;
    if (ok)
        ok = visitor(pair.vertex.code, pair.vertex.size, pair.fragment.code, pair.fragment.size, ctx);
    free(data);
    return ok;
}

bool vk_visit_resident_shader_pair(const char *game_root,
        bool (*visitor)(const uint8_t *vs, size_t vs_size, const uint8_t *fs, size_t fs_size, void *ctx), void *ctx) {
    return visit_resident_pair(game_root, &door_archives[0], &door_bg_pairs[RESIDENT_DOOR_BACKGROUND_RECIPE], visitor, ctx);
}

bool vk_visit_resident_vegetation_pair(const char *game_root,
        bool (*visitor)(const uint8_t *vs, size_t vs_size, const uint8_t *fs, size_t fs_size, void *ctx), void *ctx) {
    return visit_resident_pair(game_root, &door_archives[4], &door_vegetation_pairs[RESIDENT_DOOR_VEGETATION_RECIPE], visitor, ctx);
}

bool vk_visit_resident_vegetation_depth_pair(const char *game_root,
        bool (*visitor)(const uint8_t *vs, size_t vs_size, const uint8_t *fs, size_t fs_size, void *ctx), void *ctx) {
    return visit_resident_pair(game_root, &door_archives[4], &resident_vegetation_depth_recipe, visitor, ctx);
}

static unsigned prepare_local_archive(const char *game_root, const ShaderArchive *archive) {
    uint8_t *data = malloc(archive->size);
    if (!data) return 0;
    unsigned ready = 0;
    if (read_local_archive(game_root, archive, data))
        ready = prepare_archive(archive, data, archive->size);
    else
        LOG("[shader-prep] local %s archive unavailable\n", archive->label);
    free(data);
    return ready;
}

void vk_prepare_local_shaders(const char *game_root) {
    if (!game_root || !*game_root || getenv("EOWRECOMP_VK_NO_SPIRV_CACHE") || getenv("EOWRECOMP_SHADER_GUARD")) return;
    (void)prepare_local_archive(game_root, &postprocess);
    if (vk_spirv_cache_key("", STAGE_VERTEX) != door_policy) {
        LOG("[shader-prep] skipped door pairs for different SPIR-V policy\n");
        return;
    }
    unsigned ready = 0, count = 0;
    for (unsigned i = 0; i < sizeof door_archives / sizeof door_archives[0]; i++) {
        ready += prepare_local_archive(game_root, &door_archives[i]);
        count += door_archives[i].count;
    }
    LOG("[shader-prep] translated %u/%u known door pairs\n", ready, count);
}
