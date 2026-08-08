#!/usr/bin/env bash
# =============================================================================
#  build_native_pi.sh — NATIVE build of POM2 on a Raspberry Pi.
#
#  WHY, rather than the shipped `POM2-v*-aarch64.AppImage`: that AppImage is
#  built for GENERIC aarch64 (it has to run from a Pi 3 to a Pi 5). Here we
#  compile with -mcpu=<the actual core>, which lets GCC use the real
#  instruction set and cost model. POM2's hot loop is the 6502 interpreter and
#  the Disk II LSS — exactly the branch-dense code that cares about -mtune.
#
#  BIGGER STILL than -mcpu: profile-guided optimisation (--pgo). Compile once
#  with counters, run a representative workload (pgo_train.sh), recompile
#  feeding GCC that profile. It then knows which way each branch usually goes
#  and lays the blocks out so the frequent case falls through — fewer taken
#  jumps, and above all a far better-used instruction cache. That counts double
#  on a Cortex-A72 (32 KB L1i, a modest predictor next to a desktop x86 core).
#
#  Usage (ON the Pi, in a checkout):
#      packaging/raspberry/build_native_pi.sh              # → build-pi/
#      packaging/raspberry/build_native_pi.sh --pgo        # 2 passes + LTO
#      sudo packaging/raspberry/build_native_pi.sh --pgo --install
#      POM2_LTO=1 packaging/raspberry/build_native_pi.sh   # LTO only
#
#  --install runs `cmake --install` into $POM2_PREFIX (default /opt/POM2), the
#  layout ResourcePaths expects: <prefix>/bin/POM2 + <prefix>/share/POM2/{roms,
#  fonts,pic}. See docs/PERFORMANCE.md for the measurement recipe.
#
#  (c) 2026 VERHILLE Arnaud — POM2.
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

PREFIX="${POM2_PREFIX:-/opt/POM2}"
BUILD_DIR="${POM2_BUILD_DIR:-build-pi}"
DO_INSTALL=0
DO_PGO=0
for a in "$@"; do
    case "$a" in
        --install) DO_INSTALL=1 ;;
        --pgo)     DO_PGO=1 ;;
        *) echo "unknown option: $a  (--pgo, --install)"; exit 1 ;;
    esac
done

# --- 1. Identify the core ----------------------------------------------------
# The model string is in /proc/device-tree/model ("Raspberry Pi 4 Model B Rev
# 1.5"). We do NOT rely on -mcpu=native alone: on some 64-bit kernels the MIDR
# GCC reads is incomplete and detection silently falls back to generic.
MODEL="$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || echo unknown)"
case "$MODEL" in
    *"Raspberry Pi 5"*)            MCPU=cortex-a76 ;;
    *"Raspberry Pi 4"*|*"Pi 400"*) MCPU=cortex-a72 ;;
    *"Raspberry Pi 3"*)            MCPU=cortex-a53 ;;
    *)                             MCPU=native ;;
esac
# Guard rail: if the compiler rejects that -mcpu (GCC too old), fall back to
# generic rather than failing 20 minutes later on some random .cpp.
if ! echo 'int main(){}' | ${CXX:-g++} -x c++ -mcpu=$MCPU -o /dev/null - 2>/dev/null; then
    echo "[build_native_pi] WARNING: -mcpu=$MCPU rejected by $(${CXX:-g++} --version | head -1) → generic"
    MCPU=""
fi
ARCH_FLAGS=""
[ -n "$MCPU" ] && ARCH_FLAGS="-mcpu=$MCPU -mtune=$MCPU"

# LTO: the link takes minutes on a Pi 4 and wants ~1.5 GB. Opt-in on its own;
# --pgo turns it on for the second pass when there is enough RAM.
LTO_ON=OFF
[ "${POM2_LTO:-0}" = "1" ] && LTO_ON=ON

echo "[build_native_pi] model : $MODEL"
echo "[build_native_pi] flags : ${ARCH_FLAGS:-<generic>}$( [ "$LTO_ON" = ON ] && echo ' +LTO')$( [ "$DO_PGO" = 1 ] && echo ' +PGO')"

# A Pi 4 has 4 cores but often 2-4 GB: -j4 on heavy C++17 ends in an OOM kill
# ("c++: fatal error: Killed signal terminated program cc1plus").
MEM_MB=$(($(awk '/MemTotal/{print $2}' /proc/meminfo) / 1024))
JOBS="${POM2_JOBS:-}"
if [ -z "$JOBS" ]; then
    JOBS=$(( MEM_MB / 900 )); [ "$JOBS" -lt 1 ] && JOBS=1
    NPROC=$(nproc); [ "$JOBS" -gt "$NPROC" ] && JOBS=$NPROC
fi

# POM2_GLES=ON is not optional on a Pi: Mesa's V3D caps *desktop* GL at 3.1, so
# POM2's GL 3.2 core request fails outright and no window ever opens.
configure() {                 # configure <extra-flags> <IPO ON/OFF>
    cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
          -DPOM2_GLES=ON \
          -DPOM2_ENABLE_TESTS=OFF \
          -DCMAKE_INTERPROCEDURAL_OPTIMIZATION="$2" \
          -DCMAKE_C_FLAGS="$ARCH_FLAGS $1" \
          -DCMAKE_CXX_FLAGS="$ARCH_FLAGS $1" \
          -DCMAKE_EXE_LINKER_FLAGS="$1"
}

