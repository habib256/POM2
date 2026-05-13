# CLAUDE.md

Architecture / invariants / gotchas for the **emulator side** of POM2. User
walkthrough → `README.md`.

## Project Overview

Apple II / II+ / IIe emulator (Dear ImGui, MOS 6502 + 64 KB / 128 KB RAM
via Language Card + soft switches + text / 80-col / lo-res / hi-res
framebuffer + 1-bit speaker + cassette + joystick + Disk II in slot 6).
One concern per file: each `.cpp/.h` pair owns one subsystem.

## Build & Run

```bash
./setup_imgui.sh             # one-time deps + clones imgui/
cd build && cmake .. && make # build → build/POM2
./run_emulator.sh            # runs from repo root so roms/ probes resolve
```

ROMs are user-provided. **`roms/apple2e.rom` (16 KB or 32 KB) takes
precedence at startup** — its presence flips MainWindow into IIe mode
(128 KB, IIe soft switches, 80-col, internal $C100-$CFFF I/O ROM).
Otherwise the binary falls back to `roms/apple2.rom` (12 KB or 16 KB)
and runs as a II+. No menu / CLI flag — pure auto-detect on file
presence.

`Memory::loadAppleIIRom` accepts both common //e dump variants:
- **16 KB**: $C000-$FFFF directly. The standard MAME / AppleWin layout.
- **32 KB**: "system + video" combined dump. The standard 16 KB
  firmware lives at file offsets `0x4000-0x7FFF` (so the reset vector
  at file offset `0x7FFC` maps to `$FFFC`); the lower 16 KB carries
  the video / character data which is loaded through `loadCharRom`
  separately. Both halves are sliced and the upper one is fed through
  the normal 16 KB IIe split path so `internalIORom` gets populated.
  Falling through to the generic "best-effort" branch instead would
  load all 32 KB linearly at `$8000` and skip the internalIORom split,
  leaving `$C100-$CFFF` empty when `INTCXROM=on` — the //e firmware
  then crashes before reaching the slot 6 boot PROM.

## Architecture

### Core

- **M6502** — MOS 6502 with full instruction set, IRQ / NMI / BRK, decimal
  mode, Klaus Dormann functional-test clean. Plus the most-common 65C02
  additions: STZ (zp / zp,X / abs / abs,X), BRA, INA / DEA, PHX / PHY /
  PLX / PLY, BIT #imm / zp,X / abs,X, TSB / TRB, JMP (abs,X), and the
  zero-page-indirect mode for ORA / AND / EOR / ADC / STA / LDA / CMP /
  SBC. Plus the **Rockwell** additions RMB0..7 (`$07/$17/…/$77`),
  SMB0..7 (`$87/$97/…/$F7`), BBR0..7 (`$0F/$1F/…/$7F`), BBS0..7
  (`$8F/$9F/…/$FF`); and the **WDC** WAI / STP halts (`$CB`/`$DB`),
  modelled by parking PC at the instruction so an IRQ wakes the CPU.
  Pinned by `tests/cmos_6502_smoke_test.cpp`. Without these, 65C02-
  targeted ProDOS software (most IIe-Enhanced and IIc games on `.2mg`
  hard-disk volumes) hits an opcode like `9C` (STZ abs) or `B2` (LDA
  (zp)) on the first instruction and either no-ops past it or freezes
  (the original 6502 mapped `B2` to a halt). The Rockwell `$FF` (BBS7)
  byte matters specifically because it's also the canonical fill in
  empty slot ROMs; mis-decoding it as a 1-byte NOP makes PC drift
  through slot 2/3/4 territory and trap unpredictably.
  `setProgramCounter()` is the back-door used by the Klaus harness port.
- **CpuClock.h** — `POM2_CPU_CLOCK_HZ = 1 022 727`. The Apple II
  master oscillator is 14.31818 MHz; the CPU runs at that divided by 14.
  The "long cycle" every 65 cycles (TV scan-line alignment) is **not**
  modelled — nominal rate only.
