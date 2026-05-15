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
| Disk image, Disk II, ProDOS HDV, Snapshot | `DiskImage.*`, `DiskIICard.*`, `ProDOSVolume.*`, `SnapshotIO.*` | [DEV.md § Storage](DEV.md#storage) |
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
$C028        //c ROMBANK toggle (only when 32 KB //c firmware loaded;
             falls through to cassette on II/II+/IIe)
$C030-$C03F  Speaker toggle (any access)
$C050-$C057  Display mode pairs (text/gfx, mixed, page 1/2, lo/hi-res)
$C05E/$C05F  IIe DHGR enable / disable (AN3 pulses for Le Chat Mauve FIFO)
$C061-$C063  Push-buttons (negative when pressed)
$C064-$C067  Paddle inputs (negative while RC discharging)
$C070        Paddle reset latch (mirrored $C070-$C07F)
$C071/3/5/7  RamWorks III aux-bank select (write `data & 0x7F`)
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

| Profile | CPU default | iieMode | Main ROM probes |
|---|---|---|---|
| Apple ][ Original (1977) | NMOS | off | `apple2o.rom`, `apple2.rom` |
| Apple ][+ (1979) | NMOS | off | `apple2p.rom`, `apple2.rom` |
| Apple //e Enhanced (1985) | 65C02 | on | `apple2e.rom` |
| Apple //c (1984) | 65C02 | on | `apple2c-32Kv0.rom`, `apple2c-16K.rom` |
| Apple //c Plus (1988) | 65C02 | on | `apple2cp.rom`, `apple2c-plus.rom`, `apple2c-32Kv0.rom` |

Default cycles/frame = 17045 for II/II+/IIe/IIc; //c+ defaults to
**68180 (4×)** to match the on-board Zip-style accelerator. Real //c+
drops back to 1 MHz for disk I/O via $C036 — POM2 doesn't model that
softswitch, but its event-driven disk LSS is purely cycle-driven so
the 4× CPU still produces correctly-paced nibbles. `cpu_mode_override`
= `auto|nmos|65c02` (Machine → CPU menu). Profile switching is a full cold reset with strict ordering
— see [DEV.md § Profile switching internals](DEV.md#profile-switching-internals)
for 32 KB ROM disambiguation, `$C028` ROMBANK toggle, //c INTCXROM
override, 20 KB II+ dumps, and the 13-step `applyProfile` sequence.

CLI `--preset` triggers the same path after the legacy auto-probe — wins.
Aliases: `apple2`, `apple2plus`, `apple2e`, `apple2c`, `apple2cplus`,
`//e`, `//c`, `//c+`.

## CLI

`CliDispatcher`. Three phases: parse → pre-boot (preset/ROM/snapshot-
load/`--load addr:file`) → post-boot (tape ops/paste/run/step).

Flags: `--preset ii|ii+|iie|iic|iic+`, `--speed`, `--cpu-max`, `--tape`,
`--load addr:file`, `--run`, `--paste`, `--step`,
`--play`/`--rec`/`--rewind`, `--snapshot-save`/`--snapshot-load`.

## Version string locations

Current release: **v0.5**. Bump in:

- `main.cpp` (window title + console banner)
- `MainWindow.cpp` (About dialog)
- `CMakeLists.txt` (`project(... VERSION x.y ...)`)
- `README.md` (status section, if a version pill is reintroduced)
