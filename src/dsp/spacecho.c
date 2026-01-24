/*
 * TapeDelay Audio FX Plugin - Tape Delay
 *
 * Based on https://github.com/cyrusasfa/TapeDelay
 * Simple, clean tape delay with linear interpolation and smooth parameter ramping
 *
 * V2 API only - instance-based for multi-instance support.
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
 * SHARED STATE
 * ============================================================================ */

#define MAX_CHANNELS 2
#define RAMP_SAMPLES 2205

static const host_api_v1_t *g_host = NULL;

static void plugin_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[tapedelay] %s", msg);
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
 * V2 API - Instance-based
 * ============================================================================ */

#define AUDIO_FX_API_VERSION_2 2

typedef struct audio_fx_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *config_json);
    void (*destroy_instance)(void *instance);
    void (*process_block)(void *instance, int16_t *audio_inout, int frames);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
} audio_fx_api_v2_t;

/* Instance structure */
typedef struct {
    char module_dir[256];

    /* Delay lines and filters */
    DelayLine delayLine[MAX_CHANNELS];
    OnePoleFilter toneFilter[MAX_CHANNELS];

    /* Smoothed values */
    SmoothedValue smoothedDelayTime;
    SmoothedValue smoothedFeedback;
    SmoothedValue smoothedMix;
    SmoothedValue smoothedTone;
    SmoothedValue smoothedSaturation;

    /* Parameters */
    float param_time;
    float param_feedback;
    float param_mix;
    float param_tone;
    float param_saturation;

    int initialized;
} spacecho_instance_t;

static void* v2_create_instance(const char *module_dir, const char *config_json) {
    plugin_log("Creating instance");
    (void)config_json;

    spacecho_instance_t *inst = (spacecho_instance_t*)calloc(1, sizeof(spacecho_instance_t));
    if (!inst) {
        plugin_log("Failed to allocate instance");
        return NULL;
    }

    if (module_dir) {
        strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    }

    /* Set default parameters */
    inst->param_time = 0.3f;
    inst->param_feedback = 0.4f;
    inst->param_mix = 0.5f;
    inst->param_tone = 0.5f;
    inst->param_saturation = 0.0f;

    /* Initialize delay lines */
    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        DelayLine_Init(&inst->delayLine[ch], SAMPLE_RATE);
        OnePoleFilter_Init(&inst->toneFilter[ch]);
        OnePoleFilter_SetCutoff(&inst->toneFilter[ch], GetToneFrequency(inst->param_tone), SAMPLE_RATE);
    }

    /* Initialize smoothed values */
    SmoothedValue_Init(&inst->smoothedDelayTime, GetDelayTimeSeconds(inst->param_time));
    SmoothedValue_Init(&inst->smoothedFeedback, GetFeedback(inst->param_feedback));
    SmoothedValue_Init(&inst->smoothedMix, inst->param_mix);
    SmoothedValue_Init(&inst->smoothedTone, inst->param_tone);
    SmoothedValue_Init(&inst->smoothedSaturation, inst->param_saturation);

    inst->initialized = 1;
    plugin_log("Instance created");
    return inst;
}

static void v2_destroy_instance(void *instance) {
    spacecho_instance_t *inst = (spacecho_instance_t*)instance;
    if (!inst) return;

    plugin_log("Destroying instance");

    if (inst->initialized) {
        for (int ch = 0; ch < MAX_CHANNELS; ch++) {
            DelayLine_Free(&inst->delayLine[ch]);
        }
    }

    free(inst);
}

