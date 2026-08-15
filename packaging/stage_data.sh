#!/usr/bin/env bash
#
# stage_data.sh — copy POM2's package payload into a staging tree, and verify a
# staged tree against the same list.
#
# The list itself lives in packaging/bundle.manifest and is NOT duplicated here.
# CMake reads that manifest for its install() rules and for the WASM
# --preload-file arguments; this script is for the packagers that do not go
# through `cmake --install` — the macOS .app and the Windows .zip. (The AppImage
# does go through `cmake --install`, so it inherits the manifest for free.)
#
# Usage:
#   stage_data.sh <dest>            copy the desktop payload into <dest>
#   stage_data.sh --verify <dest>   assert the payload is there and nothing
#                                   from the deny list leaked in
#   stage_data.sh --self-test       stage into a temp dir, verify, clean up
#   stage_data.sh --list [kind]     print manifest entries (for CMake/debugging)
#
# `--verify` is the guard every CI package job runs. It exists because both
# failure modes are SILENT: a missing font drops the UI to ImGui's bitmap face
# with blank icon boxes, and a leaked disks_5.4/ turns a 6 MB download into a
# 200 MB one carrying media that is not ours to redistribute. Neither shows up
# as a build error.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$REPO_ROOT/packaging/bundle.manifest"

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
log() { printf '%s\n' "$*"; }

[ -f "$MANIFEST" ] || die "manifest not found: $MANIFEST"

# Read the manifest into three arrays. Blank lines and #-comments are dropped;
# an unknown kind is a hard error rather than a silent skip, so a typo in the
# manifest cannot quietly remove a font from every package.
DIRS=() FILES_SRC=() FILES_DST=() WASM=() DENY=() DENYGLOB=()
while read -r kind src dst _rest; do
    case "$kind" in
        ''|'#'*) continue ;;
        dir)  [ -n "$src" ] || die "manifest: 'dir' with no path";  DIRS+=("$src") ;;
        file) [ -n "$src" ] || die "manifest: 'file' with no path"
              FILES_SRC+=("$src"); FILES_DST+=("${dst:-$src}") ;;
        wasm) WASM+=("$src") ;;
        deny) DENY+=("$src") ;;
        denyglob) DENYGLOB+=("$src") ;;
        *)    die "manifest: unknown entry kind '$kind' (dir|file|wasm|deny|denyglob)" ;;
    esac
done < <(sed 's/#.*//' "$MANIFEST")

# ─── --list ─────────────────────────────────────────────────────────────────
if [ "${1:-}" = "--list" ]; then
    case "${2:-all}" in
        dir)  printf '%s\n' "${DIRS[@]:-}" ;;
        wasm) printf '%s\n' "${WASM[@]:-}" ;;
        deny) printf '%s\n' "${DENY[@]:-}" ;;
        file) for i in "${!FILES_SRC[@]}"; do
                  printf '%s\t%s\n' "${FILES_SRC[$i]}" "${FILES_DST[$i]}"; done ;;
        all)  printf '%s\n' "${DIRS[@]:-}"
              for i in "${!FILES_SRC[@]}"; do printf '%s\n' "${FILES_DST[$i]}"; done ;;
        *)    die "--list takes dir|file|wasm|deny|all" ;;
    esac
    exit 0
fi

# ─── stage ──────────────────────────────────────────────────────────────────
stage() {
    local dest="$1"
    mkdir -p "$dest"
    for d in "${DIRS[@]:-}"; do
        [ -n "$d" ] || continue
        # A missing roms/ or fonts/ is fatal, not a warning: it produces a
        # package that starts and then behaves wrongly, which is worse than one
        # that never gets built.
        [ -d "$REPO_ROOT/$d" ] || die "manifest lists dir '$d' but it does not exist"
        mkdir -p "$dest/$d"
        cp -R "$REPO_ROOT/$d/." "$dest/$d/"
        # A `dir` copies the WORKING TREE, not what git tracks, so anything
        # dropped in the folder and never committed rides along. Prune the
        # deny-globbed names right back out — archives in particular, which the
        # emulator cannot read anyway.
        for g in "${DENYGLOB[@]:-}"; do
            [ -n "$g" ] || continue
            find "$dest/$d" -type f -name "$g" -print -delete
        done
        log "staged dir  $d"
    done
    for i in "${!FILES_SRC[@]}"; do
        local src="${FILES_SRC[$i]}" dst="${FILES_DST[$i]}"
        [ -f "$REPO_ROOT/$src" ] || die "manifest lists file '$src' but it does not exist"
        mkdir -p "$dest/$(dirname "$dst")"
        cp "$REPO_ROOT/$src" "$dest/$dst"
        log "staged file $src -> $dst"
    done
}

