// Compile and run with ARMv7 gcc / qemu-arm on IN.
#define ENGINE_UI_HOOK_TEST
int hook_test_engine;
#define OA_LOG "/tmp/test-oa-replay.log"
#define OA_LOADER_MS 0
#define OA_SHADOW "/tmp/test-oa-replay/shadow-v3"
#define OA_ENGINE_COPIES "/tmp/test-oa-replay/copies/"
#include "engine_ui_hook.c"
#include <assert.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <math.h>
#include <float.h>

static unsigned dsp_calls, dsp_seen_generation, dsp_seen_sampler_generation;

// Frames left in the ramp of one common tail; -1 if it has no slot yet.
static int32_t chain_ramp(const void *common) {
    for (unsigned i = 0; i < SC_DSP_CHAIN_SLOTS; i++)
        if (sc_dsp_state()->chain[i].self == common) return sc_dsp_state()->chain[i].ramp_remaining;
    return -1;
}
static void *dsp_wanted_owner;
static const void *dsp_wanted_event;

static void mock_dsp_event(void *owner, const void *event) {
    assert(owner == dsp_wanted_owner && event == dsp_wanted_event);
    __atomic_add_fetch(&dsp_calls, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&dsp_seen_generation,
        __atomic_load_n(&sc_dsp_state()->generation, __ATOMIC_ACQUIRE), __ATOMIC_RELAXED);
    __atomic_store_n(&dsp_seen_sampler_generation,
        __atomic_load_n(&sc_dsp_state()->sampler_generation, __ATOMIC_ACQUIRE), __ATOMIC_RELAXED);
}

struct sampler_thread_args { char *image; void *handler; const void *event; };
static void *sampler_thread(void *arg) {
    const struct sampler_thread_args *a = arg;
    for (unsigned i = 0; i < 1500; i++) {
        unsigned slot = DSP_SAMPLER_IMMEDIATE + i % 3;
        ((dsp_event_fn)(a->image + dsp_hooks[slot].offset))(a->handler, a->event);
    }
    return NULL;
}

static const float sweep_samples[] = {
    NAN, INFINITY, -INFINITY, FLT_MAX, -FLT_MAX, 9.0f, -9.0f,
    8.0f, -8.0f, 0.25f, -0.5f, 0.0f, -0.0f
};
static const float sweep_expected[] = {
    0.0f, 0.0f, 0.0f, 8.0f, -8.0f, 8.0f, -8.0f,
    8.0f, -8.0f, 0.25f, -0.5f, 0.0f, -0.0f
};
static unsigned sweep_calls;
static uintptr_t sweep_owner_adjustment;
static uint32_t sweep_wanted_frames;
static void mock_sweep_process(void *self, float *const *in, float *const *out, uint32_t frames) {
    assert((uintptr_t)self == (uintptr_t)dsp_wanted_owner + sweep_owner_adjustment);
    assert(in == out && frames == sweep_wanted_frames);
    sweep_calls++;
    for (unsigned ch = 0; ch < 2; ch++)
        for (uint32_t i = 0; i < frames; i++)
            in[ch][i] = sweep_samples[(i + ch) % (sizeof sweep_samples / sizeof *sweep_samples)];
}

static unsigned gate_zero_calls, gate_nonzero_calls;
static float gate_seen_phase;
static void mock_gate_nonzero(void *self, float *const *in, float *const *out, uint32_t frames) {
    assert(frames > 0 && in == out); // displaced mov r2,r1 must still execute
    memcpy(&gate_seen_phase, (char *)self + SC_DSP_AUTOGATE_PHASE_OFFSET, sizeof gate_seen_phase);
    gate_nonzero_calls++;
}
static void mock_gate_zero(void *self, float *const *in, float *const *out, uint32_t frames) {
    (void)self;
    assert(frames == 0 && in == out);
    gate_zero_calls++;
}

// Flanger::process stand-in (#54): in place, dips the level like the vendor's 0.7 x dry mix.
static unsigned flanger_calls;
static float flanger_dip;
static void mock_flanger_process(void *self, float *const *in, float *const *out, uint32_t frames) {
    assert(self && in && out && frames);
    flanger_calls++;
    for (int ch = 0; ch < 2; ch++)
        for (uint32_t i = 0; i < frames; i++) in[ch][i] *= flanger_dip;
}

static unsigned master_calls;
static int master_seen_channels;
static uint32_t mock_master_output(void *host, void *context, int in_channels, const float *const *in,
                                   int out_channels, float *const *out, int frames) {
    assert(host == (void *)0x11 && context == (void *)0x22 && in_channels == 2 && in && (out_channels == 4 || out_channels == 8) && frames == 64);
    master_calls++;
    master_seen_channels = out_channels;
    for (int ch = 0; ch < out_channels; ch++)
        for (int i = 0; i < frames; i++) out[ch][i] = i & 1 ? -2.0f : 2.0f;
    return 0x5a;
}

static int common_bad_output;
static void mock_common_process(void *self, float *const *in, float *const *out, uint32_t frames) {
    assert(self == dsp_wanted_owner && in && out);
    for (int ch = 0; ch < 2; ch++)
        for (uint32_t i = 0; i < frames; i++)
            out[ch][i] = common_bad_output == 2 ? FLT_MAX :
                (common_bad_output ? (i & 1 ? INFINITY : NAN) : 1.0f);
}

