# Printer emulation, round two — more heads, and the LaserWriter

Follow-on to [`printer_plan.md`](printer_plan.md), which shipped the
ImageWriter faces, the screen dump, the three C. Itoh heads and the Epson
FX-80. That plan closed with four printers emulated. This one asks the
question the Grappler+ was already asking: **the card in slot 1 models a
seven-position printer-type DIP switch, so what are the other positions?**

**Status: four of five items shipped.** The Apple II Workstation Card is
specified here and NOT implemented — see [§ 5](#5-the-apple-ii-workstation-card--not-implemented),
which says exactly what it is blocked on.

## Table of contents

- [1. What the hardware actually supported](#1-what-the-hardware-actually-supported)
- [2. Epson generations and C. Itoh cousins ✅](#2-epson-generations-and-c-itoh-cousins-)
- [3. The LaserWriter, palier 1 — Diablo 630 ✅](#3-the-laserwriter-palier-1--diablo-630-)
- [4. The LaserWriter, palier 2 — PostScript by delegation ✅](#4-the-laserwriter-palier-2--postscript-by-delegation-)
- [5. The Apple II Workstation Card — NOT implemented](#5-the-apple-ii-workstation-card--not-implemented)
- [6. Still proposed: the HP LaserJet](#6-still-proposed-the-hp-laserjet)
- [7. What changed against the plan](#7-what-changed-against-the-plan)

## 1. What the hardware actually supported

Two distinct questions hide inside "what printers did the Apple II support",
and keeping them apart is what makes this plan tractable.

**Parallel, through the Grappler+.** `GrapplerCard::PrinterType` already models
the real S1 DIP: Epson, NEC 8023 / C. Itoh 8510 / DMP 85, Star Gemini, Anadex,
Okidata 82A/83A/92/93, Apple Dot Matrix, Okidata 84. Those seven positions are
the families the Grappler firmware could emit graphics dumps for. Before this
plan only two of them (Apple Dot Matrix, Epson) matched a head POM2 actually
had; the rest produced the wrong-DIP garbage a real desk would show — correct,
and frustrating.

**Serial, through the Super Serial Card.** A different population entirely, and
the one the LaserWriter belongs to. POM2 already carries it: the SSC's printer
tap spools bytes with the same `drainSpoolFrom` shape a parallel card uses, and
`PrinterCoordinator` consumes it. **The transport for a serial printer was
already wired before this plan started** — which is most of why the LaserWriter
turned out to be affordable.

| Printer | Lineage | State |
|---|---|---|
| ImageWriter II / I, Apple DMP | C. Itoh | shipped in plan 1 |
| Epson FX-80 | ESC/P | shipped in plan 1 |
| C. Itoh Prowriter 8510A, NEC PC-8023A | C. Itoh | **shipped here** |
| Epson MX-80, MX-80 Graftrax+, RX-80 | ESC/P | **shipped here** |
| Apple LaserWriter (Diablo 630 mode) | Diablo | **shipped here** |
| Apple LaserWriter (PostScript) | delegated | **shipped here** |
| Star Gemini 10X | ESC/P + Star extensions | proposed |
| Okidata 82A/83A/92/93, 84 | Okidata + "Step II" graphics | proposed |
| Anadex DP-9500 | own | not worth it |
| HP LaserJet | PCL | proposed, [§ 6](#6-still-proposed-the-hp-laserjet) |
| Apple Silentype / Scribe | own interface card | out of scope |

## 2. Epson generations and C. Itoh cousins ✅

**The cousins are a table row each.** The Prowriter 8510A is the mechanism
Apple rebadged as the DMP, and the NEC PC-8023A is the same mechanism under
NEC's badge — which is precisely why the Grappler groups them on one DIP
position. They share the DMP's banks because they *are* the DMP's banks; what
differs is documented and pinned (the DMP's Apple-imposed firmware gaps, the
quoted carriage rate). Carried as separate rows rather than aliases, for the
reason the IW-I/DMP banks already are: a future divergence should land in the
table, not silently inherit.

**The Epson generations needed a mechanism, not just rows.** ESC/P grew feature
by feature across MX → RX → FX, so `IwModelProfile` gained `escPFeatures` and
the parser gates on it:

| | MX-80 | MX-80 Graftrax+ | RX-80 | FX-80 |
|---|---|---|---|---|
| `ESC K` 60 dpi | ✅ | ✅ | ✅ | ✅ |
| `ESC L/Y/Z` | — | ✅ | ✅ | ✅ |
| `ESC *` generic density | — | — | ✅ | ✅ |
| italics `ESC 4/5` | — | ✅ | ✅ | ✅ |
| master select `ESC !` | — | — | ✅ | ✅ |
| scripts `ESC S/T` | — | ✅ | ✅ | ✅ |
| proportional | — | — | — | ✅ |

A command the fitted head lacks is treated exactly like an unknown ESC —
dropped with its ESC, following bytes printing as text. That is what the
firmware did, and POM2 already reproduces wrong-DIP garbage on purpose
(`GrapplerCard::PrinterType`), because a driver aimed at the wrong head
*looking* wrong is the diagnosis.

**Graphics was the exception, and needed real work.** Dropping `ESC *` while
its data bytes still streamed would print a screenful of them, so `BitGraph`
gained a `swallow` flag. A density of 0 could not express this: `dotW =
1/horizDens` goes infinite and `fillDots` clamps that to a filled row.

**The screen dump had to follow.** It emitted `ESC *` unconditionally for any
Epson head — which an MX-80 prints as text. `buildScreenDumpEpson` takes the
command now, and the panel picks it from the fitted head's mask. This is the
kind of consequence a capability model surfaces and a bag of enum values
hides.

## 3. The LaserWriter, palier 1 — Diablo 630 ✅

**The historical key.** The LaserWriter's back-panel switch did not only offer
PostScript: it carried a **Diablo 630 emulation**, the daisywheel command set
of the era, so unmodified software could print text. Most Apple II word
processors had a 630 driver. That is how an Apple II actually got pages out of
one without speaking PostScript, over the serial port — the transport POM2
already had.

**It is a third parser over the same mechanism, not a new subsystem.** This was
planned as a new `LaserWriter.h/.cpp` with its own 300 dpi canvas and revised on
contact with the code: a Diablo 630 is fixed-pitch text on a page with no
graphics at all, and `ImageWriter`'s mechanism — page, plotter, margins, paper,
pacing, PDF export, print history — is already the right one. It is the FX-80's
situation exactly, and the FX-80's answer generalises. So `escP` became an
`IwLineage` enum: two lineages fit in a bool, three do not, and the bool was
already lying about what it meant.

**What is genuinely different is the model of motion.** A daisywheel has no
pitch and no line spacing; it has an HMI and a VMI, indices in 1/120 and 1/48
inch that the driver sets explicitly. Those land straight in the mechanism's
existing `hmi_`, which `printCharInternal` already documents as "the guest
saying move exactly this far, which outranks the font". No parallel state was
needed.

**Backspace overstrike falls out for free.** The 630 had no bold wheel: a driver
printed the character, backed up one HMI and printed it again. The page model
ORs ink, so the overstrike simply emerges.

**Deliberately conservative on the grammar.** Only unambiguous encodings are
decoded; everything else falls through to "unknown ESC, dropped with its ESC",
which is what the firmware did. Guessing a parameter COUNT wrong does not lose
one command — it desynchronises the rest of the job. And the LaserWriter's
emulation was a subset of the real 630 anyway, so a conservative subset is
closer to the machine than an eager one. `ESC FF` (form length) and `ESC L`
(bottom margin) are named in the source as the two left out, with the reason.

**The face is a substitute and says so.** A real LaserWriter set Courier from
its own font ROM, which POM2 has no dump of. Same honesty as the CP437 fallback
the ImageWriter ROM banks replaced: the shape of the page — pitch, margins,
motion — is right, the letterforms are borrowed.

## 4. The LaserWriter, palier 2 — PostScript by delegation ✅

**Do not write an interpreter.** PostScript is not a command set. It is a
Turing-complete stack language with a full graphics model: paths and Bezier
flattening, even-odd and non-zero winding fills, clipping, halftones, and Type 1
fonts that arrive encrypted and hinted. An interpreter would be larger than the
whole rest of POM2's printer subsystem and would never be faithful — and the
failure mode of an almost-right one is a page that is *subtly* wrong, which is
worse than no page.

**So run somebody else's.** `ChildProcess` supervises Ghostscript exactly as it
supervises the FujiNet helper — the pattern was already in the tree.

**Licensing, stated where it can be found.** Ghostscript is AGPL and POM2 is
GPLv3. Running a **separate process is not linking**, so this is an optional
runtime dependency rather than a derived work: POM2 ships nothing of
Ghostscript, detects it at runtime, and degrades to a message naming the Diablo
mode as the alternative. `PostScriptRender.h` says not to turn this into a
library binding without revisiting that.

**PGM, not PNG.** `-sDEVICE=pgmraw` is a five-token ASCII header and raw bytes —
thirty lines to parse. Asking for PNG would mean carrying a PNG *decoder* to
read back what POM2 only ever writes.

**`-dSAFER` is not optional.** The job comes from emulated software, and
PostScript can open and delete host files.

**Off the UI thread.** `PostScriptSpooler` owns a guarded worker and a mailbox.
Rendering inline would freeze the window the way mounting an image under
`stateMutex` used to freeze the machine. It joins its worker outside its own
mutex — the same shape, and the same reason, as `SpOverSlipLink::stop()` — and
`reset()` waits for a render rather than abandoning an unreaped child.

**A serial line has no end-of-file**, so end-of-job is the Ctrl-D drivers send,
with a 1.5 s idle flush for the ones that simply stop. Bytes after the Ctrl-D
start the next job rather than being lost.

**`adoptRenderedPage` is the seam.** The page model was always an intensity ramp
(`indexToRgb`) and merely had no source of greys before, so anti-aliased
PostScript text keeps its edges instead of being thresholded to one bit.

## 5. The Apple II Workstation Card — NOT implemented

This is the honest part of the plan. The card is specified here; it is not
built, and the reason is not scope.

**What it is.** The Apple II Workstation Card put a IIe on LocalTalk so it could
reach an AppleShare server and the LaserWriters hanging off the same net —
which is how a LaserWriter was normally connected, the serial port being the
fallback. On board: a **Zilog 8530 SCC** doing LocalTalk's SDLC framing, and a
ROM carrying the card's half of the AppleTalk stack.

**What it needs, in order:**

1. **The card's ROM dump.** ~~Not in `roms/`~~ — **a dump has since been
   identified** (see [§ 5.1](#51-the-341-0358-a-dump)), which lifts this one.
   It mattered because POM2 could not synthesise around it the way it does for
   SmartPort: there the firmware entry contract is *published* in the Apple II
   Reference Manual, so real ProDOS drivers call a documented dispatch.
   AppleTalk has no equivalent published slot-firmware calling convention —
   guest software reaches the card through the 8530's registers and through
   Apple's own ROM entry points, so a synthesised interface would be one no
   actual Apple II software knows how to call.
2. **An 8530 SCC device.** A MAME port, per the project's convention of citing
   the MAME file and line range and pinning with a smoke test. Substantial but
   bounded, and independently useful — the Mac and IIgs serial ports are the
   same chip.
3. **The protocol stack above it**, to whatever depth the goal needs. For
   reaching a LaserWriter that is LLAP → DDP → ATP → **PAP** (Printer Access
   Protocol); for AppleShare it is also NBP and ASP. Note that on real hardware
   much of this lives in software the guest loads from disk, not in the card —
   so the emulation seam is genuinely at the chip, not at the protocol.

**What it would buy, given palier 2 already shipped:** an *alternative
transport* for the same PostScript that the serial path already carries. That
is the honest accounting — the LaserWriter works today over the SSC, and this
would make it work the way most sites actually wired it.

**Recommended shape when the ROM is available:** LLE at the chip (2), real ROM
(1), and let the guest's own AppleTalk software drive it — matching what
`docs/lle_vs_hle.md` says about picking a level. An HLE at the protocol layer
is the tempting shortcut and is the wrong one here, for the reason in (1).

### 5.1 The 341-0358-A dump

A 64 KiB dump (sha1 `59c8e8c8…`, crc32 `0x63819dcb`) has been identified as
this card's firmware. The identification is from the CONTENTS, not from a
part-number database — which is the stronger evidence, and worth writing down
so a future reader can re-check it rather than trust it:

| Evidence | Offset | What it establishes |
|---|---|---|
| `STA $C080,X` / `STA $C08F,X` | several | Slot-card firmware. `$C080,X` with X = slot×16 is *the* Apple II device-select idiom; the sixteen registers behind it are where the 8530 sits. |
| `THOMAS EAGER WROTE THE LAP DRIVERS` | `0x0DB7F` | **LAP = LocalTalk Link Access Protocol.** This ROM carries the link layer. |
| `Apple //e Boot` | `0x0D5A8` | The netboot path — a //e booting from an AppleShare server, which is what this card was for. |
| `%%IncludeProcSet IWEm 1 1` | `0x04A1D` | A LaserWriter print path. `IWEm` is the **ImageWriter Emulator** procset: the card sends PostScript that asks the LaserWriter to render an ImageWriter-shaped page. |
| 8 × 8 KiB banks, all distinct | — | 64 KiB of real content, banked — no mirroring, so the whole dump is live. |
| No Pascal 1.1 signature, no ProDOS block trio, at any 256-byte alignment | — | The `$Cn00` page is not a plain slice of this image; the banking scheme has still to be worked out. |

That last row is the honest one: identifying the dump is not the same as
knowing how the card maps it, and the mapping is the first thing an
implementation has to establish.

**What remains after the ROM.** Item (2), the 8530 SCC, is untouched and is the
substantial piece. POM2's convention is that a hardware port cites its MAME
file and line range and is pinned by a smoke test; that convention wants the
MAME source at hand rather than a datasheet reconstruction from memory, and
getting it is the next concrete step. Item (3) then largely takes care of
itself for the LocalTalk case — the LAP driver is in this ROM, and the layers
above it are software the guest loads, so the emulation seam really is the
chip.

## 6. Still proposed: the HP LaserJet

Not started; recorded so the analysis is not lost.

The LaserJet was parallel, so it *did* hang off a Grappler+ — but the Grappler
had no DIP position for it, so its graphics dumps produced text garbage on one,
which POM2 would reproduce for free. In text it worked with no driver at all;
serious software (AppleWorks 3.0, Publish It!) drove it with real PCL.

PCL is a fourth lineage and, unlike the Diablo, genuinely does not fit the
matrix mechanism: a LaserJet composes a whole page at 300 dpi and ejects it on
`ESC E` or a form feed. Phases would be PCL text (`ESC E`, orientation,
margins, pitch) then PCL raster (`ESC *t#R`, `ESC *r#A`, `ESC *b#W` with
compressions 0/1/2), which is what the AppleWorks driver actually emitted.
Downloadable soft fonts are not worth implementing — consume `ESC )s#W` and
`ESC (s#W` cleanly instead.

## 7. What changed against the plan

**The LaserWriter did not need its own files.** Planned as `LaserWriter.h/.cpp`
with a separate 300 dpi canvas; delivered as a third parser over the existing
mechanism. Reading `printCharInternal` is what settled it — `hmi_` was already
exactly a Diablo HMI, and everything below the command layer was already right.
The separate-canvas plan would have duplicated the paper, the PDF export and the
print history to gain nothing.

**`escP` had to become an enum.** A bool for "which of two grammars" does not
survive a third, and it was already misnamed for what it decided.

**A capability mask is not free.** `escPFeatures` looks like pure data until a
gated command has a BODY: `ESC *`'s data bytes had to be swallowed rather than
dropped, and the screen-dump builder had to learn which command the fitted head
can answer. Both are consequences the table surfaced, and neither was in the
plan.

**Ghostscript is a runtime dependency, not a build one**, and the test says so
out loud: most of `postscript_render` runs with no interpreter installed, and
the branch that needs one renders for real and asserts a drawn box lands where
PostScript put it — because a blank page satisfies "ok" and would hide a wrong
device or a wrong geometry.
