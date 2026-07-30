#!/usr/bin/env bash
#
# build_appimage.sh — stage an AppDir from POM2's own CMake install rules and
# turn it into a single-file AppImage.
#
# The AppDir layout is not hand-assembled: `cmake --install` with
# CMAKE_INSTALL_PREFIX=<AppDir>/usr already produces exactly what an AppImage
# wants (usr/bin/POM2 + usr/share/POM2/{fonts,pic,roms} + the .desktop and icons
# under usr/share/{applications,icons}), because POM2's install() rules target
# the FHS layout that `pom2::resourceSearchDirs()` probes as <exe>/../share/POM2.
# That means the packaged binary resolves its assets through the SAME code path
# as an apt-installed one — no packaging-only special case to rot.
#
# Two tools, both optional-with-fallback:
#   linuxdeploy   — copies the non-system shared libs POM2 needs into usr/lib
#                   and rewrites rpaths.
#   appimagetool  — squashes the AppDir into the final .AppImage.
# Inside the release container both are pre-extracted (no FUSE on CI runners);
# POM2_APPIMAGE_TOOLS_DIR points at them. Outside, we look on PATH.
#
# Env:
#   POM2_VERSION             version string for the artifact name (default: from CMake)
#   POM2_APPIMAGE_TOOLS_DIR  dir holding linuxdeploy.AppDir/ and appimagetool.AppDir/
#   POM2_APPIMAGE_SKIP_BUILD build/ is already configured+built; only package
#   POM2_CMAKE_EXTRA_ARGS    extra configure args (e.g. -DPOM2_GLES=ON for the Pi)
#   ARCH                     target arch for appimagetool (default: uname -m)
#
# Usage: packaging/linux/build_appimage.sh [<build-dir>] [<out-dir>]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="${1:-build-appimage}"
OUT_DIR="${2:-dist}"
APPDIR="${BUILD_DIR}/AppDir"
ARCH="${ARCH:-$(uname -m)}"

log() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }

# ─── Dear ImGui (pinned, same source of truth as CI) ────────────────────────
if [ ! -f imgui/imgui.cpp ]; then
    log "Fetching Dear ImGui (pinned via imgui_pin.env)"
    ./setup_imgui.sh
fi

# ─── Configure + build ──────────────────────────────────────────────────────
if [ -z "${POM2_APPIMAGE_SKIP_BUILD:-}" ]; then
    log "Configuring (Release) in ${BUILD_DIR}"
    cmake -S . -B "$BUILD_DIR" \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/usr \
          -DPOM2_ENABLE_TESTS=OFF \
          ${POM2_CMAKE_EXTRA_ARGS:-}

    log "Building"
    cmake --build "$BUILD_DIR" -j"$(nproc)"
else
    log "POM2_APPIMAGE_SKIP_BUILD set — packaging the existing ${BUILD_DIR}"
fi

VERSION="${POM2_VERSION:-$(sed -n 's/^project(pom2_imgui VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)}"
[ -n "$VERSION" ] || VERSION="0.0"
log "Packaging POM2 v${VERSION} (${ARCH})"

# ─── Stage the AppDir via the real install rules ────────────────────────────
rm -rf "$APPDIR"
DESTDIR="$PWD/$APPDIR" cmake --install "$BUILD_DIR" >/dev/null

# AppImage requires the desktop file + icon at the AppDir ROOT as well as in
# usr/share; appimagetool reads the root copies for the .desktop metadata.
cp packaging/POM2.desktop "$APPDIR/POM2.desktop"
cp packaging/POM2.svg     "$APPDIR/POM2.svg"
# A .DirIcon is what file managers show for the AppImage itself.
cp packaging/POM2-256.png "$APPDIR/.DirIcon"

install -m 0755 packaging/linux/AppRun "$APPDIR/AppRun"

# ─── Bundle non-system libraries ────────────────────────────────────────────
TOOLS="${POM2_APPIMAGE_TOOLS_DIR:-}"
run_tool() {   # run_tool <name> [args...] — prefers the extracted AppDir copy
    local name="$1"; shift
    if [ -n "$TOOLS" ] && [ -x "${TOOLS}/${name}.AppDir/AppRun" ]; then
        "${TOOLS}/${name}.AppDir/AppRun" "$@"
    elif command -v "$name" >/dev/null; then
        "$name" "$@"
    elif command -v "${name}-${ARCH}.AppImage" >/dev/null; then
        "${name}-${ARCH}.AppImage" "$@"
    else
        return 127
    fi
}

if run_tool linuxdeploy --appdir "$APPDIR" \
        --executable "$APPDIR/usr/bin/POM2" \
        --desktop-file "$APPDIR/POM2.desktop" \
        --icon-file "$APPDIR/POM2.svg" 2>"${BUILD_DIR}/linuxdeploy.log"; then
    log "linuxdeploy staged the dependency libraries"
else
    echo "  (linuxdeploy unavailable or failed — see ${BUILD_DIR}/linuxdeploy.log)" >&2
    echo "  The AppDir is still usable on a host with the same shared libs." >&2
fi

# linuxdeploy re-generates AppRun; put ours back (it owns the ROM-dir seeding).
install -m 0755 packaging/linux/AppRun "$APPDIR/AppRun"

# Graphics-driver libraries must come from the HOST, never the bundle: shipping
# our libGL/libEGL would override the user's GPU driver and silently drop them
# to software rendering (or fail outright on a mismatched driver ABI).
for drv in libGL.so.1 libEGL.so.1 libGLX.so.0 libOpenGL.so.0 libGLdispatch.so.0 \
           libgbm.so.1 libdrm.so.2; do
    rm -f "$APPDIR/usr/lib/$drv"
done

# ─── Squash into the final AppImage ─────────────────────────────────────────
mkdir -p "$OUT_DIR"
OUTFILE="${OUT_DIR}/POM2-v${VERSION}-${ARCH}.AppImage"
rm -f "$OUTFILE"

# ARCH is what appimagetool stamps into the runtime; it refuses to guess.
if ARCH="$ARCH" run_tool appimagetool "$APPDIR" "$OUTFILE"; then
    log "Wrote ${OUTFILE}"
else
    echo "ERROR: appimagetool not available — AppDir left at ${APPDIR}" >&2
    exit 1
fi

ls -lh "$OUTFILE"
