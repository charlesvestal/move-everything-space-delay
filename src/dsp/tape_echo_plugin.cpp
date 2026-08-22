/* Tape Echo 2 — Schwung audio_fx module for Ableton Move.
 *
 * A thin audio_fx_api_v2 shell around duskaudio::TapeEchoDSP, the
 * framework-free component-modeled three-head tape echo + spring reverb from
 * Tape Echo 2 by Dusk Audio (GPL-3.0). The vendored core in src/ported/ is
 * unmodified upstream; everything Move-specific lives in this file.
 *
 * Tempo sync follows the reference DPF shell: the selected Echo Rate detent
 * is mapped through the leading playback head's note table, converted to a
 * head-1 motor time at the host BPM, and CLAMPED (never octave-folded) to the
 * measured motor range. The core's motor-inertia smoother turns tempo changes
 * into tape-style glides.
 *
 * Realtime rules: process_block never allocates, never logs, never touches
 * the filesystem. Parameter setters are atomic stores; the core snapshots
 * them once per block.
 */

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "../host/plugin_api_v1.h"
#include "../host/audio_fx_api_v2.h"
#include "../ported/core/TapeEchoDSP.hpp"   /* pulls in TapeEchoParams.hpp */
#include "te2_params.h"

static const host_api_v1_t *g_host = nullptr;

static void te2_log(const char *msg)
{
    if (g_host && g_host->log) g_host->log(msg);
}

/* ------------------------------------------------------------------ */
/* Instance                                                            */
/* ------------------------------------------------------------------ */

struct te2_instance {
    duskaudio::TapeEchoDSP dsp;
    std::atomic<float> values[TE2_PARAM_COUNT];

    float bufL[MOVE_FRAMES_PER_BLOCK];
    float bufR[MOVE_FRAMES_PER_BLOCK];

    /* serve buffers (control thread only) */
    char chain_buf[8192];
    char state_buf[1536];
};

static float te2_clamp(const te2_param_t *p, float v)
{
    if (v < p->min) v = p->min;
    if (v > p->max) v = p->max;
    return v;
}

static int te2_param_index(const char *key)
{
    for (int i = 0; i < TE2_PARAM_COUNT; i++)
        if (!strcmp(te2_params[i].key, key))
            return i;
    return -1;
}

/* Push one shell value into the DSP core. RATE is deliberately absent:
 * process_block owns the motor speed every block (knob or tempo sync). */
static void te2_push(te2_instance *inst, int idx, float v)
{
    duskaudio::TapeEchoDSP &d = inst->dsp;
    switch (idx) {
    case TE2_P_MODE:        d.setMode((int)(v + 0.5f) + 1);          break;
    case TE2_P_INTENSITY:   d.setIntensity(v);                       break;
    case TE2_P_ECHO_VOL:    d.setEchoLevel(v);                       break;
    case TE2_P_ECHO_PAN:    d.setEchoPan(v);                         break;
    case TE2_P_SEND:        d.setInputSend(v > 0.5f);                break;
    case TE2_P_INPUT:       d.setInputGain(v);                       break;
    case TE2_P_BASS:        d.setBass(v);                            break;
    case TE2_P_TREBLE:      d.setTreble(v);                          break;
    case TE2_P_WOW:         d.setWowFlutter(v);                      break;
    case TE2_P_AGE:         d.setTapeAge(v * 0.5f);                  break;
    case TE2_P_REVERB_VOL:  d.setReverbLevel(v);                     break;
    case TE2_P_REVERB_PAN:  d.setReverbPan(v);                       break;
    case TE2_P_MIX:         d.setMix(v);                             break;
    case TE2_P_PINGPONG:    d.setPingPong(v > 0.5f);                 break;
    case TE2_P_WIDTH:       d.setStereoWidth(v * 0.01f);             break;
    case TE2_P_OUTPUT_VOL:  d.setOutputVolume(v);                    break;
    case TE2_P_POWER:       d.setBypass(v < 0.5f);                   break;
    case TE2_P_DRY:         d.setDryLevel(v);                        break;
    default: break; /* RATE / SYNC / NOTE / PRESET are shell-level */
    }
}

static void te2_set_index(te2_instance *inst, int idx, float v)
{
    v = te2_clamp(&te2_params[idx], v);
    inst->values[idx].store(v, std::memory_order_relaxed);
    te2_push(inst, idx, v);
}