static void v2_process_block(void *instance, int16_t *audio_inout, int frames) {
    spacecho_instance_t *inst = (spacecho_instance_t*)instance;
    if (!inst || !inst->initialized) return;

    for (int i = 0; i < frames; i++) {
        /* Get smoothed parameter values */
        float delayTime = SmoothedValue_GetNext(&inst->smoothedDelayTime);
        float feedback = SmoothedValue_GetNext(&inst->smoothedFeedback);
        float mix = SmoothedValue_GetNext(&inst->smoothedMix);
        float saturation = SmoothedValue_GetNext(&inst->smoothedSaturation);

        for (int ch = 0; ch < 2; ch++) {
            /* Convert input to float */
            float in = audio_inout[i * 2 + ch] / 32768.0f;

            /* Read from delay line */
            float delayed = DelayLine_Read(&inst->delayLine[ch], delayTime);

            /* Apply tone filter to delayed signal */
            delayed = OnePoleFilter_Process(&inst->toneFilter[ch], delayed);

            /* Tape-style saturation on the record path (input + feedback) */
            float write = in + delayed * feedback;
            float saturated = SoftSaturate(write, saturation);
            DelayLine_Write(&inst->delayLine[ch], saturated);

            /* Mix dry/wet */
            float out = in * (1.0f - mix) + delayed * mix;

            /* Soft clip output */
            if (out > 1.0f) out = 1.0f;
            if (out < -1.0f) out = -1.0f;

            /* Convert back to int16 */
            audio_inout[i * 2 + ch] = (int16_t)(out * 32767.0f);
        }
    }
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    spacecho_instance_t *inst = (spacecho_instance_t*)instance;
    if (!inst) return;

    float v = atof(val);
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;

    if (strcmp(key, "time") == 0) {
        inst->param_time = v;
        SmoothedValue_SetTarget(&inst->smoothedDelayTime, GetDelayTimeSeconds(v), RAMP_SAMPLES);
    }
    else if (strcmp(key, "feedback") == 0) {
        inst->param_feedback = v;
        SmoothedValue_SetTarget(&inst->smoothedFeedback, GetFeedback(v), RAMP_SAMPLES);
    }
    else if (strcmp(key, "mix") == 0) {
        inst->param_mix = v;
        SmoothedValue_SetTarget(&inst->smoothedMix, v, RAMP_SAMPLES);
    }
    else if (strcmp(key, "tone") == 0) {
        inst->param_tone = v;
        for (int ch = 0; ch < MAX_CHANNELS; ch++) {
            OnePoleFilter_SetCutoff(&inst->toneFilter[ch], GetToneFrequency(v), SAMPLE_RATE);
        }
    }
    else if (strcmp(key, "saturation") == 0) {
        inst->param_saturation = v;
        SmoothedValue_SetTarget(&inst->smoothedSaturation, v, RAMP_SAMPLES);
    }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    spacecho_instance_t *inst = (spacecho_instance_t*)instance;
    if (!inst) return -1;

    if (strcmp(key, "time") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->param_time);
    }
    else if (strcmp(key, "feedback") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->param_feedback);
    }
    else if (strcmp(key, "mix") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->param_mix);
    }
    else if (strcmp(key, "tone") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->param_tone);
    }
    else if (strcmp(key, "saturation") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->param_saturation);
    }
    else if (strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "TapeDelay");
    }

    /* UI hierarchy for shadow parameter editor */
    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hierarchy = "{"
            "\"modes\":null,"
            "\"levels\":{"
                "\"root\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"time\",\"feedback\",\"mix\",\"tone\",\"saturation\"],"
                    "\"params\":[\"time\",\"feedback\",\"mix\",\"tone\",\"saturation\"]"
                "}"
            "}"
        "}";
        int len = strlen(hierarchy);
        if (len < buf_len) {
            strcpy(buf, hierarchy);
            return len;
        }
        return -1;
    }

    /* Chain params metadata for shadow parameter editor */
    if (strcmp(key, "chain_params") == 0) {
        const char *params_json = "["
            "{\"key\":\"time\",\"name\":\"Time\",\"type\":\"float\",\"min\":0,\"max\":1},"
            "{\"key\":\"feedback\",\"name\":\"Feedback\",\"type\":\"float\",\"min\":0,\"max\":1},"
            "{\"key\":\"mix\",\"name\":\"Mix\",\"type\":\"float\",\"min\":0,\"max\":1},"
            "{\"key\":\"tone\",\"name\":\"Tone\",\"type\":\"float\",\"min\":0,\"max\":1},"
            "{\"key\":\"saturation\",\"name\":\"Saturation\",\"type\":\"float\",\"min\":0,\"max\":1}"
        "]";
        int len = strlen(params_json);
        if (len < buf_len) {
            strcpy(buf, params_json);
            return len;
        }
        return -1;
    }

    return -1;
}

static audio_fx_api_v2_t g_fx_api_v2;

audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_fx_api_v2, 0, sizeof(g_fx_api_v2));
    g_fx_api_v2.api_version = AUDIO_FX_API_VERSION_2;
    g_fx_api_v2.create_instance = v2_create_instance;
    g_fx_api_v2.destroy_instance = v2_destroy_instance;
    g_fx_api_v2.process_block = v2_process_block;
    g_fx_api_v2.set_param = v2_set_param;
    g_fx_api_v2.get_param = v2_get_param;

    plugin_log("V2 API initialized");

    return &g_fx_api_v2;
}
