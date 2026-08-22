#!/usr/bin/env bash
# Build the TapeDelay module (Tape Echo 2 engine) for Ableton Move (aarch64).
#
# Cross-compiles inside Docker on ubuntu:22.04 — glibc 2.35, matching the Move
# exactly. Set CROSS_PREFIX (or run inside the container) to skip Docker.
#
#   ./scripts/build.sh [cmake-target]        (default: all)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="schwung-tapedelay-builder"
TARGET="${1:-all}"

if [ -z "${CROSS_PREFIX:-}" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== TapeDelay Module Build (via Docker) ==="

    if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$SCRIPT_DIR"
    fi

    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" ./scripts/docker-build.sh "$TARGET"

    echo
    echo "=== Artifacts ==="
    ls -la "$REPO_ROOT/build/" | command grep -E "\.so|loadtest|te2_" || true
    ls -la "$REPO_ROOT/dist/" 2>/dev/null || true
    exit 0
fi

exec "$SCRIPT_DIR/docker-build.sh" "$TARGET"
