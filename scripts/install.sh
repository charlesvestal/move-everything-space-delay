#!/bin/bash
# Install TapeDelay module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/tapedelay" ]; then
    echo "Error: dist/tapedelay not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing TapeDelay Module ==="

# Deploy to Move - audio_fx subdirectory
echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/move-anything/modules/audio_fx/tapedelay"
scp -r dist/tapedelay/* ableton@move.local:/data/UserData/move-anything/modules/audio_fx/tapedelay/

# Install chain presets if they exist
if [ -d "src/chain_patches" ]; then
    echo "Installing chain presets..."
    ssh ableton@move.local "mkdir -p /data/UserData/move-anything/patches"
    scp src/chain_patches/*.json ableton@move.local:/data/UserData/move-anything/patches/
fi

# Set permissions so Module Store can update later
echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/move-anything/modules/audio_fx/tapedelay"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/move-anything/modules/audio_fx/tapedelay/"
echo "Presets installed to: /data/UserData/move-anything/patches/"
echo ""
echo "Restart Move Anything to load the new module."
