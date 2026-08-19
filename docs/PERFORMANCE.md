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

One lead listed here has since been **taken** (2026-07-30 callgrind pass):
`Memory::advanceCycles` used to call `cassette->advanceCycles`
unconditionally, even with no tape loaded — measured at 4.1 % of the core.
It is now gated (`if (cassette)`, `Memory.cpp:377`) and the call is an inline
fast path (`CassetteDevice.h:86-95`) that only takes the out-of-line playback
route when the deck is actually moving.
