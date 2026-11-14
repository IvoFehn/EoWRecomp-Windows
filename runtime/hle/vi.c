// nn::vi (display/layer) and nv:: (graphics driver init), implemented on the host.
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../hle.h"
#include "../kernel.h"

static uint32_t vsync_r, vsync_w;
static KEvent *vsync_event;
uint32_t vi_vsync_handle(void) { return vsync_r; }
volatile uint64_t g_vsync_count;  // vsyncs signalled so far (runtime/hle/framepace.c)
uint64_t g_native_window;
static pthread_mutex_t layer_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t native_window_layer, crop_layer;
static int32_t layer_crop[4];

bool vi_get_layer_crop(uint64_t native_window, int32_t crop[4]) {
    pthread_mutex_lock(&layer_lock);
    bool available = native_window && native_window == g_native_window &&
        native_window_layer && native_window_layer == crop_layer && layer_crop[2] > 0 && layer_crop[3] > 0;
    if (available) memcpy(crop, layer_crop, sizeof layer_crop);
    pthread_mutex_unlock(&layer_lock);
    return available;
}

static void *vsync_thread(void *arg) {
    (void)arg;
    const uint64_t period = 16666667;
    plat_thread_realtime(period);
    uint64_t next = plat_now_ns();
    for (;;) {
        next += period;
        plat_sleep_until_ns(next);
        uint64_t now = plat_now_ns();
        if (now > next + period * 3) {
            static int warned;
            if (warned++ < 20) LOG("[vi] vsync timer late by %llums (host throttling?)\n", (unsigned long long)((now - next) / 1000000));
            next = now;  // don't burst-signal to catch up
        }
        __atomic_add_fetch(&g_vsync_count, 1, __ATOMIC_RELAXED);
        pthread_mutex_lock(&g_klock);
        kernel_signal(vsync_event);
        pthread_mutex_unlock(&g_klock);
    }
    return NULL;
}

HLE_FUNC("_ZN2nn2vi10InitializeEv", vi_initialize) { (void)c; }
HLE_FUNC("_ZN2nn2vi8FinalizeEv", vi_finalize) { (void)c; }

HLE_FUNC("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open_default_display) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", vi_close_display) { (void)c; }

HLE_FUNC("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_create_layer) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", vi_destroy_layer) { (void)c; }
HLE_FUNC("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", vi_set_layer_crop) {
    uint64_t layer = c->x[0];
    int32_t crop[4] = {(int32_t)c->x[1], (int32_t)c->x[2], (int32_t)c->x[3], (int32_t)c->x[4]};
    pthread_mutex_lock(&layer_lock);
    if (crop_layer != layer || memcmp(layer_crop, crop, sizeof layer_crop)) {
        crop_layer = layer;
        memcpy(layer_crop, crop, sizeof layer_crop);
        if (getenv("EOWRECOMP_VI_CROP_LOG"))
            LOG("[vi] SetLayerCrop layer=%llx rect=%d,%d,%d,%d\n", (unsigned long long)layer, crop[0], crop[1], crop[2], crop[3]);
    }
    pthread_mutex_unlock(&layer_lock);
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window) {
    pthread_mutex_lock(&layer_lock);
    if (!g_native_window) g_native_window = (uint64_t)hle_alloc(0x100);
    native_window_layer = c->x[1];
    *(uint64_t *)(uintptr_t)c->x[0] = g_native_window;
    pthread_mutex_unlock(&layer_lock);
    c->x[0] = 0;
}

// Reboot: the vsync timer and its event live on (the kernel's handle table doesn't): new handles at the next boot.
RT_ON_RESET(vi_reset) {
    vsync_r = vsync_w = 0;
    g_native_window = 0;  // (HLE heap)
    native_window_layer = crop_layer = 0;
    memset(layer_crop, 0, sizeof layer_crop);
}

HLE_FUNC("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_get_vsync_event) {
    if (!vsync_event) {
        vsync_event = kernel_create_event_pair(&vsync_w, &vsync_r);
        pthread_t t;
        pthread_create(&t, NULL, vsync_thread, NULL);
    } else if (!vsync_r) {
        vsync_w = kernel_new_handle(vsync_event);
        vsync_r = kernel_new_handle(vsync_event);
    }
    // nn::os::AttachReadableHandleToSystemEvent(event, handle, managed=false, clear_mode=AutoClear)
    c->x[1] = vsync_r;
    c->x[2] = 0;
    c->x[3] = 1;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nv18InitializeGraphicsEPvm", nv_initialize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv16FinalizeGraphicsEv", nv_finalize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_allocator) { (void)c; }
HLE_FUNC("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_dt_allocator) { (void)c; }
