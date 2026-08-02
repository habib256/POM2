#!/usr/bin/env bash
#
# package_macos_release.sh — build POM2 and produce dist/POM2-macOS-v<ver>.dmg
# containing a Universal 2 (arm64 + x86_64) .app.
#
# Layout inside the bundle:
#   POM2.app/Contents/MacOS/POM2          the universal binary
#   POM2.app/Contents/Resources/POM2.icns the icon
#   POM2.app/Contents/Resources/{fonts,pic,roms}
#   POM2.app/Contents/Info.plist          from packaging/macos/Info.plist.in
#
# Asset resolution needs no special case: pom2::resourceSearchDirs() probes
# <exe>/.. , which from Contents/MacOS resolves to Contents/ — so the standard
# Resources/ location is found by adding one search root. See the note below.
#
# The full roms/ tree (system + peripheral dumps + floppy_samples) is bundled
# under Contents/Resources/roms/ so the .app boots without a separate ROM drop.
#
# Env:
#   POM2_VERSION        version for the artifact name (default: from CMakeLists)
#   POM2_MACOS_ARCHS    CMAKE_OSX_ARCHITECTURES (default "arm64;x86_64")
#   CMAKE_PREFIX_PATH   where to find the universal static GLFW
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="build-macos"
DIST_DIR="dist"
ARCHS="${POM2_MACOS_ARCHS:-arm64;x86_64}"
VERSION="${POM2_VERSION:-$(sed -n 's/^project(pom2_imgui VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)}"
[ -n "$VERSION" ] || VERSION="0.0"

log() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }

[ -f imgui/imgui.cpp ] || ./setup_imgui.sh

log "Building POM2 v${VERSION} (${ARCHS})"
cmake -S . -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="$ARCHS" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 \
      -DPOM2_ENABLE_TESTS=OFF
cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)"

APP="${DIST_DIR}/POM2.app"
log "Staging ${APP}"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

cp "$BUILD_DIR/POM2" "$APP/Contents/MacOS/POM2"
cp packaging/macos/POM2.icns "$APP/Contents/Resources/POM2.icns"
sed "s/@POM2_VERSION@/${VERSION}/g" packaging/macos/Info.plist.in \
    > "$APP/Contents/Info.plist"

# Read-only assets. Mirrors the FHS install rules (including the full roms/).
mkdir -p "$APP/Contents/Resources/fonts" \
         "$APP/Contents/Resources/pic" \
         "$APP/Contents/Resources/roms"
cp fonts/DejaVuSans.ttf fonts/fa-solid-900.ttf "$APP/Contents/Resources/fonts/"
cp pic/Apple_II_plus.jpg                       "$APP/Contents/Resources/pic/"
if [ -d roms ]; then
    cp -R roms/. "$APP/Contents/Resources/roms/"
fi
cp packaging/roms_README.txt                   "$APP/Contents/Resources/roms/README.txt"

# ResourcePaths probes <exe>/.. (= Contents/) and <exe>/../share/POM2. Give the
# Apple layout a home by ALSO exposing Resources/ under the name the FHS probe
# expects — a symlink costs nothing and means the packaged binary resolves
# assets through the same code path as the Linux build, with no #ifdef __APPLE__
# in the resolver.
ln -sfn Resources "$APP/Contents/share"
ln -sfn . "$APP/Contents/Resources/POM2" 2>/dev/null || true

log "Verifying the bundle"
lipo -info "$APP/Contents/MacOS/POM2"
for a in ${ARCHS//;/ }; do
    lipo -info "$APP/Contents/MacOS/POM2" | grep -qw "$a" \
        || { echo "ERROR: POM2 is missing the $a slice"; exit 1; }
done
# No absolute Homebrew/MacPorts dylib may survive — that is the failure mode
# build_universal_deps.sh exists to prevent.
LEAKED=$(otool -L "$APP/Contents/MacOS/POM2" | tail -n +2 | awk '{print $1}' \
         | grep -E '^(/usr/local|/opt/homebrew|/opt/local)' || true)
if [ -n "$LEAKED" ]; then
    echo "ERROR: .app references dylibs outside the bundle:"; echo "$LEAKED"; exit 1
fi

# Ad-hoc signature. Not notarised (that needs an Apple Developer account), but
# an ad-hoc signature is REQUIRED on Apple Silicon: an unsigned arm64 binary is
# killed by the kernel outright rather than merely warned about by Gatekeeper.
codesign --force --deep --sign - "$APP" 2>/dev/null || \
    echo "NOTE: codesign unavailable — the .app will need 'xattr -dr com.apple.quarantine'"

log "Building the DMG"
DMG="${DIST_DIR}/POM2-macOS-v${VERSION}.dmg"
rm -f "$DMG"
STAGE="${BUILD_DIR}/dmg-stage"
rm -rf "$STAGE"; mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
cp README.md "$STAGE/README.md"
hdiutil create -volname "POM2 ${VERSION}" -srcfolder "$STAGE" \
        -ov -format UDZO "$DMG"

log "Wrote ${DMG}"
ls -lh "$DMG"
