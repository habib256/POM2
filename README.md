# POM2 — Apple II Emulator

POM2 is a small Apple II / II+ / IIe / IIc / IIc+ emulator built around
a Dear ImGui shell: MOS 6502 / 65C02 CPU, 48 KB / 128 KB RAM, soft-switch
I/O, full text / 80-column / lo-res / hi-res / DHGR video, 1-bit speaker
audio, cassette deck, joystick, mouse, and an 8-slot peripheral bus
carrying Disk II, Super Serial, ProDOS clock, ProDOS HDV, Mockingboard,
Mouse Interface, and Le Chat Mauve RGB cards.

## Status

Working:

- **CPU** — MOS 6502 (Klaus Dormann functional-test clean, IRQ / NMI /
  BRK, BCD) plus the common 65C02 / Rockwell additions (STZ, BRA,
  INA/DEA, PHX/PHY/PLX/PLY, BIT #imm, TSB/TRB, JMP (abs,X), (zp)
  indirect mode, RMB/SMB/BBR/BBS, WAI/STP). MAME-parity on D-flag clear
  at CMOS interrupt entry, PLP/RTI U=B=1 force, JMP (abs) page-wrap
  bug NMOS-only, RMW `$C0xx` triple-dispatch through `softSwitchAccess`,
  WAI fall-through on IRQ-with-I=1, NMI > IRQ priority, and the
  CMOS-specific cycle counts (JMP indirect = 6, BIT #imm = 2, RMB/SMB
  = 5, BBR/BBS = 5, BRK = 7, RTI = 6). Klaus 65C02-extended test
  passes at `$24F1` (22 M cycles). NMOS / 65C02 mode selectable via
  `Machine → CPU` (auto-follows the active system profile).
