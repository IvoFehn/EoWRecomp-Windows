// Vulkan device, window, swapchain and presentation (counterpart of the window/present half of metal.m).
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#include "../../input_bind.h"
#include "../../paths.h"
#include "../../platform/discord_presence.h"
#include "../../settings.h"
#include "../../ui/menu.h"
#include "../../ui/touch.h"
#include "present_stats.h"
#include "vk.h"
#include "window_display.h"
#ifdef _WIN32
#include "surface_format.h"
#endif
#include "../output_scale.h"
#include "../renderer.h"

VkG g_vk;
static SDL_Window *window;
static atomic_bool ui_ready;
static VkDebugUtilsMessengerEXT messenger;
#ifdef _WIN32
static bool memory_stats, memory_budget_ext;
#endif

#ifndef EOWRECOMP_VK_CACHE_BUILD_ID
#define EOWRECOMP_VK_CACHE_BUILD_ID "unversioned"
#endif

static const char pipeline_cache_path[] = "cache/vk_pipelines.bin";
static const char pipeline_cache_magic[8] = "EOWVKC1";
static const char pipeline_cache_build_id[17] = EOWRECOMP_VK_CACHE_BUILD_ID;
enum { PIPELINE_CACHE_HEADER_SIZE = 32, PIPELINE_CACHE_MAX_SIZE = 256 * 1024 * 1024 };

const char *vk_cache_build_id(void) { return pipeline_cache_build_id; }

typedef struct {
    char magic[8];
    uint64_t checksum;
    uint32_t payload_size, driver_version;
    char build_id[17];
    uint8_t reserved[7];
} PipelineCacheFile;
_Static_assert(sizeof(PipelineCacheFile) == 48, "pipeline cache file header size");

static uint64_t pipeline_cache_checksum(const void *payload, size_t size) {
    const uint8_t *bytes = payload;
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; i++) hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}

static uint32_t pipeline_cache_word(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool pipeline_cache_matches_device(const uint8_t *payload, size_t size) {
    return size >= PIPELINE_CACHE_HEADER_SIZE && pipeline_cache_word(payload) == PIPELINE_CACHE_HEADER_SIZE &&
           pipeline_cache_word(payload + 4) == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
           pipeline_cache_word(payload + 8) == g_vk.props.vendorID && pipeline_cache_word(payload + 12) == g_vk.props.deviceID &&
           !memcmp(payload + 16, g_vk.props.pipelineCacheUUID, VK_UUID_SIZE);
}

static void *pipeline_cache_load(size_t *size, const char **state) {
    *state = "missing";
    FILE *file = fopen(pipeline_cache_path, "rb");
    if (!file) return NULL;
    void *payload = NULL;
    PipelineCacheFile header;
    *state = "invalid-file";
    if (fread(&header, sizeof header, 1, file) != 1 ||
        memcmp(header.magic, pipeline_cache_magic, sizeof header.magic) ||
        header.payload_size < PIPELINE_CACHE_HEADER_SIZE || header.payload_size > PIPELINE_CACHE_MAX_SIZE) goto done;
    if (header.driver_version != g_vk.props.driverVersion ||
        memcmp(header.build_id, pipeline_cache_build_id, sizeof header.build_id)) {
        *state = "incompatible-build-or-driver";
        goto done;
    }
    if (fseek(file, 0, SEEK_END) || ftell(file) != (long)(sizeof header + header.payload_size) ||
        fseek(file, (long)sizeof header, SEEK_SET)) goto done;
    payload = malloc(header.payload_size);
    if (!payload) {
        *state = "allocation-failed";
        goto done;
    }
    if (fread(payload, 1, header.payload_size, file) != header.payload_size || fgetc(file) != EOF || ferror(file) ||
        pipeline_cache_checksum(payload, header.payload_size) != header.checksum) goto done;
    if (!pipeline_cache_matches_device(payload, header.payload_size)) {
        *state = "incompatible-device";
        goto done;
    }
    *state = "loaded";
    *size = header.payload_size;
    fclose(file);
    return payload;
done:
    free(payload);
    fclose(file);
    return NULL;
}

static bool pipeline_cache_store(const void *payload, size_t size) {
    PipelineCacheFile header = {.checksum = pipeline_cache_checksum(payload, size), .payload_size = (uint32_t)size,
                                .driver_version = g_vk.props.driverVersion};
    memcpy(header.magic, pipeline_cache_magic, sizeof header.magic);
    memcpy(header.build_id, pipeline_cache_build_id, sizeof header.build_id);
    mkdir("cache", 0755);
    char path[] = "cache/vk_pipelines.bin.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return false;
#ifdef _WIN32
    if (_setmode(fd, _O_BINARY) < 0) { close(fd); remove(path); return false; }
#endif
    FILE *file = fdopen(fd, "wb");
    bool ok = false;
    if (file) {
        ok = fwrite(&header, sizeof header, 1, file) == 1 && fwrite(payload, 1, size, file) == size;
        ok = fclose(file) == 0 && ok;
    } else close(fd);
    if (ok && SDL_RenamePath(path, pipeline_cache_path)) return true;
    remove(path);
    return false;
}

#if defined(_WIN32) && defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2) && defined(VK_KHR_calibrated_timestamps)
#define EOW_VK_DISPLAY_TIMING 1
static bool timing_instance;
static bool timing_requested;
static bool timing_swapchain_rejected;
static PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR timing_caps;
static PFN_vkSetSwapchainPresentTimingQueueSizeEXT timing_queue_size;
static PFN_vkGetSwapchainTimeDomainPropertiesEXT timing_domains;
static PFN_vkGetPastPresentationTimingEXT timing_results;
static struct {
    VkPresentStageFlagsEXT stage;
    uint64_t domain, domains_counter, next_id, last_id, last_time, sample_start, last_feedback, last_poll;
    uint32_t pending, sample_count;
    double fps;
    bool valid;
} display_timing;
enum { TIMING_QUEUE_SIZE = 64, TIMING_BATCH_SIZE = 16 };

static bool request_display_timing(void) {
    const char *value = getenv("EOWRECOMP_VK_DISPLAY_TIMING");
    return value ? !strcmp(value, "1") : settings_vulkan_timing();
}

static bool has_timing_instance_extension(void) {
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS || !count) return false;
    VkExtensionProperties *extensions = calloc(count, sizeof *extensions);
    if (!extensions) return false;
    VkResult result = vkEnumerateInstanceExtensionProperties(NULL, &count, extensions);
    bool found = false;
    for (uint32_t i = 0; result == VK_SUCCESS && i < count; i++)
        if (!strcmp(extensions[i].extensionName, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) found = true;
    free(extensions);
    return found;
}

static const char *timing_stage_name(void) {
    return display_timing.stage == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT ? "first pixel visible" : "first pixel out";
}

static void stop_display_timing(const char *reason, VkResult result) {
    LOG("[display-timing] %s (%d); timing unavailable\n", reason, result);
    display_timing.stage = 0;
    display_timing.valid = false;
}

static VkSwapchainCreateFlagsKHR display_timing_flags(void) {
    memset(&display_timing, 0, sizeof display_timing);
    if (timing_swapchain_rejected || !timing_queue_size || !timing_domains || !timing_results) return 0;
    VkSurfaceCapabilitiesPresentId2KHR id = {.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR};
    VkPresentTimingSurfaceCapabilitiesEXT timing = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT, .pNext = &id};
    VkSurfaceCapabilities2KHR caps = {.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, .pNext = &timing};
    VkPhysicalDeviceSurfaceInfo2KHR surface = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, .surface = g_vk.surface};
    VkResult result = timing_caps(g_vk.phys, &surface, &caps);
    if (result == VK_SUCCESS && timing.presentTimingSupported && id.presentId2Supported) {
        display_timing.stage = timing.presentStageQueries & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT;
        if (!display_timing.stage) display_timing.stage = timing.presentStageQueries & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
    }
    if (display_timing.stage) return VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT | VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
    stop_display_timing("surface has no output stage", result);
    return 0;
}

static void begin_display_timing(void) {
    VkTimeDomainKHR domains[16];
    uint64_t ids[SDL_arraysize(domains)];
    VkSwapchainTimeDomainPropertiesEXT properties = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT,
        .timeDomainCount = SDL_arraysize(domains), .pTimeDomains = domains, .pTimeDomainIds = ids};
    VkResult result = timing_domains(g_vk.dev, g_vk.swapchain, &properties, &display_timing.domains_counter);
    int best = -1;
    for (uint32_t i = 0; (result == VK_SUCCESS || result == VK_INCOMPLETE) && i < properties.timeDomainCount; i++) {
        if (domains[i] == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR) { best = (int)i; break; }
        if (domains[i] == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT) best = (int)i;
    }
    if (best < 0) { stop_display_timing("no usable swapchain time domain", result); return; }
    result = timing_queue_size(g_vk.dev, g_vk.swapchain, TIMING_QUEUE_SIZE);
    if (result != VK_SUCCESS) { stop_display_timing("timing queue setup failed", result); return; }
    display_timing.domain = ids[best];
    LOG("[display-timing] stage %s domain %d id %llu\n", timing_stage_name(), domains[best], (unsigned long long)ids[best]);
}

