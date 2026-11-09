// LD_PRELOAD hook for /usr/Engine/Engine.
//
// Engine registers its QML through qRegisterResourceData. This hook swaps
// individual resource files for the ones in the active UI pack, so the UI can
// be changed without touching the read-only rootfs or the Engine binary.
// Removing the pack, or creating the safe-mode file, gives the stock UI back.
//
// Only libc is used. Static init runs before Qt is usable.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <netdb.h>
#include <pthread.h>
#include <sched.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "dsp_smooth.h"
#include "master_limiter.h"
#include "echo_out.h"
#include "pioneer_fx.h"

// The A32 trampoline installers: the AArch64 build (AZ04) never calls them.
#if defined(__aarch64__)
#define A32_ONLY __attribute__((unused))
#else
#define A32_ONLY
#endif

#define UI_ROOT "/data/sc5000-custom/ui"
#define FLAG_COMPRESSED 0x01
#define FLAG_DIRECTORY 0x02
#define FLAG_ZSTD 0x04

typedef unsigned char uchar;
typedef int (*register_fn)(int, const uchar *, const uchar *, const uchar *);
static cpu_set_t engine_cpus;
static int engine_cpus_ready;

static void initialize_engine_cpus(void) {
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online > 1) {
        CPU_ZERO(&engine_cpus);
        for (long cpu = 0; cpu < online && cpu < CPU_SETSIZE; cpu++) CPU_SET((int)cpu, &engine_cpus);
        engine_cpus_ready = 1;
    } else {
        engine_cpus_ready = sched_getaffinity(0, sizeof engine_cpus, &engine_cpus) == 0;
    }
}

static void *next_symbol(void **cache, const char *name) {
    void *fn = __atomic_load_n(cache, __ATOMIC_ACQUIRE);
    if (!fn) {
        fn = dlsym(RTLD_NEXT, name);
        if (fn) __atomic_store_n(cache, fn, __ATOMIC_RELEASE);
    }
    return fn;
}

static uint32_t be32(const uchar *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uchar *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put32(uchar *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

struct walk {
    uchar *tree;
    const uchar *names, *data;
    size_t node_size;
    char pack[128];
    int replaced;
    int has_overlay; // mods-overlay exists and mods are not quarantined: decided once per walk
#if defined(__aarch64__)
    int failed; // a file could not be placed: register the stock tree, never half a pack
#endif
};

static int is_engine(void) {
#ifdef ENGINE_UI_HOOK_TEST
    extern int hook_test_engine;
    return hook_test_engine;
#else
    static int cached = -1;
    int value = __atomic_load_n(&cached, __ATOMIC_RELAXED);
    if (value >= 0) return value;
    char exe[64];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return 0;
    exe[n] = 0;
    const char *base = strrchr(exe, '/');
    base = base ? base + 1 : exe;
    value = strcmp(base, "Engine") == 0;
    __atomic_store_n(&cached, value, __ATOMIC_RELAXED);
    return value;
#endif
}

static void *alsa_sw_cache, *alsa_tstamp_set_cache, *alsa_tstamp_get_cache;

int snd_pcm_sw_params_set_tstamp_type(void *pcm, void *params, int type) {
    int (*real)(void *, void *, int) = next_symbol(&alsa_tstamp_set_cache, "snd_pcm_sw_params_set_tstamp_type");
    if (!real) return -ENOSYS;
    // Engine already requests RAW in ALSADevice; never change an explicit monotonic clock.
    if (is_engine() && type == 0) type = 2; // GETTIMEOFDAY -> MONOTONIC_RAW
    return real(pcm, params, type);
}

int snd_pcm_sw_params(void *pcm, void *params) {
    int (*real)(void *, void *) = next_symbol(&alsa_sw_cache, "snd_pcm_sw_params");
    if (!real) return -ENOSYS;
    if (is_engine()) {
        int (*get)(const void *, int *) = next_symbol(&alsa_tstamp_get_cache, "snd_pcm_sw_params_get_tstamp_type");
        int type;
        if (get && !get(params, &type) && type == 0) {
            int rc = snd_pcm_sw_params_set_tstamp_type(pcm, params, 2);
            if (rc < 0) return rc;
        }
    }
    return real(pcm, params);
}

// #58: the toggle is master_limiter=1 in settings.conf, polled once a second
// off the audio thread; the limiter itself runs in hook_master_output.
static int lim_enabled, gain_enabled; // gain_enabled: #64 auto_gain=1
static int echo_enabled;              // #104 upfader_echo=1: Echo Out on the master bus
static int pioneer_fx_mode;           // #88 Pioneer FX emulations

static void *lim_poll(void *arg) {
    (void)arg;
    for (;;) {
        int on = 0, gain = 0, echo = 0, pioneer_fx = 0;
        FILE *f = fopen(UI_ROOT "/settings.conf", "r");
        if (f) {
            char line[64];
            while (fgets(line, sizeof line, f))
                if (!strncmp(line, "master_limiter=1", 16)) on = 1;
                else if (!strncmp(line, "auto_gain=1", 11)) gain = 1;
                else if (!strncmp(line, "upfader_echo=1", 14)) echo = 1;
                else if (!strncmp(line, "pioneer_fx=", 11)) {
                    char val[64];
                    size_t len = strcspn(line + 11, "\r\n");
                    if (len < sizeof val) {
                        memcpy(val, line + 11, len);
                        val[len] = '\0';
                        pioneer_fx = sc_pioneer_fx_parse(val);
                    }
                }
            fclose(f);
        }
        __atomic_store_n(&lim_enabled, on, __ATOMIC_RELAXED);
        __atomic_store_n(&gain_enabled, gain, __ATOMIC_RELAXED);
        __atomic_store_n(&echo_enabled, echo, __ATOMIC_RELAXED);
        __atomic_store_n(&pioneer_fx_mode, pioneer_fx, __ATOMIC_RELAXED);
        sleep(1);
    }
    return NULL;
}

static void lim_start_poll(void) {
    pthread_attr_t attr;
    struct sched_param param = {.sched_priority = 0};
    pthread_t thread;
    if (pthread_attr_init(&attr)) return;
    // Never inherit a real-time policy from whichever thread starts it.
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    pthread_attr_setschedparam(&attr, &param);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread, &attr, lim_poll, NULL);
    pthread_attr_destroy(&attr);
}

static int official_update_host(const char *node) {
    static const char host[] = "autoupdate.airmusictech.com";
    if (!node) return 0;
    size_t size = strlen(node);
    if (size && node[size - 1] == '.') size--;
    return size == sizeof host - 1 && !strncasecmp(node, host, size);
}

static void *getaddrinfo_cache;

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **result) {
    // device.inmusicbrands.com also serves OAuth/unlock; only deny the firmware-update host
    // on 32-bit decks (AZ04 uses PrimeUpdates.xml on autoupdate.airmusictech.com to download
    // its signed onboard Stems addon stems-child-process.sqsh.signed).
#if !defined(__aarch64__)
    if (is_engine() && official_update_host(node)) {
        if (result) *result = NULL;
        return EAI_NONAME;
    }
#else
    (void)official_update_host;
#endif
    int (*real)(const char *, const char *, const struct addrinfo *, struct addrinfo **) =
        next_symbol(&getaddrinfo_cache, "getaddrinfo");
    if (real) return real(node, service, hints, result);
    errno = ENOSYS;
    return EAI_SYSTEM;
}

// Edits to Engine in memory, the same as unlock_jp07_features.py makes offline, for
// a deck whose read-only root has the older binary. Each is only made if the bytes
// are as expected, so another Engine build is left alone.
struct engine_edit {
    uint32_t offset;
    const char *expected, *replacement;
    uint32_t size;
    int group; // all sites of a group are applied together or not at all
};
enum { EDIT_ONE, EDIT_STEMS, EDIT_GATEWAY };

// Everything that differs between Engine builds. The hook only patches a build
// listed here (exact GNU BuildID), and every site is checked byte for byte first.
struct engine_build {
    const char *name;
    uchar id[20];
    uint32_t dsp[21];
    const struct engine_edit *edits;
    unsigned edit_count;
    // getConfig's common return path: site, its two literal-pool words, and the
    // config fields set there (Stems integration, Bluetooth, Bluetooth send to
    // assignment, onboard Stems rendering, disableHfsPlus).
    uint32_t config_site, config_r2, config_r3;
    uint16_t config_field[5];
    // Stems feature registration: site, feature object, native true byte, enabled path, displaced instruction.
    uint32_t feature_site, feature_object, feature_true, feature_enabled, feature_displaced;
    // sqlite3_api_routines and the functions it lists ([55] exec, [70] open,
    // [135] open_v2). opendb != 0: open and open_v2 only tail-call it, hook it once.
    uint32_t sqlite_api, sqlite_exec, sqlite_open, sqlite_open_v2, sqlite_opendb;
    const char *sqlite_prefix[3]; // exec, open (or opendb), open_v2
    // #77 Config.ui.waveform.maximumZoomSecs (int): 30 on players, 10 on all-in-ones.
    // The QML map reads it at config+0x45c+96 (5.0.4) / +0x4c8+100 (5.3.0).
    uint16_t config_zoom;
};
static const struct engine_build *build;

static uint32_t stems_feature_jump[2];
static uint32_t config_jump[2];

static void *rx_code(const void *code, size_t size, size_t page) {
    void *stub = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) _exit(127);
    memcpy(stub, code, size);
    if (mprotect(stub, page, PROT_READ | PROT_EXEC)) _exit(127);
    __builtin___clear_cache(stub, (char *)stub + size);
    return stub;
}

typedef void (*dsp_event_fn)(void *, const void *);
typedef void (*dsp_process_fn)(void *, float *const *, float *const *, uint32_t);
enum {
    DSP_TURN_OFF, DSP_TURN_ON, DSP_SWITCH, DSP_CHANGE, DSP_ENABLE,
    DSP_MIX, DSP_PARAMETER, DSP_NUDGE, DSP_TEMPO, DSP_JUMP, DSP_RAMP_TIME, DSP_MOTOR_RAMP_TIME,
    DSP_SAMPLER_IMMEDIATE, DSP_SAMPLER_STOP, DSP_SAMPLER_PLAY,
    DSP_AUTOGATE, DSP_COMMON, DSP_DUB_ECHO, DSP_WASH, DSP_FLANGER, DSP_MASTER, DSP_HOOK_COUNT
};
static void *dsp_originals[DSP_HOOK_COUNT];

static void *dsp_bound_owner(void *handler) {
    // MemberFunctionHandler owns a bound object and a two-word member pointer.
    // r0 at this entry is the handler, not the bound object. Keep it intact for
    // the original dispatcher, including its virtual-function/this-adjustment path.
#if defined(__aarch64__)
    uint64_t owner; // AZ04: owner +24, adjustment +40 (x5 + (x3 >> 1) in the thunks)
    int64_t adjustment;
    memcpy(&owner, (char *)handler + 24, sizeof owner);
    memcpy(&adjustment, (char *)handler + 40, sizeof adjustment);
#else
    uint32_t owner;
    int32_t adjustment;
    memcpy(&owner, (char *)handler + 0x0c, sizeof owner);
    memcpy(&adjustment, (char *)handler + 0x14, sizeof adjustment);
#endif
    return (void *)((uintptr_t)owner + (adjustment >> 1));
}

static void dsp_transition(unsigned slot, uint32_t kind, void *handler, const void *event) {
    sc_dsp_fx_mark_transition(dsp_bound_owner(handler), kind);
    ((dsp_event_fn)dsp_originals[slot])(handler, event);
}

#define DSP_TRANSITION(name, slot, kind) \
    static void name(void *handler, const void *event) { dsp_transition(slot, kind, handler, event); }
DSP_TRANSITION(hook_fx_turn_off, DSP_TURN_OFF, SC_DSP_FX_TRANSITION_TURN_OFF)
DSP_TRANSITION(hook_fx_turn_on, DSP_TURN_ON, SC_DSP_FX_TRANSITION_TURN_ON)
DSP_TRANSITION(hook_fx_switch, DSP_SWITCH, SC_DSP_FX_TRANSITION_SWITCH)
DSP_TRANSITION(hook_fx_change, DSP_CHANGE, SC_DSP_FX_TRANSITION_CHANGE)
DSP_TRANSITION(hook_fx_enable, DSP_ENABLE, SC_DSP_FX_TRANSITION_ENABLE)
#undef DSP_TRANSITION

static void dsp_finite_event(unsigned slot, size_t offset, int is_double,
                             void *handler, const void *event) {
    if (!event) return;
    // Do not use the sanitizing sc_dsp_read_* helpers: NaN must be rejected,
    // rather than turned into a valid zero-valued control event.
    if (is_double) {
        double value;
        memcpy(&value, (const char *)event + offset, sizeof value);
        if (!sc_dsp_finite_f64(value) || (slot == DSP_TEMPO && value <= 0.0)) return;
        if ((slot == DSP_RAMP_TIME || slot == DSP_MOTOR_RAMP_TIME) && value < 0.0) return;
    } else {
        float value;
        memcpy(&value, (const char *)event + offset, sizeof value);
        if (!sc_dsp_finite_f32(value)) return;
    }
    ((dsp_event_fn)dsp_originals[slot])(handler, event);
}

static void hook_fx_mix(void *h, const void *e) { dsp_finite_event(DSP_MIX, 0x0c, 0, h, e); }
static void hook_fx_parameter(void *h, const void *e) { dsp_finite_event(DSP_PARAMETER, 0x20, 0, h, e); }
static void hook_playback_nudge(void *h, const void *e) { dsp_finite_event(DSP_NUDGE, 0x08, 1, h, e); }
static void hook_playback_tempo(void *h, const void *e) { dsp_finite_event(DSP_TEMPO, 0x08, 1, h, e); }
static void hook_time_stretch_jump(void *h, const void *e) { dsp_finite_event(DSP_JUMP, 0x08, 1, h, e); }
static void hook_ramp_time(void *h, const void *e) { dsp_finite_event(DSP_RAMP_TIME, 0x08, 1, h, e); }
static void hook_motor_ramp_time(void *h, const void *e) { dsp_finite_event(DSP_MOTOR_RAMP_TIME, 0x08, 1, h, e); }

static void dsp_sampler_event(unsigned slot, uint32_t kind, void *handler, const void *event) {
    sc_dsp_sampler_mark_event(dsp_bound_owner(handler), kind);
    ((dsp_event_fn)dsp_originals[slot])(handler, event);
}

static void hook_sampler_immediate(void *h, const void *e) {
    dsp_sampler_event(DSP_SAMPLER_IMMEDIATE, SC_DSP_SAMPLER_STOP_IMMEDIATELY, h, e);
}
static void hook_sampler_stop(void *h, const void *e) {
    dsp_sampler_event(DSP_SAMPLER_STOP, SC_DSP_SAMPLER_STOP, h, e);
}
static void hook_sampler_play(void *h, const void *e) {
    dsp_sampler_event(DSP_SAMPLER_PLAY, SC_DSP_SAMPLER_PLAY, h, e);
}

static void hook_dub_echo(void *self, float *const *in, float *const *out, uint32_t frames) {
    ((dsp_process_fn)dsp_originals[DSP_DUB_ECHO])(self, in, out, frames);
    sc_dsp_sweep_fx_post_process(self, in, frames);
}

static void hook_wash(void *self, float *const *in, float *const *out, uint32_t frames) {
    ((dsp_process_fn)dsp_originals[DSP_WASH])(self, in, out, frames);
    sc_dsp_sweep_fx_post_process(self, in, frames);
}

static void hook_autogate_process(void *self, float *const *input, float *const *output, uint32_t frames) {
    sc_dsp_autogate_pre_process(self, frames);
    ((dsp_process_fn)dsp_originals[DSP_AUTOGATE])(self, input, output, frames);
}

// Effects::Flanger::process, shared by Flanger, FlangerPositive and
// FlangerNegative (their vtables are its only references). It works in place
// on `input` and tail-calls the common chain with (input, input), so `input`
// holds the final block on return (#54).
static void hook_flanger(void *self, float *const *input, float *const *output, uint32_t frames) {
    float dry = sc_dsp_block_energy(input, frames);
    ((dsp_process_fn)dsp_originals[DSP_FLANGER])(self, input, output, frames);
    sc_dsp_makeup_apply(self, input, frames, dry);
}

