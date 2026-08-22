#!/usr/bin/env bash
# Kept for the ecosystem convention (`./scripts/install.sh`). The real work is
# in deploy.sh, which uploads to a temp name and rename(2)s into place —
# scp'ing straight over a live dsp.so mutates the mmap'd code pages of a
# running process and takes the firmware down with it. The version this
# replaced did exactly that, and also still pointed at the pre-Schwung
# /data/UserData/move-anything path.
exec "$(cd "$(dirname "$0")" && pwd)/deploy.sh" "$@"
