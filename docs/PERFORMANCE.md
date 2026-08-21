# POM2 core performance — profile, optimisations, build recipe

This document records an optimisation campaign run under **callgrind**, with
its before/after measurements and the reasoning behind each change. It serves
two purposes: give the **recipe to redo the profile**, and stop anyone from
"re-optimising" by guesswork code that has already been dealt with — or from
undoing an optimisation without knowing what it paid for.

Method and structure follow the same campaign done on **NeoST** (`../neost/docs/PERFORMANCE.md`),
POM2's sibling Atari ST emulator: same deterministic-headless-subject
discipline, same PGO/LTO recipe, same traps. The hot spots differ because the
machines differ — POM2's are the 6502 bus and the Disk II flux walker, where
NeoST's were the 68000 bus and its scheduler.

**Non-negotiable constraint for everything below**: POM2 is a cycle-accurate
emulator. None of these optimisations changes a single value it produces. Each
was validated by the full test suite (`ctest`, 182 tests) *and* by
`pom2_bench`'s output hashes — RAM and framebuffer, byte-identical before and
after, on every workload measured here.

---

## 1. The measurement subject: `pom2_bench`

`pom2_headless` cannot be profiled: it starts the worker thread, paces to
wall-clock, opens an audio device and waits for a human. `pom2_bench`
(`src/pom2_bench.cpp`) is the opposite — a closed run with no threads, no
audio device, no sockets, no pacing:

    one invocation → exactly N frames of `cyclesPerFrame` cycles

so two runs retire the **same instruction count**. That is what makes
before/after comparisons trustworthy to the percent, far more than wall time.
It prints an FNV-1a hash of RAM and of the framebuffer: an optimisation that
moves either one is not an optimisation, it is a bug.

```sh
# Representative workloads (from the repo root — ROM/disk probes are relative)
./build/pom2_bench --frames 3000 --quiet                       # ROM banner: CPU + bus + text
./build/pom2_bench --disk disks_5.4/dsk/<image>.dsk --frames 900 --quiet   # Disk II LSS
./build/pom2_bench --disk … --mode oecpu --frames 400 --quiet   # OE composite CPU demod
./build/pom2_bench --rom roms/apple2e.rom --iie --frames 3000 --quiet      # //e paging
```

⚠ `--hash-all` hashes every frame instead of the last. It is for identity
checks only: hashing 560×192×4 bytes per frame costs **more than the emulation
does** (it was 17 % of the very first profile taken here). Never combine it
with callgrind or with PGO training.

### Redoing the profile

```sh
# Profiling binary: release optimisations, minus LTO (so functions keep their
# identity in the profile), plus symbols.
cmake -B build-prof -DCMAKE_BUILD_TYPE=Release -DPOM2_ENABLE_TESTS=OFF \
      -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
      -DCMAKE_CXX_FLAGS="-g -fno-omit-frame-pointer"
cmake --build build-prof -j --target pom2_bench

valgrind --tool=callgrind --callgrind-out-file=boot.out \
    ./build-prof/pom2_bench --disk disks_5.4/dsk/<image>.dsk --frames 1200 --quiet

callgrind_annotate --threshold=80 boot.out                      # by function
callgrind_annotate --auto=yes --include=$PWD/src boot.out       # by LINE (most useful)
```

> Use **Ir (instructions retired)** to compare two versions, but **wall time**
> to judge a change that trades instructions for memory traffic or branches (a
> lookup table, branchless code). They do not say the same thing — an integer
> division is *one* instruction and 20-40 cycles.

Profile **two shapes at least**: POM2's hot spots are not the same with the
drive spinning and without it, and a lot of real use (a game loading, a
cabinet) has it spinning most of the time.

---

## 2. The starting profile

`pom2_bench --frames 600` (ROM banner, ][+, NTSC LUT) — 1 315 M instructions:

| Item | % of instructions | Cause |
|------|-------------------|-------|
| `Memory::memRead` | **23.3 %** | out-of-line call around what is usually one array index |
| `M6502::executeOpcode` | 17.7 % | the interpreter itself |
| `Memory::languageCardRead` | **11.7 %** | *every opcode fetch* of Applesoft/Monitor goes through it |
| `Memory::advanceCycles` | 10.1 % | once per emulated instruction |
| `M6502::step` | 7.9 % | |
| `SlotBus::advanceCycles` | 4.0 % | |