// airHost's device callback (5.0.4 0x1462a6c, 5.3.0 0xc77934): it zeroes and
// then fills Engine's planar float outputs, before the resampler and the S32_LE
// conversion clip them, so this is the true master bus (#58). Every stereo pair
// gets its own linked limiter. The callback runs on the one audio thread.
typedef uint32_t (*audio_callback_fn)(void *, void *, int, const float *const *, int, float *const *, int);
static struct sc_lim master_lim[8];
static struct sc_gain master_gain[8];
#define MASTER_ECHO_PAIRS 4 // 384 KB of BSS each, untouched until Echo Out is armed
static struct sc_echo master_echo[MASTER_ECHO_PAIRS];
#define MASTER_PFX_PAIRS 4
static struct sc_pioneer_fx master_pfx[MASTER_PFX_PAIRS];
// #49: in the 4-deck layout a player's 4-channel device (two stereo pairs) leaves
// decks 3 and 4 without output buffers, and Engine skips rendering them ("Mixer
// skipped ... unassigned output buffer"): their playheads stand still. All four
// render into scratch pairs (deck n into pair n-1); the deck's role (four_deck_role)
// picks which deck each of the card's pairs (RCA A, RCA B) plays. The other decks
// show and advance but stay silent here: another player or a CDJ plays them.
#define FOUR_DECK_PAD_FRAMES 8192
static float four_deck_pad[8][FOUR_DECK_PAD_FRAMES];
static int four_deck_pad_on;
static signed char four_deck_route[2] = {0, 1}; // deck (0-3) on RCA A and RCA B, -1 silent
static uint32_t hook_master_output(void *host, void *context, int in_channels, const float *const *in,
                                   int out_channels, float *const *out, int frames) {
    audio_callback_fn orig = (audio_callback_fn)dsp_originals[DSP_MASTER];
    uint32_t rc;
    if (four_deck_pad_on && out_channels == 4 && out && frames > 0 && frames <= FOUR_DECK_PAD_FRAMES) {
        float *padded[8];
        for (int ch = 0; ch < 8; ch++) padded[ch] = four_deck_pad[ch];
        rc = orig(host, context, in_channels, in, 8, padded, frames);
        for (int ch = 0; ch < 4; ch++) {
            if (!out[ch]) continue;
            int deck = four_deck_route[ch / 2];
            if (deck < 0) memset(out[ch], 0, (size_t)frames * sizeof(float));
            else memcpy(out[ch], four_deck_pad[deck * 2 + ch % 2], (size_t)frames * sizeof(float));
        }
    } else {
        rc = orig(host, context, in_channels, in, out_channels, out, frames);
    }
    // #104 Echo Out rings the master output out after music stops; before the limiter.
    if (out && frames > 0) {
        int echo = __atomic_load_n(&echo_enabled, __ATOMIC_RELAXED);
        for (int ch = 0; ch < out_channels && ch / 2 < MASTER_ECHO_PAIRS; ch += 2)
            if (out[ch]) sc_echo_planar(&master_echo[ch / 2], out[ch], ch + 1 < out_channels ? out[ch + 1] : NULL, (unsigned)frames, echo);
    }
    // #88 Pioneer FX emulations (Spiral / Pitch Echo / Slip Roll / Dub Echo / Space)
    int pfx = __atomic_load_n(&pioneer_fx_mode, __ATOMIC_RELAXED);
    if (out && frames > 0) { // also when OFF: lets the effect reset its state
        for (int ch = 0; ch < out_channels && ch / 2 < MASTER_PFX_PAIRS; ch += 2) {
            if (out[ch]) {
                float *r = ch + 1 < out_channels ? out[ch + 1] : NULL;
                sc_pioneer_fx_planar(&master_pfx[ch / 2], out[ch], r, (unsigned)frames, pfx);
            }
        }
    }
    int gain = __atomic_load_n(&gain_enabled, __ATOMIC_RELAXED);
    // Auto-Gain (#64) always runs into the limiter, so a boost never clips.
    if ((gain || __atomic_load_n(&lim_enabled, __ATOMIC_RELAXED)) && out && frames > 0)
        for (int ch = 0; ch < out_channels && ch < 16; ch += 2) {
            float *r = ch + 1 < out_channels ? out[ch + 1] : NULL;
            if (gain) sc_gain_planar(&master_gain[ch / 2], out[ch], r, (unsigned)frames);
            sc_lim_planar(&master_lim[ch / 2], out[ch], r, (unsigned)frames);
        }
    return rc;
}

static int dsp_buffers_overlap(const float *a, const float *b, uint32_t frames) {
    if (!a || !b) return 0;
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return (uint64_t)(x > y ? x - y : y - x) < (uint64_t)frames * sizeof(float);
}

static void hook_fx_common_chain(void *common, float *const *input, float *const *output, uint32_t frames) {
    // This runs on several RR49 render threads, one common per effect. Each has
    // its own ramp slot, owned (try-acquire only, never waiting) from enter to
    // exit; no lock, no allocation, no logging. chain == NULL (table full, or the
    // same common already in flight): the block is only sanitized.
    // The ramp is SC_DSP_RAMP_FRAMES long, so only that many dry frames are ever
    // read back: at most 1 KB of audio-thread stack at any block size.
    float dry_l[SC_DSP_RAMP_FRAMES], dry_r[SC_DSP_RAMP_FRAMES];
    float *dry_pair[2] = {dry_l, dry_r};
    struct sc_dsp_chain *chain = sc_dsp_chain_enter(common);
    int32_t ramp = chain ? __atomic_load_n(&chain->ramp_remaining, __ATOMIC_RELAXED) : 0;
    uint32_t snap = ramp <= 0 ? 0 : ((uint32_t)ramp < frames ? (uint32_t)ramp : frames);
    int aliases = input && output && frames &&
        (input == output || dsp_buffers_overlap(input[0], output[0], frames) ||
         dsp_buffers_overlap(input[0], output[1], frames) ||
         dsp_buffers_overlap(input[1], output[0], frames) ||
         dsp_buffers_overlap(input[1], output[1], frames));
    int saved_dry = 0;
    if (snap && aliases && input[0] && input[1]) {
        memcpy(dry_l, input[0], (size_t)snap * sizeof(float));
        memcpy(dry_r, input[1], (size_t)snap * sizeof(float));
        saved_dry = 1;
    }
    ((dsp_process_fn)dsp_originals[DSP_COMMON])(common, input, output, frames);
    // This is the input to the common tail; upstream FX may already have changed
    // it. An aliased block with nothing saved has no dry source.
    float *const *dry = saved_dry ? dry_pair : (aliases ? NULL : input);
    uint32_t dry_valid_frames = saved_dry ? snap : (aliases ? 0 : frames);
    sc_dsp_fx_common_post_process(chain, dry, output, frames, dry_valid_frames);
    // The helper sanitizes BEFORE interpolation. Opposite finite extremes can
    // still overflow (wet - dry), so check the at-most-128 frames it may remix.
    if (output) for (unsigned ch = 0; ch < 2; ch++) {
        if (!output[ch]) continue;
        for (uint32_t i = 0; i < frames && i < SC_DSP_RAMP_FRAMES; i++) {
            if (!sc_dsp_finite_f32(output[ch][i]))
                output[ch][i] = dry && dry[ch] && i < dry_valid_frames && sc_dsp_finite_f32(dry[ch][i]) ? dry[ch][i] : 0.0f;
        }
    }
    sc_dsp_chain_exit(chain); // NULL-safe; the slot is ours from enter to here
}

#if !defined(__aarch64__)
static const uint32_t dsp_event_prefix[] = {0xe1c021d0, 0xe590c00c, 0xe52db004};
static const uint32_t dsp_autogate_prefix[] = {0xe3530000, 0xe1a02001, 0x0a000035};
static const uint32_t dsp_common_prefix[] = {0xe16d42f4, 0xe1cd60f8};
static const uint32_t dsp_sweep_prefix[] = {0xe52db004, 0xe1a02001, 0xe28db000};
static const uint32_t dsp_flanger_prefix[] = {0xe16d42f4, 0xe3530000}; // strd r4,[sp,#-36]!; cmp r3,#0
// strd r4,[sp,#-36]! then mov r5,r2 (5.0.4) or strd r10,[sp,#24] (5.3.0)
static const uint32_t dsp_master_prefix[2][2] = {{0xe16d42f4, 0xe1a05002}, {0xe16d42f4, 0xe1cda1f8}};
#endif
struct dsp_hook_site {
    uint32_t offset;
    void *hook;
};
static struct dsp_hook_site dsp_hooks[DSP_HOOK_COUNT] = { // offsets: 5.0.4, replaced by select_build
    {0xd917b8, hook_fx_turn_off}, {0xd917e4, hook_fx_turn_on},
    {0xd91894, hook_fx_switch}, {0xd918c0, hook_fx_change}, {0xd918ec, hook_fx_enable},
    {0xd91810, hook_fx_mix}, {0xd91868, hook_fx_parameter},
    {0x14fff54, hook_playback_nudge}, {0x153312c, hook_playback_tempo},
    {0x1532fa0, hook_time_stretch_jump}, {0x1500950, hook_ramp_time},
    {0xd0b5bc, hook_motor_ramp_time}, {0xc5b2e0, hook_sampler_immediate},
    {0xc5b30c, hook_sampler_stop}, {0xc5b338, hook_sampler_play},
    {0xe3a598, hook_autogate_process}, {0x15fa188, hook_fx_common_chain},
    {0xe3d0a8, hook_dub_echo}, {0xe3d908, hook_wash}, {0xe3a118, hook_flanger},
    {0x1462a6c, hook_master_output}
};

#if !defined(__aarch64__)
static const void *dsp_expected(unsigned slot, size_t *size) {
    *size = slot == DSP_COMMON ? sizeof dsp_common_prefix : sizeof dsp_event_prefix;
    if (slot == DSP_DUB_ECHO || slot == DSP_WASH) return dsp_sweep_prefix;
    if (slot == DSP_FLANGER) { *size = sizeof dsp_flanger_prefix; return dsp_flanger_prefix; }
    if (slot == DSP_MASTER) { *size = sizeof dsp_master_prefix[0]; return dsp_master_prefix[dsp_hooks[slot].offset != 0x1462a6c]; }
    return slot == DSP_COMMON ? dsp_common_prefix :
        (slot == DSP_AUTOGATE ? dsp_autogate_prefix : dsp_event_prefix);
}

static void patch_bytes(uintptr_t entry, const void *bytes, size_t size, size_t page) {
    uintptr_t start = entry - entry % page;
    size_t span = ((entry + size + page - 1) / page * page) - start;
    // Constructor only, before Engine threads start. Never continue with a
    // partially installed group or an executable page left writable.
    if (mprotect((void *)start, span, PROT_READ | PROT_WRITE)) _exit(127);
    memcpy((void *)entry, bytes, size);
    if (mprotect((void *)start, span, PROT_READ | PROT_EXEC)) _exit(127);
    __builtin___clear_cache((char *)entry, (char *)entry + size);
}

static int install_dsp_hooks(uintptr_t base, size_t page) {
    // edit_engine has already verified the main ELF BuildID. Treat these hooks
    // as one group: if ANY original prefix differs, leave ALL DSP sites alone.
    if (!base || !page) return 0;
    for (unsigned i = 0; i < DSP_HOOK_COUNT; i++) {
        size_t size;
        const void *expected = dsp_expected(i, &size);
        if (memcmp((const void *)(base + dsp_hooks[i].offset), expected, size)) return 0;
    }
    for (unsigned i = 0; i < DSP_HOOK_COUNT; i++) {
        uintptr_t entry = base + dsp_hooks[i].offset;
        if (i == DSP_AUTOGATE) {
            // cmp r3,#0; mov r2,r1; BEQ -> absolute conditional load.
            // Both loads preserve NZCV; +12 is nonzero, +0xe4 is zero frames.
            const uint32_t code[] = {0xe3530000, 0xe1a02001, 0x059ff004, 0xe51ff004,
                                    (uint32_t)(entry + 12), (uint32_t)(entry + 0xe4)};
            dsp_originals[i] = rx_code(code, sizeof code, page);
        } else {
            uint32_t code[4];
            memcpy(code, (const void *)entry, 8); // no PC-relative instructions
            code[2] = 0xe51ff004;
            code[3] = (uint32_t)(entry + 8);
            dsp_originals[i] = rx_code(code, sizeof code, page);
        }
    }
    for (unsigned i = 0; i < DSP_HOOK_COUNT; i++) {
        uint32_t jump[] = {0xe51ff004, (uint32_t)(uintptr_t)dsp_hooks[i].hook, 0xe320f000};
        patch_bytes(base + dsp_hooks[i].offset, jump, i == DSP_AUTOGATE ? 12 : 8, page);
    }
    fprintf(stderr, "engine-ui-hook: %u DSP guards/transitions installed (shared FX state)\n", DSP_HOOK_COUNT);
    return 1;
}

#else
// AZ04 (64-bit): every site is a 16-byte prologue of four position-independent
// instructions, so the displaced bytes are copied into a stub that continues at
// entry + 16. The sites that need more than a copy are rewritten below. Every
// landing site (entry + 16, the sweep body, the zero-frames tail) is in a page
// that is hooked, so it has lost PROT_BTI and `br x16` may land there.
static const uint32_t a64_event_thunk[4] = {0xd503245f, 0xa9418805, 0xf9401403, 0x9341fc64};
    // bti c; ldp x5,x2,[x0,#24]; ldr x3,[x0,#40]; asr x4,x3,#1  (MemberFunctionHandler)
static const uint32_t a64_common_prefix[4] = {0xd503233f, 0xa9ba7bfd, 0x910003fd, 0xa90153f3};
static const uint32_t a64_flanger_prefix[4] = {0xd503233f, 0xa9b87bfd, 0x910003fd, 0xa90153f3};

static uintptr_t a64_branch_target(uintptr_t at);
static void *function_trampoline(uintptr_t entry, size_t page);
static void redirect_function(uintptr_t entry, const void *handler, size_t page);

// The event guards (Mix, Parameter, Nudge, Tempo, Jump, RampTime, MotorRampTime) read
// fields of the event at 32-bit layout offsets; the AZ04 event layouts are not
// verified (std::string and pointer members move them), so those sites stay stock.
static int dsp_site_active(unsigned slot) { return slot < DSP_MIX || slot > DSP_MOTOR_RAMP_TIME; }

// Builds the stub that runs the displaced prologue of slot i, or NULL if the
// bytes at its site are not the verified ones.
static void *a64_dsp_original(unsigned i, uintptr_t entry, size_t page) {
    const uint32_t *p = (const uint32_t *)entry;
    uint32_t code[12];
    if (i == DSP_COMMON || i == DSP_FLANGER) {
        if (memcmp(p, i == DSP_COMMON ? a64_common_prefix : a64_flanger_prefix, 16)) return NULL;
        return function_trampoline(entry, page);
    }
    if (i == DSP_AUTOGATE) {
        // bti c; mov x2,x1; cbz x3,<zero frames>; ldp x7,x6,[x1]. The cbz is PC-relative:
        // rebuild it as cbnz over an absolute jump to its original target (in place, past the patch).
        if (p[0] != 0xd503245f || p[1] != 0xaa0103e2 || (p[2] & 0xff00001f) != 0xb4000003 || p[3] != 0xa9401827) return NULL;
        int32_t imm = (int32_t)(p[2] << 8) >> 13;
        uint64_t zero = entry + 8 + (int64_t)imm * 4, back = entry + 16;
        if (zero < entry + 16) return NULL;
        code[0] = p[1];
        code[1] = 0xb5000063; // cbnz x3, +12
        code[2] = 0x580000d0; // ldr x16, [pc, #24]
        code[3] = 0xd61f0200; // br x16
        code[4] = p[3];
        code[5] = 0x580000b0; // ldr x16, [pc, #20]
        code[6] = 0xd61f0200;
        code[7] = 0xd503201f;
        memcpy(&code[8], &zero, 8);
        memcpy(&code[10], &back, 8);
        return rx_code(code, sizeof code, page);
    }
    if (i == DSP_DUB_ECHO || i == DSP_WASH) {
        // bti c; mov x2,x1; add x0,x0,#imm; b <shared sweep body>
        uintptr_t to = p[0] == 0xd503245f && p[1] == 0xaa0103e2 && (p[2] & 0xffc003ff) == 0x91000000 ?
            a64_branch_target(entry + 12) : 0;
        if (!to) return NULL;
        uint64_t jump = to;
        code[0] = p[1];
        code[1] = p[2];
        code[2] = 0x58000050; // ldr x16, [pc, #8]
        code[3] = 0xd61f0200;
        memcpy(&code[4], &jump, 8);
        return rx_code(code, 6 * sizeof *code, page);
    }
    if (i == DSP_MASTER) {
        // paciasp; sub sp,sp,#0x100; adrp x7,<GOT page>; ldr x7,[x7,#2144]: the adrp is
        // PC-relative, so the stub loads that GOT slot's absolute address instead.
        if (p[0] != 0xd503233f || p[1] != 0xd10403ff || (p[2] & 0x9f00001f) != 0x90000007 || p[3] != 0xf94430e7) return NULL;
        uint64_t lo = ((p[2] >> 5) & 0x7ffff) << 2 | ((p[2] >> 29) & 3);
        int64_t imm = (int64_t)(lo << 43) >> 43;
        uint64_t got = (((entry + 8) & ~(uintptr_t)0xfff) + ((uint64_t)imm << 12)) + 2144, back = entry + 16;
        code[0] = p[0];
        code[1] = p[1];
        code[2] = 0x58000087; // ldr x7, [pc, #16]
        code[3] = 0xf94000e7; // ldr x7, [x7]
        code[4] = 0x58000090; // ldr x16, [pc, #16]
        code[5] = 0xd61f0200;
        memcpy(&code[6], &got, 8);
        memcpy(&code[8], &back, 8);
        return rx_code(code, sizeof code, page);
    }
    if (memcmp(p, a64_event_thunk, 16)) return NULL;
    return function_trampoline(entry, page);
}

