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

/* Convert milliseconds to seconds */
static float GetDelayTimeSeconds(int ms) {
    /* Clamp to valid range */
    if (ms < 20) ms = 20;
    if (ms > 2000) ms = 2000;
    return (float)ms / 1000.0f;
}

static float GetFeedback(float normalized) {
    /* 0-1 maps to 0-0.95 */
    return normalized * 0.95f;
}

static float GetToneFrequency(float normalized) {
    /* 0-1 maps to 500Hz-12000Hz (exponential) */
    return 500.0f * powf(24.0f, normalized);
}

static float GetStereoWidth(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return (float)percent / 100.0f;
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

    /* Parameters */
    int param_time;        /* milliseconds (20-2000) */
    float param_feedback;
    float param_mix;
    float param_tone;
    int param_stereo_width; /* percent (0=mono, 100=full L/R) */

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
    inst->param_time = 400;  /* 400ms */
    inst->param_feedback = 0.4f;
    inst->param_mix = 0.5f;
    inst->param_tone = 0.5f;
    inst->param_stereo_width = 100;

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
        float stereoWidth = GetStereoWidth(inst->param_stereo_width);

        /* Convert input to float */
        float inL = audio_inout[i * 2] / 32768.0f;
        float inR = audio_inout[i * 2 + 1] / 32768.0f;

        /* Read from delay lines */
        float delayedL = DelayLine_Read(&inst->delayLine[0], delayTime);
        float delayedR = DelayLine_Read(&inst->delayLine[1], delayTime);

        /* Apply tone filter to delayed signal */
        delayedL = OnePoleFilter_Process(&inst->toneFilter[0], delayedL);
        delayedR = OnePoleFilter_Process(&inst->toneFilter[1], delayedR);

        /* Ping-pong: input and feedback bounce between channels */
        DelayLine_Write(&inst->delayLine[0], inR + delayedR * feedback);
        DelayLine_Write(&inst->delayLine[1], inL + delayedL * feedback);

        /* Stereo width on wet path: 0 = mono, 1 = full L/R */
        float wetMono = 0.5f * (delayedL + delayedR);
        float wetL = wetMono + (delayedL - wetMono) * stereoWidth;
        float wetR = wetMono + (delayedR - wetMono) * stereoWidth;

        /* Mix dry/wet */
        float outL = inL * (1.0f - mix) + wetL * mix;
        float outR = inR * (1.0f - mix) + wetR * mix;

        /* Soft clip output */
        if (outL > 1.0f) outL = 1.0f;
        if (outL < -1.0f) outL = -1.0f;
        if (outR > 1.0f) outR = 1.0f;
        if (outR < -1.0f) outR = -1.0f;

        /* Convert back to int16 */
        audio_inout[i * 2] = (int16_t)(outL * 32767.0f);
        audio_inout[i * 2 + 1] = (int16_t)(outR * 32767.0f);
    }
}


/* Helper to extract a JSON number value by key */
static int json_get_number(const char *json, const char *key, float *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ' ') pos++;
    *out = (float)atof(pos);
    return 0;
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    spacecho_instance_t *inst = (spacecho_instance_t*)instance;
    if (!inst) return;

    /* State restore from patch save */
    if (strcmp(key, "state") == 0) {
        float v;
        if (json_get_number(val, "time", &v) == 0) {
            int ms = (int)v;
            if (ms < 20) ms = 20;
            if (ms > 2000) ms = 2000;
            inst->param_time = ms;
            SmoothedValue_SetTarget(&inst->smoothedDelayTime, GetDelayTimeSeconds(ms), RAMP_SAMPLES);
        }
        if (json_get_number(val, "feedback", &v) == 0) {
            inst->param_feedback = v;
            SmoothedValue_SetTarget(&inst->smoothedFeedback, GetFeedback(v), RAMP_SAMPLES);
        }
        if (json_get_number(val, "mix", &v) == 0) {
            inst->param_mix = v;
            SmoothedValue_SetTarget(&inst->smoothedMix, v, RAMP_SAMPLES);
        }
        if (json_get_number(val, "tone", &v) == 0) {
            inst->param_tone = v;
            for (int ch = 0; ch < MAX_CHANNELS; ch++) {
                OnePoleFilter_SetCutoff(&inst->toneFilter[ch], GetToneFrequency(v), SAMPLE_RATE);
            }
        }
        if (json_get_number(val, "stereo_width", &v) == 0) {
            int width = (int)v;
            if (width < 0) width = 0;
            if (width > 100) width = 100;
            inst->param_stereo_width = width;
        }
        return;
    }

    if (strcmp(key, "time") == 0) {
        int ms = atoi(val);
        if (ms < 20) ms = 20;
        if (ms > 2000) ms = 2000;
        inst->param_time = ms;
        SmoothedValue_SetTarget(&inst->smoothedDelayTime, GetDelayTimeSeconds(ms), RAMP_SAMPLES);
        return;
    }

    if (strcmp(key, "stereo_width") == 0 || strcmp(key, "width") == 0) {
        int width = atoi(val);
        if (width < 0) width = 0;
        if (width > 100) width = 100;
        inst->param_stereo_width = width;
        return;
    }

    float v = atof(val);
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;

    if (0) {
        /* placeholder - time handled above */
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
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    spacecho_instance_t *inst = (spacecho_instance_t*)instance;
    if (!inst) return -1;

    if (strcmp(key, "time") == 0) {
        return snprintf(buf, buf_len, "%d", inst->param_time);
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
    else if (strcmp(key, "stereo_width") == 0 || strcmp(key, "width") == 0) {
        return snprintf(buf, buf_len, "%d", inst->param_stereo_width);
    }
    else if (strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "TapeDelay");
    }
    else if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"time\":%d,\"feedback\":%.4f,\"mix\":%.4f,\"tone\":%.4f,\"stereo_width\":%d}",
            inst->param_time, inst->param_feedback, inst->param_mix, inst->param_tone, inst->param_stereo_width);
    }

    /* UI hierarchy for shadow parameter editor */
    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hierarchy = "{"
            "\"modes\":null,"
            "\"levels\":{"
                "\"root\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"time\",\"feedback\",\"mix\",\"tone\",\"stereo_width\"],"
                    "\"params\":[\"time\",\"feedback\",\"mix\",\"tone\",\"stereo_width\"]"
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
            "{\"key\":\"time\",\"name\":\"Time\",\"type\":\"int\",\"min\":20,\"max\":2000,\"step\":10},"
            "{\"key\":\"feedback\",\"name\":\"Feedback\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"mix\",\"name\":\"Mix\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"tone\",\"name\":\"Tone\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
            "{\"key\":\"stereo_width\",\"name\":\"Stereo Width\",\"type\":\"int\",\"min\":0,\"max\":100,\"step\":1}"
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
