/* te2_loadtest — dlopen the built dsp.so exactly like Schwung's chain host.
 *
 *   ./te2_loadtest ./dsp.so
 *
 * Verifies: the move_audio_fx_init_v2 export, instance lifecycle, parameter
 * set/get round trips (including enums by name), chain_params/state JSON,
 * that an impulse actually produces a delayed echo, and the realtime factor
 * (blocks are 128 frames = 2.902 ms of audio at 44.1 kHz).
 *
 * Run ON the Move for the realtime number; runs anywhere for correctness.
 */

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>

#include "../src/host/plugin_api_v1.h"
#include "../src/host/audio_fx_api_v2.h"

static void host_log(const char *msg) { printf("[host] %s\n", msg); }
static float host_bpm(void) { return 120.0f; }

static int g_failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
    else         {              printf("  ok: " __VA_ARGS__); printf("\n"); } \
} while (0)

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "./dsp.so";

    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("FAIL: dlopen(%s): %s\n", path, dlerror()); return 1; }

    audio_fx_init_v2_fn init =
        (audio_fx_init_v2_fn)dlsym(h, AUDIO_FX_INIT_V2_SYMBOL);
    CHECK(init != nullptr, "%s export present", AUDIO_FX_INIT_V2_SYMBOL);
    if (!init) return 1;

    host_api_v1_t host;
    memset(&host, 0, sizeof host);
    host.api_version      = 1;
    host.sample_rate      = MOVE_SAMPLE_RATE;
    host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    host.log              = host_log;
    host.get_bpm          = host_bpm;

    audio_fx_api_v2_t *fx = init(&host);
    CHECK(fx && fx->api_version == AUDIO_FX_API_VERSION_2, "api_version == 2");
    CHECK(fx->create_instance && fx->destroy_instance && fx->process_block
          && fx->set_param && fx->get_param, "all required callbacks set");

    void *inst = fx->create_instance(".", nullptr);
    CHECK(inst != nullptr, "create_instance");
    if (!inst) return 1;

    char buf[8192];
    static int16_t io[MOVE_FRAMES_PER_BLOCK * 2];

    /* -- parameter round trips ---------------------------------------- */
    CHECK(fx->get_param(inst, "name", buf, sizeof buf) > 0
          && !strcmp(buf, "Tape Echo 2"), "name = \"Tape Echo 2\"");

    fx->set_param(inst, "mode", "H2+R");
    fx->get_param(inst, "mode", buf, sizeof buf);
    CHECK(!strcmp(buf, "H2+R"), "enum set/get by name (mode = H2+R)");

    fx->set_param(inst, "mode", "0");
    fx->get_param(inst, "mode", buf, sizeof buf);
    CHECK(!strcmp(buf, "H1"), "enum set by index, get by name");

    /* Movy commits enums BY NAME (verified against its model: the knob writes
     * the option string, not an index), so every published option must survive
     * a name -> set_param -> get_param round trip. */
    {
        const char *modes[12] = { "H1", "H2", "H3", "H2+3", "H1+R", "H2+R",
                                  "H3+R", "H12+R", "H23+R", "H13+R", "H123R", "Rev" };
        bool allOk = true;
        for (int i = 0; i < 12; i++) {
            fx->set_param(inst, "mode", modes[i]);
            fx->get_param(inst, "mode", buf, sizeof buf);
            if (strcmp(buf, modes[i]) != 0) { allOk = false; break; }
        }
        CHECK(allOk, "all 12 mode names round trip by name");
    }

    fx->set_param(inst, "intensity", "0.42");
    fx->get_param(inst, "intensity", buf, sizeof buf);
    CHECK(fabs(atof(buf) - 0.42) < 1e-3, "float round trip (intensity)");

    int n = fx->get_param(inst, "chain_params", buf, sizeof buf);
    CHECK(n > 0 && buf[0] == '[' && strstr(buf, "\"wow_flutter\"")
          && strstr(buf, "\"H123R\""), "chain_params JSON served (%d bytes)", n);

    /* Drift gate: module.json must embed the exact chain_params the plugin
     * serves (the chain UI and the type-metadata parser read the file, not
     * the plugin). Pass the module.json path as argv[2] to enable. */
    if (argc > 2) {
        FILE *mj = fopen(argv[2], "r");
        CHECK(mj != nullptr, "module.json readable (%s)", argv[2]);
        if (mj) {
            static char json[65536];
            size_t nr = fread(json, 1, sizeof json - 1, mj);
            json[nr] = 0;
            fclose(mj);
            const char *cp = strstr(json, "\"chain_params\"");
            const char *arr = cp ? strchr(cp, '[') : nullptr;
            bool match = false;
            if (arr) {
                int depth = 1;
                const char *e = arr + 1;
                while (*e && depth > 0) {
                    if (*e == '[') depth++;
                    else if (*e == ']') depth--;
                    e++;
                }
                /* the file is pretty-printed; compare with whitespace
                 * outside of strings stripped */
                static char flat[16384];
                size_t w = 0;
                bool instr = false;
                for (const char *s = arr; s < e && w < sizeof flat - 1; s++) {
                    if (*s == '"' && s[-1] != '\\') instr = !instr;
                    if (instr || (*s != ' ' && *s != '\n' && *s != '\t'))
                        flat[w++] = *s;
                }
                flat[w] = 0;
                match = strcmp(flat, buf) == 0;
            }
            CHECK(match, "module.json chain_params matches plugin exactly");
        }
    }

    /* -- ui_hierarchy: 8 encoders, but EVERY param published ------------ */
    n = fx->get_param(inst, "ui_hierarchy", buf, sizeof buf);
    {
        int knobs = 0, params = 0;
        const char *k = strstr(buf, "\"knobs\":[");
        const char *ps = strstr(buf, "\"params\":[");
        if (k) for (const char *c = k + 9; *c && *c != ']'; c++) if (*c == '"' && c[-1] != '\\') knobs++;
        /* count top-level objects; an option list nests its own [ ] and { } */
        if (ps) {
            int depth = 1, brace = 0;
            for (const char *c = ps + 10; *c; c++) {
                if      (*c == '[') depth++;
                else if (*c == ']') { if (--depth == 0) break; }
                else if (*c == '{') { if (++brace == 1) params++; }
                else if (*c == '}') brace--;
            }
        }
        knobs /= 2;   /* one open + one close quote per key */
        CHECK(n > 0 && knobs > 0 && knobs <= 8,
              "ui_hierarchy declares %d knobs (Move has 8 encoders)", knobs);
        CHECK(params == 16,
              "ui_hierarchy publishes all 16 published params (got %d)", params);
    }

    /* -- state round trip ---------------------------------------------- */
    fx->set_param(inst, "treble", "-0.5");
    n = fx->get_param(inst, "state", buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "\"te2\":1") && strstr(buf, "\"treble\":-0.5"),
          "state blob carries edits");
    char state[8192];
    strcpy(state, buf);
    fx->set_param(inst, "treble", "0");
    fx->set_param(inst, "state", state);
    fx->get_param(inst, "treble", buf, sizeof buf);
    CHECK(fabs(atof(buf) + 0.5) < 1e-3, "state restore returns treble = -0.5");

    /* -- unpublished but still settable (not on any knob page) ---------- */
    CHECK(!strstr(buf, "\"output_volume\"") && !strstr(buf, "\"echo_pan\"")
          && !strstr(buf, "\"power\"") && !strstr(buf, "\"preset\""),
          "ui_hierarchy omits output_volume / pans / power / preset");
    fx->set_param(inst, "output_volume", "0.75");
    fx->get_param(inst, "output_volume", buf, sizeof buf);
    CHECK(fabs(atof(buf) - 0.75) < 1e-3, "unpublished output_volume still settable");
    fx->set_param(inst, "output_volume", "0.5");

    /* -- factory preset ------------------------------------------------ */
    fx->set_param(inst, "preset", "Dub Throw");
    fx->get_param(inst, "mode", buf, sizeof buf);
    CHECK(!strcmp(buf, "H2+R"), "preset \"Dub Throw\" lands on mode 6 (H2+R)");

    /* -- ping-pong ------------------------------------------------------ */
    {
        static int16_t sa[MOVE_FRAMES_PER_BLOCK * 2];
        /* Correlation between the two outputs: 1.0 = the mono bus the original
         * produces, near 0 = the sides carrying different repeats. */
        struct Probe {
            static double corr(audio_fx_api_v2_t *fx, const char *mode, const char *pp) {
                void *i = fx->create_instance(".", nullptr);
                const char *cfg[][2] = { {"mode",mode}, {"repeat_rate","0.5"}, {"intensity","0.5"},
                                         {"echo_volume","1.0"}, {"mix","1.0"}, {"wow_flutter","0"},
                                         {"tape_age","New"}, {"ping_pong",pp}, {"stereo_width","100"} };
                for (int c = 0; c < 9; c++) fx->set_param(i, cfg[c][0], cfg[c][1]);
                static int16_t io[MOVE_FRAMES_PER_BLOCK * 2];
                for (int b = 0; b < 400; b++) { memset(io, 0, sizeof io); fx->process_block(i, io, MOVE_FRAMES_PER_BLOCK); }
                double num = 0, dl = 0, dr = 0;
                for (int b = 0; b < 400; b++) {
                    memset(io, 0, sizeof io);
                    if (b % 80 == 0) for (int k = 0; k < 8; k++) io[k*2] = io[k*2+1] = 20000;
                    fx->process_block(i, io, MOVE_FRAMES_PER_BLOCK);
                    if (b > 100) for (int k = 0; k < MOVE_FRAMES_PER_BLOCK; k++) {
                        const double l = io[k*2], r = io[k*2+1];
                        num += l * r; dl += l * l; dr += r * r;
                    }
                }
                fx->destroy_instance(i);
                return (dl > 0 && dr > 0) ? num / sqrt(dl * dr) : 1.0;
            }
        };
        const char *modes[3] = { "H1", "H2+3", "H123R" };
        bool offMono = true, onWide = true;
        for (int m = 0; m < 3; m++) {
            if (Probe::corr(fx, modes[m], "Off") < 0.99) offMono = false;
            /* every head runs its own square, so a multi-head mode separates
             * too — that is what doing this per head buys over one square on
             * the summed bus */
            if (Probe::corr(fx, modes[m], "On") > 0.5) onWide = false;
        }
        CHECK(offMono, "ping-pong off leaves the echo bus mono, as upstream");
        CHECK(onWide, "ping-pong separates the sides in single AND multi-head modes");

        /* successive repeats must land on opposite sides */
        void *shl = fx->create_instance(".", nullptr);
        const char *c2[][2] = { {"mode","H1"}, {"repeat_rate","1.0"}, {"intensity","0.62"},
                                {"echo_volume","1.0"}, {"mix","1.0"}, {"wow_flutter","0"},
                                {"tape_age","New"}, {"ping_pong","On"}, {"stereo_width","100"} };
        for (int c = 0; c < 9; c++) fx->set_param(shl, c2[c][0], c2[c][1]);
        for (int b = 0; b < 500; b++) { memset(sa, 0, sizeof sa); fx->process_block(shl, sa, MOVE_FRAMES_PER_BLOCK); }
        int found = 0, flips = 0, prev = 0;
        bool prevQuiet = true;
        for (int b = 0; b < 120 && found < 5; b++) {
            memset(sa, 0, sizeof sa);
            if (b == 0) { sa[0] = sa[1] = 24000; }
            fx->process_block(shl, sa, MOVE_FRAMES_PER_BLOCK);
            double l = 0, r = 0;
            for (int k = 0; k < MOVE_FRAMES_PER_BLOCK; k++) { l += fabs((double)sa[k*2]); r += fabs((double)sa[k*2+1]); }
            const bool loud = (l + r) > 4000;
            if (loud && prevQuiet && b > 0) {
                const int side = l > r ? 1 : -1;
                if (found > 0 && side != prev) flips++;
                prev = side; found++;
            }
            prevQuiet = !loud;
        }
        CHECK(found >= 4 && flips >= found - 1,
              "every repeat swaps sides (%d repeats, %d alternations)", found, flips);
        fx->destroy_instance(shl);
    }

    /* -- A fresh instance IS an untouched TapeDelay ---------------------- */
    /* This module inherited TapeDelay's id, so inserting it has to land
     * where inserting that one landed. Rather than trust three hand-copied
     * numbers in te2_params.h, drive the real import with the state
     * TapeDelay's own create_instance produced and require a brand new
     * instance to agree with it. Change the mapping and this fails; change
     * the defaults away from the mapping and this fails. */
    {
        void *fresh = fx->create_instance(".", nullptr);
        void *imported = fx->create_instance(".", nullptr);
        CHECK(fresh && imported, "two instances for the defaults comparison");
        if (fresh && imported) {
            /* verbatim from TapeDelay v0.4.3 create_instance */
            fx->set_param(imported, "state",
                "{\"time\":400,\"feedback\":0.4000,\"mix\":0.5000,\"tone\":0.5000,"
                "\"stereo_width\":0,\"division\":\"free\",\"bpm\":120}");

            static const char *const kMapped[] = {
                "mode", "repeat_rate", "intensity", "echo_volume", "mix", "treble",
                "tempo_sync", "stereo_width", "ping_pong",
            };
            bool agree = true;
            for (const char *k : kMapped) {
                char a[128] = {0}, b[128] = {0};
                fx->get_param(fresh, k, a, sizeof a);
                fx->get_param(imported, k, b, sizeof b);
                /* floats: compare numerically, the printf width is not the point */
                const bool same = !strcmp(a, b)
                    || (a[0] && b[0] && (isdigit((unsigned char)a[0]) || a[0] == '-')
                        && fabs(atof(a) - atof(b)) < 1e-3);
                if (!same) {
                    printf("  defaults differ on %-14s fresh=%-10s imported=%s\n", k, a, b);
                    agree = false;
                }
            }
            CHECK(agree, "fresh defaults == an imported untouched TapeDelay");

            /* and the one that matters by ear: measure it. A default insert
             * must echo at TapeDelay's 400 ms, not this engine's 177 ms. */
            char m[64] = {0};
            fx->get_param(fresh, "mode", m, sizeof m);
            CHECK(!strcmp(m, "H3"), "default mode is the head that reaches 400 ms (%s)", m);

            fx->set_param(fresh, "intensity", "0");    /* one repeat to find */
            fx->set_param(fresh, "echo_volume", "1.0");
            fx->set_param(fresh, "mix", "1.0");        /* wet only */
            for (int b = 0; b < 400; b++) {            /* settle the smoothers */
                memset(io, 0, sizeof io);
                fx->process_block(fresh, io, MOVE_FRAMES_PER_BLOCK);
            }
            double peak = 0; int at = -1;
            for (int b = 0; b < 260; b++) {
                memset(io, 0, sizeof io);
                if (b == 0) io[0] = io[1] = 20000;
                fx->process_block(fresh, io, MOVE_FRAMES_PER_BLOCK);
                for (int i = 0; i < MOVE_FRAMES_PER_BLOCK; i++) {
                    double a = fabs((double)io[i * 2]);
                    if (a > peak) { peak = a; at = b * MOVE_FRAMES_PER_BLOCK + i; }
                }
            }
            const double dms = at * 1000.0 / 44100.0;
            CHECK(fabs(dms - 400.0) < 12.0,
                  "default echo lands at TapeDelay's 400 ms (%.1f ms)", dms);
        }
        if (fresh) fx->destroy_instance(fresh);
        if (imported) fx->destroy_instance(imported);
    }

    /* -- the two import corrections, at points where they were WRONG ---- */
    /* Both of these shipped broken once, and neither is visible by reading
     * the code — the numbers only came out of measuring against the old
     * engine. Pin the shape of each so a "simplification" cannot quietly
     * restore the version that was wrong. */
    {
        /* feedback -> intensity is per head. A single table fitted on head 3
         * is 11.5 dB/repeat out on head 1, so H1 and H3 must NOT agree. */
        void *a = fx->create_instance(".", nullptr);
        void *b = fx->create_instance(".", nullptr);
        char h1[64] = {0}, h3[64] = {0};
        fx->set_param(a, "mode", "H1"); fx->set_param(a, "feedback", "0.4000");
        fx->set_param(b, "mode", "H3"); fx->set_param(b, "feedback", "0.4000");
        fx->get_param(a, "intensity", h1, sizeof h1);
        fx->get_param(b, "intensity", h3, sizeof h3);
        CHECK(fabs(atof(h1) - 0.55) < 0.02,
              "feedback 0.4 on head 1 -> intensity 0.55 (%s)", h1);
        CHECK(fabs(atof(h3) - 0.44) < 0.02,
              "feedback 0.4 on head 3 -> intensity 0.44 (%s)", h3);
        CHECK(atof(h1) - atof(h3) > 0.05,
              "the feedback table is per head, not shared");
        fx->destroy_instance(a); fx->destroy_instance(b);

        /* mix -> echo_volume is 1/(2(1-m)) below noon, NOT m/(1-m). Both give
         * 1.0 at m=0.5, which is exactly why the wrong one shipped: the
         * default could not see it. Check away from noon. */
        static const struct { const char *mix; double want; } kMix[] = {
            { "0.2000", 0.625 }, { "0.3500", 0.769 }, { "0.5000", 1.0 },
        };
        for (auto &c : kMix) {
            void *m = fx->create_instance(".", nullptr);
            char st[512];
            snprintf(st, sizeof st,
                     "{\"time\":400,\"feedback\":0.4000,\"mix\":%s,\"tone\":0.5000,"
                     "\"stereo_width\":0,\"division\":\"free\",\"bpm\":120}", c.mix);
            fx->set_param(m, "state", st);
            char got[64] = {0};
            fx->get_param(m, "echo_volume", got, sizeof got);
            CHECK(fabs(atof(got) - c.want) < 0.02,
                  "mix %s -> echo_volume %.3f (%s)", c.mix, c.want, got);
            fx->destroy_instance(m);
        }
    }

    /* -- TapeDelay (schwung-space-delay) preset import ------------------ */
    {
        /* its patch blob, verbatim from that module's get_param("state") */
        const char *legacy =
            "{\"time\":250,\"feedback\":0.8000,\"mix\":0.5000,\"tone\":0.3000,"
            "\"stereo_width\":50,\"division\":\"1/8\",\"bpm\":120}";
        fx->set_param(inst, "state", legacy);

        fx->get_param(inst, "mode", buf, sizeof buf);
        CHECK(!strcmp(buf, "H2"), "legacy 250 ms picks the head that reaches it (%s)", buf);
        fx->get_param(inst, "tempo_sync", buf, sizeof buf);
        CHECK(!strcmp(buf, "On"), "legacy division turns tempo sync on");
        fx->get_param(inst, "echo_note_name", buf, sizeof buf);
        CHECK(!strcmp(buf, "1/8"), "legacy 1/8 lands on a 1/8 detent (%s)", buf);
        fx->get_param(inst, "mix", buf, sizeof buf);
        CHECK(fabs(atof(buf) - 0.5) < 1e-3, "legacy mix carries over");
        fx->get_param(inst, "intensity", buf, sizeof buf);
        /* 250 ms lands on head 2, where feedback 0.8 calibrates to 0.55. The
         * 0.60 this expected is the head 3 figure, which is what the old
         * shared table returned for every patch regardless of head. */
        CHECK(fabs(atof(buf) - 0.55) < 1e-2,
              "legacy feedback 0.8 on head 2 -> intensity 0.55 (%s)", buf);
        fx->get_param(inst, "stereo_width", buf, sizeof buf);
        CHECK(atoi(buf) == 50, "legacy stereo_width 50 carries over (%s)", buf);
        fx->get_param(inst, "ping_pong", buf, sizeof buf);
        CHECK(!strcmp(buf, "On"), "legacy width arms ping-pong");

        /* and it must actually DELAY by ~250 ms, not merely claim to */
        fx->set_param(inst, "tempo_sync", "Off");   /* honour the stored time */
        fx->set_param(inst, "intensity", "0");
        fx->set_param(inst, "echo_volume", "1.0");
        fx->set_param(inst, "mix", "1.0");
        for (int b = 0; b < 400; b++) {             /* settle the smoothers */
            memset(io, 0, sizeof io);
            fx->process_block(inst, io, MOVE_FRAMES_PER_BLOCK);
        }
        double peak = 0; int at = -1;
        for (int b = 0; b < 260; b++) {
            memset(io, 0, sizeof io);
            if (b == 0) io[0] = io[1] = 20000;
            fx->process_block(inst, io, MOVE_FRAMES_PER_BLOCK);
            for (int i = 0; i < MOVE_FRAMES_PER_BLOCK; i++) {
                double a = fabs((double)io[i * 2]);
                if (a > peak) { peak = a; at = b * MOVE_FRAMES_PER_BLOCK + i; }
            }
        }
        const double ms = at * 1000.0 / 44100.0;
        CHECK(fabs(ms - 250.0) < 20.0,
              "legacy 250 ms preset echoes at %.0f ms", ms);
    }

    /* an unknown blob must be ignored, not half-applied */
    fx->set_param(inst, "state", "{\"nonsense\":1}");
    fx->get_param(inst, "mode", buf, sizeof buf);
    CHECK(!strcmp(buf, "H2"), "unknown state blob leaves settings alone");

    fx->set_param(inst, "preset", "Default");

    /* -- audio: an impulse must come back with a delayed echo ----------- */
    fx->set_param(inst, "preset", "Default");
    fx->set_param(inst, "echo_volume", "1.0");
    fx->set_param(inst, "intensity", "0.4");
    fx->set_param(inst, "mix", "0.5");

    const int totalBlocks = 200; /* ~580 ms */
    double energyEarly = 0, energyEcho = 0;
    for (int b = 0; b < totalBlocks; b++) {
        memset(io, 0, sizeof io);
        if (b == 0) io[0] = io[1] = 20000; /* stereo impulse */
        fx->process_block(inst, io, MOVE_FRAMES_PER_BLOCK);
        double e = 0;
        for (int i = 0; i < MOVE_FRAMES_PER_BLOCK * 2; i++)
            e += (double)io[i] * (double)io[i];
        const double ms = b * 128.0 * 1000.0 / 44100.0;
        if (ms > 30 && ms < 120)  energyEarly += e; /* before head 1 (~177 ms) */
        if (ms > 150 && ms < 400) energyEcho  += e; /* head-1 repeats land here */
    }
    CHECK(energyEcho > 1e4, "echo tail energy present (%.0f)", energyEcho);
    CHECK(energyEcho > energyEarly, "echo louder than pre-echo window");

    /* -- power off = clean passthrough ---------------------------------- */
    fx->set_param(inst, "power", "Off");
    /* The power crossfade is an asymptotic one-pole (~20 ms tau) and the
     * echo tail from the impulse test is still decaying — give both ~900 ms
     * before demanding sample-exact passthrough. */
    for (int b = 0; b < 320; b++) {
        memset(io, 0, sizeof io);
        fx->process_block(inst, io, MOVE_FRAMES_PER_BLOCK);
    }
    for (int i = 0; i < MOVE_FRAMES_PER_BLOCK; i++) {
        io[i * 2] = (int16_t)(8000.0 * sin(2.0 * M_PI * 441.0 * i / 44100.0));
        io[i * 2 + 1] = io[i * 2];
    }
    static int16_t ref[MOVE_FRAMES_PER_BLOCK * 2];
    memcpy(ref, io, sizeof ref);
    fx->process_block(inst, io, MOVE_FRAMES_PER_BLOCK);
    long maxDiff = 0;
    for (int i = 0; i < MOVE_FRAMES_PER_BLOCK * 2; i++) {
        long d = labs((long)io[i] - (long)ref[i]);
        if (d > maxDiff) maxDiff = d;
    }
    CHECK(maxDiff <= 1, "power-off passthrough (max sample diff %ld)", maxDiff);
    fx->set_param(inst, "power", "On");

    /* -- realtime factor ------------------------------------------------ */
    fx->set_param(inst, "preset", "Full Wash"); /* heads + spring: worst case */
    const int benchBlocks = 2000;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int b = 0; b < benchBlocks; b++)
        fx->process_block(inst, io, MOVE_FRAMES_PER_BLOCK);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double elapsedMs = (t1.tv_sec - t0.tv_sec) * 1000.0
                           + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    const double perBlockMs = elapsedMs / benchBlocks;
    const double budgetMs = 128.0 * 1000.0 / 44100.0; /* 2.902 ms */
    printf("  -- %.3f ms/block (budget %.3f ms, %.1f%% of one core)\n",
           perBlockMs, budgetMs, 100.0 * perBlockMs / budgetMs);
    CHECK(perBlockMs < budgetMs, "faster than realtime");

    fx->destroy_instance(inst);
    dlclose(h);

    printf(g_failures ? "\n%d FAILURES\n" : "\nALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
