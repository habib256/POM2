# CLAUDE.md

Orientation for the **emulator side** of POM2 — the always-loaded
index. Reach for the other docs in this order:

- `README.md` — user walkthrough (build, profiles, ROM/disk
  placement, keys, CLI).
- `DEV.md` — deep implementation notes (MAME-parity ports, port
  internals, format gotchas, pinned smoke tests).
- `TODO.md` — active backlog + MAME↔POM2 parity dashboard.
- `CHANGELOG.md` — resolved items + the *why* behind non-obvious
  fixes (`git log` is canonical mechanics; this captures
  rationale).

**Conventions**:

- **One concern per file** — each `.cpp/.h` pair owns one
  subsystem.
- **MAME source of truth** — when porting hardware behaviour,
  cite the MAME file + line range in a comment and pin the
  behaviour with a smoke test under `tests/`.
- **CPU → audio/UI events carry an `emuCycles` stamp.** Consumers
  measure cadence in emulated CPU cycles, not wall-clock frames.
  Disk-turbo bumps the CPU to ~60× real-time → wall-clock gaps
  collapse to zero across an audio-buffer tick. Canonical
  example: `FloppySoundDevice::drainCommands` uses the cycle
  stamp passed by `DiskIICard::seekPhaseW`.

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

ROMs are user-provided. Active profile (default `Apple ][+`) drives
ROM probe order — see [System profiles](#system-profiles). Legacy
auto-detect (flip to IIe on `roms/apple2e.rom` presence) survives
as fallback.

## Subsystem map

| Subsystem | Files | Deep notes |
|---|---|---|
| 6502 / 65C02 / Rockwell / WDC | `M6502.h/.cpp` | [DEV § CPU](DEV.md#cpu) |
| Main memory, IIe paging, RamWorks | `Memory.h/.cpp` | [DEV § Memory](DEV.md#memory) — `dataMutable()` replaced by `writeRamUnchecked` + `loadFlatTestImage` (2026-05-26) |
| Display (HGR / DHGR / 80-col) | `Apple2Display.h/.cpp` | [DEV § Display](DEV.md#display) |
| Audio / Speaker / Cassette | `AudioDevice.*`, `SpeakerDevice.*`, `CassetteDevice.*` | [DEV § Audio](DEV.md#audio) |
| Mockingboard (6522 + AY) | `Mockingboard.h/.cpp` | [DEV § Mockingboard](DEV.md#mockingboard) |
| Floppy mechanical sounds | `FloppySoundDevice.h/.cpp` | [DEV § Floppy mechanical sounds](DEV.md#floppy-mechanical-sounds) |
| Slot bus, wire-OR IRQ | `SlotBus.h`, `SlotPeripheral.h` | [DEV § Slot bus](DEV.md#slot-bus--irq-aggregation) |
| Disk image, Disk II, ProDOS HDV, Snapshot | `DiskImage.*`, `DiskIICard.*`, `ProDOSVolume.*`, `SnapshotIO.*` | [DEV § Storage](DEV.md#storage) — DiskII **multi-instance**; per-slot persistence keys `disk_path_slotN`; WOZ2 honours INFO+39 |
| ProDOS block backing + HDV-class cards | `Block512Backing.*`, `ProDOSBlockCard.h`, `ProDOSHardDiskCard.*`, `CffaCard.*`, `AtaBlockDevice.*` | [DEV § ProDOSHardDiskCard](DEV.md#prodoshardiskcard-hdv-synthetic-block-model) + [§ CffaCard](DEV.md#cffacard-cffa-20--mame-faithful-ide) — shared `Block512Backing`; synthetic `hdv` (zero-ROM, AppleWin lineage) vs MAME-faithful `cffa` (real `cffa20ee(c)02.bin` over emulated ATA; `$Cn07=$3C` ⇒ F8-bootable). `hdvDevice()` targets whichever |
| IWM (Apple FDC, //c / //c+ / Mac / IIgs) | `IWMDevice.*` | [DEV § Storage](DEV.md#storage) — live on //c+ AND on 32 KB-ROM //c rev 0/3/4; toggle via `POM2_IWM_AUTHORITATIVE=0`. Pinned `iic_boot_trace` |
| SmartPort 3.5" — //c+ on-board (IWM/GCR) | `Disk35Image.*`, `Sony35Drive.*`, `SmartPortHub.*`, `Disk35Controller_ImGui.*` | [DEV § SmartPort 3.5" stack](DEV.md#smartport-35-stack) |
| SmartPort slot card (//e / II+, Liron-class) | `SmartPortCard.*`, `SmartPortUnit.*`, `SmartPort35Unit.*`, `SmartPortHdvUnit.*` | [DEV § SmartPortCard](DEV.md#smartportcard-e-liron-class) — block-level driver, 2 units/card, polymorphic `SmartPortUnit` (Sony 800K or ProDOS HDV via `Block512Backing`). Per-unit `smartport_slotN_unitK_*`. Implements `MountableMediaCard` |
| Super Serial Card (slot 2) + telnet | `SuperSerialCard.h/.cpp` | [DEV § SSC](DEV.md#super-serial-card-slot-2--telnet-bridge) |
| Printer card (parallel, synthetic) | `PrinterCard.h/.cpp` | [DEV § Printer card](DEV.md#printer-card-parallel-synthetic) — synthetic 256 B slot ROM hooks CSWL/CSWH → host-side spool, saved as `.txt` from Devices → Printer. Built-in sl1 on //c/+; pluggable any slot on II/II+/IIe. PDF deferred. Pinned `printer_card_smoke` |
| ProDOS clock card (slot 4) | `ClockCard.h/.cpp` | [DEV § Clock card](DEV.md#prodos-clock-card-slot-4) |
| Mouse Card (slot 4 by conv.) | `MouseCard.h/.cpp`, `MouseCardAppleWin.h/.cpp` | [DEV § Mouse Card](DEV.md#mouse-card) — TWO variants: `mouse` = MAME-faithful 68705P3 + 6821 (needs `mouse_341-0270-c.bin` + `mouse_341-0269.bin`), `mouseaw` = AppleWin HLE (slot EPROM only). Pinned `mouse_card_smoke` + `mouse_card_applewin_smoke` |
| Joystick / paddles | `JoystickInput.h/.cpp` + Memory paddle RC | [DEV § Joystick / paddles](DEV.md#joystick--paddles) |
| UI (ImGui) | `MainWindow.*`, `MemoryViewer_ImGui.*`, … | [DEV § UI](DEV.md#ui-imgui) |
| Slot Configuration + card catalog + media bay | `MainWindow_Slots.cpp`, `MountableMediaCard.h`, `SlotCardCatalog.h` | [DEV § Host control center](DEV.md#host-control-center-slot-configuration--floppy-emu) — two-column panel: left assigns cards (built-ins greyed), right is a live SlotBus walk over `MountableMediaCard` bays + Disk II drives. The standalone Slot Manager was folded in (deleted 2026-05-25). Pinned `slot_multi_card_smoke` |
| Floppy Emu (BMOW SD/OLED) | `FloppyEmuDevice.*`, `FloppyEmu_ImGui.*` | [DEV § Host control center](DEV.md#host-control-center-slot-configuration--floppy-emu) — Devices → Floppy Emu. 4 modes (5.25 / 3.5 / Unidisk 3.5 / Smartport HD); SD explorer + `favdisks.txt` over `floppyemu/`; mounting **routed into existing `DiskIICard` / `SmartPortCard` units**. Pinned `floppy_emu_smoke` |
| Clock & threading | `EmulationController.h/.cpp` | [DEV § Clock & threading](DEV.md#clock--threading) |
| System profiles | `SystemProfile.h/.cpp` | [System profiles](#system-profiles) + [DEV § Profile switching](DEV.md#profile-switching-internals) |
| CLI | `CliDispatcher.h/.cpp` | [CLI](#cli) + [DEV § CLI](DEV.md#cli-clidispatcher) |

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
$C028        //c ROMBANK toggle on any //c-class ROM (both 16 KB rev-255
             and 32 KB rev-0/3/4/X dumps; alt-firmware reads further
             require `iicHasAltBank`, set only by 32 KB dumps).
             Falls through to cassette on II/II+/IIe.
$C030-$C03F  Speaker toggle (any access)
$C050-$C057  Display mode pairs (text/gfx, mixed, page 1/2, lo/hi-res)
$C05E/$C05F  IIe DHGR enable / disable (AN3 pulses for Le Chat Mauve FIFO)
$C061-$C063  Push-buttons (negative when pressed)
$C064-$C067  Paddle inputs (negative while RC discharging)
$C070        Paddle reset latch (mirrored $C070-$C07F)
$C071/3/5/7  RamWorks III aux-bank select (write `data & 0x7F`)
$C078/$C079  //c mouse-firmware IOUDIS SET/CLR mirrors (of $C07E/F)
$C07E/$C07F  IOUDIS SET/CLR (writes effective on //c/c+ only; read $C07E
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

In IIe mode the same map applies but most of `$0000-$BFFF` can route
to aux 64 KB under paging switches — see table at top of `Memory.h`.

## System profiles

| Profile | CPU | iieMode | Main ROM probes | Built-in slots (locked in UI) |
|---|---|---|---|---|
| Apple ][ Original (1977)  | NMOS  | off | `apple2o.rom`, `apple2.rom` | — (all 7 free) |
| Apple ][+ (1979)          | NMOS  | off | `apple2p.rom`, `apple2.rom` | — |
| Apple //e Unenh. (1983)   | NMOS  | on  | `apple2e_unenh.rom`, `342-0135-b.64.rom`, `apple2e.rom` | — (AUX = ext80) |
| Apple //e Enh. (1985)     | 65C02 | on  | `apple2e.rom` | — (AUX = ext80) |
| Apple //c (1984)          | 65C02 | on  | `apple2c-32Kv0.rom`, `apple2c-16K.rom` | sl1 Printer, sl2 SSC, sl4 Mouse, sl5 SmartPort, sl6 Disk II |
| Apple //c Plus (1988)     | 65C02 | on  | `apple2cp.rom`, `apple2c-plus.rom`, `apple2c-32Kv0.rom` | sl1 Printer, sl2 SSC, sl4 Mouse, sl5 SmartPort 3.5" (IWM), sl6 Disk II |

Profiles with built-in slots force the listed cards into the SlotBus
on profile load (overrides user `slot_N_card` settings) and render
those rows disabled in Slot Config with a "built-in" badge. Mechanism
detail → [DEV § Profile switching](DEV.md#profile-switching-internals).

**ROM identity check**: when the generic `apple2.rom` fallback
resolves because no profile-specific dump is present, the loader
emits a warning that the loaded ROM may not match the selected
machine.

Default `cyclesPerFrame` = 17045 for II/II+/IIe/IIc; **//c+ defaults
to 68180 (4×)** to match the on-board Zip-style accelerator. POM2
doesn't model the `$C036` 1 MHz fall-back during disk I/O, but the
event-driven disk LSS is cycle-driven so 4× CPU still produces
correctly-paced nibbles. `cpu_mode_override = auto|nmos|65c02`
(Machine → CPU menu).

**//c+ MIG + IWM**: //c+ alt firmware (bank 1) drives an Apple MIG
gate-array at `$CC00-$CCFF` / `$CE00-$CEFF` (drive enable, 2 KB MIG
RAM, 3.5" head select) and an IWM at `$C0E0-$C0EF`. POM2 implements
the minimum for cold boot (banner + 5.25" auto-boot); the full IWM
bit-shift state machine is **not** modelled, so the firmware's
IWM/Sony 3.5" boot never reaches a bootable disk. Detail → [DEV §
//c+ MIG + IWM handshake](DEV.md#profile-switching-internals).

**//c-class on-board SmartPort (3.5" + HDV boot)**: because the
real IWM/Sony GCR boot is unmodelled — and MAME doesn't emulate
3.5"/SmartPort on the plain //c at all — POM2 boots 3.5" **and**
HDV on //c/+/c+ through a **host-served SmartPort block device** at
the built-in slot 5. `Memory::memRead` punches a hole at
`$C500-$C5FF` (bank 0) for the slot-5 `SmartPortCard` firmware
**iff** the SmartPort is "armed" (`Memory::setIicSmartPortArmed`)
AND holds media (`SlotPeripheral::exposesIicOnboardRom`). The armed
gate is essential: `bootFromSlot` arms it (explicit GUI/CLI boot
only) and every reset/cold-boot disarms it, so the //c ROM's **own
autostart always sees its real `$C500` firmware** (substituting the
stub there corrupts a multi-device reboot — the "garbled Apple //c
banner" bug). Net: a reboot is always clean (Disk II / banner);
on-board SmartPort boots via the Disk Library / Slot Config "Boot"
button. Device-select I/O (`$C0D0-$C0DF`) is never masked. Pinned
by `iic_onboard_smartport_test`; see [DEV § //c-class on-board
SmartPort](DEV.md#storage) and the `project_iic_smartport_boot`
memory.

Profile switching is a full cold reset with strict ordering — see
[DEV § Profile switching](DEV.md#profile-switching-internals) for
the 13-step `applyProfile` sequence + 32 KB ROM disambiguation.

CLI `--preset` triggers the same path after the legacy auto-probe
— wins. Aliases: `apple2`, `apple2plus`, `iie-u`/`iieunenhanced`/
`apple2e-1983`, `apple2e`, `apple2c`, `apple2cplus`, `//e-u`, `//e`,
`//c`, `//c+`.

## Reset architecture

Three classes of reset, mirroring MAME's split:

| POM2 verb | Trigger | Behaviour | MAME analogue |
|---|---|---|---|
| `softReset()` | F11, toolbar, AI `/reset?kind=soft`, `applyProfile` | RAM survives. IIe-class wipes full MMU/IOU/LC list; II/II+ leaves LC + display untouched (kbd strobe only). CPU `SP -= 3`, I flag set, PC = $FFFC. | `reset_w(true→false)` per profile |
| `hardReset()` | F12, toolbar, AI `/reset?kind=hard`, `applyProfile` step 11 | RAM survives; CPU additionally zeros A/X/Y. POM2-only convention. | Same as `reset_w` plus extra register wipe |
| `coldBoot()` | Toolbar power, AI `/reset?kind=cold`, MainWindow ctor, Disk Library "Insert + boot" | Wipes user RAM + LC + aux with `00 FF 00 FF…` MAME pattern; full reset; hard reset CPU. | `machine_start` + `machine_reset` |
| `bootFromSlot(N)` | HDV / SmartPort / Disk II Library menu "Boot" | `coldBoot` then `PC = $C000 + N*256` after validating JSR-dispatch trio ($Cn01=$20, $Cn03=$00, $Cn05=$03 — Apple II Ref Manual Appx C). $Cn07 deliberately NOT validated: HDV cards have $Cn07=$01 and F8 firmware won't scan them. JSR-trio mismatch → falls back to `coldBoot`. | Synthetic shortcut |

Keyboard wiring:

- **Left Alt = Open-Apple** → `Memory::setOpenAppleKey` → $C061 bit 7
- **Right Alt = Solid-Apple** → `Memory::setSolidAppleKey` → $C062 bit 7
- F11 / F12 / F9 / Left Alt / Right Alt routed unconditionally even
  when ImGui captures keyboard focus.

## CLI

`CliDispatcher` (parser, no `EmulationController` dep) + `CliRunner`
(Phase-C runner). Three phases: parse → pre-boot (preset/ROM/
snapshot-load/`--load addr:file`) → post-boot (tape ops/paste/run/
step).

Flags: `--preset ii|ii+|iie-u|iie|iic|iic+`, `--speed`, `--cpu-max`,
`--tape`, `--35-disk1 path`/`--35-disk2 path` (//c+ Sony 3.5"),
`--load addr:file`, `--run`, `--paste`, `--step`,
`--play`/`--rec`/`--rewind`, `--snapshot-save`/`--snapshot-load`.

**Positional disk + kiosk**: `POM2 <disk-image>` mounts the image
into the slot its type maps to (`classifyDiskForSlot` in
`DiskImage.*`: 5.25" Disk II / 800K 3.5" / ProDOS HDV) under the
**saved profile + slot config**, then cold-boots it after a short
frame settle (`MainWindow::insertAndBootImage`, shared with Disk
Library "insert + boot"). `--kiosk` adds **exclusive full-screen**
with a chrome-free render path (`MainWindow::renderKiosk`).
Kiosk is **read-only** (no `imgui.ini` / `state.cfg` writes). An
HDV with no HDV/SmartPort card in the saved config auto-plugs a
`ProDOSHardDiskCard` into a free slot (`ensureHdvCardForBoot`,
session-local). Pinned: `cli_kiosk_test`.

## Version string locations

Current release: **v0.6**. Bump in:

- `main.cpp` (initial window title + console banner)
- `MainWindow_Slots.cpp` (runtime window title — `setGlfwWindow` +
  `applyProfile` step 13; **overrides** main.cpp's title once the
  profile resolves)
- `MainWindow.cpp` (About dialog)
- `CMakeLists.txt` (`project(... VERSION x.y ...)`)
- `README.md` (status section, if a version pill is reintroduced)