static int install_dsp_hooks(uintptr_t base, size_t page) {
    // edit_engine has already verified the main ELF BuildID. As on 32-bit, the hooks are
    // one group: if ANY original prologue differs, leave ALL DSP sites alone.
    if (!base || !page) return 0;
    for (unsigned i = 0; i < DSP_HOOK_COUNT; i++) {
        if (!dsp_site_active(i)) continue;
        void *original = a64_dsp_original(i, base + dsp_hooks[i].offset, page);
        if (!original) {
            for (unsigned j = 0; j < i; j++) dsp_originals[j] = NULL; // stubs are unreachable: nothing is patched yet
            return 0;
        }
        dsp_originals[i] = original;
    }
    unsigned installed = 0;
    for (unsigned i = 0; i < DSP_HOOK_COUNT; i++)
        if (dsp_site_active(i)) {
            redirect_function(base + dsp_hooks[i].offset, dsp_hooks[i].hook, page);
            installed++;
        }
    fprintf(stderr, "engine-ui-hook: %u DSP guards/transitions installed (shared FX state)\n", installed);
    return 1;
}
#endif

// #49: four_deck_waveform=1 in settings.conf unlocks Engine's own 4-deck layout
// (Layout -> Decks / Waveforms) on a player. Engine offers it only to a non-player
// with 4 decks (5.3.0 0x1c44110); 4 decks alone crash a player's layout controller.
// The sound card keeps its 2 deck outputs (PlanckClient caps decks at 12, not at
// outputs), and engine-ui-guard names the hardware's deck group for this layout.
static const char *four_deck_settings = UI_ROOT "/settings.conf";
static const char *product_code_path = "/sys/firmware/devicetree/base/inmusic,product-code";

// The standalone players (SC5000, SC5000M, SC6000, SC6000M).
// JP11 / JP11S (PRIME GO / GO+) stay off this list. They are 2-deck consoles.
// An LC6000 assignment binds only as a secondary controller on deck 3 or 4,
// and this hook does not create those decks on an all-in-one. See
// docs/LC6000-PRIME-GO-PLUS.md.
static int player_board(void) {
    char code[8] = "";
    FILE *f = fopen(product_code_path, "r");
    if (f) {
        if (!fgets(code, sizeof code, f)) code[0] = 0;
        fclose(f);
    }
    return !strncmp(code, "JP07", 4) || !strncmp(code, "JP08", 4) || !strncmp(code, "JP13", 4) || !strncmp(code, "JP14", 4);
}
static int four_deck_layout(void) {
    int v = 0;
    FILE *f = fopen(four_deck_settings, "r");
    if (f) {
        char line[64];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "four_deck_waveform=", 19)) v = atoi(line + 19);
        fclose(f);
    }
    return v;
}

// four_deck_role: the decks this player plays on RCA A / RCA B: 13, 24, 12, or a
// single deck 1 / 2 with RCA B silent. auto (default) is what engine-ui-guard resolved
// from the Engine player number before this start; it also maps the hardware.
static const char *four_deck_resolved = "/run/sc5000-assignments/role";
static void read_role(const char *path, const char *key, char *role, size_t size) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[64];
    size_t n = strlen(key);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, n)) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(role, size, "%s", line + n);
        }
    fclose(f);
}

static void four_deck_role(void) {
    char role[8] = "auto";
    read_role(four_deck_settings, "four_deck_role=", role, sizeof role);
    if (!strcmp(role, "auto")) {
        snprintf(role, sizeof role, "13");
        read_role(four_deck_resolved, "", role, sizeof role);
    }
    static const struct { const char *name; signed char a, b; } roles[] = {
        {"12", 0, 1}, {"13", 0, 2}, {"24", 1, 3}, {"34", 2, 3}, {"1", 0, -1}, {"2", 1, -1}};
    four_deck_route[0] = 0, four_deck_route[1] = 2;
    for (size_t i = 0; i < sizeof roles / sizeof *roles; i++)
        if (!strcmp(role, roles[i].name)) four_deck_route[0] = roles[i].a, four_deck_route[1] = roles[i].b;
}

A32_ONLY static void prepare_config(uintptr_t base, size_t page) {
    // getConfig's common return path runs after every model's constructor.
    uint32_t code[] = {
        0xe3a02001, // mov r2, #1
        0xe5c020db, // hasStems integration
        0xe5c0224c, // hasBluetooth
        0xe5c0224e, // hasBluetoothSendToAssignment
        0xe5c02276, // remote Stems processor support
        0xe320f000, // #152 (5.3.0+): Config.audio.hasStemsLevel ([r0, #0x54] = 1)
        0xe3a02000, // mov r2, #0
        0xe5c02289, // disableHfsPlus
        0xe320f000, // #49 when enabled: ldrb r2, [r0, #0x239] (isPlayer)
        0xe320f000, // cmp r2, #0: all-in-ones (not players) keep their config
        0xe320f000, // movne r2, #4
        0xe320f000, // strne r2, [r0, #0x90] (numberOfDecks)
        0xe320f000, // movne r2, #0
        0xe320f000, // strbne r2, [r0, #0x239]
        0xe320f000, // #77: mov r2, #30
        0xe320f000, // #77: str r2, [r0, #maximumZoomSecs]
        0xe59f2004, // replay the two displaced literal loads
        0xe59f3004,
        0xe59ff004,
        *(const uint32_t *)(base + build->config_r2),
        *(const uint32_t *)(base + build->config_r3),
        (uint32_t)(base + build->config_site + 8),
    };
    const unsigned at[] = {1, 2, 3, 4, 7};
    for (unsigned i = 0; i < 5; i++) code[at[i]] = (code[at[i]] & ~0xfffu) | build->config_field[i];
    if (build->config_zoom == 0x52c) {
        // #152: unlock native 3-band Stems Level EQ (ChannelStrip) on all 5.3.0 models.
        code[5] = 0xe5c02054;
    }
    if (build->config_zoom) {
        // #77: the players' 30 s zoom-out limit on every model (same SoC; Engine
        // sets 10 s on the all-in-ones). No flags touched; never above 30 in 32-bit.
        code[14] = 0xe3a0201e;
        code[15] = 0xe5802000 | build->config_zoom;
    }
    if (!strcmp(build->name, "5.3.0") && four_deck_layout() > 0) {
        const uint32_t unlock[] = {0xe5d02239, 0xe3520000, 0x13a02004, 0x15802090, 0x13a02000, 0x15c02239};
        memcpy(code + 8, unlock, sizeof unlock);
        four_deck_pad_on = player_board();
        four_deck_role();
        fprintf(stderr, "engine-ui-hook: 4-deck layout unlocked (#49)\n");
    }
    config_jump[0] = 0xe51ff004;
    config_jump[1] = (uint32_t)(uintptr_t)rx_code(code, sizeof code, page);
}

A32_ONLY static void prepare_stems_feature(uintptr_t base, size_t page) {
    // The old 0x8429b0 exit misses models configured False; intercept before that split.
    uint32_t code[] = {
        0xe59f3018, // ldr r3, [pc, #24] (Stems feature object)
        0xe1570003, // cmp r7, r3
        0x059f3014, // ldreq r3, [pc, #20] (native true byte)
        0x05873004, // streq r3, [r7, #4]
        0x059ff010, // ldreq pc, [pc, #16] (native enabled-feature registration)
        build->feature_displaced ? build->feature_displaced : 0xe51b3340, // displaced instruction
        0xe3540000, // cmp r4, #0 (displaced flags)
        0xe59ff008, // ldr pc, [pc, #8] (original store/branch)
        (uint32_t)(base + build->feature_object),
        (uint32_t)(base + build->feature_true),
        (uint32_t)(base + build->feature_enabled),
        (uint32_t)(base + build->feature_site + 8),
    };
    stems_feature_jump[0] = 0xe51ff004; // ldr pc, [pc, #-4]
    stems_feature_jump[1] = (uint32_t)(uintptr_t)rx_code(code, sizeof code, page);
}

#if defined(__aarch64__)
// 5.3.0 AZ04 (PRIME 4 G2 JP24, SYSTEM ONE RMZ2; BuildID 04033591...): a 64-bit
// Engine, so only data and single-instruction edits. The TidalAPI URLs are the
// same encrypted bytes as 5.3.0, split into a 16-byte q and an 8-byte d literal
// (each read once, at 0x4ecd44/0x4ecd4c and 0x4ecddc/0x4ecde4), and the auth
// suffix "1/" is the mov w0 immediate. "aac" has one xref (0x1241cc0), next to
// FLAC/flac/mp4 as in 5.3.0. The A32 trampolines (config, Stems feature) are not
// ported: they would write A32 code into an AArch64 Engine; the SQLite hook and
// the DSP guards have AArch64 forms (function_trampoline, install_dsp_hooks).
static const struct engine_edit edits_az04[] = {
    {0x1d40f28u, "aac", "mp3", 4, EDIT_ONE},
    // TidalAPI base URL -> stream-sync /v1/
    {0x1dbff30u, "\x4c\x51\x52\x57\x5b\x13\x05\x04\x4d\x5d\x47\x01\x44\x58\x56\x52",
        "\x4c\x51\x52\x57\x12\x06\x05\x1a\x1e\x1a\x00\x1f\x1e\x01\x1c\x02", 16, EDIT_GATEWAY},
    {0x1dc0ee0u, "\x58\x1b\x55\x58\x55\x16\x4c\x0a", "\x0e\x0d\x06\x0f\x08\x16\x4c\x0a", 8, EDIT_GATEWAY},
    // TidalAPI auth URL -> stream-sync /au//
    {0x1dbff40u, "\x3f\x2c\x2d\x2a\x28\x66\x72\x71\x3e\x15\x15\x0a\x4d\x10\x0c\x02",
        "\x3f\x2c\x2d\x2a\x61\x73\x72\x6f\x6d\x57\x4f\x52\x4d\x54\x4b\x57", 16, EDIT_GATEWAY},
    {0x1dc0ee8u, "\x06\x04\x47\x09\x04\x01\x42\x18", "\x5d\x50\x59\x52\x5b\x43\x0c\x1b", 8, EDIT_GATEWAY},
    // TidalAPI auth URL suffix 1/ -> //: mov w0, #0x5f5e -> mov w0, #0x5f40
    {0x4ecde0u, "\xc0\xeb\x8b\x52", "\x00\xe8\x8b\x52", 4, EDIT_GATEWAY},
    // Native AppleMusicProvider (0x932f8c) is gated at 0x9334ac by tbz w0, #0, 0x933648: nop it.
    {0x9334acu, "\xe0\x0c\x00\x36", "\x1f\x20\x03\xd5", 4, EDIT_ONE},
    // unknown filesystem -> supported (mov w3, #0 -> #4)
    {0x1a17c64u, "\x03\x00\x80\x52", "\x83\x00\x80\x52", 4, EDIT_ONE},
    // isSupportedFilesystem always true
    {0x1a17db8u, "\xf3\x07\x9f\x1a", "\x33\x00\x80\x52", 4, EDIT_ONE},
    // hfsplus not rewritten to ufsd
    {0x1717e6cu, "\x00\x36\x00\x54", "\x1f\x20\x03\xd5", 4, EDIT_ONE},
    // read-only volume not auto-ejected
    {0x752218u, "\x02\xa0\x4b\x39", "\x02\x00\x80\x52", 4, EDIT_ONE},
    // DeviceManager::isReadOnly cache -> false
    {0x1719718u, "\x60\xe2\x40\x39", "\x00\x00\x80\x52", 4, EDIT_ONE},
    // DeviceManager::isReadOnly QFileInfo -> false
    {0x17196b8u, "\x60\x02\x00\x52", "\x00\x00\x80\x52", 4, EDIT_ONE},
    // Config.audio.hasLimiter (strb w20, [x19, #48])
    {0x15f4090u, "\x74\xc2\x00\x39", "\x1f\x20\x03\xd5", 4, EDIT_ONE},
    // ShowStemsList gate: tbz -> b
    {0x8732ccu, "\x00\x01\x00\x36", "\x08\x00\x00\x14", 4, EDIT_ONE},
    // #77 all-in-one maximumZoomSecs 10 -> 30
    {0x15f4148u, "\x42\x01\x80\x52", "\xc2\x03\x80\x52", 4, EDIT_ONE},
    // #159 StreamingProviderInterface::connectedSSO() -> true, isDisconnectedFromProfile() -> false
    {0xc9cab4u, "\x00\x58\x64\x39", "\x20\x00\x80\x52", 4, EDIT_ONE},
    {0xc9cb0cu, "\xe0\x03\x01\x2a", "\x00\x00\x80\x52", 4, EDIT_ONE},
};