/* Factory programs, straight from the vendored kFactoryPresets. The stored
 * semantic division is converted to the physical detent through the preset
 * mode's leading head, exactly as the reference shell's loadProgram does. */
static void te2_apply_preset(te2_instance *inst, int presetIdx)
{
    if (presetIdx < 0 || presetIdx >= kNumFactoryPresets) return;
    const TapeEchoPreset &p = kFactoryPresets[presetIdx];

    const int mode1to12 = (int)(p.v[kParamMode] + 0.5f);
    te2_set_index(inst, TE2_P_MODE,       (float)(mode1to12 - 1));
    te2_set_index(inst, TE2_P_RATE,       p.v[kParamRepeatRate]);
    te2_set_index(inst, TE2_P_INTENSITY,  p.v[kParamIntensity]);
    te2_set_index(inst, TE2_P_ECHO_VOL,   p.v[kParamEchoLevel]);
    te2_set_index(inst, TE2_P_REVERB_VOL, p.v[kParamReverbLevel]);
    te2_set_index(inst, TE2_P_BASS,       p.v[kParamBass]);
    te2_set_index(inst, TE2_P_TREBLE,     p.v[kParamTreble]);
    te2_set_index(inst, TE2_P_INPUT,      p.v[kParamInputGain]);
    te2_set_index(inst, TE2_P_WOW,        p.v[kParamWowFlutter]);
    te2_set_index(inst, TE2_P_DRY,        p.v[kParamDryLevel]);
    te2_set_index(inst, TE2_P_SYNC,       p.v[kParamTempoSync]);

    const int division = (int)(p.v[kParamSyncDivision] + 0.5f);
    const int detent = teSyncKnobPosForDivision(
        division, teLeadingHeadIndexForMode(mode1to12)) + 1;
    te2_set_index(inst, TE2_P_NOTE, (float)detent);

    const float age = teQuantizeTapeAge(p.v[kParamTapeAge]);
    te2_set_index(inst, TE2_P_AGE, age < 0.25f ? 0.0f : (age < 0.75f ? 1.0f : 2.0f));

    te2_set_index(inst, TE2_P_OUTPUT_VOL, p.outputVolume);
    te2_set_index(inst, TE2_P_ECHO_PAN,   p.echoPan);
    te2_set_index(inst, TE2_P_REVERB_PAN, p.reverbPan);
    te2_set_index(inst, TE2_P_SEND,       p.inputSend);
    te2_set_index(inst, TE2_P_MIX,        p.mix);

    inst->values[TE2_P_PRESET].store((float)presetIdx, std::memory_order_relaxed);
}

static int te2_json_number(const char *json, const char *key, float *out);

/* ------------------------------------------------------------------ */
/* TapeDelay (schwung-space-delay) preset compatibility                */
/*                                                                     */
/* That module saves its patch state as                                */
/*   {"time":400,"feedback":0.4,"mix":0.35,"tone":0.6,                 */
/*    "stereo_width":0,"division":"free","bpm":120}                    */
/* and its own set_param takes the same keys. We accept both spellings */
/* so an old patch, or anything replaying its parameters, lands on the */
/* nearest honest Tape Echo setting instead of being dropped.          */
/* ------------------------------------------------------------------ */

/* time -> the head that can actually reach it, plus the motor speed.
 * The transport only spans 69.33..177.354 ms on head 1; heads 2 and 3
 * multiply that by 1.91172 / 2.76118, so between them they cover roughly
 * 69..490 ms. Anything outside pins at the nearest end, which is what the
 * hardware does with an out-of-range request. */
static void te2_legacy_time(te2_instance *inst, float ms)
{
    int best = 0;
    float bestErr = 1e9f;
    float bestBase = 0.0f;
    for (int h = 0; h < 3; h++) {
        const float ratio  = duskaudio::TapeEchoDSP::kHeadRatio[h];
        const float offset = duskaudio::TapeEchoDSP::kHeadOffsetMs[h];
        float base = (ms - offset) / ratio;
        if (base < duskaudio::TapeEchoDSP::kMinDelayMs) base = duskaudio::TapeEchoDSP::kMinDelayMs;
        if (base > duskaudio::TapeEchoDSP::kMaxDelayMs) base = duskaudio::TapeEchoDSP::kMaxDelayMs;
        const float got = base * ratio + offset;
        const float err = got > ms ? got - ms : ms - got;
        if (err < bestErr) { bestErr = err; best = h; bestBase = base; }
    }
    te2_set_index(inst, TE2_P_MODE, (float)best);          /* H1 / H2 / H3 */
    te2_set_index(inst, TE2_P_RATE,
                  duskaudio::TapeEchoDSP::repeatRateForDelayMs(bestBase));
}

