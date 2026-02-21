#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/dsp/spacecho.c"

static void test_log(const char *msg) {
    (void)msg;
}

static int run_left_impulse_case(audio_fx_api_v2_t *api,
                                 const char *width_value,
                                 int *left_echo,
                                 int *right_echo) {
    void *instance = api->create_instance(NULL, "{}");
    if (!instance) {
        fprintf(stderr, "failed to create instance\n");
        return 1;
    }

    const int frames = 20000;
    int16_t *buffer = calloc((size_t)frames * 2, sizeof(int16_t));
    if (!buffer) {
        fprintf(stderr, "failed to allocate test buffer\n");
        api->destroy_instance(instance);
        return 1;
    }

    api->set_param(instance, "mix", "1.0");
    api->set_param(instance, "feedback", "0.0");
    api->set_param(instance, "tone", "1.0");
    api->set_param(instance, "stereo_width", width_value);

    /* Left-only impulse */
    buffer[0] = 30000;
    buffer[1] = 0;

    api->process_block(instance, buffer, frames);

    const int delay_frame = 17640; /* 400ms default at 44.1kHz */
    *left_echo = abs(buffer[delay_frame * 2]);
    *right_echo = abs(buffer[delay_frame * 2 + 1]);

    free(buffer);
    api->destroy_instance(instance);
    return 0;
}

int main(void) {
    host_api_v1_t host = {0};
    host.log = test_log;

    audio_fx_api_v2_t *api = move_audio_fx_init_v2(&host);
    if (!api) {
        fprintf(stderr, "failed to initialize API\n");
        return 1;
    }

    int left_echo = 0;
    int right_echo = 0;

    if (run_left_impulse_case(api, "100", &left_echo, &right_echo) != 0) {
        return 1;
    }

    if (right_echo < 1000 || left_echo > 500) {
        fprintf(stderr,
                "width=100 expected first ping-pong echo on right only, got left=%d right=%d\n",
                left_echo,
                right_echo);
        return 1;
    }

    if (run_left_impulse_case(api, "0", &left_echo, &right_echo) != 0) {
        return 1;
    }

    if (left_echo < 500 || right_echo < 500 || abs(left_echo - right_echo) > 500) {
        fprintf(stderr,
                "width=0 expected mono echo (L==R), got left=%d right=%d\n",
                left_echo,
                right_echo);
        return 1;
    }

    return 0;
}
