#!/usr/bin/env bash
# build_in_bookworm_arm64.sh — the Raspberry Pi (arm64) release: COMPILE + PACKAGE.
#
# Three things differ from the x86_64 job, and all three follow from the target
# being a Pi:
#
#   1. debian:bookworm, because Raspberry Pi OS *is* Debian bookworm and an
#      AppImage never bundles glibc — its floor is the BUILD image's. Building on
#      the runner's own ubuntu-24.04-arm userland would stamp GLIBC_2.39 and the
#      package would not start on a single Pi. Bookworm's 2.36 covers Pi OS
#      bookworm + trixie and arm64 Ubuntu 24.04+.
#   2. -DPOM2_GLES=ON. Mesa's V3D caps *desktop* GL at 3.1 on Pi 4/5, so POM2's
#      default GL 3.2 core request fails outright. GLES 3.0 is what the hardware
#      actually exposes.
#   3. apt-get here rather than a pinned GHCR image. Unlike bionic, bookworm is a
#      current, supported Debian whose mirrors are healthy. Freeze it into an
#      image the day that stops being true — this script is already shaped for it.
#
# Honoured env: POM2_VERSION.
set -euxo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    ca-certificates git wget file desktop-file-utils \
    build-essential cmake pkg-config \
    libglfw3-dev \
    libgles2-mesa-dev libegl1-mesa-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev \
    libslirp-dev

# The bind-mounted repo is owned by the host uid, not root.
git config --global --add safe.directory '*'

# --- AppImage tooling (aarch64 builds) --------------------------------------
#     Extracted rather than run through FUSE, which CI containers do not have.
TOOLS=/opt/appimage-tools
mkdir -p "$TOOLS" && cd "$TOOLS"
download_verified() {
    url=$1 out=$2 expected=$3
    wget -q "$url" -O "$out"
    echo "$expected  $out" | sha256sum -c -
}
download_verified \
  "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-aarch64.AppImage" \
  appimagetool.AppImage \
  f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158
download_verified \
  "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-aarch64.AppImage" \
  linuxdeploy.AppImage \
  9f04c4c2a8b69c392c4bbcc1a88bdd4d0a8ac03f587cf5242814cb7ae47b78e5
# The ET_EXEC runtime, pinned to AppImageKit release **12**. This is not the
# `continuous` one: upstream never rebuilt the old-style runtime for ARM, so
#   continuous/runtime-x86_64  -> ET_EXEC
#   continuous/runtime-aarch64 -> ET_DYN   (static-pie; AppImageLauncher rejects
#                                           it as "type -1")
#   12/runtime-aarch64         -> ET_EXEC  (the last ET_EXEC ARM release)
# The type-2 format is unchanged — only the small bootstrap binary differs — so
# pinning 12 costs nothing and keeps the ET_EXEC contract release.yml verifies.
download_verified \
  "https://github.com/AppImage/AppImageKit/releases/download/12/runtime-aarch64" \
  runtime-aarch64 \
  207f8955500cfe8dd5b824ca7514787c023975e083b0269fc14600c380111d85
chmod +x runtime-aarch64
chmod +x ./*.AppImage
./linuxdeploy.AppImage  --appimage-extract >/dev/null && mv squashfs-root linuxdeploy.AppDir
./appimagetool.AppImage --appimage-extract >/dev/null && mv squashfs-root appimagetool.AppDir
rm -f ./*.AppImage
export POM2_APPIMAGE_TOOLS_DIR="$TOOLS"
cd /work

# --- Dear ImGui (pinned COMMIT from imgui_pin.env) --------------------------
tools/fetch_imgui_pinned.sh

# --- Build POM2 on the GLES tier --------------------------------------------
#     -static-libstdc++ / -static-libgcc so the only libc-family floor is
#     glibc 2.36 — bookworm's g++ would otherwise raise it via GLIBCXX symbols.
cmake -S . -B build-appimage \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DPOM2_ENABLE_TESTS=OFF \
    -DPOM2_GLES=ON \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
cmake --build build-appimage --parallel "${POM2_JOBS:-2}"

# --- Package ----------------------------------------------------------------
export POM2_APPIMAGE_SKIP_BUILD=1
export APPIMAGE_EXTRACT_AND_RUN=1
export ARCH=aarch64
export POM2_APPIMAGE_RUNTIME="$TOOLS/runtime-aarch64"
packaging/linux/build_appimage.sh build-appimage dist
