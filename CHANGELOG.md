# POM2 — Changelog

Notable changes, ordered most recent to oldest. The `git log` remains the
canonical source for the exact mechanics; this file captures the **"why"**
and the pitfalls we don't want to rediscover. Active backlog → `TODO.md`.
Current implementation → `DEV.md`.

## 2026-08-08 — Core 2× faster on the same output, and a Raspberry Pi build recipe

Ported the optimisation campaign method from **NeoST** (POM2's sibling Atari ST
emulator, `../neost`): a deterministic headless subject, callgrind, then PGO.
Full write-up with the before/after numbers in **`docs/PERFORMANCE.md`**.
Everything below is **output-identical** — 166/166 tests green and byte-equal
RAM/framebuffer hashes on every workload measured.

**`pom2_bench` (new target).** `pom2_headless` cannot be profiled: worker
thread, wall-clock pacing, audio device, waits for a human. `pom2_bench` runs
exactly N frames of `cyclesPerFrame` with no threads, no audio, no sockets and
no pacing, so two runs retire the *same instruction count* — which is what makes
before/after comparisons trustworthy. It prints FNV-1a hashes of RAM and of the
framebuffer: an optimisation that moves either is a bug, not an optimisation.
It doubles as the PGO training driver. ⚠ Its own per-frame framebuffer hash was
17 % of the very first profile taken — hence `--hash-all` being opt-in.

**Flux lookup: resume the search instead of redoing it (−33 % on disk).**
`DiskImage::getNextTransition` ran a full `std::lower_bound` over the track's
flux array on every call — tens of thousands of events, ~16 probes — while
`DiskIICard::lssSync` calls it once per flux event for as long as the motor
turns. Together they were **42 %** of a disk-active profile. The class now
remembers the previous index and resumes from it. The hint is *verified*, never
trusted: the fast path re-checks that the remembered index really is the lower
bound (two comparisons) before using it, so a write splice, an eject or a
snapshot restore just fails the check and falls back to the binary search.
Nothing has to invalidate it — an invalidation you can forget at one call site
is a latent correctness bug; a self-verifying hint cannot be.

**Bus reads decided in the header (−18 % on CPU-bound loads).** `Memory::memRead`
plus the `languageCardRead` it tail-calls were **35 %** of a ROM-banner profile,
most of it the out-of-line call around what is usually one array index. The two
hot cases now inline in `Memory.h` — main RAM below `$C000` on a non-//e, and
`$D000-$FFFF` when the language card maps ROM — and everything else goes to
`memReadSlow` (the original body, untouched). The second case is the trap NeoST
also hit: fast-pathing RAM alone is worth a few percent, because *the ROM is
where the code executes from*. The //e aux/main decision moved into one shared
inline helper (`iieReadFromAux`) rather than being copied.

**PGO is the single biggest win, and it touches no emulation code**: −39 % / −29 %
on top of the above. `packaging/raspberry/build_native_pi.sh --pgo` does the two
passes, `pgo_train.sh` sweeps ][+ and //e, PAL and NTSC, every video pipeline and
a 5.25" boot — *a too-narrow profile is worse than none*, it marks live code cold.
Two traps that cost the entire gain **in silence**, both closed by the script:
GCC names each `.gcda` after the object's absolute path (so both passes share one
build dir), and — POM2-specific — the training driver is `pom2_bench` while the
shipped binary is `pom2_imgui`, two different object directories for the same
sources, so the profiles are copied across and the build **fails** if any of
M6502/Memory/DiskIICard/DiskImage/Apple2Display came out untrained. Also
`pi_tuning.sh` (governor, IRQ pinning, swap) and `packaging/raspberry/README.md`.
⚠ The Pi-specific parts are not yet exercised on real hardware; the build recipe
and both traps were validated end-to-end on x86-64.

Footnote: **LTO now measures ~0 %** on these workloads — the header inlining did
by hand exactly the cross-TU call LTO was recovering. Kept anyway, as a guard.

## 2026-08-08 — CRT/NTSC shader: negotiate the GLSL dialect instead of demanding 1.50

`OpenGLShader.cpp` hardcoded `#version 150`, so on any desktop-GL driver capped
below it the whole effect stack died with *"GLSL 1.50 is not supported.
Supported versions are: 1.10, 1.20, 1.30, 1.40…"*. Mesa's V3D (Raspberry Pi)
caps *desktop* GL at 3.1 = GLSL 1.40; old llvmpipe and several VM drivers land
in the same place. Nothing needed rewriting — POM2's shader bodies only use
1.30 constructs (`in`/`out`, `texture()`, `fwidth()`); they were merely *asking*
for 1.50.

The dialect is now read from `GL_SHADING_LANGUAGE_VERSION` and tried in cascade
**150 → 140 → 130** (`300 es` unchanged where the context is GLES — the Pi's own
AppImage tier, WASM; `150` unchanged on macOS, whose core profile has no lower
rung). The cascade is a net rather than an affectation: a driver can advertise a
version and still refuse it in *this* context, and only a real compile settles
it. Intermediate failures are silent and `errorOut` is cleared on success, or
the panel would report "shader unavailable" with the stack already running. One
startup line says what was picked and what the driver claims: `[NTSC] GLSL 140
(driver: 1.40)`. Verified under Mesa llvmpipe with forced versions — 4.50 → 150,
**1.40 → 140 (the Pi case)**, 1.30 → 130, each linking cleanly. ⚠ `MESA_*_OVERRIDE`
is ignored by the NVIDIA driver; `LIBGL_ALWAYS_SOFTWARE=1
__GLX_VENDOR_LIBRARY_NAME=mesa` is required to test this at all.

## 2026-08-02 — Bundle the full `roms/` tree in release artifacts

Packaging previously shipped only `roms/floppy_samples/` plus a drop-here
README, while the copyrighted dumps were already tracked in the public repo.
That split is closed: `cmake --install`, the Linux tarball/AppImage/.deb,
the macOS `.app`/`.dmg`, and the Windows zip all copy the entire `roms/`
directory so a release boots without a separate ROM drop. Docs and
`packaging/roms_README.txt` match.

## 2026-08-02 — SmartPort access LEDs, and the disk turbo they were hiding

The status-bar row shipped earlier today left SmartPort LEDs dark because
the units exposed no activity signal. Wiring one up turned out to uncover a
bigger miss than the lamp.

`SmartPortUnit` now carries the same hysteretic counter `Block512Backing`
uses — a block access sets it, the host bleeds it off one step per frame.
It lives on the base class and `SmartPortCard::noteAccess()` bumps it, so
one site covers 3.5" and HDV units alike; that function already existed as
the audible-motor hook and already fired at all three block-dispatch sites.
Two details worth keeping: the bump goes **before** `noteAccess`'s
`sound_` early-out, because a machine with no FloppySoundDevice still has
an LED; and the unit is passed explicitly, because the SmartPort `$Cn0D`
dispatch addresses a unit by number, which need not be the one the legacy
streaming registers have selected.

`SmartPortHdvUnit` does wrap a `Block512Backing` with a counter of its own,
so it looked like the signal was already there for free. It was not usable:
nothing decays it. The host's decay loop walks `ProDOSBlockCard`
implementers, and `SmartPortCard` implements `MountableMediaCard` instead —
so reporting that counter would have latched the lamp on permanently after
the first access.

**Which is the actual find.** That same gap meant SmartPort media never
counted toward `anyBusy`, so it sat outside **disk turbo** entirely. On
//c / //c+ / //c PAL the built-in slot-5 SmartPort *is* the boot path for
3.5" and HDV, so precisely the machines that depend on it were loading at
1 MHz while an HDV card in a //e got the ~60× speed-up. The decay loop now
covers SmartPort units and they count toward turbo eligibility.

`MediaBayInfo` gained a `busy` field, so the status bar reads activity
uniformly per bay and the old `dynamic_cast<ProDOSBlockCard*>` special case
is gone — and a SmartPort's two units light independently, which a
card-wide flag could not have expressed. Pinned by `smartport_card_smoke`:
a read lights only the unit that was read, with no sound device attached,
and the lamp decays on its own within a bounded number of frames.

## 2026-08-02 — Status bar: drop the MHz readout, show every mounted volume

The achieved-clock readout is gone, along with its sampling state — the
toolbar still shows the requested budget, and the measured figure was
costing a `stateMutex` acquisition every frame to tell most users something
they never acted on.

The media row used to show exactly **one** entry: whichever Disk II was
spinning, else the primary drive, else a mounted HDV as a fallback. A
machine with two floppies, a CFFA and a SmartPort showed one of them. It now
walks the SlotBus slot by slot — each Disk II contributes both drives when
loaded, and every card implementing `MountableMediaCard` contributes each of
its bays, so multi-instance cards appear too (the named aliases in
`MainWindow` only ever remember one card per kind, which is what limited the
old code). Entries are added while there is room and dropped silently past
that, in bus order so a given machine's row does not reshuffle as drives
spin up.

The LEDs mean different things per bay, deliberately: a Disk II lights on
real spindle motion, a block device has no mechanics and bleeds off
`Block512Backing`'s activity counter instead, and SmartPort units expose no
activity signal at all, so theirs stays dark. An honest dark LED beats one
that never means anything — giving SmartPort a real one needs an activity
signal on the card first.

The row is built as a value snapshot under `stateMutex` and drawn with the
lock released — the same discipline the sibling panel snapshots were just
given. Both halves are load-bearing: `getDiskPath()` returns a reference
into live `DiskImage` state that the AI server's HTTP thread rewrites on
`/disk` and `/eject`, and holding `stateMutex` across ImGui calls is exactly
what deadlocked the memory viewer.

`indicatorDot` gained an explicit centring height. Inside a menu bar ImGui
applies no frame padding to a bare `Text()`, so every item sits at the top
of the bar; text carries that off, a circle does not, and the drive light
rode visibly high. The status bar now passes `GetFrameHeight()`.

## 2026-08-02 — `pom2_headless` did not link Winsock

Found by the first Windows package build since 7e7d8de enabled host sockets
there: 17 unresolved Winsock externals out of `SuperSerialCard.obj`. The GUI
target had been given `ws2_32`, the headless one had not — before that
commit `POM2_HAS_SOCKETS` was 0 on Windows and the socket code compiled out
entirely, so the omission was invisible. Worth noting how it surfaced: the
compile succeeded, so the `#ifdef _WIN32` branches themselves were sound;
only the link failed. CI does not build Windows — only the release workflow
does, and it had not run since.

## 2026-08-02 — Bug-hunt sweep: 21 defects across ten subsystems

A ten-way parallel audit of the whole tree, then a fix pass. Framing that
matters for reading the rest: **the suite was 158/158 green before this,
and stayed green throughout** — every defect below sat in a gap the tests
did not cover. An ASan+UBSan build of all 156 test binaries, ~24 000
hostile-input cases against the image parsers and ~6 M random CPU
instructions produced **zero** diagnostics. Everything here came out of
reading code and then proving it with a probe. The lesson is not that
dynamic analysis is weak but that it is blind where this codebase actually
breaks: the GUI, and the seams between threads.

### The four that could bite a user hard

**Print now, free later.** The ImageWriter panel bound `const Page&` to a
completed sheet at the top of the frame, then let "Print now" call
`flushPending()` before `uploadPage()` read it — an eject `push_back()`s
into `pages_` and reallocates. Heap use-after-free, confirmed under ASan,
two clicks from ordinary multi-page printing. At the 32-sheet cap the same
call `erase()`s the front instead, silently renaming every index. What
makes this worth remembering: the sibling buttons "Reset printer" and
"Clear all" were deliberately placed *before* the reference is taken, with
a comment saying why — the discipline existed and one button escaped it.
So the fix is not a reorder: sheet selection is now derived state resolved
on demand and re-resolved after every call into the printer, including the
paper-size / DPI / speed controls, which eject too and were a second
latent instance nobody had noticed.

**Editing a byte froze the emulator.** `renderMemoryViewerWindow()` held
`stateMutex` across `memViewer->render()`, and the write callback re-locked
that same non-recursive `std::mutex`. The UI thread deadlocked *while
holding* the lock the CPU worker needs, so the whole machine stopped, not
just the panel — the exact hazard `MainWindow.cpp` already documents for
`eject35()`. Edits, undo and redo are now staged and drained by
`flushPendingWrites()` after the lock is released, with the contract
written on both methods. Found while pinning it: the inline editor also
*never committed*. `SetKeyboardFocusHere()` posts a nav request ImGui
resolves in `EndFrame`, so `IsItemActive()` is false on the frame that
requests focus and the old cancel closed the box one frame after the
double-click. Byte editing had been quietly dead, which is why the
deadlock went unreported.

**Two Mockingboards, one alias.** `"mockingboard"` (A/C) and
`"mockingboard_c"` (Sound II) are distinct catalog keys, so the duplicate
guard — which compares key strings — lets both plug. But `plugMockingboard`
overwrites a single `mockingboardCard` raw alias, and teardown unregistered
only that one before `slotBus().clear()` destroyed both cards. The
miniaudio thread then dispatched through freed memory every ~5 ms.
`MainWindow` now keeps an inventory of every `AudioSource` it registered,
so a future card cannot reintroduce this by adding a second catalog key.

**A ProDOS volume that never stops unpacking.** `decodeVolumeToFolder`
walked a *guest-writable* image as if it were a tree. A subdir entry's
`key_pointer` was only range-checked, so one pointing back at an ancestor
made the graph cyclic, explored to depth 16 with a fan-out of 13 slots ×
256 chained blocks — each visit doing a real `create_directories`. Measured
before the fix: fan-out 2 produced **262 143 host directories in 10.8 s**;
fan-out 12 was still running when killed at 25 s. It sits on the eject/save
path, so POM2 could never quit while it filled the user's disk. A
malicious image is not required — a crashing guest that scribbles on block
2 will do. The depth cap was never the right tool because it bounds no
fan-out; the walk now carries a global set of expanded blocks (so each is
walked at most once) plus a hard directory budget, and reports what it
skipped instead of emitting a partial tree in silence.

### Mockingboard: two user-reported symptoms, one root shape

Both came from the same place — the audio thread's replay cursor runs
~40 ms behind CPU-now on purpose, and anything that reaches it *out of
band* arrives at the wrong time.

**Rewind silenced the card.** The cycle-stamped queue had no way to learn
the CPU timeline had jumped. After a rewind or snapshot load, `pending`
was full of pre-rewind stamps that the front-ordered render loop read as
"not due yet", blocking everything behind them: measured 0.49 s / 2.00 s /
>3 s of total silence at rewind depths of 0.5 / 2 / 5 s. The in-code
comment asserting that `caughtUp` already handled rewind had the reasoning
inverted — it moves the *cursor*; the damage is in the *queue*. A
generation counter now purges both queues and re-primes from the live bank.
Regression from 2c385ce, which removed the old per-callback `pending.clear()`
in favour of a persistent jitter buffer.

**A note hung between DIX demos.** The wholesale bank wipe reached the
audio thread through `ayResetCount_`, i.e. at CPU-now — so the ~40 ms of
already-queued pre-reset writes replayed *on top of* the zeroed bank and
the board held its last note forever. The trigger is in DIX's own GPLv3
`loader.a`: `RESET_AY`, called at every demo hand-off, silences the board
with nothing but the /RESET strobe and no volume writes. That also explains
why the user saw it only *sometimes* — `CCII_2016` silences by writing 0 to
R8/R9/R10, ordinary stamped writes that were never broken. The strobe now
travels as a `kRegAyReset` event and lands at its true cycle stamp, and
`resetGenerators()` was completed to MAME's full `ay8910_reset_ym`: it had
only reseeded the noise LFSR, so a chip mid-envelope or mid-tone-phase
carried that state across a reset. PhasorCard inherits the same fix.

**And the bass.** `AyPsgSynth.h` justified a 1-pole DC blocker by citing
MAME's `audio_effects/filter.cpp`. That citation was wrong: MAME's is a
**2-pole Butterworth biquad** (`DEFAULT_Q = 0.7071067f`, high-pass on by
default), which is maximally flat where a 1-pole is already drooping. Same
corner, different passband. Ported verbatim; measured recovery of 0.77 dB
at 27.5 Hz, 0.46 dB at 55 Hz, 0.23 dB at 82.5 Hz, now tracking the analytic
MAME response to within 0.01 dB. Stated plainly because it matters for the
next person chasing this: **≤0.8 dB below 80 Hz will not on its own be what
anyone hears as "missing bass."** The volume table (within 0.0007 of
Westcott's measurements), the linear channel sum, the box integrator (sinc
gain 1.0000 below 100 Hz) and the stereo split were all measured against
MAME and found already correct. If the perception persists, the next place
to look is the mixer's `/3` normalisation against MAME's `0.5` route gain —
downstream of the card, not inside the AY synthesis.

### Timing and hardware parity

**The VBL frame phase froze after an NTSC↔PAL switch.** `advanceCycles`
tracks the start-of-frame cycle incrementally (6e9e0f2), and the invariant
is `vblFrameBase_ % frameCycles == 0`. `setVideoStandard()` changes
`frameCycles` on a running machine and nothing re-derived the base — and
because 17030 and 20280 sit within a factor of two, the rollover branch can
never notice, so the stale residue persists forever. Boot NTSC, load the
//c PAL profile, and the VBL edge lands on scanline 252 instead of 192,
disagreeing with `$C019`, `pushVideoEventLocked` and `frameCycleToPos`,
which all take a true modulo. That is precisely the 50 Hz frame sync the
French Touch / DIX demos rely on. The base now re-aligns whenever the
*derived* period moves, so any future input feeding it re-aligns too.

**A plain annunciator poke armed a real IRQ on //c.** The `$C05A`/`$C05B`
→ VBL-mask overlay is a POM2 //e compatibility shim, but it was gated on
`iieMode`, which is also true on //c-class; since IOUDIS resets to *set*,
the MAME-faithful IOU decode was bypassed and the legacy `LDA $C05B` idiom
armed the mask. The arming guard said `iieMode` while the asserting guard
said `iicProfile_` — and on //c, unlike //e, the line really is driven, so
the guest took an unhandled 50/60 Hz IRQ storm through `$FFFE`. `LDA $C05A`
symmetrically ACKed an interrupt a //c guest had legitimately armed. MAME
`apple2e.cpp:1808-1876` keeps DisVBL/EnVBL strictly inside the
`(m_isiic || m_isace500) && !m_ioudis` branch and otherwise falls through
to plain AN0/AN1/AN2.

**The mouse MCU ran 26-50 % fast, and lost its interrupt on rewind.**
`advanceCycles` debited the accumulator by the requested budget while
`mcu.run()` finishes the instruction straddling the edge; with
per-6502-instruction budgets of 4-14 MCU cycles the discarded overshoot was
comparable to the budget itself — measured 2.4906 MCU cycles per bus cycle
against the intended 2.0, now 2.000003. Separately,
`MouseCard::loadSnapshotState` re-derived the slot IRQ from
`pia.irqA() || pia.irqB()`, but this card's IRQ is MCU port B bit 6 —
MAME's `pia_irqa_w`/`pia_irqb_w` (`mouse.cpp:235-240`) are empty stubs, so
that expression was an unconditional `assertIrq(false)` and a rewind taken
mid-MousePaint-handshake killed the mouse until reset. `MouseCardAppleWin`
serialized nothing at all and gained a snapshot blob.

### Networking, and a class of Windows-only divergence

The Windows socket paths from 7e7d8de are new, and three of them were wrong
in ways POSIX hides.

**UDP reads were sized against the ring, not the datagram.**
`recvfrom`'s buffer was `freeRoom - 1`, so with `RMSR $00` a standard
1472-byte reply into a 1 KB ring came back truncated on POSIX — the kernel
discards the remainder and reports *no error*, and the in-band length
stamped into the ring described a datagram the guest never received
(measured on Linux: 1015 bytes stamped "1015" out of 1472). The same call
on Winsock fails with `WSAEMSGSIZE`, which the error arm read as "socket is
dead". UDP now reads into an 8 KB scratch buffer — sized so truncation
cannot pass `ringHasRoomFor` by construction — and drops a datagram the
ring cannot take. TCP stays clamped to the ring, because dropping stream
bytes would tear a hole.

**Errors that describe a packet were killing the socket.** On Winsock an
ICMP port-unreachable from an earlier `sendto` surfaces as `WSAECONNRESET`
on the next `recvfrom` of an *unconnected* UDP socket, so one datagram to a
closed port destroyed the guest's socket. `SIO_UDP_CONNRESET` is now off at
creation and a per-packet error set is tolerated — **for UDP only**, since
on TCP `ECONNRESET` genuinely is the connection dying. Likewise `recvfrom`
returning 0 is a zero-length datagram, not a close; it used to fall into an
arm that destroyed the socket, an arm which turned out to be reachable
*only* in that case.

**`SO_REUSEADDR` means the opposite of what it means on POSIX.** On
Winsock it lets a second socket bind an address another is already
listening on, and the later binder wins new connections — so any local
process could take over the AI control listener and collect its token. The
intent now goes through `setListenerBindPolicy()`: `SO_REUSEADDR` on POSIX
for the wanted `TIME_WAIT` relaxation, `SO_EXCLUSIVEADDRUSE` on Windows.
These branches were verified by reading only — there is no Windows host or
mingw cross-compiler here — but the new test is written entirely through
`SocketCompat.h`, so it will exercise them for real the day a Windows CI
exists.

### Smaller, but real

- **Screen capture froze a soft 560-wide text screen.**
  `demodCompositeForCapture()` rewrites the framebuffer *after* `render()`
  has returned, and the static-text skip key survived it. The fix is
  structural rather than a call added at the guilty site: the key is now
  published at the end of `render()` behind an RAII, `useFrame80` was
  renamed `useFrame80_` so a bare assignment no longer compiles, and every
  mutation invalidates. Verified the optimisation still bites — 400 static
  text frames cost 0.09 ms skipping vs 52.6 ms repainting, a 582× ratio now
  pinned by the test.
- **`ESC R` / `ESC V` / `ESC U` froze the UI.** All three expanded a whole
  run inside the single byte that completed the sequence — past both
  catch-up budgets, which are only checked *between* bytes, and free of
  credit, since `byteCost` returns 0 while `numParam_ < neededParam_`.
  `PRINT CHR$(27);"R999";CHR$(12)` cost 773 ms in one tick at defaults and
  13.8 s at 288 dpi/Ledger while wiping the 32-sheet tray; `ESC U 9999` cost
  1.4 s per catch-up tick. A repeat is now resumable state, worst tick
  0.9 ms, and the count is never clamped — a real printer does print 999
  characters.
- **Disk-path snapshots were taken unlocked, by reference.**
  `getDiskPath()` returns a view into live `DiskImage` state, and
  `controller->stop()` parks only the CPU worker — the AI server's HTTP
  thread keeps serving `/disk` and rewrites those strings. Three snapshot
  builders now lock and copy, matching the sibling panels that already did.
- **`saveScreenshot` bypassed `demodMutex`**, running the same ~1-2 ms
  demod as the AI server's `/screen.ppm` handler with no shared lock.
- **`Disk35Image::saveDirty` truncated the user's 800K image in place** —
  the failure `DiskImage::saveDirty` was hardened against. Now temp file +
  `rename`, permissions carried across.
- **`ClockCard` raced the UI over `std::localtime`'s shared static `tm`**;
  switched to `localtime_r`, as `NoSlotClock` already did. The two UI-side
  callers were converted too.
- **The W5100's `$8000+` mirror was asymmetric** — writes masked before the
  range test, reads did not, so a guest reading `$8403` got plain memory
  instead of socket 0's status.
- **The Audio Mixer's pan slider sat ~100 px off-screen** at the panel's
  default size: the row's hard-coded pixel offsets did not follow
  `uiScale_`/`dpiScale_` while the text and padding did. Widths are now
  font-relative.

### The WASM build had been red since 7e7d8de

Not from this sweep — found while checking CI before pushing it. The Windows
socket commit left `W5100Device.cpp` naming `htons` / `ntohs` /
`SOCK_STREAM` / `IPPROTO_TCP` outside the `POM2_HAS_SOCKETS` guard, and
Emscripten compiles that file (MACRAW/IPRAW framing and the SnMR mode switch
go through `NetworkBackend`, not through a socket). Two CI runs in a row had
failed on it while the Linux job stayed green, so the tree looked healthier
than it was.

The file's own header comment already claimed `SocketCompat.h` supplied
those symbols "even where no socket is opened" — it did not. It does now:
`pom2::hostToNet16` / `netToHost16` are spelled out as the arithmetic they
are rather than borrowed from `<arpa/inet.h>`, and the protocol selectors
get a `pom2::kSockStream` family that exists in both builds. Code that opens
no socket no longer depends on the socket stack. Verified locally with
emsdk, not just left to CI: `build_wasm.sh` produces `POM2.{js,wasm}` again,
and the committed `wasm/` bundle is refreshed with it.

### Notes for next time

`disk_path_snapshot` was flaky on arrival — a reader loop that re-locks
immediately can starve the writer to zero mutations under `ctest -j`,
because `std::mutex` is not fair. Bounding the *writer* and letting the
reader run until it finishes makes the interleaving the thing under test
instead of the scheduler's generosity. Worth copying into any future
thread-stress test here.

Still open, and deliberately not done in this pass: a **ThreadSanitizer**
run over `EmulationController` / `stateMutex` / the audio thread, which is
where ASan is structurally blind and where this sweep's own findings
cluster; the `SnapshotIO` fuzzer that was built but never executed; and the
three divergent copies of the atomic-write helper (`DiskImage.cpp`,
`Disk35Image.cpp`, `ProDOSVolume.cpp`), none of which `fsync` before the
rename, so a power cut can still land an empty file.

## 2026-08-01 — ImageWriter: the freeze, the reprint, the overprint, and four reference bugs

Follow-up to the //c fix below, from the same multi-agent audit of the
print chain. Everything downstream of the interface card had held up under
compiled probes and ASan/UBSan — no memory-safety defect anywhere — so what
was left were behavioural faults, each of which needed its own reasoning.

**The freeze.** `queueBytes()` printed the whole backlog synchronously
once it passed 1 MiB. On the UI thread, from `pumpImageWriter()`. Measured
with a probe: 852 ms for plain text and **301 s for a form-feed storm, in
a single frame**, while audio and the CPU worker carried on — which reads
as a hard freeze, not a slow printer. The bug is a category error: the
credit cap in `tick()` bounds credited *seconds*, and nothing ever bounded
the *work*. Catch-up now happens across ticks under two budgets, bytes and
sheet ejects, budgeted separately because an eject copies a whole page
raster and so is orders of magnitude dearer per byte than a glyph. Worst
frame is ~14 ms. Past a 4 MiB hard ceiling the oldest input is dropped and
counted, the rule the page stack and the SSC spool already follow:
truncating a printout is bad, freezing the emulator is worse.

**The reprint.** One drain cursor serves three possible sources, and it
was re-seated at 0 whenever the source changed. A spool can outlive its
source status — the SSC tap's does, nothing clears it — so one frame with
"Feed ImageWriter printer" unticked reprinted the entire session on the
next frame. Worse on a //c, where slot 1 is the printer port and slot 2
the modem port and both are SSCs: unticking slot 1 handed the source to
slot 2 at 0 and printed the whole modem transcript onto paper. It now
adopts the new source's current total, which also gives the physically
right answer for the toggle — while the box is unticked the cable is out,
and what the guest sent meanwhile went to a port with nothing on the end.
The arithmetic moved to `PrinterFeedCursor.h`, header-only, because it
lived in `MainWindow.cpp` where no test could reach it.

**The overprint.** `resetPrinter()` did not re-arm the CR/LF detector, and
nothing else a guest can send did either — so the latch was scoped to the
host session rather than the job. Once one CR+LF driver had latched it,
every later `PR#n : LIST` printed its whole listing onto one black line,
unrecoverable from inside the guest. `ESC c` ("initialize printer") now
re-arms it. That is safe as the re-arm point precisely because Print Shop
separates its colour passes with a bare CR and never sends `ESC c` — both
directions are pinned, because getting this wrong in the other direction
brings back the coloured staircase the detector exists to prevent.

**Four reference bugs.** Auditing the port against greg-kennedy's
`imagewriter.cpp` proves faithfulness, not correctness. Checked against
the *ImageWriter II Technical Reference* instead, four faithfully-ported
behaviours put visibly wrong ink on paper, so POM2 now deviates
deliberately (documented at each site, per the CLAUDE.md convention):
HT/VT went to the *farthest* tab stop rather than the nearest, so with
stops at 10/20/30 the first TAB jumped to 30 and every later one was a
no-op; `ESC 1`..`ESC 6` assigned an absolute head position instead of
adding intercharacter space, throwing the head backwards out of the left
margin mid-line and destroying justified output; `ESC c` binned the sheet
on the platen, which the reference can afford because it wrote pages to
disk but here is silent data loss; and `ESC H`/`ESC L` took any parameter,
so `ESC H 0000` ejected on every line feed and `ESC L 999` put the margin
83″ off the sheet — both silently.

Also: the stall watchdog's patience now scales with the head byte's cost
instead of a flat 10 s, so a legitimately long form feed is no longer cut
short and logged as a STALL; and the ImageWriter panel names any printer
card that is plugged but *not* feeding, since Slot Config allows a
`PrinterCard` and a `Grappler+` at once and the loser was silently dead.

One reported defect was verified and deliberately left alone: `ESC D`/`ESC
Z` cannot set bit 7 of either soft switch, because the bit-7 mask runs
before the escape parser. Neither bit 7 is wired to anything here — A-8 is
the "LF after CR" switch, which POM2 models with `AutoFeed` rather than
the switch byte, and B-8 is unused — so the change would be a no-op with
nonzero regression risk. Recorded in DEV.md rather than fixed.

## 2026-08-01 — The //c prints again: an armed printer tap is a device on the pins

`PR#1` on //c, //c+ and //c PAL hung the guest and printed nothing. Not
"printed badly" — the machine wedged inside its own printer firmware and
never came back, on three of the eight shipped profiles, with no
workaround available: a //c has no physical slots, so the built-in SSC
printer port is the *only* route to the ImageWriter.

