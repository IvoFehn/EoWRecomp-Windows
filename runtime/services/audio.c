// audren:u (audio renderer) and audout:u.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdatomic.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "../hle.h"
#include "../settings.h"
#include "../ipc.h"
#include "audio_output.h"

#ifdef _WIN32
typedef struct {
    SDL_AudioDeviceID requested, opened;
    unsigned generation;
} AudioRoute;

static _Atomic SDL_AudioDeviceID output_preference = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
static atomic_bool output_surround;
static atomic_uint output_generation;
static bool output_enabled;

SDL_AudioDeviceID audio_output_preference(void) { return atomic_load(&output_preference); }
bool audio_output_surround(void) { return atomic_load_explicit(&output_surround, memory_order_relaxed); }
void audio_output_set_surround(bool enabled) { atomic_store_explicit(&output_surround, enabled, memory_order_relaxed); }

void audio_output_select(SDL_AudioDeviceID device) {
    if (!device || !SDL_IsAudioDevicePhysical(device) || !SDL_IsAudioDevicePlayback(device))
        device = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    atomic_store(&output_preference, device);
}

static void forget_output(SDL_AudioDeviceID device) {
    atomic_compare_exchange_strong(&output_preference, &device, SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
}

static bool SDLCALL audio_output_event(void *unused, SDL_Event *e) {
    (void)unused;
    if (e->type != SDL_EVENT_AUDIO_DEVICE_ADDED && e->type != SDL_EVENT_AUDIO_DEVICE_REMOVED &&
        e->type != SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED) return true;
    if (!e->adevice.recording) {
        if (e->type == SDL_EVENT_AUDIO_DEVICE_REMOVED) forget_output(e->adevice.which);
        atomic_fetch_add(&output_generation, 1);
    }
    return true;
}

void audio_output_init(void) {
    const char *disabled = getenv("EOWRECOMP_NO_AUDIO");
    if (disabled && strcmp(disabled, "0")) return;
    output_enabled = SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!output_enabled) LOG("[audio] SDL audio init failed: %s\n", SDL_GetError());
    else if (!SDL_AddEventWatch(audio_output_event, NULL)) LOG("[audio] SDL audio event watch failed: %s\n", SDL_GetError());
}

static bool output_pending(const AudioRoute *route) {
    return output_enabled && (route->requested != audio_output_preference() ||
                              route->generation != atomic_load(&output_generation));
}

static SDL_AudioStream *open_output(SDL_AudioDeviceID device, const SDL_AudioSpec *spec, const char *tag) {
    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(device, spec, NULL, NULL);
    if (stream && SDL_ResumeAudioStreamDevice(stream)) {
        const char *name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream));
        LOG("[%s] audio output %u: %s\n", tag, device, name ? name : "unknown device");
        return stream;
    }
    LOG("[%s] audio output %u unavailable: %s\n", tag, device, SDL_GetError());
    if (stream) SDL_DestroyAudioStream(stream);
    return NULL;
}

// Called by the stream owner, outside the guest and mixer locks. SDL's simplified streams cannot be rebound.
static bool refresh_output(AudioRoute *route, SDL_AudioStream **stream, const SDL_AudioSpec *spec, const char *tag) {
    if (!output_pending(route)) return false;
    const SDL_AudioDeviceID default_device = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    route->generation = atomic_load(&output_generation);
    SDL_AudioDeviceID wanted = audio_output_preference();
    route->requested = wanted;
    bool old_present = route->opened == default_device;
    if (wanted != default_device || (*stream && !old_present)) {
        int count = 0;
        SDL_AudioDeviceID *devices = SDL_GetAudioPlaybackDevices(&count);
        if (devices) {
            bool wanted_present = wanted == default_device;
            for (int i = 0; i < count; i++) {
                old_present |= devices[i] == route->opened;
                wanted_present |= devices[i] == wanted;
            }
            SDL_free(devices);
            if (!wanted_present) {
                forget_output(wanted);
                wanted = route->requested = default_device;
            }
        } else {
            old_present = true;  // enumeration failure provides no evidence the current device is gone
        }
    }

    if (*stream && old_present && wanted == route->opened) return true;
    SDL_AudioStream *replacement = open_output(wanted, spec, tag);
    if (!replacement && *stream && old_present) return true;
    if (!replacement && wanted != default_device) {
        wanted = default_device;
        replacement = open_output(wanted, spec, tag);
    }
    if (*stream) SDL_DestroyAudioStream(*stream);
    *stream = replacement;
    route->opened = replacement ? wanted : 0;
    return true;
}
#endif

// ------------------------------------------------------------------ audren: a CPU mixer modelled on
// Ryujinx's AudioRenderer (VoiceState / DataSourceHelper / StateUpdater, copies in notes/ryujinx_audio).
// Voices decode PCM16 / ADPCM wave buffers, resample to 48 kHz, and mix into their destination mix's
// buffers; sub mixes route into their destination; the final mix feeds the device sink and SDL.
// Delay and reverb run in mix order.
#define WB_COUNT 4
#define MIXBUF_MAX 24
#define HIST 4

typedef struct {
    uint64_t addr, size, ctx;
    uint32_t start, end, loop_start, loop_end;
    int32_t loop_count;
    uint8_t loop, eos, sent;  // sent = IsSendToAudioProcessor
} WaveBuf;

typedef struct {  // per channel resource, "VoiceUpdateState"
    uint8_t valid[WB_COUNT];
    uint32_t wb_index, consumed;
    uint64_t played;
    int32_t offset, loop_count;
    float fraction;
    int16_t hist[HIST];
    int16_t pred_scale, h0, h1;
    float bq[2][2];  // biquad filter state per voice filter
} ChanState;

typedef struct {
    float mix[MIXBUF_MAX], prev[MIXBUF_MAX];
    uint8_t used;
} ChanRes;

typedef struct {
    uint8_t in_use, is_new, play_state;  // play_state: 0 started, 1 stopped, 2 stopping, 3 paused
    uint8_t fmt, chans, drop;
    uint32_t rate, wb_count, flush;
    float pitch, volume, prev_volume;
    int32_t mix_id;
    uint32_t splitter_id;
    uint32_t chan_ids[6];
    uint64_t ds_addr;
    uint16_t behaviour;
    uint8_t bq_on[2], bq_reset[2];
    float bq[2][5];  // a0 a1 a2 b1 b2
    WaveBuf wb[WB_COUNT];
} Voice;

typedef struct {
    uint8_t used;
    float volume;
    uint32_t bufs;
    int32_t dest;
    uint32_t dest_splitter;
    float vol[MIXBUF_MAX * MIXBUF_MAX];
    float *buf;  // bufs * sample_count
} Mix;

typedef struct Effect Effect;

typedef struct {
    bool enabled, configured;
    uint32_t slot, device_sinks, raw_inputs, final_buffers;
    uint8_t used, map[6], valid_mask;
    uint64_t first_frame, samples, nonzero[6], nonfinite[6];
    float peak[6];
} AudioSinkProbe;

typedef struct {
    uint32_t sample_rate, sample_count, mix_buffers, submixes, voices, sinks, effects, perf_frames;
    uint32_t splitters, revision;
    uint64_t elapsed_frames;
    uint32_t event_r, event_w;
    KEvent *event;
    uint32_t state;  // 0 started, 1 stopped
    pthread_mutex_t lock;
    Voice *voice;
    ChanRes *chres;
    ChanState *chst;
    Mix *mix;
    uint32_t nmix;
    uint8_t sink_used, sink_inputs, sink_input[6];
    AudioSinkProbe sink_probe;
    Effect *fx;
    SDL_AudioStream *stream;
#ifdef _WIN32
    AudioRoute output;
#endif
    FILE *wav;
    uint32_t wav_bytes;
    pthread_t thread;
    atomic_bool quit;  // reboot (audio_reset)
} Renderer;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void sink_probe_report(AudioSinkProbe *p) {
    if (!p->samples) return;
    LOG("[audren-sink] pre_downmix slot=%u first_frame=%llu samples=%llu valid_mask=0x%02x "
        "nonzero=%llu,%llu,%llu,%llu,%llu,%llu peak=%.7g,%.7g,%.7g,%.7g,%.7g,%.7g "
        "nonfinite=%llu,%llu,%llu,%llu,%llu,%llu\n", p->slot,
        (unsigned long long)p->first_frame, (unsigned long long)p->samples, p->valid_mask,
        (unsigned long long)p->nonzero[0], (unsigned long long)p->nonzero[1], (unsigned long long)p->nonzero[2],
        (unsigned long long)p->nonzero[3], (unsigned long long)p->nonzero[4], (unsigned long long)p->nonzero[5],
        p->peak[0], p->peak[1], p->peak[2], p->peak[3], p->peak[4], p->peak[5],
        (unsigned long long)p->nonfinite[0], (unsigned long long)p->nonfinite[1], (unsigned long long)p->nonfinite[2],
        (unsigned long long)p->nonfinite[3], (unsigned long long)p->nonfinite[4], (unsigned long long)p->nonfinite[5]);
    p->samples = 0;
    memset(p->nonzero, 0, sizeof p->nonzero);
    memset(p->nonfinite, 0, sizeof p->nonfinite);
    memset(p->peak, 0, sizeof p->peak);
}

// Diagnostics follow the same last-device-sink selection as the stereo mixer.
static void sink_probe_configure(Renderer *r, uint32_t slot, uint32_t device_sinks, uint32_t raw_inputs) {
    AudioSinkProbe *p = &r->sink_probe;
    const Mix *final = r->nmix && r->mix[0].used ? &r->mix[0] : NULL;
    uint32_t final_buffers = final ? final->bufs : 0;
    uint8_t used = device_sinks ? r->sink_used : 0;
    uint8_t map[6] = {0}, valid_mask = 0;
    if (device_sinks) memcpy(map, r->sink_input, sizeof map);
    if (used) {
        for (int c = 0; c < r->sink_inputs; c++)
            if (map[c] < final_buffers) valid_mask |= 1u << c;
    }
    if (p->configured && p->slot == slot && p->device_sinks == device_sinks && p->raw_inputs == raw_inputs &&
        p->used == used && p->final_buffers == final_buffers && p->valid_mask == valid_mask &&
        !memcmp(p->map, map, sizeof map)) return;
    sink_probe_report(p);
    p->configured = true;
    p->slot = slot;
    p->device_sinks = device_sinks;
    p->raw_inputs = raw_inputs;
    p->final_buffers = final_buffers;
    p->used = used;
    p->valid_mask = valid_mask;
    memcpy(p->map, map, sizeof map);
    LOG("[audren-sink] config frame=%llu device_sinks=%u slot=%u used=%u raw_inputs=%u "
        "map=%u,%u,%u,%u,%u,%u final_buffers=%u valid_mask=0x%02x\n",
        (unsigned long long)r->elapsed_frames, device_sinks, slot, used, raw_inputs,
        map[0], map[1], map[2], map[3], map[4], map[5], final_buffers, valid_mask);
}

