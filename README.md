# Move Everything TapeDelay

Tape delay audio effect module for Move Everything.

## Prerequisites

- [Move Everything](https://github.com/charlesvestal/move-anything) installed on your Ableton Move

## Installation

### Via Module Store (Recommended)

1. Launch Move Everything on your Move
2. Select **Module Store** from the main menu
3. Navigate to **Audio FX** → **TapeDelay**
4. Select **Install**

### Manual Installation

```bash
./scripts/install.sh
```

## Features

- **Time**: Delay time from 20ms to 2 seconds
- **Feedback**: Echo repeats (0-95%)
- **Mix**: Dry/wet blend
- **Tone**: Lowpass filter on repeats (500Hz to 12kHz)

## Building

```bash
./scripts/build.sh      # Build for ARM64 via Docker
./scripts/install.sh    # Deploy to Move
```

## Signal Flow

```
Input ---+-------------------------------- Dry ----+
         |                                         |
         +---> Delay Line ---> Tone Filter --> Wet-+---> Mix ---> Output
                   ^                               |
                   |                               |
                   +----------- Feedback <---------+
```

## Module ID

**Note:** This module's ID is `tapedelay`. When creating Signal Chain patches, reference it as:

```json
{ "type": "tapedelay", "params": { "time": 0.5, "feedback": 0.4 } }
```

## Installation Path

The module installs to `/data/UserData/move-anything/modules/audio_fx/tapedelay/`

## Credits

Based on [TapeDelay](https://github.com/cyrusasfa/TapeDelay) by Cyrus Afsary.

Inspired by classic tape echo units.

## License

MIT License - Copyright (c) 2025 Charles Vestal

## AI Assistance Disclaimer

This module is part of Move Everything and was developed with AI assistance, including Claude, Codex, and other AI assistants.

All architecture, implementation, and release decisions are reviewed by human maintainers.  
AI-assisted content may still contain errors, so please validate functionality, security, and license compatibility before production use.