/* feedback -> intensity, CALIBRATED BY DECAY RATE.
 *
 * This was `v * 0.75f`, on the reasoning that TapeDelay's 0..1 is a loop gain
 * that always decays while Tape Echo self-oscillates past about 0.75. The
 * ceiling is right — measured, this engine hits sustain at intensity ~0.74 —
 * but a flat scale only lands at the top of the range. TapeDelay's feedback
 * is a plain loop gain; this engine's loop also carries record EQ, tape
 * saturation and head losses, so the same nominal gain decays faster, and it
 * gets proportionally worse the lower you go.
 *
 * So the pairs below are measured, not derived: for each old feedback value,
 * the intensity whose repeat train loses the same dB per repeat. (Rendered an
 * impulse through both engines and least-squares fitted the peak envelope;
 * tools/calibrate_legacy_feedback.cpp regenerates the whole table.)
 *
 * IT IS PER HEAD, and that is not a refinement — a single table fitted on head
 * 3 is 11.5 dB per repeat wrong on head 1, which is most of the decay. Head 1
 * needs intensity 0.55 where head 3 needs 0.44 for the same feedback 0.4.
 * Heads 2 and 3 are close to each other; head 1 is the outlier. The importer
 * only ever selects a single head (te2_legacy_time picks the one that reaches
 * the stored time), so three columns cover every patch that can arrive — the
 * multi-head modes are not reachable through this path and are not fitted.
 *
 * Read the head from the instance rather than taking it as an argument:
 * te2_try_legacy_state calls te2_legacy_time first so the mode is already
 * right, and on the one-key-at-a-time path there is no ordering guarantee at
 * all, so the current mode is the best information available either way.
 *
 * Resolution is the 0.01 sweep grid. The 0.68 ceiling on head 1 from feedback
 * 0.85 up is the engine's sustain threshold: past there it cannot decay slowly
 * enough to match, and running past it would turn an old preset into a siren. */
static void te2_legacy_feedback(te2_instance *inst, float v)
{
    /* { old feedback, intensity for H1, for H2, for H3 } */
    static const struct { float fb, i[3]; } kMap[] = {
        { 0.00f, { 0.00f, 0.00f, 0.00f } }, { 0.05f, { 0.34f, 0.14f, 0.15f } },
        { 0.10f, { 0.40f, 0.19f, 0.21f } }, { 0.15f, { 0.44f, 0.23f, 0.25f } },
        { 0.20f, { 0.47f, 0.29f, 0.31f } }, { 0.25f, { 0.49f, 0.32f, 0.34f } },
        { 0.30f, { 0.52f, 0.35f, 0.37f } }, { 0.35f, { 0.53f, 0.39f, 0.41f } },
        { 0.40f, { 0.55f, 0.41f, 0.44f } }, { 0.45f, { 0.56f, 0.44f, 0.47f } },
        { 0.50f, { 0.57f, 0.46f, 0.49f } }, { 0.55f, { 0.59f, 0.48f, 0.51f } },
        { 0.60f, { 0.60f, 0.49f, 0.53f } }, { 0.65f, { 0.61f, 0.50f, 0.55f } },
        { 0.70f, { 0.62f, 0.52f, 0.57f } }, { 0.75f, { 0.65f, 0.53f, 0.58f } },
        { 0.80f, { 0.67f, 0.55f, 0.60f } }, { 0.85f, { 0.68f, 0.56f, 0.62f } },
        { 0.90f, { 0.68f, 0.57f, 0.67f } }, { 1.00f, { 0.68f, 0.59f, 0.68f } },
    };
    const int n = (int)(sizeof kMap / sizeof kMap[0]);

    /* mode enum 0..11 -> DSP mode 1..12 -> leading head 0..2 */
    const int mode = (int)(inst->values[TE2_P_MODE].load(std::memory_order_relaxed) + 0.5f) + 1;
    int h = teLeadingHeadIndexForMode(mode);
    if (h < 0) h = 0;
    if (h > 2) h = 2;

    if (v <= kMap[0].fb)   { te2_set_index(inst, TE2_P_INTENSITY, kMap[0].i[h]); return; }
    if (v >= kMap[n-1].fb) { te2_set_index(inst, TE2_P_INTENSITY, kMap[n-1].i[h]); return; }
    for (int k = 1; k < n; k++) {
        if (v > kMap[k].fb) continue;
        const float span = kMap[k].fb - kMap[k-1].fb;
        const float t = span > 0.0f ? (v - kMap[k-1].fb) / span : 0.0f;
        te2_set_index(inst, TE2_P_INTENSITY,
                      kMap[k-1].i[h] + t * (kMap[k].i[h] - kMap[k-1].i[h]));
        return;
    }
}

