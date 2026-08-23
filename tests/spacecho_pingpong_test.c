#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/dsp/spacecho.c"

static void test_log(const char *msg) {
    (void)msg;
}

static int run_impulse_case(audio_fx_api_v2_t *api,
                            const char *width_value,
                            int16_t impulse_left,
                            int16_t impulse_right,
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
    if (width_value) {
        api->set_param(instance, "stereo_width", width_value);

        /* Allow smoothed width ramp to settle before testing steady-state behavior. */
        const int settle_frames = 3000;
        int16_t *settle = calloc((size_t)settle_frames * 2, sizeof(int16_t));
        if (!settle) {
            fprintf(stderr, "failed to allocate settle buffer\n");
            free(buffer);
            api->destroy_instance(instance);
            return 1;
        }
        api->process_block(instance, settle, settle_frames);
        free(settle);
    }

    buffer[0] = impulse_left;
    buffer[1] = impulse_right;

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

    if (run_impulse_case(api, "100", 30000, 0, &left_echo, &right_echo) != 0) {
        return 1;
    }

    if (right_echo < 1000 || left_echo > 500) {
        fprintf(stderr,
                "width=100 expected first ping-pong echo on right only, got left=%d right=%d\n",
                left_echo,
                right_echo);
        return 1;
    }

    if (right_echo < 15000) {
        fprintf(stderr,
                "width=100 expected compensated wet level, got right=%d\n",
                right_echo);
        return 1;
    }

    if (run_impulse_case(api, "100", 30000, 30000, &left_echo, &right_echo) != 0) {
        return 1;
    }

    if (right_echo < 1000 || left_echo > 500) {
        fprintf(stderr,
                "center input width=100 expected first echo on right only, got left=%d right=%d\n",
                left_echo,
                right_echo);
        return 1;
    }

    if (run_impulse_case(api, "0", 30000, 30000, &left_echo, &right_echo) != 0) {
        return 1;
    }

    if (left_echo < 20000 || right_echo < 20000 || abs(left_echo - right_echo) > 500) {
        fprintf(stderr,
                "width=0 expected previous-level mono echo (L==R), got left=%d right=%d\n",
                left_echo,
                right_echo);
        return 1;
    }

    if (run_impulse_case(api, NULL, 30000, 30000, &left_echo, &right_echo) != 0) {
        return 1;
    }

    if (left_echo < 20000 || right_echo < 20000 || abs(left_echo - right_echo) > 500) {
        fprintf(stderr,
                "default width expected previous-level mono echo (L==R), got left=%d right=%d\n",
                left_echo,
                right_echo);
        return 1;
    }

    return 0;
}
