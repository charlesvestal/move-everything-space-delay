/* Native validation of the vendored Tape Echo 2 core + the tempo-sync math
 * the Move shell relies on. Compiled and RUN in-container before every
 * cross-compile; a red check fails the whole build.
 *
 * The vendored TapeEchoParams.hpp already carries its own static_asserts
 * (sync table validity, round trips, ABI ordering) — those gate compilation
 * for free. These are the runtime checks on top.
 */

#include <cmath>
#include <cstdio>
#include <cstring>

#include "TapeEchoDSP.hpp"

using duskaudio::TapeEchoDSP;

static int g_failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
    else         {              printf("  ok: " __VA_ARGS__); printf("\n"); } \
} while (0)

static constexpr int kBlock = 128;
static constexpr double kFs = 44100.0;

int main()
{
    /* -- rate <-> delay mapping is a true inverse ----------------------- */
    for (float r = 0.0f; r <= 1.001f; r += 0.1f) {
        const float rr = r > 1.0f ? 1.0f : r;
        const float ms = TapeEchoDSP::delayMsForRepeatRate(rr);
        const float back = TapeEchoDSP::repeatRateForDelayMs(ms);
        if (fabsf(back - rr) > 1e-3f) {
            CHECK(false, "repeatRate round trip at %.1f (got %.4f)", rr, back);
            break;
        }
    }
    CHECK(true, "repeatRateForDelayMs inverts delayMsForRepeatRate");
    CHECK(fabsf(TapeEchoDSP::delayMsForRepeatRate(0.0f) - TapeEchoDSP::kMaxDelayMs) < 0.5f,
          "rate 0 = slowest motor (%.1f ms)", (double)TapeEchoDSP::kMaxDelayMs);
    CHECK(fabsf(TapeEchoDSP::delayMsForRepeatRate(1.0f) - TapeEchoDSP::kMinDelayMs) < 0.5f,
          "rate 1 = fastest motor (%.1f ms)", (double)TapeEchoDSP::kMinDelayMs);

    /* -- sync math: 1/8 at 120 BPM is 250 ms ---------------------------- */
    int div18 = -1;
    for (int i = 0; i < kNumSyncDivisions; i++)
        if (!strcmp(kSyncDivisions[i].name, "1/8")) div18 = i;
    CHECK(div18 >= 0, "division table contains 1/8");
    CHECK(fabs(syncDelayMs(120.0, div18) - 250.0) < 1e-6, "1/8 @ 120 BPM = 250 ms");

    /* the detent -> division mapping follows the leading head of the mode */
    CHECK(teLeadingHeadIndexForMode(6) == 1 && teLeadingHeadIndexForMode(1) == 0,
          "leading head: mode 6 -> head 2, mode 1 -> head 1");

    /* -- silence in, near-silence out (the repro noise bed is ~-124 dBFS) */
    {
        TapeEchoDSP dsp;
        dsp.prepare(kFs, kBlock);
        dsp.reset();
        dsp.setEchoLevel(1.0f);
        float L[kBlock], R[kBlock];
        const float *ins[2] = { L, R };
        float *outs[2] = { L, R };
        double peak = 0;
        for (int b = 0; b < 400; b++) {
            memset(L, 0, sizeof L);
            memset(R, 0, sizeof R);
            dsp.processBlock(ins, outs, 2, kBlock);
            for (int i = 0; i < kBlock; i++) {
                const double a = fabs((double)L[i]);
                if (a > peak) peak = a;
            }
        }
        CHECK(peak < 1e-3, "silence stays silent (peak %.2e)", peak);
    }

    /* -- an impulse produces a head-1 echo near the motor delay --------- */
    {
        TapeEchoDSP dsp;
        dsp.prepare(kFs, kBlock);
        dsp.reset();
        dsp.setMode(1);
        dsp.setRepeatRate(0.0f);   /* slowest: ~177 ms */
        dsp.setIntensity(0.0f);
        dsp.setEchoLevel(1.0f);
        dsp.setMix(1.0f);          /* wet only: the echo is unmistakable */

        float L[kBlock], R[kBlock];
        const float *ins[2] = { L, R };
        float *outs[2] = { L, R };

        /* let the per-sample smoothers glide to the targets (mix starts at
         * its 0.5 default, so an immediate impulse would leak through dry) */
        for (int b = 0; b < 120; b++) {
            memset(L, 0, sizeof L);
            memset(R, 0, sizeof R);
            dsp.processBlock(ins, outs, 2, kBlock);
        }

        const int totalBlocks = 200;
        double bestAmp = 0;
        int bestSample = -1;
        for (int b = 0; b < totalBlocks; b++) {
            memset(L, 0, sizeof L);
            memset(R, 0, sizeof R);
            if (b == 0) L[0] = R[0] = 0.7f;
            dsp.processBlock(ins, outs, 2, kBlock);
            for (int i = 0; i < kBlock; i++) {
                const double a = fabs((double)L[i]);
                if (a > bestAmp) { bestAmp = a; bestSample = b * kBlock + i; }
            }
        }
        const double echoMs = bestSample * 1000.0 / kFs;
        const double expectMs = (double)TapeEchoDSP::kMaxDelayMs
            + (double)TapeEchoDSP::leadingHeadOffsetMsForMode(1);
        CHECK(bestAmp > 0.01, "echo audible (peak %.3f)", bestAmp);
        CHECK(fabs(echoMs - expectMs) < 25.0,
              "head-1 echo at %.1f ms (expected ~%.1f ms)", echoMs, expectMs);
    }

    printf(g_failures ? "\n%d FAILURES\n" : "\nALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
