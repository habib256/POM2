# POM2 — Apple II Emulator

POM2 is a small Apple II / II+ / IIe emulator built around a Dear ImGui
shell: MOS 6502 / 65C02 CPU, 48 KB / 128 KB RAM, soft-switch I/O, full
text / 80-column / lo-res / hi-res / DHGR video, 1-bit speaker audio,
cassette deck, joystick, and an 8-slot peripheral bus carrying Disk II,
Super Serial, ProDOS clock, ProDOS HDV, Mockingboard, and Le Chat Mauve
RGB cards.

## Status (v0.2)

Working:

- **CPU** — MOS 6502 (Klaus Dormann functional-test clean, IRQ / NMI / BRK,
  BCD) plus the common 65C02 / Rockwell additions (STZ, BRA, INA/DEA,
  PHX/PHY/PLX/PLY, BIT #imm, TSB/TRB, JMP (abs,X), (zp) indirect mode,
  RMB/SMB/BBR/BBS, WAI/STP), so ProDOS hard-disk software targeting
  IIe Enhanced / IIc runs without hanging on the first CMOS-only opcode.
- **Memory** — 48 KB main + 16 KB ROM (`$C100-$FFFF`) for II/II+; or
  128 KB (main + auxiliary 64 KB bank) for IIe with all paging soft
  switches (80STORE / RAMRD / RAMWRT / INTCXROM / ALTZP / SLOTC3ROM /
  80COL / ALTCHAR), status reads at `$C013-$C018` / `$C01E` / `$C01F`,
  and the internal $C100-$CFFF I/O ROM. Auto-detected from
  `roms/apple2e.rom` presence.
- **Language Card** — 16 KB RAM bank-switched behind `$D000-$FFFF`,
  `$C080-$C08F` (read/write enable, BSR, prewrite latch), bank 1 / bank 2
  on `$D000-$DFFF`, separate aux LC banks under ALTZP.
- **Soft switches** `$C000-$C07F` — keyboard + strobe, speaker,
  cassette in/out, display modes, paddles (RC discharge), push-buttons,
  VBL `$C019` (scanline-accurate, with IIe IRQ mask via `$C05A`/`$C05B`).
- **Display** — text 40×24 with normal / inverse / flashing attribute
  (2 Hz, MAME `frame_number() & 0x10` parity); lo-res 40×48 (16 colours,
  benrg NTSC palette ported verbatim from MAME `apple2video.cpp`);
  hi-res 280×192 with NTSC artifact LUT (`artifact_color_lut[0]`,
  7-bit sliding window — also verbatim from MAME); monochrome variants
  White / Green (P31) / Amber with phosphor afterglow. IIe adds
  80-column text and DHGR 560×192 with three color paths (composite
  NTSC, Video-7-style RGB-card 4-dot block decode, and monochrome).
- **Character ROM** — 2 KB II/II+ or 4 KB IIe accepted; mousetext bank
  in IIe ALTCHAR mode; built-in 5×7 fallback if no ROM provided.
- **Mixed mode** — text rows 20-23 over hi-res / lo-res / DHGR;
  Page 1 / Page 2.
- **Le Chat Mauve / Féline** RGB card (slot 7) — direct 6-colour HGR
  decode with no inter-byte fringing (MSB selects violet/green vs.
  blue/orange bank), AN3+80COL FIFO for mode select, four modes
  (BW560 / Mixed / Chunky / COL140). Coexists with the NTSC / mono
  pipelines.
- **Audio** — 1-bit speaker through miniaudio with sub-instruction
  event timestamps and 1-pole low-pass; cassette mixed in alongside.
  Volume / mute UI.
- **Cassette** — `$C020` write-toggle and `$C060` comparator wired to
  a procedural cassette deck (Play / Record / Rewind, Font Awesome icons,
  WAV / MP3 / OGG / FLAC / `.aci` input).