static void collect_display_timing(void) {
    if (!display_timing.pending) return;
    uint64_t now = SDL_GetTicksNS();
    if (now - display_timing.last_poll < 32000000 && display_timing.pending < TIMING_QUEUE_SIZE - TIMING_BATCH_SIZE) return;
    display_timing.last_poll = now;
    VkPastPresentationTimingEXT rows[TIMING_BATCH_SIZE];
    VkPresentStageTimeEXT stages[TIMING_BATCH_SIZE];
    for (uint32_t i = 0; i < TIMING_BATCH_SIZE; i++) rows[i] = (VkPastPresentationTimingEXT){
        .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT, .presentStageCount = 1, .pPresentStages = &stages[i]};
    VkPastPresentationTimingInfoEXT query = {.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT, .swapchain = g_vk.swapchain};
    VkPastPresentationTimingPropertiesEXT results = {.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT,
        .presentationTimingCount = TIMING_BATCH_SIZE, .pPresentationTimings = rows};
    VkResult result = timing_results(g_vk.dev, &query, &results);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) { stop_display_timing("timing query failed", result); return; }
    if (results.timeDomainsCounter != display_timing.domains_counter) {
        stop_display_timing("swapchain time domains changed", VK_SUCCESS);
        return;
    }
    for (uint32_t i = 0; i < results.presentationTimingCount; i++) {
        if (!rows[i].reportComplete) continue;
        if (display_timing.pending) display_timing.pending--;
        if (rows[i].presentId <= display_timing.last_id) continue;
        display_timing.last_id = rows[i].presentId;
        if (rows[i].timeDomainId != display_timing.domain || !rows[i].presentStageCount ||
            stages[i].stage != display_timing.stage || stages[i].time <= display_timing.last_time) continue;
        display_timing.last_time = stages[i].time;
        display_timing.last_feedback = now;
        if (!display_timing.sample_start) display_timing.sample_start = stages[i].time;
        else display_timing.sample_count++;
        uint64_t span = stages[i].time - display_timing.sample_start;
        if (span < 500000000) continue;
        display_timing.fps = display_timing.sample_count * 1e9 / span;
        display_timing.sample_start = stages[i].time;
        display_timing.sample_count = 0;
        display_timing.valid = true;
    }
}
#endif

// ================================================================== instance / device
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    static int n;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && n++ < 500) LOG("[vk] %s\n", d->pMessage);
    return VK_FALSE;
}

// EOWRECOMP_VK_VALIDATION=1: Khronos validation layer (MSYS2 package vulkan-validation-layers)
static void create_instance(void) {
#ifdef VK_NO_PROTOTYPES
    if (volkInitialize() != VK_SUCCESS) FATAL("no Vulkan driver\n");
#endif
    Uint32 n_sdl = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_sdl);
    const char *ext[16];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 14; i++) ext[n_ext++] = sdl_ext[i];
    VkInstanceCreateFlags flags = 0;
#ifdef __APPLE__
    // MoltenVK is a portability driver: the loader lists it only to instances that ask for those
    ext[n_ext++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
    bool validate = getenv("EOWRECOMP_VK_VALIDATION") != NULL;
#ifdef EOW_VK_DISPLAY_TIMING
    timing_requested = request_display_timing();
    timing_instance = timing_requested && has_timing_instance_extension();
    if (timing_instance) {
        bool already_enabled = false;
        for (uint32_t i = 0; i < n_ext; i++)
            if (!strcmp(ext[i], VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) already_enabled = true;
        if (!already_enabled) ext[n_ext++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
    }
#endif
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "EoWRecomp",
                             .pEngineName = "EoWRecomp", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .flags = flags, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && validate) {
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;
        validate = false;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
    if (r != VK_SUCCESS) FATAL("vkCreateInstance: %d (is a Vulkan 1.3 driver installed?)\n", r);
#ifdef VK_NO_PROTOTYPES
    volkLoadInstance(g_vk.inst);
#endif
#ifdef EOW_VK_DISPLAY_TIMING
    if (timing_instance)
        timing_caps = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)vkGetInstanceProcAddr(g_vk.inst, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
#endif
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        if (f) f(g_vk.inst, &mi, NULL, &messenger);
    }
}

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

// Discrete GPU first (EOWRECOMP_GPU=<index> picks one), needs Vulkan 1.3 and a queue that can present.
static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    int want = getenv("EOWRECOMP_GPU") ? atoi(getenv("EOWRECOMP_GPU")) : -1, best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        LOG("[vk] gpu %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if ((int)i == want) score = 100;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) FATAL("no Vulkan 1.3 device\n");
    g_vk.phys = pd[best];
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) { g_vk.qfam = i; break; }
    }
    free(qp);
    if (g_vk.qfam == UINT32_MAX) FATAL("no graphics+present queue\n");
    LOG("[vk] using %s\n", g_vk.props.deviceName);
}

static bool g_fault_ext;
// After VK_ERROR_DEVICE_LOST: what the driver says faulted (VK_EXT_device_fault), into the log.
void vk_device_fault_report(void) {
    static int once;
    if (once++ || !g_fault_ext) return;
    PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetDeviceFaultInfoEXT");
    if (!get) return;
    VkDeviceFaultCountsEXT c = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (get(g_vk.dev, &c, NULL) != VK_SUCCESS) { LOG("[vk] device fault: no info\n"); return; }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT vend[16];
    if (c.addressInfoCount > 16) c.addressInfoCount = 16;
    if (c.vendorInfoCount > 16) c.vendorInfoCount = 16;
    c.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = vend};
    if (get(g_vk.dev, &c, &info) != VK_SUCCESS && 0) return;
    LOG("[vk] device fault: %s\n", info.description);
    static const char *const kind[] = {"none", "read invalid", "write invalid", "execute invalid", "ip unknown", "ip invalid", "ip valid"};
    for (uint32_t i = 0; i < c.addressInfoCount; i++)
        LOG("[vk]   address %s %#llx (precision %#llx)\n", addr[i].addressType < 7 ? kind[addr[i].addressType] : "?",
            (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
    for (uint32_t i = 0; i < c.vendorInfoCount; i++)
        LOG("[vk]   vendor: %s (code %#llx data %#llx)\n", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
            (unsigned long long)vend[i].vendorFaultData);
}

static void create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    const char *ext[16];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
#ifdef _WIN32
    memory_stats = ENV("EOWRECOMP_VK_MEMORY_STATS");
    memory_budget_ext = memory_stats && has_ext(ep, ne, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (memory_budget_ext) ext[n_ext++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
#endif
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.host_import = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);  // guest heap as a GPU buffer (exec.c)
    if (g_vk.host_import) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    // a portability implementation (MoltenVK) must have its subset extension enabled when it lists it
    if (has_ext(ep, ne, "VK_KHR_portability_subset")) ext[n_ext++] = "VK_KHR_portability_subset";
    bool fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);  // device-lost diagnostics (vk_device_fault_report)
    if (fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    g_fault_ext = fault;
#ifdef EOW_VK_DISPLAY_TIMING
    VkPhysicalDevicePresentId2FeaturesKHR id2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
    VkPhysicalDevicePresentTimingFeaturesEXT timing = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT, .pNext = &id2};
    bool timing_enabled = timing_caps && has_ext(ep, ne, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) &&
                          has_ext(ep, ne, VK_KHR_PRESENT_ID_2_EXTENSION_NAME) && has_ext(ep, ne, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
    if (timing_enabled) {
        VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &timing};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &query);
        timing_enabled = timing.presentTiming && id2.presentId2;
    }
#endif
    free(ep);

    // features the backend relies on (all present on current desktop drivers)
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_FALSE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE,
                                            .shaderDemoteToHelperInvocation = VK_TRUE, .shaderTerminateInvocation = VK_TRUE,
                                            .maintenance4 = VK_TRUE,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = VK_TRUE, .shaderInt8 = VK_TRUE, .samplerMirrorClampToEdge = VK_TRUE,
                                            .uniformAndStorageBuffer8BitAccess = VK_TRUE, .storageBuffer8BitAccess = VK_TRUE,
                                            .scalarBlockLayout = VK_TRUE, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = VK_TRUE, .pNext = &f12};
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE,
                                           .pNext = &f11};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = fault ? (void *)&ff : (void *)&f11};
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    b->textureCompressionBC = h->textureCompressionBC;
    b->shaderInt64 = h->shaderInt64;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    b->shaderClipDistance = h->shaderClipDistance;
    if (!h->shaderInt64) LOG("[vk] warning: no shaderInt64 (64-bit address math in shaders)\n");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
