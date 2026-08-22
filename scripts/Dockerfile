# Build environment for the Schwung Tablor module.
# ubuntu:22.04 pinned deliberately: glibc 2.35, matching Ableton Move exactly.
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-aarch64-linux-gnu g++-aarch64-linux-gnu build-essential \
        cmake ninja-build make file git ca-certificates pkg-config python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
ENV CROSS_PREFIX=aarch64-linux-gnu-