static void sink_probe_samples(Renderer *r, const Mix *final, uint32_t count) {
    AudioSinkProbe *p = &r->sink_probe;
    if (!p->configured || !p->used || !final || !final->buf || !count) return;
    if (!p->samples) p->first_frame = r->elapsed_frames;
    for (int c = 0; c < 6; c++) {
        if (!(p->valid_mask & (1u << c))) continue;
        const float *channel = final->buf + p->map[c] * count;
        for (uint32_t i = 0; i < count; i++) {
            float sample = channel[i] * final->volume;
            if (!isfinite(sample)) { p->nonfinite[c]++; continue; }
            p->nonzero[c] += sample != 0;
            float magnitude = fabsf(sample);
            if (magnitude > p->peak[c]) p->peak[c] = magnitude;
        }
    }
    p->samples += count;
    if (p->samples >= 5ull * (r->sample_rate ? r->sample_rate : 48000)) sink_probe_report(p);
}

// One sample of DSP ADPCM at absolute sample index s (frames of 8 bytes: header + 14 nibbles).
static int16_t adpcm_next(const uint8_t *data, uint64_t size, uint32_t s, const int16_t *coefs, ChanState *cs) {
    uint32_t frame = s / 14, n = s % 14;
    uint64_t hdr = (uint64_t)frame * 8;
    if (hdr + 1 + n / 2 >= size) return 0;
    uint8_t ps = data[hdr];
    int scale = ps & 15, ci = (ps >> 4) & 7;
    uint8_t b = data[hdr + 1 + n / 2];
    int nib = (n & 1) ? (int8_t)(b << 4) >> 4 : (int8_t)b >> 4;
    int pred = coefs[ci * 2] * cs->h0 + coefs[ci * 2 + 1] * cs->h1;
    int v = ((nib * (2048 << scale)) + pred + 1024) >> 11;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    cs->h1 = cs->h0;
    cs->h0 = (int16_t)v;
    return (int16_t)v;
}

static void mark_end(ChanState *cs, WaveBuf *wb, uint32_t *idx) {
    cs->valid[*idx] = 0;
    *idx = (*idx + 1) % WB_COUNT;
    cs->loop_count = 0;
    cs->consumed++;
    if (wb->eos) cs->played = 0;
}

// Decode up to `need` samples of channel `ch` into out; returns how many were produced.
static int decode_voice(Voice *v, int ch, ChanState *cs, int16_t *out, int need) {
    int y = 0;
    uint32_t idx = cs->wb_index;
    const int16_t *coefs = (const int16_t *)(uintptr_t)v->ds_addr;
    while (y < need) {
        if (!cs->valid[idx]) break;  // starving
        WaveBuf *wb = &v->wb[idx];
        if (cs->offset == 0 && v->fmt == 6 && wb->ctx) {
            const int16_t *c = (const int16_t *)(uintptr_t)wb->ctx;
            cs->pred_scale = c[0]; cs->h0 = c[1]; cs->h1 = c[2];
        }
        uint32_t start = wb->start, end = wb->end;
        if (cs->loop_count > 0 && wb->loop_start && wb->loop_end && wb->loop_start <= wb->loop_end) {
            start = wb->loop_start;
            end = wb->loop_end;
        }
        int total = (int)end - (int)start, n = 0;
        int avail = total - cs->offset;
        if (avail < 0) avail = 0;
        int want = need - y < avail ? need - y : avail;
        const uint8_t *data = (const uint8_t *)(uintptr_t)wb->addr;
        if (data && wb->size) {
            if (v->fmt == 2) {  // PCM16 interleaved
                const int16_t *pcm = (const int16_t *)data;
                uint64_t nsamp = wb->size / 2 / v->chans;
                for (; n < want; n++) {
                    uint64_t s = (uint64_t)start + cs->offset + n;
                    if (s >= nsamp) break;
                    out[y + n] = pcm[s * v->chans + ch];
                }
            } else if (v->fmt == 6 && coefs) {
                for (; n < want; n++) out[y + n] = adpcm_next(data, wb->size, start + cs->offset + n, coefs, cs);
            } else if (v->fmt == 3) {  // PCM float
                const float *pcm = (const float *)data;
                uint64_t nsamp = wb->size / 4 / v->chans;
                for (; n < want; n++) {
                    uint64_t s = (uint64_t)start + cs->offset + n;
                    if (s >= nsamp) break;
                    out[y + n] = (int16_t)clampf(pcm[s * v->chans + ch] * 32767.f, -32768.f, 32767.f);
                }
            }
        }
        y += n;
        cs->offset += n;
        cs->played += n;
        if (cs->offset >= total || n == 0) {
            cs->offset = 0;
            if (wb->loop) {
                cs->loop_count++;
                if (wb->loop_count >= 0 && (n == 0 || cs->loop_count > wb->loop_count)) mark_end(cs, wb, &idx);
                if (n == 0) break;
                if (v->behaviour & 1) cs->played = 0;  // PlayedSampleCountResetWhenLooping
            } else {
                mark_end(cs, wb, &idx);
            }
        }
    }
    cs->wb_index = idx;
    return y;
}

// UpdateParametersForCommandGeneration: returns whether the voice should be played this frame.
static int voice_prepare(Renderer *r, Voice *v) {
    int nch = v->chans > 6 ? 6 : v->chans;
    if (v->is_new) {
        for (int c = 0; c < nch; c++) memset(&r->chst[v->chan_ids[c] % r->voices], 0, sizeof(ChanState));
        v->prev_volume = v->volume;
        v->is_new = 0;
    }
    if (v->flush) {
        uint32_t wi = 0;
        for (uint32_t i = 0; i < v->flush; i++) {
            for (int c = 0; c < nch; c++) {
                ChanState *cs = &r->chst[v->chan_ids[c] % r->voices];
                if (i == 0) wi = cs->wb_index;
                cs->valid[(wi + i) % WB_COUNT] = 0;
                cs->wb_index = (cs->wb_index + 1) % WB_COUNT;
                cs->consumed++;
            }
            v->wb[(wi + i) % WB_COUNT].sent = 1;
        }
        v->flush = 0;
    }
    if (v->play_state == 0) {
        for (int i = 0; i < WB_COUNT; i++) {
            if (v->wb[i].sent) continue;
            for (int c = 0; c < nch; c++) r->chst[v->chan_ids[c] % r->voices].valid[i] = 1;
            v->wb[i].sent = 1;
        }
        ChanState *p = &r->chst[v->chan_ids[0] % r->voices];
        for (int i = 0; i < WB_COUNT; i++) if (p->valid[i]) return 1;
        return 0;
    }
    if (v->play_state == 2) {  // stopping: drop everything queued
        for (int i = 0; i < WB_COUNT; i++) {
            v->wb[i].sent = 1;
            for (int c = 0; c < nch; c++) {
                ChanState *cs = &r->chst[v->chan_ids[c] % r->voices];
                if (cs->valid[i]) { cs->wb_index = (cs->wb_index + 1) % WB_COUNT; cs->consumed++; }
                cs->valid[i] = 0;
            }
        }
        for (int c = 0; c < nch; c++) {
            ChanState *cs = &r->chst[v->chan_ids[c] % r->voices];
            cs->offset = 0; cs->played = 0; cs->fraction = 0; cs->loop_count = 0;
            memset(cs->hist, 0, sizeof cs->hist);
            cs->pred_scale = cs->h0 = cs->h1 = 0;
        }
        v->play_state = 1;
    }
    return 0;
}

static Mix *find_mix(Renderer *r, int32_t id) { return id >= 0 && (uint32_t)id < r->nmix && r->mix[id].used ? &r->mix[id] : NULL; }

// ------------------------------------------------------------------ delay lines shared by audren effects
typedef struct { float *buf; uint32_t sr, cur, last, count, max; } DelayLine;

static void dl_set(DelayLine *d, float ms) {
    uint32_t n = (uint32_t)roundf(d->sr * ms);
    d->count = n < d->max ? n : d->max;
    d->cur = 0;
    d->last = d->count ? d->count - 1 : 0;
}
static void dl_init(DelayLine *d, uint32_t sr, float ms_max) {
    d->sr = sr;
    d->max = (uint32_t)roundf(sr * ms_max);
    free(d->buf);
    d->buf = calloc(d->max + 1, sizeof(float));
    dl_set(d, ms_max);
}
static float dl_read(DelayLine *d) { return d->buf[d->cur]; }
static float dl_update(DelayLine *d, float v) {
    float o = d->buf[d->cur];
    d->buf[d->cur++] = v;
    if (d->cur >= d->last) d->cur = 0;
    return o;
}
static float dl_tap_unsafe(DelayLine *d, uint32_t idx, int off) {
    int t = (int)d->cur - ((int)idx + off);
    if (t < 0) t += (int)d->count;
    if (t < 0 || (uint32_t)t > d->max) return 0;
    return d->buf[t];
}
static float dl_tap(DelayLine *d, uint32_t idx) {
    if (idx >= d->count) idx = d->count ? d->count - 1 : 0;
    return dl_tap_unsafe(d, idx, -1);
}

// audren Delay uses Q14 Hz, whereas Reverb passes Q14 samples per millisecond.
typedef struct {
    uint8_t input[6], output[6];
    uint16_t ch_max, ch;
    uint32_t max_ms, ms, rate, in_gain, feedback, out_gain, dry_gain, spread, lowpass;
} DelayParam;
_Static_assert(sizeof(DelayParam) == 0x34, "audren DelayParameter layout");

typedef struct {
    DelayParam p;
    uint8_t status;
    float *storage;
    DelayLine line[6];
    float lowpass_z[6], base, cross, lowpass;
} Delay;

static bool effect_channels_valid(uint16_t ch) { return ch == 1 || ch == 2 || ch == 4 || ch == 6; }

static void delay_release(Delay *d) {
    free(d->storage);
    d->storage = NULL;
    memset(d->line, 0, sizeof d->line);
    memset(d->lowpass_z, 0, sizeof d->lowpass_z);
}

static float delay_q14(uint32_t value) { return value * (1.0f / 16384.0f); }

