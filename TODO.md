# POM2 — TODO

État au 2026-05-13. Pour ce qui est déjà en place voir `README.md`.

Légende sévérité : 🔴 Critical · 🟠 High · 🟡 Medium · 🟢 Low.

---

## 1. Disques

- [ ] 🟠 **WOZ write-back** — Applesauce WRIT chunk + flux re-encoding.
      Aujourd'hui les WOZ sont strictement read-only ; seuls
      `.dsk/.do/.po/.nib` persistent leurs écritures.
- [ ] 🟠 **`$C0EB` (drive 2 select) ne cycle pas `mon_w`**
      (`DiskIICard.cpp:594-597`). MAME `wozfdc.cpp:229-241` :
      `mon_w(true)` sur ancien drive, `mon_w(false)` sur nouveau.
      Important pour les protections multi-drive.
- [ ] 🟠 **`last_6502_write` legacy-gate seulement sur `$C0ED`**
      (`DiskIICard.cpp:822-826`). MAME latche sur **toute** écriture
      `$C0Ex`. Le chemin LSS est déjà OK.
- [ ] 🟡 **WOZ2 `optimal_bit_timing` (INFO+39) ignoré**. Hard-codé à
      32 ticks = 4 µs/cell. MAME aussi ne le lit pas au load mais
      utilise `track_size*2` ; vrai gap = placement `cellIdx*8+4` fixe.
- [ ] 🟡 **WOZ1 splice point (TRK +6650)** ignoré
      (`DiskImage.cpp:381-398`). MAME passe via `set_write_splice`.
      Important quand on activera le WOZ write-back.
- [ ] 🟡 **`writeBackEnabled` gate asymétrique** — bloque le LSS
      splice mais pas le legacy 32-cycle write
      (`DiskIICard.cpp:339, 466, 618`).
- [ ] 🟢 **`INFO.write_protected` lu mais jamais imposé**
      (`DiskImage.cpp:194`). Footgun pour write-back futur — utiliser
      ce flag plutôt que `wozFormat`.
- [ ] 🟢 **Inflation sub-instruction RAII** (`DiskIICard.cpp:711-744`)
      vs dispatch per-bus-cycle MAME m6502. Documenté tradeoff ; rare
      impact sur protections cycle-sensibles.

## 2. CPU

- [ ] 🟠 **Décimal 65C02 : N/V/Z sur intermédiaire binaire au lieu de A
      ajusté** (`M6502.cpp:325-345, 379-408`). MAME `ow65c02.lst:11-14`
      recompute `set_nz(m_A)` après ajustement BCD. Le +1 cycle décimal
      est OK ; reste les flags.
- [ ] 🟡 **STP réveillable par NMI** (`M6502.cpp:1033-1039`). MAME
      `ow65c02.lst:715-718` : seul RESET sort de STP. Rare.
- [ ] 🟡 **`softReset` n'efface pas D** (`M6502.cpp:1480-1485`). MAME
      `ow65c02.lst:814` clear D au reset CMOS (NMOS leave D undefined,
      OK).

## 3. IIe paging

- [ ] 🟠 **80COL `$C00C/$C00D` ne broadcast pas `broadcastVideoSwitch`
      en mode IIe** — Le Chat Mauve FIFO ne reçoit donc pas le toggle
      en IIe (fonctionne en II+ via le bloc standalone). Fix :
      ajouter le broadcast dans `iieHandleSoftSwitch` ou gate le
      standalone sur `!iieMode`.
- [ ] 🟡 **`$CFFF` ne désactive pas le slot actif quand INTCXROM=on**
      (`Memory.cpp:854-862`). MAME `c800_r:2634-2653` clear cnxx_slot
      toujours, indépendamment d'INTCXROM. Ajouter
      `if (addr == 0xCFFF) slots.deactivateExpansion();` avant le
      short-circuit internal-ROM.
- [ ] 🟡 **`$C019` non gated sur `iieMode`** (`Memory.cpp:452-463`).
      En II+ devrait retourner floating bus ; aujourd'hui POM2 retourne
      0x80/0x00 basé sur scanline pour les deux.
- [ ] 🟢 **Race condition** : `iieMemRead`/`Write` lisent
      `display.page2`/`hiRes` sans `stateMutex` (`Memory.cpp:666, 672,
      691, 700`). TSAN-flagged, en pratique bool-aligned.
