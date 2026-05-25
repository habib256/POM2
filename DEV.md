# DEV.md

Notes d'implémentation pour POM2 — refs MAME, gotchas non-obvious,
smoke tests pinning. Orientation + memory map + profile table →
`CLAUDE.md`. Walkthrough utilisateur → `README.md`. Historique des
fixes résolus → `CHANGELOG.md`.

## Table of contents

- [CPU](#cpu)
- [Memory](#memory)
- [Display](#display)
- [Audio](#audio)
- [Slot bus & IRQ aggregation](#slot-bus--irq-aggregation)
- [Storage](#storage)
- [IWM (//c+ on-board)](#iwm-c-on-board)
- [SmartPort 3.5" stack](#smartport-35-stack)
- [Peripherals](#peripherals)
- [UI (ImGui)](#ui-imgui)
- [Host control center (Slot Configuration + Floppy Emu)](#host-control-center-slot-configuration--floppy-emu)
- [Profile switching](#profile-switching)
- [CLI](#cli)

## CPU

Full NMOS 6502 + 65C02 (STZ / BRA / INA / DEA / PHX-PLY / BIT-imm /
TSB / TRB / JMP (abs,X), zp-indirect for ORA/AND/EOR/ADC/STA/LDA/CMP/
SBC) + Rockwell RMB / SMB / BBR / BBS + WDC WAI / STP (PC parks, IRQ
wakes). Klaus Dormann clean. `setCpuMode(NMOS)` re-overrides four
formerly-KIL column-2 entries ($02/$22/$42/$62) back to halt. `$0B
$2B $EB` are 1-byte NOPs on 65C02. NMOS undocumented ANC/SBC-imm
left as NOPs.

Pinned: `cmos_6502_smoke_test.cpp`, `klaus_65c02_extended_test.cpp`
(PASSES @ `$24F1`). `setProgramCounter()` is the Klaus harness back-
door.

## Memory

### `loadAppleIIRom` dump shapes

- **16 KB**: `$C000-$FFFF` directly (MAME/AppleWin layout).
- **32 KB**: "system + video" combined. Firmware at file offsets
  `0x4000-0x7FFF`; lower 16 KB is video/charset (loaded via
  `loadCharRom`). Generic fallback would load linearly at `$8000`
  and leave `$C100-$CFFF` empty when `INTCXROM=on` → //e firmware
  crashes before slot 6 boot PROM.
- **20 KB** II+ "system pack" — 4 KB filler prepended to 16 KB
  firmware. Loader skips the first 4 KB. Pinned:
  `system_profile_smoke_test.cpp::test20kIIPlusRomLoad`.

**//c-class detection** (MAME `apple2e.cpp:1275-1299` content probe):

- `payload[0x3BC0] == 0x00` → `isIIcClass = true`. Forces INTCXROM
  on every reset (//c has no physical slots, `$C100-$CFFF` must
  always read motherboard ROM). Fires for **both** 16 KB rev-255 //c
  AND 32 KB rev-0/3/4/X //c+ dumps.
- `payload[0x3BBF] == 0x05` (after //c match) → `isIIcPlus = true`.
  Gates on-board IWM ($C0E0-$C0EF) and MIG ($CC00 / $CE00) windows.
  Plain //c uses `A2BUS_DISKIING` at slot 6 (MAME `apple2c()`
  `apple2e.cpp:5168-5188`); only //c+ instantiates the IWM directly.
- `iicHasAltBank` is narrower than `isIIcClass` — only true on the
  32 KB dumps that provide an alt-firmware bank for `$C028` ROMBANK
  toggling. 16 KB rev-255 //c has `isIIcClass=true` but
  `iicHasAltBank=false` (no bank to flip).

### Memory dispatch

`memRead`/`memWrite` route every `$C000-$C07F` through
`softSwitchAccess()`; RAM bypasses (cheap). Reset vector defaults to
`$F800`. ROM regions reject writes silently.

### MemoryProfile (//c-class strategy)

All //c / //c+ memory quirks live behind `MemoryProfile`
(`MemoryProfile.h` + `MemoryProfile_IIcClass.{h,cpp}`) instead of
leaking `isIIcClass`/`isIIcPlus`/`iicHasAltBank` predicates across the
dispatcher. `Memory::iicProfile_` (a `unique_ptr<MemoryProfile>`) is
**non-null only on //c-class profiles**, created/destroyed inside
`loadAppleIIRom` from the ROM probes (MAME `apple2e.cpp:1275-1299`).
II/II+/IIe leave it null → a single `if (iicProfile_)` branch on the
hot path, **zero virtual calls** on those machines.

The dispatcher delegates each //c-specific concern: `forcesIntCxRom()`
(no slots → INTCXROM), `romBankToggle()` (`$C028`), `onResetSoftSwitches()`
(ROMBANK→bank 0), `ioReadIWM`/`ioWriteIWM` (`$C0E0-$C0EF` on-board IWM),
`internalRomRead`/`internalRomWrite` (`$C100-$CFFF` under INTCXROM —
//c+ MIG `$CC00/$CE00` + alt-firmware bank 1), and `languageCardRomRead`
(`$D000-$FFFF` alt firmware). `IIcClassProfile` owns the alt firmware
(16 KB), ROMBANK flag, //c+ flag, the 2 KB MIG gate-array (`migRead`/
`migWrite`, verbatim MAME `apple2e.cpp:532-624`) and the IWM/SmartPort
hub pointers. `Memory::setIWM/setSmartPortHub/setIWMAuthoritative` stay
as façades that forward to the profile when present (so test wiring is
order-independent). What stays in `Memory`: `ioudis` (read shared with
//e), `intC8Rom` (the //e `$C300`/`$C800`/`$CFFF` mechanism //c
inherits), LC/paging/generic soft switches. Pinned by `iic_boot_trace`,
`iic_nodisk_boot_trace`, `iicplus_boot_trace`, `system_profile_smoke`,
`iwm_device_smoke`.

### IIe paging

`isIIE()`. `setIIEMode(true)` MUST be called BEFORE `loadAppleIIRom`
— loader split depends on the flag. Adds aux 64 KB, 4 KB
`internalIORom` for `$C100-$CFFF`, aux LC bank trio. Switches at
`$C000-$C00F` update `iieMemMode` bitmask; routing per-range (ALTZP
`$0000-$01FF`, RAMRD/WRT `$0200-$BFFF`, 80STORE+PAGE2 swap on
`$0400-$07FF` + `$2000-$3FFF` when HIRES). All IIe paths gated
behind `iieMode`; II+ untouched. Pinned: `iie_memory_smoke_test.cpp`.

### RamWorks III

Verbatim port of MAME `bus/a2bus/a2eramworks3.cpp`. Tiers: 1 (stock
64K), 4 (256K), 8 (512K), 16 (1M), 48 (3M), 128 (8M cap MAME line
99-107). Bus protocol (MAME line 108-115): writes to `$C0n1/3/5/7`
(predicate `(low & 0x09) == 0x01` over `$C070-$C07F`) latch
`bank = data & 0x7F`. Same accesses still pulse paddle one-shot
mirror — they share the bus.

Storage = `ramWorksBacking_`, one 80 KB slot per bank
(`kRamWorksBankStride = 0x10000 + 0x1000 + 0x1000 + 0x2000` = main
aux + LC bank1 + LC bank2 + LC high). Visible aux* arrays always
hold the active bank — kept at fixed addresses so `Apple2Display`
caches `auxData()` once. `ramWorksSwapToBank` memcpys
visible→backing[prev] then backing[curr]→visible. ProDOS /RAM driver
does ~100s/sec → negligible cost.

Bank clamp via `(data & 0x7F) % ramWorksBanks_`. MAME does not clamp
(allocates 8 MB always, reads UB from unpopulated slots); POM2 wraps
to bound backing access — real RamWorks III with fewer banks aliases
via chip-select.

IIe-only: `setIIEMode(false)` releases backing
(`clear() + shrink_to_fit()`). Setting `ramworks_banks` (default 1).
Wired in `applyProfile` between `setIIEMode(true)` and
`loadAppleIIRom`. Pinned: `ramworks_smoke_test.cpp`.

### Soft switches

Toggled by either read or write. `$C030` toggles speaker on **every**
access in `$C030-$C03F` (alias decoded). `$C061-$C067` are paddles +
buttons on II/II+ — **NOT** cassette aliases (only `$C020`/`$C060` are).

**Open-Apple / Solid-Apple** OR'd into $C061 / $C062 bit 7 alongside
joystick buttons (MAME `apple2e.cpp:2157-2169`). Self-test firmware
reads $C061/$C062 inside reset handler to decide warm-vs-cold-vs-
self-test. Wired to host Left Alt (`Memory::setOpenAppleKey`) and
Right Alt (`setSolidAppleKey`); GLFW key callback routes those even
when ImGui has focus.

**IOUDIS** (`$C07E` SET / `$C07F` CLR + //c mirrors `$C078`/`$C079`).
Initialised `true` on every reset (MAME `apple2e.cpp:1224`). Writes
effective only on `isIIcClass`; IIe falls through (MAME
`apple2e.cpp:2569-2587` gates on `m_isiic||m_isace500`). Read of
`$C07E` on any IIe-class returns bit-7 = ioudis state (MAME
`:2276-2278`).

**LC reset state**: `lcWriteEnable=true`, `lcReadRam=false`,
`lcBank2Active=true`, `lcPrewrite=false` per Sather *Understanding
the Apple //e* Fig 5.13 (MAME `apple2e.cpp:1227-1232 + :1492-1497`).
Applied universally — II/II+ powers up in the same state.

### Power-on RAM pattern

MAME-faithful `00 FF 00 FF …` alternating fill (`Memory::clearRam()`
on user RAM + LC + aux + RamWorks). MAME refs: `apple2.cpp:294-298`
(II/II+), `apple2e.cpp:1014-1035` (IIe). Done once at `clearRam()`
time (power-on / profile switch / cold boot); soft + hard resets
preserve RAM.

### Text/HGR row interleave

Woz DRAM-refresh trick. `Apple2Display.cpp`:
- text: `addr = base + 0x80*(y%8) + 0x28*(y/8)`
- HGR:  `addr = base + 0x400*(y%8) + 0x80*((y/8)%8) + 0x28*(y/64)`

### Clock & threading

`POM2_CPU_CLOCK_HZ = 1 022 727` (14.31818 MHz / 14). 65-cycle "long
cycle" TV alignment NOT modelled. Three modes in
`EmulationController`: **Stopped** (50 ms idle), **Running**
(`cyclesPerFrame` per 60 Hz tick), **Step** (one instruction).
`M6502::run(maxCycles)` returns *actual* cycles → passed to
`Memory::advanceCycles()` so paddle RC stays synced. Single
`stateMutex` guards CPU+Memory.

### Keyboard

Latch + strobe under `kbMutex`. UI `queueKey()` sets strobe high.
CPU reads `$C000` via `softSwitchAccess()` (same mutex). Strobe
stays high until `$C010` read/write.

### Reset architecture

Two Memory-side reset paths:

- **`resetSoftSwitches()`** — full reset: display state, LC bank
  flags, `iieMemMode`, `intC8Rom`, `iicRomBank`, IOUDIS=true,
  RamWorks bank 0. Forces `MF_INTCXROM` when `isIIcClass`. Called by
  `coldBoot()`, `hardReset()`, `applyProfile` step 4, AND
  `resetSoftSwitchesWarm()` when `iieMode` is on.
- **`resetSoftSwitchesWarm()`** — Ctrl-Reset only. On `iieMode`
  delegates to full reset (MAME `apple2e.cpp:1453-1508 reset_w` wipes
  the MMU/IOU/LC list every time). On II/II+ does only keyboard-strobe
  clear (MAME `apple2.cpp:325-331` is the entire `machine_reset` —
  LC + display + cnxx_slot all SURVIVE).

CPU side: `M6502::hardReset()` doesn't wipe stack `$0100-$01FF`
(MAME `reset_w` doesn't touch RAM). `M6502::softReset()` decrements
SP by 3 (faked-BRK reset semantic) instead of snapping to `$FF`.

## Display

Pure software renderer into 280×192 (or 560×192 in IIe 80-col) RGBA.
Reads `Memory::getDisplayState()` (mutex copy) + flat RAM. UI uploads
via `glTex(Sub)Image2D`. Built-in 5×7 ASCII font when no charset ROM.
Text flash via `frame_number() & 0x10` (MAME parity).

Seven `HiResMode`:
- `ColorNTSC` — 14 KB LUT `(parity<<8)|byte`, 39 seam fix-ups, glow
  (MAME `composite_color_mode=0`).
- `ColorCompMedium` (=1), `ColorComp4Bit` (=2, no artifact).
- `ChatMauveRGB` — RGB-card decode; only with `LeChatMauveCard`.
- `MonoWhite` / `MonoGreen` (P31) / `MonoAmber` (history-buffer lerp).

### DHGR

(IIe, `eightyCol && hiRes && dhgr && !textMode`)

`renderDhgr` interleaves aux (dots `c*14..+6`) with main (`+7..+13`)
per byte → 560-dot stream. Three color paths, matching MAME
`apple2video.cpp`:

- **`ColorNTSC`** — composite artifact: 7-bit sliding window over
  560 dots → `kArtifactColorLut[128]` → `rotl4b(value, absX+1)` →
  4-bit lo-res palette index. `+1` matches MAME's `is_80_column=1`
  in `render_line_artifact_color`. Per-pixel decode.
- **`ChatMauveRGB`** — Video-7 / Le Chat Mauve RGB card. The card's
  2-bit AN3 FIFO (`LeChatMauveCard::currentMode()`) picks one of MAME's
  four `dhgr_update` rgbmodes (POM2 enum == MAME rgbmode):
  - `COL140`(3) — every 4-dot block → raw nibble (bit 0 leftmost) →
    `rotl4(n,1)` → `kChatMauveLoResPalette`.
  - `Mixed`(1) — two cols at a time as a 28-bit word; each **source
    byte's MSB** picks color (`rotl4` nibble) vs 7-dot bit-mapped mono
    for its dots (MAME `:946-977` `color_mask`).
  - `Chunky160`(2) — `aux+(main<<8)` → four 4-bit pixels of three dots
    each, 480 wide, centred in 560 with 40 black margins (MAME `:906-930`).
  - `BW560`(0) — plain mono DHR (MAME forces the mono renderer for
    rgbmode 0).
  Palette verbatim from AppleWin `PaletteRGB_Feline`; MAME's Video-7
  collapses idx 5≡10, POM2 follows AppleWin so the "two distinct grays"
  trademark survives (intentional divergence).
- **`Mono*`** — luminance × tint; persistence sized for 280-wide
  HGR so DHGR mono renders without afterglow.

Mixed = DHGR top 160 + 80-col text bottom 4 rows.

**Video-7 foreground-background colored TEXT** (`renderTextChatMauveFgBg`):
40-col text with the RGB card while the DHGR (AN3) soft-switch is on —
char code from main RAM, per-cell fg/bg colours from the **aux** byte at
the same text address (hi nibble = fg, lo = bg, lo-res palette indices);
the 7-bit glyph is doubled to 14 dots. Port of MAME `text_update`
(`:788-791`) + `render_line_color_array` (`:571-583`).

Pinned: `dhgr_render_smoke_test.cpp` (COL140 + composite/mono) and
`video7_parity_smoke_test.cpp` (all 4 rgbmodes + fg/bg vs a self-contained
MAME `dhgr_update` oracle, full 560-dot rows).

### 80-col text

Aux RAM (cells 0,2,…,78) interleaved with main (1,3,…,79) into
560-wide frame. Mixed (HIRES+80COL+MIXED) renders HGR top 20 rows
doubled to 560-wide, overlays 80-col rows 20..23. ALTCHAR plumbed
but no-op against built-in fallback (real charset ROM would consult
second 2 KB bank).

### Test framework gotcha

Tests inherit parent's `-O3 -DNDEBUG` → would silently strip every
`assert()`. `tests/CMakeLists.txt` adds `-UNDEBUG` so asserts run.

## Audio

`AudioDevice`: miniaudio mono float32. **OS-negotiated sample rate**
(often 48 kHz on Apple Silicon) — cycle-driven sources MUST query
`getActualSampleRate()` or playback drifts.

### Speaker

`SpeakerDevice` (`AudioSource`). Verbatim port MAME
`spkrdev.cpp:74-327`. CPU records each `$C030-$C03F` toggle with
**sub-instruction timestamp** (`cycleCounter +
cpu->getCurrentInstructionCycles()`) into 16 K ring. Audio thread:
rectangle-area integration → 4× oversample → 64-tap windowed sinc
(cutoff sr/4) → 0.995-pole DC blocker. Auto catch-up if drain
> 100 ms.

### Cassette

`$C020` output toggle / `$C060` input comparator sign. Separate
`AudioSource`. `CassetteDeck_ImGui` uses Font Awesome
(`fonts/fa-solid-900.ttf`), falls back to `?` glyphs if missing.

### Mockingboard

(slot 4 by convention) Sweet Microsystems shape: two 6522 VIAs each
driving an AY-3-8910 PSG. No ROM. **VIAs decoded in slot ROM window**
(`$Cn00-$Cn0F` VIA #1, `$Cn80-$Cn8F` VIA #2), hence the
`slotRomWrite` callback.

VIA → AY wiring (Sweet Microsystems, AppleWin
`Mockingboard.cpp:193`):
```
Port A       → AY data bus (D0..D7)
Port B bit 0 → AY BC1
Port B bit 1 → AY BDIR
Port B bit 2 → AY /RESET (active low; 1 = running)
```
{BDIR,BC1}: 00=INACTIVE, 01=READ, 10=WRITE, 11=LATCH-ADDR. Drivers
emit PB = `$07` (LATCH) → `$04` (INACTIVE) → `$06` (WRITE) → `$04`.
PB2 stays high — `/RESET` only on init pulses (PB = `$00`).

**6522 subset**: A/B + DDR, T1 (latch + counter, one-shot +
continuous), IFR/IER (T1 bits 6/7; bit 7 dynamic from
`ifr & ier & 0x7F`). T1CL read clears `IFR.T1`. IER bit 7 set-vs-
clear (`$C0` enables, `$40` disables). T2/SR/PCR/CA1/CB1 not
modelled (music drivers use T1 only).

**AY-3-8910 synthesis** runs on audio thread inside inner
`AudioSrc`. CPU updates regs under `mtx`; callback snapshots both
banks (32 B), releases, synthesises lock-free. Tone counters = float
phase accumulators at `clockHz/8/sampleRate`; 17-bit LFSR
`x^17 + x^14 + 1`; envelope 32 steps with R13 shape continue /
attack / alternate / hold. Both chips → mono.

Each VIA `irqOut() = (ifr & ier & 0x7F) != 0`; OR'd onto slot IRQ.

**Lazy timer sync** (`syncToCpuCycle()`): VIAs tick at Φ2 on real
HW; POM2 advances slot peripherals in ~17 045-cycle slices. Every
`slotRomRead/Write` catches the VIAs up to
`cpu_->getCycleCountNow()` first — without this, Mockingboard
detection routines that probe T1 (Nox Archaist, Skyfox, Broadside
per AppleWin issue #1175) always fail. Pinned:
`mockingboard_sync_smoke_test.cpp`.

**Tear-down**: remove `AudioSource` from `AudioDevice` BEFORE
destroying the card (source lives inside it). Persisted:
`mockingboard_volume`, `mockingboard_muted`. Pinned:
`mockingboard_smoke_test.cpp`, `mockingboard_sync_smoke_test.cpp`.

### Floppy mechanical sounds

`FloppySoundDevice`. Port of MAME `imagedev/floppy.cpp::floppy_
sound_device`. 20 source WAVs (10 × 5.25" + 10 × 3.5") in
`roms/floppy_samples/`, BSD-3-Clause.

**`FloppySoundSink` interface** (header-only): `DiskIICard` calls
`sound_->motor()` / `step()` / `click()` through it so the smoke
tests linking `DiskIICard.cpp` don't drag miniaudio.

**Step/seek decision** (MAME parity): `step(newTrack, emuCycles)`
measures gap in emulated CPU cycles — mirrors MAME
`floppy.cpp:1532-1620 floppy_sound_device::step` via
`machine().time()`. Wall-clock audio frames would be wrong under
POM2's disk turbo (~60×): boot PROM's full phase sweep lands in one
audio buffer → all events share `audioFrameCounter_` → gap=0 → buzz.

- `gap > 100 ms` (`kSeekJoinMs`) → single-step click (`525_step_1_1`).
- `gap ≤ 100 ms` → seek mode: pick seek sample whose nominal cadence
  is closest (2 / 6 / 12 / 20 ms), pitch-scale (`pitch = nominal_ms
  / gap_ms`), loop.
- No step for `kSeekTimeoutMs` → exit seek, fire final `step_1_1`.

Floor at 1 ms gap defends `mixLoop` against `INF` rate and keeps
pitch in [1, 2] for `SEEK_2MS`.

**Wall-clock motor-off hold-off** (turbo decoupling):
`diskTurboWhileMotor` bumps CPU ~60× during I/O → controller's 1-sec
spin-down (`motorOffDelay = 1'022'727` cycles) becomes ~17 ms
wall-clock. `FloppySoundDevice` defers the audible transition by
`kMotorOffHoldMs` (default 800 ms) in **audio output frames**, not
cycles; fresh `motor(true)` cancels the pending transition.

**CPU ↔ audio thread**: mutex-guarded `std::vector<Cmd>` queue. CPU
pushes `MotorOn/MotorOff/Step/Click`; audio thread drains at top of
`fillAudioBuffer`. Events sparse (≤ ~100/s during seeks).

**Hook points in `DiskIICard`**: `seekPhaseW` end → `step(head/4)` ;
`control()` `$C0E9` MODE_IDLE → MODE_ACTIVE → `motor(true)` (DELAY →
ACTIVE doesn't fire) ; `advanceCycles()` when `motorOffDelay`
expires → `motor(false)` ; `handleSwitchAccess()` legacy 32-cycle
gate immediate `motor(false/true)` (no DELAY) ; `insertDisk` /
`ejectDisk` → `click()`.

Owned by `EmulationController` so `AudioDevice` shutdown drains
audio thread before destruction. Persisted: `floppy_sound_volume`,
`floppy_sound_muted`. Pinned: `floppy_sound_smoke_test.cpp`.

## Slot bus & IRQ aggregation

`SlotBus` + `SlotPeripheral`, 8 slots. `Memory` routes four windows:

- `$C080-$C0FF` device-select (16 bytes/slot N at `$C080+N*16` ;
  slot 0 = LC hook, 1-7 = expansion).
- `$C100-$C7FF` slot ROM (256 bytes/slot 1-7).
- `$C800-$CFFF` shared expansion ROM, owned by whichever slot most
  recently touched `$CnXX`. `$CFFF` deactivates active slot;
  auto-latch on slot-ROM access.

`advanceCycles()` forwards to every plugged card. Ctrl-Reset
propagates `onReset()` to all cards.

### IRQ wire-OR

`M6502::setIrqLine(sourceId, asserted)` — **wire-OR**. 32-bit OR'd
contributor mask: slot N (1..7) = bit N, motherboard VBL = bit 8,
legacy `setIRQ(int)` = bit 31. NMI is still a single latch (no NMI
sources today). Pinned: `irq_aggregator_smoke_test.cpp`.

### `SlotPeripheral::assertIrq` API

Cards never poke `cpu->setIrqLine` directly. The base class exposes
protected `assertIrq(bool)` that debounces against internal
`irqAsserted_` cache (idempotent — only edges propagate), fans out
via `SlotBus::forwardSlotIrq(slot, asserted)` to whatever
`IrqRouter` Memory installed (`Memory::setCpu(cpu)` plants a closure
that calls `cpu->setIrqLine(slot, asserted)`).

`SlotBus::plug()` / `unplug()` / `clear()` auto-release any pending
IRQ contribution before letting the card go.

Mockingboard still holds `cpu_` (for `getCycleCountNow()` lazy-sync,
**not** IRQ); Disk II still holds `cpu_` (for sub-instruction LSS
cycle accuracy on Q6L reads). MouseCard and SSC dropped `cpu_`.
Pinned: `slot_peripheral_irq_smoke_test.cpp`.

## Storage

### DiskImage

143 360-byte 5.25": `.dsk`/`.do` (DOS 3.3 skew) or `.po` (ProDOS).
Pre-nibblized into 35 × 6656-byte tracks. GCR per "Beneath Apple
DOS". Skew tables (physical → logical):

- DOS 3.3: `{0,7,14,6,13,5,12,4,11,3,10,2,9,1,8,15}`
- ProDOS:  `{0,8,1,9,2,10,3,11,4,12,5,13,6,14,7,15}`

Both produce same on-disk layout; only file-offset → physical-sector
mapping differs. Write-back via `saveDirty()` (`.dsk`/`.do`/`.po`/
`.nib` + `.2mg` envelopes + `.woz`) opt-in via
`setWriteBackEnabled(true)`; default off.

### Format detection

`detectFormat()` + `enum ImageKind`. `loadFile(path)` slurps once,
dispatches by content. Order: MacBinary strip → 2IMG envelope → WOZ
magic → 35×6656 NIB → 35×6384 CNib2 → 143 360-byte sector. Unknown
→ false + specific `lastError`. Inspired by AppleWin
`DiskImageHelper.cpp`.

- **Skew sniff** (143 360-byte branch): validates ProDOS vol-dir key
  block at `file[0x404]` (`.po`) vs `file[0xB04]` (`.dsk`),
  overrides extension when only the other position fits. Predicate:
  `prev=0`, plausible `next`, storage_type `$F`, name chars in
  `A-Z 0-9 .` (kills `$Fx`-byte spoofs in a DOS catalog).
- **2IMG**: 64 B header → format byte (0=DOS, 1=ProDOS, 2=NIB),
  flags (bit 0 = WP, bit 8 or 31 = vol# present), dataOffset,
  dataLength. Raw header + trailer captured into `twoImgHeaderRaw` /
  `twoImgTrailerRaw`; `saveDirty()` re-emits both so the envelope
  stays byte-identical across round-trip.
- **MacBinary**: 128 B prefix from old Mac downloads. AppleWin
  predicate (`b[0]==0`, name length [1..63], terminator + reserved
  zeros).
- **CNib2** (35×6384): pad to 6656/track on load with `$FF` (sync),
  truncate to 6384 on save.
- **Volume number**: per-image now (2IMG flags or default $FE),
  threaded through `nibblizeTrack(track, sectors, vol, skew)`.

Pinned: `disk_image_smoke_test.cpp`, `disk_skew_sniff_smoke_test.cpp`,
`disk_2mg_smoke_test.cpp`, `disk_2mg_writeback_smoke_test.cpp`,
`disk_macbinary_smoke_test.cpp`, `disk_cnib2_smoke_test.cpp`,
`disk_refuse_smoke_test.cpp`.

### 13-sector (5-and-3, pre-DOS-3.3)

DOS 3.1/3.2/3.2.1 used **13 sectors/track** with **5-and-3 GCR** (vs
DOS 3.3's 16-sector 6-and-2). A whole `.d13` / `.dsk` image is 35 × 13 ×
256 = **116480** bytes. `detectFormat` maps that size → `ImageKind::
Dos32_13` (always DOS order); `loadSectorImageFromBuffer` then calls
`nibblizeTrack13` and sets `sectorsPerTrack_ = 13` (`is13Sector()`).

Codec (`DiskImage.cpp`) is a **verbatim port of MAME
`formats/ap2_dsk.cpp` `a2_13sect_format`**: `nibblizeTrack13` /
`writeDataField13` (encode, `kTranslate5[32]`, addr prologue `D5 AA B5`,
data prologue `D5 AA AD`, 411-nibble data field) + `decodeTrack13` /
`kUntranslate5` (write-back). Physical interleave `sector = (i*10)%13`;
address-field number == file sector index == S. Pinned **byte-for-byte
round-trip** by `d13_roundtrip_smoke_test.cpp` (encode→decode all 35
tracks, no network).

**Boot wiring.** `DiskIICard` serves the **341-0009 boot PROM**
(`roms/disk2_13.rom`) at `$Cn00` while a 13-sector disk is mounted:
`insertDisk` recomputes `serving13_ = any 13s disk && bootRom13Loaded`,
and `slotRomRead` indexes `bootRom13`. You can't mix 13- and 16-sector
on one controller, so a single 13s disk flips the whole card. 13-sector
disks **force the bit-level LSS** (`useBitLss`) — the 341-0009 read loop
is tighter than POM2's 16s-tuned legacy 32-cycle gate (which mis-frames
the 5-and-3 read).

Key gotcha: the **read sequencer stays the 16-sector P6** (341-0028).
The LSS is bit-level and **encoding-agnostic** — it recovers the 5-and-3
nibble stream (D5 AA B5 / translate5) just as well as 6-and-2; the
5-and-3 *decode* is software (the 341-0009 boot / DOS 3.2 RWTS). The raw
341-0010 dump does **not** drive POM2's wozfdc-port `lssSync` (it yields
zero byte-ready), whereas the 16s P6 reads 13s correctly — verified by
`dos32_boot_trace` phase 0. (`loadLssRom13` exists but `p6Rom13` is
currently unused for reads.)

**Boots end-to-end.** A real DOS 3.2 master (`disks/dsk/DOS32STD.d13`,
and the raw-nibble reference `dos32std.nib`) boots via the ][+ Autostart
ROM: DOS loads across the disk (head seeks to track 13), runs its
greeting, and drops to its hooked `]` prompt with `LANGUAGE NOT
AVAILABLE` (the disk's Integer-BASIC HELLO can't run on the Applesoft
][+ — authentic behaviour, and proof DOS itself is running). Pinned by
`tests/dos32_boot_trace.cpp` (skip-if-absent). NB: needs an Autostart
machine (][+); the non-autostart Original monitor sits at `*` (no
auto-boot), and a 13s `.nib`/`.woz` is detected by content (D5 AA B5
scan) so it serves the 341-0009 PROM too.

### `.woz`

`isWoz()`. Verbatim port of MAME `lib/formats/woz_dsk.cpp`. WOZ
stores raw bit cells — survives copy protections that tweak
inter-byte timing (re-encoded `.dsk` synthesises idealised GCR and
loses the signature). WOZ1 (160 × 6656-byte slots, `bit_count`
@+6648 u16) and WOZ2 (160 × 8-byte TRK headers, data at
`starting_block × 512`, `bit_count` u32). Bits MSB-first. Each
track 0..34 sources bits from `TMAP[track*4]` (centre quarter-
track); sub-quarter-track positions (Locksmith, David-DOS) not yet
preserved.

**Write-back**: `loadWoz()` snapshots file to `wozRaw` + per-quarter-
track `(byteOff, byteLen, bitCount)`; `writeFlux()` splices into
`bitStream[qt]`; `saveDirty()` repacks + zeroes CRC32 (Applesauce
"not computed" sentinel) + rewrites in place. `isWriteProtected()`
honours both user toggle and `INFO.write_protected`.
`DiskIICard::insertDisk` forces `useBitLss=true` when any drive
holds WOZ — legacy 32-cycle gate cannot decode bit cells. Pinned:
`woz_load_smoke_test.cpp`, `woz_writeback_smoke_test.cpp`.

### WOZ2 `optimal_bit_timing`

INFO+39 (units of 125 ns) — bit-cell duration the imager recommends.
Default 32 = 4 µs = standard 5.25" cell at the 2 MHz LSS clock = 8
LSS cycles per cell. `loadWoz` reads the byte when
`info_version >= 2`, clamps to [8, 64], stores in
`DiskImage::optimalBitTiming`.

`DiskImage::lssCyclesPerCell() = optimalBitTiming / 4`.
`expandTrackFlux` emits each "1" cell at LSS cycle `i*cyc + cyc/2`
(centre); `trackPeriod` returns `bitCellCount * cyc`. A disk
mastered at e.g. 40 (5 µs) gets 10 LSS-cycle cells throughout.
Pinned: `woz_bit_timing_smoke_test.cpp` (obt 32 / 40 / 28 + WOZ1
fallback).

### DiskIICard

256-byte P5A boot PROM. Apple part 341-0027-A (CRC32 `ce7144f6`)
**embedded** in `DiskIICard.cpp` as `kBootPromDefault[256]`;
`loadBootRom("roms/disk2.rom")` overrides if a user dump is present.
PROM autodetects slot via `JSR $FF58 / TSX / LDA $0100,X`. Soft
switches `$C0E0-$C0EF`: phases, motor, drive_select, Q6L/Q6H,
Q7L/Q7H.

**Boot signature** (Apple II Ref Manual Appx C): `$Cn00` PROM starts
with `$20 ?? $00 $03` at offsets 1/3/5 (`JSR $Cn00 → JMP $C…20`
dispatch). `$Cn07` distinguishes Disk II / SmartPort (`$3C`, scanned
by F8 Autostart `341-0020-00`) from ProDOS block devices (`$01` for
non-removable HDV). F8 firmware ONLY auto-scans `$Cn07=$3C`; HDV
needs `PR#N` or `bootFromSlot`.

`bootFromSlot()` validates $Cn01/$Cn03/$Cn05 (JSR-dispatch trio) so
clicking "Boot" on a non-bootable card logs a warning and falls back
to `coldBoot`. `$Cn07=$3C` deliberately NOT validated — would reject
HDV (`$Cn07=$01`).

**Drive switching** via `selectDrive(int)` mirrors MAME
`machine/wozfdc.cpp:264-291` (MAME moved file `bus/a2bus → machine`
upstream). When motor active: flush old drive's in-flight write
(= MAME `mon_w(true)`), clear OLD drive's `revolutionStartLssCycle`
to `kNeverRev`, anchor NEW drive's `revolutionStartLssCycle` to
current `lssCycle` (= MAME `mon_w(false)` setting
`revolution_start_time`). Each drive owns its own `DiskImage` +
head qtr-track + nibble cursor; phase magnets + motor shared.

**Per-drive `revolutionStartLssCycle[2]`** matches MAME
`floppy_image_device::m_revolution_start_time`. Cleared to
`kNeverRev` on old drive in `selectDrive()`, on motor-off-delay
expiry (MODE_DELAY → MODE_IDLE, = MAME `mon_w(true)`), and at
controller reset. Disk angular position is
`(lssCycle - revolutionStartLssCycle[drive]) mod track_period`.
Pinned: `disk_drive2_smoke_test.cpp`,
`mame_lss_parity_smoke_test.cpp`.

### DiskII multi-instances

`"diskii"` is the only slot-card type allowed in more than one slot.
Lets //e users wire `Disk II slot 6 + Disk II slot 4` (4 drives
5.25"). Both cards load the same `disk2.rom` + `diskii_p6.rom`.
Storage per-card (each `DiskIICard` owns its 2 drives + LSS state).

`MainWindow_Slots.cpp::isDuplicate` returns false unconditionally
when type is `"diskii"`; `plugSlotsFromSettings::firstOccurrence`
walk short-circuits the same way.

**Primary vs secondary**: `MainWindow` keeps a flat
`std::vector<DiskIICard*> diskCards` in slot order ascending.
`diskCard` (legacy single pointer) is an alias for
`diskCards.empty() ? nullptr : diskCards.front()` — lowest-numbered
slot wins. Legacy menu paths take `diskCard`; panel render loop
iterates `diskCards`. Same shape for `diskPanels` +
`diskPanel` alias.

**Per-slot persistence**: `disk_path_slotN` / `disk_writeback_slotN`.
Primary also writes legacy unsuffixed keys so older POM2 builds
reading the new settings.ini still see the disk. Profile-switch
captures `savedDiskPaths[slot]` from live cards before tear-down.

**Per-panel render**: one ImGui window per plugged card, titled
`"Disk II (slot N)"`. Cascade offset 30 px down-left per index on
`FirstUseEver`. Auto-turbo is global (`anyMotorOn` across all cards).

**Insert-disk popup routing**: each panel's flag, latched into
`diskDialogTargetSlot` so popup routes to the right card even if
panel pointer churns (profile-switch race).

**IWM wiring**: only the slot-6 `DiskIICard` calls
`card->setIWM(&controller->iwm())`.

### Two read paths

- **Bit-level LSS** (default when `roms/diskii_p6.rom` present) —
  verbatim port of MAME `machine/wozfdc.cpp` + flux-event subset of
  `imagedev/floppy.cpp`. MAME `cycles` = 2× CPU clock. `lssSync(extra)`
  catches up from persistent `lssCycle` to
  `cyclesLimit = cpuCycleTotal*2 + extra`. PULSE from
  `DiskImage::getNextTransition(track, lssCycle)` (event at LSS cycle
  `cellIdx*8 + 4`, cell centre). Reads of `$C0EC` pass `extra=1`
  after `control()` for read-pipe latency. P6 PROM (Apple
  341-0028-A) indexed by `(state<<4) | (Q7<<3) | (Q6<<2) | (QA<<1) |
  (!PULSE)`. ALU dispatch: `0x0..0x7` CLR, `0x8/0xC` NOP, `0x9` SL0,
  `0xA/0xE` SR-with-WP, `0xB/0xF` LD from `last_6502_write`, `0xD`
  SL1. Pinned: `diskii_lss_smoke_test.cpp`,
  `mame_lss_parity_smoke_test.cpp`.
- **Legacy 32-cycle gate** (fallback) — `kCyclesPerNibble = 32`;
  nibble every 32 cycles, `byteReady` toggles for BPL spins. Good
  for stock DOS 3.3 / ProDOS RWTS. 2-3× faster than LSS in real
  boots.

### Bit-stream expansion

`DiskImage::bitAt(track, idx)` lazily walks nibble buffer, emits 8
cells per non-FF byte + 2 trailing zero cells per `$FF` that lives
inside a run of ≥5 consecutive `$FF`s. Sync-FF padding lets the LSS
lose alignment in sync gaps and resync on the next prologue. `.nib`
path skips padding (every byte = 8 cells, total 53248). Cache
invalidates on `writeNibbleAt`.

**Why ≥5 threshold** (`kSyncMinRun = 5`): `nibblizeTrack` lays down
5-byte `$FF` runs between an address field and its data field,
14-byte runs between sectors, long `$FF` tail wrapping into the
next-revolution leader. Any real sync gap is ≥5 bytes. Earlier ≥2
threshold treated naturally occurring 2-byte in-field `$FF` pairs
as sync (4-and-4 address checksum when `vol ^ track ^ sector == $FF`
+ 6-and-2 data running XOR producing 2-byte disk `$FF` from source
`$FF $00 $FF`).

### Flux-event view

`fluxEvents(track)` + `trackPeriod(track)` — one event per "1" cell
at LSS-cycle `cellIdx*8 + 4`. `getNextTransition` verbatim from
MAME `floppy_image_device::get_next_transition`, wraps across
revolutions. `writeFlux(track, start, end, count, transitions)`
splices flux window back into nibble buffer.

### ProDOSHardDiskCard (HDV) — synthetic-block model

`ProDOSHardDiskCard.{h,cpp}`. Slot-plugged ProDOS hard disk (default
slot 5, label `hdv`) backed by a raw `.hdv` / `.2mg` block image
(`loadImage`: 2IMG-envelope parse + write-back via `saveDirty`).

**Deliberate divergence from the "MAME source of truth" convention** —
unlike every other hardware subsystem here, this is intentionally NOT a
port. MAME has no generic HDV card: it emulates the *real boards*, each
running its **dumped ROM driver** against an **emulated bus chip**, with
the image stored as **CHD / raw** behind that chip. Canonical contrast:
`src/devices/bus/a2bus/a2cffa.cpp` — CFFA = `required_device<
ata_interface_device> m_ata`, `ROM_LOAD("cffa20ee02.bin", …)`, ProDOS
drives `m_ata->cs0_w / cs0_r` over `$C0xx`, storage = ATA `"hdd"`
device. Siblings: `a2scsi.cpp` / `a2hsscsi.cpp` (NCR 5380 / 53C80 +
CHD), `a2vulcan.cpp` / `a2zipdrive.cpp` (IDE), `a2corvus.cpp`.

POM2 follows the **AppleWin `HardDisk.cpp` lineage** instead: no ATA /
SCSI silicon, no real ROM. The card *fabricates* its 256-byte slot ROM
at runtime (`buildRom`, ProDOSHardDiskCard.cpp:307-431 — hand-assembled
6502) and that firmware talks to a host-implemented streaming protocol
on the per-slot device-select window `$C080 + slot×16`:
```
off 0  write   block LO byte               (resets stream offset)
off 1  write   block HI byte               (resets stream offset)
off 2  read    next byte of selected 512 B block (auto-incr, wraps 512)
off 2  write   next byte INTO block         (write-back-gated)
off 3  read    status: bit7 = no image, bit6 = write-protected
```
`deviceSelectRead / Write` (ProDOSHardDiskCard.cpp:244-305) move bytes
straight to / from the in-memory `image` vector — a host `memcpy`, **no
GCR, no flux, no LSS**. Slot ROM marks `$Cn07 = $01` (plain ProDOS block
device, not SmartPort `$3C`); JSR trio `$Cn01/03/05 = $20/$00/$03`. F8
Autostart won't scan `$01`, so boot is via `PR#n` / `bootFromSlot`
(see § DiskIICard).

**Consequences of the divergence** (documented so a future contributor
reads it as by-design, not an unfinished port):

- ✓ Mounts `.hdv` / `.2mg` directly — the dominant Apple II hard-disk
  formats — which MAME does *not* accept as a hard disk (CHD / raw only).
- ✓ No card-ROM dump required (a true `a2cffa` port would need
  `cffa20ee02.bin`).
- ✗ Cannot execute real CFFA / SCSI firmware, cannot mount CHD, and
  card-specific behaviour (partition maps, >32 MB schemes, SmartPort
  pass-through) is unreachable. The ATA-class port has since **landed**
  as `CffaCard` (below); the synthetic model stays as the zero-ROM
  default and is being repurposed toward ProDOS-volume / host-folder
  bridging (`loadImageFromBytes`, `pom2::buildVolumeFromFolder`).

Storage now shares a common backing — `Block512Backing.{h,cpp}` — between
the synthetic `ProDOSHardDiskCard` and the faithful `CffaCard`: it owns the
in-memory image, the 2IMG envelope (header + trailer preserved on
write-back), medium WP, dirty-block tracking, opt-in host-file write-back,
and host-folder synth volumes. Both cards also implement
`pom2::ProDOSBlockCard` (image-management interface) so the HDV Library,
disk-turbo, and persistence target either uniformly via
`MainWindow::hdvDevice()` (prefers CFFA when plugged); `ProDOSBlockCard`
in turn implements `MountableMediaCard` as a single fixed bay, so both
cards also appear in the Slot Configuration panel's media column
generically (see § Host control center).

Pinned: `hdv_card_smoke_test.cpp` (`hdv_card_smoke`),
`hdv_writeback_smoke_test.cpp` (header/trailer/WP/opt-in round-trip),
`hdv_mass_storage_smoke_test.cpp` (32 MB capacity boundary, 16-bit
block addressing, .2mg data-offset ≠ 64). Multi-partition images
(1 image = N ProDOS volumes, CFFA3000-style) are **not** supported —
one image = one unit = one volume.

### CffaCard (CFFA 2.0 — MAME-faithful IDE)

`CffaCard.{h,cpp}` + `AtaBlockDevice.{h,cpp}`. The MAME-faithful counterpart
to the synthetic HDV card: the **real 4 KB firmware dump** executed over an
**emulated ATA chip**, image stored as raw LBA behind it. Ported from MAME
`src/devices/bus/a2bus/a2cffa.cpp` (master; re-pin line ranges on touch per
the "MAME path drift refresher").

- **`AtaBlockDevice`** — ATA/IDE taskfile subset over `Block512Backing`,
  isomorphic to MAME `ata_interface_device` cs0 access: `cs0_r/cs0_w(reg)`
  with the 16-bit data register at reg 0. Honours IDENTIFY DEVICE ($EC),
  READ SECTOR(S) ($20/$C4), WRITE SECTOR(S) ($30/$C5), LBA28; unknown
  commands complete as no-ops. DRQ/BSY/DRDY PIO flow; no DMA/IRQ/CHS-I/O.
  Reusable for future Vulcan/Zip/Focus. Pinned: `ata_block_device_test`.
  **Gotcha**: the CFFA firmware sizes its partitions from IDENTIFY **words
  57-58** ("current capacity in sectors"), NOT words 60-61 (LBA28 total) —
  leaving 57-58 zero makes every image fail with "Could not boot partition 1
  / Err $28" (firmware `$CD35-$CD52` reads $C0n8/$C0n0 for words 57-58 after
  skipping 57 words). `fillIdentify` sets 57-58 = 60-61 = total, word 53
  bit 0 (current fields valid). Debug aid: `POM2_TRACE_CFFA=1` logs every ATA
  command; `tests/cffa_boot_dump --image X --slot N` boots //e via CFFA.
- **`CffaCard`** — `SlotPeripheral` + `ProDOSBlockCard`. Decode mirrors
  `a2cffa.cpp`: `read_c0nx`/`write_c0nx` ($C0nX) drive the ATA taskfile with
  the 8↔16-bit data latch ($C0n0 = high byte, $C0n8 = low byte + commit;
  $C0n3/$C0n4 toggle EEPROM write-enable); `read_cnxx` ($CnXX) returns
  `rom[off + slot*0x100]`; `read_c800`/`write_c800` ($C800) returns
  `rom[off + 0x800]`, writes WP-gated. The real firmware presents
  `$Cn07 = $3C`, so unlike the synthetic HDV ($01) the **F8 Autostart boots
  it natively** — no GUI shortcut needed.
- **ROM**: user-supplied `roms/cffa20ee02.bin` (6502) / `cffa20eec02.bin`
  (65C02), 4096 B exact (CRC `3ecafce5` / `fb3726f8`); plug-time probe picks
  the variant matching the CPU. Card type hidden from Slot Config when
  absent. Sourced from dreher.net `Run6_CDROM.zip` (`Firmware/V2.0/`).
- **Image**: `.hdv` / `.2mg` raw LBA backing (compat preserved). **CHD is
  phase 2** (separate container reader). Mounts via the HDV Library.
- Pinned: `cffa_card_smoke_test.cpp` (`cffa_card_smoke`, ROM-gated) drives
  the real firmware's card view through `Memory` — slot-ROM signature +
  $C0nX→ATA READ/WRITE round-trip. Full ProDOS-boot parity = MAME oracle
  `mame apple2ee -sl7 cffa2 -hard1 <img>` (romset in `~/mame_roms/cffa2/`).

### SmartPortCard (//e Liron-class)

`SmartPortCard.{h,cpp}`. Lets //e / II+ / II / //c mount Sony 800 K
disks through a slot-plugged Apple "Disk 3.5 Controller Card"
(Liron / 670-0186). Default slot 5. Reuses the same `Disk35Image`
pair that `EmulationController` owns.

**Architecture choice — block-level, no IWM**. Real Liron carries
IWM + tiny 6502 ROM with SmartPort dispatcher (a MAME-faithful port
would model the real chip + dumped driver). POM2's //c+ profile
already emulates the full IWM stack; for slot-plugged on //e we **skip
the bit-level path entirely** and expose blocks directly through a
streaming protocol identical to `ProDOSHardDiskCard`'s — the same
synthetic-block divergence documented under § ProDOSHardDiskCard (HDV)
(cf. MAME `a2cffa.cpp`).

**Device-select protocol** (`$C0nX`):
```
$C0n0 write  drive select (0 / 1)
$C0n1 write  block LO byte
$C0n2 write  block HI byte
$C0n3 read   next byte (auto-incr 512 B)
$C0n3 write  next byte INTO current block (WB-gated)
$C0n4 read   status: bit7 = no disk, bit6 = WP
```

Per-drive `streamOffset_[2]` wraps every 512 B; drive-select latches
`activeDrive_` and resets stream offset to 0.

**Slot ROM** (`buildRom`, 256 B with slot baked in):
```
$Cn00     JMP $Cn20            (boot vector)
$Cn01     $20                  ProDOS signature byte
$Cn03     $00
$Cn05     $03
$Cn07     $3C                  SmartPort signature
$CnFE     $13                  features/units mask (2 units)
$CnFF     $50                  driver entry offset
$Cn20-..  boot routine          (load block 0 of drive 1 → $0800)
$Cn50-..  ProDOS driver         (cmd switch + read/write loops)
$CnE0-..  error halt loop
```

Driver examines ProDOS `$43` unit byte. Bit 7 = drive: 0 → write `0`
to `$C0n0` (drive 1), 1 → write `1` (drive 2). Then standard 2-page
stream into ProDOS buffer at `$44/$45`. Write probes `$C0n4` bit 6
first; returns `$2B` (write-protected) without touching memory if WP.

**Boot wiring**: a library click (or CLI insert+boot) routes 3.5"/HDV to
the primary `SmartPortCard` and `controller->bootFromSlot(smartPortCard->
getSlot())` **on every profile that has one** — including //c-class, whose
built-in slot-5 card is the boot path (see next subsection). Only when no
SmartPort card is plugged at all does it fall back to `coldBoot()`.

**Per-unit storage**: each `SmartPortUnit` owns its bytes (no
`thread_local`). The HDV-flavoured `SmartPortHdvUnit` is now a thin
adapter over the shared `Block512Backing` (the same store behind
`ProDOSHardDiskCard` / `CffaCard`) — it gets the 2IMG envelope, dirty
tracking, medium-WP and opt-in write-back for free instead of
re-implementing them. Per-unit settings persist as
`smartport_slotN_unitK_{type,path,writeback}`. The card also implements
`MountableMediaCard` over its 2 units (see § Host control center) so the
Slot Configuration panel's media column drives them generically.

Pinned: `smartport_card_smoke_test.cpp`,
`smartport_mixed_units_smoke_test.cpp`.

### //c-class on-board SmartPort (3.5" + HDV boot)

The //c, //c rev0/3/4 and //c+ profiles all boot 3.5" **and** HDV through
a host-served SmartPort block device — the **same `SmartPortCard`** as the
//e path, but built into slot 5. Why not the faithful IWM/Sony path:

- Real //c-class masks **all** slot ROM behind a forced INTCXROM
  (`IIcClassProfile::forcesIntCxRom()` ≡ true), so a normal slot card's
  `$Cn00` firmware is invisible → `bootFromSlot` reads internal ROM, not
  the card, and no slot card can ever boot.
- MAME doesn't model 3.5"/SmartPort on the plain //c at all: apple2c0/c3/c4
  use the `A2BUS_IWM` card (`bus/a2bus/a2iwm.cpp`) which wires **two 5.25"
  drives only** ("WANTED: there are no ROM dumps"). Only //c+ (apple2cp)
  models 3.5" via the motherboard IWM+MIG+Sony (ported as `SmartPortHub`)
  — but that boot path doesn't reach a bootable disk (the full IWM bit
  shifter isn't modelled). So there is no faithful //c 3.5"/HDV to port.

Mechanism:

- **ROM hole** (`Memory::memRead`, inside the //c-class INTCXROM branch):
  `$C500-$C5FF` (bank 0) returns `slots.slotRomRead(addr)` instead of
  internal ROM **iff** `iicSmartPortArmed_` AND
  `slots.peripheral(5)->exposesIicOnboardRom()` (a unit holds media). Bank 1
  is handled earlier by `internalRomRead`, so the hole is bank-0 only
  (preserves the //c+ alt firmware's `$C500` data).
- **"Armed" gate — the critical subtlety** (`Memory::setIicSmartPortArmed`).
  The stub MUST NOT be visible during the //c ROM's *own* autostart: real
  //c rev0/3/4 keeps its SmartPort firmware at `$C500`, and the autostart +
  a booted ProDOS call into `$C5xx` entries that the real firmware provides
  but the block stub does not. Substituting the stub there corrupted a
  **multi-device** boot (a Disk II disk in slot 6 + media in the on-board
  SmartPort): ProDOS booted from slot 6, enumerated slot 5, ran the wrong
  `$C5xx` bytes, and splattered stub code over the text page (the "garbled
  Apple //c banner" bug). So `EmulationController::bootFromSlot` **arms** the
  gate (explicit GUI/CLI boot only) and **every** `coldBoot`/`softReset`/
  `hardReset` disarms it. Net: a normal reboot always sees the real `$C500`
  firmware (clean Disk II boot / banner); the on-board SmartPort boots only
  via the Disk Library / Slot Configuration "Boot" button (→ `bootFromSlot`).
  Trade-off: persisted SmartPort media does not auto-reboot — use "Boot".
- **Device-select** (`$C0D0-$C0DF` = slot 5) is never masked on //c-class
  (only `$C100-$CFFF` is), so the block stub's `$C0D0-$C0D4` protocol
  already reaches the bus unchanged.
- **Stub signature/STATUS fixes** (`SmartPortCard::buildRom`): `$Cn07` is
  `$01` (ProDOS non-removable block device), NOT `$3C` — `$3C` is the **Disk
  II** marker and made a //c treat the slot-5 SmartPort as a second Disk II.
  The ProDOS STATUS call (cmd `$00`, `$CnC0` routine) returns the unit's
  block count in X/Y via `$C0n5/$C0n6`, so a volume scanner that enumerates
  the device (ProDOS ONLINE / BITSY) sizes it correctly instead of crashing.
- **Routing** (`MainWindow`): `routeMount35` uses the SmartPort card on all
  profiles; `routeMountHdv` + `ensureHdvCardForBoot` send //c-class HDV to
  the slot-5 SmartPort (a `SmartPortHdvUnit`), **never** to a cffa/hdv slot
  card (masked, unbootable there). Profile //c gains a `smartport35` built-in
  at slot 5 (//c+ already had one). `bootFromSlot(5)` then boots either unit.

Verified on screen for //c and //c+ × {3.5", HDV} (explicit boot) and a
clean multi-device reboot. Pinned by `tests/iic_onboard_smartport_test.cpp`
(armed ROM-hole gating + block I/O via `Memory`); `tests/iic_dual_boot_trace.cpp`
is a headless diagnostic for the garble. See `project_iic_smartport_boot`.

### 3.5" mechanical sounds

`Sony35Drive` carries a `FloppySoundSink* sound_` set by
`EmulationController` to the same `FloppySoundDevice` Disk II uses
— shares 5.25"/3.5" sample sets and volume/mute persistence.

**Cycle stamping**: `seekPhaseW(uint8_t phases, uint64_t emuCycles)`
takes a CPU-cycle counter at strobe edge. `SmartPortHub::onIwmPhases`
forwards `IWMDevice::emuCycles()`. `strobeWriteRegister` cases:
```
0x1  step       moved && sound_->step(track_, lastStrobeCycle_)
0x2  motor on   if (!motorOn_) sound_->motor(true,  hasDisk)
0x3  motor off  if ( motorOn_) sound_->motor(false, hasDisk)
0x4  eject      image->eject() ; sound_->click()
```

`moved` gates so head bumps at track 0 or 79 don't click. Motor
transitions edge-only (idempotent re-strobes don't replay sample).

`EmulationController::mount35` / `eject35` call
`drive->emitInsertClick()` after `notifyMediaChange()`.

### ProDOS host folder

(`prodos_disk/`) `ProDOSVolume` synthesises a read-only ProDOS
volume from a host folder. Blocks 0-1 boot (zeroed), 2-5 volume dir
key + 3 ext blocks (51 entries max), block 6 bitmap (4096 blocks =
2 MB cap), 7+ data + sapling indexes.

Scope: flat dir only; ≤ 51 files; ≤ 128 KB per file (seedling +
sapling, tree skipped with warning); type from extension; filenames
sanitised to `A-Z/0-9/.` with collision suffixes `.1/.2`.

Wiring: HDV slot 5 panel's Library shows synthetic `[host folder]
prodos_disk/` entry. Click → `buildVolumeFromFolder` →
`ProDOSHardDiskCard::loadImageFromBytes`. **No auto-boot** — user
boots ProDOS from another slot, then `/HOST/` appears as slot 5
drive (`CAT,S5,D1`). Read-only: driver returns `$2B` on writes.
Pinned: `prodos_volume_smoke_test.cpp`.

### Snapshot

`SnapshotIO`. `POM2SNAP` magic, named 8-byte sections, format shared
with POM1. Captures CPU + RAM + soft-switch display state. **Disk II
deliberately excluded** — would need mounted-image identity + head
position + dirty bits per track.

## IWM (//c+ on-board)

`IWMDevice.{h,cpp}` — verbatim port of MAME
`src/devices/machine/iwm.{h,cpp}`. Full state machine
(`MODE_IDLE/ACTIVE/DELAY` for `m_active`, `MODE_READ/WRITE` for
`m_rw`; `S_IDLE/SR_WINDOW_EDGE_0/SR_WINDOW_EDGE_1` for read bit-
window walker; `SW_WINDOW_LOAD/MIDDLE/END/UNDERRUN` for write).
Drives flux transitions via `DiskImage::getNextTransition` for 5.25"
or `Sony35Drive::nextTransition` for 3.5".

**Live wiring**:

1. `EmulationController` constructs the IWM, hands it to
   `Memory::setIWM`. Reset paths (`hardReset`, `coldBoot`,
   `bootFromSlot`) call `iwm.reset()`.
2. Memory routes `$C0E0-$C0EF` on `isIIcPlus` profiles through
   `iwmDevice->read/write` — matches MAME `apple2e.cpp:2798-2801`
   gating on `m_isiicplus && slot == 6`. Plain //c uses
   `A2BUS_DISKIING` at sl6 instead. On //c+ slot-6 DiskIICard
   still observes the access for motor sound / turbo / head tracking,
   but the byte returned to CPU is the IWM's when `iwmAuthoritative`
   is true (default).
3. DiskIICard pushes `setFloppy(image, qt)` updates to the IWM from
   `insertDisk` / `ejectDisk` / `selectDrive` / `seekPhaseW`. IWM's
   `nextTransition` queries
   `DiskImage::getNextTransition(qt, from*2) / 2` — POM2's flux
   events live in LSS-cycle space, IWM in CPU-cycle space.
4. `EmulationController::tick(cpuCycleTotal)` pulses the IWM once
   per video frame so the 1-emulated-second drive-disable timer
   drains when the //c+ alt firmware stops poking $C0Ex.

`iwmAuthoritative` runtime toggle (`Memory::setIWMAuthoritative` or
`POM2_IWM_AUTHORITATIVE=0`) drops data path back to DiskIICard's LSS
for A/B comparison. IWM state machine still advances either way, so
mode / status / WHD reads stay coherent. Pinned:
`tests/iicplus_boot_trace.cpp`.

**Window-size scaling**: MAME's `iwm.cpp:290-301 half_window_size` /
`:302-313 window_size` are IWM-clock ticks (//c+ runs IWM off
A2BUS_7M ≈ 7.16 MHz). POM2 ticks the IWM at `POM2_CPU_CLOCK_HZ`
(~1.02 MHz) for a single cycle counter — constants divided by ~7
to keep a "bit cell" window ≈ 4 µs.

**MAME parity audit 2026-05-16** — second line-by-line pass against
upstream. Fixed:

- **`data_w` handshake gate** (MAME `iwm.cpp:311-318`). POM2 cleared
  WHD bit 7 on every sync+write `data_w`; MAME does it only when
  mode bit 0 (latched handshake) is set. `wsh_` mirrored from
  `data` immediately in sync+write. Pinned by
  `iwm_device_smoke_test.cpp::testDataWLatchedMode`.
- **`mon_w` propagation** (MAME `iwm.cpp:194-195 / 234 / 91`).
  `set_floppy` / `set_sony35` drop mon_w on old drive and raise on
  new one when motor enabled. MODE_ACTIVE entry fires
  `mon_w(false)`, timer-mode motor-off and 1-sec drive-disable
  drain fire `mon_w(true)`. Wired only for `Sony35Drive`
  (DiskIICard owns 5.25" motor + audio intentionally).
- **`devsel_cb`** (MAME `iwm.cpp:79 / 236 / 92`). Fires at three
  extra moments: `device_reset` (`cb(0)`), MODE_DELAY entry in
  non-timer mode (`cb(1 or 2)` before timer), `update_timer_tick`
  exit (`cb(0)`). Pinned by `testDevselFiresOnReset`.
- **`set_write_splice`** call site (MAME `iwm.cpp:218-221`). Fires
  through `DiskImage::setWriteSplice` (still a stub — TODO.md «WOZ1
  splice point»). Wiring in place for when the stub gets a body.
- **`read_register_update_delay`** (MAME `iwm.cpp:363-366`). Returned
  1 in both branches (bug); now 1 vs 2 so mode bit 3 changes the
  delay.

Not yet ported:
- Q3 fast clock (1.86 MHz) used on Mac/IIgs but not //c+.
- Full `DiskImage::setWriteSplice` body — IWM propagates splice
  cycle, DiskImage stores it but doesn't honour during WOZ re-master.

## SmartPort 3.5" stack

`Disk35Image` + `Sony35Drive` + `SmartPortHub` — full Sony GCR
read+write for //c+. Build chronology lives in `CHANGELOG.md`.

*Image + drive*. `Disk35Image` loads 800 K `.po` / `.2mg`.
`Sony35Drive` responds to IWM phase-as-command bus (MAME
`mac_floppy.cpp::seek_phase_w` + Apple //gs hardware ref register
table) and to MIG-driven `m_35sel`/`m_intdrive`/`m_hdsel` toggles
(`apple2e.cpp:638-679 recalc_active_device`). `senseR()` returns the
active-low register file (`/INSERTED`, `/TRACK0`, `/READY`,
`/MOTOR ON`, `/SWITCHED`, …) the //c+ firmware probes.

*IWM wiring*. `IWMDevice` exposes `phasesCb_` / `devselCb_` /
`sel35Cb_` (MAME `iwm_device::phases_cb / devsel_cb / sel35_cb`);
`EmulationController` wires them through `SmartPortHub::attach`.
`nextTransition()` dispatches between `DiskImage*` and
`Sony35Drive*` via `setFloppy` / `setSony35` overloads.
$C0EE-status WPT bit consults `Sony35Drive::senseR()`.

*No-disk noise flux*. When the active drive has no media (or sits on
an unformatted track) `nextTransition()` would otherwise return
`INT64_MAX`, so the read FSM only ever shifts in 0-bits, `data_` stays
`$00`, bit-7 ("byte ready") never asserts, and the boot firmware's
wait-for-byte loop spins forever on the un-cleared power-up screen.
A real spinning empty drive feeds the IWM noise flux instead, so
`nextTransition()` falls back to `noiseTransition()` — a deterministic
LCG keyed on the read-window index that straddles `windowSize()`
boundaries so the SR accumulates a mix of 1s/0s and keeps emitting
garbage bytes with bit-7 set. This is what lets the //c reach **"Check
Disk Drive."** and the //c+ reach **"UNABLE TO FIND A BOOTABLE DISK
ONLINE."** at power-on with no disk (the 16 KB //c / II+ / IIe spin in
the dumb Disk II PROM on a HOME-cleared screen — real-hardware
behaviour, unchanged). MAME models this implicitly: the floppy reports
no transition but the IWM's window timer still cycles the SR; POM2
collapsed that timer into `nextTransition()`, so the noise is injected
there. Pinned by `tests/iic_nodisk_boot_trace.cpp`.

*GCR encoder* (`Sony35Drive.cpp`, verbatim MAME
`flopimg.cpp::build_mac_track_gcr 2017-2106`). Five concentric speed
zones (`kCellsPerRev[5] = {76950, 70695, 64234, 57749, 51388}`,
MAME `:2019-2027`), per-zone CPU-cycles-per-rev =
`60 × POM2_CPU_CLOCK_HZ / RPM`, 64-entry `kGcr6fw[]` write table
(MAME line 967), `gcr6Encode(va,vb,vc)` 3-in-4-out packer (MAME line
512). Per-sector: 8× self-sync (384 cells) + D5AA96 address prologue
+ 5 GCR header bytes + DEAAFF address epilogue + 2× self-sync gap +
D5AAAD data prologue + 174 groups of 3-in-4-out + 4-byte data
checksum + DEAAFFFF data epilogue = 6208 cells. Block-to-physical
2:1 interleave (`si = (si+2) % ns; if(si==0) si++`,
`apple_gcr_format::load 372-382`).

*Flux write-back*. `Sony35Drive::writeFlux` splices IWM-emitted
flux into cached cell buffer, runs GCR→blocks decoder (MAME
`flopimg.cpp:2107 extract_sectors_from_track_mac_gcr6`). Recovered
sectors that differ push back via `writeBlock`; image flushes to
`.po` via `saveDirty()` on `eject35` or shutdown. Write-protect
honoured: `setWriteBackEnabled(true)` short-circuits `writeFlux`.
Nibbliser port of `flopimg.cpp:1530 generate_nibbles_from_bitstream`
handles MSB-first byte alignment + zero-cell skip. **Gotcha**:
cycle↔cell rounding uses round-to-nearest on decode side; encoder
uses floor on `cycleForCell = i × period / n`. Without symmetric
rounding, integer-truncated `2.024 → 2` pushed every transition one
cell early and lost the first sector's address-field marker.

*UI / CLI / persistence*. `Disk35Controller_ImGui` panel renders two
Sony slots (internal = on-board //c+, external = SmartPort daisy-
chain). Mount/Eject, last-error, library scanner picking up `.po` /
`.2mg` of right size under `disks35/` (falls back to `disks/`).
Toggle persisted `show_disk35_panel`. `mount35(idx, path)` /
`eject35(idx)` under `stateMutex`. CLI: `--35-disk1` / `--35-disk2`;
settings: `disk35_path_1` / `disk35_path_2` (re-mounted on launch).

Pinned: `tests/smartport_35_smoke_test.cpp` — image load + size
guard, empty-slot SENSE, in-slot SENSE, motor strobe, hub recalc
(devsel=1+35sel=true AND devsel=2+intdrive=true), IWM-to-drive phase
forwarding, address+data marker placement on track 0 (12 of each),
IWM bit-cell walk to first `$AA`, full
encode→flux→splice→decode→block-readback round trip,
write-protect short-circuit.

## Peripherals

### Super Serial Card (slot 2) + telnet bridge

Minimal 6551-ACIA shape at `$C0A8-$C0AB` (data/status/cmd/ctrl).
Status bit 4 = TDRE (always 1), bit 3 = RDRF (RX queue), bits 5/6 =
DCD/DSR (TCP state). Unconnected `$C0A8` returns 0.

Slot ROM `$C200-$C2FF`: SSC autodetect bytes (`$Cn05=$38`,
`$Cn07=$18`, `$Cn0B=$01`, `$Cn0C=$31`); `JMP $Cn20` skips them.
PR#2 hooks CSWL/CSWH (`$36`/`$37`) → `$C2B0`; IN#2 hooks KSWL/KSWH
(`$38`/`$39`) → `$C2E0` (load + ORA #$80 for Apple high-bit
convention). Reset clears ring buffers.

TCP listener on `127.0.0.1:port` (default 6502); one client. 4 KB
rings; telnet IAC (WILL/WONT/DO/DONT + 2-byte cmds + `$FF $FF`
literal) swallowed by `swallowTelnetIac` so stock `telnet` connects.
`TCP_NODELAY` on. Auto-plugged at startup; listener starts only when
`ssc_listening=true`. Port + state persisted.

### ProDOS clock card (slot 4)

ThunderClock+ compatible. **ProDOS does NOT route through slot ROM**
for clock reads — at boot it copies its hardcoded ThunderClock
driver into RAM (~$D742), patches `$BF06-$BF08` to JMP it, then
driver speaks via device-select. Slot ROM only needs the detection
signature.

Slot ROM `$C400-$C4FF`: signature bytes `$08, $28, $58, $70` at
offsets `0, 2, 4, 6`. Odd-offset fillers (CLD/CLD/SEI) + `BVS +0`
form benign fall-through; `$Cs08 = RTS`.

**uPD1990AC bit-bang at `$C0C0`**:
```
write bit 0 = DATA_IN; bit 1 = CLK; bit 2 = STB; bits 3..5 = C0/C1/C2;
      bit 6 = IRQ enable ($40)
read  bit 5 = IRQ asserted; bit 7 = DATA_OUT (LSB of shift register)
```

Mode `0b011` = `MODE_TIME_READ`: arm via `$C0C0=$18`, pulse STB
(`$1C`) to latch host time into 48-bit shift register, drop STB,
read bit 7 + pulse CLK (`$1A`/`$18`) 48 times → 6 BCD bytes
(sec, min, hour, day, (month<<4)|dow, year). Mode `0b010` =
`MODE_TIME_SET`: load 48 bits via DATA_IN + 48 CLK, then STB-in-
TIME_SET commits. `commitTimeSetFromShiftReg()` decodes BCD via
`std::mktime`, captures delta as `userOffsetSeconds`;
`effectiveTime()` composes `timeFn() + offset`.

**TP (Timing-Pulse) interrupts**. The chip's TP output toggles at a
software-selected rate; the ThunderClock+ gates it onto the slot IRQ
line, giving a periodic interrupt source (clock/scheduler utilities).

- *Rates* — MAME source of truth `upd1990a.cpp:248-257` (TP modes) +
  `:176-181` (REGISTER_HOLD default). `programTpTimer()` decodes the
  latched C0/C1/C2 mode on the STB rising edge: dividers 512/128/16/8
  against the 32.768 kHz XTAL → **64 / 256 / 2048 / 4096 Hz** (modes
  4-7), plus 64 Hz for REGISTER_HOLD. SHIFT/TIME_SET/TIME_READ leave
  TP at its prior rate (MAME normal-mode behaviour). The interval
  timers (1/10/30/60 s, modes 8-15) need the uPD4990A 4-bit serial
  command and are unreachable on the parallel uPD1990AC — not
  modelled. `setTpRate()` converts to CPU cycles/toggle; `advanceCycles()`
  drives the toggle and fires the IRQ on each rising edge.
- *IRQ wiring* — **POM2-original**: MAME's `a2thunderclock.cpp` never
  binds the chip's `tp_callback`, so there is no upstream model. The
  wiring follows the **ThunderClock Plus manual ch. V**: `$C0n0` bit 6
  (`$40`) is the enable latch (`irqEnabled_`); a TP rising edge sets the
  request FF (`irqPending_`) → `assertIrq(true)` while enabled; **any**
  device-select read/write clears the request (enable latch persists, so
  a periodic source keeps ticking); read `$C0n0` bit 5 is the
  "interrupt asserted" flag (manual 5-3 polling); RESET disables.
  Pinned: `clock_card_smoke_test.cpp` — `testTpRatesProduceExpectedPulseCounts`,
  `testNoIrqWhenDisabled`, `testEnableLatchTracksBit6`,
  `testInterruptAssertedFlagBit5`, `testResetStopsTpAndIrq`.

**MODE_SHIFT lax-gating divergence**: POM2 deliberately diverges
from MAME `upd1990a.cpp:312-327` which gates CLK-shift on
`m_c == MODE_SHIFT`. POM2 shifts on every CLK rising edge regardless
— because ProDOS's hardcoded ThunderClock driver pulses CLK while
still in MODE_TIME_READ. Strict gating breaks stock ProDOS;
observed hardware permits the shortcut. DATA_IN still latched into
MSB on every CLK rise. Pinned:
`clock_card_smoke_test.cpp::testShiftLaxAcrossModes`.

### Mouse Card

(slot 4 by convention) Verbatim port of MAME
`bus/a2bus/mouse.cpp`. Pieces:
- **M68705P3** MCU (Apple 341-0269, 2 KB mask ROM). Paced at 2× CPU
  clock from `advanceCycles()` via fractional accumulator.
- **MC6821** PIA — bus side at `$C0n0-$C0n3`.
- **8516 EPROM** — 2 KB Apple-side slot ROM (Apple 341-0270-c),
  bank-switched into `$Cn00-$CnFF` via PIA PortB bits 1-3 (8 banks
  of 256; `bank = (PortB & 0x0E) << 7`).

PIA ↔ MCU bridge:
```
PIA PortA  ↔ MCU PortA            (bidir, pull-ups)
PIA PB4-7  ↔ MCU PC0-3
PIA PB1-3  → EPROM A8-10          (bank select)
MCU PB6    → slot IRQ (active low; cached, transitions only)
MCU PB7    ← mouse button (active low)
MCU PB0=X dir, PB1=X gate, PB2=Y dir, PB3=Y gate (quadrature)
```
POM2 labels the X pair `X0`/`X1` lower-bit-first (X0=PB0=dir,
X1=PB1=gate); MAME's `mouse.cpp` uses the opposite digits (X1=0x01=dir,
X0=0x02=gate). Same bits, same behaviour — only the label differs; Y
labels match MAME. `updateAxis` is line-for-line MAME `update_axis<>`.

Host routing: `MainWindow::onMouseMove` / `onMouseButton` →
`setHostMouse(rawX, rawY, button)` (clipped to screen rect). MCU
computes deltas via 8-bit subtraction with wrap; POM2 emits **at
most one quadrature edge per axis per MCU PortB read** (matches MAME
`m_last` / `m_count`).

**ROM gating**: BOTH ROMs required to plug. Slot-config UI greys
entry when missing; `plugSlotsFromSettings` refuses with a `Mouse`
log warn. Defaults: `roms/mouse_341-0270-c.bin` +
`roms/mouse_341-0269.bin`.

**Not modelled** (firmware-invisible): PAL16R4 chip-select sequencer
at U2A, PIA PortB bit 0 sync latch, motion clamping (MCU does it).
Pinned: `mouse_card_smoke_test.cpp`,
`mouse_card_quadrature_smoke_test.cpp`, and
`mouse_card_axis_parity_test.cpp` — the latter boots the **real
firmware** (both ROMs) on a full M6502+Memory, drives ProDOS
`InitMouse`/`SetMouse`/`ReadMouse` from a poked 6502 stub, and asserts
an identical host ramp moves X and Y equally (caught X==Y==800 for a
+800 px ramp).

**Historical "X stuck at ~8 px" symptom** (resolved): the card was
never at fault — `updateAxis` is symmetric and MAME-faithful. The cause
was a since-removed (`f8280bb`) `MainWindow` position-correction feedback
loop that read the ProDOS screen holes with a **scrambled layout**
(`appleX = $0478+s | ($04F8+s << 8)` mixes X-low with **Y-low**;
correct interleave is Xlo=$0478+s, Ylo=$04F8+s, Xhi=$0578+s,
Yhi=$05F8+s). The bad feedback pinned X. No screen-hole reads remain in
production; `mouse_card_axis_parity_test.cpp` guards against recurrence.

### Joystick / paddles

`JoystickInput` polls all 16 GLFW slots each UI frame (hot-plug).
One binding drives PADL(0/1) + PB0/1/2. PADL(2/3) read centered
(127). **Paddle RC** in `Memory::softSwitchAccess`: `$C064-$C067`
returns `0x80` while `(cycleCounter - paddleLatchCycle) <
paddleValue × 11`. `$C070` arms the latch. 11-cycle constant is
rough Apple II RC step.

## UI (ImGui)

`MainWindow` — menu bar + screen + emulation panel + on-demand
panels. Owns the screen GL texture. Auto-plugs Disk II in slot 6 if
`roms/disk2.rom` exists. F9 (screenshot), F11 (soft reset), F12
(hard reset) routed unconditionally even when ImGui has focus.

### MainWindow Pimpl-light

`MainWindow.h` is **forward-decl-only** for every plugin / panel /
controller type — includes only `M6502.h` and `imgui.h`. 18 owning
member types live behind `std::unique_ptr<T>`; constructor +
destructor + accessor bodies out-of-line in `MainWindow.cpp` so
unique_ptr destruction sees a complete type.

Compile-time benefit: `touch CassetteDeck_ImGui.h` → 2 TUs rebuild ;
`touch MainWindow.h` → 4 TUs (MainWindow*.cpp + main.cpp,
irreducible).

Non-owning `*Card` pointers (`diskCard`, `hdvCard`, …) stay raw —
`SlotBus` owns the cards. Forward-declared in the same block as the
panels.

- **MemoryViewer_ImGui** — hex + ASCII over full 64 KB. Reads via
  `Memory::data()` directly under `stateMutex` (held by MainWindow
  during `render()`) so viewer never triggers soft-switch side
  effects. Edits go through `Memory::memWrite` (ROM protection
  applies). Per-byte change-flash via frame-counter delta. Search
  handles hex sequences (`A9 FF 48`) and ASCII (both raw and
  high-bit-set).
- **Disassembler6502** — stateless `(mem*, pc) → mnemonic + length`.
- **main.cpp** — GLFW char/key callbacks gated by ImGui keyboard
  capture so editing widgets don't leak into Apple II.
- **Screenshot (F9)** — `screenshot_NNN.ppm` (P6 binary RGB) in cwd.

## Host control center (Slot Configuration + Floppy Emu)

Two host-side facilities sit *above* the slot bus — neither is a bus
device. Both are data-in / actions-out ImGui panels driven from a
snapshot `MainWindow` builds under `stateMutex` and apply the returned
actions itself (mount/eject/persist/restart) — the same contract as
every other `*_ImGui` panel.

### MountableMediaCard + SlotCardCatalog

`MountableMediaCard.h` is the capability mix-in that lets the GUI drive
*any* card with mountable media bays generically — no
`if (cardKey == "...")` ladder. Orthogonal **host-side** interface (NOT a
bus concern; the `$Cnxx`/`$C0nx` dispatch path never touches it), same
"mix-in alongside `SlotPeripheral`" pattern as `ProDOSBlockCard`. API:
`bayCount()`, `bayInfo(bay) → MediaBayInfo`, `mountBay` / `ejectBay` /
`setBayWriteBack`, plus type-select for bays whose kind the user may pick
(`bayTypeOptions` / `setBayType`).

- `ProDOSBlockCard` implements it as a single fixed bay, so both
  HDV-class cards (`ProDOSHardDiskCard`, `CffaCard`) gain a bay for free.
- `SmartPortCard` implements it directly over its 2 units, advertising
  per-bay type select (`""` empty / `"35"` 3.5" / `"hdv"` HDV).

`SlotCardCatalog.h` is the single list of user-assignable card types
(`kCardTypes`, index 0 = empty slot) + the ROM-presence probes
(`mouseRomsPresent()`, `cffaRomPresent()`) that gate the conditional
entries (Mouse needs both mouse ROMs, CFFA needs `cffa20ee02/eec02.bin`).
Extracted from `MainWindow_Slots.cpp` so the Slot Configuration panel
drives its dropdowns + built-in-name resolution from one source.

### Slot Configuration

`MainWindow::renderSlotConfigPanel` (`MainWindow_Slots.cpp`). One
**two-column** window (Machine → Slot Configuration) is the whole
expansion-bus control center — it absorbed the old standalone "Slot
Manager" panel (deleted 2026-05-25; `SlotManager_ImGui.*` removed):

- **LEFT — card assignment.** The AUX 80-col row (IIe-class) plus slots
  1-7. Each slot is a `kCardTypes` dropdown, EXCEPT profile built-ins
  (`builtInSlots[s]`) which render as a locked, greyed `LabelText` with a
  "card — built-in …" badge (so //c/+/c+'s SSC/Mouse/SmartPort/Disk II are
  visibly fixed). `diskii` is multi-instance (never a duplicate); other
  keys red-flag duplicates and disable Apply. Apply persists `slot_N_card`
  and calls `restartEmulationFromSettings()`.

- **RIGHT — internal disks + mountable ports.** A **live SlotBus walk**
  (`bus.peripheral(s)`, no global `*Card` pointers, so it's correct with
  multiple cards of a kind). For each plugged card:
  - `dynamic_cast<MountableMediaCard*>` → render its bays inline: status
    dot (grey empty / orange WP / green loaded), per-bay type select
    (SmartPort units only), path InputText + Mount/Eject, write-back
    checkbox, and a **Boot slot** button (`bootFromSlot`). Covers
    SmartPort (2 units), CFFA + synthetic HDV (1 bay).
  - else `dynamic_cast<DiskIICard*>` → the internal 5.25" drives (1-2),
    each with path + Insert/Eject, plus Boot slot. Drive 1 persists to
    `disk_path_slotN`; drive 2 is session-only (legacy single-key scheme).

  Each media action takes `stateMutex` and calls `persistMediaBay()`
  (SmartPort per-unit / CFFA per-slot / synthetic-HDV global keys), then
  `settings->save()`.

Settings: `show_slot_config` (now persisted). Menu: Machine → Slot
Configuration… Pinned: `slot_multi_card_smoke_test.cpp`
(`slot_multi_card_smoke`).

### Floppy Emu (BMOW)

`FloppyEmuDevice.{h,cpp}` + `FloppyEmu_ImGui.{h,cpp}` — a faithful model
of the Big Mess o' Wires **Floppy Emu** (bigmessowires.com): an SD-card +
OLED + 3-button gadget that plugs into the disk port and *becomes* a
drive. POM2 already emulates every drive type the Emu can present, so the
class models the device's *defining* behaviour rather than another FDC:

- the persistent emulation **MODE** (its NVRAM) — four `FloppyEmuMode`s
  mapped onto POM2's drives: `Disk525` (140K, Disk II), `Disk35` (800K
  dumb 3.5"), `Unidisk35` (800K smart, ejectable), `SmartportHD` (≤32 MB
  ProDOS block). The device's Dual-5.25 and Smartport-Unit-2 (IIgs
  daisy-chain boot trick) modes are out of scope for v1.
- the SD-card **file explorer** — bounded to the SD root, ".." +
  dirs-first listing sorted case-insensitively, format-filtered per mode
  (`acceptsFile`: 5.25 → dsk/do/po/nib/woz/2mg ; 3.5/Unidisk →
  dsk/do/po/2mg ; Smartport → po/hdv/2mg).
- **favorites** — `favdisks.txt` in the SD root: optional `automount N`
  first line (0 never / 1 first / 2 most-recent) then one image path per
  line (relative to the SD root or absolute), matching the real device's
  format.

The actual mounting is **routed by MainWindow** into the existing
controller cards (`DiskIICard` for 5.25/3.5, `SmartPortCard` units for
HDV) — the device only picks the image + the mode. The core is
deliberately UI- and emulator-agnostic (no ImGui / MainWindow / SlotBus)
so format filtering, SD navigation, and favdisks parsing unit-test in
isolation. Reference: BMOW Floppy Emu Model C manual §3 (Disk Emulation
Mode / Selecting an Image / Favorites) + §5 (Apple II Usage).

`FloppyEmu_ImGui` draws the device's face: a stylised 128×64
blue-on-black OLED + the three hardware buttons (PREV / NEXT / SELECT),
two OLED views (SD File Explorer + Settings → Disk Emulation Mode), the
mode always shown in the header.

The virtual "SD card" is the `floppyemu/` folder — kept SEPARATE from the
Disk Library folders (`disks/` `disks35/` `hdv/`) because the Emu is its
own card (see `floppyemu/README.txt`); subfolders are walked. Settings:
`floppyemu_mode`, `floppyemu_sd_root`, `show_floppy_emu`. Menu: Devices →
Floppy Emu (BMOW). Pinned: `floppy_emu_smoke_test.cpp`
(`floppy_emu_smoke`).

## Profile switching

`SystemProfile.h/.cpp`. Pinned: `system_profile_smoke_test.cpp`.

**32 KB ROM disambiguation**: //e and //c dumps share the 32 KB size
but encode firmware in OPPOSITE halves. `loadAppleIIRom` takes a
`pickLower16KFor32K` flag set by `applyProfile`:

- //e (`apple2e.rom`): firmware in UPPER 16 KB (file `0x4000-0x7FFF`),
  lower half is character ROM. `pickLower=false`.
- //c / //c+ (`apple2c-32Kv0.rom`, `apple2cp.rom`): TWO 16 KB
  firmware banks. Bank 0 in LOWER half (mapped at reset, cold-start
  at $FA62), bank 1 in upper half (alt firmware: AppleTalk,
  MouseText, SmartPort). `pickLower=true`; upper 16 KB stashed into
  `iicAltFirmware` for the `$C028` toggle.

Both halves can carry valid-looking reset vectors so cannot
auto-detect from bytes — profile is source of truth.

**$C028 ROMBANK** (//c-class): MAME `apple2e.cpp:1907-1923` flips
`m_romswitch` on any `$C02x` access when `m_isiic`. POM2 mirrors
with `isIIcClass`. Alt-firmware read paths additionally require
`iicHasAltBank` (32 KB only). `resetSoftSwitches` clears
`iicRomBank` so cold-boot always starts in bank 0. On II/II+/IIe
(`!isIIcClass`), `$C02x` falls through to cassette toggle.
Pinned: `system_profile_smoke_test.cpp::testIicRomBankSwitch`.

**//c-class INTCXROM override**: //c / //c+ have no physical slots,
so internal motherboard ROM mapped at `$C100-$CFFF` always. POM2
gates `internalIORom` dispatch on `(MF_INTCXROM || isIIcClass)` in
`memRead`/`memWrite` — matches MAME `apple2e.cpp:1619-1631
update_slotrom_banks`. Without this the //c reset routine at $FA62
would `JSR $CE4D` into an empty slot bus on first instructions.
`loadAppleIIRom` and `resetSoftSwitches` set `iieMemMode |=
MF_INTCXROM` when `isIIcClass`. Pinned:
`system_profile_smoke_test.cpp::testIicInternalRomAlwaysMapped`.

**Built-in slot locks** (`ProfileConfig::builtInSlots`): each
profile carries an `std::array<std::optional<BuiltInSlot>, 8>`. //c
locks sl2 (SSC modem), sl4 (Mouse), sl6 (Disk II). //c+ adds sl5
(SmartPort 3.5" via IWM). II / II+ / IIe-U / IIe leave all slots
free. `plugSlotsFromSettings` overrides `slotCards[s]` with the
forced cardKey regardless of persisted `slot_N_card`.
`renderSlotConfigPanel` renders locked slots as
`BeginDisabled(true)` `LabelText` with a "built-in" badge. Pinned:
`testBuiltInSlots`.

**//c+ MIG + IWM handshake** (//c+-only): //c+ alt firmware (bank 1)
drives a MIG gate-array + IWM. POM2 models the minimum needed for
cold boot:

- **MIG** (MAME `apple2e.cpp:598-704 mig_r / mig_w`). Memory hosts
  `migRam[0x800]`, `migPage`, `migIntDrive`, `migHdSel`; routes two
  MIG windows in bank-1 expansion ROM **only when `isIIcPlus &&
  iicRomBank`**:
  - `$CC00-$CCFF` → `migOffset 0x000-0x0FF` (drive enable/disable,
    IWM reset)
  - `$CE00-$CEFF` → `migOffset 0x200-0x2FF` (MIG RAM + auto-incr,
    3.5" head select, MIG page reset)

  3.5"-side decodes drive `SmartPortHub::setMig35Sel` /
  `setMigIntDrive`; hub's `recalc_active_device` (verbatim port of
  MAME `apple2e.cpp:724-770`). MAME `apple2e.cpp:1917-1922` resets
  `migPage` + `m_intdrive` + `m_35sel` when ROMSWITCH transitions
  back to bank 0; POM2 mirrors. `migWrite(0x40)` calls
  `iwmDevice->reset()` so //c+ alt firmware's per-boot IWM reset
  clears stale state.

- **IWM mode register + WHD handshake** on `DiskIICard` (MAME
  `iwm.cpp:103-114 read / 256-269 mode_w`). `DiskIICard` tracks
  `iwmMode` + resting `iwmWhd = 0xBF` and intercepts `$C0nE` /
  `$C0nC` / `$C0nF` combinations in both bit-LSS path and legacy
  32-cycle gate. Plain Disk II software never drives Q6+Q7 to
  mode-set state, so existing tests unaffected; //c+ alt firmware's
  IWM probe at `$E512-$E522` and write-ready loop at `$C8A6-$C8A9`
  / `$C960-$C965` both clear with these hooks. Without them the
  //c+ Monitor cold-reset path hangs before any banner.

**Profile switching = full cold reset** via
`MainWindow::applyProfile(SystemProfile)`. Order matters:

1. Stop worker.
2. Tear down slot cards under state mutex (Mockingboard's
   `AudioSource` detached from `AudioDevice` FIRST).
3. Wipe RAM/aux/LC + reset soft switches.
4. **`setIIEMode(...)` BEFORE `loadAppleIIRom`**.
5. Load ROMs (with `pickLower16KFor32K`).
6. Re-plug slots from settings.
7. Re-mount previously inserted disks/HDVs.
8. `resolveCpuMode()` (honours `cpu_mode_override`).
9. Reset cycles/frame.
10. `hardReset()`.
11. Restart worker.
12. Persist `system_profile`.
13. Refresh GLFW window title.

CLI `--preset` triggers the same path. Aliases: `apple2`,
`apple2plus`, `apple2e`, `apple2c`, `apple2cplus`, `//e`, `//c`,
`//c+`. `cpu_mode_override` = `auto|nmos|65c02`.

## CLI

`CliDispatcher`. Three phases: **A** parse, **B** pre-boot
(preset/ROM/snapshot-load/`--load addr:file`), **C** post-boot
(tape ops/paste/run/step).

Flags: `--preset ii|ii+|iie|iic|iic+`, `--speed`, `--cpu-max`,
`--tape`, `--35-disk1` / `--35-disk2`, `--load addr:file`, `--run`,
`--paste`, `--step`, `--play`/`--rec`/`--rewind`,
`--snapshot-save`/`--snapshot-load`.

**Parser / runner split** (2026-05-23): `parseCli` + `printUsage` +
`resolveSaveTapePath` live in `CliDispatcher.cpp` with **no
`EmulationController` dependency**; the Phase-C runner (`runDeferredActions`
+ `runLoad` / `runPasteFile`) moved to `CliRunner.cpp`. This lets
`tests/cli_kiosk_test.cpp` link the parser against just `DiskImage.cpp`.

**Positional disk + `--kiosk`**. A first non-flag argument is captured into
`CliPlan::bootDiskPath`; `--kiosk` sets `CliPlan::kiosk`. main.cpp:

- Picks the slot by type via `classifyDiskForSlot(path)` (`DiskImage.*` —
  `DiskSlotClass` = `Floppy525` / `Sony35` / `Hdv`, mirroring the Disk
  Library's `accept525/35/hdv` predicates), then calls
  `MainWindow::insertAndBootImage(path, err)`. That method shares the
  routing with the Disk Library UI: the `routeMount35` / `routeMountHdv`
  lambdas were promoted to `MainWindow` methods so both callers route
  identically (SmartPort unit auto-create, //c+ on-board hub, HDV card vs
  SmartPort unit 0). 5.25" → `DiskIICard::insertDisk` + `bootFromSlot`.
- **HDV auto-provision**: an HDV needs an HDV/SmartPort card, but a saved
  config may have only Disk II cards (the Library boot just errors there).
  `ensureHdvCardForBoot()` plugs a `ProDOSHardDiskCard` into a free slot
  (prefers 7) for the session if none is present, so `POM2 game.hdv` boots
  regardless of the saved slot layout. The plug is **not persisted** — see
  the no-save note below — so the user's GUI slot config is untouched.
- **No persistence in kiosk**: the `~MainWindow` `settings->save()` is
  gated `if (!kiosk_)`, so a kiosk session never writes `state.cfg` (the
  auto-plugged HDV card, the booted disk path, etc. stay session-local).
  `imgui.ini` is likewise disabled (main.cpp). Bare `POM2 <disk>` in GUI
  *does* persist normally.
- Defers the boot to a small frame countdown in the main loop (runs on the
  UI thread between frames, after the worker is up + slots plugged), so the
  SlotBus mutation doesn't race the CPU thread. Works in GUI and kiosk.
- `--kiosk` → exclusive full-screen window from the primary monitor's video
  mode (`glfwGetVideoMode` bit-depth/refresh hints + `glfwCreateWindow(...,
  monitor, ...)`); `io.IniFilename = nullptr`; `MainWindow::setKioskMode`.
  `render()` short-circuits to `renderKiosk()` — one borderless
  full-viewport window (`drawScreenImage()` letterboxed on black), no menu
  bar / toolbar / panels / dialogs. The window closes only via the OS.