static void delay_update(Delay *d, const uint8_t *e) {
    DelayParam p;
    memcpy(&p, e + 0x20, sizeof p);
    if (!effect_channels_valid(p.ch_max) || !effect_channels_valid(p.ch) || p.ch > p.ch_max) return;
    uint8_t status = e[0x54];  // 0: initialize DSP state, 1: update coefficients, 2: enabled
    uint64_t work_size;
    memcpy(&work_size, e + 0x10, sizeof work_size);
    uint32_t sr = (p.rate >> 14) / 1000;
    uint64_t max_samples = (uint64_t)sr * p.max_ms;
    if (!sr || max_samples > UINT32_MAX || max_samples * p.ch_max > work_size / sizeof(float)) return;
    bool reset = e[1] || (status == 0 && d->status != 0) || p.ch != d->p.ch ||
        p.max_ms != d->p.max_ms || p.rate != d->p.rate;
    if (reset) delay_release(d);
    d->p = p;
    d->status = status;
    if (!e[2]) return;

    bool initialize = !d->storage;
    if (initialize) {
        size_t stride = max_samples ? (size_t)max_samples : 1;
        if (stride > SIZE_MAX / sizeof(float) / p.ch) return;
        d->storage = calloc(stride * p.ch, sizeof(float));
        if (!d->storage) return;
        uint32_t count = sr * (p.ms < p.max_ms ? p.ms : p.max_ms);
        for (int c = 0; c < p.ch; c++) {
            d->line[c] = (DelayLine){.buf = d->storage + c * stride, .sr = sr,
                .max = (uint32_t)max_samples, .count = count, .last = count ? count - 1 : 0};
        }
        if (getenv("EOWRECOMP_AUDIO_LOG"))
            LOG("[audren] delay init: ch %u rate %u delay %u/%u ms, ring %u samples\n",
                p.ch, p.rate >> 14, p.ms, p.max_ms, d->line[0].last);
    }
    if (initialize || status == 1) {
        float spread = delay_q14(p.spread);
        float feedback = 0.98f * delay_q14(p.feedback);
        d->base = (1.0f - spread) * feedback;
        d->cross = spread * (p.ch >= 4 ? 0.5f : 1.0f) * feedback;
        d->lowpass = 0.95f * delay_q14(p.lowpass);
    }
}

static void delay_process(Delay *d, float *const *in, float *const *out, uint32_t n) {
    // Standard surround order is FL, FR, FC, LFE, BL, BR. The LFE loop is isolated.
    static const uint8_t quad[4][2] = {{1, 2}, {0, 3}, {0, 3}, {1, 2}};
    static const uint8_t surround[6][2] = {{2, 4}, {2, 5}, {0, 1}, {0, 0}, {0, 5}, {1, 4}};
    int ch = d->p.ch;
    float in_gain = delay_q14(d->p.in_gain), dry = delay_q14(d->p.dry_gain), wet = delay_q14(d->p.out_gain);
    float raw_feedback = delay_q14(d->p.feedback);
    for (uint32_t s = 0; s < n; s++) {
        float input[6], delayed[6];
        for (int c = 0; c < ch; c++) {
            input[c] = in[c][s] * 64.0f;
            delayed[c] = dl_read(&d->line[c]);
        }
        for (int c = 0; c < ch; c++) {
            float feedback;
            if (ch == 1 || (ch == 6 && c == 3)) feedback = delayed[c] * raw_feedback;
            else if (ch == 2) feedback = delayed[c] * d->base + delayed[c ^ 1] * d->cross;
            else {
                const uint8_t *peers = ch == 4 ? quad[c] : surround[c];
                feedback = delayed[c] * d->base + delayed[peers[0]] * d->cross + delayed[peers[1]] * d->cross;
            }
            float next = (input[c] * in_gain + feedback) * (1.0f - d->lowpass) + d->lowpass_z[c] * d->lowpass;
            d->lowpass_z[c] = next;
            dl_update(&d->line[c], next);
            out[c][s] = (input[c] * dry + delayed[c] * wet) / 64.0f;
        }
    }
}

// ------------------------------------------------------------------ reverb (port of Ryujinx ReverbState/Command)
typedef struct { DelayLine dl; float rate; } DecayDelay;
static float dd_update(DecayDelay *d, float v) {
    float p = v - d->rate * dl_read(&d->dl);
    return dl_update(&d->dl, p) + p * d->rate;
}

typedef struct {
    uint8_t input[6], output[6];
    uint16_t ch_max, ch;
    int32_t sample_rate, early_mode, early_gain, pre_delay, late_mode, late_gain, decay_time, hf_decay_ratio,
        coloration, reverb_gain, out_gain, dry_gain;
} ReverbParam;

typedef struct {
    int ready;
    uint8_t raw[0x44];  // last parameter bytes seen
    ReverbParam p;
    DelayLine fdn[4], pre, fc;
    DecayDelay decay[4];
    uint32_t early_time[10], pre_time;
    float early_gain[10], hf_direct[4], hf_prev[4], prev_fb[4];
} Reverb;

static const float rv_fdn_times[20] = {53.953247f, 79.192566f, 116.238770f, 130.615295f, 53.953247f, 79.192566f,
    116.238770f, 170.615295f, 5, 10, 5, 10, 47.03f, 71, 103, 170, 53.953247f, 79.192566f, 116.238770f, 170.615295f};