The cause is a two-days-apart interaction between two correct changes.
`$C100` on a //c is **internal** ROM, so `PR#1` runs the machine's own
printer-port firmware rather than the card's synthetic `PR#n` ROM — and
that firmware gates every single character on the 6551 status register,
spinning until `status & (DCD|TDRE)` reads "carrier present, transmitter
empty". On 2026-07-30 the DCD/DSR polarity was corrected to match MAME
(`mos6551.cpp:37-39` inits `m_dsr(1), m_dcd(1)`) and AppleWin
(`SerialComms.cpp:864` returns `ST_DSR|ST_DCD` "when nothing is
attached"). That fix was right — POM2 had the sense inverted, so a
carrier-aware guest saw "online" with an idle listener. But it answered
the pins from the **telnet connection alone**, and two days earlier
(2026-07-28) the printer tap had shipped as a second kind of device on
the same port. With no telnet client, the //c was told its printer was
absent, and it waited for a carrier that a printer never has.

The fix is not a revert — the modem polarity stays as MAME has it. It is
that "nothing is attached" was simply false: an ImageWriter cabled to the
port *is* a DCE sitting there with its lines up. `deviceAttached()`
(= telnet peer **or** armed printer tap) is now what DCD/DSR answer to.
One condition, and the //c prints.

Worth recording is **why the existing test passed throughout**.
`ssc_acia_smoke` does exercise the printer tap — but it drives it through
the *card's* synthetic `PR#n` ROM, which only ever checks TDRE. It is
structurally blind to DCD, so it could not have failed here no matter how
the polarity moved. Nothing booted a //c profile and ran `PR#1` through to
the spool, which is exactly the gap a "//c printing" feature needed
covered. `iic_printer_port` now closes it at three levels: the DCD/DSR
device-present contract, the firmware's `status & $30 == $10` wait-loop
shape, and a real DOS 3.3 boot on all three //c ROMs where `PR#1` +
`PRINT "HI"` must land bytes in the spool. Against the pre-fix source all
three ROMs fail with the PC frozen in firmware ($C2BA / $C1C2 / $C2B7);
post-fix each spools the echoed command line and its output. The
end-to-end half is ROM/disk gated and skips rather than fails when the
user-provided media is absent.

## 2026-08-01 — Host sockets on Windows: the Uthernet II now has a network there

`POM2_HAS_SOCKETS` was 0 on Windows, which took out more than it looked
like: the Uthernet II's TCP and UDP paths, the Super Serial Card's telnet
bridge and the AI control server were all compiled out of every Windows
build. The card still plugged, reset and answered its registers — it just
never saw a packet.

The blocker was never capability. Windows has the same stack behind
Winsock2, and the difference is an API, not a feature. What made it worth
a header rather than a scatter of `#ifdef`s is that **every one of those
differences is silent** — code that compiles clean against Winsock can
still be wrong:

- `SOCKET` is **unsigned** and its failure value is `INVALID_SOCKET`, not
  -1. So `if (fd >= 0)` is always true, and `fd = -1` marks a socket as
  *valid* with a huge handle. Every "is this open?" test in the POSIX
  idiom inverts its meaning without a single warning.
- Errors bypass `errno`: `WSAGetLastError`, different codes, and
  `strerror` cannot render them.
- `close()` closes a CRT file descriptor, a different namespace from
  sockets; `closesocket()` is the one that works. `fcntl(O_NONBLOCK)`
  does not exist.
- The stack needs `WSAStartup` before the first call.

`src/SocketCompat.h` is now the one place that answers "POSIX or
Winsock?", and the three TUs are written against it.

**A fifth trap was not Winsock's fault and bit anyway.** `W5100Device`
already had a member `closeSocket(size_t)` — the chip-level CLOSE for one
of its four sockets. Inside that class, unqualified lookup finds the
member first and stops; `socket_t` then converts to `size_t` without a
murmur. `closeSocket(s.fd)` compiled clean and recursed until the stack
died — `uthernet2_w5100_smoke` caught it as a segfault with 74 000
identical frames. The helper is now named `closeHostSocket`, which no
card would plausibly use for a chip-level operation.

**Readiness waits use `select()` on Windows, not `WSAPoll()`.** The
caller that decides this is `W5100Device::poll()`: it waits for WRITE on
a socket with a non-blocking connect in flight, and it has to learn about
a *refused* connection, not just a successful one. On Winsock the
documented channel for that is `select()`'s `exceptfds` — which is why
Winsock's select takes one. A wait that could only report success would
leave a guest polling `SN_SR` forever on a connection that was refused.

Two smaller Windows facts, both of the silently-wrong kind: `SO_RCVTIMEO`
takes a `DWORD` of milliseconds there, **not** a `timeval` (pass a
timeval and it is accepted, then read as garbage — a 2-second timeout
quietly becomes minutes), and `inet_ntoa`'s static buffer lets two
threads logging a connection splice each other's addresses, so both
workers now use `inet_ntop` via `peerAddressText`.

Verified by cross-compilation, not by reasoning: `x86_64-w64-mingw32-g++
-fsyntax-only` over **every** `src/*.cpp`. 83 compile for Windows
outright; the 9 GL/UI ones needed only GLFW's header staged, including
`MainWindow.cpp`, which is what proves the winsock1-vs-winsock2 include
ordering is safe in the file most likely to break it. The one remaining
ordering hazard — a TU that pulled `windows.h` in first — is now a single
`#error` instead of fifty redefinition errors. Linux: 156/156 ctest.

**What this does NOT cover: the Uthernet I on Windows.** It is a plain
NIC, so it needs raw frames, which means libslirp. vcpkg does carry a
libslirp port (4.9.1), so the library is obtainable — what is missing is
POM2's side: `SlirpNetworkBackend`'s poll loop is written against POSIX
`poll()` over the fds libslirp hands back, and that port cannot be
verified without a Windows libslirp build to test against. CMake
therefore does not look for libslirp on WIN32 at all, and says so; a
documented absence beats trading it for a wall of missing-header errors.
The vcpkg port also pulls glib, which is a real addition to the Windows
CI budget. Tracked in TODO.

## 2026-08-01 — The audio bus goes stereo

The Mockingboard is a stereo card and POM2 was summing it to one channel.
MAME wires it to a single 2-channel speaker with AY1 on channel 0 and AY2
on channel 1 (`a2mockingboard.cpp:159-165`); the Phasor gets a second one,
so left = ay1+ay2 (the VIA1 pair) and right = ay3+ay4 (the VIA2 pair)
(`:192-208`). Digidream 1 writes a deliberate A/B/C pan, and the mono sum
destroyed it — which is also why single-AY software (Digidream 2 never
touches chip 2) sat 6 dB down: it was being normalised for two chips'
worth of headroom while only ever filling one.

**Nothing moved level.** That was the constraint the design had to
satisfy, because a silent 3 dB shift across every source is the kind of
regression nobody reports and everybody hears. Three things follow from
it:

- The **mono contract is unchanged**. `fillAudioBuffer` still hands a
  source one channel; the mixer places it with `AudioSource::pan`, whose
  law is a **balance**, not constant power — centre is unity on *both*
  channels. Constant power is the textbook choice and it would have put
  the speaker, the cassette and both floppy-sound devices 3 dB down on
  day one, in exchange for faithfulness to a stereo position the Apple's
  own speaker does not have.
- Cards that really are stereo override `fillAudioBufferStereo` and own
  their placement (`pan` is then ignored — the card's wiring is the
  authority, not a mixer knob). Both keep a **mono fold-down** of
  `0.5 * (L + R)`, which is bit-for-bit the pre-stereo render: `/3` per
  side folds back to the Mockingboard's old `/6`, `/6` per side to the
  Phasor's `/12`. The test asserts that identity sample-for-sample and
  measures 0.0 worst-case error.
- The **mono-downmix switch** averages rather than sums, for the same
  reason: summing would have made every centred source 6 dB louder the
  moment a user ticked the box.

Speech stays centred: MAME routes the Mockingboard's speech chip to both
channels at unity (`:186-189`) and gives the Echo+ TMS5220 a
`front_center` speaker (`:210-219`). Where a *pair* of speech chips would
sit has no oracle, so it is a documented gap rather than a guess.

The switch is in the mixer panel next to per-channel master meters, and
the mono sources gained a pan knob (right-click to centre). Settings:
`audio_mono_downmix`, `speaker_pan`, `cassette_pan`,
`floppy_sound_pan[_35]`.

Pinned by `tests/audio_stereo_test.cpp`: the pan law including the centre
= unity guarantee, stereo passthrough, the downmix, per-chip placement on
both cards (the silent side must be *exactly* silent — any leak means
something is still summing), the fold-down identity, and a hard-panned
card mixed next to a centred source.

Also in this pass: `setup_imgui.sh` no longer aborts the whole setup when
`apt update` fails. It fails as a whole if *any* configured repository is
unreachable — one stale third-party PPA is enough — and under `set -e`
that killed the run before Dear ImGui was even cloned, despite every
package we actually need having refreshed fine.

## 2026-08-01 — Mockingboard audio: the write queue collapsed, and the synth never band-limited

Digidream 2's Mockingboard music sounded coarse. Four things were wrong; the
first is the one that mattered, and it was not in the synthesiser at all.

**1. ~90 % of AY register writes were dumped at the buffer edge.** The CPU
worker publishes one video frame of writes in a single burst (~17045 cycles),
while one audio callback only covers `periodSizeInFrames` = 256 samples =
~5937 cycles. So every burst carried ~3 callbacks' worth of *future* writes.
`fillAudioBuffer` drained the whole queue each callback, replayed what fitted
in this buffer's cycle span, applied **all the rest in bulk at the buffer
edge** — where, being applied in order to the same register bank, only the
last value written to each register survived — and then set
`audioCursor = pending.back().cycle`, parking the cursor on the newest event
at zero lag. Two callbacks later it had over-run the next burst and tripped
the backward re-anchor, roughly every third buffer.

DD2 is exactly the workload that destroys. A headless trace of the real disk
(150 s, 169 930 writes) shows **54 % of its entire register traffic is R8** —
an Atari-ST style "SID voice" that toggles channel A's volume register in a
50 % duty square between 129 and 1006 Hz, driven by VIA1 T1+T2 interrupts.
That modulation cannot survive being collapsed to one value per burst.

The queue is now a jitter buffer: un-rendered events stay queued instead of
being dumped, and the cursor deliberately runs about one producer burst
*behind* the newest event, with a +/- one-burst deadband so steady-state
playback never snaps at all. Costs one PAL frame of added latency on this
card; buys correct sub-buffer placement.

**2. The synthesiser point-sampled a signal it had already resolved.** It
advanced tone/noise/envelope in integer clock/8 ticks inside a per-output-
sample loop — then read the mixer *once*, throwing away the sub-sample edge
position it had just computed. Every square-wave edge snapped to the 44.1 kHz
grid (+/-22.7 us of jitter) and everything above Nyquist folded back in.
Measured: **7 % of total output power was inharmonic** on an ordinary 4 kHz
note; at envelope periods below 2, whole envelope steps were never sampled.

MAME does not have this problem because it never renders at the output rate:
its stream runs on the chip's own clock/8 grid (`ay8910.cpp:1298`) and a real
decimating resampler takes it to the device rate (`src/emu/resampler.cpp`).
POM2 renders straight to the device rate, so the decimation now happens
inline — the mixer is **box-integrated** across the ~2.9 base ticks each
output sample spans, weighting the partial ticks at both ends by their true
duration. Inharmonic energy drops to **0.51 %** (-22.9 dB), and edge position
becomes continuous again, which is what the volume-register PWM needs.
Cost: 0.67 % of one core per chip at realtime.

**3. No DC blocking, on a unipolar model.** A channel contributes
`kVolumeTable[level]` or nothing, so a 50 %-duty tone carries a DC term of
half its amplitude and a channel with tone *and* noise masked off in R7 (the
digi/PWM configuration) is pure DC. Every note and volume write stepped that
offset — audible clicks, and half the headroom spent on silence. Now a 1-pole
20 Hz high-pass, matching MAME's default per-speaker filter
(`src/emu/audio_effects/filter.cpp:39-44,63-68`).

**4. Level and clock.** `sample / 6.0f` normalised for *both* AYs at once,
so the very common single-AY tune (DD2 never touches the second chip) sat
6 dB down and users made it up on the volume slider — amplifying the aliasing
along with the music. Now `/3` with a `tanh` soft knee for the genuine
two-chip peak. Separately, the AY tick rate was pinned to the NTSC constant;
pin 22 is wired to the slot's phase-0 line, so a PAL machine really does
clock the chip at 1 015 625 Hz. It now derives from the live CPU clock —
PAL music was **12 cents sharp**, and the French Touch / DIX corpus this path
exists for is PAL.

**Extraction.** `MockingboardCard` and `PhasorCard` carried verbatim copies
of the synthesis (~130 lines, 4 differing lines) and had already drifted:
Phasor never gained the cycle-stamped event queue. Both now share
`src/AyPsgSynth.h` (generators, mixer, box integration, DC blocker), so an
audio fix cannot land on one card and silently skip the other. Phasor is
-149/+51 lines. It still lacks the event queue and a `setCpuClock` override —
both real, both now the only remaining divergence rather than a hidden one.

**Not changed, and worth recording as verified rather than assumed.** The
envelope state machine was suspected of an off-by-one on the alternate
shapes (`$0A`/`$0E`), where `envStep--` reaches -1 and the code tests
`envStep & 0x10`. It is correct: `-1 & 0x10 == 0x10` in C++, the same integer
promotion MAME's `s8 step` gets. All 16 shapes are now decoded back out of
the rendered audio and compared against MAME's step sequence in
`mockingboard_audio_quality`. The noise LFSR taps, prescale and period-0
handling were likewise checked against `ay8910.h:263-273` and are right.
The `kAyVolumeTable` **provenance comment** was wrong, though — it claimed
MAME's `build_single_table(normalize=1)`, which actually maps to
[-0.125, +0.375] and only applies under `AY8910_LEGACY_OUTPUT`. The data is
Westcott's measured curve renormalised; citation corrected in place.

New `mockingboard_audio_quality` test: spectral purity (FFT, inharmonic
energy), residual DC, volume-register PWM placement under a reproduced
bursty producer, and the 16 envelope shape sequences. Nothing in the suite
asserted on rendered audio before this — `mockingboard_smoke` only checked
"not silent" and pitch to +/-6 %, so every defect above passed it.

### Same day, follow-up: one regression reverted, one hypothesis retracted

Listening on the real disks corrected two things in the above.

**The `/3` + `tanh` soft knee was a regression and is reverted.** Reported
symptom: Digidream 2 much improved, Digidream 1 worse — glitchy tempo and
timbres that "do not correspond". The split is diagnostic. DD2 drives ONE
AY, so its sum never left the near-linear region of `tanh`; DD1 drives
BOTH, and its 6-channel sum routinely exceeds unity, so the waveshaper was
compressing and intermodulating it continuously. Mixing is linear `/6`
again, on Mockingboard and Phasor. Loudness is a knob the user already
has; a waveshaper across the whole mix is not something to spend it on.
The honest fix for single-AY level is true stereo, where each side carries
one chip — see `TODO.md` [Audio].

**The Digidream 1 cause, measured on the real disk.** An A/B harness
(`tests/dd1_audio_ab`) boots `DD.dsk` on //e PAL, drives one 50 Hz frame
of CPU then pulls 882 samples in 256-frame buffers, and renders through
the old renderer, the new one, and a cycle-exact zero-queue oracle. All
three produce byte-identical AY write logs, so the comparison isolates
the renderer.

The fault is the `caughtUp` guard measuring lag against
`latestAyEventCycle_` — the last WRITE — instead of against CPU-now. A
music driver writes the AY in one dense clump per frame and then leaves
it alone: DD1 is write-silent for **88 % of every frame**, worst
inter-write gap **17.6 ms**. That silence is charged against the guard's
budget as if the consumer had caught up. Measured margin before a false
trip: **1076 cycles = 1.06 ms** on DD1, against **9.6 ms** on DD2, whose
writes are 40x denser — which is exactly why one demo glitched and the
other did not.

Each false trip is a real dropout: the cursor jumps back ~30 ms and the
register bank then freezes for **7-8 consecutive callbacks = 40-46 ms**,
two frames of music. Frequency per 25 s under ordinary conditions: host
0.5 % slow → 4, host 1 % slow → 8, one dropped frame per 200 ms → 71.
Fixed by pacing against `lastSyncCycle_` (the VIA's synced "now", which
keeps advancing through write silence).

Note the earlier `!pending.empty()` guard, added from live instrumentation
that showed an endless `ANCHOR caughtUp lag=-3270`, does **not** fix this
— measured identical trip counts with and without. That instrumentation
had caught a genuine but different defect (an idle producer plus a
re-anchor target that collapsed to cycle 0); `pending` is empty in only
179 of 4306 callbacks and all 179 precede the first AY write. Both fixes
are kept; only the second one addresses DD1.

**A second defect, introduced by the lag itself.** `ayEnvWriteCount_` is
a CPU-now counter, but the cursor deliberately runs ~40 ms behind it, so
honouring it restarted the envelope a second time, ~40 ms early —
**202 spurious retriggers in 25 s** of DD1 (its 103 R13 stores x 2 chips),
moving 13.8 % of the render's RMS. The replayed event already covers
same-value R13 stores, since the producer queues an event for every write
regardless of value, so the counter path is simply dropped.

**Where the timbre change actually came from.** Attribution over a 25 s
render: **89.5 % of the OLD output's total power sat below 50 Hz.** DD1's
digidrum is unipolar PCM, so the old render carried an enormous sub-audio
pedestal that every hit stepped. The DC blocker removes it — peak 0.64 →
0.49, audible level actually **+0.79 dB**, so the "quieter" impression is
the missing bottom end, not lost loudness. Box integration is the small
term (waveform correlation 0.95-0.99 against point sampling; centroid
1327 → 1143 Hz, >6 kHz share halved). The replay timing dominates
everything: OLD vs NEW correlates only 0.10-0.22, and swapping the point
sampler back in changes that by <0.002.

Ruled out with measurements, so nobody re-opens them: envelope hold (DD1
only ever writes shape `$08`, so `envHolding` is never set); the PAL tick
rate (12.05 cents, uniform — a pitch shift, not a tempo change); queue
overflow (`pending` max 731 vs `kMaxAyEvents` 16384); disk turbo (7 turbo
frames, all before music, 1 `starved` at t=1.04 s and none after); buffer
size (256/480/512/1024/2048 all clean in the ideal cadence).

**Methodology, because this cost real time.** Two successive synthetic
harnesses PASSED against the very bugs they were written to catch. The
first produced NTSC-sized bursts while the target lag is sized in PAL
frames, so the lag never swept its critical range. The second used an
unbroken write stream, so it never modelled a production gap — which is
exactly the condition the live instrumentation caught. A Mockingboard
audio regression test is only credible once it has been demonstrated to
FAIL against the reverted fix; test 3c in
`tests/mockingboard_audio_quality` carries an explicit note that it does
not discriminate the defect it accompanies.

## 2026-07-31 — 6522 T1 continuous period is latch+2, not latch+3 (a frame clock that drifted)

`Via6522::advance` reloaded T1 in continuous mode with `latch + 3`, copied
from MAME's `t1_tick` (`6522via.cpp:536-543`) `TIMER1_VALUE + IFR_DELAY`.
**IFR_DELAY is the one-off underflow→IFR latency, not part of the recurring
period.** Folding it into the reload stretched every interval by one cycle.

One cycle per frame is inaudible in the Mockingboard's usual job (a music
tick), which is why this survived so long. It is fatal when T1 is armed as a
*frame clock*, because the error accumulates: French Touch's **MAD EFFECT**
arms T1 with one PAL frame and beam-races a 192-line picture off each
interrupt, so its drawing loop slid a cycle per frame until whole scanlines
fell past line 191, got stamped scanline 192 by `pushVideoEventLocked`, and
were dropped by the renderer. Measured on the real disk: the loop's per-frame
phase went from drifting to stable, and recovered page-flip events per frame
rose from 169 to 188 of 192.

The demo states the contract while computing its own latch (`Sources/main.a`,
GPLv3, archived in `disks_5.4/demo/madef/`):

```
; PAL delay = 65*(192+70+50) = 20280
; -2 (6522 takes 2 cycles to generate INT)
; = 20278 = $4F36
```

period == latch + 2. The first shot keeps N+3 (the IFR latency genuinely
applies once); only the reload changed.

`via_t2_timing` had pinned the *wrong* value — it asserted N+3 spacing for
the continuous reload, citing MAME. That assertion was MAME parity, not
hardware parity, and is now corrected in place with the reasoning; the new
`via_t1_continuous_period` covers eight consecutive reloads, since it is the
reload and not the first shot that accumulates. Consistent with the rest of
today's findings: MAME keeps N+3 and renders the demo wrong, AppleWin only
started running it at 1.29.6.0 after fixing this class of timing bug.

Diagnostic harness: `tests/madef_phase_probe.cpp` (built, not in ctest — it
needs the demo disk) boots the real image and reports per-frame drift,
per-line period, and where each page-flip lands horizontally.

**Still open**: the picture is much closer but not right — some scanlines
still open at column 0 because their switch lands where
`frameCycleToPos` clamps (`byteCol = clamp(hpos - 25, 0, 40)`). A sweep of
all 65 candidate phases finds none that keeps every switch inside the
40-column window (best is 28, still 55 of 380 outside), so this is **not** a
constant offset to tune — the line attribution itself is still wrong for a
subset of events.

Deriving the line origin from `main.a` was attempted and **did not settle
it**, which is worth recording so it is not retried blind. The source pins a
*relation* — 13 cycles between the `$C019` edge and the demo's "cycle 0" —
but not where that cycle 0 sits in POM2's beam coordinates; one equation,
two unknowns. The alternative reading (cycle 0 = first visible byte ⇒ edge
at hpos 12 of line 0, 25 cycles from the hpos-52 placement) was implemented
and measured: MAD EFFECT's page-flips moved from column 0 to column **1**,
not the predicted column 13, and `pal_timing` + `vbl_smoke` both failed
because they require line 192 to read VBL from its first cycle. Reverted.
The hpos-52 placement is what the demo states literally *and* what those two
pinned tests corroborate. Settling the residual needs an independent anchor
— a column-accurate reference capture, or a hardware statement of VBLBAR's
position relative to the start of active video — not more reasoning.

## 2026-07-31 — switch→column mapping is `hpos - 24`: a switch is one cycle too late for its own byte

`Apple2Display::frameCycleToPos` mapped a soft switch to a screen column with
`byteCol = clamp(hpos - 25, 0, 40)` — the raw offset of the visible window,
which opens at hpos 25 after the 25-cycle HBL. But a switch performed *at*
hpos 25+c cannot affect column c: the video scanner latches that byte during
phi1 of the very cycle whose phi2 the CPU is using for its access, so the
change first shows one column later. The effective mapping is `hpos - 24`.

Established by measurement, not by the argument above. Replaying French
Touch's **MAD EFFECT** (GPLv3 sources archived in `disks_5.4/demo/madef/`)
and sweeping all 65 candidate phases, the demo's 192 per-scanline lit-run
starts — the `$C055` whose column *is* the silhouette it draws — land wholly
inside the 40-column window only for offsets **21..24**. 25 sat one cycle
outside, which is exactly why the scanlines whose start falls at the far left
of the silhouette spilled into HBL and clamped to column 0 while every other
line drew correctly.

Method note that cost two wrong turns: sweeping *all* switches has no
solution at any phase. The `$C054` that CLOSES the lit run is legitimately
thrown in HBL — "a switch in blanking governs the whole upcoming line" is the
standard idiom. Only the opening switch must be inside the window.

**This moves the beam-racing convention**, so five pinned tests were
re-baselined: `horizontal_split`, `horizontal_split_composite`,
`horizontal_split_560`, `dix_modpage_split` and one line of `pal_timing`.
None of them measured anything — every one drives a synthetic switch at
hpos 45 and asserted the resulting column, i.e. they restated POM2's own
choice. The *expected column* was moved (20 → 21) rather than the stimulus
(45 → 44), so the change stays visible in the tests instead of hiding in a
one-character edit. `horizontal_split_smoke` now spells the rule out:
hpos 24 → col 0, hpos 25 → col 1, hpos 45 → col 21, and hpos 64 → the
end-of-window clamp (the last cycle of a line can no longer affect the last
byte, which is already latched).

Residual: the measured band was 21..24 and 24 is its edge — the value with a
mechanism behind it, but 21-23 are not excluded by the data. If real software
ever contradicts `hpos - 24`, this is the commit to reopen.

## 2026-07-31 — `$C019` intra-line phase: proposed, implemented, measured, rejected

Recorded because the reasoning is seductive and someone will try it again.

French Touch's **MAD EFFECT** syncs its whole frame off the `$C019`
VBL'→DISPLAY edge, and its cycle-annotated `Sources/main.a` says:

```
; WARNING: DISPLAY detected (VERTBLANK <0) from cycle #52 of last line (#311) of VBL
...                                        ; line 311 / cycle 54
NOP : NOP : NOP : NOP  : LDA $EA           ; +11
                                           ; = 65
; line 0 (display) / cycle 0
```