/* mix -> echo volume.
 *
 * TapeDelay's Mix was a crossfade, dry*(1-m) + wet*m, so the wet-to-dry ratio
 * it produced was m/(1-m). This engine's Mix is a unity-overlap law —
 * dryMixGain = min(1, 2(1-m)), wetMixGain = min(1, 2m), both full at noon — so
 * without a correction the repeats arrive quieter, relative to the dry, than
 * they were.
 *
 * The correction is NOT that old ratio. Echo Volume multiplies the wet path on
 * top of a law that already has its own ratio, so what is wanted is the
 * quotient of the two:
 *
 *     E = [m/(1-m)]  /  [min(1,2m) / min(1,2(1-m))]
 *
 * which is 1/(2(1-m)) below noon and 2m above it. Setting E to the old ratio
 * directly — which this did in 1.3.4 — is only correct at m = 0.5, where the
 * new law's own ratio happens to be 1. It shipped, and at m = 0.2 it landed
 * 16.7 dB light. Measured, not spotted by reading it.
 *
 * Above noon the requested value exceeds 1 and clamps: this engine's law is
 * already fading the dry there, which covers part of it but not all — a wet-
 * heavy old patch still comes back a few dB shy. That one is a ceiling, not a
 * mistake. */
static void te2_legacy_echo_volume_from_mix(te2_instance *inst, float mix)
{
    if (mix < 0.0f) mix = 0.0f;
    if (mix > 1.0f) mix = 1.0f;
    float e = (mix <= 0.5f) ? 1.0f / (2.0f * (1.0f - mix)) : 2.0f * mix;
    if (e > 1.0f) e = 1.0f;
    if (e < 0.0f) e = 0.0f;
    te2_set_index(inst, TE2_P_ECHO_VOL, e);
}

/* tone -> treble. TapeDelay's tone is a one-pole lowpass swept 500 Hz..12 kHz;
 * the nearest control here is the echo-path treble shelf, centred at its
 * midpoint. */
static void te2_legacy_tone(te2_instance *inst, float v)
{
    te2_set_index(inst, TE2_P_TREBLE, v * 2.0f - 1.0f);
}

/* division -> tempo sync + the nearest physical Echo Rate detent for whichever
 * head is leading. "free" just turns sync off. */
static void te2_legacy_division(te2_instance *inst, const char *name)
{
    static const struct { const char *name; double beats; } kDiv[] = {
        { "1/1", 4.0 }, { "1/2", 2.0 }, { "1/2d", 3.0 }, { "1/4", 1.0 },
        { "1/4d", 1.5 }, { "1/4t", 2.0 / 3.0 }, { "1/8", 0.5 }, { "1/8d", 0.75 },
        { "1/8t", 1.0 / 3.0 }, { "1/16", 0.25 }, { "1/16t", 1.0 / 6.0 },
    };
    if (!strcmp(name, "free") || !strcmp(name, "0")) {
        te2_set_index(inst, TE2_P_SYNC, 0.0f);
        return;
    }
    double beats = 0.0;
    for (size_t i = 0; i < sizeof kDiv / sizeof kDiv[0]; i++)
        if (!strcmp(kDiv[i].name, name)) { beats = kDiv[i].beats; break; }
    if (beats <= 0.0) return;                       /* unknown spelling */

    const int mode = (int)(inst->values[TE2_P_MODE].load(std::memory_order_relaxed) + 0.5f) + 1;
    const int head = teLeadingHeadIndexForMode(mode);
    int bestPos = 0;
    double bestErr = 1e9;
    for (int pos = 0; pos < kNumSyncKnobPositions; pos++) {
        const double b = kSyncDivisions[teDivisionForSyncKnobPos(pos, head)].beats;
        const double err = b > beats ? b - beats : beats - b;
        if (err < bestErr) { bestErr = err; bestPos = pos; }
    }
    te2_set_index(inst, TE2_P_SYNC, 1.0f);
    te2_set_index(inst, TE2_P_NOTE, (float)(bestPos + 1));
}