- **Memory** — flat 64 KB motherboard mirror + per-byte writable bitmap,
  plus a 16 KB Apple II/II+ Language Card overlay. ROM regions
  ($C100-$C7FF and $D000-$FFFF when LC RAM is not write-enabled) reject
  writes silently. **`memRead` / `memWrite` route every $C000-$C07F access
  through `softSwitchAccess()`**; RAM accesses bypass the dispatch entirely
  (cheap). Reset vector defaults to $F800 so a no-ROM boot still has
  somewhere to land.
  - **Apple IIe extension** (`isIIE()`). When `setIIEMode(true)` is called
    before `loadAppleIIRom`, Memory adds: a second 64 KB `aux` array, a
    4 KB `internalIORom` for $C100-$CFFF, and an aux Language Card bank
    trio. The IIe paging soft switches at $C000-$C00F (80STORE / RAMRD /
    RAMWRT / INTCXROM / ALTZP / SLOTC3ROM / 80COL / ALTCHAR) update an
    `iieMemMode` bitmask; `iieMemRead` / `iieMemWrite` route per address
    range (ALTZP for $0000-$01FF, RAMRD/WRT for $0200-$BFFF, 80STORE+PAGE2
    swap for $0400-$07FF and $2000-$3FFF when HIRES). $C100-$CFFF reads
    pull from `internalIORom` when INTCXROM=on; $C300-$C3FF stays internal
    while SLOTC3ROM=off (so `PR#3` reaches the 80-col firmware out of the
    box). Status reads at $C013-$C018, $C01E, $C01F mirror the bits. All
    IIe code paths are gated behind `iieMode` — II+ behaviour is
    untouched. Pinned by `tests/iie_memory_smoke_test.cpp`.
  - **Soft switches** are toggled by *either* read or write to their slot.
    $C000 returns the keyboard latch; the high bit of the byte reflects
    whether a key is ready (the strobe). $C010 clears the strobe (read or
    write). $C030 toggles the speaker flip-flop on **every** access in the
    $C030-$C03F range (alias decoded). $C050-$C057 = display mode pairs
    (text/graphics, mixed/full, page 1/2, lo/hi-res). $C064-$C067 paddles,
    $C070 paddle latch.
  - **Language Card** — built into `Memory`, not a normal slot card because
    it remaps `$D000-$FFFF`. `$C080-$C08F` implements bank 1/2 selection,
    ROM-vs-RAM reads, and the two-access prewrite latch for write enable.
    `$D000-$DFFF` has two 4 KB banks; `$E000-$FFFF` is one shared 8 KB RAM.
    `$C011`/`$C012` report bank-2/read-RAM status for ProDOS-style probes.
  - **Apple II text/HGR row interleave**: not a bug, a Woz feature reusing
    the row counter to refresh DRAM. Formulae in `Apple2Display.cpp`:
    text: `addr = base + 0x80*(y%8) + 0x28*(y/8)`,
    HGR:  `addr = base + 0x400*(y%8) + 0x80*((y/8)%8) + 0x28*(y/64)`.
    Memory layout above is the only reason a sequential 24 × 40 character
    write to $0400 doesn't render line-by-line.

### Emulation orchestration

- **EmulationController** — single worker thread. Holds the M6502 + Memory.
  Sleeps 50 ms when Stopped, runs `cyclesPerFrame` worth of CPU per 60 Hz
  tick when Running. Single `stateMutex` guarding the CPU/Memory pair —
  the UI thread takes it briefly each frame to render the framebuffer.
- **Apple2Display** — pure software renderer into a 280×192 RGBA buffer
  (or a 560×192 buffer in IIe 80-col modes — `width()`/`height()` reflect
  whichever is live each frame). Reads soft-switch state via
  `Memory::getDisplayState()` (cheap mutex copy) and the flat RAM array
  directly. **Owns no GL state** — UI uploads via `glTexImage2D` (on size
  change) or `glTexSubImage2D`. Built-in 5×7 ASCII font fallback when the
  user hasn't provided a character ROM. Lo-res palette is the
  //gs-corrected approximation. Hi-res has four `HiResMode` variants:
  `ColorNTSC` (default — 14 KB LUT indexed by `(parity << 8) | byte`, 39
  inter-byte seam fix-ups, optional additive horizontal glow) and three
  monochrome phosphors — `MonoWhite` / `MonoGreen` (P31) / `MonoAmber`.
  Text inverse attribute renders statically (2 Hz flashing animation
  pending).
  - **80-column text** (IIe). When `setAuxMemory(...)` has been called and
    the soft-switch state shows `eightyCol && textMode`, `render80ColumnText`
    interleaves aux RAM (even cells: 0,2,…,78) with main RAM (odd cells:
    1,3,…,79) into the 560-wide framebuffer. Mixed mode (HIRES + 80COL +
    MIXED) renders HGR top 20 rows into the 280-wide frame, doubles them
    horizontally into the 560-wide one, then overlays 80-col text rows
    20..23. ALTCHAR is plumbed through but currently a no-op against the
    built-in 5×7 fallback (a real charset ROM would consult the second
    2 KB bank for mousetext + non-flashing inverse).
  - **DHGR** (IIe). When `eightyCol && hiRes && dhgr && !textMode`,
    `renderDhgr` interleaves aux byte (dots `c*14..c*14+6`) with main byte
    (dots `c*14+7..c*14+13`) per byte position c, building a 560-dot
    stream. **Three color paths**, picked by `hiResMode`, all matching
    MAME `apple/apple2video.cpp` source-of-truth:
    - **`ColorNTSC`** — composite artifact decode. 7-bit sliding window
      over the raw 560-dot stream → indexes the shared 128-entry
      `kArtifactColorLut` (same table as HGR, MAME `artifact_color_lut[0]`)
      → `rotl4b(value, absX + 1)` extracts the 4-bit lo-res palette
      index. Per-pixel decode (560 lookups/scanline) reproduces the
      inter-cell fringing real composite IIe monitors show. The `+1`
      matches MAME's `is_80_column = 1` for DHGR in
      `render_line_artifact_color`.
    - **`ChatMauveRGB`** — clean RGB-card 4-dot block decode. Each 4
      consecutive dots → raw nibble (bit 0 = leftmost), rotated left by
      1 (matches MAME's `dhgr_update` Video-7 RGB color path
      `rotl4(n, 1)`), indexes **`kChatMauveLoResPalette`** so the "two
      distinct grays" trademark survives in DHGR. Palette values
      verbatim from AppleWin `RGBMonitor.cpp::PaletteRGB_Feline` —
      an empirical white-balanced RGB capture of a real Le Chat Mauve
      "Feline" board (idx 5 = olive-gray `rgb(0x9f,0x97,0x7e)`,
      idx 10 = mauve-gray `rgb(0x78,0x68,0x7f)`). MAME has no separate
      Chat Mauve palette (its Video-7 mode reuses `apple2_palette[]`
      which collapses 5 ≡ 10), so we follow AppleWin instead.
    - **`Mono*`** — each dot = luminance bit × phosphor tint. No
      artifact decoding. Persistence sized for 280-wide HGR, so DHGR
      mono renders without afterglow.

    Mixed mode = DHGR top 160 + 80-col text bottom 4 rows. Pinned by
    `tests/dhgr_render_smoke_test.cpp` (composite, RGB-card, Chat Mauve
    palette, mauve-regression and per-pixel coloring assertions).

    **Test-framework gotcha:** tests build with the parent project's
    Release flags (`-O3 -DNDEBUG`), which would silently strip every
    `assert()`. `tests/CMakeLists.txt` adds `-UNDEBUG` so test asserts
    actually run. Without that override, every smoke test prints "OK"
    regardless of whether its checks held.

