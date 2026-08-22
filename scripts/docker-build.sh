#!/usr/bin/env bash
# Runs INSIDE the ubuntu:22.04 build container. Do not run on a host with a
# newer glibc — the artifacts would not load on the Move (glibc 2.35).
set -euo pipefail
TARGET="${1:-all}"

# ---- Config contract: movy_config / chain_params / C table / chain UI must
# all agree, and enum options must stay distinct in movy's 5-char readout.
echo "=== config contract ==="
python3 tools/check_config.py

# ---- The modified core must be bit-identical to upstream with Ping Pong off.
# Fetch the pristine core and render the same program through both.
echo "=== core equivalence vs pristine upstream ==="
# What is under test is OUR EDIT to the core, so pristine and ours must be
# compiled against the same everything else: the vendored shared DSP headers
# and the vendored params header. Upstream's shared-* dir has drifted since
# this core was vendored, and building pristine against today's version
# reports a difference that is upstream's evolution, not our ping-pong stage.
#
# TapeEchoDSP.hpp reaches its params header by a path relative to its own
# directory ("../daf-plugin/TapeEchoParams.hpp"), so -I cannot redirect it —
# stage the upstream core in a tree that puts our vendored copy there. Upstream
# has renamed that dir once already (dpf -> daf), so satisfy both spellings.
rm -rf build-native/dusk build-native/pristine
if git clone -q --depth 1 https://github.com/dusk-audio/dusk-audio-plugins \
        build-native/dusk 2>/dev/null; then
    UP=build-native/dusk/plugins/tape-echo/core
    mkdir -p build-native/pristine/core build-native/pristine/daf-plugin \
             build-native/pristine/dpf-plugin
    cp "$UP/TapeEchoDSP.hpp" "$UP/TapeEchoDSP.cpp" build-native/pristine/core/
    cp src/ported/dpf-plugin/TapeEchoParams.hpp build-native/pristine/daf-plugin/
    cp src/ported/dpf-plugin/TapeEchoParams.hpp build-native/pristine/dpf-plugin/

    # -ffp-contract=off is REQUIRED, on both sides. The claim under test is
    # about the arithmetic, and g++ defaults to -ffp-contract=fast for C++:
    # it fuses a*b+c into an FMA wherever it likes, and merely having the
    # ping-pong branch nearby changes which multiply-adds get fused. That is
    # a last-bit difference per sample, but it feeds a feedback loop, so 400
    # blocks later the checksums are apart in the 5th digit and the gate
    # reports a divergence that does not exist in the source. Contraction off,
    # the two are identical to every digit.
    EQ_FLAGS="-O2 -std=c++17 -ffp-contract=off"
    # shellcheck disable=SC2086
    g++ $EQ_FLAGS tests/core_equivalence.cpp build-native/pristine/core/TapeEchoDSP.cpp \
        -Ibuild-native/pristine/core -Isrc/ported/shared-dpf/dsp -o build-native/eq_pristine
    # shellcheck disable=SC2086
    g++ $EQ_FLAGS tests/core_equivalence.cpp src/ported/core/TapeEchoDSP.cpp \
        -Isrc/ported/core -Isrc/ported/shared-dpf/dsp -Isrc/ported/dpf-plugin \
        -o build-native/eq_ours
    A=$(./build-native/eq_pristine); B=$(./build-native/eq_ours)
    echo "  pristine=$A  ours=$B"
    [ "$A" = "$B" ] || { echo "FAIL: the core diverges from upstream with Ping Pong off"; exit 1; }
    echo "  ok: identical with Ping Pong off"
else
    echo "  (skipped: upstream not reachable)"
fi

# ---- Native DSP tests: compile and RUN in-container before cross-compiling.
# A red test here fails the whole build.
echo "=== native DSP tests ==="
mkdir -p build-native
g++ -O2 -std=c++17 -Wall \
    tests/test_dsp.cpp src/ported/core/TapeEchoDSP.cpp \
    -Isrc -Isrc/host -Isrc/ported/core -Isrc/ported/shared-dpf/dsp \
    -o build-native/te2_test -lm -lpthread
./build-native/te2_test

# Native loadtest against a natively built tapedelay.so: full API-surface check.
echo "=== native loadtest ==="
g++ -O2 -std=c++17 -Wall -shared -fPIC \
    src/dsp/tape_echo_plugin.cpp src/ported/core/TapeEchoDSP.cpp \
    -Isrc -Isrc/host -Isrc/ported/core -Isrc/ported/shared-dpf/dsp \
    -o build-native/tapedelay.so -lm -lpthread
g++ -O2 -std=c++17 -Wall tools/loadtest.cpp \
    -Isrc -Isrc/host \
    -o build-native/te2_loadtest -ldl -lm
./build-native/te2_loadtest build-native/tapedelay.so src/module.json

cmake -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build --target "$TARGET" -j"$(nproc)"

# ---- Package for the Module Store ----
rm -rf dist/tapedelay
mkdir -p dist/tapedelay
cp build/tapedelay.so    dist/tapedelay/
cp src/module.json       dist/tapedelay/
cp src/movy_config.json  dist/tapedelay/
cp src/ui_chain.js       dist/tapedelay/
cp src/help.json         dist/tapedelay/
cp src/web_ui.html       dist/tapedelay/
cp LICENSE               dist/tapedelay/
(cd dist && tar -czf tapedelay-module.tar.gz tapedelay/)
echo "Tarball: dist/tapedelay-module.tar.gz"

echo; echo "=== Build output ==="
find build -maxdepth 1 -type f \( -name "*.so" -o -name "te2_*" \) \
    -exec sh -c 'printf "%s\n  " "$1"; file -b "$1"' _ {} \;