static const float rv_decay_times[20] = {7, 9, 13, 17, 7, 9, 13, 17, 1, 1, 1, 1, 7, 7, 13, 9, 7, 9, 13, 17};
static const float rv_early_times[50] = {0.0f, 3.5f, 2.8f, 3.9f, 2.7f, 13.4f, 7.9f, 8.4f, 9.9f, 12.0f,
    0.0f, 11.8f, 5.5f, 11.2f, 10.4f, 38.1f, 22.2f, 29.6f, 21.2f, 24.8f,
    0.0f, 41.5f, 20.5f, 41.3f, 0.0f, 29.5f, 33.8f, 45.2f, 46.8f, 0.0f,
    33.1f, 43.3f, 22.8f, 37.9f, 14.9f, 35.3f, 17.9f, 34.2f, 0.0f, 43.3f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const float rv_early_gain[50] = {0.70f, 0.68f, 0.70f, 0.68f, 0.70f, 0.68f, 0.70f, 0.68f, 0.68f, 0.68f,
    0.70f, 0.68f, 0.70f, 0.68f, 0.70f, 0.68f, 0.68f, 0.68f, 0.68f, 0.68f,
    0.50f, 0.70f, 0.70f, 0.68f, 0.50f, 0.68f, 0.68f, 0.70f, 0.68f, 0.00f,
    0.93f, 0.92f, 0.87f, 0.86f, 0.94f, 0.81f, 0.80f, 0.77f, 0.76f, 0.65f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const float rv_pre_times[5] = {12.5f, 40.0f, 50.0f, 50.0f, 0.0f};

static float q14(int32_t v) { return v / 16384.0f; }
static uint32_t rv_rate(const ReverbParam *p) {
    uint32_t sr = (uint32_t)q14(p->sample_rate);  // samples per millisecond
    return sr && sr <= 192 ? sr : 48;
}

static void reverb_update(Reverb *rv) {
    ReverbParam *p = &rv->p;
    uint32_t sr = rv_rate(p);
    float pre_ms = q14(p->pre_delay), early_gain = q14(p->early_gain), coloration = q14(p->coloration);
    float decay_time = q14(p->decay_time);
    int em = p->early_mode >= 0 && p->early_mode < 5 ? p->early_mode : 4;
    int lm = p->late_mode >= 0 && p->late_mode < 5 ? p->late_mode : 0;
    for (int i = 0; i < 10; i++) {
        uint32_t t = (uint32_t)roundf(sr * (rv_early_times[i] + pre_ms));
        rv->early_time[i] = (t < rv->pre.max ? t : rv->pre.max) + 1;
        rv->early_gain[i] = rv_early_gain[i] * early_gain;
    }
    if (p->ch == 2) { rv->early_gain[4] *= 0.5f; rv->early_gain[5] *= 0.5f; }
    uint32_t pt = (uint32_t)roundf(sr * (rv_pre_times[em] + pre_ms));
    rv->pre_time = pt < rv->pre.max ? pt : rv->pre.max;
    float hf_ratio = q14(p->hf_decay_ratio), hf_cos = cosf(1280.0f / sr);
    for (int i = 0; i < 4; i++) {
        dl_set(&rv->fdn[i], rv_fdn_times[lm * 4 + i]);
        dl_set(&rv->decay[i].dl, rv_decay_times[lm * 4 + i]);
        float a = -3.0f * (rv->decay[i].dl.count + rv->fdn[i].count);
        float b = decay_time > 0 ? a / (decay_time * sr) : -1000.0f;
        float c, d;
        if (hf_ratio < 0.995f && hf_ratio > 0) {
            float e = powf(10.0f, (((1.0f / hf_ratio) - 1.0f) * 2) / 100 * (b / 10));
            float f = 1.0f - e, g = 2.0f - (e * 2 * hf_cos);
            float h = sqrtf(fmaxf(0, g * g - f * f * 4));
            c = f != 0 ? (g - h) / (f * 2) : 0;
            d = 1.0f - c;
        } else {
            c = 0;
            d = 1;
        }
        rv->hf_direct[i] = powf(10.0f, b / 1000) * d * 0.7071f;
        rv->hf_prev[i] = c;
        rv->prev_fb[i] = 0;
        rv->decay[i].rate = 0.6f * (1.0f - coloration);
    }
}

static void reverb_init(Reverb *rv) {
    uint32_t sr = rv_rate(&rv->p);
    for (int i = 0; i < 4; i++) {
        dl_init(&rv->fdn[i], sr, rv_fdn_times[16 + i]);
        dl_init(&rv->decay[i].dl, sr, rv_decay_times[16 + i]);
    }
    dl_init(&rv->pre, sr, 150.0f);  // long pre-delay needs REV3+... 350 ms
    dl_init(&rv->fc, sr, 5.0f);
    reverb_update(rv);
    rv->ready = 1;
}

static void reverb_process(Reverb *rv, float *const *in, float *const *out, uint32_t n) {
    static const int8_t oe1[10] = {0}, te[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    static const int8_t o1[4] = {0}, tf1[4] = {0, 1, 2, 3};
    static const int8_t oe2[10] = {0, 0, 1, 1, 0, 1, 0, 0, 1, 1}, o2[4] = {0, 0, 1, 1}, tf2[4] = {2, 0, 3, 1};
    static const int8_t oe4[10] = {0, 0, 1, 1, 0, 1, 2, 2, 3, 3}, o4[4] = {0, 1, 2, 3}, tf4[4] = {0, 1, 2, 3};
    static const int8_t oe6[20] = {0, 5, 0, 5, 1, 5, 1, 5, 4, 5, 4, 5, 2, 5, 2, 5, 3, 5, 3, 5};
    static const int8_t te6[20] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9};
    static const int8_t o6[6] = {0, 1, 2, 3, 4, 5}, tf6[6] = {0, 1, 2, 3, -1, 3};
    int ch = rv->p.ch;
    const int8_t *oe, *tel, *o, *tf;
    int ne, nf;
    switch (ch) {
    case 1: oe = oe1; tel = te; o = o1; tf = tf1; ne = 10; nf = 4; break;
    case 2: oe = oe2; tel = te; o = o2; tf = tf2; ne = 10; nf = 4; break;
    case 4: oe = oe4; tel = te; o = o4; tf = tf4; ne = 10; nf = 4; break;
    case 6: oe = oe6; tel = te6; o = o6; tf = tf6; ne = 20; nf = 6; break;
    default: return;
    }
    float reverb_gain = q14(rv->p.reverb_gain), late_gain = q14(rv->p.late_gain);
    float out_gain = q14(rv->p.out_gain), dry_gain = q14(rv->p.dry_gain);
    for (uint32_t s = 0; s < n; s++) {
        float ov[6] = {0}, fb[4], fo[4], cin[6];
        for (int i = 0; i < ne; i++) ov[oe[i]] += dl_tap_unsafe(&rv->pre, rv->early_time[tel[i]], 0) * rv->early_gain[tel[i]];
        if (ch == 6) ov[5] *= 0.2f;
        float pre = 0;
        for (int c = 0; c < ch; c++) { cin[c] = in[c][s] * 64; pre += cin[c] * reverb_gain; }
        dl_update(&rv->pre, pre);
        float late = dl_tap(&rv->pre, rv->pre_time) * late_gain;
        for (int i = 0; i < 4; i++) {
            fo[i] = dl_read(&rv->fdn[i]) * rv->hf_direct[i] + rv->prev_fb[i] * rv->hf_prev[i];
            rv->prev_fb[i] = fo[i];
        }
        fb[0] = fo[2] + fo[1];
        fb[1] = -fo[0] - fo[3];
        fb[2] = fo[0] - fo[3];
        fb[3] = fo[1] - fo[2];
        for (int i = 0; i < 4; i++) {
            fo[i] = dd_update(&rv->decay[i], fb[i] + late);
            dl_update(&rv->fdn[i], fo[i]);
        }
        for (int i = 0; i < nf; i++) if (tf[i] >= 0) ov[o[i]] += fo[tf[i]];
        if (ch == 6) ov[4] += dl_update(&rv->fc, (fo[2] - fo[3]) * 0.5f);
        for (int c = 0; c < ch; c++) out[c][s] = (ov[c] * out_gain + cin[c] * dry_gain) / 64;
    }
}

struct Effect {
    uint8_t type, enabled;
    int32_t mix_id;
    uint32_t order;
    union { Delay delay; Reverb rv; };
};

static void effect_release(Effect *fx) {
    if (fx->type == 3) delay_release(&fx->delay);
    else if (fx->type == 4) {
        for (int i = 0; i < 4; i++) {
            free(fx->rv.fdn[i].buf);
            free(fx->rv.decay[i].dl.buf);
        }
        free(fx->rv.pre.buf);
        free(fx->rv.fc.buf);
    }
    memset(fx, 0, sizeof *fx);
}

// EffectInParameter (0xC0 bytes: type, is_new, enabled, mix, buffers, order, data).
static void effect_update(Effect *fx, const uint8_t *e) {
    if (fx->type != e[0]) effect_release(fx);
    fx->type = e[0];
    fx->enabled = e[2];
    memcpy(&fx->mix_id, e + 4, 4);
    memcpy(&fx->order, e + 0x18, 4);
    if (fx->type == 3) { delay_update(&fx->delay, e); return; }
    if (fx->type != 4) return;
    const uint8_t *d = e + 0x20;
    uint16_t ch_max, ch;
    memcpy(&ch_max, d + 12, 2);
    memcpy(&ch, d + 14, 2);
    if (!effect_channels_valid(ch_max) || !effect_channels_valid(ch)) return;
    if (e[1] || !fx->rv.ready) {
        memcpy(fx->rv.raw, d, sizeof fx->rv.raw);
        memcpy(&fx->rv.p, d, sizeof fx->rv.p);
        if (getenv("EOWRECOMP_AUDIO_LOG"))
            LOG("[audren] reverb init: ch %u rate %d early %d/%d pre %d late %d/%d decay %d hf %d col %d gains %d/%d/%d\n",
                ch, fx->rv.p.sample_rate, fx->rv.p.early_mode, fx->rv.p.early_gain, fx->rv.p.pre_delay,
                fx->rv.p.late_mode, fx->rv.p.late_gain, fx->rv.p.decay_time, fx->rv.p.hf_decay_ratio,
                fx->rv.p.coloration, fx->rv.p.reverb_gain, fx->rv.p.out_gain, fx->rv.p.dry_gain);
        reverb_init(&fx->rv);
    } else if (memcmp(fx->rv.raw, d, 0x40)) {  // parameters changed (status byte at 0x40 ignored)
        memcpy(fx->rv.raw, d, sizeof fx->rv.raw);
        memcpy(&fx->rv.p, d, sizeof fx->rv.p);
        reverb_update(&fx->rv);
    }
}

static void apply_effects(Renderer *r, int32_t mix_id, Mix *m) {
    static const uint8_t legacy_surround[6] = {0, 1, 4, 5, 2, 3};
    uint32_t N = r->sample_count;
    if (!m->buf) return;
    for (uint32_t order = 0; order < r->effects; order++) {
        for (uint32_t i = 0; i < r->effects; i++) {
            Effect *fx = &r->fx[i];
            if (fx->mix_id != mix_id || fx->order != order) continue;
            bool is_delay = fx->type == 3;
            if (!is_delay && (fx->type != 4 || !fx->rv.ready)) continue;
            int ch = is_delay ? fx->delay.p.ch : fx->rv.p.ch;
            const uint8_t *input = is_delay ? fx->delay.p.input : fx->rv.p.input;
            const uint8_t *output = is_delay ? fx->delay.p.output : fx->rv.p.output;
            bool legacy_delay = is_delay && ch == 6 && (r->revision >> 24) - '0' < 11;
            float *in[6], *out[6];
            int ok = 1;
            for (int c = 0; c < ch; c++) {
                int index = legacy_delay ? legacy_surround[c] : c;
                if (input[index] >= m->bufs || output[index] >= m->bufs) { ok = 0; break; }
                in[c] = m->buf + input[index] * N;
                out[c] = m->buf + output[index] * N;
            }
            if (!ok) continue;
            if (fx->enabled) {
                if (is_delay && fx->delay.storage) {
                    delay_process(&fx->delay, in, out, N);
                    continue;
                }
                if (!is_delay) {
                    static int no_reverb = -1;
                    if (no_reverb < 0) no_reverb = getenv("EOWRECOMP_NO_REVERB") ? 1 : 0;
                    static int toggle = -1;
                    if (toggle < 0) toggle = getenv("EOWRECOMP_REVERB_TOGGLE") ? atoi(getenv("EOWRECOMP_REVERB_TOGGLE")) : 0;
                    int off = no_reverb || !g_settings.reverb || (toggle && (r->elapsed_frames / toggle) % 2);
                    if (!off) {
                        reverb_process(&fx->rv, in, out, N);
                        continue;
                    }
                }
            }
            for (int c = 0; c < ch; c++) if (in[c] != out[c]) memcpy(out[c], in[c], N * sizeof(float));
        }
    }
}

static int hf_log = -1;
static double hf[1024];

static void render_device_sink(const Renderer *r, const Mix *final, uint32_t count, float *stereo, float *surround) {
    for (uint32_t i = 0; i < count; i++) {
        // Sink slots are FL, FR, FC, LFE, BL, BR. The guest maps mix buffers to these slots.
        float ch[6] = {0};
        if (final && r->sink_used)
            for (int c = 0; c < r->sink_inputs && c < 6; c++)
                if (r->sink_input[c] < final->bufs) ch[c] = final->buf[r->sink_input[c] * count + i] * final->volume;
        if (surround)
            for (int c = 0; c < 6; c++) surround[i * 6 + c] = clampf(ch[c] / 32768.f, -1, 1);
        if (!stereo) continue;
        float L = ch[0], R = ch[1];
        if (r->sink_inputs >= 6) {
            L += 0.707f * ch[2] + 0.251f * ch[3] + 0.707f * ch[4];
            R += 0.707f * ch[2] + 0.251f * ch[3] + 0.707f * ch[5];
        }
        stereo[i * 2] = clampf(L / 32768.f, -1, 1);
        stereo[i * 2 + 1] = clampf(R / 32768.f, -1, 1);
    }
}

static void render_frame(Renderer *r, float *stereo, float *surround) {
    if (hf_log < 0) hf_log = getenv("EOWRECOMP_AUDIO_HF") ? 1 : 0;
    uint32_t N = r->sample_count;
    for (uint32_t m = 0; m < r->nmix; m++)
        if (r->mix[m].buf) memset(r->mix[m].buf, 0, sizeof(float) * MIXBUF_MAX * N);
    int16_t tmp[HIST + 4096];
    float smp[4096];
    for (uint32_t vi = 0; vi < r->voices; vi++) {
        Voice *v = &r->voice[vi];
        if (!v->in_use) continue;
        int play = voice_prepare(r, v);
        if (!play || v->wb_count == 0 || v->drop) continue;
        Mix *dst = find_mix(r, v->mix_id);
        float ratio = (float)v->rate / r->sample_rate * v->pitch;
        if (!(ratio > 0) || ratio > 8) ratio = 1;
        int nch = v->chans > 6 ? 6 : v->chans;
        for (int c = 0; c < nch; c++) {
            uint32_t rid = v->chan_ids[c] % r->voices;
            ChanState *cs = &r->chst[rid];
            ChanRes *cr = &r->chres[rid];
            int need = (int)(cs->fraction + ratio * N);
            if (need > 4096) need = 4096;
            memcpy(tmp, cs->hist, sizeof cs->hist);
            int got = decode_voice(v, c, cs, tmp + HIST, need);
            if (got < need) memset(tmp + HIST + got, 0, (need - got) * 2);
            float pos = cs->fraction;
            for (uint32_t i = 0; i < N; i++, pos += ratio) {
                int k = (int)pos;
                float f = pos - k;
                if (k + 3 > need + HIST - 1) k = need + HIST - 4;
                float y0 = tmp[k], y1 = tmp[k + 1], y2 = tmp[k + 2], y3 = tmp[k + 3];
                if (ratio == 1.0f && f == 0) { smp[i] = y1; continue; }
                // 4-point Hermite between y1 and y2
                float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2 * y2 - 0.5f * y3;
                float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                smp[i] = ((c3 * f + c2) * f + c1) * f + y1;
            }
            for (int fi = 0; fi < 2; fi++) {
                if (!v->bq_on[fi]) continue;
                float *st = cs->bq[fi], *k = v->bq[fi];
                if (v->bq_reset[fi]) st[0] = st[1] = 0;
                for (uint32_t i = 0; i < N; i++) {
                    float in = smp[i], o = in * k[0] + st[0];
                    st[0] = in * k[1] + o * k[3] + st[1];
                    st[1] = in * k[2] + o * k[4];
                    smp[i] = o;
                }
            }
            cs->fraction = cs->fraction + ratio * N - need;
            if (cs->fraction < 0 || cs->fraction >= 1) cs->fraction = 0;
            memcpy(cs->hist, tmp + need, sizeof cs->hist);
            if (hf_log) {
                // Goertzel power around 19.6 kHz (scaled by volume)
                double w = 2 * M_PI * 19582.0 / r->sample_rate, cw = 2 * cos(w), q1 = 0, q2 = 0;
                for (uint32_t i = 0; i < N; i++) { double q0 = cw * q1 - q2 + smp[i]; q2 = q1; q1 = q0; }
                double e = (q1 * q1 + q2 * q2 - q1 * q2 * cw) * v->volume * v->volume / N;
                hf[vi] += e;
            }
            if (!dst) continue;
            static int solo = -2;
            if (solo == -2) solo = getenv("EOWRECOMP_AUDIO_SOLO") ? atoi(getenv("EOWRECOMP_AUDIO_SOLO")) : -1;
            if (solo >= 0 && (int)vi != solo) continue;
            for (uint32_t b = 0; b < dst->bufs && b < MIXBUF_MAX; b++) {
                float g0 = v->prev_volume * cr->prev[b], g1 = v->volume * cr->mix[b];
                if (g0 == 0 && g1 == 0) continue;
                float *o = dst->buf + b * N, dg = (g1 - g0) / N;
                for (uint32_t i = 0; i < N; i++) o[i] += smp[i] * (g0 + dg * i);
            }
        }
        for (int c = 0; c < nch; c++) {
            ChanRes *cr = &r->chres[v->chan_ids[c] % r->voices];
            memcpy(cr->prev, cr->mix, sizeof cr->prev);
        }
        v->bq_reset[0] = v->bq_reset[1] = 0;
        v->prev_volume = v->volume;
    }
    if (hf_log && r->elapsed_frames % 200 == 0) {
        LOG("[audren] hf f%llu:", (unsigned long long)r->elapsed_frames);
        for (uint32_t k = 0; k < r->voices; k++)
            if (hf[k] > 1e6)
                LOG(" v%u=%.0f(%llx bq %d%d %.3f/%.3f fmt %u rate %u pitch %.3f)", k, hf[k] / 1e6,
                    (unsigned long long)r->voice[k].wb[0].addr, r->voice[k].bq_on[0], r->voice[k].bq_on[1],
                    r->voice[k].bq[1][0], r->voice[k].bq[1][3], r->voice[k].fmt, r->voice[k].rate, r->voice[k].pitch);
        LOG("\n");
        memset(hf, 0, sizeof hf);
    }
    // sub mixes, deepest first
    int depth[64] = {0};
    int maxd = 0;
    for (uint32_t m = 1; m < r->nmix && m < 64; m++) {
        if (!r->mix[m].used) continue;
        int d = 0;
        for (Mix *x = &r->mix[m]; x && d < 64; x = find_mix(r, x->dest)) { if (x == &r->mix[0]) break; d++; }
        depth[m] = d;
        if (d > maxd) maxd = d;
    }
    for (int d = maxd; d >= 1; d--) {
        for (uint32_t m = 1; m < r->nmix && m < 64; m++) {
            Mix *s = &r->mix[m];
            if (!s->used || depth[m] != d) continue;
            apply_effects(r, (int32_t)m, s);
            Mix *t = find_mix(r, s->dest);
            if (!t || t == s) continue;
            for (uint32_t i = 0; i < s->bufs; i++)
                for (uint32_t j = 0; j < t->bufs; j++) {
                    float g = s->volume * s->vol[i * MIXBUF_MAX + j];
                    if (g == 0) continue;
                    float *src = s->buf + i * N, *o = t->buf + j * N;
                    for (uint32_t k = 0; k < N; k++) o[k] += src[k] * g;
                }
        }
    }
    // Final mix -> device sink -> playback and optional stereo diagnostic.
    Mix *f = find_mix(r, 0);
    if (f) apply_effects(r, 0, f);
    if (r->sink_probe.enabled) sink_probe_samples(r, f, N);
    render_device_sink(r, f, N, stereo, surround);
}

static double audio_gain_multiplier = 1.0;
static void load_audio_gain(void) {
    const char *env = getenv("EOWRECOMP_AUDIO_GAIN");
    if (env) audio_gain_multiplier = atof(env);
}

// Output gain: menu volume times EOWRECOMP_AUDIO_GAIN (applied live).
static void apply_gain(SDL_AudioStream *st) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load_audio_gain);
    float g = (float)(audio_gain_multiplier * g_settings.volume);
    if (st && SDL_GetAudioStreamGain(st) != g) SDL_SetAudioStreamGain(st, g);
}

