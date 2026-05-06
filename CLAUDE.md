# CLAUDE.md

Architecture / invariants / gotchas for the **emulator side** of POM2. User
walkthrough → `README.md`.

## Project Overview

Apple II / II+ emulator (Dear ImGui, MOS 6502 + 64 KB RAM via Language Card + soft switches +
text/lo-res/hi-res framebuffer + 1-bit speaker + cassette + joystick +
Disk II in slot 6). One concern per file: each `.cpp/.h` pair owns one
subsystem.

## Build & Run

```bash
./setup_imgui.sh             # one-time deps + clones imgui/
cd build && cmake .. && make # build → build/POM2
./run_emulator.sh            # runs from repo root so roms/ probes resolve
```

ROMs are user-provided. Place the Autostart + Applesoft image at
`roms/apple2.rom` (12 KB or 16 KB).

## Architecture

### Core

- **M6502** — MOS 6502 with full instruction set, IRQ / NMI / BRK, decimal
  mode, Klaus Dormann functional-test clean. `setProgramCounter()` is the
  back-door used by future Klaus harness ports.
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
- **Apple2Display** — pure software renderer into a 280×192 RGBA buffer.
  Reads soft-switch state via `Memory::getDisplayState()` (cheap mutex copy)
  and the flat RAM array directly. **Owns no GL state** — UI uploads via
  `glTexSubImage2D`. Built-in 5×7 ASCII font fallback when the user hasn't
  provided a character ROM. Lo-res palette is the //gs-corrected approximation.
  Hi-res has four `HiResMode` variants: `ColorNTSC` (default — 14 KB LUT
  indexed by `(parity << 8) | byte`, 39 inter-byte seam fix-ups, optional
  additive horizontal glow) and three monochrome phosphors —
  `MonoWhite` / `MonoGreen` (P31) / `MonoAmber`. Text inverse attribute
  renders statically (2 Hz flashing animation pending).

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

- **DiskImage** — loads a 143 360-byte `.dsk` / `.do` (DOS 3.3 logical
  sector order) and pre-nibblizes it into 35 × 6656-byte track buffers.
  GCR encoding follows "Beneath Apple DOS": 14-byte sync gap, address
  field (`D5 AA 96 [vol/trk/sec/chk in 4-and-4] DE AA EB`), 5-byte sync
  gap, data field (`D5 AA AD [86 low-2-bit nibbles REVERSED + 256 high-6
  nibbles + 1 XOR checksum] DE AA EB`). DOS 3.3 sector skewing is
  applied: physical-to-logical map `{0,7,14,6,13,5,12,4,11,3,10,2,9,1,8,15}`.
  Read-only — `.dsk` file is untouched.
- **DiskIICard** — SlotPeripheral plugged in slot 6. Holds the 256-byte
  P5A boot PROM (`roms/disk2.rom` from AppleWin) at `slotRomRead`; the
  PROM autodetects its slot via the `JSR $FF58 / TSX / LDA $0100,X`
  trick, so the Apple II main ROM must be loaded for boot to work.
  Soft switches at `$C0E0-$C0EF`: phases 0-3 off/on, motor off/on, drive
  1/2 select (only drive 1 modelled), Q6L/Q6H (shift/load), Q7L/Q7H
  (read/write — write is acknowledged but ignored). Head position in
  half-tracks; quarter-tracks unused. Nibble cursor advances every
  ~32 CPU cycles in `advanceCycles()`. `$C0EC` in read mode returns
  `image.nibbleAt(track, trackPos)`; bit-7 is implicitly always set
  because every valid GCR nibble has it set, so DOS 3.3's
  `LDA $C0EC ; BPL loop` exits immediately.
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
  control widget doesn't leak keystrokes into the Apple II). F2 always
  reaches MainWindow regardless (hard reset is global).

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
$C050/$C051  Set graphics / set text
$C052/$C053  Clear / set mixed
$C054/$C055  Page 1 / page 2
$C056/$C057  Set lo-res / set hi-res
$C061-$C063  Push-buttons (negative when pressed)
$C064-$C067  Paddle inputs (negative while RC discharging)
$C070        Paddle reset latch
$C0E0-$C0EF  Disk II controller soft switches (slot 6)
$C100-$C5FF  Slot ROMs (currently empty)
$C600-$C6FF  Disk II boot PROM (P5A, when roms/disk2.rom present)
$C700-$C7FF  Slot ROMs (currently empty)
$D000-$F7FF  Applesoft BASIC ROM
$F800-$FFFF  Monitor ROM + 6502 vectors ($FFFA-$FFFF)
```

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
