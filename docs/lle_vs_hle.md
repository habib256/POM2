# LLE vs HLE in POM2

Where POM2 emulates the *silicon* and where it emulates the *contract* —
subsystem by subsystem, with the evidence and the reason.

This is a companion to the [MAME ↔ POM2 parity dashboard](../TODO.md#mame--pom2-parity-dashboard).
The dashboard answers *"how faithful is the port?"*; this document answers the
prior question: *"faithful to what — the chip, or the service the chip
provides?"* A subsystem can be a verbatim MAME port **and** high-level
emulation at the same time (`ImageWriter`), or POM2-original **and** low-level
(`CassetteDevice`). The two axes are independent.

## Table of contents

- [The panel in the GUI](#the-panel-in-the-gui)
- [The two axes](#the-two-axes)
- [The POM2 scale](#the-pom2-scale)
- [Master table](#master-table)
- [The interesting cases](#the-interesting-cases)
- [Where the HLE seams show](#where-the-hle-seams-show)
- [The decision rule POM2 actually follows](#the-decision-rule-pom2-actually-follows)
- [Candidates to move down the stack](#candidates-to-move-down-the-stack)
- [Orthogonal: host-side machinery](#orthogonal-host-side-machinery)

## The panel in the GUI

`Help → Abstraction Levels (LLE / HLE)` renders this document's master table
**live** (`AbstractionLevels_ImGui.*`, catalog + window; `MainWindow::
renderAbstractionPanel` supplies the machine state). Since 2026-08-30 the
window is a single table — legend chips, a filter box, four columns — with all
of this document's prose living in the row tooltips. It adds the two things a
Markdown file cannot:

- **Which level is running right now.** The "Now" column reads *live*,
  *degraded* or *not plugged* per subsystem, sourced from the card ROM-state
  accessors (`DiskIICard::usingBitLss`, `ClockCard::romFromDump`,
  `GrapplerCard::isRomLoaded`, `SmartPortCard::isLironRomLoaded`). This is the
  first of the two mitigations proposed under
  [Keeping a level once you have it](#keeping-a-level-once-you-have-it) — the
  panel says *degraded*, not merely *missing*, which is the distinction that
  was invisible.
- **The switchable boundaries, as in-row buttons.** The four subsystems that
  ship both levels — Mouse Card (L0 MAME ⇄ H1 AppleWin), ProDOS block storage
  (L2 CFFA ⇄ H1 HDV), printer interface (L2 Grappler+ ⇄ H1 synthetic) and the
  colour pipeline (L1 OpenEmulator ⇄ H1 artifact LUT) — carry a "Switch"
  column: the side in use says so, the other side offers a `use L0`-style
  button right on its row. The first three swap the card **in place, in its
  own slot** and restart the machine (the tooltip warns); the fourth is a
  render-path change and is instant. A side is greyed when the dump it needs
  is missing, with the reason in the tooltip — offering a switch that would
  silently land on the fallback would repeat the exact mistake the panel
  exists to expose.

The catalog is static data mirroring the master table below. **Edit the two
together**: the doc carries the evidence, the panel carries the conclusion.

## The two axes

**Fidelity** (the dashboard's axis) — how closely the implementation tracks its
reference: *verbatim* → *partial-verbatim* → *POM2-original* → *scaffold*.

**Abstraction level** (this document's axis) — *where* the emulation boundary is
cut:

- **LLE** — the boundary is the chip's pins. POM2 models internal state
  machines and timing; the guest's own firmware (ROM dump) runs on top and
  cannot tell the difference. Guest-visible timing is emergent, not asserted.
- **HLE** — the boundary is the *service*. POM2 intercepts a call, a register
  protocol, or a byte stream, and produces the correct *result* on the host.
  Timing and internal state are asserted, not emergent.

The failure modes differ. LLE fails by being *incomplete* (an unmodelled edge
hangs the firmware — see the //c+ IWM boot scan spinning at `$F0FC`). HLE fails
by being *out of contract* (software that pokes a register the abstraction never
had gets a plausible-but-wrong answer, and nothing hangs — it just silently
diverges).

## The POM2 scale

Five levels, from silicon to pure host function:

| Level | Name | What it means | Canonical POM2 example |
|---|---|---|---|
| **L0** | Silicon | Internal state machine + cycle timing modelled; a real ROM/firmware dump executes on top | Disk II LSS (P6 PROM), M68705 mouse MCU, IWM |
| **L1** | Chip-faithful | Full register/protocol model at bus timing; firmware-invisible internals deliberately skipped | 6522 VIA, AY-3-8910, CS8900A, uPD1990AC |
| **L2** | Real firmware, host device | The card's **real ROM executes**, but what it drives is a host implementation, not the original chip | CFFA 2.0 (real 4 KB firmware → emulated ATA taskfile → host block store) |
| **H1** | Synthetic firmware | POM2 hand-assembles a slot ROM; guest 6502 code is real, but the register protocol behind it is invented and the work happens on the host | `ProDOSHardDiskCard`, `PrinterCard`, `MouseCardAppleWin` |
| **H2** | Host function | No guest-visible hardware at all; the function happens on the host and the result appears out-of-band | `ImageWriter`, `FloppySoundDevice`, `bootFromSlot` |

L0–L1 is the LLE half; H1–H2 is the HLE half. **L2 is the interesting middle**:
authentic guest code, synthetic device. It is POM2's preferred compromise
whenever a ROM dump exists but the chip behind it does not need to.

## Master table

| Subsystem | Level | What is actually modelled | Why not lower |
|---|---|---|---|
| **6502 / 65C02 / Rockwell / WDC** (`M6502`) | **L0** | Per-cycle, 100 % of the 178 documented NMOS opcodes against Tom Harte `65x02`; WDC decimal SBC silicon-exact incl. interdigit carry | — (this *is* the floor) |
| **Z80 core** (`Z80`) | **L0** | zexdoc + zexall clean; MEMPTR/X-Y flags modelled, "both modelled, not approximated" (`Z80.h:20`) | — |
| **SoftCard Z80** (`SoftCardZ80`) | **L1** | Real DMA bus arbitration, 6502 halted per instruction slice; CP/M 2.2 boots MAME-oracle-identical | — |
| **Memory / MMU / IOU / RamWorks** | **L1** | Soft switches, aux paging, LC banks, power-on `00 FF` pattern, **per-cycle floating bus** (vapor lock) | God-object split pending; behaviour already at L1 |
| **Display** (`Apple2Display`) | **L1** | Beam-raced per-byte column reconstruction from a cycle-stamped video-event log; mid-scanline mode splits at 280/560 px | A true per-scanline incremental renderer (MAME style) is the remaining L0 step — see the unidirectional page-flip limit |
| **Composite NTSC** (`ColorCompositeOE`) | **L1** | 14.318 MHz 1-bit signal → FIR demod (Y @ 2.0, C @ 0.6 MHz) → YUV→RGB, PAL line-phase | Pure-analog IIR-on-signal deferred as academic (TODO, *5–10 d*) |
| **Artifact-colour LUT modes** (`ColorNTSC`, `ColorCompMedium`, `ColorComp4Bit`, `ColorAppleWin`) | **H1** | MAME's composite colour tables indexed per dot pattern — the *result* of NTSC artifacting, tabulated, with no signal in between | Not a defect but the other end of a deliberate pair: the OE pipeline beside it *is* the low-level one, and the two are switchable at runtime. A table shows only what it has an entry for |
| **Speaker** (`SpeakerDevice`) | **L0** | Verbatim MAME `spkrdev.cpp:74-327`: 4× oversample, 64-tap windowed sinc, 0.995-pole DC blocker | — |
| **Cassette** (`CassetteDevice`) | **L1** | Real `$C020` flip-flop / `$C060` comparator sign; the guest's Monitor loops time real zero-crossings out of a host WAV | — |
| **Mockingboard / Phasor** (`Via6522`, `Ay3_8910`) | **L1** | T1/T2, IFR/IER, port latches + DDR, CA1 edges, AY counters/LFSR/envelope | Documented skips: SR, CA2/CB1/CB2 handshake, PB6 pulse counting — no POM2 card wires them |
| **SSI263 speech** (`Ssi263`) | **L1 registers / H1 audio** | Register bank, A/!R handshake, IRQ modes and phoneme **duration** are chip-exact; the sound itself is a canned PCM blob per phoneme | The real chip is an analog formant synth; AppleWin's blob is the only extant reference (MAME has no SSI263) |
| **Echo+ TMS5220** (`EchoPlusTMS5220Card`) | **H1 (scaffold)** | Stub register decode at `$Cs00-$Cs0F`, enough for driver detection | LPC10 decoder + AY-3-8913 synth not written yet |
| **Floppy mechanical sounds** | **H2** | Host sample playback, driven by `emuCycles`-stamped phase strobes | Nothing on the bus to model — it is literally acoustics |
| **DiskImage / WOZ** | **L0** | Bit-cell / flux-transition store; `getNextTransition` verbatim MAME `floppy.cpp` | `.dsk` has no flux, so its bitstream is *reconstructed* (sync-FF padding ≥ 5) — exactly what real hardware infers |
| **Disk II** (`DiskIICard`) | **L0** | Real 341-0028-A **P6 LSS PROM** indexed per LSS cycle; real P5A boot PROM; per-drive angular position vs MAME `m_revolution_start_time` | The legacy 32-cycle nibble gate is the H1 fallback, used only when `diskii_p6.rom` is absent |
| **IWM** (`IWMDevice`) | **L0** | Verbatim MAME `machine/iwm.cpp`: `m_active`/`m_rw`/read-walker/write-window state machines | Only sub-CPU-cycle Q3 phase unmodelled |
| **SmartPortHub / Sony35Drive** | **L0/L1** | Zoned GCR, LSTRB register strobes, DSKCHG latch polarity per MAME `floppy.cpp:560/672/723` | — |
| **CFFA 2.0** (`CffaCard` + `AtaBlockDevice`) | **L2** | **Real 4 KB firmware dump executes** over an ATA taskfile model isomorphic to MAME's `cs0_r/cs0_w` | ATA layer skips DMA / IRQ / SMART; CHD backing is phase 2 |
| **HDV card** (`ProDOSHardDiskCard`) | **H1** | Hand-assembled 256 B slot ROM + an invented 4-register streaming port; `deviceSelectRead/Write` = host `memcpy`. No GCR, no flux, no ATA | Deliberate: mounts `.hdv`/`.2mg` directly with **no card ROM dump required** |
| **SmartPort card** (`SmartPortCard`, Liron-class) | **H1 + L2 veneer** | The whole 256 B slot page **and** the full 2 KB `$C800` bank come from `roms/liron.rom`; only the HLE service entries (`$Cn00-02`, `$Cn0A-12`, `$Cn20-E2`) are overlaid, routing to POM2's own `$CE00` 6502 SmartPort handler; block moves are host memcpy. Both public entries now open with the real firmware's `BIT $CFFF` — see the case study | Full Liron LLE needs the IWM bit-shifter **and** the UniDisk drive-side 65C02 — out of scope |
| **//c-class on-board SmartPort** | **H1 + machine-level lie** | A `$C500-$C5FF` hole punched through the //c's forced INTCXROM, armed only by an explicit GUI/CLI boot — **plus, since 2026-08-30, the stub's `$C800-$CFFE` expansion bank**, gated by `iicCardWindow_` (opened by a fetch in the stub's page, closed by any `$C0xx` access outside its device-select). ProDOS 8 2.4.3 boots and runs games end-to-end on `--preset iic` | Real //c masks all slot ROM; MAME models no 3.5" on plain //c. See [the case study](#the-day-the-hle-stub-met-the-memory-system) |
| **ProDOS host folder** (`ProDOSVolume`) | **H1 authoring / L0 runtime** | POM2 *fabricates* a valid ProDOS volume image once; from then on the guest does genuine block reads through the real filesystem code | The fabrication is the abstraction; nothing below it is faked |
| **Super Serial Card** | **L1 chip / H1 firmware** | 6551 ACIA register-faithful; the slot ROM is synthetic (PR#n/IN#n hooks + Pascal 1.1 ID block), real SSC ROM not shipped | Chip is right; firmware is a stub because no dump is bundled |
| **Uthernet I** (`Cs8900aDevice`) | **L1** | Verbatim MAME `machine/cs8900a.cpp` (VICE lineage), packet-level | RX is pull-mode — POM2 has no `device_network_interface` push bus |
| **Uthernet II** (`W5100Device`) | **L1 — and see below** | Register/socket model per AppleWin + WIZnet datasheet; each W5100 socket owns a real host BSD socket | **The chip is itself an offload engine** — host sockets *are* the faithful model, not a shortcut |
| **FujiNet card** (`FujiNetCard`) | **H1 + relay** | Synthetic 256 B slot ROM whose only job is to trap into the host; every SmartPort call is forwarded verbatim to a real FujiNet over SP-over-SLIP (loopback TCP, or USB CDC-ACM to a physical board) | Nothing below the protocol exists to model — **the device is real and off-box**. MAME has no FujiNet device, so the source of truth is the published spec + the FujiNet AppleWin fork |
| **Network transport** (`NetworkBackend`) | **H2** | Null / Loopback / libslirp user-mode NAT | Outbound-only by design: no root, no TAP/pcap |
| **Clock card** (`ClockCard`) | **L2** | uPD1990AC bit-bang state machine per MAME `upd1990a.cpp`, driving the **real Thunderware Rev 1.3 EPROM** — `roms/thunderclock_u9_v1.3.bin` is in-repo and `tryLoadDump()` runs from the ctor (`ClockCard.cpp:89`), 2 KB mirrored into `$C800-$CFFF`. Synthetic ROM is the fallback only | Already there. The dump even settled the 40-bit-vs-48-bit shift-register question by disassembly (`$CACF` emits 4 CLK × 10 = 40) |
| **No-Slot Clock** (`NoSlotClock`) | **L1** | Full DS1216E SmartWatch 64-bit pattern-match state machine on `Memory::interceptRead` | — |
| **Printer card** (`PrinterCard`) | **H1** | Synthetic ROM whose entire job is the PR#n CSWL/CSWH hook + a 4-byte trampoline; the data port spools to a `std::vector` | No PROM dump; Pascal entry block deliberately absent (`PrinterCard.h:35`) |
| **Grappler+** (`GrapplerCard`) | **L2** | **Real 4 KB Orange Micro EPROM executes**; status byte, register decode, `$C800` banking, S1 DIPs line-cited against MAME `grappler.cpp` | `/STROBE` 7-clock pulse collapsed to instant — the synthetic printer consumes at latch time, so no observer exists |
| **ImageWriter II** (`ImageWriter` — the class now covers `IwModel` {ImageWriterII, ImageWriterI, AppleDMP, EpsonFX80}) | **H2** | Host-side printer: full control language, 4-band ribbon, 8/24-pin bit images, PNG/PDF export. **Not a bus device at all** | There is no Apple II hardware here to emulate — the printer sat on the far side of a cable |
| **Mouse card — MAME** (`MouseCard`) | **L0** | **M68705P3 MCU executing its real 2 KB mask ROM** at 2× CPU clock + MC6821 PIA + quadrature edge generation | Only the PAL16R4 chip-select sequencer is skipped (firmware-invisible) |
| **Mouse card — AppleWin** (`MouseCardAppleWin`) | **H1** | Same slot EPROM, but the MCU is a C++ command-byte state machine (`$00 SET` … `$90 TIME`); position copied from the host delta | Ships *because* the MCU mask ROM is not always available |
| **Joystick / paddles** | **L1** | Real `$C070` RC discharge timing sampled at `$C064-$C067` bit 7 | — |
| **Le Chat Mauve** (`LeChatMauveCard`) | **L1** | AN3 pulse FIFO decode (real register state machine) + AppleWin `RGBMonitor.cpp` pixel rules; Eve Color text (`$C0B8/9`) and HGR Duochrome (`$C0BA/B`) decoded (`LeChatMauveCard.cpp:48-58`, rendered by `Apple2Display::renderTextChatMauveFgBg`, snapshot v2) | — |

## The interesting cases

### The mouse card is POM2's own controlled experiment

POM2 ships **both** implementations of the same card, selectable in Slot
Config: `mouse` (L0, M68705 mask ROM executing) and `mouseaw` (H1, MCU replaced
by a C++ state machine). They share the slot EPROM, the `SlotPeripheral`
plumbing and the `setHostMouse(rawX, rawY, button)` entry point. The only
difference is where the cut is made.

That makes the trade-off measurable rather than theoretical:

- The L0 path **decodes real quadrature edges** — at most one edge per axis per
  MCU PortB read, matching MAME's `m_last`/`m_count`. Fast host motion is
  therefore rate-limited exactly as the real hardware limits it.
- The H1 path **copies the host delta** into the HLE'd MCU's `iX/iY` (see
  `CHANGELOG.md`), so it never drops motion — and needs a
  compensating absolute closed-loop cursor sync in `MainWindow` that the L0
  path does not need at all (`MainWindow.cpp:5087-5135`).

The HLE variant is *smoother* and *less correct*. It exists for one reason: the
`mouse_341-0269.bin` MCU dump is not always available, and a user with only the
slot EPROM should still get a working mouse. That is the whole HLE bargain in
one card.

### Uthernet II: HLE-looking, but actually faithful

The W5100 is **not a NIC** — it is a TCP/IP offload engine. The guest never
builds an IP header or runs a retransmit timer; it writes an address and a port
into registers, issues `CONNECT`, and pushes payload at a ring buffer. Mapping
that onto host BSD sockets is not a shortcut *around* the hardware, it is a
transcription *of* the hardware (`W5100Device.h`, "Why this is NOT a
packet-level model").

The payoff is concrete: the Uthernet II needs **no Ethernet backend at all**.
Period IRC, telnet and FTP clients work on any machine without privileges or
libslirp. Contrast the Uthernet I, whose CS8900A really is a packet-level NIC
and therefore really does need a transport.

**Lesson**: "this looks like HLE" is sometimes just "this chip's own
abstraction level is high". Judge the boundary against the datasheet, not
against intuition.

### Storage is where the split is sharpest

The same emulator holds both extremes, roughly one slot apart:

- **Disk II** — L0. Real P6 LSS PROM, flux transitions, per-drive angular
  position, sync-FF resync. Protected WOZ images with weak bits work because
  nothing is abstracted away.
- **HDV card** — H1. A hand-assembled ROM and an invented 4-register port that
  `memcpy`s 512-byte blocks. No GCR, no flux, no ATA.

Both are correct choices. The 5.25" corpus is *full* of copy protection that
reads the bitstream directly, so anything above L0 loses titles. The ProDOS
block corpus has no protection worth the name and no bundled firmware dump, so
H1 costs nothing observable and buys direct `.hdv`/`.2mg` mounting that MAME
(CHD/raw only) does not offer.

`CffaCard` was then added *beside* the HDV card rather than replacing it — the
L2 option, for users who want real firmware. Both implement
`pom2::ProDOSBlockCard` so the Library, disk-turbo and persistence target them
uniformly. **Offering both levels is a legitimate outcome**, not a failure to
decide.

### The synthetic-ROM family

`ProDOSHardDiskCard`, `PrinterCard`, `SmartPortCard` (partly), `ClockCard`
(fallback) and `SuperSerialCard` all hand-assemble 6502 into a slot ROM. This is
POM2's house style for H1, and it has a consistent shape:

1. Satisfy the **detection contract** first — the JSR dispatch trio
   `$Cn01/03/05 = $20/$00/$03`, `$Cn07` device class, the Pascal 1.1 signature
   at `$Cn05/07/0B/0C`. Get this wrong and the guest's scanner misclassifies the
   card (the `$3C` vs `$01` bug made the //c treat slot 5 as a second Disk II).
2. Expose a **documented, minimal register protocol** on `$C08n+slot×16`, written
   down in the header next to the code that implements it.
3. **Pin it with a smoke test** that drives the ROM from a real CPU, not just
   the C++ API — `printer_card_smoke` runs an actual `PR#1` + three COUT writes.

Step 3 is what keeps HLE honest here: the test executes guest code through the
synthetic firmware, so the contract is verified from the guest's side of the
boundary.

### The day the HLE stub met the memory system

The 2026-08-30 //c dig is the sharpest lesson this document owns, so it gets
told in full. Setup: the armed `$C500` stub (H1) under the most demanding real
guest there is — the ProDOS 8 2.4.3 kernel — on the most hostile memory map —
the //c's forced INTCXROM. Three defects were stacked, each masking the next,
and only the first was host-side:

1. **The stub's own `$C800` bank was invisible.** The //c hole covered
   `$C100-$C7FF` only, but the stub's driver JSRs into `$CD00`/`$CE00` — its
   expansion bank. The 6502 executed the //c's *internal* ROM as if it were
   the SmartPort handler. Lesson: **an HLE ROM's footprint is not just its
   slot page.** If the synthetic firmware follows the real firmware's
   architecture (and it should — see the `BIT $CFFF` below), the machine
   model must serve every region that architecture touches.

2. **The root cause: a missing `BIT $CFFF`.** The stub's SmartPort entry
   (`$Cn0D`) opened with the real Liron's `BIT $CFFF` dance; its ProDOS entry
   (`$Cn0A → $Cn50`) did not. The kernel calls that entry *after* using the
   80-column firmware — which latches INTC8ROM on every `$C3xx` pass. With
   the internal window locked, the driver's own subroutines executed internal
   ROM again; the kernel's //c port scan wandered, its `$FExx` config block
   described one device, the `$EE82` installer transcribed one entry, and the
   first MLI call of the loaded program died in `RESTART SYSTEM-$0A` — the
   `$E1C2` dispatch hunting the boot unit in an empty table, three layers and
   forty thousand instructions away from the cause.

   The service contract was **perfect** the whole time: every register
   answered right, the STATUS response was verified byte-for-byte in the
   caller's buffer. What was violated was the machine's **bus etiquette** —
   the claim/release discipline of the shared expansion window. That
   discipline is an LLE property, and HLE firmware living on a real bus has
   to speak it. *An HLE boundary is not a permission slip to ignore the
   contracts below it; it only changes which side of the boundary does the
   work.*

3. **The detection story vindicates the doctrine below.** The pinned smoke
   test poked the card's registers from C++ and stayed green throughout —
   the classic API-side test that "passes either way". The defect only
   became visible to a harness that **executes the boot from the guest's
   side**: force `PC = $C500` on a real `M6502` + `Memory`, run, and assert
   block 0 lands at `$0800`. That harness (plus write-watchpoints on the
   kernel's tables and a PC-ring tracer, kept in the pinned test behind
   `POM2_TRACE_HDV`) walked the causal chain link by link. The doc said it
   before the bug proved it: *a test that exercises the firmware from the
   guest's side is the only kind that can tell.*

The fix itself is three bytes of 6502 (`BIT $CFFF` first thing in the driver,
paid for by turning the dispatch's `CMP #$00` into a direct `BEQ` and
reclaiming a pad byte) plus one honest new heuristic in Memory —
`iicCardWindow_` — whose price is listed in the seams table below.

## Where the HLE seams show

Every one of these is documented in-repo. They are the price list.

| Seam | Consequence |
|---|---|
| HDV `$Cn07 = $01` | F8 Autostart only scans `$3C` — HDV needs `PR#n` / `bootFromSlot` |
| HDV / SmartPort synthetic block model | Real CFFA/SCSI firmware cannot execute; multi-partition CFFA3000 images unsupported |
| //c on-board SmartPort "armed" gate | Persisted SmartPort media does **not** auto-reboot; the stub must stay hidden during the //c ROM's own autostart or the banner garbles. And while armed, the stub **shadows the //c's internal `$C5xx` firmware** for the whole session — a program calling the internal port-5 serial/AppleTalk entries would reach the stub instead |
| `iicCardWindow_` execution-flow heuristic | The stub's `$C800` bank is served only while the flow "came from" the stub's page (opened by a `$C5xx` fetch, closed by any foreign `$C0xx` access). An interrupt handler touching soft switches in the middle of a driver call would close the window under the running driver — no IRQ source is armed in the supported //c flows, but the risk is asserted, not emergent |
| `SmartPortCard` CONTROL calls | Only code 0 works — the stub has no guest→device control-list copy; extended `$4x` calls return `$01` |
| Grappler `/STROBE` collapsed | The 7-clock pulse timer is invisible; anything timing the strobe would see zero width |
| `PrinterCard` Pascal block absent | Pascal printer drivers (PINIT/PREAD/PWRITE/PSTATUS) cannot bind — BASIC `PR#n` only |
| `MouseCardAppleWin` delta copy | No quadrature rate limit; needs a compensating cursor sync the L0 card does not |
| SSI263 phoneme blob | Arbitrary formant/filter sweeps outside the 62-phoneme set are not reproducible |
| ImageWriter character ROMs | Not chip dumps: the dot patterns Apple **published** in the ImageWriter/II Technical Reference Appendix C, transcribed by mikedaley/web-a2e (MIT) and re-generated into `src/ImageWriterRom.h`. Same provenance class as the SSI263 blob — the transcription is MIT, the typeface design is Apple's. Codes outside the tables fall back to POM2's bundled CP437 font |
| `ClockCard` synthetic ROM | No `$C800` driver load for tools that pull the driver off the card |
| Uthernet II `LISTEN` unimplemented | A direct consequence of mapping onto host sockets with no bind path — no inbound connections |
| FujiNet relay: **no peer** | A third failure category, distinct from LLE-incomplete and HLE-out-of-contract: the device is *elsewhere*. Absent or slow shows up as a bounded stall (250 ms) then SmartPort `$27`; the boot path falls back to continuing the slot scan |
| FujiNet relay: **rewind does not rewind** | The peer's clock never moves backwards. Blocks it wrote stay written, HTTP requests stay made; the card only resynchronises its sequence number on snapshot load |
| FujiNet relay: not on //c-class | Forced INTCXROM masks slot ROM; the real //c wires FujiNet to the disk port instead, which needs the on-board `$C500` path |
| `bootFromSlot` | Labelled a "synthetic shortcut" in `EmulationController.h:154`: cold boot + forced `PC = $Cn00` after validating the JSR trio. No real firmware scan happens |

And the mirror-image failure, worth keeping in view: the **//c+ IWM 3.5" boot**
is the one place where LLE is *present but incomplete*. The IWM itself is
verbatim MAME, yet the firmware's Sony boot path never reaches a bootable disk
because the full bit-shift state machine plus the UniDisk drive-side 65C02 are
missing. The HLE path (host-served SmartPort at slot 5) is what actually boots
3.5" and HDV on every //c-class profile — though "complete" had to be earned:
it took the 2026-08-30 case study's three fixes before ProDOS itself would
finish booting on the //c. **Half an LLE hangs; an HLE only works once it is
actually complete — including the bus etiquette.**

## The decision rule POM2 actually follows

Read off the codebase rather than declared in advance, the policy is:

1. **Is there a public ROM/firmware dump?** If no → HLE, full stop. This is the
   binding constraint far more often than difficulty: `SmartPortCard` (Liron ROM
   was undumped when written), `PrinterCard` (no PROM), `SuperSerialCard`
   firmware, `ClockCard` ROM, `MouseCardAppleWin` (user may lack the MCU dump).
2. **Does MAME (or AppleWin) model the chip?** If no, POM2 does not invent an
   LLE model from scratch — it ports the best available behavioural reference
   and says so: SSI263 (AppleWin, MAME has none), W5100 (AppleWin, MAME has
   none).
3. **Does the corpus depend on sub-protocol behaviour?** 5.25" copy protection
   reads raw bitstreams → L0 mandatory. ProDOS block I/O does not → H1 is free.
4. **Is there anything on the bus at all?** If the function lives past the
   connector (printer output, drive acoustics, PDF export) → H2 without
   apology.
5. **Whatever the level, write the contract down and pin it.** Every HLE
   subsystem in the master table has its register protocol in the header and at
   least one smoke test under `tests/`. That is what makes an HLE decision
   auditable later instead of load-bearing folklore.

Corollary, visible in the CFFA/HDV pair and the mouse pair: **when both levels
have real users, ship both** behind a common interface. It costs one abstraction
(`ProDOSBlockCard`, `SlotPeripheral`) and removes the need to guess.

## Candidates to move down the stack

Ordered by how much the gate has changed since the original decision.

| Candidate | Current | Target | Gate |
|---|---|---|---|
| **Liron / UniDisk 3.5** | ~~H1~~ **H1 + L2 veneer — ROM half done** | L0 | The dump has landed in-repo: `roms/liron.rom` (BMOW/Yellowstone `LIRONALL.bin`, 4 KB) is loaded by `SmartPortCard::loadLironRom`, so the slot page and `$C800` bank are the real firmware. Remaining: the IWM bit-shifter in a slot + the UniDisk drive-side 65C02 firmware. Deliberately out of scope, but no longer blocked on sourcing |
| **SSC firmware** | H1 synthetic ROM | L2 | The real SSC ROM (341-0065-A) is publicly dumped and disassembled (6502disassembly.com/a2-rom/SSC — already POM2's reference for the Pascal ID block). The 6551 underneath is already L1, so this is a sourcing + wiring job, not a modelling one — the same shape as the ClockCard move that already landed |
| **ClockCard slot ROM** | ~~H1 fallback~~ | **L2 — done** | The dump is in-repo and loads from the ctor. Residual: `clock_card_smoke` tolerates its absence (CI-safe), so nothing *fails* if the real path silently stops being taken — see the degradation hole below |
| **Echo+ TMS5220** | H1 scaffold | L1 | TMS5220 LPC10 decoder (chirp ROM + K-parameter interpolation) + AY-3-8913 synth, once the Mockingboard/Phasor AY core is extracted into a shared helper. *~3–5 d* |
| **CFFA CHD backing** | L2 (raw LBA) | L2+ | Phase 2; the ATA layer is already isomorphic to MAME's |
| **Display per-scanline incremental** | L1 beam-raced | L0 | Would fix the documented unidirectional mid-frame page-split limit (renders full-page today) |
| **Composite analog IIR** | L1 (1-bit + FIR) | L1+ | Marked academic in TODO, *5–10 d* |
| **SSI263 formant synth** | H1 audio | L1 | No reference implementation exists anywhere; would be original DSP work |

### Keeping a level once you have it

Reaching L is a one-off cost; **staying** at L is a standing one, and POM2 has a
structural hole here worth naming.

Every ROM-driven L path in the table degrades **silently** to a lower level when
its dump is absent: Disk II drops to the legacy 32-cycle nibble gate without
`diskii_p6.rom`, the mouse falls back from L0 to the H1 `mouseaw`, `ClockCard`
falls back to its synthetic ROM, `GrapplerCard` to `buildStubRom()`. That is
correct product behaviour — the user still gets a working machine. But it means
**the L path can stop being exercised without anything failing**.

CI does not cover the gap either: several dumps are in fact git-tracked and
used by tests (`diskii_p6.rom`, the mouse ROMs, `liron.rom`,
`grappler_plus.bin`, the ThunderClock EPROM), but the real guard in the
182-test ctest gate is that tests **SKIP rather than fail** when a dump is
absent (e.g. `tests/mouse_card_axis_parity_test.cpp:69-72`) — so nothing
asserts the real-ROM path is taken, and exactly the paths that define the L
levels can degrade unnoticed. `clock_card_smoke` is explicit about this ("the
ctor loads the dump *when the user has it*").

Two cheap mitigations, neither implemented:

- Have the ROM Status panel report **degraded** rather than merely *missing* —
  "running the synthetic ROM" is a different state from "card unavailable", and
  only the first is invisible today.
- Add an opt-in CI lane (or a local `ctest -L rom`) that asserts the real-ROM
  path is taken when the dumps *are* present, so a regression that quietly
  routes to the fallback fails somewhere.

The pattern to copy is `mouse_card_axis_parity_test`: it boots **both** real
ROMs on a full `M6502` + `Memory` and drives ProDOS `InitMouse/SetMouse/
ReadMouse`. A test that exercises the firmware from the guest's side is the only
kind that can tell L0 from H1 — a C++-API test passes either way.

That doctrine stopped being theoretical on 2026-08-30: the //c SmartPort smoke
had poked the stub's registers for weeks and stayed green while the *executed*
boot path was broken at two different layers (see the case study). The test now
also forces `PC = $C500` on a real CPU and asserts block 0 lands — and keeps
the full dissection harness (PC-ring tracer, table watchpoints) armed behind
`POM2_TRACE_HDV` for the next dig.

Explicitly **not** moving: `ImageWriter` (nothing to emulate), `PrinterCard`
(no PROM), `ProDOSHardDiskCard` (H1 *is* the feature — direct `.hdv`/`.2mg`
mounting), Apple II SCSI (`a2scsi.cpp` port is a *~30–50 h* lift for a need
CFFA already covers).

## Orthogonal: host-side machinery

These are neither LLE nor HLE — they have no hardware referent at all, and
should not be judged on this axis. Listed so the taxonomy is exhaustive:

- **Rewind / snapshot** (`RewindBuffer`, `MachineSnapshot`) — keyframes + XOR
  deltas at frame boundaries. Notable for how much *hardware* state it has to
  reach into to stay coherent: VIA + AY + SSI263 so music and speech survive a
  rewind, `DiskIICard` nibble track buffers so disk writes are undone.
- **Disk turbo (~60×) and the MAX speed button** — pure host pacing. This is
  precisely why the codebase mandates `emuCycles` stamping everywhere: turbo
  collapses wall-clock gaps to zero across an audio-buffer tick, so any device
  that reasoned in wall-clock would break.
- **AI Control HTTP API** (`AiControlServer`, `127.0.0.1:6503`) — out-of-band
  agent channel.
- **CRT effect stack, 3D voxel view, HGR/DHGR Paint editor** — presentation and
  authoring layers above the framebuffer.
- **Kiosk mode, CLI, profiles** — host-side orchestration.

---

*Cross-references: [`TODO.md`](../TODO.md#mame--pom2-parity-dashboard) for
fidelity-per-subsystem, [`DEV.md`](../DEV.md) for the per-subsystem deep dives
cited throughout, [`docs/test_corpus.md`](test_corpus.md) for the software that
exercises the low-level paths (DIX first).*
