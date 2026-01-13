# Move Anything Space Delay

RE-201 style tape delay audio effect module for Move Anything.

## Features

- **Time**: Delay time from 50ms to 800ms
- **Feedback**: Echo repeats with gentle saturation
- **Mix**: Dry/wet blend
- **Tone**: High-frequency rolloff on repeats (tape simulation)
- **Flutter**: Tape wow/flutter pitch modulation

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
                   ^               |               |
                   |               v               |
                   +--- Saturation <-- Feedback <--+
```

## Installation

The module installs to `/data/UserData/move-anything/modules/chain/audio_fx/spacecho/`

## License

MIT License - Copyright (c) 2025 Charles Vestal