### Audio (speaker + cassette)

- **AudioDevice** — miniaudio mono float32 mixer. Negotiates the actual
  sample rate with the OS (often 48 kHz on Apple Silicon even when 44.1
  is requested) — cycle-driven sources MUST query
  `getActualSampleRate()` or playback drifts by the rate ratio.
  `addSource(AudioSource*)` is thread-safe; the data callback runs on
  miniaudio's thread.
- **SpeakerDevice** — `AudioSource` for the 1-bit speaker. The CPU side
  records each `$C030-$C03F` toggle with a sub-instruction timestamp
  (`cycleCounter + cpu->getCurrentInstructionCycles()`) into a 16 K-event
  ring; the audio thread drains it into a square wave at the negotiated
  rate, applies a 1-pole low-pass (~5 kHz, models the speaker cone) and a
  DC blocker (avoids drift across long silence). Auto catch-up if the
  drain lags > 100 ms. UI volume + mute are atomics.
- **CassetteDevice** — Apple II `$C020` (output toggle) and `$C060` (sign
  of the audio comparator). Drives a separate `AudioSource` so tape loads
  click through the speakers; Play / Record / Rewind are exposed by the
  procedural `CassetteDeck_ImGui` panel (378×404, Font Awesome icons —
  the runtime falls back to '?' glyphs if `fonts/fa-solid-900.ttf` is
  missing). $C061-$C067 are **not** cassette aliases on the II/II+ —
  they're paddles + buttons, dispatched separately in `softSwitchAccess`.

### Joystick / paddles

- **JoystickInput** — polls all 16 GLFW slots each UI frame so a
  hot-plugged pad becomes selectable immediately. One active binding
  drives PADL(0)/PADL(1) from the host X/Y axis and PB0/PB1/PB2 from
  buttons 0/1/2. Auto-binds the first present host on first poll.
  PADL(2)/PADL(3) read centred (127).
- **Paddle RC discharge** is modelled inside `Memory::softSwitchAccess`:
  `$C064-$C067` returns `0x80` while `(cycleCounter - paddleLatchCycle)
  < paddleValue × 11`. `$C070` arms the latch. The 11-cycle constant is
  the rough Apple II RC-step duration — close enough for paddle-driven
  games, not a precision PASCAL clone.
- **JoystickPanel_ImGui** — host-pad picker, deadzone slider, axis-invert
  toggles, live axis / button readout. Visible via Hardware → Joystick.

### Slot bus

- **SlotBus** + **SlotPeripheral** — 8-slot dispatcher. `Memory::memRead`
  / `memWrite` route four windows: `$C080-$C0FF` device-select (16 bytes
  per slot N at `$C080+N*16`; slot 0 = Language Card hook, 1-7 =
  expansion), `$C100-$C7FF` slot ROM (256 bytes per slot 1-7), and
  `$C800-$CFFF` shared expansion ROM owned by whichever slot most
  recently saw a `$CnXX` access. `$CFFF` (read or write) deactivates
  the active slot; auto-latch on slot-ROM access. `advanceCycles()`
  forwards to every plugged card (Disk II head stepping today).
  Apple II Ctrl-Reset propagates `onReset()` to all cards.

### Disk II (slot 6)

