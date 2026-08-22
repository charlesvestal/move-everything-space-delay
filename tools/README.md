# tools/

`check_config.py`, `loadtest.cpp` and `movy_layout.mjs` are part of the build —
see `scripts/docker-build.sh`.

The other two are **not built by default**. They exist because the legacy-import
numbers in `src/dsp/tape_echo_plugin.cpp` and the defaults in
`src/dsp/te2_params.h` are described as *measured*, and a measured number that
cannot be re-measured is just a number somebody typed.

Both need the old TapeDelay engine, which is not in the tree — it is in this
repository's history, up to `v0.4.3`:

```bash
mkdir -p /tmp/old && cd <repo>
for f in spacecho.c audio_fx_api_v1.h plugin_api_v1.h; do
    git show v0.4.3:src/dsp/$f > /tmp/old/$f
done
docker run --rm -v "$PWD:/build" -v /tmp/old:/old -u "$(id -u):$(id -g)" \
    -w /build schwung-tapedelay-builder bash -c '
  mkdir -p build-native
  gcc -O2 -shared -fPIC /old/spacecho.c -I/old -o build-native/old.so -lm
  g++ -O2 -std=c++17 -shared -fPIC src/dsp/tape_echo_plugin.cpp \
      src/ported/core/TapeEchoDSP.cpp -Isrc -Isrc/host -Isrc/ported/core \
      -Isrc/ported/shared-dpf/dsp -o build-native/new.so -lm -lpthread
  g++ -O2 -std=c++17 tools/ab_vs_tapedelay.cpp -Isrc -Isrc/host \
      -o build-native/ab -ldl -lm
  g++ -O2 -std=c++17 tools/calibrate_legacy_feedback.cpp -Isrc -Isrc/host \
      -o build-native/calib -ldl -lm
  ./build-native/ab    build-native/old.so build-native/new.so
  ./build-native/calib build-native/old.so build-native/new.so'
```

## `ab_vs_tapedelay.cpp`

Runs both engines **at their own defaults, setting nothing**, and reports what a
listener would notice: when each repeat lands and how loud it is, the brightness
of the first repeat, and the level change on program material.

The state it is asserting (2026-08-22):

```
                      OLD TapeDelay        NEW Tape Echo 2
  repeat #1        399.1 ms  -10.6 dB    403.4 ms  -12.9 dB
  repeat #2        799.6 ms  -26.2 dB    805.4 ms  -25.9 dB
  brightness          2263 Hz               2046 Hz
  program level      -0.63 dB              +0.40 dB
```

The 2.3 dB on the first repeat is the one thing that does not close: Echo Volume
is already at its 1.0 ceiling. By the second repeat they are 0.3 dB apart.

**Window the brightness measurement past the dry hit.** The loudest sample is
the dry impulse, and the old engine is silent for 400 ms after it, so a window
anchored there reports 0 Hz for a reason that has nothing to do with tone. That
mistake is why an earlier revision of this file claimed the old engine had no
top end.

## `calibrate_legacy_feedback.cpp`

Generates the feedback → intensity table in `te2_legacy_feedback`, by matching
the dB lost per repeat rather than the nominal gain. Prints the new engine's
intensity → decay curve, then inverts it for each old feedback value, **per
head**.

The ratio is **not** constant — it runs from 2.0 at feedback 0.1 down to 0.72 at
0.95, because this engine's loop carries record EQ, saturation and head losses
that TapeDelay's plain loop gain did not. It also shows where the engine starts
to sustain (intensity ≈ 0.74), which is what the top of the table stays under.

## `scope_legacy_mapping.cpp` — run this after touching either mapping

Both mappings were first fitted at a single operating point, and **both were
wrong away from it.** Neither error was visible by reading the code; the only
thing that found them was measuring somewhere else:

- The feedback table was fitted on head 3 and applied to all heads. On head 1
  it was **11.5 dB per repeat** out — head 1 needs intensity 0.55 where head 3
  needs 0.44 for the same feedback 0.4. It is per head now. (Only heads 1–3 are
  fitted: the importer picks a single head, so the multi-head modes cannot
  arrive through this path.)
- `mix → echo_volume` shipped in 1.3.4 as `m/(1-m)`, the old crossfade's
  wet/dry ratio. That forgets that Echo Volume multiplies a law which already
  has a ratio of its own, so what is wanted is the quotient — `1/(2(1-m))`
  below noon, `2m` above. The two agree at exactly m = 0.5, which is the
  default, which is why testing the default could not see it. At mix 0.2 it was
  **16.7 dB** light; it is now 4.9.

This tool sweeps modes, tape age and mix, and drives the **real import path**
rather than recomputing the rule — an earlier version recomputed it and
therefore agreed with itself while the plugin was wrong.

Residual after both fixes, first repeat vs the old engine: −2.3 dB at mix 0.5,
−4.9 at 0.2, −6.3 at 0.8. Above noon it is a ceiling, not a mistake: Echo
Volume is already at 1.0 and cannot go further.

Tape age turned out not to matter (0.13 dB across New/Used/Old), so the table
is not fitted against it.
