# POM2 — TODO

Status as of 2026-08-14. Resolved items → `CHANGELOG.md`. MAME refs → `DEV.md`.

**Format**: `🟠 high · 🟡 medium · 🟢 low` at the head of each item. Indicative
effort in *italics*. File/line in `backticks`. Quick read:
[Quick wins](#quick-wins) then [Backlog by subsystem](#backlog).

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
| 2  | Memory + IIe + RamWorks        | Partial-verbatim | `apple2e.cpp:1275-1299`, `a2eramworks3.cpp:108-115`                      | 🟠 god-object (Keyboard/PaddleInputs to extract)                                         |
| 3  | Display HGR/DHGR/80-col        | Partial-verbatim | `apple2video.cpp:124-201`, `460-471`, `:751-758`; AppleWin `RGBMonitor.cpp` | 🟢 mono DHGR 1-px (mid-scanline, PAL 50 Hz, floating bus `$C05x`, page-flip DROL, Chat Mauve RGB: done) |
| 4  | SpeakerDevice                  | Verbatim         | `spkrdev.cpp:74-327`                                                     | —                                                                                        |
| 5  | CassetteDevice                 | POM2-original    | `apple2.cpp:362`                                                         | —                                                                                        |
| 6  | Mockingboard A/C (6522 + AY)   | Partial-verbatim | `ay8910.cpp:998-1015`, `:1077-1104`, `1309`; `6522via.cpp:959`          | 🟢 Port A read mask by DDR; 6522 subset (SR/PCR; T2 one-shot done, IRQ N+3 MAME)      |
| 6b | Mockingboard "C" Sound II      | POM2 + AppleWin  | `Mockingboard.h/.cpp` + `Via6522::setCa1NegativeEdge`                    | — (SSI263 at `$Cs40-$Cs44`, A/!R → VIA1.CA1)                                              |
| 7  | FloppySoundDevice              | Verbatim         | `floppy.cpp:1532-1620`, `:2925-3020`                                     | —                                                                                        |
| 8  | SlotBus + IRQ wire-OR          | POM2-original    | MAME slot bus pattern                                                    | —                                                                                        |
| 9  | DiskImage                      | Partial-verbatim | `woz_dsk.cpp`, `flopimg.cpp:2017-2106`                                   | 🟡 WOZ1 splice TRK+6650; 🟢 .nib2/.app, half-tracked NIB (88)                           |
| 10 | DiskIICard                     | Partial-verbatim | `machine/wozfdc.cpp:264-291`, P6 PROM 341-0028-A                         | 🟢 sub-instruction RAII vs per-cycle; Disk II out of snapshot deliberate                    |
| 11 | IWMDevice                      | Verbatim         | `machine/iwm.cpp:1-543`                                                  | 🟢 Q3 fast clock (Mac/IIgs only); window-size rounding                                  |
| 12 | SmartPortCard (//e Liron)      | POM2-original    | SmartPort spec + Apple Tech Note                                         | 🟢 multi-partition ProDOS (CFFA3000)                                                     |
| 13 | SmartPortHub + Sony35Drive     | Verbatim         | `apple2e.cpp:638-679`, `mac_floppy.cpp`, `flopimg.cpp:512/967/2017-2106` | —                                                                                        |
| 14 | CFFA (MAME-faithful IDE)       | Verbatim         | `bus/a2bus/a2cffa.cpp`                                                   | 🟢 CHD = phase 2; no media preservation on profile switch                         |
| 15 | ClockCard / ThunderClock+      | Partial-verbatim | `upd1990a.cpp:248-267`, `:312-327`; Thunderware Rev 1.3 EPROM (`roms/thunderclock_u9_v1.3.bin`) | 🟡 MODE_SHIFT lax; 🟡 DATA_OUT live vs MAME latch; 🟢 real EPROM loads from the ctor (synth ROM = fallback, untested from `$C800`) |
| 16 | SuperSerialCard                | Partial-verbatim | `mos6551.cpp:46`, `:542-543`, `a2ssc.cpp:373`                            | 🟢 IRQ gate SW2:6 DIP not gated                                                          |
| 17 | MouseCard (MAME)               | Verbatim         | `bus/a2bus/mouse.cpp`, M68705 + MC6821                                   | 🟢 PIA out_a/b without `scheduler.synchronize`                                          |
| 18 | MouseCard (AppleWin HLE)       | Verbatim         | AppleWin `source/MouseInterface.cpp`                                     | — (slot EPROM only, MCU synthesized)                                                      |
| 19 | Phasor (AE — 2×VIA, 4×AY)      | Verbatim         | MAME `a2bus/phasor.cpp` + AppleWin                                       | 🟢 EchoPlus mode (=7) routed as native Phasor; stereo L/R per VIA pair done (2026-08-01) |
| 20 | SSI263 speech (chip model)     | AppleWin-faithful| AppleWin `source/SSI263.{h,cpp}` (MAME does not implement)                 | 🟢 formant synth → PCM blob, 62 phonemes (AppleWin LGPL → GPL3)                           |
| 21 | EchoPlusCard (Cricket/SSI263, key `echoplus`) | POM2-original | Cricket / Street Elec SSI263 spec (historically mislabelled "Echo+") | 🟢 markadev audit 2026-05-28: the real Echo+ = TMS5220 (see line 21bis)                |
| 21bis | EchoPlusTMS5220Card (key `echoplus_tms`) | Scaffold       | markadev/AppleII-RevEng/Street-Electronics-Corp-ECHO+                  | 🟡 stub register decode; TMS5220 LPC + AY-3-8913 synth cores deferred                  |
| 22 | PrinterCard (parallel synth)  | POM2-original    | Apple II slot 1 convention + Pascal 1.1 sig                              | 🟡 PDF export deferred (`.txt` OK)                                                       |
| 22bis | GrapplerCard (key `grappler`) | Verbatim         | MAME `bus/a2bus/grappler.cpp` (pinned 2026-07-28, line-cited) + markadev 4 KB EPROM (`roms/grappler_plus.bin`) | 🟢 /STROBE 7-clock pulse collapsed to instant (no observer); `ackEffective()` BUSY gate is POM2's back-pressure model |
| 22ter | ImageWriter II printer (host-side, no slot) | greg-kennedy/ImageWriter (GSport/KEGS/DOSBox lineage) | Apple ImageWriter II + LQ reference manuals                 | 🟢 full control language, 4-band colour ribbon, 8-/24-pin bit images, paper tray + PNG & multi-page PDF export; fed by `printer` / `grappler` / SSC printer tap (//c PR#1) |
| 23  | UthernetCard + Cs8900aDevice (key `uthernet`) | Verbatim | MAME `machine/cs8900a.cpp` (VICE lineage) + `bus/a2bus/uthernet.cpp`, line-cited | 🟢 pull-mode RX (POM2 has no `device_network_interface` push bus); inbound frame queue out of snapshot deliberate |
| 23bis | UthernetIICard + W5100Device (key `uthernet2`) | AppleWin-faithful | AppleWin `source/Uthernet2.cpp` + `W5100.h` (MAME has no W5100 device) + WIZnet datasheet v1.2.8 | 🟡 `LISTEN` unimplemented (no inbound path); 🟢 virtual DNS is async, not blocking like AppleWin's |
| 23ter | NetworkBackend (Null / Loopback / libslirp) | POM2-original | AppleWin `Tfe/NetworkBackend.h` shape; libslirp user-mode NAT | 🟢 outbound-only by design (no root); no TAP/pcap path; 🟡 libslirp is Linux/macOS only, so Uthernet I has no transport on Windows |

## Quick wins

Suggested attack order — items with high impact/effort ratio.

| # | Item                                    | Effort  | Why                                |
| - | --------------------------------------- | ------- | --------------------------------------- |
| 1 | WASM IDBFS settings persistence         | 2-4 h   | web user has no state        |
| 2 | WOZ1 splice point TRK+6650              | 1 d     | Applesauce re-master parity             |
| 3 | Memory god-object split                 | 2 d     | cuts recompiles (IIgs itself lives in the separate pom2gs project) |
| 4 | Debugger runtime glue (BP / watch / step) | 3-5 d | 80% of the bricks are there (Disassembler + MemView) |
| 4b | ~~Digidream 1 tempo regression~~ ✅ DONE | — | cause measured (`caughtUp` paced against the last write, not CPU-now) and fixed 2026-08-01 (see [Audio]) |
| 5 | ~~CI GitHub Actions (`ctest` headless)~~ ✅ DONE | — | the dormant ctest suite (~130 tests) now gated (see [Arch]) |
| 6 | ~~Desktop drag-drop disk (`glfwSetDropCallback`)~~ ✅ DONE | — | README promise kept (see [UI/UX]) |

## Backlog

Grouped by subsystem. Severity encoded by 🟠/🟡/🟢 at the head of each item.

### [Memory] paging & RAM expansion

- 🟠 **God-object split** — extract `Keyboard` (FIFO + strobe + paste)
  and `PaddleInputs` (RC + buttons + Open/Solid Apple) from `Memory.cpp`.
  `IIcPlusBank` already done (`MemoryProfile`/`IIcClassProfile`).
  *Cuts recompiles + readability; any IIgs reuse happens in the separate
  pom2gs project, not here. ~2 d.*
- 🟡 **Saturn 128K LC** (Saturn Systems) — 16 banks ×16 KB on LC
  `$D000-$FFFF`, switches `$C080-$C08F` slot-relative. MAME refs
  `bus/a2bus/a2memexp.cpp`. *2-3 d.*
- 🟡 **`Memory::memRead` hot path** — 7-level `if` cascade
  (`Memory.cpp:1309-1437`). 256-entry dispatch table per high page.
  Prerequisite: `IIcPlusBank` extraction.
- 🟢 **Dedicated Pascal LC** — 16 KB variant shipped with Apple Pascal,
  minor differences vs IIe LC (write-protect DIP). *1 d.*

### [Display] HGR / DHGR / 80-col

- 🟡 **OE-CPU demod runs under `stateMutex`** — *small*. `MainWindow.cpp`
  `drawScreenImage` holds the emulation mutex across `display->render()`;
  in `ColorCompositeOECpu` (and mixed OE-GPU frames) that includes the
  17-tap × 560×192 FP demod (~1-2 ms/frame), blocking the CPU worker every
  UI frame — reads as emulation/audio jitter, amplified under disk-turbo.
  The demod consumes only `signalBuf` (filled under the lock); run it
  after release. (2026-07-12 graphics hunt, verified.)
- 🟢 **Frame-wrap video-event off-by-one** — *small, rare*. An instruction
  straddling the exact 17030/20280-cycle video-frame boundary publishes
  its soft-switch event into the *closing* frame (applied one frame
  early); ≤ ~7 cycles of exposure per frame. Fix: at publication, retain
  events stamped `>= frameBoundaryCycle` in the new recording log.
- 🟢 **`renderCompositeOeCpu` lacks PAL line-phase alternation** — with
  `ntsc_pal` on, mixed OE-GPU frames (CPU-demodded) treat hue differently
  from full-screen frames (GPU shader path).
- 🟢 **Golden coverage gaps** (from the 2026-07-12 audit): flash-on phase,
  ALTCHAR/mousetext + char-ROM glyphs, PAGE2/80STORE scanner-page scenes,
  rev-0 DHIRES+80COL-off HGR (would have caught the paintHgr bit7 bug),
  IIe 80COL+HIRES+MIXED without DHGR, Chat Mauve sub-modes hash-frozen,
  PAL beam-raced splits. Also: OE-GPU uploads the unused ~430 KB fallback
  framebuffer every frame (minor perf).
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
- 🟡 **Eve Color text mode `$C0B9`** — Chat Mauve/Eve variant, FG/BG
  per character. Stub `LeChatMauve_ImGui.cpp:200`. *2 d.*
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
  - **F3** vignette center-lighting ~4× too strong (`cuv = 2×qc` in OE,
    `CrtEffectStack.cpp`).
  - **F2** cosine scanline → OE's **sin²** (keep the `scanAA` anti-moiré term).
  - **F7** HGR mono: 280 px average / 3 levels → **560 binary** (copy of the
    DHGR-mono loop already shipped).
  - **F6** row-dim mask ×0.7 ⚠ (make luminance-neutral, don't drop hard).
  - **DLGR not wired** to `fillCompositeSignal` (still emits lo-res 40-col
    main-only under OE/AppleWin).
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
- ✅ **DC blocker**, 1-pole 20 Hz, matching MAME's default per-speaker
  high-pass (`src/emu/audio_effects/filter.cpp:39-44`).
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
- 🟢 **ayumi-grade resampling** (native clock/8 → 8× quadratic interp →
  192-tap FIR decimation + moving-average DC filter,
  `true-grue/ayumi`, MIT). Strictly better than the box filter and what
  chiptune players use; ~8× the inner iterations plus ~96 MACs per sample
  per channel, ~192 doubles/channel of rewind state and ~2 ms group
  delay — a real cost on the **WASM** target. Only worth it if listening
  shows the box filter is insufficient. Note this would be a deliberate
  departure from "MAME = source of truth" for the audio path.
- 🟢 **Analog output stage.** The real Sweet Micro board's LM386 pair
  makes the output *triangular*, not square (deater's scope capture:
  `deater.net/weave/vmwprod/chiptune/mock_problem/`). No emulator models
  it and there is no MAME oracle, so it would have to be an off-by-default
  toggle labelled non-authoritative — and only after band-limiting, since
  a low-pass over an aliased signal muffles rather than removes.
- 🟢 **Mutex contention.** `advanceCycles` takes the card mutex on every
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
- 🟡 **A failed insert destroys the disk that was in the drive**
  *(management audit 2026-08-08)* — `DiskImage::loadFile` clobbers the
  live buffer before it knows the new file parses, so clicking a corrupt
  / wrong-size image in the Disk Library ejects the disk that was
  running. The header documents it ("Mounting a new image discards any
  previously loaded buffer"), but no real drive behaves that way and the
  user gets no warning. Fix is a load-into-scratch-then-commit: decode
  into a temporary `DiskImage` and move-assign only on success — which
  also wants `DiskImage` to be cheaply movable (it is: 228 KB of tracks
  plus vectors). Deliberately out of scope of the 2026-08-08 sweep,
  which only made the FAILURE state coherent (`path` is cleared, so the
  drive no longer reports "empty, but here is the last disk's path").
  *~2 h.*
- 🟢 **`decodeTrack` trusts the address field** *(management audit
  2026-08-08)* — the write-back decoder reads vol/track/sector/checksum
  as 4-and-4 but validates none of them: the checksum is discarded, and
  the address field's TRACK number is ignored in favour of the buffer
  index. A guest that rewrites a whole track with a different track
  number in its address fields (sector editors, Locksmith-style
  copiers) therefore lands its sectors at the wrong file offset. `$D5`
  is not a legal GCR data byte so a spurious prologue match can't
  happen, which is why this has never bitten in practice. *~1 h.*
- 🟢 **800 K `.dsk` routes to the 5.25" bucket and fails** *(management
  audit 2026-08-08)* — `classifyDiskForSlot` / `accept525` claim every
  `.dsk` regardless of size, so an 819 200-byte one goes to the Disk II
  card, which refuses it — even though `Disk35Image::loadFile` accepts a
  bare 800 K `.dsk` payload. Needs a size-gated `.dsk` rule in BOTH
  predicates (they are deliberately kept in lock-step). *~30 min.*
- 🟢 **WOZ FLUX quarter-tracks are silently read-only** *(management
  audit 2026-08-08)* — `loadWoz`'s flux path populates `bitStream[qt]` /
  `fluxStream[qt]` but leaves `wozQtBitCount[qt]` at 0, which is exactly
  the condition `writeFlux` bails on. Correct for now (splicing into a
  delta stream needs re-encoding the tick deltas, not just flipping
  cells) but undocumented — a write to a FLUX track is dropped with no
  diagnostic. At minimum: log it once per track. *~15 min to warn, ~1 d
  to implement.*
- 🟡 **WOZ1 splice point (TRK+6650)** — `DiskImage::writeFlux` splices
  bit-cells but the full `set_write_splice` handling (TRK +6650
  splice_point/nibble/bit_count fields, parsed at `DiskImage.cpp:720`)
  is ignored; IWM call site wired (`IWMDevice.cpp:235`, see the comment
  at `IWMDevice.cpp:48`). Applesauce re-master parity. *1 d.*
- 🟡 **SmartPort ProDOS multi-partition** — 1 image = 1 unit = 1
  volume today; multi-volume CFFA3000-style not supported.
- 🟢 **UI "Force DOS / Force ProDOS"** — backend ready
  (`DiskImage::loadFile(path, SectorOrder)` at `DiskImage.cpp:212`),
  button missing in `DiskLibrary_ImGui` / `DiskController_ImGui`.
  Auto-detect (extension + vol-dir content sniff `0x400`/`0xB00`)
  already covers 99 % of cases; manual override useful for ambiguous /
  non-standard / debug images. *~30 min.*
- 🟢 **Half-tracked NIB (88)** + **Applesauce `.nib2`/`.app`** +
  **Disk II in snapshot** — deliberately out of scope as long as
  WOZ covers it.
- 🟢 **Floppy Emu Dual-5.25" + Smartport-Unit-2 modes** — out of scope
  for v1 (4 main modes covered).

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
- 🟠 **Z-80 SoftCard + CP/M** — Microsoft SoftCard, Z-80B clipped onto
  the 6502 bus, shares RAM via mode-switch. Unlocks the CP/M library
  (BASIC-80, dBase II, Turbo Pascal, WordStar). MAME refs
  `a2softcard.cpp` + Z-80 core. *10-15 d.*
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
  $Cs00-$Cs0F, enough for detection. Remaining: TMS5220 LPC10
  decoder (chirp ROM + K-parameter interpolation) and AY-3-8913 audio
  synth (usable once the Mockingboard/Phasor core is extracted into a
  shared helper). *~3-5 d.*
- ✅ **No-Slot Clock (NSC, DS1216E)** — DONE. `src/NoSlotClock.{h,cpp}`
  is a full DS1216E SmartWatch state machine, hooked into `Memory`
  read paths (`interceptRead` under the $F800 ROM window) for machines
  with no free slot (//c). MAME refs `ds1216.cpp`. Pinned by
  `no_slot_clock_smoke` (`tests/no_slot_clock_test.cpp`).
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
  there (`IWMDevice` verbatim, `Sony35Drive`, zoned GCR, `SmartPortHub`).
  Remaining: `LironCard : SlotPeripheral` + ROM 343S0001.
  **Blocker**: no public ROM dump (MAME `a2iwm.cpp` *WANTED*).
  *~8-12 h excluding ROM sourcing.*
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

### [Printer]

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

### [Input] joystick / paddles / mouse

- ✅ **Apple II square-gate stick** — DONE (2026-07-10).
  `JoystickInput::applySquareGate` expands the round modern-stick region to
  the full square so the corners (255/255) are reachable (Wings of Fury
  take-off); radial deadzone; toggle + persisted `joystick_square_gate`.
  Pinned `joystick_square_gate`. Detail → `DEV.md` § Joystick / `CHANGELOG.md`.
- ✅ **Kiosk gamepad disk selector** — DONE (2026-07-10). Start (or F10) opens
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
- 🟡 **"Zero-friction" web demo** *(commercial audit 2026-05-31)* — the WASM
  lever is throttled: user-provided ROMs + no bundled disk → the browser demo
  does not start turnkey like POM1, which kills instant conversion and viral
  sharing (3D voxel / rewind). Bundle **royalty-free demo disks** (without
  touching proprietary ROMs) playable on the WASM side. A marketing prerequisite
  before pushing to r/apple2 + Hacker News; aim in parallel for a **stable 1.0**
  (finish the //c+/IWM boot, cf. parity dashboard). *1-2 d excluding media
  sourcing.*

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
  and runs the ~130-test ctest gate (Klaus 6502+65C02, Tom Harte curated,
  `cpu_cycle_count`, golden-hash display, boot traces); **wasm** is an Emscripten
  verification build (`build_wasm.sh`) asserting `wasm/POM2.{js,wasm}` +
  `index.html` are produced. Both jobs shallow-clone Dear ImGui (gitignored); no
  test depends on the user-supplied ROMs.
- 🟠 **ThreadSanitizer pass over `EmulationController` / `stateMutex` / the
  audio thread** *(2026-08-02 bug-hunt follow-up)* — the highest-yield gap we
  know of. That sweep's ASan+UBSan build (156 test binaries, ~24 000
  hostile-input cases, ~6 M random instructions) returned **zero**
  diagnostics, yet code reading found a UI deadlock, two use-after-frees and
  three unlocked cross-thread reads in the same tree. ASan cannot see data
  races and the headless tests cannot reach the GUI, which is exactly where
  the defects were. Needs a TSan build driving the GUI with the AI server
  polling `/screen.ppm`, slot reconfiguration, and rewind under load. Would
  also retire the two findings that could not be pinned (`saveScreenshot`'s
  `demodMutex` ordering, and the threaded half of `disk_path_snapshot`).
- 🟡 **Consolidate the atomic file-write helper** *(2026-08-02)* — three
  divergent copies still: `DiskImage.cpp`'s `writeFileAtomic` (anonymous
  namespace), `Disk35Image.cpp:214` (added 2026-08-02) and
  `ProDOSVolume.cpp:664-702` — the temp-file naming, the permission carry-over
  and the error strings are hand-repeated in each. `DiskImage`'s copy caught
  up on permission preservation 2026-08-08 (it was silently resetting the
  image's mode to the umask default on every write-back); `ProDOSVolume`'s
  still hasn't. Extract to `src/FileAtomicWrite.h`.
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
- 🟢 **Run the `SnapshotIO` fuzzer** *(2026-08-02)* — built during the ASan
  sweep but never executed, so that parser is the one untrusted-input surface
  in the tree with no dynamic coverage. The disk-image and WOZ parsers came
  through 4 200 + 13 270 mutations clean; snapshots are loaded from
  user-supplied files on the same trust footing.
- 🟠 **`MainWindow.cpp` god-object (~10 200 lines)** *(audit 2026-05-31, count
  re-measured 2026-08-14 — it was ~6700 then, so the file is still growing)* — biggest
  single file in the repo, monolithic UI despite the `_Slots`/`_MemoryMaps`/
  `_ImGui` splits. Slows recompiles + hurts readability. Extract device-window
  groups into dedicated TUs (aim for < 3000 lines/file, like POM1's `MainWindow_*`
  discipline). *3-5 d.*
- 🟡 **Scattered config** — `POM2_*` env vars + CLI flags + `Settings`
  to centralize into a `Config` (env → CLI → Settings → defaults),
  list env vars in `--help`. *1 d.*
- 🟡 **`stateMutex` shared CPU+UI** (`EmulationController.h:118`) —
  `MainWindow_Slots` takes this lock during plug/unplug, audio jitter
  risk. Partition long-term.
- 🟡 **Inconsistent `pom2::` namespace** — 105/167 top-level files,
  `tests/` does not use it. Mechanical migration.
- 🟢 **Legacy M6502 style** — FR/EN comments, C-style casts,
  `void(void)`. Targeted `clang-format` + `clang-tidy modernize-*`.
- 🟢 **`*Card` raw pointers in MainWindow** (`MainWindow.h:97-103`) —
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
