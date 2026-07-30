#!/usr/bin/env bash
#
# build_universal_deps.sh — build GLFW as a UNIVERSAL 2 (arm64 + x86_64) STATIC
# library for the macOS release.
#
# Why not Homebrew: this is the exact trap POM1 fell into twice, and it is worth
# not repeating.
#   1. Homebrew is single-architecture, so a brew-linked build ships an
#      x86_64-only app — half the userbase runs it under Rosetta at best.
#   2. Worse, the linker bakes brew's ABSOLUTE prefix into the binary, so the
#      shipped app looks for /usr/local/opt/glfw/lib/libglfw.3.dylib — a path
#      that does not exist on Apple Silicon (brew lives at /opt/homebrew there).
#      Every such build dies in dyld with "Library not loaded", which Finder
#      reports as the useless "POM2 quit unexpectedly". CI cannot catch it
#      either, because the runner obviously HAS brew glfw at that path.
#
# Building GLFW statically removes the failure mode entirely: there is no dylib
# left to resolve at runtime, so the .app depends on nothing outside macOS's own
# system frameworks.
#
# Usage: packaging/macos/build_universal_deps.sh [--out <dir>]
set -euo pipefail

OUT="build-universal-deps"
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

GLFW_VER="${GLFW_VER:-3.3.10}"
GLFW_SHA256="${GLFW_SHA256:-4ff18a3377da465386374d8127e7b7349b685288cb8e17122f7e1179f73769d5}"

mkdir -p "$OUT"
cd "$OUT"
OUT_ABS="$PWD"

echo "==> Fetching GLFW ${GLFW_VER}"
curl -fsSL -o glfw.tar.gz \
     "https://github.com/glfw/glfw/archive/refs/tags/${GLFW_VER}.tar.gz"
echo "${GLFW_SHA256}  glfw.tar.gz" | shasum -a 256 -c -
rm -rf "glfw-${GLFW_VER}"
tar -xzf glfw.tar.gz

echo "==> Building universal static GLFW"
# CMAKE_OSX_ARCHITECTURES with both slices is all the Apple toolchain needs to
# emit a fat static library — no lipo pass required.
cmake -S "glfw-${GLFW_VER}" -B glfw-build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 \
      -DBUILD_SHARED_LIBS=OFF \
      -DGLFW_BUILD_EXAMPLES=OFF -DGLFW_BUILD_TESTS=OFF -DGLFW_BUILD_DOCS=OFF \
      -DCMAKE_INSTALL_PREFIX="${OUT_ABS}/glfw"
cmake --build glfw-build -j"$(sysctl -n hw.ncpu)" --target install

echo "==> Verifying both slices are present"
LIB="${OUT_ABS}/glfw/lib/libglfw3.a"
lipo -info "$LIB"
for a in arm64 x86_64; do
    lipo -info "$LIB" | grep -qw "$a" \
        || { echo "ERROR: libglfw3.a is missing the $a slice"; exit 1; }
done
echo "OK: universal static GLFW at ${OUT_ABS}/glfw"
