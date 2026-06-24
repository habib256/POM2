# Edge-case test corpus — real software for validating the emulation

> Curated list of programs (cycle-exact demos, copy-protected disks, CPU
> suites) that torture the corners of the Apple II: cycle-accurate CPU↔video
> sync, raw magnetic flux, 6502 timing, and IRQs. Serves as the **manual /
> integration test backlog** beyond the unit `ctest`s.
>
> Origin: curated research, re-verified and cross-checked against the actual
> POM2 subsystems. When a program targets a `known gap` from the
> [parity dashboard](../TODO.md#mame--pom2-parity-dashboard), the `#` number is
> cited. **Status** = what POM2 does *today*, not a promise.
>
> ⚠️ All commercial game images are **user-provided** (like the ROMs). This
> document references no binary; it describes *what to test and why it's hard*.

> **⭐ Priority reference — [DIX](https://github.com/Fr3nchT0uch/DIX/)**
> (French Touch anthology, 29+ min of Apple II demos 2014–2024, GPLv3 sources).
> It is the **most complete test bench** for chasing emulation perfection:
> vapor lock / floating bus, mid-scanline video toggles, Mockingboard + VIA IRQ,
> 128 KB aux, SmartPort/Liron + 800 KB Unidisk, PAL 50 Hz timing. If DIX runs
> glitch-free, the emulator is at the "cycle-exact demo" level; if DIX breaks,
> the corpus below tells you *which* subsystem to dig into.

## Contents

- [1. CPU↔video accuracy (cycle-exact sync)](#1-cpuvideo-accuracy-cycle-exact-sync)
- [2. Disk II controller hell (flux / WOZ)](#2-disk-ii-controller-hell-flux--woz)
- [3. CPU & hardware quirks](#3-cpu--hardware-quirks)
- [4. Audio / Mockingboard (VIA IRQ)](#4-audio--mockingboard-via-irq)
- [Appendix — Vapor Lock in detail](#appendix--vapor-lock-in-detail)
- [Corrections vs the original source](#corrections-vs-the-original-source)

---

## 1. CPU↔video accuracy (cycle-exact sync)

The Apple II has **no dedicated video timer and no VBL IRQ** (on II/II+; the //e
adds read-only `$C019` RDVBL). All fine-grained sync relies on the **floating
bus**: reading an undriven I/O address returns the last byte placed by the video
scanner (TTL capacitive effect). See
[Vapor Lock appendix](#appendix--vapor-lock-in-detail).

| Program | What it tortures | Why it's an edge case | POM2 status |
|---|---|---|---|
| **deater — "megademos"** (Vince "deater" Weaver, `deater.net/weave/vmwprod`) | Vapor lock: detects VBL by looping on an undriven `$C0xx` read until it reads a marker byte written into video RAM. | If video is a framebuffer rendered asynchronously at end-of-frame instead of interleaving CPU reads and the scanner cycle-by-cycle, the loop never "locks" → frozen / glitched screen. | ✅ **Proven (2026-06-09)**: `Memory::floatingBus()` = verbatim MAME port of `apple2video.cpp:124-201 scanner_address`, indexed on `cycleCounter`. The `vapor_lock` test *runs a real 6502 loop* (`LDA $C058 / CMP marker / BNE`) and **it locks** onto the marker placed in video RAM. The scanner geometry now follows the **video standard** (262 NTSC / **312 PAL**) — it was hard-coded to 262, which made the per-frame lock of PAL demos drift; fixed. **Sub-instruction accuracy fixed**: the `$C0xx` CPU read samples the bus at the **access cycle** (`cycleCounter + getCurrentInstructionCycles()` = last cycle of an `LDA`/`CMP`/`BIT`, consistent with the event-log timestamp) instead of the **start** cycle of the instruction. **All undriven `$C0xx` reads return the bus**: `$C040`, **`$C050-$C057`** (2026-06-10) and `$C030-$C03F` used to be 0. Pinned `vapor_lock` (§(d) = DROL cut-scene) + `floatingbus_page2_smoke`. *(Still 🟢: non-last-cycle `$C0xx` accesses, e.g. RMW — not used for vapor lock.)* |
| **DROL** (Brøderbund 1983; `disks_5.4/woz/Drol*.woz`, `disks_5.4/gist/Drol.dsk`) | Real game, double edge case: (1) **unsynchronized double-buffer page-flip** (`$C054/$C055` every ~4 frames at drifting positions) for animation; (2) **vapor-lock cut-scene** via `LDA $C050 / CMP #$80`. | (1) A naive beam-raced replay paints the band above the flip from the page **currently being redrawn** (RAM read at render time, not at the beam) → half-erased sprites. (2) A `$C050` read returning 0 instead of the bus → the loop never locks → freeze (historical LinApple hang). | ✅ **DONE (2026-06-10)**, diagnosed via `tests/drol_probe.cpp` (boots the real WOZ). (1) `forEachBeamSegment` distinguishes unidirectional flip (= buffer → final full-frame page, anti-flicker) vs bidirectional (= exact beam-racing, DIX MODPAGE). Pinned `drol_pageflip_render`. (2) reading `$C050-$C057` toggles the mode AND returns `floatingBus()`. Pinned `vapor_lock` §(d). Bonus: the 6 560-wide painters now take the band state (the bug that masked the flicker under Chat Mauve). |
| **[DIX](https://github.com/Fr3nchT0uch/DIX/)** — French Touch anthology (29+ min, //e / //c PAL, GPLv3 sources) | **All-in-one integration suite**: vapor lock, mid-scanline, DHGR/NTSC, Mockingboard, 128 KB aux, 800 KB Unidisk via Liron/SmartPort. Bundles *Mad Effect*, *Plasmagical*, *Wave* and the other recent FT productions. | A single disk that chains the edge cases of §1–4; the reference to hit before declaring the emulation "perfect." **Requires PAL 50 Hz (not NTSC).** | 🟡 **Priority #1**. Mid-scanline rendering ✅ (see next row). PAL 50 Hz timing ✅ (`iie-pal`/`iic-pal` profiles, 312-line geometry everywhere: scanner, `$C019`, events). **50/60 Hz hand-off ✅ (2026-06-10)**: the event log is published **per video frame** (65×312 cycles) and consumed by *copy* by the 60 Hz UI — the old per-worker-tick bracketing lost events between the UI take and the next tick (~1 empty log in 6 under PAL → 10 Hz flicker of the splits; invisible to the tests, which bracket synchronously). Remaining: **visual validation on a real screen**. Probes: `dix_modpage_split`, `horizontal_split*`, `dhgr_phase_signal`, `floatingbus_page2_smoke`, `pal_timing`, `video_event_publish`. |
| **"French Touch" productions** (e.g. *Mad Effect*, *Plasmagical*, *Wave* — included in DIX) | **Mid-scanline video mode changes** (TEXT↔HGR, PAGE1↔2, lo↔hi-res between two cycles of the same line). | Requires a 6502 split into **real per-access sub-cycles**: an opcode executed atomically (effects applied in one block) shifts the switch by 1-2 cycles → mis-placed color bands. | ✅ **Intra-line rendering done (2026-06-09)**: `Apple2Display::renderBeamRacing` replays the event log at the **byte-column** level (`frameCycleToPos`), horizontal TEXT/HGR/LORES/DHGR/80-col **and** PAGE1↔2 / ALTCHAR splits on the same line, in RGBA *and* composite signal. Probes: `horizontal_split`, `horizontal_split_composite`, `horizontal_split_560`, `dix_modpage_split`, `dhgr_phase_signal`, `artifact_phase_probe`. *The exact transition cycle at the character-clock remains a refinement.* Detail → `DEV.md` § Beam-racing. |
| **DHGR demos / `dapple`-like + NTSC artifact tests** | DHGR soft-switch evaluation order (`80STORE`/`PAGE2`/`HIRES`/`AN3`) and color fringing (NTSC artifacting via signal interleaving). | Validates the exact Le Chat Mauve switch order (AN3 FIFO → `$C05E/F`) and composite demodulation. | ✅/🟡 Composite NTSC pipeline (`NtscPostProcessor`, `AppleWinNtsc`) + CPU/GPU paths. Covered by `dhgr_render_smoke_test`, `oe_demod_gpu_cpu_parity_test`, `display_golden_hash_test`. Residual gap: 1-px mono DHGR, floating-TTL (`#3`). |

### Source-level DIX analysis — 2026-06-09

Reading the GPLv3 source ([Fr3nchT0uch/DIX](https://github.com/Fr3nchT0uch/DIX/),
e.g. `MADEF2/main.a`) to frame the validation. The flagship loop (`INT_ROUT1`,
page-aligned, run on the last VBL line) does, **every 65-cycle scanline** and
over 6 lines:

```asm
MODPAGE0  LDA $C054,X          ; PAGE1/PAGE2 mid-line (X = scroll offset)
MODLINE0  LDA $C056 (×11)      ; HIRES mid-line, ~44 cycles
```

It is **synchronized by a Mockingboard Timer-2 IRQ**:
`DEFAULT_SYNC_TIMER = 7479 ; IRL machines PAL`.

Consequences for POM2, cleanly separated:

1. **Mid-scanline rendering (PAGE/HIRES/mode) — ✅ DONE.** Byte-column
   beam-racing replays these toggles at the right column. **Bug found + fixed
   during validation**: the RGBA painters (`renderText/HiRes/LoRes`) re-read
   `mem.getDisplayState()` internally → the **PAGE2** (and `ALTCHAR`) selection
   used the *end-of-frame* state, not the band's. Fixed by passing the per-band
   `state` to the painters (the composite path already did so). Pinned by
   `dix_modpage_split` (the exact MODPAGE technique: page 1 on the left, page 2
   on the right, same line).
2. **Mockingboard Timer-2 IRQ — ✅ supported** (`Via6522` T2 one-shot phase-2,
   `IFR_T2`/`t2Counter`). The sync IRQ *fires*.
3. **PAL 50 Hz machine timing — ❌ BLOCKER #1.** POM2 is **NTSC only**
   (`kScanlinesPerFrame = 262`, 17045 cyc/frame; the `NtscPostProcessor`'s "PAL"
   is only a shader color mode, not machine timing). `DEFAULT_SYNC_TIMER=7479`
   and the 312-line PAL geometry place the effect vertically and pace the music
   for 50 Hz; on 262 NTSC lines, the effect is mis-positioned / rolls and the
   tempo is ~20 % too fast. **This is the prerequisite for true end-to-end DIX
   validation** (to be added to the backlog as PAL machine timing: 312 lines,
   1.0157 MHz, 50 Hz refresh).

### DIX boot on //e PAL — DONE (SmartPort `$Cn0A` entry, 2026-06-09)

The target profile is **`//e PAL` + Mockingboard slot 4 + SmartPort slot 5** (the
//c has no slot for the Mockingboard that DIX requires). Two bugs found and
fixed by driving DIX through the AI server + direct memory reads:

1. **Mockingboard detection ("KO")** — DIX (`boot_unidisk.a` `BADGUY`) writes
   `K`,`O` to `$400/$401` if a detection fails (`STX $403`: A=model, C=CPU,
   **M=Mockingboard**). The MB detection reads the 6522 Timer-1 counter at
   `$CX04` twice (8 cycles) and expects `-8`. → passes with the Mockingboard in
   slot 4 (POM2's 6522 T1 counts down correctly).
2. **Post-banner freeze** — DIX loads its menu (8 blocks → `$D000` RAM-LC) via
   `JSR $C50A`, the **fixed `$Cn0A`** driver entry of the real Liron/Unidisk
   firmware. POM2 synthesized its dispatch at `$Cn50` → `$Cn0A` = `$00` =
   **BRK**, and since DIX had just enabled RAM-LC reads (`LDA $C083 ×2`), the BRK
   vector was read from cold RAM-LC → permanent storm. **Fixed**: `JMP $Cn50` at
   `$Cn0A` (`SmartPortCard::buildRom`, same `$42-$47` convention). Pinned
   `smartport_unidisk_entry`. **This was NOT the Language Card / aux** (zero LC
   writes, MMU flags at zero) — diagnosis corrected.

**Result**: DIX boots, loads its demo into RAM-LC (`$D000+`) and **runs** — PC in
the demo code (RAM-LC), **both HGR pages filled** (`$2000` + `$4000`, animation
page-flip). The fidelity layers are in place: **PAL 50 Hz** ✅, **mid-scanline**
✅ (RGBA + composite + 560), **vapor lock** ✅ (proven + PAL-aware + access cycle),
**SmartPort `$Cn0A` boot** ✅, **Mockingboard Timer-2 sync** ✅ (IRQ at
`N+IFR_DELAY = N+3`, MAME `6522via.cpp:959`; pinned `via_t2_timing` — DIX sets
`T2 = 7512 − latency`), **50/60 Hz event-log hand-off** ✅ (2026-06-10: per-
video-frame publication, otherwise ~1 frame in 6 lost its splits under PAL;
pinned `video_event_publish`). *(Headless verify: `/mem` text/HGR page +
`/status` PC; `/screen.ppm` frozen without a UI loop. Remaining: real visual
observation to confirm the fine placement of the effects.)*

---

## 2. Disk II controller hell (flux / WOZ)

**Logical-sector** emulation (`.dsk`, `.po`) is not enough: these titles require
the **raw magnetic flux** (`.woz`) + the behavior of the stepper motor and the
300 RPM rotation.

| Program | Protection | Why it's an edge case | POM2 status |
|---|---|---|---|
| **Captain Goodnight and the Islands of Fear** (Broderbund) | **Spiradisc**: data written on a **continuous spiral** (track `$01`→`$0E`), not in concentric circles. | The controller must follow head moves **"on the fly"** while the flux streams by; an LSS that resyncs per track crashes at boot. | 🟡 Event-driven LSS + WOZ bit-stream present (`DiskIICard`, `DiskImage`, `#9/#10`). Half-tracks handled; continuous spiral tracking **to validate** on a real WOZ image. Nearby tests: `woz_bit_timing_smoke_test`, `diskii_lss_smoke_test`. |
| **Prince of Persia** (Broderbund / Roland Gustafsson) | **RWTS18**: quarter-tracks, modified sync bytes, timing bits / weak bits. | The rotation speed, the sync-nibble spacing and the weak-bit interpretation must be consistent with the 6502 cycles → otherwise the protected tracks fail to read. | 🟡 WOZ + event-driven bit-cell timing (cf. `CLAUDE.md` *"disk-turbo"* + `emuCycles`). Weak/fake bits depend on the WOZ master. Pinned on the flux side: `woz_writeflux_smoke_test`, `woz_bit_timing_smoke_test`. `Gap #9`: WOZ1 splice TRK+6650. |
| **"Floating bus as RNG" disks** (Beagle Bros protections, some demos) | Use the floating-bus byte as a random seed. | Requires a **bit-exact** replication of the scanner counter (HBL included, "$1000 phantom row"). | ✅ Handled by the verbatim `floatingBus()` port (cf. comment `Memory.cpp:1572`). This is precisely the use case cited in the code. |

---

## 3. CPU & hardware quirks

The foundation must be flawless **before** the video demos can pass.

| Program | What it validates | POM2 status |
|---|---|---|
| **Klaus Dormann — `6502_functional_test`** | 6502 arbiter: page crossing (+1 cycle), exact decimal (D) flag, etc. | ✅ `test_klaus_6502` **PASSES**. Binary auto-downloaded + SHA256 verified (`tests/CMakeLists.txt`). |
| **Klaus Dormann — `65C02_extended_opcodes_test`** | 65C02 extended opcodes (BBR/BBS/RMB/SMB, `STZ`, `(zp)`, etc.). | ✅ `test_klaus_65c02` **PASSES** @ `$24F1` (cf. `DEV.md` §CPU). |
| **NMOS "illegal opcodes" suites** (visual6502-derived) | Behavior of the undocumented 6502 NMOS opcodes. | 🟢 Partially — `#1` notes a *"$5C 8-cyc residual"*. Mainly covers the subset used in practice. Complete via `cpu_cycle_count_test`. |

> ~130 `ctest`s in total (Klaus 6502+65C02, `cpu_cycle_count`, disk, video,
> audio…). Cf. `TODO.md` Quick-win #5 (headless CI GitHub Actions).

---

## 4. Audio / Mockingboard (VIA IRQ)

Stress-test of the **hardware IRQs**: the Mockingboard's VIA 6522 timers must
neither desync the main bus nor miss their acknowledgment.

| Program | What it tortures | POM2 status |
|---|---|---|
| **Ultima V: Warriors of Destiny** (Origin) | Mockingboard music driven by VIA timer IRQ continuously during gameplay. | ✅/🟡 Mockingboard A/C (2×VIA + 2×AY) verbatim (`#6`, `ay8910.cpp`, `Via6522`). Wire-OR IRQ via `SlotBus` (`#8`). To be heard in real conditions. |
| **Music Construction Set / Willy Byte / Rescue Raiders** (confirmed Mockingboard titles) | AY-3-8910 sequencing + IRQ cadence. | ✅/🟡 Same path as above. Good test bench for the accuracy of the T1/T2 timers. |
| **Phasor / SSI263 (speech)** | 2×VIA + 4×AY (Phasor), SSI263 formant synthesis. | ✅ `PhasorCard` verbatim (`#19`); AppleWin-faithful SSI263 (`#20`). |

---

## Appendix — Vapor Lock in detail

A **purely software** solution to the lack of a VBL IRQ on II/II+. The
mechanics, from the physics to the POM2 C++:

1. **Shared bus (Φ0/Φ1 interleaving).** No dedicated VRAM: the CPU (6502) and
   the video scanner share the same RAM. Within a 1 MHz cycle, the **low phase**
   serves the scanner (generates the pixels), the **high phase** serves the CPU.
   Each µs, the bus carries first a video datum, then a CPU datum.
2. **Floating bus (TTL capacitance).** When no component drives the bus (reading
   an empty I/O, e.g. the `$C050-$C05F` mirrors during VBL), the lines hold the
   **last value** for ~½ µs via parasitic capacitance (~50 pF) — the one placed
   by the scanner just before.
3. **Algorithm.** The program writes a marker pattern (e.g. an isolated `$FF`)
   into a corner of video RAM, then tight-loops reading the floating bus. As
   soon as it reads `$FF` back, it knows the **exact beam position** at that
   cycle → "locked" sync. VBL is detected because the scanner stops reading
   structured video RAM.
4. **Emulator trap.** If the duration of `ExecuteCycle()` isn't exact (forgotten
   penalty cycle of a page-crossing `BCC`/`BCS`, etc.), the CPU drifts against
   the scanner and the lock slips after a few scanlines → glitches/crash. The
   alignment must be **perfect**.

**On the POM2 side.** `Memory::floatingBus()` (`src/Memory.cpp:1561+`) computes
the scanner address from the global `cycleCounter` (65 cycles/line × 262
lines/frame), a **verbatim** port of MAME `apple2video.cpp scanner_address`.
Reads of undriven soft-switches (`floatingBus()` call sites at
`Memory.cpp:1132/1141/1192/1218/1235/1299/1339/1347`) return this byte. It is the
foundation that makes vapor lock *possible*; it remains to prove it end-to-end on
a megademo (integration test to add).

---

## Corrections vs the original source

The original conversation contained a few inaccuracies, corrected here:

- **"Megademo by Deater (Peter Ferrie)"** → **deater = Vince Weaver**. Peter
  Ferrie (aka *qkumba*) is a different person (cracks / protection analyses,
  distinct from deater's megademos). Do not confuse them.
- **"Skyfox … Mockingboard"** → *Skyfox* (Ariolasoft/EA) outputs mostly through
  the **speaker**, not the Mockingboard. Replaced with **confirmed** Mockingboard
  titles (Ultima V, Music Construction Set, Rescue Raiders, Willy Byte).
- **VBL.** The absence of a VBL IRQ applies to **II/II+**; the **//e** exposes
  `$C019` RDVBL on read (still no IRQ). Clarified in §1.