- **DiskImage** — loads a 143 360-byte 5.25" floppy image: `.dsk` / `.do`
  in DOS 3.3 logical sector order, or `.po` in ProDOS sector order
  (extension-sniffed; `loadFile(path, SectorOrder)` lets a caller force
  either skew). Pre-nibblizes it into 35 × 6656-byte track buffers.
  GCR encoding follows "Beneath Apple DOS": 14-byte sync gap, address
  field (`D5 AA 96 [vol/trk/sec/chk in 4-and-4] DE AA EB`), 5-byte sync
  gap, data field (`D5 AA AD [86 low-2-bit nibbles REVERSED + 256 high-6
  nibbles + 1 XOR checksum] DE AA EB`). Sector skew tables (physical →
  logical):
    DOS 3.3:  `{0,7,14,6,13,5,12,4,11,3,10,2,9,1,8,15}`
    ProDOS:   `{0,8,1,9,2,10,3,11,4,12,5,13,6,14,7,15}`
  Both produce the same on-disk physical layout (16 sectors per track);
  only the file-offset → physical-sector mapping differs. The `.po`
  variant is what ProDOS-bootable disks ship as. Read-only — the source
  file is never modified.
  - **`.woz` support** (`isWoz()`). Verbatim port of MAME's
    `src/lib/formats/woz_dsk.cpp` chunk parser. WOZ stores raw bit cells
    (the LSS's natural input) instead of nibbles or sectors, which is
    why .woz disks survive copy protections that tweak inter-byte
    timing or bit alignment — re-encoded `.dsk` synthesises idealised
    GCR and loses the protection signature. Both **WOZ1** (160 fixed
    6656-byte slots; `bit_count` at offset +6648 LE u16) and **WOZ2**
    (160 × 8-byte TRK headers; track data at file offset
    `starting_block × 512` with `bit_count` as u32) are supported.
    Bits are MSB-first within each byte. `loadWoz()` walks INFO/TMAP/
    TRKS in any order, ignores META/WRIT/FLUX, and populates
    `bitStream[track]` directly — bypassing the nibble synthesis path.
    Each whole track 0..34 sources its bits from `TMAP[track*4]` (the
    canonical centre-of-track quarter-track slot); per-quarter-track
    sub-positions used by some advanced protections (Locksmith,
    David-DOS) are not yet preserved — first-cut tradeoff for the
    minimum patch that lets stock-protection disks boot.
    **Write-back is supported**: `loadWoz()` snapshots the entire
    file into `wozRaw` plus a per-quarter-track
    `(byteOff, byteLen, bitCount)` triple; `writeFlux()` splices
    cell-by-cell into `bitStream[qt]`; `saveDirty()` repacks dirty
    quarter-tracks back into `wozRaw` (MSB-first), zeroes the CRC32
    header bytes (Applesauce sentinel for "not computed") and rewrites
    the file in place. `isWriteProtected()` honours both the user
    `setWriteBackEnabled` toggle and `INFO.write_protected` (the
    physical-disk WP byte) so a WP'd source stays WP regardless of
    opt-in. `DiskIICard::insertDisk` forces `useBitLss=true` when any
    drive holds a WOZ — the legacy 32-cycle nibble gate cannot decode
    bit-cell data. Pinned by `tests/woz_load_smoke_test.cpp` (loader +
    LSS recovery) and `tests/woz_writeback_smoke_test.cpp` (WOZ1 + WOZ2
    write-back round-trip + INFO.write_protected honoured).
- **DiskIICard** — SlotPeripheral plugged in slot 6. Holds the 256-byte
  P5A boot PROM (`roms/disk2.rom` from AppleWin) at `slotRomRead`; the
  PROM autodetects its slot via the `JSR $FF58 / TSX / LDA $0100,X`
  trick, so the Apple II main ROM must be loaded for boot to work.
  Soft switches at `$C0E0-$C0EF`: phases 0-3 off/on, motor off/on,
  drive_select (`$C0nA` = drive 1 / `$C0nB` = drive 2 — both wired,
  `activeDrive` routes the LSS and the legacy gate to `images[0]` /
  `images[1]`), Q6L/Q6H (shift/load), Q7L/Q7H (read/write — write goes
  via the LSS shifter when `useBitLss` is on, otherwise the legacy
  32-cycle gate). Drive_select switches go through
  `selectDrive(int newDrive)` which mirrors MAME `wozfdc.cpp:219-241`
  on `mon_w(true)`/`mon_w(false)`: when motor is active, the old
  drive's in-flight write buffer is flushed (commits to disk), then
  `lssCycle` is reset to `cpuCycleTotal*2` so the new drive's flux
  reads start at position 0 of its current track (MAME's
  `revolution_start_time = now`). Important for multi-drive copy
  protections (Locksmith, Copy II Plus) that swap drives mid-spin.
  Each drive carries its own `DiskImage`, head position in
  quarter-tracks (`headQuarterTrack[2]`), and nibble-buffer cursor
  (`trackPos[2]`); phase magnets and motor-on are controller state and
  shared between drives (matching real hardware where only the selected
  drive's stepper responds). Pinned by
  `tests/disk_drive2_smoke_test.cpp` (LSS + legacy paths, plus per-drive
  head independence).
  - **Two read paths share the same data register**:
    - **Bit-level LSS** (default when `roms/diskii_p6.rom` is present)
      — *verbatim* port of MAME `src/devices/machine/wozfdc.cpp` and the
      flux-event subset of `src/devices/imagedev/floppy.cpp` (fetched
      2026-05-09 from `github.com/mamedev/mame` master). MAME's `cycles`
      runs at 2× CPU clock (= LSS clock); one PROM lookup per LSS cycle.
      `lssSync(extra)` catches the LSS up from a persistent `lssCycle`
      counter to a `cyclesLimit = cpuCycleTotal*2 + extra` derived from
      `advanceCycles()` totals. PULSE detection comes from
      `DiskImage::getNextTransition(track, lssCycle)` — the flux-event
      view, with one event per "1" cell at LSS-cycle `cellIdx*8 + 4`
      (cell centre). Reads of $C0EC pass `extra=1` after `control()` to
      model MAME's read-pipe latency. The 256-byte P6 sequencer PROM
      (Apple part 341-0028-A, embedded as a default + load override from
      `roms/diskii_p6.rom`) is indexed by
      `(state << 4) | (Q7 << 3) | (Q6 << 2) | (QA << 1) | (!PULSE)`. ALU
      dispatch: `0x0..0x7` CLR, `0x8/0xC` NOP, `0x9` SL0, `0xA/0xE`
      SR-with-WP, `0xB/0xF` LD from `last_6502_write`, `0xD` SL1. Pinned
      by `tests/diskii_lss_smoke_test.cpp` and
      `tests/mame_lss_parity_smoke_test.cpp` (the latter checks the flux
      cache layout, `getNextTransition` wrap semantics, and the
      `writeFlux` round-trip against MAME's algorithmic invariants).
    - **Legacy 32-cycle gate** (fallback when no P6 ROM is loaded) —
      `kCyclesPerNibble = 32`; one nibble appears at the data register
      every 32 cycles, with `byteReady` toggling so the BPL spin holds
      until the next nibble is in. Good enough for stock DOS 3.3 / ProDOS
      RWTS. Roughly 2-3× faster than the LSS in real-world boots — kept
      as the path the disk_boot / disk_write_controller / dos33_save /
      prodos_save smoke tests exercise (they don't load the P6 ROM).
  - **Bit-stream expansion** — `DiskImage::bitAt(track, idx)` lazily
    walks the 6656-byte nibble buffer once per track and emits 8 cells
    per non-FF byte, plus 2 trailing zero cells per $FF in a run of 2+
    consecutive $FFs. Sync-FF padding is what lets the LSS *lose*
    alignment in sync gaps and *re-sync* on the next data prologue. The
    `.nib` raw-nibble path skips sync padding (every byte = exactly 8
    cells, total 53248 cells). Cache invalidates on `writeNibbleAt`.
  - **Flux-event view** (`fluxEvents(track)` / `trackPeriod(track)`) —
    sits on top of the bit-cell stream and emits one event per "1" cell
    at LSS-cycle `cellIdx*8 + 4`. `getNextTransition(track, fromLssCycle)`
    is the verbatim port of MAME `floppy_image_device::get_next_transition`,
    wrapping across revolutions when no further events exist past the
    cursor. `writeFlux(track, start, end, count, transitions)` splices
    a window of flux events back into the nibble buffer (cell-windowed,
    8-bit packed), used by the LSS write side on Q7 falling edge and on
    the 30-event pre-emptive flush. Lazy build, invalidated together
    with `bitStream` on `writeNibbleAt`/`eject`/`loadFile`.
- **DiskController_ImGui** — minimal status panel: PROM loaded LED,
  motor LED, current track / half-track / nibble cursor, Insert / Eject
  buttons. No procedural-art chassis like the cassette deck — the
  Disk II's mechanism is hidden inside the case anyway.
- **MainWindow** — auto-plugs the card in slot 6 if `roms/disk2.rom` is
  present at startup. Insert dialog enumerates `disks/*.dsk`. Boot from
  Applesoft: `PR#6`.
- **Snapshot**: Disk II state is **deliberately excluded** from
  `SnapshotIO`. Save-state would have to capture mounted-image identity
  and head position; first-cut decision is to keep snapshots focused on
  CPU + RAM + soft switches.

### Super Serial Card (slot 2) + telnet bridge

- **SuperSerialCard** — minimal 6551-ACIA-shaped card in slot 2, paired
  with a TCP listener so a host terminal can talk to the running Apple
  II as a serial peer. Soft switches at `$C0A8-$C0AB`: data, status
  (`bit 4 = TDRE` always 1, `bit 3 = RDRF` follows RX queue, bits 5/6 =
  DCD/DSR mirror the TCP connect state), command, control. Unconnected
  reads of $C0A8 return 0 (no-data) — software polls bit 3 of the
  status to gate.
- **Slot ROM** at `$C200-$C2FF` exposes the SSC autodetect bytes
  (`$Cn05=$38`, `$Cn07=$18`, `$Cn0B=$01`, `$Cn0C=$31`) at the spec'd
  positions and routes execution around them via `JMP $Cn20` at
  entry. PR#2 hooks CSWL/CSWH ($36/$37) to `$C2B0` (output: spin on
  TDRE, store to data); IN#2 hooks KSWL/KSWH ($38/$39) to `$C2E0`
  (input: spin on RDRF, load + ORA #$80 for Apple-keyboard high-bit
  convention). Reset clears both ring buffers.
- **TCP bridge** — a worker thread listens on `127.0.0.1:port` (default
  6502, configurable via Hardware → Super Serial). One client at a
  time. Bytes flow through 4 KB ring buffers; telnet IAC negotiation
  (3-byte WILL/WONT/DO/DONT, 2-byte commands, $FF $FF literal) is
  silently swallowed by `swallowTelnetIac` so a stock `telnet` binary
  connects cleanly. `TCP_NODELAY` is on so single-character writes
  appear at the client immediately.
- **Wiring**: the card is auto-plugged at startup but the listener
  starts only when `ssc_listening=true` in settings (or via the
  Hardware panel's Start button). Both state and port are persisted
  across runs.

### ProDOS clock card (slot 4)

- **ClockCard** — ThunderClock+-compatible RTC, auto-plugged in slot 4.
  The card emulates the **bit-banged uPD1990AC** RTC chip at `$C0C0`;
  ProDOS does NOT route through the slot ROM for clock reads — at boot
  it copies its hardcoded ThunderClock driver into RAM (around `$D742`)
  and patches `$BF06-$BF08` to JMP into that copy, then the driver
  speaks to the chip via the device-select range. Our slot ROM only
  needs the detection signature.
- **Slot ROM** at `$C400-$C4FF`: signature bytes at the four even
  offsets ProDOS scans for — `$08, $28, $58, $70` at offsets `0, 2, 4,
  6`. Odd-offset fillers (CLD/CLD/SEI) plus `BVS +0` form a benign
  fall-through path; `$Cs08 = RTS` so any stray `JMP $Cs00` returns
  cleanly.
- **uPD1990AC bit-bang protocol** at `$C0n0` (slot 4 → `$C0C0`):
    write bit 0 = DATA_IN  (serial command bit input)
    write bit 1 = CLK      (rising edge → shift one bit)
    write bit 2 = STB      (rising edge → latch mode bits)
    write bits 3..5 = C0/C1/C2 (mode select)
    read  bit 7 = DATA_OUT (LSB of shift register, "live")
  Mode `0b011` = `MODE_TIME_READ`. The host arms it via `$C0C0=$18`,
  pulses STB high (`$1C`) to snapshot host time into a 48-bit shift
  register, drops STB, then reads bit 7 of `$C0C0` and pulses CLK
  (`$1A`/`$18`) 48 times to clock out 6 BCD bytes in order:
  `sec, min, hour, day, (month<<4)|dow, year`. The host driver converts
  BCD → binary and packs into ProDOS DATE/TIME at `$BF90-$BF93`.
  Mode `0b010` = `MODE_TIME_SET` lets the host *write* the clock: load
  48 bits into the shift register via DATA_IN + 48 CLK pulses (any mode
  — POM2 shifts lax, see below), then STB-in-TIME_SET commits the
  shifted-in time. `commitTimeSetFromShiftReg()` decodes the 6 BCD
  bytes via `std::mktime`, captures the delta vs `timeFn()` as
  `userOffsetSeconds`, and `effectiveTime()` composes `timeFn() +
  offset` on every subsequent read. Equivalent to MAME's 1 Hz
  `m_timer_clock` advancing the internal counter, but without a
  background thread — the advancement is derived from the host clock
  on demand. TP tick-pulse modes (64/256/2048/4096 Hz + interval
  timers, MAME `upd1990a.cpp:248-267`) are not yet hooked up: they
  need a slot-bus IRQ line that `SlotPeripheral` doesn't expose.
- **MODE_SHIFT gating** — POM2 deliberately diverges from MAME
  `upd1990a.cpp:312-327` which gates CLK-shift on `m_c == MODE_SHIFT`.
  POM2 shifts on every CLK rising edge regardless of the mode bits
  because ProDOS's hardcoded ThunderClock driver pulses CLK while
  still in MODE_TIME_READ (no re-switch to MODE_SHIFT between STB and
  the CLK serial-out). Strict gating breaks stock ProDOS reads;
  observed ThunderClock+ hardware permits the shortcut so we keep
  POM2 lax. DATA_IN is still latched into the MSB of the shift
  register on every CLK rise so MODE_TIME_SET can load bytes the
  normal way. Pinned by
  `tests/clock_card_smoke_test.cpp::testShiftLaxAcrossModes`.
- **Pinned by** `tests/clock_card_smoke_test.cpp`: detection signature,
  full bit-bang TIME_READ round-trip with a fixed timestamp (asserts
  every byte bit-exactly), shift register drains to zero after 48
  bits, STB re-latching, open-bus on non-`$C0n0` reads, MODE_TIME_SET
  round-trip (load shift via MODE_SHIFT + DATA_IN + commit + readback),
  and the lax-shift divergence from MAME.
- **Wiring**: auto-plugged when the `clock_card_enable` setting is
  true (default). DOS 3.3 disks ignore the card entirely. The headless
  build (`pom2_headless`) plugs it unconditionally for the live demo.

### Mockingboard (slot 4 by convention)

- **MockingboardCard** — Sweet Microsystems-compatible sound card, two
  6522 VIAs each driving an AY-3-8910 PSG (3 tone channels + noise +
  envelope). No ROM dependency; software detects the card by writing to
  the VIA at `$Cn00` and reading back. The 6522s are decoded in the
  *slot ROM* window — `$Cn00-$Cn0F` = VIA #1, `$Cn80-$Cn8F` = VIA #2 —
  not in the per-slot device-select range, which is why the card needed
  the new `slotRomWrite` callback (see `SlotPeripheral.h`).
- **VIA → AY wiring**:
  ```
  Port A (8 bits)   →  AY data bus  (D0..D7)
  Port B bit 0      →  AY !RESET    (active low)
  Port B bit 1      →  BDIR
  Port B bit 2      →  BC1
  ```
  Control sequence: `{BDIR, BC1}` = `00` inactive, `01` read, `10`
  write to latched register, `11` latch register address. Drivers do
  LATCH then WRITE (with INACTIVE between) to deposit a byte in any AY
  register. `$Cn00` (Port B) toggling between LATCH/WRITE/INACTIVE on
  rising edges is what `Ay3_8910::applyControl` keys off of.
- **6522 VIA subset** modelled: ports A/B with DDR masking, T1 timer
  (latch + counter, one-shot and continuous modes), IFR/IER (bits 6/7
  for T1; bit 7 computed dynamically from `ifr & ier & 0x7F`). The
  T1CL read clears `IFR.T1` exactly as on real silicon; IER `$80` set
  bit selects set-vs-clear semantics (write `$C0` enables T1, write
  `$40` disables). T2, SR, PCR, CA1/CB1 handshake — not modelled.
  Music drivers exclusively use T1.
- **AY-3-8910 synthesis** runs on the audio thread inside the inner
  `AudioSrc` (an `AudioSource` registered with `AudioDevice`). The
  CPU thread updates AY register state under the card's `mtx`; the
  audio callback grabs a 32-byte snapshot of both register banks
  under that mutex, releases, then synthesises samples lock-free.
  Tone counters use float phase accumulators stepping at
  `clockHz/16/sampleRate`; the 17-bit noise LFSR uses the canonical
  `x^17 + x^14 + 1` taps; envelope walks 32 steps with the WDC R13
  shape register controlling continue/attack/alternate/hold. Both AY
  chips are summed to mono (Mockingboard is stereo on real hardware,
  but `AudioDevice` is mono-only — minor loss for 3-voice arpeggios).
- **IRQ aggregation**: each VIA's `irqOut()` (= `(ifr & ier & 0x7F)
  != 0`) is OR'd onto the slot IRQ, forwarded to `M6502::setIRQ()`.
  The card caches the combined state so transitions only call the
  CPU asserter once. Pinned by both the continuous and one-shot T1
  IRQ tests in `mockingboard_smoke_test.cpp`.
- **Wiring**: not auto-plugged. The user picks "Mockingboard A/C" in
  the Slot Configuration UI for any free slot (4 by convention; some
  software hard-codes that). Volume + mute persist via
  `mockingboard_volume` / `mockingboard_muted` settings keys. Tear-down
  on slot-config restart removes the audio source from `AudioDevice`
  before destroying the card — the `AudioSource` lives inside the
  card, so order matters.
- **Pinned by** `tests/mockingboard_smoke_test.cpp`: $Cn0X / $Cn8X
  address decode, VIA → AY register addressing via the LATCH→WRITE
  sequence, T1 IRQ continuous + one-shot, T1CL read clearing IFR.T1,
  AY tone synthesis producing non-silent output for a non-zero period.

### ProDOS host folder (`prodos_disk/`)

- **`ProDOSVolume`** synthesises a read-only ProDOS volume image (block
  array) from the contents of a host folder. Layout: blocks 0-1 boot
  (zeroed — volume is not directly bootable), 2-5 volume directory key
  + 3 extension blocks (51 entries max), block 6 volume bitmap (1 block
  = 4096 blocks coverage = 2 MB cap), 7+ file data + sapling indexes.
- **Scope**: flat directory only; ≤ 51 files; ≤ 128 KB per file
  (seedling + sapling, tree files skipped with a warning); file type
  guessed from extension (`.bas/.bin/.sys/.txt/.int`, default BIN);
  filenames sanitised to A-Z/0-9/. with collision suffixes `.1/.2`.
- **Wiring**: the HDV slot 5 panel's Library shows a synthetic
  `[host folder] prodos_disk/` entry. Click it → `buildVolumeFromFolder`
  produces bytes → `ProDOSHardDiskCard::loadImageFromBytes` swaps them
  into the card. **No auto-boot** for the synth: the user must boot
  ProDOS from a Disk II disk (slot 6) or another HDV image first; once
  ProDOS is up, `/HOST/` appears as a slot 5 drive (`CAT,S5,D1`).
- **Read-only**: the card's driver returns `$2B` (write-protected) on
  any write — host files are never modified by the guest. To refresh
  after editing a host file, click the entry again.
- Pinned by `tests/prodos_volume_smoke_test.cpp`.

### Snapshot

- **SnapshotIO** — `POM2SNAP` magic, named 8-byte sections, format
  shared with POM1 (round-trip test in `tests/snapshot_io_smoke`).
  Captures CPU + RAM + soft-switch display state. Disk II state is
  **deliberately excluded** — head position + mounted-image identity
  are kept out of v1.
- **CliDispatcher** — three-phase startup: A (parse), B (apply pre-boot:
  preset, ROM, snapshot-load, --load addr:file), C (post-boot: tape ops,
  paste, run/step). Flags: `--preset ii|ii+`, `--speed`, `--cpu-max`,
  `--tape`, `--load addr:file`, `--run`, `--paste`, `--step`,
  `--play/--rec/--rewind`, `--snapshot-save/load`.

### UI (ImGui)

- **MainWindow** — one main menu bar (File / Edit / Run / Presets /
  Display / Hardware / Debug / Help) plus the Apple II Screen, Emulation
  panel, and on-demand panels for cassette deck, Disk II, joystick, and
  the memory tools below. Owns the GL texture for the screen.
- **MainWindow_MemoryMaps.cpp** — three visual layouts of the 64 KB
  memory map (Memory Map Bar / Bar Horizontal / Grid), toggled from
  Debug menu. Region colours match the memory viewer.
- **MemoryViewer_ImGui** — hex grid + ASCII column over the full 64 KB
  flat array. Region-coloured for the Apple II memory map (zero page,
  stack, text/HGR pages, I/O, slot ROMs, Applesoft, Monitor). Reads via
  `Memory::data()` directly under `stateMutex` (held by MainWindow during
  `render()`) so the viewer never triggers a soft-switch side effect.
  Edits go through `writeCallback` → `Memory::memWrite` so ROM-write
  protection still applies. Per-byte change-flash uses a frame-counter
  delta against `prevMemory`. Search supports hex byte sequences
  ("A9 FF 48") and ASCII strings (matches both raw bytes and their
  high-bit-set form so on-screen text is findable).
- **Disassembler6502** — stateless 6502 instruction decoder.
  `(mem*, pc) → mnemonic + length`. Used by the memory viewer's "Disasm"
  toggle to flip the row body from hex to one-instruction-per-line.
- **main.cpp** — GLFW + ImGui boilerplate. Forwards GLFW char/key callbacks
  to MainWindow only when ImGui isn't capturing keyboard (so editing a
  control widget doesn't leak keystrokes into the Apple II). F9
  (screenshot), F11 (soft reset / Ctrl-Reset) and F12 (hard reset /
  power cycle) are routed unconditionally so they remain reachable
  even when ImGui has focus.
- **Screenshot (F9)** — `MainWindow::saveScreenshot` snapshots the live
  framebuffer (under `stateMutex`) and writes `screenshot_NNN.ppm`
  in the working directory. The sequence number auto-advances so
  successive presses don't clobber. P6 binary RGB; preview.app on
  macOS opens it directly.

## Memory Map

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
$C000        Keyboard latch (low 7 bits = key, high bit = strobe / ready)
$C010        Clear keyboard strobe (read or write)
$C030-$C03F  Speaker toggle (flip-flop on any access)
$C000-$C00F  IIe paging soft switches (80STORE / RAMRD / RAMWRT /
             INTCXROM / ALTZP / SLOTC3ROM / 80COL / ALTCHAR — ignored on II+)
$C013-$C018  IIe paging status reads (RDRAMRD/WRT, RDCXROM, RDALTZP,
             RDC3ROM, RD80STORE — bit 7 = on)
$C01E/$C01F  IIe RDALTCHAR / RD80COL
$C050/$C051  Set graphics / set text
$C052/$C053  Clear / set mixed
$C054/$C055  Page 1 / page 2
$C056/$C057  Set lo-res / set hi-res
$C061-$C063  Push-buttons (negative when pressed)
$C064-$C067  Paddle inputs (negative while RC discharging)
$C070        Paddle reset latch
$C05E/$C05F  IIe DHGR enable / disable (DHIRESON / DHIRESOFF — also AN3
             annunciator pulses on every access for Le Chat Mauve's FIFO)
$C0A8-$C0AB  Super Serial Card ACIA (slot 2 — data/status/cmd/ctrl)
$C0C0        ThunderClock+ uPD1990AC bit-bang register (slot 4 —
             write: DATA_IN/CLK/STB/C0..C2; read bit 7: DATA_OUT)
$C0E0-$C0EF  Disk II controller soft switches (slot 6) — also the LSS
             ($C0EC = Q6L data register, $C0ED = Q6H load latch)
$C100-$C5FF  Slot ROMs (or IIe internal I/O ROM when INTCXROM=on)
$C300-$C3FF  IIe 80-col firmware (internal when SLOTC3ROM=off)
$C400-$C4FF  ProDOS clock card slot ROM (signature + clock-read body)
$C600-$C6FF  Disk II boot PROM (P5A, when roms/disk2.rom present)
$C700-$C7FF  Slot ROMs (currently empty)
$D000-$F7FF  Applesoft BASIC ROM
$F800-$FFFF  Monitor ROM + 6502 vectors ($FFFA-$FFFF)
```

In IIe mode the same map applies, but most of $0000-$BFFF can be routed
to the auxiliary 64 KB bank under the IIe paging switches. See the
table at the top of `Memory.h` for the per-range routing rules.

## Key implementation details

### CPU execution

Three modes via EmulationController: **Stopped** (idle wait), **Running**
(`cyclesPerFrame` worth of CPU per 60 Hz tick), **Step** (one instruction
per click then back to Stopped). `M6502::run(maxCycles)` returns the
*actual* cycle count — the worker passes that to `Memory::advanceCycles()`
so paddle RC discharge stays synced to the emulated clock.

### Soft switches

Apple II soft switches accept *both* reads and writes for state changes.
A reader interested in the keyboard byte at $C000 also clobbers paddle
state if they hit $C064-$C067, etc. POM2 dispatches every $C000-$C07F
access through one function so the side effects line up with hardware.

### Keyboard

Memory holds the latched key + strobe under `kbMutex`. UI thread enqueues
via `queueKey()`; the strobe is set high. CPU thread reads $C000 via
`softSwitchAccess()`, which snapshots the latch under the same mutex.
Strobe stays high until $C010 read/write.

### Speaker

Toggle-on-access counter exposed via `getSpeakerToggleCount()` for the
Emulation panel. The audio path itself lives in `SpeakerDevice` (see
*Audio* above): every `$C030-$C03F` access pushes a sub-instruction
timestamp into a ring buffer; the audio callback drains it into a square
wave through a 5 kHz LP + DC blocker. The toggle counter is debug-only —
mute it in code and audio still plays.

## Version string locations

Bump version in:

- `main.cpp` (window title + console banner)
- `MainWindow.cpp` (About dialog)
- `README.md` (status section)
- `CMakeLists.txt` (project version, when one is added)
