# POM2 — TODO

Status as of 2026-09-01 (v0.8.5). This file lists **open work only**: an item
that ships is deleted here and its "why" is written up in `CHANGELOG.md`. MAME
refs → `DEV.md`.

**Format**: `🟠 high · 🟡 medium · 🟢 low` at the head of each item. Indicative
effort in *italics*. File/line in `backticks`.

**Read in this order**:

1. [Next up](#next-up) — the ordered list of what to do next (2026-09-01):
   Le Chat Mauve (P0-P2 done, P3-P7 + goldens open), the raster offset, the
   Best1a.nib write-back.
2. [Priority order](#priority-order) — the 2026-08-28 architecture plan, P0→P3
   (P0-P2 landed; P3 are rulings).
3. [Open, and known to be open](#open-and-known-to-be-open) — things POM2
   currently gets wrong *on purpose*, with the reason.
4. [MAME ↔ POM2 parity](#mame--pom2-parity-dashboard) — the fidelity dashboard.
5. [Backlog](#backlog) — everything else, by subsystem.

**What to do next, in order (2026-09-01):** the three items under
[Next up](#next-up) — Le Chat Mauve at the silicon, the mid-scanline raster
offset French Touch's DIX exposes, and the Best1a.nib finding. The 3.5"
campaign that used to sit here (//c+, Liron card, plain //c over the SmartPort
bus) closed on 2026-09-01; the
[hand-assembled ROM family](#prodos-status-the-hand-assembled-rom-family)
closed on 2026-08-28.

**A note on what the 2026-08-28 P0 pass actually found.** Three of the four
items landed, and every one of them turned up something the plan had not
predicted, always the same shape: *a mechanism that reported success while
doing nothing.* The version header was generated to the wrong path, and a
stale copy kept local builds compiling against a two-release-old version while
no clean checkout could build at all. The file-size ratchet aborted on macOS's
bash 3.2 **with exit status 0**. `POM2_FOUNDATION_SOURCES` was written down,
commented, and never read. None of these fail loudly; all three were found by
running the guard rather than by reading it. Worth remembering the next time a
guard is added: write the test that makes it FAIL.

## Next up

Ordered on 2026-09-01. These come before everything in
[Priority order](#priority-order) and [Backlog](#backlog); each is written so
that whoever picks it up can start without re-deriving the finding.

### 1 · 🟠 Le Chat Mauve at the silicon — `docs/chatmauve_plan.md`

**P0-P2 landed 2026-09-01** (CHANGELOG of that day): one card, four variants
(Féline · Adaptateur //c · Eve · Video-7 by `chatmauve_variant`), the Féline's
mixed-DHGR and HGR rules dot-exact against AppleWin's hardware-validated
oracles, the Eve's sixteen switches + CPREG auto-write (`Memory::setAuxShadow`)
+ TXT16 / TXTGREEN + table IX-1 — with the table corrected three times by
Purplesoft's own code (BW560 = HR2+HR3; the latch plays no part on the Eve;
CP280 runs with 80COL off) and the COL280 bit order read off `& PLOT`'s bytes
(2-dot cells of the 560 stream). `tests/purplesoft_eve_probe.cpp` boots the
demo disk with an Eve and writes the frames; `DEMO GR16K` / `DEMO TEXTE` come
out as the maker drew them.

**What is left, in order** (plan § 5):

- 🟠 **Pin the Purplesoft screens** — `DEMO GR16K` modes 6-10 and `DEMO
  TEXTE` screens 1-6 as golden hashes (the probe is deterministic: same boot,
  same RND seed; ~1 s of wall time for 400 s of machine). Needs the disk in
  the tree: `disks_5.4/chatmauve/` is **untracked** today — decide whether
  the ten Purplesoft / Purple Pascal images (1.4 MB) join `disks_5.4/gist/`
  under git. A test that SKIPs when the disk is absent is the usual shape.
- 🟠 **P3 — the Eve's pixel rules from the PLA** (`Chat_Mauve_eve_PLA.jed`,
  48 terms decoded): SPEC1 / SPEC2 are modelled from the manual's prose (5-dot
  window: `11011` → black, `00100` → white), DASH renders as HRAPPLEII, HR3
  alone with AN3 off is assumed BW560 — all to be read out of the fuse map.
  Purplesoft `& GR 2..5` on the probe gives the pictures to compare.
- 🟡 **P6 — the dot-clock video tap**: a mid-line `$C05E/F` or `$C0Bx` still
  lands at the frame (`renderStateKey` is per frame). DIX's Chat Mauve
  screens and PoP's title are the goldens to add once it exists. TTL-RGBI
  palette option next to the Féline capture.
- 🟡 **Extasie / Arlequin goldens**: `Extasie disk1/2.dsk` are ProDOS
  (DOS-order sectors) and want a 128 K //e boot in the probe's shape; the
  Chat Mauve demo sides, Arlequin and Eve Leonard are still to fetch
  (apple2.org.za).
- 🟢 **P5 — //c adapter quirk** (inferred 80COL; PoP's attract loop dropping
  to mono), behind a toggle. **P4 — RVB Graph**, gated on its manual.
- 🟢 **P7 — docs**: `DEV.md` § Le Chat Mauve and `docs/lle_vs_hle.md` are
  rewritten; `docs/chatmauve_plan.md` § 2.1 is the model as built. Remaining:
  fold § 3.4's corrected table back into § 3 prose (today it is a note under
  the table), and the README's Chat Mauve paragraph.

**Found on the way — fixed the same day** (CHANGELOG 2026-09-01): headless
DOS 3.3 paid RWTS's one-second motor-on wait on nearly every sector because
the legacy nibble gate stopped the spindle instantly on `$C0E8`, freezing the
latch RWTS polls before `$C0E9`. The legacy path now models the analog card's
556 one-second coast like the bit-LSS path's MODE_DELAY; boot to the
Purplesoft prompt went from 115 s to under 10 s of machine time. Pinned
`diskii_motor_coast`.

### 2 · 🟠 Mid-scanline switch lands one character cell off — DIX's rays

**Symptom** (2026-09-01, `DIX-fix.po`, `Apple //e Enhanced PAL`, composite
OE): the French Touch raster part where horizontal purple/white bars pass
*behind* the TV frame. Where a bar is hidden by the frame, the hidden span
starts about **7 dots too early on the left and ends about 7 dots too late on
the right** — one character cell each side, in the direction that widens the
hidden span. Vertically the effect is exact, so this is the horizontal
placement of a mid-line soft-switch, not the scanline.

**Where the placement is decided**: `Memory::pushVideoEventLocked` stamps the
switch with `cycleCounter + cpu->getCurrentInstructionCycles()`, and
`Apple2Display::frameCycleToPos` maps it to `byteCol = clamp((emuCycle % 65)
− 25, 0, 40)` — "visually correct at the column boundary; the exact transition
cycle within a character clock is a later refinement" (`Apple2Display.cpp`
next to `frameCycleToPos`). Both replays (RGBA `renderInternalSegment`,
composite `paintSignalBand`) consume that column through
`forEachBeamSegment`, so the fix is in one place.

**Why one cell, and why both ways**: two candidate causes, not exclusive.
(a) The stamp is taken *inside* the instruction — the write of an `STA $C0xx`
happens in its **last** cycle, and the exact cycle the switch is honoured is
the one that decides the column; being a cycle early or late at a 65-cycle
line is at most one cell. (b) The hardware pipeline: the byte fetched during
cycle *n* is **displayed during the following cycle** (the video latch, UtA2e
ch. 8 / Sather's timing diagrams), and a mode switch reaches the display side
one character after the fetch side — a fetch-side switch (page flip) and a
display-side one (TEXT/HGR, 80COL) do **not** land on the same column for the
same cycle. A hidden span that opens with one kind of switch and closes with
the other would widen by one cell on each side — exactly the symptom. DIX's
MODPAGE trick mixes both kinds on one line.

**How to settle it, then fix it** (*1-2 d*): (1) a synthetic test that throws
each kind of switch (`$C050/51`, `$C054/55`, `$C00C/0D`, `$C05E/5F`) at a
known cycle on a known scanline and asserts the dot column of the change,
with the expected column derived from the hardware timing rather than from
POM2's current mapping; (2) AppleWin as oracle — its `NTSC_VideoUpdateCycles`
path is per-cycle and documented, so the same DIX screen in AppleWin
(RGB or NTSC) gives the reference frame; MAME is **not** an oracle here (its
Apple II video is per-scanline and does not model mid-line switches);
(3) `frameCycleToPos` then gains the per-kind one-cell offset (or the stamp
moves to the write cycle), the existing `horizontal_split*` goldens are
re-derived, and DIX's raster screen becomes a golden. Related, and closed
by the same work: the residual "mid-scanline split — exact transition cycle at
character-clock" line under [Display](#display-hgr--dhgr--80-col).

### 3 · 🟡 `Best1a.nib` did not boot — a write-back had eaten 20 bytes (fixed 2026-09-01, cause open)

**What was wrong.** `disks_5.4/gist/Best1a.nib` had, at track 0, physical
sector 7 (DOS logical 4 — the `$BA00` page of DOS itself), the 8 sync bytes,
the `D5 AA AD` prologue and the first 12 nibbles of the data field replaced by
`F0 FF FF FF BF FC FD F9 FC FB E7 CF BF BF FF FF FF FF FF FF FC FF F7`: 20
bytes, offsets 2915-2937 of the track, *nothing else on the whole disk*.
DOS 3.3's boot 1 reads track-0 sectors 0-9 through the PROM's read loop, which
has no timeout, so the machine sat in the `$C65E` prologue hunt for ever,
motor on. **Not a bad dump**: git has the file before and after commit
`06f8d62` (2026-07-30, "Update gist disk images from emulator write-back"),
and the earlier version is clean — POM2's own write-back did this. Restored
from `f1e6bb6`; the disk boots to its menu (Montezuma's Revenge, Mario Bros,
Shamus, H.E.R.O.), waiting for a key in the //e firmware's KEYIN loop at
`$C27F` — which is also what `Best4a`, `Best5a` and `Best3b` do (earlier notes
took that `$C2xx` loop for an empty-slot jump; it is the internal ROM).
`Best1b`, `Best3a` are the data sides: "NOT A STARTUP DISK." / no boot sector,
by design. The damaged copy is kept off-repo for the investigation below.

**What is open — the cause.** Twenty bytes of almost-all-ones with a stray
zero every 6-9 bits is what the LSS's *write* of a handful of self-sync `$FF`
bytes looks like once re-serialised into a byte-aligned `.nib`: a write pulse
of about 740 cycles at a random spot on track 0, then nothing — no data field
followed, so it was not a sector write, and DOS never rewrites that sector
anyway. Two candidates. (a) A **soft-switch write reaching the Disk II that
was not meant for it** — the same class as the bus-traffic corruption fixed on
2026-09-01 (`iic_external_smartport` case A, 8/31 flushes before the fix): on
a //c the SmartPort firmware's probe drives `$C0ED/$C0EF` (Q6/Q7) behind the
IWM decode, and on a 5.25" card those are "load" and "write". Case C now
pins that an *empty* port leaves the 5.25" untouched (0 flushes today), but
that is today's code: 2026-07-30 was the day of "Remove two per-instruction
hotspots" + "Cache the plugged slot cards to shorten the per-instruction
fan-out" (`6e9e0f2`, `d16d1bd`), i.e. the slot dispatch was being rewritten
under the session that wrote the file. (b) A **write-back splice** landing at
the wrong offset: `saveDirty` splices dirty quarter-tracks back into the
stream (CHANGELOG 2026-08-22, "three ways a disk write could vanish"), and a
splice of a *correct* rewrite at a 20-byte offset would look exactly like
this — but nothing legitimately wrote that sector. To settle it (*½ d*): boot
every `.nib` in `disks_5.4/gist` headless on the //e PAL, //c and //c PAL
profiles with write-back on and diff the images after 60 M cycles plus a
reset; then the same on a `6e9e0f2` worktree. Until then, a `.nib` that stops
booting is to be diffed against git before anything else — `cmp -l` by
track (6656 bytes) says in one line whether it is the disk or the emulator.
`nibwalk.py` (address/data-field walk + 6&2 decode) and the `nibboot`
harness (PC histogram + text page) live in the 2026-09-01 session scratchpad;
worth landing as `tools/nibcheck.py` the next time one is needed.

## Priority order

From the 2026-08-28 architecture assessment. **Not a second backlog** — feature
items stay under [Backlog](#backlog); this says what to do *next* when choosing
among them. Levels are sequential: P0 before anything else. Priorities that land
are deleted here and written up in `CHANGELOG.md`.

The assessment's finding in one line: POM2 was **well above average on the hard
axes** (concurrency discipline, hardware fidelity, test density) and **below it
on the easy ones** (one god-object, six hand-written ROMs with no assembler).
Both weak axes had already produced a silent bug; **both are closed as of
2026-08-28** — P0 and P1 are done, and what is left below is P2 and P3.

Standing rule, and it is a mechanism now rather than a request: **do not grow
the god-objects.** A new card gets its panel in its own `*_ImGui.cpp` and
**zero** business logic in `MainWindow.cpp`. `cmake` fails if any
`src/MainWindow*.cpp` passes 2000 lines, and `tools/check_file_sizes.sh` fails
if any first-party file passes its recorded ceiling. The rule went from 5590 to
11511 lines while it was only written down; both numbers are why it is wired to
something now.

### P0 — close what is bleeding · *landed 2026-08-28*

**P0 is closed.** 0-5 landed too: `TnfsClient` is wired rather than deleted —
`POM2 tnfs://host/path/image.po` fetches the image into a local cache and boots
it like any other disk (`TnfsMedia.*`). It needed a socket-less path first, as
recorded here, and got one. Phase 1 of the built-in FujiNet
([Network](#network)) now has a caller.

**0-1 to 0-4 landed on 2026-08-28** — see `CHANGELOG.md`. Doing them turned up
three things the plan did not know about, all recorded there: the version
header was never generated (no clean checkout could build); the ratchet had
never run on macOS *and reported success*; and `POM2_FOUNDATION_SOURCES` was
listed but never read, so the layer hole was 14 files, not 13.

### P1 — the two structural causes · *landed 2026-08-28*

**All four items are done.** Both weak axes the assessment named are closed.

**1-1 / 1-2 — the hand-written ROMs.** `SlotRomAsm.h`, and all six pages
rewritten on it, each verified **byte-identical** to what it produced before.
Two corrections to the plan, recorded because they are the kind of thing that
gets re-proposed: it is *not* `constexpr` (a slot page is parameterised by the
slot, which comes from settings at runtime, so there is no constant to fold),
and there were **six** hand-written ROMs, not seven — `ClockCard` writes nine
fixed bytes with no cursor and `DiskIICard`'s boot PROM is a verbatim dump.

**1-3 / 1-4 — the god-object.** `MainWindow.cpp` 8316 → ~1680, holding only
construction, the dock and the frame loop. Eleven sibling TUs, each named for
what it owns; the split worth knowing is `MainWindow_Media.cpp` (what happens
to a disk image) vs `MainWindow_StoragePanels.cpp` (drawing it). The ratchet
was lowered in the same commit — in fact `MainWindow.cpp` **left the budget
file entirely**, being under the 2000-line watch threshold — and a *hard* cap
now fails `cmake` if any `src/MainWindow*.cpp` passes 2000 lines. Family-wide
on purpose: the failure mode was never "MainWindow.cpp grows", it was "the file
that grows is whichever one is convenient".

Details for all four in `CHANGELOG.md`, structure in
[DEV § The MainWindow family](DEV.md#the-mainwindow-family).

### P2 — make the holes visible · *landed 2026-08-28*

**2-3 — warnings to zero, and a leg that keeps them there.** 14 → 0, and
`-DPOM2_WERROR=ON` on the macOS job. One of the fourteen was not a style nit:
a value-returning lambda in the Super Serial panel fell off its end (UB,
working only because NRVO put the object where the caller was going to read
it). Another was an initialiser list in a different order from the
declarations it initialises. Both are the argument for the flag.

**2-1 — coverage in CI.** `tools/coverage.sh` + a `Line coverage (floor)` job.
Clang source-based coverage over the code the tests link; the floor may go up
freely and may not go down, the same ratchet shape as the file-size budget.
**First measurement: 78.90 %**, floor recorded at 78.40 % (half a point of
margin — two runs of the same tree differ by ~0.1 %, and a floor that fails on
noise is a floor somebody switches off). It named real holes on its first run:
`CharRomCatalog.cpp`, `RomLoader.cpp`, `SlirpNetworkBackend.cpp`,
`SpSerialTransport.cpp` and `SuperSerialTcpTransport.cpp` are all at **0 %**.

**2-4 — `FloppyEmuDevice`.** Already tested, and the plan's third wrong
premise. `tests/floppy_emu_smoke_test.cpp` covers the mode label/key
round-trip, per-mode format filtering, SD navigation bounded to the root
(including the symlink escape), and `favdisks.txt` parsing. Nothing worth
adding was missing.

**2-2 — `FujiNetNetDevice`.** The plan's premise was wrong, in the same way
P0-5's was: it is **not** untested. `tests/fujinet_net_device_test.cpp`
already covered the happy path, the header split, the STATUS cap, a stalled
server and a blackholed host. What it did not cover has been added — a reply
over the 512 KB cap, a reply with no CRLFCRLF, the port-number and empty-host
refusals, a refused connection, `close()`, and the **error bytes**, which are
a contract with the guest rather than an implementation detail (FILE NOT FOUND
is how the firmware's table spells "no such host"; a guest that cannot tell a
typo'd hostname from a dead server has nothing to show the user). Both new
paths mutation-checked.

**What the number says to do next**, now that there is one — these are the
0 % files above, and they are a better backlog than guessing was:

| # | Item | Why |
| - | ---- | --- |
| 2-5 | ✅ **`RomLoader.cpp` and `CharRomCatalog.cpp` at 0 %** — *landed 2026-09-01, and the premise was wrong a fourth time.* `RomLoader` was not untested, it was **dead**: zero call sites anywhere in the tree (cards keep their ROM in their own byte array and serve it from the slot bus; nobody has flashed a card ROM into `Memory` for a long time), yet it was compiled into the app and nine test binaries. Deleted. `CharRomCatalog` got the test it was actually missing (`char_rom_catalog`), both new assertions mutation-checked. | — |
| 2-6 | 🟢 **The three host transports at 0 %** — `SlirpNetworkBackend`, `SpSerialTransport`, `SuperSerialTcpTransport`. | Seams exist for all three now (`ssc_transport_seam`, `fujinet_link_seam`), so these are testable in a way they were not before the 2026-08-27 seam work. |
| 2-3b | 🟢 **Extend `-Werror` to the GCC leg** — it is on for macOS/AppleClang (2026-08-28). GCC's warning set is not clang's and nobody has cleaned it, so turning it on blind would red `main`. Build the Linux job once with it, fix what it names, then wire it. | The leg that catches what clang does not — transitive includes, and its own `-Wmaybe-uninitialized` family. |

### P3 — rulings, not development

| # | Item | Why |
| - | ---- | --- |
| 3-1 | 🟡 **Write-protect: one rule per card** — see [Storage](#storage-disks--images). | Two bays of the *same* SmartPort card answer "can I write?" differently. Either rule is defensible; one card doing both is not. |
| 3-2 | 🟡 **Echo+ TMS5220: ship or hide** — see [Cards](#cards-slot-cards--peripherals). | A detect-only stub in the catalog is the wrong third option, and the backlog already says so. |
| 3-3 | 🟡 **CI `ctest -L rom` + ROM Status "degraded"** — running the synthetic fallback is not "missing". | Otherwise the L0 path rots behind a green suite that SKIPs when dumps are absent. |
| 3-4 | 🟢 **One `Config`** (env → CLI → Settings → defaults), consistent `pom2::` namespace. | Hygiene for the second contributor. |

**Explicitly not architecture** — do not pick these ahead of P0-P3. They stay in
the backlog as features: the analog IIR composite pipeline (*5-10 d*), the
Saturn 128K Language Card, ayumi-grade FIR resampling (a deliberate MAME
departure plus WASM cost), and Apple IIgs (already a separate **pom2gs**
project — see [Out of scope](#out-of-scope)).

## Open, and known to be open

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

## Quick wins

High impact/effort ratio, and independent of the architecture work. When the
two compete, [Priority order](#priority-order) wins — P0 is four hours and
closes a bug class.

| # | Item                                    | Effort  | Why                                |
| - | --------------------------------------- | ------- | --------------------------------------- |
| 1 | ~~WASM IDBFS settings persistence~~     | *done*  | landed 2026-09-01 — see [WASM](#wasm) |
| 2 | WOZ1 splice point TRK+6650              | 1 d     | Applesauce re-master parity             |

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
| 11 | IWMDevice                      | Verbatim         | `machine/iwm.cpp:1-543`                                                  | 🟢 **window sizes are MAME's own again** (2026-09-01): the state machine runs on the controller's 7.16 MHz clock (`POM2_IWM_TICKS_PER_CPU_CYCLE`), not whole CPU cycles, so 28/14/36/18 are used verbatim and a window edge lands inside a 14.17-tick Sony cell. That plus a flux-query off-by-one is what unblocked the 800K read path — pinned by `sony35_iwm_read_path`. 🟢 Q3 fast clock (Mac/IIgs only) still unmodelled, and no longer load-bearing |
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
| 23quater | TranswarpCard (key `transwarp`) | Partial-verbatim | MAME `bus/a2bus/transwarp.cpp` (R. Belmont, 363 lines; line-cited) | 🟢 **deliberate divergence**: MAME runs a SECOND W65C02 DMA-ing the Apple's bus because a MAME card cannot retime the host CPU; POM2 scales `cyclesPerFrame` and keeps the machine's own 6502 — closer to the board and free on the hot path. Register semantics, DIP defaults and the slowdown windows are verbatim. 🟡 multiplier sampled per frame (unbiased in aggregate, wrong for where in a frame slow cycles land); 🟡 ROM shadow gated on an undumped `roms/ae_transwarp_1.4.bin`. Pinned `transwarp_card` |
| 24 | FujiNetCard (key `fujinet`)    | POM2-original (relay) | No MAME device — published SmartPort/SP-over-SLIP spec + the FujiNet AppleWin fork | 🟢 not an emulation: the device is real and off-box, every SmartPort call is forwarded verbatim; no peer → bounded 250 ms stall then SmartPort `$27`; 🟡 **rewind cannot rewind it**; 🟡 not on //c-class (forced INTCXROM masks slot ROM); pinned `fujinet_card` |

## Backlog

- **[Storage] //c + ProDOS : RESOLU (2026-08-30).** Trois couches, chacune
  masquant la suivante : l'auto-interblocage du montage (routeMountHdv), la
  banque $C800 du stub jamais percee sur //c-classe (iicCardWindow_), et --
  la racine -- l'entree ProDOS $Cn0A du stub SANS le `BIT $CFFF` du vrai
  firmware Liron : appelee par le noyau APRES que le firmware 80 colonnes a
  latche INTC8ROM, ses JSR vers $CD00/$CD10 executaient la banque INTERNE,
  le scan //c de P8 2.4.3 partait dans le decor et sa table de devices
  restait vide (RESTART SYSTEM-$0A au premier MLI du programme, dissection
  complete au traceur : dispatch $E1C2, installateur $EE82, bloc de config
  $FExx). Verifie en jeu : SCOSWAMP boote et se joue sur --preset iic.
  Le harnais de dissection reste dans le test epingle
  (`POM2_TRACE_HDV=<hdv> test_iic_onboard_smartport`, + oracle //e).

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
  out). The exact transition cycle at character-clock is no longer a "later
  refinement": DIX shows it as a one-cell error each side —
  [Next up § 2](#2--mid-scanline-switch-lands-one-character-cell-off--dixs-rays). **Back-port to POM1** next (gated: LORES+TEXT rendering on
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
- 🟠 **Le Chat Mauve EVE / Féline / RVB Graph / //c adapter** — now
  [Next up § 1](#1--le-chat-mauve-at-the-silicon--docschatmauve_planmd),
  planned in `docs/chatmauve_plan.md`. Still parked alongside: **Video-7
  AppleColor RGB** (its 160×192 chunky mode and F/B text are the only things
  the plan's Féline decoder does not cover), **Color killer Rev 1**,
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
  ([Priority order](#priority-order)).
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
- ✅ **Real 3.5" boot — //c+, Liron card and plain //c** *(reopened
  2026-08-28; read path + //c+ boot, then the SmartPort bus, all landed
  2026-09-01)* — every piece was already in the tree and individually
  pinned; what was missing was that no test crossed the seams between them,
  and then one subsystem nobody had named: the drive's side of the SmartPort
  **bus**.

  **//c+ on-board: pinned** (`iicplus_boot35`). The ROM drives the MIG, the
  MIG selects the drive, the IWM walks the cells, ProDOS 8 boots off the
  internal Sony. Two controller faults stood in the way: the IWM clocked in
  whole CPU cycles (a Sony cell is 2.02 of them — it runs on its own 7.16 MHz
  clock now, `POM2_IWM_TICKS_PER_CPU_CYCLE`, MAME's 28/14/36/18 verbatim) and
  a flux query one tick late. Harness: `sony35_iwm_read_path`.

  **Liron card and plain //c: pinned** (`liron_boot35`,
  `smartport_bus_handshake`, `iic_external_smartport`). Both firmwares are
  one code base (`apple2c-32Kv0.rom` bank 1 `$C88C` ≡ the Liron's `$C806`)
  and neither talks to a 3.5" mechanism: the scan at `$C800` holds LSTRB high
  through a status read and polls SENSE — the SmartPort bus handshake, which
  only an intelligent UniDisk 3.5 answers. `SmartPortBusDevice` is that
  device at the byte level (INIT / STATUS / READ / WRITE; the protocol map
  with ROM addresses is in `DEV.md` § The SmartPort bus). `LironCard` puts it
  behind its own IWM and is in the catalog (`liron`); on the 32 KB //c,
  `IIcExternalSmartPort` puts it behind `$C0E0-$C0EF` with a private IWM as a
  register tracker, claiming only bus accesses, so the `DiskIICard` keeps the
  5.25" and `iic_diskii_no_iwm_conflict` still holds. A 5.25" boot now lists
  `S5,D1/D2` next to `S6`; Boot on slot 5 runs the real `$C500`; nothing is
  punched over it while media is mounted. The host-served `$C500` stub stays
  for the 16 KB //c and the //c+'s HDV.

  Deliberately out of scope: the UniDisk 3.5 **drive-side** 65C02 firmware —
  the protocol is the contract and both firmwares boot through it.

### [Cards] slot cards & peripherals

- 🔵 **The MAME `a2bus` backlog — what is worth porting, and why.** *(survey,
  not a task)* <a id="a2bus-backlog"></a>The Workstation Card cost what it did
  **because MAME does not have it**. That is the criterion for everything
  below: a card MAME already models is a port with an oracle; a card it does
  not is reverse engineering. `src/devices/bus/a2bus` crossed against POM2's
  actual gaps:

  **The four that earn their keep.**

  - **Videx VideoTerm** (`a2videoterm`) — *the clearest functional hole.*
    POM2 does the //e's 80 columns (internal, `$C300` firmware + AUX under
    `iieMode`), so a **II/II+ has none at all**. The VideoTerm is the card
    that made AppleWorks, word processors and CP/M usable on a II+. Not
    cheap: it carries its own 6845 and its own dot clock, so it is a second
    complete video path, not a slot peripheral. Cousins if the shape works:
    `a2ultraterm`, `suprterminal`. **The one to do first** — it changes what
    the machine can *do*, not what it can imitate.
  - **Mountain Computer Music System** (`a2mcms`) — the real blind spot for
    an emulator that already has Mockingboard A/C, Sound II, Phasor, SSI263
    and a stereo bus. 16 digital voices, the Apple II's first polyphonic
    synth, and an architecture with nothing in common with the AYs. Plays
    straight to the project's strength.
  - **E-Z Color Graphics Interface** (`ezcgi`) — a **TMS9918 in an Apple II
    slot**: hardware sprites on a machine that has none. Re-ranked after
    looking at POM1: the expensive part is already written there, and better
    than MAME's, with 31 k lines of original software behind it. Scoped,
    estimated and parked → [§ E-Z Color](#ez-color).
  - ~~**Applied Engineering TransWarp** (`transwarp`)~~ **done** —
    `TranswarpCard`, pinned by `transwarp_card`. The estimate held (the
    `cyclesPerFrame` plumbing did the work) but the shape was wrong twice:
    there are no "cache semantics" to model — the board has no cache, it has
    a bus watcher that drops to 1 MHz around slot and paddle accesses — and
    it is not a speed latch in a slot, because it decodes nothing
    slot-relative at all. `$C072`/`$C074` are global, so it needed a bus
    snoop hook rather than a device-select handler.

  **Quick wins, a few hours each.**

  - ~~**4play** (`4play`)~~ **done** — `FourPlayCard`, pinned by
    `fourplay_card`. It was not a shift register: `read_c0nx` returns one
    byte per player and `device_start()` is empty. **SNES MAX** (`snesmax`)
    is still open and is the larger of the two — its controller is serial, so
    the card clocks a latch/shift protocol rather than exposing four ports.
    Both are modern homebrew, so their value is the current Apple II scene
    (and `pom2adventure`), not a period catalogue.
  - **TimeMaster H.O.** (`timemasterho`) — the other common clock beside the
    ThunderClock+ POM2 already has.
  - **Apple Parallel Interface Card** (`a2pic`) — the third printer lineage
    beside the Grappler+ and the synthetic card. Small, and it rounds off the
    printer work.
  - **Memory Expansion Card / RamFactor** (`a2memexp`) — a slot RAM disk,
    distinct from the AUX-slot RamWorks POM2 has. Gives a II+ a RAM disk.

  **Heavier, only if the appetite is there.**

  - **Apple II SCSI / High-Speed SCSI** (`a2scsi`, `a2hsscsi`) — fidelity
    rather than capability, since CFFA and the synthetic HDV already cover
    the need. The point would be running software that talks to the real
    card.
  - **The Mill** (`a2themill`) — a 6809 coprocessor, OS-9 on an Apple II.
    Same shape as the Workstation Card, so **`Memory::ForeignBus` is already
    there for it** (PERFORMANCE § 9).
  - **PC Transporter** (`pc_xporter`) — a whole 8086. MAME has it; it is a
    project in itself.

  **Deliberately skipped.** LANceGS (two Ethernet cards already), the Z80
  variants (`a2applicard`, `softcard3`, `titan3plus2` — the Microsoft SoftCard
  covers it), IEEE-488, ComputerEyes, and the modern storage cards (`a2sd`,
  `booti`, `sider`) that FujiNet + CFFA + HDV already cover.

  **What a hardware archive adds that MAME does not.** Not schematics —
  **DIP-switch and jumper positions with their meanings**. That is exactly
  what POM2 already models for the Grappler+ (its seven printer-type
  positions) and the SSC (its two blocks), and for a Videx or a TransWarp it
  is the source MAME lacks.


- 🟡 **Apple II Workstation Card — the card boots; the host handshake does
  not.** *(~2-4 d to finish)*
  <a id="apple-ii-workstation-card"></a>The card that put a IIe on LocalTalk,
  so it could netboot from an AppleShare server and reach the LaserWriters on
  the same net. Emulated as `WorkstationCard` (catalog `workstation`), pinned
  by `workstation_card_smoke`.
  → [DEV](DEV.md#apple-ii-workstation-card-workstationcard),
  [plan 2 § 5](docs/printer_plan_2.md#5-the-apple-ii-workstation-card--it-boots)

  **What works.** Apple's real 341-0358-A firmware runs on the card's own
  65C02 — over `Memory::ForeignBus`, so the Apple II's hot path pays nothing
  (measured: PERFORMANCE § 9) — completes the power-on self-test including the
  255-byte SCC loopback, configures the chip for **LocalTalk, SDLC,
  230400 bit/s**, and then **acquires a node address and transmits real LLAP
  frames** (`0B 0B 81` lapENQ, then `FF 0B 84` and short DDP broadcasts). The
  `$Cn00` window, the `$C800-$CFFF` expansion ROM, the `$7C00` ROM banking,
  the interval timer and the snapshot (chip included) all work with the card
  in a real `SlotBus` — and **CardCat, booted on the emulated //e, names the
  card in slot 4**.

  **What remains, in order:**

  1. ✅ **The host handshake works.** AppleShare's `ATINIT` calls the card at
     `$Cn14` in ProDOS-MLI style — `JSR $Cn14 / .BYTE cmd / .WORD block`,
     command `$42` — and POM2 now services it end to end: the command byte
     reaches `$CnDB`, both rendezvous semaphores return to rest, and the call
     returns past its inline parameters. Pinned by `workstation_card_smoke`.

     **The bug was one number**: the `$C800-$CFFF` window was based at file
     `0xC800` instead of `0xC400`, so the page's `JMP $CC00` landed on a
     block-copy loop rather than the driver prologue
     (`CLD / PHP / SEI / LDA #$50 / STA $C080,X`). Nine bases were swept;
     `0xC400` is the only one at which the transaction completes.

     **Two things worth keeping from how long that took.** The card steers
     the host by *patching the host's code* — it writes `$CnBB`/`$CnBC` (the
     operands of the host's `JMP`) and `$CnC3`/`$C4`/`$C6`/`$C7` (the address
     operands of its block-move), and releases the host's spin loops by
     writing `$38` (`SEC`) over the `$18` (`CLC`) it is executing. And the
     "missing bit 6 of `$02EE`" was a **red herring**: wiring it moved the
     card one step further, which made it look right, and it was not. A
     change that unsticks a stuck system is not evidence that it is correct.

     Still open, and now cheap to look at: `$C0nX` reads answer `$FF` and
     writes are ignored, and the transaction completes anyway — so whatever
     the strobes are for, this path does not need them. `hostStrobeLog()`
     records them for whoever wants to find out.

     ✅ **Verified end to end**: `disks_3.5/AppleShare IIe Workstation.po`
     boots in POM2, its ATINIT passes the card's power-up diagnostics and
     the workstation software reaches its menu.

  2. ❌ **Why lapACK does not move the node.** Answering the card's lapENQ
     with lapACK is accepted by the chip — the FIFO fills, the interrupt fires
     — and the driver enquires again anyway rather than picking another
     address. Timing window, a status bit that is not set, or simply what this
     firmware does on a dead network. Worth an hour before (3). *~0.5 d.*
  3. ❌ **A host-side LocalTalk endpoint** — bridge the card's frames to a
     real or emulated AppleTalk network. `setFrameCallback` and
     `receiveFrame` are the seam and both work; note the card **disables its
     receiver while transmitting**, so an endpoint must wait for WR3 D0 to
     come back before answering. *~1-2 d.*

  ~~**SDLC framing.**~~ **Done** — datasheet-derived (MAME has no SDLC),
  marked `SDLC (datasheet, not MAME)` at every site, pinned by
  `scc8530_smoke`. ~~**The SCC's register file is not in the snapshot.**~~
  **Done** — `Scc8530Device::appendSnapshot`/`restoreSnapshot`, carried by the
  card's own blob.

  **Smaller gaps, worth knowing.** The interval timer's period is a **chosen**
  1 ms, not a derived one: the dump does not settle it and the firmware boots
  with it. And the card runs a second 6502 at the Apple II's own rate, so
  plugging it roughly doubles the emulation work.

  **Do not "optimise" `advanceCycles`.** Its 24-cycle interleave is
  correctness, not tuning: the POST's self-test has a fixed poll budget, and
  running the CPU for a whole 4096-cycle slot-bus chunk before the SCC moves
  fails it on a timeout no real card would see.

  **Worth knowing before finishing it:** given the LaserWriter's PostScript
  path already ships (plan 2 § 4), this card buys an *alternative transport*
  for PostScript that the Super Serial Card already carries — not a new
  capability. It is worth doing for netboot and AppleShare, and for being the
  way most sites actually wired a LaserWriter; it is not the only way to print
  to one.

- 🟢 **ProDOS `STATUS` — the hand-written ROM family** *(closed 2026-08-28)* —
  <a id="prodos-status-the-hand-assembled-rom-family"></a>kept as a record, not
  as work. All six pages are written with `SlotRomAsm.h`: an address is a
  label, so a mistyped displacement and a routine that moved out from under one
  are both unrepresentable, and two regions claiming the same bytes — the
  SmartPort bug in its purest form — is an error before a byte is written.
  Both halves of the `ProDOSHardDiskCard` finding are fixed, the second by
  making the write routine *shorter*: one `BIT $C0n3` answers "is there media?"
  and "is it locked?" at once, and both transfer routines branch to a shared
  error tail in the gap after boot. Zero slack became eight bytes.
  → `CHANGELOG.md`

  What the audit settled, so nobody re-derives it:
  - `ClockCard` writes nine fixed bytes with no cursor — nothing to assemble.
  - `DiskIICard`'s boot PROM is a verbatim dump.
  - `GrapplerCard` hand-writes only its *fallback stub*; a real `roms/` dump is
    copied verbatim.
  - `SuperSerialCard` was the tightest of the six, for a reason the others did
    not share: `$Cn0D-$Cn10` are the low bytes of four Pascal routines, so a
    routine pushed down lands the interpreter mid-instruction. Those four bytes
    are `byteOf()` now, and cannot disagree with where the routines are.

- 🟡 **SmartPortCard leftovers, still open** (2026-07-12 Liron audit
  follow-ups) — two of the original five remain:
  - boot failure is a silent `JMP $CnE0` loop; real firmware prints an error.
  - CONTROL calls that need the control-list DATA: only code 0 works, because
    the stub has no guest→device list copy. Everything else returns `$21`.

- 🟡 **Write-protect: two bays of one card answer differently** — architect
  **P3-1**. 3.5" units report `fileWriteProtected || !writeBackEnabled` (the
  rule `DiskImage` uses for 5.25"), while HDV bays report only `wpHeader_` and
  stay RAM-writable. On the *same* SmartPort card the guest's "can I write?"
  is decided by media kind plus a host-side toggle that models nothing on the
  machine. Physically, write-protect belongs to the medium; the counter-argument
  is that accepting a write with write-back off loses it silently. Both are
  defensible — one card doing both is not. **Needs a ruling, then ~1 h.**

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
- 🟠 **Real Liron / UniDisk 3.5 (IWM in a slot)** — folded into the single
  3.5"-boot campaign under [Storage](#storage-disks--images), because it shares
  its one unknown: whether the GCR read path feeds the firmware a clean
  address field. Build the harness before the card.

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
  Phases: (1) TNFS client + tests — **done**, and since 2026-08-28 it has a
  caller: `TnfsMedia.*` fetches an image from a TNFS server into a local cache
  and `POM2 tnfs://host/path/image.po` boots it like any other disk. That is
  not phase 3 — the guest sees an ordinary local image, not a block device
  backed by the network — but it makes the client reachable and useful now;
  (2) the Fuji control device (host/drive slots) so CONFIG sees real state;
  (3) block serving of a mounted image; (4) the panel's source selector.
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

- ✅ **IDBFS settings persistence** — *landed 2026-09-01.* `state.cfg` and
  `imgui.ini` now live in `pom2::userConfigDir()`, which is `/persistent`
  under Emscripten, and reach IndexedDB through a debounced `FS.syncfs`
  (`PersistentFs.h`). Two things the estimate did not know: the browser
  build has **no exit** (`simulate_infinite_loop` never runs `~MainWindow`),
  so the whole persist block had to become `MainWindow::persistSession()` on
  a heartbeat; and the shell's populate had to hold up `run()`, which is a
  boot hang if its callback never fires — hence the watchdog. Verified in
  headless Chrome over CDP, three visits. → `CHANGELOG.md`
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
  **stable 1.0**. *decision + media sourcing.*

### [Arch] refactor & tooling

- 🟠 **Two ceilings were raised instead of splitting — the debt.** *(~1 d for
  ImageWriter, less for Memory)* <a id="file-size-debt"></a>The file-size
  ratchet had been failing on `main` since **2026-08-29**, in 15 s, before the
  Linux job compiled anything — so it was also hiding that job's build and its
  GLES tier behind a red X nobody could see past. On 2026-08-31 the two
  ceilings were raised to their exact current sizes to get the gate green
  again. That is what `tools/file_size_budget.txt` is for (its script's header
  says editing it is precisely the moment someone should be asked), and it is
  the lesser half of the answer.

  - **`src/ImageWriter.cpp` 2152 → 2501 (+349)** — the printer work put the
    ImageWriter head, the paper tray, PDF export and the PostScript /
    screen-dump seams in one translation unit. Those are already separate
    concerns with clean edges; this one wants splitting, and the project's own
    rule ("new code for an existing window group belongs in its own
    translation unit") says so.
  - **`src/Memory.cpp` 2402 → 2442 (+40)** — smaller, and not one change.
    Exactly **one** line of it is the foreign-bus dispatch that lets a
    coprocessor card run the 6502 over its own map
    (`docs/PERFORMANCE.md` § 9); the other 39 predate it.

  Both are recorded at their **exact** current size, so the ratchet still
  fails on the next line either of them gains — the debt cannot quietly grow.


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
  audio thread** *(2026-08-02 bug-hunt follow-up)* — architect **P2**
  ([Priority order](#priority-order)). The highest-yield gap we
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
- 🟠 **`MainWindow.cpp` god-object (8 319 lines)** — architect **P1-3**
  ([Priority order](#priority-order)). 111 includes, ~290 declarations in the
  header; it is where rendering, input, media, profiles, panels and coordinator
  wiring all cross. The panel registry and the eleven coordinators did the hard
  part — what is left is **movement, not redesign**.
  **Left**: (a) the storage panels (Disk, Library, SmartPort, FujiNet,
  FloppyEmu, HDV) — they reference the `kProDOSHostSentinel` / `freePoNameFor`
  anonymous-namespace helpers, which must move with them; (b) the keyboard and
  welcome panels, which stay put deliberately (they load a texture through the
  `STB_IMAGE_STATIC` instance defined in `MainWindow.cpp`, whose symbols are
  internal to that TU); (c) `renderScreenWindow`, tightly coupled to the GL
  upload. Target &lt; 2 000 lines, ratchet lowered in the same commit. *2-3 d.*
  → [DEV](DEV.md#panel-registry-panelcataloghpanelregistry-mainwindow_panelscpp)

- 🟠 **No test drives the ImGui panels, so a UI-thread deadlock fails nothing**
  *(found the hard way 2026-08-27)* — a coordinator capture placed inside an
  existing `lock_guard(stateMutex())` scope would have hung the UI thread and
  the emulator together, while the full suite stayed green, because nothing
  drives the panels. `stateMutex` is non-recursive and every coordinator
  capture takes it itself.
  **Mitigation in the tree**: `tools/check_coordinator_locks.sh`, run after
  touching any coordinator call site — falsifiable against `44b715f`.
  **Still open**: `tests/frontend_device_panel_concurrency`, a headless ImGui
  frame driven while cards are replugged. That is the only thing that closes
  the class rather than scanning for it. *1 d.*

- 🟢 **W5100 name resolution is still inline** *(2026-08-27)* — deliberately
  left in `W5100Device` when the socket seam landed: an async mailbox with an
  in-flight cap, a bounded wait and its own cache, wired to register reads.
  Its own pass, and not on anything's critical path.
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

## Parked — wanted, not scheduled

Things we intend to do and are not queueing. Distinct from *Out of scope*
below: these have a clear shape and a reason, they just have no slot.

### E-Z Color Graphics Interface — a TMS9918 in an Apple II slot

<a id="ez-color"></a>**Estimate: ~3.5-4 days** for a working card
(TMS9918A only). *Card only* — `tmspaint` and `tmssprite` stay in POM1.

MAME has it (`src/devices/bus/a2bus/ezcgi.cpp`, Steve Ciarcia, *BYTE* August
1982 — a construction article, not a product), so the usual "MAME is the
oracle" rule half-applies. Only half, and this is the unusual part: **POM1
already has a better TMS9918 than MAME's.** `pom1/src/TMS9918.{h,cpp}` is
2 586 lines with a `ChipType` dispatch across TMS9918A / 9929A / 9118 / 9128 /
9129 / T7937A / T6950, modelling the Toshiba clones' suppression of sprite
cloning ("Bug N°8"); MAME models plain `tms9918a`. So the oracle here is
POM1 + the datasheet + POM1's own silicon tests, and **that must be written
down at the porting sites** the way SDLC framing is marked
`SDLC (datasheet, not MAME)` in `Scc8530Device` — otherwise a future reader
goes looking in MAME and finds something *less* accurate, which is the worst
kind of trap.

**Why it is worth doing at all**: the Apple II has no sprites, no VRAM of its
own, and colour only as an NTSC artefact. This card brings 16 KB of dedicated
VRAM, 32 hardware sprites and 15 commanded colours. And the software problem
that sinks most curiosity cards does not apply — POM1 carries **31 067 lines
of original 6502 assembly** for this chip (Rogue 6 777, a logo/scroller 5 426,
Galaga 5 127, Maze3D 4 315, plus Sokoban, Snake, Chess, Mandelbrot, Life,
Plasma, Nyan Cat). Porting those to the Apple II is a separate, later job and
is **not** in the estimate below.

**What makes the port cheap, checked rather than assumed:**

- The VDP port addresses are canonical in one place —
  `pom1/dev/lib/tms9918/tms9918.inc`, `VDP_DATA = $CC00` / `VDP_CTRL = $CC01`
  — so POM1's 6 079-line asm library is address-agnostic above two symbols.
- `BeamClock.h` is 63 lines, a pure header depending only on `<cstdint>`, and
  its own comment already names POM2 as an intended consumer.
- Both projects clock at 1 022 727 Hz exactly. No retiming.
- `pom1::Peripheral`'s pure-virtual surface is `name()` alone; everything else
  has a default. Stripping it costs almost nothing.

**What is not free:**

- `SnapshotIO.h` differs between the projects (273 vs 183 lines), so
  `serialize`/`deserialize` must be rewritten against POM2's
  `appendSnapshotState` / `loadSnapshotState` pair. The *content* is already
  enumerated in `TMS9918::Snapshot`, so it is mechanical.
- The beam/CPU sync (`renderBeamCatchUp`, `syncSpriteScanToBeam`) is tied to
  how the host feeds cycles, and is the part to read carefully rather than
  transplant.

**Breakdown:**

| | |
|---|---|
| Import + decouple the VDP core (2 586 lines + diagnostics + BeamClock), snapshot rewrite, build wiring | ~1 d |
| `EzCgiCard : SlotPeripheral` — offset 0 ↔ VRAM, offset 1 ↔ register/status, `$FF` elsewhere, `advanceCycles` feeding the beam. Slot-agnostic by construction | ~3 h |
| Display path — a second source composited into POM2's framebuffer. **The risk item**: not the rasterising, but the interaction with `NtscPostProcessor` / `CrtEffectStack` / the display-mode menu. The VDP's output is RGB and must NOT go through the composite shaders | ~1-1.5 d |
| Tests — a subset of POM1's ten (sprite status, per-scanline, silicon-strict) plus a card smoke at `$C0nX` | ~0.5 d |
| Catalog, plug site, docs (CLAUDE map, DEV section, README, CHANGELOG) | ~0.5 d |

**Deliberately out of v1**: the `ezcgi_9938` / `ezcgi_9958` variants (V9938 /
V9958 with an IRQ line back to the Apple II). MAME itself annotates their
clocks "typical … not verified".

**The standing objection, which grows with every shared file.** `hgrpaint/` is
already duplicated between POM1 and POM2 with no shared build. Adding the VDP
puts ~2 700 more lines in both trees, and a silicon-behaviour fix will then
have to be applied twice with nothing to flag the omission. That is
survivable — `hgrpaint` proves it — but at this volume the question "shared
module or verbatim copy?" deserves deciding on its own, not during a port.
→ [`a2bus` survey](#a2bus-backlog)

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