// Execute the actual ARM entry patches and displaced prologues, followed by
// small ABI-correct native tails into mocks. No vendor DSP needs to be emulated.
static void test_dsp_hooks(void) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE), image_size = 0x1800000;
    char *image = mmap(NULL, image_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(image != MAP_FAILED);
    const uint32_t dispatcher[] = {
        0xe1c021d0, 0xe590c00c, 0xe52db004, 0xe28db000, 0xe3130001, 0xe08c00c3,
        0x179c30c3, 0x17932002, 0xe28bd000, 0xe49db004, 0xe12fff12
    };
    for (unsigned i = 0; i < DSP_AUTOGATE; i++)
        memcpy(image + dsp_hooks[i].offset, dispatcher, sizeof dispatcher);
    memcpy(image + dsp_hooks[DSP_AUTOGATE].offset, dsp_autogate_prefix, sizeof dsp_autogate_prefix);
    uint32_t jump_nonzero[] = {0xe51ff004, (uint32_t)(uintptr_t)mock_gate_nonzero};
    uint32_t jump_zero[] = {0xe51ff004, (uint32_t)(uintptr_t)mock_gate_zero};
    memcpy(image + dsp_hooks[DSP_AUTOGATE].offset + 12, jump_nonzero, sizeof jump_nonzero);
    memcpy(image + dsp_hooks[DSP_AUTOGATE].offset + 0xe4, jump_zero, sizeof jump_zero);
    // Undo the real common-chain strd prologue before tail-calling a C mock.
    const uint32_t common[] = {0xe16d42f4, 0xe1cd60f8, 0xe1cd40d0, 0xe1cd60d8,
                              0xe28dd024, 0xe51ff004, (uint32_t)(uintptr_t)mock_common_process};
    memcpy(image + dsp_hooks[DSP_COMMON].offset, common, sizeof common);
    // Actual SweepFx prefix and native owner adjustment, then restore fp before
    // entering a C mock. r2 must become the original input pointer, not output.
    for (unsigned i = DSP_DUB_ECHO; i <= DSP_WASH; i++) {
        const uint32_t sweep[] = {0xe52db004, 0xe1a02001, 0xe28db000,
            i == DSP_DUB_ECHO ? 0xe2800060 : 0xe2800070,
            0xe28bd000, 0xe49db004, 0xe51ff004, (uint32_t)(uintptr_t)mock_sweep_process};
        memcpy(image + dsp_hooks[i].offset, sweep, sizeof sweep);
    }
    // Actual Flanger prefix (strd sets up a frame, cmp feeds a later beq); undo
    // the strd before entering the C mock.
    const uint32_t flanger[] = {0xe16d42f4, 0xe3530000, 0xe1cd40d0, 0xe28dd024,
                               0xe51ff004, (uint32_t)(uintptr_t)mock_flanger_process};
    memcpy(image + dsp_hooks[DSP_FLANGER].offset, flanger, sizeof flanger);
    // Actual airDeviceCallback prefix of this build (strd; ...), undone before the
    // 7-argument C mock so its three stack arguments are where the caller put them.
    // 5.3.0's second word is strd r10,[sp,#24], a harmless store into the same frame.
    size_t master_size;
    const uint32_t *master_prefix = dsp_expected(DSP_MASTER, &master_size);
    const uint32_t master[] = {master_prefix[0], master_prefix[1], 0xe1cd40d0, 0xe28dd024,
                              0xe51ff004, (uint32_t)(uintptr_t)mock_master_output};
    memcpy(image + dsp_hooks[DSP_MASTER].offset, master, sizeof master);
    // Every checked byte, including byte 12 of a dispatcher, gates the WHOLE group.
    for (unsigned i = 0; i < DSP_HOOK_COUNT; i++) {
        size_t size;
        dsp_expected(i, &size);
        image[dsp_hooks[i].offset + size - 1] ^= 1;
        assert(!install_dsp_hooks((uintptr_t)image, page));
        image[dsp_hooks[i].offset + size - 1] ^= 1;
        for (unsigned j = 0; j < DSP_HOOK_COUNT; j++) {
            const void *expected = dsp_expected(j, &size);
            assert(!memcmp(image + dsp_hooks[j].offset, expected, size));
            assert(!dsp_originals[j]);
        }
    }
    assert(!mprotect(image, image_size, PROT_READ | PROT_EXEC));
    __builtin___clear_cache(image, image + image_size);
    assert(install_dsp_hooks((uintptr_t)image, page));
    assert(!install_dsp_hooks((uintptr_t)image, page)); // repeat install leaves original stubs intact
    for (unsigned i = 0; i < DSP_HOOK_COUNT; i++) {
        uint32_t *at = (void *)(image + dsp_hooks[i].offset);
        assert(at[0] == 0xe51ff004 && at[1] == (uint32_t)(uintptr_t)dsp_hooks[i].hook);
        if (i == DSP_AUTOGATE) assert(at[2] == 0xe320f000);
    }
    memset(sc_dsp_state(), 0, sizeof *sc_dsp_state());
    uint32_t owner[8] = {0}, handler[6] = {0};
    unsigned char raw_event[0x40] = {0};
    // Deliberately unaligned payload; guards must use memcpy rather than casts.
    void *event = raw_event + 1;
    handler[3] = (uintptr_t)owner;
    handler[4] = (uintptr_t)mock_dsp_event;
    dsp_wanted_owner = owner;
    dsp_wanted_event = event;
    for (unsigned i = 0; i < DSP_MIX; i++) {
        unsigned before = dsp_calls, gen = sc_dsp_state()->generation;
        ((dsp_event_fn)(image + dsp_hooks[i].offset))(handler, event);
        assert(dsp_calls == before + 1 && dsp_seen_generation == gen + 1);
        assert(chain_ramp(owner) == -1); // a transition only bumps the generation
    }
    // Preserve the dispatcher's virtual target resolution and signed this adjustment.
    uint32_t vtable[] = {(uintptr_t)mock_dsp_event};
    owner[0] = (uintptr_t)vtable; handler[4] = 0; handler[5] = 1;
    ((dsp_event_fn)(image + dsp_hooks[DSP_SWITCH].offset))(handler, event);
    handler[3] = (uintptr_t)(owner + 1); handler[4] = (uintptr_t)mock_dsp_event; handler[5] = (uint32_t)-8;
    ((dsp_event_fn)(image + dsp_hooks[DSP_SWITCH].offset))(handler, event);
    handler[3] = (uintptr_t)owner; handler[5] = 0;

    const double values[] = {NAN, INFINITY, -INFINITY, 0.0, -0.0, -0.5, 0.5};
    for (unsigned i = DSP_MIX; i <= DSP_MOTOR_RAMP_TIME; i++) {
        dsp_event_fn call = (void *)(image + dsp_hooks[i].offset);
        size_t offset = i == DSP_MIX ? 0x0c : (i == DSP_PARAMETER ? 0x20 : 0x08);
        for (size_t j = 0; j < sizeof values / sizeof *values; j++) {
            double value = values[j]; float f = (float)value;
            if (i < DSP_NUDGE) memcpy((char *)event + offset, &f, sizeof f);
            else memcpy((char *)event + offset, &value, sizeof value);
            unsigned before = dsp_calls;
            call(handler, event);
            int accepted = sc_dsp_finite_f64(value) && (i != DSP_TEMPO || value > 0.0);
            if ((i == DSP_RAMP_TIME || i == DSP_MOTOR_RAMP_TIME) && value < 0.0) accepted = 0;
            assert(dsp_calls == before + (unsigned)accepted);
        }
        unsigned before = dsp_calls;
        call(handler, NULL);
        assert(dsp_calls == before);
    }
    assert(DSP_HOOK_COUNT == 21);
    puts("ARM DSP install: all 21 prefix guards, repeat install, event ABI/virtual/adjusted this, finite controls: PASS");

    unsigned fx_generation = sc_dsp_state()->generation;
    // Count before calling the original, without inspecting payloads or arming FX.
    dsp_wanted_event = NULL;
    for (unsigned i = DSP_SAMPLER_IMMEDIATE; i <= DSP_SAMPLER_PLAY; i++) {
        unsigned before = dsp_calls, gen = sc_dsp_state()->sampler_generation;
        ((dsp_event_fn)(image + dsp_hooks[i].offset))(handler, NULL);
        assert(dsp_calls == before + 1 && dsp_seen_sampler_generation == gen + 1);
    }
    dsp_wanted_event = event;
    uint32_t sampler_before = sc_dsp_state()->sampler_generation;
    sc_dsp_sampler_mark_event(owner, 0); sc_dsp_sampler_mark_event(owner, 4);
    assert(sc_dsp_state()->sampler_generation == sampler_before);
    struct sampler_thread_args args = {image, handler, event};
    pthread_t sampler_workers[4];
    unsigned calls_before = dsp_calls;
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&sampler_workers[i], NULL, sampler_thread, &args));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(sampler_workers[i], NULL));
    assert(sc_dsp_state()->sampler_generation == sampler_before + 6000 && dsp_calls == calls_before + 6000);
    __atomic_store_n(&sc_dsp_state()->sampler_generation, UINT32_MAX, __ATOMIC_RELEASE);
    ((dsp_event_fn)(image + dsp_hooks[DSP_SAMPLER_PLAY].offset))(handler, event);
    assert(sc_dsp_state()->sampler_generation == 0 && dsp_seen_sampler_generation == 0);
    assert(sc_dsp_state()->generation == fx_generation);
    puts("ARM Sampler: all three dispatchers, mark-before-forward, null payload, four producers, wrap and FX isolation: PASS");

    for (unsigned slot = DSP_DUB_ECHO; slot <= DSP_WASH; slot++) {
        float in_l[32], in_r[32], untouched_l[32], untouched_r[32];
        float *in_pair[2] = {in_l, in_r}, *untouched[2] = {untouched_l, untouched_r};
        dsp_process_fn call = (void *)(image + dsp_hooks[slot].offset);
        sweep_owner_adjustment = slot == DSP_DUB_ECHO ? 0x60 : 0x70;
        for (unsigned alias = 0; alias < 2; alias++) {
            for (unsigned i = 0; i < 32; i++) {
                in_l[i] = in_r[i] = 17.0f;
                untouched_l[i] = untouched_r[i] = 23.0f;
            }
            sweep_wanted_frames = 27;
            call(owner, in_pair, alias ? in_pair : untouched, sweep_wanted_frames);
            for (unsigned ch = 0; ch < 2; ch++) for (unsigned i = 0; i < 27; i++) {
                float expected = sweep_expected[(i + ch) % (sizeof sweep_expected / sizeof *sweep_expected)];
                assert(!memcmp(&in_pair[ch][i], &expected, sizeof expected));
            }
            for (unsigned i = 0; i < 32; i++) assert(untouched_l[i] == 23.0f && untouched_r[i] == 23.0f);
            assert(in_l[27] == 17.0f && in_r[31] == 17.0f);
            sweep_wanted_frames = 0;
            call(owner, in_pair, untouched, 0);
            assert(in_l[27] == 17.0f);
        }
    }
    assert(sweep_calls == 8);
    float single_sample = NAN, *partial_buffers[2] = {&single_sample, NULL};
    sc_dsp_sweep_fx_post_process(NULL, NULL, 1);
    sc_dsp_sweep_fx_post_process(NULL, partial_buffers, 0);
    assert(!sc_dsp_finite_f32(single_sample));
    sc_dsp_sweep_fx_post_process(NULL, partial_buffers, 1);
    assert(single_sample == 0.0f);
    // Include signaling NaNs and subnormals, beyond the usual math.h constants.
    const uint32_t raw_bits[] = {0x7f800001, 0xff800001, 0x7fc00123, 0x80000000, 0x00000001};
    const uint32_t wanted_bits[] = {0, 0, 0, 0x80000000, 0x00000001};
    float bit_samples[5]; memcpy(bit_samples, raw_bits, sizeof raw_bits);
    float *bit_buffers[2] = {bit_samples, NULL};
    sc_dsp_sweep_fx_post_process(NULL, bit_buffers, 5);
    assert(!memcmp(bit_samples, wanted_bits, sizeof wanted_bits));
    assert(sc_dsp_state()->generation == fx_generation);
    puts("ARM DubEcho/Wash: prologue/this adjustment, input/output ABI, NaN/Inf/extreme clamp, zeros and bounds: PASS");

    {
        float fl_l[128], fl_r[128], scratch_l[128], scratch_r[128];
        float *fl[2] = {fl_l, fl_r}, *scratch[2] = {scratch_l, scratch_r};
        unsigned char flanger_self[0x220] = {0};
        dsp_process_fn call = (void *)(image + dsp_hooks[DSP_FLANGER].offset);
        flanger_dip = 0.7f;
        for (unsigned block = 0; block < 3000; block++) {
            for (unsigned i = 0; i < 128; i++) fl_l[i] = fl_r[i] = 0.5f * sinf((float)(block * 128 + i) * 0.02f);
            call(flanger_self, fl, scratch, 128);
        }
        float wet = 0.0f, dry = 0.0f;
        for (unsigned i = 0; i < 128; i++) {
            float x = 0.5f * sinf((float)(2999 * 128 + i) * 0.02f);
            dry += x * x; wet += fl_l[i] * fl_l[i];
        }
        assert(fabsf(sqrtf(wet / dry) - 1.0f) < 0.03f);
        // A level the flanger did not lower passes bit-exact on a fresh instance.
        unsigned char plain_self[0x220] = {0};
        flanger_dip = 1.0f;
        for (unsigned i = 0; i < 128; i++) fl_l[i] = fl_r[i] = 0.125f;
        call(plain_self, fl, scratch, 128);
        for (unsigned i = 0; i < 128; i++) assert(fl_l[i] == 0.125f && fl_r[i] == 0.125f);
        assert(flanger_calls == 3001);
        puts("ARM Flanger (#54): displaced strd/cmp prologue, in-place ABI, makeup restores the dry level, unity when no dip: PASS");
    }

    {
        typedef uint32_t (*master_fn)(void *, void *, int, const float *const *, int, float *const *, int);
        master_fn call = (master_fn)(image + dsp_hooks[DSP_MASTER].offset);
        static float buf[4][64], in0[64];
        const float *in[2] = {in0, in0};
        float *out[4] = {buf[0], buf[1], buf[2], buf[3]};
        __atomic_store_n(&lim_enabled, 0, __ATOMIC_RELAXED);
        assert(call((void *)0x11, (void *)0x22, 2, in, 4, out, 64) == 0x5a);
        assert(buf[0][0] == 2.0f && buf[3][1] == -2.0f); // off: untouched
        __atomic_store_n(&lim_enabled, 1, __ATOMIC_RELAXED);
        assert(call((void *)0x11, (void *)0x22, 2, in, 4, out, 64) == 0x5a);
        for (int ch = 0; ch < 4; ch++)
            for (int i = 0; i < 64; i++) assert(fabsf(buf[ch][i]) <= SC_LIM_CEIL && fabsf(buf[ch][i]) > 0.9f);
        __atomic_store_n(&lim_enabled, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&gain_enabled, 1, __ATOMIC_RELAXED); // #64 alone still ends in the limiter
        assert(call((void *)0x11, (void *)0x22, 2, in, 4, out, 64) == 0x5a);
        for (int ch = 0; ch < 4; ch++)
            for (int i = 0; i < 64; i++) assert(fabsf(buf[ch][i]) <= SC_LIM_CEIL && fabsf(buf[ch][i]) > 0.9f);
        __atomic_store_n(&gain_enabled, 0, __ATOMIC_RELAXED);
        assert(master_calls == 3);
        puts("ARM airDeviceCallback (#58): displaced per-build prologue, 7-argument ABI, both stereo pairs limited to -0.1 dBFS only when on: PASS");
        // #49: in the 4-deck layout Engine gets 8 outputs, the card's 4 first.
        four_deck_pad_on = 1;
        memset(buf, 0, sizeof buf);
        assert(call((void *)0x11, (void *)0x22, 2, in, 4, out, 64) == 0x5a);
        // Engine gets 8 scratch outputs; the role picks the deck on each of the card's pairs.
        assert(master_seen_channels == 8 && buf[0][0] == 2.0f && buf[3][1] == -2.0f && four_deck_pad[7][1] == -2.0f);
        four_deck_route[0] = 2, four_deck_route[1] = -1; // role 13 / 1 style: RCA B silent
        memset(buf, 0, sizeof buf);
        four_deck_pad[4][0] = 0; // deck 3, rewritten by the mock to 2.0
        assert(call((void *)0x11, (void *)0x22, 2, in, 4, out, 64) == 0x5a);
        assert(buf[0][0] == 2.0f && buf[2][0] == 0.0f && buf[3][1] == 0.0f);
        four_deck_route[0] = 0, four_deck_route[1] = 1;
        four_deck_pad_on = 0;
        assert(call((void *)0x11, (void *)0x22, 2, in, 4, out, 64) == 0x5a && master_seen_channels == 4);
        puts("ARM airDeviceCallback (#49): all 4 decks render, the role routes them to the card's pairs, only in the 4-deck layout: PASS");
    }

    unsigned char gate[0x200] = {0};
    const float phase = 0.75f;
    memcpy(gate + SC_DSP_AUTOGATE_PHASE_OFFSET, &phase, sizeof phase);
    float *empty[2] = {NULL, NULL};
    dsp_process_fn gate_call = (void *)(image + dsp_hooks[DSP_AUTOGATE].offset);
    gate_call(gate, empty, NULL, 1);
    assert(gate_seen_phase == phase);
    sc_dsp_fx_mark_transition(owner, SC_DSP_FX_TRANSITION_SWITCH);
    gate_call(gate, empty, NULL, 1);
    assert(gate_seen_phase == 0.0f);
    memcpy(gate + SC_DSP_AUTOGATE_PHASE_OFFSET, &phase, sizeof phase);
    gate_call(gate, empty, NULL, 1);
    assert(gate_seen_phase == phase); // only clear once per generation
    gate_call(gate, empty, NULL, 0);
    assert(gate_nonzero_calls == 3 && gate_zero_calls == 1);
    puts("ARM AutoGate: phase reset before original, one clear per generation, relocated zero/nonzero branches: PASS");

    enum { N = 2049 };
    static float left[N], right[N], out_l[N], out_r[N];
    float *in[2] = {left, right}, *out[2] = {out_l, out_r};
    dsp_process_fn process = (void *)(image + dsp_hooks[DSP_COMMON].offset);
    dsp_wanted_owner = owner;
    // Separate, identical pointer arrays, L alias, R-only alias and crossed alias.
    for (unsigned mode = 0; mode < 5; mode++) {
        memset(left, 0, sizeof left); memset(right, 0, sizeof right);
        out[0] = mode == 2 ? left : (mode == 4 ? right : out_l);
        out[1] = mode == 3 ? right : (mode == 4 ? left : out_r);
        sc_dsp_fx_mark_transition(owner, SC_DSP_FX_TRANSITION_TURN_ON);
        float **dest = mode == 1 ? in : out;
        process(owner, in, dest, 160);
        for (unsigned ch = 0; ch < 2; ch++)
            for (unsigned i = 0; i < 160; i++)
                assert(dest[ch][i] == (i < 128 ? (float)i / 128.0f : 1.0f));
        assert(chain_ramp(owner) == 0);
    }
    // Partially overlapping buffers need a snapshot too, including the right channel.
    memset(left, 0, sizeof left); memset(right, 0, sizeof right);
    out[0] = left + 1; out[1] = right + 1;
    sc_dsp_fx_mark_transition(owner, SC_DSP_FX_TRANSITION_CHANGE);
    process(owner, in, out, 128);
    assert(out[0][0] == 0.0f && out[1][127] == 127.0f / 128.0f);
    // A block far beyond the snapshot size still ramps: only its first 128 dry frames
    // are copied, so the stack stays at 1 KB for any block size.
    for (unsigned size = N - 1; size <= N; size++) {
        memset(left, 0, sizeof left); memset(right, 0, sizeof right);
        sc_dsp_fx_mark_transition(owner, SC_DSP_FX_TRANSITION_CHANGE);
        process(owner, in, in, size);
        assert(left[0] == 0.0f && left[127] == 127.0f / 128.0f && left[128] == 1.0f && left[size - 1] == 1.0f);
        assert(right[0] == 0.0f && right[size - 1] == 1.0f && chain_ramp(owner) == 0);
    }
    // A second common has its own ramp: the first one's finished ramp does not
    // swallow it, and it ramps when first seen.
    unsigned char other[8];
    memset(left, 0, sizeof left); memset(right, 0, sizeof right);
    dsp_wanted_owner = other;
    process(other, in, in, 64);
    dsp_wanted_owner = owner;
    assert(chain_ramp(other) == 64 && left[63] == 63.0f / 128.0f && chain_ramp(owner) == 0);
    // A short block resumes the same ramp in the next call.
    memset(left, 0, sizeof left); memset(right, 0, sizeof right);
    out[0] = out_l; out[1] = out_r;
    sc_dsp_fx_mark_transition(owner, SC_DSP_FX_TRANSITION_CHANGE);
    process(owner, in, out, 64);
    assert(out_l[0] == 0.0f && out_r[63] == 63.0f / 128.0f);
    process(owner, in, out, 64);
    assert(out_l[0] == 0.5f && out_r[63] == 127.0f / 128.0f);
    process(owner, in, out, 0);
    assert(chain_ramp(owner) == 0);
    common_bad_output = 1;
    for (unsigned i = 0; i < 160; i++) { left[i] = 0.25f; right[i] = 0.5f; }
    process(owner, in, out, 160);
    assert(out_l[0] == 0.25f && out_r[1] == 0.5f);
    left[0] = NAN; right[1] = INFINITY;
    process(owner, in, out, 160);
    assert(out_l[0] == 0.0f && out_r[1] == 0.0f);
    process(owner, in, in, 160);
    assert(left[0] == 0.0f && right[1] == 0.0f);
    common_bad_output = 0;
    // NaN/Inf sanitation must also cover overflow CREATED by the ramp itself.
    // Opposite finite extremes overflow the helper's (wet - dry) intermediate.
    common_bad_output = 2;
    for (unsigned i = 0; i < 160; i++) left[i] = right[i] = -FLT_MAX;
    sc_dsp_fx_mark_transition(owner, SC_DSP_FX_TRANSITION_TURN_ON);
    process(owner, in, out, 160);
    for (unsigned i = 0; i < 160; i++)
        assert(sc_dsp_finite_f32(out_l[i]) && sc_dsp_finite_f32(out_r[i]));
    common_bad_output = 0;
    for (unsigned i = 0; i < DSP_HOOK_COUNT; i++) {
        assert(!munmap(dsp_originals[i], page)); dsp_originals[i] = NULL;
    }
    memset(sc_dsp_state(), 0, sizeof *sc_dsp_state());
    assert(!munmap(image, image_size));
    for (unsigned i = 0; i < SC_DSP_CHAIN_SLOTS; i++)
        assert(sc_dsp_state()->chain[i].busy == 0); // every enter was paired with an exit
    puts("ARM FX common tail: ABI/prologue replay, in/out/overlap snapshots, 128-frame ramp, per-common slots, any block size, finite output: PASS");
}

