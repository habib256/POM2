# CLAUDE.md

Architecture / invariants / gotchas for the **emulator side** of POM2. User
walkthrough → `README.md`. One concern per file: each `.cpp/.h` pair owns
one subsystem.

## Table of contents

- [Build & run](#build--run)
- [CPU & memory](#cpu--memory)
- [Display](#display)
- [Audio](#audio)
- [Slot bus & IRQ aggregation](#slot-bus--irq-aggregation)
- [Storage](#storage)
- [Peripherals](#peripherals)
- [UI](#ui)
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

## CPU & memory

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

### `Memory::loadAppleIIRom` accepts two //e dump shapes

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

### IIe paging (`isIIE()`)

`setIIEMode(true)` MUST be called BEFORE `loadAppleIIRom` — loader's
IIe-vs-II+ split depends on the flag. Adds aux 64 KB, 4 KB
`internalIORom` for `$C100-$CFFF`, aux LC bank trio. Switches at
`$C000-$C00F` update an `iieMemMode` bitmask; routing per-range (ALTZP
`$0000-$01FF`, RAMRD/WRT `$0200-$BFFF`, 80STORE+PAGE2 swap `$0400-$07FF`
and `$2000-$3FFF` when HIRES). All IIe paths gated behind `iieMode` —
II+ untouched. Pinned: `iie_memory_smoke_test.cpp`.

### Soft switches

Toggled by *either* read or write. `$C030` toggles speaker on **every**
access in `$C030-$C03F` (alias decoded). `$C061-$C067` are paddles +
buttons on II/II+ — **NOT** cassette aliases (only `$C020`/`$C060` are).

### Apple II text/HGR row interleave

Woz DRAM-refresh trick. Formulae in `Apple2Display.cpp`:

- text: `addr = base + 0x80*(y%8) + 0x28*(y/8)`
- HGR:  `addr = base + 0x400*(y%8) + 0x80*((y/8)%8) + 0x28*(y/64)`

This is why sequential writes to `$0400` don't render line-by-line.

### Memory map

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
$C030-$C03F  Speaker toggle (any access)
$C050-$C057  Display mode pairs (text/gfx, mixed, page 1/2, lo/hi-res)
$C05E/$C05F  IIe DHGR enable / disable (AN3 pulses for Le Chat Mauve FIFO)
$C061-$C063  Push-buttons (negative when pressed)
$C064-$C067  Paddle inputs (negative while RC discharging)
$C070        Paddle reset latch
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

## Display (`Apple2Display`)

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

### DHGR (IIe, `eightyCol && hiRes && dhgr && !textMode`)

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

### 80-col text (IIe)

Aux RAM (cells 0,2,…,78) interleaved with main (1,3,…,79) into 560-wide
frame. Mixed (HIRES+80COL+MIXED) renders HGR top 20 rows doubled into
560-wide, overlays 80-col rows 20..23. ALTCHAR plumbed but no-op against
built-in fallback (real charset ROM would consult second 2 KB bank).

### Test framework gotcha

Tests inherit parent's `-O3 -DNDEBUG` → would silently strip every
`assert()`. `tests/CMakeLists.txt` adds `-UNDEBUG` so asserts actually
run. Without that, every smoke test prints "OK" regardless.

## Audio

### `AudioDevice`

miniaudio mono float32. **OS-negotiated sample rate** (often 48 kHz on
Apple Silicon even when 44.1 requested) — cycle-driven sources MUST
query `getActualSampleRate()` or playback drifts by the rate ratio.

### `SpeakerDevice` (`AudioSource`)

Verbatim port of MAME `spkrdev.cpp:74-327`. CPU records each
`$C030-$C03F` toggle with **sub-instruction timestamp**
(`cycleCounter + cpu->getCurrentInstructionCycles()`) into 16 K ring.
Audio thread: rectangle-area integration → 4× oversample → 64-tap
windowed sinc (cutoff sr/4) → 0.995-pole DC blocker
(`y[n] = x[n] - x[n-1] + 0.995*y[n-1]`). Auto catch-up if drain > 100 ms.

### `CassetteDevice`

`$C020` output toggle / `$C060` input comparator sign. Separate
`AudioSource` so tape loads click through speakers. `CassetteDeck_ImGui`
uses Font Awesome (`fonts/fa-solid-900.ttf`); falls back to `?` glyphs
if missing.

### Mockingboard (slot 4 by convention)

Sweet Microsystems shape: two 6522 VIAs each driving an AY-3-8910 PSG.
No ROM. **VIAs decoded in slot ROM window** (`$Cn00-$Cn0F` = VIA #1,
`$Cn80-$Cn8F` = VIA #2) — NOT device-select range, hence the
`slotRomWrite` callback.

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

### Floppy mechanical sounds (`FloppySoundDevice`)

Port of MAME `src/devices/imagedev/floppy.cpp::floppy_sound_device` —
sample-based playback of head-step click, motor spin-up/down,
insert/eject. 20 source WAVs (10 × 5.25" + 10 × 3.5", ~150+210 KB)
vendored in `roms/floppy_samples/` from `mamedev/mame` master;
BSD-3-Clause (`README.txt`).

**`FloppySoundSink` interface** (header-only, zero deps): `DiskIICard`
calls `sound_->motor()` / `step()` / `click()` through it so the 13
smoke tests linking `DiskIICard.cpp` don't also drag the miniaudio TU.

**Step / seek decision** (MAME parity): `step(newTrack)` measures the
gap since previous step in audio output frames.
- `gap > 100 ms` (`kSeekJoinMs`) → single-step click (`525_step_1_1`).
- `gap ≤ 100 ms` → seek mode: pick the seek sample whose nominal
  cadence is closest to the gap (2 / 6 / 12 / 20 ms), pitch-scale
  (`pitch = nominal_ms / gap_ms`), loop.
- No step for `kSeekTimeoutMs` → exit seek, fire final `step_1_1` to
  "land" the head.

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
`floppy_sound_smoke_test.cpp`.

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

### `SlotPeripheral::assertIrq(bool)` — card-side API

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

### `DiskImage`

143 360-byte 5.25" floppy: `.dsk`/`.do` (DOS 3.3 skew) or `.po` (ProDOS
skew). Pre-nibblized into 35 × 6656-byte tracks. GCR per "Beneath Apple
DOS". Skew tables (physical → logical):

- DOS 3.3: `{0,7,14,6,13,5,12,4,11,3,10,2,9,1,8,15}`
- ProDOS:  `{0,8,1,9,2,10,3,11,4,12,5,13,6,14,7,15}`

Both produce same on-disk layout; only file-offset → physical-sector
mapping differs. Write-back via `saveDirty()` (`.dsk`/`.do`/`.po`/`.nib`
+ `.2mg` envelopes + `.woz`) opt-in via `setWriteBackEnabled(true)`;
default off.

### Format detection (`detectFormat()` + `enum ImageKind`)

`loadFile(path)` slurps once, dispatches by content. Order: MacBinary
strip → 2IMG envelope → WOZ magic → 35×6656 NIB → 35×6384 CNib2 →
143 360-byte sector. Unknown → false + specific `lastError` (mirrored
into the Disk II panel as red text); no silent fallback. Inspired by
AppleWin `DiskImageHelper.cpp`.

- **Skew sniff** (143 360-byte branch): validates a ProDOS vol-dir key
  block at `file[0x404]` (`.po`) vs `file[0xB04]` (`.dsk`), overrides
  extension when only the other position fits. Predicate: `prev=0`,
  plausible `next`, storage_type `$F`, name chars in `A-Z 0-9 .` (kills
  `$Fx`-byte spoofs in a DOS catalog). Catches cc65-Chess.po case
  (`.po` ext, DOS skew).
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

### `.woz` (`isWoz()`)

Verbatim port of MAME `src/lib/formats/woz_dsk.cpp`. WOZ stores raw bit
cells — **why .woz disks survive copy protections** that tweak
inter-byte timing (re-encoded `.dsk` synthesises idealised GCR and loses
the signature). WOZ1 (160 × 6656-byte slots, `bit_count` @+6648 u16) and
WOZ2 (160 × 8-byte TRK headers, data at `starting_block × 512`,
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

### `DiskIICard` (slot 6)

256-byte P5A boot PROM (`roms/disk2.rom`); PROM autodetects slot via
`JSR $FF58 / TSX / LDA $0100,X` → Apple II main ROM required for boot.
Soft switches `$C0E0-$C0EF`: phases, motor, drive_select, Q6L/Q6H,
Q7L/Q7H.

**Drive switching** via `selectDrive(int)` mirrors MAME
`wozfdc.cpp:219-241`: when motor active, flush old drive's in-flight
write, reset `lssCycle` to `cpuCycleTotal*2` so new drive's reads start
at position 0 of its track (MAME `revolution_start_time = now`).
Critical for multi-drive protections (Locksmith, Copy II Plus). Each
drive owns its own `DiskImage` + head qtr-track + nibble cursor; phase
magnets + motor shared (matches real silicon). Pinned:
`disk_drive2_smoke_test.cpp`.

### Two read paths share the data register

- **Bit-level LSS** (default when `roms/diskii_p6.rom` present) —
  verbatim port of MAME `wozfdc.cpp` + flux-event subset of `floppy.cpp`
  (fetched 2026-05-09 from `mamedev/mame` master). MAME `cycles` = 2×
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
per non-FF byte + 2 trailing zero cells per `$FF` in runs of 2+
consecutive `$FF`s. Sync-FF padding lets LSS lose alignment in sync gaps
and resync on next prologue. `.nib` path skips padding (every byte = 8
cells, total 53248). Cache invalidates on `writeNibbleAt`.

### Flux-event view

`fluxEvents(track)` + `trackPeriod(track)` — one event per "1" cell at
LSS-cycle `cellIdx*8 + 4`. `getNextTransition` verbatim from MAME
`floppy_image_device::get_next_transition`, wraps across revolutions.
`writeFlux(track, start, end, count, transitions)` splices flux window
back into nibble buffer (cell-windowed, 8-bit packed) — used by LSS
write side on Q7 falling edge and 30-event pre-emptive flush.
Invalidated with `bitStream` on `writeNibbleAt` / `eject` / `loadFile`.

### ProDOS host folder (`prodos_disk/`)

`ProDOSVolume` synthesises a read-only ProDOS volume from a host folder.
Layout: blocks 0-1 boot (zeroed, not bootable), 2-5 volume dir key + 3
ext blocks (51 entries max), block 6 bitmap (4096 blocks = 2 MB cap),
7+ data + sapling indexes.

Scope: flat dir only; ≤ 51 files; ≤ 128 KB per file (seedling + sapling,
tree skipped with warning); type from extension; filenames sanitised to
`A-Z/0-9/.` with collision suffixes `.1/.2`.

Wiring: HDV slot 5 panel's Library shows synthetic `[host folder]
prodos_disk/` entry. Click → `buildVolumeFromFolder` →
`ProDOSHardDiskCard::loadImageFromBytes`. **No auto-boot** — user must
boot ProDOS from slot 6 or another HDV first; then `/HOST/` appears as
slot 5 drive (`CAT,S5,D1`). Read-only: driver returns `$2B` on writes.
Refresh by clicking entry again. Pinned: `prodos_volume_smoke_test.cpp`.

### Snapshot (`SnapshotIO`)

`POM2SNAP` magic, named 8-byte sections, format shared with POM1
(round-trip: `tests/snapshot_io_smoke`). Captures CPU + RAM +
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

### Mouse Card (slot 4 by convention)

Verbatim port of MAME `src/devices/bus/a2bus/mouse.cpp`. Pieces:

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

## System profiles

`SystemProfile.h/.cpp`. Pinned: `system_profile_smoke_test.cpp`.

| Profile | CPU default | iieMode | Main ROM probes |
|---|---|---|---|
| Apple ][ Original (1977) | NMOS | off | `apple2o.rom`, `apple2.rom` |
| Apple ][+ (1979) | NMOS | off | `apple2p.rom`, `apple2.rom` |
| Apple //e Enhanced (1985) | 65C02 | on | `apple2e.rom` |
| Apple //c (1984) | 65C02 | on | `apple2c-32Kv0.rom`, `apple2c-16K.rom` |
| Apple //c Plus (1988) | 65C02 | on | `apple2cp.rom`, `apple2c-plus.rom`, `apple2c-32Kv0.rom` |

Default cycles/frame = 17045 across all profiles (//c+ real silicon is
4× but unmodelled).

**Profile switching = full cold reset** via
`MainWindow::applyProfile(SystemProfile)`. Order matters:

1. Stop worker.
2. Tear down slot cards under state mutex (Mockingboard's `AudioSource`
   detached from `AudioDevice` FIRST or audio thread dereferences freed
   memory).
3. Wipe RAM/aux/LC + reset soft switches.
4. **`setIIEMode(...)` BEFORE `loadAppleIIRom`**.
5. Load ROMs.
6. Re-plug slots from settings.
7. Re-mount previously inserted disks/HDVs (cross-profile media
   persistence).
8. `resolveCpuMode()` (honours `cpu_mode_override`).
9. Reset cycles/frame.
10. `hardReset()`.
11. Restart worker.
12. Persist `system_profile`.

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

## Version string locations

Current release: **v0.5**. Bump in:

- `main.cpp` (window title + console banner)
- `MainWindow.cpp` (About dialog)
- `CMakeLists.txt` (`project(... VERSION x.y ...)`)
- `README.md` (status section, if a version pill is reintroduced)