#ifdef EOW_VK_DISPLAY_TIMING
    if (timing_enabled) {
        ext[n_ext] = VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
        ext[n_ext + 1] = VK_KHR_PRESENT_ID_2_EXTENSION_NAME;
        ext[n_ext + 2] = VK_EXT_PRESENT_TIMING_EXTENSION_NAME;
        di.enabledExtensionCount += 3;
        timing.presentAtAbsoluteTime = timing.presentAtRelativeTime = VK_FALSE;
        id2.pNext = f.pNext;
        f.pNext = &timing;
    }
    VkResult result = vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev);
    if (result != VK_SUCCESS && timing_enabled) {
        LOG("[display-timing] device creation failed (%d); retry without timing\n", result);
        di.enabledExtensionCount = n_ext;
        f.pNext = id2.pNext;
        timing_enabled = false;
        result = vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev);
    }
    VK_CHECK(result);
    if (timing_enabled) {
        timing_queue_size = (PFN_vkSetSwapchainPresentTimingQueueSizeEXT)vkGetDeviceProcAddr(g_vk.dev, "vkSetSwapchainPresentTimingQueueSizeEXT");
        timing_domains = (PFN_vkGetSwapchainTimeDomainPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetSwapchainTimeDomainPropertiesEXT");
        timing_results = (PFN_vkGetPastPresentationTimingEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetPastPresentationTimingEXT");
    }
    if (timing_requested && (!timing_queue_size || !timing_domains || !timing_results))
        LOG("[display-timing] required extensions, features or commands unavailable\n");
#else
    VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
#endif
#ifdef VK_NO_PROTOTYPES
    volkLoadDevice(g_vk.dev);
#endif
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .vulkanApiVersion = VK_API_VERSION_1_3};
#ifdef VK_NO_PROTOTYPES
    VmaVulkanFunctions vf = {.vkGetInstanceProcAddr = vkGetInstanceProcAddr, .vkGetDeviceProcAddr = vkGetDeviceProcAddr};
    ai.pVulkanFunctions = &vf;
#endif
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));

    size_t sz = 0;
    const char *cache_state;
    void *data = pipeline_cache_load(&sz, &cache_state);
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz,
                                    .pInitialData = data};
    if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) {
        cache_state = "driver-rejected";
        pc.initialDataSize = 0;
        pc.pInitialData = NULL;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache));
    }
    if (ENV("EOWRECOMP_VK_CACHE_STATS"))
        LOG("[vk-cache] pipeline load=%s bytes=%zu build=%s vendor=%04x device=%04x driver=%08x\n", cache_state,
            pc.initialDataSize, pipeline_cache_build_id, g_vk.props.vendorID, g_vk.props.deviceID, g_vk.props.driverVersion);
    free(data);
}

void vk_pipeline_cache_save(void) {
    size_t sz = 0;
    if (!g_vk.pcache || vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) != VK_SUCCESS ||
        sz < PIPELINE_CACHE_HEADER_SIZE || sz > PIPELINE_CACHE_MAX_SIZE) return;
    void *data = malloc(sz);
    if (!data) return;
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, data) == VK_SUCCESS && pipeline_cache_matches_device(data, sz)) {
        bool stored = pipeline_cache_store(data, sz);
        if (ENV("EOWRECOMP_VK_CACHE_STATS")) LOG("[vk-cache] pipeline save=%s bytes=%zu\n", stored ? "stored" : "failed", sz);
    }
    free(data);
}

#ifdef _WIN32
static void report_gpu_memory(uint64_t frame) {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 memory = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                                .pNext = memory_budget_ext ? &driver : NULL};
    vkGetPhysicalDeviceMemoryProperties2(g_vk.phys, &memory);
    VmaBudget vma[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(g_vk.vma, vma);
    for (uint32_t i = 0; i < memory.memoryProperties.memoryHeapCount; i++) {
        VkMemoryHeap heap = memory.memoryProperties.memoryHeaps[i];
        uint64_t budget = memory_budget_ext ? driver.heapBudget[i] : vma[i].budget;
        uint64_t usage = memory_budget_ext ? driver.heapUsage[i] : vma[i].usage;
        LOG("[vk-memory] frame=%llu heap=%u device_local=%u source=%s heap_bytes=%llu budget_bytes=%llu usage_bytes=%llu "
            "vma_block_bytes=%llu vma_allocation_bytes=%llu\n", (unsigned long long)frame, i,
            (unsigned)!!(heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT), memory_budget_ext ? "driver" : "vma-estimate",
            (unsigned long long)heap.size, (unsigned long long)budget, (unsigned long long)usage,
            (unsigned long long)vma[i].statistics.blockBytes, (unsigned long long)vma[i].statistics.allocationBytes);
    }
}
#endif

// ================================================================== swapchain
#ifdef _WIN32
static uint64_t temporal_surface_generation;

uint64_t vk_temporal_surface_generation(void) { return temporal_surface_generation; }
#endif
static bool g_sc_turned;  // the compositor turns our images (IDENTITY on a rotated display): presents say "suboptimal"
static const char *active_present_mode = "unavailable";
static VkPresentModeKHR present_mode(void) {
    // MAILBOX by default: the game paces itself (vi.c's emulated 60 Hz vsync), and with FIFO every submission
    // queued behind the one waiting for a window image, so the game's fence waits followed the presentation engine:
    // a throttled / captured / busy compositor slowed the whole game (4-21 fps with the GPU mostly idle; mailbox:
    // 60). No tearing. EOWRECOMP_VK_PRESENT=fifo|immediate|mailbox to choose.
    const char *e = getenv("EOWRECOMP_VK_PRESENT");
    VkPresentModeKHR want = !e || !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}

static void destroy_swapchain(void) {
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    g_vk.sc_count = 0;
    if (g_vk.swapchain) vkDestroySwapchainKHR(g_vk.dev, g_vk.swapchain, NULL);
    g_vk.swapchain = VK_NULL_HANDLE;
}

static VkResult create_swapchain_handle(VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *swapchain) {
    VkResult result = vkCreateSwapchainKHR(g_vk.dev, ci, NULL, swapchain);
#ifdef EOW_VK_DISPLAY_TIMING
    if (ci->flags && result == VK_ERROR_INITIALIZATION_FAILED) {
        timing_swapchain_rejected = true;
        stop_display_timing("swapchain creation failed; retrying without timing flags", result);
        // Creation retires oldSwapchain even on failure. Recreation already waited for rendering work.
        destroy_swapchain();
        ci->oldSwapchain = VK_NULL_HANDLE;
        ci->flags = 0;
        result = vkCreateSwapchainKHR(g_vk.dev, ci, NULL, swapchain);
    }
#endif
    return result;
}

static atomic_bool g_background, g_surface_stale;
#ifdef _WIN32
static bool sdr_format_unavailable;
#endif

static bool create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VkResult caps_result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps);
#ifdef _WIN32
    if (caps_result == VK_ERROR_SURFACE_LOST_KHR && g_vk.sc_format != VK_FORMAT_UNDEFINED) {
        atomic_store(&g_surface_stale, true);
        return false;
    }
#endif
    VK_CHECK(caps_result);
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
#ifdef _WIN32
    VkSurfaceFormatKHR sf;
    VkResult format_result = vk_query_sdr_surface_format(vkGetPhysicalDeviceSurfaceFormatsKHR,
                                                         g_vk.phys, g_vk.surface, g_vk.sc_format, &sf);
    if (format_result == VK_ERROR_SURFACE_LOST_KHR && g_vk.sc_format != VK_FORMAT_UNDEFINED) {
        atomic_store(&g_surface_stale, true);
        return false;
    }
    if (format_result == VK_INCOMPLETE) {
        if (g_vk.sc_format == VK_FORMAT_UNDEFINED) FATAL("[vk] SDR surface format enumeration remained incomplete\n");
        return false;
    }
    if (format_result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
        if (g_vk.sc_format == VK_FORMAT_UNDEFINED)
            FATAL("[vk] surface has no supported SDR UNORM format\n");
        if (!sdr_format_unavailable)
            LOG("[vk] surface does not support the active SDR UNORM format %d; waiting for a compatible surface\n",
                g_vk.sc_format);
        sdr_format_unavailable = true;
        return false;
    }
    VK_CHECK(format_result);
    // The launcher can initialize ImGui before a minimized window has drawable images.
    if (!ext.width || !ext.height) {
        g_vk.sc_format = sf.format;
        return false;
    }
#else
    if (!ext.width || !ext.height) return false;
#endif
    // A rotated display (a phone held in landscape: currentTransform 90 / 270): the images are made in the window's
    // own (landscape) shape - the extent it reports - and the compositor turns them (IDENTITY). (preTransform =
    // currentTransform would say they were turned already: the picture came out sideways. LASRecomp-Android-Plus
    // pre-rotates everything it draws instead, which saves the compositor's pass: a later optimisation.)
    VkSurfaceTransformFlagBitsKHR xf = caps.currentTransform;
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) xf = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    g_vk.sc_quarter = 0;
    bool turned_by_system = xf != caps.currentTransform;
#ifdef __ANDROID__
    // Pre-rotation: the images in the panel's own (portrait) orientation with its transform, everything drawn into
    // them turned (vk_sc_rect, the present and UI vertex shaders). Otherwise the compositor turns the whole landscape
    // layer with the GPU at every present: ~13% of the GPU's time on a Snapdragon 865 (LASRecomp-Android-Plus).
    // EOWRECOMP_PRETRANSFORM=0: the system turns them.
    static int pre = -1;
    if (pre < 0) pre = getenv("EOWRECOMP_PRETRANSFORM") ? atoi(getenv("EOWRECOMP_PRETRANSFORM")) != 0 : 1;
    if (pre && (caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) {
        xf = caps.currentTransform;
        g_vk.sc_quarter = xf == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ? 1 : 3;
        turned_by_system = false;
    }
#endif
    g_sc_turned = turned_by_system;
#ifndef _WIN32
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    // scanout shows the bytes the game wrote (see the raw views in the present pass): a UNORM swapchain like Metal's
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
#endif
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    // (Android's compositors often take INHERIT only)
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
        for (uint32_t b = 1; b <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; b <<= 1)
            if (caps.supportedCompositeAlpha & b) { alpha = (VkCompositeAlphaFlagBitsKHR)b; break; }
    VkExtent2D phys = g_vk.sc_quarter ? (VkExtent2D){ext.height, ext.width} : ext;
    VkPresentModeKHR mode = present_mode();
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = phys, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = xf,
        .compositeAlpha = alpha, .presentMode = mode, .clipped = VK_TRUE,
        .oldSwapchain = g_vk.swapchain};
#ifdef EOW_VK_DISPLAY_TIMING
    ci.flags = display_timing_flags();