- [ ] 🟢 **`$C040` utility strobe pulse** non modélisé. MAME
      `apple2e.cpp:1711-1716`. Pas de consumer POM2.
- [ ] 🟢 **Annunciators AN0-AN2 `$C058-$C05D`** non drivés (MAME
      `:1750-1773`).

## 4. Display

- [ ] 🟠 **DHIRES annunciator non honoré en HGR standard** (rev-0
      emulation, sans orange/bleu quand DHIRES=on + 80COL=off). MAME
      `apple2video.cpp:747` : `bit7_mask = m_dhires ? 0 : 0x80`. POM2
      honore toujours bit 7.
- [ ] 🟠 **Palette Chat Mauve = POM2-original** (lo-res
      `Apple2Display.cpp:474-491` + HGR `:691-702`). Pas dans MAME ;
      à documenter comme stylistique ou aligner sur `apple2_palette[]`.
- [ ] 🟡 **Path HGR Chat Mauve écrit dans buffer 280-wide au lieu de
      560** (`Apple2Display.cpp:778-787`). Perd la netteté qui est la
      raison d'être de la carte.
- [ ] 🟡 **Persistence buffer afterglow mono dimensionné 280×192** →
      DHGR mono 560 sans afterglow (`Apple2Display.cpp:107, 1087-1097`).
- [ ] 🟢 **`monochrome_dhr_shift()` 1-px alignment manquant** en DHGR
      mono (MAME `apple2video.cpp:460-471`). Cosmétique.
- [ ] 🟢 **Floating-TTL** `empty_words[40]={0x3fff,…}` pour rows
      hors-bounds non modélisé (MAME `:751-758`). Jamais atteint en 48K+.
- [ ] 🟢 **`cursorOverlay` dead code** (`Apple2Display.h:79-80`).
      Supprimer ou implémenter.
- [ ] 🟢 **DHGR per-scanline mode switching** — bottom-of-mixed utilise
      une région 4-line statique.

## 5. Audio

- [ ] 🟡 **AY float tone counter aliase à périodes très courtes (1-3)**
      (`Mockingboard.cpp:449-456`). MAME utilise compteur entier.
      Inaudible à period normal, manifeste sur tricks PWM.
- [ ] 🟢 **AY amplitude table** dévie ~15-22% de la courbe canonique
      MAME `ay8910_param` Westcott 2001 + `build_single_table(normalize=1)`.
      Valeurs à swapper :
      `{0.0000, 0.0105, 0.0154, 0.0223, 0.0321, 0.0468, 0.0635, 0.1061,
       0.1319, 0.2164, 0.2974, 0.3909, 0.5128, 0.6371, 0.8186, 1.0000}`
- [ ] 🟢 **Port A read mask par DDR** (`Mockingboard.cpp:126-130`).
      Devrait retourner pin state. Sans impact concret sur Mockingboard.
- [ ] 🟡 **Cassette seuil `±0.02` pré-extrait au load** vs MAME
      `> 0.0` à runtime. Rejette les rips faible volume.
- [ ] 🟡 **Cassette auto-rewind 500 ms** si pas de poll $C060
      (`CassetteDevice.cpp:461-465`). MAME ne rewind jamais. Casse les
      loaders custom polling sporadique.

## 6. Super Serial Card

- [ ] 🟠 **Echo mode (REM, command bit 4) non appliqué**. MAME
      `mos6551.cpp:584-594`. Casse le diagnostic Apple "SSC TEST".
- [ ] 🟠 **Control reg jamais consulté** : baud rate, word length,
      stop bits stockés mais ignorés. Sans rate-limiting les listings
      `PR#2` arrivent à vitesse host.
- [ ] 🟠 **`read_rdr` ne clear pas PARITY_ERROR / FRAMING_ERROR /
      OVERRUN / RDRF** (MAME `mos6551.cpp:231-236`).
- [ ] 🟠 **DTR (command bit 0) side-effects manquants** (MAME
      `mos6551.cpp:290-292`). Toggle DTR doit reset RX state + forcer
      txd MARK + disable RX/TX IRQ. Casse les drivers modem-style.
- [ ] 🟠 **DCD/DSR transitions ne raise pas IRQ** (MAME
      `mos6551.cpp:443-461`). Casse ProTERM "carrier detect".