__attribute__((naked)) static void invoke_stub(void *feature __attribute__((unused)),
    void *frame __attribute__((unused)), void *stub __attribute__((unused)),
    uint32_t *out __attribute__((unused))) {
    __asm__("push {r4, r5, r7, r8, fp, lr}\n"
            "mov r7, r0\nmov fp, r1\nmov r8, r2\nmov r5, r3\n"
            "mov r0, #17\nmov r1, #23\nmov r2, #29\nmov r4, #0\nblx r8\nstm r5, {r0, r1, r2, r3}\n"
            "pop {r4, r5, r7, r8, fp, pc}");
}

static void test_stems(void) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    char *image = mmap(NULL, 0x2d00000, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(image != MAP_FAILED);
    int is_sclive = build->feature_displaced == 0xe51b3338;
    int fp_displaced = is_sclive ? 824 : 832;
    int fp_enabled1 = is_sclive ? 800 : 808;
    int fp_enabled2 = is_sclive ? 796 : 804;
    uint32_t enabled[] = {
        is_sclive ? 0xe51b2320 : 0xe51b2328,
        is_sclive ? 0xe51b331c : 0xe51b3324,
        0xe12fff1e
    };
    uint32_t ordinary[] = {0x05873004, 0xe12fff1e}; // streq r3,[r7,#4]; bx lr
    memcpy(image + build->feature_enabled, enabled, sizeof enabled);
    memcpy(image + (build->feature_site + 8), ordinary, sizeof ordinary);
    char *code_page = image + (build->feature_site + 8) / page * page;
    assert(mprotect(code_page, page, PROT_READ | PROT_EXEC) == 0);
    __builtin___clear_cache(code_page, code_page + page);
    code_page = image + build->feature_enabled / page * page;
    assert(mprotect(code_page, page, PROT_READ | PROT_EXEC) == 0);
    __builtin___clear_cache(code_page, code_page + page);
    image[build->feature_true] = 1;
    prepare_stems_feature((uintptr_t)image, page);
    uint32_t *feature = (void *)(image + build->feature_object);
    uint32_t *other = feature + 8;
    uint32_t false_ptr = (uint32_t)(uintptr_t)(image + (build->feature_true + 1));
    feature[1] = other[1] = false_ptr;
    uint32_t frame[0x100] = {0}, out[4];
    char *fp = (char *)frame + 0x350;
    *(uint32_t *)(fp - fp_enabled1) = 1234;
    *(uint32_t *)(fp - fp_enabled2) = 5678;
    *(uint32_t *)(fp - fp_displaced) = false_ptr;
    void *stub = (void *)(uintptr_t)stems_feature_jump[1];
    invoke_stub(feature, fp, stub, out);
    assert(feature[1] == (uint32_t)(uintptr_t)(image + build->feature_true));
    assert(*(char *)(uintptr_t)feature[1] == 1);
    assert(out[0] == 17 && out[1] == 23 && out[2] == 1234 && out[3] == 5678);
    invoke_stub(other, fp, stub, out);
    assert(other[1] == false_ptr);
    assert(out[0] == 17 && out[1] == 23 && out[2] == 29 && out[3] == false_ptr);
    puts("ARM Stems binding, other-feature isolation and register replay: PASS");
    munmap(image, 0x2d00000);
}

