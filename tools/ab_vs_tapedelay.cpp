/* A/B two audio_fx v2 modules AT THEIR OWN DEFAULTS and report the things a
 * musician would actually notice: when the first repeat lands, how many
 * repeats you hear and how fast they die, how bright they are, and how loud
 * the whole thing is relative to the input.
 *
 *   ./ab <old.so> <new.so>
 *
 * Nothing is set on either instance — the whole point is to compare what you
 * get when you insert them.
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

struct Run { std::vector<float> out; double inRms; };

/* One impulse, then silence: the repeat train, isolated. */
static Run render_impulse(audio_fx_api_v2_t *fx, void *inst, int blocks)
{
    Run r; r.out.reserve((size_t)blocks * NB);
    int16_t io[NB * 2];
    for (int b = 0; b < 200; b++) {          /* settle smoothers */
        memset(io, 0, sizeof io);
        fx->process_block(inst, io, NB);
    }
    for (int b = 0; b < blocks; b++) {
        memset(io, 0, sizeof io);
        if (b == 0) { io[0] = io[1] = 20000; }
        fx->process_block(inst, io, NB);
        for (int i = 0; i < NB; i++)
            r.out.push_back(0.5f * ((float)io[i*2] + (float)io[i*2+1]));
    }
    r.inRms = 20000.0;
    return r;
}

/* A short tone burst, for level: what happens to program material. */
static double render_burst_rms(audio_fx_api_v2_t *fx, void *inst, double *inRmsOut)
{
    int16_t io[NB * 2];
    for (int b = 0; b < 200; b++) { memset(io, 0, sizeof io); fx->process_block(inst, io, NB); }
    double sumIn = 0, sumOut = 0; long n = 0;
    for (int b = 0; b < 400; b++) {
        for (int i = 0; i < NB; i++) {
            const long t = (long)b * NB + i;
            /* 2 s of 220 Hz, so the tail is included in the measurement */
            const double s = 0.35 * sin(2.0 * M_PI * 220.0 * t / SR);
            const short v = (short)(s * 20000.0);
            io[i*2] = io[i*2+1] = v;
            sumIn += (double)v * v; n++;
        }
        fx->process_block(inst, io, NB);
        for (int i = 0; i < NB; i++) {
            const double o = 0.5 * ((double)io[i*2] + (double)io[i*2+1]);
            sumOut += o * o;
        }
    }
    *inRmsOut = sqrt(sumIn / n);
    return sqrt(sumOut / n);
}

/* Peak-pick the repeat train: local maxima of a short-window envelope. */
static void report_repeats(const char *label, const Run &r)
{
    const int W = 64;                        /* ~1.5 ms envelope window */
    std::vector<double> env(r.out.size() / W, 0.0);
    for (size_t k = 0; k < env.size(); k++) {
        double m = 0;
        for (int j = 0; j < W; j++) { double a = fabs(r.out[k*W + j]); if (a > m) m = a; }
        env[k] = m;
    }
    double gmax = 0; for (double v : env) if (v > gmax) gmax = v;
    printf("  %s repeat train (threshold -40 dB of loudest):\n", label);
    const double thr = gmax * 0.01;
    double prevT = -1;
    int count = 0;
    for (size_t k = 1; k + 1 < env.size(); k++) {
        if (env[k] < thr) continue;
        if (env[k] <= env[k-1] || env[k] < env[k+1]) continue;
        /* suppress ripples: require 30 ms since the last reported peak */
        const double t = (k * W) * 1000.0 / SR;
        if (prevT >= 0 && t - prevT < 30.0) continue;
        printf("      #%-2d  %7.1f ms   %6.1f dB", ++count, t, 20*log10(env[k]/gmax));
        if (prevT >= 0) printf("   (+%.1f ms)", t - prevT);
        printf("\n");
        prevT = t;
        if (count >= 12) break;
    }
    if (!count) printf("      (none above threshold)\n");
}

/* Spectral centroid of the wet tail — a brightness proxy, in Hz. */
static double centroid(const std::vector<float> &x, size_t from, size_t to)
{
    const int N = 8192;
    if (to > x.size()) to = x.size();
    if (from + N > to) return 0.0;
    double num = 0, den = 0;
    for (int k = 1; k < N/2; k++) {
        double re = 0, im = 0;
        const double w = 2.0 * M_PI * k / N;
        for (int n = 0; n < N; n++) {
            const double s = x[from + n] * (0.5 - 0.5 * cos(2.0*M_PI*n/(N-1)));
            re += s * cos(w*n); im -= s * sin(w*n);
        }
        const double mag = sqrt(re*re + im*im);
        num += mag * (double)k * SR / N;
        den += mag;
    }
    return den > 0 ? num / den : 0.0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: ab <old.so> <new.so>\n"); return 2; }
    host_api_v1_t host{};
    host.sample_rate = SR; host.frames_per_block = NB;
    host.log = host_log; host.get_bpm = host_bpm;

    for (int a = 1; a <= 2; a++) {
        const char *label = a == 1 ? "OLD TapeDelay" : "NEW Tape Echo 2";
        void *h = dlopen(argv[a], RTLD_NOW);
        if (!h) { printf("dlopen %s: %s\n", argv[a], dlerror()); return 1; }
        auto init = (audio_fx_init_v2_fn)dlsym(h, AUDIO_FX_INIT_V2_SYMBOL);
        if (!init) { printf("no init in %s\n", argv[a]); return 1; }
        audio_fx_api_v2_t *fx = init(&host);
        void *inst = fx->create_instance(".", nullptr);

        printf("\n=== %s (defaults, nothing set) ===\n", label);
        Run r = render_impulse(fx, inst, 400);
        report_repeats(label, r);

        /* Brightness of the FIRST REPEAT. Window on the repeat, not on the
         * loudest sample: the loudest sample is the dry impulse, and the old
         * engine is silent for 400 ms after it, so that window reports 0 Hz
         * for a reason that has nothing to do with tone. */
        size_t first = 0;
        {
            const size_t skip = (size_t)(0.05 * SR);   /* past the dry hit */
            double m = 0;
            for (size_t i = skip; i < r.out.size(); i++)
                if (fabs(r.out[i]) > m) { m = fabs(r.out[i]); first = i; }
        }
        printf("  brightness: first repeat centroid %.0f Hz", centroid(r.out, first, r.out.size()));
        const size_t late = first + (size_t)(1.2 * SR);
        if (late + 8192 < r.out.size())
            printf(", ~1.2 s later %.0f Hz", centroid(r.out, late, r.out.size()));
        printf("\n");

        fx->destroy_instance(inst);
        inst = fx->create_instance(".", nullptr);
        double inRms = 0;
        const double outRms = render_burst_rms(fx, inst, &inRms);
        printf("  level: program in %.0f rms -> out %.0f rms  (%+.2f dB)\n",
               inRms, outRms, 20*log10(outRms/inRms));
        fx->destroy_instance(inst);
    }
    printf("\n");
    return 0;
}
