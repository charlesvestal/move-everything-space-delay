# CLAUDE.md

Instructions for Claude Code when working with this repository.

## Project Overview

`tapedelay` is an audio_fx module for [Schwung](https://github.com/charlesvestal/schwung).
Since v1.3.3 the engine is **Tape Echo 2** by Dusk Audio (GPL-3.0), ported to
Move by athousanddetails. Up to v0.4.3 this id shipped a much simpler MIT
delay (`src/dsp/spacecho.c`, by cyrusasfa) — that code is in the history, not
in the tree.

**The id stayed `tapedelay` on purpose.** The Module Store extracts a tarball's
own top-level directory into `modules/audio_fx/`, so keeping the id is what
makes this an in-place upgrade rather than a second module sitting alongside
the old one. Renaming the id would strand every existing patch and slot
autosave.

## Architecture

```
src/
  dsp/
    tape_echo_plugin.cpp   # audio_fx v2 shell: params, state, legacy import
    te2_params.h           # the C-side parameter table
  host/                    # Schwung API headers (audio_fx_api_v2.h, plugin_api_v1.h)
  ported/
    core/TapeEchoDSP.{hpp,cpp}   # Dusk Audio's core + the ping-pong addition
    dpf-plugin/TapeEchoParams.hpp
    shared-dpf/dsp/              # Dusk shared DSP primitives, snapshot at vendor time
  module.json  movy_config.json  ui_chain.js  help.json  web_ui.html
```

## The .so is named after the id, not "dsp.so"

For `audio_fx`, Schwung's chain host dlopens `<audio_fx>/<id>/<id>.so` and
**ignores** module.json's `dsp` field. So the CMake target's `OUTPUT_NAME` is
`tapedelay` and the packaged file is `tapedelay/tapedelay.so`. Only
`sound_generator` modules use `dsp.so`.

## Legacy state import

`tape_echo_plugin.cpp` sniffs an incoming `state` blob: if it does not parse as
ours it is tried as an old TapeDelay state and mapped (time → the head that can
reach it, feedback → intensity, tone → treble, division, mix, stereo_width →
ping-pong width). The old parameter *names* are also accepted individually by
`set_param`. Neither path keys on the module id, so both keep working now that
the engine ships under the id the old one used.

**Two of those mappings are measured, not reasoned.** Both were wrong in a way
that only showed up by ear, and the fix in each case was to render an impulse
through both engines and compare (`tools/README.md` has the how):

- `feedback → intensity` was a flat `v * 0.75`, on the correct observation that
  this engine sustains past ~0.75 and TapeDelay never did. But this loop also
  carries record EQ, saturation and head losses, so the same nominal gain
  decays faster — and proportionally worse the lower you go. The measured ratio
  runs from **2.0** at feedback 0.1 to **0.72** at 0.95; the flat scale was
  right at 0.80 and nowhere else. At the old default of 0.4 it asked for 0.30
  where 0.44 is needed, so **every** imported patch came back a repeat or two
  short. It is a calibration table now.
- `mix → echo_volume` did not exist. TapeDelay's Mix was a crossfade, so the
  wet/dry ratio it produced was `m/(1-m)`; this engine's Mix is a unity-overlap
  law, so that ratio has to come from Echo Volume or the repeats arrive quiet.

The one-at-a-time key path deliberately does **not** apply the echo-volume
correction: `mix` is also this engine's own parameter name, so there is no way
to distinguish an old module replaying its patch from a user turning the knob.
A whole state blob is unambiguous; a single key is not.

## Build

```bash
./scripts/build.sh          # Docker, ubuntu:22.04 (glibc 2.35), aarch64 cross
./scripts/deploy.sh <host>  # atomic-rename deploy; never scp over a live .so
```

`scripts/docker-build.sh` runs three gates before cross-compiling. Two notes on
the third, both of which have already cost a red build:

- **The pristine core must be staged, not flat-copied.** `TapeEchoDSP.hpp`
  includes its params header by a path relative to its own directory
  (`../daf-plugin/TapeEchoParams.hpp`), so `-I` cannot redirect it. Upstream
  renamed that directory (`dpf-` → `daf-`) in Aug 2026, which is why the script
  creates both spellings.
- **`-ffp-contract=off` on both sides is load-bearing.** g++ defaults to
  `-ffp-contract=fast` for C++, and merely having the ping-pong branch nearby
  changes which multiply-adds get fused into FMAs. That is a last-bit
  difference per sample fed into a feedback loop, so 400 blocks later the
  checksums differ in the 5th digit and the gate reports a divergence that is
  not in the source. With contraction off the two are identical to every digit.
  Do not "fix" a red equivalence gate by loosening the comparison.

## Release

Bump `src/module.json` version, commit, `git tag vX.Y.Z && git push --tags`.
The workflow builds in Docker, attaches `dist/tapedelay-module.tar.gz`, and
rewrites `release.json` on main. The tag must match module.json or the
workflow fails the version check.
