# POM2 — TODO

Status as of 2026-08-19. Resolved items → `CHANGELOG.md`. MAME refs → `DEV.md`.

**Format**: `🟠 high · 🟡 medium · 🟢 low` at the head of each item. Indicative
effort in *italics*. File/line in `backticks`. Quick read:
[Open and known to be open](#open-and-known-to-be-open--2026-08-22-bug-hunt)
first, then [Architect attack order](#architect-attack-order) (sequencing),
then [Quick wins](#quick-wins), then [Backlog by subsystem](#backlog).

## Open, and known to be open — 2026-08-22 bug hunt

Findings that are **deliberately not fixed**. They sit at the top because each
is something POM2 currently gets wrong that somebody would otherwise rediscover
the hard way — and because the reason for leaving them is part of the finding.

The 2026-08-22 architecture audit closed the exception-barrier gap, the file
size ratchet, the CI platform gap, the test-timing gap and most of the
`stateMutex` family; what it left is recorded below and in `CHANGELOG.md`.

- 🟠 **Blocking work under `stateMutex` — what is LEFT of the family.** The
  state mutex is taken by the CPU worker for each 4096-cycle chunk AND by the
  UI thread to paint every frame, so anything slow holding it freezes the
  machine *and* the window, buttons included. The 2026-08-22 audit found the
  family was ~20 sites rather than the four first recorded, and fixed the
  structural cause for most of them (`MediaMount.h`: read + decode unlocked,
  swap the finished object in under the lock). **Done**: every Disk II 5.25"
  mount (17 sites), `EmulationController::mount35`, and the AI server's
  `/snapshot/save` + `/snapshot/load` (which now serialise into RAM under the
  lock and commit through `pom2::writeFileAtomic` outside it). **Left**:
  - ✅ **HDV / block-device mount — DONE 2026-08-22.** Was the worst case in
    the tree (32 MiB under the lock). `Block512Backing::loadImage` split into a
    static `readImageFile` (unlocked) and `adoptImage` (locked, no syscalls);
    the pair is on `ProDOSBlockCard` and `SmartPortUnit`, forwarded in one line
    each by `CffaCard`, `ProDOSHardDiskCard` and `SmartPortHdvUnit`. All seven
    UI sites go through `pom2::mountBlockCard` / `mountSmartPortUnit`.
    Measured on a 32 MiB image: **25.8 ms under the lock → 0.0 ms**, because a
    raw `.hdv` now moves phase 1's buffer instead of copying it. The inline
    path halved as a side effect (25.8 → 13.4 ms), which the CLI and the
    profile-switch remount get for free. Pinned by `two_phase_block_mount`,
    whose case 2 guards the `loadImageFromBytes` trap and was verified to fail
    when the trap is walked into. → [DEV](DEV.md#the-block-device-half-hdv--2img-converted-2026-08-22)
  - ✅ **`SpOverSlipLink::transact` — DONE 2026-08-23.** The wait itself was
    never the bug; a bounded stall repeated without bound is not bounded. A
    write failure already declared the peer lost, but a SILENCE did not — so a
    helper that accepted writes and never answered cost the full `timeoutMs()`
    (250 ms default) on *every* call, forever, and a ProDOS boot became a
    string of quarter-second freezes with the panel's own Stop button
    unreachable. Three consecutive timeouts now drop the link
    (`kMaxConsecutiveTimeouts`), which closes the socket, so every later call
    returns at the `isOpen()` gate for nothing. Three, not one: a single
    timeout is an ordinary hiccup on a busy helper. Total cost of a dead peer
    goes from unbounded to ~0.75 s. Pinned by
    `sp_over_slip_link` / `testSilentPeerIsDroppedRatherThanPaidForEveryCall`,
    verified falsifiable.
  - 🟢 **The thread `join()`s — examined 2026-08-23, deliberately left.** Both
    turn out to be defensible, and the analysis is recorded so nobody
    re-derives it:
    * `slotBus().clear()` on a profile switch is not a machine freeze at all —
      `applyProfile` has already stopped the CPU worker, so only the UI blocks,
      during a modal operation that is a full cold reset anyway. Same class as
      the profile-switch remount above.
    * The FujiNet **Stop / Drop-peer** buttons genuinely need that lock. It is
      not there for tidiness: `FujiNetCard` reaches `transact()` from the CPU
      thread *under* `stateMutex`, and `stop()` ends in `transport_.reset()` —
      so dropping the lock trades a 200 ms wait for a use-after-free on a live
      `SpTransport*`. The clean fix is to move that mutual exclusion onto the
      link's own `callMtx_` (lock order `callMtx_` → `stateMtx_` is already
      what `transact` uses, so there is no inversion), letting the caller stop
      taking `stateMutex`. It is worth doing, but it is a lock-order change in
      a three-thread subsystem to save 200 ms on a button the user pressed on
      purpose — a one-off, expected pause, not the repeated freeze the item
      above was. Not a good trade to make in a hurry.
  - 🟢 Deliberate, documented, and staying: the profile-switch remount in
    `MainWindow_Slots.cpp` (the SlotBus rebuild and the remounts must be one
    atomic step against the AI server, and the CPU worker is stopped anyway),
    and the outgoing medium's write-back inside `installDisk` (swapping before
    knowing the old medium could be written loses the user's changes).
  Deliberate and bounded, so listed for completeness rather than as a defect:
  the Uthernet II guest DNS wait (`kDnsWaitMs` = 120 ms, `W5100Device.h`).

- ✅ **Rewind: resuming from anywhere but "resume here" left the ring
  non-monotonic — found 2026-08-22, DONE 2026-08-23.** `indexForCycle`
  early-breaks on the first frame past its target, which is only a correct
  search while stamps increase — assumed, never enforced, and the machine broke
  it routinely: only `rewindEndAndResume` truncated the abandoned future and
  cleared `Rewind_ImGui::scrubbing_`, while the toolbar Play button, Machine ▸
  Run, the `machine.run` palette command and the kiosk menu all called
  `setMode(Running)` directly. Fixed at the two layers that own the two halves
  rather than at the four call sites: `RewindBuffer::capture` now drops every
  frame stamped at-or-after the incoming one (one compare on the hot path; a
  jump back past the oldest frame clears the ring so the restart is a keyframe,
  not a delta against a dead timeline), and the **controller owns the scrub**
  (`scrubIndex_` / `rewindScrubbing()`), ended by `setMode(m != Stopped)`
  whoever asks, with the panel's flag reduced to a view of it. The truncation
  is deliberately *not* in `setMode`: it is reached from callers already
  holding `stateMutex` (the Disk II Library boot buttons), which is
  non-recursive — deferring the drop to the worker's next capture keeps every
  resume path lock-free. Pinned by `rewind_roundtrip` case 5 +
  `rewind_transport` case 6, each half verified falsifiable on its own.
  → [DEV](DEV.md#rewind--time-travel)

- ✅ **Watchpoints: the WRITE half shipped free — 2026-08-22 finding, DONE
  2026-08-23; the READ half the same day, by a coarser shape that measured
  free too.** The naive shape (wrap
  `memRead`/`memWrite`, test one pointer, call a sink) measured **+13.4 % /
  +16.5 % / +9.2 %** and was thrown away; forcing it inline made it worse,
  which located the cost in the extra branch and the code growth around the
  emulator's hottest function rather than in an inlining accident.
  What shipped adds **nothing to the fast path**, because it does not test for
  a watchpoint there — it removes the address *from* the fast path: arming a
  write watch clears the address's `writable[]` byte, so `memWrite`'s existing
  test fails and the write falls into `memWriteSlow`, which reports the access
  and performs the write from a shadowed copy of the real permission
  (`Memory::setWriteWatch`). Interleaved best-of-9 on three `pom2_bench`
  workloads, RAM hashes identical: −2.1 % / −0.7 % / −0.1 % — no measurable
  cost. $C000 and up needs no diversion at all (those writes already reach the
  slow path), so soft switches, slot I/O and the language card are watchable
  for free. Three traps, all pinned by `debugger` cases 7-9: the write must
  still LAND, the diversion must never invent write permission (incl.
  `markRomRegion` running while a watch is armed), and `restoreMainRam` must
  consult the real permission or a rewind silently refuses to restore that one
  byte. Of the two caveats this item used to list, **one did not exist**:
  Language-Card paging does not rewrite `writable[]` at all — `markRomRegion`
  is its only mutator and the LC has its own path — so the shadow only had to
  survive that one function. **Read watchpoints** have no per-address table
  to hide in, so they use ONE flag that diverts every read to `memReadSlow`
  while any read watch is armed. Testing that flag on the fast path measured
  **+7.2 % / +4.2 %** and was rejected; folding it into three tests the path
  already made (`plainRead_` / `iieFastRead_` / `romFastRead_`) measured
  **+0.0 % / −3.0 % / −0.5 %** un-armed. Armed, every read goes out of line
  (+11 % to +55 %), paid only by the session that armed one. A second trap
  on the way — the wrapped slow body left out of line, +1.0 % on the ][+
  banner — was caught by the same measurement and force-inlined. The panel
  offers R / W / RW, default W. Pinned by `debugger` cases 10-11.
  Numbers: [PERFORMANCE § 8.3 + 8.5](docs/PERFORMANCE.md)
  → [DEV](DEV.md#debugger-debuggerhcpp-debugger_imgui)

- ✅ **`disk_path_snapshot` SIGBUS on arm64 macOS — found 2026-08-22, DONE
  2026-08-23.** Neither half of the test was at fault: `lldb` put the fault in
  `___chkstk_darwin` under `DiskImage::loadFile` on the writer thread — a
  **stack overflow**. `sizeof(DiskImage)` is 247 480 B (in-object track
  buffers) and the insert chain stacked three temporaries (`insertDisk` →
  `prepareDisk` → `loadFile`, ~725 KB), which fits a Linux thread's 8 MB and
  not a macOS `std::thread`'s 512 KB. The AI control server's HTTP thread
  takes the same path, so this was a real app crash on macOS, not a test
  artefact. The six temporaries are heap-allocated now; the object's layout
  (and the LSS hot path) is untouched. Pinned by `diskii_insert_thread_stack`
  on an explicit 512 KB pthread stack so it fails on Linux too; verified
  falsifiable (exit 138 without the fix). macOS CI runs `ctest` again.
  → `CHANGELOG.md` 2026-08-23.

- ✅ **`w5100_udp_recv` flake — found 2026-08-22, DONE 2026-08-23. A test
  artefact, and the diagnosis is the interesting part.** Not a lost datagram:
  a torn read of a 16-bit register. `pollForData` read `SN_RX_RSR` as two
  byte accesses, and on this chip reading `SN_RX_RSR0` is what *pulls* the
  datagram off the host socket (it is polled — no RX interrupt). So a datagram
  arriving between the hi and lo reads gave the old hi with the new lo — a
  1408-byte datagram read back as 128 (`0x0080` vs `0x0580`), ~1 run in 40.
  Instrumented to catch the actual staged value, which is what named the tear.
  Fixed in the test, not the model: a real W5100 driver reads RSR until two
  consecutive reads agree (datasheet §5.2.2), and `pollForData` now does too —
  the model is faithful, the test was reading a moving 16-bit register
  non-atomically. 0/250 locally after the fix, all three previously-flaky
  cases. → `CHANGELOG.md` 2026-08-23.

## MAME ↔ POM2 parity (dashboard)

Canonical reference for what is ported and **how faithfully** — the `Parity`
column grades the port against its reference (verbatim → partial-verbatim →
POM2-original → scaffold). The `Known gaps` listed here point to detailed items
in the [backlog](#backlog).

The independent axis — **where the emulation boundary is cut**, at the chip's
pins (LLE) or at the service it provides (HLE) — lives in
[`docs/lle_vs_hle.md`](docs/lle_vs_hle.md). The two do not correlate: a verbatim
port can be high-level (`ImageWriter`) and a POM2-original can be low-level
(`CassetteDevice`).

| #  | Subsystem                  | Parity           | MAME / AppleWin refs                                                     | Known gaps                                                                              |
| --- | ---------------------------- | ---------------- | ------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------- |
| 1  | M6502 / 65C02 / Rockwell / WDC | Verbatim         | `om6502.lst`, `ow65c02.lst`; Tom Harte `65x02`                          | 🟢 NMOS 100% Tom Harte on all 178 documented opcodes; 🟢 WDC decimal SBC now silicon-exact (interdigit carry, 2026-07-30); 🟢 $5C 8-cyc = deliberate (matches MAME, not Harte) |
| 1bis | Z80 core (standalone)          | POM2-original (L0) | z80.info/decoding.htm field decomposition (same structure MAME's `z80.cpp` flattens) | — (zexdoc + zexall clean; MEMPTR + X/Y flags modelled, not approximated; pinned `z80_core`, `z80_zexdoc`, `z80_zexall`, `z80_block_io_flags`) |
| 1ter | SoftCard Z80 (key `softcard`)  | Verbatim         | MAME `bus/a2bus/a2softcard.cpp` (R. Belmont, 176 lines; line-cited)      | — (real DMA bus arbitration, 6502 halted per instruction slice; CP/M 2.2 boots MAME-oracle-identical; pinned `softcard_toggle`, `softcard_cpm_boot`, `softcard_cpm_boot_iie`) |
| 2  | Memory + IIe + RamWorks        | Partial-verbatim | `apple2e.cpp:1275-1299`, `a2eramworks3.cpp:108-115`                      | 🟠 god-object (Keyboard/PaddleInputs to extract)                                         |
| 3  | Display HGR/DHGR/80-col        | Partial-verbatim | `apple2video.cpp:124-201`, `460-471`, `:751-758`; AppleWin `RGBMonitor.cpp` | 🟢 mono DHGR 1-px (mid-scanline, PAL 50 Hz, floating bus `$C05x`, page-flip DROL, Chat Mauve RGB: done) |
| 3bis | Le Chat Mauve RGB (key `chatmauve`) | POM2 + AppleWin | AppleWin `RGBMonitor.cpp` pixel rules + the Eve/Chat Mauve brevet (MAME has no model) | 🟢 AN3 pulse FIFO decode, Eve Color text `$C0B8/9` + HGR Duochrome `$C0BA/B`, snapshot v2; pinned `le_chat_mauve_smoke` |
| 4  | SpeakerDevice                  | Verbatim         | `spkrdev.cpp:74-327`                                                     | —                                                                                        |
| 5  | CassetteDevice                 | POM2-original    | `apple2.cpp:362`                                                         | —                                                                                        |
| 6  | Mockingboard A/C (6522 + AY)   | Partial-verbatim | `ay8910.cpp:998-1015`, `:1077-1104`, `1309`; `6522via.cpp:959`          | 🟢 Port A read mask by DDR; 6522 subset (SR/PCR; T2 one-shot done, IRQ N+3 MAME)      |
| 6b | Mockingboard "C" Sound II      | POM2 + AppleWin  | AppleWin `source/Mockingboard.cpp` + `source/SSI263.{h,cpp}`             | — (SSI263 at `$Cs40-$Cs44`, A/!R → VIA1.CA1)                                              |
| 7  | FloppySoundDevice              | Verbatim         | `floppy.cpp:1532-1620`, `:2925-3020`                                     | —                                                                                        |
| 8  | SlotBus + IRQ wire-OR          | POM2-original    | MAME slot bus pattern                                                    | —                                                                                        |
| 9  | DiskImage                      | Partial-verbatim | `woz_dsk.cpp`, `flopimg.cpp:2017-2106`                                   | 🟡 WOZ1 splice TRK+6650; 🟢 .nib2/.app, half-tracked NIB (88)                           |
| 10 | DiskIICard                     | Partial-verbatim | `machine/wozfdc.cpp:264-291`, P6 PROM 341-0028-A                         | 🟢 sub-instruction RAII vs per-cycle                    |
| 11 | IWMDevice                      | Verbatim         | `machine/iwm.cpp:1-543`                                                  | 🟢 Q3 fast clock (Mac/IIgs only); window-size rounding                                  |
| 12 | SmartPortCard (//e Liron)      | POM2-original (real EPROM → L2) | SmartPort spec + Apple Tech Note; real Liron firmware `roms/liron.rom` (BMOW 4 KB) + real `$Cn0D` dispatch, pinned `liron_smartport_dispatch` | 🟢 multi-partition ProDOS (CFFA3000)                                                     |
| 13 | SmartPortHub + Sony35Drive     | Verbatim         | `apple2e.cpp:638-679`, `mac_floppy.cpp`, `flopimg.cpp:512/967/2017-2106` | —                                                                                        |
| 14 | CFFA (MAME-faithful IDE)       | Verbatim         | `bus/a2bus/a2cffa.cpp`                                                   | 🟢 CHD = phase 2; no media preservation on profile switch                         |
| 14bis | ProDOSHardDiskCard (key `hdv`) | POM2-original (H1) | No MAME/AppleWin analogue — hand-assembled 256 B slot ROM + an invented 4-register streaming port | 🟢 deliberate: mounts `.hdv`/`.2mg` with **no card ROM dump required**; no GCR/flux/ATA below it; `$Cn07 = $01` so the F8 autostart never scans it (use `PR#n` / `bootFromSlot`); pinned `hdv_card_smoke`, `hdv_writeback_smoke`, `hdv_mass_storage_smoke` |
| 15 | ClockCard / ThunderClock+      | Partial-verbatim | `upd1990a.cpp:248-267`, `:312-327`; Thunderware Rev 1.3 EPROM (`roms/thunderclock_u9_v1.3.bin`) | 🟡 MODE_SHIFT lax; 🟡 DATA_OUT live vs MAME latch; 🟢 real EPROM loads from the ctor (synth ROM = fallback, untested from `$C800`) |
| 15bis | NoSlotClock (DS1216E, no slot used) | Verbatim | MAME `ds1216.cpp`; protocol verified against AppleWin `NoSlotClock.cpp` (Nick Westgate csa2 + Dallas datasheet) | 🟢 full 64-bit pattern-match state machine on reads **and** writes (key bit rides on the address); window follows the machine — `$F800-$FFFF` on II/II+, `$C300`/`$C800` on //e + //c-class; injectable time source; pinned `no_slot_clock_smoke` |
| 16 | SuperSerialCard                | Partial-verbatim | `mos6551.cpp:46`, `:542-543`, `a2ssc.cpp:373`                            | 🟢 IRQ gate SW2:6 DIP not gated                                                          |
| 17 | MouseCard (MAME)               | Verbatim         | `bus/a2bus/mouse.cpp`, M68705 + MC6821                                   | 🟢 PIA out_a/b without `scheduler.synchronize`                                          |
| 18 | MouseCard (AppleWin HLE)       | Verbatim         | AppleWin `source/MouseInterface.cpp`                                     | — (slot EPROM only, MCU synthesized)                                                      |
| 19 | Phasor (AE — 2×VIA, 4×AY)      | Partial-verbatim | MAME `a2bus/phasor.cpp` + AppleWin                                       | 🟢 EchoPlus mode (=7) routed as native Phasor; stereo L/R per VIA pair done (2026-08-01). 🟡 **no cycle-stamped event queue** — the AY writes are applied when the audio callback runs, not at their `emuCycles` stamp the way Mockingboard's are, so beam-raced register changes quantise to the buffer. Bus decode is verbatim; the audio timeline is not, hence Partial not Verbatim. |
| 20 | SSI263 speech (chip model)     | AppleWin-faithful| AppleWin `source/SSI263.{h,cpp}` (MAME does not implement)                 | 🟢 formant synth → PCM blob, 62 phonemes (AppleWin LGPL → GPL3)                           |
| 21 | EchoPlusCard (Cricket/SSI263, key `echoplus`) | POM2-original | Cricket / Street Elec SSI263 spec (historically mislabelled "Echo+") | 🟢 markadev audit 2026-05-28: the real Echo+ = TMS5220 (see line 21bis)                |
| 21bis | EchoPlusTMS5220Card (key `echoplus_tms`) | Scaffold       | markadev/AppleII-RevEng/Street-Electronics-Corp-ECHO+                  | 🟡 stub register decode (kept for software detection); TMS5220 LPC + AY-3-8913 synth cores deferred. Catalog label says "silent, detect-only" so it does not pose as a working card (2026-08-23). |
| 22 | PrinterCard (parallel synth)  | POM2-original    | Apple II slot 1 convention + Pascal 1.1 sig                              | — (PDF export shipped: `src/ImageWriterPdf.*`, pinned `imagewriter_pdf`)                 |
| 22bis | GrapplerCard (key `grappler`) | Verbatim         | MAME `bus/a2bus/grappler.cpp` (pinned 2026-07-28, line-cited) + markadev 4 KB EPROM (`roms/grappler_plus.bin`) | 🟢 /STROBE 7-clock pulse collapsed to instant (no observer); `ackEffective()` BUSY gate is POM2's back-pressure model |
| 22ter | ImageWriter II printer (host-side, no slot) | Verbatim         | greg-kennedy/ImageWriter (GSport/KEGS/DOSBox lineage) + Apple ImageWriter II/LQ reference manuals | — (full control language, 4-band colour ribbon, 8-/24-pin bit images, paper tray + PNG & multi-page PDF export; fed by `printer` / `grappler` / SSC printer tap (//c PR#1)) |
| 23  | UthernetCard + Cs8900aDevice (key `uthernet`) | Verbatim | MAME `machine/cs8900a.cpp` (VICE lineage) + `bus/a2bus/uthernet.cpp`, line-cited | 🟢 pull-mode RX (POM2 has no `device_network_interface` push bus); inbound frame queue out of snapshot deliberate |
| 23bis | UthernetIICard + W5100Device (key `uthernet2`) | AppleWin-faithful | AppleWin `source/Uthernet2.cpp` + `W5100.h` (MAME has no W5100 device) + WIZnet datasheet v1.2.8 | 🟡 `LISTEN` unimplemented (no inbound path); 🟢 virtual DNS is async, not blocking like AppleWin's |
| 23ter | NetworkBackend (Null / Loopback / libslirp) | POM2-original | AppleWin `Tfe/NetworkBackend.h` shape; libslirp user-mode NAT | 🟢 outbound-only by design (no root); no TAP/pcap path; 🟡 libslirp is Linux/macOS only, so Uthernet I has no transport on Windows |
| 24 | FujiNetCard (key `fujinet`)    | POM2-original (relay) | No MAME device — published SmartPort/SP-over-SLIP spec + the FujiNet AppleWin fork | 🟢 not an emulation: the device is real and off-box, every SmartPort call is forwarded verbatim; no peer → bounded 250 ms stall then SmartPort `$27`; 🟡 **rewind cannot rewind it**; 🟡 not on //c-class (forced INTCXROM masks slot ROM); pinned `fujinet_card` |

## Quick wins

High impact/effort ratio. For **what to do next among architecture items**,
see [Architect attack order](#architect-attack-order) (P0 freeze of
`MainWindow.cpp` outranks every row here).

| # | Item                                    | Effort  | Why                                |
| - | --------------------------------------- | ------- | --------------------------------------- |
| 1 | WASM IDBFS settings persistence         | 2-4 h   | web user has no state        |
| 2 | WOZ1 splice point TRK+6650              | 1 d     | Applesauce re-master parity             |
| 3 | Memory god-object split                 | 2 d     | cuts recompiles; architect **P2** (after an I/O test net; not merged with the `memRead` dispatch) |
| 4 | ~~Debugger runtime glue (BP / watch / step)~~ ✅ DONE 2026-08-22, watchpoints 2026-08-23 | — | breakpoints, step, step-over, run-to-cursor + **write** watchpoints (free — the address is diverted off `memWrite`'s fast path) + **read** watchpoints (free un-armed — the divert flag is folded into tests the fast path already made; see [Open](#open-and-known-to-be-open--2026-08-22-bug-hunt)) |
| 4b | ~~Digidream 1 tempo regression~~ ✅ DONE | — | cause measured (`caughtUp` paced against the last write, not CPU-now) and fixed 2026-08-01 (see [Audio]) |
| 5 | ~~CI GitHub Actions (`ctest` headless)~~ ✅ DONE | — | the dormant ctest suite (182 tests) now gated (see [Arch]) |
| 6 | ~~Desktop drag-drop disk (`glfwSetDropCallback`)~~ ✅ DONE | — | README promise kept (see [UI/UX]) |

## Architect attack order

Sequencing constraint from a 2026-08-19 architecture pass — **not a second
backlog**. Feature items stay under [Backlog](#backlog); this list says what
to do **next when choosing among them**, and what not to pick instead.
Cross-links point at the detailed items. Two of the original P1s had already
landed; they stay in the order so the ranking is auditable.

Standing rule while P0 is open: **do not grow the god-objects.** A new card
gets its panel in its own `*_ImGui.cpp` and **zero** business logic in
`MainWindow.cpp`.

| Pri | Item | Status | Detail |
| --- | ---- | ------ | ------ |
| **P0** | Stop growing the god-objects, and **decompose** rather than relocate: the split into `MainWindow_<Area>.cpp` moves rows between files and leaves the coupling intact. Target &lt; 3000 lines/file, POM1 `MainWindow_*` discipline. | 🟢 **the UI half is done 2026-08-23.** `MainWindow.cpp` went 5 590 → 6 622 (the audit that set the target) → 11 511 → **11 154** — the first fall since the rule was written, and it came from removing the SIX parallel per-panel lists (settings load 32, save 32, palette 38, palette dispatch 38, menu rows 37, WASM chrome-light 28), then the 38 `bool showXxx` members, then the ~43 `renderXxxWindow()` calls. One catalog + one registry; adding a panel is a row and a `draw` line. `tools/check_file_sizes.sh` ratchets the ceiling, which now only falls. **Left**: moving panel *bodies* into their own TUs — a move rather than a rewrite now that nothing outside a body refers to it. | [Arch](#arch-refactor--tooling) `MainWindow.cpp` god-object; [DEV § Panel registry](DEV.md#panel-registry-panelcataloghpanelregistry-mainwindow_panelscpp) |
| **P1** | TSan on the **GUI** half + remaining mutex grain. ASan cannot see UI races; audio jitter under disk-turbo is a product bug, not a micro-opt. Mockingboard SPSC handoff next **if** a profile still shows the per-instruction card mutex. | 🟡 open, but no longer unattended: a **nightly ASan+UBSan / TSan matrix** runs in `ci.yml` as of 2026-08-22 (`POM2_SANITIZE` had been a CMake option CI never used, so the "controller TSan clean 2026-08-17" result had nothing keeping it true). GUI / `demodMutex` / slot re-plug under load still need a targeted pass. OE-CPU demod **already** runs after `stateMutex` release (2026-07-12). | [Arch](#arch-refactor--tooling) TSan; [Audio](#audio) mutex contention; [Display](#display-hgr--dhgr--80-col) demod ✅ |
| **P1** | Transactional disk insert (load-into-scratch-then-commit). Perceived quality + media integrity. | ✅ DONE 2026-08-13 (`9ae1784`) | [Storage](#storage-disks--images) |
| **P2** | Split `Memory` (`Keyboard` + `PaddleInputs`) **after** an I/O-path test net, not before. The 256-entry `memRead` dispatch is a **perf** job; the split is **compileability**. Do not merge them. | 🟠 open | [Memory](#memory-paging--ram-expansion) god-object vs `memRead` hot path — two items |
| **P2** | Debugger runtime glue (BP / watch / step). 80 % of the bricks exist (`Disassembler6502` + MemView). An emulator at this fidelity with no BP/step is a simulator you *watch*, not one you *interrogate* — and it blocks contribs. | ✅ **BP + step + step-over + run-to-cursor DONE 2026-08-22; WRITE watchpoints DONE 2026-08-23** (`Debugger.h/.cpp`, `Debugger_ImGui.*`, `MemoryWatchSink.h`, pinned `debugger`; zero measurable cost armed or not, PERFORMANCE § 8.3). READ watchpoints the same day: no free per-address hook on `memRead`, so one divert flag folded into existing fast-path tests — +0.0 % un-armed, PERFORMANCE § 8.5. | [Arch](#arch-refactor--tooling); [§ Debugger](DEV.md#debugger-debuggerhcpp-debugger_imgui) |
| **P3** | Kill or officialise the scaffolds. | ✅ **officialised 2026-08-23.** (1) **IWM data path**: already settled — default `iwmAuthoritative_ = true` (IWM is the truth for 3.5"; 5.25" always from DiskIICard's LSS), the only flip is the env var `POM2_IWM_AUTHORITATIVE=0`, a debug-only A/B toggle never reachable from the UI or settings. Not dual product behaviour; documented as debug. (2) **`echoplus_tms`**: kept for software detection but its catalog label now says "— silent, detect-only", so it no longer poses as a working speech card. (3) **Phasor**: dashboard #19 reclassed Verbatim → Partial-verbatim — the bus decode is verbatim, the audio timeline is not (no `emuCycles`-stamped AY write queue; the callback snapshots the banks). Implementing that queue stays a 🟡 [Audio] item, but "verbatim" no longer overstates it. | `Memory.h` IWM flag; dashboard #19/#21bis; [Audio](#audio) Phasor queue |
| **P3** | CI `ctest -L rom` + ROM Status **degraded** (running the synthetic fallback is not « missing »). Otherwise the L0 path rots behind a green suite that SKIPs when dumps are absent. | 🟡 open |
| **P3** | ~~Finish the `stateMutex` family: the HDV / block-device mount~~ ✅ **DONE 2026-08-22** — 25.8 ms under the lock → 0.0 ms. What is left of the family is the FujiNet `transact` wait and two thread `join()`s, both 🟡. | ✅ | [Open and known to be open](#open-and-known-to-be-open--2026-08-22-bug-hunt) | [`docs/lle_vs_hle.md`](docs/lle_vs_hle.md) § Keeping a level once you have it |
| **P4** | Hygiene for the second contributor: one `Config` (env → CLI → Settings → defaults), `pom2::` namespace, remaining atomic-write helper copies. | 🟡 open | [Arch](#arch-refactor--tooling) scattered config / namespace / `AtomicFileReplace.h` |

**Explicitly not architecture** — do not pick these ahead of P0–P4. They stay
in the backlog as features:

- Analog IIR-on-signal composite pipeline (academic, *5–10 d*)
- Saturn 128K Language Card
- ayumi-grade FIR resampling (deliberate MAME departure + WASM cost)
- Apple IIgs (already a separate **pom2gs** project — see [Out of scope](#out-of-scope))

## Backlog

Grouped by subsystem. Severity encoded by 🟠/🟡/🟢 at the head of each item.

### [Memory] paging & RAM expansion

- 🟠 **God-object split** — extract `Keyboard` (FIFO + strobe + paste)
  and `PaddleInputs` (RC + buttons + Open/Solid Apple) from `Memory.cpp`.
  `IIcClassProfile` already done (`MemoryProfile`/`MemoryProfile_IIcClass.h`).
  *Cuts recompiles + readability; any IIgs reuse happens in the separate
  pom2gs project, not here. ~2 d.*
  Architect order: **P2** — after an I/O-path test net, and **not** merged
  with the `memRead` dispatch table below ([Architect attack order](#architect-attack-order)).
- 🟡 **Saturn 128K LC** (Saturn Systems) — 16 banks ×16 KB on LC
  `$D000-$FFFF`, switches `$C080-$C08F` slot-relative. MAME refs
  `bus/a2bus/a2memexp.cpp`. *2-3 d.*
  Feature, not architecture — do not pick ahead of P0–P4.
- 🟡 **`Memory::memRead` hot path** — the multi-level `if` cascade is
  `Memory::memReadSlow`; `memRead` itself is the inline fast path (RAM, ROM
  window, and since 2026-08-20 the //e internal `$C100-$CFFF` ROM;
  `memWrite` has the same split). What remains is the condition chain in
  front of the ROM-window hit (~15 % of a ][+ banner, `PERFORMANCE.md` § 7.4):
  a 256-entry dispatch table per high page would replace it with one indexed
  load, at the price of an invalidation at every paging-state writer. Any
  change here must keep `tests/bus_fastpath_test.cpp` green — it is the
  differential oracle for the fast paths. Prerequisite: `IIcClassProfile`
  extraction (done). Perf job, orthogonal to the Keyboard/Paddle split — do
  not merge the two.
- 🟢 **Dedicated Pascal LC** — 16 KB variant shipped with Apple Pascal,
  minor differences vs IIe LC (write-protect DIP). *1 d.*

### [Display] HGR / DHGR / 80-col

- ✅ **OE-CPU demod out of `stateMutex`** — DONE (2026-07-12, wave 4).
  `drawScreenImage` now scopes `lockState()` around `display->render()`
  only (`MainWindow.cpp:2354-2364`); the 17-tap × 560×192 FP demod runs
  in `finishPendingCpuDemod()` after release. → `CHANGELOG.md`.
- ✅ **Frame-wrap video-event off-by-one** — DONE (2026-07-12, wave 4).
  Events stamped past the frame boundary now carry into the new recording
  frame; pinned by a real-6502 straddle case in `video_event_publish`.
  → `CHANGELOG.md`.
- ✅ **`renderCompositeOeCpu` PAL line-phase alternation** — DONE
  (2026-07-12, OE composite hunt). The CPU demod applies the per-line V
  sign (`palMode && (y & 1)`, `Apple2Display.cpp:~896`), pinned
  pixel-identical to the GPU shader by `oe_demod_gpu_cpu_parity`.
- 🟢 **Golden coverage gaps** (from the 2026-07-12 audit; mostly closed
  2026-07-12 wave 4, table 112 → 164 pins — flash-on phase, PAGE2/80STORE,
  rev-0 HGR+AN3, IIe 80COL+HIRES+MIXED without DHGR, Chat Mauve sub-modes
  all hash-frozen: `iie/text40flash`, `text40page2`, `hgrpage2`,
  `hgr80store2`, `hgran3`, `hgr80colmix`, `textcolorcm`). Remaining:
  ALTCHAR/mousetext + char-ROM glyphs (need a user ROM), PAL beam-raced
  splits (stay behavioural). Also: OE-GPU uploads the unused ~430 KB
  fallback framebuffer every frame (minor perf).
- ✅ **CRT post-process shader** — DONE. `CrtEffectStack` applies barrel →
  hue → BCS → phosphor curve → scanlines → shadow mask → vignette → luminance
  gain → edge-mask → persistence on any framebuffer, with a "CRT Settings
  (sliders)" panel and `crt_effects_enabled` toggle. Detail → `DEV.md` §
  CrtEffectStack.
- ✅ **Signal-level analog NTSC pipeline** — DONE. Signal-level demod
  (`ColorCompositeOE` GPU + `ColorCompositeOECpu`: 14.318 MHz waveform → FIR
  Y@2.0 MHz / chroma@0.6 MHz → YUV→RGB, PAL line-phase) plus the phosphor
  curve added 2026-05-31 (`phosphorGamma`, key `ntsc_phosphor_gamma`). Detail →
  `CHANGELOG.md` / `DEV.md`.
  - 🟢 Remaining *(deferred, academic)*: pure-analog signal-level pipeline (IIR
    on the signal itself before demod) vs the current 1-bit signal + FIR, *5-10 d*.
    Feature, not architecture — do not pick ahead of P0–P4.
- ✅ **Beam-racing per-scanline composite** — DONE (2026-05-31).
  `fillCompositeSignal(mem, events)` replays the event log band by band so
  mid-scanline switches reach the composite modes (OE GPU/CPU, AppleWin), not
  just LUT modes. Pinned `beam_race_composite`. Detail → `DEV.md` § Beam-racing.
  - 🟢 Remaining: `signalPhaseOffset_` stays a per-frame constant (mid-frame
    HGR↔DHGR split approximated); lo-res clips at block-row (4 lines), like the
    RGBA path.
- ✅ **Horizontal mid-scanline split (per-byte granularity)** — DONE
  (2026-06-09, 280-wide + composite signal + 560-wide IIe/Chat Mauve).
  Beam-racing is no longer scanline-quantized: `renderBeamRacing`
  (`Apple2Display.cpp:~336`) reconstructs per-scanline column segments so a mode
  can change mid-scanline (TEXT/LORES/HGR on the same line — Codebreaker GEN2
  "color peg"); split is visible in ColorCompositeOE GPU/CPU + AppleWin and in
  80-col/DHGR/DLGR/Le Chat Mauve. Pinned `horizontal_split`,
  `horizontal_split_composite`, `horizontal_split_560`. Detail → `DEV.md` §
  Beam-racing / `CHANGELOG.md`.
  - 🟢 Remaining *(deferred)*: 40-col (280) + 80-col (560) mixed on the same line
    is undefined (separate `frame`/`frame80` buffers, scoped out); exact
    transition cycle at character-clock = later refinement. **Back-port to POM1**
    next (gated: LORES+TEXT rendering on GEN2 — HGR-only today — + HBLANK flag
    Phase 2 per Bernie's spec).
- ✅ **PAL 50 Hz machine timing** — DONE (2026-06-09, full machine). `enum
  VideoStandard{NTSC,PAL}` + `VideoTiming` table in `CpuClock.h` (NTSC
  262/60/1.0227 MHz, PAL 312/~50/1.0156 MHz, 20313 cyc/frame), threaded through
  `Memory::pushVideoEventLocked`, `Apple2Display::frameCycleToPos` and
  `EmulationController::setVideoStandard`. PAL profiles `Apple //e PAL` and
  `Apple //c PAL (Le Chat Mauve)`, wired in `applyProfile`, Presets menu, and CLI
  `--preset iie-pal|iic-pal|chatmauve`. Pinned `pal_timing`, `video_event_publish`.
  Detail → `CLAUDE.md` § System profiles + `docs/test_corpus.md` § DIX /
  `CHANGELOG.md`.
  - 🟢 Remaining: device clocks (AY/IWM/SSI263) stay at NTSC nominal (0.7 % delta
    = inaudible audio pitch, not retimed — speaker + cassette realtime audio ARE
    retimed since 2026-07-11/12, their queues starve audibly otherwise); WASM
    pacing (RAF 60 Hz) not yet
    switched to 50 Hz; manual NTSC/PAL toggle + auto-PAL when a Chat Mauve card
    is plugged (the PAL profiles already cover the use case).
- ✅ **DROL — page-flip flicker + cut-scene hang** — DONE (2026-06-10). Three
  bugs found booting the real `Drol.woz`/`.dsk` (probe `tests/drol_probe.cpp`):
  (1) page-flip flicker — `forEachBeamSegment` now detects unidirectional PAGE2
  events in a frame as a buffer flip → final full-frame page (bidirectional DIX
  MODPAGE keeps exact replay); (2) cut-scene hang — `$C05x`/`$C030-3F` reads now
  toggle the mode/click AND return `floatingBus()` instead of hard 0; (3) the 6
  560-wide painters now read band state, not live state. Pinned
  `drol_pageflip_render`, `vapor_lock` §(d). Detail → `DEV.md` § Beam-racing +
  `CHANGELOG.md`.
  - 🟢 Assumed limit: an intentional unidirectional mid-frame page split renders
    full-page (true remedy = incremental per-scanline rendering MAME-style).
- ✅ **Le Chat Mauve HGR resolution (AppleWin RGB decode)** — DONE
  (2026-06-10). `renderHiResChatMauve80` ported AppleWin's `RGBMonitor.cpp
  UpdateHiResRGBCell`: a pixel is COLOR only if it forms an isolated 010/101
  pattern with its neighbors, otherwise black/white at full 280 px resolution so
  white runs (text, outlines) regain their sharpness. Pinned `le_chat_mauve_smoke`
  + `display_persistence_smoke`. Detail → `CHANGELOG.md`.
- ✅ **Eve Color text mode `$C0B8/9`** — DONE (2026-05-28). FG/BG per
  character: toggle decoded at `LeChatMauveCard.cpp:54-55`, rendered by
  `renderTextChatMauveFgBg`, persisted as `chatmauve_color_text`,
  hash-frozen by golden scene `iie/textcolorcm`.
- 🟢 **"Smooth" interpolated sub-pixel mode** — bilinear/Lanczos on
  HGR/DHGR, UI toggle. Inspired by microM8. *2 d.*
- 🟢 **DHGR mono 1-px alignment + floating-TTL `empty_words` +
  per-scanline mode switch** — cosmetic / out-of-bounds.
- 🟢 **CRT parity refinements vs OpenEmulator** — low-priority residuals from
  the 2026-05-30 video audit (detailed implementation notes →
  `docs/archive/video_parity_revalidation_2026-05-30.md` §4):
  - **F4** POM2 CRT defaults 0.25/0.5/0.4 (scanlines/mask/persistence) vs OE
    ~0.05/0.05/0 — *biggest visual gain*, OR own the "punchy" choice and
    document it (`NtscPostProcessor.h`).
  - **F3** vignette — ✅ DONE (2026-05-30): OE-exact
    `exp(−dot(cuv·(1/cl−1)))` (`CrtEffectStack.cpp:301-306`), default
    `centerLighting = 1.0` = flat (`NtscPostProcessor.h:74`).
  - **F2** cosine scanline → OE's **sin²** (keep the `scanAA` anti-moiré term).
  - **F7** HGR mono: 280 px average / 3 levels → **560 binary** (copy of the
    DHGR-mono loop already shipped).
  - **F6** row-dim mask ×0.7 ⚠ (make luminance-neutral, don't drop hard).
  - **DLGR wired** to `fillCompositeSignal` — ✅ DONE (2026-05-30):
    `paintLoResDouble` aux+main (`Apple2Display.cpp:2372/:2536/:2606`).
  - *(Non-items, documented: F1 clamp double > AppleWin float; F8/F9 amber/green
    tints assumed — optional "AppleWin-faithful" preset.)*
- 🟢 **Le Chat Mauve EVE** (64 KB ext RAM + SPEC1/SPEC2/DASH/COL280),
  **Video-7 AppleColor RGB**, **Color killer Rev 1**,
  **Strapping RAM 4K→48K**.

### [Audio]

#### Mockingboard output level vs MAME's route gain — open question (2026-08-02)

Raised by "il reste des choses à améliorer au niveau des basses". The
2026-08-02 sweep measured the whole card render chain against MAME and found
it already correct down to 27.5 Hz — volume table within 0.0007 of Westcott's
data, linear channel sum matching `a2mockingboard.cpp`, box integrator with a
sinc gain of 1.0000 below 100 Hz, no stereo cancellation. The one real gap
was the DC blocker (1-pole where MAME uses a 2-pole Butterworth), now ported,
worth ≤0.8 dB below 80 Hz.

That is almost certainly **too small to be what is actually heard**, so the
remaining suspect is level rather than frequency response: POM2 normalises
the per-side chip sum by `/3` (Mockingboard) and `/6` (Phasor), while MAME
routes each AY channel through `add_route(ALL_OUTPUTS, "speaker", 0.5, ch)`.
Those are different scalings, and a card that sits low against the speaker
and disk channels reads as thin. Wants a numbers-first comparison of POM2's
end-to-end output level against MAME's for the same register stream, not more
tuning inside `AyPsgSynth.h`. → `CHANGELOG.md` 2026-08-02.

#### Mockingboard AY-3-8910 rendering — 2026-08-01 pass

Triggered by "DD2's Mockingboard music sounds coarse". Four research
agents (MAME `ay8910.cpp`/`resampler.cpp`, AppleWin + `ayumi`, a POM2
audit, and a headless register trace of the real disks) plus a rendering
rework. Full reasoning → `CHANGELOG.md`; abstraction rationale →
`docs/lle_vs_hle.md`. Probes: `tests/mockingboard_audio_quality`
(spectral purity / DC / write placement / 16 envelope shapes),
`tests/dd2_ay_trace`, `tests/dd1_audio_ab`.

**Landed:**

- ✅ **Band-limiting.** The mixer is box-integrated over the ~2.9 clock/8
  ticks each output sample spans instead of point-sampled once. MAME
  sidesteps this by rendering on the chip's clock/8 grid
  (`ay8910.cpp:1298`) and decimating (`src/emu/resampler.cpp`); POM2
  renders at the device rate, so the decimation is inline. Inharmonic
  energy on a 4 kHz note: **7 % → 0.51 %**. Cost 0.67 % of a core/chip.
- ✅ **Register replay is a jitter buffer.** Un-rendered events stay
  queued; the cursor is held ~2 PAL frames behind `latestAyEventCycle_`.
  The old code drained the queue every callback, applied the leftovers in
  bulk at the buffer edge (so only the last value per register survived)
  and parked the cursor on the newest event at zero lag.
- ✅ **DC blocker**, matching MAME's default per-speaker high-pass
  (`src/emu/audio_effects/filter.cpp`): landed as 1-pole 20 Hz, upgraded
  2026-08-02 to MAME's actual 2-pole Butterworth biquad
  (`AyPsgSynth.h:329-337`, see the 2026-08-02 note above).
- ✅ **PAL AY clock.** Tick rate now derives from the live CPU clock —
  pin 22 is the slot's phase-0 line, so PAL really is 1 015 625 Hz. PAL
  music was 12 cents sharp.
- ✅ **Shared synth core** `src/AyPsgSynth.h`. Mockingboard and Phasor
  carried verbatim copies (~130 lines, 4 differing lines) that had
  already drifted. Phasor is −149/+51.
- ✅ **Verified, not assumed:** the envelope state machine is correct on
  all 16 shapes including the alternate ones ($0A/$0E) — `-1 & 0x10` is
  the same integer promotion MAME's `s8 step` gets. Noise LFSR taps,
  prescale and period-0 handling match `ay8910.h:263-273`. The
  `kAyVolumeTable` **provenance comment** was wrong and is corrected.

**DD1 regression — ✅ RESOLVED 2026-08-01, cause measured:**

- ✅ **Root cause: the `caughtUp` guard measured lag against the last
  WRITE (`latestAyEventCycle_`) instead of CPU-now.** DD1 writes one dense
  clump per frame and is write-silent for **88 % of every frame** (worst
  gap 17.6 ms); that silence was charged against the guard's budget.
  Margin before a false trip: **1.06 ms on DD1 vs 9.6 ms on DD2** — hence
  one demo glitching and not the other. Each trip froze the register bank
  for **40-46 ms** (two frames). Fixed by pacing against `lastSyncCycle_`.
  Measured with `tests/dd1_audio_ab` on the real disk.
- ✅ **Double envelope retrigger, introduced by the 40 ms lag.**
  `ayEnvWriteCount_` is a CPU-now counter read while the cursor runs 40 ms
  behind, so every R13 store retriggered twice — 202 spurious retriggers
  in 25 s, 13.8 % of RMS. The replayed event already covers same-value
  stores; the counter path is dropped.
- ✅ **Timbre change attributed:** **89.5 % of the OLD render's power was
  below 50 Hz** (unipolar digidrum PCM pedestal). The DC blocker removes
  it; audible level is **+0.79 dB**, so "quieter" was the missing bottom.
- ⚠️ **Probe gotcha:** `tests/dd2_ay_trace` never calls `setCpu()`, so
  `lastSyncCycle_` stays 0, every event is stamped cycle 0 and the whole
  replay path is silently bypassed. Any new Mockingboard probe must wire
  the CPU or it measures nothing.
- ⚠️ **Methodology.** Two successive synthetic harnesses PASSED against
  the bugs they were written to catch (NTSC bursts vs PAL-sized lag; an
  unbroken write stream vs a production gap). The fault needs the audio
  and CPU clocks to be genuinely independent. **A Mockingboard audio
  regression test is only credible once shown to FAIL against the
  reverted fix.**

**Landed 2026-08-01 (stereo pass):**

- ✅ **True stereo bus.** `AudioDevice` carries interleaved stereo;
  Mockingboard routes AY1 → L / AY2 → R at `/3` per side, speech centred
  (MAME `a2mockingboard.cpp:159-165`, `:186-189`), Phasor splits by VIA
  pair at `/6` per side (`:192-208`). The mono contract is untouched —
  a mono source is centred at unity on both channels, so nothing moved
  level — and each card keeps a mono fold-down that reproduces its old
  summed render bit-for-bit. DD1's deliberate A/B/C pan survives the mix
  now. `setMonoDownmix` restores a centred image for mono gear.
  Pinned: `tests/audio_stereo_test.cpp`. → [DEV § Stereo
  bus](DEV.md#stereo-bus-2026-08-01).

**Deferred, with the trade-off recorded:**

- 🟢 **SSI263 / Echo+ placement is a guess beyond MAME.** MAME centres
  the Mockingboard's speech chip and gives the Echo+ TMS5220 a
  `front_center` speaker, which is what POM2 does; where a *pair* of
  speech chips would sit (a two-SSI263 Sound II, or Phasor + Echo+ mode)
  has no oracle. Left centred until one turns up.
- 🟢 **Phasor: no cycle-stamped event queue, no `setCpuClock` override.**
  Every register write inside a buffer still collapses to its last value,
  and PAL clocks its AYs 0.7 % fast. Now the only divergence from
  Mockingboard rather than a hidden one.
  Architect order: **P3** — until this lands, « verbatim » is an audio lie
  ([Architect attack order](#architect-attack-order)).
- 🟢 **ayumi-grade resampling** (native clock/8 → 8× quadratic interp →
  192-tap FIR decimation + moving-average DC filter,
  `true-grue/ayumi`, MIT). Strictly better than the box filter and what
  chiptune players use; ~8× the inner iterations plus ~96 MACs per sample
  per channel, ~192 doubles/channel of rewind state and ~2 ms group
  delay — a real cost on the **WASM** target. Only worth it if listening
  shows the box filter is insufficient. Note this would be a deliberate
  departure from "MAME = source of truth" for the audio path.
  Feature, not architecture — do not pick ahead of P0–P4.
- 🟢 **Analog output stage.** The real Sweet Micro board's LM386 pair
  makes the output *triangular*, not square (deater's scope capture:
  `deater.net/weave/vmwprod/chiptune/mock_problem/`). No emulator models
  it and there is no MAME oracle, so it would have to be an off-by-default
  toggle labelled non-authoritative — and only after band-limiting, since
  a low-pass over an aliased signal muffles rather than removes.
- 🟢 **Mutex contention.** Architect **P1** (SPSC handoff *if* a profile
  still shows it after TSan-GUI). `advanceCycles` takes the card mutex on every
  emulated instruction (~1 M/s) and the realtime audio callback needs the
  same one, holding it across the whole SSI263 render on Sound II.
  Classic priority inversion; wants an SPSC handoff.
- 🟢 **Mute drops the queue.** The `isMuted` early-out returns after
  `pending` has been drained, so writes are lost while muted and `vol`
  changes are applied as a hard step at buffer boundaries (click).

- 🟢 **8-bit DAC (Marczewski)** — 8-bit slot latch → R-2R DAC. Niche
  demos (Music Studio, trackers). AppleWin refs `Card::CT_DX1`. *1 d.*
- 🟢 **Passport MIDI Music Card** — 6840 + 6850, Master Tracks Pro /
  Performer. MAME refs `mc6840.cpp` + `acia6850.cpp`. *3 d.*
- 🟢 **AY Port A read mask by DDR** (R14/R15) — academic.

### [Storage] disks & images

- 🟢 **`DiskImage` is a 242 KB object, and the stack-overflow class is only
  patched, not closed.** *1 d, measure first.* The 2026-08-23 macOS SIGBUS
  fix heap-allocates the six insert-path temporaries; any future
  `DiskImage` local on a secondary thread (512 KB on macOS) reintroduces the
  crash, and `diskii_insert_thread_stack` only pins the insert path. Closing
  the class means moving `tracks` (35 × 6656 B, in-object) to the heap —
  which also turns every `DiskImage` move (the install under `stateMutex`)
  from a 233 KB memcpy into a pointer swap. It adds one indirection to the
  bit-stream rebuild and to `writeFlux`, neither per-nibble-hot, but the
  LSS is the emulator's hottest disk code: interleaved best-of-9 on the
  three `pom2_bench` workloads (PERFORMANCE § 8) before and after, or not
  at all. Until then the NOTE on `class DiskImage` is the only guard.

- 🟢 **Empty Disk II drive: the two write-protect answers disagreed — ✅
  RESOLVED 2026-08-22.** Write-protect is ONE wire and POM2 offered two
  ways to read it: the canonical `LDA $C08D,X / LDA $C08E,X / BMI`
  sequence, which returned `0x80` (protected) for an empty drive, and
  POM2's own shortcut at `$C0nD`, which returned `0x00` (writable) when
  `!isLoaded()`. What a guest was told therefore depended on which idiom
  it used. Protected is the correct answer and not only for consistency:
  the sense is a phototransistor watching the write-enable notch, and
  with no disk in the way the light reaches it, which *is* the protected
  state. Both sites (bit-LSS and legacy gate) now answer alike.
  Pre-existing since `be6d8be` (2026-05-27), surfaced by the 2026-08-22
  bug hunt. Pinned by `diskii_empty_drive`, which checks both probes
  against each other on both gates, empty and loaded — the loaded case
  with write-back ON, or `isWriteProtected()` is true for every image and
  the check would only be exercising the toggle.

- 🟢 **//c+ 5.25" dual-controller — ✅ RESOLVED 2026-07-29.** The repro
  (headless //c+ cold boot, `tests/iicplus_boot_probe`) exposed that the
  visible failure was upstream of the dual controller: (1) IWM status
  with no selected drive answered with the 5.25" image's WP bit instead
  of MAME's "no floppy → SENSE high" (`iwm.cpp:129`), and (2) the Sony
  DSKCHG sense had inverted polarity for an empty drive — together they
  hung the firmware's boot drive-scan at $F0FC (no banner, ever). The
  dual-controller hazard itself was real on writes: the IWM's flushWrite
  pushed 5.25" flux into the same DiskImage the LSS wrote (now
  suppressed — DiskIICard owns 5.25" flux), and the IWM read walker
  mis-framed RWTS verify (SAVE → I/O ERROR) — $C0Ex reads are now
  IWM-authoritative only while the hub routes to a 3.5" Sony. Pinned by
  `iic_plus_boot_write` (boot to DOS 3.3 banner + SAVE/LOAD/RUN
  round-trip on the //c+ profile).
- ✅ **A failed insert destroys the disk that was in the drive** — DONE
  (2026-08-13, `9ae1784`). Load-into-scratch-then-commit landed in both
  `DiskImage::loadFile` overloads (`DiskImage.cpp:568`, `:725`): decode
  into a temporary `DiskImage`, move-assign only on success, so a corrupt
  / wrong-size image leaves the mounted disk untouched.
- 🟢 **`decodeTrack` trusts the address field** *(management audit
  2026-08-08)* — the write-back decoder reads vol/track/sector/checksum
  as 4-and-4 but validates none of them: the checksum is discarded, and
  the address field's TRACK number is ignored in favour of the buffer
  index. A guest that rewrites a whole track with a different track
  number in its address fields (sector editors, Locksmith-style
  copiers) therefore lands its sectors at the wrong file offset. `$D5`
  is not a legal GCR data byte so a spurious prologue match can't
  happen, which is why this has never bitten in practice. *~1 h.*
- ✅ **800 K `.dsk` routes to the 3.5" bucket** — DONE (2026-08-14,
  autoboot hardening — see [UI/UX]). The size-gated `.dsk` rule is in
  BOTH predicates, kept in lock-step (`classifyDiskForSlot`
  `DiskImage.cpp:2695`, `accept525` `DiskLibrary_ImGui.cpp:35`).
- ✅ **WOZ FLUX quarter-tracks are no longer *silently* read-only** —
  DONE (2026-08-13; documented as a deliberate refusal, CHANGELOG
  2026-08-18). A medium with populated FLUX tracks is file-write-
  protected at mount, with the FLUX count and "file-WP" in the load log
  (`DiskImage.cpp:1302-1325`). Honouring writes would need re-encoding
  the delta stream — still out of scope.
- 🟡 **WOZ1 splice point (TRK+6650)** — `DiskImage::writeFlux` splices
  bit-cells but the full `set_write_splice` handling (TRK +6650
  splice_point/nibble/bit_count fields, parsed at `DiskImage.cpp:827`)
  is ignored; IWM call site wired (`IWMDevice.cpp:235`, see the comment
  at `IWMDevice.cpp:48`). Applesauce re-master parity. *1 d.*
- 🟡 **SmartPort ProDOS multi-partition** — 1 image = 1 unit = 1
  volume today; multi-volume CFFA3000-style not supported.
- 🟢 **UI "Force DOS / Force ProDOS"** — backend ready
  (`DiskImage::loadFile(path, SectorOrder)` at `DiskImage.cpp:725`),
  button missing in `DiskLibrary_ImGui` / `DiskController_ImGui`.
  Auto-detect (extension + vol-dir content sniff `0x400`/`0xB00`)
  already covers 99 % of cases; manual override useful for ambiguous /
  non-standard / debug images. *~30 min.*
- 🟢 **Half-tracked NIB (88)** — deliberately out of scope as long as
  WOZ covers it. Its two former companions are done: **Disk II in
  snapshot** (snapshot v2 with nibble track buffers,
  `DiskIICard.h:254-255`, pinned `rewind_disk_write` — see [UI/UX]
  Rewind) and the
  **Applesauce CNib2 format** (`DiskImage.cpp:505`, pinned
  `disk_cnib2_smoke`) — only the literal `.nib2`/`.app` extensions are
  still missing from `classifyDiskForSlot` / `accept525`.
- 🟢 **Floppy Emu Dual-5.25" + Smartport-Unit-2 modes** — out of scope
  for v1 (4 main modes covered).
- 🟡 **//c+ on-board 3.5" boot through the real IWM** — the pieces are all
  there and individually pinned: `IWMDevice` is a verbatim MAME port
  (dashboard #11) including the bit-cell read walker and the write windows,
  `Sony35Drive` + `Sony35Gcr` serve zoned 800 K GCR, and the //c+ alt
  firmware's MIG gate-array windows (`$CC00-$CCFF` / `$CE00-$CEFF`) are
  decoded. What does **not** work is the //c+ ROM's own boot path *through*
  them: a cold //c+ with only a 3.5" image mounted never reaches a bootable
  disk. The supported route is the host-served SmartPort block device at
  built-in slot 5 (`iic_onboard_smartport_smoke`), which boots 3.5"/HDV on
  every //c-class profile — so this is a fidelity gap, not a functional one,
  and it is **owned-out-of-scope for 1.0**. Referenced from `CLAUDE.md`
  § System profiles and from [WASM](#wasm) below.

### [Cards] slot cards & peripherals

- ✅ **LOW batch from the 2026-07-29 hunt — ALL CLEARED 2026-07-30.**
  The last seven (NMOS NOP abs,X page-cross, lazy snapshot DMA disarm,
  CLI Phase-C ordering gate, AY READ bus latch + VIA port-A input pin
  model, Apple2Display published-frame routing, $C019 sub-instruction
  VBL sampling, Z80 block-repeat X/Y-from-PCH) are fixed — see
  CHANGELOG 2026-07-30. Nothing from that hunt remains open.
- 🟢 **Microsoft SoftCard (Z80) + CP/M — ✅ ALL 3 PHASES DONE 2026-07-12**
  (Z80 core zexdoc+zexall 100 % → `SoftCardZ80` card + generic DMA
  arbitration → CP/M 2.2 boots to `A>`: 44K v2.20 master on II+ 40-col
  and 60K v2.23 on //e 80-col, MAME-oracle-identical; pinned by
  `softcard_toggle` + media-gated `softcard_cpm_boot[_iie]`; see
  [DEV § SoftCard Z80](DEV.md#softcard-z80-softcardz80hcpp--cpm-phase-2)).
  Post-MVP ideas: Videx Videoterm 80-col for II+ CP/M, PCPI Applicard
  (reuses the Z80 core + DMA hook as-is), Turbo Pascal / WordStar /
  MBASIC corpus entries in `docs/test_corpus.md`.
- 🟢 **SmartPortCard leftovers** (2026-07-12 Liron audit follow-ups —
  STATUS pre-flight, the SmartPort `$Cn0D` dispatch and the real-ROM
  identity all landed same-day, see CHANGELOG): empty-bay WP error code
  is $2B where $28 "no device" is the honest one; boot failure is a
  silent `JMP $CnE0` loop (real firmware prints an error); 3.5-type units
  present WP-until-write-back while HDV bays are RAM-writable —
  inconsistent on the same card; CONTROL calls needing the control-list
  DATA (only code 0 works — the stub has no guest→device list copy);
  extended $4x calls return $01.
- 🟢 **`$C05E/F` ignores IOUDIS on //c-class** (MAME gates DHIRES on
  `m_ioudis`); II+ broadcasts `$C00C/D` on reads while IIe is write-only —
  both flagged for awareness by the 2026-07-12 Chat Mauve review.
- ✅ **Grappler+ printer (`GrapplerCard`) — MAME pin DONE 2026-07-28.**
  Pinned line-by-line against MAME `bus/a2bus/grappler.cpp` (status byte,
  register decode, ROM side effects, $C800 banking, S1 DIPs — ranges
  cited at each block). The audit fixed one divergence: reset no longer
  clears the ROM bank (U2D isn't wired to RESET; MAME `reset_from_bus`
  :536-539), and `$CnXX` writes now drop the bank (`write_cnxx`
  :586-591 via `slotRomWrite`). PDF output shipped with the ImageWriter
  PDF export (see [Printer]). Detail → `DEV.md` § Grappler+.
- 🟡 **EchoPlusTMS5220Card (real Echo+)** — catalog scaffold
  `echoplus_tms`: SlotPeripheral + stub register decode at
  $Cs00-$Cs0F, enough for detection (`EchoPlusTMS5220Card.h:15-17`).
  Remaining: TMS5220 LPC10 decoder (chirp ROM + K-parameter
  interpolation) and AY-3-8913 audio synth — the shared AY core it
  needs already exists (`src/AyPsgSynth.h`, extracted 2026-08-01,
  see [Audio]). *~3-5 d.*
  Architect **P3**: hide from the catalog until the chip exists, or ship
  it — a detect-only stub is the wrong third option.
- ✅ **No-Slot Clock (NSC, DS1216E)** — DONE. `src/NoSlotClock.{h,cpp}`
  is a full DS1216E SmartWatch state machine for machines with no free
  slot (//c), hooked into `Memory`'s read **and** write paths — the key
  bit rides on the address, so `STA`-driven drivers unlock it too. The
  watched window follows the ROM the era's drivers probe: `$F800-$FFFF`
  on II/II+, `$C300-$C3FF` + `$C800-$C8FF` on //e and //c-class (where
  ProDOS 8 ≥ 2.0.3 and GS/OS actually scan). MAME refs `ds1216.cpp`.
  Pinned by `no_slot_clock_smoke` (`tests/no_slot_clock_test.cpp`).
  Detail → `DEV.md` § No-Slot Clock.
- 🟢 **SSC IRQ gate SW2:6 DIP** not implemented (MAME `a2ssc.cpp:373`).
- ✅ **Real ClockCard slot ROM** — DONE (2026-07-30, shipped in `f7af757`).
  `roms/thunderclock_u9_v1.3.bin` (Thunderware Rev 1.3, 2 KB, source
  markadev/AppleII-RevEng) is in-repo and `ClockCard::tryLoadDump()` runs
  from the ctor (`ClockCard.cpp:78`): both the 256 B slot window and the
  full 2 KB `$C800-$CFFF` expansion are fed from the dump, gated on size +
  the `$08/$28/$58/$70` ProDOS signature, with the synthetic ROM kept as
  the fallback. The card is therefore **L2** — real firmware over the L1
  uPD1990AC model (→ [`docs/lle_vs_hle.md`](docs/lle_vs_hle.md)).
  Disassembling the dump also settled the shift-register width: the nibble
  routine at `$CACF` emits 4 CLK pulses × 10 calls = **40 bits, no year
  field**, so `clock_card_smoke` drives 40 pulses and is a firmware-parity
  test instead of a model tautology (CHANGELOG 2026-07-30).
  - 🟢 Remaining: nothing asserts the *real* path is taken when the dump is
    present — `clock_card_smoke` tolerates its absence so CI stays
    ROM-free, so a regression that silently routed back to the synthetic
    ROM would fail nothing. Same silent-degradation hole as every other
    ROM-driven L path (Disk II P6, mouse MCU, Grappler EPROM);
    → `docs/lle_vs_hle.md` § Keeping a level once you have it. The DOS 3.3 /
    Applesoft tools that pull the driver from `$C800` are still untested.
- 🟡 **[P2] Real Liron / UniDisk 3.5 (IWM in a slot)** — stack already
  there (`IWMDevice` verbatim, `Sony35Drive`, zoned GCR, `SmartPortHub`),
  and the ROM is no longer a blocker: `roms/liron.rom` (4 KB BMOW dump)
  is in-repo, catalogued (`RomCatalog.h:75`) and loaded by
  `SmartPortCard` (`SmartPortCard.cpp:64-65`, loader `:665-711`), pinned
  by `liron_smartport_dispatch`. Remaining: `LironCard : SlotPeripheral`
  driving the real IWM in a slot. *~8-12 h.*
- 🟢 **[P3] Apple II SCSI / High-Speed SCSI + CHD** — MAME
  `a2scsi.cpp` (NCR 5380) / `a2hsscsi.cpp` (53C80). Big lift for a
  niche need (CFFA suffices). *~30-50 h.*
- 🟢 **Apple II VGA / Second Sight (VGA video card)** — slot card that
  shadows the Apple II framebuffer and outputs a clean VGA signal
  (scanline mode + text/HGR/DHGR/lo-res modes). Two incarnations: the
  open-hardware project **markadev/AppleII-VGA** (RP2040, free firmware +
  KiCad, so registers and timing are documented) and the commercial
  **Second Sight** (reactivemicro, Brutal Deluxe manual). POM2 already has
  all the video decode (`Apple2Display`); the value would be modelling the
  card's soft-switches/registers for software detection and an optional
  "VGA-clean" output. Code + doc refs:
  - <https://github.com/markadev/AppleII-VGA> (RP2040 firmware + KiCad)
  - <https://www.brutaldeluxe.fr/documentation/secondsight/secondsight_manual.pdf> (Second Sight manual)
  - <https://downloads.reactivemicro.com/Apple%20II%20Items/Hardware/SecondSite_VGA/> (ReactiveMicro dumps/ROMs)
  - <https://www.apple2history.org/history/ah13/#05> (historical context)
  *~5-10 d (register sourcing + integration mode to be decided).*
- 🟢 **UDC (Apple 1991)** — 4 heterogeneous bays (3.5"/5.25"/HDV).
- 🟢 **Slinky / RamFAST RAM disk** — limited utility vs RamWorks III.
- 🟢 **Apple 3.5" Controller IWM-level** — refactor IWMDevice attached
  to a slot card (rare).

### [Cassette]

- 🟢 **Enriched WAV record/playback** — POM2 supports .wav; missing
  analog tape filtering (hiss, drop-out), VU-meter, timecode.
  MAME refs `apple2.cpp` cassette. *2 d.*

### [Printer]

- ✅ **Real ImageWriter character ROMs** — DONE (2026-08-10). Seven banks
  (IW II correspondence/draft/NLQ fixed+proportional, IW I fixed+prop) with
  7 locale variants each, generated into `src/ImageWriterRom.h` by
  `tools/import_printer_roms.py` from
  [mikedaley/web-a2e](https://github.com/mikedaley/web-a2e) (MIT). `ESC a`
  now changes the face, proportional advance comes from the glyph's own
  escapement, and the international sets are the ROM's real substitutions.
  The CP437 font is kept as the fallback. Pinned `printer_glyph`.
  Provenance settled 2026-08-10: keep, under web-a2e's MIT grant, with the
  source manual named in the generated header (`docs/printer_plan.md` § 3).
- ✅ **Screen dump to the printer** — DONE (2026-08-10).
  `PrinterScreenDump` synthesises the `ESC G` stream and feeds it through
  the real parser, so it obeys ribbon/pacing/paper and lands in the tray and
  the PDF. Palette: `printer.dumpscreen`. Pinned `printer_screen_dump`
  (round trip against the parser).
- ✅ **Printer power / online switch + custom paper geometry** — DONE
  (2026-08-10). Power off discards input and KEEPS the sheet (unlike
  `powerCycle`); offline likewise; paper is settable in ¼" steps with
  clamping that reports what it committed.
- ✅ **ImageWriter I + Apple DMP** — DONE (2026-08-10) via `IwModelProfile`,
  a three-row capability table rather than the class hierarchy the plan
  proposed: the two extra heads differ only in DATA (ROM banks, colour
  ribbon, absent ESC codes, power-on pitch, carriage rate). Note their
  correspondence faces are byte-identical to the II's upstream — POM2 keeps
  separate banks so a future divergence lands by itself. Pinned in
  `printer_glyph`.
- ✅ **[Printer] Epson FX-80** — DONE (2026-08-10). Its own ESC/P parser over
  the shared mechanism (`ESC *` / `ESC K L Y Z` graphics with binary counts
  and bit 7 as the top dot, n/216 and n/72 spacing, master select, the style
  and pitch set). Unimplemented commands are consumed WITH their parameters
  so a stray byte never prints as text. Pinned by an ESC/P round trip and by
  the `ESC G` collision (graphics on C. Itoh, double-strike on ESC/P). Also
  serves the FujiNet path, whose firmware printer is `epson80`.
- ✅ **Printer sound** — DONE (2026-08-10). SYNTHESISED, not sampled: there
  is no free ImageWriter sample set and web-a2e ships no audio assets
  either (checked), so `PrinterSoundDevice` ports its grain model — one
  bandpassed-noise grain per character/line feed, spaced along the audio
  timeline so they overlap into the buzz. The scheduling cursor is capped
  0.2 s ahead, which is what makes a full-black screen dump THIN instead of
  queueing 100 seconds of rattle. Pinned `printer_sound`. NOTE: the plan's
  § 9 claim that this needed `emuCycles` was wrong — the ImageWriter paces
  itself in wall clock, so the events are already in real time.
- ✅ **[Printer] Print history** — DONE (2026-08-10). Every ejected sheet is
  written to `printouts/history/` as a PNG with its printer / ribbon / paper,
  listed in the ImageWriter panel and clickable back onto the canvas. Index
  is tab-separated text, not JSON (POM2 has no JSON parser and this is a few
  dozen lines). Pinned `printer_history`. **This completes
  `docs/printer_plan.md` — all six phases.**
- ✅ **ImageWriter II printer** — DONE (2026-07-26). Host-side
  `ImageWriter` (ported from greg-kennedy/ImageWriter) + paper-tray
  window: full control language, four-band colour ribbon with subtractive
  overprint, `ESC G`/`ESC C` bit-image graphics, page stack, PNG export.
  Fed from `PrinterCard` / `GrapplerCard` spools by
  `MainWindow::pumpImageWriter()`. Pinned `imagewriter_smoke`.
  Detail → `DEV.md` § ImageWriter / `CHANGELOG.md`.
- ✅ **ImageWriter on the Super Serial Card** — DONE (2026-07-28).
  `SuperSerialCard::setPrinterTap` mirrors accepted-TX bytes into a
  `drainSpoolFrom`-shaped spool `pumpImageWriter()` consumes (parallel
  cards outrank it); defaults ON for slot 1 (//c printer port), persisted
  `ssc_printer_tap_slotN`. Also fixed en route: the synthetic SSC ROM's
  PR#n/IN#n entries now init the ACIA (cmd=$0B) like the real firmware —
  before, `PR#n : PRINT` bytes were DTR-dropped and only Pascal could
  transmit. Pinned in `ssc_acia_smoke`. Detail → `DEV.md` § ImageWriter.
- ✅ **PDF export** — DONE (2026-07-28). `ImageWriterPdf.{h,cpp}`:
  "Save PDF" writes all sheets as one multi-page PDF (8-bit Indexed
  images, FlateDecode via in-repo `stbi_zlib_compress`, per-sheet
  `/MediaBox` from the new `Page::dpi`). Chosen over the reference's
  PostScript route — same one-image-per-page idea, universally viewable.
  Pinned `imagewriter_pdf`. Detail → `DEV.md` § ImageWriter.

### [Network]

- ✅ **FujiNet relay (SP-over-SLIP)** — DONE (2026-08-10). `FujiNetCard`
  presents a SmartPort controller and forwards every call to a real
  FujiNet: a desktop build over loopback TCP 1985, or a **physical ESP32
  board over USB CDC-ACM**. One protocol carries every FujiNet function
  (block storage, the `N:` network device, clock, printer, modem, CP/M)
  because on the Apple II they are all SmartPort units. New host
  primitive `SerialPort` (POSIX termios / Win32 DCB) came with it.
  Pinned `slip_framer`, `serial_port`, `sp_over_slip_link`,
  `fujinet_card`. Detail → `DEV.md` § FujiNet, design →
  `docs/fujinet_plan.md`.
- ✅ **[FujiNet] Phase 2 printer tap** — DONE (2026-08-10). Bytes the guest
  WRITEs to the peer's printer unit are spooled to POM2's `ImageWriter`
  through the same `bytesWritten()`/`drainSpoolFrom()` contract as
  `PrinterCard`, ranked between the parallel cards and the SSC tap. The
  unit is identified by its DIB **name**, because the firmware's own
  `iwmPrinter::create_dib_reply_packet` labels the printer
  `SP_TYPE_BYTE_FUJINET_MODEM` — an upstream bug the test reproduces.
- ✅ **[FujiNet] Phase 3 helper process** — DONE (2026-08-10), by a
  different route than planned: POM2 launches and reaps an EXISTING FujiNet
  desktop binary (`ChildProcess`) instead of vendoring the firmware. See
  `docs/fujinet_plan.md` § 8 for why the vendored build was rejected.
- 🔵 **[FujiNet] built-in FujiNet, native in POM2 — DECIDED 2026-08-21.**
  Chosen architecture: a **native `FujiNetDevice` in POM2's own C++ covering
  the disk perimeter** (host slots, drive slots, a TNFS client, images served
  as SmartPort block devices), driven from the FujiNet panel, **coexisting**
  with the existing SP-over-SLIP relay for a real USB board or a full external
  firmware. The panel gets a source selector: *Built-in / USB board / External
  firmware* — the same "choose the level, not the catalog key" shape as the
  Abstraction Levels panel.
  Why this over the alternatives: hosting the vendor firmware (prebuilt or
  built from source) does **not** fix the CONFIG bug above, because the guest
  still reaches it through the relay's control plane; when POM2 *is* the
  device, that class of bug cannot exist. Building the firmware into POM2's
  build was re-costed and re-rejected (`docs/fujinet_plan.md` § 8).
  Out of scope for the native path, deliberately: the `N:` network device
  (HTTP/SSH/JSON), the modem and CP/M — those stay the relay's job.
  Phases: (1) TNFS client + tests; (2) the Fuji control device (host/drive
  slots) so CONFIG sees real state; (3) block serving of a mounted image;
  (4) the panel's source selector.
- ✅ **[FujiNet] CONFIG works from the Apple II** — DONE (2026-08-21). It
  never was a peer problem: **three POM2 bugs** stood between the guest and
  the FujiNet, all now fixed and detailed in `DEV.md` § FujiNet.
  (1) `SpOverSlipLink::control` sent the control list WITHOUT its mandatory
  2-byte length prefix, so a short list ran the peer's parser past the end of
  the packet and **aborted the FujiNet process** — that was every "the
  firmware keeps dying" symptom — while a long one had its first two bytes
  eaten, which is why CONFIG read empty host and drive slots.
  (2) The DIB name arrived malformed from upstream (`FUJINET_DISK_48` for
  `_0`) and is now repaired in the relayed status.
  (3) `kMaxUnits` was 8, so the enumeration stopped after the disk slots and
  never reached the Fuji device, `NETWORK`, the clock or the printer — and
  since POM2 answers the guest's "how many devices?" locally, the guest never
  probed past 8 either. Raised to 32.
  Verified: NETCAT now reports `NET DEV IS 11` and reaches
  `CONNECTED to N:HTTP://THEOLDNET.COM/`; the panel lists all 13 devices.
- 🟡 **[FujiNet] a `CONTROL` to the peer's PRINTER unit kills it** — measured
  2026-08-21, reproducible three runs out of three. The packet is
  byte-identical in shape to the ones units 10-12 answer normally
  (`04 03 0D 00 00 00 …`, an empty control list), yet the peer throws
  `std::length_error` out of `Request::from_packet` and aborts. Upstream bug
  in the printer device — the same unit whose DIB already advertises the
  modem's type byte. POM2 relays faithfully and now REPORTS the death
  (`peer LOST after N s — M call(s) served`), which is what localised it in a
  single run. Worth reporting upstream; POM2 has nothing to fix.
- ✅ **[FujiNet] built-in `N:` — DONE (2026-08-21).** `FujiNetNetDevice`
  serves the network device from POM2's own host sockets, so the Apple II
  browses the web through the FujiNet card even though the desktop firmware's
  own `N:` is inert (below) — and even with no peer attached, since it
  answers the DIB and is counted in the device count itself. Phase 2 of the
  native FujiNet decided above, arriving before the Fuji control device
  because it is what the machine actually needed. HTTP over plain TCP only.
  Pinned by `fujinet_net_device`; verified against theoldnet.com in the
  FujiNet Contiki browser.
- 🟡 **[FujiNet] the desktop firmware's `N:` device never opens a socket** —
  it answers the guest's open with success (`CONNECTED to
  N:HTTP://THEOLDNET.COM/` appears on the Apple II) and then no outbound TCP
  is ever created, watched live on the peer's own descriptors. NOT POM2: the
  same firmware, same machine, opens real TCP for TNFS. Its WiFi is a
  `DummyWiFiManager` and giving it an SSID does not help. A real FujiNet
  board over USB is the path for `N:`; the relay is unchanged for it.
- 🟡 **[FujiNet] the 250 ms relay timeout is sized for local media only** —
  measured 2026-08-21. `SpOverSlipLink::kDefaultTimeoutMs` is fine while the
  peer answers out of its own SPIFFS (booting `autorun.po` never approaches
  it), but every block read of a TNFS-hosted image crosses the internet and
  overruns it, so the guest gets `FN ERROR` instead of a boot. Workaround
  today is the per-slot `fujinet_timeout_ms_slot<N>` key (3000 works); it has
  no UI and nothing tells the user it is why their network disk will not
  boot. Options: raise the default, or measure the peer's round-trip at
  enumeration and size the timeout from it. *~2 h.* → `DEV.md` § FujiNet.
- 🟡 **[FujiNet] a network-backed SmartPort call freezes the emulator** —
  `transact()` blocks the CPU thread under `stateMutex` by design (see the
  threading note in `SpOverSlipLink.h`, and § 9 of the plan for why that was
  the right call). Invisible at 250 ms over loopback; very visible once the
  timeout is raised for a peer whose media lives on the internet — the UI and
  the AI control server both stall for the length of every read. Wants at
  least a "waiting on FujiNet" indication, and possibly a bounded pump of the
  UI while a call is outstanding.
- 🟢 **[FujiNet] `PR#n` before the peer attaches prints `FN ERROR`** — the
  autostart slot scan handles the no-peer case correctly (the card steps
  aside and the scan carries on to slot 6), but a manual `PR#n` in the same
  state just fails. The card could wait briefly for a peer, or say *why* it
  failed. Cosmetic, but it is the first thing anyone hits.
- 🟢 **[FujiNet] media bays + modem bridge — DECIDED AGAINST.** Surfacing
  the peer's block units as `MountableMediaCard` bays would add rows whose
  Mount/Eject cannot work (the images live on the FujiNet's own SD/TNFS
  storage, which POM2 has no path to write); the FujiNet panel's device
  table already answers "what has it got mounted". Bridging its modem unit
  into the SSC telnet path would fight the FujiNet's own stack, which
  already reaches the network — POM2 dialling out in parallel would break
  the connection state the guest thinks it owns.
- 🟡 **[FujiNet] //c-class support** — the card is II+ / //e only: a
  //c's forced INTCXROM masks all slot ROM. On real hardware the FujiNet
  *is* the SmartPort on the disk port, so the correct integration hangs
  the relay off the on-board `$C500` hole (`exposesIicOnboardRom`,
  see `project_iic_smartport_boot`) rather than a slot card. *~1-2 d.*
- 🟢 **[FujiNet] embedded firmware** — `fujinet-go-apple2-desktop` builds
  the FujiNet firmware as a shared library and `dlopen`s it, so the user
  needs no second program. Deliberately NOT done: it drags in mbedtls,
  expat, a pinned submodule and a patch set anchored to exact upstream
  text. Revisit only if "having to install a second program" turns out to
  be the real blocker.

- ✅ **Uthernet I/II Ethernet TCP/IP** — DONE (2026-07-28).
  `UthernetCard` + `Cs8900aDevice` (MAME `machine/cs8900a.cpp` +
  `bus/a2bus/uthernet.cpp`, line-cited) and `UthernetIICard` +
  `W5100Device` (AppleWin `source/Uthernet2.cpp` — MAME has no W5100).
  Host transport = `NetworkBackend` with Null / Loopback / **libslirp**
  (optional dep, user-mode NAT, no root). Key point: the **Uthernet II
  needs no backend** for TCP/UDP — its W5100 is a hardware stack POM2
  runs on host sockets, so period IRC / telnet / FTP works out of the
  box. Virtual DNS resolves off the CPU thread. Ethernet status panel.
  Pinned `uthernet_cs8900_smoke` + `uthernet2_w5100_smoke` (the latter
  runs a real TCP session). Detail → `DEV.md` § Uthernet.
- ✅ **Host sockets on Windows** — DONE (2026-08-01). `POM2_HAS_SOCKETS`
  was 0 on Windows, so the Uthernet II's TCP/UDP paths, the SSC telnet
  bridge and the AI control server were all compiled out of the Windows
  build. The POSIX-vs-Winsock difference now lives in one header,
  `src/SocketCompat.h`, and those three TUs are written against it.
  Verified by cross-compiling every `src/*.cpp` with
  `x86_64-w64-mingw32-g++`. Detail (including the five silent traps and
  why the readiness wait is `select`, not `WSAPoll`) → [DEV § Host
  sockets](DEV.md#host-sockets-posix--winsock).
- 🟡 **Uthernet I has no host transport on Windows** — libslirp is the
  only backend that moves raw frames, and CMake deliberately does not
  look for it on WIN32. vcpkg *does* carry a libslirp port (4.9.1), so
  the library is obtainable; what is missing is POM2's side —
  `SlirpNetworkBackend`'s poll loop is POSIX `poll()` over the fds
  libslirp returns, and porting it cannot be verified without a Windows
  libslirp build to test against. Note the vcpkg port pulls **glib**,
  which is a heavy addition to the Windows CI job. Uthernet II is
  unaffected (hardware TCP/IP on host sockets). *1-2 d + CI budget.*
- 🟡 **Uthernet II inbound (`LISTEN`)** — the W5100 `LISTEN` command is
  decoded but unimplemented: neither transport can route an inbound
  connection to the guest (libslirp is outbound-only without explicit
  port forwarding). Needs a user-configured host port to bind plus a
  slirp `hostfwd`-style mapping. *1 d.*
- 🟢 **Uthernet I on WASM** — the CS8900A model is browser-safe but has
  no transport there (no raw sockets, and libslirp isn't in the
  Emscripten build). A websocket-proxied backend would fix both cards'
  raw modes in the browser. *2-3 d.*

### [Input] joystick / paddles / mouse

- ✅ **Apple II square-gate stick** — DONE (2026-07-10).
  `JoystickInput::applySquareGate` expands the round modern-stick region to
  the full square so the corners (255/255) are reachable (Wings of Fury
  take-off); radial deadzone; toggle + persisted `joystick_square_gate`.
  Pinned `joystick_square_gate`. Detail → `DEV.md` § Joystick / `CHANGELOG.md`.
- ✅ **Kiosk gamepad disk selector** — DONE (2026-07-10). Start (or F1) opens
  a name-proximity-filtered picker of sibling disks; A mounts in-place, with
  Reset/Quit action rows. Detail → `DEV.md` § Host control (kiosk) /
  `CHANGELOG.md`.
- 🟡 **PADL(2)/PADL(3) host binding** — second stick centered at 127
  (`JoystickInput.cpp:65-75`).
- 🟡 **Mouse → paddles mapping** — paddle 0/1 on host mouse X/Y axes
  (alternative to pads).

### [Paint editor] HGR / GR / DHGR / DLGR (hgrpaint/ + hgrsprite/)

- ✅ **2026-07-12 batch — ALL 17 items DONE** (same day as planned): GR/DLGR
  screen-hole masking · HGR import scores LUT row 0 (pinned vs `renderHiRes`)
  · session persistence · canvas multi-pipeline (NTSC/Medium/4-bit/Chat
  Mauve) · 4:3 aspect · DHGR fringing overlay · 16-colour copy/paste +
  FlipH/V/Rot · MacPaint fill patterns · X/Y mirror symmetry · DHGR text ·
  onion skin · DHGR mono import · save-to-ProDOS (host-folder + `#TTAAAA`
  tags in `buildVolumeFromFolder`) · flipbook page 1↔2 + ghost · sprite
  editor port (`hgrsprite/`) · **DLGR mode** (aux nibble rotation pinned) ·
  **DHGR NTSC 8-px chroma import** (ii-pix palette, BSD-2). Detail →
  `DEV.md` § Paint editor, why → `CHANGELOG.md`.
- ✅ **Remaining niceties — DONE (2026-07-12, same evening)**: mono lo-res
  rendering (GR + DLGR nibbles display as their 14 MHz bit patterns through
  the phosphor, pinned in `dhgr_paint_model`); composite canvas pipelines
  (AppleWin NTSC + OE-CPU added to the editor's pipeline combo — the
  NTSC-8-px import previews faithfully); sprite editor DHGR target
  (stamp/grab/preview/ASM-export the shape as 16-colour fat pixels,
  aux+main pair tables).

### [UI/UX]

- ✅ **Desktop disk drag & drop** — DONE (2026-05-31). `glfwSetDropCallback`
  wired in `main.cpp`, routes the first recognized file via `insertAndBootImage`
  (auto-route Disk II / SmartPort 3.5" / ProDOS HDV) and reports the result in
  the status bar; unrecognized extensions are flagged. Detail → `CHANGELOG.md`.
  - ✅ **Autoboot hardened (2026-08-14)** — seven defects downstream of the
    callback: 3.5" drops now auto-plug a SmartPort card like HDV already did
    (`ensureSmartPortCardForBoot`); `bootFromSlot` returns `bool` so "booted"
    is never claimed for a silent cold-boot fallback (and a drop with no ROM
    is refused up front); `.2mg` is classified by **parsing its header**, not
    by guessing from the file size; an 800K `.dsk`/`.image` routes to 3.5";
    the Library's `acceptHdv` no longer hides `.hdv` files the drop boots;
    plus three write-back data-loss fixes and an LSS write-splice on
    `insertDisk`. Pinned by `cli_kiosk`. Detail → `CHANGELOG.md`.
- ✅ **Onboarding: Welcome / no-ROM panel** — DONE (2026-05-31).
  `renderWelcomePanelWindow`: no-ROM banner with probed dirs, expected ROM name
  for the active profile, "Reload ROM (re-probe)" button, plus quick-start;
  auto-opened on first launch without a ROM, also via `Help → Welcome / Quick
  Start`. Detail → `CHANGELOG.md`.
  - 🟢 Remaining: deeper guided tutorials.
- ✅ **UI density / discoverability** — DONE (2026-05-31). `Devices` menu
  grouped under `SeparatorText` headers via the `devItem` helper that adds a
  tooltip to every entry; `F6` shown on Rewind, ~25 tooltips added. Detail →
  `CHANGELOG.md`.
  - 🟢 Remaining: airier default layout (dedicated item below).
- 🟢 **MicroM8-style Rewind** — continuous state recording +
  scrub/step-back/rewind-live. **Phases 0→5 done** (2026-05-31,
  `CHANGELOG.md`): memory backend `SnapshotIO`, shared `MachineSnapshot`,
  `RewindBuffer` (keyframes + XOR deltas, memory budget), frame-boundary
  capture (`workerLoop` + `tickFrame` WASM), parked-worker transport +
  `Rewind_ImGui` UI (timeline / transport / `F6` rewind-live), `DiskIICard`
  drive state via `SlotPeripheral::*SnapshotState`, audio flush on restore.
  Pinned `snapshot_memory_roundtrip`, `rewind_roundtrip`, `rewind_delta`,
  `rewind_transport`, `rewind_slot_state`, `rewind_audio_state`
  (Mockingboard/Phasor VIA+AY+SSI263 → music **and** speech survive the rewind),
  `rewind_disk_write` (DiskIICard snapshot v2 = nibble track buffers → disk
  writes are undone on rewind). **Remaining**: writes to a writable WOZ not
  undone (`wozRaw` is a separate store; WOZ originals usually write-protected);
  "redo" (replay an undone future) not implemented. Detail → `DEV.md`
  § Rewind / time-travel.
- 🟡 **MicroM8-style 3D voxel view ("Voxel Cube")** — screen **stood up**
  (monitor, XY plane) as a 4:3 slab of **uniform-depth** cubes + per-color "pop"
  relief, orbital camera. NB: the initial luminance extrusion gave flat
  stalactites — fixed after scraping MicroM8 (cf. `CHANGELOG.md` + `DEV.md` § 3D
  voxel view). **Phases 0→3 done** (2026-05-31, `CHANGELOG.md`): `Mat4.h`
  (Vec3+Mat4+OrbitCamera, pinned `voxel3d_math`), `Voxel3DRenderer` (instanced
  cubes, FBO+depth, per-vertex color texture-fetch, derivative shading,
  **anti-moiré supersampling** + contiguous `cubeFill=1`), **native resolution**
  (1 voxel/pixel, 280|560×192), tap **before** `CrtEffectStack` (independent of
  CRT effects), *(P2)* left-drag orbit + middle-button **pan** + wheel zoom,
  *(P3)* View ▸ "3D voxel settings…" panel (depth/pop/fill/AA/ambient/mono/
  per-colour, persisted `voxel_*`), *(P4)* **WASM perf guard** (`ss≤2`+FBO≤2048²+
  `gridW≤280` under Emscripten), *(bonus)* **Mono mode** + **depth by color
  index** (snap lo-res palette `kVoxelPalette`). **WASM build OK** (+ browser
  wheel fix: `emscripten_set_wheel_callback` → `io.MouseWheel`, cf. `main.cpp`).
  **Remaining**: *(P5, deferred on request)* rewind tie-in "freeze + orbit a
  rewound frame" — already works for free (the view samples the live framebuffer
  that rewind restore updates), so doc + polish rather than plumbing; *(option)*
  alternative heightfield-mesh mode. Detail → `DEV.md` § 3D voxel view. *P5≈0.5 d.*
- 🟢 **Airier default layout** — ImGui Docking or
  `SetNextWindowPos` adaptive cascade.
- 🟢 **`isDuplicate` flags cffa/smartport35 duplicates** in the Slot
  Config assignment column — cosmetic.
- 🟢 **On-screen touchscreen / virtual joystick** — ImGui virtual
  joystick for mobile WASM builds (separate from raw touch routing). Two
  thumb-sticks + Open/Solid Apple buttons. Inspired by microM8 / A2TS.
  *2 d.*

### [WASM]

- 🟡 **IDBFS settings persistence** — `/persistent` mounted via IDBFS
  (`CMakeLists.txt:241`) but `Settings.cpp` writes to `$HOME`;
  `state.cfg` + `imgui.ini` do not survive a reload. Route via
  `ResourcePaths` under `__EMSCRIPTEN__`. *2-4 h.* ⭐ quick win
- 🟡 **File picker / drop-zone disks** — build-time bundling
  only. HTML5 drop-zone → `FS.writeFile('/uploads/…')` →
  `DiskIICard::insert`. *~1 d.*
- 🟢 **Mobile touch input** — GLFW3 under Emscripten does not map
  touch → mouse off-canvas. JS wrapper `touchstart/move/end` →
  `Module._inject_mouse_*`.
- 🟢 **Audio worklet tuning** — miniaudio Web Audio works but
  latency ~150 ms is audible on speaker click. Explore a custom
  `AudioWorkletNode` or shrink the buffer.
- 🟡 **"Zero-friction" web demo — the licensing call** *(commercial audit
  2026-05-31; premises re-checked 2026-08-19)* — the technical side is done:
  `roms/` is baked into `POM2.data` (`CMakeLists.txt:489-491`), `disks_3.5`
  can be bundled (`POM2_WASM_BUNDLE_DISKS`, `:539`) and `wasm/shell.html`
  auto-boots Total Replay from the bundled `floppyemu/`. What remains is
  **licensing**: shipping Apple ROM dumps + non-free media in a public web
  demo vs sourcing royalty-free replacements. A marketing prerequisite
  before pushing to r/apple2 + Hacker News; aim in parallel for a
  **stable 1.0** (the //c+/IWM 3.5" boot stays owned-out-of-scope — see
  [Storage](#storage-disks--images) above). *decision + media sourcing.*

### [Media] formats

- ✅ **3.5" WOZ images mount** — DONE (2026-08-18). The Sony 800K GCR read
  path moved to `src/Sony35Gcr.{h,cpp}`, shared by `Sony35Drive` (guest
  writes) and `Disk35Image::loadWoz` (file loads), so a `.woz` is decoded to
  blocks once at mount. Read-only: POM2 has no GCR encoder, so handing blocks
  back to flux is not something it can honour. Verified byte-identical (1 600
  /1 600 blocks) against an independent conversion of
  `disks_3.5/The New Print Shop 800K.woz`; pinned by `woz35_load`.
  **Remaining**: write-back to `.woz` needs the encoder (`buildTrackBits` is
  right there in `Sony35Drive`, still file-local) plus a WOZ writer that
  preserves the container — a separate job, and one nobody has asked for.

### [Arch] refactor & tooling

- 🟢 **Z80/SoftCard cleanup backlog** (2026-07-12 bug-hunt survivors — quality,
  not correctness): SoftCardZ80 SFZ2 blob → `pom2::byteio` putU16/Reader like
  every other card (3 hand-synced layout copies today);
  `softcard_cpm_boot_test` → `pom2::findResource` + `Apple2Display::
  textRowAddress` instead of private copies (6th in-repo transcription of the
  text-row interleave); Z80.cpp decoder dedup: rp-selector switch pasted 8×
  (readRP/writeRP helpers), JR cc's inline condition test vs `ccTest`,
  `memEA` body re-inlined twice for special timings (chargeless
  `indexedEA()` split); `xlate` 5-compare chain → 16-entry per-4K-page
  offset LUT; drop the per-instruction `mem_` null tests in the dmaRun hot
  loop (guard once at entry). Boot test could also pump a public
  controller slice hook instead of re-implementing arbitration.
- ✅ **CI GitHub Actions** — DONE (`.github/workflows/ci.yml`). Two jobs on
  push-to-`main` / PR / manual dispatch, with in-flight cancellation: **linux**
  builds the full tree (GUI + `pom2_headless` + tests, `POM2_ENABLE_TESTS=ON`)
  and runs the 182-test ctest gate (Klaus 6502+65C02, Tom Harte curated,
  `cpu_cycle_count`, golden-hash display, boot traces); **wasm** is an Emscripten
  verification build (`build_wasm.sh`) asserting `wasm/POM2.{js,wasm}` +
  `index.html` are produced. Both jobs shallow-clone Dear ImGui (gitignored); no
  test depends on the user-supplied ROMs.
- 🟠 **ThreadSanitizer pass over `EmulationController` / `stateMutex` / the
  audio thread** *(2026-08-02 bug-hunt follow-up)* — architect **P1**
  ([Architect attack order](#architect-attack-order)). The highest-yield gap we
  know of. That sweep's ASan+UBSan build (156 test binaries, ~24 000
  hostile-input cases, ~6 M random instructions) returned **zero**
  diagnostics, yet code reading found a UI deadlock, two use-after-frees and
  three unlocked cross-thread reads in the same tree. ASan cannot see data
  races and the headless tests cannot reach the GUI, which is exactly where
  the defects were. Needs a TSan build driving the GUI with the AI server
  polling `/screen.ppm`, slot reconfiguration, and rewind under load. Would
  also retire the two findings that could not be pinned (`saveScreenshot`'s
  `demodMutex` ordering, and the threaded half of `disk_path_snapshot`).
  - **The controller half is done and clean** (2026-08-17, bug hunt 8): a TSan
    harness drove the real thread shape without a GUI — CPU worker, a UI thread
    running the transport verbs (rewind scrub/seek/resume, cassette, 3.5"
    mount/eject, speed, mode toggles, a `lockState()` read per frame), an
    AI-server thread doing `lockState()` reads + snapshot capture/restore + key
    injection through `kbMutex`, the live miniaudio callback, and a Mockingboard
    in slot 4 fed by a guest loop so the emuCycles AY queue (the one real
    CPU↔audio producer/consumer) is exercised. Zero races. **Caveat worth
    keeping**: TSan instruments every load/store in the interpreter's hot loop,
    so the CPU manages only ~400-1 400 emulated cycles/s — the *lock protocol*
    is covered thoroughly, *emulated execution* thinly. What remains is the GUI
    half: ImGui panels, `demodMutex`, slot re-plug under load.
- 🟡 **Consolidate the atomic file-write helper** *(2026-08-02)* — three
  divergent copies still: `DiskImage.cpp`'s `writeFileAtomic` (anonymous
  namespace), `Disk35Image.cpp:264-310` (added 2026-08-02) and
  `ProDOSVolume.cpp:667-710` — the temp-file naming, the permission carry-over
  and the error strings are hand-repeated in each. `DiskImage`'s copy caught
  up on permission preservation 2026-08-08 (it was silently resetting the
  image's mode to the umask default on every write-back); `ProDOSVolume`'s
  still hasn't. The home for the extracted helper is the header the three
  already share for the COMMIT step — `AtomicFileReplace.h`, next to
  `pom2::replaceFileAtomic` — not a new file. Architect **P4**.
  - ✅ **The durability half is closed** (2026-08-14): the `fsync` went into
    the COMMIT step they already share, `pom2::replaceFileAtomic`
    (`AtomicFileReplace.h`) — data flushed before the rename, parent
    directory flushed after it, best-effort — so a power cut can no longer
    land an empty file where the user's disk image was, on any of the ten
    call sites rather than the three this item names. Pinned
    `atomic_file_replace`. What remains here is duplication, not data loss.
- 🟡 **`hgrpaint/` has no headless harness** *(2026-08-14)* — the editor's
  state (mode flags, shadow buffer, tools) is private and only reachable
  through `render()`, i.e. through an ImGui frame, so nothing in `ctest`
  can exercise it: `dhgr_paint_model` pins the free functions in
  `HgrPaintModel.h`, not `HgrPaintEditor`. That is how three
  out-of-bounds accesses on the DLGR shadow survived from the DLGR page's
  arrival (2026-07-12) to 2026-08-14 with a green suite. Cheapest fix: a
  test-only seam (a friend fixture, or a small `EditorTestAccess` struct)
  driving the tools against a stub `IHgrPaintHost` — bearing in mind
  `hgrpaint/` is shared verbatim with POM1, so the seam must be additive.
  *~1 d.*
  - **`hgrsprite/` had the same shape and cost the same kind of bug**
    (2026-08-17, bug hunt 8): no test at all, and its ca65 DHGR export read
    32 bytes past the pair buffer at the UI maxima. The byte layer is now
    pinned by `hgr_sprite_blit` — including the two helpers the export's
    clipping moved into (`dhgrExportRowBytes`, `extractDhgrPlanes`) — but
    `HgrSpriteEditor` itself is still only reachable through an ImGui frame,
    exactly like `HgrPaintEditor`. One seam would serve both.
- ✅ **Run the `SnapshotIO` fuzzer** — DONE (2026-08-17, bug hunt 8 round 2).
  It had been built during the 2026-08-02 ASan sweep and never executed, which
  left snapshots as the one untrusted-input surface with no dynamic coverage
  (the disk-image and WOZ parsers had 4 200 + 13 270 mutations behind them).
  **40 000 mutated blobs** — bit flips, truncation, extension, corrupted
  section lengths and names, duplicated sections, absurd lengths, random tails
  behind a valid magic — through `restoreMachineState` under ASan+UBSan: 6 915
  accepted (each then RUN, so a crafted paging state has to survive execution),
  33 085 rejected, no diagnostic. It also pins the property `MachineSnapshot`
  states in prose — a rejected file is observationally transactional to the
  machine — by re-capturing after every rejection: **0 violations**.
  - Two more surfaces got the same treatment in that round and were also
    clean: the **AI control server** (6 000 hostile HTTP requests, plus a
    `GET /status` liveness probe so a wedged worker thread would show) and the
    **cassette loader** (12 000 mutated `.wav`/`.aci` tapes, 2 493 of them
    loaded and then played/seeked/re-saved). The **printer** control-stream
    fuzzer from the same round is the one that found a defect — see CHANGELOG.
  - Not committed, matching the precedent from hunt 5: the harnesses link
    against source files rather than a build-system target, which is a CI
    decision rather than a bug fix.
- 🟡 **`MainWindow.cpp` god-object (11 154 lines)** *(audit 2026-05-31 at
  10 669; 11 495 before the panel work, 11 154 after — the first fall since
  the rule was written)* — architect **P0**
  ([Architect attack order](#architect-attack-order)). **The lesson of
  2026-08-23: the file split was never the fix.** `_Slots` / `_MemoryMaps` /
  `_ImGui` moved code without removing coupling — what hurt was that one panel
  was described in six unlinked places, plus a `bool showXxx` member, plus a
  hand-ordered render call. They had drifted: seven panels never persisted, the
  WASM chrome-light block missed every panel added after it was written, and
  one menu row wore another's tooltip. `PanelCatalog.h` + `PanelRegistry` +
  `MainWindow_Panels.cpp` made all of that one table: the 38 members are gone
  (`show(PanelId::X)`), the 43 render calls are one loop, and a forgotten
  catalog row now fails the build rather than the UI.
  **Left**: move the panel *bodies* into their own translation units. That is
  now a move rather than a rewrite — nothing outside a body refers to it — and
  it is the ordinary half of the work. *1-2 d.*
  → [DEV](DEV.md#panel-registry-panelcataloghpanelregistry-mainwindow_panelscpp)
- 🟡 **Scattered config** — `POM2_*` env vars + CLI flags + `Settings`
  to centralize into a `Config` (env → CLI → Settings → defaults),
  list env vars in `--help`. *1 d.* Architect **P4**.
- 🟡 **`stateMutex` shared CPU+UI** (`EmulationController.h:229`) —
  `MainWindow_Slots` takes this lock during plug/unplug, audio jitter
  risk. Partition long-term. Architect **P1** grain (GUI TSan first).
- ✅ **Debugger runtime glue (BP / watch / step)** — architect **P2**,
  DONE 2026-08-22 (breakpoints, step, step-over, run-to-cursor) and
  2026-08-23 (**write** watchpoints, free: the address is diverted off
  `memWrite`'s fast path instead of the fast path testing for it).
  `Debugger.h/.cpp`, `Debugger_ImGui.*`, `MemoryWatchSink.h`, pinned by
  `debugger`. Read watchpoints (2026-08-23) divert every read while one is
  armed, through a flag folded into the fast path's existing tests —
  measured free un-armed ([PERFORMANCE § 8.2-8.5](docs/PERFORMANCE.md)).
- ✅ **`POM2_IWM_AUTHORITATIVE` / IWM vs Disk II shadow — settled
  2026-08-23.** Not dual product behaviour: the default is IWM-authoritative
  (`iwmAuthoritative_ = true`; 5.25" data still comes from DiskIICard's LSS
  because the walker mis-framed RWTS), and full-shadow mode is reachable ONLY
  by the env var `POM2_IWM_AUTHORITATIVE=0`, a debug A/B toggle with no UI or
  settings surface. Documented as debug, so it is not a scaffold in limbo.
  (The old name `POM2_IWM_LEGACY_DATA_PATH` in this list was stale — the var
  is `POM2_IWM_AUTHORITATIVE`, read once in `EmulationController`.)
- 🟡 **CI `ctest -L rom` + ROM Status « degraded »** — architect **P3**.
  Tests SKIP when a dump is absent, so the L0 path can rot behind a
  green suite. ROM Status reports missing, not « running the synthetic
  fallback ». Detail → [`docs/lle_vs_hle.md`](docs/lle_vs_hle.md)
  § Keeping a level once you have it.
- 🟡 **Inconsistent `pom2::` namespace** — 163/233 top-level files,
  `tests/` does not use it. Mechanical migration. Architect **P4**.
- 🟢 **Legacy M6502 style** — FR/EN comments, C-style casts,
  `void(void)`. Targeted `clang-format` + `clang-tidy modernize-*`.
- 🟢 **`*Card` raw pointers in MainWindow** (`MainWindow.h:320-358`) —
  no notification when SlotBus replugs. Observer pattern or
  `controller.slotBus().peripheral(N)`.

## Edge-case test corpus

Backlog of **manual / integration tests** with real software that tortures the
corners (cycle-exact CPU↔video sync, protected WOZ flux, VIA IRQ) — beyond the
unit `ctest`s. Curated list + POM2 status + cross-refs to the dashboard's
`Known gaps`: **[`docs/test_corpus.md`](docs/test_corpus.md)**.

- 🟠 **[DIX](https://github.com/Fr3nchT0uch/DIX/) — French Touch demo
  anthology**. **Priority reference** for emulation perfection: chains vapor
  lock, mid-scanline, Mockingboard, 128 KB aux, Unidisk/Liron. Validate DIX
  first before any other corpus title. Full description → `docs/test_corpus.md`.
  - ⚠️ **French Touch demos expect the Mockingboard in slot 4** — MAD EFFECT
    (`disks_5.4/demo/madef/Sources/main.a:176-218`) and the DIX productions
    address the 6522 in hard-coded `$C4xx` with no slot scan; the whole
    frame sync is the T1 IRQ. With the card anywhere else (or a Mouse Card in
    slot 4) the demo arms a timer that never fires and waits forever — a
    frozen screen after the loader, no code regression. Diagnosed 2026-08-20
    from a `slot_4_card=mouse` / `slot_7_card=mockingboard` config; the
    `madef_phase_probe` shows 0 page-flips/frame in slot 7 vs ~191 in slot
    4. Slot 3 is no alternative: the //e's internal 80-column firmware owns
    `$C300-$C3FF` (SLOTC3ROM off) so a Mockingboard there is silent. Open
    idea: a Slot Config hint ("DIX / French Touch → MB in slot 4") next to
    the existing slot-3 warnings in `MainWindow_Slots.cpp`.
- ✅ **Vapor lock** — DONE/proven (2026-06-09, extended 2026-06-10). Test
  `vapor_lock`: a real 6502 `LDA $C058 / CMP marker / BNE` loop locks on the
  marker in video RAM; `floatingBus()` tracks the beam per cycle. Scanner
  geometry is now PAL-aware (262/312 lines per `VideoStandard`); sub-instruction
  precision corrected (`$C0xx` read sampled at the access cycle); all
  non-driven `$C0xx` reads return the bus (`$C040`, `$C050-$C057`,
  `$C030-$C03F`). *(🟢 Remaining: non-last-cycle RMW-type accesses — outside
  vapor lock.)*
- ✅ **Mid-scanline video switch** (French Touch *Mad Effect*/*Plasmagical*,
  included in DIX) — DONE (intra-line per-byte-column rendering, RGBA + composite
  + 560-wide; cf. [Display]). Beam-racing vs double-buffer distinction: a
  unidirectional page flip = buffer (full-frame render, DROL anti-flicker),
  bidirectional = exact beam-racing. → `Gap #3` (residual: exact transition cycle
  at character-clock, 40/80-col mixed on same line).
- 🟡 **Spiradisc / RWTS18** (*Captain Goodnight*, *Prince of Persia*) — spiral
  tracking + weak bits to validate on real WOZ images. → `Gap #9/#10`.

## Deliberate skips (documented inline)

Conscious MAME divergences, justified in the code at the relevant spot.
Do not re-litigate without re-reading the original comment.

- 🟢 **`$C040` STRB not gated `!//c`** (MAME `apple2e.cpp:1927`) —
  no sink wired.
- 🟢 **ClockCard DATA_OUT live** vs MAME latch on CLK edge in
  MODE_SHIFT (`ClockCard.cpp:193-200`) — strict would break stock
  ProDOS.
- 🟢 **MouseCard PIA out_a/b without `scheduler.synchronize`** (MAME
  `mouse.cpp:280-294`) — no firmware-visible race.
- 🟢 **ClockCard offset model vs MAME `set_time`** — behaviorally
  equivalent as long as `timeFn()` is lock-step.
- 🔁 **MAME path drift refresher** — re-check ~every 6 months to
  track upstream renames (recent: `wozfdc.cpp`
  `bus/a2bus → machine`).

## Out of scope

Things we will not do unless explicitly requested + clear ROI.

- **Apple IIgs / ProDOS 16** — lives in the separate **pom2gs** project
  (Mega II + FPI + GLU + Ensoniq DOC); never in POM2.
- **Apple ///** + SOS — niche, *20-40 d*.
- **Clones** Franklin / Laser / Pravetz / Basis 108 — *2-5 d/clone*,
  low demand.
- **CFFA CompactFlash** — HDV + host folder suffices; MAME-faithful
  port already covered by P1 (CFFA done), P2/P3 above.

## Changelog

See [`CHANGELOG.md`](CHANGELOG.md).
