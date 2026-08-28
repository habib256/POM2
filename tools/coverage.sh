#!/usr/bin/env bash
# POM2 — line coverage over the test suite, with a floor.
#
# WHY. The 2026-08-28 architecture plan asserted that three subsystems were
# untested: TnfsClient, FujiNetNetDevice and FloppyEmuDevice. All three had
# test suites. Holes were being found by reading, and reading got it wrong
# three times out of three. This measures instead, and names the LINES.
#
# Clang source-based coverage, not gcov: it counts regions, so a half-taken
# `a && b` and an untaken `else` are visible rather than reading as covered.
#
#   tools/coverage.sh                # measure, report, check the floor
#   tools/coverage.sh --update       # re-record the floor from this run
#   tools/coverage.sh --html DIR     # also write a browsable report
#
# The floor may go UP freely; it may not go down. Same ratchet shape as
# tools/check_file_sizes.sh, and for the same reason: a rule with no
# mechanism measured -74 % on this repo (see that script's header).

set -uo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${POM2_COVERAGE_BUILD:-build-coverage}"
FLOOR_FILE="tools/coverage_floor.txt"
UPDATE=0
HTML_DIR=""

while [ $# -gt 0 ]; do
    case "$1" in
        --update) UPDATE=1 ;;
        --html)   shift; HTML_DIR="${1:-coverage-html}" ;;
        *) echo "usage: $0 [--update] [--html DIR]" >&2; exit 2 ;;
    esac
    shift
done

# The Xcode toolchain ships these but does not put them on PATH.
if command -v xcrun > /dev/null 2>&1; then
    PROFDATA=$(xcrun --find llvm-profdata 2>/dev/null || echo llvm-profdata)
    COV=$(xcrun --find llvm-cov 2>/dev/null || echo llvm-cov)
else
    PROFDATA="${LLVM_PROFDATA:-llvm-profdata}"
    COV="${LLVM_COV:-llvm-cov}"
fi
for t in "$PROFDATA" "$COV"; do
    command -v "$t" > /dev/null 2>&1 || {
        echo "coverage: $t not found — install the LLVM tools" >&2; exit 2; }
done

echo "coverage: configuring $BUILD_DIR"
# C is not enabled as a project language, so naming a C compiler only earns a
# "manually-specified variable was not used" warning.
mkdir -p "$BUILD_DIR"
cmake -S . -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE=Debug \
      -DPOM2_ENABLE_TESTS=ON \
      -DPOM2_COVERAGE=ON \
      -DCMAKE_CXX_COMPILER=clang++ > "$BUILD_DIR/configure.log" 2>&1 || {
    echo "coverage: configure failed" >&2
    tail -30 "$BUILD_DIR/configure.log" >&2
    exit 1
}

echo "coverage: building"
cmake --build "$BUILD_DIR" --parallel "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
      > "$BUILD_DIR/build.log" 2>&1 || {
    echo "coverage: build failed" >&2
    echo "--- compiler ---" >&2
    "${CXX:-clang++}" --version 2>&1 | head -2 >&2
    # The ERRORS, not the tail. A --parallel build ends with whatever finished
    # last, which is usually a target that succeeded, so a tail is exactly the
    # part of the log that does not say what went wrong. (This cost a CI round
    # trip the first time the job ran.)
    echo "--- errors ---" >&2
    grep -nE "error:|Error [0-9]|fatal error" "$BUILD_DIR/build.log" | head -40 >&2
    # A build that ran out of room says so in a way easy to miss among 200
    # parallel targets, and the coverage tree is big enough for that to be the
    # first thing to check.
    echo "--- disk ---" >&2
    df -h . >&2
    du -sh "$BUILD_DIR" 2>/dev/null >&2
    echo "--- context around the first error ---" >&2
    grep -n -B6 -A12 -m1 "error:" "$BUILD_DIR/build.log" >&2 || \
        tail -40 "$BUILD_DIR/build.log" >&2
    exit 1
}

PROF_DIR="$BUILD_DIR/profraw"
rm -rf "$PROF_DIR"; mkdir -p "$PROF_DIR"

# One .profraw per PROCESS (%p), not per test: several tests fork, and a
# shared filename would have the children stamp on each other's counters.
#
# pom2_core_sdk_consumer is excluded, and the reason is not "it is slow". It
# configures a SEPARATE cmake project against the installed POM2::core and
# links it with plain flags — against a coverage-instrumented archive, which
# needs -fprofile-instr-generate at link time and does not get it. Teaching
# the exported package about it would bake a build-mode flag into what
# consumers install. It measures the install/export contract, not POM2's
# code, so it has nothing to contribute to a coverage number anyway; the
# normal build/ run is where it belongs.
echo "coverage: running the suite"
( cd "$BUILD_DIR" && \
  LLVM_PROFILE_FILE="$PWD/profraw/pom2-%p.profraw" \
  ctest --output-on-failure -E '^pom2_core_sdk_consumer$' \
        -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
  > ctest.log 2>&1 )
CTEST_RC=$?
if [ "$CTEST_RC" -ne 0 ]; then
    echo "coverage: the suite is not green — measure on a green tree" >&2
    grep -E "tests passed|FAILED" "$BUILD_DIR/ctest.log" >&2
    exit 1
fi

