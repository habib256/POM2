#!/usr/bin/env bash
#
# build_dist.sh — produce POM2 release artifacts into DIST/.
#
# Today: Linux only (Windows / macOS / WASM are tracked separately on the
# release-infra branch). Run from the repo root:
#
#     ./build_dist.sh              # build + package Linux into DIST/
#     ./build_dist.sh --tests      # also run the ctest suite first
#     ./build_dist.sh --clean      # wipe build-dist/ before configuring
#
# Outputs (all under DIST/):
#   POM2-v<ver>-linux-<arch>.tar.gz   relocatable bundle (bin/ + share/POM2)
#   POM2-v<ver>-linux-<arch>.deb      Debian/Ubuntu package (if cpack DEB ok)
#   POM2-v<ver>-x86_64.AppImage       single-file portable (if tooling present)
#
# The Apple ROMs are always bundled ; each artifact ships a
# roms/README.txt telling the user where to drop their own dumps.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="build-dist"
DIST_DIR="DIST"
RUN_TESTS=0
DO_CLEAN=0
for arg in "$@"; do
    case "$arg" in
        --tests) RUN_TESTS=1 ;;
        --clean) DO_CLEAN=1 ;;
        -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; exit 2 ;;
    esac
done

log() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }

# ─── Preconditions ──────────────────────────────────────────────────────────
if [ ! -f imgui/imgui.cpp ]; then
    echo "Dear ImGui not found — run ./setup_imgui.sh first." >&2
    exit 1
fi
command -v cmake >/dev/null || { echo "cmake not found." >&2; exit 1; }

VERSION="$(sed -nE 's/^project\(pom2_imgui VERSION ([0-9.]+).*/\1/p' CMakeLists.txt)"
ARCH="$(uname -m)"
log "POM2 v${VERSION} — Linux ${ARCH} release build"

[ "$DO_CLEAN" = 1 ] && { log "Cleaning ${BUILD_DIR}/"; rm -rf "$BUILD_DIR"; }
mkdir -p "$DIST_DIR"

# ─── Configure + build ──────────────────────────────────────────────────────
log "Configuring (Release, /usr prefix)"
cmake -S . -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr \
      -DPOM2_ENABLE_TESTS="$([ "$RUN_TESTS" = 1 ] && echo ON || echo OFF)" \
      >/dev/null

log "Building POM2"
cmake --build "$BUILD_DIR" --target pom2_imgui -j"$(nproc)"

if [ "$RUN_TESTS" = 1 ]; then
    log "Running ctest"
    cmake --build "$BUILD_DIR" -j"$(nproc)" >/dev/null
    ( cd "$BUILD_DIR" && ctest --output-on-failure -j"$(nproc)" )
fi

# ─── 1. Relocatable tarball (manual staging for a clean, prefix-free tree) ──
STAGE="${BUILD_DIR}/stage/POM2-v${VERSION}-linux-${ARCH}"
log "Staging relocatable bundle → ${STAGE}"
rm -rf "${BUILD_DIR}/stage"
mkdir -p "$STAGE"
DESTDIR="$(pwd)/${STAGE}" cmake --install "$BUILD_DIR" --prefix /usr >/dev/null
# Flatten /usr → bundle root so the layout is <root>/bin + <root>/share.
mv "${STAGE}/usr/"* "$STAGE/"
rmdir "${STAGE}/usr"
# Convenience launcher at the bundle root (the binary also works directly:
# ResourcePaths finds share/POM2 relative to bin/POM2).
cat > "${STAGE}/POM2" <<'LAUNCH'
#!/usr/bin/env bash
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/bin/POM2" "$@"
LAUNCH
chmod +x "${STAGE}/POM2"
# Top-level quickstart so a user who extracts the tarball isn't lost.
cat > "${STAGE}/README.txt" <<EOF
POM2 v${VERSION} — Apple II emulator (Linux ${ARCH})

Run:        ./POM2          (or bin/POM2 directly)
ROMs:       drop your Apple ROM dumps into share/POM2/roms/
            (see share/POM2/roms/README.txt — they are NOT bundled)
Needs:      libglfw3 and an OpenGL driver installed on the system.
Docs:       share/doc/POM2/README.md
EOF

TARBALL="${DIST_DIR}/POM2-v${VERSION}-linux-${ARCH}.tar.gz"
log "Packing ${TARBALL}"
tar -C "${BUILD_DIR}/stage" -czf "$TARBALL" "POM2-v${VERSION}-linux-${ARCH}"

# ─── 2. Debian package (best-effort; needs dpkg tooling) ────────────────────
if command -v dpkg-deb >/dev/null; then
    log "Building .deb via CPack"
    if ( cd "$BUILD_DIR" && cpack -G DEB >/dev/null 2>cpack-deb.log ); then
        find "$BUILD_DIR" -maxdepth 1 -name '*.deb' -exec cp {} "$DIST_DIR/" \;
    else
        echo "  (.deb build failed — see ${BUILD_DIR}/cpack-deb.log; skipping)" >&2
    fi
else
    log "dpkg-deb not found — skipping .deb"
fi

# ─── 3. AppImage (best-effort; needs linuxdeploy on PATH) ───────────────────
if command -v linuxdeploy >/dev/null || command -v linuxdeploy-x86_64.AppImage >/dev/null; then
    LD="$(command -v linuxdeploy || command -v linuxdeploy-x86_64.AppImage)"
    log "Building AppImage via ${LD##*/}"
    APPDIR="${BUILD_DIR}/AppDir"
    rm -rf "$APPDIR"
    DESTDIR="$(pwd)/${APPDIR}" cmake --install "$BUILD_DIR" --prefix /usr >/dev/null
    if OUTPUT="${DIST_DIR}/POM2-v${VERSION}-${ARCH}.AppImage" \
       "$LD" --appdir "$APPDIR" \
             --desktop-file "$APPDIR/usr/share/applications/POM2.desktop" \
             --output appimage 2>"${BUILD_DIR}/linuxdeploy.log"; then
        # linuxdeploy writes into CWD; move whatever AppImage it produced.
        find . -maxdepth 1 -name 'POM2*.AppImage' -newer "$TARBALL" \
            -exec mv {} "$DIST_DIR/" \; 2>/dev/null || true
    else
        echo "  (AppImage build failed — see ${BUILD_DIR}/linuxdeploy.log)" >&2
    fi
else
    log "linuxdeploy not found — skipping AppImage"
    echo "    (install from https://github.com/linuxdeploy/linuxdeploy to enable)"
fi

# ─── Summary ────────────────────────────────────────────────────────────────
log "Done. Artifacts in ${DIST_DIR}/:"
ls -lh "$DIST_DIR" | tail -n +2 | awk '{printf "    %s  %s\n", $5, $9}'