- **Disk II controller** in slot 6 — boot via `PR#6`, two drives, 35
  half-tracks. Loaders: `.dsk` / `.do` (DOS 3.3 logical sector order),
  `.po` (ProDOS), `.nib` (raw nibbles), `.woz` (WOZ1 + WOZ2, read-only —
  unlocks copy-protected disks that idealised GCR can't carry). LSS-level
  bit-cell read path (verbatim port of MAME `wozfdc.cpp` + P6 PROM)
  plus a legacy 32-cycle gate. Write-back to `.dsk`/`.do`/`.po`/`.nib`
  is opt-in (default off — source files are never touched unless you
  enable it).
- **ProDOS Clock card** (slot 4) — ThunderClock+-compatible RTC,
  bit-bang uPD1990AC at `$C0C0`. ProDOS file timestamps work out of the
  box; DOS 3.3 disks ignore the card.
- **ProDOS HardDisk** (slot 5) — 32 MB ProDOS HDV image mount, plus a
  synthetic read-only volume built on the fly from any host folder
  (`prodos_disk/` by default — appears as `/HOST/` once ProDOS is up).
- **Super Serial Card** (slot 2) — minimal 6551 ACIA + TCP listener on
  `127.0.0.1:6502` with telnet IAC stripping, so a host terminal can
  speak serial to the guest.
- **Mockingboard** — dual 6522 VIA + dual AY-3-8910 PSG, user-pluggable
  into any free slot (slot 4 by convention). Volume + mute persist.
- **Joystick** — GLFW host pad → paddles 0/1 + buttons PB0/PB1/PB2
  (`$C061-$C063`, `$C064-$C067`), hot-plug-friendly, autobinds first
  present pad.
- **8-slot expansion bus** — `$C080-$C0FF` device-select, `$C100-$C7FF`
  slot ROM, `$C800-$CFFF` shared expansion ROM with `$CFFF` disable.
  Slot configuration UI (Hardware → Slot configuration) lets you assign
  cards to slots, with state persisted across runs.
- **Snapshot** — save / load (`POM2SNAP` magic, named 8-byte sections,
  CPU + RAM + soft-switches; Disk II + peripheral state intentionally
  excluded).
- **CLI** — `--preset ii|ii+`, `--speed`, `--cpu-max`, `--tape`,
  `--load addr:file`, `--run`, `--paste`, `--step`,
  `--play/--rec/--rewind`, `--snapshot-save/load`.
- **Threading** — UI 60 Hz, CPU pinned to 1.0227 MHz nominal (uncapped
  MAX available), single `stateMutex` between worker and renderer.
- **Memory viewer / hex editor** — region-coloured 64 KB grid, ASCII
  column, change-flash, undo/redo, hex / ASCII search, bookmarks,
  6502 disassembly toggle. Three memory-map widgets (vertical bar,
  horizontal bar, grid).

Not yet:

- Integer BASIC preset (only Applesoft / Autostart is exercised today)
- Disk II quarter-tracks — only whole tracks read from WOZ TMAP, which
  blocks Spiradisc / RWTS-18 / Locksmith-class copy protection
- WOZ FLUX chunk parsing (post-WOZ2 v2.1 raw flux images)
- WOZ write-back (always read-only, even when write-back is enabled
  for the legacy formats)
- VIA T2 timer on Mockingboard (blocks Ultima IV speech driver)
- Annunciators `$C058-$C05F`
- Mouse Card, RAMWorks, CP/M Z80 SoftCard
- DHGR per-scanline mode switching (the bottom-of-mixed region uses a
  static 4-line region)

See `TODO.md` for the full backlog including the MAME-divergence audit
(§14).

## Build

```bash
./setup_imgui.sh   # one-time: installs deps + clones Dear ImGui
cd build && cmake .. && make -j
./run_emulator.sh  # equivalent to ./build/POM2 launched from the repo root
```

The setup script handles macOS (Homebrew), Debian / Ubuntu (apt),
Fedora (dnf), and Arch (pacman). Windows: install GLFW via vcpkg and
run CMake manually.

## ROM placement

Drop your own Apple II / II+ / IIe ROMs at:

- `roms/apple2e.rom` — **takes precedence**. 16 KB or 32 KB IIe main ROM
  (`$C000-$FFFF` plus optional video / character data in the lower
  16 KB). When present POM2 boots as a IIe (128 KB RAM via auxiliary
  bank, IIe paging soft switches, 80-column text, DHGR). The status
  pill in the menu bar reads `IIe (128K): roms/apple2e.rom`.
- `roms/apple2.rom` — 12 KB Autostart Monitor + Applesoft
  (`$D000-$FFFF`) or 16 KB II+ image (`$C000-$FFFF`); the I/O page is
  preserved. Used when no IIe ROM is present.
- `roms/apple2_char.rom` — 2 KB (II/II+) or 4 KB (IIe with mousetext)
  character ROM (optional; POM2 falls back to a built-in 5×7 ASCII font).
- `roms/disk2.rom` — 256 B P5A boot PROM (AppleWin's copy works). When
  present POM2 auto-plugs the Disk II card in slot 6 at startup.
- `roms/diskii_p6.rom` — 256 B sequencer PROM. When present, the LSS
  bit-level path activates (required for `.woz` decode and cycle-accurate
  protection); otherwise the legacy 32-cycle nibble gate is used.

Without any of the above the CPU still runs but executes whatever stub
is left at `$F800` — the screen will stay dark. POM2 prints the ROM
status in the menu bar (`IIe (128K): ...`, `loaded: ...`, or `NO ROM`).
To switch between IIe and II+ at runtime, move `roms/apple2e.rom` aside
(or rename it) and restart.

## Disk images

Drop images into `disks/`:
- `.dsk` / `.do` — 143 360 B, DOS 3.3 logical sector order
- `.po` — 143 360 B, ProDOS sector order (boots ProDOS, A2DeskTop, …)
- `.nib` — 232 960 B, raw nibble image
- `.woz` — WOZ1 or WOZ2 (Applesauce); read-only, unlocks stock
  copy-protected disks

Use **Hardware → Insert disk image** to mount, then boot with `PR#6`,
or click the "boot disk" button in the Disk II panel. The image file
is read-only by default; toggle write-back in the panel to persist
writes back to `.dsk`/`.do`/`.po`/`.nib` (WOZ stays read-only).

For ProDOS hard-disk volumes drop `.hdv` / `.2mg` images into `hdv/`
and mount via **Hardware → ProDOS HardDisk (slot 5)**. The Library tab
also lists a synthetic `[host folder] prodos_disk/` entry that builds a
read-only volume on the fly from the contents of `prodos_disk/` — once
ProDOS is up it appears as `/HOST/`.

## Keyboard

The host keyboard maps straight to the Apple II keyboard latch. Special
keys:

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
| F9          | Screenshot (`screenshot_NNN.ppm` in CWD) |
| F11         | Reset / Ctrl-Reset (soft) |
| F12         | Hard reset / power-cycle |

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

## Slot configuration

Cards are wired through `SlotBus`. The default layout matches the
classic Apple II/IIe convention:

| Slot | Card                                |
|------|-------------------------------------|
| 1    | (free — printer card hook reserved) |
| 2    | Super Serial Card (TCP `127.0.0.1:6502`) |
| 3    | (IIe internal 80-column firmware when `SLOTC3ROM=off`) |
| 4    | ProDOS Clock card (ThunderClock+) — or Mockingboard, user choice |
| 5    | ProDOS HardDisk (HDV + host folder)  |
| 6    | Disk II (auto-plugged if `roms/disk2.rom` is present) |
| 7    | Le Chat Mauve RGB monitor             |

Open **Hardware → Slot configuration** to swap cards in or out. The
chosen layout, individual card volume / mute / port settings, and
TCP listener state persist across runs via `settings.json`.

## Layout

ImGui main window with one menu bar and several panels:

- **Apple II Screen** — 280×192 (HGR) or 560×192 (DHGR / 80-col) RGBA
  texture rendered each frame.
- **Emulation** — CPU registers, cycle counter, speed selector
  (1×, 2×, MAX), ROM status, audio volume / mute.
- **Cassette deck** — procedural 378×404 deck (Hardware → Cassette deck).
- **Disk II (slot 6)** — PROM/motor LEDs, current track, Insert / Eject,
  write-back toggle.
- **ProDOS HDV (slot 5)** — image picker, library tab with the
  `[host folder]` synth entry.
- **Super Serial (slot 2)** — start/stop the TCP listener, change port.
- **Le Chat Mauve (slot 7)** — FIFO state, manual mode override, reset.
- **Mockingboard** — per-VIA register dump, AY register state, volume.
- **Joystick** — host-pad picker, live axis / button readout.
- **Memory viewer** — togglable via Debug → Memory viewer. Region-coloured
  hex grid + ASCII column + 6502 disassembly toggle, search / edit /
  bookmarks / undo-redo.
- **Memory Map widgets** — three layouts (Bar, Bar Horizontal, Grid)
  for visualising the 64 KB layout at a glance.

## Licence

GPL-3.0.
