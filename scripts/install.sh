#!/bin/bash
# Install Space Echo module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/spacecho" ]; then
    echo "Error: dist/spacecho not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing Space Echo Module ==="

# Deploy to Move (audio_fx path)
echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/move-anything/modules/chain/audio_fx/spacecho"
scp -r dist/spacecho/* ableton@move.local:/data/UserData/move-anything/modules/chain/audio_fx/spacecho/

# Install chain presets if they exist
if [ -d "src/chain_patches" ]; then
    echo "Installing chain presets..."
    ssh ableton@move.local "mkdir -p /data/UserData/move-anything/modules/chain/patches"
    scp src/chain_patches/*.json ableton@move.local:/data/UserData/move-anything/modules/chain/patches/
fi

# Set permissions so Module Store can update later
echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/move-anything/modules/chain/audio_fx/spacecho"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/move-anything/modules/chain/audio_fx/spacecho/"
echo "Chain presets installed to: /data/UserData/move-anything/modules/chain/patches/"
echo ""
echo "Restart Move Anything to load the new module."
