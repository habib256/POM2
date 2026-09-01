# Le Chat Mauve, at the silicon — what the cards do, what POM2 does, and the plan

*2026-09-01. Research pass over every primary source that could be found on
the public Internet (manuals, the Video-7 patent, the Eve's PLA dump, the
measurements AppleWin's contributors made on real cards), read against
POM2's `LeChatMauveCard` and the Chat Mauve render paths in
`Apple2Display`. The point of this document is precision: each rule below
carries where it comes from, and each gap says what would close it.*

## 0. In one paragraph

POM2 today models **one** Chat Mauve: a Féline-class 2-bit mode latch with
the four Video-7 DHGR interpretations (560 mono / 140 colour / mixed /
160 chunky), a Video-7-style fg/bg colour TEXT, two "Eve" toggles at
`$C0B8-$C0BB`, and the palette AppleWin captured off a Féline. That is a
good Féline / //c-adapter at the **byte** level, and a wrong Eve: the Eve
has **sixteen write-only switches at `$C0B0-$C0BF`, a data register (CPREG)
that the card writes into auxiliary memory behind the CPU's back, ten
graphics modes selected by AN3 + three switches, colour text whose nibbles
are the other way round from Video-7's, and a colour decoder that is a
PLS100 PLA whose fuse map is public** — and none of that is modelled.
The plan below takes the Féline to dot-level exactness first (it is what
Extasie and DIX need), then builds the Eve from its manual and its PLA,
then the RVB Graph and the //c adapter's quirks, on a dot-clock video tap
that all of them share.

## 1. The family, and what each card can do

Four products carried the name (plus the Apple-branded //c adapter, which
is a Féline without RAM). Sources: the Eve reference manual (archive.org,
OCR), the Féline manual (apple-iigs.info), fenarinarsa's survey and his
measurements on a //c adapter and, through a friend, an Eve (AppleWin
#764/#850), the Video-7 patent US 4,631,692.

| Mode | RVB Graph (II/II+) | **Eve** (//e, aux slot, 64 K) | **Féline** (//e, aux slot, 64 K) | **Adaptateur //c** |
|---|---|---|---|---|
| TEXT 40/80 mono, GR, HGR (LCM colour rules) | ✓ | ✓ | ✓ | ✓ |
| HGR colour variants SPEC1 / SPEC2 / DASH / HRBW | — | ✓ (HR1-3) | HRBW only (AN3 off) | HRBW only (AN3 off) |
| HGR COL280A / COL280B (280×192, 4 colours) | — | ✓ | — | — |
| HGR CP280 (280×192, fg/bg per 7 dots, 16 colours) | — | ✓ | — | — |
| DHGR COL140 | ✓ | ✓ | ✓ | ✓ |
| DHGR BW560 | ✓ | ✓ | ✓ | ✓ |
| DHGR **mixed** COL140 + BW560 | — | **—** | ✓ | ✓ |
| DHGR 160×192 chunky (Video-7 / Apple RGB only) | — | — | — | — |
| Double lo-res | — | **✗** | ✓ | ✓ |
| TEXT colour (fg/bg per character, aux byte) | whole-screen register | ✓ (TXT16) | — | — |
| TEXT green monochrome | ✓ ($C0F1/3) | ✓ (TXTGREEN) | — | — |
| Registers | `$C0F0-$C0F3` (+ HGR colour regs) | `$C0B0-$C0BF` | none | none |
| Mode latch (AN3 clocks /80COL) | ? | ✓ | ✓ | ✓ |

Two hardware facts that matter for a model:

- **The Eve and the Féline occupy the auxiliary slot** and see the
  motherboard's `~80COL`, `VID7`, `AN3`, `TEXT`, `GR`, `SEGB`, `14M`,
  `LDPS` directly. **The //c adapter hangs off the DB-15 and has only 14M,
  VID7M, ~LDPS, TEXT, GR, SEGB** — it has to *infer* 80COL from the timing
  of VID7M/LDPS. That inference is the documented source of its erratic
  mode switches (Prince of Persia's title screen dropping to mono after
  the first attract loop; fenarinarsa's three sheets of non-reproducible
  patterns). An Eve on a //e shows PoP correctly.
- **The Eve rev A applies the wrong DHGR colours to the 4-bit patterns**
  (it shipped before Apple's DHGR); rev B fixed it. The manual's addendum
  gives the difference exactly: its colour-table program for showing a
  rev A picture on a rev B (`DATA 0,8,1,9,2,10,3,11,4,12,5,13,6,14,7,15`,
  poked into the sixteen table entries) maps rev-B code *i* to rev-A code
  `(i >> 1) | ((i & 1) << 3)` — the 4-bit code **rotated right by one**,
  the same rotation AppleWin's DHGR path applies. No capture is needed.

## 2. What POM2 models today, and where it departs

`LeChatMauveCard` (161 + 107 lines) + `Apple2Display::renderDhgr /
renderHiResChatMauve80 / renderHgrDuochrome / renderTextChatMauveFgBg`.

| Area | POM2 today | Hardware | Verdict |
|---|---|---|---|
| Mode latch | 2-bit FIFO, shifts 80COL on the `$C05E→$C05F` edge, resets to `11` (COL140), `an3Prev` starts high | Patent: 2-bit shift register, clock AN3, data **/80COL**, power-on **set** (F1=F2=1). No precondition on MIXED/HIRES (measured). | Correct up to the data polarity convention (POM2 clocks 80COL and numbers its enum accordingly; AppleWin clocks /80COL). Keep, document. |
| DHGR interpretations | MAME `dhgr_update` port: COL140, Mixed (per **source byte** MSB), Chunky160, BW560 | Féline/IIc: no 160 mode — falls back to COL140. Mixed: 4-dot grid, byte-boundary rules (§ 3.3). | 160 must fall back on Féline; mixed boundary rules are the byte-level approximation, not the measured dot rules. |
| HGR with the card | 280-wide LCM path (`renderHiResChatMauve80`) — check against § 3.4 | 2-bit cell colour + 3-bit window: only `010` / `101` are coloured | Verify pixel-for-pixel; AN3 off → HGR **mono** on Féline/IIc (`POKE -16290,0`). |
| TEXT colour | Video-7 F/B: aux byte **hi = fg, lo = bg**, enabled when 40-col text + AN3; "Eve" toggle `$C0B8/9` | Eve TXT16: aux byte **hi = background, lo = foreground** (manual IV-2.2, III-2: `POKE -16199,16*F+C`), needs 80COL **off**, `$C0B9` on / `$C0B8` off, and CPREG auto-writes the aux byte | Nibble order is inverted for the Eve; CPREG mechanism absent. |
| `$C0BA/$C0BB` | "HGR Duochrome" toggle | **TXTGREEN**: green monochrome text 40/80 (`POKE -16197,0` on, `-16198` off) | Mislabelled; the fg/bg HGR the code renders is the Eve's **CP280** (AN3 off, HR1+HR2+HR3 on, 80COL on), selected elsewhere. |
| `$C0B0-$C0B7`, `$C0BC-$C0BF` | not decoded | ENHRCPREG, HR1, HR2, HR3, LOCKCPREG, LOCKRES; every write also loads **CPREG** | Missing entirely. |
| Reset | `onReset` re-arms the latch | Eve: Ctrl-Reset clears all sixteen switches unless LOCKRES; latch set at power-on | Add. |
| Palette | AppleWin `PaletteRGB_Feline` (two distinct greys) | Féline capture, white-balanced; Eve rev B "same as NTSC but grey1/grey2 differ"; rev A wrong DHGR colours | Fine for Féline; Eve untested; TTL vs analog output not modelled. |
| Beam racing | per-frame mode | the card is combinational on the dot stream; switches take effect at the dot | Mid-line switches (DIX) need the dot-level tap (§ 4). |

## 3. The hardware, rule by rule

### 3.1 The mode latch (all cards with DHGR)

US 4,631,692, FIG. 1: *"a two bit shift register which uses AN3 as its
clock and 80COL as its data input"*, `~80COL` being the only polarity the
hardware sees; *"set upon power-on such that F1 and F2 get initialized to
their 'on' state"*. Measured on the //c adapter (fenarinarsa, 2020-10):
switching `$C05E → $C05F` injects the **current** `~80COL` into F1 and
pushes F1 into F2; **no precondition** — MIXED on, HIRES off, 80STORE off
all still clock. The only requirement is that 80COL was *written* since
the last `$C05E→$C05F` (its detection is what a toggle resets). Resulting
modes, in AppleWin's numbering (F2 F1 from `~80COL`):

| clocked (80COL then 80COL) | F2 F1 | Video-7 / Apple RGB | Féline / IIc | Eve |
|---|---|---|---|---|
| `$C00C`, `$C00C` | 1 1 | 560 mono | 560 mono | BW560 |
| `$C00D`, `$C00D` | 0 0 | 140 colour | 140 colour | COL140 |
| `$C00C`, `$C00D` | 1 0 | **mixed** | **mixed** | (unsupported → 140 colour) |
| `$C00D`, `$C00C` | 0 1 | 160 chunky | (unsupported → 140 colour) | (→ 140 colour) |

Reference sequences (Arlequin / Extasie, `Extasie Reloaded` annexe 6):

```
STA $C050 ; STA $C054 ; STA $C052 ; STA $C057 ; STA $C05D
STA $C00C ; STA $C05E ; STA $C05F        ; bit 1 (80COL off)
STA $C00D ; STA $C05E ; STA $C05F        ; bit 2 (80COL on)  → mixed
STA $C05E                                ; DHGR on
```

On the //c the `$C05E/$C05F` pair is reached through IOUDIS (`$C07E`),
and PoP's `STA ADCOLon / BIT HIRESon` order is enough to derail the
adapter's inferred 80COL — a real quirk of that box, not of the Féline.

### 3.2 The Féline's HGR (also the //c adapter; presumably the Eve's HRAPPLE)

fenarinarsa, verified against every bit combination on the //c adapter;
GrafX2 `Update_color_hgr_pixel` implements it; AppleWin
`UpdateHiResRGBCell` too:

1. Slice the 280 dots of a line into 140 **cells of 2 bits**. `01` → colour
   1, `10` → colour 2 (`00`/`11` no colour). The byte's bit 7 picks the
   pair: 0 → violet / green, 1 → blue / orange (AppleWin: `offset ?
   palette[1+c] : palette[6-c]`).
2. For each dot look at it with its two neighbours (3-bit window; the dot
   before the first and after the last are 0). **`010` or `101` → the middle
   dot takes its cell's colour. Anything else → white if 1, black if 0.**
   No half-dot shift, no fringing, no NTSC.

AN3 **off** in HGR makes the Féline/IIc render HGR in **monochrome** (the
Féline manual's `POKE -16290,0`; Ctrl-Reset restores colour). On the Apple
RGB card that same state is the F/B (duochrome) mode; on the Eve it is not.

### 3.3 The Féline's mixed DHGR — the mode Extasie was written for

Official statement, *Manuel Arlequin*: *"Mode mixte : uniquement Féline
IIe et //c avec interface Péritel/RVB. La carte Eve n'est pas compatible
avec ce mode. Le bit 7 de chaque octet est utilisé dans l'affichage mixé.
Si ce bit est à 0, l'octet est destiné à être affiché en monochrome, sinon
en couleur"* and, decisive for the dot rules: *"**le bit 7 de l'octet dans
lequel se trouve le bit 0 du quadruplet** détermine la couleur de
celui-ci"* — a 4-bit colour cell ("quadruplet") is coloured or not by the
bit 7 of the byte that holds the cell's **first** bit. The cell grid is
fixed to the line: 7 cells per 4 bytes,

```
Aux      Princ    Aux      Princ
76543210 76543210 76543210 76543210     bits
X2103210 X1032103 X0321032 X3210321     which quadruplet each bit feeds
```

and the manual adds: *"il est fortement conseillé que les 4 bits 7 des
octets d'une cellule soient dans un même état"* — because the boundary
cases are where the hardware does something the manual would rather not
document. AppleWin's rules, **validated against the //c adapter's output**
(PR #837, `UpdateDHiResCellRGB`):

- bit 7 of each byte defines the mode of the following 7 bits; BW pixels
  are 1 dot, colour pixels 4 dots;
- a colour cell that crosses into a BW byte is **cut** there (less than 4
  dots wide);
- a BW cell that crosses into a colour byte has its **last BW dot
  repeated** until the next cell boundary.

fenarinarsa's first note (2020-02) said the switch only takes effect "from
the next 4-bit sequence"; the later, validated PR refined that into the
two rules above. **Both are consistent with the patent's mechanism**: the
byte's VID7 selects the 560 path or the 140 path *for the next seven 14 MHz
periods*, while the 140 path's 4-bit latch keeps running on its own grid.
The plan models exactly that (mux per byte, latch per 4 dots) and pins the
three cases with Extasie's slideshow. Dragon Wars encodes bit 7 the other
way round; POM2's `invertBit7` stays as the compatibility switch.

### 3.4 The Eve: switches, CPREG, and table IX-1

Manual, chapter IX (p. 117-119). Write-only, **all off at power-on**, at
the slot-3 addresses `$C0B0-$C0BF` (−16208 … −16193). **Every write to any
of them also latches the data byte into CPREG** (dot colour in the low
nibble, background in the high nibble). Reading returns nothing useful.

| off | on | Switch | Effect |
|---|---|---|---|
| `$C0B0` | `$C0B1` | **ENHRCPREG** | off: CPREG stops acting on `$2000-$3FFF`; on: CPREG also mirrors HGR writes. *Must be off while AN3 is on.* |
| `$C0B2` | `$C0B3` | **HR1** | see table |
| `$C0B4` | `$C0B5` | **HR2** | see table |
| `$C0B6` | `$C0B7` | **HR3** | see table |
| `$C0B8` | `$C0B9` | **TXT16** | on + 80COL off: 40-col text is colour text, each char's aux byte = its colours (**hi nibble = background, lo = foreground**). With LOCKCPREG off, CPREG is written to aux on every character sent to the text page. |
| `$C0BA` | `$C0BB` | **TXTGREEN** | white → green monochrome text, 40 and 80 col. |
| `$C0BC` | `$C0BD` | **LOCKCPREG** | on: CPREG frozen (no auto-write, text or CP280). |
| `$C0BE` | `$C0BF` | **LOCKRES** | off: Ctrl-Reset clears all sixteen switches; on: the card survives Ctrl-Reset. |

**CPREG's auto-write is the LLE-relevant mechanism**: when it is "en
fonction", a CPU write to main `$0400-$07FF` (text page) or `$2000-$3FFF`
(HGR page, only with ENHRCPREG) makes the card write CPREG's value into
**auxiliary** memory at the same address. That is how `PRINT` in colour
text and HPLOT in CP280 get their colours without the program touching aux
— and it is a `Memory` write hook, not a video rule. Purplesoft's
`&COLOR=` / `&BACK=` and the Pascal unit drive it; the manual's BASIC-only
recipe is `POKE -16199,16*F+C` (TXT16 on with CPREG = F<<4 | C) then
`HOME`.

**Table IX-1 — choosing a graphics mode** (read from the scanned page 118;
the manual prints "COL280A" on both 4-colour rows, the second is COL280B
by chapter VI):

| AN3 | HR1 | HR2 | HR3 | Mode | Note |
|---|---|---|---|---|---|
| on | off | off | off | HRAPPLEII — standard HGR | |
| on | on | · | · | HRSPEC1 — HGR, isolated colour dots on white → black | |
| on | · | on | · | HRSPEC2 — SPEC1 + isolated colour dots on black → white | |
| on | · | · | on | HRDASH — coloured horizontal lines drawn dotted | |
| on | off | on | on | HRBW — HGR in black and white | |
| off | off | off | off | **COL140** — 140/line, 16 colours | needs 80COL on |
| off | on | off | off | **COL280A** — 280/line, 4 colours (black, orange, green, white) | needs 80COL on |
| off | off | on | off | **COL280B** — 280/line, 4 colours (black, light blue, pink, yellow) | needs 80COL on |
| off | on | on | off | screen blanked to black | CPREG keeps working (LOCKCPREG off, ENHRCPREG on) |
| off | off | off | on | **BW560** — 560/line, black and white | needs 80COL on |
| off | on | on | on | **CP280** — 280/line, fg/bg per 7 dots, 16 colours | 80COL on; CPREG active iff LOCKCPREG off and ENHRCPREG on |

Mode semantics (manual IV-2, IV-3, VI-3):

- **HRSPEC1/2, HRDASH, HRBW** are decoder variants of the LCM HGR rule of
  § 3.2: SPEC1 removes the "colour dot on a white background" case
  (window `x1x` with the cell coloured → black), SPEC2 additionally turns a
  colour dot on black into white, DASH renders the horizontal runs of the
  four HGR colours as dotted lines, HRBW is the 560-style monochrome.
- **CP280** ("coloriage foreground-background par série de sept points"):
  main byte = 7 dots (LSB leftmost), aux byte at the same address = colours,
  **high nibble background, low nibble foreground**. This is what POM2
  currently calls "HGR Duochrome", with the nibbles swapped.
- **COL280A/B**: 280 dots, 2 bits per dot ("point par point"), 16 K — one
  bit from main and one from aux per dot, two fixed palettes; the bit
  order (which bank is the LSB) is not in the manual's prose and must be
  read out of Purplesoft's `&PLOT` (disassembly) or the PLA.
- **COL140 / BW560**: as on the Féline. No mixed mode; no double lo-res.
- Colour numbering is Apple's lo-res order (0 black, 1 magenta, 2 dark
  blue, 3 purple, 4 dark green, 5 grey 1, 6 medium blue, 7 light blue,
  8 brown, 9 orange, 10 grey 2, 11 pink, 12 green, 13 yellow, 14 aqua,
  15 white).
- Text: TXT16 as above; TXTGREEN; 80-col text is mono only.
- **Charsets** are software (Purplesoft `&CHRS n`, eight fonts drawn on
  the graphics page) — nothing in the card.

### 3.5 The Eve's colour decoder is a PLA, and its fuse map is public

`Chat_Mauve_eve_PLA.jed` (Apple II Documentation Project, ROM Images):
Signetics **PLS100 / 82S100**, 16 inputs, 8 outputs, 48 product terms,
1928 fuses. Decoded with the usual "0 = intact" convention it yields 48
clean terms (no contradictory literals), output polarity fuses
`[0,0,0,0,0,0,1,1]` (F6, F7 active-low). The structure is legible without
the schematic:

- **I4** splits the whole map: `/I4` terms use I1-I3, I5-I11 as a pixel
  window and I12-I15 as mode bits; `I4` terms are short (`I0 & I4`,
  `/I0 & I4 & /I15`, `I4 & I9`) — I4 is almost certainly the **TEXT/GR
  side** of the decode (or AN3), the `/I4` side the graphics decoder.
- **I12, I13, I14, I15** behave as mode selects (`I12 & /I14`, `I12 &
  I14`, `/I12 & /I13`, `/I12 & I13`) — the HR1/HR2/HR3/AN3/80COL family.
- **F1, F2, F4, F5** are set in the combinations a 4-bit colour code would
  produce (`F1,F2,F4,F5` = white; `F2,F4`; `F1,F5`; …): the **R, G, B,
  I** outputs. F0/F3 and the inverted F6/F7 look like blanking / sync /
  path-select lines.

The plan's Eve decoder is that PLA evaluated per dot (a 64 K-entry truth
table, trivially fast), with the pin roles pinned by consistency against
table IX-1 and the demo disks. That is as LLE as an Eve can get without
the board schematic, which is not public.

### 3.6 The RVB Graph (II / II+)

Registers at the slot-7 device select (system-cfg forum, from a Sonotec
clone's analysis): `POKE -16144,0` ($C0F0) colours + white text,
`-16143` ($C0F1) colours + green text, `-16142` ($C0F2) monochrome white,
`-16141` ($C0F3) monochrome green. fenarinarsa: a whole-screen **text
colour register** (any of the 16 GR colours), **HGR colour registers**
(choose the 6 HGR colours among the 16) and a **dotted-line** option.
Output is RGBI over a 74LS164 + 74LS175 (four dots at 14 MHz). The manual
has not been found online; the HGR colour registers stay a gap.

### 3.7 Palette, outputs, timing

- Féline: AppleWin's `PaletteRGB_Feline` (white-balanced OSSC capture;
  greys 5 and 10 differ). POM2 uses it.
- Eve rev B: "a bit different from NTSC, grey1/grey2 differ" — no capture.
  Rev A: rev B's palette with the 4-bit code rotated right by one (§ 1,
  from the addendum's colour-table program) — no capture needed.
- The cards have **two connectors**: analog RGB (Péritel, with three trim
  pots for R, G, B gain — Féline manual p. 13) and **TTL RGB**. An RGBI
  TTL output has exactly 16 colours; the analog one is those 16 through
  resistor networks. A "TTL" palette (pure RGBI) is the honest second
  option next to the captured one.
- PAL 50 Hz: the //c PAL and //e PAL profiles already run 312 lines; a
  Chat Mauve plugged on an NTSC profile should keep the machine's timing
  (the card does not generate sync) — offer auto-PAL as a profile
  convenience only.

## 4. Architecture: a dot-clock video tap, and cards as logic on it

Today's renderers work per frame, per byte, per mode. The cards are
combinational logic on the motherboard's **video stream** — `VID7M`
(serialised dots), `VID7` (the latched byte's bit 7), `LDPS` (byte latch),
`TEXT`, `GR`, `SEGB`, `~80COL`, `AN3`, `14M`, blanking — and sequential
logic no deeper than a 4-bit latch and the 2-bit mode register. The
faithful architecture is therefore:

```
Memory (main/aux, soft-switch edge log with cycles)
   │
   ▼
VideoTap        per scanline: 560 dots + per-byte VID7 + line-state
                (TEXT/GR/SEGB/80COL/AN3) sampled at the byte, from the
                existing beam-raced event log (mid-line switches for free)
   │
   ├─▶ FelineDecoder   mode latch · 4-dot cell latch · per-byte 560/140 mux
   │                   · HGR 2-bit cell + 3-bit window · AN3-off mono
   ├─▶ EveDecoder      $C0Bx switches · CPREG · PLA truth table on the
   │                   window/mode inputs · TXT16/TXTGREEN · LOCKRES
   ├─▶ RvbGraphDecoder $C0Fx · text colour reg · HGR colour regs
   └─▶ IIcAdapter      = Feline + inferred-80COL quirk model (optional)
   │
   ▼
560 × 192 RGB frame (existing GPU/CRT stages unchanged)
```

Cost: 560 × 192 × 50 = 5.4 M dots/s, a few ns each — negligible against the
NTSC shader path. The tap replaces the per-mode branches in
`Apple2Display` for the Chat Mauve case only; the NTSC/MAME paths stay.

Two things live outside the video path: **CPREG's auto-write** is a
`Memory::memWrite` hook (main `$0400-$07FF` / `$2000-$3FFF` → aux mirror,
gated by TXT16 / ENHRCPREG / LOCKCPREG), and **LOCKRES** hooks the reset
path (`onReset` clears the switches unless locked).

## 5. The plan

Estimates are for one person; each phase ends green with its own pinned
tests and a CHANGELOG entry, in the repo's usual form.

**P0 — Corpus and harness (½ d).** Register the material in
`docs/test_corpus.md`: the two Chat Mauve demo sides (Pascal
`EDITEUR`, ProDOS `ARLEQUIN` with `GLI16.2` and the `*.CHAR` fonts),
Purplesoft DOS 3.3 (Juillet 83, Rev. B Octobre 83 — `DEMO GR16K`, `TEXT
DEMO`), Purple Pascal 1.1, Arlequin boot + editor, **Extasie** (program,
slideshow, `DSP.IMG` viewer with the `$F2` images), `Eve Leonard`. Extend
the golden-hash harness with a "card variant" axis and a per-dot RGB dump
(`--dump-rgb-line`), so every rule below is pinned by an image, not a
sentence.

**P1 — The Féline and the //c adapter, dot-exact (1½ d).** The mode latch
as the patent draws it (already there; add the "80COL must have been
written" detail and the 160 → 140 fallback). The mixed mode as a per-byte
560/140 mux over a free-running 4-dot cell latch, pinned on Extasie's
slideshow and on three synthetic boundary cases (colour→BW cut, BW→colour
repeat, aligned). The LCM HGR rule (2-bit cell, 3-bit window) checked
pixel-for-pixel against `UpdateHiResRGBCell`; AN3-off HGR mono
(`POKE -16290,0`). Retire the byte-level MAME mixed rule for this card.
Golden hashes: Extasie slides, DIX's Chat Mauve screens, PoP title (both
adapter behaviours).

**P2 — The Eve's switches and CPREG (1½ d).** Full `$C0B0-$C0BF` decode,
CPREG latch on every write, the auto-write hook in `Memory`, TXT16 with
the Eve nibble order, TXTGREEN (drop the "HGR Duochrome" label — the mode
it renders is CP280 and is selected by table IX-1), LOCKCPREG, ENHRCPREG,
LOCKRES on Ctrl-Reset, all switches off at power-on. Table IX-1 wired to
the existing renderers where they already exist (COL140, BW560, CP280 =
today's fg/bg HGR with nibbles swapped) and stubs that blank correctly
for the rest. Card **variant** in the catalog / Slot Config: *Féline*,
*Eve rev B*, *Adaptateur //c*, *RVB Graph* — the variant decides which
registers exist and which modes fall back. Snapshot v3. Pinned by
`Purplesoft DEMO GR16K` and `TEXT DEMO` goldens and a register test
driven from BASIC POKEs.

**P3 — The Eve's pixel rules, from the PLA (2-3 d, research).** Pin the
PLS100's pin roles: assume the 7-dot window and mode inputs, evaluate the
decoded terms against COL140 / BW560 / HRAPPLE (whose outputs are known)
until the assignment is unique, then read COL280A/B, CP280, SPEC1/2, DASH
straight out of the fuse map — the Eve decoder becomes the PLA itself.
Cross-check with Purplesoft's `&PLOT` for the COL280 bit order. Where the
PLA leaves a choice (rev A palette, the "blanked" row), document it.

**P4 — RVB Graph (1 d, gated on its manual).** `$C0F0-$C0F3`, the text
colour register, the HGR colour registers and dotted lines, on the II+
profiles. Blocked until the RVB Graph manual or a board photo surfaces;
the plan records exactly which forum threads to ask.

**P5 — //c adapter quirk (½-1 d, optional).** Model 80COL as inferred from
the VID7M/LDPS cadence rather than read from the switch, behind a toggle,
so the PoP regression reproduces as on the real box. Off by default.

**P6 — Video tap (1-2 d).** Move the Chat Mauve renderers onto the
per-dot tap driven by the existing soft-switch event log, so mid-scanline
mode changes (DIX) and per-line register changes land at the dot; PAL
convenience; TTL-RGBI palette option next to the capture; UI panel shows
the sixteen Eve switches and CPREG live.

**P7 — Docs.** `DEV.md` § Le Chat Mauve rewritten from this document,
`docs/lle_vs_hle.md` row moved from "H1 + machine-level lie" to the level
each variant actually reaches, `CLAUDE.md` subsystem row, CHANGELOG.

Total: ~8-10 days, P1 + P2 (≈3 d) delivering what Extasie, Arlequin and
Purplesoft need.

## 6. Open questions, and what closes each

| Question | Closes with |
|---|---|
| COL280A/B bit order (main vs aux per dot) | Purplesoft `&PLOT` disassembly, or the PLA once pins are assigned |
| PLA pin assignment | consistency search (P3); a board photo of the Eve would settle it in an hour |
| Eve rev B palette (rev A is rev B rotated, see § 1) | a capture from an owner (system-cfg, WDA forums) |
| RVB Graph HGR colour registers | its manual — not found online |
| //c adapter internals | a schematic exists in a clone thread on forum.silicium.org (behind a bot wall; readable with a browser) |
| Mixed-mode boundary: "cut / repeat" vs "next quadruplet" | both are reproduced by the mux+latch model; Extasie's own images decide (P1) |

## 7. Sources

- Le Chat Mauve, *Eve — Manuel de référence* (chapters III-2, IV, V, VI,
  IX; table IX-1) — archive.org `Le_Chat_Mauve_Manuel_de_référence` (OCR
  text) and apple2.org.za *Apple II Documentation Project / Interface
  Cards / Apple IIe / Le chat mauve Eve* (PDF + erratum, PLA `.jed`, demo
  disks, photos).
- Le Chat Mauve, *Féline* manual — apple-iigs.info (chapter 5, p. 37-41).
- *Manuel Arlequin*, mixed-mode statement, and the `$F2` image format and
  switch sequences — "Extasie Chat Mauve Reloaded" (boutillon / fenarinarsa
  mirror), annexes.
- fenarinarsa, *Le Chat Mauve sur Apple II* and *Modes vidéo chelous*;
  AppleWin issues #764, #850, #631, #1152, PR #837; `RGBMonitor.cpp`
  (`RGB_SetVideoMode`, `UpdateHiResRGBCell`, `UpdateDHiResCellRGB`,
  `PaletteRGB_Feline`).
- Video-7, US patent 4,631,692 *RGB interface* (FIG. 1 shift register,
  the four modes, the MIX mux per seven 14 MHz periods).
- forum.system-cfg.com t=9395 (RVB Graph / Sonotec `$C0F0-$C0F3`).
- MAME `apple2video.cpp` `dhgr_update` (the byte-level Video-7 rules POM2
  ports today).
