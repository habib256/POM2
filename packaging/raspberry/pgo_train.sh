#!/usr/bin/env bash
# =============================================================================
#  pgo_train.sh — TRAINING run for profile-guided optimisation.
#
#  Called by build_native_pi.sh --pgo between the two compile passes. The
#  binary passed as $1 is the INSTRUMENTED pom2_bench (-fprofile-generate):
#  every run drops counters that the second pass reads back.
#
#  WHY PGO matters more here than elsewhere: POM2's hot loop is a 6502
#  interpreter (an indirect branch on the opcode followed by a great many
#  rarely-taken conditionals) plus the Disk II LSS, which walks a flux stream
#  one bit-cell at a time. With no profile GCC assumes both sides of each
#  branch are equally likely; with one it orders the blocks so the frequent
#  case falls through — fewer taken jumps, less predictor pressure, and a much
#  better-used instruction cache. On a Cortex-A72 (32 KB L1i) that is where the
#  throughput is decided.
#
#  WHAT THE RUN MUST COVER — a too-narrow profile is WORSE than no profile: it
#  marks as "cold" code that is not. So we sweep the families of load, not one
#  boot:
#    · ROM banner on ][+ and //e (the universal path: CPU + bus + text render)
#    · a 5.25" boot (Disk II LSS + flux walker + the loader the disk runs)
#    · the video pipelines, which are separate decoders: NTSC LUT, mono,
#      the OE composite signal generator, the OE-CPU demod, AppleWin's IIR
#    · PAL timing (different frame geometry and event schedule)
#    · a pure no-render run (isolates the CPU/bus path)
#
#  Usage:  pgo_train.sh <path/to/pom2_bench> [repo-root]
#
#  (c) 2026 VERHILLE Arnaud — POM2.
# =============================================================================
set -uo pipefail

BENCH="${1:?usage: pgo_train.sh <pom2_bench> [root]}"
ROOT="${2:-$(cd "$(dirname "$0")/../.." && pwd)}"
cd "$ROOT"

[ -x "$BENCH" ] || { echo "ERROR: $BENCH not found or not executable"; exit 1; }

# A training load must NEVER fail the build: a missing disk image (not all of
# them are redistributable) means a slightly poorer profile, not a broken
# build.
run() {                      # run <label> <args…>
    local label="$1"; shift
    printf '  [pgo] %-40s' "$label"
    if "$BENCH" "$@" >/dev/null 2>&1; then echo "ok"; else echo "skipped"; fi
}

have() { [ -f "$1" ]; }

# Pick the first 5.25" image that exists, so the training still covers the
# disk path on a checkout with a different disks_5.4/ content.
DISK=""
for d in disks_5.4/dsk/*.dsk disks_5.4/dsk/*.do disks_5.4/woz/*.woz; do
    [ -f "$d" ] && { DISK="$d"; break; }
done

echo "[pgo_train] training run — $BENCH"

# --- 1. Banner: the most universal path (CPU + bus + text render) ------------
have roms/apple2p.rom && run "][+ banner, NTSC LUT"      --rom roms/apple2p.rom --frames 1500 --quiet
have roms/apple2e.rom && run "//e banner, NTSC LUT"      --rom roms/apple2e.rom --iie --frames 1500 --quiet
have roms/apple2e.rom && run "//e banner, PAL timing"    --rom roms/apple2e.rom --iie --pal --frames 800 --quiet
have roms/apple2p.rom && run "][+ banner, no render"     --rom roms/apple2p.rom --frames 1500 --no-render --quiet

# --- 2. Video pipelines: each is a separate decoder --------------------------
if have roms/apple2p.rom; then
    for m in mono medium 4bit oe oecpu applewin; do
        run "][+ banner, $m pipeline" --rom roms/apple2p.rom --mode "$m" --frames 400 --quiet
    done
fi

# --- 3. Disk: LSS + flux walker + whatever the disk loads --------------------
if [ -n "$DISK" ]; then
    run "5.25\" boot, NTSC LUT"   --disk "$DISK" --frames 900 --quiet
    run "5.25\" boot, OE-CPU"     --disk "$DISK" --mode oecpu --frames 400 --quiet
    have roms/apple2e.rom && \
        run "5.25\" boot on //e"  --rom roms/apple2e.rom --iie --disk "$DISK" --frames 900 --quiet
else
    echo "  [pgo] no 5.25\" image found — the Disk II LSS path will be COLD in"
    echo "        the profile, which makes it slower than an untrained build."
fi

echo "[pgo_train] done."