__attribute__((naked)) static void invoke_config(void *config __attribute__((unused)),
    void *stub __attribute__((unused)), uint32_t *out __attribute__((unused))) {
    __asm__("push {r4, lr}\nmov r4, r2\nblx r1\nstm r4, {r0, r2, r3}\npop {r4, pc}");
}

static void test_config(void) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    char *image = mmap(NULL, 0x1600000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(image != MAP_FAILED);
    uint32_t resume = 0xe12fff1e;
    memcpy(image + (build->config_site + 8), &resume, 4);
    *(uint32_t *)(image + build->config_r2) = 123;
    *(uint32_t *)(image + build->config_r3) = 456;
    assert(!mprotect(image + build->config_site / page * page, page, PROT_READ | PROT_EXEC));
    __builtin___clear_cache(image + build->config_site / page * page, image + (build->config_site / page + 1) * page);
    prepare_config((uintptr_t)image, page);
    const uint16_t *fields = build->config_field;
    for (int model = 0; model < 6; model++) {
        uchar config[0x600], expected[0x600];
        memset(config, 0xa0 + model, sizeof config);
        memcpy(expected, config, sizeof expected);
        for (size_t i = 0; i < 5; i++) expected[fields[i]] = i == 4 ? 0 : 1;
        if (build->config_zoom == 0x52c) expected[0x54] = 1; // #152: Config.audio.hasStemsLevel
        if (build->config_zoom) *(uint32_t *)(expected + build->config_zoom) = 30; // #77
        uint32_t out[3];
        invoke_config(config, (void *)(uintptr_t)config_jump[1], out);
        assert(!memcmp(config, expected, sizeof config));
        assert(out[0] == (uintptr_t)config && out[1] == 123 && out[2] == 456);
    }
    // #49: four_deck_waveform=1 turns a 5.3.0 player into a 4-deck non-player and
    // leaves an all-in-one (already not a player) alone.
    if (!strcmp(build->name, "5.3.0")) {
        char path[] = "/tmp/four-deck-XXXXXX";
        int fd = mkstemp(path);
        const char flag[] = "lang=en\nfour_deck_waveform=1\n";
        assert(fd >= 0 && write(fd, flag, sizeof flag - 1) == (ssize_t)(sizeof flag - 1));
        close(fd);
        four_deck_settings = path;
        prepare_config((uintptr_t)image, page);
        for (int player = 0; player < 2; player++) {
            uchar config[0x600];
            memset(config, 0, sizeof config);
            config[0x239] = (uchar)player;
            *(uint32_t *)(config + 0x90) = 2;
            *(uint32_t *)(config + build->config_zoom) = player ? 30 : 10;
            uint32_t out[3];
            invoke_config(config, (void *)(uintptr_t)config_jump[1], out);
            assert(config[0x239] == 0 && *(uint32_t *)(config + 0x90) == (player ? 4u : 2u));
            assert(*(uint32_t *)(config + build->config_zoom) == 30); // #77, also in 4-deck mode
            assert(config[fields[0]] == 1 && config[fields[4]] == 0);
            assert(out[0] == (uintptr_t)config && out[1] == 123 && out[2] == 456);
        }
        // Only a player's 4-channel device gets the scratch outputs for decks 3/4.
        // JP11 / JP11S stay off with the other consoles: PRIME GO / GO+ have no
        // deck 3/4 scratch pair for an LC6000 layer. docs/LC6000-PRIME-GO-PLUS.md
        char code_path[] = "/tmp/product-code-XXXXXX";
        int cfd = mkstemp(code_path);
        assert(cfd >= 0);
        close(cfd);
        product_code_path = code_path;
        const char *boards[][2] = {{"JP07", "1"}, {"JP14", "1"}, {"JC11", "0"}, {"JP21", "0"}, {"JP11", "0"}, {"JP11S", "0"}};
        for (size_t i = 0; i < sizeof boards / sizeof boards[0]; i++) {
            FILE *f = fopen(code_path, "w");
            assert(f && fputs(boards[i][0], f) >= 0 && !fclose(f));
            prepare_config((uintptr_t)image, page);
            assert(four_deck_pad_on == (boards[i][1][0] == '1'));
        }
        unlink(code_path);
        four_deck_pad_on = 0;
        // four_deck_role picks the decks on RCA A / RCA B; auto takes engine-ui-guard's
        // answer (none here: 13), anything unknown is 13.
        four_deck_resolved = "/nonexistent/role";
        const struct { const char *role; signed char a, b; } roles[] = {
            {"24", 1, 3}, {"13", 0, 2}, {"34", 2, 3}, {"2", 1, -1}, {"1", 0, -1}, {"12", 0, 1}, {"auto", 0, 2}, {"9;rm", 0, 2}};
        for (size_t i = 0; i < sizeof roles / sizeof *roles; i++) {
            FILE *f = fopen(path, "w");
            assert(f && fprintf(f, "four_deck_waveform=1\nfour_deck_role=%s\n", roles[i].role) > 0 && !fclose(f));
            prepare_config((uintptr_t)image, page);
            assert(four_deck_route[0] == roles[i].a && four_deck_route[1] == roles[i].b);
        }
        char resolved[] = "/tmp/four-deck-role-XXXXXX";
        int rfd = mkstemp(resolved);
        assert(rfd >= 0 && write(rfd, "24\n", 3) == 3);
        close(rfd);
        four_deck_resolved = resolved;
        FILE *af = fopen(path, "w");
        assert(af && fputs("four_deck_waveform=1\nfour_deck_role=auto\n", af) >= 0 && !fclose(af));
        prepare_config((uintptr_t)image, page);
        assert(four_deck_route[0] == 1 && four_deck_route[1] == 3); // auto followed the guard
        unlink(resolved);
        unlink(path);
        four_deck_settings = UI_ROOT "/settings.conf";
        prepare_config((uintptr_t)image, page);
        puts("4-deck layout unlock: players only: PASS");
    }
    munmap(image, 0x1600000);
    puts("Common config hook, six input layouts, literal/register replay: PASS");
}

static int exec_count;
static int real_sqlite_exec(void *db, const char *sql, int (*cb)(void *, int, char **, char **), void *arg, char **err) {
    exec_count++;
    return sqlite3_exec(db, sql, cb, arg, err);
}

