// Included by exec.c: observes the actual draw/copy state without changing commands.
#include "temporal_viewport.h"

typedef struct {
    const NvnTexture *color, *depth;
    VkImage color_image, depth_image;
    TemporalSample sample;
    bool inconsistent;
} TemporalWorld;

static struct {
    WideFrame lineage;
    TemporalWorld worlds[4];
    TemporalWorld world;
    const NvnTexture *scene, *presented;
    VkImage scene_image, presented_image;
    uint64_t scene_allocation;
    VkTemporalScene current;
    unsigned camera, composite, hud, copy;
} temporal_frame;
static TemporalHistory temporal_history;
static TemporalDecision temporal_last_action;
static unsigned temporal_repeats;

static void temporal_reset(void) {
    memset(&temporal_frame, 0, sizeof temporal_frame);
    temporal_history = (TemporalHistory){0};
    temporal_cbuf4_valid = false;
    temporal_repeats = 0;
}

static void temporal_begin(uint64_t id) {
    if (temporal_frame.lineage.frame == id) return;
    memset(&temporal_frame, 0, sizeof temporal_frame);
    temporal_frame.lineage.frame = id;
}

static void temporal_forget_world(const NvnTexture *target) {
    for (unsigned i = 0; i < 4; i++) {
        TemporalWorld *world = &temporal_frame.worlds[i];
        if (world->depth != target && world->color != target) continue;
        wide_frame_replace(&temporal_frame.lineage, temporal_frame.lineage.frame, (uintptr_t)world->color);
        *world = (TemporalWorld){0};
    }
    if (temporal_frame.world.depth == target || temporal_frame.world.color == target) {
        temporal_frame.world = (TemporalWorld){0};
        temporal_frame.current = (VkTemporalScene){0};
    }
}

static void temporal_replace(const NvnTexture *target) {
    if (!target) return;
    temporal_begin(renderer_present_count());
    wide_frame_replace(&temporal_frame.lineage, temporal_frame.lineage.frame, (uintptr_t)target);
    temporal_forget_world(target);
    if (temporal_frame.scene == target) {
        temporal_frame.scene = NULL;
        temporal_frame.current = (VkTemporalScene){0};
    }
    if (temporal_frame.presented == target) temporal_frame.presented = NULL;
}

static void temporal_depth_write(const NvnTexture *target) {
    if (!target) return;
    temporal_begin(renderer_present_count());
    // The renderer resolves attachment aliases before the draw. Use the
    // image actually written, not a former alias source.
    const TexBackend *backend = target->backend;
    const VkImage image = backend ? backend->image : VK_NULL_HANDLE;
    for (unsigned i = 0; i < 4; i++) {
        TemporalWorld *world = &temporal_frame.worlds[i];
        if (world->depth != target && (!image || world->depth_image != image)) continue;
        wide_frame_replace(&temporal_frame.lineage, temporal_frame.lineage.frame, (uintptr_t)world->color);
        *world = (TemporalWorld){0};
    }
    TemporalWorld *world = &temporal_frame.world;
    if (world->depth == target || (image && world->depth_image == image)) {
        temporal_frame.world = (TemporalWorld){0};
        temporal_frame.current = (VkTemporalScene){0};
    }
}

static bool temporal_base_view(bool enabled, const NvnTextureView *view) {
    return !enabled || (!((view->setmask & 1) && view->base_level) &&
                        !((view->setmask & 2) && view->min_layer));
}