static const struct engine_build engine_builds[] = {
    {.name = "5.3.0-az04",
     .id = "\x04\x03\x35\x91\x8d\xb1\x14\x4f\x0c\x34\x52\xd6\xf2\x30\x88\xd3\xc3\xa9\x46\x13",
     // 21 DSP sites (RTTI-verified, systemone-analysis/dsp-map.json); same order as the 5.3.0 table.
     .dsp = {0xf22110, 0xf22140, 0xf221e8, 0xf22210, 0xf22240, 0xf22168, 0xf221c0, 0x17a2348, 0x17a46e0,
             0x17a4560, 0x17a2cd0, 0xead030, 0xe0d5a0, 0xe0d5c8, 0xe0d5f0, 0xff5c60, 0x1747320, 0xfdc6e0,
             0xff08c0, 0xfda420, 0xd30e68},
     .edits = edits_az04, .edit_count = sizeof edits_az04 / sizeof *edits_az04,
     // sqlite3_open/open_v2 only tail-call openDatabase: hook that. Its 16-byte
     // prologue has no PC-relative instruction, so it is copied as is.
     .sqlite_api = 0x2db6b68, .sqlite_exec = 0x1bb2f48, .sqlite_open = 0x1b8b7a0,
     .sqlite_open_v2 = 0x1bbd5c0, .sqlite_opendb = 0x1b8b060,
     .sqlite_prefix = {"", "\x3f\x23\x03\xd5\xff\x43\x02\xd1\xfd\x7b\x03\xa9\xfd\xc3\x00\x91", ""}},
};
#else
static const struct engine_edit edits_504[] = {
    // Stems remote processor: exact Engine build from the verified patch manifest.
    {0x85ef9cu, "\xbc\x26\x9f\xe5\xbc\x36\x9f\xe5\xf4\x42\x6d\xe1\xf8\xa1\xcd\xe1\xf8\x60\xcd\xe1\xf0\x81\xcd\xe1\x20\xe0\x8d\xe5\x20\xb0\x8d\xe2\xe4\xd0\x4d\xe2\x02\x20\x8f\xe0\x3c\x10\x91\xe5\x00\x01\x0b\xe5\x94\x46\x9f\xe5\x03\x30\x92\xe7\x00\x00\x51\xe3\x04\x40\x8f",
        "\x10\x40\x2d\xe9\x00\x40\xa0\xe1\x1e\x10\xa0\xe3\x0c\x20\x8f\xe2\x04\xd5\xe3\xeb\x04\x00\xa0\xe1\x10\x80\xbd\xe8\x00\xf0\x20\xe3\x2f\x75\x73\x72\x2f\x6c\x6f\x63\x61\x6c\x2f\x62\x69\x6e\x2f\x73\x74\x65\x6d\x73\x2d\x70\x72\x6f\x63\x65\x73\x73\x6f\x72\x00", 63, EDIT_STEMS},
    {0x85e5f8u, "\x90\xc7\x9f\xe5",
        "\x1e\xff\x2f\xe1", 4, EDIT_STEMS},
    {0xf6303cu, "\xe0\x20\x9f\xe5\xd8\x30\x9f\xe5", (const char *)config_jump, 8, EDIT_STEMS},
    // TIDAL's client names a downloaded file after its codec and knows FLAC and AAC,
    // so an MP3 from the gateway would be saved with no extension and fail to load.
    // Its one "aac" string is both what it compares the codec with and the extension
    // it then uses. TIDAL's own AAC is labelled mp4a.40.x and keeps working.
    {0x1DB8E90u, "aac", "mp3", 4, EDIT_ONE},
    // Retain the pre-existing +0x24 capability patch.
    {0xF659B8u, "\x24\x30\xc4\xe5", "\x00\xf0\x20\xe3", 4, EDIT_ONE},
    {0x842644u, "\x40\x33\x1b\xe5\x00\x00\x54\xe3", (const char *)stems_feature_jump, 8, EDIT_ONE},
    // Preserve ShowStemsList eligibility in r8 across the extra device feature gate.
    {0x6D3E00u, "\x07\x00\x00\x0a", "\x07\x00\x00\xea", 4, EDIT_ONE},
    // unknown filesystem -> supported
    {0x1897e70u, "\x00\x60\xa0\xe3", "\x04\x60\xa0\xe3", 4, EDIT_ONE},
    // isSupportedFilesystem always true
    {0x1897dccu, "\x00\x40\xa0\x03", "\x01\x40\xa0\x03", 4, EDIT_ONE},
    // hfsplus not rewritten to ufsd
    {0x1678644u, "\xd1\x01\x00\x1a", "\x00\xf0\x20\xe3", 4, EDIT_ONE},
    // TidalAPI base URL -> stream-sync /v1/
    {0x336740u, "\x69\x75\x75\x71\x74\x3b\x30\x30\x62\x71\x6a\x2f\x75\x6a\x65\x62\x6d\x2f\x64\x70\x6e\x30\x77\x32", "\x69\x75\x75\x71\x3b\x30\x30\x32\x33\x38\x2f\x31\x2f\x31\x2f\x32\x3b\x39\x31\x39\x31\x30\x77\x32", 24, EDIT_GATEWAY},
    // TidalAPI auth URL -> stream-sync /au//
    {0x336758u, "\x6f\x7b\x7b\x77\x7a\x41\x36\x36\x68\x7c\x7b\x6f\x35\x7b\x70\x6b\x68\x73\x35\x6a\x76\x74\x36\x7d", "\x6f\x7b\x7b\x77\x41\x36\x36\x38\x39\x3e\x35\x37\x35\x37\x35\x38\x41\x3f\x37\x3f\x37\x36\x68\x7c", 24, EDIT_GATEWAY},
    // TidalAPI auth URL suffix 1/ -> //
    {0x33652cu, "\x38\x36\x03\xe3", "\x36\x36\x03\xe3", 4, EDIT_GATEWAY},
    // read-only volume not auto-ejected
    {0x58af34u, "\x30\x32\xd0\xe5", "\x00\x30\xa0\xe3", 4, EDIT_ONE},
    // DeviceManager::isReadOnly cache -> false
    {0x167295cu, "\x1c\x00\xd4\xe5", "\x00\x00\xa0\xe3", 4, EDIT_ONE},
    // DeviceManager::isReadOnly QFileInfo -> false
    {0x16728c0u, "\x01\x00\x24\xe2", "\x00\x00\xa0\xe3", 4, EDIT_ONE},
};

// 5.3.0 (BuildID be537e90...): sites found by RTTI/vtable slots (DSP), the QML
// property map (config fields), sqlite3_api_routines, and masked-context matches,
// each checked against the 5.0.4 disassembly.
static const struct engine_edit edits_530[] = {
    {0x8bf744u, "\xbc\x26\x9f\xe5\xbc\x36\x9f\xe5\xf4\x42\x6d\xe1\xf8\xa1\xcd\xe1\xf8\x60\xcd\xe1\xf0\x81\xcd\xe1\x20\xe0\x8d\xe5\x20\xb0\x8d\xe2\xe4\xd0\x4d\xe2\x02\x20\x8f\xe0\x3c\x10\x91\xe5\x00\x01\x0b\xe5\x94\x46\x9f\xe5\x03\x30\x92\xe7\x00\x00\x51\xe3\x04\x40\x8f",
        // As 5.0.4, with the BL re-encoded for QString::fromUtf8(QByteArrayView)@plt at 0x15f768.
        "\x10\x40\x2d\xe9\x00\x40\xa0\xe1\x1e\x10\xa0\xe3\x0c\x20\x8f\xe2\x03\x80\xe2\xeb\x04\x00\xa0\xe1\x10\x80\xbd\xe8\x00\xf0\x20\xe3\x2f\x75\x73\x72\x2f\x6c\x6f\x63\x61\x6c\x2f\x62\x69\x6e\x2f\x73\x74\x65\x6d\x73\x2d\x70\x72\x6f\x63\x65\x73\x73\x6f\x72\x00", 63, EDIT_STEMS},
    {0x8be39cu, "\x8c\xc7\x9f\xe5", "\x1e\xff\x2f\xe1", 4, EDIT_STEMS},
    {0x1537148u, "\xe4\x20\x9f\xe5\xdc\x30\x9f\xe5", (const char *)config_jump, 8, EDIT_STEMS},
    {0x1efc6bcu, "aac", "mp3", 4, EDIT_ONE},
    // Config.audio.hasLimiter (root +0x24), as the 5.0.4 +0x24 patch.
    {0x153DDB0u, "\x24\x30\xc4\xe5", "\x00\xf0\x20\xe3", 4, EDIT_ONE},
    {0x8a1eacu, "\x40\x33\x1b\xe5\x00\x00\x54\xe3", (const char *)stems_feature_jump, 8, EDIT_ONE},
    // ShowStemsList gate (eligibility is in r7 here, r8 in 5.0.4).
    {0x7398FCu, "\x07\x00\x00\x0a", "\x07\x00\x00\xea", 4, EDIT_ONE},
    {0x1a28ea8u, "\x00\x60\xa0\xe3", "\x04\x60\xa0\xe3", 4, EDIT_ONE},
    {0x1a28e04u, "\x00\x40\xa0\x03", "\x01\x40\xa0\x03", 4, EDIT_ONE},
    {0x167c6f8u, "\xfa\x01\x00\x1a", "\x00\xf0\x20\xe3", 4, EDIT_ONE},
    // TidalAPI base URL -> stream-sync /v1/ (5.3.0 XORs byte i with 0x24 + i)
    {0x3155d0u, "\x4c\x51\x52\x57\x5b\x13\x05\x04\x4d\x5d\x47\x01\x44\x58\x56\x52\x58\x1b\x55\x58\x55\x16\x4c\x0a", "\x4c\x51\x52\x57\x12\x06\x05\x1a\x1e\x1a\x00\x1f\x1e\x01\x1c\x02\x0e\x0d\x06\x0f\x08\x16\x4c\x0a", 24, EDIT_GATEWAY},
    // TidalAPI auth URL -> stream-sync /au// (5.3.0 XORs byte i with 0x57 + i)
    {0x3155e8u, "\x3f\x2c\x2d\x2a\x28\x66\x72\x71\x3e\x15\x15\x0a\x4d\x10\x0c\x02\x06\x04\x47\x09\x04\x01\x42\x18", "\x3f\x2c\x2d\x2a\x61\x73\x72\x6f\x6d\x57\x4f\x52\x4d\x54\x4b\x57\x5d\x50\x59\x52\x5b\x43\x0c\x1b", 24, EDIT_GATEWAY},
    // TidalAPI auth URL suffix 1/ -> //
    {0x3153acu, "\x5e\x3f\x05\xe3", "\x40\x3f\x05\xe3", 4, EDIT_GATEWAY},
    // The config field moved from 560 to 568.
    {0x5e28ccu, "\x38\x32\xd0\xe5", "\x00\x30\xa0\xe3", 4, EDIT_ONE},
    {0x167e32cu, "\x1c\x00\xd4\xe5", "\x00\x00\xa0\xe3", 4, EDIT_ONE},
    {0x167e290u, "\x01\x00\x24\xe2", "\x00\x00\xa0\xe3", 4, EDIT_ONE},
    // #159 StreamingProviderInterface::connectedSSO() -> true, isDisconnectedFromProfile() -> false
    {0xbf59bcu, "\xc2\x05\xd0\xe5", "\x01\x00\xa0\xe3", 4, EDIT_ONE},
    {0xbf5a18u, "\x01\x00\x20\xe2", "\x00\x00\xa0\xe3", 4, EDIT_ONE},
};

static const struct engine_edit edits_sclive[] = {
    {0x8ba000u, "\xc0\x26\x9f\xe5\xc0\x36\x9f\xe5\xf4\x42\x6d\xe1\xf8\xa1\xcd\xe1\xf8\x60\xcd\xe1\xf0\x81\xcd\xe1\x20\xe0\x8d\xe5\x20\xb0\x8d\xe2\xe4\xd0\x4d\xe2\x02\x20\x8f\xe0\x3c\x10\x91\xe5\x00\x01\x0b\xe5\x98\x46\x9f\xe5\x03\x30\x92\xe7\x00\x00\x51\xe3\x04\x40\x8f",
        "\x10\x40\x2d\xe9\x00\x40\xa0\xe1\x1e\x10\xa0\xe3\x0c\x20\x8f\xe2\x47\x9c\xe2\xeb\x04\x00\xa0\xe1\x10\x80\xbd\xe8\x00\xf0\x20\xe3\x2f\x75\x73\x72\x2f\x6c\x6f\x63\x61\x6c\x2f\x62\x69\x6e\x2f\x73\x74\x65\x6d\x73\x2d\x70\x72\x6f\x63\x65\x73\x73\x6f\x72\x00", 63, EDIT_STEMS},
    {0x8bbaa4u, "\x8c\xc7\x9f\xe5", "\x1e\xff\x2f\xe1", 4, EDIT_STEMS},
    {0x159f4a8u, "\xe4\x20\x9f\xe5\xdc\x30\x9f\xe5", (const char *)config_jump, 8, EDIT_STEMS},
    {0x1f69520u, "aac", "mp3", 4, EDIT_ONE},
    {0x15b6fb0u, "\x24\x30\xc4\xe5", "\x00\xf0\x20\xe3", 4, EDIT_ONE},
    {0x89cd64u, "\x38\x33\x1b\xe5\x00\x00\x54\xe3", (const char *)stems_feature_jump, 8, EDIT_ONE},
    {0x72c284u, "\x07\x00\x00\x0a", "\x07\x00\x00\xea", 4, EDIT_ONE},
    {0x1a8dbc0u, "\x00\x60\xa0\xe3", "\x04\x60\xa0\xe3", 4, EDIT_ONE},
    {0x1a8db1cu, "\x00\x40\xa0\x03", "\x01\x40\xa0\x03", 4, EDIT_ONE},
    {0x16d2208u, "\xfa\x01\x00\x1a", "\x00\xf0\x20\xe3", 4, EDIT_ONE},
    {0x31a880u, "\x4c\x51\x52\x57\x5b\x13\x05\x04\x4d\x5d\x47\x01\x44\x58\x56\x52\x58\x1b\x55\x58\x55\x16\x4c\x0a", "\x4c\x51\x52\x57\x12\x06\x05\x1a\x1e\x1a\x00\x1f\x1e\x01\x1c\x02\x0e\x0d\x06\x0f\x08\x16\x4c\x0a", 24, EDIT_GATEWAY},
    {0x31a898u, "\x3f\x2c\x2d\x2a\x28\x66\x72\x71\x3e\x15\x15\x0a\x4d\x10\x0c\x02\x06\x04\x47\x09\x04\x01\x42\x18", "\x3f\x2c\x2d\x2a\x61\x73\x72\x6f\x6d\x57\x4f\x52\x4d\x54\x4b\x57\x5d\x50\x59\x52\x5b\x43\x0c\x1b", 24, EDIT_GATEWAY},
    {0x31a65cu, "\x5e\x3f\x05\xe3", "\x40\x3f\x05\xe3", 4, EDIT_GATEWAY},
    {0x5d5418u, "\x38\x32\xd0\xe5", "\x00\x30\xa0\xe3", 4, EDIT_ONE},
    {0x16d3e3cu, "\x1c\x00\xd4\xe5", "\x00\x00\xa0\xe3", 4, EDIT_ONE},
    {0x16d3da0u, "\x01\x00\x24\xe2", "\x00\x00\xa0\xe3", 4, EDIT_ONE},
    // Native AppleMusicProvider gate: beq 0x7f13dc -> nop (AZ04: 0x9334ac)
    {0x7f11b4u, "\x88\x00\x00\x0a", "\x00\xf0\x20\xe3", 4, EDIT_ONE},
    // #159 StreamingProviderInterface::connectedSSO() -> true, isDisconnectedFromProfile() -> false
    {0xbf91d8u, "\xc2\x05\xd0\xe5", "\x01\x00\xa0\xe3", 4, EDIT_ONE},
    {0xbf9234u, "\x01\x00\x20\xe2", "\x00\x00\xa0\xe3", 4, EDIT_ONE},
};

static const struct engine_build engine_builds[] = {
    {"5.0.4", "\xaf\x07\x38\x65\xa3\x31\x8d\x80\x7a\x83\x03\xf7\x16\x15\x2e\xac\x92\x3a\x4c\x6f",
     {0xd917b8, 0xd917e4, 0xd91894, 0xd918c0, 0xd918ec, 0xd91810, 0xd91868, 0x14fff54, 0x153312c,
      0x1532fa0, 0x1500950, 0xd0b5bc, 0xc5b2e0, 0xc5b30c, 0xc5b338, 0xe3a598, 0x15fa188, 0xe3d0a8, 0xe3d908, 0xe3a118, 0x1462a6c},
     edits_504, sizeof edits_504 / sizeof *edits_504,
     0xf6303c, 0xf63124, 0xf63120, {0xdb, 0x24c, 0x24e, 0x276, 0x289},
     0x842644, 0x29bc83c, 0x258e185, 0x8429b0, 0xe51b3340,
     0x29980a4, 0x1c91f74, 0x1cba858, 0x1cba874, 0,
     {"\xf4\x42\x6d\xe1\x02\x50\xa0\xe1", "\x04\xb0\x2d\xe5\x00\x30\xa0\xe3", "\x04\xb0\x2d\xe5\x00\xb0\x8d\xe2"}, 0x4bc},
    {"5.3.0", "\xbe\x53\x7e\x90\x81\x21\xe7\x61\x64\xdd\xbb\xf9\x73\x0d\x47\x2b\xa0\xb8\xf9\xd4",
     {0xe6a090, 0xe6a0bc, 0xe6a16c, 0xe6a198, 0xe6a1c4, 0xe6a0e8, 0xe6a140, 0x170e714, 0x1710c00,
      0x1710a74, 0x170f110, 0xdde87c, 0xd265cc, 0xd265f8, 0xd26624, 0xf1afa4, 0x16b38e0, 0xf1dd20, 0xf1ede4, 0xf1ab24, 0xc77934},
     edits_530, sizeof edits_530 / sizeof *edits_530,
     0x1537148, 0x1537234, 0x1537230, {0xe3, 0x254, 0x256, 0x2a6, 0x2b9},
     0x8a1eac, 0x2c8e808, 0x21b8b98, 0x8a2218, 0xe51b3340,
     0x2b9bee8, 0x1d7289c, 0x1d47324, 0x1d7e2bc, 0x1d46a6c,
     {"\xf4\x42\x6d\xe1\xf8\x60\xcd\xe1", "\xf4\x42\x6d\xe1\x20\xe0\x8d\xe5", ""}, 0x52c},
    {"5.3.0-sclive4", "\xb9\x46\xc3\xb8\xd9\x9d\x32\x99\x8c\x81\x97\xd5\xdc\x90\x5a\x6b\xdd\xaa\x5d\x2f",
     {0xe774fc, 0xe77528, 0xe775d8, 0xe77604, 0xe77630, 0xe77554, 0xe775ac, 0x17597bc, 0x179b090,
      0x179af04, 0x175a1b8, 0xdeacec, 0xd31818, 0xd31844, 0xd31870, 0xf27698, 0x172e668, 0xf2a410, 0xf2b9b0, 0xf27218, 0xc7cb2c},
     edits_sclive, sizeof edits_sclive / sizeof *edits_sclive,
     0x159f4a8, 0x159f594, 0x159f590, {0xe3, 0x254, 0x256, 0x2a6, 0x2b9},
     0x89cd64, 0x2c28744, 0x222cc8d, 0x89d0d0, 0xe51b3338,
     0x2c1bea4, 0x1ddf11c, 0x1db3ba4, 0x1deab3c, 0x1db32ec,
     {"\xf4\x42\x6d\xe1\xf8\x60\xcd\xe1", "\xf4\x42\x6d\xe1\x20\xe0\x8d\xe5", ""}, 0x52c},
};
#endif