static void emit_tail(char *at, const uint32_t *prefix, size_t count, const void *fn) {
    memcpy(at, prefix, count * 4);
    uint32_t jump[] = {0xe51ff004, (uint32_t)(uintptr_t)fn};
    memcpy(at + count * 4, jump, sizeof jump);
}

static int pragma(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int value = sqlite3_column_int(stmt, 0);
    assert(sqlite3_finalize(stmt) == SQLITE_OK);
    return value;
}

static void test_sqlite(void) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    char *image = mmap(NULL, 0x2d00000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(image != MAP_FAILED);
    // Replay each build's actual prologues before tail-calling real SQLite.
    uint32_t open_code[] = {0xe52db004, 0xe3a03000, 0xe28db000, 0xe28bd000, 0xe49db004};
    uint32_t v2_code[] = {0xe52db004, 0xe28db000, 0xe28bd000, 0xe49db004};
    uint32_t exec_code[] = {0xe16d42f4, 0xe1a05002, 0xe1cd40d0, 0xe28dd024};
    if (build->sqlite_opendb) {
        uint32_t shared_code[] = {0xe16d42f4, 0xe58de020, 0xe59de020, 0xe1cd40d0, 0xe28dd024};
        uint32_t exec_shared[] = {0xe16d42f4, 0xe1cd60f8, 0xe1cd60d8, 0xe1cd40d0, 0xe28dd024};
        emit_tail(image + build->sqlite_opendb, shared_code, 5, sqlite3_open_v2);
        emit_tail(image + build->sqlite_exec, exec_shared, 5, real_sqlite_exec);
        uint32_t open_shared[] = {0xe3a03000, 0xe3a02006,
            0xea000000u | (((build->sqlite_opendb - build->sqlite_open - 16) >> 2) & 0xffffffu)};
        uint32_t v2_shared = 0xea000000u | (((build->sqlite_opendb - build->sqlite_open_v2 - 8) >> 2) & 0xffffffu);
        memcpy(image + build->sqlite_open, open_shared, sizeof open_shared);
        memcpy(image + build->sqlite_open_v2, &v2_shared, sizeof v2_shared);
    } else {
        emit_tail(image + build->sqlite_open, open_code, 5, sqlite3_open);
        emit_tail(image + build->sqlite_open_v2, v2_code, 4, sqlite3_open_v2);
        emit_tail(image + build->sqlite_exec, exec_code, 4, real_sqlite_exec);
    }
    uint32_t entry = build->sqlite_opendb ? build->sqlite_opendb : build->sqlite_open;
    uint32_t original = *(uint32_t *)(image + entry);
    uint32_t *api = (void *)(image + build->sqlite_api);
    api[70] = (uintptr_t)image + build->sqlite_open;
    api[135] = (uintptr_t)image + build->sqlite_open_v2;
    api[55] = (uintptr_t)image + build->sqlite_exec;
    image[build->sqlite_exec] ^= 1;
    install_sqlite_hooks((uintptr_t)image, page);
    assert(*(uint32_t *)(image + entry) == original);
    image[build->sqlite_exec] ^= 1;
    assert(!mprotect(image, 0x2d00000, PROT_READ | PROT_EXEC));
    __builtin___clear_cache(image, image + 0x2d00000);
    assert(!mprotect(image + build->sqlite_api / page * page, page, PROT_READ | PROT_WRITE));
    api[55] = 0;
    install_sqlite_hooks((uintptr_t)image, page);
    assert(*(uint32_t *)(image + entry) == original);
    api[55] = (uintptr_t)image + build->sqlite_exec;
    install_sqlite_hooks((uintptr_t)image, page);
    sqlite_open_fn open_fn = (void *)(image + build->sqlite_open);
    sqlite_open_v2_fn v2_fn = (void *)(image + build->sqlite_open_v2);
    void *db = NULL;
    char path[128];
    snprintf(path, sizeof path, "/tmp/sc5000-hook-test-%ld.db", (long)getpid());
    unlink(path);
    assert(v2_fn(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "unix") == SQLITE_OK);
    assert(exec_count == 1);
    assert(pragma(db, "PRAGMA synchronous") == 1);
    assert(pragma(db, "PRAGMA temp_store") == 2);
    assert(pragma(db, "PRAGMA cache_size") == -8000);
    assert(sqlite3_exec(db, "CREATE TABLE t(v); INSERT INTO t VALUES(42);", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
    assert(open_fn(path, &db) == SQLITE_OK && exec_count == 2);
    assert(pragma(db, "SELECT v FROM t") == 42);
    assert(sqlite3_close(db) == SQLITE_OK);
    assert(v2_fn(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK && exec_count == 3);
    assert(pragma(db, "PRAGMA cache_size") == -8000);
    assert(sqlite3_close(db) == SQLITE_OK);
    unlink(path);
    const char *memory[] = {"/nonexistent/analytics/posthog.db", "/nonexistent/analytics/events.db", ":memory:"};
    for (size_t i = 0; i < sizeof memory / sizeof *memory; i++) {
        assert(v2_fn(memory[i], &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) == SQLITE_OK);
        assert(!strcmp(sqlite3_db_filename(db, "main"), ""));
        assert(sqlite3_close(db) == SQLITE_OK);
    }
    assert(exec_count == 3);
    assert(open_fn(memory[0], &db) == SQLITE_OK && exec_count == 3);
    assert(sqlite3_close(db) == SQLITE_OK);
    assert(v2_fn("/nonexistent/hook-test/m.db", &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK);
    assert(exec_count == 3);
    if (db) sqlite3_close(db);
    db = NULL;
    assert(v2_fn(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "nonexistent-vfs") != SQLITE_OK);
    if (db) sqlite3_close(db);
    tune_database(SQLITE_OK, path, NULL);
    assert(exec_count == 3);
    munmap(image, 0x2d00000);
    puts("Installed ARM SQLite hooks, real PRAGMA readback, memory telemetry, errors/readonly: PASS");
}

static void test_files(void) {
    char path[PATH_MAX], target[PATH_MAX], mapped[PATH_MAX];
    snprintf(path, sizeof path, "/tmp/sc5000-hook-%ld/CrashReportDB/report.lock", (long)getpid());
    hook_test_engine = 1;
    const char *p = volatile_path(AT_FDCWD, path, mapped);
    assert(p == mapped && strstr(p, "/tmp/sc5000-engine-volatile-") == p);
    strcpy(target, mapped);
    assert(volatile_path(AT_FDCWD, target, mapped) == target);
    int (*openers[])(const char *, int, ...) = {hook_open, hook_open64};
    int fd;
    for (size_t i = 0; i < sizeof openers / sizeof *openers; i++) {
        fd = openers[i](path, O_CREAT | O_RDWR | O_TRUNC, 0600);
        assert(fd >= 0 && write(fd, "ok", 2) == 2);
        close(fd);
    }
    struct stat st;
    assert(stat(path, &st) == -1 && stat(target, &st) == 0 && st.st_size == 2);
    char *slash = strrchr(path, '/'); *slash = 0;
    // Resolve relative openat through an existing directory without bypassing redirection.
    char *dest_slash = strrchr(target, '/'); *dest_slash = 0;
    int dirfd = open(target, O_RDONLY | O_DIRECTORY);
    assert(dirfd >= 0);
    // An analytics directory used as dirfd must retain filename/flags/mode.
    char analytics[PATH_MAX];
    snprintf(analytics, sizeof analytics, "/tmp/sc5000-hook-%ld/analytics", (long)getpid());
    mkdir(path, 0700); // Missing original parents are intentional; do not create them.
    char root[128]; snprintf(root, sizeof root, "/tmp/sc5000-hook-%ld", (long)getpid());
    assert(!mkdir(root, 0700) || errno == EEXIST);
    assert(!mkdir(analytics, 0700));
    int ad = open(analytics, O_RDONLY | O_DIRECTORY);
    assert(ad >= 0);
    int (*at_openers[])(int, const char *, int, ...) = {hook_openat, hook_openat64};
    for (size_t i = 0; i < sizeof at_openers / sizeof *at_openers; i++) {
        fd = at_openers[i](ad, "events.db-journal", O_CREAT | O_WRONLY, 0640);
        assert(fd >= 0 && !fstat(fd, &st) && (st.st_mode & 0777) == 0640);
        close(fd);
    }
    assert(faccessat(ad, "events.db-journal", F_OK, 0) == -1);
    close(ad); close(dirfd);
    assert(volatile_path(AT_FDCWD, "/tmp/ordinary.db", mapped) != mapped);
    pid_t pid = 12345;
    char *argv[] = {"crashpad_handler", NULL};
    assert(posix_spawn(&pid, "/usr/bin/crashpad_handler", NULL, NULL, argv, environ) == ENOENT && pid == 12345);
    assert(execv("/usr/bin/crashpad_handler", argv) == -1 && errno == ENOENT);
    argv[0] = "true";
    assert(posix_spawn(&pid, "/bin/true", NULL, NULL, argv, environ) == 0);
    int status; assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    hook_test_engine = 0;
    assert(volatile_path(AT_FDCWD, "/data/analytics/posthog.db", mapped) != mapped);
    assert(hook_openat(-99, "events.db", O_RDONLY) == -1 && errno == EBADF);
    puts("File/openat64 isolation, mode/flags, Crashpad rejection and ordinary spawn: PASS");
}

static void *named_worker(void *unused) {
    (void)unused;
    assert(pthread_setname_np(pthread_self(), "Analysis Queue") == 0);
    cpu_set_t cpus; assert(pthread_getaffinity_np(pthread_self(), sizeof cpus, &cpus) == 0);
    assert(!CPU_ISSET(0, &cpus) && CPU_COUNT(&cpus) == 1);
    int policy; struct sched_param p;
    assert(!pthread_getschedparam(pthread_self(), &policy, &p) && policy == SCHED_IDLE);
    assert(getpriority(PRIO_PROCESS, 0) == 19);
    assert((syscall(SYS_ioprio_get, 1, 0) >> 13) == 3);
    return NULL;
}

static void *foreign_named_worker(void *fd) {
    char c;
    assert(read(*(int *)fd, &c, 1) == 1);
    cpu_set_t cpus; assert(!pthread_getaffinity_np(pthread_self(), sizeof cpus, &cpus));
    assert(!CPU_ISSET(0, &cpus));
    assert(getpriority(PRIO_PROCESS, 0) == 10);
    return NULL;
}

struct qt_thread_case { const char *name; int time64, kind; };

static void *qt_named_worker(void *arg) {
    const struct qt_thread_case *test = arg;
    int (*name_fn)(int, ...) = test->time64 ? hook_prctl_time64 : hook_prctl;
    assert(!name_fn(PR_SET_NAME, (unsigned long)test->name));
    char name[16];
    assert(!name_fn(PR_GET_NAME, (unsigned long)name));
    assert(!strcmp(name, test->name));
    struct sched_param normal = {.sched_priority = 0}, actual;
    // Qt applies its requested scheduling policy after setting the name.
    assert(!pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal));
    int policy;
    assert(!pthread_getschedparam(pthread_self(), &policy, &actual));
    assert(policy == (test->kind == 2 ? SCHED_IDLE : SCHED_OTHER));
    assert(actual.sched_priority == 0);
    assert(getpriority(PRIO_PROCESS, 0) == (test->kind == 2 ? 19 : 10));
    cpu_set_t wanted = engine_cpus, got, cpu1;
    CPU_CLR(0, &wanted);
    if (test->kind == 2) {
        int last = CPU_SETSIZE - 1;
        while (!CPU_ISSET(last, &wanted)) last--;
        CPU_ZERO(&wanted); CPU_SET(last, &wanted);
        assert((syscall(SYS_ioprio_get, 1, 0) >> 13) == 3);
    }
    assert(!pthread_getaffinity_np(pthread_self(), sizeof got, &got));
    assert(CPU_EQUAL(&got, &wanted));
    CPU_ZERO(&cpu1); CPU_SET(1, &cpu1);
    assert(!pthread_setaffinity_np(pthread_self(), sizeof cpu1, &cpu1));
    assert(!pthread_getaffinity_np(pthread_self(), sizeof got, &got));
    assert(CPU_EQUAL(&got, &wanted));
    assert(name_fn(PR_SET_NAME, 0UL, 0UL, 0UL, 0UL) == -1 && errno == EFAULT);
    return NULL;
}

static void test_threads(void) {
    hook_test_engine = 1;
    cpu_set_t saved, cpu1;
    assert(!sched_getaffinity(0, sizeof saved, &saved) && CPU_ISSET(1, &saved));
    CPU_ZERO(&cpu1); CPU_SET(1, &cpu1);
    assert(!sched_setaffinity(0, sizeof cpu1, &cpu1));
    initialize_engine_cpus();
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    assert(online > 1 && online <= CPU_SETSIZE);
    assert(CPU_COUNT(&engine_cpus) == online && CPU_ISSET((int)online - 1, &engine_cpus));
    int before = getpriority(PRIO_PROCESS, 0);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, named_worker, NULL));
    assert(!pthread_join(thread, NULL));
    assert(getpriority(PRIO_PROCESS, 0) == before);
    int fds[2]; assert(!pipe(fds));
    assert(!pthread_create(&thread, NULL, foreign_named_worker, &fds[0]));
    assert(!pthread_setname_np(thread, "QQuickPixmapRea"));
    assert(getpriority(PRIO_PROCESS, 0) == before);
    assert(write(fds[1], "x", 1) == 1);
    assert(!pthread_join(thread, NULL));
    close(fds[0]); close(fds[1]);
    const struct qt_thread_case cases[] = {
        {"Analysis Queue ", 0, 2}, {"Freeze Checker ", 1, 2},
        {"QQuickPixmapRea", 0, 1}, {"FileInfoThread", 1, 1},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        assert(!pthread_create(&thread, NULL, qt_named_worker, (void *)&cases[i]));
        assert(!pthread_join(thread, NULL));
    }
    cpu_set_t main_cpus;
    assert(!sched_getaffinity(0, sizeof main_cpus, &main_cpus) && CPU_EQUAL(&main_cpus, &cpu1));
    assert(getpriority(PRIO_PROCESS, 0) == before);
    assert(hook_prctl_time64(PR_GET_DUMPABLE, 0UL, 0UL, 0UL, 0UL) >= 0);
    assert(thread_class("AudioOut") == 0 && thread_class("AudioRenderer") == 0);
    assert(thread_class("QQuickPixmapRea") == 1 && thread_class("Freeze Checker") == 2);
    hook_test_engine = 0;
    struct sched_param normal = {.sched_priority = 0};
    assert(!pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal));
    assert(!sched_setaffinity(0, sizeof saved, &saved));
    engine_cpus_ready = 0;
    puts("pthread/prctl/time64 naming, Qt policy override, online CPUs and caller isolation: PASS");
}

