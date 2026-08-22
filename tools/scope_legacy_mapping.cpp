/* How far does the calibration actually generalise? It was fitted at ONE
 * operating point (mode H3, mix 0.5, tape age Used, drive 0.5). Check the
 * decay it produces across modes and tape age, and check the
 * echo_volume rule at mix values other than 0.5.
 *
 * This exists because BOTH mappings were first fitted at one point and both
 * were wrong away from it — the feedback table by 11.5 dB on head 1, the mix
 * rule by 16.7 dB at mix 0.2. Run it after touching either.
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <vector>
#include <cstdlib>

#include "../src/host/plugin_api_v1.h"
#include "../src/host/audio_fx_api_v2.h"

static const int SR = 44100, NB = 128;
static void host_log(const char *) {}
static float host_bpm(void) { return 120.0f; }

static double decay(audio_fx_api_v2_t *fx, void *inst, double *first, int *nrep)
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
    std::vector<double> lv; double prevT = -1;
    for (size_t k = 4; k + 1 < env.size(); k++) {
        if (env[k] < dry*0.002) continue;
        if (env[k] <= env[k-1] || env[k] < env[k+1]) continue;
        const double tm = (k*W)*1000.0/SR;
        if (prevT >= 0 && tm - prevT < 150.0) continue;
        lv.push_back(20*log10(env[k]/dry)); prevT = tm;
        if (lv.size() >= 8) break;
    }
    *first = lv.empty() ? -99 : lv[0];
    *nrep = (int)lv.size();
    if (lv.size() < 2) return -99;
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

    /* reference: the old engine at feedback 0.4 */
    void *oi = O->create_instance(".", nullptr);
    O->set_param(oi, "feedback", "0.4000");
    double of; int on_;
    const double od = decay(O, oi, &of, &on_);
    O->destroy_instance(oi);
    printf("OLD feedback 0.40: %+.2f dB/repeat, first %+.1f dB, %d repeats\n\n", od, of, on_);

    printf("calibration fitted at mode H3 / age Used. Does it hold elsewhere?\n");
    printf("  (intensity 0.44 = what the table maps feedback 0.4 to)\n\n");
    const char *modes[] = {"H1","H2","H3","H2+3","H12+R","H123R"};
    for (const char *m : modes) {
        void *i2 = N->create_instance(".", nullptr);
        N->set_param(i2, "mode", m);
        N->set_param(i2, "intensity", "0.44");
        N->set_param(i2, "echo_volume", "1.0");
        double f; int nr;
        const double d = decay(N, i2, &f, &nr);
        N->destroy_instance(i2);
        printf("  mode %-6s  %+7.2f dB/repeat  (vs old %+.2f, delta %+5.2f)  %d repeats\n",
               m, d, od, d - od, nr);
    }
    printf("\n");
    for (const char *age : {"New","Used","Old"}) {
        void *i2 = N->create_instance(".", nullptr);
        N->set_param(i2, "tape_age", age);
        N->set_param(i2, "intensity", "0.44");
        N->set_param(i2, "echo_volume", "1.0");
        double f; int nr;
        const double d = decay(N, i2, &f, &nr);
        N->destroy_instance(i2);
        printf("  age %-5s   %+7.2f dB/repeat  (delta %+5.2f)\n", age, d, d - od);
    }

    printf("\necho_volume via the real import path: does it hold away from 0.5?\n");
    printf("  compares wet/dry ratio of repeat #1 against the old engine\n\n");
    for (double m : {0.2, 0.35, 0.5, 0.65, 0.8}) {
        char v[32];
        void *i1 = O->create_instance(".", nullptr);
        snprintf(v,sizeof v,"%.4f",m); O->set_param(i1, "mix", v);
        O->set_param(i1, "feedback", "0.4000");
        double f1; int n1; decay(O, i1, &f1, &n1);
        O->destroy_instance(i1);

/* drive the REAL import path, do not recompute the rule here */
        void *i2 = N->create_instance(".", nullptr);
        char st[512];
        snprintf(st, sizeof st,
                 "{\"time\":400,\"feedback\":0.4000,\"mix\":%.4f,\"tone\":0.5000,"
                 "\"stereo_width\":0,\"division\":\"free\",\"bpm\":120}", m);
        N->set_param(i2, "state", st);
        char evbuf[64] = {0};
        N->get_param(i2, "echo_volume", evbuf, sizeof evbuf);
        const double ev = atof(evbuf);
        double f2; int n2; decay(N, i2, &f2, &n2);
        N->destroy_instance(i2);
        printf("  mix %.2f -> echo_vol %.2f   old first %+7.1f dB   new %+7.1f dB   delta %+5.1f\n",
               m, ev, f1, f2, f2 - f1);
    }
    return 0;
}