#endif
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkResult create_result = create_swapchain_handle(&ci, &swapchain);
#ifdef _WIN32
    if (create_result == VK_ERROR_SURFACE_LOST_KHR && g_vk.sc_format != VK_FORMAT_UNDEFINED) {
        atomic_store(&g_surface_stale, true);
        return false;
    }
#endif
    VK_CHECK(create_result);
    destroy_swapchain();
    g_vk.swapchain = swapchain;
#ifdef _WIN32
    temporal_surface_generation++;
#endif
#ifdef EOW_VK_DISPLAY_TIMING
    if (ci.flags) begin_display_timing();
#endif
    active_present_mode = mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" :
                          mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "FIFO";
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    g_vk.sc_phys = phys;
#ifdef _WIN32
    sdr_format_unavailable = false;
    LOG("[vk] Windows SDR surface format %d color space %d\n", sf.format, sf.colorSpace);
#endif
    if (g_vk.sc_quarter) LOG("[vk] swapchain pre-rotated %d degrees (images %ux%u)\n", g_vk.sc_quarter * 90, phys.width, phys.height);
    g_vk.sc_count = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
    }
    return true;
}

VkRect2D vk_sc_rect(int x, int y, int w, int h) {
    int W = (int)g_vk.sc_extent.width, H = (int)g_vk.sc_extent.height;
    if (g_vk.sc_quarter == 1) return (VkRect2D){{H - y - h, x}, {(uint32_t)h, (uint32_t)w}};
    if (g_vk.sc_quarter == 3) return (VkRect2D){{y, W - x - w}, {(uint32_t)h, (uint32_t)w}};
    return (VkRect2D){{x, y}, {(uint32_t)w, (uint32_t)h}};
}

static bool recreate_swapchain(void) {
    vkDeviceWaitIdle(g_vk.dev);
    return create_swapchain();
}

// Phones: an app in the background has no window - its surface is gone, and drawing waits until it's in front again,
// then the window gets a new surface and swapchain (after LASRecomp-Android-Plus).
#ifdef _WIN32
static bool surface_retry_since_present;
static SDL_WindowID presentation_window_id;
static atomic_bool presentation_window_minimized;
#endif

static bool SDLCALL lifecycle_watch(void *user, SDL_Event *e) {
    (void)user;
#ifdef _WIN32
    // SDL owns the window on the UI thread. The guest can present on another thread.
    if ((e->type == SDL_EVENT_WINDOW_MINIMIZED || e->type == SDL_EVENT_WINDOW_RESTORED ||
         e->type == SDL_EVENT_WINDOW_MAXIMIZED) && e->window.windowID == presentation_window_id)
        atomic_store(&presentation_window_minimized, e->type == SDL_EVENT_WINDOW_MINIMIZED);
#endif
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND || e->type == SDL_EVENT_DID_ENTER_BACKGROUND) {
        if (!atomic_exchange(&g_background, true)) {
            LOG("[vk] in the background: drawing paused\n");
            if (g_vk.dev) vk_pipeline_cache_save();  // (phones end apps in the background without exit())
        }
    } else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND && atomic_load(&g_background)) {
        LOG("[vk] in front again\n");
        atomic_store(&g_surface_stale, true);
        atomic_store(&g_background, false);
    }
    return true;
}

static void new_surface(void) {
    vkDeviceWaitIdle(g_vk.dev);
    destroy_swapchain();
    if (g_vk.surface) vkDestroySurfaceKHR(g_vk.inst, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) {
        LOG("[vk] new surface: %s\n", SDL_GetError());
        atomic_store(&g_surface_stale, true);  // (the window isn't back yet: next frame)
        return;
    }
    create_swapchain();
}

// ================================================================== frames (start screen and game)
// Acquire semaphores rotate per frame in flight; a slot is reused once the submission that waited on it is done.
#define FRAMES 3
static VkSemaphore acquire_sem[FRAMES], done_sem[8];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

typedef struct { uint32_t image; VkSemaphore acquired; bool ok; } Frame;
static Frame cur_frame;

// Acquire the next window image and begin rendering into it (cleared to `clear`). Returns false when there is
// nothing to draw into (minimised window).
bool vk_frame_begin(const float clear[4]) {
    cur_frame.ok = false;
#ifdef _WIN32
    if (atomic_load(&presentation_window_minimized)) return false;
#endif
    int w, h;
    if (!SDL_GetWindowSizeInPixels(window, &w, &h) || !w || !h) return false;
    if (atomic_load(&g_background)) return false;
    if (atomic_exchange(&g_surface_stale, false)) new_surface();
#ifdef _WIN32
    if (atomic_load(&g_surface_stale)) return false;
#endif
    if (!g_vk.surface) return false;
    bool needs_swapchain = !g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height;
#ifdef _WIN32
    needs_swapchain |= sdr_format_unavailable;
#endif
    if (needs_swapchain && !recreate_swapchain()) return false;
    if (!g_vk.swapchain) return false;
    static int trace = -1;
    if (trace < 0) trace = getenv("EOWRECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] frame begin %dx%d (swapchain %ux%u)\n", w, h, g_vk.sc_extent.width, g_vk.sc_extent.height);
    slot = (slot + 1) % FRAMES;
    if (!acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
    vk_wait_seq(slot_seq[slot]);
    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        if (!recreate_swapchain()) return false;
        r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    }
    if (r == VK_ERROR_SURFACE_LOST_KHR) { atomic_store(&g_surface_stale, true); return false; }
#ifdef _WIN32
    if (r == VK_ERROR_OUT_OF_DATE_KHR && !surface_retry_since_present) {
        LOG("[vk] swapchain still out of date after recreation; replacing window surface\n");
        surface_retry_since_present = true;
        atomic_store(&g_surface_stale, true);
        return false;
    }
#endif
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LOG("[vk] acquire failed: %d\n", r); return false; }
    cur_frame.acquired = acquire_sem[slot];
    slot_seq[slot] = vk_pending_seq();
    VkCommandBuffer cb = vk_cmd();
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue.color.float32 = {clear[0], clear[1], clear[2], clear[3]}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_phys}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_phys.width, (float)g_vk.sc_phys.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_phys};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    extern void vk_imgui_prerotation(int quarter);  // (the UI drawn into this image turned like it: imgui_glue.cpp)
    vk_imgui_prerotation(g_vk.sc_quarter);
    cur_frame.ok = true;
    return true;
}

// Passes of their own between vk_frame_begin and vk_frame_end (exec.c post passes): end the window image's
// rendering, and begin it again keeping what was drawn.
void vk_frame_suspend(void) {
    if (cur_frame.ok) vkCmdEndRendering(vk_cmd());
}
void vk_frame_resume(void) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_phys}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_phys.width, (float)g_vk.sc_phys.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_phys};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
}

VkExtent2D vk_frame_extent(void) { return g_vk.sc_extent; }
void *vk_frame_cmd(void) { return (void *)vk_cmd(); }