static void select_build(const struct engine_build *b) {
    build = b;
    for (unsigned i = 0; i < DSP_HOOK_COUNT; i++) dsp_hooks[i].offset = b->dsp[i];
}

static int engine_base(struct dl_phdr_info *info, size_t size, void *out) {
    (void)size;
    for (size_t i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type != PT_NOTE) continue;
        const uchar *at = (const uchar *)(info->dlpi_addr + p->p_vaddr), *end = at + p->p_memsz;
        while ((size_t)(end - at) >= sizeof(ElfW(Nhdr))) {
            const ElfW(Nhdr) *n = (const void *)at;
            size_t names = ((size_t)n->n_namesz + 3) & ~(size_t)3;
            size_t desc = ((size_t)n->n_descsz + 3) & ~(size_t)3;
            at += sizeof *n;
            if (names > (size_t)(end - at) || desc > (size_t)(end - at) - names) break;
            if (n->n_type == NT_GNU_BUILD_ID && n->n_namesz == 4 && n->n_descsz == 20 && !memcmp(at, "GNU", 4))
                for (size_t b = 0; b < sizeof engine_builds / sizeof *engine_builds; b++)
                    if (!memcmp(at + names, engine_builds[b].id, 20)) {
                        select_build(&engine_builds[b]);
                        *(uintptr_t *)out = info->dlpi_addr;
                    }
            at += names + desc;
        }
    }
    return 1; // the first object is the program itself
}

static int edit_state(uintptr_t base, const struct engine_edit *e) {
    const void *at = (const void *)(base + e->offset);
    if (!memcmp(at, e->replacement, e->size)) return 2;
    if (!memcmp(at, e->expected, e->size)) return 1;
    // Upgrade the previous processor-only getConfig branch without mixing code paths.
    if (build == &engine_builds[0] && e->offset == 0xf6303c && !memcmp(at, "\xef\xef\xe3\xea\xd8\x30\x9f\xe5", 8)) return 1;
    return 0;
}

static void install_sqlite_hooks(uintptr_t base, size_t page);

int g_hook_initialized = 0;

__attribute__((constructor)) static void edit_engine(void) {
#ifdef ENGINE_UI_HOOK_TEST
    g_hook_initialized = 1;
#else
    const char *preload = getenv("LD_PRELOAD");
    if (preload && strstr(preload, "libengine-ui-hook.so")) {
        g_hook_initialized = 1;
    }
#endif
    if (!is_engine()) return;
    if (!g_hook_initialized) return;
    initialize_engine_cpus();
    uintptr_t base = 0;
    dl_iterate_phdr(engine_base, &base);
    if (!base) return;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return;
    uintptr_t page = (uintptr_t)page_size;
    fprintf(stderr, "engine-ui-hook: Engine %s\n", build->name);
#if !defined(__aarch64__)
    prepare_config(base, page);
    uint32_t expected_feature[2] = {build->feature_displaced ? build->feature_displaced : 0xe51b3340, 0xe3540000};
    if (build->feature_true && !memcmp((const void *)(base + build->feature_site), expected_feature, 8))
        prepare_stems_feature(base, page);
#endif
    // A group is applied only if every site of it is stock or already patched.
    int ready[3] = {1, 1, 1};
    for (unsigned i = 0; i < build->edit_count; i++)
        if (!edit_state(base, &build->edits[i])) ready[build->edits[i].group] = 0;
    for (unsigned i = 0; i < build->edit_count; i++) {
        const struct engine_edit *e = &build->edits[i];
        if (e->group != EDIT_ONE && !ready[e->group]) continue;
        // An unprepared trampoline is all zero: never write it.
        if (e->replacement == (const char *)stems_feature_jump && !stems_feature_jump[1]) continue;
        char *at = (char *)(base + e->offset);
        if (edit_state(base, e) != 1) continue;
        uintptr_t first = (uintptr_t)at - (uintptr_t)at % page;
        if (e->size > UINTPTR_MAX - (uintptr_t)at) continue;
        uintptr_t end = (uintptr_t)at + e->size;
        uintptr_t tail = end % page;
        if (tail && page - tail > UINTPTR_MAX - end) continue;
        if (tail) end += page - tail;
        char *start = (char *)first;
        size_t span = (size_t)(end - first);
        if (mprotect(start, span, PROT_READ | PROT_WRITE)) {
            // A partial Stems trampoline set must never reach Engine startup.
            _exit(127);
        }
        memcpy(at, e->replacement, e->size);
        if (mprotect(start, span, PROT_READ | PROT_EXEC)) {
            // Do not execute Engine code with an incomplete protection transition.
            static const char error[] = "engine-ui-hook: restoring RX protection failed\n";
            write(STDERR_FILENO, error, sizeof error - 1);
            _exit(127);
        }
        __builtin___clear_cache(at, at + e->size);
        fprintf(stderr, "engine-ui-hook: applied 0x%x (%u bytes)\n", e->offset, e->size);
    }
    install_sqlite_hooks(base, page);
    if (install_dsp_hooks(base, page)) lim_start_poll();
}

// Unknown or unsafe values fall back to the English pack.
static void pack_dir(char *out, size_t size) {
    char lang[16] = "en";
    FILE *f = fopen(UI_ROOT "/settings.conf", "r");
    if (f) {
        char line[64];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "lang=", 5))
                continue;
            line[strcspn(line, "\r\n")] = 0;
            if (!strcmp(line + 5, "zh-Hans") || !strcmp(line + 5, "zh-Hant"))
                strcpy(lang, line + 5);
        }
        fclose(f);
    }
    snprintf(out, size, UI_ROOT "/packs/%s", lang);
}

// Returns {be32 size, bytes} as rcc lays out an uncompressed file, or NULL.
static uchar *load_blob(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct stat st;
    uchar *blob = NULL;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < 16 * 1024 * 1024 &&
        (blob = malloc((size_t)st.st_size + 4))) {
        put32(blob, (uint32_t)st.st_size);
        size_t got = 0;
        while (got < (size_t)st.st_size) {
            ssize_t n = read(fd, blob + 4 + got, (size_t)st.st_size - got);
            if (n <= 0) {
                free(blob);
                blob = NULL;
                break;
            }
            got += (size_t)n;
        }
    }
    close(fd);
    return blob;
}

#if defined(__aarch64__)
// Qt finds a file at data + a signed 32-bit offset (sign-extended with sxtw on AArch64).
// On a 64-bit Engine the heap is far from data, so map each blob within 0..0x7fffffff
// (positive signed 32-bit offset) above it; start 512 MiB up, clear of the Engine image
// and its brk heap.
// End of the first mapping overlapping [addr, addr + span), page-aligned minus one page
// (the loop adds it back); addr itself when /proc/self/maps cannot say.
static uintptr_t skip_mapping(uintptr_t addr, size_t span, size_t page) {
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) return addr;
    unsigned long start, end;
    uintptr_t out = addr;
    while (fscanf(f, "%lx-%lx%*[^\n]", &start, &end) == 2)
        if (start < addr + span && end > addr) { out = ((uintptr_t)end + page - 1) / page * page - page; break; }
    fclose(f);
    return out;
}

static uintptr_t near_next; // next candidate address for a blob

static uchar *near_data(uchar *blob, const uchar *data) {
    uintptr_t next = near_next;
    size_t page = (size_t)sysconf(_SC_PAGESIZE), n = 4 + (size_t)be32(blob), span = (n + page - 1) / page * page;
    uintptr_t lo = ((uintptr_t)data + page - 1) / page * page, hi = (uintptr_t)data + 0x7fffffffu - span;
    if (next < lo || next > hi) next = lo + 0x20000000u;
    for (; next <= hi; next += page) {
        void *p = mmap((void *)next, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) {
            if (errno != EEXIST) break; // ENOMEM/EINVAL: retrying every page cannot help
            next = skip_mapping(next, span, page);
            continue;
        }
        if ((uintptr_t)p != next) { munmap(p, span); continue; } // kernel without NOREPLACE took it as a hint
        memcpy(p, blob, n);
        free(blob);
        near_next = next + span;
        return p;
    }
    free(blob);
    return NULL;
}
#endif

// Node name in ASCII, or 0 for a name a pack could not use.
static int node_name(const struct walk *w, uint32_t offset, char *out, size_t size) {
    const uchar *p = w->names + offset;
    size_t len = be16(p);
    if (len == 0 || len + 1 > size)
        return 0;
    for (size_t i = 0; i < len; i++) {
        if (p[6 + i * 2] || p[7 + i * 2] < 0x20 || p[7 + i * 2] > 0x7e || p[7 + i * 2] == '/')
            return 0;
        out[i] = (char)p[7 + i * 2];
    }
    out[len] = 0;
    return 1;
}

// Engine 5.3.0 forces Qt's QML disk cache (QML_FORCE_DISK_CACHE beats QML_DISABLE_DISK_CACHE)
// into /data, and Qt reuses a .qmlc while the source timestamp is unchanged: a cache compiled
// from stock QML, an older pack or the other language would win over the pack forever.
// A timestamp from the content makes Qt recompile exactly when the text changes.
static uint64_t content_stamp(const uchar *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL; // FNV-1a
    while (n--) h = (h ^ *p++) * 0x100000001b3ULL;
    return 1577836800000ULL + h % 2524608000000ULL; // ms in 2020..2100, a valid QDateTime
}

static uint32_t max_node(const struct walk *w, uint32_t node) {
    const uchar *e = w->tree + node * w->node_size;
    uint32_t top = node;
    if (be16(e + 4) & FLAG_DIRECTORY) {
        uint32_t count = be32(e + 6), first = be32(e + 10);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t m = max_node(w, first + i);
            if (m > top)
                top = m;
        }
    }
    return top;
}

static void apply(struct walk *w, uint32_t node, const char *prefix, int depth) {
#if defined(__aarch64__)
    if (w->failed) return; // no address left: the whole pack is dropped anyway
#endif
    uchar *e = w->tree + node * w->node_size;
    uint16_t flags = be16(e + 4);
    char name[128], path[512];
    if (node == 0) {
        path[0] = 0;
    } else {
        if (!node_name(w, be32(e), name, sizeof name))
            return;
        if (snprintf(path, sizeof path, "%s/%s", prefix, name) >= (int)sizeof path)
            return;
    }
    if (flags & FLAG_DIRECTORY) {
        if (depth > 32)
            return;
        uint32_t count = be32(e + 6), first = be32(e + 10);
        for (uint32_t i = 0; i < count; i++)
            apply(w, first + i, path, depth + 1);
        return;
    }
    char file[700];
    uchar *blob = NULL;
    // A QML mod's file wins over the pack, which stays untouched (quarantine switches mods off).
    if (w->has_overlay) {
        snprintf(file, sizeof file, UI_ROOT "/mods-overlay%s", path);
        blob = load_blob(file);
    }
    if (!blob) {
        snprintf(file, sizeof file, "%s%s", w->pack, path);
        blob = load_blob(file);
    }
    if (!blob)
        return;
#if defined(__aarch64__)
    if (!(blob = near_data(blob, w->data))) {
        w->failed = 1;
        return;
    }
#endif
    // Qt adds the offset to the data pointer. On a 32-bit target it wraps,
    // so the blob can live anywhere in the address space.
    put32(e + 10, (uint32_t)((uintptr_t)blob - (uintptr_t)w->data));
    e[4] = (uchar)((flags & ~(FLAG_COMPRESSED | FLAG_ZSTD)) >> 8);
    e[5] = (uchar)(flags & ~(FLAG_COMPRESSED | FLAG_ZSTD));
    if (w->node_size >= 22) { // rcc v2+: be64 last-modified ms at +14
        uint64_t stamp = content_stamp(blob + 4, be32(blob));
        put32(e + 14, (uint32_t)(stamp >> 32));
        put32(e + 18, (uint32_t)stamp);
    }
    w->replaced++;
}

int _Z21qRegisterResourceDataiPKhS0_S0_(int version, const uchar *tree, const uchar *names, const uchar *data) {
    static void *cache;
    static int active = -1;
    register_fn real = next_symbol(&cache, "_Z21qRegisterResourceDataiPKhS0_S0_");
    if (!real)
        return 0;
    if (active < 0)
        active = is_engine() && access(UI_ROOT "/safe-mode", F_OK) != 0;
    if (!active || version < 1 || version > 3)
        return real(version, tree, names, data);

    struct walk w = {.tree = (uchar *)tree, .names = names, .data = data, .node_size = version >= 2 ? 22 : 14,
                  .has_overlay = access(UI_ROOT "/mods-quarantined", F_OK) != 0 && access(UI_ROOT "/mods-overlay", F_OK) == 0};
    pack_dir(w.pack, sizeof w.pack);
    size_t bytes = (size_t)(max_node(&w, 0) + 1) * w.node_size;
    uchar *copy = malloc(bytes);
    if (!copy)
        return real(version, tree, names, data);
    memcpy(copy, tree, bytes);
    w.tree = copy;
    apply(&w, 0, "", 0);
#if defined(__aarch64__)
    if (w.failed) {
        fprintf(stderr, "engine-ui-hook: %s: no address for a pack file, stock UI\n", w.pack);
        w.replaced = 0;
    }
#endif
    if (!w.replaced) {
        free(copy);
        return real(version, tree, names, data);
    }
    fprintf(stderr, "engine-ui-hook: %s: %d files\n", w.pack, w.replaced);
    return real(version, copy, names, data);
}

static int thread_class(const char *name) {
    static const char *idle[] = {"Analysis Queue", "OfflineAnalyzer", "Dropbox", "Beatport",
        "J:I:Takingfreez", "Taking freeze", "Freeze Checker", "Collecting anal", "sentry", "CrashHandler"};
    for (size_t i = 0; i < sizeof idle / sizeof *idle; i++)
        if (!strncmp(name, idle[i], strlen(idle[i]))) return 2;
    return !strncmp(name, "QQuickPixmapRea", 15) || !strncmp(name, "FileInfoThread", 14);
}

static int guard_affinity(pthread_t thread, int kind) {
    static void *get_cache, *set_cache;
    int (*get)(pthread_t, size_t, cpu_set_t *) = next_symbol(&get_cache, "pthread_getaffinity_np");
    int (*set)(pthread_t, size_t, const cpu_set_t *) = next_symbol(&set_cache, "pthread_setaffinity_np");
    if (!get || !set) return ENOSYS;
    cpu_set_t allowed;
    if (engine_cpus_ready) allowed = engine_cpus;
    else {
        int rc = get(thread, sizeof allowed, &allowed);
        if (rc) return rc;
    }
    CPU_CLR(0, &allowed);
    if (!CPU_COUNT(&allowed)) return EINVAL; // A single-core target cannot avoid CPU0.
    if (kind == 2) {
        int last = CPU_SETSIZE - 1;
        while (!CPU_ISSET(last, &allowed)) last--;
        CPU_ZERO(&allowed);
        CPU_SET(last, &allowed);
    }
    return set(thread, sizeof allowed, &allowed);
}

static void lower_io_nice(pid_t tid, int nice, int idle_io) {
    setpriority(PRIO_PROCESS, (id_t)tid, nice);
    if (idle_io) syscall(SYS_ioprio_set, 1, tid, 3 << 13);
}