struct mock_sw_params { int type; unsigned long start, stop, silence_threshold, silence_size; };
static int mock_get_rc, mock_set_rc, mock_apply_rc, mock_seen_type;
static char alsa_order[16];
static unsigned alsa_count;
static void *mock_pcm;

static int mock_tstamp_get(const void *params, int *type) {
    alsa_order[alsa_count++] = 'g';
    if (!mock_get_rc) *type = ((const struct mock_sw_params *)params)->type;
    return mock_get_rc;
}

static int mock_tstamp_set(void *pcm, void *params, int type) {
    assert(pcm == mock_pcm);
    alsa_order[alsa_count++] = 's';
    mock_seen_type = type;
    if (!mock_set_rc) ((struct mock_sw_params *)params)->type = type;
    return mock_set_rc;
}

static int mock_sw_apply(void *pcm, void *params) {
    assert(pcm == mock_pcm && params);
    alsa_order[alsa_count++] = 'a';
    return mock_apply_rc;
}

static void test_alsa(void) {
    void *old_sw = alsa_sw_cache, *old_set = alsa_tstamp_set_cache, *old_get = alsa_tstamp_get_cache;
    alsa_sw_cache = mock_sw_apply;
    alsa_tstamp_set_cache = mock_tstamp_set;
    alsa_tstamp_get_cache = mock_tstamp_get;
    char pcm; mock_pcm = &pcm;
    hook_test_engine = 1;
    for (int type = 0; type <= 2; type++) {
        struct mock_sw_params params, wanted;
        memset(&params, 0x5a, sizeof params);
        params.type = type; params.silence_threshold = 0;
        memcpy(&wanted, &params, sizeof wanted);
        wanted.type = type == 0 ? 2 : type;
        alsa_count = 0; memset(alsa_order, 0, sizeof alsa_order);
        assert(snd_pcm_sw_params(&pcm, &params) == 0);
        assert(!strcmp(alsa_order, type == 0 ? "gsa" : "ga"));
        assert(!memcmp(&params, &wanted, sizeof params));
    }
    struct mock_sw_params params = {.type = 0, .silence_threshold = 0};
    alsa_count = 0; mock_set_rc = -EINVAL;
    assert(snd_pcm_sw_params(&pcm, &params) == -EINVAL && alsa_count == 2 && params.type == 0);
    mock_set_rc = 0; mock_get_rc = -EINVAL; mock_apply_rc = -EBUSY; alsa_count = 0;
    assert(snd_pcm_sw_params(&pcm, &params) == -EBUSY && alsa_count == 2 && params.type == 0);
    mock_get_rc = 0; mock_apply_rc = 0;
    for (int type = -1; type <= 3; type++) {
        alsa_count = 0;
        assert(!snd_pcm_sw_params_set_tstamp_type(&pcm, &params, type));
        assert(mock_seen_type == (type == 0 ? 2 : type));
    }
    hook_test_engine = 0; params.type = 0; alsa_count = 0;
    assert(!snd_pcm_sw_params(&pcm, &params) && alsa_count == 1 && params.type == 0);
    alsa_count = 0;
    assert(!snd_pcm_sw_params_set_tstamp_type(&pcm, &params, 0) && mock_seen_type == 0);
    alsa_sw_cache = old_sw; alsa_tstamp_set_cache = old_set; alsa_tstamp_get_cache = old_get;
    puts("ALSA setup: realtime -> RAW, preserve explicit clocks/silence, call order and errors: PASS");
}

static int dns_calls;
static const char *dns_node, *dns_service;
static const struct addrinfo *dns_hints;

static int mock_dns(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **result) {
    assert(node == dns_node && service == dns_service && hints == dns_hints);
    dns_calls++;
    *result = NULL;
    return EAI_AGAIN;
}