static void save_window_image(VkCommandBuffer cb, VkSpan *out) {
    uint32_t w = g_vk.sc_phys.width, h = g_vk.sc_phys.height;
    *out = vk_readback((uint64_t)w * h * 4);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rg = {.bufferOffset = out->off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->buf, 1, &rg);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

#ifdef _WIN32
// Bounded SDR diagnostics. Keep a private staging slice for each sampled frame
// and read it only after the submission timeline has retired the whole range.
enum { TRACE_FRAMES = 512, TRACE_TILE = 16, TRACE_TILES = 5, TRACE_COPIES = TRACE_TILES + 1,
       TRACE_STRIDE = TRACE_COPIES * TRACE_TILE * TRACE_TILE * 4 };
static struct {
    bool configured, finished;
    uint64_t first, last, last_seq;
    VkBuffer buf;
    VmaAllocation allocation;
    const uint8_t *data;
    struct {
        LARGE_INTEGER before, after;
        VkResult result;
    } frames[TRACE_FRAMES];
} present_trace;

static void present_trace_clear(void) {
    if (present_trace.buf) vmaDestroyBuffer(g_vk.vma, present_trace.buf, present_trace.allocation);
    present_trace.buf = VK_NULL_HANDLE;
    present_trace.finished = true;
}

static void present_trace_poll(uint64_t frame) {
    if (!present_trace.configured) {
        unsigned first, last;
        char extra;
        const char *arg = getenv("EOWRECOMP_VK_TRACE_PRESENT");
        if (arg && sscanf(arg, "%u:%u%c", &first, &last, &extra) == 2 && first && last >= first && last - first < TRACE_FRAMES) {
            present_trace.first = first;
            present_trace.last = last;
        }
        present_trace.configured = true;
    }
    if (!present_trace.buf || frame <= present_trace.last || vk_completed_seq() < present_trace.last_seq) return;
    VK_CHECK(vmaInvalidateAllocation(g_vk.vma, present_trace.allocation, 0, VK_WHOLE_SIZE));
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    LOG("[vk-present-trace] first=%llu last=%llu qpc_hz=%lld tile=%u tiles=%u\n",
        (unsigned long long)present_trace.first, (unsigned long long)present_trace.last, (long long)freq.QuadPart,
        (unsigned)TRACE_TILE, (unsigned)TRACE_TILES);
    for (uint64_t id = present_trace.first; id <= present_trace.last; id++) {
        size_t slot = (size_t)(id - present_trace.first);
        if (!present_trace.frames[slot].before.QuadPart) continue;
        unsigned means[TRACE_TILES];
        for (unsigned tile = 0; tile < TRACE_TILES; tile++) {
            const uint8_t *px = present_trace.data + slot * TRACE_STRIDE + tile * TRACE_TILE * TRACE_TILE * 4;
            unsigned sum = 0;
            for (unsigned j = 0; j < TRACE_TILE * TRACE_TILE; j++, px += 4) sum += px[0] + px[1] + px[2];
            means[tile] = sum / (TRACE_TILE * TRACE_TILE * 3);
        }
        LOG("[vk-present-trace-row] frame=%llu qpc_begin=%lld qpc_end=%lld result=%d rgb_mean=%u,%u,%u,%u,%u\n",
            (unsigned long long)id, (long long)present_trace.frames[slot].before.QuadPart,
            (long long)present_trace.frames[slot].after.QuadPart, (int)present_trace.frames[slot].result,
            means[0], means[1], means[2], means[3], means[4]);
        const uint8_t *px = present_trace.data + slot * TRACE_STRIDE + TRACE_TILES * TRACE_TILE * TRACE_TILE * 4;
        unsigned green = 0;
        for (unsigned j = 0; j < TRACE_TILE * TRACE_TILE; j++, px += 4)
            green += px[1] > 25 && 2 * px[1] > 3 * px[0] && 2 * px[1] > 3 * px[2];
        LOG("[vk-present-foliage] frame=%llu green=%u pixels=%u\n", (unsigned long long)id, green,
            (unsigned)(TRACE_TILE * TRACE_TILE));
    }
    LOG("[vk-present-trace-end]\n");
    present_trace_clear();
}

static int present_trace_copy(VkCommandBuffer cb, uint64_t frame) {
    if (present_trace.finished || frame < present_trace.first || frame > present_trace.last) return -1;
    VkFormat format = g_vk.sc_format;
    uint32_t w = g_vk.sc_phys.width, h = g_vk.sc_phys.height;
    if ((format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_B8G8R8A8_SRGB &&
         format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB) ||
        w < 64 || h < 64 || g_vk.sc_quarter) return -1;
    if (!present_trace.buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = (present_trace.last - present_trace.first + 1) * TRACE_STRIDE, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &present_trace.buf, &present_trace.allocation, &info));
        present_trace.data = info.pMappedData;
    }
    static const unsigned xy[TRACE_COPIES][2] = {{50,50}, {38,42}, {62,42}, {38,63}, {70,70}, {82,82}};
    int slot = (int)(frame - present_trace.first);
    VkBufferImageCopy copies[TRACE_COPIES] = {0};
    for (unsigned i = 0; i < TRACE_COPIES; i++) {
        copies[i] = (VkBufferImageCopy){
            .bufferOffset = (uint64_t)slot * TRACE_STRIDE + i * TRACE_TILE * TRACE_TILE * 4,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {w * xy[i][0] / 100 - TRACE_TILE / 2, h * xy[i][1] / 100 - TRACE_TILE / 2, 0},
            .imageExtent = {TRACE_TILE, TRACE_TILE, 1}};
    }
    VkImage image = g_vk.sc_images[cur_frame.image];
    image_barrier(cb, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, present_trace.buf, TRACE_COPIES, copies);
    VkBufferMemoryBarrier2 host_read = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = present_trace.buf, .offset = (uint64_t)slot * TRACE_STRIDE, .size = TRACE_STRIDE};
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &host_read};
    vkCmdPipelineBarrier2(cb, &dependency);
    image_barrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    present_trace.last_seq = vk_pending_seq();
    return slot;
}
#endif

// End rendering into the window image and present it. shot_path: also save the window image as PNG.
static uint64_t frame_end(const char *shot_path, bool track_present, uint64_t guest_frame) {
    if (!cur_frame.ok) return 0;
    VkCommandBuffer cb = vk_cmd();
    vkCmdEndRendering(cb);
    VkSpan shot = {0};
    if (shot_path) save_window_image(cb, &shot);
#ifdef _WIN32
    int trace_slot = guest_frame && present_trace.first ? present_trace_copy(cb, guest_frame) : -1;
#else
    (void)guest_frame;
#endif
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    uint32_t img = cur_frame.image;
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    vk_submit_extra(cur_frame.acquired, done_sem[img], VK_NULL_HANDLE);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done_sem[img],
                           .swapchainCount = 1, .pSwapchains = &g_vk.swapchain, .pImageIndices = &img};
#ifdef EOW_VK_DISPLAY_TIMING
    uint64_t present_id = 0;
    VkPresentTimingInfoEXT timing = {.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT};
    VkPresentTimingsInfoEXT timings = {.sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT, .swapchainCount = 1, .pTimingInfos = &timing};
    VkPresentId2KHR id = {.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR, .pNext = &timings, .swapchainCount = 1, .pPresentIds = &present_id};
    if (track_present && display_timing.stage) {
        collect_display_timing();
        if (display_timing.pending == TIMING_QUEUE_SIZE) stop_display_timing("timing backlog limit reached", VK_SUCCESS);
        if (display_timing.stage) {
            present_id = ++display_timing.next_id;
            timing.timeDomainId = display_timing.domain;
            timing.presentStageQueries = display_timing.stage;
            pi.pNext = &id;
        }
    }
#endif
#ifdef _WIN32
    if (trace_slot >= 0) QueryPerformanceCounter(&present_trace.frames[trace_slot].before);
#endif
    VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);
#ifdef _WIN32
    if (trace_slot >= 0) {
        QueryPerformanceCounter(&present_trace.frames[trace_slot].after);
        present_trace.frames[trace_slot].result = r;
    }
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) surface_retry_since_present = false;
#endif
    uint64_t submitted_at = track_present && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) ? SDL_GetTicksNS() : 0;
#ifdef EOW_VK_DISPLAY_TIMING
    if (present_id && submitted_at) display_timing.pending++;
    // A queue-full result still consumes the present wait semaphore. Leave this image alone.
    if (present_id && r == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT) stop_display_timing("driver timing queue full", r);
#endif
    if (r == VK_ERROR_SURFACE_LOST_KHR) atomic_store(&g_surface_stale, true);
    // (a swapchain the system turns, IDENTITY on a rotated display, is "suboptimal" at every present: that's this,
    // not a new size - recreating it each frame would cost the frame)
    else if (r == VK_ERROR_OUT_OF_DATE_KHR || (r == VK_SUBOPTIMAL_KHR && !g_sc_turned)) recreate_swapchain();
    cur_frame.ok = false;
    if (shot_path) {
        vk_wait_seq(seq);
        uint32_t w = g_vk.sc_phys.width, h = g_vk.sc_phys.height;
        uint8_t *px = shot.ptr;
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        for (uint64_t i = 0; bgr && i < (uint64_t)w * h; i++) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) px[i * 4 + 3] = 255;
        if (g_vk.sc_quarter) {  // (the pre-rotated image turned back into the window's shape)
            uint32_t W = h, H = w;  // landscape
            uint8_t *t = malloc((size_t)W * H * 4);
            for (uint32_t y = 0; y < H; y++)
                for (uint32_t x = 0; x < W; x++) {
                    uint32_t qx = g_vk.sc_quarter == 1 ? H - 1 - y : y, qy = g_vk.sc_quarter == 1 ? x : W - 1 - x;
                    memcpy(t + ((size_t)y * W + x) * 4, px + ((size_t)qy * w + qx) * 4, 4);
                }
            vk_save_png(shot_path, t, W, H);
            free(t);
        } else {
            vk_save_png(shot_path, px, w, h);
        }
    }
    return submitted_at;
}

void vk_frame_end(const char *shot_path) { (void)frame_end(shot_path, false, 0); }

void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
    if (!s || !SDL_SavePNG(s, path)) LOG("[vk] saving %s failed: %s\n", path, SDL_GetError());
    else LOG("[vk] saved %s (%ux%u)\n", path, w, h);
    SDL_DestroySurface(s);
}

// ================================================================== game presentation (metal.m backend_present)
// Draws the game's image into the window with the widescreen crop, then the overlay menu.
extern void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]);
void renderer_submit(void);
void renderer_set_scale(float sx, float sy);
extern void widescreen_set_factor(float f);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);

static uint64_t frames, total_frames, g_frame_counter;
uint64_t renderer_present_count(void) { return total_frames; }
uint64_t vk_frame_counter(void) { return g_frame_counter; }
static float g_shot_crop[4] = {0, 0, 1, 1};
static double last_fps_time;
static NvnTexture *g_last_presented;
static int32_t g_last_present_crop[4];
#ifdef _WIN32
static bool last_wide_frame;
#endif

// The render thread collects and reads these intervals. Reboot clears them after guest threads stop.
static struct {
    PresentStats snapshot;
    uint64_t interval_start, last_submitted, max_gap;
    uint64_t guest, represent, submitted;
} present_stats;

PresentStats vk_present_stats(void) {
    PresentStats stats = present_stats.snapshot;
    stats.present_mode = active_present_mode;
#ifdef EOW_VK_DISPLAY_TIMING
    if (display_timing.stage) {
        stats.display_stage = timing_stage_name();
        stats.display_valid = display_timing.valid && SDL_GetTicksNS() - display_timing.last_feedback < 1000000000;
        if (stats.display_valid) stats.display_fps = display_timing.fps;
    }
#endif
    return stats;
}

