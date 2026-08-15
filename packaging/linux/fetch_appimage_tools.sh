#!/usr/bin/env bash
# fetch_appimage_tools.sh — download, verify and EXTRACT the AppImage tooling
# an aarch64 packaging job needs (linuxdeploy, appimagetool, and the ET_EXEC
# runtime), then leave them where build_appimage.sh looks.
#
# Extracted rather than run through FUSE: CI containers have no /dev/fuse.
#
# Split out of build_in_bookworm_arm64.sh when a SECOND aarch64 job appeared
# (the generic ARM64 AppImage alongside the Raspberry Pi one). Two copies of a
# list of pinned SHA-256 digests is two copies to update on the day upstream
# rotates a release — and the failure mode of a stale copy is a package whose
# runtime AppImageLauncher rejects, which nothing but the release job notices.
#
# x86_64 does NOT come through here: its tools are baked into the pinned bionic
# builder image (POM2_APPIMAGE_TOOLS_DIR=/opt/appimage-tools), because that job
# runs on a glibc too old to fetch them.
#
# Usage:  packaging/linux/fetch_appimage_tools.sh [<dest-dir>]   (default /opt/appimage-tools)
# Prints: the directory, so a caller can `export POM2_APPIMAGE_TOOLS_DIR=$(...)`.
set -euo pipefail

DEST="${1:-/opt/appimage-tools}"
ARCH="${ARCH:-$(uname -m)}"

if [ "$ARCH" != "aarch64" ] && [ "$ARCH" != "arm64" ]; then
    echo "ERROR: $0 only handles aarch64 (got '$ARCH')." >&2
    echo "       x86_64 tooling is pre-baked into the bionic builder image." >&2
    exit 1
fi

mkdir -p "$DEST"
cd "$DEST"

download_verified() {
    local url=$1 out=$2 expected=$3
    wget -q "$url" -O "$out"
    echo "$expected  $out" | sha256sum -c -
}

# appimagetool from AppImageKit release **12** — the SAME release as the
# runtime below, and that is the whole point.
#
# The obvious modern choice, appimagetool 1.9.1, cannot be used here: its
# bundled mksquashfs supports **zstd only**, while the ET_EXEC runtime this
# packaging needs (see below) reads only xz and zlib. The two cannot be made to
# agree — `--comp xz` fails with "Compressor xz is not supported", and the
# default zstd produces an image whose own runtime cannot open it ("Squashfs
# image uses (null) compression"). That mismatch shipped silently: the package
# has the right name, ELF type and magic, and dies on first extraction.
#
# Taking both halves from release 12 makes them compatible by construction, and
# both are immutable release assets rather than a moving `continuous` tag.
# build_appimage.sh proves the pairing after every build by asking the image to
# extract itself.
download_verified \
  "https://github.com/AppImage/AppImageKit/releases/download/12/appimagetool-aarch64.AppImage" \
  appimagetool.AppImage \
  c9d058310a4e04b9fbbd81340fff2b5fb44943a630b31881e321719f271bd41a
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

# `A --appimage-extract && mv B` looks like it fails loudly under `set -e`, and
# does not: POSIX exempts every command of an AND-OR list except the last, so a
# failed extraction left NO AppDir and this script still exited 0. The caller
# then built happily until appimagetool turned out to be "not available", one
# hundred log lines away from the real message — which was
# `libz.so: cannot open shared object file` (AppImageKit-12's appimagetool
# links the unversioned soname, so the build image needs zlib1g-dev, not just
# zlib1g). Separate statements, then assert the result.
extract_tool() {   # extract_tool <file.AppImage> <dest.AppDir>
    local img="$1" dir="$2"
    rm -rf squashfs-root
    if ! "./$img" --appimage-extract >/dev/null; then
        echo "ERROR: $img could not extract itself." >&2
        echo "       Missing a shared library? Try: ./$img --appimage-extract" >&2
        exit 1
    fi
    mv squashfs-root "$dir"
    [ -x "${dir}/AppRun" ] || { echo "ERROR: ${dir}/AppRun missing after extraction" >&2; exit 1; }
}
extract_tool linuxdeploy.AppImage  linuxdeploy.AppDir
extract_tool appimagetool.AppImage appimagetool.AppDir
rm -f ./*.AppImage

echo "$DEST"