/* Pull a JSON string value out of a flat object. */
static int te2_json_string(const char *json, const char *k, char *out, size_t cap)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":", k);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n < cap - 1) out[n++] = *p++;
    out[n] = 0;
    return 0;
}

/* Returns 1 if the blob was a TapeDelay state and has been applied. */
static int te2_try_legacy_state(te2_instance *inst, const char *val)
{
    float v;
    /* "time" plus "feedback" is the signature; a Tape Echo blob has neither. */
    if (te2_json_number(val, "time", &v) != 0) return 0;
    float fb;
    if (te2_json_number(val, "feedback", &fb) != 0) return 0;

    te2_legacy_time(inst, v);
    te2_legacy_feedback(inst, fb);
    if (te2_json_number(val, "mix", &v) == 0) {
        te2_set_index(inst, TE2_P_MIX, v);
        te2_legacy_echo_volume_from_mix(inst, v);
    }
    if (te2_json_number(val, "tone", &v) == 0) te2_legacy_tone(inst, v);
    char div[24];
    if (te2_json_string(val, "division", div, sizeof div) == 0)
        te2_legacy_division(inst, div);
    /* stereo_width used to have no counterpart and was dropped. Now that the
     * ping-pong widener exists it maps directly, and any non-zero width arms
     * ping-pong — matching TapeDelay, where width IS what makes its two
     * cross-fed lines alternate. */
    if (te2_json_number(val, "stereo_width", &v) == 0) {
        te2_set_index(inst, TE2_P_WIDTH, v);
        te2_set_index(inst, TE2_P_PINGPONG, v > 0.0f ? 1.0f : 0.0f);
    }
    te2_log("tape-echo2: imported a TapeDelay preset");
    return 1;
}

/* ------------------------------------------------------------------ */
/* v2 entry points                                                     */
/* ------------------------------------------------------------------ */

static void *te2_create_instance(const char * /*module_dir*/,
                                 const char * /*config_json*/)
{
    auto *inst = new (std::nothrow) te2_instance();
    if (!inst) return nullptr;

    const int sr     = g_host ? g_host->sample_rate     : MOVE_SAMPLE_RATE;
    const int frames = g_host ? g_host->frames_per_block : MOVE_FRAMES_PER_BLOCK;
    inst->dsp.prepare((double)sr,
                      frames > MOVE_FRAMES_PER_BLOCK ? frames : MOVE_FRAMES_PER_BLOCK);
    inst->dsp.reset();

    for (int i = 0; i < TE2_PARAM_COUNT; i++)
        te2_set_index(inst, i, te2_params[i].def);

    te2_log("tape-echo2: instance created");
    return inst;
}

static void te2_destroy_instance(void *instance)
{
    delete (te2_instance *)instance;
}