`pom2_bench --disk … --frames 1200` (5.25" boot then run) — 6 014 M instructions:

| Item | % of instructions | Cause |
|------|-------------------|-------|
| `DiskIICard::lssSync` | **21.5 %** | the LSS state machine, 2 steps per CPU cycle while the motor runs |
| `DiskImage::getNextTransition` | **28.4 %** (incl. inlined STL) | a full binary search over the track's flux array, per call |
| `Memory::memRead` | 9.9 % | |
| `M6502::executeOpcode` | 8.5 % | |

---

## 3. The optimisations, and what each pays for

### 3.1 Flux lookup — resume the search instead of redoing it

`DiskImage::getNextTransition` answers "next flux transition at or after this
angular position" with `std::lower_bound` over the quarter-track's event
array. A 5.25" track holds tens of thousands of events, so every call paid
~16 probes — and `DiskIICard::lssSync` calls it once per flux event, forever,
while the motor turns.

But the LSS walks a revolution **strictly sequentially**: the answer is nearly
always the index the previous call returned, or the one after it. So the class
now remembers that index (`lbHintQt_` / `lbHintIdx_`) and the search resumes
from it: a bounded forward walk first, full binary search only on a genuine
jump (revolution wrap, track step, seek).

The hint is **verified, never trusted**. Before use, the fast path re-checks
that the remembered index really *is* the lower bound (`flux[i-1] < pos <=
flux[i]`, two comparisons). A stale hint — a write splice that rebuilt the
array, an eject, a snapshot restore — simply fails that test and falls back.
Nothing has to invalidate it, and that is the design point: an invalidation
you can forget to place at one of its call sites is a latent correctness bug;
a self-verifying hint cannot be.

**−33 % wall time on every disk-active workload**, output hash unchanged.

### 3.2 Bus reads — decide the hot cases in the header

`Memory::memRead` lived entirely in `Memory.cpp`. It is the most-executed
function in the emulator (once or twice per emulated cycle), and callgrind put
it plus the `languageCardRead` it tail-calls at **35 % of the banner profile**
— most of that being the out-of-line call itself, wrapped around one array
index.

`memRead` is now an inline function in `Memory.h` that decides the two hot
cases and delegates everything else to `memReadSlow` (the original body,
untouched):

* `$0000-$BFFF` → main RAM (on a //e, the shared `iieReadFromAux` helper
  picks aux vs main inline — `Memory.h:186-197`);
* `$D000-$FFFF` with the language card mapped to ROM → ROM.

> **The trap, and it is the same one NeoST hit.** The first instinct is to
> fast-path RAM only. On the ST that yielded −4 % because *the TOS executes
> from ROM*; here it is exactly the same — with no LC RAM mapped, every
> opcode fetch of Applesoft and the Monitor goes through the language-card
> path. The ROM window is not an extra case, it is half the traffic.

The fast path's conditions are the exact negation of `memReadSlow`'s own
guards (`!lcReadRam && !iicProfile_` and the precise NoSlotClock window), so
behaviour is identical by construction. The //e aux-vs-main decision moved
into one shared inline helper, `Memory::iieReadFromAux`, used by both the fast
path and `iieMemRead` — two copies of that table would be a divergence waiting
to happen.

**−18 % on the banner workload**, output hashes unchanged.

---

## 4. Result of the code optimisations

Same compile flags before and after (`-O3` + LTO), same machine
(x86-64, GCC 13):

| Workload | Before | After | Gain |
|----------|--------|-------|------|
| ROM banner, 3000 frames | 0.425 s | **0.350 s** | −18 % |
| 5.25" boot, 900 frames | 0.566 s | **0.361 s** | −36 % |
| 5.25" boot + OE-CPU demod, 400 frames | 0.332 s | **0.217 s** | −35 % |
| Instructions, banner 600 frames | 1 315 M | **972 M** | −26 % |
| Instructions, disk 1200 frames | 6 014 M | **4 635 M** | −23 % |

---

## 5. The build recipe: PGO (+ LTO)

This is the **largest single gain of the whole campaign**, and it touches no
emulation code at all.

POM2's hot loop is the 6502 interpreter — an indirect branch on the opcode
followed by a great many rarely-taken conditionals — plus the Disk II LSS,
which steps a state machine per bit-cell. With no profile, GCC assumes both
sides of every branch are equally likely. With one, it orders the blocks so
the frequent case falls through: fewer taken jumps, less predictor pressure,
and above all a far better-used instruction cache. That counts double on a
**Cortex-A72** (32 KB L1i and a modest predictor next to a desktop x86 core),
which is why this lives in `packaging/raspberry/`.

| Variant | Banner, 3000 fr. | 5.25" boot, 900 fr. |
|---------|------------------|---------------------|
| `-O3` + LTO | 0.350 s | 0.361 s |
| `-O3` + LTO + PGO | **0.212 s (−39 %)** | **0.255 s (−29 %)** |

Output hashes identical across all variants — that is the check that says the
faster binary is still the same emulator.

> **LTO on its own now measures ~0 %** on these workloads. That is not a
> reason to drop it (it still guards against future cross-TU calls appearing
> on the hot path), but it *is* worth knowing: §3.2 hand-inlined by hand
> precisely the cross-TU call LTO used to be recovering.

**Nothing about this requires compiling on the Pi.** The
`Raspberry Pi packages` workflow (`.github/workflows/pi400.yml`) runs both
passes and the training on GitHub's native ARM64 runner, in a `debian:bookworm`
container, and uploads an AppImage + a tarball built for one core:

```sh
gh workflow run pi400.yml -f mcpu=cortex-a72     # Pi 4 / Pi 400
gh run download <run-id> -n POM2-pi400-aarch64
```

On the Pi itself — for iterating on the source — the same recipe is:

```sh
packaging/raspberry/build_native_pi.sh --pgo                 # 2 passes + LTO
sudo packaging/raspberry/build_native_pi.sh --pgo --install  # + /opt/POM2 (hard-fails without root)
```

The two scripts (`build_native_pi.sh` on the Pi, `build_in_bookworm_pi.sh` in
the container) close the same two traps below, deliberately in duplicate:
each has to be correct on its own, and a shared helper would make the CI job
depend on a script whose failure mode is silent.

The training run is `packaging/raspberry/pgo_train.sh`. It deliberately covers
several families of load — ][+ and //e banners, PAL and NTSC, every video
pipeline (NTSC LUT, mono, OE signal, OE-CPU demod, AppleWin IIR), a 5.25"
boot, and a no-render run: **a too-narrow profile is worse than no profile**,
because it marks as "cold" code that is not.

### ⚠ Two PGO traps, both of which cost the entire gain in silence

1. **GCC names each `.gcda` after the ABSOLUTE PATH of the object it belongs
   to.** Instrument in `build-A`, read back from `build-B`, and no profile is
   found — and `-Wno-missing-profile` (which is needed anyway, for the
   ImGui/GLFW TUs that are never trained) makes that failure *completely
   silent*: the binary comes out with zero gain and zero diagnostics. Both
   passes therefore share one build directory.

2. **POM2-specific, and worse.** The training driver is `pom2_bench`; the
   shipped binary is `POM2` (target `pom2_imgui`). CMake compiles each
   target's sources into its own object directory, so the emulator core exists
   twice on disk — `CMakeFiles/pom2_bench.dir/src/Memory.cpp.o` and
   `CMakeFiles/pom2_imgui.dir/src/Memory.cpp.o` — and the profile is named
   after the first only. Pass 2 would rebuild POM2 with **no profile at all**
   for exactly the files that matter. `build_native_pi.sh` copies each `.gcda`
   across (GCC mangles the object path into the file name with `/` → `#`, so
   it is a string substitution), and then **fails the build** if any of
   `M6502`, `Memory`, `DiskIICard`, `DiskImage`, `Apple2Display` has no
   profile under the `pom2_imgui` objects.

---

## 6. What is left on the table

In order of weight in the final profile, with the reason it was left alone:

| Item | Share | Why it stayed |
|------|-------|---------------|
| `DiskIICard::lssSync` | 28 % (disk) | the LSS runs 2 steps per CPU cycle while the motor turns; cutting it means leaving the per-bit-cell model, i.e. the thing that makes WOZ protections work |
| `DiskImage::getNextTransition` | 13 % (disk) | after §3.1 what remains is one 64-bit division (angular reduction) per call. Cacheable in principle — `fullRevs` only ever changes at a revolution boundary — but the anchor moves on motor-on/off and track steps, so it needs care |
| `M6502::executeOpcode` | 24 % (banner) | the interpreter. PGO is the lever here, not source changes |
| `Memory::advanceCycles` | 14 % (banner) | already incremental (the `% scanlinesPerFrame` division was removed in 2026-07); the rest is the per-instruction VBL edge, which is the model |
| `SlotBus::advanceCycles` | 5 % | already dispatches through a cached active-card array; the cost is the virtual calls themselves |

Two of these have since moved — see § 7 for `Memory::advanceCycles`; the
`lssSync` / `getNextTransition` entries stand.

One lead listed here has since been **taken** (2026-07-30 callgrind pass):
`Memory::advanceCycles` used to call `cassette->advanceCycles`
unconditionally, even with no tape loaded — measured at 4.1 % of the core.
It is now gated (`if (cassette)`, `Memory.cpp:377`) and the call is an inline
fast path (`CassetteDevice.h:86-95`) that only takes the out-of-line playback
route when the deck is actually moving.

---

## 7. Second campaign — 2026-08-20, Apple M1, `sample`

Same discipline as §§ 1-5, different host: no valgrind on macOS/arm64, so the
profiles are `sample <pid> 5 1 -mayDie` over a long `pom2_bench` run of the
profiling build (`build-prof`, § 1 recipe), and the comparisons are wall time
(best of 5) on the release build. The subject is unchanged, and so is the
rule: **every change below leaves both `pom2_bench` hashes byte-identical on
every workload, and `ctest` green (186 tests)**. A new test,
`bus_fastpath`, is part of that — see 7.2.

### 7.0 The bench's //e workload was a BRK loop

Before any optimisation: `pom2_bench --iie` called `loadAppleIIRom()` *before*
`setIIEMode(true)`. The loader only splits a 16/32 KB //e dump into the
internal `$C100-$CFFF` I/O ROM when `iieMode` is already on
(`MainWindow_Slots.cpp:1074-1083` documents the ordering rule), so the //e
booted into an empty `$C300`, executed `BRK` (`$00`) forever, and every "//e"
number this file ever quoted — and **three of the PGO training runs in
`pgo_train.sh`** — measured a BRK loop. `M6502::BRK` at 13 % of a banner
profile was the tell. The order is fixed, and `pom2_bench --dump-text`
prints text page 1 + the PC after the run so the next person can *see* what a
workload is doing before profiling it. The corrected //e banner is ~2×
slower than the BRK loop was, which is why the //e row below starts from
0.403 s and not the 0.187 s a stale note might show.

### 7.1 What the profiles said

| Shape | Top of stack | Share | Cause |
|---|---|---|---|
| //e banner | `Memory::memReadSlow` | **33 %** | the //e's keyboard loop, 80-col firmware and Monitor glue execute from the internal `$C100-$CFFF` ROM, which `memRead()`'s fast path did not cover — the exact "ROM window" trap of § 3.2, one machine later |
| 5.25" boot + game (Lode Runner in HGR, 20 000 frames) | `Apple2Display::renderHiRes` | **34 %** | three lookups + a `rotl4b` per sub-pixel, an `avgRgb` per pixel, 560 × 192 per frame, every frame, even when the screen has not changed |
| ][+ banner | `Memory::advanceCycles` | 15 % | the whole VBL/frame-publication body ran once per emulated instruction, including a 64-bit division with a runtime divisor (`cycleCounter / frameCycles`) that had been added after the 2026-07 `%` removal |
| ][+ banner | `pthread_mutex_lock` + unlock | ~5 % | `softSwitchAccess` took `kbMutex` on **every** `$C000-$C07F` access — speaker, paddles, display switches included — and the Monitor's `KEYIN` loop reads `$C000` continuously |
| //e banner | `Memory::memWrite` + `iieMemWrite` | 8 % | entirely out of line; the //e routing ran as a second call |

### 7.2 The changes, and what each pays for

**//e internal-ROM read fast path** (`Memory.h`, `memRead`). The inline
function now returns `internalIORom[addr - 0xC000]` directly in exactly the
cases where `memReadSlow()` would have done so *with no side effect*: no
//c-class profile, no NoSlotClock, not `$CFFF`, and either INTCXROM on
(except a `$C3xx` read that would still latch INTC8ROM) or INTCXROM off with
the latch already set. The latch edge itself, `$CFFF`, and everything
//c-shaped still take the slow path, so the side effects keep living in one
place. **−33 % on the //e banner.**

> **The bug this shipped with, and the test that now pins it.** The first
> version had no `addr < 0xD000` bound. Everything above `$C000` that is not
> the ROM window falls through to this block — which includes a `$D000+`
> read with the language card mapping RAM. CP/M maps LC RAM; the //e banner
> does not; `pom2_bench`'s hashes were identical and `softcard_cpm_boot_iie`
> failed. A hash check only covers the states the bench visits.
> `tests/bus_fastpath_test.cpp` is the differential answer: over **every
> address × every paging state** that feeds the fast-path conditions
> (INTCXROM / SLOTC3ROM / INTC8ROM / LC RAM / RAMRD / RAMWRT / ALTZP /
> 80STORE / PAGE2 / HIRES, 1024 states), `memRead(a)` must equal
> `memReadSlow(a)` and must not leave a side effect for the slow call to
> perform; `memWrite` must land in the same bank as `memWriteSlow`. Its own
> first version also let the bug through — all banks were zero, so the wrong
> bank read the same bytes — so it now seeds main, aux and both LC banks with
> distinct patterns, and was checked to FAIL on the bug before being kept.
> That is the habit worth keeping: a pin test that has never been seen to
> fail has not been shown to pin anything.

**Inline `memWrite` fast path** (`Memory.h`). Mirrors `memRead`: writable RAM
below `$C000` is handled inline, on the //e through `iieWriteToAux()`, the
write-side twin of `iieReadFromAux()` (`iieMemWrite()` uses the same helper —
one routing table, not two). Everything else — and writes to `$0400-$0427`,
which carry an opt-in trace hook — goes to `memWriteSlow()`, the old body.

**`advanceCycles` split** (`Memory.h` / `Memory.cpp`). The per-instruction
part is now inline and does only what genuinely happens per instruction:
`cycleCounter += cycles`, the cassette and slot fan-outs (the latter skipped
on an empty bus via `SlotBus::hasActiveCards()`). The video-timing body —
VBL edge, frame rollover, per-video-frame event publication — is
`advanceCyclesVideo()`, unchanged, and runs only when `cycleCounter` reaches
`vblNextEventCycle_`: the VBL edge of the current frame, then its end. It
recomputes that threshold every time it runs. The per-instruction division
is gone too: the frame boundary is "`vblFrameBase_` moved", which the
incremental scheme already knows, so the publication compares that instead
of dividing. **Anyone moving `cycleCounter` or the frame period behind its
back must zero the threshold** — `setCycleCounter()`, `setVideoStandard()`
and the snapshot restore do; the slow path already self-heals from any jump,
the gate just has to let it run.

**Keyboard latch mirror** (`Memory.h`, `kbLatchMirror_`). `lastKey |
keyReady << 7` is republished — under `kbMutex`, by every writer, through
`publishKbLatch()` — into one `std::atomic<uint8_t>`, and the `$C000-$C01F`
read takes a relaxed load instead of the lock. Non-keyboard soft switches
never touch it at all. A reader that lands between a writer's member stores
and its publish sees the previous pair, which is what it would have seen had
it taken the lock a moment earlier.

**`renderHiRes` tables + row cache** (`Apple2Display.cpp`/`.h`). The NTSC-LUT
branch folds artifact LUT + `rotl4b` phase select + palette into
`phaseIdx[row][w & 0x7F][absX & 3]`, and `avgRgb` of two palette entries
into `pairAvg[16][16]` — both tables computed once *with the original
functions*, which is why the framebuffer hash does not move. On top of that,
`hgrRowCache_[192]` remembers each row's 40 doubled words, the decode flavour
and its 280 output pixels: a row whose words have not changed costs an
80-byte compare and a 1 KB `memcpy`. The cache maps *input → output* (never
"the framebuffer already holds this"), so it stays correct whatever else
painted the row since — mixed-mode text, a beam-raced column split, a
capture demod. Partial-column writes decode the full row, as before, and
clip only the write-back.

**Diagnostic statics hoisted** (`M6502.cpp`, `DiskIICard.cpp`). The opt-in
trace switches (`POM2_TRACE_HANG`, `POM2_TRACE_ILLEGAL`, `POM2_TRACE_PC`,
`POM2_DEBUG_DISK`, `POM2_TRACE_LSS`) were function-local statics inside
`step()`, `executeOpcode()` and `lssSync()`. A function-local static costs an
initialisation-guard check — an acquire load and a branch — on *every* call,
and those three functions run per instruction / per LSS sync: ~3 % of a
banner profile for switches that are off. They are namespace-scope constants
now, resolved at load. Same behaviour, plain loads.

**Idle Disk II early-out** (`DiskIICard::advanceCycles`). With the motor off
and the spin-down elapsed the function did nothing but reach `lssSync()` and
return; it is called once per instruction through the slot fan-out for as
long as a Disk II is plugged, which is always. It now returns right after
bumping `cpuCycleTotal`. **−6 % on the disk shapes.**

> **A negative result worth keeping.** Caching `memRead()`'s ROM-window
> condition (`!lcReadRam && !iicProfile_ && !noSlotClock_`) in one bool
> measured **4 % slower** on the M1, reproducibly, and was removed. The three
> members are adjacent loads the compiler already schedules well; the extra
> bool added a line and an update obligation for nothing. Measure, do not
> assume — the § 1 rule, applied to one's own change.

### 7.3 Results (release build, `-O3` + LTO, Apple M1, best of 5)

| Workload | Before | After | Gain |
|----------|--------|-------|------|
| ][+ ROM banner, 3000 frames | 0.266 s | **0.216 s** | −19 % |
| 5.25" boot, 900 frames | 0.254 s | **0.188 s** | −26 % |
| 5.25" boot + OE-CPU demod, 400 frames | 0.139 s | 0.134 s | −4 % |
| //e banner (corrected workload), 3000 frames | 0.403 s | **0.261 s** | −35 % |
| //e PAL, no render, 3000 frames | 0.460 s | **0.290 s** | −37 % |
| 5.25" boot + game in HGR, 20 000 frames (profiling build) | 3.93 s | **2.80 s** | −29 % |
| same, release build, after the Disk II early-out | — | **1.96 s** | |

RAM and framebuffer hashes identical on all six, before and after each step.

### 7.4 What is left on the table, second look

| Item | Share (after) | Why it stayed |
|---|---|---|
| `M6502::executeOpcode` + `step` | ~30 % (banner) | the interpreter and its two indirect calls per instruction — PGO's job, not source changes (§ 5) |
| `Memory::memRead` condition chain | ~15 % (banner) | four loads and branches before the ROM-window hit. The next step is the per-page dispatch table in `TODO.md`, which trades them for one indexed load at the price of an invalidation at every paging-state writer — the class of bug § 3.1 was designed to avoid. Not worth it below ~10 % |
| `DiskIICard::lssSync` | ~15 % (disk, motor on) | unchanged from § 6 — the per-bit-cell model |
| `SlotBus::advanceCycles` fan-out | ~4 % (disk) | one virtual call per plugged card per instruction; a "needs ticking" mask would save the idle ones at the cost of every card having to keep it honest |
| `renderText` + `glyphRows7` | ~3 % (banner) | the full-frame static-text skip already covers the static case |