static void wav_header(FILE *f, uint32_t bytes) {
    uint32_t h[11] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020003, 48000, 48000 * 8,
                      0x00200008, 0x61746164, bytes};
    fseek(f, 0, SEEK_SET);
    fwrite(h, 4, 11, f);
    fseek(f, 0, SEEK_END);
}

static int renderer_queue_target(Renderer *r, uint32_t N, int channels) {
    int frame_bytes = (int)N * channels * (int)sizeof(float);
    int target = 4 * frame_bytes;  // queued audio to keep ahead of the device: >= 2 device periods
    if (r->stream) {
        SDL_AudioSpec dev;
        int dev_frames = 0;
        if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(r->stream), &dev, &dev_frames) && dev_frames > 0) {
            int t = (2 * dev_frames * (int)r->sample_rate / (dev.freq ? dev.freq : 48000) + (int)N) * channels * (int)sizeof(float);
            if (t > target) target = t;
            LOG("[audren] device %d Hz, %d frames/period; queue target %d samples\n", dev.freq, dev_frames,
                target / (channels * (int)sizeof(float)));
        }
    }
    return target;
}

#ifdef _WIN32
static int renderer_output_channels(SDL_AudioStream *stream, SDL_AudioSpec source, int current, bool surround) {
    if (!stream) return 2;
    SDL_AudioSpec device;
    int device_channels = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &device, NULL)) device_channels = device.channels;
    source.channels = surround && (device_channels == 6 || device_channels == 8) ? 6 : 2;
    if (source.channels != current && !SDL_SetAudioStreamFormat(stream, &source, NULL)) {
        LOG("[audren] cannot change stream source to %d channels: %s\n", source.channels, SDL_GetError());
        return current;
    }
    if (surround || current != source.channels)
        LOG("[audren] layout %s, source %d channels, endpoint %d channels\n",
            source.channels == 6 ? "5.1" : "stereo", source.channels, device_channels);
    return source.channels;
}
#endif

static void *renderer_tick(void *arg) {
    Renderer *r = arg;
    float stereo[2 * 4096];
    float *surround = NULL;
    uint32_t N = r->sample_count > 4096 ? 4096 : r->sample_count;
    int stereo_bytes = (int)(N * 2 * sizeof(float));
    int channels = 2;
    uint64_t next = 0;
    struct timespec now;
    int target = renderer_queue_target(r, N, channels);
#ifdef _WIN32
    float multichannel[6 * 4096];
    bool requested_surround = false;
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, (int)r->sample_rate};
#endif
    while (!r->quit) {
#ifdef _WIN32
        bool want_surround = audio_output_surround();
        SDL_AudioStream *previous = r->stream;
        bool refreshed = refresh_output(&r->output, &r->stream, &spec, "audren");
        if (refreshed || requested_surround != want_surround) {
            requested_surround = want_surround;
            if (r->stream != previous) channels = 2;  // A replacement stream opens in stereo.
            channels = renderer_output_channels(r->stream, spec, channels, want_surround);
            surround = channels == 6 ? multichannel : NULL;
            target = renderer_queue_target(r, N, channels);
            next = 0;
        }
#endif
        if (r->stream) {
            if (SDL_GetAudioStreamQueued(r->stream) >= target) {
                struct timespec ts = {0, 1000000};
                nanosleep(&ts, NULL);
                continue;
            }
        } else {
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t t = now.tv_sec * 1000000000ull + now.tv_nsec;
            if (!next) next = t;
            if (next > t) { struct timespec ts = {0, (long)(next - t)}; nanosleep(&ts, NULL); }
            next += (uint64_t)N * 1000000000ull / r->sample_rate;
        }
        float *playback = surround ? surround : stereo;
        int playback_bytes = (int)(N * channels * sizeof(float));
        pthread_mutex_lock(&r->lock);
        bool running = r->state == 0;
        if (running) {
            render_frame(r, !surround || r->wav ? stereo : NULL, surround);
            r->elapsed_frames++;
        } else {
            memset(playback, 0, playback_bytes);
        }
        pthread_mutex_unlock(&r->lock);
        if (r->stream) { apply_gain(r->stream); SDL_PutAudioStreamData(r->stream, playback, playback_bytes); }
        if (r->wav && running) {
            fwrite(stereo, 1, stereo_bytes, r->wav);
            r->wav_bytes += stereo_bytes;
            if ((r->elapsed_frames & 255) == 0) { wav_header(r->wav, r->wav_bytes); fflush(r->wav); }
        }
        pthread_mutex_lock(&g_klock);
        kernel_signal(r->event);
        pthread_mutex_unlock(&g_klock);
    }
    return NULL;
}

