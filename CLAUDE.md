# CLAUDE.md

Instructions for Claude Code when working with this repository.

## Project Overview

TapeDelay is an audio effect module for Move Anything that provides tape delay with flutter, tone filtering, and soft saturation.

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
