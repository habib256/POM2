# CLAUDE.md

Orientation **always-loaded index** — keep terse, defer detail to other docs.

- `README.md` — user walkthrough (build, profiles, ROM/disk placement, keys, CLI).
- `DEV.md` — implementation deep-dives (MAME-parity ports, internals, gotchas, pinned tests).
- `TODO.md` — active backlog + MAME↔POM2 parity dashboard.
- `CHANGELOG.md` — resolved items + the **why** behind non-obvious fixes.

**Conventions**:

- **One concern per file** — each `.cpp/.h` pair owns one subsystem.
- **MAME = source of truth** — when porting hardware, cite the MAME file + line range in a comment and pin with a smoke test under `tests/`.
- **`emuCycles` everywhere** — CPU → audio/UI events carry a CPU-cycle stamp, not wall-clock. Disk-turbo (~60×) collapses wall-clock gaps to zero across an audio-buffer tick. Example: `FloppySoundDevice::drainCommands` consumes the stamp from `DiskIICard::seekPhaseW`.

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
./run_emulator.sh            # cwd = repo root so roms/ probes resolve
```

`POM2_CPU_CLOCK_HZ = 1 022 727` (14.31818 MHz / 14). UI 60 Hz; CPU worker runs `cyclesPerFrame=17045` per tick. Single `stateMutex` guards CPU + Memory.

ROMs are user-provided. Default profile is `Apple ][+`; see [System profiles](#system-profiles) for ROM probe order.

## Subsystem map

Detail lives in `DEV.md`. This map is the index — file pair + one-line note + DEV anchor.

| Subsystem | Files | DEV anchor |
|---|---|---|
| 6502 / 65C02 / Rockwell / WDC | `M6502.h/.cpp` | [§ CPU](DEV.md#cpu) |
| Memory + IIe paging + RamWorks | `Memory.h/.cpp` | [§ Memory](DEV.md#memory) |
| Display (HGR / DHGR / 80-col) | `Apple2Display.h/.cpp` | [§ Display](DEV.md#display) |
| Speaker / Cassette / Audio bus | `AudioDevice.*`, `SpeakerDevice.*`, `CassetteDevice.*` | [§ Audio](DEV.md#audio) |
| Mockingboard A/C + Sound II | `Mockingboard.h/.cpp` + `Via6522.h` + `Ay3_8910.h` | [§ Mockingboard](DEV.md#mockingboard), [§ Sound II](DEV.md#mockingboardcard-variantsoundii) |
| Phasor (2×VIA, 4×AY) | `PhasorCard.h/.cpp` | [§ Phasor](DEV.md#phasor-applied-engineering) |
| SSI263 speech chip | `Ssi263.h/.cpp` + `Ssi263PhonemeData.h/.cpp` | [§ SSI263](DEV.md#ssi263--echo-street-electronics) |
| Echo+ (standalone SSI263) | `EchoPlusCard.h/.cpp` | [§ EchoPlusCard](DEV.md#echopluscard) |
| Floppy mechanical sounds | `FloppySoundDevice.h/.cpp` | [§ Floppy sounds](DEV.md#floppy-mechanical-sounds) |
| Slot bus + wire-OR IRQ | `SlotBus.h`, `SlotPeripheral.h` | [§ Slot bus](DEV.md#slot-bus--irq-aggregation) |
| DiskImage / DiskIICard / Snapshot | `DiskImage.*`, `DiskIICard.*`, `SnapshotIO.*` | [§ Storage](DEV.md#storage) |
| ProDOS block backing + HDV cards | `Block512Backing.*`, `ProDOSHardDiskCard.*`, `CffaCard.*`, `AtaBlockDevice.*` | [§ HDV](DEV.md#prodoshardiskcard-hdv-synthetic-block-model), [§ CFFA](DEV.md#cffacard-cffa-20--mame-faithful-ide) |
| IWM (//c, //c+, Mac, IIgs) | `IWMDevice.*` | [§ IWM](DEV.md#iwm-c-on-board) |
| SmartPort 3.5" //c+ on-board | `Disk35Image.*`, `Sony35Drive.*`, `SmartPortHub.*` | [§ SmartPort 3.5"](DEV.md#smartport-35-stack) |
| SmartPort slot card (Liron-class) | `SmartPortCard.*`, `SmartPort*Unit.*` | [§ SmartPortCard](DEV.md#smartportcard-e-liron-class) |
| Super Serial + telnet | `SuperSerialCard.h/.cpp` | [§ SSC](DEV.md#super-serial-card-slot-2--telnet-bridge) |
| Printer card (synthetic → spool) | `PrinterCard.h/.cpp` | [§ Printer](DEV.md#printer-card-parallel-synthetic) |
| ProDOS clock card | `ClockCard.h/.cpp` | [§ Clock](DEV.md#prodos-clock-card-slot-4) |
| Mouse Card (MAME + AppleWin HLE) | `MouseCard.*`, `MouseCardAppleWin.*` | [§ Mouse](DEV.md#mouse-card) |
| Joystick / paddles | `JoystickInput.h/.cpp` | [§ Joystick](DEV.md#joystick--paddles) |
| UI (ImGui) | `MainWindow.*`, `*_ImGui.*` | [§ UI](DEV.md#ui-imgui) |
| Slot Config + catalog + media bay | `MainWindow_Slots.cpp`, `MountableMediaCard.h`, `SlotCardCatalog.h` | [§ Host control](DEV.md#host-control-center-slot-configuration--floppy-emu) |
| Floppy Emu (BMOW SD/OLED) | `FloppyEmuDevice.*`, `FloppyEmu_ImGui.*` | [§ Floppy Emu](DEV.md#floppy-emu-bmow) |
| Clock & threading | `EmulationController.h/.cpp` | [§ Threading](DEV.md#clock--threading) |
| System profiles | `SystemProfile.h/.cpp` | [§ Profiles](DEV.md#profile-switching-internals) |
| CLI | `CliDispatcher.h/.cpp` | [§ CLI](DEV.md#cli-clidispatcher) |
| WebAssembly build | `build_wasm.sh`, `web/shell.html` | [§ WASM](DEV.md#webassembly-browser-build) |

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
$C028        //c ROMBANK toggle (any //c-class ROM; alt-firmware reads
             further require `iicHasAltBank`). Cassette on II/II+/IIe.
$C030-$C03F  Speaker toggle (any access)
$C050-$C057  Display mode pairs (text/gfx, mixed, page 1/2, lo/hi-res)
$C05E/$C05F  IIe DHGR enable/disable (AN3 pulses → Le Chat Mauve FIFO)
$C061-$C063  Push-buttons (negative when pressed)
$C064-$C067  Paddle inputs (negative while RC discharging)
$C070        Paddle reset latch (mirrored $C070-$C07F)
$C071/3/5/7  RamWorks III aux-bank select (write `data & 0x7F`)
$C078/$C079  //c mouse-firmware IOUDIS SET/CLR mirrors (of $C07E/F)
$C07E/$C07F  IOUDIS SET/CLR (writes effective on //c/c+ only)
$C0A8-$C0AB  SSC ACIA (slot 2)
$C0C0        ThunderClock+ uPD1990AC bit-bang (slot 4)
$C0E0-$C0EF  Disk II soft switches (slot 6 — $C0EC=Q6L, $C0ED=Q6H)
$C0(8+s)X    Per-slot device select (e.g. Phasor mode soft-switch
             $C0(8+s)0..F when a Phasor sits in slot s)
$C100-$C5FF  Slot ROMs (or IIe internal I/O ROM when INTCXROM=on).
             When MockingboardCard SoundII is in slot s, $Cs40-$Cs44
             routes to the SSI263; when EchoPlusCard is in slot s,
             $Cs00-$Cs04 routes to its SSI263.
$C300-$C3FF  IIe 80-col firmware (internal when SLOTC3ROM=off)
$C400-$C4FF  ProDOS clock card slot ROM
$C600-$C6FF  Disk II boot PROM (when roms/disk2.rom present)
$C700-$C7FF  Slot ROMs (currently empty)
$D000-$F7FF  Applesoft BASIC ROM
$F800-$FFFF  Monitor ROM + 6502 vectors ($FFFA-$FFFF)
```

In IIe mode the same map applies but most of `$0000-$BFFF` can route to aux 64 KB under paging switches — see table at top of `Memory.h`.

## System profiles

| Profile | CPU | iieMode | Main ROM probes | Built-in slots (locked in UI) |
|---|---|---|---|---|
| Apple ][ Original (1977)  | NMOS  | off | `apple2o.rom`, `apple2.rom` | — |
| Apple ][+ (1979)          | NMOS  | off | `apple2p.rom`, `apple2.rom` | — |
| Apple //e Unenh. (1983)   | NMOS  | on  | `apple2e_unenh.rom`, `apple2e.rom` | — (AUX = ext80) |
| Apple //e Enh. (1985)     | 65C02 | on  | `apple2e.rom` | — (AUX = ext80) |
| Apple //c (1984)          | 65C02 | on  | `apple2c-32Kv0.rom`, `apple2c-16K.rom` | sl1 SSC (printer port) · sl2 SSC (modem port) · sl4 Mouse · sl5 SmartPort · sl6 Disk II |
| Apple //c Plus (1988)     | 65C02 | on  | `apple2cp.rom`, `apple2c-plus.rom` | sl1 SSC (printer port) · sl2 SSC (modem port) · sl4 Mouse · sl5 SmartPort 3.5" (IWM) · sl6 Disk II |

Built-in slots force their listed card onto the SlotBus on profile load (overriding `slot_N_card` settings) and grey out their row in Slot Config. Detail → [DEV § Profile switching](DEV.md#profile-switching-internals).

**ROM identity check**: when the generic `apple2.rom` fallback resolves (no profile-specific dump present), the loader warns the ROM may not match the selected machine.

Default `cyclesPerFrame` = 17045 for II/II+/IIe/IIc; **//c+ defaults to 68180 (4×)** for its on-board Zip-style accelerator. `$C036` 1 MHz fall-back during disk I/O not modelled (event-driven disk LSS keeps nibbles cycle-correct anyway). `cpu_mode_override = auto|nmos|65c02` (Machine → CPU menu).

**//c+ MIG + IWM**: //c+ alt firmware (bank 1) drives the Apple MIG gate-array at `$CC00-$CCFF` / `$CE00-$CEFF` + IWM at `$C0E0-$C0EF`. POM2 implements enough for cold boot (banner + 5.25" auto-boot); the full IWM bit-shift state machine is **not** modelled, so the firmware's IWM/Sony 3.5" boot path never reaches a bootable disk. → [DEV](DEV.md#profile-switching-internals).

**//c-class on-board SmartPort (3.5" + HDV boot)**: real IWM/Sony GCR boot is unmodelled, and MAME doesn't emulate 3.5"/SmartPort on the plain //c. POM2 boots 3.5" and HDV on //c/+/c+ through a host-served SmartPort block device at built-in slot 5. `Memory::memRead` punches a hole at `$C500-$C5FF` for the SmartPort firmware iff the slot is **armed** + holds media. `bootFromSlot` arms it; every reset disarms it, so the //c ROM's autostart always sees its real `$C500` firmware (avoids the "garbled //c banner" bug). Pinned by `iic_onboard_smartport_test`. → [DEV § Storage](DEV.md#c-class-on-board-smartport-35--hdv-boot).

Profile switching is a full cold reset with strict ordering — 13-step `applyProfile` sequence detailed in [DEV](DEV.md#profile-switching-internals).

CLI `--preset` triggers the same path. Aliases: `apple2`, `apple2plus`, `iie-u`, `apple2e`, `apple2c`, `apple2cplus`, `//e`, `//c`, `//c+`.

## Reset architecture

Three classes of reset (+ one boot shortcut), mirroring MAME's split:

| POM2 verb | Trigger | Behaviour | MAME analogue |
|---|---|---|---|
| `softReset()` | F11, toolbar, AI `/reset?kind=soft`, `applyProfile` | RAM survives. IIe-class wipes full MMU/IOU/LC; II/II+ leaves LC + display untouched (kbd strobe only). CPU `SP -= 3`, I flag set, PC = $FFFC. | `reset_w(true→false)` |
| `hardReset()` | F12, toolbar, AI `/reset?kind=hard`, `applyProfile` step 11 | RAM survives; CPU additionally zeros A/X/Y. POM2-only convention. | `reset_w` + register wipe |
| `coldBoot()` | Toolbar power, AI `/reset?kind=cold`, MainWindow ctor, "Insert + boot" | Wipes user RAM + LC + aux with `00 FF 00 FF…` MAME pattern; full reset; hard reset CPU. | `machine_start` + `machine_reset` |
| `bootFromSlot(N)` | HDV / SmartPort / Disk II Library "Boot" | `coldBoot` then `PC = $C000 + N*256` after validating JSR-dispatch trio ($Cn01=$20, $Cn03=$00, $Cn05=$03 — Apple II Ref Manual Appx C). $Cn07 NOT validated (HDV cards have $Cn07=$01). Mismatch → falls back to `coldBoot`. | Synthetic shortcut |

Keyboard wiring:

- **Left Alt = Open-Apple** → $C061 bit 7
- **Right Alt = Solid-Apple** → $C062 bit 7
- F11 / F12 / F9 / Left Alt / Right Alt routed unconditionally (even when ImGui captures keyboard focus).

## CLI

`CliDispatcher` (parser, no `EmulationController` dep) + `CliRunner` (Phase-C runner). Three phases: parse → pre-boot (preset/ROM/snapshot-load/`--load addr:file`) → post-boot (tape ops/paste/run/step).

Flags: `--preset ii|ii+|iie-u|iie|iic|iic+`, `--speed`, `--cpu-max`, `--tape`, `--35-disk1 path`/`--35-disk2 path` (//c+ Sony 3.5"), `--load addr:file`, `--run`, `--paste`, `--step`, `--play`/`--rec`/`--rewind`, `--snapshot-save`/`--snapshot-load`.

**Positional disk + kiosk**: `POM2 <disk-image>` mounts the image into the slot its type maps to (`classifyDiskForSlot`: 5.25" Disk II / 800K 3.5" / ProDOS HDV) under the saved profile + slot config, then cold-boots. `--kiosk` adds exclusive full-screen with a chrome-free render path. Kiosk is read-only (no settings writes). An HDV with no HDV/SmartPort card in the saved config auto-plugs a `ProDOSHardDiskCard` into a free slot. Pinned: `cli_kiosk_test`.

## Version string locations

Current release: **v0.6**. Bump in:

- `main.cpp` (initial window title + console banner)
- `MainWindow_Slots.cpp` (runtime title — overrides main.cpp's once the profile resolves)
- `MainWindow.cpp` (About dialog)
- `CMakeLists.txt` (`project(... VERSION x.y ...)`)
- `README.md` (status section)
