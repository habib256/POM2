# DEV.md

Deep emulator implementation notes for POM2: MAME-parity ports, port
internals, format gotchas, pinned smoke tests. Quick orientation,
build instructions, memory map, and profile table → `CLAUDE.md`. User
walkthrough → `README.md`.

## Table of contents

- [CPU](#cpu)
- [Memory](#memory)
  - [`Memory::loadAppleIIRom` dump shapes](#memoryloadappleiirom-dump-shapes)
  - [Memory dispatch](#memory-dispatch)
  - [IIe paging](#iie-paging)
  - [RamWorks III](#ramworks-iii)
  - [Soft switches](#soft-switches)
  - [Text/HGR row interleave](#textgr-row-interleave)
  - [Clock & threading](#clock--threading)
  - [Keyboard](#keyboard)
- [Display](#display)
  - [DHGR](#dhgr)
  - [80-col text](#80-col-text)
  - [Test framework gotcha](#test-framework-gotcha)
- [Audio](#audio)
  - [Speaker](#speaker)
  - [Cassette](#cassette)
  - [Mockingboard](#mockingboard)
  - [Floppy mechanical sounds](#floppy-mechanical-sounds)
- [Slot bus & IRQ aggregation](#slot-bus--irq-aggregation)
  - [IRQ wire-OR](#irq-wire-or)
  - [`SlotPeripheral::assertIrq` API](#slotperipheralassertirq-api)
- [Storage](#storage)
  - [DiskImage](#diskimage)
  - [Format detection](#format-detection)
  - [`.woz`](#woz)
  - [WOZ2 `optimal_bit_timing`](#woz2-optimal_bit_timing)
  - [DiskIICard (slot 6)](#diskiicard-slot-6)
  - [DiskII multi-instances (option C)](#diskii-multi-instances-option-c)
  - [Two read paths](#two-read-paths)
  - [Bit-stream expansion](#bit-stream-expansion)
  - [Flux-event view](#flux-event-view)
  - [SmartPortCard (//e Liron-class)](#smartportcard-e-liron-class)
  - [3.5" mechanical sounds](#35-mechanical-sounds)
  - [ProDOS host folder](#prodos-host-folder)
  - [Snapshot](#snapshot)
- [Peripherals](#peripherals)
  - [Super Serial Card (slot 2) + telnet bridge](#super-serial-card-slot-2--telnet-bridge)
  - [ProDOS clock card (slot 4)](#prodos-clock-card-slot-4)
  - [Mouse Card](#mouse-card)
  - [Joystick / paddles](#joystick--paddles)
- [UI (ImGui)](#ui-imgui)
  - [MainWindow Pimpl-light](#mainwindow-pimpl-light)
- [Profile switching internals](#profile-switching-internals)
- [CLI (`CliDispatcher`)](#cli-clidispatcher)

## CPU

### `M6502`

Full NMOS 6502 + common 65C02 (STZ/BRA/INA/DEA/PHX-PLY/BIT-imm/TSB/TRB/
JMP (abs,X), zp-indirect for ORA/AND/EOR/ADC/STA/LDA/CMP/SBC) + Rockwell
RMB/SMB/BBR/BBS + WDC WAI/STP (PC parks, IRQ wakes). Klaus Dormann clean.
Pinned: `cmos_6502_smoke_test.cpp`, `klaus_65c02_extended_test.cpp`
(PASSES @ `$24F1`). `setProgramCounter()` is the Klaus harness back-door.

**Why all this**: 65C02-targeted ProDOS / `.2mg` software hits `$9C`
(STZ abs) or `$B2` (LDA (zp)) on first instruction. Rockwell `$FF` (BBS7)
matters because `$FF` is canonical empty-slot ROM fill; mis-decoding as
1-byte NOP drifts PC through slot 2/3/4 and traps unpredictably.

**65C02 reserved NOPs**: column-2 `$02 $22 $42 $62 $82 $C2 $E2` are
2-byte NOPs (`Unoff2`) on 65C02 but KIL halts on NMOS. CMOS table is
default; `setCpuMode(NMOS)` re-overrides the four formerly-KIL entries
($02/$22/$42/$62) back to halt. `$0B $2B $EB` are 1-byte NOPs on 65C02.
NMOS undocumented ANC/SBC-imm not modelled (left as NOPs).

## Memory

### `Memory::loadAppleIIRom` dump shapes

Accepts two //e dump shapes:

- **16 KB**: `$C000-$FFFF` directly (MAME/AppleWin layout).
- **32 KB**: "system + video" combined. 16 KB firmware at file offsets
  `0x4000-0x7FFF`; lower 16 KB is video/charset (loaded via
  `loadCharRom`). Must slice and feed the upper half through the 16 KB
  IIe split path so `internalIORom` gets populated. Generic best-effort
  branch loads linearly at `$8000` and leaves `$C100-$CFFF` empty when
  `INTCXROM=on` → //e firmware crashes before slot 6 boot PROM.

### Memory dispatch

`memRead`/`memWrite` route every `$C000-$C07F` access through
`softSwitchAccess()`; RAM bypasses (cheap). Reset vector defaults to
`$F800`. ROM regions (`$C100-$C7FF`, `$D000-$FFFF` when LC RAM not
write-enabled) reject writes silently.

### IIe paging

`isIIE()`. `setIIEMode(true)` MUST be called BEFORE `loadAppleIIRom` —
loader's IIe-vs-II+ split depends on the flag. Adds aux 64 KB, 4 KB
`internalIORom` for `$C100-$CFFF`, aux LC bank trio. Switches at
`$C000-$C00F` update an `iieMemMode` bitmask; routing per-range (ALTZP
`$0000-$01FF`, RAMRD/WRT `$0200-$BFFF`, 80STORE+PAGE2 swap `$0400-$07FF`
and `$2000-$3FFF` when HIRES). All IIe paths gated behind `iieMode` —
II+ untouched. Pinned: `iie_memory_smoke_test.cpp`.

### RamWorks III

`setRamWorksBanks(N)`. Applied Engineering aux-slot RAM expansion.
Verbatim port of MAME `src/devices/bus/a2bus/a2eramworks3.cpp`. Tiers:
1 (stock 64K), 4 (256K), 8 (512K), 16 (1M), 48 (3M), 128 (8M, MAME
cap line 99-107). Bank index held in `ramWorksBank_`; bank 0 = standard
IIe aux (regression-safe).

**Bus protocol** (MAME line 108-115): writes to `$C0n1/3/5/7`
(predicate `(low & 0x09) == 0x01` over `$C070-$C07F`) latch
`bank = data & 0x7F`. The same accesses still pulse the paddle one-shot
mirror — they share the bus. Reads of those addresses do nothing
RamWorks-side (paddle latch + floating bus only). Reset → bank 0
(MAME `device_reset` line 67).

**Storage** is `ramWorksBacking_`, one 80 KB slot per bank
(`kRamWorksBankStride = 0x10000 + 0x1000 + 0x1000 + 0x2000` = main aux
+ LC bank1 + LC bank2 + LC high). The four visible aux* arrays always
hold the active bank — kept at fixed addresses so `Apple2Display`
caches `auxData()` once and never invalidates. `ramWorksSwapToBank`
memcpys visible→backing[prev] then backing[curr]→visible. ~80 KB per
switch; ProDOS /RAM driver does ~100s/sec → negligible cost.

**Bank clamp**: `(data & 0x7F) % ramWorksBanks_`. MAME deliberately
does NOT clamp (it always allocates 8 MB and reads garbage from
unpopulated slots — UB-adjacent in C++); we wrap to bound backing
access. A real RamWorks III with fewer than 128 banks installed has
chip-select aliasing that produces the same visible behaviour.

**IIe-only**: `setIIEMode(false)` releases the backing
(`ramWorksBacking_.clear() + shrink_to_fit()`). The dispatch in
`softSwitchAccess` is gated on `iieMode && ramWorksBanks_ > 1`, so
plain II/II+ falls through to the paddle-reset mirror unchanged.

**Setting**: `ramworks_banks` int in settings (default 1 = stock).
Wired in `MainWindow::applyProfile` between `setIIEMode(true)` and
`loadAppleIIRom`. Pinned: `ramworks_smoke_test.cpp`.

### Soft switches

Toggled by *either* read or write. `$C030` toggles speaker on **every**
access in `$C030-$C03F` (alias decoded). `$C061-$C067` are paddles +
buttons on II/II+ — **NOT** cassette aliases (only `$C020`/`$C060` are).

### Text/HGR row interleave

Woz DRAM-refresh trick. Formulae in `Apple2Display.cpp`:

- text: `addr = base + 0x80*(y%8) + 0x28*(y/8)`
- HGR:  `addr = base + 0x400*(y%8) + 0x80*((y/8)%8) + 0x28*(y/64)`

This is why sequential writes to `$0400` don't render line-by-line.

### Clock & threading

`POM2_CPU_CLOCK_HZ = 1 022 727` (14.31818 MHz / 14). 65-cycle "long
cycle" TV alignment **not** modelled. Three modes in
`EmulationController`: **Stopped** (50 ms idle), **Running**
(`cyclesPerFrame` per 60 Hz tick), **Step** (one instruction then
Stopped). `M6502::run(maxCycles)` returns *actual* cycles — worker
passes that to `Memory::advanceCycles()` so paddle RC stays synced.
Single `stateMutex` guards CPU+Memory; UI takes it briefly each frame.

### Keyboard

Latch + strobe under `kbMutex`. UI thread `queueKey()` sets strobe high.
CPU reads `$C000` via `softSwitchAccess()` (snapshots under same mutex).
Strobe stays high until `$C010` read/write.

## Display

Pure software renderer into 280×192 (or 560×192 in IIe 80-col) RGBA.
Reads `Memory::getDisplayState()` (mutex copy) + flat RAM. Owns no GL —
UI uploads via `glTex(Sub)Image2D`. Built-in 5×7 ASCII font when no
charset ROM. Text flash via `frame_number() & 0x10` (MAME parity).

Seven `HiResMode`:

- `ColorNTSC` — 14 KB LUT `(parity<<8)|byte`, 39 seam fix-ups, glow
  (MAME `composite_color_mode=0`).
- `ColorCompMedium` (=1), `ColorComp4Bit` (=2, no artifact).
- `ChatMauveRGB` — RGB-card decode; only with `LeChatMauveCard` plugged.
- `MonoWhite` / `MonoGreen` (P31) / `MonoAmber` (history-buffer lerp).

### DHGR

(IIe, `eightyCol && hiRes && dhgr && !textMode`)

`renderDhgr` interleaves aux (dots `c*14..+6`) with main (`+7..+13`) per
byte → 560-dot stream. Three color paths, all matching MAME
`apple/apple2video.cpp`:

- **`ColorNTSC`** — composite artifact: 7-bit sliding window over 560
  dots → `kArtifactColorLut[128]` (shared with HGR) →
  `rotl4b(value, absX+1)` → 4-bit lo-res palette index. `+1` matches
  MAME's `is_80_column=1` in `render_line_artifact_color`. Per-pixel
  decode (560 lookups/scanline).
- **`ChatMauveRGB`** — 4-dot block → raw nibble (bit 0 = leftmost) →
  `rotl4(n, 1)` (MAME Video-7 `dhgr_update`) → `kChatMauveLoResPalette`.
  Palette verbatim from AppleWin `RGBMonitor.cpp::PaletteRGB_Feline`
  (empirical capture; idx 5 = olive-gray, idx 10 = mauve-gray). MAME's
  Video-7 collapses 5≡10 — we follow AppleWin so the "two distinct
  grays" trademark survives.
- **`Mono*`** — luminance × tint; persistence sized for 280-wide HGR so
  DHGR mono renders without afterglow.

Mixed = DHGR top 160 + 80-col text bottom 4 rows. Pinned:
`dhgr_render_smoke_test.cpp`.

### 80-col text

(IIe) Aux RAM (cells 0,2,…,78) interleaved with main (1,3,…,79) into
560-wide frame. Mixed (HIRES+80COL+MIXED) renders HGR top 20 rows
doubled into 560-wide, overlays 80-col rows 20..23. ALTCHAR plumbed but
no-op against built-in fallback (real charset ROM would consult second
2 KB bank).

### Test framework gotcha

Tests inherit parent's `-O3 -DNDEBUG` → would silently strip every
`assert()`. `tests/CMakeLists.txt` adds `-UNDEBUG` so asserts actually
run. Without that, every smoke test prints "OK" regardless.

## Audio

`AudioDevice`: miniaudio mono float32. **OS-negotiated sample rate**
(often 48 kHz on Apple Silicon even when 44.1 requested) — cycle-driven
sources MUST query `getActualSampleRate()` or playback drifts by the
rate ratio.

### Speaker

`SpeakerDevice` (`AudioSource`). Verbatim port of MAME
`spkrdev.cpp:74-327`. CPU records each `$C030-$C03F` toggle with
**sub-instruction timestamp**
(`cycleCounter + cpu->getCurrentInstructionCycles()`) into 16 K ring.
Audio thread: rectangle-area integration → 4× oversample → 64-tap
windowed sinc (cutoff sr/4) → 0.995-pole DC blocker
(`y[n] = x[n] - x[n-1] + 0.995*y[n-1]`). Auto catch-up if drain > 100 ms.

### Cassette

`CassetteDevice`. `$C020` output toggle / `$C060` input comparator sign.
Separate `AudioSource` so tape loads click through speakers.
`CassetteDeck_ImGui` uses Font Awesome (`fonts/fa-solid-900.ttf`);
falls back to `?` glyphs if missing.

### Mockingboard

(slot 4 by convention) Sweet Microsystems shape: two 6522 VIAs each
driving an AY-3-8910 PSG. No ROM. **VIAs decoded in slot ROM window**
(`$Cn00-$Cn0F` = VIA #1, `$Cn80-$Cn8F` = VIA #2) — NOT device-select
range, hence the `slotRomWrite` callback.

VIA → AY wiring (Sweet Microsystems, AppleWin `Mockingboard.cpp:193`):

```
Port A       → AY data bus (D0..D7)
Port B bit 0 → AY BC1
Port B bit 1 → AY BDIR
Port B bit 2 → AY /RESET (active low; 1 = chip running)
```

{BDIR,BC1}: 00=INACTIVE, 01=READ, 10=WRITE, 11=LATCH-ADDR. Drivers emit
PB = `$07` (LATCH) → `$04` (INACTIVE) → `$06` (WRITE) → `$04`. PB2
stays high — `/RESET` only on init pulses (PB = `$00`). **Gotcha**:
earlier versions had PB0=/RESET and PB2=BC1 inverted; every INACTIVE
looked like /RESET, wiping the AY bank → silent Nox Archaist / Ultima
IV / Total Replay. Fixed 2026-05-14 (`9177cb0`). Pinned by
`mockingboard_smoke_test.cpp::testAyRegisterWrite`.

**6522 subset**: A/B + DDR, T1 (latch + counter, one-shot + continuous),
IFR/IER (T1 bits 6/7; bit 7 dynamic from `ifr & ier & 0x7F`). T1CL read
clears `IFR.T1`. IER bit 7 set-vs-clear (`$C0` enables, `$40` disables).
T2/SR/PCR/CA1/CB1 not modelled (music drivers use T1 only).

**AY-3-8910 synthesis** runs on audio thread inside inner `AudioSrc`.
CPU updates regs under `mtx`; callback snapshots both banks (32 B),
releases, synthesises lock-free. Tone counters = float phase
accumulators at `clockHz/16/sampleRate`; 17-bit LFSR `x^17 + x^14 + 1`;
envelope 32 steps with R13 shape continue/attack/alternate/hold. Both
chips → mono (real HW is stereo; `AudioDevice` is mono-only).

Each VIA `irqOut() = (ifr & ier & 0x7F) != 0`; OR'd onto slot IRQ. Card
caches combined state so transitions call asserter once.

**Lazy timer sync** (`syncToCpuCycle()`): VIA T1/T2 tick at Φ2 on real
HW; POM2 advances slot peripherals in ~17 045-cycle slices at end of
each CPU run. A `STA T1CH ; ... ; LDA IFR` within one slice would see
stale IFR. Every `slotRomRead/Write` catches the VIAs up to
`cpu_->getCycleCountNow()` first. Without this, **Mockingboard
detection routines that probe T1 (Nox Archaist, Skyfox, Broadside per
AppleWin issue #1175) always fail** — they conclude "no card" because
IFR.T1 hasn't flipped. Music drivers with longer periods worked anyway
(IRQ lands at slice boundary), which is why Ultima IV had been fine.
Pinned: `mockingboard_sync_smoke_test.cpp`.

**Tear-down**: remove `AudioSource` from `AudioDevice` BEFORE
destroying the card (source lives inside it). Persisted:
`mockingboard_volume`, `mockingboard_muted`. Pinned:
`mockingboard_smoke_test.cpp`, `mockingboard_sync_smoke_test.cpp`.

### Floppy mechanical sounds

`FloppySoundDevice`. Port of MAME
`src/devices/imagedev/floppy.cpp::floppy_sound_device` — sample-based
playback of head-step click, motor spin-up/down, insert/eject. 20
source WAVs (10 × 5.25" + 10 × 3.5", ~150+210 KB) vendored in
`roms/floppy_samples/` from `mamedev/mame` master; BSD-3-Clause
(`README.txt`).

**`FloppySoundSink` interface** (header-only, zero deps): `DiskIICard`
calls `sound_->motor()` / `step()` / `click()` through it so the 13
smoke tests linking `DiskIICard.cpp` don't also drag the miniaudio TU.

**Step / seek decision** (MAME parity): `step(newTrack, emuCycles)`
measures the gap since previous step in **emulated CPU cycles** —
mirrors MAME `floppy_sound_device::step` (`floppy.cpp` ~lines
1532-1620) which uses `machine().time()`. Wall-clock audio frames
would be wrong: under POM2's disk turbo (~60× emulated speed) the
boot PROM's full phase sweep lands in one audio buffer, so all
events share the same `audioFrameCounter_` → gap=0 for every step
after the first → fallback to single-click with `stepPos_=0` reset
per event → user hears `step_1_1`'s 5 ms attack repeated buffer
after buffer (buzz / "haché"). Caller (DiskIICard `seekPhaseW`)
passes `cpuCycleTotal`.
- `gap > 100 ms` (`kSeekJoinMs`) → single-step click (`525_step_1_1`).
- `gap ≤ 100 ms` → seek mode: pick the seek sample whose nominal
  cadence is closest to the gap (2 / 6 / 12 / 20 ms), pitch-scale
  (`pitch = nominal_ms / gap_ms`), loop.
- No step for `kSeekTimeoutMs` → exit seek, fire final `step_1_1` to
  "land" the head.

Floor at 1 ms gap defends `mixLoop` against `INF` rate (`pos += INF`
spins forever, `INF - len == INF` in IEEE 754) and keeps pitch in
[1, 2] for `SEEK_2MS`. Same-cycle events (multiple steps queued at
the same `cpuCycleTotal`, edge case) and the defensive backwards
case both route through the floor.

DOS 3.3 / ProDOS issue 4 phase pulses per track; each fires one
`step()` via `seekPhaseW`. 0→34 seek = ~140 events / ~150 ms — well
into seek-mode territory.

**Wall-clock motor-off hold-off** (turbo decoupling):
`diskTurboWhileMotor` bumps CPU to ~60 MHz during I/O, so the
controller's 1-sec spin-down delay (`motorOffDelay = 1'022'727` cycles
in `DiskIICard::control()` `$C0E8`) becomes ~17 ms wall-clock. Without
compensation, audio thread gets `motor(false)` before spin-up sample
finishes ("click, end click, silence"). `FloppySoundDevice` defers the
audible transition by `kMotorOffHoldMs` (default 800 ms) in **audio
output frames**, not cycles; fresh `motor(true)` cancels the pending
transition. Controller stays source of truth for "motor mechanically
on" (disk reads remain timing-accurate). Pinned:
`floppy_sound_smoke_test.cpp::testRapidMotorTogglePreservesLoop`.

**CPU ↔ audio thread**: mutex-guarded `std::vector<Cmd>` queue. CPU
pushes `MotorOn/MotorOff/Step/Click`; audio thread drains at top of
`fillAudioBuffer`. Events sparse (≤ ~100/s during seeks), so SPSC ring
would be over-engineering. `audioFrameCounter_` atomic, CPU-side reads
for diagnostics only.

**Hook points in `DiskIICard`**:
- `seekPhaseW` end → `step(head/4)` when `head` moved.
- `control()` `$C0E9` (`MODE_IDLE → MODE_ACTIVE`) → `motor(true)`.
  `MODE_DELAY → MODE_ACTIVE` does **not** fire (motor already audibly
  spinning).
- `advanceCycles()` when `motorOffDelay` expires → `motor(false)`. ~1 s
  real delay places the sample at the right moment, not at `$C0E8`.
- `handleSwitchAccess()` (legacy 32-cycle gate) `$C0E8`/`$C0E9` →
  immediate `motor(false/true)` (no DELAY state).
- `insertDisk` / `ejectDisk` → `click()` (`525_step_1_1` at elevated
  gain).

`loadSamples(dir, FormFactor::FF35)` switches to the 3.5" set; no
consumer today (no SmartPort / UniDisk / Liron) but path is wired.
Owned by `EmulationController` (alongside Speaker / Cassette) so the
`AudioDevice` shutdown drains audio thread before destruction —
different from Mockingboard which owns its source via the card.
Persisted: `floppy_sound_volume`, `floppy_sound_muted`. Pinned:
`floppy_sound_smoke_test.cpp` (10 cases, incl. `testRapidStepsNoHang`
and `testSameCycleStepsClampGracefully` for the turbo-batch / same-
cycle pathological inputs).

## Slot bus & IRQ aggregation

`SlotBus` + `SlotPeripheral`, 8 slots. `Memory` routes four windows:

- `$C080-$C0FF` device-select (16 bytes/slot N at `$C080+N*16`; slot 0 =
  LC hook, 1-7 = expansion).
- `$C100-$C7FF` slot ROM (256 bytes/slot 1-7).
- `$C800-$CFFF` shared expansion ROM, owned by whichever slot most
  recently touched `$CnXX`. `$CFFF` deactivates active slot; auto-latch
  on slot-ROM access.

`advanceCycles()` forwards to every plugged card. Ctrl-Reset propagates
`onReset()` to all cards.

### IRQ wire-OR

`M6502::setIrqLine(sourceId, asserted)` — **wire-OR**. The 6502 IRQ pin
is active-low pulled by *any* device; releases only when *all* stop
pulling. 32-bit OR'd contributor mask: slot N (1..7) = bit N, motherboard
VBL = bit 8, legacy `setIRQ(int)` = bit 31. **Previously** each card
called `cpu->setIRQ(0|1)` directly → last-writer-won bug → mixing
IRQ-driven cards (Mockingboard + SSC + Mouse) was unreliable. NMI is
still a single latch (no NMI sources today). Pinned:
`irq_aggregator_smoke_test.cpp`.

### `SlotPeripheral::assertIrq` API

Cards never poke `cpu->setIrqLine` directly. The base class exposes a
protected `assertIrq(bool)` that:

- debounces against an internal `irqAsserted_` cache (idempotent — only
  edges propagate);
- fans out via `SlotBus::forwardSlotIrq(slot, asserted)` to whatever
  `IrqRouter` Memory installed (`Memory::setCpu(cpu)` plants a closure
  that calls `cpu->setIrqLine(slot, asserted)`);
- gets its slot number from `SlotBus::plug()` at attach-time — cards
  don't remember which slot they live in or hold an `M6502*` just for
  IRQs.

`SlotBus::plug()` / `unplug()` / `clear()` auto-release any pending IRQ
contribution before letting the card go — the legacy
`onUnplug()`-must-clear-the-bit dance is gone, which is what made
profile switches leak stuck bits in the first place.

Mockingboard still holds `cpu_` (for `getCycleCountNow()` lazy-sync of
VIA timers, **not** IRQ); Disk II still holds `cpu_` (for sub-
instruction LSS cycle accuracy on Q6L reads). MouseCard and SSC dropped
their `cpu_` entirely. Pinned: `slot_peripheral_irq_smoke_test.cpp`.

## Storage

### DiskImage

143 360-byte 5.25" floppy: `.dsk`/`.do` (DOS 3.3 skew) or `.po` (ProDOS
skew). Pre-nibblized into 35 × 6656-byte tracks. GCR per "Beneath Apple
DOS". Skew tables (physical → logical):

- DOS 3.3: `{0,7,14,6,13,5,12,4,11,3,10,2,9,1,8,15}`
- ProDOS:  `{0,8,1,9,2,10,3,11,4,12,5,13,6,14,7,15}`

Both produce same on-disk layout; only file-offset → physical-sector
mapping differs. Write-back via `saveDirty()` (`.dsk`/`.do`/`.po`/`.nib`
+ `.2mg` envelopes + `.woz`) opt-in via `setWriteBackEnabled(true)`;
default off.

### Format detection

`detectFormat()` + `enum ImageKind`. `loadFile(path)` slurps once,
dispatches by content. Order: MacBinary strip → 2IMG envelope → WOZ
magic → 35×6656 NIB → 35×6384 CNib2 → 143 360-byte sector. Unknown →
false + specific `lastError` (mirrored into the Disk II panel as red
text); no silent fallback. Inspired by AppleWin `DiskImageHelper.cpp`.

- **Skew sniff** (143 360-byte branch): validates a ProDOS vol-dir key
  block at `file[0x404]` (`.po`) vs `file[0xB04]` (`.dsk`), overrides
  extension when only the other position fits. Predicate: `prev=0`,
  plausible `next`, storage_type `$F`, name chars in `A-Z 0-9 .` (kills
  `$Fx`-byte spoofs in a DOS catalog). Catches cc65-Chess.po case
  (`.po` ext, DOS skew). The smoke test fixture probe historically
  missed the `dsk/` subdirectory and silently skipped — fixed in
  `disk_skew_sniff_smoke_test.cpp` to fail-loud on missing fixture and
  probe all of `disks/`, `disks/dsk/`, `disks/woz/`, `disks2/`.
- **2IMG**: 64 B header → format byte (0=DOS, 1=ProDOS, 2=NIB), flags
  (bit 0 = WP, bit 8 or 31 = vol# present), dataOffset, dataLength.
  Raw header + trailer captured into `twoImgHeaderRaw` /
  `twoImgTrailerRaw`; `saveDirty()` re-emits both so the envelope stays
  byte-identical across round-trip.
- **MacBinary**: 128 B prefix from old Mac downloads. AppleWin predicate
  (`b[0]==0`, name length [1..63], terminator + reserved zeros).
  Stripped before any other detection.
- **CNib2** (35×6384): pad to 6656/track on load with `$FF` (sync),
  truncate to 6384 on save. `cnib2Format` gates the truncation.
- **Volume number**: per-image now (2IMG flags or default), threaded
  through `nibblizeTrack(track, sectors, vol, skew)`. Was hardcoded 254.

Pinned: `disk_image_smoke_test.cpp`, `disk_skew_sniff_smoke_test.cpp`,
`disk_2mg_smoke_test.cpp`, `disk_2mg_writeback_smoke_test.cpp`,
`disk_macbinary_smoke_test.cpp`, `disk_cnib2_smoke_test.cpp`,
`disk_refuse_smoke_test.cpp`.

### `.woz`

`isWoz()`. Verbatim port of MAME `src/lib/formats/woz_dsk.cpp`. WOZ
stores raw bit cells — **why .woz disks survive copy protections** that
tweak inter-byte timing (re-encoded `.dsk` synthesises idealised GCR and
loses the signature). WOZ1 (160 × 6656-byte slots, `bit_count` @+6648
u16) and WOZ2 (160 × 8-byte TRK headers, data at `starting_block × 512`,
`bit_count` u32). Bits MSB-first. Each track 0..34 sources bits from
`TMAP[track*4]` (centre quarter-track); sub-quarter-track positions
(Locksmith, David-DOS) not yet preserved.

**Write-back**: `loadWoz()` snapshots file to `wozRaw` + per-quarter-
track `(byteOff, byteLen, bitCount)`; `writeFlux()` splices into
`bitStream[qt]`; `saveDirty()` repacks + zeroes CRC32 (Applesauce "not
computed" sentinel) + rewrites in place. `isWriteProtected()` honours
both user toggle and `INFO.write_protected`. `DiskIICard::insertDisk`
forces `useBitLss=true` when any drive holds WOZ — legacy 32-cycle gate
cannot decode bit cells. Pinned: `woz_load_smoke_test.cpp`,
`woz_writeback_smoke_test.cpp`.

### WOZ2 `optimal_bit_timing`

INFO+39 (units of 125 ns) — bit-cell duration the imager recommends.
Default 32 = 4 µs = standard 5.25" cell at the 2 MHz LSS clock = 8 LSS
cycles per cell. `loadWoz` reads the byte when `info_version >= 2`,
clamps to [8, 64], stores in `DiskImage::optimalBitTiming` (defaults to
32 for WOZ1 and non-WOZ).

`DiskImage::lssCyclesPerCell() = optimalBitTiming / 4`. `expandTrackFlux`
emits each "1" cell at LSS cycle `i*cyc + cyc/2` (centre); `trackPeriod`
returns `bitCellCount * cyc`. A disk mastered at e.g. 40 (5 µs) gets
10 LSS-cycle cells throughout, so the LSS state machine clocks out
nibbles at the captured rate instead of POM2's 4 µs default. The
flux-event count per revolution stays the same — only spacing changes.

Pinned: `woz_bit_timing_smoke_test.cpp` (obt 32 / 40 / 28 + WOZ1
fallback round trip).

### DiskIICard (slot 6)

256-byte P5A boot PROM (`roms/disk2.rom`); PROM autodetects slot via
`JSR $FF58 / TSX / LDA $0100,X` → Apple II main ROM required for boot.
Soft switches `$C0E0-$C0EF`: phases, motor, drive_select, Q6L/Q6H,
Q7L/Q7H.

**Drive switching** via `selectDrive(int)` mirrors MAME
`wozfdc.cpp:264-291` (file moved from `bus/a2bus/` to `machine/` in
MAME tree; line numbers are current as of 2026-05-14 audit). When
motor active: flush old drive's in-flight write (= MAME
`mon_w(true)`), clear the OLD drive's `revolutionStartLssCycle` to
`kNeverRev`, anchor the NEW drive's `revolutionStartLssCycle` to the
current `lssCycle` (= MAME `mon_w(false)` setting
`revolution_start_time = machine().time()`). Each drive owns its own
`DiskImage` + head qtr-track + nibble cursor; phase magnets + motor
shared (matches real silicon). Pinned: `disk_drive2_smoke_test.cpp`.

**Per-drive `revolutionStartLssCycle[2]`**: matches MAME
`floppy_image_device::m_revolution_start_time`. Set on
`lssStart()` (the controller's MODE_IDLE → MODE_ACTIVE transition,
= MAME `mon_w(false)`) and on the new drive in `selectDrive()` when
the controller is already spinning. Cleared to `kNeverRev` on the
old drive in `selectDrive()`, on motor-off-delay expiry
(MODE_DELAY → MODE_IDLE, = MAME `mon_w(true)`), and at controller
reset. The disk's angular position is
`(lssCycle - revolutionStartLssCycle[drive]) mod track_period`,
computed inside `DiskImage::getNextTransition`. This replaced the
old "reset `lssCycle` on every drive swap" hack, which made the
new drive land at an arbitrary modulo-wrapped slot of its track
instead of starting from position 0 of that disk's current
revolution. The hack also surfaced as cc65-Chess.po / Shamus
boot failures on `mario.dsk` where the angular position drift
across consecutive sectors threw the LSS shift register out of
phase by a single cell partway through a data field. Pinned:
`disk_drive2_smoke_test.cpp`, `mame_lss_parity_smoke_test.cpp`.

### DiskII multi-instances (option C)

Since 2026-05-15 `"diskii"` is the only slot-card type allowed in more
than one slot. Lets `//e` users wire `Disk II slot 6 + Disk II slot 4`
for the historical 4-drives-5.25" config without any per-instance ROM
plumbing — both cards load the same `disk2.rom` + optional
`diskii_p6.rom` from `roms/`. Storage is per-card (each `DiskIICard`
owns its 2 drives + LSS state), so the drives don't see each other's
flux.

**Skipping the uniqueness check**: `MainWindow_Slots.cpp::isDuplicate`
returns false unconditionally when the type is `"diskii"`;
`MainWindow.cpp::plugSlotsFromSettings::firstOccurrence` walk also
short-circuits the same way. Every other card stays single-instance
(driver, ROM signature, or settings keys assume exclusivity).

**Primary vs secondary**: `MainWindow` keeps a flat
`std::vector<DiskIICard*> diskCards` populated in slot order ascending
(via `plugDiskII`). `diskCard` (the legacy single pointer) is an
**alias** for `diskCards.empty() ? nullptr : diskCards.front()` — the
lowest-numbered slot wins because `plugSlotsFromSettings` iterates
1..7. Legacy menu paths (AI control attach, top-bar Eject, auto-turbo
governor) still take `diskCard`; the panel render loop iterates
`diskCards`. Same shape for `diskPanels` (a `vector<unique_ptr<...>>`)
+ `diskPanel` alias.

**Per-slot persistence**: `disk_path_slotN` / `disk_writeback_slotN`
keys, one set per plugged card. The **primary** card additionally
writes to the legacy unsuffixed `disk_path` / `disk_writeback` so a
POM2 build older than 2026-05-15 reading the new settings.ini still
sees the disk where it expects. Profile-switch path captures
`savedDiskPaths[slot]` from the live cards before tear-down, then
re-inserts after `plugSlotsFromSettings` rebuilds.

**Per-panel render**: one ImGui window per plugged card, titled
`"Disk II (slot N)"` (snprintf'd from `card->getSlot()`). Cascade
offset of 30 px down-left per index on `FirstUseEver` so secondary
panels don't perfectly overlap the primary. Auto-turbo is global
(`anyMotorOn` across all cards) since the //e bus has one CPU. Boot
button computes `PC := $C000 + (slot << 8)` per card.

**Insert-disk popup routing**: each panel's `Insert .dsk…` button
sets its own `insertDialogOpen` flag. `MainWindow::renderDiskFileDialog`
scans the panels every frame, finds whichever has the flag set, and
latches `diskDialogTargetSlot` so the popup routes the eventual
`insertDisk` to the right card even if the panel pointer churns (rare
profile-switch race).

**IWM wiring**: only the slot-6 `DiskIICard` calls
`card->setIWM(&controller->iwm())`. A second Disk II in slot 4 stays
off the //c+ IWM path — the IWM is the //c+ on-board controller and
only sees slot-6 traffic in real silicon.

### Two read paths

Two read paths share the data register:

- **Bit-level LSS** (default when `roms/diskii_p6.rom` present) —
  verbatim port of MAME `machine/wozfdc.cpp` + flux-event subset of
  `imagedev/floppy.cpp` (originally `bus/a2bus/wozfdc.cpp`; MAME
  refactored the location post-fetch). MAME `cycles` = 2×
  CPU clock. `lssSync(extra)` catches up from persistent `lssCycle` to
  `cyclesLimit = cpuCycleTotal*2 + extra`. PULSE from
  `DiskImage::getNextTransition(track, lssCycle)` (event at LSS cycle
  `cellIdx*8 + 4`, cell centre). Reads of `$C0EC` pass `extra=1` after
  `control()` for read-pipe latency. P6 PROM (Apple 341-0028-A) indexed
  by `(state<<4) | (Q7<<3) | (Q6<<2) | (QA<<1) | (!PULSE)`. ALU
  dispatch: `0x0..0x7` CLR, `0x8/0xC` NOP, `0x9` SL0, `0xA/0xE`
  SR-with-WP, `0xB/0xF` LD from `last_6502_write`, `0xD` SL1. Pinned:
  `diskii_lss_smoke_test.cpp`, `mame_lss_parity_smoke_test.cpp`.
- **Legacy 32-cycle gate** (fallback) — `kCyclesPerNibble = 32`; nibble
  every 32 cycles, `byteReady` toggles for BPL spins. Good for stock
  DOS 3.3 / ProDOS RWTS. 2-3× faster than LSS in real boots — kept as
  the path the disk_boot / disk_write_controller / dos33_save /
  prodos_save smoke tests exercise (no P6 ROM loaded).

### Bit-stream expansion

`DiskImage::bitAt(track, idx)` lazily walks nibble buffer, emits 8 cells
per non-FF byte + 2 trailing zero cells per `$FF` that lives inside a
run of ≥5 consecutive `$FF`s. Sync-FF padding lets the LSS lose
alignment in sync gaps and resync on the next prologue. `.nib` path
skips padding (every byte = 8 cells, total 53248). Cache invalidates on
`writeNibbleAt`.

**Why the ≥5 threshold** (`kSyncMinRun = 5`): `nibblizeTrack` lays
down 5-byte `$FF` runs between an address field and its data field,
14-byte runs between sectors, and a long `$FF` tail that wraps into
the next-revolution leader. So any "real" sync gap is ≥5 bytes. The
earlier ≥2 threshold treated naturally occurring 2-byte in-field
`$FF` pairs as sync, dropping +2 zero cells per byte and shifting the
bit stream out from under the LSS. Two ways this surfaced:

- 4-and-4 address-field checksum encodes as `$FF $FF` whenever
  `vol ^ track ^ sector == $FF` (e.g. on a default vol-$FE disk
  with `T^S == $01`: T11 S$0A, T10 S$0B, etc.). With the old
  threshold every such address field corrupted the bit stream for
  the rest of the track.
- 6-and-2 data-field running XOR occasionally produces 2-byte
  disk `$FF` pairs from source `$FF $00 $FF` patterns. Shamus on
  `mario.dsk` and H.E.R.O. (similar source layout) failed to boot
  because of this — the corruption hit the very first data field
  partway through the prologue chain.

Pinned indirectly: `disk_image_smoke` round-trips encoded sectors,
and `mame_lss_parity_smoke` walks the resulting bit stream through
the LSS flux model.

### Flux-event view

`fluxEvents(track)` + `trackPeriod(track)` — one event per "1" cell at
LSS-cycle `cellIdx*8 + 4`. `getNextTransition` verbatim from MAME
`floppy_image_device::get_next_transition`, wraps across revolutions.
`writeFlux(track, start, end, count, transitions)` splices flux window
back into nibble buffer (cell-windowed, 8-bit packed) — used by LSS
write side on Q7 falling edge and 30-event pre-emptive flush.
Invalidated with `bitStream` on `writeNibbleAt` / `eject` / `loadFile`.

### SmartPortCard (//e Liron-class)

`SmartPortCard.{h,cpp}`. Lets a //e / II+ / II / //c profile mount
Sony 800 K disks through a slot-plugged Apple "Disk 3.5 Controller
Card" (the Liron / 670-0186). Default slot 5 (same convention as
HDV — user picks one or the other in Slot Configuration). Reuses the
same `Disk35Image` pair that `EmulationController` owns, so the
Disk 3.5" panel shows the same images whether they're driven by the
//c+ on-board hub or by this slot card.

**Architecture choice — block-level, no IWM**. The real Liron carries
an IWM + a tiny 6502 ROM with the SmartPort dispatcher; ProDOS calls
the dispatcher and the dispatcher talks GCR to the Sony drive. POM2's
//c+ profile already emulates that full stack (`IWMDevice`,
`SmartPortHub`, `Sony35Drive` with the zoned GCR encoder). For a
slot-plugged card on //e we **skip the bit-level path entirely** —
no game flips the Liron at flux level — and expose blocks directly
through a streaming protocol identical to `ProDOSHardDiskCard`'s.
Image storage stays `Disk35Image::readBlock` / `writeBlock`.

**Device-select protocol** (`$C0nX`):

```
$C0n0 write  drive select (0 / 1)
$C0n1 write  block LO byte of the selected drive
$C0n2 write  block HI byte of the selected drive
$C0n3 read   next byte of the selected drive's current block (auto-incr)
$C0n3 write  next byte INTO the selected drive's current block (WB-gated)
$C0n4 read   status: bit7 = no disk, bit6 = write-protected
```

Per-drive `streamOffset_[2]` wraps every 512 B; drive-select latches
`activeDrive_` and resets the active drive's stream offset to 0.
Block cache: `static thread_local cache[2][512]` for reads + `buf[2][512]`
for writes — committed via `writeBlock` on stream-offset wrap. (Single
CPU thread so thread-local is fine; if multi-instance SmartPortCard
ever ships, lift caches to members.)

**Slot ROM** (`buildRom`, 256 B with slot baked in):

```
$Cn00     JMP $Cn20            (boot vector)
$Cn01     $20                  ProDOS signature byte
$Cn03     $00
$Cn05     $03
$Cn07     $3C                  SmartPort signature (non-zero ≠ $01)
$CnFE     $13                  features/units mask (read+status, 2 units)
$CnFF     $50                  driver entry offset
$Cn20-..  boot routine          (load block 0 of drive 1 → $0800, JMP $0801)
$Cn50-..  ProDOS driver         (cmd switch + read/write loops)
$CnE0-..  error halt loop
```

The driver examines ProDOS's `$43` unit byte. Bit 7 = drive: 0 →
write `0` to `$C0n0` (drive 1), 1 → write `1` to `$C0n0` (drive 2).
Then standard 2-page stream into the ProDOS buffer at `$44/$45`.
Write path probes `$C0n4` bit 6 first and returns `$2B`
(write-protected) without touching memory when the WP bit is set.

**Boot wiring**: when the user clicks a library entry in the Disk 3.5"
panel on a non-//c+ profile, `MainWindow::renderDisk35PanelWindow`
calls `controller->bootFromSlot(smartPortCard->getSlot())` after
`mount35`. On //c+ on-board it falls back to `coldBoot()` so the
ROM autostart picks up the built-in SmartPort firmware.

Pinned: `smartport_card_smoke_test.cpp` (ROM signature, drive-select
+ streaming reads, status byte, write-back round trip).

### 3.5" mechanical sounds

`Sony35Drive` carries a `FloppySoundSink* sound_` member set by
`EmulationController` to the same `FloppySoundDevice` the Disk II
uses — so the //c+ on-board Sony drives + any slot-plugged Liron-
class card all share the 5.25"/3.5" sample sets in
`roms/floppy_samples/` and the same volume/mute persistence keys.

**Cycle stamping**: `seekPhaseW(uint8_t phases, uint64_t emuCycles)`
takes a CPU-cycle counter at the strobe edge. `SmartPortHub::onIwmPhases`
forwards `IWMDevice::emuCycles()` (= the last `tick()` value).
`strobeWriteRegister` cases 0x1 / 0x2 / 0x3 / 0x4 emit:

```
0x1  step       moved && sound_->step(track_, lastStrobeCycle_)
0x2  motor on   if (!motorOn_) sound_->motor(true,  hasDisk)
0x3  motor off  if ( motorOn_) sound_->motor(false, hasDisk)
0x4  eject      image->eject() ; sound_->click()
```

`moved` gates so head bumps at track 0 or 79 don't click on real
hardware. Motor transitions are edge-only (idempotent re-strobes
from the //c+ firmware don't replay the sample).

**User-initiated click**: `EmulationController::mount35` and
`eject35` call `drive->emitInsertClick()` after `notifyMediaChange()`
— mirrors the Disk II `insertDisk` / `ejectDisk` path.

Pinned indirectly through `smartport_35_smoke_test.cpp` (sink stays
nullptr in tests; production wiring asserts via runtime audio).

### ProDOS host folder

(`prodos_disk/`) `ProDOSVolume` synthesises a read-only ProDOS volume
from a host folder. Layout: blocks 0-1 boot (zeroed, not bootable), 2-5
volume dir key + 3 ext blocks (51 entries max), block 6 bitmap (4096
blocks = 2 MB cap), 7+ data + sapling indexes.

Scope: flat dir only; ≤ 51 files; ≤ 128 KB per file (seedling + sapling,
tree skipped with warning); type from extension; filenames sanitised to
`A-Z/0-9/.` with collision suffixes `.1/.2`.

Wiring: HDV slot 5 panel's Library shows synthetic `[host folder]
prodos_disk/` entry. Click → `buildVolumeFromFolder` →
`ProDOSHardDiskCard::loadImageFromBytes`. **No auto-boot** — user must
boot ProDOS from slot 6 or another HDV first; then `/HOST/` appears as
slot 5 drive (`CAT,S5,D1`). Read-only: driver returns `$2B` on writes.
Refresh by clicking entry again. Pinned: `prodos_volume_smoke_test.cpp`.

### Snapshot

`SnapshotIO`. `POM2SNAP` magic, named 8-byte sections, format shared
with POM1 (round-trip: `tests/snapshot_io_smoke`). Captures CPU + RAM +
soft-switch display state. **Disk II deliberately excluded** — would
need mounted-image identity + head position + dirty bits per track; v1
keeps snapshots focused on CPU + RAM + soft switches.

## Peripherals

### Super Serial Card (slot 2) + telnet bridge

Minimal 6551-ACIA shape at `$C0A8-$C0AB` (data/status/cmd/ctrl). Status
bit 4 = TDRE (always 1), bit 3 = RDRF (RX queue), bits 5/6 = DCD/DSR
(TCP state). Unconnected `$C0A8` returns 0.

Slot ROM `$C200-$C2FF`: SSC autodetect bytes (`$Cn05=$38`, `$Cn07=$18`,
`$Cn0B=$01`, `$Cn0C=$31`) at spec'd offsets; `JMP $Cn20` skips over them.
PR#2 hooks CSWL/CSWH (`$36`/`$37`) → `$C2B0`; IN#2 hooks KSWL/KSWH
(`$38`/`$39`) → `$C2E0` (load + ORA #$80 for Apple high-bit convention).
Reset clears ring buffers.

TCP listener on `127.0.0.1:port` (default 6502); one client. 4 KB rings;
telnet IAC (WILL/WONT/DO/DONT + 2-byte cmds + `$FF $FF` literal)
swallowed by `swallowTelnetIac` so stock `telnet` connects cleanly.
`TCP_NODELAY` on. Auto-plugged at startup; listener starts only when
`ssc_listening=true`. Port + state persisted.

### ProDOS clock card (slot 4)

ThunderClock+ compatible. **ProDOS does NOT route through slot ROM** for
clock reads — at boot it copies its hardcoded ThunderClock driver into
RAM (~$D742), patches `$BF06-$BF08` to JMP it, then driver speaks via
device-select. Our slot ROM only needs the detection signature.

Slot ROM `$C400-$C4FF`: signature bytes `$08, $28, $58, $70` at offsets
`0, 2, 4, 6`. Odd-offset fillers (CLD/CLD/SEI) + `BVS +0` form benign
fall-through; `$Cs08 = RTS` so stray `JMP $Cs00` returns.

**uPD1990AC bit-bang at `$C0C0`**:

```
write bit 0 = DATA_IN; bit 1 = CLK; bit 2 = STB; bits 3..5 = C0/C1/C2
read  bit 7 = DATA_OUT (LSB of shift register)
```

Mode `0b011` = `MODE_TIME_READ`: arm via `$C0C0=$18`, pulse STB (`$1C`)
to latch host time into 48-bit shift register, drop STB, then read bit 7
+ pulse CLK (`$1A`/`$18`) 48 times → 6 BCD bytes (sec, min, hour, day,
(month<<4)|dow, year). Mode `0b010` = `MODE_TIME_SET`: load 48 bits via
DATA_IN + 48 CLK, then STB-in-TIME_SET commits.
`commitTimeSetFromShiftReg()` decodes BCD via `std::mktime`, captures
delta vs `timeFn()` as `userOffsetSeconds`; `effectiveTime()` composes
`timeFn() + offset`. (No background thread — advancement derived from
host clock on demand.)

TP tick-pulse modes (64/256/2048/4096 Hz, MAME `upd1990a.cpp:248-267`)
**not hooked up**. The slot-bus IRQ line is now exposed via
`SlotPeripheral::assertIrq` (see [IRQ wire-OR](#irq-wire-or)) —
remaining work is wiring the four dividers and pulsing `assertIrq` at
the TP rate.

**MODE_SHIFT lax-gating divergence**: POM2 **deliberately diverges**
from MAME `upd1990a.cpp:312-327` which gates CLK-shift on
`m_c == MODE_SHIFT`. POM2 shifts on every CLK rising edge regardless of
mode bits — because ProDOS's hardcoded ThunderClock driver pulses CLK
while still in MODE_TIME_READ (no re-switch between STB and serial-out).
Strict gating breaks stock ProDOS; observed ThunderClock+ hardware
permits the shortcut. DATA_IN still latched into MSB on every CLK rise
so MODE_TIME_SET works the normal way. Pinned:
`clock_card_smoke_test.cpp::testShiftLaxAcrossModes`.

Auto-plugged when `clock_card_enable=true` (default). DOS 3.3 ignores
the card. `pom2_headless` plugs unconditionally.

### Mouse Card

(slot 4 by convention) Verbatim port of MAME
`src/devices/bus/a2bus/mouse.cpp`. Pieces:

- **M68705P3** MCU (Apple 341-0269, 2 KB mask ROM). Paced at 2× CPU
  clock from `advanceCycles()` via fractional accumulator.
- **MC6821** PIA — bus side at `$C0n0-$C0n3`.
- **8516 EPROM** — 2 KB Apple-side slot ROM (Apple 341-0270-c),
  bank-switched into `$Cn00-$CnFF` via PIA PortB bits 1-3 (8 banks of
  256; `bank = (PortB & 0x0E) << 7`).

PIA ↔ MCU bridge:

```
PIA PortA  ↔ MCU PortA            (bidir, pull-ups)
PIA PB4-7  ↔ MCU PC0-3
PIA PB1-3  → EPROM A8-10          (bank select)
MCU PB6    → slot IRQ (active low; cached, transitions only)
MCU PB7    ← mouse button (active low)
MCU PB0/1, PB2/3 ← X/Y quadrature (CLK + DIR per axis)
```

Host routing: `MainWindow::onMouseMove` / `onMouseButton` →
`setHostMouse(rawX, rawY, button)` (clipped to screen rect). MCU
computes deltas via 8-bit subtraction with wrap; POM2 emits **at most
one quadrature edge per axis per MCU PortB read** (matches MAME
`m_last` / `m_count`).

**ROM gating**: BOTH ROMs required to plug. Slot-config UI greys entry
when missing; `plugSlotsFromSettings` refuses with a `Mouse` log warn.
Defaults: `roms/mouse_341-0270-c.bin` + `roms/mouse_341-0269.bin`.

**Not modelled** (firmware-invisible): PAL16R4 chip-select sequencer at
U2A, PIA PortB bit 0 sync latch (firmware paces against video timing —
we enable IRQs unconditionally), motion clamping (MCU does it). Pinned:
`mouse_card_smoke_test.cpp`, `mouse_card_quadrature_smoke_test.cpp`.

### Joystick / paddles

- `JoystickInput` polls all 16 GLFW slots each UI frame (hot-plug). One
  binding drives PADL(0/1) + PB0/1/2. PADL(2/3) read centered (127).
- **Paddle RC** in `Memory::softSwitchAccess`: `$C064-$C067` returns
  `0x80` while `(cycleCounter - paddleLatchCycle) < paddleValue × 11`.
  `$C070` arms the latch. 11-cycle constant is rough Apple II RC step —
  good enough for paddle games, not a PASCAL clone.

## UI (ImGui)

- **MainWindow** — menu bar + screen + emulation panel + on-demand
  panels. Owns the screen GL texture. Auto-plugs Disk II in slot 6 if
  `roms/disk2.rom` exists. F9 (screenshot), F11 (soft reset), F12 (hard
  reset) routed unconditionally even when ImGui has keyboard focus.

### MainWindow Pimpl-light

Since 2026-05-15 `MainWindow.h` is **forward-decl-only** for every
plugin / panel / controller type — it includes `M6502.h` (for the
nested `CpuMode` in a signature) and `imgui.h` (for `ImU32` /
`ImVec2`), nothing else. The 18 owning member types live behind
`std::unique_ptr<T>`; constructor + destructor + accessor
(`emul()`, `displayRef()`) bodies are out-of-line in
`MainWindow.cpp` so the unique_ptr destruction sees a complete type.

**Compile-time benefit measured**: `touch CassetteDeck_ImGui.h` →
**2 TUs** rebuild (was previously dragging in MainWindow.cpp +
MainWindow_MemoryMaps.cpp + MainWindow_Slots.cpp + every test
linking against the panel cone). `touch MainWindow.h` → **4 TUs**
(MainWindow*.cpp + main.cpp — irreducible).

Side-effect: the non-owning `*Card` pointers (`diskCard`,
`hdvCard`, …) are still raw, because `SlotBus` owns the cards
themselves. Forward-declared their types in the same block as the
panels (each card class is in the root namespace bar
`pom2::SmartPortCard` which is namespaced).
- **MemoryViewer_ImGui** — hex + ASCII over full 64 KB. Reads via
  `Memory::data()` directly under `stateMutex` (held by MainWindow
  during `render()`) so viewer never triggers soft-switch side effects.
  Edits go through `Memory::memWrite` (ROM protection still applies).
  Per-byte change-flash uses frame-counter delta vs `prevMemory`. Search
  handles hex sequences (`A9 FF 48`) and ASCII (matches both raw and
  high-bit-set so on-screen text is findable).
- **Disassembler6502** — stateless `(mem*, pc) → mnemonic + length`.
- **main.cpp** — GLFW char/key callbacks gated by ImGui keyboard capture
  so editing widgets don't leak into Apple II.
- **Screenshot (F9)** — `screenshot_NNN.ppm` (P6 binary RGB) in cwd;
  sequence auto-advances.

## Profile switching internals

`SystemProfile.h/.cpp`. Pinned: `system_profile_smoke_test.cpp`.

**32 KB ROM disambiguation**: //e and //c dumps share the 32 KB size
but encode firmware in OPPOSITE halves. `loadAppleIIRom` takes a
`pickLower16KFor32K` flag set by `applyProfile` based on profile:

- //e (`apple2e.rom`): firmware in UPPER 16 KB (file 0x4000-0x7FFF),
  lower half is character ROM data. `pickLower=false`.
- //c / //c+ (`apple2c-32Kv0.rom`, `apple2cp.rom`): TWO 16 KB firmware
  banks. Bank 0 in LOWER half (the one mapped at reset, contains cold-
  start at $FA62), bank 1 in upper half (alt firmware: AppleTalk,
  MouseText, SmartPort drivers). `pickLower=true`; the upper 16 KB is
  stashed into `iicAltFirmware` for the $C028 toggle.

Both halves can carry valid-looking reset vectors so we cannot reliably
auto-detect from bytes alone — the profile is the source of truth.

**$C028 ROMBANK** (//c only): when `iicHasAltBank` is set (32 KB dump
loaded via `pickLower=true`), any access to `$C028` toggles
`iicRomBank`. Reads of `$C100-$CFFF` (under `INTCXROM`) and `$D000-$FFFF`
(under LC ROM) consult the flag and dispatch to `iicAltFirmware` instead
of `internalIORom` / `mem` when bank 1 is active. `resetSoftSwitches`
clears the flag so cold-boot always starts in bank 0. On II/II+/IIe (no
alt bank), the `$C028` access falls through to the cassette-output
toggle just like the rest of `$C020-$C02F`. Pinned:
`system_profile_smoke_test.cpp::testIicRomBankSwitch`.

**//c INTCXROM override** (//c-only): the //c has no physical slots, so
internal motherboard ROM is mapped at `$C100-$CFFF` at all times. POM2
gates `internalIORom` dispatch on `(MF_INTCXROM || iicHasAltBank)` in
`memRead` and `memWrite` — matches MAME `apple2e.cpp:1617-1635
update_slotrom_banks`, which ORs `m_isiic` into every internal-ROM
gate. **Without this**, the //c reset routine at `$FA62` would `JSR
$CE4D` into an empty slot bus on the very first instructions and crash
before booting. `loadAppleIIRom` and `resetSoftSwitches` both set
`iieMemMode |= MF_INTCXROM` when `iicHasAltBank` is true so `$C015`
(RDCXROM) reads back consistently (MAME `apple2e.cpp:1273` does the
same in `machine_reset`). Pinned:
`system_profile_smoke_test.cpp::testIicInternalRomAlwaysMapped`.

**//c+ MIG + IWM handshake** (//c+-only): the //c+'s alt firmware
(bank 1, mapped via `$C028` ROMBANK) is built around two pieces of
hardware POM2 only models in the minimum form needed for cold boot:

- **MIG** (Multi-drive Interface Glue, MAME `apple2e.cpp:451-624
  mig_r / mig_w`). Apple gate-array that bridges the IWM to the //c+'s
  on-board 5.25" + 3.5" SmartPort drives. POM2's Memory module hosts
  the MIG state (`migRam[0x800]`, `migPage`, `migIntDrive`, `migHdSel`)
  and routes the two MIG windows in the bank-1 expansion ROM area:
  - `$CC00-$CCFF` → `migOffset 0x000-0x0FF` (drive enable/disable,
    IWM reset)
  - `$CE00-$CEFF` → `migOffset 0x200-0x2FF` (MIG RAM + auto-increment,
    3.5" head select, MIG page reset)
  The 3.5"-side decodes (`hdsel` toggles via `$240-$27F`) are stored
  but a no-op functionally — POM2 has no 3.5" SmartPort drive. The
  2 KB MIG RAM is fully implemented; the //c+ firmware uses it to
  cache disk geometry / accelerator state across resets. MAME
  `apple2e.cpp:1700-1703` resets `migPage` when ROMSWITCH transitions
  back to bank 0; POM2 mirrors that in `softSwitchAccess` $C028
  handling.

- **IWM mode register + WHD handshake** on `DiskIICard` (MAME
  `iwm.cpp:103-114 read / 256-269 mode_w`). A real Disk II / wozfdc
  only decodes Q6/Q7 for read vs write data; the IWM additionally
  exposes a *mode register* (via `$C0nF` writes when Q6 is already
  high — control byte = 0xC0) and returns a *status register* (via
  `$C0nE` reads when Q6 is high — control byte = 0x40, low 5 bits =
  mode low 5) plus a *write-handshake register* (`$C0nC` reads with
  Q7 high — control byte = 0x80). POM2's `DiskIICard` tracks
  `iwmMode` + a resting `iwmWhd = 0xBF` (bit 7 = ready, bit 6 clear
  per MAME `iwm.cpp:57`) and intercepts those three combinations in
  both the bit-LSS path and the legacy 32-cycle gate. Plain Disk II
  software never drives Q6+Q7 to the mode-set state, so existing
  smoke tests are unaffected; the //c+ alt firmware's IWM probe at
  `$E512-$E522` (mode-echo handshake) and the write-ready loop at
  `$C8A6-$C8A9` / `$C960-$C965` both clear with these hooks in
  place. **Without them**, the //c+ Monitor cold-reset path
  `$FA62 → JSR $C740 → STA $C028 → JMP $C711 in bank 1` hangs
  before any banner reaches the screen.

**Standalone IWMDevice** (`IWMDevice.{h,cpp}`): a separate file with a
verbatim port of MAME `src/devices/machine/iwm.{h,cpp}` — the full
state machine (`MODE_IDLE / ACTIVE / DELAY / READ / WRITE` for the
top-level controller; `S_IDLE / SR_WINDOW_EDGE_0 / SR_WINDOW_EDGE_1`
for the read bit-window walker; `SW_WINDOW_LOAD / MIDDLE / END /
UNDERRUN` for write). Drives flux transitions via
`DiskImage::getNextTransition` (POM2's MAME-compatible
`floppy_image_device::get_next_transition` analogue). Designed as
the eventual replacement for the //c+ profile's $C0E0-$C0EF slot-6
mux — DiskIICard's current IWM-mode / IWM-WHD shadows are the
keep-it-booting interim, IWMDevice is the SmartPort-grade real
thing.

**Live wiring** (current build):

  1. `EmulationController` constructs the IWMDevice next to the
     audio / cassette / speaker / floppy-sound devices and hands it
     to `Memory::setIWM`. Reset paths (`hardReset`, `coldBoot`,
     `bootFromSlot`) call `iwm.reset()`.
  2. Memory routes $C0E0-$C0EF on `iicHasAltBank` profiles through
     `iwmDevice->read` / `write` — matches MAME `apple2e.cpp:2430-
     2432 c080_r` which gates on `m_isiicplus && slot == 6`. The
     slot-6 DiskIICard still observes the access so motor sound,
     disk-turbo gating, and head-position tracking stay current,
     but the **byte returned to the CPU is the IWM's** when
     `iwmAuthoritative` is true (the default).
  3. DiskIICard pushes `setFloppy(image, qt)` updates to the IWM
     from `insertDisk`, `ejectDisk`, `selectDrive`, and the
     head-step path in `seekPhaseW`. The IWM's `nextTransition`
     helper queries `DiskImage::getNextTransition(qt, from*2) / 2`
     — POM2's flux events live in LSS-cycle space (`lssCycle =
     cpuCycleTotal * 2`, see `DiskIICard::lssSync`), so the IWM
     transits the boundary at the lookup edge to stay single-clock
     with the rest of the machine.
  4. EmulationController pulses `iwm.tick(cpuCycleTotal)` once per
     video frame so the 1-emulated-second drive-disable timer
     (MAME `iwm.cpp:70-84 update_timer_tick`) still drains when the
     //c+ alt firmware stops poking $C0Ex between disk operations.

`iwmAuthoritative` is a runtime toggle (`Memory::setIWMAuthoritative`,
or `POM2_IWM_AUTHORITATIVE=0` env var) that drops the data path back
to DiskIICard's LSS for A/B comparison during regression bisect; the
IWM state machine still advances on every access in that mode, so
mode / status / WHD register reads stay coherent regardless. Pinned:
`tests/iicplus_boot_trace.cpp` boots both modes; both reach the
disk-loaded boot stage with the //c+ banner displayed and the
disk loader running in user RAM (PC = $39xx after 6M cycles).

**Window-size scaling**: MAME's `iwm.cpp:290-301 half_window_size`
and `:302-313 window_size` are in IWM-clock ticks (the //c / //c+
runs the IWM off A2BUS_7M ≈ 7.16 MHz). POM2 ticks the IWM at
POM2_CPU_CLOCK_HZ (≈ 1.023 MHz) to keep a single cycle counter,
so the constants are divided by ~7 to keep a "bit cell" window at
≈ 4 µs of emulated time. Tracked verbatim in
`IWMDevice::windowSize` / `halfWindowSize` with the MAME-side
values in the comment block for quick cross-reference.

NOT yet ported (groundwork for the next pass):
  * The 1 s drive-disable delay (MAME `iwm.cpp:70-84
    update_timer_tick`). MAME schedules an `emu_timer` to drain
    `m_active = MODE_DELAY` back to `MODE_IDLE` and clear WHD
    bit 6. POM2 should run the equivalent from `sync()` once it
    detects the deadline has elapsed.
  * The Q3 fast clock (1.86 MHz) used on Mac/IIgs but not //c+; the
    field is present but `q3ClockActive_` stays `false`.
  * `set_floppy` mon_w / set_write_splice plumbing (still owned by
    DiskIICard).

**SmartPort 3.5" Phase 1** (`Disk35Image`, `Sony35Drive`,
`SmartPortHub`): scaffolding for //c+ Sony 3.5" disks. Loads 800K
`.po` / `.2mg` images, exposes a Sony 3.5" drive that responds to the
IWM's phase-as-command bus (port of MAME `mac_floppy.cpp::seek_phase_w`
+ Apple //gs hardware ref register table) and to MIG-driven
`m_35sel`/`m_intdrive`/`m_hdsel` toggles (port of `apple2e.cpp:638-679
recalc_active_device`). The IWM gains `phasesCb_`/`devselCb_`/`sel35Cb_`
callbacks (MAME `iwm_device::phases_cb/devsel_cb/sel35_cb`) that
EmulationController wires through `SmartPortHub::attach`. The active
3.5" drive sees phase strobes (CA0/CA1/CA2/LSTRB) and SEL changes; its
`senseR()` returns the active-low register file (`/INSERTED`,
`/TRACK0`, `/READY`, `/MOTOR ON`, `/SWITCHED`, …) that the //c+ alt
firmware probes during cold-boot SmartPort discovery. Pinned:
`tests/smartport_35_smoke_test.cpp` (image load + size guard, empty-
slot SENSE, in-slot SENSE, motor strobe, hub recalc for both
devsel=1+35sel=true and devsel=2+intdrive=true paths, IWM-to-drive
phase forwarding).

**SmartPort 3.5" Phase 2** — Sony zoned GCR encoder + IWM dispatch.
The on-demand encoder in `Sony35Drive.cpp` is a verbatim port of MAME
`flopimg.cpp::build_mac_track_gcr` (lines 2017-2106): five concentric
speed zones (`kCellsPerRev[5] = {76950, 70695, 64234, 57749, 51388}`,
matching MAME `flopimg.cpp:2019-2027`), per-zone CPU-cycles-per-rev
computed from `60 × POM2_CPU_CLOCK_HZ / RPM`, the 64-entry
`kGcr6fw[]` write table (MAME line 967), and the
`gcr6Encode(va,vb,vc)` 3-in-4-out packer (MAME line 512). Per-sector
layout: 8× self-sync (384 cells) + D5AA96 address prologue + 5 GCR
header bytes (track/sec/side/format/checksum) + DEAAFF address
epilogue + 2× self-sync gap + D5AAAD data prologue + 174 groups of
3-in-4-out checksummed data + 4-byte data checksum + DEAAFFFF data
epilogue = 6208 cells per sector. Block-to-physical mapping uses
MAME's 2:1 interleave schedule (`si = (si+2) % ns; if(si==0) si++`)
from `apple_gcr_format::load` lines 372-382.

IWMDevice now dispatches `nextTransition()` between the existing
5.25" `DiskImage*` path and the new 3.5" `Sony35Drive*` path via
`setFloppy(DiskImage*, qt)` / `setSony35(Sony35Drive*)` overloads
(MAME `iwm_device::set_floppy` line 85). The WPT bit on
$C0EE-status reads consults `Sony35Drive::senseR()` for 3.5" so
the //c+ firmware sees the full Sony register file via the same
status-bit mechanism. Pinned: `tests/smartport_35_smoke_test.cpp`
now validates address+data marker placement on track 0 (12 of each,
matching the 12-sector zone schedule) AND clocks the IWM bit-cell
walker through the Sony flux stream until it recovers the `$AA`
sync byte from a `D5 AA 96` prologue.

**SmartPort 3.5" Phase 3** — UI + CLI + persistence. New
`Disk35Controller_ImGui` panel renders the two Sony slots side-by-
side (internal = on-board //c+, external = SmartPort daisy-chain),
with Mount/Eject buttons, last-error display, and a library scanner
that picks up `.po`/`.2mg` images of the right size under
`disks35/` (falls back to `disks/`). Hardware → Devices menu hides
or shows it; visibility persisted as `show_disk35_panel`.
EmulationController gains `mount35(idx, path)` / `eject35(idx)`
under `stateMutex`. CLI flags `--35-disk1` / `--35-disk2` route
into `CliPlan` and `main.cpp` calls the new mount API after Phase A
ROM/preset selection. Settings persists the two paths
(`disk35_path_1`, `disk35_path_2`); the constructor re-mounts on
launch if the file still exists.

**SmartPort 3.5" Phase 4** — flux write-back. `Sony35Drive::writeFlux`
splices IWM-emitted flux transitions into the cached cell buffer,
then runs the GCR→blocks decoder (port of MAME `flopimg.cpp:2107
extract_sectors_from_track_mac_gcr6`) over the result. Recovered
sectors that differ from the loaded `Disk35Image` get pushed back
via `writeBlock`; the image marks itself dirty and gets flushed to
`.po` via `saveDirty()` on `EmulationController::eject35` or on
MainWindow shutdown (mirrors the 5.25" save-on-eject path). Write-
protect is honoured: an image without `setWriteBackEnabled(true)`
short-circuits `writeFlux` before touching cells, so the //c+
firmware can freely write but the host file stays untouched until
the user opts in. The nibbliser (port of `flopimg.cpp:1530
generate_nibbles_from_bitstream`) handles the IWM's MSB-first
byte-alignment + zero-cell skip semantics. Cycle ↔ cell rounding
uses round-to-nearest on the decode side (the encoder uses floor
on `cycleForCell = i × period / n`; without symmetric rounding the
integer-truncated 2.024 → 2 mapping pushed every transition one
cell early and lost the first sector's address-field marker).
Pinned: `tests/smartport_35_smoke_test.cpp` now covers a full
encode → flux → splice → decode → block-readback round trip on
sector 0 + verifies write-protect short-circuits.

Pinned:
  * `system_profile_smoke_test.cpp::testIicInternalRomAlwaysMapped`
    (MIG window assertion at `$CE4D` in bank 1).
  * `tests/iwm_device_smoke_test.cpp` (IWMDevice reset state, control
    bit decode, mode/status echo, WHD cold read, sync no-crash).
  * `tests/smartport_35_smoke_test.cpp` (SmartPort 3.5" wiring +
    Sony register protocol).
  * `tests/iicplus_boot_trace.cpp` (boots `apple2cp.rom` headlessly,
    prints PC + text page fingerprint after 6M cycles — used to debug
    where the //c+ ROM gets stuck during a port pass).

**20 KB Apple II+ ROM dumps**: some "system pack" dumps prepend 4 KB
of filler — unused Integer BASIC bank or zeros — to the 16 KB
`$C000-$FFFF` firmware. `loadAppleIIRom` recognises `size == 20*1024`
and skips the leading 4 KB; the high 16 KB loads as a standard II+
image. The old "best effort" fallback landed `loadAddr` at `$B000`,
clobbering user RAM with filler. Pinned:
`system_profile_smoke_test.cpp::test20kIIPlusRomLoad`.

**Profile switching = full cold reset** via
`MainWindow::applyProfile(SystemProfile)`. Order matters:

1. Stop worker.
2. Tear down slot cards under state mutex (Mockingboard's `AudioSource`
   detached from `AudioDevice` FIRST or audio thread dereferences freed
   memory).
3. Wipe RAM/aux/LC + reset soft switches.
4. **`setIIEMode(...)` BEFORE `loadAppleIIRom`**.
5. Load ROMs (with `pickLower16KFor32K` derived from the profile).
6. Re-plug slots from settings.
7. Re-mount previously inserted disks/HDVs (cross-profile media
   persistence).
8. `resolveCpuMode()` (honours `cpu_mode_override`).
9. Reset cycles/frame.
10. `hardReset()`.
11. Restart worker.
12. Persist `system_profile`.
13. Refresh GLFW window title with `cfg.displayName` so the user sees
    "Apple //c" / "Apple //e" without opening the Profile menu. Status
    pill in the menu bar also shows the active profile name.

CLI `--preset` triggers the same path after the legacy auto-probe — wins.
Aliases: `apple2`, `apple2plus`, `apple2e`, `apple2c`, `apple2cplus`,
`//e`, `//c`, `//c+`. `cpu_mode_override` = `auto|nmos|65c02`
(Machine → CPU menu).

## CLI (`CliDispatcher`)

Three phases: **A** parse, **B** pre-boot (preset/ROM/snapshot-load/
--load addr:file), **C** post-boot (tape ops/paste/run/step).

Flags: `--preset ii|ii+|iie|iic|iic+`, `--speed`, `--cpu-max`, `--tape`,
`--load addr:file`, `--run`, `--paste`, `--step`,
`--play`/`--rec`/`--rewind`, `--snapshot-save`/`--snapshot-load`.