static void test_update_dns(void) {
    void *old = getaddrinfo_cache; getaddrinfo_cache = mock_dns;
    hook_test_engine = 1;
    const char *blocked[] = {"autoupdate.airmusictech.com", "AUTOUPDATE.AIRMUSICTECH.COM", "autoupdate.airmusictech.com."};
    struct addrinfo hints = {.ai_family = AF_UNSPEC}, *result;
    for (size_t i = 0; i < sizeof blocked / sizeof *blocked; i++) {
        result = &hints;
        assert(getaddrinfo(blocked[i], "443", &hints, &result) == EAI_NONAME && !result);
    }
    assert(!dns_calls);
    const char *allowed[] = {"device.inmusicbrands.com", "auth.profile.inmusicbrands.com",
        "ipv4.connman.inmusicbrands.com", "api.github.com", "notautoupdate.airmusictech.com",
        "autoupdate.airmusictech.com.example", NULL};
    dns_service = "443"; dns_hints = &hints;
    for (size_t i = 0; i < sizeof allowed / sizeof *allowed; i++) {
        dns_node = allowed[i];
        assert(getaddrinfo(dns_node, dns_service, dns_hints, &result) == EAI_AGAIN);
    }
    hook_test_engine = 0; dns_node = blocked[0];
    assert(getaddrinfo(dns_node, dns_service, dns_hints, &result) == EAI_AGAIN);
    assert(dns_calls == (int)(sizeof allowed / sizeof *allowed) + 1);
    getaddrinfo_cache = old;
    puts("Update DNS: exact official updater host blocked; auth/GitHub/connman/other processes preserved: PASS");
}

// A pack file replaces its qrc node: data, flags and a content timestamp (the QML disk cache key).
static void test_resources(void) {
    char dir[] = "/tmp/hook-qrc-XXXXXX", path[64];
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/a.qml", dir);
    FILE *f = fopen(path, "w"); assert(f && fputs("Item {}", f) >= 0 && !fclose(f));
    static const uchar names[] = {0, 5, 0, 0, 0, 0, 0, 'a', 0, '.', 0, 'q', 0, 'm', 0, 'l'};
    static const uchar data[] = {0, 0, 0, 4, 's', 't', 'o', 'k'};
    uchar tree[44] = {0}; // rcc v3: node 0 = root dir (1 child, node 1), node 1 = zlib "a.qml" stamped 1
    tree[5] = FLAG_DIRECTORY; tree[9] = 1; tree[13] = 1;
    tree[27] = FLAG_COMPRESSED; tree[43] = 1;
    struct walk w = {.tree = tree, .names = names, .data = data, .node_size = 22};
    snprintf(w.pack, sizeof w.pack, "%s", dir);
    apply(&w, 0, "", 0);
    const uchar *e = tree + 22, *blob = data + be32(e + 10); // the offset wraps on 32-bit, as in Qt
    assert(w.replaced == 1 && !(be16(e + 4) & (FLAG_COMPRESSED | FLAG_ZSTD)));
    assert(be32(blob) == 7 && !memcmp(blob + 4, "Item {}", 7));
    uint64_t stamp = (uint64_t)be32(e + 14) << 32 | be32(e + 18);
    assert(stamp == content_stamp((const uchar *)"Item {}", 7) && stamp != 1);
    assert(stamp >= 1577836800000ULL && stamp < 4102444800000ULL);
    assert(content_stamp((const uchar *)"Item { }", 8) != stamp); // new text, new stamp
    unlink(path); rmdir(dir);
    puts("qrc: pack file replaces data, drops compression, stamps content for the QML cache: PASS");
}

static void test_patch_files(const char *filename) {
    FILE *f = fopen(filename, "rb"); assert(f);
    assert(!fseek(f, 0, SEEK_END)); long size = ftell(f); assert(size > 0);
    rewind(f); char *image = malloc((size_t)size); assert(image);
    assert(fread(image, 1, (size_t)size, f) == (size_t)size); fclose(f);
    ElfW(Ehdr) *header = (void *)image;
    struct dl_phdr_info info = {.dlpi_addr = (uintptr_t)image,
        .dlpi_phdr = (void *)(image + header->e_phoff), .dlpi_phnum = header->e_phnum};
    uintptr_t found = 0;
    engine_base(&info, sizeof info, &found);
    assert(found == (uintptr_t)image);
    const uchar *id = build->id;
    char *at = memmem(image, (size_t)size, id, 20); assert(at);
    *at ^= 1; found = 0;
    engine_base(&info, sizeof info, &found);
    assert(found == 0);
    *at ^= 1;
    for (size_t i = 0; i < build->edit_count; i++) {
        const struct engine_edit *e = &build->edits[i];
        assert(e->offset + e->size <= (size_t)size);
        int state = edit_state((uintptr_t)image, e);
        assert(state == 1 || state == 2);
        memcpy(image + e->offset, e->replacement, e->size);
        assert(edit_state((uintptr_t)image, e) == 2);
        image[e->offset] ^= 0xff;
        assert(edit_state((uintptr_t)image, e) == 0);
    }
    for (unsigned i = 0; i < DSP_HOOK_COUNT; i++) {
        size_t bytes;
        const void *expected = dsp_expected(i, &bytes);
        assert(dsp_hooks[i].offset + bytes <= (size_t)size);
        assert(!memcmp(image + dsp_hooks[i].offset, expected, bytes));
    }
    // #77: a model constructor stores the all-in-ones' 10 s zoom-out limit at config_zoom
    // (mov rX, #10 ... str rX, [r4, #config_zoom]).
    int zoom10 = 0;
    for (size_t a = 0; a + 36 <= (size_t)size && !zoom10; a += 4) {
        uint32_t x;
        memcpy(&x, image + a, 4);
        if ((x & 0xffff0fff) != 0xe3a0000a) continue;
        for (int k = 1; k <= 8 && !zoom10; k++) {
            uint32_t y;
            memcpy(&y, image + a + 4 * k, 4);
            zoom10 = y == (0xe5840000 | (x & 0xf000) | build->config_zoom);
        }
    }
    assert(zoom10);
    const uint32_t *api = NULL;
    // The data LOAD segment's file offset differs from its runtime address.
    for (unsigned i = 0; i < info.dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info.dlpi_phdr[i];
        if (p->p_type != PT_LOAD || build->sqlite_api < p->p_vaddr ||
            build->sqlite_api + 136 * sizeof *api > p->p_vaddr + p->p_filesz) continue;
        size_t offset = p->p_offset + build->sqlite_api - p->p_vaddr;
        assert(offset + 136 * sizeof *api <= (size_t)size);
        api = (const void *)(image + offset);
    }
    assert(api);
    assert(api[55] == build->sqlite_exec && api[70] == build->sqlite_open && api[135] == build->sqlite_open_v2);
    assert(!memcmp(image + build->sqlite_exec, build->sqlite_prefix[0], 8));
    if (build->sqlite_opendb) {
        const uint32_t *open = (const void *)(image + build->sqlite_open);
        assert(open[0] == 0xe3a03000 && open[1] == 0xe3a02006);
        assert(arm_branch_target((uintptr_t)&open[2]) == (uintptr_t)image + build->sqlite_opendb);
        assert(arm_branch_target((uintptr_t)image + build->sqlite_open_v2) == (uintptr_t)image + build->sqlite_opendb);
        assert(!memcmp(image + build->sqlite_opendb, build->sqlite_prefix[1], 8));
    } else {
        assert(!memcmp(image + build->sqlite_open, build->sqlite_prefix[1], 8));
        assert(!memcmp(image + build->sqlite_open_v2, build->sqlite_prefix[2], 8));
    }
    free(image);
    printf("Stock/patched/mismatched edit classification: %s PASS\n", filename);
}

// #55: Engine's own setpriority/sched_setscheduler on an analyzer keep it idle.
static void test_offline_analyzer(void) {
    int ready[2];
    assert(!pipe(ready));
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        prctl(PR_SET_NAME, "OfflineAnalyzer");
        char c = 1; assert(write(ready[1], &c, 1) == 1);
        pause();
        _exit(0);
    }
    char c; assert(read(ready[0], &c, 1) == 1);
    hook_test_engine = 1;
    struct sched_param other = {.sched_priority = 0};
    assert(!setpriority(PRIO_PROCESS, (id_t)child, 5) && getpriority(PRIO_PROCESS, (id_t)child) == 19);
    assert(!sched_setscheduler(child, SCHED_OTHER, &other) && sched_getscheduler(child) == SCHED_IDLE);
    // Anything else, and anything outside Engine, is left alone.
    assert(!setpriority(PRIO_PROCESS, 0, 3) && getpriority(PRIO_PROCESS, 0) == 3);
    hook_test_engine = 0;
    assert(!sched_setscheduler(child, SCHED_OTHER, &other) && sched_getscheduler(child) == SCHED_OTHER);
    kill(child, SIGKILL);
    assert(waitpid(child, NULL, 0) == child);
    setpriority(PRIO_PROCESS, 0, 0); // only effective as root; harmless otherwise
    puts("OfflineAnalyzer (#55): Engine's priority/policy changes on an analyzer stay idle, others untouched: PASS");
}

