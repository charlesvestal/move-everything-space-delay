# CLAUDE.md

Instructions for Claude Code when working with this repository.

## Project Overview

TapeDelay is an audio effect module for Schwung: tape delay with flutter, tone
filtering and soft saturation. Module id `tapedelay`.

## This id briefly shipped a different engine — do not repeat it

For one day (2026-08-22, v1.3.3-v1.3.5) `tapedelay` shipped **Tape Echo 2**, a
component-modelled three-head machine ported from Dusk Audio. It was reverted at
v2.0.0 for one measured reason:

| | this engine | Tape Echo 2 |
|---|---|---|
| per block on the Move | 0.013 ms | 0.37 ms |
| % of the ~900 us DSP budget | 1.4% | 41% |
| instances that fit | dozens | **2** |

**Quote the ~900 us budget, not the 2.902 ms block period.** The block period is
what a loadtest naturally prints and it is three times too generous — the SPI
callback has ~900 us left after the transfer, and that covers all four slots,
master FX and mixing (`docs/REALTIME_SAFETY.md`). Measuring against the wrong
one is how a 41% module looked like a 14% module for a day.

The lesson is about the ID, not the engine: `tapedelay` is a utility delay that
people put on several slots, and swapping its engine for something 28x heavier
changed what the id promises. Tape Echo 2 is a good module; it is its own
module.

Its GPL-3.0 source is in this repository's history at tags `v1.3.3`-`v1.3.5`.
This tree is MIT.

Version numbering: 2.0.0 rather than 0.4.4 because the store compares versions,
and anyone on v1.3.x would never be offered anything lower.

## Architecture

```
src/
  dsp/
    spacecho.c          # Main DSP implementation
    audio_fx_api_v1.h   # Audio FX API (from move-anything)
    plugin_api_v1.h     # Plugin API types (from move-anything)
  module.json           # Module metadata
```

## Key Implementation Details

### Audio FX API

Implements Move Anything audio_fx_api_v1:
- `on_load`: Initialize delay buffer and DSP state
- `on_unload`: Cleanup
- `process_block`: In-place stereo audio processing
- `set_param`: time, feedback, mix, tone, flutter
- `get_param`: Returns current parameter values

### DSP Components

1. **Delay Line**: Circular buffer (~35000 samples for 800ms at 44100Hz)
2. **Flutter LFO**: ~5Hz sine modulating delay read position
3. **Tone Filter**: One-pole lowpass (1kHz to 8kHz)
4. **Soft Saturation**: tanh waveshaping on feedback path
5. **Mix**: Dry/wet crossfade

### Signal Flow

```
Input ---+-------------------------------- Dry ----+
         |                                         |
         +---> Delay Line ---> Tone Filter --> Wet-+---> Mix ---> Output
                   ^               |               |
                   |               v               |
                   +--- Saturation <-- Feedback <--+
```

### Signal Chain Integration

Module declares `"chainable": true` and `"component_type": "audio_fx"` in module.json.

Installs to: `/data/UserData/move-anything/modules/audio_fx/tapedelay/`

## Build Commands

```bash
./scripts/build.sh      # Build for ARM64 via Docker
./scripts/install.sh    # Deploy to Move
```
