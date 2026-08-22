#!/usr/bin/env bash
#
# Install the Tape Echo 2 module on the Move SAFELY.
#
# Critical: never scp directly over a live dsp.so. The shim dlopen()s it, so
# overwriting the file mutates the mmap'd code pages of a running process —
# which segfaults the whole firmware. Upload to a temp name, then mv:
# rename(2) is atomic and leaves the old inode intact for the running
# process. New code is picked up when the slot next loads the module.
#
#   ./scripts/deploy.sh [host]      (default: move.local)
set -euo pipefail

HOST="${1:-move.local}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
DEST="/data/UserData/schwung/modules/audio_fx/tapedelay"

SO="tapedelay.so"   # audio_fx: the chain host dlopens <id>/<id>.so, never dsp.so
[ -f "$SRC/build/$SO" ] || { echo "no build/$SO — run ./scripts/build.sh first" >&2; exit 1; }

echo "==> $HOST:$DEST"
ssh "$HOST" "mkdir -p $DEST"

scp -q "$SRC/build/$SO"             "$HOST:$DEST/$SO.new"
scp -q "$SRC/src/module.json"       "$HOST:$DEST/module.json.new"
scp -q "$SRC/src/movy_config.json"  "$HOST:$DEST/movy_config.json.new"
scp -q "$SRC/src/ui_chain.js"       "$HOST:$DEST/ui_chain.js.new"
scp -q "$SRC/src/help.json"         "$HOST:$DEST/help.json.new"
scp -q "$SRC/src/web_ui.html"       "$HOST:$DEST/web_ui.html.new"

# Atomic swap. Do NOT replace this with a direct scp.
ssh "$HOST" "cd $DEST && \
    mv -f $SO.new $SO && \
    mv -f module.json.new module.json && \
    mv -f movy_config.json.new movy_config.json && \
    mv -f ui_chain.js.new ui_chain.js && \
    mv -f help.json.new help.json && \
    mv -f web_ui.html.new web_ui.html && \
    chmod 755 $SO && rm -f dsp.so && ls -l $SO module.json movy_config.json ui_chain.js"

# Loader test binary (run it on the device: cd $DEST && ./te2_loadtest ./$SO)
if [ -f "$SRC/build/te2_loadtest" ]; then
    scp -q "$SRC/build/te2_loadtest" "$HOST:$DEST/te2_loadtest.new"
    ssh "$HOST" "cd $DEST && mv -f te2_loadtest.new te2_loadtest && chmod 755 te2_loadtest"
fi

echo "==> done. Reload the FX slot (or restart the Shadow UI) to pick up new code."
