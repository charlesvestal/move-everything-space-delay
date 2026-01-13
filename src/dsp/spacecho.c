/*
 * Space Echo Audio FX Plugin - Tape Delay
 *
 * Based on https://github.com/cyrusasfa/TapeDelay
 * Simple, clean tape delay with linear interpolation and smooth parameter ramping
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "audio_fx_api_v1.h"

#define SAMPLE_RATE 44100.0f
#define MAX_DELAY_SECONDS 2.0f

/* ============================================================================
 * SMOOTHED VALUE - For click-free parameter changes
 * ============================================================================ */

typedef struct {
    float currentValue;
    float targetValue;
    float step;
    int stepsRemaining;
} SmoothedValue;

static void SmoothedValue_Init(SmoothedValue *sv, float initialValue) {
    sv->currentValue = initialValue;
    sv->targetValue = initialValue;
    sv->step = 0.0f;
    sv->stepsRemaining = 0;
}

static void SmoothedValue_SetTarget(SmoothedValue *sv, float newTarget, int rampSamples) {
    sv->targetValue = newTarget;
    if (rampSamples > 0) {
        sv->step = (newTarget - sv->currentValue) / (float)rampSamples;
        sv->stepsRemaining = rampSamples;
    } else {
        sv->currentValue = newTarget;
        sv->step = 0.0f;
        sv->stepsRemaining = 0;
    }
}

static float SmoothedValue_GetNext(SmoothedValue *sv) {
    if (sv->stepsRemaining > 0) {
        sv->currentValue += sv->step;
        sv->stepsRemaining--;
        if (sv->stepsRemaining == 0) {
            sv->currentValue = sv->targetValue;
        }
    }
    return sv->currentValue;
}

/* ============================================================================
 * DELAY LINE - Circular buffer with linear interpolation
 * ============================================================================ */

typedef struct {
    float *buffer;
    int bufferLength;
    int writePosition;
    float sampleRate;
} DelayLine;

static void DelayLine_Init(DelayLine *dl, float sampleRate) {
    dl->sampleRate = sampleRate;
    dl->bufferLength = (int)(MAX_DELAY_SECONDS * sampleRate);
    dl->buffer = (float *)calloc(dl->bufferLength, sizeof(float));
    dl->writePosition = 0;
}

static void DelayLine_Free(DelayLine *dl) {
    if (dl->buffer) {
        free(dl->buffer);
        dl->buffer = NULL;
    }
}

static void DelayLine_Clear(DelayLine *dl) {
    memset(dl->buffer, 0, dl->bufferLength * sizeof(float));
}

static void DelayLine_Write(DelayLine *dl, float sample) {
    dl->buffer[dl->writePosition] = sample;
    dl->writePosition++;
    if (dl->writePosition >= dl->bufferLength) {
        dl->writePosition = 0;
    }
}

static float DelayLine_Read(DelayLine *dl, float delayTimeSeconds) {
    /* Calculate read position with fractional sample */
    float delaySamples = delayTimeSeconds * dl->sampleRate;

    /* Clamp delay to valid range */
    if (delaySamples < 1.0f) delaySamples = 1.0f;
    if (delaySamples > dl->bufferLength - 1) delaySamples = dl->bufferLength - 1;

    float readPos = (float)dl->writePosition - delaySamples;
    if (readPos < 0) readPos += dl->bufferLength;

    /* Linear interpolation */
    float fraction = readPos - floorf(readPos);
    int index0 = (int)floorf(readPos);
    int index1 = (index0 + 1) % dl->bufferLength;

    float sample0 = dl->buffer[index0];
    float sample1 = dl->buffer[index1];

    return sample0 + fraction * (sample1 - sample0);
}

/* ============================================================================
 * SIMPLE ONE-POLE FILTER - For tone control
 * ============================================================================ */

typedef struct {
    float z1;
    float a0;
    float b1;
} OnePoleFilter;

static void OnePoleFilter_Init(OnePoleFilter *f) {
    f->z1 = 0.0f;
    f->a0 = 1.0f;
    f->b1 = 0.0f;
}

