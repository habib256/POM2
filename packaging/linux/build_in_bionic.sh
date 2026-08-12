#!/usr/bin/env bash
# build_in_bionic.sh — the Linux x86_64 release: COMPILE + PACKAGE only.
#
# The glibc-2.27 toolchain (g++-9, CMake 3.27, GLFW 3.3, appimagetool,
# linuxdeploy) is BAKED into the pinned builder image, packaging/linux/
# Dockerfile.bionic. This script runs INSIDE that image with the repo
# bind-mounted at /work; it touches no apt mirror and downloads no toolchain, so
# a release does not depend on bionic's flaky EOL infrastructure. The build is
# reproducible by image digest.
#
# WHY `docker run` AND NOT a `container:` KEY in the workflow: GitHub's node24
# actions (checkout@v5, upload-artifact@v5) need glibc >= 2.28 — one notch above
# bionic's 2.27 — so they cannot execute inside a bionic `container:`. The
# actions run on the host; only this script runs in the container.
#
# Honoured env: POM2_VERSION.
# Provided by the image ENV: PATH (CMake), POM2_APPIMAGE_TOOLS_DIR, gcc/g++ -> 9.
set -euxo pipefail

# The bind-mounted repo is owned by the host uid, not root — let git touch it.
git config --global --add safe.directory '*'

# --- Dear ImGui (pinned COMMIT from imgui_pin.env) --------------------------
#     Not baked into the image, so the image stays decoupled from the ImGui
#     version; a pinned github.com commit is far more reliable than the bionic
#     apt mirrors this whole arrangement exists to avoid. Uses the dedicated
#     fetch helper rather than setup_imgui.sh, which would `sudo apt install`
#     system dependencies — exactly what the baked toolchain is here to avoid.
tools/fetch_imgui_pinned.sh

# --- Build POM2 --------------------------------------------------------------
#     -static-libstdc++ / -static-libgcc so the ONLY libc-family floor the
#     AppImage imposes is glibc 2.27. Without them, g++-9's newer GLIBCXX and
#     libgcc symbols would raise the effective floor above what the older
#     distros this image exists to support actually ship.
#     Tests are off: the CI workflow already gates the suite on every push, and
#     bionic's toolchain is not what we want validating emulator behaviour.
cmake -S . -B build-appimage \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DPOM2_ENABLE_TESTS=OFF \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
cmake --build build-appimage --parallel "${POM2_JOBS:-2}"

# --- Package the AppImage ----------------------------------------------------
#     SKIP_BUILD: the binary is already built above with the exact linker flags
#     that set the glibc floor — letting build_appimage.sh reconfigure would
#     drop them. APPIMAGE_EXTRACT_AND_RUN: the container has no FUSE.
export POM2_APPIMAGE_SKIP_BUILD=1
export APPIMAGE_EXTRACT_AND_RUN=1
packaging/linux/build_appimage.sh build-appimage dist
