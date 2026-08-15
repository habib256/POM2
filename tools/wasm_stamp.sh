#!/usr/bin/env bash
# =============================================================================
#  Fingerprint of the SOURCES that determine the committed WebAssembly bundle.
#
#  Why a guard at all: the live browser demo IS the `wasm/` folder in this
#  repository — GitHub Pages serves it straight from the branch. So the bundle
#  is not a build artifact that can lag harmlessly; it is published content. A
#  forgotten rebuild leaves an old demo online, silently, with nothing failing.
#
#  Why fingerprint the SOURCES and not the bundle: emcc output is not
#  reproducible across emsdk versions, and CI does not necessarily run the same
#  one as the dev machine. A byte-diff of POM2.wasm would fail permanently, for
#  nothing. The sources, by contrast, are deterministic.
#
#  "Deterministic" takes three precautions, all learned the same way — a red CI
#  when nothing had changed, because the stamp written on one machine did not
#  reproduce on another:
#    · the file list comes from `git ls-files`, NOT `find`: git sees only
#      TRACKED files (a scratch file left in src/ no longer shifts the result)
#      and sorts by bytes, independent of locale — `sort` under en_US.UTF-8 and
#      under C do NOT agree once a '/' or '_' decides the order;
#    · each file's line is recomposed HERE ("hash<space>path") instead of
#      copying the tool's own output: sha256sum, shasum and openssl each format
#      it differently;
#    · the hashing tool is picked among the three, since macOS has no
#      sha256sum without coreutils.
#
#  Usage:
#      tools/wasm_stamp.sh            # print the fingerprint
#      tools/wasm_stamp.sh --write    # write it to wasm/SOURCE_STAMP
#      tools/wasm_stamp.sh --check    # compare; non-zero exit if stale
#
#  Ported from NeoST's tools/wasm_stamp.sh (same author, same problem).
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

STAMP_FILE="wasm/SOURCE_STAMP"

if command -v sha256sum >/dev/null 2>&1; then
    SHA_CMD=(sha256sum)
elif command -v shasum >/dev/null 2>&1; then
    SHA_CMD=(shasum -a 256)
elif command -v openssl >/dev/null 2>&1; then
    SHA_CMD=(openssl dgst -sha256 -r)
else
    echo "ERROR: no sha256 tool available (sha256sum, shasum, openssl)." >&2
    exit 1
fi

# sha256 of stdin, bare hex. All three tools print "hash<sep>name"; keep field 1.
sha256_stdin() { "${SHA_CMD[@]}" | cut -d' ' -f1; }

# Everything that goes into the bundle: the emulator sources, the HTML shell,
# the build options, and the payload manifest (which decides what lands in
# POM2.data). NOT the payload files themselves — roms/ and floppyemu/ are large
# and change rarely, and hashing 33 MB on every check would buy nothing.
compute() {
    if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        echo "ERROR: not inside a git repository — the file list comes from git ls-files." >&2
        exit 1
    fi
    while IFS= read -r -d '' f; do
        case "$f" in
            src/*.cpp|src/*.h|src/*.hpp|src/hgrpaint/*|src/hgrsprite/*|\
            wasm/shell.html|CMakeLists.txt|packaging/bundle.manifest|imgui_pin.env)
                printf '%s %s\n' "$(sha256_stdin < "$f")" "$f" ;;
        esac
    done < <(git ls-files -z -- src wasm/shell.html CMakeLists.txt \
                                packaging/bundle.manifest imgui_pin.env) | sha256_stdin
}

CUR="$(compute)"

case "${1:-}" in
    --write)
        mkdir -p wasm
        printf '%s\n' "$CUR" > "$STAMP_FILE"
        echo "stamp written: $CUR"
        ;;
    --check)
        if [ ! -f "$STAMP_FILE" ]; then
            echo "ERROR: $STAMP_FILE is missing — the committed wasm/ bundle is untraceable." >&2
            echo "       Rebuild, then: tools/wasm_stamp.sh --write" >&2
            exit 1
        fi
        OLD="$(cat "$STAMP_FILE")"
        if [ "$CUR" != "$OLD" ]; then
            cat >&2 <<EOF
ERROR: the committed wasm/ bundle is STALE.
       source fingerprint : $CUR
       wasm/ fingerprint  : $OLD

GitHub Pages serves the wasm/ folder FROM THIS REPOSITORY, so until it is
rebuilt the online demo stays the old one.

  source /path/to/emsdk/emsdk_env.sh
  ./build_wasm.sh
  tools/wasm_stamp.sh --write
  git add wasm/ && git commit

Without emsdk: the release workflow's \`wasm\` job uploads the bundle it just
built (artifact POM2-web-wasm) EVEN when this check fails — unzip it into
wasm/, then --write.
EOF
            exit 1
        fi
        echo "OK: committed wasm/ bundle is current ($CUR)"
        ;;
    *)
        printf '%s\n' "$CUR"
        ;;
esac