shopt -s nullglob
RAW=( "$PROF_DIR"/*.profraw )
if [ ${#RAW[@]} -eq 0 ]; then
    echo "coverage: no .profraw produced — is POM2_COVERAGE really on?" >&2
    exit 2
fi
echo "coverage: merging ${#RAW[@]} profiles"
"$PROFDATA" merge -sparse -o "$BUILD_DIR/pom2.profdata" "${RAW[@]}" || exit 1

# WHAT IS MEASURED: the code the TEST SUITE LINKS, not the whole program.
#
# Reporting against the test binaries rather than the GUI one is a deliberate
# scope, and the alternative is worse. `POM2` links the ImGui frontend, which
# no headless test can exercise, so including it would put ~15 000 unreachable
# lines in the denominator, report ~27 %, and make the floor a measure of how
# much UI exists rather than of how well anything is tested.
#
# So the number answers: "of the code POM2's tests are built against, how much
# do they actually run?" That is the question a ratchet can act on.
#
# llvm-cov wants the archive members that carry the coverage mapping, and an
# Apple `.a` does not hand them over — the executables do.
OBJS=()
for f in "$BUILD_DIR"/tests/test_*; do
    [ -f "$f" ] && [ -x "$f" ] && OBJS+=(-object "$f")
done
if [ ${#OBJS[@]} -eq 0 ]; then
    echo "coverage: no test binaries in $BUILD_DIR/tests" >&2
    exit 2
fi
# The first -object is positional for llvm-cov; the rest keep their flag.
OBJS=("${OBJS[@]:1}")

# Vendored, generated, and the tests' own bodies are not ours to cover.
IGNORE='(third_party/|stb_image|imgui/|/tests/|Ssi263PhonemeData|ImageWriterRom|AppleIIeKeyboardLayout)'

REPORT=$("$COV" report "${OBJS[@]}" -instr-profile="$BUILD_DIR/pom2.profdata" \
         -ignore-filename-regex="$IGNORE" 2>/dev/null)
SUMMARY=$(printf '%s\n' "$REPORT" | grep '^TOTAL')
[ -n "$SUMMARY" ] || { echo "coverage: llvm-cov produced no summary" >&2; exit 2; }

# llvm-cov's columns: Regions/Missed/Cover, Functions/Missed/Executed,
# Lines/Missed/Cover, Branches/Missed/Cover. The THIRD percentage is lines.
pct3() { awk '{ n=0; for (i=1;i<=NF;i++) if ($i ~ /%$/) { n++; if (n==3) { gsub(/%/,"",$i); print $i; exit } } }'; }
LINES_PCT=$(printf '%s\n' "$SUMMARY" | pct3)
[ -n "$LINES_PCT" ] || { echo "coverage: could not read the line column" >&2; exit 2; }

if [ -n "$HTML_DIR" ]; then
    "$COV" show "${OBJS[@]}" -instr-profile="$BUILD_DIR/pom2.profdata" \
        -ignore-filename-regex="$IGNORE" -format=html -output-dir="$HTML_DIR" \
        > /dev/null 2>&1 && echo "coverage: HTML report in $HTML_DIR/index.html"
fi

echo ""
echo "coverage: line coverage ${LINES_PCT}%  (over the code the tests link)"
echo ""
echo "Least-covered first-party files — this is the list the number is for:"
printf '%s\n' "$REPORT" \
  | grep -E '\.(cpp|h)[[:space:]]' \
  | awk '{ n=0; for (i=1;i<=NF;i++) if ($i ~ /%$/) { n++; if (n==3) { gsub(/%/,"",$i); printf "%7.2f%%  %s\n", $i, $1; break } } }' \
  | sort -n | head -15 | sed 's/^/  /'

if [ "$UPDATE" -eq 1 ]; then
    # Recorded HALF A POINT BELOW what was measured, on purpose. Two runs of
    # the same tree differ by ~0.1 % — tests that fork, a timing-shaped case
    # that takes a different branch — and a floor pinned to the exact reading
    # would fail on noise, which is how a ratchet gets switched off. Half a
    # point absorbs that and still catches a real regression: adding one
    # untested file moves the number by far more.
    MARGIN=$(awk -v v="$LINES_PCT" 'BEGIN { m = v - 0.5; if (m < 0) m = 0; printf "%.2f", m }')
    printf '%s\n' "$MARGIN" > "$FLOOR_FILE"
    echo ""
    echo "coverage: measured ${LINES_PCT}%, floor recorded at ${MARGIN}%"
    echo "          (half a point of margin — see the note in this script)"
    exit 0
fi

if [ ! -f "$FLOOR_FILE" ]; then
    echo "coverage: $FLOOR_FILE is missing — run '$0 --update'" >&2
    exit 2
fi
FLOOR=$(head -1 "$FLOOR_FILE")
echo ""
# Integer compare on tenths: bash has no floats, and a floor to 0.1 % is
# finer than the noise between two runs of the same tree.
cur=$(printf '%.0f' "$(echo "$LINES_PCT" | awk '{print $1*10}')")
flr=$(printf '%.0f' "$(echo "$FLOOR"     | awk '{print $1*10}')")
if [ "$cur" -lt "$flr" ]; then
    echo "FAIL  line coverage ${LINES_PCT}% is below the floor ${FLOOR}%" >&2
    echo "      Add tests for what you changed, or lower the floor in" >&2
    echo "      $FLOOR_FILE and say why in the commit." >&2
    exit 1
fi
echo "coverage: passed (floor ${FLOOR}%)"
exit 0
