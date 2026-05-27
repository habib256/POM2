#!/usr/bin/env bash
# POM2 — WebAssembly build driver.
#
# Output: wasm/
#   index.html        — entry point (renamed from POM2.html)
#   POM2.js           — Emscripten loader
#   POM2.wasm         — compiled module
#   POM2.data         — preloaded asset bundle (roms/ + disks/ + …)
#   POM2.worker.js    — pthread worker (when USE_PTHREADS=1)
#   serve.py          — local dev server that sets COOP+COEP headers
#                       (required for SharedArrayBuffer / pthreads)
#
# Prerequisites:
#   - Emscripten SDK on PATH (`emcc -v` must succeed).
#       Install:  https://emscripten.org/docs/getting_started/downloads.html
#       Then:     source path/to/emsdk/emsdk_env.sh
#   - The host `./setup_imgui.sh` must have populated `imgui/` already.
#
# Usage:
#   ./build_wasm.sh                # release build
#   ./build_wasm.sh --clean        # nuke build_wasm/ first
#   ./build_wasm.sh --serve        # build + launch dev server on :8080

set -euo pipefail

cd "$(dirname "$0")"

CLEAN=0
SERVE=0
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        --serve) SERVE=1 ;;
        -h|--help)
            sed -n '2,/^set -e/p' "$0" | sed '$d' | sed 's/^# \?//'
            exit 0 ;;
        *) echo "unknown flag: $arg" >&2; exit 2 ;;
    esac
done

if ! command -v emcc >/dev/null 2>&1; then
    cat >&2 <<EOF
emcc not found. Install + activate the Emscripten SDK:
  git clone https://github.com/emscripten-core/emsdk.git
  cd emsdk && ./emsdk install latest && ./emsdk activate latest
  source ./emsdk_env.sh
EOF
    exit 1
fi

if [ ! -d imgui ]; then
    echo "imgui/ not found — running ./setup_imgui.sh first..."
    ./setup_imgui.sh
fi

BUILD_DIR=build_wasm
WASM_DIR=wasm

if [ $CLEAN -eq 1 ]; then
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR" "$WASM_DIR"

# Configure with the Emscripten toolchain.
emcmake cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPOM2_ENABLE_TESTS=OFF

# Build. -j auto: emmake picks reasonable parallelism.
cmake --build "$BUILD_DIR" -j

# Stage artefacts into wasm/.
ARTEFACTS=(POM2.html POM2.js POM2.wasm POM2.data POM2.worker.js)
for f in "${ARTEFACTS[@]}"; do
    src="$BUILD_DIR/$f"
    if [ -f "$src" ]; then
        cp -f "$src" "$WASM_DIR/"
    fi
done
# Rename the entry document so the deployment URL is clean.
if [ -f "$WASM_DIR/POM2.html" ]; then
    mv -f "$WASM_DIR/POM2.html" "$WASM_DIR/index.html"
fi

# Local dev server with COOP + COEP — required for SharedArrayBuffer /
# pthreads. python3's built-in http.server doesn't ship those headers,
# so we provide a tiny wrapper.
cat > "$WASM_DIR/serve.py" <<'PY'
#!/usr/bin/env python3
"""Local dev server for POM2 WASM with COOP+COEP headers.

Usage:  python3 serve.py [port]   # defaults to 8080
"""
import http.server, sys, os
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
os.chdir(os.path.dirname(os.path.abspath(__file__)))
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy",   "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        super().end_headers()
print(f"POM2 WASM dev server: http://127.0.0.1:{PORT}/")
http.server.ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
PY
chmod +x "$WASM_DIR/serve.py"

echo
echo "── done ────────────────────────────────────────────"
echo "  artefacts: $WASM_DIR/"
ls -lh "$WASM_DIR/" | awk 'NR>1 {printf "    %s %s\n", $5, $9}'
echo "  serve:    cd $WASM_DIR && python3 serve.py"
echo "  open:     http://127.0.0.1:8080/"
echo

if [ $SERVE -eq 1 ]; then
    exec python3 "$WASM_DIR/serve.py"
fi
