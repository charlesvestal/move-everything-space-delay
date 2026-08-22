/* Calibrate old TapeDelay `feedback` against new Tape Echo 2 `intensity` by
 * the thing that is actually audible: how many dB a repeat loses to the next
 * one. Prints a table, and for each old feedback the intensity that matches
 * its decay.
 *
 *   ./calib <old.so> <new.so>
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <vector>

#include "../src/host/plugin_api_v1.h"
#include "../src/host/audio_fx_api_v2.h"

static const int SR = 44100, NB = 128;
static void host_log(const char *) {}
static float host_bpm(void) { return 120.0f; }

/* dB lost per repeat, averaged over the repeats we can see; -99 if fewer
 * than two repeats clear the floor. Also returns the first repeat's level. */
static double decay_per_repeat(audio_fx_api_v2_t *fx, void *inst, double *first)
{
    int16_t io[NB*2];
    std::vector<float> x;
    for (int b = 0; b < 200; b++) { memset(io,0,sizeof io); fx->process_block(inst,io,NB); }
    for (int b = 0; b < 900; b++) {
        memset(io,0,sizeof io);
        if (b == 0) io[0] = io[1] = 20000;
        fx->process_block(inst, io, NB);
        for (int i = 0; i < NB; i++) x.push_back(0.5f*((float)io[i*2]+(float)io[i*2+1]));
    }
    const int W = 64;
    std::vector<double> env(x.size()/W, 0.0);
    for (size_t k = 0; k < env.size(); k++) {
        double m = 0;
        for (int j = 0; j < W; j++) { double a = fabs(x[k*W+j]); if (a > m) m = a; }
        env[k] = m;
    }
    double dry = 0;
    for (size_t k = 0; k < 4 && k < env.size(); k++) if (env[k] > dry) dry = env[k];

    std::vector<double> lv;
    double prevT = -1;
    const double thr = dry * 0.002;              /* -54 dB */
    for (size_t k = 4; k + 1 < env.size(); k++) {
        if (env[k] < thr) continue;
        if (env[k] <= env[k-1] || env[k] < env[k+1]) continue;
        const double tm = (k*W)*1000.0/SR;
        if (prevT >= 0 && tm - prevT < 200.0) continue;
        lv.push_back(20*log10(env[k]/dry));
        prevT = tm;
        if (lv.size() >= 8) break;
    }
    *first = lv.empty() ? -99 : lv[0];
    if (lv.size() < 2) return -99;
    /* least-squares slope over repeat index = dB per repeat */
    const int n = (int)lv.size();
    double sx=0, sy=0, sxx=0, sxy=0;
    for (int i = 0; i < n; i++) { sx+=i; sy+=lv[i]; sxx+=(double)i*i; sxy+=i*lv[i]; }
    return (n*sxy - sx*sy) / (n*sxx - sx*sx);
}

int main(int argc, char **argv)
{
    host_api_v1_t host{};
    host.sample_rate=SR; host.frames_per_block=NB; host.log=host_log; host.get_bpm=host_bpm;

    void *ho = dlopen(argv[1], RTLD_NOW);
    audio_fx_api_v2_t *O = ((audio_fx_init_v2_fn)dlsym(ho, AUDIO_FX_INIT_V2_SYMBOL))(&host);
    void *hn = dlopen(argv[2], RTLD_NOW);
    audio_fx_api_v2_t *N = ((audio_fx_init_v2_fn)dlsym(hn, AUDIO_FX_INIT_V2_SYMBOL))(&host);

    /* new engine: intensity -> decay, at echo_volume 1.0 (the level match) */
    printf("new engine: intensity -> dB/repeat\n");
    std::vector<double> ni, nd;
    for (int i = 10; i <= 80; i += 2) {
        void *inst = N->create_instance(".", nullptr);
        char v[32]; snprintf(v,sizeof v,"%.2f", i/100.0);
        N->set_param(inst, "intensity", v);
        N->set_param(inst, "echo_volume", "1.0");
        double f;
        const double d = decay_per_repeat(N, inst, &f);
        N->destroy_instance(inst);
        printf("   %.2f  %+7.2f dB   (first %+.1f)\n", i/100.0, d, f);
        if (d > -90) { ni.push_back(i/100.0); nd.push_back(d); }
    }

    printf("\nold feedback -> matching intensity, PER HEAD\n");
    printf("   fb    decay      H1     H2     H3\n");
    for (int f = 5; f <= 95; f += 5) {
        void *inst = O->create_instance(".", nullptr);
        char v[32]; snprintf(v,sizeof v,"%.4f", f/100.0);
        O->set_param(inst, "feedback", v);
        double fi;
        const double d = decay_per_repeat(O, inst, &fi);
        O->destroy_instance(inst);
        printf("   %.2f  %+7.2f  ", f/100.0, d);
        for (const char *mode : {"H1","H2","H3"}) {
            double best = 0, bestErr = 1e9;
            for (int i = 6; i <= 74; i += 1) {
                void *n2 = N->create_instance(".", nullptr);
                N->set_param(n2, "mode", mode);
                snprintf(v,sizeof v,"%.2f", i/100.0);
                N->set_param(n2, "intensity", v);
                N->set_param(n2, "echo_volume", "1.0");
                double f2;
                const double d2 = decay_per_repeat(N, n2, &f2);
                N->destroy_instance(n2);
                if (d2 < -90) continue;
                const double e = fabs(d2 - d);
                if (e < bestErr) { bestErr = e; best = i/100.0; }
            }
            printf(" %.2f ", best);
        }
        printf("\n");
    }
    return 0;
}