Read naively this says the edge is 13 cycles before the line boundary, and
POM2 derived the flag from the scanline number alone (`scanline = now / 65`),
i.e. with **no** horizontal phase — apparently a bug. It is not: the sentence
pins a *relation* (13 cycles from the edge to the demo's "cycle 0"), not a
*position*, because nothing says where that cycle 0 sits in POM2's beam
coordinates. One equation, two unknowns.

Both anchorings were implemented and falsified against the real disk, using
the count of MAD EFFECT's 192 per-scanline lit-run starts (`$C055`) that fall
outside the 40-column visible window:

| `$C019` lead | clean-phase band | distance from `frameCycleToPos`'s 25 |
| --- | --- | --- |
| +13 (edge at hpos 52 of the previous line) | 9–12 | 13–16 cycles |
| +28 | ~58 | worse |
| **0 (no shift — what POM2 already did)** | **21–24** | **1–4 cycles** |

So the un-phased implementation was already within 1–4 cycles and every
proposed shift moved *away* from the answer. A third variant (edge at hpos 12
of line 0, from reading the demo's "cycle 0" as the first visible byte) also
broke `pal_timing` and `vbl_smoke`, which independently require line 192 to
read VBL from its very first cycle. All reverted; `vbl_edge_phase` now pins
the un-phased edge together with this history, and the //c VBLINT latch path
carries a matching note.

Method note, since it cost two wrong turns: the first phase sweep demanded
that *every* switch land inside the visible window and therefore had no
solution at all. The `$C054` that CLOSES the lit run is legitimately thrown
in HBL — the standard "a switch in blanking governs the whole upcoming line"
idiom. Only the `$C055` that OPENS it has to be inside the window, because
its column *is* the silhouette. Sweeping those alone is what produced the
table above.

**Resolved the same day** — see the `hpos - 24` entry above: the residual was
the cycle→column mapping, not this edge.

## 2026-07-31 — 8 KB international //e video ROM (342-0274-A)

`Memory::loadCharRom` used to reject anything that was not 2 KB or 4 KB, on the
stated grounds that "no shipped char ROM is 8K". The genuine **342-0274-A** is
8 KB, and it is the part fitted to the French //e — MAME's //e character
generator region (`gfx1`) is 8 KB = **two 4 KB banks**, and the machine's
charset switch picks one. The US `apple2ee` fills both banks with the same 4 KB
part (`342-0265-a.chr` at offset 0 *and* 0x1000); `apple2eefr` instead ships one
8 KB part carrying two different sets.

POM2 now collapses an 8 KB dump to a selected bank and runs its ordinary 4 KB
normalization on it — one normalization routine, not two. The bank layout was
established by CRC rather than assumed: **bank 0 == `apple2e_char_frca.rom`
(2c8fc403), bank 1 == `apple2e_char.rom` (2651014d)**, both of which POM2
already ships standalone, so the two halves are independently checkable.

The picker gains two entries (`iie_fr8k_fr` / `iie_fr8k_us`) rather than
modelling the hardware charset switch, which is not emulated. `charRomBank()`
carries the bank to every `loadCharRom` call site — without that plumbing both
entries would silently load bank 0 and draw identical glyphs.

**Catalogue correction found on the way**: the existing "//e/c — Français
(342-0274-A)" entry points at a 4 KB file that is byte-identical to
`apple2e_char_frca_unenh.rom` (both ab0be706), so it was never 342-0274-A. The
label is now honest and the real part is offered alongside it.

Pinned by `char_rom_8k_bank` (5 checks: both banks match their standalone 4 KB
dumps, the banks differ so the argument is load-bearing, out-of-range clamps,
and a 4 KB dump ignores the bank). It soft-skips without the user-provided ROMs.
`char_rom_test` pinned the *old* "8 K is rejected" contract and was updated
deliberately, not deleted.

Also wired: **`3420033a.256`** (MAME `apple2c0`, the "//c UniDisk 3.5" ROM
revision) appended **last** in the //c probe order — a fallback for users who
own only that dump, not an upgrade. It does not unlock hardware-accurate 3.5
boot on //c: POM2 still serves 3.5"/HDV there through the host-side SmartPort at
built-in slot 5, because the IWM bit-shift path is deliberately unmodelled.
**`342-0326-a.f12`** (French keyboard decode ROM) is catalogued as oracle-only —
POM2 maps host keys directly and has no keyboard-decode ROM. `a2c.128` is
byte-identical to the existing `apple2c-16K.rom` and needed nothing.

## 2026-07-31 — Bug hunt 6: stale screen on the Le Chat Mauve Eve registers

**A regression in the same day's frame skip, found by hunting it rather than
trusting it.** `TextFrameKey` keyed on `Memory::DisplayState`, and that is not
the whole picture: a **Le Chat Mauve "Eve"** has its own $C0B8-$C0BB registers
which select the colour-TEXT renderer (and with it the 560-wide `frame80`).
They are guest writes — `STA $C0B9` — but they reach the card through
`SlotBus::broadcastVideoSwitch` and, unlike $C05E/$C05F, push **no video
event**. So the frame after such a write has an empty event log *and* an
unchanged `DisplayState`: every term of the key agreed, the skip fired, and the
screen kept a stale picture at the wrong geometry (280-wide mono served where
560-wide colour text was due). The card is the **//c PAL profile's built-in
slot 7** — the French Touch / DIX target hardware — so this was not a corner
case. The key now carries the card's identity plus its mode + both Eve toggles.

Two process notes worth keeping:

- The original mutation sweep could not have caught this. It toggled the
  **host-side** `hiResMode` but never the card's **own guest-facing** switches,
  so it proved the key handled everything it already knew about — the classic
  shape of a test that confirms its author's model instead of the behaviour.
- The new section 9 **passed on the first attempt, vacuously**: the card was
  handed to the display via `setChatMauveCard()` but never PLUGGED into the
  `SlotBus`, so `broadcastVideoSwitch` reached nobody and the guest writes went
  nowhere. Plugging it made the failure appear immediately. That is now recorded
  in the test header, because it is the second time in this file that a
  side-by-side harness passed while testing nothing (the first was the shared
  `Memory` draining `takeVideoEvents()`).

All six key terms are now mutation-proven load-bearing: flash phase, video RAM,
DisplayState, colour mode, Chat Mauve state, beam-raced-frame exclusion.

## 2026-07-31 — Static-text frame skip: −84 % on the display

The remaining big win from the 2026-07-30 profile: the display re-decoded all
960 character cells every frame even when the screen had not changed a byte —
`glyphRows7` alone was 56 % of display cost, ~887 host instructions per cell.
`Apple2Display::render` now compares the frame against a **`TextFrameKey`** and
returns without painting when nothing that could affect the pixels has moved.

Measured on booted DOS 3.3 (3000 frames, render phase only, II+):
**93.4 → 15.0 µs/frame, −84 %**, with a byte-identical pixel checksum. The
worst case — text churning every frame, so the key can never match and its
copy+memcmp is pure overhead — is 94.8 → 94.7 µs, i.e. free: 16 KB of memcmp is
nothing against ~850 K instructions of glyph decoding.

**The skip is deliberately narrow, and every exclusion has a reason:**

- **Beam racing.** A frame carrying video events is painted as several bands
  with *different* DisplayStates (and, on the 560-wide path, a column-bounded
  save/restore), so it corresponds to no single whole-frame state. `render()`
  only consults the key on the `events.empty()` branch; the beam-raced branch
  invalidates it. This is what the DIX / French Touch demos depend on.
- **Persistence.** The graphics painters implement a phosphor rule
  (`max(target, prev × decay)`), so their output legitimately changes every
  frame from identical inputs. Only FULL-SCREEN TEXT is skipped —
  `renderText`/`renderText80` write no persistence at all, which makes their
  output a pure function of the key.
- **CPU demod.** AppleWin / OE-CPU overwrite `frame80` from the composite
  signal, so the key would describe pixels that are no longer on screen; that
  branch invalidates too.

**PAL was checked, not assumed.** FLASH is
`frameCounter / kFlashHalfPeriodFrames & 1`, and `frameCounter` is the
*emulated* frame index — `cycleCounter / (65 × scanlinesPerFrame)` — so PAL's
312-line/50 Hz frame and NTSC's 262-line/60 Hz frame each advance it at their
own rate and the key follows automatically. A skip keyed on a host frame
counter would have drifted on PAL only.

The key stores video RAM **by value** — `$0400-$0BFF` from main *and* aux, the
union of text/lo-res pages 1 and 2 — rather than resolving which page is live.
Over-covering costs a bigger memcmp; under-covering would freeze a stale screen.
The character ROM goes in by value too, not by pointer: reloading a different
character set can reuse the same heap block, and a pointer+size compare would
then report "unchanged" across an actual glyph change.

New `display_dirty_skip` test (151/151 green). It runs **two machines in
lockstep** — one display allowed to skip, one forced to repaint via the new
`invalidateTextFrameCache()` — and requires bit-identical framebuffers over a
113-frame script, under **both** video standards. Two separate `Memory`
instances, not one shared: `takeVideoEvents()` drains, so a second display on
the same Memory would see an empty log and never take the beam-racing path —
the test would have passed while testing nothing. (It did, until that was
found; the published events confirm the split now lands at NTSC line 87/174 and
PAL line 104/192 — different positions, same script.)

Mutation-tested: deleting the flash-phase, video-RAM, DisplayState,
colour-mode or beam-race-exclusion terms each makes the test fail. Two terms
survive deletion and are therefore **defensive, not load-bearing**: the
`mixedMode` exclusion (`renderInternalBand`'s `if (state.textMode)`
short-circuits before any mixed handling, in both the 40- and 80-column paths,
so MIXED cannot alter a full-text frame) and the `iie` flag (only changes
across a profile switch, which rebuilds the display). Catching the colour-mode
term required installing a **Le Chat Mauve** card: its colour-TEXT path is the
only text renderer whose pixels depend on the host-side colour mode — every
other mode draws text hard-coded white-on-black.

## 2026-07-30 — Profiling: −17 % on the emulation core

A Callgrind pass over a deterministic `tickFrame()` driver (600 frames of
steady-state DOS, **3 969 356 emulated instructions**) put ~70 % of POM2's work
in the emulation core and ~30 % in the display, and showed the core spending
about as much on the per-instruction device fan-out as on the 6502 itself. Two
fixes came out of it, both measured:

**The VBL check did a runtime-divisor modulo on every emulated instruction.**
`Memory::advanceCycles` derived the scanline with
`(cycleCounter / 65) % scanlinesPerFrame`. The `/ 65` is free — a compile-time
constant the compiler strength-reduces to a multiply-shift — but the `%` has a
*runtime* divisor and stays a real hardware division, executed ~4 M times per
10 emulated seconds. The frame origin is now tracked incrementally, and the
division only runs to resynchronise.

Worth recording because it is a trap: Callgrind rated that line at 1.7 % of the
core, yet removing it was worth **15 %** of wall-clock. Callgrind counts
*instructions*, and a `div` is one instruction of 20-40 cycles — so on
division-heavy code its ranking badly understates the real cost. The full
before/after confirms it: instructions fell 4.6 % while time fell 17.5 %.

The incremental base has to survive `setCycleCounter()`, which snapshot restore
and rewind use to move the counter **arbitrarily, backwards included**. The
subtraction is deliberately unsigned so a backwards jump wraps to a huge value,
misses the one-frame-rollover test and lands in the resync branch — self-healing
in a single division. (A first draft reset the base to 0 and walked forward with
a `while` loop instead; that would have spun for millions of iterations on any
rewind with a large cycle counter. It passed the whole suite regardless, which
is a fair warning about what the suite does not cover.)

**The cassette burned 4.1 % of the core with no tape loaded.** `advancePlayback`
returns immediately when idle, so that was pure call overhead — ~17 instructions
per emulated instruction to decide there was nothing to do, ~4 M times.
`advanceCycles` is now inline in the header and gates the out-of-line work on
the deck actually moving.

`currentCycle` still advances unconditionally, and that is not incidental: it is
the RECORDING timebase (`toggleOutput` measures pulse widths against it), so
gating the whole call — which the first draft did — would silently corrupt the
durations of a tape recorded from a deck that had nothing loaded, i.e. the normal
way to record one. The suite passed that draft too.

Net: **−17.5 % on the core**, −13.8 % including display, 150/150 green.

**Third fix: `SlotBus` now keeps a compact list of the plugged cards.** The
per-instruction fan-out walked all eight `unique_ptr` slots to find the one or
two that exist — 65 host instructions per emulated instruction with a single
Disk II, 15.7 % of the core, plus another 1.9 % attributed separately to
`unique_ptr.h`. `SlotBus::advanceCycles` fell **258 M → 127 M instructions
(−51 %)**.

The cache holds RAW, NON-OWNING pointers, so it dangles the moment a card is
destroyed. That is safe here for three checkable reasons, and they are the whole
argument: `slots` is private with no accessor that can reseat a slot from
outside; there are exactly three mutation points (`plug`, `unplug`, `clear`) and
each rebuilds; and mutations run under `stateMutex` — the same lock the CPU
worker holds around `runCpuSlice` — with `applyProfile` additionally stopping
the worker first. `unplug()` rebuilds *before* returning, since the `return
std::move(slots[slot])` is what empties the slot. A debug-only assertion in
`advanceCycles` cross-checks the cache against `slots` every call, so a future
fourth mutation point fails loudly instead of silently skipping a card or
following a freed pointer; the whole suite was run in a Debug build with it live.

Cumulative across the three fixes: **−25.6 % on the emulation core**, −19.8 %
including display (0.531 s → 0.395 s for 2000 frames), 1 645 M → 1 407 M
instructions. POM2 goes from ~47x to ~59x realtime.

Still on the table: the display re-decodes the whole text screen every frame
even when nothing changed — `glyphRows7` alone is 56 % of display cost, ~887
instructions per character cell. Dirty-region tracking is the remaining big win,
but it touches the beam-racing path the DIX demos depend on, so it is not free.
*(Done 2026-07-31 — see the entry above.)*

## 2026-07-30 — v0.8: GLES tier, four-platform release CI, portability fixes

**`POM2_GLES` — the OpenGL ES 3.0 tier is now a build option, not a browser
accident.** POM2 already contained a complete GLES path (`GLFW_OPENGL_ES_API`,
`#version 300 es`, direct entry points) — it was simply gated on
`__EMSCRIPTEN__` across seven translation units. That conflated two different
questions, *"do we speak GLES?"* and *"are we in a browser?"*, and the
conflation is precisely what made the Raspberry Pi unreachable: a Pi needs the
GLES tier while being an ordinary native Linux build, so every guard took the
desktop branch and the result asked for a GL 3.2 core context, which Mesa's V3D
cannot give (it caps *desktop* GL at 3.1). `src/Pom2Build.h` now owns the
distinction via `POM2_GL_ES`, set by Emscripten **or** `-DPOM2_GLES=ON`.

One detail worth keeping: native GLES must go through **EGL**
(`GLFW_CONTEXT_CREATION_API = GLFW_EGL_CONTEXT_API`). GLX only hands out a GLES
context when the X server advertises `GLX_EXT_create_context_es2_profile`, which
V3D does not — without the hint the context request fails on exactly the
hardware the tier exists for. libEGL itself is *dlopened by GLFW*, not linked by
us (`readelf -d` shows libGLESv2 only); CMake locates it purely as a
presence check so a missing package fails at configure time with a package name
rather than at runtime with "context creation failed".

**Release CI on four native runners**, modelled on POM1: Linux x86_64 (pinned
bionic container, glibc floor 2.27), Raspberry Pi arm64 (bookworm, GLES, floor
2.36), macOS Universal 2 `.dmg`, Windows self-contained `.zip`, plus a publish
job attaching everything with `SHA256SUMS.txt`. POM2 **reuses POM1's**
`pom1-bionic-builder` image rather than duplicating a near-identical one — the
requirements are the same, and one image beats two to keep in sync.

**Five real portability bugs, all found by the first CI runs** — none of them
reachable on the dev machine:
- `MemoryProfile.h` used `size_t` with no `<cstddef>`. libstdc++ 14 leaks it via
  other headers; Debian bookworm's 12 does not, so the arm64 build failed.
- `AudioDevice.cpp` used `std::fabs` with no `<cmath>` — same class, MSVC.
- The macOS deployment target was set to 10.13, but POM2 uses `std::filesystem`
  throughout and libc++ marks those symbols unavailable before **10.15**.
- Seven TUs each hand-rolled the GL include block and had drifted apart. On
  Windows that block had never been exercised, and it was wrong twice over: the
  SDK's `<GL/gl.h>` is not self-contained (needs `<windows.h>` first) and is
  frozen at GL **1.1**, so `GL_CLAMP_TO_EDGE` (GL 1.2) did not exist.
  `src/Pom2GL.h` now does it once, and `opengl-registry` supplies `GL/glext.h`.
- The aarch64 AppImage came out **ET_DYN**, which AppImageLauncher rejects as
  "type -1". AppImageKit never rebuilt the old-style runtime for ARM:
  `continuous/runtime-aarch64` is ET_DYN while `12/runtime-aarch64` is ET_EXEC,
  so the Pi job pins release 12 and passes it via `--runtime-file`.

**Windows ships without host networking for now** (`POM2_HAS_SOCKETS`, new in
`Pom2Build.h`). POM2's three networking TUs are written against POSIX sockets;
Windows has Winsock2, which is a different API, not a `#define`. Rather than
guess, Windows takes the road Emscripten already takes: the SSC opens no telnet
listener and the Uthernet I/II cards plug, reset and answer their registers but
see no traffic. Everything else — CPU, video, audio, disks, printer — is
complete. Guard host-socket code with `POM2_HAS_SOCKETS`, never
`#ifndef __EMSCRIPTEN__`: that assumption ("not a browser, therefore POSIX") is
what broke the Windows build in the first place.

## 2026-07-30 — Bug hunt 5: UI panels fuzzed headlessly; no defects found

The 16 `src/*_ImGui.cpp` files were the largest completely untested surface left
— nothing in `tests/` instantiates an ImGui context. They turn out to be
reachable headlessly: Dear ImGui itself is platform-agnostic (only the backends
need a window), so a context with a built font atlas, a display size and
NewFrame/Render executes every layout, string-formatting and clipping path with
no GPU, no GLFW and no live machine. The panels' own design does the rest —
each takes a plain `Snapshot` struct and returns a plain `Result`, so they can
be driven entirely from synthetic state.

Six panels (Uthernet, Joystick, FloppyEmu, SmartPort, Le Chat Mauve, Toolbar)
driven for **~21 500 frames** under ASan+UBSan with adversarial snapshots —
empty/oversized strings, ImGui markup in labels (`##`, `###`), format-specifier
strings (`%s`, `%d`), NaN/Inf floats, negative and out-of-range indices, empty
and large vectors — at hostile display sizes including 1×1, 4096×2160 and
degenerate 800×1, with the mouse driven over the whole area and buttons/wheel
firing. **No crashes, no undefined behaviour, no ImGui assertion failures** (the
latter would catch unmatched Begin/End or PushID/PopID).

Inspection agreed: cursors are clamped before use (`FloppyEmu` re-homes and
clamps `browseCursor_`/`settingsCursor_` every frame, and loops are bounded by
the vector sizes rather than by the incoming index), lookups are by value with
a null check rather than by raw index (`JoystickPanel::findHost`), fixed-size
`std::array` members are indexed only by constants that match their declared
extents (`kAxes = 2`, `kButtons = 3`), and the `if (!Begin(...)) { End(); }`
pattern is used correctly.

The harness is not committed — it links ImGui into a test target, which is a
build/CI decision rather than a bug fix. It is written and working if that
coverage is wanted.

## 2026-07-30 — Bug hunt 4: persisted floats did not round-trip

**Every float setting shifted on the first save/load cycle.** `Settings::
setFloat` serialised with a bare `os << v`, and `ostringstream` defaults to
**6 significant digits** — not enough to round-trip a float. `1.0f/3.0f` wrote
as `"0.333333"` and read back as a different float. That covers all five
volumes (master / speaker / cassette / both floppy), `ui_scale`, and the ~15
NTSC/CRT and voxel shader parameters: what the user dialled in was not what
they got back. The drift is one-shot rather than cumulative (the reloaded value
re-serialises to the same text), and each shift is ~1e-7, so nothing was
visibly broken — but a persistence layer that doesn't round-trip is wrong, and
it is the same class as the SSC baud-rate bug from hunt 1.

`setFloat` now emits the **shortest** width that round-trips, capped at
`max_digits10` (9). Shortest rather than always-9 on purpose: state.cfg's own
header invites hand-editing, and `0.5` stays `0.5` instead of becoming
`0.500000000` — only genuinely awkward values widen (`0.33333334`).

`settings_roundtrip_test` already existed but only exercised STRING values
(plus two typed values spot-checked as raw strings), which is exactly why this
survived. Extended to cover the typed accessors properly — int/float/bool — and
also boundary whitespace, which `escapeValue` handles specially (load() trims
each line to drop CRLF artifacts, so a value with a leading/trailing space
survives only because of that encoding) but which nothing tested.

**Sweeps that came back clean**, recorded so they aren't redone blind:
- **Write-back identity across the whole writable library** (179 images): load,
  mark every track dirty *without altering a nibble*, `saveDirty()` — the file
  must be byte-identical. It is, for every image. This is the data-loss class:
  if the decode-back-to-source-format were not an exact inverse of the load-time
  encode, merely touching a disk would silently rewrite it with drifted content
  (the shape of the MacBinary bug already recorded in `detectFormat`). Note
  `writeNibbleAt` only dirties on a real change, so the harness has to flip a
  nibble and flip it back; and real WOZ dumps are physically write-protected, so
  WOZ write-back is not covered by this.
- **Mouse card cross-implementation differential** — nothing previously compared
  POM2's two Apple Mouse Card implementations against each other, although both
  claim the same ProDOS firmware contract and each has its own smoke tests. Ran
  identical host-motion scripts through both on full machines, reading the same
  screen holes. Status/button bytes agree exactly; position agrees within a **±2
  quadrature phase residue** that does not converge with more idle time and
  flips sign with the sampling phase. That is inherent to modelling quadrature
  at all (the MCU path decodes real encoder edges; the HLE copies the host delta
  and so has no residue), **not** a defect in either — recorded here so the next
  person to run that comparison doesn't mistake 59 "divergences" for a bug.

## 2026-07-30 — Bug hunt 3: no defects found; six sweeps recorded

A third pass over areas the first two didn't touch. **No bugs.** Recording what
was covered and how, so it isn't redone blind — and one characterised follow-up.

- **//e MMU bank routing, full cross-product**: all six memory-affecting soft
  switches (80STORE, RAMRD, RAMWRT, ALTZP, PAGE2, HIRES) crossed against every
  region boundary, reads and writes — 2 048 checks, clean. The oracle was written
  from the IIe Tech Ref / Sather independently of the implementation, so it
  genuinely tests the properties `iie_aux_paging_conformance_test`'s spot checks
  cannot see: ALTZP governing $0000-$01FF *alone* (RAMRD/RAMWRT must not reach it,
  and ALTZP must not reach $0200+), the 80STORE window not leaking outside
  $0400-$07FF / $2000-$3FFF, HIRES gating only the $2000 window, and PAGE2 alone
  never affecting routing.
- **$C000-$C00F access-type matrix**: those eight switch pairs are write-only on
  the //e. All sixteen addresses verified from both polarities of every switch
  (the existing regression check covers two), plus the $C013-$C018 status reads
  reporting correctly and not self-toggling. 88 checks, clean.
- **Snapshot REPLAY determinism** — the strongest property tried so far, and
  strictly stronger than a capture→restore→recapture blob compare: run to T,
  snapshot, run M more → state A; restore, run M again → state B; A must equal B.
  State absent from the snapshot but still steering execution shows up as a
  RAM/CPU difference, and RAM/CPU *are* in the blob. 60 configurations (5 disk
  images × 2 ROMs × 6 snapshot points, including 5 frames in — mid-boot, LSS
  mid-track) plus //e enhanced/unenhanced and the 1977 ROM: bit-identical every
  time, on the same machine, on a fresh machine, and against two cold boots.
  Caveat worth knowing: this cannot catch host-side-only state (it would NOT have
  found the SSC baud bug, whose effect never re-enters machine state).
- **ProDOS volume decode fuzz**: `decodeVolumeToFolder` writes host files from a
  guest-writable image, so its jail is the thing that matters. 4 000 mutations
  with path-traversal names planted directly at the directory-entry name field
  (`..`, `A/B`, `/etc`, control bytes, dot-only, over-length), self-referencing
  subdir entries, smashed key pointers/EOF fields — checked structurally (scan for
  any entry created outside the target folder + an untouched canary), not by
  trusting the name filter. No escapes, no sanitizer reports.
- **Whole real disk library**: all **4 771** images in the repo through the loader
  and every read path under ASan+UBSan. Zero crashes. 50 refusals, all correct and
  all the same 7 files (duplicated across leftover `.claude/worktrees/` copies):
  800K/hard-disk-sized images handed to the 5.25" loader, which correctly says they
  belong on the HDV card.
- **3.5" / HDV / ATA loaders** (`Disk35Image`, `Block512Backing`) — the untrusted
  surface round 2's DiskImage fuzz skipped: 127 real images + 3 000 mutated cases,
  block and byte accessors probed past the end, truncated `loadFromBytes`. Clean.

**Characterised but deliberately NOT implemented: the Z80 Q register.** The
SCF/CCF gap that keeps the Harte Z80 sweep off a clean unmasked 100 % is now
fully pinned down — exact rule, including why the DD/FD-prefixed forms differ,
validated 1000/1000 on each of the six affected opcode files. Written up in
`DEV.md § Z80 core`. Not done because closing it means maintaining `q` in
**every** instruction epilogue for undocumented flag bits no CP/M or Apple II
software reads; that is a scope decision, not a bug fix. Also recorded there:
MAME's own SCF/CCF expression does not transcribe literally (its `Q` is derived,
not a raw mask — 548/1000 at face value), the same lazy-field trap as `pv_val`.

## 2026-07-30 — Bug hunt 2: Z80 block-I/O repeat flags; sanitizer + fuzz sweeps

**INIR/OTIR/INDR/OTDR set the wrong flags on every repeating iteration.** The
repeating block-I/O opcodes do not just leave the per-iteration INI/OUTI flag
formula in place: while B ≠ 0 the Z80 re-derives X/Y from the rewound PC's high
byte (the same rule LDIR/CPIR already used here), H from B's low nibble when
carry is set, and P/V from the parity of B±1 (or B) xored against the incoming
P/V — plus `WZ = PC+1`. MAME has this as `block_io_interrupted_flags()`
(`z80.cpp:580-604`); POM2 applied the non-repeating formula to all eight
opcodes. The four non-repeating forms (`ed a2/a3/aa/ab`) were already exact.

**Why it survived two exhaustive exercisers:** zexdoc and zexall run under CP/M
and never execute an I/O block instruction, so they are structurally blind to
this — both stayed 100 % green while all four repeating opcodes were wrong on
~99.5 % of vectors. It took a different oracle to see it:
[SingleStepTests/z80](https://github.com/SingleStepTests/z80) publishes the
same per-opcode JSON as the 6502 corpus that found the decimal-SBC bug earlier
the same day, and `Z80::State` already exposes everything it pins (WZ, I, R,
IM, IFF1/2). A full local sweep — **1 092 opcode files, 1 092 000 vectors**,
base + CB + ED + DD + FD + DD CB + FD CB — is now **100 %** apart from SCF/CCF's
F bits 3+5, which need the Q register (already an owned out-of-scope item).

One trap worth recording: MAME's `pv_val` is a **lazy** field whose getter
re-parities the stored byte, so the flag that actually reaches `get_f()` is the
*inverse* of the `(pv_old ^ pv())` MAME stores — P/V lands SET when the two
agree. Transcribing MAME's line literally gets it exactly backwards; the rule
was confirmed against all 3 990 repeat-branch vectors before touching the core.
Pinned by `z80_block_io_flags` (16 vectors inline, spanning carry set/clear ×
data bit 7 set/clear × both H nibble edges — no 1.4 GB download).

**Unsynchronised cycle-counter read in `pom2_headless`.** Its main loop sampled
`Memory::getCycleCounter()` bare while the CPU worker wrote it under
`stateMutex` — a real data race (ThreadSanitizer on the running binary), benign
on x86-64 but UB, and the wrong example to set next to `MainWindow::
renderStatusBar` and `AiControlServer`, which both take the lock for the same
read. `pasteText` on the next line was already safe (Memory's own `kbMutex`).

**Sweeps that came back clean** — recorded so they don't get redone blind:
- **ASan + UBSan over the whole 149-test suite**: zero memory-safety and zero
  undefined-behaviour reports. (Use an in-tree build: 3 tests exceed their tight
  `TIMEOUT` under instrumentation and `disk_skew_sniff` needs cwd = repo root.)
- **TSan on the shipping cross-thread surface**: 113 k concurrent HTTP requests
  across every state-touching AI-control endpoint against a *running* CPU worker,
  with rewind scrub/park interleaved — no races. Worth knowing that
  `ai_control_server_smoke_test` deliberately parks the controller in `Stopped`,
  so that configuration otherwise has no coverage at all. TSan needs
  `setarch -R` on this kernel (high-entropy ASLR → "unexpected memory mapping").
- **7 329 mutated disk images** (truncations, header-field smashing, magic
  swaps, forged MacBinary wrappers, seeded from real WOZ1/WOZ2/2MG/NIB/D13/PO/
  DO/DSK) through `loadFile` + every nibble/bit-cell/flux read path + the media
  snapshot round trip, under ASan+UBSan: no crashes.
- **~30 k mutated machine snapshots** (section-length smashing, tag rewriting,
  chunk splicing, truncation) through `restoreMachineState` with real cards
  plugged, under ASan+UBSan: no crashes.

## 2026-07-30 — Bug hunt: CMOS decimal SBC, CPU-oracle harness, SSC baud restore

**The 65C02 decimal SBC was using the NMOS correction rule.** The WDC 65C02
lets the low nibble's `-6` decimal adjustment **borrow into the high nibble**;
MAME names it outright in `w65c02.cpp:28-46` (`do_sbc_cd`): *"SBC allows
interdigit carry from decimal adjustment on 65C02"*. It packs both nibble
differences and only then applies `-6`/`-$60` to the whole byte, so the borrow
propagates. The NMOS part corrects each nibble in isolation and it never does —
meaning **the two CPU parts genuinely return different accumulators for the same
operands**, and a single shared code path cannot be right for both. `M6502::SBC`
now branches on `cpuMode`.

Measured against a full 256-opcode Tom Harte sweep: every decimal SBC
addressing mode was wrong ~3.4% of the time — `e1,e5,e9,ed,f1,f2,f5,f9,fd`,
*nine* opcodes, where the previous note in `M6502.cpp` had claimed only `e9`
and had written the divergence off as an unmodellable silicon quirk. It is
modellable; MAME models it, and MAME is this project's source of truth. Result
values moved by exactly $10 (the dropped borrow) and the N flag with them.
Divergence is confined to **invalid** BCD digits, so no correct-software
behaviour changes — but "officially undefined" is not the same as "free to get
wrong", and it was masking a real parity gap. WDC CMOS is now 100% on 255 of
256 opcodes (2 530 000 vectors). Pinned by `decimal_sbc_cmos`, which embeds the
corpus vectors inline so it runs without the 1.4 GB download.

**The exhaustive CPU oracle could not actually be run.** `tomharte_cpu_test`'s
`runVector` restored PC/SP/A/X/Y/P and RAM per vector but never re-armed the
CPU's KIL/JAM + STP `halted` latch. `step()` short-circuits before the opcode
fetch while that latch is set and only a reset clears it, so the **first**
vector landing on an NMOS JAM ($02/$12/$22/…) or a CMOS STP ($DB) froze the
shared CPU for every remaining vector in the run: a full NMOS sweep scored
20 000/2 560 000, because file `02` poisoned the other 254 files. The curated
CTest subset never caught it — no JAM opcode is in the manifest. This is why
the "100%" claims in `DEV.md` had only ever been checked on 41 opcodes; with the
latch cleared, the real numbers are in `DEV.md § Tom Harte`, including the fact
that the failing NMOS files are **exactly** the 78 undocumented opcodes with
observable side effects (a much stronger statement than the subset could make).

Two related traps recorded rather than "fixed", since both are deliberate:
- **`$5C` fails the CMOS sweep on purpose.** POM2 charges 3 bytes / 8 cycles,
  matching MAME (`ow65c02.lst` `nop_c_aba`) and the standard 65C02 unused-opcode
  tables. All three of Harte's 65C02 variants say 4. That corpus is generated
  from an implementation conforming to documentation, not from silicon, so MAME
  wins — but `5c : 0/10000` is now documented as expected, not a regression.
- **`tomharte_6502`/`tomharte_65c02` report `Passed` in 0.00 s with no corpus
  on disk** (soft-skip, so networkless CI stays green). A green tick from those
  two names does not mean the CPU was validated. Called out in `DEV.md`.

**Super Serial Card restored its baud rate but not its baud pacing.**
`bytesPerSecond_` is derived from the control register's divider and is not
serialized — the same reason `wordLength_`/`extraStop_` are restored
explicitly. Only `applyControlReg` ever computed it, and a snapshot load is not
a register write, so a restore left the rate the **live** session was last
programmed to: a 300-baud snapshot loaded into a 19 200-baud session drained
the TX ring 64× too fast, and the reverse stalled it. Rewind hit this on every
frame. `loadSnapshotState` now recomputes it and resets the pacing budget +
drain clock, exactly as `applyControlReg` does — a stale `lastDrainTime_` would
otherwise credit the restored rate for all the wall-clock time before the load
and dump a burst. Pinned in `card_snapshot_state` (both directions, so the fix
can't be a hard-coded slow default).

## 2026-07-30 — Post-review sweep: 13 defects in the same day's own code

Two adversarial reviewers went over everything written that day (none of it
had been read by anyone). Findings, all fixed:

**The window persistence did not actually work** — the feature validated by
hand was broken twice over:
- The shutdown capture was **dead code**: `~MainWindow` runs *after*
  `glfwTerminate()` (it is a local of `main`), so `glfwGetWindowPos/Size`
  bailed on the un-init check, zeroed their out-params, and the
  `savedWinW_ <= 0` guard swallowed the write. Quitting normally persisted
  nothing. Moved into `captureWindowGeometryNow()`, called by `main()` while
  GLFW is still alive.
- **`--kiosk` triggered a video-MODE SWITCH**: `setGlfwWindow` (which
  restores geometry) ran *before* `setKioskMode`, so `kiosk_` was still
  false and `glfwSetWindowSize` hit an exclusive full-screen window — which
  GLFW reads as "change the desired video mode". Ordering swapped.
- On X11, measuring right after `glfwRestoreWindow` still read the
  **maximized** rect (the call only posts a `_NET_WM_STATE` message), and
  un-maximized the user's window for nothing. The flag is now recorded
  without touching the window.
- The geometry clamp validated against the primary monitor only, so a
  window kept on a **second display** was dragged back to screen 1 on every
  launch (a monitor left of primary has negative virtual-screen X). Now
  checked against every monitor's work area.

**The kiosk read-only promise had holes**: only 4 of ~20 save sites checked
the flag, so `--kiosk` → F10 → change a profile rewrote `state.cfg`. Fixed
centrally in `Settings::save()` — no call site can forget it now. Also: F10
fired on auto-repeat (holding it flipped full-screen ~30×/s, each entry
doing a disk write); the kiosk menu footer reserved 4 action rows for 5, so
QUIT was clipped below the panel edge with no scrollbar; and leaving kiosk
resumed a machine the user had **deliberately** paused before pressing F10
(the menu's pause is not ours to undo when it was a no-op).

**Emulation core:**
- **The VBL IRQ survived a reset** with no way to clear it: a //c that had
  enabled it re-asserted on the next frame edge into a freshly reset
  machine, and `$C05A` (DisVBL) only decodes while IOUDIS is clear — which
  reset forces back true. `resetSoftSwitches` now disarms and drops the
  line. Self-inflicted by the same day's decision to start asserting it.
- **Snapshot restore did not re-drive the VBL line**, and the `$C070` ack
  was gated on the latch — a rewind landing "not pending" while the line
  was asserted wedged the //c in its IRQ vector forever. The ack is now
  unconditional (as MAME's `lower_irq` is) and restore re-drives the line.
- **The Mockingboard could drone forever after a reset**: the audio thread
  resyncs its register bank only when `ayResetCount_` *changes*, and
  `onReset` set it to 0 — a no-op whenever it already was. It now bumps,
  and clears the pending event queue.
- The AY replay cursor **truncated to whole cycles** (23.19 → 23), drifting
  0.8-1.4 % slower than the producer — larger than the PAL/NTSC delta
  `setCpuClock` exists to fix. Now carries a fractional remainder, like
  `SpeakerDevice::subSampleAccum`.
- A **rewind stranded that cursor**: rolled-back stamps fall far below it,
  so every event collapsed onto sample 0 for the whole rewound span. A
  backward-jump guard re-anchors it.
- Queue overflow dropped the **oldest** event permanently (nothing else
  re-seeds the audio bank); it now drops the queue and forces a resync.
- `M68705P3::kSnapshotBytes` was **137 for 138 bytes written** (the register
  group is four bytes, not three), letting a short blob past the length
  guard and reading one byte past the caller's buffer.
- Two `>` that should be `>=` in "untrusted" clamps (ATA `wordIdx_`, HDV
  `streamOffset`) allowed an index one past the end — an OOB read *and
  write* on the ATA PioOut path.
- The Chat Mauve slot-3 guard tested **occupancy, not type**, so plugging
  the card into slot 3 made it inert; it now only yields to a foreign card.
- `ClockCard::setCpuClock` silently no-opped for non-TP modes (it replayed
  `programTpTimer`, whose `default:` leaves the rate alone); it now
  re-derives from the cached rate. Its snapshot also clamps `tpRateHz_` /
  `tpAccumCycles_` — a crafted blob could give `advanceCycles` a ~2^31
  iteration loop. Stale "48-bit shift register" docs corrected to 40.
- `MouseCard` restore did not re-drive the slot IRQ (the wire-OR lives in
  `SlotPeripheral`, not the PIA), losing a pending mouse interrupt.
- **`tickFrame`'s wall-clock scaling is now `#ifdef __EMSCRIPTEN__`.** The
  browser is its only production caller; every other caller is a headless
  test where "one call = one frame budget" is the contract, and scaling
  collapsed the budget to ~1 cycle. Two existing tests survived only by
  accident. (The suite's slow-test time went 116 s → 295 s once they began
  doing real work again — the fix is measurable.)

## 2026-07-30 — Full screen ⇄ windowed at runtime (kiosk is now a mode, not a launch flag)

Kiosk used to be decided once, on the command line. It is now a runtime
toggle in both directions: **F10**, View → Full screen (kiosk), the
`view.kiosk` command-palette entry, or a new **EXIT KIOSK (WINDOWED)** action
in the in-kiosk menu (so someone who never reads shortcuts can still get
out).

**No snapshot round-trip is involved** — that was the obvious idea, and it
turned out to be unnecessary. Kiosk touches exactly three things: the GLFW
window (exclusive full-screen vs windowed), the render path (`render()`
early-returns to `renderKiosk()`), and settings writing. The CPU, memory and
slot cards are never involved, so the switch is instant and lossless: a game
keeps playing across it, mid-frame. Entering saves the windowed geometry and
restores it on the way back; a session launched with `--kiosk` (which has no
windowed geometry to restore) gets a centred default.

The window geometry is now **persisted** (`window_x/y/w/h`,
`window_maximized` in `state.cfg`) — it previously lived nowhere at all, so
there was literally nothing to restore from. It is written on the way INTO
kiosk (the last chance: kiosk never writes state.cfg) and at normal
shutdown, and applied in `setGlfwWindow` so a plain relaunch also reopens
where you left off. Leaving kiosk prefers the geometry measured this
session, falls back to the persisted one (the `--kiosk`-launch case), and
only then to a centred default. A saved position is clamped back onto a
monitor so a window saved on a since-disconnected screen can't reopen
off-screen.

Details worth knowing:
- **F10 was already taken.** It was the keyboard fallback for the in-kiosk
  Start menu, so entering kiosk ALSO opened that menu in the same frame
  (`onKey` runs during `glfwPollEvents`, before render) — the user asked
  for the game to go full-screen, not for a menu. The kiosk menu's keyboard
  fallback moved to **F1**; the gamepad Start button is unchanged.
- **Leaving full-screen needs the geometry re-applied explicitly.** Many
  window managers ignore the position/size passed to
  `glfwSetWindowMonitor` when un-fullscreening, so the call is now followed
  by `glfwSetWindowSize` + `glfwSetWindowPos` (the standard GLFW
  workaround), and a maximized window is remembered as a flag and
  re-maximized rather than restored as a giant un-maximized rectangle.
- **Leaving kiosk un-pauses.** The in-kiosk menu pauses the machine while
  it is up; exiting from an open menu would otherwise strand the user in
  the GUI with a silently stopped CPU.
- **F10 is routed unconditionally**, alongside F9/F11/F12 — entering kiosk
  from a focused text field must work, and *leaving* it must ALWAYS work. It
  also fires with the in-kiosk menu open, which otherwise swallows keys.
- **The `--kiosk` read-only promise is preserved.** The README says a kiosk
  session "can't disturb your desktop setup". Naively, toggling to the GUI
  would have resumed writing `state.cfg`. A session LAUNCHED in kiosk now
  stays read-only for its whole life (`settingsReadOnly()`), while a GUI
  session that enters kiosk saves once on the way in (so GUI-side changes
  aren't lost if the user quits from kiosk) and is read-only only while
  there.

## 2026-07-30 — PAL + Le Chat Mauve audit: the PAL clock is right, NTSC is the off one

Targeted hunt on the PAL profiles and the Le Chat Mauve RGB card.

Closed the two follow-ups from that audit:
- **WASM ran PAL 20 % fast.** `tickFrame()` burned a full `cyclesPerFrame`
  budget per call, but the browser drives it once per DISPLAY refresh
  (`emscripten_set_main_loop_arg(..., fps = 0)`) — on a 60 Hz panel a PAL
  profile executed 20313 × 60 = 1.22 MHz with a guest VBL at 60.1 Hz
  instead of 50.08. (NTSC had the same hazard on 120/144 Hz panels.) The
  budget is now scaled by the wall time actually elapsed, in units of the
  machine's own `frameIntervalUs`, capped at 4 frames so a backgrounded
  tab can't dump seconds of emulated time into one call. The threaded path
  is untouched — `workerLoop` already sleeps to an absolute deadline.
- **The "unreachable MAME RGB HGR mode" was a false positive**, verified
  against the fetched `apple2video.cpp`. MAME's `hgr_update` gate
  (`rgb_monitor() && m_dhires && !m_80col`, where `m_dhires = !AN3`) is the
  **Video-7** card's foreground-background mode — an American product POM2
  does not model. POM2's Duochrome is the **Le Chat Mauve "Eve"**'s own
  $C0BA/$C0BB soft switch (brevet), which is why the `!state.dhgr` term
  looks inverted, and it IS guest-reachable (`STA $C0BB`; snapshotted since
  blob v2) rather than UI-only. Changing the gate would have broken the Eve
  model and altered the //c PAL default picture, so the divergence is now
  spelled out in a citation comment instead — a real Video-7 would need its
  own card class.

**Headline: POM2's PAL frequency is correct — more accurate than MAME's.**
An Apple II scanline is 65 CPU cycles but **912 master-clock periods**
(64 × 14 plus one stretched "long cycle" of 16). The PAL crystal
14.250450 MHz was chosen so that same 912-period line lands exactly on the
PAL broadcast line rate (912 × 15 625 = 14 250 000), so the long-cycle
average is 15 625 × 65 = **1 015 625 Hz** — POM2's value, right to 0.003 %.
The naive 14.25045/14 = 1 017 889 ignores the long cycle (0.22 % fast), and
MAME's 1 016 966 is just its NTSC figure scaled by the crystal ratio, so it
inherits that figure's own 0.13 % error. The comment in `CpuClock.h` had
this reasoning **backwards** (it apologised for a "deliberate deviation"
that is in fact the accurate number) and has been rewritten — a future
"align with MAME" would have made PAL worse. Noted alongside: POM2's NTSC
clock IS the naive divider and runs 0.22 % fast (guest sees 60.05 Hz vs a
real 59.92 Hz); left as-is because every NTSC-era constant, test and golden
capture is calibrated against it.

Verified correct, no change needed: the floating-bus scanner is fully
PAL-parameterised (its vertical counter runs $C8..$1FF on PAL vs $FA..$1FF
on NTSC, a faithful port of MAME `apple2video.cpp`), as are
`frameCycleToPos`, `pushVideoEventLocked`, the beam segmentation, the FLASH
counter and the 50 Hz worker pacing. The Chat Mauve's AN3 FIFO is bit-exact
with MAME (rising-edge only, 2-bit depth, COL140 reset) and the //c PAL
profile still reaches all four RGB modes after last pass's IOUDIS gating —
IOUDIS powers up *true*, so $C05E/$C05F reach the card.

Fixed:
- **Mockingboard AY replay cursor was left on the NTSC clock** — a
  regression from this same day's emuCycles queue. Under PAL the audio
  thread advanced its cursor 0.7 % faster than the CPU produced cycles, so
  it outran every queued write and applied them all at the buffer START,
  silently undoing the sub-buffer timing on exactly the PAL demos (French
  Touch / DIX) it was built for. Retuned via a new virtual
  `SlotPeripheral::setCpuClock`, applied both on a video-standard change
  and at plug time (a Slot Config "Apply" re-plugs without re-running the
  profile's standard step).
- **The //c VBL interrupt is finally asserted.** `vblIrqPending` was set
  but the CPU line was never driven, on the stated grounds that POM2 "does
  not model IOUDIS" — stale since this week: IOUDIS *is* modelled and
  $C05A/$C05B only reach the VBL mask on //c-class with IOUDIS clear, so
  the arm is now unambiguous. A //c PAL demo using the VBL IRQ as its 50 Hz
  frame sync previously spun on its wait flag forever or free-ran with
  tearing. IIe keeps the polling-only behaviour (there $C05A/B really are
  annunciators, and asserting would resurrect the original ProDOS crash).
- **AppleWin mouse VBL period used the CPU budget, not the video frame** —
  20313 instead of 20280, drifting 33 cycles/frame, a full frame of phase
  every ~12 s. A //c PAL program using the mouse VBL IRQ as a raster
  timebase watched its sync point crawl down the screen. (The card's own
  comment claimed it was locked to the beam.)
- **ClockCard TP period followed the NTSC constant** — the uPD1990AC's TP
  derives from the card's own crystal (a real-time reference), so 64 Hz TP
  ran at 63.55 Hz wall-clock under PAL.
- **Le Chat Mauve's $C0B8-$C0BB Eve registers aliased slot 3.** That range
  IS slot 3's device-select window: an SSC there drives its ACIA
  data/status/command/**control** at exactly those addresses, so a serial
  driver's `STA $C0BB` (baud setup) flipped the Eve's HGR-Duochrome bit and
  turned the picture to garbage. Now decoded only when slot 3 is empty —
  which the card's real home (//c-class, no physical slots) always is.
- **The two Eve toggles are snapshotted** (blob v2, v1 still loads). They
  were documented as "user settings, not guest-volatile" but the
  $C0B8-$C0BB decode mutates them from the guest bus, so a rewind past a
  `STA $C0BB` left the display stuck in Duochrome.
- **`pal_timing_test` now pins the floating bus under PAL** — the one
  PAL-geometry consumer nothing covered, and the one where a stray 262
  would be silent (a wrong RNG byte, a vapor-lock that never fires). RAM is
  filled with an address-revealing pattern so the probe actually
  discriminates.

## 2026-07-30 (later) — The LOW backlog is empty

All seven remaining items from the 2026-07-29 workflow hunt, fixed and
tested:

- **NMOS `NOP abs,X` pays its page-cross cycle.** New `UnoffAbsX` handler
  (reads the operand, adds X, +1 when the page changes — MAME om6502
  `nop_abx`); `$1C/$3C/$5C/$7C/$DC/$FC` route through it in NMOS mode.
  The 65C02's flat-4 entries for `$DC/$FC` stay as they were.
- **Snapshot DMA disarm is lazy.** Kicking a live bus master (SoftCard
  Z80) off the bus used to happen BEFORE a single byte was read, so an
  empty, foreign or immediately-truncated file killed CP/M for nothing.
  It now fires on the first section that actually mutates the machine —
  and a well-formed file carrying neither CPU nor MEM is reported as an
  error instead of a silent success.
- **CLI Phase-C ordering is deterministic.** The deferred actions
  (`--run` / `--paste` / `--step`) slept a fixed 250 ms while the
  positional-disk boot fired on a 30-frame countdown, so the order
  flipped with the host refresh rate (~500 ms at 60 Hz, ~208 ms at
  144 Hz — the actions then ran against a machine the boot was about to
  reset). The deferred thread now waits on a `bootDiskSettled` gate
  (bounded ~5 s so a failed boot can't wedge it), pre-set when there is
  no positional disk so that path keeps its exact old timing.
- **AY READ drives the bus.** `applyControl` counted the READ strobe and
  did nothing else, so a driver probing the chip (write a register, read
  it back — a common presence check, and how some Phasor mode detectors
  identify the board) saw the VIA's own stale port-A output. The AY now
  exposes `busOut`, `Via6522` grew a real port-A INPUT pin model
  (`readPortA` mixes `(out & ddr) | (pin & ~ddr)`, snapshotted), and both
  Mockingboard and Phasor latch the value on a READ — MAME's `m_porta`
  shadow.
- **`Apple2Display::render()` routes from the PUBLISHED frame.** It
  sampled `mem.getDisplayState()` — the live recording frame, already
  running ahead — to pick the demod / mixed-mode path for pixels
  belonging to the published frame, and stored that same wrong state in
  `lastRenderState_` (which the present path consumes). It now folds the
  published events onto the published frame-start state, and keeps the
  composite path alive when ANY band in the frame was graphics. With no
  events the two states are identical, so the non-beam-raced path is
  bit-for-bit unchanged.
- **`$C019` VBL samples at the data-fetch cycle.** The IIe beam-state
  read used bare `cycleCounter`, which only advances at end-of-
  instruction — up to 7 cycles early, enough to report the wrong side of
  a VBL boundary to beam-racing code. It now adds the in-flight
  instruction progress, the same stamp `floatingBus()` and
  `pushVideoEventLocked()` already use.
- **Z80 block-repeat X/Y flags come from PCH.** On a repeating LDIR /
  LDDR / CPIR / CPDR iteration MAME overwrites the undocumented X/Y
  flags with bits 13/11 of PC (`m_f.yx_val = PC >> 8`); POM2 kept the
  per-iteration data-derived value, so F was wrong for the entire run of
  the instruction. zexall stays clean.

## 2026-07-30 — Mockingboard emuCycles queue, MouseCard MCU snapshot, serial parity

The last four items from the workflow hunt's backlog, all pinned:

- **Mockingboard AY register writes are now emuCycles-stamped.** The audio
  thread used to snapshot both AY register banks ONCE per buffer, so every
  write inside that window collapsed to the last value — an arpeggio or
  fast envelope written at ~1 ms intervals came out quantised to the
  ~10 ms buffer (notes merged or dropped outright). That was the real
  emuCycles violation. The CPU thread now stamps each accepted AY store
  with the VIA's synced cycle and queues it; the audio thread owns its
  register bank and replays each write at its exact sample offset, using
  SpeakerDevice's cursor idiom (including the catch-up snap for
  pause/resume and turbo). A PB2 reset still resyncs the bank wholesale
  since that path zeroes registers outside the event stream.
- **MouseCard finally snapshots** — the last card without serialization.
  It needed state surfaces on its two embedded components first:
  `M68705P3` (registers, 112 B RAM, port latch/DDR/input triples, timer,
  interrupt latches — the 2 KB EPROM is ROM and stays out) and `MC6821`
  (both register pairs + the CA/CB edge latches). The card wraps them
  with its bridge state (ROM bank, PIA→MCU port shadows, quadrature
  counters, MCU pacing). Host pointer position is deliberately NOT
  serialized: that is where the user's mouse physically is, not emulated
  state, and forcing it backwards on a rewind would fight the UI.
- **Three serial/clock claims that died unverified on a spend limit were
  re-judged and all three survived**:
  - *SSC RDRF never re-armed.* Real hardware has a one-byte RDR, so MAME's
    mos6551 raises the RX interrupt for every assembled byte; POM2 holds a
    4 KB host ring and raised it once per delivered TCP chunk, so an
    interrupt-driven guest driver read ONE byte per chunk and stalled. The
    RDR read now re-arms while bytes remain queued (each re-arm consumes a
    byte, so it cannot storm).
  - *DCD/DSR polarity was inverted.* These are active-low pins: the status
    BIT is set when the line is INACTIVE. MAME inits `m_dsr(1), m_dcd(1)`
    and AppleWin is explicit ("DSR is active low (see SY6551 datasheet)").
    POM2 reported carrier-present on an idle listener and carrier-lost the
    moment a client connected. Flipped, with the test expectation.
  - *The uPD1990AC shift register is 40 bits, not 48.* MAME shifts 5 bytes
    for a non-4990A part, and disassembling the shipped
    `roms/thunderclock_u9_v1.3.bin` settles it: the nibble routine at
    $CACF emits 4 CLK pulses and the time-read path calls it 10× = 40
    pulses over sec/min/hour/day/month+dow — **there is no year on this
    card**. Reads were unaffected, but MODE_TIME_SET landed one byte out
    of alignment and committed the garbage silently (a set of 23:58:59
    Dec-31 read back as 00:59:58 day 23). The register is now 40-bit, the
    year comes from the host clock (what a real ThunderClock+ does — ProDOS
    supplies its own), and `clock_card_smoke` drives 40 pulses, making it a
    firmware-parity test instead of a model tautology.
- **NMOS KIL/JAM now really jams.** It re-pointed PC at the opcode, so
  step() still serviced IRQ/NMI and an interrupt-driven program could walk
  out of a jam real silicon never releases. It routes through the same
  `halted` latch STP uses — checked before the interrupt poll, cleared only
  by reset, and snapshotted.

## 2026-07-29 — Workflow bug hunt #2: 38 confirmed findings, 13 fixed this pass

A 10-finder / adversarial-verify agent workflow swept the subsystems the
first hunt didn't touch (CPU, paging, Z80, display, audio, storage,
SmartPort/HDV, snapshot, serial, CLI). 38 findings survived verification;
the highs and the sharpest mediums are fixed, the rest is tracked in
TODO.md ([Cards] section). Fixed here:

- **MacBinary-wrapped .dsk write-back corrupted every non-dirty track**:
  detectFormat stripped the 128-byte header at load but saveDirty
  pre-filled from file offset 0 and truncated the file to a bare image.
  The MacBinary wrapper now rides the 2IMG envelope plumbing (captured at
  load, re-emitted by every save path — .dsk/.nib/.d13/.woz), and
  MacBinary-wrapped WOZs actually load now (loadWoz re-read the file and
  demanded the magic at offset 0).
- **saveDirty wrote in place with O_TRUNC**: an ENOSPC/IO failure
  mid-write destroyed the original image, and the retry then zero-filled
  the rest. All four branches now write a sibling temp file and rename
  over the original (atomic on POSIX); failure leaves the source intact.
- **SmartPort 3.5" eject discarded dirty blocks** despite the UI
  checkbox literally saying "save on eject" — eject() now flushes first,
  mirroring SmartPortHdvUnit.
- **Rewind/snapshot-load left the beam-racing video-event log stale**:
  events stamped with pre-restore (future) cycles broke the publication
  carry loop — publishedEvents_ came out empty every frame for the whole
  rewound span while videoEvents_ grew without bound. The log is now
  resynced from the restored clock in Memory::loadSnapshotState.
- **Slot/ROM rebuilds ran outside stateMutex while the AI control server
  was live** (applyProfile steps 5-7, restartEmulationFromSettings 3-4):
  a /reset or /cpu poll during a profile switch raced the ROM rewrite
  and SlotBus unique_ptr swaps. Both spans now hold the lock.
- **STP ($DB) halt latch invisible to snapshot/rewind**: rewinding out of
  a crash kept the machine frozen; restoring a halted snapshot woke STP
  without RESET. Serialized (CPU section 16→17 B, legacy blobs default
  to not-halted).
- **Truncated snapshot half-restored and reported success** —
  restoreMachineState now surfaces the reader's error state.
- **VIA T1LH write now clears IFR.T1** (MAME 6522via.cpp VIA_T1LH; the
  old comment claimed the opposite while citing the same case).
- **13-sector WOZ never detected** (DOS 3.2 .woz couldn't boot): WOZ2
  INFO+38 boot_sector_format==2 is honoured, with a bit-aligned
  D5 AA B5-vs-D5 AA 96 track-0 sniff for WOZ1/format-0.
- **//c IOU parity set**: IOUDIS finally gates $C058-$C05F (MAME do_io
  `(m_isiic) && !m_ioudis` — mouse/VBL switches, no AN3/DHGR flips);
  $C019 on //c returns the LATCHED VBLINT flag (Tech Note #9 semantics,
  MAME c000_iic_r:2256) instead of the IIe beam state; any $C070-$C07F
  access acks the VBL interrupt; and INTC8ROM/IOUDIS/VBL-mask/pending
  ride a new length-prefixed snapshot section.
- **AI /screen vs UI demod race**: the post-stateMutex demod/pixels
  phase is now serialized by a display-owned demodMutex taken in the
  same stateMutex→demodMutex order on both threads.
- **2IMG-wrapped 5.25" floppies classify correctly** (the common Asimov
  format fell through to Unknown in classifyDiskForSlot/accept525).
- **Legacy-spun Disk II motor is promoted into the LSS on first insert**
  (motorOn=true with the LSS idle used to hang the boot PROM poll).

A second pass the same day cleared most of the LOW batch too:
- **NMOS undoc-NOP cycle counts** for the opcodes setCpuMode(NMOS)
  remaps: $14/$34/$74 → 4 (zp,X), $0C/$1C/$3C/$7C → 4 (abs/abs,X),
  $80/$89 → 2 (#imm) — MAME om6502; the generic Unoff2/Unoff3
  stand-ins drifted 1 cycle per instruction (the Mr. Robot RWTS drift
  class). Pinned in cpu_cycle_count_test.
- **VIA ACR write re-arms T1 in continuous mode** (MAME VIA_ACR:
  `m_t1_active = 1` + adjust) — a one-shot-fired T1 stayed dead after
  the guest flipped ACR to continuous.
- **Hostile-WOZ hardening**: per-track bitCount cap (1 Mi-bit) +
  aggregate 32 MB expansion budget; also fixed three stale pre-strip
  size bounds the MacBinary WOZ fix had left behind (OOB read on a
  wrapped WOZ).
- **Snapshot restore honesty**: MEX section failure now propagates
  (state was mutated with ok=true before); speaker/rewind resync runs
  even when the restore FAILS (a truncated file has already applied
  CPU+MEM — the early return skipped the resync exactly when needed);
  the snapshot's cpuMode byte is no longer applied (machine
  configuration, not state — it bypassed resolveCpuMode's
  soldered-65C02 clamp and froze a //c on an NMOS-mode blob).
- **Rewind media restore is gated on the capture predicate** — applying
  a ring frame's decoded tracks onto a drive that NOW holds a WOZ wiped
  the WOZ's canonical bit streams.

A third pass closed the per-card snapshot gaps — five of the six cards
that serialized nothing now carry their guest-visible state, pinned by
the new `card_snapshot_state` test (each case drives the card into a
distinctive state, restores into a fresh card, and re-serializes to
prove the PRIVATE fields travelled; every loader also ignores a foreign
blob):
- **CffaCard** — the whole ATA taskfile plus the in-flight PIO phase,
  LBA, sector counter, 512-byte word buffer and `wordIdx_` cursor
  (`AtaBlockDevice::append/loadSnapshotState`, CHS geometry included
  with divide-by-zero guards on restore). A rewind mid-transfer used to
  resume the guest's read loop against the live cursor.
- **ProDOSHardDiskCard** — selected block + byte cursor within it.
- **SmartPortCard** — a v1.1 tail carrying the $Cn0D protocol call
  engine (`spCollect_`/`spCollectN_`/`spResult_`/`spResultPos_`/
  `spPushPages_`/`spError_`); v1 blobs still load and reset the engine
  rather than letting the live one leak through.
- **SuperSerialCard** — the ACIA command/control decode (DTR, RX-IRQ
  enable, echo, word length, baud index), sticky status errors and IRQ
  mask. The socket, rings and printer spool stay host-side on purpose:
  a rewind cannot un-send bytes that already left the wire.
- **ClockCard** — the uPD1990AC 48-bit shift register, edge-detect
  latches, mode, user time offset and the TP/IRQ timer, re-driving the
  slot IRQ line from the restored flip-flop. A rewind mid-shift-out
  used to hand ProDOS a garbled date.

MouseCard is deliberately left for its own session (it embeds an
M68705P3 MCU + MC6821 PIA that need state surfaces first) — see TODO.md.
Also still deferred: the Mockingboard AY event-queue redesign and the
residual LOW items (KIL IRQ-permeability, Z80 block-op X/Y flags, AY
READ bus latch, NOP abs,X page-cross +1, and friends). The serial-input
finder's claims died unverified on a spend limit — parked, not judged.

## 2026-07-29 — //c+ boots and WRITES 5.25" for real (the "dual-controller" was three bugs)

The bug-hunt's 🔴 "//c+ dual-controller" entry got its repro — and the
repro showed the //c+ never even reached the disk: **every cold boot hung
at $F0FC with a blank screen**, in the alt firmware's boot drive-scan.
`tests/iicplus_boot_probe` (headless full //c+ stack: IWM + SmartPortHub +
2× Sony 3.5" + slot-6 Disk II) plus a `POM2_TRACE_IWM_SENSE=1` diagnostic
narrowed it to three distinct bugs:

1. **IWM SENSE with no selected drive** — the firmware polls the IWM
   status register with devsel=0 before enabling anything. MAME
   `iwm.cpp:129` reads `(!m_floppy || m_floppy->wpt_r()) ? 0x80 : 0` —
   no floppy → SENSE pulls HIGH. POM2's `disk_` is permanently attached
   by DiskIICard, so the read answered with the 5.25" image's
   write-protect bit: a writable disk → 0 forever → the scan's very
   first `LDA $C0EE / BPL` never fell through.
2. **Sony DSKCHG polarity** — MAME (`floppy.cpp:560/672/723`, mac wpt_r
   `!m_dskchg`) senses HIGH for an *empty* drive; POM2's `diskSwitched_`
   flip-flop read 0 ("disk in place") for an empty external 3.5", so the
   scan walked into the read-a-disk path of a drive with no disk. DIR
   init was also inverted (MAME `m_dir(0)`). A dead `SmartPortHub::
   onIwmMotor` broadcast helper (motor to BOTH Sonys, contradicting
   MAME `iwm.cpp:99-115 set_floppy` motor-follows-selection) was removed
   before it could be wired by accident.
3. **The actual dual-controller hazard, on writes** — with the boot
   fixed, DOS 3.3 booted but `SAVE` ended in **I/O ERROR**: the IWM's
   bit-cell walker (authoritative $C0EC reads) mis-frames RWTS's
   write-verify, and `IWMDevice::flushWrite` pushed 5.25" flux into the
   same DiskImage DiskIICard's LSS was writing (double write). Fixes:
   the IWM never writes 5.25" flux (DiskIICard owns it; the IWM keeps
   the 3.5" Sony write path, which no other controller sees), and
   `ioReadIWM` is authoritative **only while the hub routes to a 3.5"
   Sony** — the POM2 split of MAME's single-controller
   `recalc_active_device` model.

After: //c+ cold-boots to the DOS 3.3 banner and a full
`SAVE / LOAD / RUN` round-trip works on the //c+ profile, in both
authoritative and shadow modes. Print Shop boots to its title screen.
Pinned by `iic_plus_boot_write` (unit sense polarities + full-machine
boot + write round-trip). The MAME oracle (`apple2cp` romset assembled
from POM2's own `apple2cp.rom`, CRC-identical to 341-0625-a.256)
confirmed the expected boot banner behaviour.

Worth keeping: the "//c+ 5.25" auto-boot works" claim had silently
rotted — no test covered it, so the SENSE regressions were invisible
until the dual-controller investigation went looking. The scan hang
looked exactly like the dual-controller symptom but was three unrelated
MAME-parity deviations stacked.

## 2026-07-29 — Bug-hunt sweep: 20+ fixes across W5100, CS8900A, ImageWriter, IWM snapshots

A five-agent audit of the two Ethernet/printer commits (04890e1, f7af757)
plus their integration seams, every finding verified in code before fixing.
The ones worth remembering:

- **W5100 TCP silently lost data on a slow peer.** SEND advanced SN_TX_RD
  *before* the single non-blocking `sendto()`, so a short write or EAGAIN
  dropped the tail while the guest saw a fully-free ring. TCP now stashes
  the unsent tail per-socket (`pendingTx`) and `poll()` retries until it
  drains (1 MiB cap → honest connection close). UDP keeps fire-and-forget
  on purpose: queueing datagrams would fuse their boundaries, and dropping
  one on EAGAIN is legal.
- **Peer FIN now parks in SOCK_CLOSE_WAIT ($1C)** instead of collapsing to
  CLOSED — the guest can still SEND before DISCON, which is what every
  drain-then-disconnect W5100 driver keys on. CONNECT is gated on
  TCP-INIT (it used to "establish" UDP sockets and drop their RX header),
  and CONNECT to DIPR 0.0.0.0 closes instead of reaching 127.0.0.1 via
  Linux's `connect(INADDR_ANY)` semantics.
- **Ring geometry vs. stale state**: shrinking RMSR under staged data (or
  a crafted snapshot) underflowed the free-room math to ~64 K and let the
  RX writer stomp the neighbouring socket's ring. `clampRingState` re-fits
  the cursors on every geometry rebuild and on snapshot load; a clamped
  non-pow2 carve is rounded down to a power of two so the `& (size-1)`
  masks stay exact.
- **`romBank_` ($C028) was not snapshotted** — the highest-impact //c-class
  rewind gap: restoring a PC captured under one firmware bank while the ROM
  reader served the other. Now in the MIG blob's optional tail together
  with `migIntDrive_`/`migHdSel_`; the IWM blob likewise gained `phases_` +
  `writeDataLoaded_`, and both loaders re-fire the phases/devsel callbacks
  so the SmartPort hub is told about the restored lines (its own state is
  live, and a transition-gated callback stays silent when values happen to
  match).
- **ImageWriter parser hardening**: a non-digit in an ESC G count went
  negative → `uint32_t` cast → ~4 G bytes of "graphics" and a deaf printer
  (`paramDigit` now clamps); ESC '/ESC I left the previous command's
  parameter count armed and ate up to 6 characters; ESC r (reverse feed)
  produced *negative* pacing costs (credit grew past the cap and dumped
  the queue in one frame) and walked the head to negative Y.
- **"Clear all" UB in the paper tray panel**: `nDone` was captured before
  the front-panel buttons, so the follow logic indexed `completedPage()`
  into the vector the button had just emptied. Counted after the buttons
  now; completed-sheet texture identity also includes `droppedPageCount()`
  so the 32-page cap can't leave a dropped sheet's pixels under a new
  label.
- **Kiosk mode never pumped the ImageWriter** (early-return before
  `pumpImageWriter()`): a printing //c parked every byte in the card spool
  forever. The pump also tracks *which* source its drain cursor counts
  against — carrying it from an unplugged PrinterCard onto the SSC tap
  skipped or replayed part of the stream. Spool growth is bounded (SSC tap
  trims its consumed prefix using absolute offsets; the mechanism
  force-drains past a 1 MiB backlog).
- **CS8900A**: `UthernetCard::onReset` re-stamped `kDefaultMac`, reverting
  the guest-programmed IA on every Ctrl-Reset (MAME preserves it — pinned
  by `testMacSurvivesCardReset`); the RxEvent Extradata bit (0x4000) was
  computed after the clamp that made it dead code; the data-window
  read/write now feeds the `ioRegs_` cache so `peek()` stops reporting $00.
- **Not fixed on purpose**: the //c+ still runs the same IWM + DiskIICard
  dual-controller arrangement on $C0EC that f7af757 removed from the plain
  //c — it needs a repro (Print Shop save on //c+) before touching the
  routing, because the //c+ genuinely needs the IWM for MIG/3.5". Filed in
  TODO.md [Storage] with the mechanism spelled out. HT/VT jumping to the
  *farthest* tab stop is byte-identical to the reference implementation —
  owned as parity, not silently "fixed".

Pinned by new cases in `uthernet2_w5100_smoke` (half-close, RMSR shrink,
CONNECT gating, ≥$8000 mirror writes), `uthernet_cs8900_smoke` (MAC across
reset), `imagewriter_smoke` (parser hardening ×3) and `iwm_mig_snapshot`
(romBank round-trip + old-blob compatibility).

## 2026-07-28 — The //c prints for real: on-board IWM was fighting the Disk II

Printing on a //c through the slot-1 SSC to the host ImageWriter looked wired
up and produced blank paper. The serial path was never the problem — it is
byte-exact (600+ chars via `PR#1`, zero loss) and Print Shop's short SETUP
test print rendered correctly the whole time. What failed was **disk**.

POM2 mirrors `$C0E0-$C0EF` into the on-board IWM on //c-class profiles that
have an alt firmware bank. MAME wires its IWM as *the* slot-6 controller,
replacing the Disk II. **POM2 does not** — `iwmAuthoritative` leaves the
slot-6 `DiskIICard` answering for 5.25". So on a plain //c the mirror was a
*second* controller on the same soft switches, supplying no data path but
still running its own phase/motor handling. Two controllers stepping one
drive drifts the head, and DOS 3.3 RWTS then falls into endless seek/retry
storms — `$B948-$B956` with the head oscillating between the target track
and 0. Print Shop could neither save its setup nor load its print overlay,
so it returned to its menu without even rasterising, and nothing reached the
printer.

Gating the mirror on `isPlus_` fixes it: the IWM is consulted only where it
actually owns a drive, the //c+ MIG / Sony 3.5" path. With that, the //c
prints a full Print Shop greeting card — **104 097 bytes, 105 `ESC G`
graphics bands, 2 sheets**, border plus tiled artwork — matching the //e
byte-for-byte in structure.

The lesson worth keeping: POM2 and MAME differ on *who owns slot 6* on a
//c, so MAME's wiring cannot be copied verbatim here. Pinned by
`iic_diskii_no_iwm_conflict`, which asserts the plain //c never routes
`$C0E0-$C0EF` to the IWM (and never even ticks it) while the //c+ still
does.

Two red herrings ruled out along the way, both reverted: the SSC pins
`SR_TDRE` high (real 6551 pacing at the programmed 9600 baud changes
nothing), and asserting DCD for the printer tap actively **hangs** the //c
serial firmware in its modem path.

## 2026-07-28 — Ethernet: Uthernet I + II, and the last big functional gap closes

POM2 can now talk to the modern internet from an Apple II, which was the last
item on the backlog with no implementation at all.

**Two cards, two completely different animals.** This is the thing to
internalise before touching either file:

- **Uthernet I** (`uthernet`) is a *NIC*. Its CS8900A moves raw Ethernet
  frames and nothing else; the TCP/IP lives on the Apple side (IP65, Contiki,
  ADTPro-ethernet). It is useless without a host transport that speaks
  Ethernet.
- **Uthernet II** (`uthernet2`) is a *TCP/IP offload engine*. The guest writes
  an address and a port into W5100 registers, issues `CONNECT`, and pushes
  payload at a ring buffer — the chip does the protocol work. That maps
  one-for-one onto host BSD sockets, so **the Uthernet II needs no Ethernet
  backend at all** for TCP and UDP. Period IRC, telnet and FTP clients work on
  a stock build with no libslirp and no privileges. Only its MACRAW/IPRAW
  modes need a transport.

That asymmetry is why the host side is optional. `NetworkBackend` has three
implementations: `Null` (always present, drops everything, keeps the cards
pluggable and software-detectable), `Loopback` (transmit feeds receive — drives
both smoke tests so CI never touches a network), and `Slirp` (libslirp
user-mode NAT, gated on `POM2_HAVE_SLIRP`). libslirp rather than TAP or pcap
because both of those need root; slirp terminates the guest's IP in-process and
re-opens ordinary user-space sockets. Virtual network is QEMU's: guest
10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3.

**The CS8900A is a verbatim MAME port** (`machine/cs8900a.cpp`, itself a VICE
port) with line citations throughout, plus the ~40-line card shim from
`bus/a2bus/uthernet.cpp`. Three deliberate deltas: MAME is *pushed* frames by
`device_network_interface`, which POM2 has no equivalent of, so `pumpBackend()`
pulls on the cycle hook applying the same `shouldAccept()` pre-filter (bounded
per call so a busy link can't stall the CPU thread); the `assert()`-heavy
PacketPage macros became clamped accessors, because a mis-decoded `$C0nX` must
never take the emulator down; and `peek()` replaces
`machine().side_effects_disabled()` for the debug panel.

The subtle part of that chip is that **transmit is a four-step handshake** —
TxCMD, TxLength, *read* BusST and observe `Rdy4TxNOW`, then push bytes — and
skipping any step must emit nothing. Easy to "simplify" into a bug; pinned.
Likewise reading RxEvent before draining a staged frame is an "implied skip"
that discards it, which is real hardware, not a defect.

**The W5100 had no MAME device to port**, so the reference is AppleWin's
`source/Uthernet2.cpp` cross-checked against the WIZnet datasheet. One
substantive improvement over the reference: AppleWin resolves virtual-DNS names
with a blocking `getaddrinfo()`. Under POM2's `stateMutex` that would stall
emulation for however long the resolver takes. Ours runs the lookup on a
detached thread with a 120 ms bounded wait, and a late answer lands in a
mutex-guarded mailbox that `poll()` folds into the cache on the CPU thread — so
the guest's retry succeeds instantly and the audio never glitches.

Snapshot rules worth not rediscovering: the CS8900A's inbound frame queue and
the W5100's live sockets are **deliberately excluded**. Both mirror host
network state that has moved on by the time a rewind replays. A restored TCP
socket that claimed to still be `ESTABLISHED` would hang the guest waiting on a
peer that is gone, so it comes back `CLOSED`; the raw modes carry no host state
and do return. The 4 KB PacketPage and 32 KB W5100 memory *are* saved, which
sounds expensive for a 60 Hz ring until you remember `RewindBuffer` XOR-deltas
them — an idle NIC costs a handful of bytes per frame.

`LISTEN` is decoded but unimplemented, and says so rather than pretending:
neither transport can route an inbound connection to the guest.

Pinned by `uthernet_cs8900_smoke` (MAME-parity register, handshake and filter
behaviour over a loopback backend) and `uthernet2_w5100_smoke`, which runs a
**real TCP session** — OPEN, CONNECT, SEND, RECV, CLOSE against a listener the
test opens itself, deliberately with no backend plugged, so the "TCP needs no
transport" claim is a test and not a comment.

## 2026-07-28 — The //c prints: SSC printer tap, multi-page PDF, Grappler+ pinned

Three follow-ups that close out the printing chantier.

**The //c's printer port feeds the ImageWriter.** On a real //c the printer
port is the *serial* port — there is no parallel card to plug — so a stock //c
profile could render paper only by lying about its hardware. The SSC now has a
printer tap: every byte the ACIA accepts for transmit is mirrored into a
host-visible spool with the same `drainSpoolFrom` shape as the parallel cards,
and `pumpImageWriter()` treats it as a third source (parallel cards outrank
it, so a IIe with both keeps its routing). Slot 1 taps by default — `PR#1 :
PRINT` prints with zero configuration.

The pitfall worth remembering: enabling that path exposed that POM2's
synthetic SSC ROM never initialised the ACIA outside Pascal's PINIT. A 6551
with DTR de-asserted parks its transmitter at MARK (MAME `mos6551.cpp:
317-321`), so every `PR#n : PRINT` byte was silently dropped — the TCP telnet
bridge had the same latent bug, masked whenever a host program or test wrote
the command register first. The PR#n/IN#n entries now program cmd=$0B before
hooking CSW/KSW, exactly what the real SSC firmware's DIP-switch init does.

**Multi-page PDF export** (`ImageWriterPdf.{h,cpp}`). The reference emits
PostScript; a bare `.ps` is a dead end on modern hosts, and the cost
difference vanished once the page raster existed. Each sheet embeds as an
8-bit `/Indexed /DeviceRGB` image — the ImageWriter raster already *is* one
byte per pixel with a recoverable palette — FlateDecoded through the stb zlib
compressor that was already in-repo for PNG. Zero new dependencies. Completed
sheets now carry their own `dpi` (`Page::dpi`), so a sheet ejected at 144 dpi
keeps its true `/MediaBox` even if the user then flips the printer to 288.
Pinned by `imagewriter_pdf` including a byte-exact xref audit and a Flate
round-trip through stb_image's inflater; `pdfinfo` validates the output.

**Grappler+ pinned against MAME `bus/a2bus/grappler.cpp`** with line ranges
cited at every ported block. One silent divergence found and fixed: POM2
cleared the ROM bank on reset, but the U2D bank flip-flop is not wired to bus
RESET (`reset_from_bus:536-539` touches only the ACK latch) — a reset
mid-graphics-dump must leave the high $C800 bank selected until the next
$CnXX fetch. Also added: `$CnXX` *writes* (bus conflicts on real hardware)
drop the bank like reads do (`write_cnxx:586-591`). Documented-deliberate
divergences: the 7-clock /STROBE pulse collapses to instant (nothing observes
it), the edge-driven IRQ flip-flop is derived as its equivalent level, and
`ackEffective()`'s BUSY gate is POM2's host back-pressure model, which MAME —
wired to a live centronics /ACK — does not need.

## 2026-07-28 — ROM Status panel: what you have, what's missing, what it costs

POM2 ships no ROMs, so the most common failure mode by far is a dump that is
absent, mis-named or the wrong variant — and every symptom that produces shows
up a long way from the cause. A profile silently boots the wrong firmware; a
card refuses to plug with a one-line warning in a log nobody reads; a Grappler+
prints fine but AppleWorks doesn't recognise it. The information existed, spread
across eight probe sites and a log file.

Help → ROM Status puts the whole picture in one window: every ROM POM2 probes,
in probe order, resolved against the live ResourcePaths search roots, with size,
CRC32, and — on hover — the full candidate list and *what breaks* if nothing
resolves. Machine firmware and character generators are read straight from
`profileConfig()`, so a new profile appears there with no edit; the peripheral
side lives in `RomCatalog.h`, mirroring each card's probe list at its plug site.

Three judgements, kept deliberately separate:

- **Missing** is an error (red) for machine firmware, a warning for everything
  else — most card ROMs degrade rather than fail, and the row says how.
- **Size** is the only hard check. A Disk II PROM is 256 bytes and a Grappler+
  EPROM is 4 KB, full stop; a mismatch is a wrong file, not a variant.
- **CRC32** is shown for identification always, but only *judged* where POM2 has
  a documented reference dump to compare against (the two CFFA 2.0 images).
  Asserting a checksum POM2 cannot vouch for would turn a legitimate variant
  into a false alarm.

A `(fallback)` mark flags the case that used to be invisible: the //e
Unenhanced profile resolving to `apple2e.rom`, i.e. running 1983 hardware on
Enhanced firmware because the dedicated dump is absent.

## 2026-07-28 — Slot Configuration and the media bays are two windows

Yesterday's pass added banners to both columns of Slot Configuration so the
window would stop hiding that its halves run opposite interaction models: the
left is staged (edit, then Apply — which restarts the machine — or Revert), the
right is immediate (Mount / Insert / Eject act at once). Narrating the split was
the wrong fix. Apply and Revert sit at the bottom of the assignment column and
still LOOK like window-level buttons, so "mount a disk, then Revert" reads as
undoable no matter what the banner says.

They are now separate windows: **Slot Configuration** (Machine →) is only the
per-slot card list plus Apply / Revert, and **Internal Disks & Media**
(Devices → Storage) is only the internal drives and the mountable bays of the
plugged storage cards. Each header points at the other, so neither is a dead
end. Side effects worth having: the assignment list gets the full window width
instead of 52 % of it, and the media column stops collapsing into a ~100 px
sliver when the panel is docked into a side dock — the responsive
two-column/stacked dance that existed only to survive that squeeze is gone.

Persisted as `show_media_panel`; command palette `panel.media`; both windows
are cleared in kiosk mode, and the Emulation dock preset docks both.

## 2026-07-28 — Disk II write-back: two bugs that made writing impossible

Symptom that opened it: The Print Shop hangs forever on "PRESS RETURN TO SAVE
SETUP INFO ON PRINT SHOP DISK", and every module that runs afterwards then
reads the FACTORY printer config off the disk (slot 1 / EPSON APL) and spins
in its handshake loop against whatever card is really in slot 1. Nothing about
that is a printer problem — the setup save never lands, so nothing downstream
can work. Underneath were two independent defects.

**1. The write-back opt-in never reached a running machine.** `plugDiskII`
never applied `disk_writeback[_slotN]` to the card it had just built — its
`plugHdv` / `plugCffa` siblings, twenty lines below, always did — and
`applyProfile`'s media snapshot carried the mounted PATH per slot but not the
toggle (the CFFA snapshot right beside it carries `{path, writeBack}`). Since
`applyProfile` re-plugs on every profile switch INCLUDING the one the
constructor runs at startup, whatever the MainWindow ctor restored was thrown
away moments later. `isWriteProtected()` is `fileWriteProtected || !writeBack`,
so the guest simply saw a write-protected disk: DOS 3.3 answered WRITE
PROTECTED, and Print Shop retried forever. Both sites now restore it, and
`applyProfile` carries the LIVE toggle so a mid-session change made from the
Disk II panel survives the rebuild.

**2. The flux→nibble re-pack corrupted the track it wrote.** With the first bug
fixed, DOS got as far as writing and answered I/O ERROR — and the disk was
unreadable from then on, CATALOG included. A nibble store has no angular
length, so `writeFlux` has to turn the flux the head lays down back into
nibbles. It did that by walking the PADDED cell timeline of the track that was
already there (8 cells per nibble, +2 for each $FF inside a sync run) and
overwriting the nibbles the window covered. That is only correct while the new
content pads exactly like the old one, and a sector write never does — DOS
writes its own sync run (a 40-cycle loop = a 10-cell $FF) wherever it likes, so
from the first sync byte on, the old grid and the new stream disagree and every
following nibble is assembled from its neighbours' cells. Rewriting a track
with its own contents was idempotent, which is why it survived so long; writing
an actual data field mangled 345 of its 353 nibbles and spilled into the next
sector's address field.

The head has no grid. It writes a continuous bit stream and the reader
self-syncs on it, so POM2 now FRAMES the incoming cells the same way: skip
0-cells until a 1, then that 1 plus the next seven cells are the nibble (the
two 0-cells trailing a sync $FF are skipped, which is exactly what makes them
sync), and the nibbles are laid down sequentially from the slot the head is
over. The shift accumulator lives in `DiskImage::writeFraming[track]` because
DiskIICard flushes every ~30 transitions and a nibble straddles chunks
constantly.

**And the cell grid comes from the WRITE clock, not the revolution anchor.**
The head emits one cell every `lssCyclesPerCell()` LSS cycles from the moment
write mode came on, so a burst's transitions are exact multiples of that apart.
Quantising them against the revolution phase — `(t - revolutionStart) mod
period / cyc`, which is right for READS — put the grid at an arbitrary sub-cell
offset: adjacent transitions rounded into the same cell, one of the two was
dropped, the nibble lost a bit and the framing slipped. The anchor is still
what says WHERE the burst starts; it is now consulted once, to pick the nibble
the head is over, not per transition. A mid-nibble splice leaves that nibble's
old value alone (a real write splice leaves exactly that stub) and frames into
the following slot.

Verified end to end, not just in the unit: DOS 3.3 `SAVE`, then `CATALOG`,
`LOAD` and `LIST` return the program; the host `.dsk` gets 38 bytes on tracks
10 and 17 (the file plus the VTOC/catalog). The Print Shop setup save now
completes and writes its 3 config bytes at track 11 — `01 01 01` → `07 04 03`,
slot 7 / Apple ImageWriter / Grappler+ — and a greeting card prints from a
disk that configured itself.

Pinned: `disk_writeflux_framing`. Note that `disk_write_controller_smoke`
could not have caught this: it never calls `loadLssRom`, so it exercises the
legacy 32-cycle nibble gate, while the shipped app bundles `roms/diskii_p6.rom`
and therefore always runs the LSS/flux path.

Diagnostics added: `POM2_TRACE_WRITEFLUX=1` logs each splice window (cells,
anchor nibble, framing state), and `POM2_TRACE_PC_MAX` raises the instruction
cap on `POM2_TRACE_PC` for a window that sits past a boot + menu walk.

## 2026-07-28 — ImageWriter II: the paper is continuous fanfold

The panel drew the printable raster alone, so POM2's paper read as a cut A4
sheet out of an inkjet. An ImageWriter II is fed 9.5" fanfold: the printable
body plus a 0.5" pin-feed strip each side, each strip perforated off along a
line of sprocket holes on 1/2" centres, and each sheet joined to the next by a
horizontal perforation. The strips and perforations are drawn around the page
texture, not into it, so the page bitmap and "Save sheet as PNG" stay pure
printable area.

## 2026-07-27 — UI pass 5: Slot Config stops hiding which changes are staged

The panel has always run on two different interaction models and never said so.
Left column: edit the slot combos, then Apply (which restarts the emulator) or
Revert — staged. Right column: Mount / Insert / Eject — immediate. Apply and
Revert sat at the bottom of the left child, so they read as governing the whole
window: mount a disk on the right, hit Revert on the left, and expecting the
mount to come back is a perfectly reasonable reading.

Both columns now announce their model. Changed rows get an accent dot whose
tooltip names the card actually plugged; a badge counts "N staged change(s) —
not applied yet"; Apply is disabled when nothing is staged, because a button
that restarts the machine should never be a no-op someone hits by reflex.

Two layout defects fixed while in there:

**Slot numbers trailed their own control.** `LabelText` / `BeginCombo` put the
label on the right, so the panel read "(empty) v  Slot 1" — the number, which
is exactly what the eye scans down the column for, came last. Labels now lead,
with the gutter measured off the widest one so it survives the UI zoom.

**The assignment column was a hardcoded 400 px.** Fine at the 880 px
free-floating default; once docking landed and the panel went into a side dock,
the media column got a ~100 px sliver with every label clipped to
"Mount / Inser". The columns now go side-by-side only above 46 em and stack
below it.

Also renamed `pom2::statusLed` (added in pass 1 for the status-bar drive light)
to `pom2::indicatorDot`. `StatusLed.h` already owned a `pom2::statusLed` for
*media* status with its own colour table and tooltips. Overload resolution
happened to pick correctly at every call site — exact match beats a bool→ImU32
conversion — but two same-named functions in one namespace meaning different
things is a trap set for whoever writes the next call.

## 2026-07-27 — UI pass 4: command palette, and the Disk Library becomes a browser

**Command palette (Ctrl+Shift+P).** 42 menu items across 8 menus, ~33 panels,
four keyboard shortcuts. Type "mock", "amber", "eject", "pal" and hit Enter.
New `CommandPalette_ImGui`; dispatch is one switch in `MainWindow::runCommand`.

Shift is load-bearing in that chord: plain Ctrl-P must keep reaching the guest
because CP/M under the SoftCard uses it for printer echo. The palette also joins
`isGlobalKey` so it opens from a focused text field — same reasoning as F11/F12.

Unavailable commands stay listed and greyed instead of being filtered out:
seeing "Phasor (no card plugged)" teaches where the thing lives.

**Disk Library.** A flat list of ~950 rows with full relative paths became a
nested folder tree with pinned Favourites and Recent sections. Two bugs on the
way, both the kind that look fine until you read the output carefully:

*A flat lexicographic sort does not group directories.* `demo/PLASMAG.dsk` sorts
before `demo/digidream/DD.dsk` which sorts before `demo/zzz.dsk` — so walking
the list and opening a node whenever the directory prefix changes emitted
`demo` **twice**, two `TreeNodeEx` calls with one ID, colliding in ImGui's
storage and sharing a single open/closed state. That was visible as folders
that wouldn't stay open. Now built as a real nested structure.

*ImGui applies tree indentation to the first column only.* The favourite star
had its own narrow column at index 0, which ate the whole indent and left every
filename flush left at any depth — a tree with no hierarchy to read. Name moved
to column 0; star and mounted dot became inline prefixes.

The sort selector (Name / Size / Date) is gone: the latter two forced a flat
list, because you cannot group by folder and order by size at the same time, so
they were quietly fighting the tree. Size / Date columns are now hideable, which
is what makes the panel usable in a narrow dock.

Favourites and recents live in `MainWindow`, not the panel — it has no Settings
access and no business acquiring one. Both pack into one `state.cfg` value
joined by **0x1F**: the file is flat `key=value` and a disk path can contain
spaces, commas, semicolons and colons, so the separator must be a byte a path
cannot hold. Recents track the panel's mount *requests* rather than the cards,
so a CLI or drag-and-drop mount doesn't reorder the list behind the user.

The favourite toggle sits in the right-click menu rather than being a clickable
star: the row is already a full-span selectable, and an overlapping hit target
inside it mis-fires — on a panel whose left-click cold-boots the machine, that
is not a cosmetic concern.

**`tools/dedupe_library.py`.** A duplicate on disk is a duplicate in the
browser, and this library had 20 groups / 21 redundant files / 4 MB of them:
`dsk/` ↔ `gist/` copies (sometimes renamed — `CRIME_A.dsk` is
`Le Crime du Parking A.dsk`), verbose archive names beside short hand-written
ones (`Congo Bongo (1983)(Sega)[48K].woz` = `Congo Bongo.woz`), flat files
duplicating their own per-game subfolder, and a `Copy (1)` + `Copy (2)` +
original triple. Groups by size first and hashes only within same-size buckets.
Dry-run by default; keeps the shortest path, and never keeps a `Copy (N)`.

## 2026-07-27 — UI pass 3: CRT Settings stops asking for 13 numbers

The panel opened on 13 bare numeric knobs with no starting points and one
"Reset to defaults". Almost nobody wants to dial a luminance gain; they want to
pick a look. There is now a preset row — **Clean / Composite TV / Trinitron /
Arcade** — and the sliders moved behind a collapsed `Advanced`.

Presets deliberately **preserve `palMode` and `textSharp`**. PAL composite
describes the machine being emulated (the two PAL profiles), and sharp text is
a legibility preference; a look picker that silently flipped either would be
wrong. Only the glass is a "look".

**The real bug was the messaging.** A green "CRT Effects: ON" banner sat
directly above a red "Shader unavailable — POM2 falls back to the standard NTSC
LUT". Both were true and they read as a flat contradiction, leaving no way to
tell whether the controls below did anything. They describe *different passes*:
only the OpenEmulator demodulation shader was missing, and the CRT glass stack
is a separate pass that still runs. The warning now scopes itself explicitly.

Layout fix worth recording: ImGui's `SliderFloat` puts its label on the
**right**, so the panel read "bar → number → name" and clipped the longest label
to "Phosphor curve (ga…". Labels now lead, via `SameLine(labelW)` +
`SetNextItemWidth(-FLT_MIN)`, with `labelW` measured from the widest label so it
survives the UI zoom rather than being hardcoded. Values dropped to two
decimals — `0.055` on a perceptual knob was false precision.

## 2026-07-27 — UI pass 2: docking, so 33 panels stop fighting over the screen

POM2 had ~33 free-floating panels. Opening two meant one covered the other and
usually the Apple II screen too; positions lived in `imgui.ini` as absolute
pixels, so they also went stale the moment the UI zoom changed (that gap was
called out in pass 1). There is now a **DockSpace over the viewport work area**
with a curated default layout and four task presets (View ▸ Layout).

**This changed a vendored dependency.** Docking is not on Dear ImGui `master` —
no `IMGUI_HAS_DOCK`, no `ImGuiConfigFlags_DockingEnable`. `imgui/` moved to the
`docking` branch, and because that branch is **force-pushed on every upstream
rebase**, it is pinned to a commit. The pin lives in one place,
`imgui_pin.env`, sourced by `setup_imgui.sh` *and* both CI jobs — all three
previously did an unpinned `git clone --depth 1` of master, so a fresh clone or
a CI run would have failed to compile the moment docking landed. Multi-viewport
is deliberately left off: separate OS windows for panels means per-viewport GL
contexts and a different render loop, for no benefit here.

**The chrome now reserves its own space instead of assuming offsets.** Menu
bar, toolbar and status bar are all `BeginViewportSideBar` windows, each adding
to the viewport work-area inset, and the dockspace covers what's left. The
toolbar had to be converted from a hand-positioned `SetNextWindowPos(WorkPos)`
window to get this — which also fixes the pass-1 defect where a 150 % zoom made
the toolbar taller than the saved `Apple II Screen` position and the screen
window drew over it. Both bars got `NoDocking`; without it a dragged panel can
be dropped into the one-line status strip.

Three non-obvious things worth recording:

**Seeding is gated on a persisted flag, not on "is the node empty".** By the
time we could inspect it, `DockSpaceOverViewport` has already created the node,
so emptiness can't distinguish a fresh install from a user who undocked
everything on purpose — and rebuilding each launch would silently discard their
layout. Hence `ui_dock_seeded` in `state.cfg`.

**Docking a *hidden* panel is the point, not a no-op.** The assignment is
written into the window's settings, so opening the Memory viewer later makes it
a tab in the bottom-right group instead of a window floating over the game.
That is most of the value of seeding a layout.

**The screen window's manual title-bar drag had to be disabled while docked.**
`Apple II Screen` carries `NoMove` (click-drag inside the screen must reach the
guest's Mouse Card) plus a hand-rolled title-bar drag. Docked, it has no title
bar and the dock node owns its position: the computed rect lands on the node's
tab bar and `SetWindowPos` fights the node every frame — the screen jitters and
the tab won't drag out. Guarded with `ImGui::IsWindowDocked()`.

Limitation, by construction: presets place windows by **literal title**, so the
slot-numbered panels (Disk II, 3.5", HDV, SmartPort, Printer) build their title
at runtime and can't be reached. They float on first open and stay where the
user docks them.

Migration note: moving to docking necessarily replaces any previously saved
free-floating layout — the default is seeded once on the first docking run.

## 2026-07-27 — UI pass 1: opaque theme, DPI/zoom scaling, a status bar that says something

A design audit of the running UI turned up four things worth fixing before
any layout work.

**Panels were translucent over a running game.** POM2 ran on bare
`ImGui::StyleColorsDark()`, whose `WindowBg` sits at alpha 0.94. Invisible on
a black boot screen, unreadable over HGR: the CRT Settings sliders rendered on
top of Disk Library rows. New `Pom2Theme.{h,cpp}` owns the palette and makes
every background opaque, with rounded geometry and a phosphor accent (amber
default; P31 green / cold blue / slate, `ui_accent`).

The non-obvious part was the **surface ramp ordering**. First cut had
`PopupBg` and `FrameBg` at the same value, which left the zoom slider *inside
a menu* with no visible track — just a floating grab. Popups must sit below
frames on the ramp. See DEV § Theme.

**No DPI awareness at all** — font hardcoded at 14 px, no `ScaleAllSizes`,
nothing read from the windowing system, so the whole UI was microscopic on a
HiDPI display with no way to enlarge it. Now: monitor scale × a persisted user
zoom (View ▸ Interface, 75–250 %, `ui_scale`).

Two traps here. `ScaleAllSizes()` is **cumulative**, so `applyTheme()` rebuilds
the style from a pristine `ImGuiStyle` on every call — otherwise each nudge of
the zoom slider compounds the padding. And the DPI factor must come from
`ImGui_ImplGlfw_GetContentScaleForWindow()`, **not** `glfwGetWindowContentScale()`:
on macOS, Wayland, Emscripten and Android the framebuffer already carries the
scale, the backend helper returns 1.0f there, and querying GLFW ourselves
would have scaled those platforms twice. Caught before shipping by reading the
backend rather than by testing — the dev machine is 1× X11, where both agree.

**The status bar was three fields in `TextDisabled` grey.** It now carries the
drive LED + mounted image + track, the *achieved* clock, and a host caps-lock
badge — the three questions that previously required opening a panel. The
achieved clock is sampled from `Memory::getCycleCounter()` over ≥250 ms and is
resync-guarded: rewind, snapshot restore and profile switch all roll the
counter backwards, and an unsigned delta there would print a garbage MHz.

Its warn threshold is deliberately 10 %, not 5 %: a vsynced 60 Hz host running
a 50 Hz PAL profile lands ~4–5 % short of nominal from frame-pacing jitter
alone, and a 5 % band made a perfectly healthy machine flicker green/amber.

**Toolbar** — power-cycle (the only destructive control) is the only red
glyph; run/pause tracks the action in green/amber. The `"|"` text characters
between groups became real drawn rules (`pom2::verticalRule`), and the
hardcoded combo widths (86/90/110 px) became self-measuring — the new
`FramePadding` alone clipped "//e PAL" to "//e PA", and any zoom would have
done it again.

Not addressed, and the next real constraint: 33 free-floating panels with
absolute `imgui.ini` positions. Changing zoom mid-session doesn't move panels
placed at the old scale. Docking is the fix.

## 2026-07-27 — The IWM froze after a rewind on //c-class; MIG RAM was lost

`IWMDevice` was never serialized. It holds **eight absolute emuCycles
stamps** — `now_`, `lastSync_`, `nextStateChange_`, `syncUpdate_`,
`asyncUpdate_`, `revStart35_`, `fluxWriteStart_`, `delayDeadline_` — so a
rewind rolled the machine's `cycleCounter` backwards while the controller
kept its older, *larger* `lastSync_`. `sync()`'s `while (nextSync >
lastSync_)` walker then had nothing to do, and the IWM sat frozen until
emulated time climbed back to where it had been before the rewind.

Reachable on every //c-class profile, not just the 3.5" path: `ioReadIWM`
ticks the IWM on each `$C0E0-$C0EF` access **before** testing
`iwmAuthoritative_`, so even a //c+ booting 5.25" in shadow mode — where
the data itself comes from `DiskIICard` — advances it. The visible damage
was bounded because the shadow path supplies the bytes, which is why this
never showed up as a boot failure.

The //c+ **MIG** gate array had the same gap: its 2 KB `migRam_` and the
auto-incrementing `migPage_` pointer came back zeroed, so the alt firmware
read something other than what it had written.

Both now ride in a second, length-prefixed trailer on the `Memory` blob,
each section self-identifying by magic (`IWM1`, `MIG1`). Length prefixes
so a loader can skip a section it does not understand; a blob may carry
neither, either, or both. Older snapshots simply lack the trailer and keep
the live values — exactly the pre-fix behaviour, so nothing regresses on an
existing save. `MemoryProfile` grew a pair of no-op virtuals for this;
only the //c-class profile overrides them. The MIG page pointer is masked
to `0x7FF` on the way in rather than trusted, since `migRead` indexes
`migRam_[migPage_ + (offset & 0x1F)]`.

Pinned by `iwm_mig_snapshot`, checked against the unfixed code: with the
trailer read stubbed out, `testMemoryTrailerCarriesIwm` fails on the
restored device still sitting at cycle 0. The round-trip assertion
compares a **re-serialized** blob rather than the one public accessor, so
the private stamps are actually covered.

## 2026-07-27 — Rewind timeline read 4× long on //c+; five test harnesses revived

The rewind panel divided cycle spans by a hardcoded `1022727.0`. That is
the NTSC nominal, and the //c Plus carries a 4× Zip-style accelerator —
68180 cycles per 60 Hz frame, ~4.09 MHz. A 30-second ring displayed as
**"120.0 s"**, contradicting by a factor of four the "history (s)" slider
sitting immediately beside it, and the scrub readout was wrong by the same
factor. The conversion now asks the profile: `cyclesPerFrame × refreshHz`
is what the worker actually spends per wall-clock second, accelerator
included. The frames↔seconds conversions had the matching bug in the other
direction — a hardcoded `/60` and `*60` made the slider read 20 % short on
the 50 Hz PAL profiles.

Separately, `tests/{cpu_smoke,disasm_smoke,iic_dump,rom_basic,rom_boot}.cpp`
had been unbuildable since the sources moved into `src/`: each carried a
hand-written `g++ -I. tests/foo.cpp M6502.cpp Memory.cpp` line that no
longer resolved, and `Memory.cpp` has since grown a dozen dependencies.
They are now declared in `tests/CMakeLists.txt` as `EXCLUDE_FROM_ALL`
targets, so CMake supplies the dependency list and they cannot rot
silently again — but they stay outside ctest, because they print and
assert nothing. Their headers say so, and name the test that does gate the
same ground (`klaus_6502_functional` for `cpu_smoke`,
`system_profile_smoke` for `rom_boot`, and so on). Zero cost to normal
builds and CI.

## 2026-07-27 — Printer trace was silently truncated; Grappler DIP raced the CPU

Four fixes from a review pass over the printer work, none of them
user-visible until the moment they bite.

**The trace log always ended short.** `ImageWriter` opened the trace file
but never closed it: `stopTrace()` was reachable only from the panel's
checkbox, and the class had no destructor. Bytes already `fprintf`'d
survived — the C runtime flushes stdio at exit — but the hex row still
being assembled in `traceRow_` had never reached stdio at all, so up to
15 bytes vanished, and the file ended with no `# trace closed` footer.
That is worst exactly where the trace matters most: `POM2_TRACE_PRINTER=1`
is the path you use to capture a stream for a bug report, and it *always*
ended truncated, with nothing to distinguish a complete trace from one cut
short by a crash. The printer now has a destructor that closes the trace,
and — since it owns a `FILE*` — copy construction and assignment are
deleted rather than left to `fclose` the same handle twice. Pinned by
`testTraceClosedOnDestruction`, which was checked against the unfixed
code: without the destructor it fails on the missing row.

**The Grappler's printer-type DIP crossed threads unguarded.** The
ImageWriter panel lets you change the S1 switches while the guest runs, so
`setPrinterType` fires on the UI thread during ImGui rendering; the CPU
worker reads `dipType_` in `deviceSelectRead`. The reader holds
`stateMutex`, but the writer never did, so that mutex bought nothing. In
practice the worst case on any real target is one status poll seeing the
old switch position — harmless — but it is a data race, and `busy_` three
lines below was already `std::atomic` with a comment spelling out the very
same UI→CPU crossing. `dipType_` is now atomic too, and both members say
which thread touches them (`dipMsb_` stays plain: it is written at plug
time, before the card reaches the bus).

**The status-byte comment described the old behaviour.** It still claimed
`DIP = 000 = Epson series` while the code beside it returned the S1
switches, defaulting to `101` = Apple Dot Matrix — the whole point of the
change that introduced it. A misleading comment in a MAME-parity block is
worse than none: it is what someone debugging a DIP problem reads first.

**The stall watchdog stayed armed between jobs.** `stalledFor_` was reset
whenever the queue made progress, but not when the queue was emptied by
`flushPending()` ("Print now"), by `resetPrinterHard()`, or by draining to
zero. A job that had stalled left the counter loaded, so the *next* job's
first expensive byte could trip the watchdog immediately and be forced
through ahead of its schedule. One byte, a few milliseconds early, once —
never observable, but the reset now happens on all three paths.

## 2026-07-26 — The printer no longer freezes the Apple II by default

Printing still looked like a crash. Not a wedge this time — the *real
handshake* doing its job: a Print Shop page is tens of KB of dot columns,
the printer eats them at 250 cps, and the guest sits in its firmware ACK
loop for the whole job. Measured on the captured streams: 5.2 s for the
5 KB test page, 11.7 s for an 8.7 KB screen dump, and a full greeting card
is 10-20x that — minutes of an emulator that answers nothing.

That is exactly what a real Apple II did, and it is still available:
*Printer settings → "Make the Apple II wait for the printer"*. But it is
**off by default** now. The page still builds up line by line at the
printer's real speed — which was the point — while the guest carries on.
Realism that is indistinguishable from a hang needs to be asked for, not
inflicted.

A print in progress is also shown in the status bar (`printing 4312 B`,
plus `(Apple II waiting)` when the handshake is on), so it is never a
mystery pause with no explanation on screen.

## 2026-07-26 — Ribbon cartridge modelled; read-only disks say why

Two "why doesn't it…" answers turned into settings:

* **Ribbon**: `Four-colour` (default) / `Black` in *Printer settings*.
  There is no colour "mode" on an ImageWriter II — colour is the ribbon
  you install, and software asks for a band with `ESC K`. With the black
  cartridge fitted the printer still accepts `ESC K` and prints black,
  like the real one. Worth knowing: the guest has to ask, so Print Shop
  only produces colour when its Setup names "Apple Imagewriter II **(C)**".
* **Write-back**: the Disk II panel now states, on the mounted image,
  *why* it is read-only — "the image itself is write-protected (WOZ/2IMG
  flag)" vs "write-back is off". The default stays off (running a program
  must never silently rewrite a source image, and the drive reporting
  write-protect beats accepting writes and dropping them on eject), but
  nothing connected that default to the guest's "disk is write-protected"
  message — Print Shop refusing to save its own Setup read as an emulator
  bug for exactly that reason.

## 2026-07-26 — Print Shop prints in colour: line feed after CR is detected

Print Shop's test page came out as a coloured staircase — "Welcome to The
Print Shop" with each colour pass one line lower than the last. The trace
(and the raw stream, both new this round) showed why in one glance:

```
ESC T16 CR LF          ← advance one line
ESC K1 CR  ESC G0396…  ← yellow pass
ESC K3 CR  ESC G0442…  ← cyan pass    — bare CR: SAME line
ESC K2 CR  ESC G0326…  ← magenta pass — bare CR: SAME line
```

The colour passes are separated by a **bare CR** precisely so they
overprint. POM2 defaulted SW A-8 (line feed after CR) to ON — right for a
bare `PR#n : PRINT`, wrong for every real driver (which sends CR+LF and
then double-spaces), and destructive for a colour driver.

There is no static default that satisfies all three, so `AutoFeed::Auto`
now settles it from the stream: feed on CR until the guest sends its own
LF right after one, then swallow that LF and stop feeding. A plain BASIC
listing, a Grappler+ printout and Print Shop's colour page all come out
right with nothing to configure. The switch can still be pinned On/Off.

Worth keeping: the printer was never the bug in any of this. What made it
findable was making the *byte stream* visible — the trace log said `ESC K`
+ bare CR, and at that point the answer was in the manual.

## 2026-07-26 — The printer could wedge the Apple II; printer trace log

Print Shop froze the moment it printed. Not a crash — a deadlock the
pacing work had just introduced: `tick()` capped its banked mechanism
time at 1 s, but a form feed costs `(bottomMargin - curY) / 5 ips` = up
to 2.2 s of paper transport on a Letter sheet. That byte could never be
afforded, so the queue stalled *forever*; BUSY stayed asserted; and the
guest, which now correctly waits on the Grappler's ACK bit, spun in its
firmware loop with nothing to wait for. Any page eject did it.

Two fixes, because one of them should have made the other impossible:

1. The credit cap is now `max(kMaxCredit, cost of the head byte)`.
2. A watchdog forces any byte that has waited 10 s through regardless,
   and says so in the trace. A cost-model mistake must degrade to
   "printed late" — never to a hung guest. Wiring a device that can
   block the CPU means the host side needs a floor, not just a model.

Also this round:

* **Trace log**, since "it prints nothing" and "it prints noise" are both
  protocol questions. *Printer settings → Log the printer stream to a
  file* (or `POM2_TRACE_PRINTER=1`) writes the byte stream as a hex dump
  interleaved with the decoded escape sequences, bit-image setup, page
  ejects, BUSY transitions and queue depth.
* **"Follow" now tracks the last inked sheet.** After a form feed the
  panel was showing the fresh blank sheet under the head while the
  printed one sat on the stack one click to the left — which is why a
  job that printed correctly still looked like it had printed nothing.

Field notes from driving the real *New Print Shop* (WOZ) headless, worth
keeping: its Setup carries **Printer = Apple Imagewriter II (C)** but
**Interface Card = Built-in** — the //c/IIgs on-board port, which sends
nothing at all to a card in a slot. And its program disk is
write-protected, so a corrected setup can't be saved (hence its own
"current Setup was done on a different computer" warning at every boot).
Neither is an emulation bug; both look exactly like "the printer doesn't
work".

## 2026-07-26 — Grappler+ was talking Epson to an ImageWriter

Printouts came out as pages of meaningless characters in double-width,
with the ribbon colour flipping — while the panel showed a moving head
and a healthy byte count. The printer parser was not the problem: it was
audited case-by-case against the original (`david-schmidt/gsport`
`src/imagewriter.cpp`, 84/84 commands present, same framing, same
parameter counts). The card was.

The Grappler+'s S1 DIP block tells its firmware which printer is on the
cable, and POM2 reported MAME's default: **Epson**. Captured from the real
4 KB dump, the same `^I G` screen dump emits:

```
S1=000 Epson      ESC A <07>  …  ESC K <18><01> + binary graphics
S1=001 C. Itoh    ESC T14     …  ESC S0280      + graphics
S1=101 Apple DMP  ESC T14     …  ESC G0280      + graphics
```

An ImageWriter II parses the Epson stream as "1/6 in spacing", "select
ribbon colour $18", then prints every graphics byte as a glyph — 32 sheets
of noise, exactly what the panel was showing. POM2's printer *is* an
ImageWriter, so S1 now defaults to Apple Dot Matrix (101) instead of
Epson, is settable as *Card emulates* in the ImageWriter panel (persisted,
with a warning when it is set to a dialect this printer can't read), and
the S1:1 MSB switch masks bit 7 at the latch like MAME's `data_latched`.
With that, an HGR `^I G` dump renders as the picture.

MAME is right to default to Epson — it wires the card to a generic
centronics printer. The default is only wrong once you know what is on
the other end of the cable, which POM2 does.

## 2026-07-26 — ImageWriter prints at printer speed; Grappler+ ACK handshake

The printer worked but printed *instantly*: the card spools a page in a
millisecond of emulated time and the whole sheet appeared in one frame.
`ImageWriter::queueBytes()` + `tick(dt)` now model the mechanism — bytes
wait in the printer's input buffer and land as the head reaches them, at
Apple's published rates (250 cps draft / 45 cps NLQ, *ImageWriter II
Owner's Manual*). Speed is a *Printer settings* combo (`Instant` keeps
the old behaviour); the status line shows the queue and a "Print now"
escape hatch. Pacing runs off the host frame time, not `emuCycles`: the
paper keeps moving while the guest is paused, turbo'd or rewound.

Three things worth keeping:

1. **The firmware waits on ACK, not BUSY.** Holding BUSY (bit 3) high did
   nothing — the guest printed straight through it. The genuine Grappler+
   dump spins on bit 0 instead (`$CD89 JSR $CDE1 / AND #$01 / BEQ $CD89`),
   so a full 2 KB printer buffer has to read back as *not acknowledged*
   (`GrapplerCard::ackEffective()`). With that wired, a long print job
   blocks the Apple II in its firmware loop while the paper catches up —
   which is what "printing" felt like in 1985.
2. **Draft is bidirectional.** Charging every `CR` a full carriage-return
   slew halved the throughput (3.4 lines/s instead of ~6). Draft prints on
   the return sweep; only NLQ pays the slew.
3. **A printer card in slot 3 of a //e is broken by design.** The internal
   80-column firmware keeps `OURCH`/`OURCV` in the slot-3 screen holes,
   which is where printer firmware keeps its column/line counters — so the
   Grappler reads the cursor position back as its line width and emits
   `CR LF` after *every character*. Reproduced against the real ROM dump
   on `apple2e.rom` (slots 1/2/4/5/7 are clean, and II+ slot 3 is clean).
   Faithful, not a bug: Slot Config now warns and points at slot 1.

Also fixed: `grapplerCard` / `echoPlusTmsCard` were the only non-owning
card pointers the two slot-teardown paths forgot to null, so unplugging a
Grappler+ or switching profiles left `pumpImageWriter()` dereferencing a
freed card every frame.

## 2026-07-26 — Apple ImageWriter II printer + paper-tray window

POM2 could capture printer output but only as a text spool: `PR#1` gave
you bytes, not a page. Anything that printed *graphics* — screen dumps,
Print Shop, the Grappler's `^I G` dump — spooled a pile of escape codes
and looked like garbage. `ImageWriter` (`ImageWriter.h/.cpp`) interprets
those bytes and paints the page; `ImageWriter_ImGui` shows it, with page
navigation, zoom and PNG export (*Devices → ImageWriter II (printout)*).

**Ported from greg-kennedy/ImageWriter** (`imagewriter.cpp`, the GSport /
KEGS / DOSBox lineage), which was written against Apple's ImageWriter II
and LQ reference manuals. Command dispatch, soft switches, density tables
and the ribbon encoding are line-for-line, with the reference's line
ranges cited in the code. Full detail → DEV § ImageWriter.

Non-obvious decisions worth keeping:

1. **The printer is not a card.** It has no catalog key and no slot. The
   Apple II talks to a *printer interface card*; `pumpImageWriter()`
   streams that card's spool into the printer once per frame via a new
   `drainSpoolFrom(consumed, out)` on `PrinterCard` / `GrapplerCard`.
   Modelling it as a slot card would have made "Grappler+ *and* an
   ImageWriter" unrepresentable, which is the normal 1985 desk.
2. **Auto line-feed defaults ON.** The Apple II's `COUT` emits a bare CR
   (`$8D`) and never an LF. With the ImageWriter's SW A-8 open — the
   reference's default — every printout overprints a single line into an
   unreadable smear. This was caught end-to-end (real `apple2p.rom`,
   `PR#1`, real spool), not by unit test; the first rendered page was one
   black stripe. Exposed as a checkbox because CR+LF drivers need it off.
3. **No FreeType, no SDL.** The reference needs both; POM2 links neither.
   Glyphs come from the repo's own 8×8 CP437 font
   (`hgrpaint::kBBFontCp437`), which is *closer* to the hardware — an
   ImageWriter draft cell really is 8 dots wide at the pitch's density and
   8 pins tall at 1/72 in, so text and graphics share one dot plotter.
4. **Dots are the page-pixel interval they cover**, replacing the
   reference's `pixsize` heuristic and its "Primative scaling function"
   fudge (`imagewriter.cpp:1556-1573`) — that produced seams and randomly
   doubled columns at page DPIs that aren't integer multiples of the
   graphics density. Adjacent dots now abut at any DPI.
5. **Ribbon colour is subtractive by construction.** The page is indexed
   `yyyxxxxx` (5-bit intensity + 3-bit band) with bands assigned so that
   OR-ing inks mixes them: magenta|yellow = red, cyan|yellow = green, all
   three = black. Overprinting is a plain `|=`, no blend maths.
6. **The completed-sheet stack is capped** at 32 with older sheets rolled
   off and counted. A guest that form-feeds in a loop would otherwise eat
   ~2 MB of host RAM per sheet forever.

Two deliberate bug-fixes against the reference: `resetPrinter()` leaves
bold off (the reference sets `STYLE_BOLD` to fatten a thin TrueType face —
on a dot-matrix cell it just smears), and parameter space-normalisation
touches only digit positions, so `ESC R nnn ' '` repeats a space instead
of printing zeros.

Not modelled: user-defined character sets (absent from the reference too)
and `ESC ?` "send ID string" (no printer→computer back-channel). An
ImageWriter on the Super Serial Card — the //c's real printer port — needs
a host-visible TX spool on the SSC first; only the parallel cards feed the
printer today.

Pinned: `imagewriter_smoke` (paper geometry, bit-7 strip, CR/LF + spacing,
`ESC K` overprint + palette, `ESC G`/`ESC C` bit images, `ESC R` framing,
form feed + page cap, RGBA export, and the spool→printer streaming seam
including resync after "Clear spool").

## 2026-07-12 — SoftCard/Z80 bug hunt: 6 confirmed, 6 fixed, 1 refuted-as-faithful

Adversarial review (8 finder angles + per-candidate verification) over the
day's Z80/SoftCard/DMA work. Fixed same-day:

1. **WASM link break** — `runCpuSlice` was defined inside the
   `#ifndef __EMSCRIPTEN__` worker block while the unguarded `tickFrame`
   (the browser-RAF CPU driver) calls it. Moved above the guard.
2. **Z80 `IN r,(C)` MEMPTR** — WZ was computed *after* the register
   write, so `IN B,(C)`/`IN C,(C)` latched WZ from the modified BC. Port
   address captured first now. zex can't catch this class (no I/O).
3. **Unbounded DD/FD chains** — a whole prefix run used to fold into ONE
   `Z80::step()` (a crashed guest in a $DD/$FD sea → one giant
   advanceCycles lump under stateMutex; wrapped 64 K of prefixes → host
   hang). Each prefix now retires as its own 4-T step with interrupts
   deferred to the opcode (`State::pendingPrefix`, faithful to silicon);
   SoftCard blob bumped to `SFZ2`. `Z80::run()` (dead, misleading
   contract) deleted.
4. **Snapshot-load during CP/M** — file snapshots carry no SLOTn
   sections, so restoring left a live SoftCard `enabled_` and the stale
   Z80 executed over the restored RAM. `restoreMachineState` now
   force-disarms DMA claimants first (rewind, which captures slots,
   already round-tripped correctly).
5. **Step verbs during CP/M** — debugger/CLI/AI single-step always
   stepped the parked 6502, running its post-hand-back continuation
   early (a DMA-halted CPU executes nothing). New
   `EmulationController::stepBusMaster` steps the claimant's Z80
   (`dmaRun(1)` = one instruction) instead.
6. **POM2_TRACE_HANG false positive** — the detector samples the 6502 PC
   per frame; under Z80 ownership that PC is legitimately frozen →
   guaranteed "HANG DETECTED" spam on healthy CP/M. Sampling now skips
   (and resets the ring) while a DMA claimant owns the bus.

Refuted with evidence, kept as-designed (documented in DEV § SoftCard):
Z80 writes to $C007 via the $E000 window wedging a IIe until RESET is
what real MMU silicon does (UTAIIe 5-28) — MAME's write-through there is
its own simplification. Cleanup backlog (ByteIO for the SFZ2 blob,
findResource/textRowAddress reuse in the boot test, rp-selector/ccTest
dedup in the Z80 decoder, xlate LUT) → TODO [Arch].

## 2026-07-12 — CP/M 2.2 boots to A> (SoftCard Phase 3 — plan complete)

Microsoft CP/M 2.2 now boots end-to-end: Disk II loads the system tracks,
the 6502 loader finds the SoftCard by its $CnXX write probe, the Z80 runs
CCP/BDOS/BIOS out of the six translated windows, and a live `A>` prompt
lands on the text page in ~11 M cycles. Two media-gated ctest gates:
`softcard_cpm_boot` (II+, 44K v2.20 1980 master, 40-col) and
`softcard_cpm_boot_iie` (//e, 60K v2.23, 80-col) — the latter verified
against the MAME `apple2ee -sl4 softcard` oracle with a byte-identical
banner.

The bring-up lesson: every "failure" was a **sysgen/machine mismatch,
not an emulation bug**. The 56K/60K sysgens require the IIe-class
console — on a II+ they paint $00s (MAME does the same), and their
output goes through the IIe 80-col firmware, which stores even display
columns in AUX $0400: a main-RAM screen scrape shows every second
character missing ("Sfcr PM" for "Softcard CP/M") and reads like a Z80
bug until the aux page is interleaved in. The 44K 2.20 master is the
correct II+ image. Don't re-diagnose this; check the sysgen first.

## 2026-07-12 — Microsoft SoftCard card + dual-CPU DMA arbitration (CP/M Phase 2)

`SoftCardZ80` (catalog `softcard`) ports MAME `a2bus/a2softcard.cpp`. Two
findings corrected the Phase-1 plan the moment the MAME source was read —
worth remembering because both were "obvious" wrong guesses:

- The bus toggle is a **write to $CnXX** (the slot-ROM window), not a
  DEVSEL $C0nX access, and reads never toggle.
- The Z80→6502 translation is **six windows, not `+$1000` with wrap**:
  the Z80's $B000-$DFFF lands on the Language Card ($D000-$FFFF), $E000
  reaches the I/O page (that's how the Z80 releases the bus itself), and
  $F000 wraps to the zero page. CP/M's 60K layout only exists because of
  the LC remap.

Arbitration is a generic DMA daisy-chain hook (`SlotPeripheral::
dmaActive/dmaRun`, `SlotBus::dmaClaimant`, `EmulationController::
runCpuSlice`) rather than SoftCard special-casing — MAME's a2bus has the
same abstraction, and a future Applicard reuses it as-is. Hand-over is
instruction-precise both ways (the granting STA calls `M6502::stop()`,
whose `run()` re-arms on the next call — the same yield WAI/STP uses; the
chunk remainder goes to the other CPU). The Z80's 2× clock is converted
2 T-states → 1 cycle with an odd-T carry so `emuCycles` stays in the 6502
domain — video/LSS/audio never learn a second CPU exists. Pinned by
`softcard_toggle` incl. a full tickFrame 6502→Z80→6502 round trip.

## 2026-07-12 — Z80 core lands (SoftCard/CP/M Phase 1), zexdoc+zexall 100 %

First deliverable of the Microsoft SoftCard + CP/M plan: a standalone
`pom2::Z80` core (`src/Z80.h/.cpp`), bus-abstracted behind `Z80Bus` so it
links with zero Apple II sources — the SlotPeripheral card and the
dual-CPU arbitration come in Phase 2 (see TODO [Cards]). Full opcode
coverage including the undocumented surface (IXH/IXL, DD CB write-back,
SLL, X/Y flags, MEMPTR/WZ), IM 0/1/2, NMI, EI shadow, documented T-state
totals. Pinned by `z80_core` (committed smoke) + `z80_zexdoc`/`z80_zexall`
(configure-time downloads, SHA-256 pinned, Klaus pattern) — both
exercisers pass 100 %.

The pitfall worth remembering: **zexdoc green ≠ core correct**. zexdoc
masks the undocumented X/Y flags, and the one bug the first full run
surfaced was the `BIT n,r` X/Y rule — X/Y copy bits 3/5 of the *full
tested register*, not of the masked single-bit result (and for
`BIT n,(HL)` they come from WZ's high byte, which is why the core carries
a real MEMPTR). Only zexall's silicon-captured CRCs catch this class;
any future Z80 touch-up must keep both exercisers in the gate.

## 2026-07-12 — OE composite bug hunt: GPU/CPU demod knob parity

Bug hunt on the OpenEmulator composite pipeline, GPU shader vs CPU demod.
The demod *math* checked out OE-exact (kernels recomputed from libemulation's
`chebyshevWindow × lanczosWindow` realIDFT recipe to ≤5e-6; Y'UV matrix and
sin→U / cos→V / PAL-flips-V conventions match `OpenGLCanvas.cpp` line for
line) — every real bug was in *which path applies which knob*:

- **Hue / Sharpness / PAL / Sharp-text were GPU-only.**
  `renderCompositeOeCpu` ignored all four `NtscParams` demod knobs, yet
  MainWindow still *neutralised* hue+sharpness in the CrtEffectStack pass on
  the OE-CPU branch ("the demod already applied them" — only true for the
  GPU). Net effect: the sliders were silently dead in OE-CPU mode, and —
  worse — **popped off on OE-GPU mixed frames**, whose graphics band is CPU-
  demodulated (`mixedCompositeUsesFramebuffer`): entering a splitscreen
  (game score band, BASIC) visibly dropped the user's hue/PAL. Fixed by
  mirroring the live knobs into the display each frame
  (`Apple2Display::setOeDemodParams`) and implementing hue rotation, the
  soft↔sharp chroma-kernel blend and PAL V-sign alternation in the CPU
  demod — same formulas as the GLSL, pinned pixel-identical (maxDelta 0) by
  the extended `oe_demod_gpu_cpu_parity` (now also covers
  hue+sharpness+PAL engaged, and demodulated TEXT).
- **AI `/screen` lied in OE-GPU mode.** `pixels()` returns the LUT fallback
  framebuffer there (the composite image only exists in a GL texture), so
  agent screenshots showed ColorNTSC colours, not what's on screen. New
  `Apple2Display::demodCompositeForCapture()` schedules the pixel-identical
  CPU demod after the server's render; no-op in every other mode.
- **One-frame present race.** `drawScreenImage` re-polled
  `Memory::getDisplayState()` (the CPU worker may have advanced past the
  rendered frame) to route sharp-text/demod presentation; a text↔graphics
  switch in that window flashed one LUT frame. It now reads
  `Apple2Display::lastRenderState()` — the snapshot render() actually used.
- **Golden rebaseline (15 hashes).** `display_golden_hash` was red at HEAD:
  the intentional mono lo-res dot-pattern rendering (previous entry) landed
  without re-recording the `lores/dlgr × mono*` hashes, which still pinned
  the pre-fix "mono lo-res ≡ colour hash" behaviour. Re-recorded; diff
  audited to be exactly those 15 entries.
- **Parity contract documented.** The demod now exists in three deliberate
  copies (GLSL, `renderCompositeOeCpu`, the test's re-simulation); each site
  now carries a cross-pin comment naming the other two, since an edit to one
  alone is invisible to CI until the parity test is also updated.

## 2026-07-12 — paint-editor crumbs: mono lo-res, composite canvas, DHGR sprites

The three leftovers from the 17-item batch. 133 tests (pins folded in).

- **Mono lo-res rendering** (`renderLoRes` / `renderLoResDouble`). On a mono
  monitor a lo-res nibble is NOT a grey: the colour generator keeps cycling
  at 14.318 MHz, so a block displays as its repeating 4-bit dot pattern —
  the same serialisation the composite-signal path already used
  (`fillCompositeSignal`, absolute-sample indexed). GR's 280-wide pixels
  average their two samples (grey 5 → uniform 127); DLGR renders per-dot at
  560 (grey 5 → fine stripes), aux rotation included. Standard
  max(target, prev × decay) phosphor rule; `Phosphor`/`phosphorFor` moved
  above the lo-res painters. Pinned in `dhgr_paint_model`.
- **Composite canvas pipelines.** "AppleWin NTSC (composite)" and "OE
  composite (CPU)" join the paint editor's pipeline combo (scratch display
  in AppleWin *Monitor* sub-mode — Tv's frame blend would smear a static
  canvas). This is what makes the DHGR NTSC-8-px import previewable
  faithfully: its 86 colours only exist after composite demodulation, and
  the import preview now points at the pipeline combo instead of
  apologising. The 560-wide composite output of the 280 modes reuses the
  existing pair-averaging width adapter.
- **Sprite editor DHGR target** (gated on `supportsDhgr()`). The editing
  canvas stays the mono shape; a "DHGR target" toggle re-aims Stamp / Grab /
  colour preview / ca65 export at the DHGR page: lit pixels plot as 140-px
  colour pixels in a picked 16-colour hue (transparent background,
  plane-aware pokes), Grab lifts shape + dominant hue back, and the ASM
  export emits `name_aux` / `name_main` byte-pair tables (blit at an
  aligned byte-column pair). Placement is pixel-granular (`Px X`) — DHGR
  pixels are nibble-aligned, so there is no ×2-style parity honesty problem.

## 2026-07-12 — paint-editor batch: 17 items (tools, DLGR, NTSC 8-px, sprites)

The full improvement backlog proposed after the 560-dot import landed in one
wave. 133 tests (DLGR + NTSC-8 pins folded into the two existing paint tests).
Highlights and the non-obvious bits:

- **GR/DLGR screen holes masked.** Bulk editor ops (clear/import/load) used
  to write the text-page screen holes ($x78-$x7F per 128-byte group) on the
  LIVE machine — peripheral firmware (SmartPort, mouse) scratches there, so
  a Clear Page could corrupt a running driver. Text-page loads now go
  byte-wise through pokes with holes skipped (both planes in DLGR).
- **HGR import now scores LUT row 0.** Closes yesterday's ~22 % divergence:
  the importer's decode is pinned byte-identical to `renderHiRes` ColorNTSC
  in `dhgr_convert`. POM1 parity for this file is deliberately dropped
  (comment documents it).
- **DLGR mode.** 80×48 blocks over the aux+main text pages; the aux nibble
  displays ROTATED LEFT one bit (MAME renderLoResDouble), so the model
  stores rotr4(colour) in aux nibbles — pinned against the real renderer +
  a lo-res palette cross-pin in `dhgr_paint_model`. Editor paints it in a
  560-dot logical space (7 dots per block), pxScreenW()=0.5.
- **DHGR NTSC 8-px import** (`imageToDhgrPage560Ntsc`): scores against the
  86-colour trailing-8-dot palette from ii-pix (`DhgrNtsc8Palette.cpp`,
  BSD-2-Clause data, table anchors pinned). The model is CAUSAL — no right
  context, no guessed neighbour, no refinement pass needed. The extra
  colours only appear on composite viewing targets (OE/AppleWin modes);
  the import preview warns that the MAME-LUT canvas under-sells it.
- **Save to ProDOS**: `buildVolumeFromFolder` now parses CiderPress-style
  `NAME#TTAAAA` filename tags into file_type/aux_type (pinned in
  `prodos_volume_smoke`), and the paint editor's file browser homes to
  `prodos_folder/` — a default "PIC#062000" save is BLOAD-able by name at
  $2000 after the next host-folder mount.
- **Tools**: 16-colour copy/paste (mode-tagged clip + FlipH/FlipV/Rot90),
  MacPaint 8×8 fill patterns (page-anchored so strokes tile seamlessly;
  patterned fills over the same colour are allowed), X/Y mirror symmetry
  (applyPlot level — region ops deliberately exempt), DHGR text, onion-skin
  tracing overlay (fit/crop-aware placement), page 1↔2 flipbook + ghost
  (double-buffer animation authoring), canvas pipeline selector
  (NTSC/Medium/4-bit/Chat Mauve — ChatMauve's 560-wide HGR output is
  pair-averaged down to the 280 canvas), 4:3 aspect, DHGR fringing overlay,
  session persistence, and the POM1 sprite editor (`hgrsprite/`) wired to
  the same host seam.

## 2026-07-12 — DHGR import upgraded to the true 560-dot model

Follow-up to the paint-editor port after comparing against ii-pix and
bmp2dhr: the 140-px block quantiser is bmp2dhr's model, which ii-pix
dropped as "only useful to show why this is not the right approach to
DHGR". 133 tests (one new).

- **`imageToDhgrPage560`** — ii-pix's "4-pixel colour" model with the HGR
  converter's proven architecture: per-byte-column (7 dots, 128
  candidates) branch-and-bound analysis-by-synthesis, warm-started per
  row, in-candidate linear-RGB error walk scored in CAM16-UCS via the
  local Jacobian, then monotone cross-column ICM refinement with dirty
  tracking. Exploits the full 560-dot resolution and pilots the colour
  fringing the block model suffers blindly. ~180 ms/photo (vs 4 ms for
  140-px) — live-slider friendly. Default; the block model stays as the
  "140 px blocks (Dazzle Draw)" combo choice (instant, retouch-friendly).
- **Optimisation target = canvas, bit-exact.** The candidate renderer is a
  copy of POM2's ColorNTSC DHGR decode (Apple2VideoDecode LUT row 0 +
  rotl4b(absX+1)), pinned byte-identical to `renderDhgr` in the new
  `dhgr_convert` test — something ii-pix can't do for a specific emulator
  (it scores against colour-picker-measured palettes). Also pinned: exact
  solid fields with dither off, tone conservation dithered (mean linear
  RGB within 0.06 — full-strength diffusion turns the unavoidable
  black-context edge artifact into a never-decaying ripple, an inherent
  error-diffusion property, so exactness there would be a wrong pin), and
  refinement monotonicity.
- **LUT archaeology.** The ~22 % HGR importer/canvas divergence documented
  yesterday has a root cause: POM1's GraphicsCard NTSC LUT is MAME's
  medium-color **row 1**; POM2's ColorNTSC is **row 0**. HGR keeps row 1
  verbatim (POM1 parity); the DHGR converter carries row 0.
- **Resampler pixel aspect.** `resampleToLinearRgb` gained a
  `pixelAspect` parameter (default 1.0 — every existing caller unchanged);
  the 560-dot path passes 0.5 so fit/letterbox is computed in visual space
  (a square source fills the width instead of coming out 2× too narrow).

## 2026-07-12 — HGR Paint editor ported from POM1 + DHGR mode

The portable `hgrpaint/` module (MacPaint-style editor + ii-pix-style
image importer with CAM16-UCS perceptual dithering) is copied verbatim
from POM1 and driven by a new `Pom2HgrPaintHost` (Tools → HGR Paint
Editor). 132 tests (one new).

- **Offscreen canvas trick.** The host renders the editor page through a
  private, never-clocked IIe `Memory` + `Apple2Display` pair: the scratch's
  cycle counter never advances, so its video-event log never publishes and
  `render()` always takes the fast single-state path — the canvas is
  pixel-identical to the live screen with zero beam-racing interaction.
- **Pokes bypass IIe paging on purpose.** `writeRamUnchecked` (main) / raw
  aux bank (DHGR) instead of `memWrite`, so live 80STORE/RAMWRT state can't
  silently reroute the editor's writes to the wrong plane.
- **DHGR mode (POM2-only module extension).** New DHGR/DHGR2 pages (gated
  on `host->supportsDhgr()`): 140×192 16-colour aligned block model over
  the aux+main pair, 16 KB A2FC load/save, DHGR image import (CAM16-UCS +
  error diffusion at native resolution). The nibble↔colour law
  (`colour = rotl4(nibble,1)`) was derived from MAME's square-filter
  decode and is pinned by `dhgr_paint_model` against the real
  `renderDhgr` in two colour modes + a lo-res palette cross-pin — so an
  Apple2Display palette/decode change that would desync the paint model
  fails CI, not the user's picture.
- **stb gotcha.** MainWindow's `stb_image` impl is `STB_IMAGE_STATIC`
  (TU-local); the host TU compiles the only exported stb_image +
  stb_image_write implementations, which `HgrImageDecode.cpp` links
  against. Don't add a second non-static impl.

## 2026-07-12 — wave 4 (deferred fixes cleared + real Liron implemented)

The four items wave 3 deferred, plus the Liron follow-through now that
the real ROM is public. 131 tests (one new).

- **OE-CPU demod out of `stateMutex`.** The 17-tap × 560×192 FP demod
  (~1-2 ms) ran inside the lock every UI frame, stalling the CPU worker
  (read as emulation/audio jitter, worse under disk-turbo). `render()`
  now defers it (`pendingCpuDemodRows_`) and MainWindow runs
  `finishPendingCpuDemod()` after releasing the lock — it consumes only
  display-owned buffers. The mixed text band is patched under the lock
  (it reads guest RAM) and the per-row FIR only rewrites rows [0,160) in
  mixed mode, so the patch survives; `pixels()` finishes lazily so every
  render→pixels consumer (tests, screenshots) stays correct.
- **Frame-wrap video-event off-by-one.** An instruction straddling the
  17030/20280-cycle video-frame boundary stamped its soft-switch event
  past the boundary but publication ran after the instruction — the event
  closed into frame N and applied the switch one frame early. Stamps are
  non-decreasing, so boundary-crossers form the log's tail: they now
  carry into the new recording frame. Pinned by a real-6502 straddle case
  in `video_event_publish` (STA $C055 started 2 cycles before the
  boundary).
- **SmartPort STATUS pre-flights.** $CnC0 now returns SEC+$28 on no
  media and SEC+$2B on write-protect before handing back the block count
  (TRM driver conventions) — formatters that pre-flight STATUS used to
  get CLC on an empty/WP bay. Exactly 32 bytes, fills $CnC0-$CnDF.
- **Golden table: 112 → 164 pins.** New hash-frozen scenes: FLASH-on
  phase (16 emu-frames parked), text/HGR PAGE2, HGR 80STORE+PAGE2
  (asserted **equal** to the page-1 hash — the Sather 5.10 gate),
  HGR+AN3 rev-0 bit-7 mask (would have caught wave 3's paintHgr bug),
  80COL+HIRES+MIXED without DHGR (upscale path), and the Chat Mauve
  BW560/Mixed/Chunky160/Duochrome sub-modes. All 112 pre-existing hashes
  unchanged. Deliberate gap kept: mousetext/char-ROM glyphs need a user
  ROM; PAL beam-raced splits stay behavioural.
- **Real Liron controller ROM in `roms/liron.rom` + SmartPort-protocol
  dispatch.** The BMOW dump (4 KB, SHA1 fa94ecc2…, per-slot $Cn00 pages
  at slot×256 + $C800 bank at 2048) now ships in roms/; on slot-having
  machines SmartPortCard re-bases its page on the real dump — authentic
  identity $Cn07=$00/$CnFB=$00/$CnFE=$BF/$CnFF=$0A — with the HLE
  entries overlaid (the real IWM/UniDisk code can't run without the
  drive-side 65C02; never loaded on //c-class, see
  project_iic_smartport_boot). And $Cn0D is no longer fail-closed: a
  168-byte 6502 handler in the $C800 bank implements the real SmartPort
  call convention (inline cmd + param-list pointer, RA+3, ZP $42-$45
  saved) against a C++ engine — STATUS incl. unit-0 controller status
  and the 25-byte DIB, READ/WRITE (through the legacy commit machinery),
  FORMAT/CONTROL/INIT, real error codes ($01/$04/$21/$27/$28/$2B/$2D/
  $2F). Pinned by `liron_smartport_dispatch`: the full matrix executed
  by a real 6502, in both synthetic and real-ROM identity passes.

## 2026-07-12 — wave 3 (graphics system + Liron SmartPort audit)

Targeted hunts: display decode / composite pipelines / voxel & Chat Mauve
(three review agents), and the Liron-class SmartPort card audited against
primary sources (Apple Tech Notes, ProDOS 8 TRM, and the REAL Liron
firmware — see below). ASan+UBSan build of the full suite: clean (and it
exposed a real link bug: `test_ai_control_server` was missing
`MouseCardAppleWin.cpp`; Release builds optimized the dynamic_cast typeinfo
away, sanitizer builds didn't).

**Headline discovery: the Liron controller ROM is publicly dumped.**
BMOW/Yellowstone published `LIRONALL.bin` (4 KB) + full disassembly in
2018-2019; MAME's "WANTED — never dumped" listing (which CLAUDE.md echoed
as "cannot be implemented regardless of effort") is stale — MAME just
never ingested it. Verified byte-level: `$Cn07=$00`, `$CnFE=$BF`,
`$CnFF=$0A` → ProDOS entry fixed at `$Cn0A` (independently confirms the
DIX `JSR $C50A` fix), SmartPort dispatch at `$Cn0D`. CLAUDE.md corrected.

Display/composite fixes:
- **FLASH / phosphor / Tv-blur paced by the host monitor (medium).**
  `frameCounter` was ++ per render() call — the UI renders at vsync, so a
  120/144 Hz panel blinked FLASH 2-2.4× too fast, decayed MonoAmber
  afterglow 2.4× faster, and collapsed the AppleWin Tv 50 % blend; even at
  60 Hz the PAL profiles flashed at the NTSC rate. Now derived from the
  emulated frame index (`cycleCounter / 65·scanlines`, standard-aware) —
  exactly MAME's `frame_number() & 0x10`; decay is raised to the
  elapsed-emu-frames power and the Tv stash only advances with the
  machine. `hgr_render_smoke` now pins the invariant both ways (decay on
  frame step, NO decay on same-frame re-render).
- **CRT glass vanished on mixed/sharp-text/fallback frames in OE-GPU mode
  (medium-high).** The effect-stack gate only covered the pure GPU-demod
  and OE-CPU branches; a mixed graphics+text frame (score bands, BASIC)
  presents the CPU-rendered framebuffer and got NO scanlines/mask/
  persistence — effects flickered off/on during gameplay with a stale-
  persistence ghost on re-entry. Gate is now `oeFamily && presentTex ==
  screenTexture` — every OE path that still presents the raw framebuffer
  gets the glass.
- **Composite HGR dropped the IIe rev-0 DHIRES bit-7 mask (low-med).**
  `paintHgr` hardcoded `bit7Mask=0xFF` while the RGBA twin honors
  `state.dhgr ? 0x7F : 0xFF` (MAME `bit7_mask`): with AN3 on + 80COL off,
  the composite modes rendered the half-dot delay the LUT modes correctly
  suppressed — two POM2 outputs disagreed on the same frame.
- **Settings hardening.** NTSC/CRT floats + voxel tunables now clamped to
  slider ranges on load, and all 17 sliders got `AlwaysClamp` (a
  hand-edited `ntsc_center_lighting=0` was `1/x` → `exp(-inf)` → fully
  black screen in every glass mode, surviving restarts).
- **Chat Mauve rewind/snapshot hooks** ('CM' blob: FIFO/mode/AN3/80COL
  latches) — rewinding past a BW560/Mixed switch kept rendering the later
  mode until the guest re-clocked. Panel slot label unhardcoded (any
  slot 1-7 on //e). Voxel shader program no longer leaked at shutdown
  (via new `pom2::deleteShaderProgram`).

Liron/SmartPort fixes (spec citations in the audit, `$` = ProDOS codes):
- **READ/WRITE on an empty bay silently "succeeded" (medium ×2).** A read
  streamed a $FF buffer with CLC — ProDOS ONLINE saw a garbage volume and
  `PR#5` with no media booted 512 bytes of $FF and jumped into them; a
  write dropped 512 bytes. Both now latch the I/O error → carry-set.
- **`iicSmartPortArmed_` serialized (medium)** as a backwards-compatible
  MEX trailer byte — a rewind-ring entry captured after a //c HDV/3.5"
  boot restored with the $C500 stub swapped back to real //c firmware
  under a live ProDOS (next MLI call executed unrelated ROM bytes).
- **SmartPortCard transfer state serialized (medium-low)** ('SP' blob:
  unit/block/stream offset/half-filled write buffer/error latches) — a
  rewind landing mid-512-byte stream desynced the transfer.
- **//c arming scoped + fallback leak (low-med).** `bootFromSlot` armed
  the on-board SmartPort for ANY slot (a slot-6 5.25" Library boot with
  SmartPort media re-created the dual-device "garbled banner" scenario)
  and the no-signature fallback returned without disarming.
- **Capability byte honest:** `$CnFE` $13→$17 (write bit was missing —
  capability-inspecting utilities saw a read-only device). **`$Cn0D`
  fails closed** (SmartPort-convention callers used to fall through NOP
  padding INTO the boot routine). **Bad-command error** now $27 (real
  driver code) instead of the invented $01 Filer surfaced on FORMAT.
- **Host read-only images mount write-protected** (Block512Backing +
  Disk35Image probe writability at load) — write-back on a chmod-read-only
  .hdv used to accept a whole session of writes and lose them at flush.

Verified clean (highlights): text/HGR/DHGR interleave + `frameCycleToPos`
beam math both standards (13/13 display tests), GL lifecycle/resize/
GLES-WASM in both composite paths, Mat4/camera (pinned by `voxel3d_math`),
Chat Mauve FIFO edge model, SmartPort dispatch offsets against the real
ROM's conventions, `$Cn0A` DIX path. Deferred with TODO entries: OE-CPU
demod under `stateMutex` (~1-2 ms/frame of worker stall), frame-wrap
video-event off-by-one (≤7 cycles exposure), golden coverage gaps,
SmartPort STATUS pre-flight semantics.

## 2026-07-12 — wave 2 (seams: stale rewind ring, snapshot-load audio, kiosk K leak)

Adversarial re-review of wave 1's fixes + a sweep of the cross-subsystem
seams (snapshot/rewind × profiles, AI server × kiosk, CLI boot × slot
config). Wave 1's fixes all held; the seams gave up two mediums.

- **Stale rewind ring across profile switch / bootFromSlot (medium).**
  `RewindBuffer.h` documents "drop every retained frame on cold boot /
  profile switch", but the ONLY clear site was `coldBoot()`. `applyProfile`
  and `bootFromSlot` wipe RAM/aux (and applyProfile rebuilds the card set)
  without clearing — so with rewind enabled, F6 after a II+→//e switch
  restored II+ RAM/CPU/slot state onto the //e ROM (PC into II+ Applesoft →
  crash), and `truncateAfter` made that garbage the new timeline. Now
  cleared in `bootFromSlot`, `applyProfile` (after step-1 stop) and
  `restartEmulationFromSettings` (its SLOTn sections describe the card set
  being torn down).
- **Snapshot load left the speaker dead for minutes (medium).**
  `SpeakerDevice`'s reconstruction cursor only snaps FORWARD and purges
  older-stamped toggles as stale; rewind/scrub knew this
  (`flushAudioForRewind`) but `/snapshot/load` (AI server) and CLI
  `--snapshot-load` didn't — loading a snapshot with a smaller
  cycleCounter muted audio until the counter re-passed its pre-load value
  (10 min of play ≈ 10 emulated minutes of silence). Both callers now
  flush the speaker + drop the rewind ring (whose stamps would break
  `indexForCycle` monotonicity) after a successful restore.
- **3.5" boot without 3.5" hardware failed silently (low).**
  `insertAndBootImage`'s Sony35 branch fell through to
  `controller->mount35()` — the //c+-only on-board Sony hub — on ANY
  machine without a SmartPort card, then cold-booted to the BASIC prompt
  with `true` returned and "booted disk" logged. Now errors out ("no 3.5"
  device in this config") unless a SmartPort card is present or the
  profile is the //c+.
- **Kiosk K-key leak, open direction (minor, wave-1 fix was one-sided).**
  Closing the Keys band with K was fixed, but OPENING it still typed a
  live 'k' into the running game: the GLFW char callback fires while the
  menu is still closed, then the same frame's `updateKioskMenu` opens the
  non-pausing band. K (and Ctrl-K's $0B) is now reserved in kiosk mode.
- **Swallow latch scoped to gamepad-mapped pads.** Raw pads can't drive
  the menu (nav needs a mapping), so their held fire button across a
  keyboard-driven menu close is legitimate game input — no longer eaten.
- **Adversarially re-verified, no change needed:** onKey/onChar gate (F10
  via ImGui, AI keys via `pasteText` direct, Alt tracking above the gate),
  swallow drain on unplug/Emscripten, F6 re-park (worst case one worker
  tick behind the overlay), cassette clock domain split, stickToPaddles
  math (new tests fail on the old code: 143 vs ≤132, 134 vs 128). Owned
  trade-offs: ~13-count axis-snap detent at the cone boundary (no
  hysteresis until someone reports flicker), F6 inert during the Keys
  band, stick-nav edges without the history guard (can only move a cursor
  one step).
- **Release audit.** Docs drift fixed (README documents the kiosk in-game
  menu and drops "no menu to quit"; DEV.md §kiosk rewritten against
  `openKioskStartMenu`/`updateKioskMenu`/`renderKioskMenu` + §joystick
  against the stickToPaddles pipeline and GamepadPlay mapping; CLAUDE/TODO
  note speaker+cassette ARE retimed). CI now triggers on `v*` tags — tag
  builds previously ran NO CI. Decision (2026-08-02): keep the in-repo
  dumps and **bundle the full `roms/` tree in every release artifact** —
  packaging/docs now match the tree that has been public since the
  initial commit.

## 2026-07-12 (pre-release bug hunt: kiosk input leaks, PAL cassette clock)

Adversarial review of everything landed since v0.7 (kiosk menu, square-gate
joystick, PAL speaker fix). Two real input-isolation leaks in the kiosk menu,
one sibling of the PAL speaker bug, and a fistful of minors.

- **Kiosk menu keyboard fallbacks leaked into the machine (major).** The
  menu's arrows/Enter/Esc/K are polled via `ImGui::IsKeyPressed`, but the
  overlay window never sets `WantCaptureKeyboard`, so `glfw_key_callback`
  kept forwarding every key to `MainWindow::onKey`/`onChar` → the $C000
  latch. Concretely: Enter on the key band sent the selected cell **and**
  injected $0D (every send double-typed); Esc closed the menu and delivered
  a stray $1B to the game on resume; K typed a literal 'k' into the running
  title. Fix: `onKey`/`onChar` early-return while `kioskMenuOpen_` (menu
  navigation itself is unaffected — it never used the callback path).
- **Gamepad close-press leaked PB0/PB1 into the game (major).**
  `pollJoystickAndPushToMemory` runs *before* `updateKioskMenu` and gates
  suppression on `kioskMenuOpen_`, which lags a close by one frame — and
  Circle/Cross double as the menu's B/A **and** the Apple game-port
  buttons. Dismissing the menu with B therefore fired PB0 in the game for
  several frames (ditto Cross→PB1 after Restart, and a D-pad direction held
  at close fired an arrow key). Fix: latch `kioskSwallowPad_` on the
  open→closed edge and keep faces + D-pad suppressed until the pad is fully
  released; analog paddles stay live (no edge to leak). The auto-repeat
  history (`padArrowHeld_`) resets while suppressed so a held direction
  re-arms cleanly.
- **F6 rewind unpaused the machine behind the open menu (minor).** A hold
  released while the Start menu had the worker parked ended in
  `rewindEndAndResume` → `Mode::Running`, and `kioskSetPaused` early-outs
  (it still believed "paused"), so the game ran with audio under the
  overlay until reopen. Fix: F6 is inert while the menu is open **and**
  `updateKioskMenu` re-parks the worker if anything resumed it behind a
  wanted pause.
- **PAL cassette pulse audio — same bug the speaker fix cured (minor).**
  `CassetteDevice::queueAudioSegment` converted cycle durations with the
  hardcoded NTSC `kRealtimeAudioTimebaseHz`; under PAL the queue fills
  ~0.7 % slower than the callback drains (~330 samples/s short at 48 kHz)
  → periodic level dips on sustained tones. Fix mirrors the speaker:
  atomic `realtimeTimebaseHz_` + `setCpuClock()` wired from
  `setVideoStandard`. The tape-**file** timebase stays NTSC-nominal on
  purpose — it's the format's cycle definition, not playback pacing.
- **Kiosk minors.** GAMES list now rescans on the RomDirs→List transition
  (a folder added via the browser used to stay invisible until the menu was
  reopened); the mounted-disk ● marker matches canonically (a kiosk
  launched with a *relative* path, `POM2 games/foo.dsk`, never matched the
  canonicalized scan entries — cursor landed on index 0, no ●).
- **Paddle deadzone: continuous engage + axis-snap (follow-up).** Two
  behavioral gaps left by the per-axis → radial deadzone switch: (1) the
  hard cutoff stepped ~12 counts the instant the stick left the dead zone —
  now the radial deadzone **rescales** ([dz..1] → [0..1] along the ray), so
  the reading is continuous (128 → 130 across the edge, pinned); (2) radial
  lost the old per-axis suppression of cross-axis drift — 5 % Y wobble
  during a full X push read PDL(1)≈134 and crept games. Now an
  **axis-snap notch** zeroes the small axis while it's under `dz × |big|`;
  the threshold scales with the dominant component, so diagonals are never
  notched and the square-gate corner guarantee (full diagonal → 255/255)
  survives — both pinned. The whole pipeline (invert → deadzone → notch →
  gate → 0..255) is now a pure static `stickToPaddles()` that
  `paddleValue()` routes through, closing the review's "composition not
  unit-testable" gap: `joystick_square_gate_test` pins center/NaN/corners/
  rails, the deadzone edge, drift suppression, the gate-off path and invert.
- **Joystick minors.** `edge()` now requires prior-poll history — the first
  poll after a (re)bind treated an already-held button as a fresh press
  (Start held across a rebind popped the kiosk menu). Explicit
  `#include <algorithm>` (was compiling through transitive includes).
  Removed dead `activeMouseSlot` (last -Wunused-variable in the GUI build).

## 2026-07-11 (kiosk follow-ups: PAL speaker clock, HDV/3.5 in menu, DS4 in-game mapping)

Fixes + refinements on top of the same-day in-game menu.

- **PAL speaker audio fix (real bug).** The 1-bit speaker's cycle→sample
  reconstruction hard-coded the NTSC CPU clock (`SpeakerDevice::kCpuClockHz =
  POM2_CPU_CLOCK_HZ`, 1.0227 MHz) and `setVideoStandard()` never retuned it. On
  a PAL profile (CPU actually 1.0156 MHz) the audio path consumed ~0.7 % more
  cycles/sec of toggles than the CPU produced, starving the reconstructor →
  periodic snap-forward glitches on continuous speaker music. **H.E.R.O. on the
  //e-PAL profile sounded broken; the same disk under `--preset iie` (NTSC) was
  clean** — which pinned it. Fix: `kCpuClockHz` → runtime `cpuClockHz_`
  (`std::atomic<double>`) + `SpeakerDevice::setCpuClock()`, called from
  `EmulationController::setVideoStandard()` with the standard's real clock
  (`pom2VideoTiming(s).cpuClockHz`). NTSC numbers unchanged → zero regression.
  (AY/SSI263 device clocks stay NTSC-nominal by design.) Confirmed by ear.
- **Kiosk pause no longer swallows audio on resume.** While the menu parked the
  worker (`Mode::Stopped`), the audio thread kept advancing the speaker cursor
  over silence, leaving it far ahead of the frozen production; on resume the
  catch-up purge would eat the game's first sounds for ~the pause duration.
  `kioskSetPaused(false)` now `speaker().reset()`s on the paused→running edge.
- **Mounted-disk marker.** The games list prefixes the disk currently in the
  boot drive with a ● (`ICON_FA_COMPACT_DISC`) and lands the cursor on it.
- **HDV + 3.5" reachable from the menu.** The scan no longer filters to 5.25"
  only — it accepts everything `classifyDiskForSlot` recognises (5.25"/3.5"/
  HDV). 5.25" still hot-swaps in place (flip-disk); 3.5"/HDV route through
  `insertAndBootImage` (the CLI launcher's path) and boot immediately. The ROM-
  folders browser reaches `hdv/` and `disks_3.5/` (add once, persisted).
- **DS4 in-game mapping.** New `JoystickInput::GamepadPlay` (from the standard
  gamepad layout, `valid` only when gamepad-mapped): the analog stick stays the
  Apple II paddles; **Cross/Circle → PB0/PB1**; the **D-pad → Apple II arrow
  keys** (←$08 →$15 ↑$0B ↓$0A) with //e-style auto-repeat (350 ms delay →
  ~16/s); **Square → SPACE, Triangle → RETURN** (one key per press, via
  `Memory::queueKey`). Menu-gated (no injection while the overlay is up) and
  only for gamepad-mapped pads — a raw pad keeps the legacy buttons 0/1/2 →
  PB0/1/2 fallback. The button D-pad is used here, NOT the stick (the stick is
  the joystick), so `GamepadPlay` never folds stickX/Y in the way `UiNav` does.

## 2026-07-11 (kiosk in-game menu — two-zone method)

Reworked the kiosk disk picker into a full two-zone in-game menu. Still
**exclusive to `--kiosk`** — the whole thing lives behind the `if (kiosk_)`
gate.

- **Two entry points.** **START** (pad, standard GLFW mapping) / **F10** opens
  the two-zone **Start menu**; **SELECT** (Back button) / **K** opens the
  **Keyboard band** directly, even mid-game.
- **Two-zone Start menu** — the headline UX win. A **GAMES** list and an
  **ACTIONS** column (Restart / Keyboard / ROM folders / Quit) coexist:
  **◀▶ swaps focus** between the two zones, up/down moves within the focused
  zone, **A** validates it. No more scrolling past every disk to reach an
  action. The focused zone is vivid with a green ▶; the other is dimmed.
- **Machine paused** (`Mode::Stopped`) on every Start-menu page — like
  inserting a disk on a real machine at rest — but the **Keyboard band leaves
  the game running** so injected keys land live. `kioskSetPaused()` only
  resumes a worker it parked itself, so it never fights F6-rewind's mode moves.
- **Keyboard band** = a 2D grid of Apple II keys; **A** sends one via
  `Memory::queueKey`. The Apple keyboard is a **latch+strobe**, so a one-shot
  queue with no key-up bookkeeping is correct.
- **Temporal auto-repeat** (400 ms delay → 150 ms cadence, clock-based off
  `ImGui::GetTime()`) for held directions, plus **L1/R1** fast page jump (±10)
  in the games list — needed because the paused menu loop runs unthrottled and
  would otherwise scroll unaimably fast.
- **Proximity SORT, not filter.** The old build *hid* every non-sibling disk;
  now all mountable 5.25" images are shown, with the mounted title's other
  sides sorted to the top (selection anchored on the mounted disk).
- **ROM-folders manager + gamepad directory browser** (`◀▶`/shortcuts to `/`,
  Home, removable mounts). Extra scan folders persist in a sibling
  **`kiosk_romdirs.txt`** — deliberately *outside* `state.cfg` so the kiosk's
  read-only main config is never written (keeps POM2's strict kiosk contract).
- Plumbing: `JoystickInput::UiNav` gained `left/right/select/pageUp/pageDown`
  edges + raw `*Held` levels (the latter feed the temporal repeat).

## 2026-07-10 (kiosk gamepad UX + Apple II square-gate joystick)

Kiosk mode made lean-back / controller-friendly, plus a faithful joystick fix.

- **Square-gate joystick** (`JoystickInput::applySquareGate`, default on, key
  `joystick_square_gate`). Modern analog sticks ride a *round* gate, so a full
  diagonal only reaches ~217/217 and the extreme corners are physically
  unreachable — but the original Apple II stick rode a *square* gate where full
  X **and** full Y at once (255/255) were reachable, which **Wings of Fury's
  take-off requires**. `paddleValue()` now processes the X/Y pair together
  (radial deadzone, not per-axis — a per-axis one notched the diagonals) and
  expands the inscribed circle to the full square (`s = mag / max(|x|,|y|)`),
  leaving pure-axis directions untouched. Toggle in the Joystick panel; pinned
  by the new `joystick_square_gate` test (129 → 130 ctests).
- **Kiosk gamepad disk selector.** In `--kiosk`, the pad's **Start** (standard
  GLFW gamepad mapping) — or **F10** when the pad has no SDL mapping — opens an
  on-screen picker of the 5.25" images sitting **next to** the booted disk,
  filtered by **name proximity** (longest common prefix) so only the same
  title's other sides/disks appear (Wings of Fury Side A ↔ Side B), not the
  whole 700-disk folder. D-pad/stick move, **A** mounts in-place (no reboot,
  the flip-disk gesture), and trailing **Reset** (reboot on the mounted disk)
  and **Quit** action rows finish the job — all without a keyboard.
- Pitfalls captured: the overlay first rendered *behind* the opaque
  full-viewport kiosk window (fixed by dropping `NoBringToFrontOnFocus` +
  `SetNextWindowFocus`), and the file list stayed tiny under
  `SetWindowFontScale` because a `BeginChild` is a separate ImGui window with
  its own scale (re-applied inside). The Start button silently did nothing on an
  unmapped pad — hence the F10 fallback + a one-shot `gamepad-mapped=yes/no`
  diagnostic log.

## 2026-07-09 (v0.7 — packaging, CI & desktop integration)

First tagged release. Focus on shipping, not the core emulator.

- **GitHub Actions CI** (`.github/workflows/ci.yml`): a `linux` job builds the
  full tree and runs the ~130-test ctest suite (Klaus 6502/65C02, Tom Harte,
  cpu_cycle_count, golden-hash display, boot traces) as the non-regression gate,
  plus a `wasm` Emscripten verification build. The suite was dormant — nothing
  ran it automatically before.
- **Fixed a latent packaging blocker**: `packaging/roms_README.txt` was
  referenced by `install(FILES …)` but never existed, so *every*
  `cmake --install` / `cpack` / `build_dist.sh` staging aborted. Created it (the
  ROM drop-here note). Linux install now succeeds end-to-end.
- **Desktop integration**: an application icon (`packaging/POM2.svg` +
  rasterised hicolor PNGs), a MIME type `application/x-apple2-disk`
  (`.dsk/.do/.po/.nib/.woz/.d13/.hdv/.2mg`) so a double-clicked disk opens in
  POM2, and Debian maintainer scripts that refresh the mime/desktop/icon caches.
- Metadata fixes: real homepage URL in the package (`github.com/habib256/POM2`)
  and a `.desktop` keyword typo.
- **Grappler+**: bundled the 4 KB Orange Micro Grappler+ EPROM dump so the card
  exposes its full firmware (graphics-dump entry points + ROM fingerprint)
  instead of the fallback stub.

## 2026-06-16 (Tom Harte 65x02 ProcessorTests — cycle-exact validation)

Added the Tom Harte [`SingleStepTests/65x02`](https://github.com/SingleStepTests/65x02)
suite — 10,000 random vectors per opcode pinning the full state (registers +
memory) **and** the cycle count. Data-driven harness `tomharte_cpu_test
<nmos|cmos> <dir>` (in-house JSON scanner, no vendored lib), curated +
SHA-pinned ctest gate (`tomharte_6502` / `tomharte_65c02`), download behind
`-DPOM2_FETCH_TOMHARTE=ON` (full corpus ~1.4 GB/CPU → `tests/fetch_tomharte.sh`
for exhaustive runs of all 256 opcodes).

Results: **NMOS 6502 = 100%** on 41 opcodes (410,000 vectors, decimal
included); **WDC 65C02 = 100%** everywhere except decimal SBC on **invalid**
BCD digits.

The suite flushed out **4 genuine decimal bugs** in `M6502::ADC/SBC`, all
**provably identical for valid BCD** (only invalid digits — never produced by
correct software — change), hence zero regression risk (`cpu_cycle_count_test`
+ Klaus stay green):

- **ADC low nibble**: `tmp+6` overflowed onto bit 5 when the low-nibble sum
  reached `$1A-$1F` (invalid BCD digit) → `accumulator & 0xF0` injected `$20`
  instead of the single `$10` carry, inflating the result by `$10`.
  The silicon re-packs: `((tmp+6)&0x0F)+0x10`.
- **ADC decimal carry**: tested `tmp & 0x100`, but the `+$60` high-nibble
  correction can push an invalid-BCD sum up to bit 9 (`$240`, bit 8 = 0) →
  lost carry. Fix `tmp >= 0x100` (identical for valid BCD ≤ `$190`).
  Pitfall: an `int tmp` made it look like `& 0x100` sufficed — it's the carry
  rising to bit 9 that betrays it, visible only via instrumentation.
- **ADC V (CMOS)**: was forced to the binary overflow; the WDC uses the
  "high-nibble-sum" V already computed at line ~421. Removed the overwrite.
- **SBC low nibble**: `tmp-6` left bit 4 (the borrow the high nibble reads via
  `accumulator & 0x10`) un-re-packed → lost borrow, result +`$10`.
  Fix `((tmp-6)&0x0F)-0x10`.

Documented pitfall (not fixed): the WDC 65C02 decimal SBC on **invalid** BCD
follows a silicon correction *distinct* from the NMOS (officially undefined,
data-dependent ±1 errors) that we don't model — `e9` diverges at ~3.4% on
`wdc65c02/v1`, the NMOS being exact (`6502/v1/e9` = 100%). Opcode excluded from
the CMOS gate, tracked in `tests/tomharte_wdc65c02.manifest`.

Architecture note: instruction-stepped core + non-virtual `Memory::memRead/Write`
→ we validate the final state + the cycle count (not the per-cycle bus order),
which covers exactly the class of timing bugs (cf. the historical RMW
under-count of `cpu_cycle_count_test`).

## 2026-06-12 (wave 4: remaining peripherals, UI, snapshot — MAME oracle)

Four read-only hunters over the never-audited areas (Grappler / LLE+hand
mouse, UI layer, //c-IWM-Sony stack, snapshot/rewind/cassette), then verified
fixes + pins. Suite: 127/127.

- **Grappler+: inverted register decode.** The real data port is
  `!(offset & 3)` ($C0n0/4/8/C) — POM2 spooled offset 1, which is the
  **ROM bank select** on the real card: the authentic 4 KB firmware printed
  into the void, and its status poll read $FF = "busy + out of paper"
  (the worst possible value). Rewritten per MAME grappler.cpp:
  IRQ|DIP|BUSY|PE|SELECT|ACK status, $C800 banks (A0 set / read $CnXX
  reset + A6 ACK-detection trick), A1/A2 IRQ on the bus. The ROM stub also
  wrote via offset 1 — hence green tests that pinned the stub, never the real
  ROM path (pitfall: the test validated the implementation against itself).
- **Sony 3.5": register table aligned with MAME `mac_floppy_device`.**
  Address bit 3 = HEAD-SELECT line (ssW), not the IWM drive-select;
  MotorOff is strobe 0x6 (0x3 = EjectOff, no-op — the old "boot-tuned" table
  put motor-off there: a conformant firmware killed the motor thinking it was
  cancelling an eject); the disk-change latch lives in sense 0x3 and clears
  via the DskchgClear STROBE (0xC), not on read; DIRTN polarity fixed; a
  write-protect sense (0x9) finally exists — a protected 3.5" image was
  invisible to the firmware (writes silently lost). IWM motor-off delay:
  8388608 ticks of the 7 MHz clock ≈ 1.17 s (was 1 s CPU, with a false
  comment about the IWM clock).
- **UI: Floppy Emu panel's `insertDisk` without `stateMutex`** while the
  worker streams nibbles — potential corruption/UAF on click; all neighboring
  paths locked. Slot Config state reads moved to snapshot-under-lock;
  Disk II `motorOn` made atomic (read by the UI-side auto-turbo); per-cell
  PushID in the memory viewer (hundreds of "00" cells shared the same
  ImGui ID).
- **Snapshot: cards that gained state didn't participate.**
  Grappler+ (banks/ACK/IRQ) and EchoPlus (full SSI263 — the Mockingboard
  SoundII captured the same chip from the start) now have their append/load
  hooks + round-trip pins. CLI `--snapshot-save/load` were silent **no-ops**
  documented as functional — wired onto the same mechanics as the AI server.
  Doc: the "CASS" section never existed.
- **No-Slot Clock: write cycles wired** (AppleWin parity — the DS1216E key
  bit travels on A0 of the ADDRESS, R/W indifferent: drivers feeding the key
  with STA never unlocked the clock).
- **68705: level-sensitive timer IRQ** (vector pull no longer clears the
  request while TIR=1/TIM=0 — MAME parity). **LLE mouse**: no more cursor
  jump after reset (delta counters re-primed from the host). Size caps on
  the .wav/.aci files (pre-slurp), joystick NaN guard, `POM2_AUTO_*` env
  timers fixed and cancellable.

## 2026-06-12 (bug hunt: full audit validated against the MAME oracle)

Systematic audit of the subsystems (CPU/memory, video, audio, storage,
threading/cards), each fix validated against the MAME sources (file+line
citations in comments) and pinned by a test. Suite: 127/127.

- **LSS disk writes angularly mispositioned** (the worst — silent corruption
  in the default config). `DiskImage::writeFlux` reduced the splice window
  with a raw `startLssCycle % period`, while the read (`getNextTransition`)
  is anchored on `revolutionStart` (port of MAME `find_position`). The anchor
  being arbitrary (2×cpuCycleTotal at motor-on), every bit-level write landed
  `revStart mod period` cells away from where the controller had just read the
  address — the data field overwrote another area of the track. **Pitfall #2
  on the same path**: the cell→nibble re-pack assumed 8 cells/byte, but the
  `expandTrackBits` timeline adds +2 cells of padding per sync $FF — drift of
  ~4.75 nibbles/sector on a standard .dsk. Why no test caught it:
  `diskii_lss_smoke_test::testLssWrite` **explicitly skipped the positional
  assertion**. `writeFlux` now takes the anchor (same convention as the read),
  the re-pack walks the padded timeline, and the positional pin is active
  (`disk_writeflux_anchor` + strengthened LSS test).
- **DHGR: hues rotated 90° in the composite OE demods (CPU+GPU).**
  The subcarrier phase shift was applied **twice** (sin/cos table construction
  with `(k+po)&3` AND indexing with `(xi+po)&3`). The GLSL shader comment
  documented the wrong conclusion: the old GPU formula (single application)
  was the right one, it "diverged" because the CPU was wrong. HGR (po=0) was
  unaffected — hence an "excellent" calibration that masked the bug. Pitfall:
  `dhgr_phase_signal_test` pinned the bug **tautologically** (its anchor
  replicated the buggy formula) — the test now derives its expectation from
  the independent MAME LUT path.
- **DLGR: nibble pattern restarted per 7-dot half-cell** instead of the
  absolute 14.318 MHz phase (`paintLoRes40` already did it right) — colors
  alternating per column. Pin: exact samples in absolute phase (the naive
  `sig[i]!=sig[7+i]` test is invalid: at rotl4(1)=2 from x=0 and main 1 from
  x=7 ≡ 3 (mod 4) yield the **same sequence** at different phases).
- **Sound II silent**: the SSI263 emulation (registers+IRQ) was complete but
  `fillAudioBuffer` never mixed `ssi_->fillAudio()` (only EchoPlusCard did).
  **VIA 6522**: the ORA access (reg 1) didn't clear IFR.CA1 (MAME
  `CLR_PA_INT()`) → speech IRQ stuck for drivers using the standard idiom;
  first T1 strike at N+1 instead of N+3 (the +2 bias already existed for T2,
  same DIX rationale). **Native Phasor**: MAME VIA decode
  (`$Cs10`→VIA1, `$Cs80`→VIA2, `$Cs90`→broadcast both, nothing at `$Cs00`).
  **AY**: envelope period 0 = double speed (MAME doesn't clamp to 1).
- **SSC/telnet**: `send()` without `MSG_NOSIGNAL` → a peer that disconnects
  rudely **killed the process** (SIGPIPE); EAGAIN treated as fatal + partial
  sends silently lost (breaks ADTPro/XMODEM); blocking `accept()` not woken by
  `shutdown()` on macOS/BSD (the same bug already documented+fixed in
  AiControlServer — ported the `poll()` pattern).
- **Slot Config "Apply" overwrote the slot config on //c** — exactly the bug
  fixed at release on 2026-06-10, but the Apply path lacked the `builtInSlots`
  guard. **`applyProfile` without lock**: `stop()` didn't wait for the worker
  to park and the frame loop didn't re-check `mode` → ROM/SlotBus/disks
  rebuilt while a turbo frame was still running (UB). The worker re-checks
  between 4096-cycle chunks and the switch waits for `workerParked_`.
  CLI `--speed` clamped to 2 M like the AI server.
- **2IMG: lock bit = bit 31** (spec/CiderPress/AppleWin), not bit 0 — locked
  images were writable; the DOS volume read 0 instead of 254 on a locked dump
  without bit 8. The test pinned the wrong interpretation (written from the
  code, not from the spec — classic pitfall).
- **NMOS CPU: undocumented multi-byte opcodes** ($x3, $4B/$6B/$8B/$AB/$CB,
  $1B..$FB) dispatched as 1-byte NOP → instruction-stream desync (the exact
  class already fixed for $0B/$2B/$EB); $CB/$DB were remapped
  "undefined WAI/STP on NMOS" while NMOS puts SBX #imm (2 bytes) and DCP
  abs,Y (3 bytes) there. 1-byte NOP: 1 cycle on 65C02 (fetch only, MAME
  ow65c02) vs 2 on NMOS; WAI/STP 3 cycles. NMOS decimal SBC: deterministic V
  (binary difference, MAME `do_sbc_d`) — decimal ADC already did it, V stayed
  stale on the SBC side.
- **IIe/II+ memory**: $C010-$C01F is the keyboard strobe mirror on II/II+
  (MAME `.mirror(0xf)`, read OR write — `STA $C01x` never cleared it);
  on IIe any $C01x write clears it (reads $C011-$C01F stay status-only).
  **$FE sentinel**: `iieReadStatus` returned $FE for "not a status" — but
  `0x80|transchar($7E '~')` == $FE is a legitimate read → RDRAMRD polls sent
  to the floating bus (OFF read while ON). Out-of-band signal now. **INTC8ROM**:
  arms on any $C3xx access with SLOTC3ROM=off **including under INTCXROM=on**
  (UTAIIe 5-28) and on the write path; a $C3xx write no longer steals the
  $C800 window from the legitimate card. **Video events in VBL**: stamped at
  line 192 ("end of frame") instead of being clamped to 191 — a mode switch
  thrown in VBL (the canonical anti-tearing practice) no longer paints a
  spurious split on the last visible line.
- **Misc validated**: HDV STATUS returns the block count in X/Y (the BITSY
  crash already fixed on the SmartPortCard side); a 32 MiB volume of exactly
  65,536 blocks clamped to $FFFF (read 0 before); `writeBackEnabled`
  propagated to images on snapshot restore; ClockCard no longer loses the time
  on Ctrl-Reset (battery-backed uPD1990AC); the AI server's `/mouse`
  recognizes the AppleWin HLE mouse (//c default); mouse VBL in profile cycles
  (PAL 20313); PAL clock: provenance honestly documented (locked at
  line 15625×65 by design; MAME = 1,016,966 — assumed 0.13% gap, same class
  as the "device clocks stay NTSC" approximation).

## 2026-06-10 (//c: NMOS CPU freeze, rear Chat Mauve, slot config preserved)

- **"POM2 crashes when I select the Apple //c (1984) profile"** — it was a
  **CPU freeze**, not a segfault. Diagnosis: the user's config had
  `cpu_mode_override=nmos` (sticky setting, set once on a II+).
  `resolveCpuMode` therefore returned **always NMOS**, including for the //c —
  which has a **soldered 65C02**. The //c ROM runs 65C02 opcodes
  (`LDA (zp)`=$B2…) that **decode as KIL on NMOS** (`M6502::Hang` = `PC--` →
  infinite loop) → frozen CPU → dead screen = "crashed". (Not reproducible
  headless because the symptom is frozen emulation, not a process crash;
  isolated by analyzing `M6502.cpp` + reproducing the mid-frame switch.)
  **Fix**: `resolveCpuMode` honors an **NMOS** override only if the profile is
  NMOS by default (II/II+///e-unenh); the **65C02-only** machines (//c, //c+,
  //e enhanced, PAL variants) always run in CMOS. The Machine→CPU menu
  **greys out** "NMOS 6502" on these profiles. Also answers "the CPU should
  switch NMOS↔65C02 per the //e/enhanced profile". Verified: //c resolves
  `CPU = 65C02` despite the override.
- **Le Chat Mauve on //c (rear connector).** The //c took the
  **"IIc Adapter"** Le Chat Mauve on its DB-15 video port (cf.
  fenarinarsa.com/?p=1370 + CLAUDE.md § profiles). POM2 ignored any card on a
  `noPhysicalSlots` profile. **Fix**: exception for `chatmauve` — the RGB card
  plugs into the //c-class machines (it's a video adapter, not a peripheral
  slot card). The Slot Config panel offers a combo **{(empty), Le Chat Mauve
  RGB (rear connector)}** on //c/+ (nothing else is pluggable; the duplicate
  check limits it to one adapter).
  **The "Apple //c PAL (Le Chat Mauve)" profile now wires the card in hard**
  (built-in **sl7** = "IIc Adapter") — it carried the name without plugging in
  the card. On this profile the other slots' combo is greyed out (a single
  adapter), and a redundant user `chatmauve` elsewhere is ignored (no double
  card).
- **Slot config overwritten on exit on //c.** `persistSettings` saved the
  **live** `slotCards` mapping, so quitting on //c wrote the forced built-ins
  (`mouseaw`…) over the user choice (`slot_4_card=mockingboard` lost on return
  to //e). **Fix**: don't persist profile-forced slots (built-ins + slots
  cleared by `noPhysicalSlots`, except the user-controllable Chat Mauve) — the
  user setting stays intact. Same class as
  [[pom2-cffa-profile-switch-drop]]. Verified: `slot_4_card=mockingboard`
  preserved after a clean exit on //c.

## 2026-06-10 (DROL cut-scene: $C050-$C057 reads → floating bus)

- **DROL cut-scene hang.** Disk image scan: the cut-scene overlays
  (offsets 0x14359/0x143d5/0x14be0 of `Drol.dsk`) sync via
  `LDX #$02 / LDA $C050 / CMP #$80 / BNE / DEX / BPL` — three consecutive
  reads of the **video scanner** through a display soft-switch. POM2 returned
  a hard 0 on `$C050-$C057` reads → infinite loop (LinApple's historical hang;
  AppleWin fixed it in 1.13.0 by implementing the floating bus). **Fix**: a
  `$C050-$C057` read toggles the mode AND returns `floatingBus()` (MAME
  `apple2.cpp do_io` does the same); likewise for the speaker `$C030-$C03F`
  (latch undriven). **Pitfall avoided**: the block held `stateMutex` and
  `floatingBus()` re-takes it — work scoped before the return. Pinned by
  section (d) of `vapor_lock` (the game's exact loop locks on an HGR page
  filled with $80, and the TEXT-off side effect is preserved).

## 2026-06-10 (DROL: double-buffer flips vs beam-racing; Chat Mauve: AppleWin decode)

- **DROL flicker (unsynchronized page flips)** — all display modes, NTSC as
  well as PAL. Diagnosed by probing the real disk (`tests/drol_probe.cpp`,
  WOZ): DROL flips `$C054/$C055` every ~4 frames at **drifting** positions
  (23/31 flips in the visible zone) — free double-buffering, NOT beam-racing
  (its flipper is self-modifying at `$6138`; DROL's floating bus only serves
  the cut-scene, cf. AppleWin 1.13.0 "fixed the hang at Drol's cut-scene").
  **Why it flickered**: the beam-raced replay paints the band above the flip
  from the page the game is **already redrawing** — POM2 reads RAM at render
  time, not at beam passage → half-erased sprites. (The real beam read the
  still-intact page; before per-frame publishing, these events were often lost
  → accidentally "clean" full-page render.) **Fix**: in
  `forEachBeamSegment`, a frame whose PAGE2 events all go in the SAME
  direction = buffer flip → final page applied to the whole frame (= the RAM
  actually displayable); a frame that flips in BOTH directions (DIX MODPAGE:
  page 1 left, page 2 right of the same line) keeps the exact replay. Pinned
  `drol_pageflip_render`; `dix_modpage_split` unchanged.
- **The 6 560-wide painters re-read the live state** (`renderText80`,
  `renderDhgr`, `renderLoResDouble`, `renderTextChatMauveFgBg`,
  `renderHgrDuochrome`, `renderHiResChatMauve80` → internal
  `mem.getDisplayState()` instead of the band's `state`) — same class of bug
  as the one already fixed on the legacy painters: PAGE2/ALTCHAR mid-frame
  splits were ignored in 560 (that's why "Chat Mauve didn't flicker": it
  masked the flips). Threaded signatures, state passed everywhere.
- **Chat Mauve HGR resolution**: the color decode overwrote EVERYTHING in
  aligned-pair blocks (1 color / 4 dots = 140 effective → "soft" image).
  Ported the AppleWin `RGBMonitor.cpp UpdateHiResRGBCell` algo: a pixel is
  COLOR only if it forms an isolated 010/101 pattern with its neighbors
  (its aligned pair's color, 2 dots); everything else is black/white
  **at the full 280 px resolution** — the white runs (text, sprite outlines)
  recover their sharpness, faithful to the real RGB card. `*/hgr*/chatmauve`
  goldens regenerated; semantics pinned by `le_chat_mauve_smoke` +
  updated `display_persistence_smoke`.

## 2026-06-10 (PAL beam-racing: per-video-frame publishing + 1× speeds per standard)

- **Publishing the video event log per video frame** (the 50/60 Hz hand-off).
  The old model opened the log on every worker CPU tick
  (`beginVideoEventFrame`) and the UI **stole** it at vsync (`takeVideoEvents`
  closed the bracket); any event recorded between the UI take and the next
  tick was **silently lost** (`recordVideoEvent` no-op with the bracket
  closed). **Why it mattered**: in PAL the worker runs at 50 Hz and the UI at
  60 Hz → systematic 10 Hz beat: ~1 UI render in 6 fell in the same tick and
  received an *empty* log (→ `renderInternal`, zero splits), the others a
  *partial* log — the French Touch mid-scanline effects (*Mad Effect*, DIX)
  flickered and lost bands. No test caught it (they bracket synchronously).
  **Fix**: continuous recording; `Memory::advanceCycles` **publishes**
  `{frame-start state, events}` at each crossing of a video-frame boundary
  (65 × 262 NTSC / 312 PAL cycles — aligned on the scanner geometry, not on
  the worker's 17045/20313 budget); `takeVideoEvents` returns a **copy** of
  the last published frame, re-renderable at will by the 60 Hz UI. The
  synchronous bracket remains available for tests (`legacyEventBracket_`). A
  reset purges both logs (otherwise a phantom replay against the wiped state).
  Bonus: the WASM path (which never called `beginVideoEventFrame`) gains
  beam-racing. Pinned `video_event_publish`.
- **`$C019`/VBL follows the video standard**: the VBL edge detection
  (`advanceCycles`) and the `$C019` read used a hard-coded 262 lines; a PAL
  demo that measures the VBL period saw a 17030-cycle frame while the floating
  bus swept 20280 — two contradictory machines. Pinned by extending
  `pal_timing` (§ 4: lines 262–311 = VBL under PAL, wrap at 312).
- **1×/2×/4× speed derived from the active standard** (toolbar, AI server
  `/speed` preset, disk-turbo restoration re-seeded by `applyProfile`).
  **Why**: the hard-coded 17045 ran a PAL machine at 17045 × 50 Hz = 852 kHz
  (−16%) on the first "1×" click — effects that drag, sluggish Mockingboard
  music. Remaining assumption: `MouseCardAppleWin::kCyclesPerVbl` = 17045
  (60 Hz VBL pacing of the mouse HLE even under PAL — no effect on the demos,
  to be reworked with the //c port).

## 2026-06-01 (Release v0.7)

- **Version bump v0.6 → v0.7.** Updated the version string in the
  **5 canonical locations** listed by `CLAUDE.md` § Version string locations:
  `CMakeLists.txt` (`project(... VERSION 0.7 ...)`, which also drives
  `CPACK_PACKAGE_VERSION` + the `build_dist.sh` archive name), `src/main.cpp`
  (console banner + initial window title), `src/MainWindow_Slots.cpp`
  (runtime title that overwrites `main.cpp`'s once the profile is resolved — at
  the constructor **and** at the profile switch), `src/MainWindow.cpp`
  (*About* dialog), and `README.md` (title). `CLAUDE.md` itself updated
  (`Current release: **v0.7**`). **Why note it**: the version lives in
  duplicated strings not derived from a single source, so any bump must touch
  these points en bloc on pain of drift (window title vs About vs package).
  Single CMake source → generated header = separate backlog item.

## 2026-05-31 (Composite: signal beam-racing + phosphor curve)

- **Composite signal beam-racing.** `fillCompositeSignal` read *a single*
  end-of-frame `getDisplayState()`: the mid-scanline display switches
  (text↔graphics, page flip, DHGR on/off) were invisible in **all** composite
  modes (`ColorCompositeOE` GPU, `ColorCompositeOECpu`, `ColorAppleWin`) —
  only the LUT modes benefited from beam-racing (`renderBeamRacing` on the
  RGBA side). **Why it mattered**: a demo that goes from text to HGR mid-screen
  displayed entirely in HGR under OE/AppleWin. **Fix**: `render()` takes the
  event log **once** and passes it to both consumers;
  `fillCompositeSignal(mem, events)` replays the log band by band (zero
  `signalBuf` → `getDisplayStateAtFrameStart()` → `paintSignalBand(y0,y1)`
  which reuses the same `bandRows`/`bandScanlines` clipping as
  `renderInternalBand`). The painted `state` is a *mutable* local so the
  helpers (captured by reference) see each switch. Empty log →
  `paintSignalBand(0,192)` = byte-for-byte the old dispatch (OE GPU/CPU
  goldens unchanged). **Pitfalls**: `signalPhaseOffset_` stays a per-frame
  constant (last graphics band wins → mid-frame HGR↔DHGR split approximated);
  lo-res clips at the block-row (4 lines), like the RGBA path. Pinned
  `beam_race_composite` (text→HGR frame at scanline 96: top band = text
  waveform, bottom band = HGR waveform, and **not** HGR at the top like the
  pre-fix bug).
- **Phosphor curve (CRT gamma).** The signal-level NTSC pipeline (already
  complete: FIR Y@2.0 MHz / chroma@0.6 MHz, YUV→RGB, PAL line-phase) had no
  phosphor response. Added a `phosphorGamma` (per-channel power-law `rgb^γ`)
  in `CrtEffectStack`, **after** BCS and **before** scanlines/mask (the mask
  attenuates the light the phosphor has already produced). γ = 1.0 = identity →
  **no golden/parity touched**; γ > 1 deepens the shadows, γ < 1 lifts them.
  It's the *luminance* half of the phosphor model; `persistence` is the
  *temporal* half. Slider "Phosphor curve (gamma)" 0.6–2.6, persisted
  `ntsc_phosphor_gamma`. NB: a *glass* effect, hence always active under OE and
  under the other modes when "CRT effects on all modes" is on.

## 2026-05-31 (3D voxel view — phases 0+1; toolbar rewind button)

- **3D voxel view — "Voxel Cube" rework faithful to MicroM8 (fix).**
  The first version extruded **height = luminance** on a screen laid **flat**
  (XZ plane): bright pixels turned into stalactites, catastrophic angle,
  misshapen voxels. Scraped MicroM8 (`paleotronic.com` Quick Start +
  Features): the "Voxel Cube Color" mode stands the screen **upright**
  (monitor, XY plane) and gives **each pixel a cube of the same thickness**
  extruded toward the viewer on **+Z** ("Voxel Depth"). The height is **never**
  tied to luminance; the per-color relief (`colorShift`, "Z-axis 3D offset")
  is an **option** (default 0 → a flat slab you rotate to see the thickness).
  - **Geometry**: cube footprint XY + depth Z; column→X, row→Y (row 0 = top);
    **real 4:3** plane (2.0 × 1.5) to keep the shape of Apple II pixels.
    `heightScale`→`voxelDepth` (0.06), `+colorShift`.
  - **Camera**: defaults near front-on + slight 3/4 (azimuth 0.32 /
    elevation 0.20 / distance 2.8 / fovY ~40°), target recentered at the
    origin. **Orbit on left-drag + wheel zoom** (in `drawScreenImage`, mutate
    `voxelCam_`). `voxel3d_math` stays green (the camera math is unchanged).
  - **Follow-up (same day)**: (1) **top/bottom inverted** — the
    FBO→`ImGui::Image` presentation is a vertical mirror (like the 2D NTSC
    passes); pre-flip `gl_Position.y` in the vertex shader. (2) **Native
    resolution** — `gridW/gridH` driven by `display->width()/height()`
    (280/560 × 192) → one voxel per Apple II pixel (before: 140×96, half the
    info lost); `voxelDepth`/`colorShift` passed in **cell units** to stay
    constant between 280 and 560 wide. (3) **Per-color relief enabled**
    (`colorShift` 8 cells, luminance-weighted) → requested pin-art "pop".
    (4) **CRT-independent** — the voxel taps the color image **before**
    `CrtEffectStack` via a separate `voxelSrcTex` handle (otherwise
    scanlines/mask/barrel ended up baked into the 50k cubes).
    (5) **Pan/strafe on the middle button** — `OrbitCamera::pan` slides the
    target in the camera's right/up plane, scaled in world-units/pixel for 1:1
    tracking (orbit = left-drag, zoom = wheel). (6) **Moiré removed** —
    **abutting** cubes (`cubeFill` 0.9→1.0: a flat fill becomes a continuous
    slab again, end of the grid of gaps) **+ supersampling** (`superSample`
    2×: FBO rendered at 2×, mip-chain, trilinear minify by ImGui →
    anti-aliasing without MSAA resolve).
  - **Phase 3 — settings panel** (`renderVoxelSettingsWindow`, View ▸
    "3D voxel settings…"): live sliders `voxelDepth` / `colorShift` /
    `cubeFill` / `superSample` (pushed to **3×** by default) / `ambient`, +
    Reset view / Reset settings buttons. The renderer is **owned from the
    moment settings load** (ctor without GL) so the panel and the `voxel_*`
    keys (persisted) wire directly onto `voxel3d_`, even before enabling the
    3D view. The grid resolution stays auto (= screen).
  - **P4 — WASM perf guard**: under `__EMSCRIPTEN__`, `process()` caps
    `superSample ≤ 2` + FBO ≤ 2048² (reduces the factor until it fits) and
    `MainWindow` caps `gridW ≤ 280` (halves the 560 DHGR geometry).
    Native unchanged (`ss ≤ 4`, 8192²). WASM/WebGL2 compile re-checked.
  - **Fidelity bonus — Mono + per-color-index depth**: `mono` checkbox
    ("Voxel Cube Mono", grey output, relief preserved) and `perColorDepth`
    (snap to the nearest of the 16 lo-res `kVoxelPalette` colors → discrete
    per-color relief instead of continuous luminance). 16-iteration search
    loop in the vertex shader (per instance, not per pixel); `glUniform3fv`
    added to the loader. Persisted `voxel_mono` / `voxel_percolor_depth`.
    P5 (rewind tie-in) **deferred** on request.
  - **Wheel fix in WASM**: Emscripten's GLFW port doesn't deliver `wheel`
    events to ImGui (`io.MouseWheel` stayed at 0 → 3D zoom inoperative in the
    browser). Added an `emscripten_set_wheel_callback("#canvas")` in
    `main.cpp` that feeds `io.AddMouseWheelEvent` (same scale as the ImGui
    backend) — a surgical choice so as not to touch the shell's canvas sizing
    (vs `ImGui_ImplGlfw_InstallEmscriptenCallbacks` which also hooks
    resize/fullscreen).

- **Rewind button in the toolbar** (left of Pause, mirroring Step on the
  right): `ICON_FA_BACKWARD_FAST`, **hold = live-rewind** (same gesture as
  `F6` / the Devices ▸ Rewind bar). Greyed out while there's no history.
  `F6` and the button share a single edge-tracker (`driveRewindHold`).

- **MicroM8-style 3D voxel view — foundations (phases 0+1).**
  - **Why / arch**: extrude the screen into cubes (height = pixel luminance)
    with an orbital camera. Key choice: it's an **orthogonal view axis**, not
    a `HiResMode` — a render pass that consumes the **already-decoded RGBA
    texture** (any color mode + NTSC/CRT), exactly like `CrtEffectStack`.
    Universal, free for all modes.
  - **Phase 0 — `Mat4.h`**: Vec3 + column-major Mat4 (perspective/lookAt/
    multiply) + `OrbitCamera` (azimuth/elevation/distance → view-proj). No
    dependency (no glm). Pinned `voxel3d_math` (perspective entries,
    orthonormal lookAt basis, projection of the target to the center).
  - **Phase 1 — `Voxel3DRenderer.{h,cpp}`**: **instanced** cubes
    (`glDrawElementsInstanced`, ~13k for 140×96), height+color per
    **vertex texture-fetch** of the framebuffer, shading by **screen
    derivatives** (no normal attribute → stays on the single `aPos` bound by
    the shared shader helper). FBO **with depth** (the 2D passes have none).
    Same lazy-init + GL state save/restore pattern as `NtscPostProcessor`;
    WebGL2/GLES3 compatible (instancing + VTF + derivatives, no geometry
    shader). Toggle **View ▸ "3D voxel view"** (persisted `show_3d_voxel`),
    wired into `drawScreenImage` before the final blit.
  - **To follow**: orbital camera on mouse drag + zoom (P2), settings panel +
    lighting (P3), resolution steps / heightfield (P4), rewind tie-in
    "freeze + orbit" (P5). The GL render is verified by running the app
    (no golden hash — the camera math, itself, is tested).

## 2026-05-31 (Rewind — delta codec, UI, disk state, heavy cases: phases 2→5)

- **MicroM8-style rewind completed (phases 2 to 5).** The base (phases 0+1,
  below) stored full snapshots; these phases make it actually usable.
  All pinned: `rewind_delta`, `rewind_transport`, `rewind_slot_state` (+
  `rewind_roundtrip` unchanged = the API's regression safety net).
  - **Phase 2 — XOR delta + keyframes codec** (`RewindBuffer`): a full
    keyframe every `keyframeInterval` frames (default 120 ≈ 2 s), XOR deltas
    between (only the modified spans, coalesced over gaps < 16 bytes). 30 s
    goes from ~315 MB to ~10 MB. Reconstruction = nearest keyframe ≤ i + XOR
    of the deltas. **rebase-on-evict** eviction: the front always stays a
    keyframe (the next delta is promoted before dropping). Public API
    unchanged → `rewind_roundtrip` (phase 1) passes as-is = proof of
    non-regression. *Why keyframes+delta rather than reverse-delta alone:
    XOR is its own inverse, so a single delta direction serves bidirectional
    scrubbing, and keyframes bound the cost of random seek.*
  - **Phase 3 — UI + transport + live-rewind**: `Rewind_ImGui` (Devices ▸
    Rewind) — Record toggle, timeline, transport |< / << (hold) / <| / |> /
    resume, history-duration slider; `F6` = hold-to-live-rewind everywhere
    (MicroM8 gesture). Restore served with the **worker parked**:
    `rewindBeginScrub()` sets Stopped then `waitUntilParked()` waits for
    `workerParked_` (set in the worker's Stopped CV wait) → a UI restore can't
    be overwritten by an in-flight Running frame (the Running branch exhausts
    its whole budget before re-checking the mode). `rewindEndAndResume`
    restores + `truncateAfter` (discards the abandoned future) + restarts.
    Ring emptied on `coldBoot`.
  - **Phase 4 — slot card state**: `SlotPeripheral::append/loadSnapshotState`
    (no-op by default); `DiskIICard` serializes its mechanical state + LSS
    (quarter-track head, motor, phase magnets, data register, sequencer,
    rotational timing — **not** the media or the PROMs). `MachineSnapshot`
    writes `SLOTn` sections **only if `includeSlots=true`** (rewind opt-in;
    the AI-control `/snapshot` API keeps its "disk excluded" contract — an
    archive file may survive a media change). The restore routes to the slot's
    card (magic+version → a foreign card ignores a blob that isn't its own)
    and tolerates their absence. A rewind during a disk I/O no longer leaves
    the head on the wrong nibble. Full bit-for-bit machine round-trip (incl.
    SLOT6) pinned.
  - **Post-review hardening** (multi-agent review): (a) the park handshake
    race fixed — `setMode(non-Stopped)` clears `workerParked_` on the setter
    side, otherwise a fast resume→rescrub read a stale flag; (b)
    `DiskIICard::loadSnapshotState` bounds `activeDrive` (index guard); (c)
    history slider disabled during scrub (eviction would shift the indices);
    (d) `SnapshotIO` memory backend rewritten as a zero-copy streambuf
    (`VectorOutBuf`/`ArrayInBuf`) — removes the double-copy via `stringstream`
    on each capture (~21 MB/s on IIe, much more with RamWorks).
  - **Phase 5 — heavy cases**: `maxBytes_` memory budget (default 256 MiB) on
    top of the frame cap → RamWorks (~10 MB/keyframe) bounded (less history
    rather than RAM that explodes). `flushAudioForRewind()` (speaker reset) on
    each restore → a time jump is silent, not a "pop". Capture also wired into
    `tickFrame()` (single-thread WASM path).
  - **Audio chips serialized** (closing the audio gap): `MockingboardCard` and
    `PhasorCard` serialize the register/timer state of their `Via6522` (24 b) +
    `Ay3_8910` (34 b) via the `SlotPeripheral` hook — `append/loadSnapshot`
    helpers shared by both cards, LE packing pooled in `ByteIO.h`. So the music
    survives a rewind (not just the speaker flush). The AY is a register model
    (the synthesis derives from the 16 registers) → restoring the registers
    restores the sound exactly. Pinned `rewind_audio_state` (full bit-for-bit
    machine round-trip incl. Mockingboard).
  - **SSI263 speech serialized**: `Ssi263::append/loadSnapshot` (30 b: 5
    registers + the phoneme read cursor), wired into the Sound II variant of
    `MockingboardCard` → speech also survives a rewind. Covered by
    `rewind_audio_state` (Sound II block).
  - **Disk writes undone on rewind** (Phase 6): the DiskIICard snapshot bumped
    to v2 — it carries the nibble track buffers for the loaded disks that are
    physically writable and non-WOZ
    (`DiskImage::append/loadMediaSnapshot`), so a disk write is undone by a
    rewind. The delta codec keeps the cost ~nil as long as no track is
    written; the read caches re-derive from the restored nibbles. Read-only /
    WOZ / empty disks = 1 flag byte. Pinned `rewind_disk_write` (media COW +
    write-via-card undone end-to-end).
  - **Remaining gap**: writes on a writable WOZ not undone (WOZ stores its bits
    in `wozRaw`, a distinct store; WOZ originals are generally
    write-protected). Tracked cleanly if needed. Detail → `DEV.md` § Rewind.

## 2026-05-31 (Rewind — foundations, phases 0+1)

- **MicroM8-style rewind — capture/restore state base (no UI).**
  - **Why**: record the machine state continuously to allow going back in time
    (scrub/step-back). Architecture choice: a ring-buffer of state snapshots
    (RetroArch-style) rather than deterministic input replay — decoupled from
    the CPU hot-path, robust, and reuses `SnapshotIO` as-is. The delta/keyframe
    (to reduce the ~175 KB/frame cost) is phase 2; here we store full snapshots
    to validate the capture→restore loop bit-for-bit.
  - **Phase 0 — `SnapshotIO` memory backend**: `SnapshotWriter(vector&)` /
    `SnapshotReader(ptr,len)` alongside the existing file backend, via a
    `std::stringstream` bound to a `std::ostream&`/`std::istream&` member
    (all the section/length logic reused). Binary format identical between the
    two backends. Pinned: `snapshot_memory_roundtrip` (round-trip +
    byte-for-byte parity vs the file writer).
  - **`MachineSnapshot.{h,cpp}`**: extraction of the canonical
    `CPU`/`MEM`/`MEX` sequence out of `AiControlServer` (which slims down by
    ~63 lines). Single source of truth shared by the AI-control API AND the
    rewind, so no more possible divergence. The security hardening stays: a
    16-byte length gate on the CPU section (over-read of a forged blob,
    "round 10 #3") + a 16 MiB MEX cap → `RestoreResult{false,…}` (the API
    always returns 400). Covered by `ai_control_server_smoke` (no regression).
  - **Phase 1 — `RewindBuffer.{h,cpp}`**: a `std::deque` ring of full
    snapshots, oldest-first eviction beyond `maxFrames` (default 1800 ≈ 30 s
    @ 60 Hz), `restore(i)` / `restoreToCycle(cycle)`. Capture wired at the
    `workerLoop`'s quiescent frame boundary (after the CPU budget + IWM tick),
    guarded by `enabled()` before taking `stateMtx` → zero cost when disabled
    (default). Pinned: `rewind_roundtrip` (bit-for-bit round-trip + eviction +
    `restoreToCycle` seek).
  - **Assumed gaps this phase**: card/disk state out of the snapshot (a rewind
    during disk I/O leaves the head where the live sim put it → phase 4:
    `SlotPeripheral` hook + `DiskIICard` drive state); audio chips desynced;
    no UI (phase 3); WASM not wired (phase 5). Detail → `DEV.md` § Rewind /
    time-travel.

## Earlier (≤ 2026-05-30) — archived

The entries from 2026-05-30 back to 2026-05-14 (pre-v0.7) are moved into
[`docs/archive/CHANGELOG-2026-05.md`](docs/archive/CHANGELOG-2026-05.md) to
keep this file focused on the current cycle. Full history → `git log`.
