# CLAUDE.md

Orientation **always-loaded index** — keep terse, defer detail to other docs.

- `README.md` — user walkthrough (build, profiles, ROM/disk placement, keys, CLI).
- `DEV.md` — implementation deep-dives (MAME-parity ports, internals, gotchas, pinned tests).
- `TODO.md` — active backlog + MAME↔POM2 parity dashboard.
- `docs/test_corpus.md` — edge-case integration corpus; **[DIX](https://github.com/Fr3nchT0uch/DIX/)** (French Touch anthology) is the priority gold-standard benchmark.
- `docs/lle_vs_hle.md` — abstraction level per subsystem (silicon vs contract), the HLE seams, and the rule POM2 follows when picking a level.
- `docs/printer_plan.md` — dot-matrix printer gap analysis vs `web-a2e` + phased plan (character ROMs, screen dump, more heads).
- `docs/PERFORMANCE.md` — core profile (callgrind recipe + `pom2_bench`), the optimisations already done and **why**, and the PGO/LTO build recipe. Read before "optimising" anything on the hot path.
- `CHANGELOG.md` — resolved items + the **why** behind non-obvious fixes.
- `docs/releases/v<x.y>.md` — the release notes for that tag, written by hand. The publish job fetches the file matching the tag and uses it as the GitHub Release body (generated commit list only as fallback).

**Conventions**:

- **One concern per file** — each `.cpp/.h` pair owns one subsystem.
- **MAME = source of truth** — when porting hardware, cite the MAME file + line range in a comment and pin with a smoke test under `tests/`.
- **`emuCycles` everywhere** — CPU → audio/UI events carry a CPU-cycle stamp, not wall-clock. Disk-turbo (~60×) collapses wall-clock gaps to zero across an audio-buffer tick. Example: `FloppySoundDevice::drainCommands` consumes the stamp from `DiskIICard::seekPhaseW`.
- **Never hold `stateMutex` across file I/O** — the lock is taken by the CPU
  worker every 4096-cycle chunk *and* by the UI thread to paint every frame, so
  slow work inside it freezes the machine and the window together, cancel
  button included. Mount media through the two-phase form (`MediaMount.h`:
  read + decode unlocked, take the lock only to swap the finished object in).
  Measured floor for the naive form: 12.8 ms to read a 32 MB image on a warm
  cache, 30.1 ms for a 4 MB write + one `fsync`, against a 20 ms PAL frame.
  Block devices go through the same shape (`Block512Backing::readImageFile` +
  `adoptImage`, wrapped by `pom2::mountBlockCard`): 32 MiB HDV mounts hold the
  lock for **0 ms**. One documented exception survives, in
  `MainWindow_Slots.cpp`'s profile-switch remount, where atomicity against the
  AI server outranks latency and the CPU worker is stopped anyway.
- **Every long-lived thread wears an exception barrier** — an exception
  escaping a `std::thread` callable calls `std::terminate()`, killing the
  process with no log line: a crash the user cannot report and you cannot
  diagnose. Spawn through `pom2::guardedThread(tag, fn)` or wrap the body in
  `pom2::runGuarded` (`ThreadGuard.h`). Pinned by `thread_guard`, which forks a
  child so the regression fails a test instead of killing the runner.
- **Reach the emulated state through the lock** — `controller->lockState()` returns a `pom2::StateAccess` RAII handle that hands back `Memory` and the CPU, so `st.memory()` cannot be written without having taken `stateMutex`. Bare `stateMutex()` is reserved for mutual exclusion that touches neither (serialising a card pointer against a profile switch, say). It is **non-recursive** — a helper called from both locked and unlocked callers takes a `const pom2::StateAccess&` and lets the caller prove ownership (`MainWindow::plugSlotsFromSettings`). Only three things may reach `memory()`/`cpu()` unlocked: code running before the CPU worker starts, the keyboard latch / paste queue (own `Memory::kbMutex`) and atomics, and UI-thread-confined SlotBus *topology* reads.
- **Docs in English** — English is the reference language for all Markdown docs (README, CLAUDE, DEV, TODO, CHANGELOG, `docs/`). Write new docs and edits in English; the historical snapshots under `docs/archive/` are unmaintained and may still be French.

## Table of contents

- [Build & run](#build--run)
- [Subsystem map](#subsystem-map)
- [Memory map](#memory-map)
- [System profiles](#system-profiles)
- [Reset architecture](#reset-architecture)
- [CLI](#cli)
- [Version string locations](#version-string-locations)
- [Package payload](#package-payload)

## Build & run

```bash
./setup_imgui.sh             # one-time deps + clones imgui/ at the pinned commit
cd build && cmake .. && make # → build/POM2
./run_emulator.sh            # cwd = repo root so roms/ probes resolve
```

**Dear ImGui is pinned** — `imgui_pin.env` is the single source of truth (repo + branch + commit), shared by `setup_imgui.sh`, `tools/fetch_imgui_pinned.sh` (the release + packaging path) and both CI jobs. POM2 requires the **`docking`** branch: the DockSpace that hosts its ~36 panels needs `ImGuiConfigFlags_DockingEnable`, which does not exist on `master`. The branch is force-pushed on every upstream rebase, hence the commit pin. Multi-viewport stays off. → [DEV § Docking](DEV.md#docking--layout-presets)

`POM2_CPU_CLOCK_HZ = 1 022 727` (14.31818 MHz / 14). UI 60 Hz; CPU worker runs `cyclesPerFrame=17045` per tick. Single `stateMutex` guards CPU + Memory.

Release packages ship the full `roms/` tree; a source build uses the same in-repo dumps. Default profile is **`Apple //e Enhanced PAL`** (`Apple ][+` only when no //e ROM is found); see [System profiles](#system-profiles) for ROM probe order and the default slot map.

## Subsystem map

Detail lives in `DEV.md`. This map is the index — file pair + one-line note + DEV anchor.

| Subsystem | Files | DEV anchor |
|---|---|---|
| 6502 / 65C02 / Rockwell / WDC | `M6502.h/.cpp` | [§ CPU](DEV.md#cpu) |
| Z80 core (standalone, zexall-clean) | `Z80.h/.cpp` | [§ Z80 core](DEV.md#z80-core-z80hcpp--softcardcpm-phase-1) |
| Microsoft SoftCard (Z80 DMA bus master) — catalog `softcard` | `SoftCardZ80.h/.cpp` | [§ SoftCard Z80](DEV.md#softcard-z80-softcardz80hcpp--cpm-phase-2) |
| Memory + IIe paging + RamWorks | `Memory.h/.cpp` | [§ Memory](DEV.md#memory) |
| Display (HGR / DHGR / 80-col) | `Apple2Display.h/.cpp` | [§ Display](DEV.md#display) |
| Composite NTSC shader (OpenEmulator-style) | `NtscPostProcessor.*`, `OpenGLShader.*` | [§ Composite NTSC shader](DEV.md#composite-ntsc-shader-colorcompositeoe) |
| AppleWin NTSC (CPU IIR-LUT) | `AppleWinNtsc.h/.cpp` | [§ AppleWin NTSC](DEV.md#applewin-ntsc-colorapplewin) |
| CRT glass pass (scanlines, mask, vignette, phosphor γ) | `CrtEffectStack.*` | [§ CRT effect stack](DEV.md#universal-crt-effect-stack-crteffectstack) |
| Le Chat Mauve RGB (Eve-class; //c DB-15 adapter) — catalog `chatmauve` | `LeChatMauveCard.h/.cpp`, `LeChatMauve_ImGui.*` | [§ Display](DEV.md#display) |
| Speaker / Cassette / Audio bus (stereo; mono sources are pan-placed) | `AudioDevice.*`, `SpeakerDevice.*`, `CassetteDevice.*` | [§ Audio](DEV.md#audio), [§ Stereo bus](DEV.md#stereo-bus-2026-08-01) |
| Mockingboard A/C + Sound II | `Mockingboard.h/.cpp` + `Via6522.h` + `Ay3_8910.h` + `AyPsgSynth.h` | [§ Mockingboard](DEV.md#mockingboard), [§ Sound II](DEV.md#mockingboardcard-variantsoundii) |
| Phasor (2×VIA, 4×AY) | `PhasorCard.h/.cpp` + `AyPsgSynth.h` | [§ Phasor](DEV.md#phasor-applied-engineering) |
| AY PSG audio-thread synth (shared: generators, mixer, band-limiting, DC) | `AyPsgSynth.h` | [§ Mockingboard](DEV.md#mockingboard) |
| SSI263 speech chip | `Ssi263.h/.cpp` + `Ssi263PhonemeData.h/.cpp` | [§ SSI263](DEV.md#ssi263--echo-street-electronics) |
| Cricket / Echo (SSI263) — catalog `echoplus` | `EchoPlusCard.h/.cpp` | [§ EchoPlusCard](DEV.md#echopluscard-cricket--ssi263-class--catalog-echoplus) |
| Echo+ (TMS5220 + 2×AY, scaffold) — catalog `echoplus_tms` | `EchoPlusTMS5220Card.h/.cpp` | [§ EchoPlusTMS5220Card](DEV.md#echoplustms5220card) |
| Grappler+ (Orange Micro, ROM-gated) — catalog `grappler` | `GrapplerCard.h/.cpp` | [§ Grappler+](DEV.md#grappler-orange-micro) |
| Floppy mechanical sounds (MAME WAV samples) | `FloppySoundDevice.h/.cpp` | [§ Floppy sounds](DEV.md#floppy-mechanical-sounds) |
| Printer mechanical sounds (**synthesised** — no sample set exists) | `PrinterSoundDevice.*`, `PrinterSoundSink.h` | [§ Printer sound](DEV.md#printer-sound-printersounddevice) |
| Slot bus + wire-OR IRQ | `SlotBus.h`, `SlotPeripheral.h` | [§ Slot bus](DEV.md#slot-bus--irq-aggregation) |
| DiskImage / DiskIICard / Snapshot | `DiskImage.*`, `DiskIICard.*`, `SnapshotIO.*` | [§ Storage](DEV.md#storage) |
| Atomic **+ durable** file commit — every write-back goes through it | `AtomicFileReplace.h` | [§ Write-back commit](DEV.md#how-a-media-write-back-commits-atomicfilereplaceh) |
| Two-phase media mount — keeps the file read off `stateMutex` | `MediaMount.h/.cpp` | [§ Two-phase mount](DEV.md#two-phase-media-mount-mediamounthcpp) |
| Machine snapshot + Rewind ring (MicroM8-style) | `MachineSnapshot.*`, `RewindBuffer.*` | [§ Rewind](DEV.md#rewind--time-travel) |
| 3D voxel view (MicroM8-style) + camera math | `Voxel3DRenderer.*`, `Mat4.h` | [§ 3D voxel](DEV.md#3d-voxel-view) |
| ProDOS block backing + HDV cards | `Block512Backing.*`, `ProDOSHardDiskCard.*`, `CffaCard.*`, `AtaBlockDevice.*` | [§ HDV](DEV.md#prodosharddiskcard-hdv--synthetic-block-model), [§ CFFA](DEV.md#cffacard-cffa-20--mame-faithful-ide) |
| ProDOS host folder (a directory served as a volume) | `ProDOSVolume.*`, `ProDOSBlockCard.h` | [§ ProDOS host folder](DEV.md#prodos-host-folder) |
| IWM (//c, //c+, Mac, IIgs) | `IWMDevice.*` | [§ IWM](DEV.md#iwm-c-on-board) |
| SmartPort 3.5" //c+ on-board (`.po`/`.2mg`/`.woz`) | `Disk35Image.*`, `Sony35Drive.*`, `Sony35Gcr.*`, `SmartPortHub.*` | [§ SmartPort 3.5"](DEV.md#smartport-35-stack) |
| SmartPort slot card (Liron-class) | `SmartPortCard.*`, `SmartPort*Unit.*` | [§ SmartPortCard](DEV.md#smartportcard-e-liron-class) |
| Super Serial + telnet | `SuperSerialCard.h/.cpp` | [§ SSC](DEV.md#super-serial-card-slot-2--telnet-bridge) |
| Uthernet I (CS8900A NIC) | `UthernetCard.*`, `Cs8900aDevice.*` | [§ Uthernet I](DEV.md#uthernet-i-cs8900a) |
| Uthernet II (W5100 hardware TCP/IP) | `UthernetIICard.*`, `W5100Device.*` | [§ Uthernet II](DEV.md#uthernet-ii-w5100) |
| Ethernet host transport (libslirp, optional — Linux/macOS) | `NetworkBackend.h`, `SlirpNetworkBackend.*` | [§ Network backends](DEV.md#network-backends) |
| Host sockets (POSIX / Winsock, one compat header) | `SocketCompat.h`, `SocketUtil.h` | [§ Host sockets](DEV.md#host-sockets-posix--winsock) |
| Host serial ports (POSIX termios / Win32 DCB) | `SerialPort.h/.cpp` | [§ Host serial](DEV.md#host-serial-ports-serialport) |
| Helper-process supervision (POSIX fork / Win32 CreateProcess) | `ChildProcess.h/.cpp` | [§ FujiNet](DEV.md#fujinet-sp-over-slip-relay) |
| FujiNet relay (SP-over-SLIP; TCP + USB CDC) — catalog `fujinet` | `FujiNetCard.*`, `SpOverSlipLink.*`, `SpTransport.h`, `SpTcpTransport.cpp`, `SpSerialTransport.cpp`, `SlipFramer.h` | [§ FujiNet](DEV.md#fujinet-sp-over-slip-relay) |
| Printer card (synthetic → spool) | `PrinterCard.h/.cpp` | [§ Printer](DEV.md#printer-card-parallel-synthetic) |
| Grappler+ printer (ROM-gated) | `GrapplerCard.h/.cpp` | [§ Grappler+](DEV.md#grappler-orange-micro) |
| ImageWriter II printer + paper tray (host-side, fed by any printer card, the SSC tap or a FujiNet printer unit) + PDF export | `ImageWriter.*`, `ImageWriterRom.h` (generated), `ImageWriterPdf.*`, `ImageWriter_ImGui.*`, `PrinterFeedCursor.h` | [§ ImageWriter](DEV.md#imagewriter-ii-printer-host-side) |
| Screen dump → printer (synthesised `ESC G` stream) | `PrinterScreenDump.h/.cpp` | [§ Screen dump](DEV.md#screen-dump-printerscreendump) |
| Print history (durable printouts, `printouts/history/`) | `PrinterHistory.h/.cpp` | [§ Print history](DEV.md#print-history-printerhistory) |
| ProDOS clock card | `ClockCard.h/.cpp` | [§ Clock](DEV.md#prodos-clock-card-slot-4) |
| No-Slot Clock (DS1216E, sits under a ROM — no slot used) | `NoSlotClock.h/.cpp` | [§ No-Slot Clock](DEV.md#no-slot-clock-noslotclock--ds1216e-smartwatch) |
| Mouse Card (MAME + AppleWin HLE) + host pointer capture | `MouseCard.*`, `MouseCardAppleWin.*`, `MouseGrab.h` | [§ Mouse](DEV.md#mouse-card), [§ Pointer capture](DEV.md#pointer-capture-mouse-grab--mousegrabh) |
| Joystick / paddles | `JoystickInput.h/.cpp` | [§ Joystick](DEV.md#joystick--paddles) |
| AI control server (HTTP on loopback, opt-in) | `AiControlServer.h/.cpp` | [§ AI control server](DEV.md#ai-control-server-aicontrolserver) |
| UI (ImGui) | `MainWindow.*`, `*_ImGui.*` | [§ UI](DEV.md#ui-imgui) |
| UI theme + DPI/zoom scaling | `Pom2Theme.h/.cpp` | [§ Theme](DEV.md#theme--ui-scaling-pom2theme) |
| Command palette (Ctrl+Shift+P) | `CommandPalette_ImGui.h/.cpp` | [§ Palette](DEV.md#command-palette-commandpalette_imgui) |
| Docking + layout presets | `MainWindow.cpp` (`renderDockSpace`/`applyDockLayout`), `imgui_pin.env` | [§ Docking](DEV.md#docking--layout-presets) |
| HGR/DHGR Paint editor + sprite editor (portable, shared w/ POM1) | `hgrpaint/*`, `hgrsprite/*`, `Pom2HgrPaintHost.*` | [§ Paint editor](DEV.md#hgr--dhgr-paint-editor-hgrpaint-shared-with-pom1) |
| Slot Config + Internal Disks & Media (2 windows) | `MainWindow_Slots.cpp`, `MountableMediaCard.h`, `SlotCardCatalog.h` | [§ Host control](DEV.md#host-control-center-slot-configuration--floppy-emu) |
| ROM inventory panel (present / missing / identity) | `RomStatus_ImGui.*`, `RomCatalog.h` | [§ ROM Status](DEV.md#rom-status-panel) |
| Abstraction levels panel (LLE/HLE per subsystem, live + switchable) | `AbstractionLevels_ImGui.*` | [§ Abstraction Levels](DEV.md#abstraction-levels-panel-lle--hle) |
| Clickable Apple //e keyboard (photo + measured hotspots) | `Keyboard_ImGui.*`, `AppleIIeKeyboardLayout.*` (generated), `AppleKeyLatch.h`, `tools/gen_keyboard_layout.py` | [§ Keyboard panel](DEV.md#apple-e-keyboard-panel) |
| Floppy Emu (BMOW SD/OLED) | `FloppyEmuDevice.*`, `FloppyEmu_ImGui.*` | [§ Floppy Emu](DEV.md#floppy-emu-bmow) |
| Run-control debugger (breakpoints, step, step-over) | `Debugger.h/.cpp`, `Debugger_ImGui.*` | [§ Debugger](DEV.md#debugger-debuggerhcpp-debugger_imgui) |
| Clock & threading | `EmulationController.h/.cpp` | [§ Threading](DEV.md#clock--threading) |
| Thread exception barrier (every long-lived thread) | `ThreadGuard.h` | [§ Threading](DEV.md#thread-exception-barrier-threadguardh) |
| System profiles | `SystemProfile.h/.cpp` | [§ Profiles](DEV.md#profile-switching-internals) |
| CLI | `CliDispatcher.h/.cpp` | [§ CLI](DEV.md#cli-clidispatcher) |
| WebAssembly build | `build_wasm.sh`, `wasm/shell.html` | [§ WASM](DEV.md#webassembly-browser-build) |

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
$C028        //c ROMBANK toggle (decoded across $C020-$C02F on any
             //c-class ROM; alt-firmware reads further require
             `IIcClassProfile::hasAltBank_`). Cassette on II/II+/IIe.
$C030-$C03F  Speaker toggle (any access)
$C050-$C057  Display mode pairs (text/gfx, mixed, page 1/2, lo/hi-res)
$C05E/$C05F  IIe DHGR enable/disable (AN3 pulses → Le Chat Mauve FIFO)
$C061-$C063  Push-buttons (negative when pressed)
$C064-$C067  Paddle inputs (negative while RC discharging)
$C070        Paddle reset latch (mirrored $C070-$C07F)
$C071/3/5/7  RamWorks III aux-bank select (write `data & 0x7F`)
$C078-$C07D  //c IOUDIS mirrors of $C07E/F (even = SET, odd = CLR;
             //c-class writes only)
$C07E/$C07F  IOUDIS SET/CLR (writes effective on //c/c+ only)
$C0A8-$C0AB  SSC ACIA (slot 2)
$C0C0        ThunderClock+ uPD1990AC bit-bang (slot 4)
$C0E0-$C0EF  Disk II soft switches (slot 6 — $C0EC=Q6L, $C0ED=Q6H)
$C0(8+s)X    Per-slot device select (e.g. Phasor mode soft-switch
             $C0(8+s)0..F when a Phasor sits in slot s; a whole
             CS8900A when an Uthernet I sits there; the W5100's
             4-register indirect window — mode / addr-hi / addr-lo /
             data — repeated 4× when it's an Uthernet II)
$C100-$C5FF  Slot ROMs (or IIe internal I/O ROM when INTCXROM=on).
             When MockingboardCard SoundII is in slot s, $Cs40-$Cs4F
             writes shadow into the SSI263 (reads stay VIA); when
             EchoPlusCard is in slot s, $Cs00-$Cs04 routes to its
             SSI263.
$C300-$C3FF  IIe 80-col firmware (internal when SLOTC3ROM=off)
$C400-$C4FF  ProDOS clock card slot ROM
$C600-$C6FF  Disk II boot PROM (341-0027-A embedded; roms/disk2.rom
             overrides)
$C700-$C7FF  Slot 7 ROM (free by default; //c PAL forces Le Chat
             Mauve here, and the CLI prefers it for FujiNet / the
             HDV auto-plug)
$D000-$F7FF  Applesoft BASIC ROM
$F800-$FFFF  Monitor ROM + 6502 vectors ($FFFA-$FFFF)
```

In IIe mode the same map applies but most of `$0000-$BFFF` can route to aux 64 KB under paging switches — see table at top of `Memory.h`.

## System profiles

| Profile | CPU | iieMode | Main ROM probes | Built-in slots (locked in UI) |
|---|---|---|---|---|
| Apple ][ Original (1977)  | NMOS  | off | `apple2o.rom`, `apple2.rom` | — |
| Apple ][+ (1979)          | NMOS  | off | `apple2p.rom`, `apple2.rom` | — |
| Apple //e Unenh. (1983)   | NMOS  | on  | `apple2e_unenh.rom`, `342-0135-b.64.rom`, `apple2e.rom` | — (AUX = ext80) |
| Apple //e Enh. (1985)     | 65C02 | on  | `apple2e.rom` | — (AUX = ext80) |
| Apple //c (1984)          | 65C02 | on  | `apple2c-32Kv0.rom`, `apple2c-16K.rom`, `3420033a.256` | sl1 SSC (printer port) · sl2 SSC (modem port) · sl4 Mouse (AppleWin HLE) · sl5 SmartPort · sl6 Disk II |
| Apple //c Plus (1988)     | 65C02 | on  | `apple2cp.rom`, `apple2c-plus.rom`, `apple2c-32Kv0.rom` | sl1 SSC (printer port) · sl2 SSC (modem port) · sl4 Mouse (AppleWin HLE) · sl5 SmartPort 3.5" · sl6 Disk II (IWM) |
| Apple //e Enh. PAL (50 Hz) | 65C02 | on  | `apple2e.rom` | — (AUX = ext80) · **PAL timing** |
| Apple //c PAL (Le Chat Mauve) | 65C02 | on  | `apple2c-32Kv0.rom`, `apple2c-16K.rom`, `3420033a.256` | same as //c **+ sl7 built-in Le Chat Mauve RGB** (Adaptateur IIc) · **PAL timing** |

Built-in slots force their listed card onto the SlotBus on profile load (overriding `slot_N_card` settings) and grey out their row in Slot Config. Detail → [DEV § Profile switching](DEV.md#profile-switching-internals).

**Fresh-install defaults** — no `system_profile` / `slot_N_card` / `hi_res_mode` keys in `state.cfg`:

| | |
|---|---|
| Profile | `Apple //e Enhanced PAL (50 Hz)` — falls back to `Apple ][+` if no //e ROM resolves |
| Display | `Composite (OpenEmulator)` GPU pipeline |
| sl1 | `grappler` — Grappler+ parallel printer |
| sl2 | `mouseaw` — Mouse, AppleWin HLE |
| sl3 | *empty* — the //e's 80 columns are **internal** ($C300 firmware + AUX ext80 under `iieMode`), never a slot card |
| sl4 | `mockingboard` — Mockingboard A/C |
| sl5 | `smartport35` — SmartPort 3.5" |
| sl6 | `diskii` — Disk II |
| sl7 | `chatmauve` — Le Chat Mauve RGB |

The map lives in `kDefaults[]` in `MainWindow::plugSlotsFromSettings`; any `slot_N_card` key in the settings file wins over it.

**//c-class CPU + Chat Mauve rules.** The //c/+/enhanced-//e/PAL profiles have a **65C02 soldered** (`defaultCpu = CMOS`); an `cpu_mode_override = nmos` is *ignored* there (`resolveCpuMode` clamp) — forcing NMOS made their 65C02 ROMs hit KIL opcodes and freeze. The **Le Chat Mauve RGB** card is the one peripheral allowed on a `noPhysicalSlots` //c (it's the rear DB-15 "Adaptateur IIc", not a slot card): user-pluggable on plain //c/+ via a `{empty, Le Chat Mauve}` Slot-Config combo, and a **fixed sl7 built-in on the //c PAL profile**. Profile-forced slots are no longer persisted to `slot_N_card` on exit (quitting on //c used to clobber the user's //e card config).

**Video standard (NTSC/PAL).** Each profile carries a `VideoStandard` (`CpuClock.h`): NTSC (262 lines, 60 Hz, 1.0227 MHz) for US machines, **PAL (312 lines, ~50 Hz, ~1.0156 MHz)** for the two PAL profiles. The European //c PAL is the machine that took the **Le Chat Mauve** RGB Péritel adapter on its DB-15 port; French Touch / DIX demos are PAL-timed, so their beam-raced effects and Mockingboard-T2 frame sync only land correctly under PAL. MAME oracles for European PAL: **`apple2eefr`** (//e enhanced France) and **`apple2cfr`** / **`apple2c0fr`** (//c France, UniDisk variant) — all 312 vtotal, 50.146 Hz, 14.2375 MHz pixclock. Not the US `apple2ee` / `apple2c`. Note: MAME's //c FR still has no usable 3.5" media path for an 800K `.po`; DIX on MAME stays on `apple2eefr -sl5 superdrive`. `applyProfile` calls `controller->setVideoStandard()`, which sets the worker's 50/60 Hz pacing (`frameIntervalUs`) and the 262/312-line geometry in `Memory` (`pushVideoEventLocked`) + `Apple2Display::frameCycleToPos`. The CPU budget `defaultCyclesPerFrame` (17045 NTSC / 20313 PAL) × refresh = the effective clock. Device *generator* clocks (AY/IWM/SSI263) stay at the NTSC nominal — the 0.7 % delta is an inaudible audio-pitch approximation — but `setVideoStandard` calls `setCpuClock` on **every slot card plus speaker and cassette**: their emuCycles replay cursors and cycles→samples queues starve under a wrong clock, which is audible. Pinned by `pal_timing`. CLI: `--preset iie-pal|iic-pal` (alias `chatmauve`).

**ROM identity check**: when the generic `apple2.rom` fallback resolves (no profile-specific dump present), the loader warns the ROM may not match the selected machine.

Default `cyclesPerFrame` = 17045 for II/II+/IIe/IIc; **//c+ defaults to 68180 (4×)** for its on-board Zip-style accelerator. `$C036` 1 MHz fall-back during disk I/O not modelled (event-driven disk LSS keeps nibbles cycle-correct anyway). `cpu_mode_override = auto|nmos|65c02` (Machine → CPU menu).

**//c+ MIG + IWM**: //c+ alt firmware (bank 1) drives the Apple MIG gate-array at `$CC00-$CCFF` / `$CE00-$CEFF` + IWM at `$C0E0-$C0EF`. The IWM state machine **is** ported (`IWMDevice`, verbatim MAME, incl. the bit-cell read walker and write windows) and the Sony GCR drives exist (`Sony35Drive`, `--35-disk1/2`); what remains unfinished is the //c+ firmware's on-board 3.5" **boot** through them — it does not yet reach a bootable disk (TODO § Backlog § [Storage]). The Liron controller ROM **has been publicly dumped** (BMOW/Yellowstone `LIRONALL.bin`, 4 KB, [github.com/steve-chamberlin/fpga-disk-controller](https://github.com/steve-chamberlin/fpga-disk-controller), with full disassembly; MAME still lists it *WANTED* only because it never ingested the dump) and POM2 ships it as `roms/liron.rom`; the UniDisk 3.5 drive-side 65C02 firmware stays deliberately out of scope. The supported path is the host-served SmartPort block device at built-in slot 5, which boots 3.5"/HDV on all //c-class machines. → [DEV](DEV.md#profile-switching-internals).

**//c-class on-board SmartPort (3.5" + HDV boot)**: the firmware's IWM/Sony GCR boot doesn't reach a bootable disk (see above), and MAME doesn't emulate 3.5"/SmartPort on the plain //c. POM2 boots 3.5" and HDV on //c/+/c+ through a host-served SmartPort block device at built-in slot 5. `Memory::memRead` punches a hole at `$C500-$C5FF` for the SmartPort firmware iff the slot is **armed** + holds media. `bootFromSlot` arms it; every reset disarms it, so the //c ROM's autostart always sees its real `$C500` firmware (avoids the "garbled //c banner" bug). Pinned by `iic_onboard_smartport_smoke`. → [DEV § Storage](DEV.md#c-class-on-board-smartport-35--hdv-boot).

Profile switching is a full cold reset with strict ordering — 14-step (0-13) `applyProfile` sequence detailed in [DEV](DEV.md#profile-switching-internals).

CLI `--preset` triggers the same path. Canonical keys: `ii`, `ii+`, `iie-u`, `iie`, `iic`, `iic+`, `iie-pal`, `iic-pal`; `parsePresetName` (`CliDispatcher.cpp`) carries the full alias table (`apple2`, `apple2plus`, `apple2e`, `apple2c`, `apple2cplus`, `//e`, `//c`, `//c+`, `chatmauve`, …).

## Reset architecture

Three classes of reset (+ one boot shortcut), mirroring MAME's split:

| POM2 verb | Trigger | Behaviour | MAME analogue |
|---|---|---|---|
| `softReset()` | F11, toolbar, AI `/reset?kind=soft` | RAM survives. IIe-class wipes full MMU/IOU/LC; II/II+ leaves LC + display untouched (kbd strobe only). CPU `SP -= 3`, I flag set, PC = $FFFC. | `reset_w(true→false)` |
| `hardReset()` | F12, toolbar, AI `/reset?kind=hard`, `applyProfile` step 11 | RAM survives; CPU additionally zeros A/X/Y, SP = $FF, P = $24 \| I. **II/II+**: display/LC preserved (same as soft reset). **IIe-class**: full MMU wipe → TEXT. | `reset_w` + register wipe |
| `coldBoot()` | Toolbar power, AI `/reset?kind=cold`, MainWindow ctor, "Insert + boot" | Wipes user RAM + LC + aux with `00 FF 00 FF…` MAME pattern; full reset; hard reset CPU. | `machine_start` + `machine_reset` |
| `bootFromSlot(N)` | HDV / SmartPort / Disk II Library "Boot" | coldBoot-equivalent (inlined; also arms the //c on-board SmartPort iff N=5) then `PC = $C000 + N*256` after validating JSR-dispatch trio ($Cn01=$20, $Cn03=$00, $Cn05=$03 — Apple II Ref Manual Appx C). $Cn07 NOT validated (HDV cards have $Cn07=$01). Mismatch → falls back to `coldBoot`. | Synthetic shortcut |

Keyboard wiring:

- **Left Alt = Open-Apple** → $C061 bit 7
- **Right Alt = Solid-Apple** → $C062 bit 7 — both wires have **two** sources (host Alt + the on-screen //e keyboard's latches), held apart and OR'd in `AppleKeyLatch.h`; a source that assigned the wire directly released the other one. Pinned by `apple_key_latch`.
- **Ctrl+Alt+F = full screen ⇄ windowed** (kiosk toggle — see CLI section). **F10** does the same; the chord exists because F10 is swallowed by the window manager on several desktops.
- **Ctrl+Alt+G = capture / release the host pointer** for the Mouse Card (a middle click toggles it too; a left click never captures; policy in `MouseGrab.h`) → [DEV § Pointer capture](DEV.md#pointer-capture-mouse-grab--mousegrabh)
- F9 / F10 / F11 / F12 / Ctrl+Alt+F / Ctrl+Alt+G / Ctrl+Shift+P / Left Alt / Right Alt routed unconditionally (even when ImGui captures keyboard focus).

## CLI

`CliDispatcher` (parser, no `EmulationController` dep) + `CliRunner` (Phase-C runner). Three phases: parse → pre-boot (preset / ROM / display / speed) → post-boot Phase C (deferred actions: `--load addr:file`, `--snapshot-load`/`--snapshot-save`, tape ops, paste, run, step).

Flags: `--preset ii|ii+|iie-u|iie|iic|iic+|iie-pal|iic-pal`, `--speed`, `--cpu-max`, `--display`, `--tape`, `--save-tape`/`--save-tape-format aci|wav`, `--35-disk1 path`/`--35-disk2 path` (//c+ Sony 3.5"), `--load addr:file`, `--run`, `--paste`, `--step`, `--play`/`--rec`/`--rewind`, `--snapshot-save`/`--snapshot-load`, `--fujinet[=PORT]`/`--fujinet-serial[=DEV]`/`--fujinet-slot N`, `--rgb-card-invert-bit7[=on|off]`, `--kiosk`. `printUsage()` in `CliDispatcher.cpp` is the source of truth.

**Kiosk is a runtime mode, not just a flag**: `MainWindow::toggleKioskMode()`
(Ctrl+Alt+F, F10, View menu, `view.kiosk` palette command, or the in-kiosk menu's
EXIT KIOSK action) moves the GLFW window between exclusive full-screen and
its saved windowed geometry and flips `kiosk_`. The machine is untouched —
kiosk is only windowing + the chrome-free render path + suppressed settings
writes — so no snapshot round-trip is involved. A session LAUNCHED with
`--kiosk` stays settings-read-only for its whole life even after toggling to
the GUI (`settingsReadOnly()`), preserving the documented "a kiosk session
can't disturb your desktop setup" promise.

**Positional disk + kiosk**: `POM2 <disk-image>` mounts the image into the slot its type maps to (`classifyDiskForSlot`: 5.25" Disk II / 800K 3.5" / ProDOS HDV) under the saved profile + slot config, then cold-boots. `--kiosk` adds exclusive full-screen with a chrome-free render path. Kiosk is read-only (no settings writes). An HDV with no HDV/SmartPort card in the saved config auto-plugs a `ProDOSHardDiskCard` into a free slot. Pinned: `cli_kiosk`.

## Version string locations

Current release: **v0.8.3**. **Single source of truth = `CMakeLists.txt`
`project(pom2_imgui VERSION x.y.z ...)`.** A `configure_file` expands it into
`build/generated/Version.h` (from `src/Version.h.in`); all C++ pulls the
version from there (`POM2_VERSION` / `POM2_VERSION_STRING` macros + `pom2::
kVersion[String]`). Consumers — `main.cpp` (banner + window title),
`MainWindow_Slots.cpp` (runtime title), `MainWindow.cpp` (About) — no longer
hard-code it. Bumping `project(VERSION)` re-runs CMake and rebuilds them.

To bump a release, edit **`CMakeLists.txt`** then the prose-only files that
cannot `#include` the header:

- `CMakeLists.txt` (`project(... VERSION x.y.z ...)`) — **drives all code**
- `README.md` (title, and the package names in § Download) — manual
- `CLAUDE.md` (this line) — manual
- `docs/releases/v<x.y>.md` — the release notes; the publish job reads the
  file whose name matches the tag, so a missing one silently downgrades the
  Release body to a generated commit list

## Package payload

**`packaging/bundle.manifest` is the single list of what ships inside a
package** — `roms/` + `fonts/` + the About photo, `wasm floppyemu` as the
browser-only extra (it holds the demo's boot disk), a `deny` list
(`disks_5.4`, `hdv`, `snapshots`, …) that must never appear in one, and a
`denyglob` archive filter (`*.zip`, `*.7z`, …) applied inside copied dirs. CMake
parses it at configure time for **both** the `install()` rules and the WASM
`--preload-file` arguments; `packaging/stage_data.sh` parses it for the macOS
`.app` and the Windows `.zip`, and `--verify` is the guard every release job
runs against its staged tree. Pinned by `bundle_manifest`. Adding an asset
means editing the manifest, nothing else. → [DEV § Package payload](DEV.md#package-payload--packagingbundlemanifest)
