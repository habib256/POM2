# POM2 — Changelog

Notable changes, ordered most recent to oldest. The `git log` remains the
canonical source for the exact mechanics; this file captures the **"why"**
and the pitfalls we don't want to rediscover. Active backlog → `TODO.md`.
Current implementation → `DEV.md`.

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
