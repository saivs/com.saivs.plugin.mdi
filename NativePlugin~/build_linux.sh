#!/usr/bin/env bash
# Builds libGfxPluginMDI.so for Linux x86_64.
# Run from a Linux machine or container: ./build_linux.sh
# Extra args are forwarded to the CMake configure step (e.g. -DMDI_DEBUG_LOG=ON).
#
# The CMake post-build step copies the result to ../Plugins/x86_64/libGfxPluginMDI.so.
# Build against the oldest glibc you intend to ship for (e.g. an ubuntu:20.04
# container) — the .so links whatever glibc it was compiled on.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build_linux"

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$BUILD" -j"$(nproc)"

OUT="$ROOT/../Plugins/x86_64/libGfxPluginMDI.so"
echo "Built: $OUT"
file "$OUT" 2>/dev/null || true
