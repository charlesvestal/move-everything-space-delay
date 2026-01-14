#!/usr/bin/env bash
# Build Space Echo module for Move Anything (ARM64)
#
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Space Echo Module Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building Space Echo Module ==="
echo "Cross prefix: $CROSS_PREFIX"

# Create build directories
mkdir -p build
mkdir -p dist/spacecho

# Compile DSP plugin (with aggressive optimizations for CM4)
echo "Compiling DSP plugin..."
${CROSS_PREFIX}gcc -Ofast -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer -fno-stack-protector \
    -DNDEBUG \
    src/dsp/spacecho.c \
    -o build/spacecho.so \
    -Isrc/dsp \
    -lm

# Copy files to dist (use cat to avoid ExtFS deallocation issues with Docker)
echo "Packaging..."
cat src/module.json > dist/spacecho/module.json
cat build/spacecho.so > dist/spacecho/spacecho.so
chmod +x dist/spacecho/spacecho.so

# Create tarball for release
cd dist
tar -czvf spacecho-module.tar.gz spacecho/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/spacecho/"
echo "Tarball: dist/spacecho-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