static void guard_named_thread(pthread_t thread, const char *name) {
    if (!is_engine()) return;
    int kind = thread_class(name);
    if (!kind) return;
    int saved_errno = errno;
    struct sched_param param = {.sched_priority = 0};
    if (kind == 2) pthread_setschedparam(thread, SCHED_IDLE, &param);
    guard_affinity(thread, kind);
    if (pthread_equal(thread, pthread_self())) {
        lower_io_nice((pid_t)syscall(SYS_gettid), kind == 2 ? 19 : 10, kind == 2);
    } else {
        // Naming another pthread must not lower the caller (often Engine's main thread).
        // Equal kernel names have the same policy, so duplicate worker names are safe.
        DIR *tasks = opendir("/proc/self/task");
        struct dirent *entry;
        while (tasks && (entry = readdir(tasks))) {
            char *end;
            long tid = strtol(entry->d_name, &end, 10);
            if (*end || tid <= 0) continue;
            char path[80], comm[32];
            snprintf(path, sizeof path, "/proc/self/task/%ld/comm", tid);
            FILE *f = fopen(path, "r");
            if (!f) continue;
            int match = fgets(comm, sizeof comm, f) &&
                (comm[strcspn(comm, "\n")] = 0, !strncmp(comm, name, 15));
            fclose(f);
            if (match) lower_io_nice((pid_t)tid, kind == 2 ? 19 : 10, kind == 2);
        }
        if (tasks) closedir(tasks);
    }
    errno = saved_errno;
}

// #49 shadow decks: a shadow deck's copy of a track is analysed again by
// OfflineAnalyzer (13-18 s), and the result differs from the source's grid
// and overview. stream-sync writes the source's analysis next to the copy as
// <file>.perf; BeatDetectionRunner::process(path, prefs, overviewSize) then
// emits it as the stock analysis would and skips decoding. Engine stores it in
// its own library, so nothing outside Engine writes m.db.
// 5.3.0 armhf OfflineAnalyzer (sha256 2f328a9c…) and AZ04's 64-bit one (sha256 22333ecb…,
// BuildID f6566956…). Its analyser thread calls process() through a PMF connection, so
// the entry itself is hooked; the staticMetaObject's stringdata and static_metacall
// identify the build.
#if defined(__aarch64__)
#define OA_META 0x178df8u
#define OA_STRINGDATA 0x1236b0u
#define OA_METACALL 0x38580u
#define OA_PROCESS_FN 0x48c20u
// paciasp; stp x29,x30,[sp,#-160]!; mov x29,sp; stp x25,x26,[sp,#64]
static const uint32_t oa_process_prefix[4] = {0xd503233f, 0xa9b67bfd, 0x910003fd, 0xa9046bf9};
#define OA_PROGRESS 0xd0 // float member process() keeps in step with detectionProgress
#define OA_STOP 32       // byte stopBeatDetection() sets under the runner's mutex
#define OA_MAKE_BYTES "_ZN10QByteArrayC1EPKcx" // qsizetype is long long here
#else
#define OA_META 0x146510u
#define OA_STRINGDATA 0x12b808u
#define OA_METACALL 0x27b78u
#define OA_PROCESS_FN 0x3ad7cu
static const uint32_t oa_process_prefix[2] = {0xe16d42f4, 0xe1cd60f8}; // strd r4,[sp,#-36]!; strd r6,[sp,#8]
#define OA_PROGRESS 0x90
#define OA_STOP 16
#define OA_MAKE_BYTES "_ZN10QByteArrayC1EPKci"
#endif
#ifndef OA_LOADER_MS
#define OA_LOADER_MS 1500
#endif
#ifndef OA_LOG
#define OA_LOG "/var/log/sc5000-uimod-oa.log"
#define OA_SHADOW "/data/sc5000-custom/shadow-v3" // pkg/webui shadowDir()
#define OA_ENGINE_COPIES "/data/inMusicTempFiles/"
#endif
enum { OA_FINISHED = 0, OA_STARTED = 1, OA_DONE = 2, OA_STOPPED = 5, OA_PROGRESS_SIG = 6, OA_TRACK = 8, OA_BEATS = 9,
       OA_OVERVIEW = 10, OA_OVERVIEW_RGB = 11 };
// Qt 6 QByteArray / QString: qsizetype is ssize_t-wide (int on armhf, long long on AArch64)
struct oa_bytes { int *d; char *ptr; ssize_t size; };
struct oa_string { void *d; const uint16_t *ptr; ssize_t size; };
typedef void (*oa_process_fn)(void *, const struct oa_string *, const uchar *, int);
static oa_process_fn oa_orig;
static const void *oa_meta;
static void (*oa_activate)(void *, const void *, int, void **);
static void (*oa_make_bytes)(struct oa_bytes *, const char *, ssize_t);

// EXPF v1, little-endian: "EXPF", u32 1, f64 bpm, then trackData, beatData and
// overviewWaveFormData uncompressed, each as u32 length + bytes (pkg/webui/shadow.go).
struct oa_perf { double bpm; const char *part[3]; uint32_t len[3]; char *buf; };

static int oa_read_perf(const char *path, struct oa_perf *p) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    char *b = NULL;
    size_t size = 0, got = 0;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size >= 28 && st.st_size <= (16 << 20)) {
        size = (size_t)st.st_size;
        b = malloc(size);
        for (ssize_t n; b && got < size && (n = read(fd, b + got, size - got)) > 0;) got += (size_t)n;
    }
    close(fd);
    uint32_t v;
    if (!b || got != size || memcmp(b, "EXPF", 4) || (memcpy(&v, b + 4, 4), v != 1)) goto bad;
    memcpy(&p->bpm, b + 8, 8);
    size_t off = 16;
    for (int i = 0; i < 3; i++) {
        if (size - off < 4) goto bad;
        memcpy(&p->len[i], b + off, 4);
        off += 4;
        if (p->len[i] > size - off) goto bad;
        p->part[i] = b + off;
        off += p->len[i];
    }
    // trackData is >d Q i + peaks; an overview has its >q q d header and the 3 maxima.
    if (off != size || p->len[0] < 20 || p->len[2] < 27 || !(p->bpm > 0 && p->bpm < 1000)) goto bad;
    p->buf = b;
    return 1;
bad:
    free(b);
    return 0;
}

static void oa_emit(void *o, int signal, void *arg) {
    void *args[] = {NULL, arg};
    oa_activate(o, oa_meta, signal, arg ? args : NULL);
}

// Queued receivers copy the argument, so a refcounted QByteArray is required.
static void oa_emit_bytes(void *o, int signal, const char *data, uint32_t len) {
    struct oa_bytes b;
    oa_make_bytes(&b, data, (ssize_t)len);
    oa_emit(o, signal, &b);
    if (b.d && __atomic_sub_fetch(b.d, 1, __ATOMIC_ACQ_REL) == 0) free(b.d); // inline ~QByteArray
}

static size_t utf16_to_utf8(const uint16_t *s, int n, char *out, size_t cap) {
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (s[++i] - 0xdc00u);
        if (!c || o + 5 > cap) return 0;
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) out[o++] = (char)(0xc0 | c >> 6), out[o++] = (char)(0x80 | (c & 63));
        else if (c < 0x10000)
            out[o++] = (char)(0xe0 | c >> 12), out[o++] = (char)(0x80 | (c >> 6 & 63)), out[o++] = (char)(0x80 | (c & 63));
        else
            out[o++] = (char)(0xf0 | c >> 18), out[o++] = (char)(0x80 | (c >> 12 & 63)),
            out[o++] = (char)(0x80 | (c >> 6 & 63)), out[o++] = (char)(0x80 | (c & 63));
    }
    out[o] = 0;
    return o;
}

static void oa_log(const char *what, const char *path, long us) {
    int fd = open(OA_LOG, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    char line[PATH_MAX + 96];
    int n = snprintf(line, sizeof line, "%ld %s %ldus %s\n", (long)time(NULL), what, us, path);
    if (n > 0) (void)!write(fd, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
    close(fd);
}

// Engine loads a /data file from its own copy, /data/inMusicTempFiles/<id>_<name>,
// and analyses that: the .perf is next to the shadow copy with that name and
// size, in <copy dir>/ or <copy dir>/Contents/ (a rekordbox export's track).
static int oa_shadow_perf(const char *path, char *out, size_t cap) {
    const size_t n = strlen(OA_ENGINE_COPIES);
    const char *name = strncmp(path, OA_ENGINE_COPIES, n) ? NULL : strchr(path + n, '_');
    struct stat st;
    if (!name || strchr(name, '/') || stat(path, &st)) return 0;
    DIR *dir = opendir(OA_SHADOW);
    if (!dir) return 0;
    int found = 0;
    for (struct dirent *e; !found && (e = readdir(dir));) {
        for (int sub = 0; !found && sub < 2 && e->d_name[0] != '.'; sub++) {
            struct stat copy;
            int len = snprintf(out, cap, "%s/%s/%s%s", OA_SHADOW, e->d_name, sub ? "Contents/" : "", name + 1);
            found = len > 0 && (size_t)len + 6 <= cap && !stat(out, &copy) &&
                S_ISREG(copy.st_mode) && copy.st_size == st.st_size;
        }
    }
    closedir(dir);
    if (found) strcat(out, ".perf");
    return found;
}

// The stock process() emits: detectionStarted; per block beatData (unless
// prefs byte 12 is set), overview and RGB overview, each a whole blob or a
// chunk in the stored format; progress; trackData; detectionFinished(bpm as
// float); finished. One block covering the whole track is the stored result.
// Engine's deck loader decodes the whole file (3 s for 4 min on a JP07) and
// then resets an unfinished analysis: grid, overview and peaks sent before it
// were lost (BPM 120). The stock analysis always ended after the loader, so
// the replay waits it out (idle, honouring stop) and then sends everything.
static int oa_settle(void *o, const char *track, uint32_t len) {
    double rate = 0, seconds = 0;
    uint64_t samples = 0;
    if (len >= 16) {
        uint64_t bits = 0;
        for (int i = 0; i < 8; i++) bits = bits << 8 | (uchar)track[i];
        memcpy(&rate, &bits, 8);
        for (int i = 8; i < 16; i++) samples = samples << 8 | (uchar)track[i];
        if (rate > 0) seconds = (double)samples / rate;
    }
    long wait = OA_LOADER_MS + (long)(seconds * 1000 / 40);
    if (wait > 90000) wait = 90000;
    for (long t = 0; t < wait; t += 50) {
        if (__atomic_load_n((uchar *)o + OA_STOP, __ATOMIC_ACQUIRE)) return 0;
        usleep(50000);
    }
    return !__atomic_load_n((uchar *)o + OA_STOP, __ATOMIC_ACQUIRE);
}

static int oa_replay(void *o, const struct oa_string *s, const uchar *prefs) {
    char path[PATH_MAX];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (!s || s->size <= 0 || !utf16_to_utf8(s->ptr, s->size, path, sizeof path - 5)) return 0;
    char perf[PATH_MAX + 8];
    snprintf(perf, sizeof perf, "%s.perf", path);
    if (access(perf, F_OK) && !oa_shadow_perf(path, perf, sizeof perf)) {
        if (!strncmp(path, OA_ENGINE_COPIES, strlen(OA_ENGINE_COPIES))) oa_log("analyse", path, 0);
        return 0;
    }
    struct oa_perf p;
    if (!oa_read_perf(perf, &p)) {
        oa_log("bad", perf, 0);
        return 0;
    }
    oa_emit(o, OA_STARTED, NULL);
    if (!oa_settle(o, p.part[0], p.len[0])) {
        oa_emit(o, OA_STOPPED, NULL);
        oa_emit(o, OA_FINISHED, NULL);
        free(p.buf);
        oa_log("stopped", perf, 0);
        return 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long waited = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
    oa_emit_bytes(o, OA_TRACK, p.part[0], p.len[0]);
    if (!prefs[12]) {
        if (p.len[1]) oa_emit_bytes(o, OA_BEATS, p.part[1], p.len[1]);
        oa_emit_bytes(o, OA_OVERVIEW, p.part[2], p.len[2]);
    }
    oa_emit_bytes(o, OA_OVERVIEW_RGB, p.part[2], p.len[2]);
    double progress = 1.0, bpm = (float)p.bpm;
    *(float *)((char *)o + OA_PROGRESS) = 1.0f;
    oa_emit(o, OA_PROGRESS_SIG, &progress);
    oa_emit_bytes(o, OA_TRACK, p.part[0], p.len[0]);
    oa_emit(o, OA_DONE, &bpm);
    oa_emit(o, OA_FINISHED, NULL);
    free(p.buf);
    struct timespec t2;
    clock_gettime(CLOCK_MONOTONIC, &t2);
    oa_log("waited", perf, waited);
    oa_log("replayed", perf, (t2.tv_sec - t1.tv_sec) * 1000000L + (t2.tv_nsec - t1.tv_nsec) / 1000);
    return 1;
}

// BeatDetectionRunner::process(const QString &path, const AnalysisPreferences &prefs, int overviewSize)
static void oa_process(void *o, const struct oa_string *path, const uchar *prefs, int overview_size) {
    if (!oa_replay(o, path, prefs)) oa_orig(o, path, prefs, overview_size);
}

static int oa_main_bias(struct dl_phdr_info *info, size_t size, void *out) {
    (void)size;
    *(uintptr_t *)out = info->dlpi_addr; // the first object is the executable
    return 1;
}

static void oa_install(void) {
    uintptr_t base = 0;
    dl_iterate_phdr(oa_main_bias, &base);
    const uintptr_t *meta = (const uintptr_t *)(base + OA_META), entry = base + OA_PROCESS_FN;
    if (meta[1] != base + OA_STRINGDATA || meta[3] != base + OA_METACALL ||
        memcmp((const void *)entry, oa_process_prefix, sizeof oa_process_prefix)) return; // other build
    oa_activate = dlsym(RTLD_DEFAULT, "_ZN11QMetaObject8activateEP7QObjectPKS_iPPv");
    oa_make_bytes = dlsym(RTLD_DEFAULT, OA_MAKE_BYTES);
    if (!oa_activate || !oa_make_bytes) return;
    // Constructor: no other thread runs yet. The displaced instructions are not PC-relative.
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
#if defined(__aarch64__)
    uint32_t orig[8], jump[4] = {0x58000050, 0xd61f0200}; // ldr x16, #8; br x16
    uint64_t back = entry + sizeof oa_process_prefix, to = (uintptr_t)oa_process;
    memcpy(orig, oa_process_prefix, sizeof oa_process_prefix);
    orig[4] = 0x58000050, orig[5] = 0xd61f0200;
    memcpy(&orig[6], &back, 8);
    memcpy(&jump[2], &to, 8);
#else
    const uint32_t orig[] = {oa_process_prefix[0], oa_process_prefix[1], 0xe51ff004, (uint32_t)(entry + 8)};
    const uint32_t jump[] = {0xe51ff004, (uint32_t)(uintptr_t)oa_process}; // ldr pc, [pc, #-4]
#endif
    void *stub = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) return;
    memcpy(stub, orig, sizeof orig);
    if (mprotect(stub, page, PROT_READ | PROT_EXEC)) return;
    __builtin___clear_cache(stub, (char *)stub + sizeof orig);
    uintptr_t start = entry - entry % page;
    size_t span = (entry + sizeof jump + page - 1) / page * page - start;
    if (mprotect((void *)start, span, PROT_READ | PROT_WRITE)) return;
    oa_meta = meta;
    oa_orig = (oa_process_fn)stub;
    memcpy((void *)entry, jump, sizeof jump);
    mprotect((void *)start, span, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)entry, (char *)entry + sizeof jump);
    oa_log("installed", "", 0);
}

// #55: Engine analyses in OfflineAnalyzer child processes, which inherit this
// preload. Analysing a whole drive in the background must never compete with
// playback: idle CPU class, nice 19 and idle I/O, off CPU 0; threads inherit all.
__attribute__((constructor)) static void lower_offline_analyzer(void) {
    char exe[64];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = 0;
    const char *name = strrchr(exe, '/');
    if (strcmp(name ? name + 1 : exe, "OfflineAnalyzer")) return;
    struct sched_param param = {.sched_priority = 0};
    sched_setscheduler(0, SCHED_IDLE, &param);
    lower_io_nice(0, 19, 1);
    cpu_set_t cpus;
    if (!sched_getaffinity(0, sizeof cpus, &cpus) && CPU_ISSET(0, &cpus) && CPU_COUNT(&cpus) > 1) {
        CPU_CLR(0, &cpus);
        sched_setaffinity(0, sizeof cpus, &cpus);
    }
    oa_install();
}

// Engine itself raises its analyzers to nice -5 / SCHED_OTHER once they connect
// (seen on 5.3.0): keep them idle (#55).
static int offline_analyzer(pid_t pid) {
    char path[48], comm[32] = "";
    if (pid <= 0) return 0;
    snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int match = fgets(comm, sizeof comm, f) && !strncmp(comm, "OfflineAnalyzer", 15);
    fclose(f);
    return match;
}

int setpriority(__priority_which_t which, id_t who, int prio) {
    static void *cache;
    int (*real)(__priority_which_t, id_t, int) = next_symbol(&cache, "setpriority");
    if (!real) { errno = ENOSYS; return -1; }
    if (which == PRIO_PROCESS && is_engine() && offline_analyzer((pid_t)who)) prio = 19;
    return real(which, who, prio);
}

int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param) {
    static void *cache;
    int (*real)(pid_t, int, const struct sched_param *) = next_symbol(&cache, "sched_setscheduler");
    if (!real) { errno = ENOSYS; return -1; }
    static const struct sched_param idle = {.sched_priority = 0};
    if (is_engine() && offline_analyzer(pid)) policy = SCHED_IDLE, param = &idle;
    return real(pid, policy, param);
}

