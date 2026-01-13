# Move Anything Space Delay

RE-201 style tape delay audio effect module for Move Anything.

## Prerequisites

- [Move Anything](https://github.com/charlesvestal/move-anything) installed on your Ableton Move

## Installation

### Via Module Store (Recommended)

1. Launch Move Anything on your Move
2. Select **Module Store** from the main menu
3. Navigate to **Audio FX** → **Space Echo**
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
- **Saturation**: Soft tape-style saturation on feedback

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

## Module ID

**Note:** This module's ID is `spacecho` (not `space-delay`). When creating Signal Chain patches, reference it as:

```json
{ "type": "spacecho", "params": { "time": 0.5, "feedback": 0.4 } }
```

## Installation Path

The module installs to `/data/UserData/move-anything/modules/chain/audio_fx/spacecho/`

## Credits

Based on [TapeDelay](https://github.com/cyrusasfa/TapeDelay) by Cyrus Afsary.

Inspired by the [Roland RE-201 Space Echo](https://www.roland.com/global/promos/space_echo_history/) tape delay unit (1974).

## License

MIT License - Copyright (c) 2025 Charles Vestal