- [ ] 🟠 **Overrun tracking** : SR_OVERRUN jamais set
      (`SuperSerialCard.cpp:163-179`). Casse retransmit Kermit /
      XMODEM-CRC.
- [ ] 🟡 **Signature slot ROM incomplète pour GS/OS / Pascal 1.1** :
      manque Pascal 1.1 ID block `$CnFB-$CnFF`.
- [ ] 🟡 **Telnet IAC strip mange les `$FF` valides en RX 8-bit
      binaire**. Proposer toggle "raw mode" dans le panneau.
- [ ] 🟡 **LF→CR mapping** appliqué uniquement sur keyboard sink, pas
      sur `rxBuf` (`SuperSerialCard.cpp:187-198`).
- [ ] 🟢 **IRQ gate sur DIP** : MAME `a2ssc.cpp:373` n'arme l'IRQ que
      si SW2:6 (Interrupts) est on. POM2 expose toujours.

## 7. ClockCard / ThunderClock+

- [ ] 🟠 **Seul `MODE_TIME_READ` implémenté** (`ClockCard.cpp:13-16`).
      Manque TIME_SET (impossible de régler), TP=64/256/2048 Hz
      (utilitaires interval-timing comme *Clockworks* ne tickent
      jamais).
- [ ] 🟠 **`MODE_SHIFT` non gaté** — POM2 shift sur n'importe quel CLK
      rise. MAME `upd1990a.cpp:312-327` shift only when `m_c ==
      MODE_SHIFT`.
- [ ] 🟡 **DATA_OUT live** (`ClockCard.cpp:88-94`) vs MAME latch sur
      CLK edge en MODE_SHIFT. Differs hors ProDOS.
- [ ] 🟡 **`read_c0nx` ignore offset** — POM2 retourne `0xFF` pour
      `$C0n1-$C0nF` ; MAME mirror DATA_OUT sur tous les 16 offsets.
- [ ] 🟢 **Pas de compteur interne 1 Hz** — `std::time()` à chaque
      STB. Casse déterminisme snapshot.
- [ ] 🟢 **Slot ROM essentiellement vide** (256 B signature + NOPs).
      Vrai ThunderClock+ ROM = 2 KB avec driver RTS-able pour DOS 3.3
      / Applesoft.

## 8. Soft switches misc

- [ ] 🟡 **`$C000` non mirroré sur `$C001-$C00F`** comme keyboard latch
      en mode II+ (`Memory.cpp:425`). MAME `apple2.cpp:548`
      `.mirror(0xf)`.
- [ ] 🟡 **`$C010` bit 7 devrait refléter any-key-down en IIe**
      (`Memory.cpp:429`). MAME `apple2e.cpp:1833`.
- [ ] 🟡 **`$C068-$C06F` mirror `$C060-$C067` manquant** sur II+ et
      IIe (STATEREG / band-select sont IIgs-only, pas IIe).
- [ ] 🟡 **`$C070` PTRIG ne retourne pas floating bus**
      (`Memory.cpp:582-585`) et n'alias pas `$C070-$C07F`. MAME
      `apple2.cpp:555` `.mirror(0xf)` ; MAME modèle aussi le one-shot
      558 (strobe ignored if timer still running).
- [ ] 🟢 **Floating-bus low 7 bits sur `$C061-$C067`** (paddles +
      buttons). POM2 retourne `0x80`/`0x00` purs, MAME OR avec video
      MUX byte.

## 9. Features missing

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
- [ ] **Annunciators `$C058-$C05F`** — voir §3.
- [ ] **Color killer** (logique Rev 1) — cosmétique.
- [ ] **Strapping RAM 4 K → 48 K** — modéliser la machine de 1977
      d'origine (POM2 colle 48 K en dur aujourd'hui).
- [ ] **Le Chat Mauve EVE variante** (64 KB d'extension RAM intégrée
      + modes SPEC1/SPEC2/DASH/COL280).

## Hors scope

- 80-column / IIe-only sur machine sans IIe ROM
- IIgs / ProDOS 16
- Disk II 13-sector (Apple II 1977-1979, pré-DOS 3.3)
- Z-80 SoftCard CP/M
- CFFA CompactFlash (POM2 a déjà ProDOS HDV slot 5 + host folder)