- **Five system profiles** — Apple ][ Original (1977, NMOS, Integer
  BASIC ROM), Apple ][+ (1979, NMOS, Applesoft), Apple //e Enhanced
  (1985, 65C02, 128 KB), Apple //c (1984, 65C02), Apple //c Plus
  (1988, 65C02). Each profile has its own ROM probe order, CPU mode,
  and IIe-paging flag; switching profiles is a full cold reset that
  re-plugs slot cards and re-mounts the previously inserted disks.
  Mounted media persists across profile switches.
- **Memory** — 48 KB main + 16 KB ROM (`$C100-$FFFF`) for II/II+; or
  128 KB (main + auxiliary 64 KB bank) for IIe/IIc with all paging soft
  switches (80STORE / RAMRD / RAMWRT / INTCXROM / ALTZP / SLOTC3ROM /
  80COL / ALTCHAR), status reads at `$C013-$C018` / `$C01E` / `$C01F`
  with the last keyboard character OR'd into the low 7 bits, and the
  internal $C100-$CFFF I/O ROM.
- **Language Card** — 16 KB RAM bank-switched behind `$D000-$FFFF`,
  `$C080-$C08F` (read/write enable, BSR, prewrite latch — armed only by
  reads, cleared by writes per Sather 5-12), bank 1 / bank 2 on
  `$D000-$DFFF`, separate aux LC banks under ALTZP.
- **Soft switches** `$C000-$C07F` — keyboard + strobe, speaker, cassette
  in/out, display modes, paddles (RC discharge ~11 cycles/step per MAME
  `apple2.cpp:247-248`), push-buttons with Open Apple / Solid Apple /
  Shift-key mod OR'd on `$C061-$C063` in IIe mode, annunciators
  `$C058-$C05F` (AN0/AN1/AN2 state tracked, AN3 doubles as DHGR enable
  + Le Chat Mauve FIFO clock), VBL `$C019` (scanline-accurate, with IIe
  IRQ mask via `$C05A`/`$C05B`), `$C040` strobe swallowed,
  `$C068-$C07F` mirrors + floating-bus on `$C061-$C067` low 7 bits.
- **Display** — text 40×24 with normal / inverse / flashing attribute
  (2 Hz, MAME `frame_number() & 0x10` parity); lo-res 40×48 (16 colours,
  benrg NTSC palette ported verbatim from MAME `apple2video.cpp`);
  hi-res 280×192 with three composite color modes (canonical artifact
  LUT, medium-color biased LUT, and 4-bit square filter — MAME's
  `composite_color_mode 0/1/2`); monochrome variants White / Green (P31)
  / Amber with phosphor afterglow. IIe adds 80-column text and DHGR
  560×192 with all three color paths (composite NTSC, Video-7-style
  RGB-card 4-dot block decode, and monochrome). The HGR / lo-res / text
  scanners honour `80STORE+PAGE2(+HIRES)` aux-RAM routing.
- **Character ROM** — 2 KB II/II+ or 4 KB IIe accepted; mousetext bank
  in IIe ALTCHAR mode; built-in 5×7 fallback if no ROM provided.
- **Mixed mode** — text rows 20-23 over hi-res / lo-res / DHGR;
  Page 1 / Page 2.
- **Le Chat Mauve / Féline** RGB card — direct 6-colour HGR decode
  with no inter-byte fringing (MSB selects violet/green vs.
  blue/orange bank), AN3+80COL FIFO for mode select, four modes
  (BW560 / Mixed / Chunky / COL140). Lo-res + HGR + DHGR palettes use
  the AppleWin `PaletteRGB_Feline` capture (two distinct grays
  preserved). Coexists with the NTSC / mono pipelines.
- **Audio** — 1-bit speaker through miniaudio with MAME-grade
  reconstruction: rectangle-area integration of every $C030 toggle
  into 4× oversampled intermediate samples, 64-tap windowed sinc
  convolution (cutoff ≈ sr/4), and a 0.995-pole DC blocker. Verbatim
  port of MAME `spkrdev.cpp:74-327`. Cassette mixed in alongside.
  Volume / mute UI.
- **Cassette** — `$C020` write-toggle and `$C060` comparator wired to
  a procedural cassette deck (Play / Record / Rewind, Font Awesome
  icons, WAV / MP3 / OGG / FLAC / `.aci` input).
- **Disk II controller** in slot 6 — boot via `PR#6`, two drives, 35
  whole tracks × 4 quarter-tracks (160 quarter-track slots on WOZ).
  Loaders: `.dsk` / `.do` (DOS 3.3 sector order), `.po` (ProDOS),
  `.nib` (raw nibbles, 35×6656 or the rarer CNib2 35×6384), `.2mg` /
  `.2img` (envelope around any of the above), `.woz` (WOZ1 + WOZ2
  with full quarter-track decode + CRC32 validation — unlocks
  Spiradisc / RWTS18 / Locksmith Fast Copy). MacBinary 128-byte
  wrappers are stripped transparently. Format detection is
  content-driven: a `.po` that's actually DOS-skewed or a `.dsk`
  containing ProDOS data is recognised via vol-directory sniff;
  unrecognised files surface a clear error message under the slot
  rather than silently failing. LSS-level bit-cell read path
  (verbatim port of MAME `wozfdc.cpp` + P6 PROM) plus a legacy
  32-cycle gate. Write-back to `.dsk`/`.do`/`.po`/`.nib`/`.2mg` AND
  `.woz` — opt-in (default off). 2IMG envelopes (header + comment /
  creator chunks) are preserved byte-for-byte across round-trips.
- **ProDOS Clock card** (slot 4) — ThunderClock+-compatible RTC,
  bit-bang uPD1990AC at `$C0C0`. TIME_READ and TIME_SET both wired:
  ProDOS file timestamps + software clock-set utilities both work.
  DOS 3.3 disks ignore the card.
- **ProDOS HardDisk** (slot 5) — 32 MB ProDOS HDV image mount, plus a
  synthetic read-only volume built on the fly from any host folder
  (`prodos_disk/` by default — appears as `/HOST/` once ProDOS is up).
- **Super Serial Card** (slot 2) — 6551 ACIA model with full slot-IRQ
  wiring (RDRF / TDRE / DCD / DSR transitions, gated by command-register
  RX-IRQ-enable bit AND DTR), echo mode (REM, command bit 4),
  software baud-rate divider (MAME `mos6551.cpp` table — 1.8432 MHz
  xtal ÷ divider ÷ 16 ÷ 10), DTR side effects (drops IRQs, flushes
  TX, rejects further writes), overrun tracking, A0-A1 mirror across
  `$C0nC-$C0nF`, DIP-switch readback at `$C0n0-$C0n7`, 8-bit clean
  TX (XMODEM / Kermit / ADTPro friendly). TCP listener on
  `127.0.0.1:6502` with telnet IAC stripping.
- **Mockingboard** — dual 6522 VIA + dual AY-3-8910 PSG,
  user-pluggable into any free slot (slot 4 by convention). Full T1
  + T2 timers (Ultima IV speech driver works), MAME-verbatim 4-flag
  envelope state machine (shapes 0-15 including the vibrato-friendly
  10 / 12 / 14), 17-bit noise LFSR. Volume + mute persist.
- **Mouse Interface** — Apple Mouse Card, slot 4 by convention.
  Verbatim port of MAME `bus/a2bus/mouse.cpp`: M68705P3 microcontroller
  (Apple 341-0269) + MC6821 PIA + 2 KB bank-switched EPROM (Apple
  341-0270-c). Slot ROM is bank-selected via PIA Port B; PB6 of the
  MCU asserts the slot IRQ. Host mouse position + button forwarded
  through the Apple II screen's bounding box. Requires both ROMs
  (`roms/mouse_341-0269.bin` and `roms/mouse_341-0270-c.bin`); the
  slot-config UI greys the entry out when they're missing.
- **Joystick** — GLFW host pad → paddles 0/1 + buttons PB0/PB1/PB2
  (`$C061-$C063`, `$C064-$C067`), hot-plug-friendly, autobinds first
  present pad.
- **8-slot expansion bus** — `$C080-$C0FF` device-select, `$C100-$C7FF`
  slot ROM, `$C800-$CFFF` shared expansion ROM with `$CFFF` disable.
  Slot configuration UI (Hardware → Slot configuration) lets you
  assign cards to slots, with state persisted across runs.
- **Snapshot** — save / load (`POM2SNAP` magic, named 8-byte sections,
  CPU + RAM + soft-switches; Disk II + peripheral state intentionally
  excluded).
- **CLI** — `--preset <ii|ii+|iie|iic|iic+>`, `--speed`, `--cpu-max`,
  `--tape`, `--load addr:file`, `--run`, `--paste`, `--step`,
  `--play/--rec/--rewind`, `--snapshot-save/load`.
- **Threading** — UI 60 Hz, CPU pinned to 1.0227 MHz nominal (uncapped
  MAX available), single `stateMutex` between worker and renderer.
- **Memory viewer / hex editor** — region-coloured 64 KB grid, ASCII
  column, change-flash, undo/redo, hex / ASCII search, bookmarks,
  6502 disassembly toggle. Three memory-map widgets (vertical bar,
  horizontal bar, grid).

Not yet (see `TODO.md` for the full backlog):

- ClockCard TP-pin tick rates (64/256/2048/4096 Hz + interval timers)
  — needs a slot-bus IRQ line first
- WOZ2 `optimal_bit_timing` honoured, WOZ1 splice point
- PADL(2)/PADL(3) host binding (currently centred at 127)
- Mouse → paddles 0/1 alternative mapping
- Eve Color text mode (`$C0B9`), Video-7 AppleColor RGB
- RAMWorks (IIe-only, ProDOS 3+), CP/M Z80 SoftCard
- DHGR per-scanline mode switching (bottom-of-mixed region uses a
  static 4-line region)
- Integer BASIC ROM (the `Apple ][ Original` profile probes
  `roms/apple2o.rom` — works if you provide one; Applesoft is what
  most users actually exercise)

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

Drop your own Apple II / II+ / IIe / IIc / IIc+ ROMs into `roms/`.
ROMs are probed per the active system profile (selectable via
`Presets` menu or `--preset` CLI flag):

| Profile          | Main ROM probes                                       |
|------------------|-------------------------------------------------------|
| Apple ][ (1977)  | `apple2o.rom`, `apple2.rom`                           |
| Apple ][+ (1979) | `apple2p.rom`, `apple2.rom`                           |
| Apple //e (1985) | `apple2e.rom`                                         |
| Apple //c (1984) | `apple2c-32Kv0.rom`, `apple2c-16K.rom`                |
| Apple //c+ (1988)| `apple2cp.rom`, `apple2c-plus.rom`, `apple2c-32Kv0.rom` |

First existing file wins. Sizes accepted:
- 12 KB Autostart Monitor + Applesoft (`$D000-$FFFF`) — II/II+
- 16 KB (`$C000-$FFFF`) — II+, IIe, IIc
- 32 KB "system + video" combined dump — IIe / IIc (the lower 16 KB
  carries video/char data; loaded through the char-ROM path)

Other ROMs:

- `roms/apple2_char.rom` — 2 KB (II/II+) or 4 KB (IIe with mousetext)
  character ROM (optional; POM2 falls back to a built-in 5×7 ASCII
  font).
- `roms/disk2.rom` — 256 B P5A boot PROM (AppleWin's copy works).
  When present POM2 auto-plugs the Disk II card in slot 6 at startup.
- `roms/diskii_p6.rom` — 256 B sequencer PROM. When present, the LSS
  bit-level path activates (required for `.woz` decode and
  cycle-accurate protection); otherwise the legacy 32-cycle nibble
  gate is used.
- `roms/mouse_341-0270-c.bin` (2 KB) and `roms/mouse_341-0269.bin`
  (2 KB) — Apple Mouse Card slot EPROM + MCU mask ROM. Both required
  to enable the `Mouse Interface` slot entry.

The status pill in the menu bar shows the resolved profile + ROM.
Without any main ROM the CPU still runs but executes whatever stub is
left at `$F800`.

## Disk images

Drop images into `disks/`. Supported formats:
- `.dsk` / `.do` — 143 360 B, DOS 3.3 sector order
- `.po` — 143 360 B, ProDOS sector order (boots ProDOS, A2DeskTop, …)
- `.nib` — 232 960 B (35×6656) or 223 440 B (35×6384, "CNib2") raw
  nibble stream
- `.2mg` / `.2img` — 2IMG envelope around any DOS / ProDOS / NIB
  payload (the dominant interchange format on Asimov and similar
  archives). Volume number and write-protect flag from the header
  are honoured; comment / creator chunks are preserved on save.
- `.woz` — WOZ1 or WOZ2 (Applesauce), full quarter-track decoding +
  CRC32 validation on load.

Format detection is content-driven, so a `.po` that's actually
DOS-skewed (older cc65 `acmd --d33` output, for example) is still
recognised correctly; if it really can't be identified the Disk II
panel shows a red error line under the slot.

Use **Hardware → Insert disk image** to mount, then boot with `PR#6`
or click the "boot disk" button in the Disk II panel. The image file
is read-only by default; toggle write-back in the panel to persist
writes (`.dsk` / `.do` / `.po` / `.nib` / `.2mg` / `.woz`). WOZ
`INFO.write_protected` and the 2IMG WP flag are always honoured.
MacBinary 128-byte wrappers (legacy Mac downloads) are stripped
transparently.

For ProDOS hard-disk volumes drop `.hdv` / `.2mg` images into `hdv/`
and mount via **Hardware → ProDOS HardDisk (slot 5)**. The Library
tab also lists a synthetic `[host folder] prodos_disk/` entry that
builds a read-only volume on the fly from the contents of
`prodos_disk/` — once ProDOS is up it appears as `/HOST/`.

## Keyboard

The host keyboard maps straight to the Apple II keyboard latch.
Special keys:

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

Both upper and lower case letters pass straight through — POM2
doesn't force-uppercase what you type.

## Joystick

Plug a USB pad before launch (or hot-plug — POM2 polls all 16 GLFW
slots each frame). The first present pad auto-binds;
**Hardware → Joystick** opens a panel to pick a different one, set
deadzone, and invert axes. Mapping:

| Host        | Apple II        |
|-------------|-----------------|
| Axis X      | PADL(0) / `$C064` |
| Axis Y      | PADL(1) / `$C065` |
| Button 0    | PB0 / `$C061` (open-apple) |
| Button 1    | PB1 / `$C062` (closed-apple) |
| Button 2    | PB2 / `$C063` |

PADL(2) and PADL(3) read centred (127).

## Slot configuration

Cards are wired through `SlotBus`. Eight available card types:
`(empty)`, `Disk II`, `ProDOS HDV`, `Super Serial`, `Clock (ProDOS)`,
`Le Chat Mauve`, `Mouse Interface`, `Mockingboard A/C`. Each type may
appear in at most one slot. Default layout matches the classic
Apple II/IIe convention:

| Slot | Card                                                   |
|------|--------------------------------------------------------|
| 1    | (free — printer card hook reserved)                    |
| 2    | Super Serial Card (TCP `127.0.0.1:6502`)               |
| 3    | (IIe internal 80-column firmware when `SLOTC3ROM=off`) |
| 4    | ProDOS Clock card — or Mockingboard, or Mouse Interface |
| 5    | ProDOS HardDisk (HDV + host folder)                    |
| 6    | Disk II (auto-plugged if `roms/disk2.rom` is present)  |
| 7    | Le Chat Mauve RGB monitor                              |

Open **Hardware → Slot configuration** to swap cards in or out. The
chosen layout, individual card volume / mute / port settings, and
TCP listener state persist across runs via `settings.json`.

## Layout

ImGui main window with one menu bar and several panels:

- **Apple II Screen** — 280×192 (HGR) or 560×192 (DHGR / 80-col) RGBA
  texture rendered each frame.
- **Emulation** — CPU registers, cycle counter, speed selector
  (1×, 2×, MAX), ROM status, audio volume / mute.
- **Cassette deck** — procedural 378×404 deck
  (Hardware → Cassette deck).
- **Disk II (slot 6)** — PROM/motor LEDs, current track, Insert /
  Eject, write-back toggle.
- **ProDOS HDV (slot 5)** — image picker, library tab with the
  `[host folder]` synth entry.
- **Super Serial (slot 2)** — start/stop the TCP listener, change
  port.
- **Le Chat Mauve** — FIFO state, manual mode override, reset.
- **Mockingboard** — per-VIA register dump, AY register state, volume.
- **Joystick** — host-pad picker, live axis / button readout.
- **Memory viewer** — togglable via Debug → Memory viewer.
  Region-coloured hex grid + ASCII column + 6502 disassembly toggle,
  search / edit / bookmarks / undo-redo.
- **Memory Map widgets** — three layouts (Bar, Bar Horizontal, Grid)
  for visualising the 64 KB layout at a glance.

## Licence

GPL-3.0.