static void update_voice(Renderer *r, Voice *v, const uint8_t *p, uint8_t *out) {
    uint8_t is_new = p[8];
    if (is_new) {
        memset(v, 0, sizeof *v);
        v->play_state = 1;
        v->mix_id = -1;
        v->splitter_id = 0xFFFFFFFF;
        for (int i = 0; i < WB_COUNT; i++) v->wb[i].sent = 1;
        v->is_new = 1;
    }
    v->in_use = 1;
    uint8_t ps = p[10];
    if (ps == 0) v->play_state = 0;
    else if (ps == 1) { if (v->play_state != 1) v->play_state = 2; }
    else if (ps == 2) v->play_state = 3;
    v->fmt = p[11];
    memcpy(&v->rate, p + 12, 4);
    uint32_t ch;
    memcpy(&ch, p + 24, 4);
    v->chans = ch > 6 ? 6 : (uint8_t)ch;
    memcpy(&v->pitch, p + 28, 4);
    memcpy(&v->volume, p + 32, 4);
    memcpy(&v->wb_count, p + 60, 4);
    memcpy(&v->ds_addr, p + 72, 8);
    memcpy(&v->mix_id, p + 88, 4);
    memcpy(&v->splitter_id, p + 92, 4);
    memcpy(v->chan_ids, p + 320, 24);
    v->flush += p[345];
    memcpy(&v->behaviour, p + 348, 2);
    if (p[344]) v->drop = 0;
    for (int f = 0; f < 2; f++) {  // BiquadFilterParameter: enable, reserved, s16 numerator[3], denominator[2] (Q14)
        const uint8_t *b = p + 36 + f * 12;
        if (b[0] && !v->bq_on[f]) {
            v->bq_reset[f] = 1;
            if (getenv("EOWRECOMP_AUDIO_LOG")) {
                int16_t k[5];
                memcpy(k, b + 2, 10);
                LOG("[audren] v%ld biquad%d on: %d %d %d / %d %d\n", (long)(v - r->voice), f, k[0], k[1], k[2], k[3], k[4]);
            }
        }
        v->bq_on[f] = b[0];
        int16_t c[5];
        memcpy(c, b + 2, 10);
        for (int k = 0; k < 5; k++) v->bq[f][k] = c[k] / 16384.0f;
    }
    if (v->splitter_id != 0xFFFFFFFF && v->mix_id < 0) {
        static int warned;
        if (!warned++) LOG("[audren] splitter destinations not implemented\n");
    }
    ChanState *cs0 = &r->chst[v->chan_ids[0] % r->voices];
    if (is_new)
        for (int c = 0; c < v->chans; c++) memset(r->chst[v->chan_ids[c] % r->voices].valid, 0, WB_COUNT);
    for (int i = 0; i < WB_COUNT; i++) {
        const uint8_t *w = p + 96 + i * 0x38;
        WaveBuf *wb = &v->wb[i];
        if (!cs0->valid[i] && wb->sent) wb->addr = 0;
        if (!w[26]) {  // !SentToServer: a new buffer for the DSP
            memcpy(&wb->addr, w, 8);
            memcpy(&wb->size, w + 8, 8);
            memcpy(&wb->start, w + 16, 4);
            memcpy(&wb->end, w + 20, 4);
            wb->loop = w[24];
            wb->eos = w[25];
            memcpy(&wb->loop_count, w + 28, 4);
            memcpy(&wb->ctx, w + 32, 8);
            memcpy(&wb->loop_start, w + 48, 4);
            memcpy(&wb->loop_end, w + 52, 4);
            if ((r->revision >> 24) - '0' < 8) {  // WaveBuffer version 1: loop forever over [start, end)
                wb->loop_count = -1;
                wb->loop_start = wb->loop_end = 0;
            }
            wb->sent = 0;
            static int dumped;
            if (getenv("EOWRECOMP_AUDIO_DUMPWB") && !dumped && wb->size > 0x100000 && v->fmt == 6 && v->ds_addr) {
                dumped = 1;
                FILE *f = fopen("build/wb_dump.bin", "wb");
                if (f) {
                    uint32_t hdr[4] = {(uint32_t)wb->size, wb->start, wb->end, (uint32_t)(wb->ctx != 0)};
                    fwrite(hdr, 4, 4, f);
                    fwrite((const void *)(uintptr_t)v->ds_addr, 1, 32, f);
                    int16_t ctx[3] = {0};
                    if (wb->ctx) memcpy(ctx, (const void *)(uintptr_t)wb->ctx, 6);
                    fwrite(ctx, 2, 3, f);
                    fwrite((const void *)(uintptr_t)wb->addr, 1, wb->size, f);
                    fclose(f);
                    LOG("[audren] dumped wave buffer %llx (%llx bytes) ds %llx ctx %d %d %d\n", (unsigned long long)wb->addr,
                        (unsigned long long)wb->size, (unsigned long long)v->ds_addr, ctx[0], ctx[1], ctx[2]);
                }
            }
            static int vlog = -1;
            if (vlog < 0) vlog = getenv("EOWRECOMP_AUDIO_LOG") ? 1 : 0;
            if (vlog)
                LOG("[audren] f%llu v%ld wb%d fmt %u rate %u ch %u pitch %.3f vol %.3f mix %d res %u addr %llx size %llx "
                    "[%u,%u) loop %u/%d eos %u ctx %llx ds %llx\n", (unsigned long long)r->elapsed_frames,
                    (long)(v - r->voice), i, v->fmt, v->rate, v->chans, v->pitch, v->volume, v->mix_id, v->chan_ids[0],
                    (unsigned long long)wb->addr, (unsigned long long)wb->size, wb->start, wb->end, wb->loop,
                    wb->loop_count, wb->eos, (unsigned long long)wb->ctx, (unsigned long long)v->ds_addr);
            if (vlog && v->ds_addr) {
                const int16_t *k = (const int16_t *)(uintptr_t)v->ds_addr;
                LOG("[audren]   coefs v%ld:", (long)(v - r->voice));
                for (int j = 0; j < 19; j++) LOG(" %d", k[j]);
                LOG("\n");
            }
        }
    }
    // VoiceOutStatus
    if (is_new || v->is_new) {
        memset(out, 0, 0x10);
    } else {
        memcpy(out, &cs0->played, 8);
        memcpy(out + 8, &cs0->consumed, 4);
        out[12] = v->drop;
    }
}

static void update_mix(Renderer *r, const uint8_t *p, int id) {
    if (id < 0 || (uint32_t)id >= r->nmix) return;
    Mix *m = &r->mix[id];
    memcpy(&m->volume, p, 4);
    memcpy(&m->bufs, p + 8, 4);
    if (m->bufs > MIXBUF_MAX) m->bufs = MIXBUF_MAX;
    m->used = p[12];
    memcpy(&m->dest, p + 0x924, 4);
    memcpy(&m->dest_splitter, p + 0x928, 4);
    memcpy(m->vol, p + 0x24, sizeof m->vol);
    if (!m->buf) m->buf = calloc(MIXBUF_MAX * r->sample_count, sizeof(float));
    LOG("[audren] mix %d used %u vol %.3f bufs %u dest %d:", id, m->used, m->volume, m->bufs, m->dest);
    for (int i = 0; i < MIXBUF_MAX * MIXBUF_MAX; i++)
        if (m->vol[i] != 0) LOG(" %d>%d=%.3f", i / MIXBUF_MAX, i % MIXBUF_MAX, m->vol[i]);
    LOG("\n");
}

// RequestUpdate: input is an UpdateDataHeader + sections; write a matching output.
static void request_update(Renderer *r, IpcReq *req, IpcResp *resp) {
    IpcBuf *in = in_buf(req, 0), *out = out_buf(req, 0);
    if (!in || !out || !in->size || !out->size) return;
    const uint8_t *ip = (const uint8_t *)(uintptr_t)in->addr;
    uint8_t *op = (uint8_t *)(uintptr_t)out->addr;
    const uint32_t *h = (const uint32_t *)ip;
    uint32_t behavior_sz = h[1], pools_sz = h[2], voices_sz = h[3], chans_sz = h[4], effects_sz = h[5];
    uint32_t mixes_sz = h[6], sinks_sz = h[7], perf_sz = h[8];
    {
        static int dump_n = -1;
        static long count;
        if (dump_n < 0) dump_n = getenv("EOWRECOMP_AUDIO_DUMP") ? 1 : 0;
        count++;
        if (dump_n && (count == 300 || count == 2000 || count == 6000 || count == 12000)) {
            char path[64];
            snprintf(path, sizeof path, "build/audren_in_%ld.bin", count);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(ip, 1, in->size, f); fclose(f); }
            LOG("[audren] dumped %s (%llu bytes, rev %08x)\n", path, (unsigned long long)in->size, r->revision);
        }
    }
    uint32_t npools = pools_sz / 0x20, nvoices = voices_sz / 0x170, neffects = effects_sz / 0xC0;
    uint32_t nchans = chans_sz / 0x70, nsinks = sinks_sz / 0x140;
    if (nvoices > r->voices) nvoices = r->voices;
    if (nchans > r->voices) nchans = r->voices;
    memset(op, 0, out->size);
    uint32_t *oh = (uint32_t *)op;
    oh[0] = h[0];
    uint32_t o = 0x40;
    pthread_mutex_lock(&r->lock);
    // memory pools: acknowledge attach/detach requests
    const uint8_t *pin = ip + 0x40 + behavior_sz;
    oh[2] = npools * 0x10;
    for (uint32_t i = 0; i < npools && o + 0x10 <= out->size; i++, o += 0x10) {
        uint32_t st = *(const uint32_t *)(pin + i * 0x20 + 0x10);
        *(uint32_t *)(op + o) = st == 4 ? 5 : st == 2 ? 3 : st;
    }
    // voice channel resources
    const uint8_t *cin = pin + pools_sz;
    for (uint32_t i = 0; i < nchans; i++) {
        const uint8_t *c = cin + i * 0x70;
        uint32_t id;
        memcpy(&id, c, 4);
        if (id >= r->voices) continue;
        r->chres[id].used = c[0x64];
        if (c[0x64]) memcpy(r->chres[id].mix, c + 4, sizeof r->chres[id].mix);
    }
    // voices
    const uint8_t *vin = cin + chans_sz;
    oh[3] = r->voices * 0x10;
    for (uint32_t i = 0; i < r->voices; i++) r->voice[i].in_use = 0;
    for (uint32_t i = 0; i < nvoices && o + 0x10 <= out->size; i++, o += 0x10) {
        const uint8_t *v = vin + i * 0x170;
        if (v[9]) update_voice(r, &r->voice[i], v, op + o);
    }
    // Effect parameters and their enabled/disabled status reply.
    const uint8_t *ein = vin + voices_sz;
    // effect output: EffectOutStatus 0x10 bytes; from REV9 on (EffectInfo version 2) 0x90 - the state plus 0x80 bytes of
    // result state. A wrong size here makes the SDK's RequestUpdateAudioRenderer fail (Echoes of Wisdom: REV11, abort).
    uint32_t eout = (r->revision >> 24) - '0' >= 9 ? 0x90 : 0x10;
    oh[5] = neffects * eout;
    for (uint32_t i = 0; i < neffects && o + eout <= out->size; i++, o += eout) {
        const uint8_t *e = ein + i * 0xC0;
        op[o] = e[2] ? 3 : 4;  // EffectState Enabled / Disabled
        if (i < r->effects) effect_update(&r->fx[i], e);
        static uint32_t seen_types;
        if (e[0] && e[0] < 32 && !(seen_types & (1u << e[0]))) {
            seen_types |= 1u << e[0];
            LOG("[audren] effect type %u used%s: slot %u mix %d enabled %u\n", e[0], e[0] == 3 || e[0] == 4 ? "" : " (not implemented)", i,
                *(const int32_t *)(e + 4), e[2]);
        }
        if (e[0] && getenv("EOWRECOMP_AUDIO_LOG")) {
            static uint8_t last[16][0xC0];
            if (i < 16 && memcmp(last[i], e, 0xC0)) {
                memcpy(last[i], e, 0xC0);
                LOG("[audren] f%llu effect %u:", (unsigned long long)r->elapsed_frames, i);
                for (int k = 0; k < 0x70; k += 4) LOG(" %08x", *(const uint32_t *)(e + k));
                LOG("\n");
            }
        }
    }
    const uint8_t *min = ein + effects_sz;
    if (r->splitters && *(const uint32_t *)min == 0x48444E53) {
        static int warned;
        if (!warned++) LOG("[audren] splitter section present (ignored)\n");
    }
    // mixes: the section ends right before the sinks
    const uint8_t *sin = ip + h[15] - perf_sz - sinks_sz;
    min = sin - mixes_sz;
    if ((r->revision >> 24) - '0' >= 7 && *(const uint32_t *)min == 0xCAFEBABE) {
        uint32_t n = *(const uint32_t *)(min + 4);
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *m = min + 0x20 + i * 0x930;
            int32_t id;
            memcpy(&id, m + 16, 4);
            update_mix(r, m, id);
        }
    } else {
        for (uint32_t i = 0; i < mixes_sz / 0x930; i++) update_mix(r, min + i * 0x930, i);
    }
    // sinks
    oh[7] = nsinks * 0x20;
    uint32_t probe_slot = UINT32_MAX, probe_device_sinks = 0, probe_raw_inputs = 0;
    for (uint32_t i = 0; i < nsinks; i++, o += 0x20) {
        const uint8_t *s = sin + i * 0x140;
        if (s[0] == 1) {  // device sink
            r->sink_used = s[1];
            uint32_t n;
            memcpy(&n, s + 0x20 + 0x100, 4);
            r->sink_inputs = n > 6 ? 6 : n;
            memcpy(r->sink_input, s + 0x124, 6);
            if (r->sink_probe.enabled) {
                probe_slot = i;
                probe_device_sinks++;
                probe_raw_inputs = n;
            }
        }
    }
    if (r->sink_probe.enabled) sink_probe_configure(r, probe_slot, probe_device_sinks, probe_raw_inputs);
    uint64_t elapsed = r->elapsed_frames;
    pthread_mutex_unlock(&r->lock);
    oh[8] = perf_sz ? 0x10 : 0;
    o += oh[8];
    oh[1] = 0xB0;  // behavior / error info
    o += 0xB0;
    oh[10] = 0x10;  // render info: elapsed frame count
    if (o + 0x10 <= out->size) *(uint64_t *)(op + o) = elapsed;
    o += 0x10;
    oh[15] = o;
    (void)resp;
}