static void te2_process_block(void *instance, int16_t *audio_inout, int frames)
{
    auto *inst = (te2_instance *)instance;
    if (!inst || !audio_inout || frames <= 0) return;

    /* Motor speed, once per block: tempo sync wins, otherwise the knob. */
    if (inst->values[TE2_P_SYNC].load(std::memory_order_relaxed) > 0.5f) {
        double bpm = (g_host && g_host->get_bpm) ? (double)g_host->get_bpm() : 120.0;
        const int mode = (int)(inst->values[TE2_P_MODE].load(std::memory_order_relaxed) + 0.5f) + 1;
        const double ratio  = duskaudio::TapeEchoDSP::leadingHeadRatioForMode(mode);
        const double offset = duskaudio::TapeEchoDSP::leadingHeadOffsetMsForMode(mode);
        const int division = teDivisionForSyncKnobPos(
            (int)(inst->values[TE2_P_NOTE].load(std::memory_order_relaxed) + 0.5f) - 1,
            teLeadingHeadIndexForMode(mode));
        const double requestedHead1Ms = (syncDelayMs(bpm, division) - offset) / ratio;
        /* CLAMP, do not octave-fold — the hardware pins out-of-range notes at
         * the motor limit (see the reference shell for the measurements). */
        double clamped = requestedHead1Ms;
        if (clamped < (double)duskaudio::TapeEchoDSP::kMinDelayMs)
            clamped = (double)duskaudio::TapeEchoDSP::kMinDelayMs;
        if (clamped > (double)duskaudio::TapeEchoDSP::kMaxDelayMs)
            clamped = (double)duskaudio::TapeEchoDSP::kMaxDelayMs;
        inst->dsp.setRepeatRate(
            duskaudio::TapeEchoDSP::repeatRateForDelayMs((float)clamped));
    } else {
        inst->dsp.setRepeatRate(
            inst->values[TE2_P_RATE].load(std::memory_order_relaxed));
    }

    int16_t *p = audio_inout;
    while (frames > 0) {
        const int n = frames > MOVE_FRAMES_PER_BLOCK ? MOVE_FRAMES_PER_BLOCK : frames;

        for (int i = 0; i < n; i++) {
            inst->bufL[i] = (float)p[i * 2]     * (1.0f / 32768.0f);
            inst->bufR[i] = (float)p[i * 2 + 1] * (1.0f / 32768.0f);
        }

        const float *ins[2] = { inst->bufL, inst->bufR };
        float *outs[2]      = { inst->bufL, inst->bufR };
        inst->dsp.processBlock(ins, outs, 2, n);

        for (int i = 0; i < n; i++) {
            float l = inst->bufL[i] * 32767.0f;
            float r = inst->bufR[i] * 32767.0f;
            if (l > 32767.0f) l = 32767.0f; else if (l < -32768.0f) l = -32768.0f;
            if (r > 32767.0f) r = 32767.0f; else if (r < -32768.0f) r = -32768.0f;
            p[i * 2]     = (int16_t)l;
            p[i * 2 + 1] = (int16_t)r;
        }

        p += n * 2;
        frames -= n;
    }
}

/* ------------------------------------------------------------------ */
/* set / get                                                           */
/* ------------------------------------------------------------------ */

static int te2_enum_index(const te2_param_t *prm, const char *val)
{
    for (int i = 0; i < prm->n_options; i++)
        if (!strcmp(prm->options[i], val))
            return i;
    /* case-insensitive fallback */
    for (int i = 0; i < prm->n_options; i++) {
        const char *a = prm->options[i], *b = val;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*a && !*b) return i;
    }
    return -1;
}

static int te2_json_number(const char *json, const char *key, float *out)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    *out = (float)atof(p);
    return 0;
}

static void te2_set_param(void *instance, const char *key, const char *val)
{
    auto *inst = (te2_instance *)instance;
    if (!inst || !key || !val) return;

    /* the chain host may deliver keys with a component prefix */
    const char *colon = strrchr(key, ':');
    if (colon) key = colon + 1;

    if (!strcmp(key, "state")) {
        float v;
        if (te2_json_number(val, "te2", &v) != 0) {
            /* Not ours — it may still be a TapeDelay patch worth importing. */
            te2_try_legacy_state(inst, val);
            return;
        }
        for (int i = 0; i < TE2_PARAM_COUNT; i++) {
            if (i == TE2_P_PRESET) continue; /* a launcher, not state */
            if (te2_json_number(val, te2_params[i].key, &v) == 0)
                te2_set_index(inst, i, v);
        }
        return;
    }

    /* TapeDelay parameter names, for anything that replays them one at a time.
     * "mix" and "stereo_width" are spelled the same in both and need no alias
     * — stereo_width now drives the real ping-pong widener.
     *
     * Deliberately NOT the echo-volume-from-mix correction the state path
     * applies: "mix" is also this engine's own parameter, so there is no way
     * to tell an old module replaying its patch from a user turning the Mix
     * knob, and moving Echo Volume underneath the latter would be indefensible.
     * A whole state blob is unambiguous; a single key is not. */
    if (!strcmp(key, "time"))         { te2_legacy_time(inst, (float)atof(val)); return; }
    if (!strcmp(key, "feedback"))     { te2_legacy_feedback(inst, (float)atof(val)); return; }
    if (!strcmp(key, "tone"))         { te2_legacy_tone(inst, (float)atof(val)); return; }
    if (!strcmp(key, "division"))     { te2_legacy_division(inst, val); return; }

    const int idx = te2_param_index(key);
    if (idx < 0) return;
    const te2_param_t *prm = &te2_params[idx];

    float v;
    if (prm->type == TE2_ENUM) {
        const int oi = te2_enum_index(prm, val);
        v = oi >= 0 ? (float)oi : (float)atof(val);
    } else {
        v = (float)atof(val);
    }

    if (idx == TE2_P_PRESET) {
        te2_apply_preset(inst, (int)(te2_clamp(prm, v) + 0.5f));
        return;
    }
    te2_set_index(inst, idx, v);
}