static void update_present_stats(bool guest, uint64_t now, uint64_t submitted_at) {
    if (submitted_at) {
        if (present_stats.last_submitted) {
            uint64_t gap = submitted_at - present_stats.last_submitted;
            if (gap > present_stats.max_gap) present_stats.max_gap = gap;
        }
        present_stats.last_submitted = submitted_at;
    }
    if (!present_stats.interval_start) {
        present_stats.interval_start = now;
        return;
    }
    present_stats.guest += guest;
    present_stats.represent += !guest;
    present_stats.submitted += submitted_at != 0;
    uint64_t elapsed = now - present_stats.interval_start;
    if (elapsed < 500000000) return;

    double seconds = elapsed / 1e9;
    present_stats.snapshot = (PresentStats){
        .guest_fps = present_stats.guest / seconds,
        .submitted_fps = present_stats.submitted / seconds,
        .present_max_ms = present_stats.max_gap / 1e6,
        .present_mode = active_present_mode,
        .valid = true,
    };
    static int log = -1;
    if (log < 0) {
        const char *flag = getenv("EOWRECOMP_PRESENT_STATS");
        log = flag && !strcmp(flag, "1");
    }
    if (log) {
        PresentStats snapshot = vk_present_stats();
        const PresentStats *s = &snapshot;
        LOG("[present-stats] span %.3fs guest %llu (%.1f/s) represent %llu submitted %llu (%.1f/s) "
            "max %.2fms mode %s display %s %.1f/s\n", seconds, (unsigned long long)present_stats.guest,
            s->guest_fps, (unsigned long long)present_stats.represent, (unsigned long long)present_stats.submitted,
            s->submitted_fps, s->present_max_ms, s->present_mode, s->display_valid ? s->display_stage : "unavailable", s->display_fps);
    }
    present_stats.interval_start = now;
    present_stats.guest = present_stats.represent = present_stats.submitted = present_stats.max_gap = 0;
}

const float *vk_shot_crop(void) { return g_shot_crop; }

static void present_texture(NvnTexture *t, bool guest);
void backend_present(NvnTexture *t, const int32_t crop[4]) {
    if (crop) memcpy(g_last_present_crop, crop, sizeof g_last_present_crop);
    else memset(g_last_present_crop, 0, sizeof g_last_present_crop);
    present_texture(t, true);
}
void backend_represent(void) {
    if (g_last_presented) {
        present_texture(g_last_presented, false);
        return;
    }
    // A software keyboard can block the guest before the next usable image.
    // Its dialog still needs a window frame while the guest texture is unavailable.
    static const float black[4] = {0, 0, 0, 1};
    if (vk_frame_begin(black)) {
        menu_render((void *)vk_cmd(), g_vk.sc_extent.width, g_vk.sc_extent.height);
        vk_frame_end(NULL);
    } else SDL_Delay(16);
}