static void renderer_open_output(Renderer *r) {
    if (getenv("EOWRECOMP_AUDIO_WAV")) {
        r->wav = fopen(getenv("EOWRECOMP_AUDIO_WAV"), "wb");
        if (r->wav) wav_header(r->wav, 0);
    }
#ifndef _WIN32
    if (getenv("EOWRECOMP_NO_AUDIO") && strcmp(getenv("EOWRECOMP_NO_AUDIO"), "0")) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { LOG("[audren] SDL audio init failed: %s\n", SDL_GetError()); return; }
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, (int)r->sample_rate};
    r->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!r->stream) { LOG("[audren] SDL audio open failed: %s\n", SDL_GetError()); return; }
    if (getenv("EOWRECOMP_AUDIO_GAIN")) SDL_SetAudioStreamGain(r->stream, (float)atof(getenv("EOWRECOMP_AUDIO_GAIN")));
    SDL_ResumeAudioStreamDevice(r->stream);
#endif
}

static void audio_renderer(Service *s, IpcReq *req, IpcResp *resp) {
    Renderer *r = s->state;
    switch (req->cmd) {
    case 0: OUT(uint32_t, 0) = r->sample_rate; resp_size(resp, 4); break;
    case 1: OUT(uint32_t, 0) = r->sample_count; resp_size(resp, 4); break;
    case 2: OUT(uint32_t, 0) = r->mix_buffers; resp_size(resp, 4); break;
    case 3: OUT(uint32_t, 0) = r->state; resp_size(resp, 4); break;
    case 4: case 10: request_update(r, req, resp); break;
    case 5: r->state = 0; break;
    case 6: r->state = 1; break;
    case 7: resp->copy_handles[resp->ncopy++] = r->event_r; break;
    case 8: break;
    case 9: OUT(uint32_t, 0) = 100; resp_size(resp, 4); break;
    default: LOG("[audren] IAudioRenderer cmd %u (ok)\n", req->cmd);
    }
}

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

static void audio_device(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: case 6: {  // ListAudioDeviceName(Auto)
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size >= 0x100) strcpy((char *)(uintptr_t)b->addr, "AudioTvOutput");
        OUT(uint32_t, 0) = 1;
        resp_size(resp, 4);
        break;
    }
    case 3: case 10: {  // GetActiveAudioDeviceName(Auto)
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size) strncpy((char *)(uintptr_t)b->addr, "AudioTvOutput", b->size);
        break;
    }
    case 4: case 11: case 12: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;
    case 5: OUT(uint32_t, 0) = 2; resp_size(resp, 4); break;  // GetActiveChannelCount
    case 8: OUT(float, 0) = 1.0f; resp_size(resp, 4); break;
    default: break;
    }
}

typedef struct AudioOut AudioOut;
static void audio_track(Renderer *r, AudioOut *a);

SERVICE("audren:u", audren_u) {
    (void)s;
    switch (req->cmd) {
    case 0: {  // OpenAudioRenderer(params, work size, aruid, pid, transfer mem, process)
        Renderer *r = calloc(1, sizeof *r);
        const uint32_t *p = (const uint32_t *)req->data;
        r->sample_rate = p[0]; r->sample_count = p[1]; r->mix_buffers = p[2]; r->submixes = p[3];
        r->voices = p[4]; r->sinks = p[5]; r->effects = p[6]; r->perf_frames = p[7];
        r->splitters = p[9];
        r->revision = p[12];
        r->state = 1;
        const char *sink_probe = getenv("EOWRECOMP_AUDIO_SINK_PROBE");
        r->sink_probe.enabled = sink_probe && !strcmp(sink_probe, "1");
        atomic_init(&r->quit, false);
        if (r->voices > 1024) r->voices = 1024;
        r->nmix = r->submixes + 1;
        pthread_mutex_init(&r->lock, NULL);
        r->voice = calloc(r->voices, sizeof(Voice));
        r->chres = calloc(r->voices, sizeof(ChanRes));
        r->chst = calloc(r->voices, sizeof(ChanState));
        r->mix = calloc(r->nmix, sizeof(Mix));
        r->fx = calloc(r->effects ? r->effects : 1, sizeof(Effect));
        renderer_open_output(r);
        r->event = kernel_create_event_pair(&r->event_w, &r->event_r);
        pthread_create(&r->thread, NULL, renderer_tick, r);
        audio_track(r, NULL);
        LOG("[audren] renderer %u Hz, %u samples/frame, %u voices, %u mixes, %u mix buffers, %u splitters, %u effects, "
            "revision %.4s (REV%d)\n", r->sample_rate, r->sample_count, r->voices, r->nmix, r->mix_buffers, r->splitters,
            r->effects, (const char *)&r->revision, (int)((r->revision >> 24) - '0'));
        ipc_return_object(resp, service_new("IAudioRenderer", audio_renderer, r));
        break;
    }
    case 1: OUT(uint64_t, 0) = 0x1000000; resp_size(resp, 8); break;  // GetWorkBufferSize: 16 MB
    case 2: case 4: ipc_return_object(resp, service_new("IAudioDevice", audio_device, NULL)); break;
    default: LOG("[audren] cmd %u (ok)\n", req->cmd);
    }
}

// ------------------------------------------------------------------ audout (buffers released in real time)
typedef struct AudioOut {
    uint32_t rate, channels, state;
    uint64_t tags[64], sizes[64], datas[64];
    SDL_AudioStream *stream;
#ifdef _WIN32
    AudioRoute output;
#endif
    FILE *wav;
    uint32_t wav_bytes;
    int queued, released_n;
    uint64_t released[64];
    uint32_t ev_r, ev_w;
    KEvent *event;
    uint64_t played;
    pthread_mutex_t lock;
    pthread_t thread;
    atomic_bool quit;  // reboot (audio_reset)
} AudioOut;

// Every renderer / audio out of this boot: a reboot stops their threads (they read guest memory and signal this
// boot's kernel events) and closes their output streams.
static Renderer *g_rens[8];
static AudioOut *g_outs[8];
static pthread_mutex_t g_track_lock = PTHREAD_MUTEX_INITIALIZER;
static void audio_track(Renderer *r, AudioOut *a) {
    pthread_mutex_lock(&g_track_lock);
    for (int i = 0; i < 8; i++) {
        if (r && !g_rens[i]) { g_rens[i] = r; r = NULL; }
        if (a && !g_outs[i]) { g_outs[i] = a; a = NULL; }
    }
    pthread_mutex_unlock(&g_track_lock);
}

static void audout_free(AudioOut *a) {
    a->quit = true;
    pthread_join(a->thread, NULL);
    if (a->stream) SDL_DestroyAudioStream(a->stream);
    if (a->wav) fclose(a->wav);
    free(a);
}

// The game closes its IAudioOut after every movie (the title's attract loop opens one per showing): stop the
// thread and the output stream then, or each showing leaves both running and fills g_outs.
static void audout_close(Service *s) {
    AudioOut *a = s->state;
    bool mine = false;
    pthread_mutex_lock(&g_track_lock);
    for (int i = 0; i < 8; i++) if (g_outs[i] == a) { g_outs[i] = NULL; mine = true; }
    pthread_mutex_unlock(&g_track_lock);
    if (!mine) return;  // (a reboot already closed it)
    s->state = NULL;
    audout_free(a);
    LOG("[audout] closed\n");
}

