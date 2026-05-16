# CLAUDE.md

Orientation for the **emulator side** of POM2. User walkthrough →
`README.md`. Deep implementation notes (MAME-parity ports, port
internals, format gotchas, pinned smoke tests) → `DEV.md`.

**Conventions**:

- **One concern per file** — each `.cpp/.h` pair owns one subsystem.
- **MAME source of truth** — when porting hardware behaviour, cite the
  MAME file + line range in a comment and pin the behaviour with a
  smoke test under `tests/`.
- **CPU → audio/UI events carry an `emuCycles` stamp**. Consumers
  measure cadence in emulated CPU cycles, not wall-clock frames.
  POM2's disk-turbo bumps the CPU to ~60× real-time, which compresses
  an entire audio-buffer's worth of CPU activity into one
  `audioFrameCounter_` tick — wall-clock gap measurement degenerates
  to zero. Canonical example: `FloppySoundDevice::drainCommands`
  using the cycle stamp passed by `DiskIICard::seekPhaseW`. Same
  rule applies to any future cycle-rate-sensitive sink (sound /
  scope / trace recorder).

## Table of contents

- [Build & run](#build--run)
- [Subsystem map](#subsystem-map)
- [Memory map](#memory-map)
- [System profiles](#system-profiles)
- [Reset architecture](#reset-architecture)
- [CLI](#cli)
- [Version string locations](#version-string-locations)

## Build & run

```bash
./setup_imgui.sh             # one-time deps + clones imgui/
cd build && cmake .. && make # → build/POM2
./run_emulator.sh            # runs from repo root so roms/ probes resolve
```

ROMs are user-provided. Active profile (default `Apple ][+`) drives ROM
probe order — see [System profiles](#system-profiles). Legacy auto-detect
(flip to IIe on `roms/apple2e.rom` presence) survives as fallback.

## Subsystem map

| Subsystem | Files | Deep notes |
|---|---|---|
| 6502 / 65C02 / Rockwell / WDC | `M6502.h/.cpp` | [DEV.md § CPU](DEV.md#cpu) |
| Main memory, IIe paging, RamWorks | `Memory.h/.cpp` | [DEV.md § Memory](DEV.md#memory) |
| Display (HGR / DHGR / 80-col) | `Apple2Display.h/.cpp` | [DEV.md § Display](DEV.md#display) |
| Audio output, Speaker, Cassette | `AudioDevice.*`, `SpeakerDevice.*`, `CassetteDevice.*` | [DEV.md § Audio](DEV.md#audio) |
| Mockingboard (6522 + AY-3-8910) | `Mockingboard.h/.cpp` | [DEV.md § Mockingboard](DEV.md#mockingboard) |
| Floppy mechanical sounds | `FloppySoundDevice.h/.cpp` | [DEV.md § Floppy mechanical sounds](DEV.md#floppy-mechanical-sounds) |
| Slot bus, wire-OR IRQ | `SlotBus.h`, `SlotPeripheral.h` | [DEV.md § Slot bus](DEV.md#slot-bus--irq-aggregation) |
| Disk image, Disk II, ProDOS HDV, Snapshot | `DiskImage.*`, `DiskIICard.*`, `ProDOSVolume.*`, `SnapshotIO.*` | [DEV.md § Storage](DEV.md#storage) — DiskII is **multi-instance** (option C 2026-05-15): `diskii` may live in several slots ; per-slot persistence keys `disk_path_slotN`. WOZ2 honours INFO+39 `optimal_bit_timing`. |
| IWM (Apple FDC for //c / //c+ / Mac / IIgs) | `IWMDevice.*` | [DEV.md § Storage](DEV.md#storage) (live + authoritative on //c+ AND on 32 KB-ROM //c rev 0/3/4; Memory routes $C0E0-$C0EF through IWM whenever `iicHasAltBank` is set — MAME wires `A2BUS_IWM` for `apple2c0`/`apple2c3`/`apple2c4`/`apple2cp` per `apple2e.cpp:5249-5272` + `6281-6291`. Only the 16 KB rev-255 //c keeps `A2BUS_DISKIING`. Toggle shadow mode via `POM2_IWM_AUTHORITATIVE=0`. Pinned by `tests/iic_boot_trace.cpp`.) |
| SmartPort 3.5" — //c+ on-board (full IWM/GCR) | `Disk35Image.*`, `Sony35Drive.*`, `SmartPortHub.*`, `Disk35Controller_ImGui.*` | [DEV.md § Storage](DEV.md#storage) — image loader, Sony register protocol, MIG `recalc_active_device`, zoned 4:4 GCR encoder + decoder, IWM `setSony35()` dispatch (read + write), ImGui panel, CLI `--35-disk1/2`, Settings persistence, flux write-back with save-on-eject. Mechanical sound sink wired (`Sony35Drive::setFloppySound`). |
| SmartPort slot card (//e / II+, Liron-class) | `SmartPortCard.*`, `SmartPortUnit.*`, `SmartPort35Unit.*`, `SmartPortHdvUnit.*`, `SmartPort_ImGui.*` | [DEV.md § SmartPortCard](DEV.md#smartportcard-e-liron-class) — block-level ProDOS driver, 2 units per card, each unit is a polymorphic `SmartPortUnit` (today: 3.5" Sony 800K OR ProDOS HDV; extensible via `makeSmartPortUnit`). Each card OWNS its unit storage — no sharing with the //c+ on-board hub. Per-unit type + path + write-back persisted as `smartport_slotN_unitK_{type,path,writeback}`. Plug as `smartport35` in Slot Configuration; configure units via the **SmartPort Configuration** panel (Panels menu). Pinned by `tests/smartport_card_smoke_test.cpp` + `tests/smartport_mixed_units_smoke_test.cpp`. |
| Super Serial Card + telnet (slot 2) | `SuperSerialCard.h/.cpp` | [DEV.md § SSC](DEV.md#super-serial-card-slot-2--telnet-bridge) |
| ProDOS clock card (slot 4) | `ClockCard.h/.cpp` | [DEV.md § Clock card](DEV.md#prodos-clock-card-slot-4) |
| Mouse Card (slot 4 by conv.) | `MouseCard.h/.cpp` | [DEV.md § Mouse Card](DEV.md#mouse-card) |
| Joystick / paddles | `JoystickInput.h/.cpp` + Memory paddle RC | [DEV.md § Joystick / paddles](DEV.md#joystick--paddles) |
| UI (ImGui) | `MainWindow.*`, `MemoryViewer_ImGui.*`, … | [DEV.md § UI](DEV.md#ui-imgui) |
| Clock & threading | `EmulationController.h/.cpp` | [DEV.md § Clock & threading](DEV.md#clock--threading) |
| System profiles | `SystemProfile.h/.cpp` | [System profiles](#system-profiles) + [DEV.md § Profile internals](DEV.md#profile-switching-internals) |
| CLI | `CliDispatcher.h/.cpp` | [CLI](#cli) + [DEV.md § CLI](DEV.md#cli-clidispatcher) |

`POM2_CPU_CLOCK_HZ = 1 022 727` (14.31818 MHz / 14). UI 60 Hz; CPU
worker runs `cyclesPerFrame=17045` per tick. Single `stateMutex`
guards CPU + Memory.

## Memory map

```
$0000-$00FF  Zero page
$0100-$01FF  Stack
$0200-$03FF  Input buffer / user
$0400-$07FF  Text page 1 / Lo-res page 1 (interleaved row layout)
$0800-$0BFF  Text page 2 / Lo-res page 2
$0C00-$1FFF  User RAM
$2000-$3FFF  Hi-res page 1
$4000-$5FFF  Hi-res page 2
$6000-$BFFF  User RAM
$C000        Keyboard latch (low 7 = key, high = strobe)
$C000-$C00F  IIe paging (80STORE/RAMRD/RAMWRT/INTCXROM/ALTZP/SLOTC3ROM/
             80COL/ALTCHAR — ignored on II+)
$C010        Clear keyboard strobe
$C013-$C018  IIe paging status reads (bit 7 = on)
$C01E/$C01F  IIe RDALTCHAR / RD80COL
$C028        //c ROMBANK toggle on any //c-class ROM (`isIIcClass`,
             both 16 KB rev-255 and 32 KB rev-0/3/4/X dumps —
             the alt-firmware read paths additionally require
             `iicHasAltBank` which is set only by the 32 KB dumps).
             Falls through to cassette on II/II+/IIe.
$C030-$C03F  Speaker toggle (any access)
$C050-$C057  Display mode pairs (text/gfx, mixed, page 1/2, lo/hi-res)
$C05E/$C05F  IIe DHGR enable / disable (AN3 pulses for Le Chat Mauve FIFO)
$C061-$C063  Push-buttons (negative when pressed)
$C064-$C067  Paddle inputs (negative while RC discharging)
$C070        Paddle reset latch (mirrored $C070-$C07F)
$C071/3/5/7  RamWorks III aux-bank select (write `data & 0x7F`)
$C078/$C079  //c mouse-firmware IOUDIS SET/CLR mirrors (mirrors of $C07E/F)
$C07E/$C07F  IOUDIS SET/CLR (writes effective on //c/c+ only ; read $C07E
             returns bit-7 IOUDIS state on all IIe-class)
$C0A8-$C0AB  SSC ACIA (slot 2)
$C0C0        ThunderClock+ uPD1990AC bit-bang (slot 4)
$C0E0-$C0EF  Disk II soft switches (slot 6 — $C0EC=Q6L data, $C0ED=Q6H)
$C100-$C5FF  Slot ROMs (or IIe internal I/O ROM when INTCXROM=on)
$C300-$C3FF  IIe 80-col firmware (internal when SLOTC3ROM=off)
$C400-$C4FF  ProDOS clock card slot ROM
$C600-$C6FF  Disk II boot PROM (when roms/disk2.rom present)
$C700-$C7FF  Slot ROMs (currently empty)
$D000-$F7FF  Applesoft BASIC ROM
$F800-$FFFF  Monitor ROM + 6502 vectors ($FFFA-$FFFF)
```

In IIe mode the same map applies but most of `$0000-$BFFF` can route to
aux 64 KB under paging switches — see table at top of `Memory.h`.

## System profiles

| Profile | CPU default | iieMode | Main ROM probes | Built-in slots (locked in Slot Config UI) |
|---|---|---|---|---|
| Apple ][ Original (1977) | NMOS | off | `apple2o.rom`, `apple2.rom` | — (all 7 slots free) |
| Apple ][+ (1979) | NMOS | off | `apple2p.rom`, `apple2.rom` | — |
| Apple //e (1983, Unenhanced) | NMOS | on | `apple2e_unenh.rom`, `342-0135-b.64.rom`, `apple2e.rom` | — (AUX = ext80 label) |
| Apple //e Enhanced (1985) | 65C02 | on | `apple2e.rom` | — (AUX = ext80 label) |
| Apple //c (1984) | 65C02 | on | `apple2c-32Kv0.rom`, `apple2c-16K.rom` | sl2 SSC, sl4 Mouse, sl6 Disk II |
| Apple //c Plus (1988) | 65C02 | on | `apple2cp.rom`, `apple2c-plus.rom`, `apple2c-32Kv0.rom` | sl2 SSC, sl4 Mouse, sl5 SmartPort 3.5", sl6 Disk II (IWM) |

Profiles with built-in slots force the listed cards into the SlotBus on
profile load (overrides user `slot_N_card` settings) and render those
rows disabled in the Slot Configuration panel with a "built-in" badge.
See [DEV.md § Profile switching internals](DEV.md#profile-switching-internals)
for the `ProfileConfig::builtInSlots` mechanism. Mechanism (Theme 4
audit fix) added 2026-05-16.

**ROM identity check**: when the generic `apple2.rom` fallback resolves
because no profile-specific dump is present, the loader emits a warning
that the loaded ROM may not match the selected machine (II ↔ II+
mis-identification was previously silent — Theme 9 audit fix).

Default cycles/frame = 17045 for II/II+/IIe/IIc; //c+ defaults to
**68180 (4×)** to match the on-board Zip-style accelerator. Real //c+
drops back to 1 MHz for disk I/O via $C036 — POM2 doesn't model that
softswitch, but its event-driven disk LSS is purely cycle-driven so
the 4× CPU still produces correctly-paced nibbles. `cpu_mode_override`
= `auto|nmos|65c02` (Machine → CPU menu).

**//c+ MIG + IWM**: the //c+ alt firmware (bank 1) drives an Apple
MIG gate-array at the `$CC00-$CCFF` / `$CE00-$CEFF` windows in the
expansion ROM area (drive enable, 2 KB MIG RAM, 3.5" head select)
and an IWM at `$C0E0-$C0EF` with mode + status + WHD registers on
top of the normal Q6/Q7 control. POM2 implements both in the
minimum form needed for cold boot (banner display + 5.25" auto-
boot); the full IWM bit-shift state machine is **not** modelled.
See [DEV.md § //c+ MIG + IWM handshake](DEV.md#storage) for the
exact register decode and the pinned smoke test. Profile switching is a full cold reset with strict ordering
— see [DEV.md § Profile switching internals](DEV.md#profile-switching-internals)
for 32 KB ROM disambiguation, `$C028` ROMBANK toggle, //c INTCXROM
override, 20 KB II+ dumps, and the 13-step `applyProfile` sequence.

CLI `--preset` triggers the same path after the legacy auto-probe — wins.
Aliases: `apple2`, `apple2plus`, `iie-u` / `iieunenhanced` /
`apple2e-1983`, `apple2e`, `apple2c`, `apple2cplus`, `//e-u`, `//e`,
`//c`, `//c+`.

## Reset architecture

Three classes of reset, mirroring MAME's split:

| POM2 verb | Trigger | Behaviour | MAME analogue |
|---|---|---|---|
| `softReset()` | F11, toolbar, AI `/reset?kind=soft`, `applyProfile` | RAM survives. On IIe-class wipes the full MMU/IOU/LC list; on II/II+ leaves LC + display untouched (kbd strobe only). CPU `SP -= 3`, I flag set, PC = $FFFC. | `reset_w(true→false)` sequence per profile |
| `hardReset()` | F12, toolbar, AI `/reset?kind=hard`, `applyProfile` step 11 | RAM survives, CPU additionally zeros A/X/Y. POM2-only convention. | Same as `reset_w` plus extra register wipe |
| `coldBoot()` | Toolbar power button, AI `/reset?kind=cold`, MainWindow ctor, Disk Library "Insert + boot" | Wipes user RAM + LC + aux with `00 FF 00 FF…` MAME pattern; full reset; hard reset CPU. | `machine_start` + `machine_reset` |
| `bootFromSlot(N)` | HDV / SmartPort / Disk II Library menu "Boot" buttons | `coldBoot` then `PC = $C000 + N*256` after validating the JSR-dispatch trio ($Cn01=$20, $Cn03=$00, $Cn05=$03 — Apple II Ref Manual Appx C). The full F8 Autostart further requires $Cn07=$3C (Disk II / SmartPort), but we deliberately **don't** check that byte: HDV cards have $Cn07=$01 (ProDOS non-removable) and the F8 firmware won't scan them, so the GUI "Boot HDV" shortcut is the only way to boot a hard-disk image. JSR-trio mismatch → falls back to plain `coldBoot`. | Synthetic shortcut — MAME's F8 firmware scans natively but only Disk II / SmartPort signature; HDV needs the shortcut |

Keyboard wiring (post Theme 1):

- **Left Alt = Open-Apple** → `Memory::setOpenAppleKey` → $C061 bit 7
- **Right Alt = Solid-Apple** → `Memory::setSolidAppleKey` → $C062 bit 7
- F11 / F12 / F9 / Left Alt / Right Alt routed unconditionally even when
  ImGui captures keyboard focus (so $C061/$C062 edges + reset keys
  always reach the emulated machine).

## CLI

`CliDispatcher`. Three phases: parse → pre-boot (preset/ROM/snapshot-
load/`--load addr:file`) → post-boot (tape ops/paste/run/step).

Flags: `--preset ii|ii+|iie-u|iie|iic|iic+`, `--speed`, `--cpu-max`, `--tape`,
`--35-disk1 path`/`--35-disk2 path` (//c+ Sony 3.5"),
`--load addr:file`, `--run`, `--paste`, `--step`,
`--play`/`--rec`/`--rewind`, `--snapshot-save`/`--snapshot-load`.

## Version string locations

Current release: **v0.5**. Bump in:

- `main.cpp` (window title + console banner)
- `MainWindow.cpp` (About dialog)
- `CMakeLists.txt` (`project(... VERSION x.y ...)`)
- `README.md` (status section, if a version pill is reintroduced)