# ─── verify ─────────────────────────────────────────────────────────────────
# Checks the payload of a STAGED tree (an .app's Resources/, a zip's root, an
# extracted AppDir's usr/share/POM2). Both halves matter: everything promised
# is present, and nothing forbidden came along.
verify() {
    local dest="$1"
    [ -d "$dest" ] || die "verify: '$dest' is not a directory"
    local failed=0

    for d in "${DIRS[@]:-}"; do
        [ -n "$d" ] || continue
        if [ -d "$dest/$d" ] && [ -n "$(ls -A "$dest/$d" 2>/dev/null)" ]; then
            log "OK   dir  $d"
        else
            printf 'MISSING dir  %s\n' "$d" >&2; failed=1
        fi
    done
    for i in "${!FILES_SRC[@]}"; do
        local dst="${FILES_DST[$i]}"
        if [ -s "$dest/$dst" ]; then
            log "OK   file $dst"
        else
            printf 'MISSING file %s\n' "$dst" >&2; failed=1
        fi
    done

    # Deny list. Matched anywhere under the payload root, not just at the top:
    # a packager that staged `Resources/media/disks_5.4` would otherwise pass.
    for d in "${DENY[@]:-}"; do
        [ -n "$d" ] || continue
        local hit
        hit=$(find "$dest" -name "$(basename "$d")" -maxdepth 4 2>/dev/null | head -1 || true)
        if [ -n "$hit" ]; then
            printf 'LEAKED  %s (found at %s)\n' "$d" "$hit" >&2; failed=1
        fi
    done

    # Deny-globbed names, anywhere in the payload. Same reason as the deny list,
    # but for things that arrive INSIDE a copied folder rather than as one.
    for g in "${DENYGLOB[@]:-}"; do
        [ -n "$g" ] || continue
        local hits
        hits=$(find "$dest" -type f -name "$g" 2>/dev/null | head -5 || true)
        if [ -n "$hits" ]; then
            printf 'LEAKED  files matching %s:\n%s\n' "$g" "$hits" >&2; failed=1
        fi
    done

    # The browser-only extras must not be in a desktop package either — that is
    # the whole reason they carry their own kind.
    for d in "${WASM[@]:-}"; do
        [ -n "$d" ] || continue
        if [ -e "$dest/$d" ]; then
            printf 'LEAKED  %s (wasm-only, must not ship in a desktop package)\n' "$d" >&2
            failed=1
        fi
    done

    [ "$failed" -eq 0 ] || die "payload verification failed for '$dest'"
    log "OK: payload verified in $dest"
}

case "${1:-}" in
    --verify)
        [ $# -ge 2 ] || die "usage: stage_data.sh --verify <dest>"
        verify "$2"
        ;;
    --self-test)
        # Pinned by the `bundle_manifest` CTest. Proves the manifest parses, the
        # sources it names all exist, staging reproduces them, and the verifier
        # actually rejects a leak (a verifier that always passes is worse than
        # none — it reads as a guarantee).
        TMP="$(mktemp -d)"
        trap 'rm -rf "$TMP"' EXIT
        stage "$TMP"
        verify "$TMP"
        # Negative control: plant something from the deny list and require a
        # failure. `deny` entries are directories in the manifest.
        DENY_ONE="${DENY[0]:-}"
        if [ -n "$DENY_ONE" ]; then
            mkdir -p "$TMP/$(basename "$DENY_ONE")"
            # In a SUBSHELL: verify() reports a failure through die(), which
            # exits — inside `if` that would take this script down with it
            # instead of being caught as a false condition.
            if ( verify "$TMP" ) >/dev/null 2>&1; then
                die "self-test: verify() accepted a leaked '$DENY_ONE'"
            fi
            rm -rf "${TMP:?}/$(basename "$DENY_ONE")"
            log "OK: verify() rejects a leaked $DENY_ONE"
        fi
        log "OK: bundle manifest self-test passed"
        ;;
    ''|-h|--help)
        sed -n '3,25p' "$0"
        [ -z "${1:-}" ] && exit 1 || exit 0
        ;;
    *)
        stage "$1"
        verify "$1"
        ;;
esac
