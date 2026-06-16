#!/usr/bin/env bash
# Download Tom Harte "SingleStepTests/65x02" ProcessorTests for exhaustive,
# local CPU validation. The full corpus is ~1.4 GB per CPU variant (256 opcode
# files × ~5 MB), far too large to vendor or fetch at CMake-configure time, so
# the CTest gate ships only a curated subset (tests/tomharte_*.manifest). Use
# this script to pull a complete variant when you want to run every opcode.
#
#   tests/fetch_tomharte.sh <variant> <out-dir> [--curated|--all]
#
#   variant  : 6502 | wdc65c02 | rockwell65c02 | synertek65c02
#   out-dir  : destination directory (created if missing)
#   --all    : 256 opcode files 00..ff   (default)
#   --curated: only the opcodes in the matching tests/tomharte_*.manifest
#
# Then point the harness at it:
#   build/test_tomharte_cpu nmos <out-dir>          # for 6502
#   build/test_tomharte_cpu cmos <out-dir>          # for any 65C02 variant
#
# Source of truth: https://github.com/SingleStepTests/65x02
set -euo pipefail

BASE="https://raw.githubusercontent.com/SingleStepTests/65x02/main"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

variant="${1:-}"
outdir="${2:-}"
mode="${3:---all}"

if [[ -z "$variant" || -z "$outdir" ]]; then
    grep '^#' "$0" | sed 's/^# \{0,1\}//' | head -n 18
    exit 2
fi
case "$variant" in
    6502|wdc65c02|rockwell65c02|synertek65c02) ;;
    *) echo "error: unknown variant '$variant'" >&2; exit 2 ;;
esac

mkdir -p "$outdir"

# Build the opcode list.
ops=()
if [[ "$mode" == "--curated" ]]; then
    manifest="$SCRIPT_DIR/tomharte_${variant}.manifest"
    [[ "$variant" != 6502 && "$variant" != wdc65c02 ]] && manifest="$SCRIPT_DIR/tomharte_wdc65c02.manifest"
    [[ -f "$manifest" ]] || { echo "error: no manifest $manifest" >&2; exit 2; }
    while read -r op _; do [[ "$op" =~ ^[0-9a-fA-F]{2}$ ]] && ops+=("$op"); done < "$manifest"
else
    for i in $(seq 0 255); do ops+=("$(printf '%02x' "$i")"); done
fi

echo "Fetching ${#ops[@]} opcode file(s) for '$variant' → $outdir"
n=0
for op in "${ops[@]}"; do
    dst="$outdir/$op.json"
    if [[ -s "$dst" ]]; then continue; fi
    curl -fsS -m 120 -o "$dst" "$BASE/$variant/v1/$op.json" &
    n=$((n + 1))
    # throttle to ~8 parallel transfers
    if (( n % 8 == 0 )); then wait; fi
done
wait

got=$(find "$outdir" -name '*.json' -size +0c | wc -l | tr -d ' ')
echo "Done: $got JSON file(s) present in $outdir"