int pthread_setname_np(pthread_t thread, const char *name) {
    static void *cache;
    int (*real)(pthread_t, const char *) = next_symbol(&cache, "pthread_setname_np");
    if (!real) return ENOSYS;
    int rc = real(thread, name);
    if (!rc) guard_named_thread(thread, name);
    return rc;
}

static int named_prctl(void **cache, const char *symbol, int option, va_list ap) {
    int (*real)(int, ...) = next_symbol(cache, symbol);
    if (!real) { errno = ENOSYS; return -1; }
    // prctl's libc wrapper forwards four machine-word arguments to the kernel.
    unsigned long a2 = va_arg(ap, unsigned long), a3 = va_arg(ap, unsigned long);
    unsigned long a4 = va_arg(ap, unsigned long), a5 = va_arg(ap, unsigned long);
    int rc = real(option, a2, a3, a4, a5);
    if (!rc && option == PR_SET_NAME) guard_named_thread(pthread_self(), (const char *)a2);
    return rc;
}

int hook_prctl(int, ...) __asm__("prctl");
int hook_prctl_time64(int, ...) __asm__("__prctl_time64");
#define PRCTL_WRAPPER(fn, symbol) \
int fn(int option, ...) { \
    static void *cache; va_list ap; va_start(ap, option); \
    int rc = named_prctl(&cache, symbol, option, ap); va_end(ap); return rc; \
}
PRCTL_WRAPPER(hook_prctl, "prctl")
PRCTL_WRAPPER(hook_prctl_time64, "__prctl_time64")

int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param *param) {
    static void *cache;
    int (*real)(pthread_t, int, const struct sched_param *) = next_symbol(&cache, "pthread_setschedparam");
    if (!real) return ENOSYS;
    char name[16];
    struct sched_param idle = {.sched_priority = 0};
    if (is_engine() && !pthread_getname_np(thread, name, sizeof name) && thread_class(name) == 2) {
        policy = SCHED_IDLE;
        param = &idle;
    }
    return real(thread, policy, param);
}

static int analytics_db(const char *filename) {
    if (!filename) return 0;
    int uri = !strncmp(filename, "file:", 5);
    const char *start = filename + (uri ? 5 : 0), *end = start + strlen(start);
    if (uri) { const char *query = strchr(start, '?'); if (query) end = query; }
    const char *base = start;
    for (const char *p = start; p < end; p++) if (*p == '/') base = p + 1;
    size_t size = (size_t)(end - base);
    return (size == 10 && !memcmp(base, "posthog.db", 10)) ||
           (size == 9 && !memcmp(base, "events.db", 9));
}

typedef int (*sqlite_open_v2_fn)(const char *, void **, int, const char *);
typedef int (*sqlite_open_fn)(const char *, void **);
typedef int (*sqlite_exec_fn)(void *, const char *, int (*)(void *, int, char **, char **), void *, char **);
static sqlite_open_v2_fn sqlite_open_v2_original;
static sqlite_open_fn sqlite_open_original;
static sqlite_exec_fn sqlite_exec_original;

static void tune_database(int rc, const char *filename, void **db) {
    if (rc || !db || !*db || (filename && !strcmp(filename, ":memory:"))) return;
    sqlite_exec_original(*db, "PRAGMA synchronous = NORMAL; PRAGMA temp_store = MEMORY; PRAGMA cache_size = -8000;",
                         NULL, NULL, NULL);
}

static int __attribute__((used)) tuned_sqlite_open_v2(const char *filename, void **db, int flags, const char *vfs) {
    const char *target = analytics_db(filename) ? ":memory:" : filename;
    int rc = sqlite_open_v2_original(target, db, flags, vfs);
    tune_database(rc, target, db);
    return rc;
}

static int __attribute__((used)) tuned_sqlite_open(const char *filename, void **db) {
    const char *target = analytics_db(filename) ? ":memory:" : filename;
    int rc = sqlite_open_original(target, db);
    tune_database(rc, target, db);
    return rc;
}

#if defined(__aarch64__)
// Target of an unconditional B at "at", or 0.
static uintptr_t a64_branch_target(uintptr_t at) {
    uint32_t insn = *(const uint32_t *)at;
    if ((insn & 0xfc000000u) != 0x14000000u) return 0;
    return at + (uintptr_t)((int64_t)((int32_t)(insn << 6) >> 6) * 4);
}

// The displaced 16 bytes, then ldr x16, #8; br x16 to entry + 16. The caller has checked
// they are position independent. Hooked pages lose PROT_BTI when made RX again, so
// landing mid-function is allowed.
static void *function_trampoline(uintptr_t entry, size_t page) {
    uint32_t code[8];
    memcpy(code, (const void *)entry, 16);
    code[4] = 0x58000050; code[5] = 0xd61f0200;
    uint64_t back = entry + 16;
    memcpy(&code[6], &back, 8);
    return rx_code(code, sizeof code, page);
}

static void redirect_function(uintptr_t entry, const void *handler, size_t page) {
    uint32_t jump[4] = {0x58000050, 0xd61f0200};
    uint64_t to = (uintptr_t)handler;
    memcpy(&jump[2], &to, 8);
    uintptr_t first = entry - entry % page;
    size_t span = ((entry + sizeof jump + page - 1) / page * page) - first;
    if (mprotect((void *)first, span, PROT_READ | PROT_WRITE)) _exit(127);
    memcpy((void *)entry, jump, sizeof jump);
    if (mprotect((void *)first, span, PROT_READ | PROT_EXEC)) _exit(127);
    __builtin___clear_cache((char *)entry, (char *)entry + sizeof jump);
}

static void install_sqlite_hooks(uintptr_t base, size_t page) {
    const struct engine_build *b = build;
    if (!b->sqlite_opendb) return;
    const uint64_t *api = (const void *)(base + b->sqlite_api);
    if (api[55] != base + b->sqlite_exec || api[70] != base + b->sqlite_open || api[135] != base + b->sqlite_open_v2) return;
    // open = "bti c; mov x3,#0; mov w2,#6; b opendb", open_v2 = "bti c; b opendb"
    const uint32_t *open = (const void *)(base + b->sqlite_open), *v2 = (const void *)(base + b->sqlite_open_v2);
    if (open[0] != 0xd503245f || open[1] != 0xd2800003 || open[2] != 0x528000c2 ||
        a64_branch_target((uintptr_t)&open[3]) != base + b->sqlite_opendb || v2[0] != 0xd503245f ||
        a64_branch_target((uintptr_t)&v2[1]) != base + b->sqlite_opendb ||
        memcmp((const void *)(base + b->sqlite_opendb), b->sqlite_prefix[1], 16)) return;
    sqlite_exec_original = (sqlite_exec_fn)(base + b->sqlite_exec);
    sqlite_open_v2_original = (sqlite_open_v2_fn)function_trampoline(base + b->sqlite_opendb, page);
    redirect_function(base + b->sqlite_opendb, tuned_sqlite_open_v2, page);
    fprintf(stderr, "engine-ui-hook: SQLite open/open_v2 hooks installed\n");
}
#else
// Target of an unconditional ARM B at "at", or 0.
static uintptr_t arm_branch_target(uintptr_t at) {
    uint32_t insn = *(const uint32_t *)at;
    if ((insn & 0xff000000u) != 0xea000000u) return 0;
    int32_t offset = (int32_t)(insn << 8) >> 6;
    return at + 8 + (uintptr_t)offset;
}

static void *function_trampoline(uintptr_t entry, size_t page) {
    uint32_t code[4];
    memcpy(code, (const void *)entry, 8);
    code[2] = 0xe51ff004;
    code[3] = (uint32_t)(entry + 8);
    return rx_code(code, sizeof code, page);
}

static void redirect_function(uintptr_t entry, const void *handler, size_t page) {
    uint32_t jump[] = {0xe51ff004, (uint32_t)(uintptr_t)handler};
    uintptr_t first = entry - entry % page;
    size_t span = ((entry + sizeof jump + page - 1) / page * page) - first;
    if (mprotect((void *)first, span, PROT_READ | PROT_WRITE)) _exit(127);
    memcpy((void *)entry, jump, sizeof jump);
    if (mprotect((void *)first, span, PROT_READ | PROT_EXEC)) _exit(127);
    __builtin___clear_cache((char *)entry, (char *)entry + sizeof jump);
}

A32_ONLY static void install_sqlite_hooks(uintptr_t base, size_t page) {
    // Identified through sqlite3_api_routines slots 55/70/135 and ARM disassembly.
    const struct engine_build *b = build;
    const uint32_t *api = (const void *)(base + b->sqlite_api);
    if (api[55] != base + b->sqlite_exec || api[70] != base + b->sqlite_open || api[135] != base + b->sqlite_open_v2) return;
    if (memcmp((const void *)(base + b->sqlite_exec), b->sqlite_prefix[0], 8)) return;
    sqlite_exec_original = (sqlite_exec_fn)(base + b->sqlite_exec);
    if (b->sqlite_opendb) {
        // open = "mov r3,#0; mov r2,#6; b opendb", open_v2 = "b opendb": opendb has
        // open_v2's signature and is the only way in, so hooking it covers both.
        const uint32_t *open = (const void *)(base + b->sqlite_open), *v2 = (const void *)(base + b->sqlite_open_v2);
        if (open[0] != 0xe3a03000 || open[1] != 0xe3a02006 || arm_branch_target((uintptr_t)&open[2]) != base + b->sqlite_opendb ||
            arm_branch_target((uintptr_t)v2) != base + b->sqlite_opendb ||
            memcmp((const void *)(base + b->sqlite_opendb), b->sqlite_prefix[1], 8)) return;
        sqlite_open_v2_original = (sqlite_open_v2_fn)function_trampoline(base + b->sqlite_opendb, page);
        redirect_function(base + b->sqlite_opendb, tuned_sqlite_open_v2, page);
    } else {
        if (memcmp((const void *)(base + b->sqlite_open), b->sqlite_prefix[1], 8) ||
            memcmp((const void *)(base + b->sqlite_open_v2), b->sqlite_prefix[2], 8)) return;
        sqlite_open_original = (sqlite_open_fn)function_trampoline(base + b->sqlite_open, page);
        sqlite_open_v2_original = (sqlite_open_v2_fn)function_trampoline(base + b->sqlite_open_v2, page);
        redirect_function(base + b->sqlite_open, tuned_sqlite_open, page);
        redirect_function(base + b->sqlite_open_v2, tuned_sqlite_open_v2, page);
    }
    fprintf(stderr, "engine-ui-hook: SQLite open/open_v2 hooks installed\n");
}
#endif

static const char *volatile_path(int dirfd, const char *path, char out[PATH_MAX]) {
    if (!path || !is_engine()) return path;
    if (*path == '/' && !strstr(path, "/analytics/posthog.db") && !strstr(path, "/analytics/events.db") &&
        !strstr(path, "/CrashReportDB/")) return path; // fast path: the common open
    char full[PATH_MAX];
    if (*path == '/') {
        if (strlen(path) >= sizeof full) { errno = ENAMETOOLONG; return NULL; }
        strcpy(full, path);
    } else {
        char dir[PATH_MAX];
        if (dirfd == AT_FDCWD) {
            if (!getcwd(dir, sizeof dir)) return path;
        } else {
            char fdpath[64];
            snprintf(fdpath, sizeof fdpath, "/proc/self/fd/%d", dirfd);
            ssize_t n = readlink(fdpath, dir, sizeof dir - 1);
            if (n < 0) return path;
            dir[n] = 0;
        }
        if (snprintf(full, sizeof full, "%s/%s", dir, path) >= (int)sizeof full) {
            errno = ENAMETOOLONG; return NULL;
        }
    }
    if (!strstr(full, "/analytics/posthog.db") && !strstr(full, "/analytics/events.db") &&
        !strstr(full, "/CrashReportDB/")) return path;
    if (strstr(full, "/../") || !strcmp(full + strlen(full) - 3, "/..")) {
        errno = EINVAL; return NULL;
    }
    char root[80];
    snprintf(root, sizeof root, "/tmp/sc5000-engine-volatile-%u", (unsigned)geteuid());
    if (!strncmp(full, root, strlen(root)) && full[strlen(root)] == '/') return path;
    if (mkdir(root, 0700) && errno != EEXIST) return NULL;
    struct stat st;
    if (lstat(root, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077)) {
        errno = EACCES; return NULL;
    }
    if (snprintf(out, PATH_MAX, "%s%s", root, full) >= PATH_MAX) {
        errno = ENAMETOOLONG; return NULL;
    }
    for (char *p = out + strlen(root) + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        int failed = mkdir(out, 0700) && errno != EEXIST;
        *p = '/';
        if (failed) return NULL;
    }
    return out;
}

static int redirected_open(int dirfd, const char *path, int flags, mode_t mode) {
    static void *cache;
    int (*real)(int, const char *, int, ...) = next_symbol(&cache, "openat");
    char mapped[PATH_MAX];
    const char *target = volatile_path(dirfd, path, mapped);
    if (!target) return -1;
    if (target == mapped) dirfd = AT_FDCWD;
    return real ? real(dirfd, target, flags, mode) : (int)syscall(SYS_openat, dirfd, target, flags, mode);
}

#define OPEN_MODE(flags, ap) (((flags) & O_CREAT) || (((flags) & O_TMPFILE) == O_TMPFILE) ? va_arg(ap, mode_t) : 0)
#define OPEN_WRAPPER(fn, large) \
int fn(const char *path, int flags, ...) { \
    va_list ap; va_start(ap, flags); mode_t mode = OPEN_MODE(flags, ap); va_end(ap); \
    return redirected_open(AT_FDCWD, path, flags | (large), mode); \
}
#define OPENAT_WRAPPER(fn, large) \
int fn(int dirfd, const char *path, int flags, ...) { \
    va_list ap; va_start(ap, flags); mode_t mode = OPEN_MODE(flags, ap); va_end(ap); \
    return redirected_open(dirfd, path, flags | (large), mode); \
}
// Explicit assembler names also export both variants on Debian's default time64 ABI.
int hook_open(const char *, int, ...) __asm__("open");
int hook_open64(const char *, int, ...) __asm__("open64");
int hook_openat(int, const char *, int, ...) __asm__("openat");
int hook_openat64(int, const char *, int, ...) __asm__("openat64");
OPEN_WRAPPER(hook_open, 0)
OPEN_WRAPPER(hook_open64, O_LARGEFILE)
OPENAT_WRAPPER(hook_openat, 0)
OPENAT_WRAPPER(hook_openat64, O_LARGEFILE)

static int crashpad(const char *file) {
    const char *base = file ? strrchr(file, '/') : NULL;
    return file && !strcmp(base ? base + 1 : file, "crashpad_handler");
}

static void *access_cache;
static int raw_access(const char *path, int mode) {
    int (*real)(const char *, int) = next_symbol(&access_cache, "access");
    return real ? real(path, mode) : -1;
}

static const char *stems_processor_path(const char *path) {
    // AZ04 looks in the official add-on mount and /usr/Engine; redirect only while the stock binary is absent.
    if (path && (!strcmp(path, "/usr/local/bin/stems-processor") ||
                 !strcmp(path, "/usr/Engine/stems-processor") ||
                 !strcmp(path, "/secure-media/stems-processor/bin/stems-processor"))) {
        if (raw_access(path, X_OK) != 0) {
            if (raw_access("/data/sc5000-custom/bundle/stems-processor", X_OK) == 0)
                return "/data/sc5000-custom/bundle/stems-processor";
            if (raw_access("/data/sc5000-custom/bundle/bin/stems-processor", X_OK) == 0)
                return "/data/sc5000-custom/bundle/bin/stems-processor";
        }
    }
    return path;
}

