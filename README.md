# TapeDelay

Tape delay audio effect for [Schwung](https://github.com/charlesvestal/schwung)
on Ableton Move. Module id `tapedelay`.

A circular delay line with a flutter LFO, a one-pole tone control and soft
saturation on the feedback path. Deliberately cheap: **~0.013 ms per block on
the Move, about 1.4% of the DSP budget**, so you can put it on every slot and
still have the machine to yourself.

## Controls

- **Time** — 20 ms to 2 s
- **Division** — free, or synced (1/1 … 1/16t)
- **Feedback** — repeats, 0–95%
- **Mix** — dry/wet
- **Tone** — lowpass on the repeats, 500 Hz to 12 kHz
- **Stereo Width** — 0 = mono, 100 = full L/R ping-pong

## About v1.3.x, and why this is v2.0.0

For one day (2026-08-22) this id shipped a different engine: **Tape Echo 2**, a
component-modelled three-head machine with a spring tank, ported from Dusk
Audio by athousanddetails. It sounds far better than this module and it is not
a replacement for it — measured on the device it costs **0.37 ms per block,
about 41% of the DSP budget, so only two instances fit** where this one fits
dozens. A utility delay that can only be used twice is not a utility delay.

So `tapedelay` is the cheap delay again, and Tape Echo 2 is its own module.
Install both; they are for different jobs.

The version is 2.0.0 rather than 0.4.4 for a mechanical reason: the store
compares versions, and anyone who took the v1.3.x update would never be offered
anything numbered below it. If you were on v1.3.x, this update returns you to
this engine and your Tape Echo 2 settings will not carry across — the two have
nothing in common to carry. Nothing breaks; the old engine ignores a Tape Echo 2
patch and comes up at its defaults.

Tape Echo 2's source is GPL-3.0 and remains in this repository's history at tags
`v1.3.3`–`v1.3.5`. This tree is MIT again.

## Building

```bash
./scripts/build.sh      # ARM64 via Docker
./scripts/install.sh    # deploy to move.local
```

## Credits

Original spacecho engine by **cyrusasfa**, ported by charlesvestal. MIT.
