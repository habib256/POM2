# POM2 — Apple II Emulator

![Disk II closeup](pic/diskii_closeup.webp)

POM2 is an Apple II / II+ / IIe / IIc / IIc+ emulator built around Dear
ImGui — MOS 6502 / 65C02, 48 KB / 128 KB RAM, full text / 80-column /
lo-res / hi-res / DHGR video, 1-bit speaker audio, cassette deck,
joystick, mouse, and an 8-slot peripheral bus (Disk II, Super Serial,
ProDOS clock, ProDOS HDV, Mockingboard, Mouse, Le Chat Mauve RGB).

## Table of contents

- [Quick start](#quick-start)
- [System profiles](#system-profiles)
- [ROM placement](#rom-placement)
- [Features](#features)
- [Disk images](#disk-images)
- [Keyboard](#keyboard)
- [Joystick](#joystick)
- [Slot configuration](#slot-configuration)
- [UI panels](#ui-panels)
- [Roadmap](#roadmap)
- [Licence](#licence)

## Quick start

```bash
./setup_imgui.sh   # one-time: installs deps + clones Dear ImGui
cd build && cmake .. && make -j
./run_emulator.sh  # = ./build/POM2 launched from the repo root
```

`setup_imgui.sh` covers macOS (Homebrew), Debian / Ubuntu (apt), Fedora
(dnf), and Arch (pacman). Windows: install GLFW via vcpkg and run CMake
manually. Drop ROMs into `roms/` and disk images into `disks/`; see the
two sections below for probe order and supported formats.

## System profiles


| Profile           | CPU   | iieMode | Main ROM probes                                         |
| ------------------- | ------- | --------- | --------------------------------------------------------- |
| Apple ][ (1977)   | NMOS  | off     | `apple2o.rom`, `apple2.rom`                             |
| Apple ][+ (1979)  | NMOS  | off     | `apple2p.rom`, `apple2.rom`                             |
| Apple //e (1985)  | 65C02 | on      | `apple2e.rom`                                           |
| Apple //c (1984)  | 65C02 | on      | `apple2c-32Kv0.rom`, `apple2c-16K.rom`                  |
| Apple //c+ (1988) | 65C02 | on      | `apple2cp.rom`, `apple2c-plus.rom`, `apple2c-32Kv0.rom` |

Pick the profile via the `Presets` menu or `--preset <ii|ii+|iie|iic|iic+>`.
Switching profiles is a full cold reset that re-plugs slot cards and
re-mounts previously inserted disks. The CPU mode follows the profile
unless overridden in `Machine → CPU`.

## ROM placement

Drop your ROMs into `roms/` (first existing file in the probe order
wins). Main-ROM sizes accepted: 12 KB (`$D000-$FFFF`, II/II+), 16 KB
(`$C000-$FFFF`, II+/IIe/IIc), 32 KB combined "system + video" dump
(IIe/IIc — lower 16 KB carries char/video data). Optional ROMs:

- `apple2_char.rom` — 2 KB (II/II+) or 4 KB (IIe mousetext); 5×7 fallback
- `disk2.rom` — 256 B P5A boot PROM; presence auto-plugs Disk II slot 6
- `diskii_p6.rom` — 256 B LSS sequencer PROM; needed for `.woz` and
  cycle-accurate protections (legacy 32-cycle gate is the fallback)
- `mouse_341-0270-c.bin` + `mouse_341-0269.bin` — both needed for the
  Mouse Interface slot entry

The status pill in the menu bar shows the resolved profile + ROM.

## Features

### CPU & memory

- MOS 6502 (Klaus Dormann clean, IRQ / NMI / BRK, BCD) + common 65C02 /
  Rockwell extensions (STZ, BRA, INA/DEA, PHX/PLY, BIT #imm, TSB/TRB,
  JMP (abs,X), zp-indirect, RMB/SMB/BBR/BBS, WAI/STP). NMOS or 65C02
  selectable in `Machine → CPU`.
- 48 KB + 16 KB ROM for II/II+; 128 KB (main + aux 64 KB) for IIe/IIc
  with all paging soft switches (80STORE, RAMRD, RAMWRT, INTCXROM,
  ALTZP, SLOTC3ROM, 80COL, ALTCHAR) and the internal `$C100-$CFFF` ROM.
- Language Card 16 KB bank-switched at `$D000-$FFFF` (two banks, aux
  banks under ALTZP); soft switches `$C000-$C07F` (keyboard, speaker,
  cassette, display modes, paddles, push-buttons, annunciators,
  scanline-accurate VBL).

### Display

- Text 40×24 (normal / inverse / 2 Hz flash) + lo-res 40×48 (16 NTSC
  colours).
- Hi-res 280×192 with three composite colour modes (artifact, medium,
  4-bit square filter) plus monochrome White / Green / Amber with
  phosphor afterglow.
- IIe: 80-column text + DHGR 560×192 (composite NTSC, Video-7 RGB,
  monochrome). Mixed mode on Page 1 / Page 2.
- **Le Chat Mauve / Féline** RGB card: direct 6-colour HGR with no
  inter-byte fringing, AN3+80COL FIFO mode select, four modes (BW560 /
  Mixed / Chunky / COL140), AppleWin `PaletteRGB_Feline` palette.

### Audio

- 1-bit speaker via miniaudio: rectangle-area integration of every
  `$C030` toggle → 4× oversample → 64-tap windowed sinc → DC blocker.
- **Cassette deck** (`$C020` / `$C060`) — Play / Record / Rewind, WAV /
  MP3 / OGG / FLAC / `.aci` input.
- **Mockingboard** — dual 6522 VIA + dual AY-3-8910 PSG (slot 4 by
  convention). Full T1 + T2 timers, envelope shapes 0-15, 17-bit noise
  LFSR.
- **Floppy mechanical sounds** — head-step click, motor spin-up / down,
  insert / eject click. WAV samples vendored in `roms/floppy_samples/`
  (ported from MAME `floppy_sound_device`). 800 ms wall-clock hold-off
  on motor-off so disk-turbo doesn't silence the spin-up sample.

### Storage

- **Disk II** (slot 6), two drives, 35 tracks × 4 quarter-tracks (160
  WOZ slots). Boot with `PR#6` or the panel's "boot disk" button. Two
  read paths share the data register: bit-level LSS (needs
  `roms/diskii_p6.rom`, required for `.woz` and cycle-accurate
  protections) or a legacy 32-cycle gate fallback.
- **ProDOS HardDisk** (slot 5) — 32 MB `.hdv` / `.2mg` mount, plus a
  synthetic read-only volume built on the fly from any host folder
  (`prodos_disk/` shows up as `/HOST/`).

### Peripherals (8-slot bus)

- **Super Serial Card** (slot 2) — 6551 ACIA, full slot-IRQ wiring, echo
  mode, software baud-rate divider, DTR side effects, 8-bit clean TX
  (XMODEM / Kermit / ADTPro). TCP listener on `127.0.0.1:6502` with
  telnet IAC stripping.
- **ProDOS Clock card** (slot 4) — ThunderClock+-compatible RTC,
  bit-bang uPD1990AC at `$C0C0`. TIME_READ + TIME_SET both wired.
- **Mouse Interface** — Apple Mouse Card. M68705P3 MCU + MC6821 PIA +
  bank-switched 2 KB EPROM. Host mouse position + buttons forwarded
  through the screen's bounding box.
- **Joystick** — GLFW host pad → paddles 0/1 + PB0/1/2, hot-plug,
  autobinds the first present pad.

### Tooling

- **CLI** — `--preset`, `--speed`, `--cpu-max`, `--tape`,
  `--load addr:file`, `--run`, `--paste`, `--step`,
  `--play / --rec / --rewind`, `--snapshot-save / --snapshot-load`.
- **Snapshot** — `POM2SNAP` save / load (CPU + RAM + soft switches;
  Disk II + peripheral state intentionally excluded).
- **AI control HTTP server** — `127.0.0.1:6503`, HTTP/1.1, drives the
  emulator from an external agent (snapshots, disk operations, paste,
  step, screen capture). Contract in `AiControlServer.h`.
- **Memory viewer** — region-coloured 64 KB hex + ASCII, change-flash,
  undo / redo, hex / ASCII search, bookmarks, 6502 disassembly toggle.
- **Threading** — UI 60 Hz, CPU pinned to 1.0227 MHz nominal (uncapped
  MAX available), single `stateMutex` between worker and renderer.

## Disk images

Drop floppy images into `disks/`, hard-disk images into `hdv/`. Mount
via `Hardware → Insert disk image` (slot 6) or `Hardware → ProDOS HardDisk (slot 5)`. Supported formats:

- `.dsk` / `.do` — 143 360 B, DOS 3.3 sector order
- `.po` — 143 360 B, ProDOS sector order
- `.nib` — 232 960 B (35×6656) or 223 440 B (35×6384, "CNib2")
- `.2mg` / `.2img` — 2IMG envelope (Asimov interchange format); volume
  number, WP flag, comment + creator chunks preserved on save
- `.woz` — WOZ1 / WOZ2 (Applesauce), full quarter-track decoding with
  CRC32 validation; unlocks Spiradisc / RWTS18 / Locksmith Fast Copy

Format detection is content-driven (a `.po` that's actually DOS-skewed
is recognised via vol-directory sniff); unrecognised files surface a
red error line under the slot. MacBinary 128-byte wrappers are stripped
transparently. Images are read-only by default; toggle write-back in
the Disk II panel to persist writes. WOZ `INFO.write_protected` and the
2IMG WP flag are always honoured.

## Keyboard


| Host        | Apple II                                 |
| ------------- | ------------------------------------------ |
| Enter       | Return                                   |
| Backspace   | ←                                       |
| Left arrow  | ←                                       |
| Right arrow | →                                       |
| Up arrow    | ↑                                       |
| Down arrow  | ↓                                       |
| Esc         | ESC                                      |
| Ctrl-A..Z   | $01..$1A                                 |
| F9          | Screenshot (`screenshot_NNN.ppm` in CWD) |
| F11         | Reset / Ctrl-Reset (soft)                |
| F12         | Hard reset / power-cycle                 |

Both upper and lower case letters pass straight through — POM2 doesn't
force-uppercase what you type.

## Joystick

Plug a USB pad before launch or hot-plug it (POM2 polls all 16 GLFW
slots each frame). The first present pad auto-binds; `Hardware → Joystick` opens a panel to pick a different one, set deadzone, and
invert axes.


| Host     | Apple II                    |
| ---------- | ----------------------------- |
| Axis X   | PADL(0) /`$C064`            |
| Axis Y   | PADL(1) /`$C065`            |
| Button 0 | PB0 /`$C061` (open-apple)   |
| Button 1 | PB1 /`$C062` (closed-apple) |
| Button 2 | PB2 /`$C063`                |

PADL(2) and PADL(3) read centred (127).

## Slot configuration

Cards are wired through `SlotBus`. Eight available card types: `(empty)`,
`Disk II`, `ProDOS HDV`, `Super Serial`, `Clock (ProDOS)`, `Le Chat Mauve`, `Mouse Interface`, `Mockingboard A/C`. Each type may appear in
at most one slot. Default layout:


| Slot | Card                                                     |
| ------ | ---------------------------------------------------------- |
| 1    | (free — printer card hook reserved)                     |
| 2    | Super Serial Card (TCP`127.0.0.1:6502`)                  |
| 3    | (IIe internal 80-column firmware when`SLOTC3ROM=off`)    |
| 4    | ProDOS Clock card — or Mockingboard, or Mouse Interface |
| 5    | ProDOS HardDisk (HDV + host folder)                      |
| 6    | Disk II (auto-plugged if`roms/disk2.rom` is present)     |
| 7    | Le Chat Mauve RGB monitor                                |

Open `Hardware → Slot configuration` to swap cards in or out. The chosen
layout, individual card volume / mute / port settings, and TCP listener
state all persist across runs via `settings.json`.

## UI panels

- **Apple II Screen** — 280×192 (HGR) or 560×192 (DHGR / 80-col) RGBA.
- **Emulation** — registers, cycle counter, speed (1× / 2× / MAX), audio.
- **Cassette deck** — procedural 378×404 deck (Font Awesome icons).
- **Disk II (slot 6)** — motor LEDs, current track, Insert / Eject,
  write-back toggle.
- **ProDOS HDV (slot 5)** — image picker, Library tab with the
  `[host folder]` synthetic entry.
- **Super Serial (slot 2)** — start / stop TCP listener, change port.
- **Le Chat Mauve** — FIFO state, manual mode override, reset.
- **Mockingboard** — per-VIA register dump, AY state, volume.
- **Joystick** — host-pad picker, live axis / button readout.
- **Memory viewer / Memory Map** — `Debug` menu; three map layouts
  (Bar / Bar-H / Grid) for the 64 KB at a glance.

## Licence

GPL-3.0.