static int te2_write_str(char *buf, int buf_len, const char *s)
{
    int n = (int)strlen(s);
    if (n >= buf_len) n = buf_len - 1;
    memcpy(buf, s, (size_t)n);
    buf[n] = 0;
    return n;
}

/* The knobs the root page offers, in order. Move has 8 encoders, and the
 * chain UI shows exactly the "knobs" list — the rest stay reachable through
 * the full "params" array (and through movy's own pages). */
static const char *const te2_root_knobs[8] = {
    "mode", "repeat_rate", "intensity", "echo_volume",
    "reverb_volume", "mix", "tempo_sync", "echo_rate_note",
};

/* Writes the parameter array shared by chain_params and ui_hierarchy.
 * Returns bytes written, or -1 if it would not fit. */
static int te2_write_param_array(char *o, size_t cap)
{
    size_t w = 0;
    w += (size_t)snprintf(o + w, cap - w, "[");
    for (int i = 0; i < TE2_VISIBLE_PARAM_COUNT; i++) {
        const te2_param_t *p = &te2_params[i];
        if (i) w += (size_t)snprintf(o + w, cap - w, ",");
        w += (size_t)snprintf(o + w, cap - w,
                              "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\"",
                              p->key, p->name,
                              p->type == TE2_ENUM ? "enum"
                              : p->type == TE2_INT ? "int" : "float");
        if (p->type == TE2_ENUM) {
            w += (size_t)snprintf(o + w, cap - w, ",\"options\":[");
            for (int j = 0; j < p->n_options; j++)
                w += (size_t)snprintf(o + w, cap - w, "%s\"%s\"",
                                      j ? "," : "", p->options[j]);
            w += (size_t)snprintf(o + w, cap - w, "],\"default\":\"%s\"}",
                                  p->options[(int)(p->def + 0.5f)]);
        } else if (p->type == TE2_INT) {
            w += (size_t)snprintf(o + w, cap - w,
                                  ",\"min\":%d,\"max\":%d,\"default\":%d}",
                                  (int)p->min, (int)p->max, (int)p->def);
        } else {
            w += (size_t)snprintf(o + w, cap - w,
                                  ",\"min\":%g,\"max\":%g,\"default\":%g}",
                                  (double)p->min, (double)p->max, (double)p->def);
        }
        if (w >= cap - 2) return -1;
    }
    w += (size_t)snprintf(o + w, cap - w, "]");
    if (w >= cap) return -1;
    return (int)w;
}

static int te2_serve_chain_params(te2_instance *inst, char *buf, int buf_len)
{
    if (te2_write_param_array(inst->chain_buf, sizeof inst->chain_buf) < 0)
        return -1;
    return te2_write_str(buf, buf_len, inst->chain_buf);
}

/* Full hierarchy: EVERY parameter, with the 8 encoder assignments up front.
 * Built from the same table as chain_params so the two can never disagree —
 * an earlier hand-written version advertised only 4 params and hid the other
 * fifteen from anything that reads the hierarchy. */