RT_ON_RESET(audio_reset) {
    int n = 0;
    for (int i = 0; i < 8; i++) {
        Renderer *r = g_rens[i];
        if (r) {
            r->quit = true;
            pthread_join(r->thread, NULL);
            if (r->sink_probe.enabled) sink_probe_report(&r->sink_probe);
            if (r->stream) SDL_DestroyAudioStream(r->stream);
            if (r->wav) { wav_header(r->wav, r->wav_bytes); fclose(r->wav); }
            for (uint32_t m = 0; m < r->nmix; m++) free(r->mix[m].buf);
            for (uint32_t j = 0; j < r->effects; j++) effect_release(&r->fx[j]);
            free(r->voice), free(r->chres), free(r->chst), free(r->mix), free(r->fx);
            free(r);
            g_rens[i] = NULL;
            n++;
        }
        if (g_outs[i]) { audout_free(g_outs[i]); g_outs[i] = NULL; n++; }
    }
    LOG("[audio] reboot: %d outputs closed\n", n);
}

// Buffers are copied into an SDL stream and released once the stream has drained below its target, so the
// stream plays the role of the hardware queue. Without a device, buffers are released in real time.
static int audout_queue_target(AudioOut *a, uint32_t ch) {
    if (!a->stream) return 0;
    SDL_AudioSpec dev;
    int dev_frames = 0;
    if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(a->stream), &dev, &dev_frames) || dev_frames <= 0)
        dev_frames = 1024;
#ifdef _WIN32
    else if (dev.freq > 0) dev_frames = (int)((int64_t)dev_frames * a->rate / dev.freq);
#endif
    return 2 * dev_frames * 2 * (int)ch;
}

static void *audout_thread(void *arg) {
    AudioOut *a = arg;
    uint32_t ch = a->channels ? a->channels : 2;
    int target = audout_queue_target(a, ch);
#ifdef _WIN32
    SDL_AudioSpec spec = {SDL_AUDIO_S16, (int)ch, (int)a->rate};
#endif
    while (!a->quit) {
#ifdef _WIN32
        if (refresh_output(&a->output, &a->stream, &spec, "audout")) target = audout_queue_target(a, ch);
#endif
        pthread_mutex_lock(&a->lock);
        uint64_t size = 0, tag = 0, data = 0;
        if (a->state == 0 && a->queued) { size = a->sizes[0]; tag = a->tags[0]; data = a->datas[0]; }
        pthread_mutex_unlock(&a->lock);
        if (!size) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); continue; }
        uint64_t frames = size / (2 * ch);
        if (a->stream) {
            apply_gain(a->stream);
            if (data) SDL_PutAudioStreamData(a->stream, (const void *)(uintptr_t)data, (int)size);
            while (SDL_GetAudioStreamQueued(a->stream) > target && !a->quit) {
#ifdef _WIN32
                SDL_AudioStream *before = a->stream;
                if (refresh_output(&a->output, &a->stream, &spec, "audout")) {
                    target = audout_queue_target(a, ch);
                    if (a->stream != before) break;
                }
#endif
                struct timespec ts = {0, 1000000};
                nanosleep(&ts, NULL);
            }
        } else {
            uint64_t ns = frames * 1000000000ull / (a->rate ? a->rate : 48000);
            struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
            nanosleep(&ts, NULL);
        }
        if (a->wav && data) { fwrite((const void *)(uintptr_t)data, 1, size, a->wav); a->wav_bytes += (uint32_t)size; }
        pthread_mutex_lock(&a->lock);
        if (a->queued && a->tags[0] == tag) {
            if (a->released_n < 64) a->released[a->released_n++] = a->tags[0];
            memmove(a->tags, a->tags + 1, (a->queued - 1) * 8);
            memmove(a->sizes, a->sizes + 1, (a->queued - 1) * 8);
            memmove(a->datas, a->datas + 1, (a->queued - 1) * 8);
            a->queued--;
            a->played += frames;
        }
        pthread_mutex_unlock(&a->lock);
        pthread_mutex_lock(&g_klock);
        kernel_signal(a->event);
        pthread_mutex_unlock(&g_klock);
    }
    return NULL;
}

static void audio_out(Service *s, IpcReq *req, IpcResp *resp) {
    AudioOut *a = s->state;
    if (getenv("EOWRECOMP_AUDIO_LOG") && req->cmd != 3 && req->cmd != 7) {
        IpcBuf *ob = out_buf(req, 0);
        LOG("[audout] cmd %u out %llx/%llx\n", req->cmd, ob ? (unsigned long long)ob->addr : 0ull,
            ob ? (unsigned long long)ob->size : 0ull);
    }
    switch (req->cmd) {
    case 0: OUT(uint32_t, 0) = a->state; resp_size(resp, 4); break;   // GetAudioOutState
    case 1: a->state = 0; break;                                      // Start
    case 2: a->state = 1; break;                                      // Stop
    case 3: case 7: {  // AppendAudioOutBuffer(tag) <- AudioOutBuffer {next, samples, capacity, size, offset}
        IpcBuf *b = in_buf(req, 0);
        uint64_t tag = IN(uint64_t, 0);
        uint64_t size = b && b->size >= 0x28 ? ((uint64_t *)(uintptr_t)b->addr)[3] : 0;
        if (getenv("EOWRECOMP_AUDIO_LOG") && b)
            LOG("[audout] append tag %llx buf %llx size %llx: %llx %llx %llx %llx %llx\n", (unsigned long long)tag,
                (unsigned long long)b->addr, (unsigned long long)b->size, ((unsigned long long *)(uintptr_t)b->addr)[0],
                ((unsigned long long *)(uintptr_t)b->addr)[1], ((unsigned long long *)(uintptr_t)b->addr)[2],
                ((unsigned long long *)(uintptr_t)b->addr)[3], ((unsigned long long *)(uintptr_t)b->addr)[4]);
        pthread_mutex_lock(&a->lock);
        // AudioOutBuffer {next, samples, capacity, size, offset}
        uint64_t data = b && b->size >= 0x28 ? ((uint64_t *)(uintptr_t)b->addr)[1] + ((uint64_t *)(uintptr_t)b->addr)[4] : 0;
        if (a->queued < 64) {
            a->tags[a->queued] = tag;
            a->sizes[a->queued] = size ? size : 4096;
            a->datas[a->queued] = size ? data : 0;
            a->queued++;
        }
        pthread_mutex_unlock(&a->lock);
        break;
    }
    case 4: resp->copy_handles[resp->ncopy++] = a->ev_r; break;  // RegisterBufferEvent
    case 5: case 8: {  // GetReleasedAudioOutBuffers -> tags
        IpcBuf *b = out_buf(req, 0);
        pthread_mutex_lock(&a->lock);
        int n = a->released_n;
        int max = b ? (int)(b->size / 8) : 0;
        if (n > max) n = max;
        if (b && b->size) memset((void *)(uintptr_t)b->addr, 0, b->size);  // SDK reads entry 0 even when n == 0
        for (int i = 0; i < n; i++) ((uint64_t *)(uintptr_t)b->addr)[i] = a->released[i];
        if (getenv("EOWRECOMP_AUDIO_LOG") && n) LOG("[audout] released %d (first %llx)\n", n, (unsigned long long)a->released[0]);
        memmove(a->released, a->released + n, (a->released_n - n) * 8);
        a->released_n -= n;
        pthread_mutex_unlock(&a->lock);
        OUT(uint32_t, 0) = (uint32_t)n;
        resp_size(resp, 4);
        break;
    }
    case 6: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    case 9: OUT(uint32_t, 0) = (uint32_t)a->queued; resp_size(resp, 4); break;
    case 10: OUT(uint64_t, 0) = a->played; resp_size(resp, 8); break;
    case 11: OUT(uint8_t, 0) = 1; resp_size(resp, 1); break;
    case 12: break;
    case 13: OUT(float, 0) = 1.0f; resp_size(resp, 4); break;
    default: LOG("[audout] IAudioOut cmd %u (ok)\n", req->cmd);
    }
}

SERVICE("audout:u", audout_u) {
    (void)s;
    switch (req->cmd) {
    case 0: case 2: {  // ListAudioOuts
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size >= 0x100) strcpy((char *)(uintptr_t)b->addr, "DeviceOut");
        OUT(uint32_t, 0) = 1;
        resp_size(resp, 4);
        break;
    }
    case 1: case 3: {  // OpenAudioOut(rate, channels, aruid, name) -> rate, channels, format, state + IAudioOut
        AudioOut *a = calloc(1, sizeof *a);
        a->rate = IN(uint32_t, 0) ? IN(uint32_t, 0) : 48000;
        a->channels = IN(uint16_t, 4) ? IN(uint16_t, 4) : 2;
        if (a->channels != 1 && a->channels != 2 && a->channels != 6) a->channels = 2;
        a->state = 1;
        atomic_init(&a->quit, false);
        pthread_mutex_init(&a->lock, NULL);
        a->event = kernel_create_event_pair(&a->ev_w, &a->ev_r);
        if (getenv("EOWRECOMP_AUDOUT_WAV")) a->wav = fopen(getenv("EOWRECOMP_AUDOUT_WAV"), "wb");
#ifndef _WIN32
        if (!(getenv("EOWRECOMP_NO_AUDIO") && strcmp(getenv("EOWRECOMP_NO_AUDIO"), "0")) && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            SDL_AudioSpec spec = {SDL_AUDIO_S16, (int)a->channels, (int)a->rate};
            a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
            if (a->stream) {
                if (getenv("EOWRECOMP_AUDIO_GAIN")) SDL_SetAudioStreamGain(a->stream, (float)atof(getenv("EOWRECOMP_AUDIO_GAIN")));
                SDL_ResumeAudioStreamDevice(a->stream);
            }
        }
#endif
        pthread_create(&a->thread, NULL, audout_thread, a);
        audio_track(NULL, a);
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size) strncpy((char *)(uintptr_t)b->addr, "DeviceOut", b->size);
        OUT(uint32_t, 0) = a->rate;
        OUT(uint32_t, 4) = a->channels;
        OUT(uint32_t, 8) = 2;  // PCM16
        OUT(uint32_t, 12) = 1; // stopped
        resp_size(resp, 16);
        Service *svc = service_new("IAudioOut", audio_out, a);
        svc->on_close = audout_close;
        ipc_return_object(resp, svc);
        LOG("[audout] opened %u Hz, %u ch\n", a->rate, a->channels);
        break;
    }
    default: LOG("[audout] cmd %u (ok)\n", req->cmd);
    }
}