static const TexBackend *temporal_image(const NvnTexture *texture, VkFormat format) {
    const TexBackend *image = texture ? texture->backend : NULL;
    if (!image || !image->image || image->format != format || image->samples != 1 || image->layers != 1 ||
        image->depth != 1 || !(image->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) return NULL;
    return image;
}

static bool temporal_camera(TemporalWorld *world, const NvnTexture *color, const NvnTexture *depth,
                            const ShaderKey *key) {
    const TexBackend *cb = temporal_image(color, VK_FORMAT_B10G11R11_UFLOAT_PACK32);
    const TexBackend *db = temporal_image(depth, VK_FORMAT_D24_UNORM_S8_UINT);
    if (!temporal_cbuf4_valid || !cb || !db || !cb->allocation_id || !db->allocation_id ||
        cb->width != db->width || cb->height != db->height ||
        cb->width != pass_w || cb->height != pass_h ||
        pass_depth != VK_FORMAT_D24_UNORM_S8_UINT || !key->flip_y || key->flip_x || key->depth_minus_one ||
        !temporal_full_viewport(ss.vp, (VkExtent2D){pass_w, pass_h}) || fabsf(pass_sx - pass_sy) > 0.0001f ||
        !temporal_base_view(st.rt.has_dview, &st.rt.dview) ||
        !vk_wide_camera_matches(temporal_cbuf4, sizeof temporal_cbuf4, depth->width, depth->height, depth->format))
        return false;
    *world = (TemporalWorld){.color = color, .depth = depth, .color_image = cb->image, .depth_image = db->image,
        .sample = {.width = cb->width, .height = cb->height, .scene_format = cb->format, .depth_format = db->format,
                   .color_allocation = cb->allocation_id, .depth_allocation = db->allocation_id}};
    TemporalCamera *camera = &world->sample.camera;
    memcpy(camera->rows, temporal_cbuf4, sizeof camera->rows);
    const float (*m)[4] = camera->rows;
    for (unsigned a = 0; a < 2; a++) {
        const float squared = vk_wide_camera_dot(m[a], m[a]);
        camera->focal[a] = sqrtf(squared);
        for (unsigned i = 0; i < 3; i++) camera->eye[i] -= m[a][3] * m[a][i] / squared;
    }
    const float w2 = vk_wide_camera_dot(m[3], m[3]);
    camera->depth_scale = vk_wide_camera_dot(m[2], m[3]) / w2;
    camera->near_intercept = m[2][3] - m[3][3] * camera->depth_scale;
    const float w = sqrtf(w2);
    for (unsigned i = 0; i < 3; i++) {
        camera->eye[i] -= m[3][3] * m[3][i] / w2;
        camera->forward[i] = m[3][i] / w;
        if (!isfinite(camera->eye[i]) || !isfinite(camera->forward[i])) return false;
    }
    const float viewport[] = {ss.vp.x, ss.vp.y, ss.vp.width, ss.vp.height, ss.vp.minDepth, ss.vp.maxDepth};
    memcpy(camera->viewport, viewport, sizeof viewport);
    return true;
}

static bool temporal_current_images(void) {
    const TexBackend *color = temporal_image(temporal_frame.scene, VK_FORMAT_B10G11R11_UFLOAT_PACK32);
    const TexBackend *depth = temporal_image(temporal_frame.world.depth, VK_FORMAT_D24_UNORM_S8_UINT);
    return color && depth && color->image == temporal_frame.scene_image &&
           depth->image == temporal_frame.world.depth_image &&
           color->allocation_id == temporal_frame.scene_allocation &&
           depth->allocation_id == temporal_frame.world.sample.depth_allocation &&
           color->width == depth->width && color->height == depth->height &&
           color->width == temporal_frame.world.sample.width && color->height == temporal_frame.world.sample.height;
}

static void temporal_draw(const Prog *program, const PipeState *pipe, const ShaderKey *key,
                          bool indexed, uint32_t count, uint32_t instances, NvnTexture *first, NvnTexture *second) {
    const uint64_t id = renderer_present_count();
    temporal_begin(id);
    if (!count) return;
    if (ss.dw && temporal_base_view(st.rt.has_dview, &st.rt.dview)) temporal_depth_write(st.rt.depth);
    WideFrame *chain = &temporal_frame.lineage;
    const NvnTexture *color = st.rt.ncolor ? st.rt.color[0] : NULL;
    const WideSurface target = wide_surface(color);
    const float *vp = st.viewport[0];
    const bool full = !vp[0] && !vp[1] && vp[2] == target.width && vp[3] == target.height;
    const bool primary = st.rt.ncolor == 1 && pipe->write_mask[0] && full &&
                         temporal_base_view(st.rt.has_cview[0], &st.rt.cview[0]);
    if (primary && temporal_current_images() &&
        wide_frame_composite(chain, id, g_cur_fs_hash, indexed, count, instances, st.rt.ncolor,
                             st.rt.depth != NULL, full, program->fi->ntex, target, wide_surface(first))) {
        TemporalSample sample = temporal_frame.world.sample;
        sample.surface = vk_temporal_surface_generation();
        sample.color_allocation = temporal_frame.scene_allocation;
        sample.output_width = g_vk.sc_extent.width;
        sample.output_height = g_vk.sc_extent.height;
        sample.output_format = g_vk.sc_format;
        temporal_frame.current = (VkTemporalScene){.frame = id + 1, .color = temporal_frame.scene_image,
            .depth = temporal_frame.world.depth_image, .sample = sample,
            .decision = temporal_history_decide(&temporal_history, id + 1, true, &sample)};
        if (temporal_frame.current.decision == TEMPORAL_REUSE) {
            temporal_frame.current.previous_frame = temporal_history.frame;
            temporal_frame.current.previous_camera = temporal_history.sample.camera;
        }
        temporal_frame.composite++;
        temporal_frame.hud = 0;
        return;
    }

    for (int i = 0; i < st.rt.ncolor; i++) {
        const NvnTexture *dst = st.rt.color[i];
        if (!dst || !pipe->write_mask[i]) continue;
        if (wide_frame_hud(chain, id, (uintptr_t)dst, g_cur_fs_hash)) {
            temporal_frame.hud++;
        } else {
            wide_frame_overwrite(chain, id, (uintptr_t)dst);
            if (dst == temporal_frame.scene) {
                temporal_frame.scene = NULL;
                temporal_frame.current = (VkTemporalScene){0};
            }
            if (dst == temporal_frame.presented) temporal_frame.presented = NULL;
        }
    }
    if (!primary) return;
    if (st.rt.depth && !ss.dw && ss.dt && (ss.dop == VK_COMPARE_OP_EQUAL || ss.dop == VK_COMPARE_OP_LESS_OR_EQUAL) &&
        wide_frame_surface_is(target, 1408, 792, 0x3f)) {
        TemporalWorld *slot = NULL;
        for (unsigned i = 0; i < 4; i++) {
            TemporalWorld *candidate = &temporal_frame.worlds[i];
            if (candidate->color == color) { slot = candidate; break; }
            if (!slot && !candidate->color) slot = candidate;
        }
        TemporalWorld world;
        if (slot && !slot->inconsistent && temporal_camera(&world, color, st.rt.depth, key)) {
            if (!slot->color) {
                *slot = world;
                wide_frame_scene_draw(chain, id, target);
                temporal_frame.camera++;
            } else if (slot->depth != world.depth || slot->color_image != world.color_image ||
                       slot->depth_image != world.depth_image ||
                       slot->sample.color_allocation != world.sample.color_allocation ||
                       slot->sample.depth_allocation != world.sample.depth_allocation ||
                       memcmp(slot->sample.camera.rows, world.sample.camera.rows, sizeof world.sample.camera.rows) ||
                       !temporal_same_extent(&slot->sample, &world.sample)) {
                slot->inconsistent = true;
                wide_frame_replace(chain, id, (uintptr_t)color);
                if (temporal_frame.world.color == color) {
                    temporal_frame.world = (TemporalWorld){0};
                    temporal_frame.current = (VkTemporalScene){0};
                }
            }
        }
    }
    if (st.rt.depth || !indexed || count != 6 || instances != 1 || program->fi->ntex != 2 ||
        !wide_frame_scene_image(chain, id, g_cur_fs_hash, target, wide_surface(first), wide_surface(second))) return;
    const NvnTexture *source = wide_frame_surface_is(wide_surface(first), 1408, 792, 0x3f) ? first : second;
    const TexBackend *scene = temporal_image(color, VK_FORMAT_B10G11R11_UFLOAT_PACK32);
    const TexBackend *world_color = temporal_image(source, VK_FORMAT_B10G11R11_UFLOAT_PACK32);
    temporal_frame.scene = NULL;
    temporal_frame.current = (VkTemporalScene){0};
    for (unsigned i = 0; i < 4; i++) {
        TemporalWorld *world = &temporal_frame.worlds[i];
        if (world->color != source || world->inconsistent || !scene || !scene->allocation_id || !world_color ||
            world_color->image != world->color_image || world_color->allocation_id != world->sample.color_allocation)
            continue;
        if (scene->width != world->sample.width || scene->height != world->sample.height) break;
        temporal_frame.scene = color;
        temporal_frame.scene_image = scene->image;
        temporal_frame.scene_allocation = scene->allocation_id;
        temporal_frame.world = *world;
        break;
    }
}

static void temporal_copy(const NvnTexture *source, const NvnTexture *destination, bool full) {
    const uint64_t id = renderer_present_count();
    temporal_begin(id);
    temporal_forget_world(destination);
    if (temporal_frame.scene == destination) {
        temporal_frame.scene = NULL;
        temporal_frame.current = (VkTemporalScene){0};
    }
    if (temporal_frame.presented == destination) temporal_frame.presented = NULL;
    if (!wide_frame_copy(&temporal_frame.lineage, id, wide_surface(source), wide_surface(destination), full)) return;
    const TexBackend *out = destination ? destination->backend : NULL;
    if (!out || !out->image) return;
    temporal_frame.presented = destination;
    temporal_frame.presented_image = out->image;
    temporal_frame.copy++;
}

bool vk_temporal_scene_current(VkTemporalScene *scene) {
    if (!vk_temporal_scene_enabled || !scene || temporal_frame.current.frame != renderer_present_count() + 1 ||
        temporal_frame.current.sample.surface != vk_temporal_surface_generation() ||
        temporal_frame.current.sample.output_width != g_vk.sc_extent.width ||
        temporal_frame.current.sample.output_height != g_vk.sc_extent.height ||
        temporal_frame.current.sample.output_format != (uint32_t)g_vk.sc_format ||
        !temporal_current_images()) return false;
    *scene = temporal_frame.current;
    return true;
}

void vk_temporal_scene_present(const NvnTexture *texture, uint64_t frame, bool submitted) {
    const uint64_t id = frame - 1;
    temporal_begin(id);
    const TexBackend *out = texture ? texture->backend : NULL;
    const bool exact = temporal_frame.current.frame == frame && temporal_frame.hud && temporal_frame.presented == texture &&
        out && out->image == temporal_frame.presented_image && wide_frame_is_wide(&temporal_frame.lineage, id, (uintptr_t)texture) &&
        temporal_current_images();
    TemporalSample sample = temporal_frame.current.sample;
    const uint64_t surface = vk_temporal_surface_generation();
    const bool surface_changed = sample.surface != surface;
    sample.surface = surface;
    sample.output_width = g_vk.sc_extent.width;
    sample.output_height = g_vk.sc_extent.height;
    sample.output_format = g_vk.sc_format;
    const bool valid = exact && sample.output_width && sample.output_height && sample.output_format != VK_FORMAT_UNDEFINED;
    const uint64_t previous = temporal_history.frame;
    TemporalDecision action = temporal_history_commit(&temporal_history, frame, submitted, valid ? &sample : NULL);
    if (submitted && valid && surface_changed && action == TEMPORAL_REUSE) action = TEMPORAL_SURFACE;
    if (!temporal_repeats || action != temporal_last_action) temporal_repeats = 1;
    else if (temporal_repeats < 5) temporal_repeats++;
    temporal_last_action = action;
    if (temporal_repeats == 1 || (action == TEMPORAL_REUSE && temporal_repeats < 5)) {
        const char *const names[] = {"invalid", "unsubmitted", "first", "resume", "gap", "surface", "extent", "cut", "reuse", "backing"};
        LOG("[temporal-scene] frame=%llu action=%s valid=%u submitted=%u previous=%llu scene=%ux%u output=%ux%u camera=%u composite=%u hud=%u copy=%u\n",
            (unsigned long long)frame, names[action], valid, submitted, (unsigned long long)previous,
            valid ? sample.width : 0, valid ? sample.height : 0, sample.output_width, sample.output_height,
            temporal_frame.camera, temporal_frame.composite, temporal_frame.hud, temporal_frame.copy);
    }
    // Only the scalar history and copied camera survive a guest Present.
    memset(&temporal_frame, 0, sizeof temporal_frame);
}
