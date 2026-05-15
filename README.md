# POM2 — Apple II Emulator

![Disk II closeup](pic/diskii_closeup.webp)

POM2 is an Apple II / II+ / IIe / IIc / IIc+ emulator built around Dear
ImGui — MOS 6502 / 65C02, 48 KB / 128 KB / up to 8 MB RAM, full text /
80-column / lo-res / hi-res / DHGR video, 1-bit speaker audio,
Mockingboard, cassette deck, joystick, mouse, and an 8-slot peripheral
bus (Disk II × multi-instance, ProDOS HDV, SmartPort 3.5", Super Serial,
ProDOS clock, Mouse, Le Chat Mauve RGB, Mockingboard A/C).

## Table of contents

- [Quick start](#quick-start)
- [System profiles](#system-profiles)
- [ROM placement](#rom-placement)
- [Features](#features)
- [Disk images](#disk-images)
- [Slot configuration](#slot-configuration)
- [Keyboard & joystick](#keyboard--joystick)
- [UI panels](#ui-panels)
- [CLI](#cli)
- [Licence](#licence)

## Quick start

```bash
./setup_imgui.sh   # one-time: installs deps + clones Dear ImGui
cd build && cmake .. && make -j
./run_emulator.sh  # = ./build/POM2 launched from the repo root
```

`setup_imgui.sh` covers macOS (Homebrew), Debian / Ubuntu (apt), Fedora
(dnf), and Arch (pacman). Windows: install GLFW via vcpkg and run CMake
manually. Drop ROMs into `roms/`, 5.25" images into `disks/`, 3.5"
images into `disks35/` (or `disks/`), hard-disk images into `hdv/`.

## System profiles

| Profile           | CPU   | iieMode | Main ROM probes                                         |
| ------------------- | ------- | --------- | --------------------------------------------------------- |
| Apple ][ (1977)   | NMOS  | off     | `apple2o.rom`, `apple2.rom`                             |
| Apple ][+ (1979)  | NMOS  | off     | `apple2p.rom`, `apple2.rom`                             |
| Apple //e (1985)  | 65C02 | on      | `apple2e.rom`                                           |
| Apple //c (1984)  | 65C02 | on      | `apple2c-32Kv0.rom`, `apple2c-16K.rom`                  |
| Apple //c+ (1988) | 65C02 | on      | `apple2cp.rom`, `apple2c-plus.rom`, `apple2c-32Kv0.rom` |

Pick the profile from the `Machine → Profile` menu (active row gets a
native ImGui checkmark) or `--preset <ii|ii+|iie|iic|iic+>` on the
command line. Switching profiles is a full cold reset that re-plugs
slot cards and re-mounts previously inserted disks. The CPU mode
follows the profile unless overridden in `Machine → CPU`.

## ROM placement

Drop your ROMs into `roms/` (first existing file in the probe order
wins). Main-ROM sizes accepted: 12 KB (`$D000-$FFFF`, II/II+), 16 KB
(`$C000-$FFFF`, II+/IIe/IIc), 20 KB ("system pack" with 4 KB filler),
32 KB combined "system + video" dump (IIe/IIc — lower 16 KB carries
char/video data on //e, two firmware banks side-by-side on //c/c+).

| File                        | Size | Role                                                              |
| ----------------------------- | ------ | ------------------------------------------------------------------- |
| `apple2e.rom`               | 16/32 KB | //e firmware (mainboard + optional charset block)               |
| `apple2cp.rom`              | 32 KB  | //c+ firmware (banks 0 + 1, ROMSWITCH `$C028` toggles)            |
| `apple2_char.rom`           | 2/4 KB | Character ROM (2 KB II/II+, 4 KB IIe enhanced with mousetext)     |
| `disk2.rom`                 | 256 B  | Disk II P5A boot PROM — required for the Disk II slot card        |
| `diskii_p6.rom`             | 256 B  | Disk II P6 LSS sequencer — required for `.woz` & cycle-protections|
| `mouse_341-0270-c.bin`      | 2 KB   | Mouse Card slot ROM                                               |
| `mouse_341-0269.bin`        | 2 KB   | Mouse Card 68705 MCU mask ROM                                     |
| `roms/floppy_samples/*.wav` | varies | MAME-vendored mechanical-sound samples (5.25" + 3.5")             |

The status pill in the menu bar shows the resolved profile + ROM.

## Features

### CPU & memory

- **6502** (Klaus Dormann clean, IRQ / NMI / BRK, BCD) + common **65C02 /
  Rockwell** extensions (STZ, BRA, INA/DEA, PHX/PLY, BIT #imm, TSB/TRB,
  JMP (abs,X), zp-indirect, RMB/SMB/BBR/BBS, WAI/STP). NMOS or 65C02
  selectable in `Machine → CPU`.
- 48 KB + 16 KB ROM for II/II+; 128 KB (main + aux 64 KB) for IIe/IIc
  with all paging soft switches (80STORE, RAMRD, RAMWRT, INTCXROM,
  ALTZP, SLOTC3ROM, 80COL, ALTCHAR) and the internal `$C100-$CFFF` ROM.
- **RamWorks III** aux-slot RAM expansion up to 8 MB (1 / 4 / 8 / 16 /
  48 / 128 × 64 KB banks). IIe only.
- Language Card 16 KB bank-switched at `$D000-$FFFF`; aux LC trio under
  ALTZP; sub-instruction-accurate soft-switch timing for cycle-precise
  copy protections.

### Display

- Text 40×24 (normal / inverse / 2 Hz flash) + lo-res 40×48 (16 NTSC
  colours).
- Hi-res 280×192 with three composite colour modes (artifact, medium,
  4-bit square filter) plus monochrome **White / Green / Amber** with
  phosphor afterglow.
- IIe: **80-column** text + **DHGR 560×192** (composite NTSC, Video-7
  RGB, monochrome). Mixed mode on Page 1 / Page 2.
- **Le Chat Mauve / Féline** RGB card — direct 6-colour HGR with no
  inter-byte fringing, AN3+80COL FIFO mode select, four modes (BW560 /
  Mixed / Chunky / COL140), AppleWin `PaletteRGB_Feline` palette.

### Audio

- 1-bit **speaker** via miniaudio: rectangle-area integration of every
  `$C030` toggle → 4× oversample → 64-tap windowed sinc → DC blocker.
- **Cassette deck** (`$C020` / `$C060`) — Play / Record / Rewind, WAV /
  MP3 / OGG / FLAC / `.aci` input.
- **Mockingboard** — dual 6522 VIA + dual AY-3-8910 PSG. Full T1 + T2
  timers, envelope shapes 0-15, 17-bit noise LFSR, IRQ-driven music
  drivers (Ultima IV, Nox Archaist, Skyfox, Total Replay).
- **Floppy mechanical sounds** — head-step click, motor spin-up / down,
  insert / eject click for **both** Disk II (5.25") and Sony 3.5" drives.
  Samples vendored in `roms/floppy_samples/`. Step cadence measured in
  emulated CPU cycles so the seek "brrrt" stays coherent at any disk-
  turbo multiplier.

### Storage

- **Disk II** (5.25") — **multi-instance**: pick `Disk II` in as many
  slots as you want (e.g. slot 6 + slot 4 = 4 drives). Each card opens
  its own panel titled `Disk II (slot N)`; persistence keys are
  per-slot (`disk_path_slotN`, `disk_writeback_slotN`). Two drives per
  card, 35 tracks × 4 quarter-tracks. Two read paths: bit-level LSS
  (needs `diskii_p6.rom`, required for `.woz`) or legacy 32-cycle gate.
- **ProDOS HDV** — pluggable in any slot (default 5). 32 MB `.hdv` /
  `.2mg` mount, plus a synthetic read-only volume built on the fly
  from any host folder (`prodos_disk/` shows up as `/HOST/`). Boot
  path follows the slot the card is actually plugged in (panel title,
  `Boot HDV (slot N)` menu, `bootHdvImage()` all use the live slot).
- **SmartPort 3.5"** — **two ways to get Sony 800 K disks**:
  - On **//c+** the on-board hub uses the full IWM + Sony GCR
    encoder/decoder stack (MAME-faithful). Flux-level write-back.
  - On **//e / II+ / II / //c** plug a **SmartPort 3.5" card**
    (Apple "Liron" / 670-0186, default slot 5) — block-level ProDOS
    driver dispatching the same two `Disk35Image` objects. 2 drives
    per card.
  Same Disk 3.5" panel for both; title reflects on-board vs slot N.
- **WOZ format**: WOZ1 + WOZ2 with **`optimal_bit_timing`** honoured
  (INFO+39 — disks mastered at non-standard bit-cell durations play
  back at their captured rate). CRC32 validated. Unlocks Spiradisc /
  RWTS18 / Locksmith Fast Copy.

### Peripherals (8-slot bus)

- **Super Serial Card** — 6551 ACIA, full slot-IRQ wiring, echo mode,
  software baud-rate divider, 8-bit clean TX (XMODEM / Kermit / ADTPro).
  TCP listener on `127.0.0.1:6502` with telnet IAC stripping.
- **ProDOS Clock card** — ThunderClock+-compatible RTC, bit-bang
  uPD1990AC at `$C0C0`. TIME_READ + TIME_SET both wired.
- **Mouse Interface** — Apple Mouse Card. M68705P3 MCU + MC6821 PIA +
  bank-switched 2 KB EPROM. Host mouse position + buttons forwarded
  through the screen's bounding box. *(See [Known limitations](#known-limitations)
  for absolute-position sync caveats with A2Desktop / MGTK.)*
- **Joystick** — GLFW host pad → paddles 0/1 + PB0/1/2, hot-plug,
  autobinds the first present pad.

### Tooling

- **AI control HTTP server** — `127.0.0.1:6503`, HTTP/1.1, drives the
  emulator from an external agent (snapshots, disk operations, paste,
  step, screen capture). Contract in `AiControlServer.h`.
- **Snapshot** — `POM2SNAP` save/load (CPU + RAM + soft switches; disk
  state intentionally excluded — would need image identity + per-track
  dirty bits).
- **Memory viewer** — region-coloured 64 KB hex + ASCII, change-flash,
  undo / redo, hex / ASCII search, bookmarks, 6502 disassembly toggle.
- **Threading** — UI 60 Hz, CPU pinned to 1.0227 MHz nominal (uncapped
  MAX available), single `stateMutex` between worker and renderer.

## Disk images

Drop floppy images into `disks/`, hard-disk images into `hdv/`,
SmartPort 3.5" images into `disks35/` (`disks/` is also scanned as
fallback). Mount via each card's panel or `Hardware → Insert disk
image`. Supported formats:

| Format            | Size              | Notes                                                  |
| ------------------- | ------------------- | -------------------------------------------------------- |
| `.dsk` / `.do`    | 143 360 B         | DOS 3.3 sector order                                   |
| `.po`             | 143 360 / 819 200 | ProDOS sector order — 143 KB = 5.25", 800 KB = 3.5"  |
| `.nib`            | 232 960 / 223 440 | Raw nibbles (35×6656 or CNib2 35×6384)                 |
| `.2mg` / `.2img`  | + 64 B            | 2IMG envelope (Asimov); volume #/WP/comment preserved  |
| `.woz`            | varies            | WOZ1 / WOZ2 with `optimal_bit_timing`, CRC32 validated |

Format detection is **content-driven** (a `.po` that's actually
DOS-skewed is recognised via vol-directory sniff); unrecognised files
surface a red error line in the panel. MacBinary 128-byte wrappers are
stripped transparently. Write-back is **opt-in** per card via the
panel's `Write-back (save on eject)` checkbox; WOZ `INFO.write_protected`
and 2IMG WP flag are always honoured.

## Slot configuration

Cards wire through `SlotBus`. Open `Hardware → Slot Configuration`
to pick a card per slot; click **Apply** to restart the emulator with
the new layout. Available card types:

| Key            | Card                                                  |
| ---------------- | ------------------------------------------------------- |
| `(empty)`      | nothing plugged                                       |
| `diskii`       | Disk II (5.25") — **multi-instance allowed**         |
| `hdv`          | ProDOS HDV (block device)                             |
| `smartport35`  | SmartPort 3.5" (Apple "Liron" card, 2 Sony drives)    |
| `ssc`          | Super Serial Card                                     |
| `clock`        | ProDOS clock card (ThunderClock+)                     |
| `chatmauve`    | Le Chat Mauve RGB monitor                             |
| `mouse`        | Mouse Interface (needs both Apple ROMs in `roms/`)    |
| `mockingboard` | Mockingboard A/C                                      |

Default layout:

| Slot | Card                                                   |
| ------ | -------------------------------------------------------- |
| 1    | (free — printer card hook reserved)                   |
| 2    | Super Serial Card (TCP `127.0.0.1:6502`)               |
| 3    | (IIe internal 80-column firmware when `SLOTC3ROM=off`) |
| 4    | ProDOS Clock card                                      |
| 5    | ProDOS HDV (or SmartPort 3.5" — pick one)              |
| 6    | Disk II (auto-plugged if `disk2.rom` is present)       |
| 7    | Le Chat Mauve RGB                                      |

Each card type is single-instance **except** `diskii`, which can live
in any number of slots. The chosen layout + per-card volume / mute /
port settings + TCP listener state all persist across runs via
`settings.json`.

**Boot paths follow slot positions**: `Boot HDV (slot N)`, `PR#N` and
the SmartPort "Mount + boot" library action all read the card's live
slot via `card->getSlot()` — move HDV from slot 5 to slot 2 in the
Slot Configuration panel and the boot path follows automatically.

## Keyboard & joystick

### Keyboard

| Host        | Apple II                                 |
| ------------- | ------------------------------------------ |
| Enter       | Return                                   |
| Backspace   | ←                                       |
| Arrows      | ← → ↑ ↓                              |
| Esc         | ESC                                      |
| Ctrl-A..Z   | `$01..$1A`                               |
| F9          | Screenshot (`screenshot_NNN.ppm` in CWD) |
| F11         | Reset / Ctrl-Reset (soft)                |
| F12         | Hard reset / power-cycle                 |

Both upper and lower case letters pass straight through — POM2 doesn't
force-uppercase what you type.

### Joystick

Plug a USB pad before launch or hot-plug it (POM2 polls all 16 GLFW
slots each frame). The first present pad auto-binds; `Hardware →
Joystick` opens a panel to pick a different one, set deadzone, and
invert axes.

| Host     | Apple II                    |
| ---------- | ----------------------------- |
| Axis X   | PADL(0) / `$C064`            |
| Axis Y   | PADL(1) / `$C065`            |
| Button 0 | PB0 / `$C061` (open-apple)   |
| Button 1 | PB1 / `$C062` (closed-apple) |
| Button 2 | PB2 / `$C063`                |

PADL(2) and PADL(3) read centred (127) — second-stick host binding is
on the roadmap.

## UI panels

- **Apple II Screen** — 280×192 (HGR) or 560×192 (DHGR / 80-col) RGBA.
- **Emulation** — registers, cycle counter, speed (1× / 2× / MAX), audio.
- **Cassette deck** — procedural 378×404 deck (Font Awesome icons).
- **Disk II (slot N)** — one window **per plugged DiskII card**. Motor
  LED, current track, Insert / Eject, write-back, library list, library
  click = insert + cold-boot, right-click = insert only.
- **Disk 3.5" (slot N or //c+ on-board)** — two drives stacked, motor
  LED + status + write-back + library. Library left-click = mount drive 1
  + cold-boot, right-click context menu = pick drive + boot-or-not.
- **HDV (slot N)** — image picker, Library tab with the `[host folder]`
  synthetic entry. Mount-and-boot follows the slot the card is in.
- **Super Serial (slot 2)** — start / stop TCP listener, change port.
- **Le Chat Mauve** — FIFO state, manual mode override, reset.
- **Mockingboard** — per-VIA register dump, AY state, volume.
- **Joystick** — host-pad picker, live axis / button readout.
- **Memory viewer / Memory Map** — `Debug` menu; three map layouts
  (Bar / Bar-H / Grid) for the 64 KB at a glance.

## CLI

`CliDispatcher`. Three phases: parse → pre-boot (preset/ROM/snapshot-
load/`--load addr:file`) → post-boot (tape ops/paste/run/step).

| Flag                                  | Effect                                        |
| --------------------------------------- | ----------------------------------------------- |
| `--preset ii\|ii+\|iie\|iic\|iic+`    | Pick the system profile up front              |
| `--speed N`                           | Emulated CPU cycles/frame (1× = 17 045)       |
| `--cpu-max`                           | Uncap CPU                                     |
| `--tape PATH`                         | Pre-load cassette image                       |
| `--35-disk1 PATH` / `--35-disk2 PATH` | Mount Sony 3.5" image into drive 1 / 2        |
| `--load ADDR:FILE`                    | Splash a binary at hex address                |
| `--run` / `--step`                    | Auto-run or single-step after boot            |
| `--paste TEXT`                        | Type text into the keyboard buffer            |
| `--play` / `--rec` / `--rewind`       | Cassette transport                            |
| `--snapshot-save FILE`                | Dump CPU + RAM at exit                        |
| `--snapshot-load FILE`                | Restore at startup before `--run`             |

## Known limitations

- **Mouse absolute position** — A2Desktop / MGTK's mouse cursor and
  the host cursor drift in absolute position because POM2's tracking
  is delta-based (no programmatic re-centring against the guest's
  internal `mouse_state` data segment, whose offset isn't known yet).
  Buttons and relative motion work fine.
- **DHGR composite artifact on some titles** — Shamus on `mario.dsk`
  in original WOZ form shows visual artefacts that don't appear on
  AppleWin; investigation ongoing.
- **Anti-//e games** — twelve Brøderbund + Gebelli 1982 titles refuse
  to boot on //e/IIc/IIc+ in their original WOZ form (faithful
  hardware behaviour — the games detect the upgraded ROM). Use a 4am
  crack or run them under the II+ profile.

See `TODO.md` for the full backlog.

## Licence

GPL-3.0.