static void OnePoleFilter_SetCutoff(OnePoleFilter *f, float cutoffHz, float sampleRate) {
    float w = 2.0f * 3.14159265f * cutoffHz / sampleRate;
    f->b1 = expf(-w);
    f->a0 = 1.0f - f->b1;
}

static float OnePoleFilter_Process(OnePoleFilter *f, float input) {
    f->z1 = input * f->a0 + f->z1 * f->b1;
    return f->z1;
}

/* ============================================================================
 * SOFT SATURATION - Gentle tape-style saturation
 * ============================================================================ */

static float SoftSaturate(float x, float amount) {
    if (amount <= 0.0f) return x;

    /* Soft clipping using tanh */
    float drive = 1.0f + amount * 3.0f;
    return tanhf(x * drive) / drive;
}

/* ============================================================================
 * PLUGIN STATE
 * ============================================================================ */

#define MAX_CHANNELS 2

static DelayLine g_delayLine[MAX_CHANNELS];
static OnePoleFilter g_toneFilter[MAX_CHANNELS];
static SmoothedValue g_smoothedDelayTime;
static SmoothedValue g_smoothedFeedback;
static SmoothedValue g_smoothedMix;
static SmoothedValue g_smoothedTone;

/* Parameters */
static float g_param_time = 0.3f;       /* 0-1 maps to 0.01-2.0 seconds */
static float g_param_feedback = 0.4f;   /* 0-1 maps to 0-0.95 */
static float g_param_mix = 0.5f;        /* 0-1 dry/wet */
static float g_param_tone = 0.5f;       /* 0-1 dark to bright */
static float g_param_saturation = 0.0f; /* 0-1 saturation amount */

static const host_api_v1_t *g_host = NULL;
static audio_fx_api_v1_t g_fx_api;
static int g_initialized = 0;

/* Ramp time in samples (~50ms at 44.1kHz for smooth changes) */
#define RAMP_SAMPLES 2205

static void fx_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[spacecho] %s", msg);
        g_host->log(buf);
    }
}

/* Convert normalized parameters to actual values */
static float GetDelayTimeSeconds(float normalized) {
    /* 0-1 maps to 0.02-2.0 seconds (exponential for better feel) */
    return 0.02f + normalized * normalized * 1.98f;
}

static float GetFeedback(float normalized) {
    /* 0-1 maps to 0-0.95 */
    return normalized * 0.95f;
}

static float GetToneFrequency(float normalized) {
    /* 0-1 maps to 500Hz-12000Hz (exponential) */
    return 500.0f * powf(24.0f, normalized);
}

/* ============================================================================
 * AUDIO FX API IMPLEMENTATION
 * ============================================================================ */

static int fx_on_load(const char *module_dir, const char *config_json) {
    fx_log("Space Echo loading...");

    /* Initialize delay lines */
    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        DelayLine_Init(&g_delayLine[ch], SAMPLE_RATE);
        OnePoleFilter_Init(&g_toneFilter[ch]);
        OnePoleFilter_SetCutoff(&g_toneFilter[ch], GetToneFrequency(g_param_tone), SAMPLE_RATE);
    }

    /* Initialize smoothed values */
    SmoothedValue_Init(&g_smoothedDelayTime, GetDelayTimeSeconds(g_param_time));
    SmoothedValue_Init(&g_smoothedFeedback, GetFeedback(g_param_feedback));
    SmoothedValue_Init(&g_smoothedMix, g_param_mix);
    SmoothedValue_Init(&g_smoothedTone, g_param_tone);

    g_initialized = 1;
    fx_log("Space Echo initialized");
    return 0;
}

static void fx_on_unload(void) {
    fx_log("Space Echo unloading...");
    if (g_initialized) {
        for (int ch = 0; ch < MAX_CHANNELS; ch++) {
            DelayLine_Free(&g_delayLine[ch]);
        }
        g_initialized = 0;
    }
}