static void present_texture(NvnTexture *t, bool guest) {
#ifdef _WIN32
    if (guest && !present_trace.finished) present_trace_poll(total_frames + 1);
#endif
    TexBackend *b = vk_tex_for_present(t);
    b->presented = true;
    vk_end_pass();
    bool hold_wide_transition = false;
#ifdef _WIN32
    extern bool vk_wide_frame_ready(const NvnTexture *t);
    if (guest) last_wide_frame = vk_wide_frame_ready(t);
    bool experimental_wide = ENV("EOWRECOMP_VK_WIDESCREEN") != NULL;
    double source_shape = b->scalable && b->sy > 0 ? b->sx / b->sy : 1.0;
    hold_wide_transition = experimental_wide && guest && source_shape > 1.005 && !last_wide_frame;
    if (hold_wide_transition) {
        // A new menu or effect may already use the full wide target. Hold the last
        // displayed image while the next guest frame is drawn into a 16:9 target.
        OutScale next;
        output_scale(g_vk.sc_extent.width, g_vk.sc_extent.height, false, SDL_GetTicksNS() / 1e9, -1, &next);
        widescreen_set_factor(1);
        renderer_set_scale((float)next.sx, (float)next.sy);
        memcpy(g_shot_crop, (const float[4]){0, 0, 1, 1}, sizeof g_shot_crop);
        if (ENV("EOWRECOMP_LOG_WIDESCREEN"))
            LOG("[wide] frame=%llu verified=0 source=%.6f next=1.000000 submitted=0 reason=unverified-wide\n",
                (unsigned long long)total_frames + 1, source_shape);
    }
#endif
    g_last_presented = hold_wide_transition ? NULL : t;
    static const float black[4] = {0, 0, 0, 1};
    uint64_t t0 = SDL_GetTicksNS();
    bool have = !hold_wide_transition && vk_frame_begin(black);
    uint64_t nd = SDL_GetTicksNS() - t0;
    if (nd > 30000000) {
        static int warned;
        if (warned++ < 20) LOG("[vk] acquire blocked %llums\n", (unsigned long long)(nd / 1000000));
    }
    uint64_t submitted_at = 0;
    if (have) {
        double dw = g_vk.sc_extent.width, dh = g_vk.sc_extent.height;
        extern void widescreen_frame_end(uint64_t frame);
        widescreen_frame_end(total_frames);
        extern bool widescreen_allowed(void);
        // render scale + placement in the window: runtime/nvn/output_scale.c (Video tab "Internal resolution");
        // no GPU timer here (-1): dynamic resolution goes by the frame time
        OutScale os;
#ifdef _WIN32
        double window_shape = (dw / dh) / (16.0 / 9.0);
        bool requested = experimental_wide && g_settings.aspect == ASPECT_UNLOCKED &&
                         !settings_guest_handheld() && window_shape > 1.005;
        double next_shape = requested && last_wide_frame ? fmin(window_shape, 2.0) : 1.0;
        if (next_shape > 1)
            output_scale(dh * (16.0 / 9.0) * next_shape, dh, true, SDL_GetTicksNS() / 1e9, -1, &os);
        else
#endif
            output_scale(dw, dh, widescreen_allowed(), SDL_GetTicksNS() / 1e9, -1, &os);
        double sx = os.sx, sy = os.sy, vx = os.vx, vy = os.vy, vw = os.vw, vh = os.vh, shape = os.shape;
        // interiors / dungeons: their 16:9 centre, or the part inside a room wider than one screen
        extern void widescreen_present_crop(uint64_t, double, float *, double *, double *, double *, double *);
        float crop[4];  // uv origin and size
        widescreen_present_crop(total_frames, shape, crop, &vx, &vy, &vw, &vh);
#ifdef _WIN32
        if (experimental_wide) {
            // Limit this experimental camera to the verified range. Render geometry follows the next target's
            // scale, while this present uses the aspect of the texture that was actually drawn.
            double content_shape = requested && last_wide_frame ? source_shape : 1.0;
            if (requested || source_shape > 1.005) {
                double aspect = (16.0 / 9.0) * content_shape;
                vh = fmin(dh, dw / aspect); vw = vh * aspect;
                vx = (dw - vw) * 0.5; vy = (dh - vh) * 0.5;
            }
            if (source_shape > content_shape) {
                crop[2] = (float)(content_shape / source_shape);
                crop[0] = (1 - crop[2]) * 0.5f;
            }
            if (guest && ENV("EOWRECOMP_LOG_WIDESCREEN")) {
                static double previous_source, previous_next;
                static bool previous_ready;
                if (previous_source != source_shape || previous_next != next_shape || previous_ready != last_wide_frame) {
                    LOG("[wide] frame=%llu verified=%u source=%.6f next=%.6f crop=%.6f viewport=%.0fx%.0f\n",
                        (unsigned long long)total_frames + 1, (unsigned)last_wide_frame, source_shape, next_shape, crop[2], vw, vh);
                    previous_source = source_shape; previous_next = next_shape; previous_ready = last_wide_frame;
                }
            }
        }
#endif
        if (g_last_present_crop[0] >= 0 && g_last_present_crop[1] >= 0 &&
            g_last_present_crop[2] > 0 && g_last_present_crop[3] > 0 &&
            (uint64_t)g_last_present_crop[0] + g_last_present_crop[2] <= t->width &&
            (uint64_t)g_last_present_crop[1] + g_last_present_crop[3] <= t->height) {
            float width = (float)t->width, height = (float)t->height;
            float u = g_last_present_crop[2] / width, v = g_last_present_crop[3] / height;
            crop[0] = g_last_present_crop[0] / width + crop[0] * u;
            crop[1] = g_last_present_crop[1] / height + crop[1] * v;
            crop[2] *= u;
            crop[3] *= v;
        }
        widescreen_set_factor((float)shape);
        renderer_set_scale((float)sx, (float)sy);
        VkCommandBuffer cb = vk_cmd();
        float pv[8] = {renderer_origin_lower_left() ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
        memcpy(g_shot_crop, crop, sizeof crop);
        vk_present_draw(cb, b, (float)vx, (float)vy, (float)vw, (float)vh, pv);
        static long menu_at = -1;
        if (menu_at < 0) menu_at = getenv("EOWRECOMP_MENU_AT") ? atol(getenv("EOWRECOMP_MENU_AT")) : 0;
        if (menu_at && (long)total_frames + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
        menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
        static long win_shot = -1;
        if (win_shot < 0) win_shot = getenv("EOWRECOMP_WINDOW_SHOT") ? atol(getenv("EOWRECOMP_WINDOW_SHOT")) : 0;
        bool shot = win_shot && (long)total_frames + 1 == win_shot;
        submitted_at = frame_end(shot ? getenv("EOWRECOMP_SHOT_PATH") ? getenv("EOWRECOMP_SHOT_PATH") : "build/window.png" : NULL,
                                 true, guest ? total_frames + 1 : 0);
        if (shot && getenv("EOWRECOMP_EXIT_AFTER_SHOT")) { fflush(stderr); _exit(0); }
    } else {
        renderer_submit();
    }
    extern void vk_prof_frame(uint64_t frame, uint64_t present_ns);
    uint64_t present_end = SDL_GetTicksNS();
    vk_prof_frame(total_frames, present_end - t0);
    if (guest) {
#ifdef _WIN32
        if (vk_temporal_scene_enabled)
            vk_temporal_scene_present(t, total_frames + 1, submitted_at != 0);
#endif
        extern void vk_present_shader_stats(uint64_t frame, bool submitted);
        vk_present_shader_stats(total_frames + 1, submitted_at != 0);
    }
    update_present_stats(guest, present_end, submitted_at);
    frames++;
    total_frames++;
    g_frame_counter++;
    extern void vk_pace_frame(void);
    vk_pace_frame();  // (exec.c: the game's 30 fps mode when frames don't fit 60)
    extern void input_frame(uint64_t frame);
    input_frame(total_frames);
    static long slow_ms = -1;
    if (slow_ms < 0) slow_ms = getenv("EOWRECOMP_SLOW_FRAME_MS") ? atol(getenv("EOWRECOMP_SLOW_FRAME_MS")) : 0;
    if (slow_ms > 0) {
        static long slow_from = -1, slow_until = -1;  // frame bounds for pacing recovery tests
        if (slow_from < 0) slow_from = getenv("EOWRECOMP_SLOW_FRAME_FROM") ? atol(getenv("EOWRECOMP_SLOW_FRAME_FROM")) : 2600;
        if (slow_until < 0) slow_until = getenv("EOWRECOMP_SLOW_FRAME_UNTIL") ? atol(getenv("EOWRECOMP_SLOW_FRAME_UNTIL")) : 0;
        if ((long)total_frames > slow_from && (!slow_until || (long)total_frames < slow_until)) SDL_Delay((Uint32)slow_ms);
    }
    extern void fnprof_frame(uint64_t frame);
    fnprof_frame(total_frames);
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (EOWRECOMP_REBOOT_AT)
    reboot_test_frame(total_frames);
    extern void vk_level_change_check(void);
    vk_level_change_check();
    static long shot_every = -1;
    if (shot_every < 0) { const char *e = getenv("EOWRECOMP_SCREENSHOT_EVERY"); shot_every = e ? atol(e) : 0; }
    extern bool g_debug_frame;
    static long debug_at = -1;
    if (debug_at < 0) { const char *e = getenv("EOWRECOMP_DEBUG_FRAME"); debug_at = e ? atol(e) : 0; }
    extern volatile int g_debug_frame_request;
    if (g_debug_frame_request) { g_debug_frame_request = 0; debug_at = (long)total_frames + 1; }
    // EOWRECOMP_DEBUG_FRAMES=<n> (with EOWRECOMP_DEBUG_FRAME / F9): dump n consecutive frames (build/pass_<frame>_NNN.png)
    static long debug_n = -1;
    if (debug_n < 0) { const char *e = getenv("EOWRECOMP_DEBUG_FRAMES"); debug_n = e && atol(e) > 1 ? atol(e) : 1; }
    g_debug_frame = debug_at && total_frames >= (uint64_t)debug_at && total_frames < (uint64_t)debug_at + (uint64_t)debug_n;
    {   // EOWRECOMP_GPUPROF_AT=<frame>[,<frame>...]: GPU time per pass of those frames ("[gpuprof]" in the log)
        extern bool g_gpuprof_frame;
        extern void vk_gpuprof_report(void);
        if (g_gpuprof_frame) { g_gpuprof_frame = false; vk_gpuprof_report(); }
        const char *e = getenv("EOWRECOMP_GPUPROF_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)total_frames) { g_gpuprof_frame = true; break; }
            if (*p == ',') p++; else break;
        }
    }
    if (g_debug_frame) LOG("===== debug frame %llu =====\n", (unsigned long long)total_frames);
    {   // EOWRECOMP_SHOT_AT=<frame>,<frame>,... (ascending): the game image at those frames -> build/frames/frame_N.png
        static const char *next;
        static int init;
        if (!init) { init = 1; next = getenv("EOWRECOMP_SHOT_AT"); if (next) mkdir("build/frames", 0755); }
        while (next && *next) {
            long at = strtol(next, (char **)&next, 10);
            if ((long)total_frames < at) { while (next > getenv("EOWRECOMP_SHOT_AT") && next[-1] != ',') next--; break; }
            if ((long)total_frames == at) {
                char path[256];
                snprintf(path, sizeof path, "build/frames/frame_%05llu.png", (unsigned long long)total_frames);
                renderer_save_texture_tex(b, path);
            }
            if (*next == ',') next++;
        }
    }
    static long shot_from = -1;
    if (shot_from < 0) { const char *e = getenv("EOWRECOMP_SCREENSHOT_FROM"); shot_from = e ? atol(e) : 0; }
    if (shot_every && total_frames % shot_every == 0 && (long)total_frames >= shot_from) {
        char path[256];
        if (shot_from) mkdir("build/frames", 0755);
        snprintf(path, sizeof path, shot_from ? "build/frames/frame_%05llu.png" : "build/frame_%05llu.png",
                 (unsigned long long)total_frames);
        renderer_save_texture_tex(b, path);
        // a series (SCREENSHOT_FROM) runs on until EOWRECOMP_WINDOW_SHOT exits
        if (getenv("EOWRECOMP_EXIT_AFTER_SHOT") && !shot_from) { fflush(stderr); _exit(0); }
    }
    double now = SDL_GetTicks() / 1000.0;
    static double fps_iv = -1, prev_present, max_gap;
    if (fps_iv < 0) fps_iv = getenv("EOWRECOMP_FPS_INTERVAL") ? atof(getenv("EOWRECOMP_FPS_INTERVAL")) : 2.0;
    if (prev_present > 0 && now - prev_present > max_gap) max_gap = now - prev_present;
    prev_present = now;
    if (now - last_fps_time >= fps_iv) {
        extern const char *game_level_name(void);
        if (last_fps_time > 0)
            LOG("[vk] t=%.1f frame %llu %.1f fps max %.0fms level %s (%s)\n", now, (unsigned long long)total_frames,
                frames / (now - last_fps_time), max_gap * 1000.0, game_level_name(), renderer_stats());
#ifdef _WIN32
        if (memory_stats) {
            extern void vk_texture_memory_stats(uint64_t frame);
            report_gpu_memory(total_frames);
            vk_texture_memory_stats(total_frames);
        }
#endif
        max_gap = 0;
        frames = 0;
        last_fps_time = now;
    }
}

// ================================================================== UI thread
void vk_exec_init(void);
void vk_shaders_init(void);
void vk_pipeline_replay_save(void);

// exit() from any thread: let the GPU finish, keep the pipeline cache, then end the process without the DLL
// teardown (driver / validation-layer threads can stall it for minutes while the game's threads still run).
static void quick_exit_handler(void) {
    static int trace = -1;
    if (trace < 0) trace = getenv("EOWRECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] exit: waiting for the GPU\n");
    if (g_vk.dev) {
        vkDeviceWaitIdle(g_vk.dev);
        if (trace) LOG("[vk] exit: saving the pipeline cache\n");
        vk_pipeline_replay_save();
        vk_pipeline_cache_save();
    }
    if (trace) LOG("[vk] exit: terminating\n");
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
}

// macOS: the .app's own MoltenVK (Contents/Resources/vulkan/icd.d, tools/bundle_macos.sh), not another one installed
// on the machine (Homebrew's: a different version, and the device listed twice); a source build has none. Before
// SDL_Init: SDL's Vulkan loading scans the drivers too.
static void use_bundled_driver(void) {
#ifdef __APPLE__
    char icd[1100];
    if (!getenv("VK_DRIVER_FILES") && !getenv("VK_ICD_FILENAMES") &&
        rt_resource("vulkan/icd.d/MoltenVK_icd.json", NULL, icd, sizeof icd))
        setenv("VK_DRIVER_FILES", icd, 1);
#endif
}

#ifdef _WIN32
typedef struct {
    SDL_DisplayID id;
    WindowDisplay ui;
} DisplayEntry;

static SDL_SpinLock display_lock;
static DisplayEntry *display_entries;
static int display_count;
static SDL_DisplayID primary_display;
static Uint64 last_display_check;

static void display_key(SDL_DisplayID id, char key[WINDOW_DISPLAY_KEY_MAX]) {
    SDL_PropertiesID props = SDL_GetDisplayProperties(id);
    HMONITOR handle = SDL_GetPointerProperty(props, SDL_PROP_DISPLAY_WINDOWS_HMONITOR_POINTER, NULL);
    MONITORINFOEXW info = {.cbSize = sizeof info};
    if (!handle || !GetMonitorInfoW(handle, (MONITORINFO *)&info)) return;

    // A cloned desktop has no unique monitor preference for this SDL display.
    DISPLAY_DEVICEW monitor = {.cb = sizeof monitor};
    for (DWORD i = 0; EnumDisplayDevicesW(info.szDevice, i, &monitor, EDD_GET_DEVICE_INTERFACE_NAME); i++) {
        if (!(monitor.StateFlags & DISPLAY_DEVICE_ACTIVE) || !monitor.DeviceID[0]) continue;
        if (key[0]) { key[0] = '\0'; return; }
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, monitor.DeviceID, -1,
                                 key, WINDOW_DISPLAY_KEY_MAX, NULL, NULL)) {
            key[0] = '\0';
            return;
        }
    }
}

