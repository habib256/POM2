# POM2 — TODO

Status as of 2026-08-26 (v0.8.5). Resolved items → `CHANGELOG.md`. MAME refs →
`DEV.md`. This file lists **open work only**: an item that ships is deleted
here and its "why" is written up in `CHANGELOG.md`.

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
| 2  | Memory + IIe + RamWorks        | Partial-verbatim | `apple2e.cpp:1275-1299`, `a2eramworks3.cpp:108-115`                      | 🟢 Keyboard + PaddleInputs extracted (2026-08-23); `memRead` 256-entry dispatch (perf) still in Memory |
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

## Architect attack order

Sequencing constraint from a 2026-08-19 architecture pass — **not a second
backlog**. Feature items stay under [Backlog](#backlog); this list says what
to do **next when choosing among them**, and what not to pick instead.
Cross-links point at the detailed items. Priorities that have landed are
deleted rather than struck through — `CHANGELOG.md` is the audit trail.

Standing rule while P0 is open: **do not grow the god-objects.** A new card
gets its panel in its own `*_ImGui.cpp` and **zero** business logic in
`MainWindow.cpp`.

| Pri | Item | Status | Detail |
| --- | ---- | ------ | ------ |
| **P0** | Stop growing the god-objects, and **decompose** rather than relocate: the split into `MainWindow_<Area>.cpp` moves rows between files and leaves the coupling intact. Target &lt; 3000 lines/file, POM1 `MainWindow_*` discipline. | 🟢 **the UI half is done 2026-08-23.** `MainWindow.cpp` went 5 590 → 6 622 (the audit that set the target) → 11 511 → **11 154** — the first fall since the rule was written, and it came from removing the SIX parallel per-panel lists (settings load 32, save 32, palette 38, palette dispatch 38, menu rows 37, WASM chrome-light 28), then the 38 `bool showXxx` members, then the ~43 `renderXxxWindow()` calls. One catalog + one registry; adding a panel is a row and a `draw` line. `tools/check_file_sizes.sh` ratchets the ceiling, which now only falls. **Body extraction 2026-08-23**, all verbatim moves into `MainWindow_<Area>.cpp`: audio (mixer, Le Chat Mauve, Mockingboard, Phasor, Echo+), non-audio devices (Ethernet, SSC, printer, ImageWriter, AI server), settings/input (No-Slot Clock, voxel, NTSC, joystick, mouse) and misc (memory viewer, cassette, HGR paint + sprite, rewind) — **11 154 → 8 913 lines** (−2 241), ratchet lowered each step. **Left**: (a) the storage panels (Disk, Library, SmartPort, FujiNet, FloppyEmu, HDV) — they reference the `kProDOSHostSentinel` / `freePoNameFor` anonymous-namespace helpers, which must move with them; (b) the keyboard and welcome panels, which stay put deliberately — they load a texture via the `STB_IMAGE_STATIC` instance defined in `MainWindow.cpp`, whose symbols are internal to that TU; (c) `renderScreenWindow` (the main framebuffer, tightly coupled to the GL upload). | [Arch](#arch-refactor--tooling) `MainWindow.cpp` god-object; [DEV § Panel registry](DEV.md#panel-registry-panelcataloghpanelregistry-mainwindow_panelscpp) |
| **P1** | TSan on the **GUI** half + remaining mutex grain. ASan cannot see UI races; audio jitter under disk-turbo is a product bug, not a micro-opt. Mockingboard SPSC handoff next **if** a profile still shows the per-instruction card mutex. | 🟡 open, but no longer unattended: a **nightly ASan+UBSan / TSan matrix** runs in `ci.yml` as of 2026-08-22 (`POM2_SANITIZE` had been a CMake option CI never used, so the "controller TSan clean 2026-08-17" result had nothing keeping it true). **The UI↔worker input contention now has a targeted pass (2026-08-23)**: `ui_worker_contention` drives the real worker while a second thread hammers it with MainWindow's exact input disciplines — paddles/buttons under `lockState()`, `queueKey` via `kbMutex`, Open/Solid-Apple via atomics — and the worker's loop reads `$C000`/`$C061`/`$C064` concurrently. TSan-clean locally and added to the nightly TSan matrix, so a future access that escapes those disciplines turns the leg red. **Still open**: `demodMutex` and slot re-plug under load (both need their own driver), and MainWindow itself cannot run under TSan headless (GLFW/GL). OE-CPU demod **already** runs after `stateMutex` release (2026-07-12). | [Arch](#arch-refactor--tooling) TSan; [Audio](#audio) mutex contention; [Display](#display-hgr--dhgr--80-col) demod |
| **P3** | CI `ctest -L rom` + ROM Status **degraded** (running the synthetic fallback is not « missing »). Otherwise the L0 path rots behind a green suite that SKIPs when dumps are absent. | 🟡 open |
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
  extraction (done). Perf job, orthogonal to the `Keyboard`/`PaddleInputs`
  split that already shipped — do not merge the two.
- 🟢 **Dedicated Pascal LC** — 16 KB variant shipped with Apple Pascal,
  minor differences vs IIe LC (write-protect DIP). *1 d.*

### [Display] HGR / DHGR / 80-col

- 🟢 **Golden coverage gaps** (from the 2026-07-12 audit; mostly closed
  2026-07-12 wave 4, table 112 → 164 pins — flash-on phase, PAGE2/80STORE,
  rev-0 HGR+AN3, IIe 80COL+HIRES+MIXED without DHGR, Chat Mauve sub-modes
  all hash-frozen: `iie/text40flash`, `text40page2`, `hgrpage2`,
  `hgr80store2`, `hgran3`, `hgr80colmix`, `textcolorcm`). Remaining:
  ALTCHAR/mousetext + char-ROM glyphs (need a user ROM), PAL beam-raced
  splits (stay behavioural). Also: OE-GPU uploads the unused ~430 KB
  fallback framebuffer every frame (minor perf).
- 🟢 **Pure-analog signal-level composite pipeline** *(deferred, academic)* —
  IIR on the signal itself before demod, against today's 1-bit signal + FIR.
  *5-10 d.* Feature, not architecture — do not pick ahead of P0–P4.
- 🟢 **Beam-racing residuals** — `signalPhaseOffset_` stays a per-frame
  constant, so a mid-frame HGR↔DHGR split is approximated; lo-res clips at
  block-row (4 lines), like the RGBA path.
- 🟢 **Mid-scanline split residuals** — 40-col (280) + 80-col (560) mixed on
  the same line is undefined (separate `frame`/`frame80` buffers, scoped
  out); the exact transition cycle at character-clock is a later
  refinement. **Back-port to POM1** next (gated: LORES+TEXT rendering on
  GEN2 — HGR-only today — + HBLANK flag Phase 2 per Bernie's spec).
- 🟢 **PAL residuals** — device generator clocks (AY/IWM/SSI263) stay at the
  NTSC nominal (0.7 % delta = inaudible pitch, deliberately not retimed;
  speaker + cassette realtime audio ARE retimed, their queues starve
  audibly otherwise); WASM pacing (RAF 60 Hz) not yet switched to 50 Hz;
  manual NTSC/PAL toggle + auto-PAL when a Chat Mauve card is plugged (the
  two PAL profiles already cover the use case).
- 🟢 **Unidirectional mid-frame page split renders full-page** — assumed
  limit inherited from the DROL page-flip fix; the true remedy is
  incremental per-scanline rendering, MAME-style. → `CHANGELOG.md`.
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
  - **F2** cosine scanline → OE's **sin²** (keep the `scanAA` anti-moiré term).
  - **F7** HGR mono: 280 px average / 3 levels → **560 binary** (copy of the
    DHGR-mono loop already shipped).
  - **F6** row-dim mask ×0.7 ⚠ (make luminance-neutral, don't drop hard).
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

- 🟢 **`decodeTrack` trusts the address field** *(management audit
  2026-08-08)* — the write-back decoder reads vol/track/sector/checksum
  as 4-and-4 but validates none of them: the checksum is discarded, and
  the address field's TRACK number is ignored in favour of the buffer
  index. A guest that rewrites a whole track with a different track
  number in its address fields (sector editors, Locksmith-style
  copiers) therefore lands its sectors at the wrong file offset. `$D5`
  is not a legal GCR data byte so a spurious prologue match can't
  happen, which is why this has never bitten in practice. *~1 h.*
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
- 🟡 **EchoPlusTMS5220Card (real Echo+)** — catalog scaffold
  `echoplus_tms`: SlotPeripheral + stub register decode at
  $Cs00-$Cs0F, enough for detection (`EchoPlusTMS5220Card.h:15-17`).
  Remaining: TMS5220 LPC10 decoder (chirp ROM + K-parameter
  interpolation) and AY-3-8913 audio synth — the shared AY core it
  needs already exists (`src/AyPsgSynth.h`, extracted 2026-08-01,
  see [Audio]). *~3-5 d.*
  Architect **P3**: hide from the catalog until the chip exists, or ship
  it — a detect-only stub is the wrong third option.
- 🟢 **SSC IRQ gate SW2:6 DIP** not implemented (MAME `a2ssc.cpp:373`).
- 🟢 **Nothing asserts the real ClockCard ROM path is taken** when the dump
  is present — `clock_card_smoke` tolerates its absence so CI stays
  ROM-free, so a regression that silently routed back to the synthetic ROM
  would fail nothing. Same silent-degradation hole as every other
  ROM-driven L path (Disk II P6, mouse MCU, Grappler EPROM); →
  [`docs/lle_vs_hle.md`](docs/lle_vs_hle.md) § Keeping a level once you
  have it. The DOS 3.3 / Applesoft tools that pull the driver from
  `$C800` are still untested.
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

### [Network]

- 🔵 **[FujiNet] built-in FujiNet, native in POM2 — DECIDED 2026-08-21.**
  Chosen architecture: a **native `FujiNetDevice` in POM2's own C++ covering
  the disk perimeter** (host slots, drive slots, a TNFS client, images served
  as SmartPort block devices), driven from the FujiNet panel, **coexisting**
  with the existing SP-over-SLIP relay for a real USB board or a full external
  firmware. The panel gets a source selector: *Built-in / USB board / External
  firmware* — the same "choose the level, not the catalog key" shape as the
  Abstraction Levels panel.
  Why this over the alternatives: hosting the vendor firmware (prebuilt or
  built from source) does **not** remove the class of bug that the CONFIG
  breakage belonged to (fixed 2026-08-21, → `CHANGELOG.md`), because the guest
  still reaches the device through the relay's control plane; when POM2 *is*
  the device, that class of bug cannot exist. Building the firmware into POM2's
  build was re-costed and re-rejected (`docs/fujinet_plan.md` § 8).
  Out of scope for the native path, deliberately: the `N:` network device
  (HTTP/SSH/JSON), the modem and CP/M — those stay the relay's job.
  Phases: (1) TNFS client + tests; (2) the Fuji control device (host/drive
  slots) so CONFIG sees real state; (3) block serving of a mounted image;
  (4) the panel's source selector.
- 🟡 **[FujiNet] a `CONTROL` to the peer's PRINTER unit kills it** — measured
  2026-08-21, reproducible three runs out of three. The packet is
  byte-identical in shape to the ones units 10-12 answer normally
  (`04 03 0D 00 00 00 …`, an empty control list), yet the peer throws
  `std::length_error` out of `Request::from_packet` and aborts. Upstream bug
  in the printer device — the same unit whose DIB already advertises the
  modem's type byte. POM2 relays faithfully and now REPORTS the death
  (`peer LOST after N s — M call(s) served`), which is what localised it in a
  single run. Worth reporting upstream; POM2 has nothing to fix.
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

- 🟡 **PADL(2)/PADL(3) host binding** — second stick centered at 127
  (`JoystickInput.cpp:65-75`).
- 🟡 **Mouse → paddles mapping** — paddle 0/1 on host mouse X/Y axes
  (alternative to pads).

### [UI/UX]

- 🟢 **Deeper guided tutorials** — the Welcome / no-ROM panel covers a first
  launch without a ROM; step-by-step tutorials do not exist.
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
- 🟡 **`MainWindow.cpp` god-object (8 912 lines)** *(audit 2026-05-31 at
  10 669; 11 495 before the panel work, 11 154 after the registry, 8 912 after
  the first body extractions — the ceiling in `tools/file_size_budget.txt`
  only falls)* — architect **P0**
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
  **Left**: the storage panels (Disk, Library, SmartPort, FujiNet, FloppyEmu,
  HDV — they reference the `kProDOSHostSentinel` / `freePoNameFor`
  anonymous-namespace helpers, which must move with them); the keyboard and
  welcome panels, which stay put deliberately (they load a texture through the
  `STB_IMAGE_STATIC` instance defined in `MainWindow.cpp`); and
  `renderScreenWindow`, tightly coupled to the GL upload. Target is still
  &lt; 3 000 lines/file. *1-2 d.*
  → [DEV](DEV.md#panel-registry-panelcataloghpanelregistry-mainwindow_panelscpp)
- 🟠 **Wire the remaining coordinators into `MainWindow`** *(2026-08-26 merge;
  in progress 2026-08-27)* — **done**: `MouseCoordinator`,
  `PrinterCoordinator`, `AudioCoordinator`, `DevicePanelCoordinator`. 8 of the
  18 card aliases gone (mouse ×2, printer ×2, audio ×4, Chat Mauve, Clock,
  Uthernet ×2), and three real defects fixed on the way: a mixer that hid a
  coexisting second Mockingboard (and let it inherit the other's persisted
  level), an ImageWriter "not feeding" list that never named the FujiNet unit
  so a FujiNet-only setup showed an unconnected printer, and a spool read with
  no lock under a comment claiming the opposite.

  **`StorageCoordinator` is wired** (2026-08-27, nine commits). All five
  aliases gone; topology, media commands, two-phase construct-then-restore,
  both rebuild paths, the SmartPort panel, the 3.5" panel and routing, the
  flush and the shutdown persistence all go through it. Eleven defects fixed on
  the way, most of them two shapes — a drive parameter defaulting to 0, or a
  mutation that never wrote the key recording it:

  | Defect | Was |
  |---|---|
  | drive 2 never restored at startup | ctor's own restore loop, `insertDisk(path)` |
  | drive 2 lost on every profile switch | `isDiskLoaded()` / `getDiskPath()` in the snapshot |
  | drive 2 dropped by Slot Config Apply | same, in the settings sync |
  | drive 2 never persisted on exit | same, in the shutdown loop |
  | eject-all left drive 2 mounted | `ejectDisk()` |
  | eject-all reported success over a failed write-back | return value ignored |
  | Eject disk / Eject HDV re-mounted next launch | key never cleared |
  | Disk II + HDV write-back toggles forgotten | key never written |
  | 3.5" on-board eject + write-back not persisted | only the SmartPort branch wrote keys |
  | //c+ 3.5" panel showed the wrong drives | panel excluded //c+, mount did not |
  | SmartPort unit reused across ~5 lock acquisitions | one `SmartPortUnit*` per frame |

  Also removed a double restore (the ctor's loop ran after the plug pass, so
  every image opened twice at startup), and made the coordinator's
  `mountDiskII` two-phase — as written it read the file under `lockState()`,
  which would have undone v0.8.5's fix.

  **Left in `StorageCoordinator` — one item, and it is an interface change:**

- 🟡 **Three coordinator mounts still read the file under the machine lock**
  *(2026-08-27)* — `mountMediaBay`, `mountBlockBytes` and `mountHdv` do their
  read inside `lockState()`, the one-phase form `MediaMount.h` exists to
  prevent. The HDV case is the one measured at **25.8 ms under the lock**
  before v0.8.5 split it, against a 20 ms PAL frame. They are therefore **not
  wired**: `MainWindow` still calls `pom2::mountBlockCard`, which is already
  two-phase and correct, so nothing is broken — but the coordinator's own
  versions are a trap for whoever wires them next.
  Fixing them is not a substitution. `MountableMediaCard::mountBay` is a
  virtual that hides whether the target has block backing at all (the 3.5"
  SmartPort unit does not, which is why `MediaMount.cpp`'s `mountBlockLike`
  carries an inline-`loadImage` fallback), so the interface needs a two-phase
  pair — prepare-unlocked / adopt-locked — before the coordinator can offer
  one. `StorageCoordinator::mountDiskII` shows the target shape.
  *1 d.*

  **Left elsewhere:**

  | Coordinator | Aliases | Note |
  |---|---|---|
  | `SlotCardFactory` + `SlotConfiguration` + `SlotProvisioning` + `SlotRebuild` | `slotCards[]` draft/live | slot composition + teardown transaction |
  | `DebugCoordinator` | — | needs Dear ImGui, so frontend-only |
  | `NetworkCoordinator` | `sscCards` `sscCard` `fujiNetCard` (~29 sites) | **blocked** on the device seams below |

  → `CHANGELOG.md` 2026-08-26/27. *1-2 d left.*

- 🟠 **No test drives the ImGui panels, so a UI-thread deadlock fails nothing**
  *(found the hard way 2026-08-27)* — wiring `PrinterCoordinator` into
  `renderFujiNetPanelWindow` put a `captureHost()` inside an existing
  `lock_guard(stateMutex())` scope. Every coordinator capture/apply takes the
  machine lock itself and `stateMutex` is **non-recursive**, so opening that
  panel would have hung the UI thread and the emulator together — while the
  full 207-test suite stayed green, because nothing drives the panels.

  Mitigation in the tree now: **`tools/check_coordinator_locks.sh`**, run it
  after touching any coordinator call site. It is falsifiable — checked out
  against `44b715f` it reports the real bug and exits 1. Two working notes it
  encodes, both learned by getting them wrong:
  - it must match **both** `lockState()` and the bare `stateMutex()`; the
    first version looked only for the former and missed this bug;
  - a lock opened in a nested block that has since closed is NOT held —
    naive brace counting produces false positives on `renderMenuBar`,
    `renderStatusBar` and `applyProfile`, all of which are fine.

  The real fix is still `tests/frontend_device_panel_concurrency` (a headless
  ImGui frame driven while cards are replugged), which sits on
  `refactor/core-boundaries-and-coordinators` waiting on the device seams.
  *1 d, after the seams.*

- 🟡 **The wiring pattern, so the remaining tranches match the done ones**
  *(2026-08-27, working note)* — capture per call site rather than caching a
  per-frame snapshot in `MainWindow` (that is what the branch does and what
  its TSan pass covers); re-resolve the card inside the coordinator before
  applying a command, because ImGui callbacks (`onCardDipChanged`) fire later
  in the frame than the panel that installed them; bind panel-registry
  availability + label to a snapshot, not to `card(&alias)` — the registry
  takes the *address* of the member, so the alias cannot simply be deleted;
  and replace alias-nulling in the two `MainWindow_Slots.cpp` teardown blocks
  with an explicit invalidation where the coordinator holds identity across a
  rebuild (`PrinterCoordinator::resetFeedCursor` — a rebuild can hand a
  replacement card the same allocator address).

- 🟡 **Re-derive the device injection seams on top of v0.8.5** *(2026-08-26
  merge)* — `W5100Socket` / `SuperSerialTransport` / `FujiNetLink` and their
  deterministic fakes stayed on `refactor/core-boundaries-and-coordinators`
  because the branch's versions predate main's socket hardening
  (`disableSigpipe`, `MSG_NOSIGNAL`), `ThreadGuard` and the torn-RSR-read fix.
  The seams are worth having — they buy device tests that open no host socket,
  serial listener or process — but they must be rebuilt over the hardened code,
  not merged under it. `NetworkCoordinator` lands with them. *2-3 d.*
- 🟢 **The SDK install contract + the CMake layer guard** *(2026-08-26 merge)* —
  `find_package(pom2_core)` / `POM2::core`, the standalone consumer example and
  the configure-time rejection of upward includes all rest on the branch's
  layered object libraries (media / machine / devices / runtime). The flat
  v0.8.5 source lists have no installable library target to attach them to, so
  these land with that layering or not at all. The facade itself
  (`include/pom2/core.hpp`, pinned by `pom2_core_api`) is already in.
- 🟢 **What is still parked on `refactor/core-boundaries-and-coordinators`**
  *(inventory, 2026-08-27)* — so nobody re-derives it from the diff: the
  branch's own `MainWindow`/`Memory` decompositions (superseded by v0.8.5's and
  **not** wanted), the device seams `W5100Socket` / `SuperSerialTransport` /
  `FujiNetLink` + `FujiNetHost` + `W5100HostSockets` + `SuperSerialTcpTransport`,
  the three deterministic fakes and `tests/deterministic_adapter_injection`,
  `NetworkCoordinator`, `tests/frontend_device_panel_concurrency`,
  `tests/input_c0xx_contract`, `tests/mainwindow_tu_size.cmake`,
  `cmake/Pom2Architecture.cmake` + the layer-guard tests,
  `cmake/pom2_coreConfig.cmake.in` + `examples/pom2_core_consumer` +
  `tests/pom2_core_sdk_consumer.cmake`, and `docs/ARCHITECTURE.md`. Everything
  else from that branch is merged. Keep the branch until the seams item is
  done; it is the only copy.
- 🟢 **New headless tests link `pom2_core_test`** *(2026-08-26, working note)* —
  the assertion-enabled core-minus-renderer archive added with the merge
  (`CMakeLists.txt`), plus the `pom2_add_headless_test()` constructor in
  `tests/cmake/Pom2Test.cmake`. Declare a new test with SOURCES + `LINK_LIBRARIES
  pom2_core_test` instead of re-listing a private bundle of core sources. The
  197 pre-existing tests deliberately keep their explicit bundles — the archive
  is linked per target, not at directory scope, so converting them is optional
  cleanup rather than a prerequisite. The archive excludes `JoystickInput.cpp`
  (GLFW) and `DebugCoordinator.cpp` (Dear ImGui), the two core-list files that
  do reach the renderer.
- 🟡 **Scattered config** — `POM2_*` env vars + CLI flags + `Settings`
  to centralize into a `Config` (env → CLI → Settings → defaults),
  list env vars in `--help`. *1 d.* Architect **P4**.
- 🟡 **`stateMutex` shared CPU+UI** (`EmulationController.h:229`) —
  `MainWindow_Slots` takes this lock during plug/unplug, audio jitter
  risk. Partition long-term. Architect **P1** grain (GUI TSan first).
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
  port already covered by P1 (CFFA done).

## Changelog

See [`CHANGELOG.md`](CHANGELOG.md).
