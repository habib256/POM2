# POM2 — Apple II Emulator

POM2 is a small Apple II / II+ emulator built around a Dear ImGui shell:
MOS 6502 CPU, 48 KB RAM, soft-switch I/O, text / lo-res / hi-res video.

## Status (v0.1)

Working:

- MOS 6502 (Klaus Dormann functional-test clean)
- 48 KB RAM + 12 KB ROM ($D000-$FFFF) with the standard reset / IRQ / NMI vectors
- Soft switches at $C000-$C07F: keyboard, strobe, speaker, paddles, push-buttons
- Display modes: text 40×24, lo-res 40×48, hi-res 280×192 (monochrome)
- Mixed mode (text rows 20-23 over hi-res / lo-res)
- Page 1 / Page 2 selection
- Threaded emulation (UI 60 Hz, CPU pinned to 1.0227 MHz)
- Hard reset (F2)
- Built-in 5×7 ASCII font fallback when no character ROM is present
- Memory viewer / hex editor (Debug → Memory viewer): region-coloured 64 KB
  grid, ASCII column, change-flash highlighting, undo/redo, hex / ASCII
  search, bookmarks, togglable 6502 disassembly view sharing the cursor

Not yet:

- NTSC artifact colour for hi-res (currently rendered monochrome)
- Speaker audio (the toggle counter is exposed but not piped to an audio device)
- Disk II / .dsk / .nib / .woz support
- Slot ROM space ($C100-$C7FF) is empty
- Joystick / paddle live input (only the soft-switch bits are wired)
- 80-column / //e auxiliary memory
- Language Card / RAM bank-switching at $D000

## Build

```bash
./setup_imgui.sh   # one-time: installs deps + clones Dear ImGui
cd build && cmake .. && make -j
./run_emulator.sh  # equivalent to ./build/POM2 launched from the repo root
```

The setup script handles macOS (Homebrew), Debian / Ubuntu (apt), Fedora (dnf),
and Arch (pacman). Windows: install GLFW via vcpkg and run CMake manually.

## ROM placement

Drop your own Apple II / II+ ROMs at:

- `roms/apple2.rom` — 12 KB Autostart Monitor + Applesoft (`$D000-$FFFF`)
  or 16 KB image (`$C000-$FFFF`); the I/O page is preserved.
- `roms/apple2_char.rom` — 2 KB / 4 KB / 8 KB character ROM (optional).

Without `apple2.rom` the CPU still runs but executes whatever ROM stub is
left at `$F800` — the screen will stay dark. POM2 prints the ROM status in
the menu bar (`loaded: ...` or `NO ROM`).

## Keyboard

The host keyboard maps straight to the Apple II keyboard latch. Special keys:

| Host        | Apple II  |
|-------------|-----------|
| Enter       | Return    |
| Backspace   | ←         |
| Left arrow  | ←         |
| Right arrow | →         |
| Up arrow    | ↑         |
| Down arrow  | ↓         |
| Esc         | ESC       |
| Ctrl-A..Z   | $01..$1A  |
| F2          | Hard reset (host-side) |

Both upper and lower case letters pass straight through — POM2 doesn't
force-uppercase what you type.

## Layout

Single ImGui main window with three child panels:

- **Apple II Screen** — 280×192 RGBA texture rendered each frame.
- **Emulation** — CPU registers, cycle counter, speed selector (1×, 2×, MAX),
  ROM status.
- **Memory viewer** — togglable via `Debug → Memory viewer`. Hex grid +
  ASCII column over the full 64 KB, region-coloured by Apple II zone,
  search / edit / bookmarks / 6502 disassembly toggle.

## Licence

GPL-3.0.