static bool refresh_displays(SDL_Window *w, bool force) {
    Uint64 now = SDL_GetTicks();
    if (!force && now - last_display_check < 500) return false;
    last_display_check = now;
    int count;
    SDL_DisplayID *ids = SDL_GetDisplays(&count);
    if (!ids) return false;
    DisplayEntry *next = SDL_calloc((size_t)count, sizeof *next);
    if (!next && count) { SDL_free(ids); return false; }

    SDL_DisplayID current = w ? SDL_GetDisplayForWindow(w) : 0;
    SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    for (int i = 0; i < count; i++) {
        const char *name = SDL_GetDisplayName(ids[i]);
        next[i].id = ids[i];
        next[i].ui.current = ids[i] == current;
        snprintf(next[i].ui.name, sizeof next[i].ui.name, "%s (%d)", name ? name : "Display", i + 1);
        display_key(ids[i], next[i].ui.key);
    }
    SDL_free(ids);

    bool changed = count != display_count || primary != primary_display;
    for (int i = 0; !changed && i < count; i++) {
        changed = next[i].id != display_entries[i].id ||
                  SDL_strcasecmp(next[i].ui.key, display_entries[i].ui.key) != 0;
    }
    SDL_LockSpinlock(&display_lock);
    DisplayEntry *previous = display_entries;
    display_entries = next;
    display_count = count;
    primary_display = primary;
    SDL_UnlockSpinlock(&display_lock);
    SDL_free(previous);
    return changed;
}

int vk_window_displays(WindowDisplay *out, int capacity) {
    int copied = 0;
    SDL_LockSpinlock(&display_lock);
    for (int i = 0; i < display_count && copied < capacity; i++) {
        if (display_entries[i].ui.key[0]) out[copied++] = display_entries[i].ui;
    }
    SDL_UnlockSpinlock(&display_lock);
    return copied;
}

bool vk_refresh_window_displays(void *sdl_window) { return refresh_displays(sdl_window, false); }

static SDL_DisplayID choose_display(SDL_DisplayID current, const char *key) {
    bool current_available = false;
    for (int i = 0; i < display_count; i++) {
        const DisplayEntry *display = &display_entries[i];
        if (key && *key && !SDL_strcasecmp(display->ui.key, key)) return display->id;
        if (display->id == current) current_available = true;
    }
    return current_available ? current : primary_display;
}

static bool set_fullscreen(SDL_Window *w, bool fullscreen) {
    return SDL_SetWindowFullscreen(w, fullscreen) && SDL_SyncWindow(w);
}

void vk_apply_window_mode(void *sdl_window, bool fullscreen, const char *monitor_key) {
    SDL_Window *w = sdl_window;
    if (getenv("EOWRECOMP_WINDOW") || getenv("EOWRECOMP_HIDDEN")) return;
    SDL_DisplayID current = SDL_GetDisplayForWindow(w);
    SDL_DisplayID target = choose_display(current, monitor_key);
    SDL_WindowFlags flags = SDL_GetWindowFlags(w);
    bool is_fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;
    bool move = target && target != current;
    if (!move && fullscreen == is_fullscreen) return;

    if (is_fullscreen && !fullscreen) {
        if (!set_fullscreen(w, false)) goto failed;
        is_fullscreen = false;
        move = target && target != SDL_GetDisplayForWindow(w);
    }
    if (move) {
        // SDL does not move a maximized window. Restore its floating size first.
        bool maximized = !is_fullscreen && (SDL_GetWindowFlags(w) & SDL_WINDOW_MAXIMIZED);
        if (maximized && (!SDL_RestoreWindow(w) || !SDL_SyncWindow(w))) goto failed;
        int pos = SDL_WINDOWPOS_CENTERED_DISPLAY(target);
        bool moved = SDL_SetWindowPosition(w, pos, pos) && SDL_SyncWindow(w);
        bool restored = !maximized || (SDL_MaximizeWindow(w) && SDL_SyncWindow(w));
        if (!moved || !restored) goto failed;
    }
    if (fullscreen && !is_fullscreen && !set_fullscreen(w, true)) goto failed;
    refresh_displays(w, true);
    if (target && SDL_GetDisplayForWindow(w) != target)
        LOG("[window] requested monitor is unavailable for this window\n");
    return;

failed:
    LOG("[window] monitor or fullscreen change failed: %s\n", SDL_GetError());
    refresh_displays(w, true);
}

static SDL_Window *create_windows_window(int width, int height, SDL_WindowFlags flags) {
    refresh_displays(NULL, true);
    char key[SETTINGS_MONITOR_KEY_SIZE];
    settings_get_monitor_key(key);
    SDL_DisplayID target = choose_display(0, key);
    int pos = SDL_WINDOWPOS_CENTERED_DISPLAY(target);
    if (g_settings.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props) return NULL;
    SDL_Window *w = NULL;
    if (SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "EoWRecomp") &&
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width) &&
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height) &&
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, pos) &&
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, pos) &&
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, (Sint64)flags)) {
        w = SDL_CreateWindowWithProperties(props);
    }
    SDL_DestroyProperties(props);
    return w;
}
#endif

void ui_window_init(void) {
    if (window) return;
    atexit(quick_exit_handler);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");  // (Back opens the menu, not "close the app")
#endif
    settings_load();
    use_bundled_driver();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) FATAL("SDL_Init: %s\n", SDL_GetError());
    int win_w = 1280, win_h = 720;
    if (getenv("EOWRECOMP_WINDOW")) sscanf(getenv("EOWRECOMP_WINDOW"), "%dx%d", &win_w, &win_h);
    SDL_WindowFlags wflags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (getenv("EOWRECOMP_HIDDEN")) wflags |= SDL_WINDOW_HIDDEN;
#ifdef __ANDROID__
    wflags |= SDL_WINDOW_FULLSCREEN;  // (immersive: no status / navigation bars over the game)
#endif
    {   // scripted test runs (user input ignored: runtime/hle/input.c input_user_blocked) open without taking the
        // keyboard focus from whatever the player is doing
        const char *v = getenv("EOWRECOMP_NO_USER_INPUT");
        bool scripted = v ? atoi(v) != 0 : (getenv("EOWRECOMP_INPUT") || getenv("EOWRECOMP_DOORJUMP_TEST") || getenv("EOWRECOMP_PLACE") ||
                                            getenv("EOWRECOMP_HOST_INPUT") || getenv("EOWRECOMP_BIND_TEST"));
        if (scripted) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    }
    bool test_window = getenv("EOWRECOMP_WINDOW") || getenv("EOWRECOMP_HIDDEN");
#ifdef _WIN32
    window = test_window ? SDL_CreateWindow("EoWRecomp", win_w, win_h, wflags) :
                           create_windows_window(win_w, win_h, wflags);
#else
    window = SDL_CreateWindow("EoWRecomp", win_w, win_h, wflags);
#endif
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
#ifdef _WIN32
    presentation_window_id = SDL_GetWindowID(window);
    atomic_store(&presentation_window_minimized, (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0);
#endif
    SDL_AddEventWatch(lifecycle_watch, NULL);
    rt_window_icon(window);
    // the Fullscreen setting applies from the start screen on (not only once the game runs); test windows stay windows
#ifdef _WIN32
    refresh_displays(window, true);
#else
    if (g_settings.fullscreen && !test_window) SDL_SetWindowFullscreen(window, true);
#endif
    create_instance();
    if (!SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    pick_device();
    create_device();
    create_swapchain();
    vk_exec_init();
    vk_shaders_init();
}

SDL_Window *ui_window(void) { return window; }

void backend_init(void) {
    while (!atomic_load(&ui_ready)) SDL_Delay(1);
    LOG("[vk] %s, swapchain %ux%u\n", g_vk.props.deviceName, g_vk.sc_extent.width, g_vk.sc_extent.height);
}

// ---- in-place reboot (runtime/main.c teardown; main thread, guest threads stopped)
void backend_reset(void) {
    if (!g_vk.dev) return;
    vkDeviceWaitIdle(g_vk.dev);
    extern void vk_textures_reset(void);
    extern void vk_exec_reset(void);
    vk_textures_reset();
    vk_exec_reset();
    g_last_presented = NULL;
#ifdef _WIN32
    last_wide_frame = false;
    if (present_trace.buf) present_trace_clear();
#endif
    memset(&present_stats, 0, sizeof present_stats);
#ifdef EOW_VK_DISPLAY_TIMING
    display_timing.last_id = display_timing.next_id;
    display_timing.last_time = display_timing.sample_start = display_timing.sample_count = 0;
    display_timing.valid = false;
#endif
}
void backend_guest_memory_reset(void) {}  // (exec.c re-imports the heap on first use)
void backend_clear_window(void) {
    static const float black[4] = {0, 0, 0, 1};
    if (g_vk.dev && vk_frame_begin(black)) vk_frame_end(NULL);
}

int ui_run(void) {
    ui_window_init();
    input_refresh_gamepads();
    atomic_store(&ui_ready, true);
    for (;;) {
#ifdef _WIN32
        discord_presence_update(settings_discord_presence(), true);
#endif
        int reboot = rt_reboot_requested();
        if (reboot) {
            input_stop_rumble();
            return reboot;
        }
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 16)) {
            if (e.type == SDL_EVENT_QUIT) {
                input_stop_rumble();
                LOG("window closed\n");
                exit(0);
            }
            input_pump_rumble();
            if (input_user_blocked(&e)) continue;  // scripted test runs: the real keyboard / mouse / pads do nothing (as metal.m)
            if (!input_track_gamepad(&e)) continue;
            if (menu_handle_event(&e)) continue;
            if (touch_handle_event(&e)) continue;  // (phones: the on-screen controls)
            input_handle_event(&e);
        }
        input_pump_rumble();
        menu_apply_window_requests(window);
    }
}
