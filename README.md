# TapeDelay — Tape Echo and Spring Reverb for Ableton Move

Module id `tapedelay` for [Schwung](https://github.com/charlesvestal/schwung).

A component-modelled vintage three-head tape echo with spring reverb. Twelve
head and reverb combinations, record EQ and tape saturation inside the feedback
loop so every repeat darkens and compresses, wow and flutter, tape age, and
tempo sync with the reference machine's leading-head note tables.

The engine is **Tape Echo 2** by [Dusk Audio](https://github.com/dusk-audio/dusk-audio-plugins),
ported to Move by **athousanddetails**. It replaced the original `spacecho.c`
delay that shipped under this id through v0.4.3 — see *Upgrading* below.

## Controls

| Page | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| **Echo** | Mode | Rate | Intensity | Echo Vol | Reverb Vol | Mix | Tempo Sync | Rate Note |
| **Tape** | Drive | Bass | Treble | Wow/Flutter | Tape Age | Input Send | Ping Pong | Width |

Modes are `H1 H2 H3 H2+3 H1+R H2+R H3+R H12+R H23+R H13+R H123R Rev` — digits
are the live playback heads, `R` is the spring tank.

Intensity above ~75% self-oscillates. Input Send is the dub switch: off stops
feeding the tape while existing repeats wash out. Bass and Treble are on the
echo path only.

Ping Pong alternates successive repeats left and right, Width sets how far it
swings. Each head alternates on its own delay, so the multi-head modes separate
too. With it off the echo bus is mono and the output is bit-identical to Dusk
Audio's original — the build checks that against upstream on every run.

## Upgrading from the old TapeDelay (≤ 0.4.3)

Nothing to do. The module id is unchanged, so the Module Store upgrades in
place, and the engine reads the old TapeDelay state directly: the stored delay
time picks the playback head that can reach it, and feedback, mix, tone, note
division and stereo width carry across. Existing patches and slot autosaves
name `tapedelay` and keep working.

The old parameter names (`time`, `feedback`, `tone`, `division`, `mix`,
`stereo_width`) are still accepted as set_param keys for anything that replays
them one at a time.

## Remote panel

A tape-deck style editor in the browser: draggable knobs, mode and rate-note
selectors, tempo sync and input send switches, and a record meter. Open it
while the module is loaded in an FX slot:

```
move.local:7700/api/remote-ui/module-assets/tapedelay/web_ui.html
```

Where Schwung offers custom UIs to FX slots, the same panel appears inside the
module's section on `move.local:7700/remote-ui`. It reads the component it is
driving from the host, so it addresses the right FX slot either way.

Works with [Movy](https://github.com/DimaDake/schwung-movy) — a
`movy_config.json` ships with the module.

## Install

Via the Schwung Module Store, or manually: copy `dist/tapedelay/` to
`/data/UserData/schwung/modules/audio_fx/tapedelay/` on the device.

## Building

Requires Docker (cross-compiles for the Move's ARM64, pinned to glibc 2.35):

```bash
./scripts/build.sh               # builds build/tapedelay.so + dist/tapedelay-module.tar.gz
./scripts/deploy.sh <host>       # safe deploy (atomic rename, never over a live .so)
```

The build gates on three things before it cross-compiles: the config contract
(`movy_config` / `chain_params` / the C table / the chain UI all agree), the
native DSP tests, and a core-equivalence check against a fresh checkout of
upstream.

## Credits

- **[Tape Echo 2](https://github.com/dusk-audio/dusk-audio-plugins/tree/main/plugins/tape-echo)**
  by **Dusk Audio** (GPL-3.0) — the DSP, and all of the modelling credit.
  The core in `src/ported/` carries one Schwung-specific change, marked in the
  files: a per-head ping-pong stage on the echo output tap. Switch Ping Pong
  off and it is upstream sample for sample, which the build verifies against a
  fresh checkout of the original.
- Move port by **athousanddetails**.
- Packaged under the `tapedelay` id by charlesvestal, with the port author's
  permission.

GPL-3.0, inherited from upstream. The original `spacecho.c` TapeDelay engine
(MIT, by cyrusasfa) is in this repository's history up to tag `v0.4.3`.
