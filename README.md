# POM2 — Apple II Emulator

POM2 is a small Apple II / II+ emulator built around a Dear ImGui shell:
MOS 6502 CPU, 48 KB RAM, soft-switch I/O, full text / lo-res / hi-res
video, 1-bit speaker audio, cassette deck, joystick, and a Disk II
controller in slot 6.

## Status (v0.1)

Working:

- MOS 6502 (Klaus Dormann functional-test clean, IRQ / NMI / BRK, BCD)
- 48 KB RAM + 12 KB ROM (`$D000-$FFFF`) with reset / IRQ / NMI vectors
- Soft switches `$C000-$C07F`: keyboard + strobe, speaker, cassette in/out,
  display modes, paddles (RC discharge modelled), push-buttons
- Display modes: text 40×24 (normal + inverse attribute), lo-res 40×48
  (16-colour //gs-corrected palette), hi-res 280×192
- Hi-res rendering: NTSC artifact colour (3-pass GEN2 LUT, 39 inter-byte
  seam fix-ups, optional additive glow) + monochrome White / Green (P31) /
  Amber phosphor variants
- Mixed mode (text rows 20-23 over hi-res / lo-res), Page 1 / Page 2
- 1-bit speaker through miniaudio: sub-instruction event timestamps,
  1-pole low-pass + DC blocker, volume / mute UI
- Cassette: `$C020` write-toggle and `$C060` comparator wired to a
  procedural cassette deck (Play / Record / Rewind, Font Awesome icons)
- Disk II controller in slot 6: 35 × 6656-byte pre-nibblised tracks,
  `.dsk` / `.do` (DOS 3.3 logical sector order), boot via `PR#6`, read-only
- Joystick: GLFW host pad → paddles 0/1 + buttons PB0/PB1/PB2 (`$C061-$C063`,
  `$C064-$C067`), hot-plug-friendly, autobinds first present pad
- 8-slot expansion bus: `$C080-$C0FF` device-select, `$C100-$C7FF` slot ROM,
  `$C800-$CFFF` shared expansion ROM with `$CFFF` disable
- Snapshot save / load (`POM2SNAP` magic, named 8-byte sections, CPU + RAM
  + soft-switches; Disk II state intentionally excluded)
- CLI: `--preset ii|ii+`, `--speed`, `--cpu-max`, `--tape`, `--load addr:file`,
  `--run`, `--paste`, `--step`, `--play/--rec/--rewind`, `--snapshot-save/load`
- Threaded emulation (UI 60 Hz, CPU pinned to 1.0227 MHz, MAX uncapped)
- Reset: F11 (Ctrl-Reset / soft) and F12 (hard reset / power-cycle)
- Memory viewer / hex editor: region-coloured 64 KB grid, ASCII column,
  change-flash, undo/redo, hex / ASCII search, bookmarks, 6502 disassembly
  toggle. Plus three memory-map widgets (vertical bar, horizontal bar, grid).

Not yet:

- Language Card / RAM bank-switching at `$D000` (blocks ProDOS, Pascal, etc.)
- Disk II writes are accepted but not persisted; `.nib` / `.woz` / `.po`
  formats not loaded
- Slot ROM space outside slot 6 is empty (no Mockingboard, printer card, …)
- 80-column / //e auxiliary memory
- Integer BASIC preset (only Applesoft / Autostart is exercised today)
- Character-ROM flashing attribute animation (inverse renders statically)
- VBL strobe `$C019`, annunciators `$C058-$C05F`

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
- `roms/apple2_char.rom` — 2 KB / 4 KB / 8 KB character ROM (optional;
  POM2 falls back to a built-in 5×7 ASCII font).
- `roms/disk2.rom` — 256 B P5A boot PROM (AppleWin's copy works). When
  present POM2 auto-plugs the Disk II card in slot 6 at startup.

Without `apple2.rom` the CPU still runs but executes whatever stub is
left at `$F800` — the screen will stay dark. POM2 prints the ROM status in
the menu bar (`loaded: ...` or `NO ROM`).

## Disk images

Drop `.dsk` or `.do` images (143 360 bytes, DOS 3.3 logical sector order)
into `disks/`. Use **Hardware → Insert disk image** to mount one, then
boot it with `PR#6` from Applesoft. The image file is never modified —
writes go to a scratch nibble buffer and are dropped on eject.

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
| F11         | Reset / Ctrl-Reset (soft, host-side) |
| F12         | Hard reset / power-cycle (host-side) |

Both upper and lower case letters pass straight through — POM2 doesn't
force-uppercase what you type.

## Joystick

Plug a USB pad before launch (or hot-plug — POM2 polls all 16 GLFW slots
each frame). The first present pad auto-binds; **Hardware → Joystick**
opens a panel to pick a different one, set deadzone, and invert axes.
Mapping:

| Host        | Apple II        |
|-------------|-----------------|
| Axis X      | PADL(0) / `$C064` |
| Axis Y      | PADL(1) / `$C065` |
| Button 0    | PB0 / `$C061` (open-apple) |
| Button 1    | PB1 / `$C062` (closed-apple) |
| Button 2    | PB2 / `$C063` |

PADL(2) and PADL(3) read centred (127).

## Layout

ImGui main window with one menu bar and several panels:

- **Apple II Screen** — 280×192 RGBA texture rendered each frame.
- **Emulation** — CPU registers, cycle counter, speed selector
  (1×, 2×, MAX), ROM status, audio volume / mute.
- **Cassette deck** — procedural 378×404 deck (Hardware → Cassette deck).
- **Disk II (slot 6)** — PROM/motor LEDs, current track, Insert / Eject.
- **Joystick** — host-pad picker, live axis / button readout.
- **Memory viewer** — togglable via Debug → Memory viewer. Region-coloured
  hex grid + ASCII column + 6502 disassembly toggle, search / edit /
  bookmarks / undo-redo.
- **Memory Map** widgets — three layouts (Bar, Bar Horizontal, Grid) for
  visualising the 64 KB layout at a glance.

## Licence

GPL-3.0.
