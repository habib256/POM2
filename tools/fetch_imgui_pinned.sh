#!/usr/bin/env bash
#
# fetch_imgui_pinned.sh — clone Dear ImGui at the pinned commit, and NOTHING else.
#
# `setup_imgui.sh` is the developer-facing convenience: it also installs system
# dependencies (`sudo apt install …` on Linux, `brew install cmake glfw …` on
# macOS). Neither is wanted during a release:
#
#   * Inside the bionic builder container the whole toolchain is already baked
#     in, and touching bionic's EOL apt mirrors is the exact fragility that
#     container exists to eliminate.
#   * On the macOS runner, `brew install glfw` would put a single-architecture
#     GLFW with an absolute Homebrew prefix on the image — competing with the
#     universal STATIC GLFW the release builds from source, and risking the
#     "Library not loaded /usr/local/opt/glfw/..." failure on Apple Silicon that
#     packaging/macos/build_universal_deps.sh exists to prevent.
#
# The pin itself still comes from imgui_pin.env, so there is exactly ONE place
# that decides which Dear ImGui POM2 builds against — dev, CI and release alike.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# shellcheck source=../imgui_pin.env
source "${REPO_ROOT}/imgui_pin.env"

if [ -d imgui/.git ] && [ "$(git -C imgui rev-parse HEAD 2>/dev/null)" = "$IMGUI_COMMIT" ]; then
    echo "Dear ImGui already at the pinned ${IMGUI_COMMIT:0:12}"
    exit 0
fi

rm -rf imgui
echo "Cloning Dear ImGui ($IMGUI_BRANCH @ ${IMGUI_COMMIT:0:12}, v$IMGUI_VERSION)"
git init -q imgui
git -C imgui remote add origin "$IMGUI_REPO"

# Fetch the exact commit if the server allows it (GitHub does), which keeps the
# clone shallow. `docking` is force-pushed on every upstream rebase, so falling
# back to the branch tip means building something other than what was tested —
# that is a hard error in a release, not a warning.
if git -C imgui fetch --depth 1 origin "$IMGUI_COMMIT" 2>/dev/null; then
    git -C imgui checkout -q FETCH_HEAD
else
    echo "ERROR: could not fetch the pinned commit $IMGUI_COMMIT." >&2
    echo "       Refusing to fall back to the ${IMGUI_BRANCH} tip for a release build." >&2
    exit 1
fi

HEAD_SHA="$(git -C imgui rev-parse HEAD)"
[ "$HEAD_SHA" = "$IMGUI_COMMIT" ] \
    || { echo "ERROR: imgui HEAD is $HEAD_SHA, expected $IMGUI_COMMIT" >&2; exit 1; }
echo "OK: Dear ImGui at $HEAD_SHA"
