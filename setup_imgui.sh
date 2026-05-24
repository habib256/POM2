#!/bin/bash
set -e

echo "=== POM2 setup (Dear ImGui + GLFW) ==="

if [[ "$OSTYPE" == "darwin"* ]]; then
    if ! command -v brew &> /dev/null; then
        /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    fi
    brew install cmake glfw pkg-config

elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    if   command -v apt    &> /dev/null; then
        sudo apt update
        sudo apt install -y cmake libglfw3-dev pkg-config libgl1-mesa-dev g++
    elif command -v dnf    &> /dev/null; then
        sudo dnf install -y cmake glfw-devel pkgconfig mesa-libGL-devel gcc-c++
    elif command -v pacman &> /dev/null; then
        sudo pacman -S --needed cmake glfw-x11 pkgconfig mesa gcc
    else
        echo "Unknown package manager — install cmake, GLFW3 dev headers, and a C++17 compiler manually."
    fi
fi

if [ ! -d "imgui" ]; then
    echo "Cloning Dear ImGui..."
    git clone --depth 1 https://github.com/ocornut/imgui.git
fi

mkdir -p build
cd build
cmake ..

cat <<MSG

=== Setup complete. ===

Build:
  cd build && make -j

Run from the repo root (so roms/ probes resolve):
  ./build/POM2

ROM placement:
  Drop an Apple II / II+ ROM image at roms/apple2.rom (12 KB = \$D000-\$FFFF
  or 16 KB = \$C000-\$FFFF). Optional 2 KB character ROM at roms/apple2_char.rom
  (current build falls back to a built-in 5x7 ASCII font when missing).
MSG
