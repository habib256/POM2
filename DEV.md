# DEV.md

Implementation notes: MAME refs, non-obvious gotchas, pinned smoke
tests. Orientation + memory map + profile table → `CLAUDE.md`. User
walkthrough → `README.md`. Fix history + rationale → `CHANGELOG.md`.

Convention: each subsystem cites the MAME file + line range it ports
from. When MAME upstream renames a path (e.g. `wozfdc.cpp` `bus/a2bus
→ machine`), refresh refs in a pass.

## Table of contents

- [CPU](#cpu)
- [Memory](#memory)
- [Display](#display)
- [Audio](#audio) · [Mockingboard](#mockingboard) · [Floppy mechanical sounds](#floppy-mechanical-sounds)
- [Slot bus & IRQ aggregation](#slot-bus--irq-aggregation)
- [Storage](#storage) · [ProDOSHardDiskCard](#prodoshardiskcard-hdv-synthetic-block-model) · [CffaCard](#cffacard-cffa-20--mame-faithful-ide) · [SmartPortCard](#smartportcard-e-liron-class)
- [IWM (//c+ on-board)](#iwm-c-on-board)
- [SmartPort 3.5" stack](#smartport-35-stack)
- [Peripherals](#peripherals) · [SSC](#super-serial-card-slot-2--telnet-bridge) · [ClockCard](#prodos-clock-card-slot-4) · [MouseCard](#mouse-card) · [Joystick / paddles](#joystick--paddles)
- [UI (ImGui)](#ui-imgui)
- [Host control center](#host-control-center-slot-configuration--floppy-emu)
- [Profile switching internals](#profile-switching-internals)
- [CLI (CliDispatcher)](#cli-clidispatcher)
- [Clock & threading](#clock--threading)

## CPU

Full NMOS 6502 + 65C02 (STZ / BRA / INA / DEA / PHX-PLY / BIT-imm /
TSB / TRB / JMP (abs,X), zp-indirect) + Rockwell RMB/SMB/BBR/BBS +
WDC WAI/STP (PC parks, IRQ wakes). Klaus Dormann clean.
`setCpuMode(NMOS)` re-overrides four KIL column-2 entries
($02/$22/$42/$62) back to halt, and remaps `$0B/$2B/$EB` as
`UnoffImm` (2-byte NOPs). 65C02 undoc-NOP cycles: imm=2, zp,X=4,
abs,X=4, zp=3 ($5C left at 8). Pinned: `cmos_6502_smoke_test`,
`klaus_65c02_extended_test` (PASSES @ `$24F1`),
`cpu_cycle_count_test`. `setProgramCounter()` is the Klaus harness
back-door.

## Memory

### `loadAppleIIRom` dump shapes

- **16 KB**: `$C000-$FFFF` direct (MAME/AppleWin).
- **20 KB** II+ system pack: 4 KB filler skipped (loader). Pinned
  `system_profile_smoke::test20kIIPlusRomLoad`.
- **32 KB** //e "system+video": firmware at offsets `0x4000-0x7FFF`;
  lower half = video/charset (`loadCharRom`). `pickLower16KFor32K=false`.
- **32 KB** //c/+ dumps: TWO 16 KB banks side-by-side (bank 0 lower
  = cold-reset entry; bank 1 upper = alt firmware via `$C028`
  ROMBANK). `pickLower16KFor32K=true`; upper stashed into
  `iicAltFirmware`. Both halves carry valid-looking reset vectors —
  profile is source of truth.

### //c-class detection (MAME `apple2e.cpp:1275-1299` content probe)

- `payload[0x3BC0]==0x00` → `isIIcClass=true` (forces INTCXROM on
  every reset; //c has no physical slots). Fires for both 16 KB
  rev-255 AND 32 KB rev-0/3/4/X //c+ dumps.
- `payload[0x3BBF]==0x05` (after //c match) → `isIIcPlus=true`
  (gates on-board IWM + MIG). Plain //c uses `A2BUS_DISKIING` at
  slot 6 (MAME `apple2c()` `apple2e.cpp:5168-5188`).
- `iicHasAltBank` is narrower: true only on 32 KB dumps providing
  an alt-firmware bank. 16 KB rev-255 //c has `isIIcClass=true` but
  `iicHasAltBank=false`.

### MemoryProfile (//c-class strategy)

All //c/+ memory quirks behind `MemoryProfile`
(`MemoryProfile.h` + `MemoryProfile_IIcClass.{h,cpp}`).
`Memory::iicProfile_` is **null on II/II+/IIe** → one `if
(iicProfile_)` branch on the hot path, zero virtual calls. Profile
owns: alt firmware (16 KB), ROMBANK flag, //c+ flag, 2 KB MIG
gate-array (`migRead`/`migWrite`, verbatim MAME
`apple2e.cpp:532-624`), IWM/SmartPort hub pointers. Dispatcher
delegates: `forcesIntCxRom`, `romBankToggle($C028)`,
`onResetSoftSwitches`, `ioReadIWM/ioWriteIWM ($C0E0-$C0EF)`,
`internalRomRead/Write` ($C100-$CFFF under INTCXROM, incl. //c+ MIG
`$CC00/$CE00` + alt-firmware bank 1), `languageCardRomRead`
($D000-$FFFF alt firmware). `Memory::setIWM/setSmartPortHub/
setIWMAuthoritative` are façades. What stays in Memory: `ioudis`
(shared with //e), `intC8Rom`, LC/paging/generic soft switches.
Pinned: `iic_boot_trace`, `iic_nodisk_boot_trace`,
`iicplus_boot_trace`, `system_profile_smoke`, `iwm_device_smoke`.

### IIe paging

`setIIEMode(true)` MUST be called BEFORE `loadAppleIIRom` (loader
split depends on the flag). Adds aux 64 KB, 4 KB `internalIORom`
for `$C100-$CFFF`, aux LC bank trio. `$C000-$C00F` switches update
`iieMemMode` bitmask; per-range routing (ALTZP `$00-$01FF`,
RAMRD/WRT `$02-$BFFF`, 80STORE+PAGE2 swap on `$04-$07FF` +
`$2000-$3FFF` when HIRES). All IIe paths gated behind `iieMode`.
Pinned: `iie_memory_smoke_test`.

### RamWorks III

Verbatim port of MAME `bus/a2bus/a2eramworks3.cpp`. Tiers 1/4/8/16/
48/128 (8 MB cap, MAME `:99-107`). Bus (MAME `:108-115`): writes to
`$C0n1/3/5/7` (predicate `(low & 0x09) == 0x01` over
`$C070-$C07F`) latch `bank = data & 0x7F`. Same accesses still
pulse paddle one-shot mirror.

Storage = `ramWorksBacking_`, one 80 KB slot per bank
(`kRamWorksBankStride = 0x10000 + 0x1000 + 0x1000 + 0x2000`).
Visible aux* arrays always hold the active bank (`Apple2Display`
caches `auxData()` once). `ramWorksSwapToBank` memcpys
visible→backing[prev] then backing[curr]→visible.

Bank clamp `(data & 0x7F) % ramWorksBanks_` (MAME doesn't clamp,
allocates 8 MB always). IIe-only: `setIIEMode(false)` releases
backing. Wired in `applyProfile` between `setIIEMode(true)` and
`loadAppleIIRom`. Pinned: `ramworks_smoke_test`.

### Soft switches

Read OR write triggers. `$C030` toggles speaker on every access in
`$C030-$C03F`. `$C061-$C067` are paddles + buttons on II/II+ —
NOT cassette aliases (only `$C020`/`$C060` are). Reads of
`$C050-$C05F` return `floatingBus()`, not 0 (some RNG / anti-copy
code samples those).

**Open-Apple/Solid-Apple** OR'd into $C061/$C062 bit 7 alongside
joystick buttons (MAME `apple2e.cpp:2157-2169`); wired to host
Left/Right Alt (`Memory::setOpenAppleKey/setSolidAppleKey`); GLFW
key callback routes those even when ImGui has focus.

**IOUDIS** (`$C07E` SET / `$C07F` CLR + //c mirrors `$C078/$C079`).
Init `true` every reset (MAME `apple2e.cpp:1224`). Writes effective
only on `isIIcClass` (MAME `:2569-2587` gates `m_isiic`). Read
`$C07E` on any IIe-class returns bit-7 = ioudis state (MAME
`:2276-2278`).

**LC reset state**: `lcWriteEnable=true`, `lcReadRam=false`,
`lcBank2Active=true`, `lcPrewrite=false` (Sather Fig 5.13; MAME
`apple2e.cpp:1227-1232 + :1492-1497`). Applied universally.

### Power-on RAM pattern

MAME-faithful `00 FF 00 FF …` fill (`Memory::clearRam()` on user
RAM + LC + aux + RamWorks). MAME refs: `apple2.cpp:294-298` (II/+),
`apple2e.cpp:1014-1035` (IIe). Done at power-on / profile switch /
cold boot only; soft + hard resets preserve RAM.

### Text/HGR row interleave (Woz DRAM-refresh trick)

- text: `addr = base + 0x80*(y%8) + 0x28*(y/8)`
- HGR:  `addr = base + 0x400*(y%8) + 0x80*((y/8)%8) + 0x28*(y/64)`

### Keyboard

Latch + strobe under `kbMutex`. UI `queueKey()` sets strobe high.
CPU reads `$C000` via `softSwitchAccess()` (same mutex). Strobe
stays high until `$C010`.

### Reset architecture

- **`resetSoftSwitches()`** — full reset: display state, LC flags,
  `iieMemMode`, `intC8Rom`, `iicRomBank`, IOUDIS=true, RamWorks
  bank 0. Forces `MF_INTCXROM` when `isIIcClass`. Called by
  `coldBoot()`, `hardReset()`, `applyProfile` step 4, and
  `resetSoftSwitchesWarm()` when `iieMode` is on.
- **`resetSoftSwitchesWarm()`** — Ctrl-Reset only. On `iieMode`
  delegates to full reset (MAME `apple2e.cpp:1453-1508 reset_w`
  wipes everything every time). On II/II+ does only keyboard-strobe
  clear (MAME `apple2.cpp:325-331`).

CPU side: `M6502::hardReset()` doesn't wipe stack `$0100-$01FF`
(MAME `reset_w` doesn't touch RAM); `M6502::softReset()` decrements
SP by 3 (faked-BRK reset semantic).

### Test/debug write helpers

`Memory::dataMutable()` is gone — the raw pointer let a stray poke
silently clobber ROM. Replacements: `writeRamUnchecked(addr, val)`
(`assert(addr < 0xC000)`, bypass IIe paging → main bank) for
targeted RAM pokes; `loadFlatTestImage(src, len)` (asserts
`testMode == true`) for Klaus 64 KB bulk loads.

## Display

Pure software renderer into 280×192 (or 560×192 in IIe 80-col)
RGBA. Reads `Memory::getDisplayState()` (mutex copy) + flat RAM.
UI uploads via `glTex(Sub)Image2D`. Text flash via
`frame_number() & 0x10` (MAME parity).

Nine `HiResMode`:
- `ColorNTSC` — 14 KB LUT `(parity<<8)|byte`, 39 seam fix-ups,
  glow (MAME `composite_color_mode=0`).
- `ColorCompMedium` (=1), `ColorComp4Bit` (=2, no artifact).
- `ChatMauveRGB` — only with `LeChatMauveCard`.
- `ColorCompositeOE` — OpenEmulator-style true NTSC simulation
  via GLSL shader (see § Composite NTSC shader below).
- `MonoWhite` / `MonoGreen` (P31) / `MonoAmber` (history-buffer
  lerp).
- `ColorAppleWin` — AppleWin-style IIR-based NTSC simulation
  via 4-phase × 4096-entry CPU LUT (see § AppleWin NTSC below).

The deep per-mode comparison with each origin source — algorithm
provenance, deviations, pinned tests and side-by-side captures —
lives in [`docs/graphics_modes_comparison.md`](../docs/graphics_modes_comparison.md).

### DHGR (IIe, `eightyCol && hiRes && dhgr && !textMode`)

`renderDhgr` interleaves aux (dots `c*14..+6`) with main (`+7..+13`)
per byte → 560-dot stream. Three color paths, matching MAME
`apple2video.cpp`:

- **`ColorNTSC`** — composite artifact: 7-bit sliding window over
  560 dots → `kArtifactColorLut[128]` → `rotl4b(value, absX+1)` →
  4-bit lo-res palette. `+1` = MAME `is_80_column=1` in
  `render_line_artifact_color`. Per-pixel decode.
- **`ChatMauveRGB`** — Video-7 / Le Chat Mauve. 2-bit AN3 FIFO
  (`LeChatMauveCard::currentMode()`) picks one of four MAME
  `dhgr_update` rgbmodes (POM2 enum == MAME rgbmode):
  - `COL140`(3): 4-dot block → raw nibble → `rotl4(n,1)` →
    `kChatMauveLoResPalette`.
  - `Mixed`(1): two cols as 28-bit word; each **source byte's MSB**
    picks color vs 7-dot mono (MAME `:946-977` `color_mask`).
  - `Chunky160`(2): `aux+(main<<8)` → four 4-bit pixels of three
    dots each, 480 wide centred in 560 with 40 black margins (MAME
    `:906-930`).
  - `BW560`(0): plain mono DHR.
  Palette verbatim AppleWin `PaletteRGB_Feline`; MAME's Video-7
  collapses idx 5≡10, POM2 follows AppleWin (intentional).
- **`Mono*`** — luminance × tint; persistence sized for 280-wide
  HGR.

Mixed = DHGR top 160 + 80-col text bottom 4 rows.

**Video-7 fg/bg colored TEXT** (`renderTextChatMauveFgBg`): 40-col
text with RGB card + DHGR (AN3) on — char code from main, per-cell
fg/bg from aux at same text address (hi nibble = fg, lo = bg);
7-bit glyph doubled to 14 dots. Port of MAME `text_update`
(`:788-791`) + `render_line_color_array` (`:571-583`).

Pinned: `dhgr_render_smoke_test`, `video7_parity_smoke_test`
(all 4 rgbmodes + fg/bg vs self-contained MAME oracle).

### 80-col text

Aux RAM (cells 0,2,…) interleaved with main (1,3,…) into 560-wide
frame. Mixed (HIRES+80COL+MIXED): HGR top 20 rows doubled, 80-col
rows 20..23 overlay. ALTCHAR plumbed but no-op against built-in
fallback.

### Composite NTSC shader (`ColorCompositeOE`)

OpenEmulator-inspired GPU pass: instead of decoding to RGB on the
CPU, `Apple2Display::fillCompositeSignal()` serialises the active
video mode (HGR / DHGR / 40-col text / 80-col text / 40-col lo-res)
into a 1-bit 14.318 MHz luminance waveform — 560 samples × 192
lines, one byte per sample (`signalBuf`). HGR reuses the existing
`buildBitStream()` so the per-byte half-dot delay is preserved.
Lo-res emits `(nibble >> (absX & 3)) & 1` at every sample, letting
the shader's NTSC demodulator recover the 16 colours from the same
spectral mechanism a real CRT uses (no palette lookup).

`MainWindow::drawScreenImage()` uploads `signalBuf` to an `R8` GL
texture and runs `NtscPostProcessor::process()`. The fragment shader
(`NtscPostProcessor.cpp` `kFragmentShader`):

1. Optional barrel distortion of UVs.
2. For each output fragment, 17-tap accumulation of signal taps through
   **OpenEmulator-exact FIR kernels** — a Dolph-Chebyshev(50 dB) window ×
   sinc lowpass, reproduced with libemulation's own realIDFT recipe
   (`OEVector::chebyshevWindow`/`lanczosWindow` + `OpenGLCanvas.cpp`) at
   the *AppleColor Composite Monitor IIe* config (luma 2.0 MHz, chroma
   0.6 MHz, Y'UV). Hard-coded as 9 symmetric coeffs each:
   - **`lumaK`** (sum 1) **notches fs/4** (`|H(0.25)|` ≈ 0.002, −3 dB ≈
     1.64 MHz), killing the dot-crawl the old gaussian (sigmaY 0.8,
     `|H(0.25)|` ≈ 0.46) produced.
   - **Chroma** (sum 2 = the ×2 demod gain): the **Sharpness** knob blends
     the OE-faithful soft kernel (0.6 MHz — *exactly* OE at Sharpness 0)
     ↔ a sharp 2.0 MHz kernel. Both reject fs/4 (`|H(0.25)|` ≈ 0.0004 /
     0.004), so every blend keeps DC = 2 and stays subcarrier-clean.
   The CPU path (`Apple2Display::renderCompositeOeCpu`) mirrors `lumaK`
   and uses the OE-faithful 0.6 MHz chroma (no Sharpness param there).
3. Chroma is recovered by multiplying each tap with
   `sin(π/2 · x)` and `cos(π/2 · x)` — Apple II's 4× subcarrier
   alignment means phase is just the dot index.
4. YIQ → RGB via the standard NTSC matrix, then **hue** rotates the
   IQ vector, **brightness**/**contrast**/**saturation** apply
   in RGB space.
5. **Persistence** is a `max(decoded, prev * decay)` blend with the
   previous output frame held in a ping-pong FBO.
6. **Scanlines** darken odd output rows (output texture is 2× the
   signal height); the leftover **barrel** factor curls UVs at the
   edges.
7. Optional **shadow mask** post-effect: procedural RGB-stripe mask
   (`Triad` / `ApertureGrille` / `Dot`) multiplied into the pixel
   after demodulation. No texture upload — driven by `mod(outX, 3)`
   so the cost is one branch + one vec3 multiply per pixel. `Dot`
   alternates triplet phase every other row for the quincunx look.
8. Optional **PAL composite** mode: flips the sign of the Q chroma
   tap on odd scanlines. Approximates PAL's line-phase alternation
   (the cancellation of hue errors at the cost of vertical chroma
   resolution). NTSC mode by default.

**Sharp-text bypass.** TEXT under composite is faithful to a real
CRT but blurry — fine for nostalgia, awkward for everyday use. The
`textSharp` knob makes `MainWindow::drawScreenImage()` skip the
shader for the whole text screen and draw the crisp RGB framebuffer
instead. Toggled live in the CRT Settings panel; on by default.

`OpenGLShader.cpp` provides the small `compileShaderProgram()` helper
+ a lazy `glfwGetProcAddress` table on Linux/Windows (macOS gets
GL 3.x from `<OpenGL/gl3.h>`, Emscripten from `<GLES3/gl3.h>`). The
shader source is single-pass, gated on `#version 150` (desktop) /
`#version 300 es` (WebGL2). No OpenEmulator / libemulation code is
copied — the implementation is rewritten from the public NTSC spec
(FCC/CCIR §73.682) and the openemulator-explainer notebook by
Zellyn Hunter (algorithm description only).

All knobs persist under settings.json keys `ntsc_brightness`,
`ntsc_contrast`, `ntsc_saturation`, `ntsc_hue`, `ntsc_sharpness`,
`ntsc_persistence`, `ntsc_scanlines`, `ntsc_barrel`,
`ntsc_shadow_mask` (int 0..3), `ntsc_shadow_strength`, `ntsc_pal`,
`ntsc_text_sharp`. The CRT Settings panel (View → CRT Settings)
drives them live.

If shader compilation fails (driver too old, GLES2-only context,
…), `NtscPostProcessor::available()` returns false and POM2 silently
falls back to the regular `ColorNTSC` LUT framebuffer for the mode —
the menu entry stays usable but the result is indistinguishable
from `ColorNTSC` until the GL state catches up.

> **Note:** since the Phase-4 split, `NtscPostProcessor` is **demod-only**
> (steps 2–4, 8 above). The CRT *glass* — barrel geometry (step 1),
> persistence (5), scanlines (6) and shadow mask (7) — moved to the shared
> `CrtEffectStack` (below), so OE chains into it like every other mode.

### Universal CRT effect stack (`CrtEffectStack`)

`src/CrtEffectStack.{h,cpp}` applies the CRT glass on top of *any* RGBA
framebuffer (MAME LUT, Chat Mauve, mono, AppleWin) — gated by "CRT effects
on all modes" — and is the single effect implementation OE also chains into.
Effect order in the fragment shader: barrel → hue → BCS → scanlines →
shadow mask → center-lighting (vignette) → luminance gain → edge-mask →
persistence (ping-pong FBO, applied last so the afterglow isn't re-attenuated
by the glass each frame). The scanline→mask→lighting→luminanceGain ordering
matches OpenEmulator's display shader (`OpenGLCanvas.cpp:117-126`).

**Glass details (2026-05 parity pass).**
- **Hue** is applied here (RGB→YUV BT.601, rotate U/V by `hue·π`, YUV→RGB) so
  the knob works on every mode, not just OE. The OE demod already rotates hue,
  so MainWindow passes `hue = 0` to the stack on the OE path (no double spin).
- **Shadow mask** uses the Lottes dark/light triplet (off-channels → 0.5, lit
  channel → 1.5) so the triad preserves average luminance, instead of the old
  pure-primary `(1,0,0)` mask that crushed 2/3 channels and over-darkened.
- **Luminance gain** (`luminanceGain`, default 1.0) re-brightens post-mask,
  mirroring OpenEmulator's stage — pairs with scanlines/mask to recover
  brightness.
- **Center lighting / vignette** (`centerLighting`, default 1.0 = flat, OE's
  Apple II default): `lighting = cuv·(1/cl − 1); rgb *= exp(−dot(lighting))`,
  verbatim OpenEmulator. Lower values darken the edges.
- **Persistence** carries a `−0.5/256` noise floor (OpenEmulator) so faint
  trails decay fully to black instead of lingering at the quantization step;
  the slider stays a per-frame retention factor (POM2's documented model,
  not OE's seconds time-constant).
- **Not ported (intentional):** OpenEmulator and POM2 both run the glass in
  gamma space, so crt-lottes-style linear-light is a *beyond-OE* option, not a
  parity gap — left out.
- **Defaults are deliberately punchier than OpenEmulator** (`scanlines 0.25`,
  `shadowMaskStrength 0.5`, `persistence 0.4` vs OE's ~0.05/0.05/0). This is an
  intentional product choice (a visible CRT look out of the box), not an
  oversight; the dark/light mask keeps strength 0.5 tasteful. OE-faithful 0.05
  values remain available via the sliders.

**Anti-moiré (2026-05).** Barrel distortion warps the UVs non-linearly; the
scanline (period = 2 source-rows) and shadow-mask (period = 3 units) patterns
are high-frequency, so where the warp compresses the picture they exceed the
output Nyquist and alias into moiré "lines". Two-part fix:

- `MainWindow::drawScreenImage()` computes the on-screen target size **up
  front** and passes it to `CrtEffectStack::process(src, srcW, srcH, dstW,
  dstH)`, which renders the pass at **native output resolution** (decoupled
  from the source dims, which now only drive the pattern *frequency* via
  `uSrcSize`). ImGui then blits the result 1:1 — no second resample beat.
- The shader **analytically anti-aliases** the patterns: `fwidth()` of the
  scanline/mask coordinate measures how many pattern-units one output pixel
  spans; as that approaches Nyquist (which is exactly where the warp
  compresses) the modulation fades smoothly to neutral instead of moiréing.
  Scanlines also use a smooth `cos` beam rather than a hard `fract` edge, and
  the curved barrel border is a soft `fwidth`-based edge mask (no jaggies).

Inspect via the offscreen diagnostic `tests/crt_barrel_view`
(`EXCLUDE_FROM_ALL`): renders a barrel + scanline + mask test (optional PPM
source) to `/tmp/crt_barrel_{on,off}.ppm`. No CI hash — the GL path is
FP/driver-dependent, so it's eyeballed, not pinned.

### AppleWin NTSC (`ColorAppleWin`)

**Faithful port** of AppleWin's CPU-side NTSC composite simulation
(`source/NTSC.cpp::initChromaPhaseTables`, by Sheldon Simms / Tom
Charlesworth / Michael Pohoreski — GPL v2+). Per the project convention
(AppleWin = source of truth) the algorithm, IIR filter coefficients
(`NTSC.cpp:115-132`), YIQ→RGB matrix and white/black/grey special-casing
are ported line-for-line and cited inline in `src/AppleWinNtsc.cpp`.

Consumes the same 14.318 MHz luminance bitstream `fillCompositeSignal`
generates for `ColorCompositeOE`. Decoding happens through static
`[4][4096]` phase tables built once at first use by
`AppleWinNtsc::ensureInitialized()`:

- For each (colour phase 0..3, 12-bit signal history): walk the 12 bits
  *oldest first*, **2× oversampled** (`phi += 45°` per half-step, 90°
  per dot — Apple II's 4× subcarrier alignment), through three cascaded
  2-pole IIR filters: `initFilterSignal` (input low-pass),
  `initFilterChroma` (band-pass @ fs/4 — the inverted-`x[0]` zero is what
  actually isolates chroma), `initFilterLuma0/1` (luma low-pass).
- Quadrature-demodulate chroma (cos→I, sin→Q, single-pole `/8`
  smoothing), then YIQ→RGB (FCC matrix). `y0` → Monitor table; `y1`
  (luma of *signal − chroma*, a comb) → Color-TV table.
- Runtime is a pure causal 12-bit shift register + one LUT lookup per
  dot (`NTSC.cpp:331`), no window-centring.

> **Why the rewrite (2026-05):** the prior gaussian-moving-average
> approximation computed luma with a window too narrow to notch the
> subcarrier, so luma absorbed the subcarrier and `signal − luma`
> cancelled chroma inside steady colour fills — the "almost no colour"
> bug (only edge fringes survived). The dedicated band-pass fixes it.

Three sub-modes via `Apple2Display::AppleWinSubMode`:

- **Monitor** — `g_hueMonitor` (luma y0). Sharp, full composite artifacts.
- **TV** — `g_hueColorTV` (comb luma y1) + 50% blend with the previous
  frame's same scanline (`appleWinPrev80`), approximating phosphor
  persistence + comb-filter blur of a consumer TV.
- **Idealized** — POM2-only (no AppleWin equivalent): Monitor luma with
  chroma boost ×1.6 for a punchy flat-panel look.

`CYCLESTART = 45°` aligns hues to the MAME reference out of the box (no
extra phase calibration); `rebuildForPhase()` adds an offset for the
render tool's sweep.

Pinned by `applewin_ntsc_smoke` (idempotent init, all-black/all-white
sanity, $7F neutral luma, **$2A solid-fill saturation guard** — the
regression test for the no-colour bug, Idealized artifact non-black, Tv
convergence, multi-line wrapping).

Full mode-by-mode comparison vs MAME / OpenEmulator / hardware lives
in [`docs/graphics_modes_comparison.md`](../docs/graphics_modes_comparison.md).

### Test framework gotcha

Tests inherit parent's `-O3 -DNDEBUG` → would strip `assert()`.
`tests/CMakeLists.txt` adds `-UNDEBUG`.

## Audio

`AudioDevice`: miniaudio mono float32. **OS-negotiated sample rate**
(often 48 kHz on Apple Silicon) — cycle-driven sources MUST query
`getActualSampleRate()`.

### Speaker

`SpeakerDevice` (`AudioSource`). Verbatim MAME `spkrdev.cpp:74-327`.
CPU records each `$C030-$C03F` toggle with sub-instruction timestamp
(`cycleCounter + cpu->getCurrentInstructionCycles()`) into 16 K ring.
Audio thread: rectangle integration → 4× oversample → 64-tap windowed
sinc (cutoff sr/4) → 0.995-pole DC blocker. Auto catch-up if drain >
100 ms.

### Cassette

`$C020` output toggle / `$C060` input comparator sign. Separate
`AudioSource`. `CassetteDeck_ImGui` uses Font Awesome
(`fonts/fa-solid-900.ttf`), falls back to `?` if missing.
Auto-rewind 500 ms is opt-in, default off.

### Mockingboard

Sweet Microsystems: two 6522 VIAs each driving an AY-3-8910. No ROM
— VIAs decoded in slot ROM window (`$Cn00-$Cn0F` VIA#1,
`$Cn80-$Cn8F` VIA#2) via `slotRomWrite`.

VIA → AY (Sweet, AppleWin `Mockingboard.cpp:193`):
```
Port A       → AY data bus (D0..D7)
Port B bit 0 → AY BC1
Port B bit 1 → AY BDIR
Port B bit 2 → AY /RESET (active low; 1 = running)
```
{BDIR,BC1}: 00=INACTIVE, 01=READ, 10=WRITE, 11=LATCH-ADDR. Drivers
emit PB = `$07 → $04 → $06 → $04`. PB2 stays high; `/RESET` only on
PB=`$00`.

**6522 subset**: A/B + DDR, T1 (latch + counter, one-shot +
continuous), IFR/IER (T1 bits 6/7; bit 7 dynamic from `ifr & ier &
0x7F`). T1CL read clears `IFR.T1`. T1L-H ($07) does NOT clear
`IFR.T1` (real 6522 only T1C-L or T1C-H do). IER bit 7 set-vs-
clear (`$C0` enables, `$40` disables). T2/SR/PCR/CA1/CB1 not
modelled (music drivers use T1 only).

**AY-3-8910 synthesis** runs on audio thread inside inner
`AudioSrc`. CPU updates regs under `mtx`; callback snapshots both
banks (32 B), releases, synthesises lock-free. Tone counters =
integer at `clockHz/8/sampleRate`; 17-bit LFSR `x^17 + x^14 + 1`;
envelope 32 steps with R13 shape continue / attack / alternate /
hold. Both chips → mono.

Each VIA `irqOut() = (ifr & ier & 0x7F) != 0`; OR'd onto slot IRQ.

**Lazy timer sync** (`syncToCpuCycle()`): every `slotRomRead/Write`
catches VIAs up to `cpu_->getCycleCountNow()` first. **Gotcha**:
`advanceCycles` syncs to `getCycleCountNow() - cycles`, not
`now` — `cycleCounter` is bumped before slot dispatch but
`cpu->cycles` hasn't been zeroed yet, so the naive `now`
over-counts by one instruction (broke Nox/Skyfox/Broadside T1 IRQ
detection until 2026-05-25). Pinned:
`mockingboard_sync_smoke::testNoEndOfStepOvershoot`.

**Tear-down**: remove `AudioSource` from `AudioDevice` BEFORE
destroying the card. Persisted: `mockingboard_volume`,
`mockingboard_muted`. Pinned: `mockingboard_smoke`,
`mockingboard_sync_smoke`.

### SSI263 + Echo+ (Street Electronics)

`pom2::Ssi263` (`Ssi263.h/.cpp`) — Silicon Systems Inc. SSI263A
phoneme speech synth, shared chip model used by both
`EchoPlusCard` (standalone, slot ROM at `$Cs00-$Cs04`) and
`MockingboardCard` `Variant::SoundII` (chip at `$Cs40-$Cs44`,
A/!R wired to VIA1.CA1).

**No MAME reference**: MAME does NOT implement the SSI263 (verified
2026-05-27 — no `ssi263*` file in `src/devices/sound`). The canonical
reference is AppleWin `source/SSI263.cpp`. POM2's chip emulation is
independent code modelling the same protocol contract.

Register layout (5 registers at $00..$04 within the chip's window):

```
$00 DURPHON  bits 7:6 = mode (00=IRQ disabled, 01=frame imm. infl.,
                              10=phon. imm. infl., 11=phon. trans. infl.)
             bits 5:0 = phoneme code (0..63; 62 defined)
$01 INFLECT  inflection value
$02 RATEINF  bits 7:4 = rate (playback speed)
             bits 3:0 = inflection low
$03 CTTRAMP  bit 7    = CTL (1 = power-down/silent; 0 = run)
             bits 6:4 = articulation
             bits 3:0 = amplitude
$04 FILFREQ  filter frequency (formant 4 cutoff)
```

Reading any register returns a status byte with **bit 7 = A/!R**
(Acknowledge / not Request) — high while the chip is requesting the
next phoneme. The CPU clears A/!R by writing to one of $00..$02 (also
de-asserts IRQ). Writes to $03 (CTTRAMP) or $04 (FILFREQ) do not ack.

CTL H→L transition (power-down exit) restarts the loaded phoneme
without bumping `phonemeWriteCount`. CTL L→H clears A/!R + silences.

**Phoneme duration formula** (AppleWin parity):
```
ms = ((16 - (rate>>4)) * 4096 / 1023) * (4 - (dur>>6))
cycles = ms * POM2_CPU_CLOCK_HZ / 1000
```
Range: ~4 ms fastest (rate=15, dur=3 → ~4090 cyc), ~256 ms slowest
(rate=0, dur=0 → ~262k cyc). Pinned by `ssi263_smoke`.

`MODE_IRQ_DISABLED` (mode 00) suppresses the **host IRQ only** — A/!R
(D7) is still asserted on phoneme completion (AppleWin `SSI263.cpp`
~line 724: D7 is raised regardless of the DR1:0 mode bits; only
power-down holds it low). So polling drivers that select mode 00 and
watch the D7 status bit to detect phoneme-complete still work. On
completion the duration counter parks at 0 (it does not re-tick), so
the chip is quiescent until the next DURPHON write — it does **not**
"repeat". `Ssi263::advance()` returns `irqEnabled()` (gates the host
IRQ edge) but always sets `aRequest_`. Pinned by `ssi263_smoke`
(`testIrqDisabledMode`).

#### MockingboardCard Variant::SoundII

`MockingboardCard` accepts a `Variant` constructor parameter
(default `AC`). With `Variant::SoundII` an `Ssi263` is instantiated
and slot ROM decode carves $40-$4F (5 SSI263 regs + 3 mirrors) out
of the VIA1 mirror range — so the same card surfaces the A/C
VIAs at $Cs00-$Cs0F + $Cs80-$Cs8F AND speech at $Cs40-$Cs44, the
exact layout of real Sound II hardware.

SSI263 A/!R wires (inverted) into VIA1.CA1 → on each phoneme-end
edge, `advanceCycles` calls `via_[0]->setCa1NegativeEdge()` which
latches `IFR.CA1` if `PCR.0 == 0` (the AppleWin-faithful default
config used by Sound II drivers). Once the host CPU enables
`IER.CA1`, the slot IRQ asserts → music driver's IRQ handler
dequeues the next phoneme.

Catalog key `mockingboard_c` selects this variant in Slot
Configuration; `mockingboard` keeps the vanilla A/C decode (no
SSI263, ssi_ stays null). The Mockingboard UI panel grows an
SSI263 section at the bottom only when `hasSsi263()` returns
true.

Pinned by `mockingboard_smoke::testSoundIIVariantSSI263` —
verifies no-SSI263 on AC variant, register decode at $40-$4F,
A/!R → IFR.CA1 latching, IER.CA1 → slot IRQ.

#### EchoPlusCard (Cricket / SSI263-class — catalog `echoplus`)

`EchoPlusCard` (`EchoPlusCard.h/.cpp`) — single-SSI263 card at
$Cs00-$Cs04, A/!R wired directly to the slot IRQ line. No 6522. Open
bus ($FF) for the rest of the slot ROM page.

**Naming caveat** — historically labelled "Echo+" in POM2's UI and
settings, but the markadev/AppleII-RevEng audit (2026-05-28) confirms
the real Street Electronics ECHO+ used 2× AY-3-8913 + TMS5220, not the
SSI263. The SSI263-based Street Electronics product was the Cricket.
The catalog key stays `"echoplus"` for `settings.json` back-compat;
the user-visible label is now "Cricket / Echo (SSI263)". See
[§ EchoPlusTMS5220Card](#echoplustms5220card) for the real Echo+ chipset.

`advanceCycles` ticks the chip and asserts slot IRQ on A/!R edge;
host writes to $00/$01/$02 release the IRQ. Default slot 4, pluggable
in any slot via Slot Configuration. Pairs naturally with a
Mockingboard A/C at slot 4 + Echo+ at slot 2 (the standard "MB for
music, Echo+ for speech" combo).

**Audio**: live. `Ssi263::fillAudio` pulls samples from the 62-phoneme
PCM blob in `Ssi263PhonemeData.cpp` (~313 KB, ported verbatim from
AppleWin `source/SSI263Phonemes.h` — LGPL → GPL3 compat), resamples
from the chip's native 22050 Hz to the host audio rate via a simple
linear cursor, scales by the AMP register (R3[3:0]). Power-down
(CTL=1) and the `FILTER_FREQ_SILENCE` sentinel ($FF in R4) squelch
output. The audio thread reads the chip's register banks + playback
cursor under the host card's mutex. Pinned by `ssi263_smoke` test 6
(RMS > 0.005 on a real phoneme; 0.0 in both squelch paths).

**UI**: Devices → Echo+ panel. Mode + IRQ enable + A/!R + power-down
state + current phoneme + duration countdown (cycles + ms) + the 5
register banks.

#### EchoPlusTMS5220Card

`EchoPlusTMS5220Card` (`EchoPlusTMS5220Card.h/.cpp`) — Street Electronics
ECHO+ **as actually shipped**: 2× AY-3-8913 PSGs + TMS5220 LPC speech
chip. Distinct from the SSI263-based `EchoPlusCard` above. Catalog
key `"echoplus_tms"`, default slot 2.

**v1 scaffold — chip cores deferred.** The card registers on the slot
bus with a stub register decode at $Cs00-$Cs0F so software that probes
for the chipset finds something coherent (not open bus). TMS5220 LPC
decoding (chirp ROM, K-parameter interpolation, energy/pitch tables)
and AY synth are both stubs — audio is silent. The provisional address
map (pin to markadev's schematic on next pass):

```
$Cs00  TMS5220 status / data (rd = status, wr = command/data byte)
$Cs01  TMS5220 stop / reset
$Cs04-05  AY-3-8913 #1 (address latch / data write)
$Cs06-07  AY-3-8913 #2 (address latch / data write)
$Cs08-FF  open bus
```

Source: markadev/AppleII-RevEng/Street-Electronics-Corp-ECHO+ (index.md
states "two AY-3-8913 Programmable Sound Generator chips and a TMS5220
Speech Synthesizer chip").

### Phasor (Applied Engineering)

`PhasorCard` (`PhasorCard.h/.cpp`) — dual-mode successor to the
Mockingboard. 2× 6522 VIA + 4× AY-3-8913 PSG (12 voices). Same VIA +
AY hardware as Mockingboard (verbatim from `Via6522.h` + `Ay3_8910.h`,
extracted 2026-05-27 specifically so the two cards share the same VIA
timing + AY register-bank decoder).

Address map (s = slot, slotHi = $C0+s):

```
$Cs00..$Cs0F   VIA1   (drives AY1 / AY2)
$Cs10..$Cs7F   VIA1 mirrors (partial decode)
$Cs80..$Cs8F   VIA2   (drives AY3 / AY4)
$Cs90..$CsFF   VIA2 mirrors
$C0(8+s)0..F   Mode soft-switch (responds to BOTH reads and writes)
```

**Mode soft-switch** (AppleWin rules):
- Read OR write to `$C0(8+s)X` triggers the update — the address (not
  the data) drives the mode bits.
- `if offset & 0x8`: clear mode bits 2:0
- `mode |= offset & 0x7`
- Power-up = `PH_Mockingboard` (0). Canonical writes:
  - `$C0(8+s)8` → mode = 0 = MB compat
  - `$C0(8+s)D` → mode = 5 = Phasor native
  - `$C0(8+s)F` → mode = 7 = EchoPlus (acknowledged, routed as native
    in v1)

**Chip-select decode** (Phasor native only):
```
chip_sel = (~(port_b >> 3)) & 3
  0  no AY selected      (PB3=1, PB4=1)
  1  primary AY only     (PB3=0, PB4=1)  → VIA1: AY1; VIA2: AY3
  2  secondary AY only   (PB3=1, PB4=0)  → VIA1: AY2; VIA2: AY4
  3  BOTH AYs broadcast  (PB3=0, PB4=0)
```

In `PH_Mockingboard` mode the chip-select bits are **ignored**: each
VIA always drives its primary AY only (AY1 / AY3), and the secondary
AYs (AY2 / AY4) stay silent — matching the real card's compat
default. This is what lets a vanilla Mockingboard music driver run
unchanged on a Phasor.

**Clock scaling**. `clockScale() == 2` in `PH_Phasor`; 1 in MB /
EchoPlus. The audio synth multiplies the AY input clock by this
factor — same register values produce notes one octave higher in
native mode (real Phasor halves the AY divider).

**Audio synth — 4-AY mono mix**. The `AudioSrc` snapshots the 4
register banks + reset/env-write counts + the current `clockScale()`
under the parent mutex, then runs the MAME-parity AY synth loop per
chip: integer tone counter + fractional accumulator (no float-aliasing
drift), 17-bit LFSR noise with prescale (clock/16/NP effective LFSR
rate), 4-flag envelope state machine (set_shape on every R13 store
including same-value re-stores). Mono mix divides by 12 — 4 chips ×
3 channels × peak 1.0 — so a maxed-out Phasor-native signal sits at
1.0 before the volume knob. `clockScale` multiplies the per-sample
step rate for tone / noise / envelope counters so the same register
values produce notes one octave higher in native mode (chip clock
doubles; AY periods unchanged).

In PH_Mockingboard only AY1 + AY3 receive strobes (chip-select
ignored), so the effective mix sits ~6 dB lower than a real
Mockingboard. The user compensates with the volume slider. The
alternative — a dynamic divisor — would clip when Phasor-native
software hits full amplitude across all 4 chips. Predictable
headroom wins.

Pinned: `phasor_card_smoke` — dual-VIA register layout + mirrors,
mode soft-switch decode, MB-compat routing (primary AY only),
Phasor-native chip-select PB3/PB4 decode (4 cases:
pri/sec/both/none), telemetry counters, 4-AY non-silent mix +
mute path, **clockScale ×2 pitch doubling measured by
zero-crossing** (target 2.0, observed ~2.01).

**UI**: Devices → Phasor panel. Banner with current mode (MB / Phasor
/ EchoPlus — color-coded), clock multiplier, slot IRQ, volume, the
device-select mode-switch addresses for the slot. 2 columns of VIA
telemetry (T1 counter / ACR / IFR / IER), 4 columns of AY register
banks with R0/R1/R2/R3/R4/R5/R6 channel periods + R8/R9/R10 volume
decoded for quick read. In MB-compat mode the secondary AY columns
(AY1, AY3) carry a "(MB-compat: silent)" tag so the user understands
why those banks stay zero even with a music driver running.

### Floppy mechanical sounds

`FloppySoundDevice`. Port of MAME `imagedev/floppy.cpp::
floppy_sound_device`. 20 source WAVs (10 × 5.25" + 10 × 3.5") in
`roms/floppy_samples/`, BSD-3-Clause.

**`FloppySoundSink` interface** (header-only): `DiskIICard` calls
`sound_->motor()/step()/click()` through it so smoke tests don't
drag miniaudio.

**Step/seek decision** (MAME parity): `step(newTrack, emuCycles)`
measures gap in emulated CPU cycles (MAME `floppy.cpp:1532-1620`).
Wall-clock audio frames would be wrong under disk turbo (~60×):
PROM's full phase sweep lands in one audio buffer → gap=0 → buzz.

- `gap > 100 ms` (`kSeekJoinMs`) → single-step click.
- `gap ≤ 100 ms` → seek mode: pick seek sample whose nominal cadence
  is closest (2/6/12/20 ms), pitch-scale (`pitch = nominal_ms /
  gap_ms`), loop.
- No step for `kSeekTimeoutMs` → exit, final `step_1_1`.

Floor at 1 ms gap defends `mixLoop` against `INF` rate; pitch in
[1, 2] for `SEEK_2MS`.

**Wall-clock motor-off hold-off**: turbo bumps CPU ~60× → 1-sec
spin-down (`motorOffDelay = 1'022'727` cycles) becomes ~17 ms
wall-clock. Device defers audible transition by `kMotorOffHoldMs`
(default 800 ms) in **audio output frames** not cycles; fresh
`motor(true)` cancels.

**CPU ↔ audio**: mutex-guarded `std::vector<Cmd>` queue. CPU pushes
`MotorOn/MotorOff/Step/Click`; audio thread drains at top of
`fillAudioBuffer`.

**Hook points in `DiskIICard`**: `seekPhaseW` end → `step(head/4)`;
`control()` `$C0E9` MODE_IDLE→ACTIVE → `motor(true)`;
`advanceCycles()` when `motorOffDelay` expires → `motor(false)`;
`handleSwitchAccess()` legacy 32-cyc gate immediate motor toggle;
`insertDisk`/`ejectDisk` → `click()`.

Owned by `EmulationController` (audio shutdown drains thread).
Persisted: `floppy_sound_volume`, `floppy_sound_muted`. Pinned:
`floppy_sound_smoke_test`.

## Slot bus & IRQ aggregation

`SlotBus` + `SlotPeripheral`, 8 slots. Memory routes 4 windows:

- `$C080-$C0FF` device-select (16 B/slot N at `$C080+N*16` ; slot 0
  = LC hook, 1-7 = expansion).
- `$C100-$C7FF` slot ROM (256 B/slot 1-7).
- `$C800-$CFFF` shared expansion ROM, owned by whichever slot most
  recently touched `$CnXX`. `$CFFF` deactivates active slot;
  auto-latch on slot-ROM access.

`advanceCycles()` forwards to every plugged card. Ctrl-Reset
propagates `onReset()`.

### IRQ wire-OR

`M6502::setIrqLine(sourceId, asserted)` — wire-OR. 32-bit OR'd
contributor mask: slot N (1..7) = bit N, VBL = bit 8, legacy
`setIRQ(int)` = bit 31. NMI is a single latch. Pinned:
`irq_aggregator_smoke_test`.

### `SlotPeripheral::assertIrq` API

Cards never poke `cpu->setIrqLine` directly. Protected
`assertIrq(bool)` debounces against `irqAsserted_` cache
(idempotent — only edges propagate), fans out via
`SlotBus::forwardSlotIrq(slot, asserted)` to whatever `IrqRouter`
Memory installed (`Memory::setCpu(cpu)` plants a closure).
`SlotBus::plug()/unplug()/clear()` auto-release pending IRQ
contribution. Pinned: `slot_peripheral_irq_smoke_test`.

Mockingboard keeps `cpu_` for `getCycleCountNow()` lazy-sync only;
Disk II keeps `cpu_` for sub-instruction LSS accuracy on Q6L reads.
MouseCard and SSC dropped `cpu_`.

## Storage

### DiskImage

143 360-byte 5.25": `.dsk`/`.do` (DOS 3.3 skew) or `.po` (ProDOS).
Pre-nibblized into 35 × 6656-byte tracks. GCR per "Beneath Apple
DOS". Skew tables (physical → logical):

- DOS 3.3: `{0,7,14,6,13,5,12,4,11,3,10,2,9,1,8,15}`
- ProDOS:  `{0,8,1,9,2,10,3,11,4,12,5,13,6,14,7,15}`

Write-back via `saveDirty()` (`.dsk`/`.do`/`.po`/`.nib` + `.2mg`
envelopes + `.woz`) opt-in via `setWriteBackEnabled(true)`.

### Format detection

`detectFormat()` + `enum ImageKind`. `loadFile(path)` slurps once,
dispatches by content. Order: MacBinary strip → 2IMG envelope → WOZ
magic → 35×6656 NIB → 35×6384 CNib2 → 143 360-byte sector. Unknown
→ false + specific `lastError`.

- **Skew sniff** (143 360 branch): validates ProDOS vol-dir key
  block at `file[0x404]` (`.po`) vs `file[0xB04]` (`.dsk`),
  overrides extension when only the other position fits. Predicate:
  `prev=0`, plausible `next`, storage_type `$F`, name chars in
  `A-Z 0-9 .`.
- **2IMG**: 64 B header → format byte (0=DOS, 1=ProDOS, 2=NIB),
  flags (bit 0 = WP, bit 8 or 31 = vol# present), dataOffset,
  dataLength. Raw header + trailer captured into
  `twoImgHeaderRaw`/`twoImgTrailerRaw`; `saveDirty()` re-emits
  both so envelope stays byte-identical.
- **MacBinary** 128 B prefix stripped (AppleWin predicate: `b[0]==0`,
  name length [1..63], terminator + reserved zeros).
- **CNib2** (35×6384): pad to 6656/track on load with `$FF` (sync),
  truncate to 6384 on save.
- **Volume number**: per-image (2IMG flags or default $FE), threaded
  through `nibblizeTrack(track, sectors, vol, skew)`.

Pinned: `disk_image_smoke`, `disk_skew_sniff_smoke`, `disk_2mg_smoke`,
`disk_2mg_writeback_smoke`, `disk_macbinary_smoke`, `disk_cnib2_smoke`,
`disk_refuse_smoke`.

`classifyDiskForSlot` (`DiskImage.*` — `DiskSlotClass` =
`Floppy525/Sony35/Hdv`) routes positional disk CLI by content. `.hdv`
is HDV at any 512-aligned size; `.2mg` shares 3.5"/HDV by size.

### 13-sector (5-and-3, pre-DOS-3.3)

DOS 3.1/3.2/3.2.1: 13 sectors/track × 5-and-3 GCR. Image = 35×13×256
= **116480 B**. `detectFormat` maps that size → `Dos32_13` (always
DOS order); `loadSectorImageFromBuffer` calls `nibblizeTrack13` and
sets `sectorsPerTrack_=13` (`is13Sector()`).

Codec = verbatim MAME `formats/ap2_dsk.cpp` `a2_13sect_format`:
`nibblizeTrack13/writeDataField13` (encode, `kTranslate5[32]`, addr
prologue `D5 AA B5`, data prologue `D5 AA AD`, 411-nibble data) +
`decodeTrack13/kUntranslate5` (write-back). Physical interleave
`sector = (i*10)%13`. Pinned **byte-for-byte round-trip**:
`d13_roundtrip_smoke_test`.

**Boot wiring**: `DiskIICard` serves 341-0009 boot PROM
(`roms/disk2_13.rom`) at `$Cn00` while a 13s disk is mounted
(`serving13_ = any 13s && bootRom13Loaded`). 13-sector disks
**force the bit-level LSS** — the 341-0009 read loop is tighter
than the legacy 32-cycle gate. The **read sequencer stays 16-sector
P6** (341-0028); the LSS is encoding-agnostic, 5-and-3 decode is
software (boot PROM + DOS 3.2 RWTS). Pinned: `dos32_boot_trace`.

### `.woz`

Verbatim port of MAME `lib/formats/woz_dsk.cpp`. WOZ stores raw bit
cells — survives copy protections that tweak timing. WOZ1 (160 ×
6656-byte slots, `bit_count` @+6648 u16) and WOZ2 (160 × 8-byte TRK
headers, data at `starting_block × 512`, `bit_count` u32). Bits
MSB-first. Each track 0..34 sources bits from `TMAP[track*4]`
(centre qt); sub-qt positions (Locksmith, David-DOS) not yet
preserved.

**Write-back**: `loadWoz()` snapshots file to `wozRaw` +
per-qt-track `(byteOff, byteLen, bitCount)`; `writeFlux()` splices
into `bitStream[qt]`; `saveDirty()` repacks + zeros CRC32
(Applesauce "not computed" sentinel) + rewrites in place.
`isWriteProtected()` honours both user toggle and
`INFO.write_protected`. `DiskIICard::insertDisk` forces
`useBitLss=true` when any drive holds WOZ. Pinned:
`woz_load_smoke`, `woz_writeback_smoke`.

### WOZ2 `optimal_bit_timing`

INFO+39 (units of 125 ns) — bit-cell duration. Default 32 = 4 µs =
standard cell @ 2 MHz LSS = 8 LSS cyc/cell. `loadWoz` reads when
`info_version >= 2`, clamps [8, 64], stores in `optimalBitTiming`.
`lssCyclesPerCell() = optimalBitTiming / 4`. `expandTrackFlux`
emits each "1" cell at `i*cyc + cyc/2` (centre). Pinned:
`woz_bit_timing_smoke` (obt 32/40/28 + WOZ1 fallback).

### DiskIICard

256-byte P5A boot PROM. Apple 341-0027-A (CRC `ce7144f6`) embedded
as `kBootPromDefault[256]`; `loadBootRom("roms/disk2.rom")` overrides.
PROM autodetects slot via `JSR $FF58 / TSX / LDA $0100,X`. Soft
switches `$C0E0-$C0EF`: phases, motor, drive_select, Q6L/Q6H,
Q7L/Q7H.

**Boot signature** (Apple II Ref Manual Appx C): `$Cn00` starts with
`$20 ?? $00 $03` at offsets 1/3/5 (JSR dispatch trio). `$Cn07`
distinguishes Disk II / SmartPort (`$3C`, scanned by F8 Autostart
`341-0020-00`) from ProDOS block devices (`$01` for non-removable
HDV). F8 ONLY auto-scans `$Cn07=$3C`; HDV needs `PR#N` /
`bootFromSlot`.

`bootFromSlot()` validates the JSR trio so clicking "Boot" on a
non-bootable card warns + falls back to `coldBoot`. `$Cn07=$3C` is
NOT validated — would reject HDV.

**Drive switching** via `selectDrive(int)` mirrors MAME
`machine/wozfdc.cpp:264-291`. When motor active: flush in-flight
write on old drive (= MAME `mon_w(true)`), clear OLD drive's
`revolutionStartLssCycle` to `kNeverRev`, anchor NEW drive's to
current `lssCycle` (= MAME `mon_w(false)`). Per-drive
`revolutionStartLssCycle[2]` matches MAME
`floppy_image_device::m_revolution_start_time`. Disk angular
position = `(lssCycle - revolutionStartLssCycle[drive]) mod
track_period`. Pinned: `disk_drive2_smoke`,
`mame_lss_parity_smoke`.

### DiskII multi-instances

`"diskii"` is the only slot-card type allowed in >1 slot
(`isDuplicate` short-circuits when type=="diskii"; same in
`firstOccurrence` walk). Both cards load same `disk2.rom` +
`diskii_p6.rom`. Per-card 2 drives + LSS state.

**Primary**: `MainWindow` keeps `std::vector<DiskIICard*> diskCards`
in slot-ascending order. `diskCard` (legacy) = `diskCards.front()` —
lowest-slot wins. Per-slot persistence: `disk_path_slotN` /
`disk_writeback_slotN`. Primary also writes legacy unsuffixed keys
for older builds. Profile-switch captures `savedDiskPaths[slot]`
from live cards before tear-down. **IWM wiring**: only slot-6
`DiskIICard` calls `card->setIWM(&controller->iwm())`.

### Two read paths

- **Bit-level LSS** (default when `roms/diskii_p6.rom` present) —
  verbatim port of MAME `machine/wozfdc.cpp` + flux-event subset
  of `imagedev/floppy.cpp`. MAME `cycles` = 2× CPU clock.
  `lssSync(extra)` catches up from `lssCycle` to `cyclesLimit =
  cpuCycleTotal*2 + extra`. PULSE from
  `DiskImage::getNextTransition(track, lssCycle)` (event @
  `cellIdx*8 + 4`, cell centre). Reads of `$C0EC` pass `extra=1`
  after `control()` (read-pipe latency). P6 PROM (341-0028-A)
  indexed by `(state<<4) | (Q7<<3) | (Q6<<2) | (QA<<1) | (!PULSE)`.
  Pinned: `diskii_lss_smoke`, `mame_lss_parity_smoke`.
- **Legacy 32-cycle gate** (fallback) — `kCyclesPerNibble = 32`;
  nibble every 32 cycles, `byteReady` toggles for BPL spins.
  2–3× faster than LSS in stock boots.

### Bit-stream expansion

`DiskImage::bitAt(track, idx)` lazily walks nibble buffer, emits 8
cells per non-FF byte + 2 trailing zero cells per `$FF` inside a
run ≥ `kSyncMinRun = 5` consecutive `$FF`. Sync-FF padding lets
the LSS lose alignment in sync gaps and resync on the next prologue.
`.nib` path skips padding (every byte = 8 cells, total 53248). Cache
invalidates on `writeNibbleAt`.

The ≥5 threshold avoids matching the naturally-occurring 2-byte
in-field `$FF` pairs (4-and-4 address checksum when `vol ^ track ^
sector == $FF`, or 6-and-2 data XOR producing disk `$FF` from
source `$FF $00 $FF`).

### Flux-event view

`fluxEvents(track)` + `trackPeriod(track)` — one event per "1" cell
at LSS-cycle `cellIdx*8 + 4`. `getNextTransition` verbatim MAME
`floppy_image_device::get_next_transition`, wraps across revs.
`writeFlux(track, start, end, count, transitions)` splices flux
window back into nibble buffer.

### ProDOSHardDiskCard (HDV — synthetic-block model)

Slot-plugged ProDOS hard disk (default slot 5, label `hdv`) backed by
`.hdv`/`.2mg`. **Deliberate divergence from MAME**: no ATA/SCSI, no
real ROM. The card fabricates its 256-byte slot ROM at runtime
(`buildRom`, hand-assembled 6502) and talks to a host-implemented
streaming protocol on `$C080+slot×16`:

```
off 0  write   block LO byte               (resets stream offset)
off 1  write   block HI byte               (resets stream offset)
off 2  read    next byte of selected 512 B block (auto-incr, wraps)
off 2  write   next byte INTO block         (write-back-gated)
off 3  read    status: bit7 = no image, bit6 = WP
```

`deviceSelectRead/Write` move bytes via host `memcpy` — no GCR, no
flux. `$Cn07=$01` (plain ProDOS block, not SmartPort `$3C`); JSR
trio `$Cn01/03/05 = $20/$00/$03`. F8 Autostart won't scan `$01` →
boot via `PR#n` / `bootFromSlot`.

**Trade-off**: mounts `.hdv`/`.2mg` directly (MAME accepts only
CHD/raw), no card-ROM dump needed; cannot execute real CFFA/SCSI
firmware. The ATA-class port now lives as `CffaCard` (below).

Storage shared with `CffaCard` via `Block512Backing.{h,cpp}`: in-mem
image, 2IMG envelope (header+trailer preserved), medium WP,
dirty-block tracking, opt-in host-file write-back, host-folder synth
volumes. Both cards implement `pom2::ProDOSBlockCard` (image-mgmt
iface) so HDV Library / disk-turbo / persistence target uniformly via
`MainWindow::hdvDevice()` (prefers CFFA when plugged). Also
implements `MountableMediaCard` as a single fixed bay.

Pinned: `hdv_card_smoke`, `hdv_writeback_smoke` (header/trailer/WP/
opt-in round-trip), `hdv_mass_storage_smoke` (32 MB boundary, 16-bit
block addressing, `.2mg` data-offset ≠ 64). Multi-partition images
(CFFA3000-style) not supported — 1 image = 1 unit = 1 volume.

### CffaCard (CFFA 2.0 — MAME-faithful IDE)

`CffaCard.{h,cpp}` + `AtaBlockDevice.{h,cpp}`. **Real 4 KB firmware
dump executed over an emulated ATA chip**, image stored as raw LBA.
Ported from MAME `bus/a2bus/a2cffa.cpp`.

- **`AtaBlockDevice`** — ATA/IDE taskfile subset over
  `Block512Backing`, isomorphic to MAME `ata_interface_device` cs0
  access: `cs0_r/cs0_w(reg)`, 16-bit data register at reg 0.
  IDENTIFY DEVICE ($EC), READ SECTOR(S) ($20/$C4), WRITE SECTOR(S)
  ($30/$C5), LBA28. Unknown commands no-op. DRQ/BSY/DRDY PIO; no
  DMA/IRQ/CHS. Reusable for future Vulcan/Zip/Focus. Pinned:
  `ata_block_device_test`.

  **Gotcha**: CFFA firmware sizes partitions from IDENTIFY **words
  57-58** ("current capacity in sectors"), NOT 60-61 (LBA28 total)
  — leaving 57-58 zero ⇒ "Could not boot partition 1 / Err $28"
  (firmware `$CD35-$CD52` reads $C0n8/$C0n0 for words 57-58).
  `fillIdentify` sets 57-58 = 60-61 = total, word 53 bit 0 (current
  fields valid). Debug: `POM2_TRACE_CFFA=1`;
  `tests/cffa_boot_dump --image X --slot N`.

- **`CffaCard`** — `SlotPeripheral + ProDOSBlockCard`. Decode mirrors
  `a2cffa.cpp`: `read_c0nx/write_c0nx` ($C0nX) drive ATA taskfile
  with 8↔16-bit latch ($C0n0=high byte, $C0n8=low byte+commit;
  $C0n3/$C0n4 toggle EEPROM WE); `read_cnxx` ($CnXX) → `rom[off +
  slot*0x100]`; `$C800` shared expansion, writes WP-gated. Real
  firmware presents `$Cn07=$3C` → **F8 Autostart boots natively** (no
  GUI shortcut).

- **ROM**: user-supplied `roms/cffa20ee02.bin` (6502) /
  `cffa20eec02.bin` (65C02), 4096 B exact (CRC `3ecafce5`/
  `fb3726f8`); plug-time probe picks variant matching CPU. Card type
  hidden from Slot Config when absent. Source: dreher.net
  `Run6_CDROM.zip` (`Firmware/V2.0/`).

- **Image**: `.hdv`/`.2mg` raw LBA (compat preserved). **CHD = phase
  2**. Mounts via HDV Library.

Pinned: `cffa_card_smoke` (ROM-gated). Full MAME oracle: `mame
apple2ee -sl7 cffa2 -hard1 <img>` (romset `~/mame_roms/cffa2/`).

### SmartPortCard (//e Liron-class)

`SmartPortCard.{h,cpp}`. Slot-plugged Apple "Disk 3.5 Controller
Card" (Liron / 670-0186) for //e / II+ / II / //c. Default slot 5.
**Block-level, no IWM** (same synthetic-block divergence as HDV).

**Device-select protocol** (`$C0nX`):
```
$C0n0 write  drive select (0 / 1)
$C0n1 write  block LO byte
$C0n2 write  block HI byte
$C0n3 read   next byte (auto-incr 512 B)
$C0n3 write  next byte INTO current block (WB-gated)
$C0n4 read   status: bit7 = no disk, bit6 = WP
```

Per-drive `streamOffset_[2]` wraps every 512 B; drive-select latches
`activeDrive_` and resets stream offset.

**Slot ROM** (`buildRom`, 256 B with slot baked in):
```
$Cn00     JMP $Cn20              (boot vector)
$Cn01     $20                    ProDOS signature byte
$Cn03     $00
$Cn05     $03
$Cn07     $3C                    SmartPort signature (note: see //c
                                  on-board variant below — $01 there)
$CnFE     $13                    features/units mask (2 units)
$CnFF     $50                    driver entry offset
$Cn20-..  boot (load blk 0 of drv 1 → $0800)
$Cn50-..  ProDOS driver
$CnE0-..  error halt
```

Driver examines ProDOS `$43` unit byte: bit 7 = drive (0 → drv 1, 1 →
drv 2). Write probes `$C0n4` bit 6 first; returns `$2B` (WP) without
touching memory if WP.

**Per-unit storage**: each `SmartPortUnit` owns its bytes. The
HDV-flavoured `SmartPortHdvUnit` wraps `Block512Backing` (2IMG/dirty/
WP/write-back for free). Per-unit settings persist as
`smartport_slotN_unitK_{type,path,writeback}`. Card implements
`MountableMediaCard` over its 2 units.

**Boot wiring**: a library click (or CLI insert+boot) routes 3.5"/HDV
to the primary `SmartPortCard` and `controller->bootFromSlot(card->
getSlot())` on every profile that has one — including //c-class
(built-in slot 5).

Pinned: `smartport_card_smoke_test`, `smartport_mixed_units_smoke_test`.

### //c-class on-board SmartPort (3.5" + HDV boot)

//c / //c rev0/3/4 / //c+ all boot 3.5" **and** HDV through a
host-served SmartPort block device — the **same `SmartPortCard`** as
//e, but built into slot 5. Why not faithful IWM/Sony:

- Real //c-class masks all slot ROM (forced INTCXROM) → normal slot
  card's `$Cn00` invisible → `bootFromSlot` reads internal ROM, never
  the card.
- MAME doesn't model 3.5"/SmartPort on plain //c (`A2BUS_IWM` is
  5.25"-only; "WANTED: no Liron ROM dumps"). Only //c+ models 3.5"
  via the on-board IWM/MIG/Sony — but that boot path doesn't reach a
  bootable disk (full IWM bit shifter unmodelled).

**Mechanism**:

- **ROM hole** (`Memory::memRead`, //c-class INTCXROM branch):
  `$C500-$C5FF` (bank 0) returns `slots.slotRomRead(addr)` instead of
  internal ROM **iff** `iicSmartPortArmed_` AND
  `slots.peripheral(5)->exposesIicOnboardRom()` (unit holds media).
  Bank 1 handled earlier by `internalRomRead` → hole is bank-0 only
  (preserves //c+ alt firmware's `$C500` data).

- **"Armed" gate — critical subtlety** (`Memory::setIicSmartPortArmed`).
  The stub MUST NOT be visible during the //c ROM's own autostart:
  real //c rev0/3/4 keeps its SmartPort firmware at `$C500`, and a
  booted ProDOS calls into `$C5xx` entries the real firmware provides
  but the stub does not. Substituting the stub corrupts a
  multi-device boot (Disk II in slot 6 + media in on-board SmartPort)
  → "garbled Apple //c banner". So `bootFromSlot` **arms** (explicit
  GUI/CLI boot only) and every `coldBoot/softReset/hardReset`
  **disarms**. Net: normal reboot always sees real `$C500` firmware;
  on-board SmartPort boots only via Library / Slot Config "Boot".
  Trade-off: persisted SmartPort media doesn't auto-reboot.

- **Device-select** (`$C0D0-$C0DF` = slot 5) never masked — block
  stub's `$C0D0-$C0D4` protocol already reaches the bus.

- **Stub fixes** (`SmartPortCard::buildRom`): `$Cn07` = `$01` (ProDOS
  non-removable block device), NOT `$3C` — `$3C` is the Disk II
  marker and made //c treat slot 5 as a second Disk II. ProDOS STATUS
  call (cmd `$00`, `$CnC0` routine) returns block count in X/Y via
  `$C0n5/$C0n6` so ProDOS ONLINE / BITSY size it correctly.

- **Routing** (`MainWindow`): `routeMount35` uses SmartPort on all
  profiles; `routeMountHdv` + `ensureHdvCardForBoot` send //c-class
  HDV to slot-5 SmartPort (`SmartPortHdvUnit`), **never** cffa/hdv
  slot card (masked, unbootable). Profile //c gains a `smartport35`
  built-in at slot 5; //c+ already had one.

Pinned: `iic_onboard_smartport_test` (armed ROM-hole gating + block
I/O via `Memory`); `iic_dual_boot_trace` (headless diagnostic for the
garble). See `project_iic_smartport_boot`.

### 3.5" mechanical sounds

`Sony35Drive` carries `FloppySoundSink* sound_` set by
`EmulationController` to same `FloppySoundDevice` Disk II uses —
shares samples + volume/mute persistence.

**Cycle stamping**: `seekPhaseW(phases, emuCycles)` takes
CPU-cycle counter at strobe edge. `SmartPortHub::onIwmPhases`
forwards `IWMDevice::emuCycles()`. Strobe cases:
```
0x1  step       moved && sound_->step(track_, lastStrobeCycle_)
0x2  motor on   if (!motorOn_) sound_->motor(true,  hasDisk)
0x3  motor off  if ( motorOn_) sound_->motor(false, hasDisk)
0x4  eject      image->eject() ; sound_->click()
```
`moved` gates so head bumps at track 0 or 79 don't click. Motor
transitions edge-only.

`EmulationController::mount35/eject35` call
`drive->emitInsertClick()` after `notifyMediaChange()`.

### ProDOS host folder

`prodos_disk/`. `ProDOSVolume` synthesises a read-only ProDOS volume.
Blocks 0-1 boot (zeroed), 2-5 vol-dir key + 3 ext (51 entries max),
block 6 bitmap (4096 blocks = 2 MB cap), 7+ data + sapling indexes.

Scope: flat dir; ≤ 51 files; ≤ 128 KB per file (seedling + sapling,
tree skipped); type from extension; filenames sanitised to
`A-Z/0-9/.` with collision suffixes `.1/.2`.

Wiring: HDV slot 5 panel's Library shows `[host folder] prodos_disk/`
entry. Click → `buildVolumeFromFolder` →
`ProDOSHardDiskCard::loadImageFromBytes`. **No auto-boot** — user
boots ProDOS elsewhere, then `/HOST/` appears as slot 5 drive
(`CAT,S5,D1`). Read-only: driver returns `$2B` on writes. Pinned:
`prodos_volume_smoke_test`.

### Snapshot

`SnapshotIO`. `POM2SNAP` magic, named 8-byte sections, format shared
with POM1. Captures CPU + RAM + soft-switch display state. **Disk II
deliberately excluded** — would need mounted-image identity + head
position + dirty bits per track.

## IWM (//c+ on-board)

`IWMDevice.{h,cpp}` — verbatim MAME `machine/iwm.{h,cpp}`. Full state
machine (`MODE_IDLE/ACTIVE/DELAY` for `m_active`; `MODE_READ/WRITE`
for `m_rw`; `S_IDLE/SR_WINDOW_EDGE_0/SR_WINDOW_EDGE_1` for read bit
walker; `SW_WINDOW_LOAD/MIDDLE/END/UNDERRUN` for write). Drives flux
via `DiskImage::getNextTransition` (5.25") or
`Sony35Drive::nextTransition` (3.5").

**Live wiring**:

1. `EmulationController` constructs the IWM, hands it to
   `Memory::setIWM`. Reset paths (`hardReset`, `coldBoot`,
   `bootFromSlot`) call `iwm.reset()`.
2. Memory routes `$C0E0-$C0EF` on `isIIcPlus` through IWM (MAME
   `apple2e.cpp:2798-2801` gating on `m_isiicplus && slot == 6`).
   Plain //c uses `A2BUS_DISKIING` at sl6. On //c+ slot-6 DiskIICard
   still observes the access (motor sound / turbo / head tracking);
   byte returned is IWM's when `iwmAuthoritative=true` (default).
3. DiskIICard pushes `setFloppy(image, qt)` to IWM from `insertDisk`/
   `ejectDisk`/`selectDrive`/`seekPhaseW`. IWM's `nextTransition`
   queries `DiskImage::getNextTransition(qt, from*2) / 2` (flux events
   in LSS-cycle space; IWM in CPU-cycle space).
4. `EmulationController::tick(cpuCycleTotal)` pulses IWM once per
   video frame so the 1-emulated-second drive-disable timer drains
   when //c+ alt firmware stops poking `$C0Ex`.

`iwmAuthoritative` toggle (`Memory::setIWMAuthoritative` or
`POM2_IWM_AUTHORITATIVE=0`) drops data path back to DiskIICard's LSS
for A/B compare. IWM state advances either way. Pinned:
`iicplus_boot_trace`.

**Window-size scaling**: MAME's `iwm.cpp:290-301 half_window_size` /
`:302-313 window_size` are IWM-clock ticks (//c+ runs IWM off
A2BUS_7M ≈ 7.16 MHz). POM2 ticks IWM at `POM2_CPU_CLOCK_HZ` (~1.02
MHz) for a single cycle counter — constants divided by ~7 to keep
"bit cell" ≈ 4 µs.

**MAME parity audit fixes** (2026-05-16): `data_w` handshake gate
(MAME `:311-318` — clear WHD bit 7 only when mode bit 0 set);
`mon_w` propagation (`:194-195/234/91` — drop old / raise new on
motor); `devsel_cb` extra moments (`:79/236/92` — `device_reset`,
MODE_DELAY entry in non-timer mode, `update_timer_tick` exit);
`set_write_splice` call site wired (`:218-221`, body still stub —
`DiskImage::setWriteSplice` TODO); `read_register_update_delay`
(`:363-366`, returned 1/1 instead of 1/2). Pinned:
`iwm_device_smoke_test`.

**Not yet ported**: Q3 fast clock (1.86 MHz, Mac/IIgs only); full
`DiskImage::setWriteSplice` body (WOZ re-master parity).

## SmartPort 3.5" stack

`Disk35Image` + `Sony35Drive` + `SmartPortHub` — full Sony GCR
read+write for //c+.

*Image+drive*. `Disk35Image` loads 800 K `.po`/`.2mg`. `Sony35Drive`
responds to IWM phase-as-command bus (MAME
`mac_floppy.cpp::seek_phase_w` + Apple //gs HW ref) and to
MIG-driven `m_35sel/m_intdrive/m_hdsel` (MAME `apple2e.cpp:638-679
recalc_active_device`). `senseR()` returns active-low register file
(`/INSERTED`, `/TRACK0`, `/READY`, `/MOTOR ON`, `/SWITCHED`, …).

*IWM wiring*. `IWMDevice` exposes `phasesCb_/devselCb_/sel35Cb_`
(MAME `iwm_device::phases_cb/devsel_cb/sel35_cb`); wired via
`SmartPortHub::attach`. `nextTransition()` dispatches between
`DiskImage*` and `Sony35Drive*` via `setFloppy/setSony35`. `$C0EE`
WPT bit consults `Sony35Drive::senseR()`.

*No-disk noise flux*. With no media `nextTransition()` would return
`INT64_MAX` → read FSM shifts only 0-bits, `data_` stays `$00`, bit-7
never asserts → boot's wait-for-byte loop spins forever. Falls back
to `noiseTransition()` — deterministic LCG keyed on read-window
index, straddles `windowSize()` boundaries so SR accumulates 1s/0s
and emits garbage bytes with bit-7 set. Lets //c reach **"Check Disk
Drive."** and //c+ reach **"UNABLE TO FIND A BOOTABLE DISK ONLINE."**
at power-on. Pinned: `iic_nodisk_boot_trace`.

*GCR encoder* (verbatim MAME `flopimg.cpp::build_mac_track_gcr
2017-2106`). Five speed zones (`kCellsPerRev[5] = {76950, 70695,
64234, 57749, 51388}`, MAME `:2019-2027`), per-zone CPU-cycles-per-rev
= `60 × POM2_CPU_CLOCK_HZ / RPM`, 64-entry `kGcr6fw[]` (MAME line
967), `gcr6Encode(va,vb,vc)` 3-in-4-out packer (MAME line 512).
Per-sector: 8× self-sync (384 cells) + D5AA96 addr prologue + 5 GCR
header + DEAAFF addr epilogue + 2× self-sync + D5AAAD data prologue +
174× 3-in-4-out + 4-byte checksum + DEAAFFFF epilogue = 6208 cells.
Block-to-physical 2:1 interleave (`si = (si+2) % ns; if(si==0) si++`).

*Flux write-back*. `Sony35Drive::writeFlux` splices flux into cached
cell buffer, runs GCR→blocks decoder (MAME `flopimg.cpp:2107
extract_sectors_from_track_mac_gcr6`). Recovered sectors that differ
push via `writeBlock`; image flushes to `.po` via `saveDirty()` on
`eject35` or shutdown. WP honoured. Nibbliser port of `flopimg.cpp:
1530 generate_nibbles_from_bitstream`. **Gotcha**: cycle↔cell
rounding uses round-to-nearest on decode side; encoder uses floor on
`cycleForCell = i × period / n`. Without symmetric rounding,
integer-truncated `2.024 → 2` pushed every transition one cell early
and lost the first sector's addr marker.

*UI / CLI / persistence*. `Disk35Controller_ImGui` (2 Sony slots:
internal = on-board //c+; external = SmartPort daisy-chain).
Mount/Eject, last-error, scanner picks up `.po`/`.2mg` of right size
under `disks35/` (falls back to `disks/`). Toggle
`show_disk35_panel`. CLI: `--35-disk1/--35-disk2`; settings:
`disk35_path_1/_2`.

Pinned: `smartport_35_smoke_test` — load + size guard, SENSE
empty/in-slot, motor strobe, hub recalc (devsel=1+35sel=true AND
devsel=2+intdrive=true), phase fwd, marker placement (12+12 on track
0), full encode→flux→splice→decode→block-readback round trip, WP
short-circuit.

## Peripherals

### Super Serial Card (slot 2) + telnet bridge

6551 ACIA at `$C0A8-$C0AB` (data/status/cmd/ctrl). Status bit 4 =
TDRE (always 1), bit 3 = RDRF (RX queue), bits 5/6 = DCD/DSR (TCP
state). Unconnected `$C0A8` returns 0.

Slot ROM `$C200-$C2FF`: autodetect bytes (`$Cn05=$38`,
`$Cn07=$18`, `$Cn0B=$01`, `$Cn0C=$31`); `JMP $Cn20` skips them.
PR#2 hooks CSWL/CSWH (`$36/$37`) → `$C2B0`; IN#2 hooks KSWL/KSWH
(`$38/$39`) → `$C2E0` (load + ORA #$80). Reset clears rings.

**Pascal 1.1 ID block** at `$Cn0D-$Cn10` (NOT `$CnFB-$CnFF` — TODO
note was wrong): offsets of PINIT/PREAD/PWRITE/PSTATUS routines
after the `$Cn0B=$01`/`$Cn0C=$31` signature. Layout + calling
convention per real SSC ROM (6502disassembly.com/a2-rom/SSC). Pinned:
`ssc_acia_smoke::testPascalIdBlock`.

TCP listener on `127.0.0.1:port` (default 6502); one client. 4 KB
rings; telnet IAC (WILL/WONT/DO/DONT + 2-byte + `$FF $FF` literal)
swallowed by `swallowTelnetIac` so stock `telnet` connects.
`TCP_NODELAY` on. Auto-plugged at startup; listener starts only when
`ssc_listening=true`. LF→CR RX symmetric; raw-mode toggle (default
OFF). Port + state persisted. Pinned: `ssc_acia_smoke`.

### ProDOS clock card (slot 4)

ThunderClock+ compatible. **ProDOS does NOT route through slot ROM**
— boot copies hardcoded driver to RAM (~$D742), patches
`$BF06-$BF08` to JMP it, then driver speaks device-select. Slot ROM
only needs detection signature.

Slot ROM `$C400-$C4FF`: signature bytes `$08, $28, $58, $70` at
offsets 0/2/4/6. Odd-offset fillers form benign fall-through;
`$Cs08 = RTS`.

**uPD1990AC bit-bang at `$C0C0`**:
```
write bit 0 = DATA_IN; bit 1 = CLK; bit 2 = STB; bits 3..5 = C0/C1/C2;
      bit 6 = IRQ enable ($40)
read  bit 5 = IRQ asserted; bit 7 = DATA_OUT (LSB of shift register)
```

Mode `0b011` = `MODE_TIME_READ`: arm via `$C0C0=$18`, pulse STB
(`$1C`) to latch host time into 48-bit shift register, drop STB,
read bit 7 + pulse CLK (`$1A`/`$18`) 48 times → 6 BCD bytes (sec,
min, hour, day, (month<<4)|dow, year). Mode `0b010` = `MODE_TIME_SET`:
load 48 bits via DATA_IN + 48 CLK, then STB-in-TIME_SET commits via
`commitTimeSetFromShiftReg()` (`std::mktime`, delta captured as
`userOffsetSeconds`).

**TP interrupts** (POM2-original — MAME's `a2thunderclock.cpp` never
binds `tp_callback`). Wiring per ThunderClock Plus manual ch. V:
`$C0n0` bit 6 (`$40`) is enable latch; TP rising edge sets request FF
→ `assertIrq(true)` while enabled; **any** device-select read/write
clears request (enable latch persists, periodic source keeps ticking);
read `$C0n0` bit 5 = "interrupt asserted" flag; RESET disables.
Rates decode latched C0/C1/C2 on STB rising edge: dividers 512/128/
16/8 against 32.768 kHz XTAL → **64/256/2048/4096 Hz** (modes 4-7),
plus 64 Hz for REGISTER_HOLD. Interval timers (1/10/30/60 s, modes
8-15) need uPD4990A 4-bit serial, unreachable on parallel uPD1990AC —
not modelled. Pinned: `clock_card_smoke` (TP rates, IRQ enable, bit-5
flag, reset).

**MODE_SHIFT lax-gating divergence**: POM2 shifts on **every** CLK
rising edge regardless of mode (MAME `upd1990a.cpp:312-327` gates on
`m_c == MODE_SHIFT`). ProDOS's hardcoded driver pulses CLK while
still in MODE_TIME_READ; strict gating breaks stock ProDOS. Observed
HW permits the shortcut. Pinned: `testShiftLaxAcrossModes`.

**Optional real ROM dump.** Drop `roms/thunderclock_u9_v1.3.bin` (also
accepted: `thunderclock_u9.bin`, `thunderclock.rom`,
`Thunderware_REV_1.3_ROM_U9.bin`) and `ClockCard` swaps the synthetic
slot-ROM stub for the dumped U9 EPROM. Accepts 256 B (slot ROM only)
or 2 KB (slot ROM + $C800 expansion ROM mirroring the same chip into
both windows so the firmware's $C8nn JMP continuations resolve).
Source: markadev/AppleII-RevEng/Thunderware-Thunderclock-Plus. The
load path validates the $08/$28/$58/$70 ProDOS signature at
offsets 0/2/4/6 and falls back to the synth ROM if absent.

### Printer card (parallel, synthetic)

`PrinterCard` (`PrinterCard.h/.cpp`) — host-side spool that captures every
byte the Apple II "prints" through `PR#n` into a `std::vector<uint8_t>`
the UI saves to `.txt` (PDF deferred — see TODO). No PROM dump
required; the synthetic 256-byte slot ROM only does the PR#n CSWL/CSWH
hook + a 4-byte output trampoline.

Slot ROM layout (s = slot, slotHi = $C0+s):

```
$Cn00  4C 20 ss   JMP $Cn20            (skip the Pascal sig region)
$Cn05  38         Pascal 1.1 sig 1     (SEC)
$Cn07  18         Pascal 1.1 sig 2     (CLC)
$Cn0B  01         Pascal firmware rev
$Cn0C  00         Pascal device class = printer
$Cn20  A9 31      LDA #$31             ; CSWL low byte
$Cn22  85 36      STA CSWL
$Cn24  A9 ss      LDA #slotHi
$Cn26  85 37      STA CSWH
$Cn28  60         RTS
$Cn31  8D 91 c0   STA $C0(8+s)1        ; data port write
$Cn34  60         RTS
```

Data port at `$C0(8+s)1`: write enqueues the byte verbatim (no high-bit
strip — the UI/spoolText() does that), read returns $FF (always
ready). Other device-select offsets read $FF / writes ignored.

The full Pascal 1.1 entry block (PINIT/PREAD/PWRITE/PSTATUS at
$Cn0D-$Cn10) is **not** implemented — BASIC `PR#n` is the only
documented use case for a printer card in the POM2 software corpus,
and Pascal printer drivers were rare. Signature bytes alone are
enough to keep ProDOS's device scanner happy.

**Built-in for //c and //c+** (`SystemProfile.cpp:cfgAppleIIc /
cfgAppleIIcPlus`) at slot 1, free-slot pick on II / II+ / //e via
the Slot Configuration panel. The //c built-in is a **POM2-original
substitution** — real //c shipped a *second* SSC at $C100 (firmware
labelled "printer port" but electrically serial); we substitute the
synthetic parallel card so PR#1 from BASIC has a useful sink that
spools to a host file, matching the macOS print-to-PDF affordance.
Divergent from MAME's `apple2c` (which keeps the serial SSC at $C100)
but consistent with POM2's earlier //c liberties (Mouse at sl4 where
MAME has Mockingboard).

Pinned: `printer_card_smoke` — ROM fingerprint + data-port spool
semantics + CPU-driven `PR#1` + 3 COUT-style writes flow.

### Grappler+ (Orange Micro)

`GrapplerCard` (`GrapplerCard.h/.cpp`) — ROM-gated parallel printer
card. Catalog key `"grappler"`, default slot 1. Adds two things over
`PrinterCard`:

* **4 KB real ROM** (`roms/grappler_plus.bin`, also accepted:
  `roms/grappler+.bin`, `roms/grappler.bin`). First 256 B map at
  `$CnXX`; the lower 2 KB of the 4 KB EPROM are mirrored into the
  shared expansion-ROM window at `$C800-$CFFF` so Grappler-aware
  software (e.g. AppleWorks "Printer = Grappler+") finds the ROM
  fingerprint. Wrong-size or missing dumps are rejected with a log
  warning; the card falls back to a synthetic stub identical in
  shape to `PrinterCard` so `PR#n` still works.
* **Spool semantics identical to PrinterCard.** Data port at
  `$C0(8+s)1` enqueues bytes verbatim; the host UI saves the spool
  as `.txt`. Grappler-graphic-dump commands (`^I G` / `^I H`) emit
  Epson-style printer escapes — those bytes are spooled too, ready
  for a future "render as raster" mode.

**Bank switching not modelled.** Real Grappler+ exposes the upper
2 KB of its 4 KB EPROM via a write to a $C0(8+s)X bank-select
register. POM2 currently only serves the lower 2 KB in the expansion
window — enough for PR#n detection + the standard graphics-dump
entry points but not for ROM tools that probe the upper bank. Pin
to MAME `a2grappler.cpp` on next pass.

Source: markadev/AppleII-RevEng/Orange-Micro-Grappler+ (4 KB
EPROM dump). Pinned: `grappler_card_smoke` — stub ROM fingerprint
+ data-port spool + ROM-load size gate.

### Mouse Card

Verbatim port of MAME `bus/a2bus/mouse.cpp`. Pieces:
- **M68705P3** MCU (Apple 341-0269, 2 KB mask ROM). Paced at 2× CPU
  clock from `advanceCycles()` via fractional accumulator.
- **MC6821** PIA — bus side at `$C0n0-$C0n3`.
- **8516 EPROM** — 2 KB slot ROM (Apple 341-0270-c), bank-switched
  into `$Cn00-$CnFF` via PIA PortB bits 1-3 (`bank = (PortB & 0x0E)
  << 7`).

PIA ↔ MCU bridge:
```
PIA PortA  ↔ MCU PortA            (bidir, pull-ups)
PIA PB4-7  ↔ MCU PC0-3
PIA PB1-3  → EPROM A8-10          (bank select)
MCU PB6    → slot IRQ (active low; cached, transitions only)
MCU PB7    ← mouse button (active low)
MCU PB0=X dir, PB1=X gate, PB2=Y dir, PB3=Y gate (quadrature)
```
POM2 labels X pair `X0/X1` lower-bit-first (X0=PB0=dir, X1=PB1=gate);
MAME's `mouse.cpp` uses opposite digits (X1=0x01=dir, X0=0x02=gate).
Same bits, same behaviour — only label differs; Y labels match MAME.
`updateAxis` line-for-line MAME `update_axis<>`.

Host routing: `MainWindow::onMouseMove/onMouseButton` →
`setHostMouse(rawX, rawY, button)` (clipped to screen rect). MCU
computes deltas via 8-bit subtraction with wrap; POM2 emits **at most
one quadrature edge per axis per MCU PortB read** (matches MAME
`m_last`/`m_count`).

**ROM gating**: BOTH ROMs required. Slot-config UI greys entry when
missing; `plugSlotsFromSettings` refuses with a Mouse log warn.
Defaults: `roms/mouse_341-0270-c.bin` + `roms/mouse_341-0269.bin`.

**Not modelled** (firmware-invisible): PAL16R4 chip-select sequencer
U2A, PIA PortB bit 0 sync latch, motion clamping (MCU does it).
Pinned: `mouse_card_smoke`, `mouse_card_quadrature_smoke`, and
`mouse_card_axis_parity_test` — the latter boots **real firmware**
(both ROMs) on a full M6502+Memory, drives ProDOS
`InitMouse/SetMouse/ReadMouse` from a stub, asserts identical host
ramp moves X and Y equally (caught X==Y==800 for a +800 px ramp).

#### AppleWin HLE variant — `MouseCardAppleWin` (card key `mouseaw`)

Alternative implementation, verbatim from AppleWin
`source/MouseInterface.cpp` (CMouseInterface). Same SlotPeripheral,
same `setHostMouse(rawX,rawY,button)` UI plumbing, **same slot
EPROM** (`mouse_341-0270-c.bin`) — but **no MCU mask ROM**: the
68705P3 side is a C++ command-byte state machine. Plug as
`"mouseaw"`; mutually exclusive with MAME `"mouse"`.

Protocol (mirrored from AppleWin `OnCommand`/`OnWrite` — opcodes are
high nibble of first command byte):
```
$00 MOUSE_SET     1 B   set mode (MOUSE_ON / INT_VBL / INT_BUTTON / INT_MOVEMENT)
$10 MOUSE_READ    6 B   reply Xlo, Xhi, Ylo, Yhi, status
$20 MOUSE_SERV    2 B   pending-IRQ source + CpuIrqDeassert
$30 MOUSE_CLEAR   1 B   wipe position + state
$40 MOUSE_POS     5 B   set absolute position (X16, Y16)
$50 MOUSE_INIT    3 B   clamp 0..1023, position = 0, canned $FF reply
$60 MOUSE_CLAMP   5 B   set X or Y clamp window (cmd byte bit 0 = axis)
$70 MOUSE_HOME    1 B   re-home to (0, 0)
$90 MOUSE_TIME    1..4 B no-op
```

PIA Port B as 2-line handshake (AppleWin `On6821_B`): BIT5 (PB5) =
write-strobe (firmware → "MCU"), BIT4 (PB4) = read-strobe. BIT6/BIT7
driven back to firmware for poll loops. BIT1..BIT3 still
slot-ROM bank-select (`bank = (by6821B << 7) & 0x0700`).

VBL interrupt: `OnMouseEvent(true)` fires once per ~17045 cycles
(60 Hz @ 1 MHz) from `advanceCycles`; host-input poll
(`pollHostInput`) drains atomic shadow each `advanceCycles` so
movement/button changes raise IRQ immediately when mode bits allow.
`CpuIrqAssert(IS_MOUSE)` → `assertIrq(true)`; `CpuIrqDeassert` (in
MOUSE_SERV) → `assertIrq(false)`.

Pinned: `mouse_card_applewin_smoke` — slot-ROM bank-select round
trip, size/missing-file rejection, BIT5 strobe → `OnCommand`
(MOUSE_INIT writes canned $FF to PRA).

Why ship both? `mouse` (MAME) is preferred — it boots verbatim Apple
ROMs. But the MCU mask ROM (`mouse_341-0269.bin`) is not always
available; `mouseaw` lets users with just the slot EPROM get a
working mouse.

### Joystick / paddles

`JoystickInput` polls all 16 GLFW slots each UI frame (hot-plug).
One binding drives PADL(0/1) + PB0/1/2. PADL(2/3) read centred
(127). **Paddle RC** in `Memory::softSwitchAccess`: `$C064-$C067`
returns `0x80` while `(cycleCounter - paddleLatchCycle) <
paddleValue × 11`. `$C070` arms latch. 11-cycle constant = rough
Apple II RC step.

## UI (ImGui)

`MainWindow` — menu bar + screen + emulation panel + on-demand
panels. Owns the screen GL texture. Auto-plugs Disk II in slot 6 if
`roms/disk2.rom` exists. F9 (screenshot), F11 (soft reset), F12
(hard reset) routed unconditionally even when ImGui has focus.

### MainWindow Pimpl-light

`MainWindow.h` is forward-decl-only for every plugin/panel/controller
— includes only `M6502.h` and `imgui.h`. 18 owning members behind
`std::unique_ptr<T>`; ctor/dtor/accessor bodies out-of-line so
unique_ptr destruction sees a complete type. Compile-time: `touch
CassetteDeck_ImGui.h` → 2 TUs rebuild; `touch MainWindow.h` → 4 TUs.

Non-owning `*Card` pointers (`diskCard`, `hdvCard`, …) stay raw —
`SlotBus` owns the cards.

- **MemoryViewer_ImGui** — hex + ASCII over 64 KB. Reads via
  `Memory::data()` under `stateMutex` (held by MainWindow during
  `render()`) so viewer never triggers soft-switch side effects.
  Edits go through `Memory::memWrite` (ROM protection applies).
  Per-byte change-flash via frame-counter delta. Search: hex
  sequences and ASCII (raw + high-bit-set).
- **Disassembler6502** — stateless `(mem*, pc) → mnemonic + length`.
- **main.cpp** — GLFW char/key callbacks gated by ImGui keyboard
  capture so editing widgets don't leak into Apple II.
- **Screenshot (F9)** — `screenshot_NNN.ppm` in cwd.

## Host control center (Slot Configuration + Floppy Emu)

Two host-side facilities above the slot bus — neither is a bus
device. Both are data-in / actions-out ImGui panels driven from a
snapshot `MainWindow` builds under `stateMutex` and apply the
returned actions itself (mount/eject/persist/restart).

### MountableMediaCard + SlotCardCatalog

`MountableMediaCard.h` is the capability mix-in that lets the GUI
drive *any* card with mountable media bays generically — no
`if (cardKey == "...")` ladder. Orthogonal host-side interface
(NOT a bus concern). API: `bayCount()`, `bayInfo(bay) →
MediaBayInfo`, `mountBay/ejectBay/setBayWriteBack`, plus
`bayTypeOptions/setBayType` for bays whose kind the user may pick.

- `ProDOSBlockCard` implements as a single fixed bay → both
  HDV-class cards (`ProDOSHardDiskCard`, `CffaCard`) gain a bay
  free.
- `SmartPortCard` implements directly over its 2 units, advertising
  per-bay type (`""` empty / `"35"` 3.5" / `"hdv"` HDV).

`SlotCardCatalog.h` is the single list of user-assignable card types
(`kCardTypes`, index 0 = empty) + ROM-presence probes
(`mouseRomsPresent()`, `cffaRomPresent()`) that gate conditional
entries (Mouse needs both mouse ROMs, CFFA needs
`cffa20ee02/eec02.bin`).

### Slot Configuration

`MainWindow::renderSlotConfigPanel` (`MainWindow_Slots.cpp`). One
**two-column** window (Machine → Slot Configuration) is the whole
expansion-bus control center (absorbed the old standalone "Slot
Manager" panel — `SlotManager_ImGui.*` removed 2026-05-25).

- **LEFT — card assignment.** AUX 80-col row (IIe-class) + slots 1-7.
  Each slot a `kCardTypes` dropdown, EXCEPT profile built-ins
  (`builtInSlots[s]`) which render as locked, greyed `LabelText` with
  "card — built-in …" badge. `diskii` is multi-instance (never a
  duplicate); other keys red-flag duplicates and disable Apply.
  Apply persists `slot_N_card` and calls
  `restartEmulationFromSettings()`.

- **RIGHT — internal disks + mountable ports.** Live SlotBus walk
  (`bus.peripheral(s)`, no global `*Card` pointers, so correct with
  multi cards of a kind). For each plugged card:
  - `dynamic_cast<MountableMediaCard*>` → render bays inline:
    status dot (grey empty / orange WP / green loaded / red error),
    per-bay type select (SmartPort), path InputText + Mount/Eject,
    write-back, Boot slot. Covers SmartPort (2 units), CFFA + HDV
    (1 bay).
  - else `dynamic_cast<DiskIICard*>` → internal 5.25" drives (1-2),
    each with path + Insert/Eject, Boot slot. Drive 1 persists to
    `disk_path_slotN`; drive 2 is session-only.

  Each media action takes `stateMutex` and calls `persistMediaBay()`
  (per-unit/per-slot/global keys), then `settings->save()`.

Settings: `show_slot_config` (persisted). Pinned:
`slot_multi_card_smoke_test`.

### Floppy Emu (BMOW)

`FloppyEmuDevice.{h,cpp}` + `FloppyEmu_ImGui.{h,cpp}` — model of the
BMOW **Floppy Emu** (bigmessowires.com): SD-card + OLED + 3-button
gadget that plugs into the disk port and *becomes* a drive. POM2
already emulates every drive type the Emu presents → the class models
the device's *defining* behaviour, not another FDC:

- persistent emulation **MODE** (NVRAM): 4 `FloppyEmuMode`s mapped
  onto POM2's drives: `Disk525` (140 K, Disk II), `Disk35` (800 K
  dumb 3.5"), `Unidisk35` (800 K smart, ejectable), `SmartportHD`
  (≤32 MB ProDOS block). Dual-5.25 and Smartport-Unit-2 (IIgs
  daisy-chain) modes out of scope.
- SD-card **file explorer** — bounded to SD root, `..` + dirs-first,
  case-insensitive, format-filtered per mode (`acceptsFile`: 5.25 →
  dsk/do/po/nib/woz/2mg; 3.5/Unidisk → dsk/do/po/2mg; Smartport →
  po/hdv/2mg).
- **favorites** — `favdisks.txt` in SD root: optional `automount N`
  first line (0 never / 1 first / 2 most-recent) then one image path
  per line (relative to SD root or absolute), matching real device.

Actual mounting **routed by MainWindow** into existing controller
cards (`DiskIICard` for 5.25/3.5, `SmartPortCard` units for HDV) —
device only picks the image + the mode. Core is UI/emulator-agnostic
(no ImGui / MainWindow / SlotBus) so format filtering, SD navigation,
and favdisks parsing unit-test in isolation. Ref: BMOW Floppy Emu
Model C manual §3 + §5.

`FloppyEmu_ImGui` draws the device's face: stylised 128×64
blue-on-black OLED + 3 hardware buttons (PREV / NEXT / SELECT), two
OLED views (SD File Explorer + Settings → Disk Emulation Mode).

Virtual "SD card" = `floppyemu/` (separate from Disk Library folders).
Settings: `floppyemu_mode`, `floppyemu_sd_root`, `show_floppy_emu`.
Pinned: `floppy_emu_smoke_test`.

## Profile switching internals

`SystemProfile.h/.cpp`. Pinned: `system_profile_smoke_test`.

**32 KB ROM disambiguation**: //e and //c dumps share 32 KB but
encode firmware in OPPOSITE halves. `loadAppleIIRom` takes a
`pickLower16KFor32K` flag set by `applyProfile`:

- //e (`apple2e.rom`): firmware in UPPER 16 KB (file `0x4000-0x7FFF`),
  lower = character ROM. `pickLower=false`.
- //c / //c+ (`apple2c-32Kv0.rom`, `apple2cp.rom`): TWO 16 KB banks.
  Bank 0 in LOWER half (mapped at reset, cold-start at $FA62), bank 1
  in upper (alt firmware: AppleTalk, MouseText, SmartPort).
  `pickLower=true`; upper stashed into `iicAltFirmware`.

Both halves can carry valid-looking reset vectors → can't auto-detect
from bytes. **Profile is source of truth.** When the generic
`apple2.rom` fallback resolves because no profile-specific dump is
present, the loader emits a warning.

**$C028 ROMBANK** (//c-class): MAME `apple2e.cpp:1907-1923` flips
`m_romswitch` on any `$C02x` access when `m_isiic`. POM2 mirrors via
`isIIcClass`. Alt-firmware read paths additionally require
`iicHasAltBank` (32 KB only). `resetSoftSwitches` clears `iicRomBank`
so cold-boot starts in bank 0. On II/II+/IIe, `$C02x` falls through
to cassette. Pinned: `system_profile_smoke::testIicRomBankSwitch`.

**//c-class INTCXROM override**: //c/+ have no physical slots →
internal ROM always at `$C100-$CFFF`. POM2 gates `internalIORom`
dispatch on `(MF_INTCXROM || isIIcClass)` (MAME `apple2e.cpp:1619-1631
update_slotrom_banks`). `loadAppleIIRom` and `resetSoftSwitches` set
`iieMemMode |= MF_INTCXROM` when `isIIcClass`. Pinned:
`testIicInternalRomAlwaysMapped`.

**Built-in slot locks** (`ProfileConfig::builtInSlots`): each profile
carries `std::array<std::optional<BuiltInSlot>, 8>`. //c locks sl2
(SSC), sl4 (Mouse), sl6 (Disk II). //c+ adds sl5 (SmartPort 3.5" via
IWM). `plugSlotsFromSettings` overrides `slotCards[s]` with forced
cardKey regardless of persisted `slot_N_card`. `renderSlotConfigPanel`
renders locked slots disabled with "built-in" badge. Pinned:
`testBuiltInSlots`.

**//c+ MIG + IWM handshake** (//c+-only): alt firmware (bank 1)
drives a MIG gate-array + IWM. POM2 models the minimum for cold boot:

- **MIG** (MAME `apple2e.cpp:598-704 mig_r/mig_w`). Profile hosts
  `migRam[0x800]`, `migPage`, `migIntDrive`, `migHdSel`; routes two
  MIG windows in bank-1 expansion ROM **only when `isIIcPlus &&
  iicRomBank`**:
  - `$CC00-$CCFF` → `migOffset 0x000-0x0FF` (drive enable/disable,
    IWM reset)
  - `$CE00-$CEFF` → `migOffset 0x200-0x2FF` (MIG RAM + auto-incr,
    3.5" head select, MIG page reset)

  3.5"-side decodes → `SmartPortHub::setMig35Sel`/`setMigIntDrive`;
  hub's `recalc_active_device` (verbatim MAME `apple2e.cpp:724-770`).
  MAME `:1917-1922` resets `migPage + m_intdrive + m_35sel` on
  ROMSWITCH → bank 0; POM2 mirrors. `migWrite(0x40)` calls
  `iwmDevice->reset()` so alt firmware's per-boot IWM reset clears
  stale state.

- **IWM mode register + WHD handshake** on `DiskIICard` (MAME
  `iwm.cpp:103-114 read / 256-269 mode_w`). DiskIICard tracks
  `iwmMode` + resting `iwmWhd = 0xBF` and intercepts `$C0nE/$C0nC/
  $C0nF` combos in both LSS path and legacy gate. Plain Disk II
  software never drives Q6+Q7 to mode-set state, so existing tests
  unaffected; alt firmware's IWM probe at `$E512-$E522` and
  write-ready loop at `$C8A6-$C8A9 / $C960-$C965` both clear with
  these hooks. Without them //c+ Monitor cold-reset hangs before any
  banner.

**Profile switching = full cold reset** via
`MainWindow::applyProfile(SystemProfile)`. Order matters:

1. Stop worker.
2. Tear down slot cards under state mutex (Mockingboard's
   `AudioSource` detached from `AudioDevice` FIRST).
3. Wipe RAM/aux/LC + reset soft switches.
4. **`setIIEMode(...)` BEFORE `loadAppleIIRom`**.
5. Load ROMs (with `pickLower16KFor32K`).
6. Re-plug slots from settings.
7. Re-mount previously inserted disks/HDVs.
8. `resolveCpuMode()` (honours `cpu_mode_override`).
9. Reset cycles/frame.
10. `hardReset()`.
11. Restart worker.
12. Persist `system_profile`.
13. Refresh GLFW window title.

CLI `--preset` triggers the same path (after legacy auto-probe —
wins). Aliases: `apple2`, `apple2plus`, `iie-u` / `iieunenhanced` /
`apple2e-1983`, `apple2e`, `apple2c`, `apple2cplus`, `//e-u`, `//e`,
`//c`, `//c+`. `cpu_mode_override` = `auto|nmos|65c02`.

## CLI (CliDispatcher)

`CliDispatcher` (parser, no `EmulationController` dep) + `CliRunner`
(Phase-C runner — split out so parser is unit-testable). Three
phases: **A** parse, **B** pre-boot
(preset/ROM/snapshot-load/`--load addr:file`), **C** post-boot
(tape ops/paste/run/step).

Flags: `--preset ii|ii+|iie-u|iie|iic|iic+`, `--speed`, `--cpu-max`,
`--tape`, `--35-disk1 path`/`--35-disk2 path`, `--load addr:file`,
`--run`, `--paste`, `--step`, `--play`/`--rec`/`--rewind`,
`--snapshot-save`/`--snapshot-load`.

**Positional disk + `--kiosk`**. First non-flag arg → `CliPlan::
bootDiskPath`; `--kiosk` → `CliPlan::kiosk`. `main.cpp`:

- Picks slot by content via `classifyDiskForSlot(path)`, then calls
  `MainWindow::insertAndBootImage(path, err)` (shared with Disk
  Library UI; `routeMount35`/`routeMountHdv` are `MainWindow` methods
  so both callers route identically — SmartPort unit auto-create,
  //c+ on-board hub, HDV card vs SmartPort unit 0). 5.25" →
  `DiskIICard::insertDisk` + `bootFromSlot`.

- **HDV auto-provision**: an HDV needs an HDV/SmartPort card. A saved
  config may have only Disk II cards. `ensureHdvCardForBoot()` plugs a
  `ProDOSHardDiskCard` into a free slot (prefers 7) for the session
  if none present. Plug **not persisted** — user's GUI config stays
  untouched.

- **No persistence in kiosk**: `~MainWindow`'s `settings->save()` is
  gated `if (!kiosk_)`. `imgui.ini` is also disabled. Bare `POM2
  <disk>` in GUI *does* persist.

- Defers boot to small frame countdown in main loop (UI thread between
  frames, after worker is up + slots plugged) → no race with CPU
  thread.

- `--kiosk` → exclusive full-screen from primary monitor's video mode
  (`glfwGetVideoMode` + `glfwCreateWindow(.., monitor, ..)`);
  `io.IniFilename = nullptr`; `setKioskMode`. `render()`
  short-circuits to `renderKiosk()` — one borderless full-viewport
  window (`drawScreenImage()` letterboxed on black), no menu / toolbar
  / panels / dialogs. Window closes only via the OS.

Pinned: `cli_kiosk_test` (parser links against just `DiskImage.cpp`).

## Clock & threading

`POM2_CPU_CLOCK_HZ = 1 022 727` (14.31818 MHz / 14). 65-cycle "long
cycle" TV alignment NOT modelled. Three modes in
`EmulationController`: **Stopped** (50 ms idle), **Running**
(`cyclesPerFrame` per 60 Hz tick), **Step** (one instruction).
`M6502::run(maxCycles)` returns *actual* cycles → passed to
`Memory::advanceCycles()` so paddle RC stays synced. Single
`stateMutex` guards CPU + Memory.

CPU → audio/UI events carry an `emuCycles` stamp. Consumers measure
cadence in emulated CPU cycles, not wall-clock frames (disk-turbo
bumps the CPU to ~60×, which collapses wall-clock gaps to zero
across an audio-buffer tick). Canonical example:
`FloppySoundDevice::drainCommands` uses the cycle stamp passed by
`DiskIICard::seekPhaseW`.

## WebAssembly (browser build)

Driver: `build_wasm.sh` → `dist/wasm/{index.html, POM2.js, POM2.wasm,
POM2.data, serve.py}`. Per-folder doc: `dist/wasm/README.md` (build,
deploy, caching hints). User-facing summary lives in `README.md`
§ "WebAssembly (browser)".

**Single-threaded by design**. No `std::thread`, no `SharedArrayBuffer`,
no COOP/COEP — runs on any static host (GitHub Pages, Cloudflare
Pages, plain S3). The CPU worker thread is replaced by
`EmulationController::tickFrame()` called from the render loop in
`main.cpp` (look for `#ifdef __EMSCRIPTEN__`). Trade-off vs the
native build: no parallel audio thread, but miniaudio's Web Audio
backend runs in a browser-managed worklet anyway, so the difference
is invisible in practice.

**CMake Emscripten branch** at `CMakeLists.txt:212-276`:

- `-sUSE_GLFW=3 -sUSE_WEBGL2=1 -sFULL_ES3=1` — Emscripten ships
  GLFW3 + WebGL2 ports built-in, so the ImGui GLFW/OpenGL3 backends
  link unchanged.
- `-sINITIAL_MEMORY=134217728` (128 MiB) `-sALLOW_MEMORY_GROWTH=1` —
  grows on demand; 128 MiB is enough for a IIe with RamWorks III
  + a few mounted HDV images.
- `-lidbfs.js` + `-sFORCE_FILESYSTEM=1` — IndexedDB-backed filesystem
  mountable at `/persistent` via `FS.mount(IDBFS, …)` in the shell
  preRun hook (see `wasm/shell.html`). **Not yet wired to
  `Settings.cpp`** — see TODO 🟡 [WASM] IDBFS settings persistence.
- `--preload-file roms@/roms fonts@/fonts` — baked into `POM2.data`
  at build time. Disks are opt-in via `-DPOM2_WASM_BUNDLE_DISKS=ON`
  (folds in `disks/`, `disks35/`, `hdv/`, `floppyemu/`).
- `pom2_headless` target is skipped under EMSCRIPTEN
  (`CMakeLists.txt:332`) — no TCP listener, no terminal.

**Compile-out gates** (sandbox-incompatible POSIX bridges, guarded
by `#ifdef __EMSCRIPTEN__`):

| Subsystem | Stub behaviour | Apple II side |
|---|---|---|
| Super Serial Card TCP listener (`SuperSerialCard.cpp:153`, `:203`, `:227`, `:241`, `:366`) | `startListening` returns false + logs; `acceptClient`/`pollRx`/`writeTx` no-op | ACIA still emulated — software inside the Apple II can still PR#2 / read $C0A9; just no host network bridge |
| AiControlServer HTTP listener (`AiControlServer.cpp:381-430`) | `start()` returns false; `stop()` no-op | None — entire feature is a host-side control plane |

The symbols stay declared so every caller still links — only the
implementation degrades. **Rule for editors of these two files**:
keep the `#ifdef __EMSCRIPTEN__` guards intact; new socket calls
must have a no-op WASM branch returning a safe sentinel
(`false`/0/empty), not `#error`.

**Asset resolution**. `ResourcePaths` searches CWD-relative paths
(`./roms/apple2.rom`, etc.). Under Emscripten the CWD is `/` and
preloaded folders live at `/roms`, `/fonts`, `/disks`, … — same
relative shape, so probes resolve unchanged. The native
exec-relative path (added in d582b2f for Linux dist) is also
applied via the IDBFS mount path for future user uploads.

**Known gaps** (tracked in `TODO.md`):

- IDBFS settings persistence not wired → `state.cfg`/`imgui.ini`
  reset on every page reload.
- No file picker / drop-zone for user disks (`.dsk`/`.woz`/`.hdv`).
- No touch input on mobile (GLFW3-EM doesn't synthesise
  touch→mouse outside the canvas).
- Audio worklet latency not tuned.

**No CI yet** — `./build_wasm.sh --clean` can regress silently on
refactors of `main.cpp`, `MainWindow.cpp`, `EmulationController.cpp`,
`AiControlServer.cpp`, `SuperSerialCard.cpp`, or `CMakeLists.txt`.
Run it manually after touching any of those.
