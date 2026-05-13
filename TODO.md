# POM2 — TODO

État au 2026-05-13. Pour ce qui est déjà en place voir `README.md`.

Légende sévérité : 🔴 Critical · 🟠 High · 🟡 Medium · 🟢 Low.

Sections triées par poids des items 🟠 (le plus urgent d'abord). Items
au sein de chaque section ordonnés 🟠 → 🟡 → 🟢. Sections terminées
regroupées en fin de fichier.

---

## 1. Super Serial Card

DONE 2026-05-13 (port MAME `mos6551.cpp`, pinned by
`tests/ssc_acia_smoke_test.cpp`):

- **Echo mode (REM, cmd bit 4)** — `applyCommandReg` décode bit 4,
  `deliverRxBytes` réinjecte chaque byte RX dans txBuf si echoMode_ on,
  suppression en cas d'OVERRUN comme MAME `mos6551.cpp:584-594`.
- **Control reg baud-rate decode** — table `kInternalDivider` portée de
  MAME, conversion en bytes/sec (1.8432 MHz xtal ÷ divider ÷ 16 ÷ 10),
  rate-limit appliqué au drain TX worker via accumulateur `sendBudget_`.
  Index 0 (16x ext clk) traité comme unconstrained.
- **`read_rdr` clears errors + RDRF** — clear `SR_PARITY_ERROR |
  SR_FRAMING_ERROR | SR_OVERRUN` sur lecture $C0n8, plus drop
  `IRQ_RDRF` (MAME `mos6551.cpp:231-236`). RDRF dérive de
  `rxBuf.empty()` donc clear naturel.
- **DTR side-effects** — cmd bit 0 = 0 désasserte DTR : RX/TX IRQ
  désactivés via `!dtrAsserted_` gate, pending IRQ_RDRF/IRQ_TDRE
  drop, force MARK en flushant txBuf, futurs writes TDR rejetés
  (MAME `mos6551.cpp:290-292, 317-321`).
- **DCD/DSR transitions raise IRQ** — `onConnectionEdge(bool)` appelé
  par le worker sur connect/disconnect, raise `IRQ_DCD | IRQ_DSR`
  seulement si DTR asserted (MAME `mos6551.cpp:443-461`).
- **Overrun tracking** — `deliverRxBytes` set `SR_OVERRUN` quand le
  ring TCP overflow (eviction du plus ancien), clear sur read_rdr
  (MAME `mos6551.cpp:542-543`).
- Bonus : programmed reset ($C0n9 write) préserve les bits 5-7 (parity)
  via `applyCommandReg(cmd & ~0x1F)` au lieu d'écraser cmdReg.

Reste :

- [ ] 🟡 **Signature slot ROM incomplète pour GS/OS / Pascal 1.1** :
      manque Pascal 1.1 ID block `$CnFB-$CnFF`.
- [ ] 🟡 **Telnet IAC strip mange les `$FF` valides en RX 8-bit
      binaire**. Proposer toggle "raw mode" dans le panneau.
- [ ] 🟡 **LF→CR mapping** appliqué uniquement sur keyboard sink, pas
      sur `rxBuf` (`SuperSerialCard.cpp:187-198`).
- [ ] 🟢 **IRQ gate sur DIP** : MAME `a2ssc.cpp:373` n'arme l'IRQ que
      si SW2:6 (Interrupts) est on. POM2 expose toujours.

## 2. Disques

DONE 2026-05-13 :

- **WOZ write-back** (pinned by `tests/woz_writeback_smoke_test.cpp`).
  `loadWoz()` capture les bytes bruts du fichier + per-qt
  `(byteOff, byteLen, bitCount)`. `writeFlux()` splice cell-par-cell
  dans `bitStream[qt]` au lieu du early-out précédent. `saveDirty()`
  repacke chaque qt dirty dans `wozRaw` (MSB-first) et zeroes le CRC32
  header (sentinel Applesauce "not computed"). `isWriteProtected()`
  n'override plus inconditionnellement les WOZ — laisse passer la
  bascule user via `setWriteBackEnabled` tout en honorant
  `INFO.write_protected` (bit physique du disque source). WOZ1 et WOZ2
  testés.
- **`$C0EB` (drive 2 select) cycle `mon_w`** via le nouveau
  `selectDrive(int newDrive)`. MAME `wozfdc.cpp:219-241` : flush du
  write buffer sur l'ancien drive (= `mon_w(true)` → commit_image),
  reset de `lssCycle` à `cpuCycleTotal*2` pour le nouveau (= MAME
  `mon_w(false)` → revolution_start_time = now). Appelé depuis les
  deux paths (LSS `control()` et legacy `handleSwitchAccess`).

Reste :

- [ ] 🟡 **WOZ2 `optimal_bit_timing` (INFO+39) ignoré**. Hard-codé à
      32 ticks = 4 µs/cell. MAME aussi ne le lit pas au load mais
      utilise `track_size*2` ; vrai gap = placement `cellIdx*8+4` fixe.
- [ ] 🟡 **WOZ1 splice point (TRK +6650)** ignoré
      (`DiskImage.cpp:381-398`). MAME passe via `set_write_splice`.
      Maintenant que WOZ write-back est en place ce hint compte
      vraiment (re-master parité avec l'imager Applesauce).
- [ ] 🟢 **Inflation sub-instruction RAII** (`DiskIICard.cpp:711-744`)
      vs dispatch per-bus-cycle MAME m6502. Documenté tradeoff ; rare
      impact sur protections cycle-sensibles.

## 3. ClockCard / ThunderClock+

DONE 2026-05-13 :

- **MODE_TIME_SET** — `commitTimeSetFromShiftReg()` décode les 6 bytes
  BCD chargés via DATA_IN+CLK (en MODE_SHIFT), reconvertit en `time_t`
  via `std::mktime`, et stocke le delta vs `timeFn()` comme
  `userOffsetSeconds`. Les MODE_TIME_READ suivants composent
  `timeFn() + offset` via `effectiveTime()` — équivalent fonctionnel
  du compteur interne 1 Hz de MAME `upd1990a.cpp:194-225` mais sans
  thread de tick (l'avancement est dérivé de l'horloge host à la
  demande). Pinned by `tests/clock_card_smoke_test.cpp::testTimeSetRoundTrip`.
- **DATA_IN injection au MSB** sur CLK rising — nécessaire pour
  TIME_SET (sans ça le shift se remplit de zéros au lieu de prendre
  les bits du host).
- **MODE_SHIFT lax** documenté comme divergence délibérée. POM2 shift
  sur n'importe quel CLK rise (pas gaté sur `m_c == MODE_SHIFT`).
  Raison : la driver ProDOS shifte en MODE_TIME_READ directement, sans
  re-switch vers MODE_SHIFT entre STB et les CLK pulses — gater
  strict comme MAME `upd1990a.cpp:312-327` casserait la lecture stock
  de ProDOS. Pinned by `testShiftLaxAcrossModes`.

Reste :

- [ ] 🟠 **TP tick rates (64/256/2048/4096 Hz + interval timers)** non
      implémentés. MAME `upd1990a.cpp:248-267`. Nécessitent une ligne
      IRQ slot-bus (pas encore plumbed dans `SlotPeripheral`).
      Utilitaires interval-timing (Clockworks) ne tickent jamais.
- [ ] 🟡 **DATA_OUT live** (`ClockCard.cpp:88-94`) vs MAME latch sur
      CLK edge en MODE_SHIFT. Differs hors ProDOS.
- [ ] 🟢 **Slot ROM essentiellement vide** (256 B signature + NOPs).
      Vrai ThunderClock+ ROM = 2 KB avec driver RTS-able pour DOS 3.3
      / Applesoft.

## 4. Display

DONE 2026-05-13 :

- **Palette Chat Mauve alignée sur AppleWin** (lo-res + HGR). Valeurs
  verbatim de `RGBMonitor.cpp::PaletteRGB_Feline` — capture RGB
  white-balanced d'une vraie carte Le Chat Mauve "Feline". Les deux
  gris distincts (trademark) sont préservés : idx 5 = olive-gris
  `rgb(0x9f,0x97,0x7e)`, idx 10 = mauve-gris `rgb(0x78,0x68,0x7f)`.
  MAME `apple2_palette[]` collapse 5 ≡ 10 (NTSC composite), donc on
  suit AppleWin qui modélise spécifiquement le décodeur RGB Péritel.
  Pinned by `tests/le_chat_mauve_smoke_test.cpp` (HGR magenta/green/
  blue + lo-res tinted grays distinguishable) et
  `tests/dhgr_render_smoke_test.cpp` (DHGR cell decode → palette).

Reste :

- [ ] 🟡 **Path HGR Chat Mauve écrit dans buffer 280-wide au lieu de
      560** (`Apple2Display.cpp:778-787`). Perd la netteté qui est la
      raison d'être de la carte.
- [ ] 🟡 **Persistence buffer afterglow mono dimensionné 280×192** →
      DHGR mono 560 sans afterglow (`Apple2Display.cpp:107, 1087-1097`).
- [ ] 🟢 **`monochrome_dhr_shift()` 1-px alignment manquant** en DHGR
      mono (MAME `apple2video.cpp:460-471`). Cosmétique.
- [ ] 🟢 **Floating-TTL** `empty_words[40]={0x3fff,…}` pour rows
      hors-bounds non modélisé (MAME `:751-758`). Jamais atteint en 48K+.
- [ ] 🟢 **DHGR per-scanline mode switching** — bottom-of-mixed utilise
      une région 4-line statique.

## 5. Audio

- [ ] 🟡 **AY float tone counter aliase à périodes très courtes (1-3)**
      (`Mockingboard.cpp:449-456`). MAME utilise compteur entier.
      Inaudible à period normal, manifeste sur tricks PWM.
- [ ] 🟡 **Cassette auto-rewind 500 ms** si pas de poll $C060
      (`CassetteDevice.cpp:461-465`). MAME ne rewind jamais. Casse les
      loaders custom polling sporadique.
- [ ] 🟢 **Port A read mask par DDR** (`Mockingboard.cpp:126-130`).
      Devrait retourner pin state. Sans impact concret sur Mockingboard.

## 6. Features missing

- [ ] **Integer BASIC preset** — sélecteur runtime II (Integer BASIC)
      vs II+ (Applesoft + Autostart).
- [ ] **Mouse Card** (slot 4 conventional) — 68705 µC + 6821 PIA.
      Pour AppleWorks et les apps GUI.
- [ ] **Mapping souris** alternative aux pads : paddle 0/1 sur axe
      X/Y de la souris host.
- [ ] **PADL(2)/PADL(3)** binding host (second stick, actuellement
      centrés à 127).
- [ ] **Video-7 AppleColor RGB** — équivalent américain de Le Chat
      Mauve avec même AN3+80COL mais encodage FIFO différent.
- [ ] **Eve Color text mode (`$C0B9`)** — attributs FG/BG par
      caractère via banque aux dédiée (variante Chat Mauve / Eve).
- [ ] **RAMWorks 1 MB** (IIe-only, ProDOS 3+).
- [ ] **Annunciators `$C058-$C05F`** — voir §4.
- [ ] **Color killer** (logique Rev 1) — cosmétique.
- [ ] **Strapping RAM 4 K → 48 K** — modéliser la machine de 1977
      d'origine (POM2 colle 48 K en dur aujourd'hui).
- [ ] **Le Chat Mauve EVE variante** (64 KB d'extension RAM intégrée
      + modes SPEC1/SPEC2/DASH/COL280).

---

## Done 2026-05-13

### CPU

- Décimal 65C02 N/V/Z recomputé sur A ajusté (+1 cycle) — MAME
  `ow65c02.lst:11-14`
- STP : flag `halted` qui ignore IRQ/NMI ; seul RESET wake — MAME
  `ow65c02.lst:715-718`
- `softReset` clear D en mode CMOS — MAME `ow65c02.lst:814`

### IIe paging

- 80COL broadcast déplacé dans `iieHandleSoftSwitch`, standalone
  block gated `!iieMode`
- `$CFFF` deactivate explicit avant le short-circuit INTCXROM (read +
  write paths) — MAME `apple2e.cpp:2636-2645`
- `$C019` gated sur `iieMode` (II+ tombe sur floating bus)
- Race annotée (writeur + lecteur tous deux sur CPU thread — pas de
  vraie race ; UI thread passe par `getDisplayState()` sous lock)
- `$C040` strobe swallowed (no-op return)
- AN0/AN1/AN2 state tracking ajouté ($C058-$C05D)

### Soft switches misc

- `$C000-$C00F` keyboard latch mirror (read returns latch, write
  dispatches paging) — MAME `apple2.cpp:548` + `apple2e.cpp:1825-1828`
- `$C010` IIe any-key-down bit 7 (approximé via `keyReady` strobe
  state pre-clear)
- `$C068-$C06F` mirror $C060-$C067 (low 3 bits déterminent le
  registre)
- `$C070-$C07F` PTRIG alias + retour floating-bus (one-shot 558
  monostable laissé comme simplification)
- Floating-bus dans low 7 bits sur `$C061-$C067` (OR avec
  `floatingBus() & 0x7F`)

### Disques

- **`last_6502_write` legacy-gate sur tout `$C0Ex`** — aligned with
  MAME `wozfdc.cpp`. Le chemin LSS l'était déjà ; le legacy le fait
  maintenant aussi.
- **`writeBackEnabled` gate symétrique** — legacy `writeNibbleAt`
  désormais gaté comme le LSS splice.
- **`INFO.write_protected` intégré à `isWriteProtected()`**. No-op
  aujourd'hui (WOZ déjà bloqué par `wozFormat`) mais évite le footgun
  quand WOZ write-back sera implémenté.

### Display

- **DHIRES annunciator HGR rev-0** — `buildHgrWordRow` /
  `buildBitStream` acceptent un `bit7Mask` ; passé à `0x7F` quand
  `state.dhgr` est on en `renderHiRes`.
- `cursorOverlay` dead code removed.

### Audio

- **AY amplitude table** aligned with MAME `build_single_table
  (normalize=1)` + `ay8910_param` Westcott 2001.
- Cassette seuil sign-based (MAME `apple2.cpp:362` parity).

### ClockCard

- `read_c0nx` mirror DATA_OUT sur tous les offsets — MAME
  `a2thunderclock.cpp:112-115`

---

## Hors scope

- 80-column / IIe-only sur machine sans IIe ROM
- IIgs / ProDOS 16
- Disk II 13-sector (Apple II 1977-1979, pré-DOS 3.3)
- Z-80 SoftCard CP/M
- CFFA CompactFlash (POM2 a déjà ProDOS HDV slot 5 + host folder)