int access(const char *path, int mode) {
    int (*real)(const char *, int) = next_symbol(&access_cache, "access");
    if (!real) { errno = ENOSYS; return -1; }
    if (is_engine()) path = stems_processor_path(path);
    return real(path, mode);
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
    static void *cache;
    if (is_engine()) {
        if (crashpad(path)) return ENOENT; // Never return success with a fabricated PID.
        path = stems_processor_path(path);
    }
    int (*real)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                const posix_spawnattr_t *, char *const [], char *const []) = next_symbol(&cache, "posix_spawn");
    return real ? real(pid, path, actions, attr, argv, envp) : ENOSYS;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    static void *cache;
    if (is_engine()) {
        if (crashpad(path)) { errno = ENOENT; return -1; }
        path = stems_processor_path(path);
    }
    int (*real)(const char *, char *const [], char *const []) = next_symbol(&cache, "execve");
    if (real) return real(path, argv, envp);
    errno = ENOSYS; return -1;
}

int execv(const char *path, char *const argv[]) {
    extern char **environ;
    return execve(path, argv, environ);
}

// Engine's RenderStemsJob sets thread affinity to CPU cores that exist on 6/8-core models
// (e.g. CPU 4/5), but on the 4-core SC5000 (cores 0..3) pthread_setaffinity_np fails with
// EINVAL, causing Engine to call QMessageLogger::fatal("Failed setting CPU Affinity") and abort.
int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize, const cpu_set_t *cpuset) {
    static void *cache;
    int (*real_fn)(pthread_t, size_t, const cpu_set_t *) = next_symbol(&cache, "pthread_setaffinity_np");
    if (!real_fn) return ENOSYS;
    int res = real_fn(thread, cpusetsize, cpuset);
    if (!is_engine()) return res;
    if (res == EINVAL) {
        cpu_set_t fallback;
        if (engine_cpus_ready) fallback = engine_cpus;
        else if (sched_getaffinity(0, sizeof fallback, &fallback)) return res;
        res = real_fn(thread, sizeof fallback, &fallback);
    }
    char name[16];
    if (!res && !pthread_getname_np(thread, name, sizeof name)) {
        int kind = thread_class(name);
        if (kind) res = guard_affinity(thread, kind);
    }
    return res;
}

int pthread_getaffinity_np(pthread_t thread, size_t cpusetsize, cpu_set_t *cpuset) {
    static void *cache;
    int (*real_fn)(pthread_t, size_t, cpu_set_t *) = next_symbol(&cache, "pthread_getaffinity_np");
    if (!real_fn) return ENOSYS;
    int res = real_fn(thread, cpusetsize, cpuset);
    return res;
}

// #61 / #67: One-shot ALSA boot loader for SC LIVE 4 (inmusic,jp21) and devices
// with read-only root filesystems where LD_PRELOAD cannot be placed in rootfs.
__attribute__((visibility("default")))
int sc5000_alsa_boot_hook(void *root, void *config, void **dst, void *private_data) {
    (void)root;
    (void)config;
    (void)private_data;
    if (dst) *dst = NULL;
    if (g_hook_initialized) return 0;
    if (!is_engine()) return 0;
    if (access("/run/sc5000-alsa-reexec.once", F_OK) == 0) return 0;

    int fd = open("/run/sc5000-alsa-reexec.once", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) close(fd);

    fprintf(stderr, "engine-ui-hook: ALSA one-shot boot hook triggered; activating EngineX OS\n");
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2) close(devnull);
        }
        unsetenv("LD_PRELOAD");
        char *const argv[] = {"/bin/sh", "/data/sc5000-custom/bundle/boot-activate.sh", NULL};
        execv("/bin/sh", argv);
        _exit(1);
    }
    return 0;
}

__attribute__((visibility("default")))
int snd_config_hook_sc5000_alsa_boot_hook(void *root, void *config, void **dst, void *private_data) {
    return sc5000_alsa_boot_hook(root, config, dst, private_data);
}

__attribute__((used, visibility("default")))
const char _sc5000_alsa_boot_hook_dlsym_config_hook_001[] = "ALSA_0.9";
__attribute__((used, visibility("default")))
const char _snd_config_hook_sc5000_alsa_boot_hook_dlsym_config_hook_001[] = "ALSA_0.9";

// ---- #119: second-generation *-Update.img (EngineX data bundle in an image-shaped file) ----
//
// <PREFIX>-<VERSION>-EngineX-OS-<Model>-Update.img is a 128-byte header
// ("ENGINEX_DATA_BUNDLE_IMG_V1\n<sha256 hex>\n<prefix>\n<version>\n", NUL padded) plus the
// Data-Bundle .tar.gz. These decks check the AZ0x signature in software, so the stock updater
// would refuse it. The hooks below make az01-update / planck-remote-screen accept such a file
// after checking its SHA-256 (integrity only, not a signature), and only ever write /data/enginex.
// The child processes of Scripts/engine inherit this LD_PRELOAD, so the updater and the
// Factory Reset path (az01-update --erase-data) see the hooks.
#define ENGINEX_IMG_MAGIC "ENGINEX_DATA_BUNDLE_IMG_V1\n"
#define ENGINEX_IMG_MAGIC_LEN 27
#define ENGINEX_IMG_HEADER 128
#define ENGINEX_DATA_DIR "/data/enginex"
#define ENGINEX_ERR_BAD_IMAGE (-13)

enum { IMG_NONE = 0, IMG_OK = 1, IMG_BAD = -1 };

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
} sha256_ctx;

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + sha256_k[i] + w[i];
        uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(sha256_ctx *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->n = 0;
}

static void sha256_update(sha256_ctx *c, const uint8_t *p, size_t len) {
    c->len += len;
    while (len) {
        size_t k = 64 - c->n < len ? 64 - c->n : len;
        memcpy(c->buf + c->n, p, k);
        c->n += k; p += k; len -= k;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}

static void sha256_final(sha256_ctx *c, char hex[65]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, zero = 0, lenbe[8];
    sha256_update(c, &pad, 1);
    while (c->n != 56) sha256_update(c, &zero, 1);
    for (int i = 0; i < 8; i++) lenbe[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(c, lenbe, 8);
    for (int i = 0; i < 8; i++) snprintf(hex + 8 * i, 9, "%08x", c->h[i]);
}

static ssize_t read_retry(int fd, void *buf, size_t count) {
    ssize_t n;
    do {
        n = read(fd, buf, count);
    } while (n < 0 && errno == EINTR);
    return n;
}

static size_t read_full(int fd, uint8_t *buf, size_t count) {
    size_t total = 0;
    while (total < count) {
        ssize_t n = read_retry(fd, buf + total, count - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    return total;
}

// IMG_NONE: not an EngineX data bundle at all. IMG_BAD: it claims to be one but has no V1 header,
// is truncated or its payload SHA-256 differs from the header's. IMG_OK: header and payload agree.
static int enginex_img_check(const char *path) {
    uint8_t head[ENGINEX_IMG_HEADER];
    int fd = path ? open(path, O_RDONLY | O_CLOEXEC) : -1;
    if (fd < 0) return IMG_NONE;
    size_t n = read_full(fd, head, sizeof head);
    if (n < 19 || memcmp(head, "ENGINEX_DATA_BUNDLE", 19) != 0) { close(fd); return IMG_NONE; }
    int ok = n == ENGINEX_IMG_HEADER && memcmp(head, ENGINEX_IMG_MAGIC, ENGINEX_IMG_MAGIC_LEN) == 0 &&
             head[ENGINEX_IMG_MAGIC_LEN + 64] == '\n';
    sha256_ctx c;
    char hex[65];
    uint8_t buf[16384];
    size_t payload = 0;
    if (ok) {
        sha256_init(&c);
        ssize_t r;
        while ((r = read_retry(fd, buf, sizeof buf)) > 0) {
            sha256_update(&c, buf, (size_t)r);
            payload += (size_t)r;
        }
        if (r < 0 || payload == 0) ok = 0;
    }
    close(fd);
    if (!ok) return IMG_BAD;
    sha256_final(&c, hex);
    return strncasecmp(hex, (const char *)head + ENGINEX_IMG_MAGIC_LEN, 64) == 0 ? IMG_OK : IMG_BAD;
}

static int enginex_gen2_locked(void) {
    const char *root = getenv("ENGINEX_TEST_DATA_ROOT");
    if (root && *root) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/enginex/gen2-locked", root);
        return access(path, F_OK) == 0;
    }
    return access(ENGINEX_DATA_DIR "/gen2-locked", F_OK) == 0;
}

static uint64_t enginex_dummy_img[16];
static int enginex_img_state = IMG_NONE;  // result of the last az01_open_image on an EngineX image

// az01_download_image / az01-update --file: open -> set_onboard_key_lookup -> verify -> close -> apply.
// The stand-in handle is never given to the real library.
__attribute__((visibility("default")))
int az01_open_image(const char *path, void **img_out) {
    int state = enginex_img_check(path);
    if (state != IMG_NONE) {
        if (!img_out) return ENGINEX_ERR_BAD_IMAGE;
        __atomic_store_n(&enginex_img_state, state, __ATOMIC_RELEASE);
        *img_out = enginex_dummy_img;
        return 0;
    }
    static void *orig_fn = NULL;
    int (*fn)(const char *, void **) = next_symbol(&orig_fn, "az01_open_image");
    return fn ? fn(path, img_out) : -1;
}

__attribute__((visibility("default")))
int az01_image_set_onboard_key_lookup(void *img) {
    if (img == (void *)enginex_dummy_img) return 0;
    static void *orig_fn = NULL;
    int (*fn)(void *) = next_symbol(&orig_fn, "az01_image_set_onboard_key_lookup");
    return fn ? fn(img) : -1;
}

__attribute__((visibility("default")))
int az01_verify_image(void *img, int flags) {
    if (img == (void *)enginex_dummy_img) {
        int state = __atomic_load_n(&enginex_img_state, __ATOMIC_ACQUIRE);
        return state == IMG_OK && !enginex_gen2_locked() ? 0 : ENGINEX_ERR_BAD_IMAGE;
    }
    static void *orig_fn = NULL;
    int (*fn)(void *, int) = next_symbol(&orig_fn, "az01_verify_image");
    return fn ? fn(img, flags) : -1;
}

__attribute__((visibility("default")))
int az01_close_image(void *img) {
    if (img == (void *)enginex_dummy_img) {
        __atomic_store_n(&enginex_img_state, IMG_NONE, __ATOMIC_RELEASE);
        return 0;
    }
    static void *orig_fn = NULL;
    int (*fn)(void *) = next_symbol(&orig_fn, "az01_close_image");
    return fn ? fn(img) : -1;
}

static int run_wait(const char *prog, char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        unsetenv("LD_PRELOAD");
        execv(prog, argv);
        _exit(127);
    }
    if (pid < 0) return -1;
    int status = 0;
    pid_t rc;
    do {
        rc = waitpid(pid, &status, 0);
    } while (rc < 0 && errno == EINTR);
    return rc == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void enginex_reboot(void) {
    sync();
    char *const argv[] = {"systemctl", "reboot", NULL};
    if (run_wait("/bin/systemctl", argv) != 0)
        run_wait("/usr/bin/systemctl", argv);
}

// Intercepts az01_apply_image for EngineX data bundles; everything else goes to the stock updater.
__attribute__((visibility("default")))
int az01_apply_image(const char *path, int flags) {
    int state = enginex_img_check(path);
    if (state == IMG_BAD) return ENGINEX_ERR_BAD_IMAGE;
    if (state == IMG_OK) {
        if (enginex_gen2_locked()) return ENGINEX_ERR_BAD_IMAGE;
        fprintf(stderr, "engine-ui-hook: intercepted az01_apply_image with an EngineX data bundle image (%s)\n", path);
        const char *test_root = getenv("ENGINEX_TEST_DATA_ROOT");
        int testing = test_root && *test_root;
        char custom_dir[PATH_MAX] = "";
        if (testing) snprintf(custom_dir, sizeof custom_dir, "%s/enginex", test_root);
        // Unpacked next to the install (the /tmp tmpfs is small) and installed by the bundle's own
        // install.sh, so an older install.sh that does not know "install.sh <img>" is not needed.
        static const char script[] =
            "set -e; d=\"${2:-" ENGINEX_DATA_DIR "}\"; "
            "[ -z \"$2\" ] || export SC5000_CUSTOM=\"$d\" SC5000_ALSA_OVERLAY=\"$d/alsa-conf.d\"; "
            "mkdir -p \"$d\"; w=$(mktemp -d \"$d/.ota-img.XXXXXX\"); trap 'rm -rf \"$w\"' 0; "
            "tail -c +129 \"$1\" | tar -xzf - -C \"$w\"; sh \"$w\"/*/install.sh";
        char *const argv[] = {"sh", "-c", (char *)script, "sh", (char *)path, custom_dir, NULL};
        if (run_wait("/bin/sh", argv) != 0) return -1;
        if (flags & 1) unlink(path);
        if (!testing) enginex_reboot();
        return 0;
    }
    typedef int (*az01_apply_fn)(const char *, int);
    static void *orig_fn = NULL;
    az01_apply_fn fn = (az01_apply_fn)next_symbol(&orig_fn, "az01_apply_image");
    if (fn) {
        return fn(path, flags);
    }
    return -1;
}

// Removes path (a file, a symlink itself, or a directory tree on root_dev) without following symlinks
// or crossing a mount point.
static void rm_tree(int parent, const char *name, dev_t root_dev) {
    if (unlinkat(parent, name, 0) == 0 || errno == ENOENT) return;
    struct stat st;
    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) != 0 || st.st_dev != root_dev) return;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        if (fstat(fd, &st) == 0 && st.st_dev == root_dev) {
            DIR *d = fdopendir(fd);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)) != NULL)
                    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) rm_tree(dirfd(d), e->d_name, root_dev);
                closedir(d);
            } else {
                close(fd);
            }
        } else {
            close(fd);
        }
    }
    unlinkat(parent, name, AT_REMOVEDIR);
}

// Factory Reset on a /data-bundle deck: wipes the user's data and EngineX's own settings but keeps
// the installed bundle, UI packs, version, unlock flag, SSH keys and /data/system, so EngineX OS
// survives. /data/enginex and the /data/sc5000-custom compat symlink are never removed.
// ENGINEX_TEST_DATA_ROOT redirects /data (and skips the reboot) for the unit test.
__attribute__((visibility("default")))
int az01_erase_data_partition(void) {
    const char *root = getenv("ENGINEX_TEST_DATA_ROOT");
    int testing = root && *root;
    if (!testing) root = "/data";
    char probe_boot[PATH_MAX], probe_inst[PATH_MAX];
    snprintf(probe_boot, sizeof probe_boot, "%s/enginex/bundle/boot-activate.sh", root);
    snprintf(probe_inst, sizeof probe_inst, "%s/enginex/bundle/install.sh", root);
    if (getenv("SC5000_TEST_PASSTHROUGH_ERASE") ||
        (access(probe_boot, F_OK) != 0 && access(probe_inst, F_OK) != 0) ||
        access("/usr/share/sc5000-custom/sc5000-suite.xz", F_OK) == 0) {
        // 1st-gen: EngineX lives in the rootfs and sc5000-custom-prepare restores it after the stock erase.
        static void *orig_fn = NULL;
        int (*fn)(void) = next_symbol(&orig_fn, "az01_erase_data_partition");
        return fn ? fn() : -1;
    }
    int rootfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rootfd < 0) return -1;
    struct stat root_st;
    if (fstat(rootfd, &root_st) != 0) { close(rootfd); return -1; }
    DIR *d = fdopendir(rootfd);
    if (!d) { close(rootfd); return -1; }
    static const char *const keep[] = {"enginex", "system", "sc5000-custom", "lost+found"};
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int skip = !strcmp(e->d_name, ".") || !strcmp(e->d_name, "..");
        for (size_t i = 0; i < sizeof keep / sizeof *keep; i++) skip |= !strcmp(e->d_name, keep[i]);
        if (!skip) rm_tree(rootfd, e->d_name, root_st.st_dev);
    }
    int ef = openat(rootfd, "enginex", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (ef >= 0) {
        static const char *const gone[] = {
            "state", "mods", "Stream", "shadow-v3", "ui-scale.env", "ssh-config.json", "mounts.json", "automix.json",
            "ui/settings.conf", "ui/views.json", "ui/mods-overlay", "ui/mods-quarantined",
            "ui/safe-mode", "ui/boot-count", "ui/boot-token", "ui/last-crash", "ui/running-sentinel"};
        for (size_t i = 0; i < sizeof gone / sizeof *gone; i++) rm_tree(ef, gone[i], root_st.st_dev);
        mkdirat(ef, "state", 0755);
        mkdirat(ef, "mods", 0755);
        close(ef);
    }
    closedir(d);
    if (!testing) enginex_reboot();
    else sync();
    return 0;
}