static int te2_serve_ui_hierarchy(te2_instance *inst, char *buf, int buf_len)
{
    char *o = inst->chain_buf;
    const size_t cap = sizeof inst->chain_buf;
    size_t w = (size_t)snprintf(o, cap,
        "{\"modes\":null,\"levels\":{\"root\":{\"children\":null,\"knobs\":[");
    for (int i = 0; i < 8; i++)
        w += (size_t)snprintf(o + w, cap - w, "%s\"%s\"", i ? "," : "", te2_root_knobs[i]);
    w += (size_t)snprintf(o + w, cap - w, "],\"params\":");
    if (w >= cap - 2) return -1;
    const int n = te2_write_param_array(o + w, cap - w);
    if (n < 0) return -1;
    w += (size_t)n;
    w += (size_t)snprintf(o + w, cap - w, "}}}");
    if (w >= cap) return -1;
    return te2_write_str(buf, buf_len, o);
}

static int te2_get_param(void *instance, const char *key, char *buf, int buf_len)
{
    auto *inst = (te2_instance *)instance;
    if (!inst || !key || !buf || buf_len <= 1) return -1;

    const char *colon = strrchr(key, ':');
    if (colon) key = colon + 1;

    if (!strcmp(key, "name"))
        return te2_write_str(buf, buf_len, "Tape Echo 2");

    if (!strcmp(key, "chain_params"))
        return te2_serve_chain_params(inst, buf, buf_len);

    if (!strcmp(key, "state")) {
        char *o = inst->state_buf;
        const size_t cap = sizeof inst->state_buf;
        size_t w = (size_t)snprintf(o, cap, "{\"te2\":1");
        for (int i = 0; i < TE2_PARAM_COUNT; i++) {
            if (i == TE2_P_PRESET) continue;
            w += (size_t)snprintf(o + w, cap - w, ",\"%s\":%.6g",
                                  te2_params[i].key,
                                  (double)inst->values[i].load(std::memory_order_relaxed));
            if (w >= cap - 32) return -1;
        }
        w += (size_t)snprintf(o + w, cap - w, "}");
        return te2_write_str(buf, buf_len, o);
    }

    if (!strcmp(key, "ui_hierarchy"))
        return te2_serve_ui_hierarchy(inst, buf, buf_len);

    /* record-path meters (0..3), for UI polling */
    if (!strcmp(key, "out_level"))
        return snprintf(buf, buf_len, "%.3f", (double)inst->dsp.getRecordVuLevel());
    if (!strcmp(key, "peak_level"))
        return snprintf(buf, buf_len, "%.3f", (double)inst->dsp.getRecordPeakLevel());

    /* the note name behind the current Echo Rate detent (leading-head table) */
    if (!strcmp(key, "echo_note_name")) {
        const int mode = (int)(inst->values[TE2_P_MODE].load(std::memory_order_relaxed) + 0.5f) + 1;
        const int division = teDivisionForSyncKnobPos(
            (int)(inst->values[TE2_P_NOTE].load(std::memory_order_relaxed) + 0.5f) - 1,
            teLeadingHeadIndexForMode(mode));
        return te2_write_str(buf, buf_len, kSyncDivisions[division].name);
    }

    const int idx = te2_param_index(key);
    if (idx < 0) return -1;
    const te2_param_t *prm = &te2_params[idx];
    const float v = inst->values[idx].load(std::memory_order_relaxed);

    if (prm->type == TE2_ENUM) {
        int oi = (int)(v + 0.5f);
        if (oi < 0) oi = 0;
        if (oi >= prm->n_options) oi = prm->n_options - 1;
        return te2_write_str(buf, buf_len, prm->options[oi]);
    }
    if (prm->type == TE2_INT)
        return snprintf(buf, buf_len, "%d", (int)(v + (v >= 0 ? 0.5f : -0.5f)));
    return snprintf(buf, buf_len, "%.4f", (double)v);
}

/* ------------------------------------------------------------------ */

static audio_fx_api_v2_t g_fx_api_v2;

extern "C" audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host)
{
    g_host = host;

    memset(&g_fx_api_v2, 0, sizeof g_fx_api_v2);
    g_fx_api_v2.api_version      = AUDIO_FX_API_VERSION_2;
    g_fx_api_v2.create_instance  = te2_create_instance;
    g_fx_api_v2.destroy_instance = te2_destroy_instance;
    g_fx_api_v2.process_block    = te2_process_block;
    g_fx_api_v2.set_param        = te2_set_param;
    g_fx_api_v2.get_param        = te2_get_param;
    g_fx_api_v2.on_midi          = nullptr;

    te2_log("tape-echo2: Tape Echo 2 (Dusk Audio) audio_fx v2 initialized");
    return &g_fx_api_v2;
}