// #49: OfflineAnalyzer replays <file>.perf instead of analysing the copy.
static int oa_seen[16], oa_count;
static float oa_self[64]; // the runner: process() keeps its progress at +0x90
static char oa_payload[16][64];
static double oa_double[16];
static void mock_activate(void *o, const void *meta, int signal, void **args) {
    assert(o == (void *)oa_self && meta == oa_meta && oa_count < 16);
    if (args && (signal == OA_PROGRESS_SIG || signal == OA_DONE)) oa_double[oa_count] = *(double *)args[1];
    else if (args) {
        const struct oa_bytes *b = args[1];
        assert(b->size < 64 && *b->d >= 1);
        memcpy(oa_payload[oa_count], b->ptr, (size_t)b->size);
    } else assert(signal == OA_STARTED || signal == OA_STOPPED || signal == OA_FINISHED);
    oa_seen[oa_count++] = signal;
}
static void mock_make_bytes(struct oa_bytes *b, const char *data, int len) {
    b->d = malloc(16 + (size_t)len);
    b->d[0] = 1;
    b->ptr = (char *)b->d + 16;
    b->size = len;
    memcpy(b->ptr, data, (size_t)len);
}
static void test_shadow_replay(void) {
    oa_activate = mock_activate;
    oa_make_bytes = mock_make_bytes;
    oa_meta = (void *)&oa_seen;
    static const char dir[] = "/tmp/test-oa-replay";
    mkdir(dir, 0755);
    // "é" exercises the UTF-16 -> UTF-8 path conversion.
    static const uint16_t name[] = {'/','t','m','p','/','t','e','s','t','-','o','a','-','r','e','p','l','a','y','/',0xe9,'.','f','l','a','c'};
    struct oa_string qs = {NULL, name, sizeof name / sizeof *name};
    char track[44] = "TRACK", beats[5] = "BEATS", overview[27] = "OVERVIEW";
    double bpm = 123.456;
    uint32_t one = 1, lt = sizeof track, lb = sizeof beats, lo = sizeof overview;
    FILE *f = fopen("/tmp/test-oa-replay/\xc3\xa9.flac.perf", "wb");
    assert(f);
    fwrite("EXPF", 1, 4, f); fwrite(&one, 4, 1, f); fwrite(&bpm, 8, 1, f);
    fwrite(&lt, 4, 1, f); fwrite(track, 1, lt, f);
    fwrite(&lb, 4, 1, f); fwrite(beats, 1, lb, f);
    fwrite(&lo, 4, 1, f); fwrite(overview, 1, lo, f);
    fclose(f);
    uchar prefs[13] = {0};
    assert(oa_replay(oa_self, &qs, prefs));
    static const int want[] = {OA_STARTED, OA_TRACK, OA_BEATS, OA_OVERVIEW, OA_OVERVIEW_RGB, OA_PROGRESS_SIG, OA_TRACK, OA_DONE, OA_FINISHED};
    assert(oa_count == 9 && !memcmp(oa_seen, want, sizeof want));
    assert(!strcmp(oa_payload[1], "TRACK") && !strcmp(oa_payload[2], "BEATS") && !strcmp(oa_payload[4], "OVERVIEW"));
    assert(oa_double[5] == 1.0 && oa_self[OA_PROGRESS / 4] == 1.0f && oa_double[7] == (double)(float)bpm);
    // stopBeatDetection while the replay waits for Engine's loader: stopped, finished.
    oa_count = 0;
    ((uchar *)oa_self)[OA_STOP] = 1;
    assert(oa_replay(oa_self, &qs, prefs) && oa_count == 3 && oa_seen[0] == OA_STARTED && oa_seen[1] == OA_STOPPED && oa_seen[2] == OA_FINISHED);
    ((uchar *)oa_self)[OA_STOP] = 0;
    // prefs byte 12 set: the stock analysis sends no grid and no 3-band overview.
    oa_count = 0;
    prefs[12] = 1;
    assert(oa_replay(oa_self, &qs, prefs) && oa_count == 7 && oa_seen[2] == OA_OVERVIEW_RGB);
    // A corrupt or missing .perf falls through to the stock analysis.
    f = fopen("/tmp/test-oa-replay/\xc3\xa9.flac.perf", "r+b"); assert(f); fwrite("XXXX", 1, 4, f); fclose(f);
    oa_count = 0;
    assert(!oa_replay(oa_self, &qs, prefs) && oa_count == 0);
    unlink("/tmp/test-oa-replay/\xc3\xa9.flac.perf");
    assert(!oa_replay(oa_self, &qs, prefs) && oa_count == 0);
    // Engine analyses its own copy of a /data file: the shadow copy's .perf
    // with that name and size is used; another size is a different file.
    static const uint16_t copy[] = {'/','t','m','p','/','t','e','s','t','-','o','a','-','r','e','p','l','a','y','/',
        'c','o','p','i','e','s','/','a','b','-','c','d','_','x','_','y','.','m','p','3'};
    struct oa_string cs = {NULL, copy, sizeof copy / sizeof *copy};
    mkdir("/tmp/test-oa-replay/copies", 0755);
    mkdir("/tmp/test-oa-replay/shadow-v3", 0755);
    mkdir("/tmp/test-oa-replay/shadow-v3/peer-1", 0755);
    mkdir("/tmp/test-oa-replay/shadow-v3/peer-1/Contents", 0755);
    f = fopen("/tmp/test-oa-replay/copies/ab-cd_x_y.mp3", "wb"); assert(f); fputs("AUDIO", f); fclose(f);
    f = fopen("/tmp/test-oa-replay/shadow-v3/peer-1/Contents/x_y.mp3", "wb"); assert(f); fputs("AUDIOX", f); fclose(f);
    f = fopen("/tmp/test-oa-replay/shadow-v3/peer-1/Contents/x_y.mp3.perf", "wb"); assert(f);
    fwrite("EXPF", 1, 4, f); fwrite(&one, 4, 1, f); fwrite(&bpm, 8, 1, f);
    fwrite(&lt, 4, 1, f); fwrite(track, 1, lt, f);
    fwrite(&lb, 4, 1, f); fwrite(beats, 1, lb, f);
    fwrite(&lo, 4, 1, f); fwrite(overview, 1, lo, f);
    fclose(f);
    oa_count = 0;
    assert(!oa_replay(oa_self, &cs, prefs) && oa_count == 0);
    f = fopen("/tmp/test-oa-replay/shadow-v3/peer-1/Contents/x_y.mp3", "wb"); assert(f); fputs("AUDIO", f); fclose(f);
    prefs[12] = 0;
    assert(oa_replay(oa_self, &cs, prefs) && oa_count == 9 && oa_seen[2] == OA_BEATS);
    unlink("/tmp/test-oa-replay/shadow-v3/peer-1/Contents/x_y.mp3.perf");
    unlink("/tmp/test-oa-replay/shadow-v3/peer-1/Contents/x_y.mp3");
    unlink("/tmp/test-oa-replay/copies/ab-cd_x_y.mp3");
    rmdir("/tmp/test-oa-replay/shadow-v3/peer-1/Contents");
    rmdir("/tmp/test-oa-replay/shadow-v3/peer-1");
    rmdir("/tmp/test-oa-replay/shadow-v3");
    rmdir("/tmp/test-oa-replay/copies");
    rmdir(dir);
    unlink(OA_LOG);
    puts("OfflineAnalyzer (#49): <file>.perf is replayed as the stock signals, bad or missing ones fall through: PASS");
}

static void test_stems_processor_fallback(void) {
    hook_test_engine = 1;
    if (access("/usr/local/bin/stems-processor", X_OK) != 0) {
        assert(!strcmp(stems_processor_path("/usr/local/bin/stems-processor"), "/usr/local/bin/stems-processor"));
        system("mkdir -p /data/sc5000-custom/bundle 2>/dev/null");
        const char *bundle_file = "/data/sc5000-custom/bundle/stems-processor";
        FILE *f = fopen(bundle_file, "w");
        if (f) {
            fputs("#!/bin/sh\nexit 0\n", f);
            fclose(f);
            chmod(bundle_file, 0755);
            assert(!strcmp(stems_processor_path("/usr/local/bin/stems-processor"), bundle_file));
            assert(access("/usr/local/bin/stems-processor", X_OK) == 0);
            unlink(bundle_file);
        }
    }
    assert(!strcmp(stems_processor_path("/bin/sh"), "/bin/sh"));
    hook_test_engine = 0;
    puts("stems-processor fallback: redirects to /data/sc5000-custom/bundle/stems-processor when absent: PASS");
}

static void test_alsa_boot_hook(void) {
    hook_test_engine = 1;
    g_hook_initialized = 1;
    void *dst = (void *)0x1234;
    assert(sc5000_alsa_boot_hook(NULL, NULL, &dst, NULL) == 0);
    assert(dst == NULL);
    dst = (void *)0x1234;
    assert(snd_config_hook_sc5000_alsa_boot_hook(NULL, NULL, &dst, NULL) == 0);
    assert(dst == NULL);

    g_hook_initialized = 0;
    system("mkdir -p /run 2>/dev/null; touch /run/sc5000-alsa-reexec.once");
    assert(sc5000_alsa_boot_hook(NULL, NULL, &dst, NULL) == 0);
    unlink("/run/sc5000-alsa-reexec.once");

    g_hook_initialized = 1;
    hook_test_engine = 0;
    puts("sc5000_alsa_boot_hook: no-op when initialized or reexec marker exists, resets dst: PASS");
}

int main(int argc, char **argv) {
    for (size_t b = 0; b < sizeof engine_builds / sizeof *engine_builds; b++) {
        // Hooks and counters are process-global; each build starts pristine.
        fflush(NULL);
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            select_build(&engine_builds[b]);
            printf("Engine %s\n", build->name);
            test_dsp_hooks();
            test_stems();
            test_config();
            test_sqlite();
            exit(0);
        }
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    test_files();
    test_threads();
    test_alsa();
    test_update_dns();
    test_resources();
    test_offline_analyzer();
    test_shadow_replay();
    test_stems_processor_fallback();
    test_alsa_boot_hook();
    for (int i = 1; i < argc; i++) test_patch_files(argv[i]);
    return 0;
}