# TUs that MUST carry a profile. They are POM2's measured hot spots (see
# docs/PERFORMANCE.md): the CPU core, the bus, the LSS + flux walker, and the
# video decoders. If any of them has no .gcda, the training run did not
# actually execute — fail loudly instead of shipping a binary that silently
# got no benefit (this is the trap -Wno-missing-profile makes invisible).
HOT_TUS="M6502 Memory DiskIICard DiskImage Apple2Display"

if [ "$DO_PGO" = "1" ]; then
    PROFDIR="$ROOT/$BUILD_DIR-profile"
    # ⚠ BOTH PASSES SHARE ONE BUILD DIRECTORY. GCC names each .gcda after the
    # ABSOLUTE PATH of the object it belongs to. Instrumenting in one build dir
    # and reading back from another finds NO profile — and with
    # -Wno-missing-profile (which we need for the ImGui/GLFW TUs that are never
    # trained) the failure is COMPLETELY SILENT: the binary comes out with zero
    # gain and zero diagnostics.
    rm -rf "$PROFDIR"; mkdir -p "$PROFDIR"

    echo "[build_native_pi] PGO pass 1/2: instrumented pom2_bench"
    configure "-fprofile-generate=$PROFDIR" OFF
    cmake --build "$BUILD_DIR" -j"$JOBS" --target pom2_bench

    echo "[build_native_pi] PGO: training run (a few minutes)"
    "$ROOT/packaging/raspberry/pgo_train.sh" "$BUILD_DIR/pom2_bench" "$ROOT"

    for must in $HOT_TUS; do
        find "$PROFDIR" -name "*${must}*.gcda" | grep -q . \
            || { echo "ERROR: no profile for $must — the training run executed nothing"; exit 1; }
    done

    # ⚠ SECOND HALF OF THE SAME TRAP, and it is POM2-specific. The training
    # driver is `pom2_bench`; the binary we ship is `POM2` (target
    # pom2_imgui). CMake compiles each target's sources into ITS OWN object
    # directory, so the emulator core exists twice on disk:
    #     build-pi/CMakeFiles/pom2_bench.dir/src/Memory.cpp.o
    #     build-pi/CMakeFiles/pom2_imgui.dir/src/Memory.cpp.o
    # and the .gcda GCC wrote is named after the FIRST path only. Without the
    # copy below, pass 2 would rebuild POM2 with no profile at all for the very
    # files that matter — again silently. GCC mangles the object path into the
    # file name by turning '/' into '#', so swapping the target directory is a
    # pure string substitution on the name.
    copied=0
    for f in "$PROFDIR"/*pom2_bench.dir*.gcda; do
        [ -e "$f" ] || continue
        cp -f "$f" "${f//pom2_bench.dir/pom2_imgui.dir}" && copied=$((copied+1))
    done
    echo "[build_native_pi] PGO: $copied profiles mapped onto the pom2_imgui objects"
    for must in $HOT_TUS; do
        find "$PROFDIR" -name "*pom2_imgui.dir*${must}*.gcda" | grep -q . \
            || { echo "ERROR: $must has no profile under the pom2_imgui objects —"; \
                 echo "       the shipped binary would get NO benefit. Check the"; \
                 echo "       target/object names above."; exit 1; }
    done

    # LTO on the second pass only if the machine can take it: POM2's LTO link
    # wants well over 1 GB. On a 1 GB Pi keep PGO alone (most of the gain).
    PGO_IPO=ON
    [ "$MEM_MB" -lt 2000 ] && { PGO_IPO=OFF; echo "[build_native_pi] < 2 GB RAM → LTO off"; }
    echo "[build_native_pi] PGO pass 2/2: final build (profile${PGO_IPO:+ + LTO})"
    configure "-fprofile-use=$PROFDIR -fprofile-correction -fprofile-partial-training -Wno-missing-profile" "$PGO_IPO"
    cmake --build "$BUILD_DIR" -j"$JOBS"
else
    echo "[build_native_pi] building with -j$JOBS"
    configure "" "$LTO_ON"
    cmake --build "$BUILD_DIR" -j"$JOBS"
fi

echo "[build_native_pi] OK: $BUILD_DIR/POM2"

# --- 3. Install (optional) ---------------------------------------------------
if [ "$DO_INSTALL" = "1" ]; then
    [ "$(id -u)" -eq 0 ] || { echo "ERROR: --install needs root (sudo)"; exit 1; }
    cmake --install "$BUILD_DIR" --prefix "$PREFIX"
    # The bench is not part of the install rules but is what you re-measure
    # with after any change, so put it beside the binary it measures.
    install -m 755 "$BUILD_DIR/pom2_bench" "$PREFIX/bin/" 2>/dev/null || true
    echo "[build_native_pi] installed into $PREFIX"
fi
