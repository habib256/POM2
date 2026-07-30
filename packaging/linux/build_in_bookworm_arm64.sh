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
wget -q "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-aarch64.AppImage" -O appimagetool.AppImage
wget -q "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-aarch64.AppImage" -O linuxdeploy.AppImage
# The ET_EXEC runtime. appimagetool-aarch64 embeds a static-pie ET_DYN one by
# default, and AppImageLauncher refuses those ("type -1"), so hand it this
# instead via --runtime-file. x86_64's appimagetool already defaults to ET_EXEC.
wget -q "https://github.com/AppImage/AppImageKit/releases/download/continuous/runtime-aarch64" -O runtime-aarch64
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
cmake --build build-appimage -j"$(nproc)"

# --- Package ----------------------------------------------------------------
export POM2_APPIMAGE_SKIP_BUILD=1
export APPIMAGE_EXTRACT_AND_RUN=1
export ARCH=aarch64
export POM2_APPIMAGE_RUNTIME="$TOOLS/runtime-aarch64"
packaging/linux/build_appimage.sh build-appimage dist