static void fx_process_block(int16_t *audio_inout, int frames) {
    if (!g_initialized) return;

    for (int i = 0; i < frames; i++) {
        /* Get smoothed parameter values (same for both channels) */
        float delayTime = SmoothedValue_GetNext(&g_smoothedDelayTime);
        float feedback = SmoothedValue_GetNext(&g_smoothedFeedback);
        float mix = SmoothedValue_GetNext(&g_smoothedMix);

        for (int ch = 0; ch < 2; ch++) {
            /* Convert input to float */
            float in = audio_inout[i * 2 + ch] / 32768.0f;

            /* Read from delay line */
            float delayed = DelayLine_Read(&g_delayLine[ch], delayTime);

            /* Apply tone filter to delayed signal */
            delayed = OnePoleFilter_Process(&g_toneFilter[ch], delayed);

            /* Apply soft saturation to feedback path */
            float saturated = SoftSaturate(delayed, g_param_saturation);

            /* Write input + feedback to delay line */
            DelayLine_Write(&g_delayLine[ch], in + saturated * feedback);

            /* Mix dry/wet */
            float out = in * (1.0f - mix) + delayed * mix;

            /* Soft clip output to prevent harsh clipping */
            if (out > 1.0f) out = 1.0f;
            if (out < -1.0f) out = -1.0f;

            /* Convert back to int16 */
            audio_inout[i * 2 + ch] = (int16_t)(out * 32767.0f);
        }
    }
}

static void fx_set_param(const char *key, const char *val) {
    float v = atof(val);

    /* Clamp to 0-1 range */
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;

    if (strcmp(key, "time") == 0) {
        g_param_time = v;
        SmoothedValue_SetTarget(&g_smoothedDelayTime, GetDelayTimeSeconds(v), RAMP_SAMPLES);
    }
    else if (strcmp(key, "feedback") == 0) {
        g_param_feedback = v;
        SmoothedValue_SetTarget(&g_smoothedFeedback, GetFeedback(v), RAMP_SAMPLES);
    }
    else if (strcmp(key, "mix") == 0) {
        g_param_mix = v;
        SmoothedValue_SetTarget(&g_smoothedMix, v, RAMP_SAMPLES);
    }
    else if (strcmp(key, "tone") == 0) {
        g_param_tone = v;
        for (int ch = 0; ch < MAX_CHANNELS; ch++) {
            OnePoleFilter_SetCutoff(&g_toneFilter[ch], GetToneFrequency(v), SAMPLE_RATE);
        }
    }
    else if (strcmp(key, "saturation") == 0) {
        g_param_saturation = v;
    }
}

static int fx_get_param(const char *key, char *buf, int buf_len) {
    if (strcmp(key, "time") == 0) {
        return snprintf(buf, buf_len, "%.2f", g_param_time);
    }
    else if (strcmp(key, "feedback") == 0) {
        return snprintf(buf, buf_len, "%.2f", g_param_feedback);
    }
    else if (strcmp(key, "mix") == 0) {
        return snprintf(buf, buf_len, "%.2f", g_param_mix);
    }
    else if (strcmp(key, "tone") == 0) {
        return snprintf(buf, buf_len, "%.2f", g_param_tone);
    }
    else if (strcmp(key, "saturation") == 0) {
        return snprintf(buf, buf_len, "%.2f", g_param_saturation);
    }
    else if (strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "Space Echo");
    }
    return -1;
}

/* ============================================================================
 * ENTRY POINT
 * ============================================================================ */

audio_fx_api_v1_t* move_audio_fx_init_v1(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_fx_api, 0, sizeof(g_fx_api));
    g_fx_api.api_version = AUDIO_FX_API_VERSION;
    g_fx_api.on_load = fx_on_load;
    g_fx_api.on_unload = fx_on_unload;
    g_fx_api.process_block = fx_process_block;
    g_fx_api.set_param = fx_set_param;
    g_fx_api.get_param = fx_get_param;

    fx_log("Space Echo plugin initialized");
    return &g_fx_api;
}
