# POM2 — TODO

## Infrastructure

- [x] **M6502** — porté + Klaus Dormann functional test passe en 0.46 s
      (`tests/klaus_6502_functional`, SHA-256 pinned, gate de non-régression)
- [x] **AudioDevice** + miniaudio + stb_vorbis vendoring
- [x] **CassetteDevice** (cassette Apple II native)
- [x] **CassetteDeck_ImGui** (deck procédural 378×404 + Font Awesome)
- [x] **RomLoader** — `loadBinary/loadBytes/probeStandardRomPath` + bypass
      write-protect (`Memory::loadRomBytes`). Prêt pour Disk II / Language
      Card / character ROM.
- [x] **SnapshotIO** — magic `POM2SNAP`, sections nommées 8 octets, format
      identique POM1 — round-trip test `tests/snapshot_io_smoke`
- [x] **CliDispatcher** — flags Apple II (`--preset ii|ii+`, `--speed`,
      `--cpu-max`, `--tape`, `--load addr:file`, `--run`, `--paste`,
      `--step`, `--play/--rec/--rewind`, `--snapshot-save/load`),
      3 phases A/B/C
- [x] **SlotBus** + `SlotPeripheral` — dispatcher 8 slots, latch
      `activeExpansionSlot` automatique, `$CFFF` disable. Test
      `tests/slot_bus_smoke` couvre device-select, slot ROM, expansion
      ROM, unplug.

---

## 1. Page d'I/O `$C000-$C07F` (soft switches)

- [x] Latch clavier `$C000` + strobe `$C010`
- [x] Speaker `$C030` (toggle on access)
- [x] Modes vidéo `$C050-$C057`
- [x] Cassette **Apple II** : `$C020` (toggle sortie) / `$C060` (entrée
      comparateur, bit 7) — câblé dans `Memory::softSwitchAccess()`,
      forwarde vers `CassetteDevice::toggleOutput()` / `readTapeInput()`
- [x] Game I/O `$C061-$C067` + latch `$C070` : décharge RC modélisée
      (`$C064-$C067` reste à `0x80` tant que `cycleCounter -
      paddleLatchCycle < paddleValue × 11`), boutons PB0/PB1/PB2 sur
      `$C061-$C063`, `$C070` arme la latch.
- [ ] VBL/utility strobes `$C040` (annunciator)
- [ ] Annunciators `$C058-$C05F`

## 2. Modes vidéo Apple II

- [x] Texte 40×24 entrelacé Woz `addr = base + 0x80*(y%8) + 0x28*(y/8)`
- [x] **Attributs caractère** Apple II : bits 7-6 = normal/inverse, low
      6 bits = ASCII index (cf. `resolveGlyph` dans `Apple2Display.cpp`).
      Le clignotement 2 Hz reste à animer (rendu actuel = inverse statique).
- [x] **Lo-res 40×48 / 16 couleurs** : implémenté, palette //gs-corrected,
      partage entrelacé avec le texte (`renderLoRes`)
- [x] **Hi-res 280×192 + couleur NTSC** : pipeline 3 passes 
  - [x] LUT 14 KB indexée par `(parity << 8) | byte` — 6 couleurs
        violet/vert/bleu/orange + noir/blanc, palette flag bit 7 lu par
        octet
  - [x] Fix-up des 39 seams inter-octets (bit 6 + bit 0 lits → 2 pixels
        blancs)
  - [x] Glow horizontal additif optionnel (toggle `Display → Hi-res glow`)
  - [x] **HGR page 2** `$4000-$5FFF` via `hgrRowAddress(y, page2)`
  - [x] Scanlines non-linéaires (formule Woz `addr = base + 0x400*(y%8)
        + 0x80*((y/8)%8) + 0x28*(y/64)`)
  - [x] Test `tests/hgr_render_smoke` pin la LUT (4 corners palette ×
        parité), les seams, le pass-through glow
- [x] **Mixed mode** : 4 dernières lignes texte sur graphique
      (`render()` appelle `renderText(20, 24)` au-dessus du graphique)
- [x] **Page 1 / Page 2** vidéo : `state.page2` switch déjà câblé partout

## 3. Architecture des 8 slots d'extension

POM1 a un `PeripheralBus` à priorité — pattern réutilisable mais le
décodage Apple II est spécifique. Implémenté via `SlotBus` + interface
`SlotPeripheral` :

- [x] `$C080-$C0FF` : device-select 16 octets par slot N (slot N à
      `$C080+N*16` ; slot 0 = language card, slots 1-7 = expansion)
- [x] `$C100-$C7FF` : ROM de slot 256 octets pour slots 1-7
- [x] `$C800-$CFFF` : ROM d'extension partagée — un seul slot actif à la
      fois, marqué automatiquement par tout accès `$CnXX`
- [x] **Switch `$CFFF`** : read/write désactive l'expansion ROM
- [x] `Memory::memRead/memWrite` route les 4 fenêtres via `SlotBus`
- [x] Reset Apple II (Ctrl-Reset) propage `onReset()` à toutes les cartes
- [x] `advanceCycles()` forwardé aux cartes (pour Disk II stepper, UART…)
- [x] **Disk II en slot 6 branché** (auto-plug si `roms/disk2.rom` présent
      au démarrage). Reste à brancher d'autres cartes (Mockingboard,
      printer, …)

## 4. Disk II

Écrit intégralement dans POM2 (`DiskImage` + `DiskIICard` +
`DiskController_ImGui`) :

- [x] ROM de boot Disk II (256 B P5A AppleWin chargée à `roms/disk2.rom`,
      autodétection du slot via `JSR $FF58 / TSX / LDA $0100,X`)
- [x] Encodage 6-and-2 nibble (gap sync 14 octets, address field
      `D5 AA 96 [vol/trk/sec/chk 4-and-4] DE AA EB`, data field
      `D5 AA AD [86 lo + 256 hi + 1 XOR] DE AA EB`) + DOS 3.3 sector
      skewing
- [x] Phases moteur pas-à-pas `$C0n0-$C0n7` en demi-pistes (les
      quart-pistes ne sont pas modélisées)
- [x] Sélection drive 1/2 (seul drive 1 modélisé), motor on/off,
      Q6L/Q6H + Q7L/Q7H — write acquitté mais ignoré
- [x] Loaders `.dsk` / `.do` (143 360 octets, ordre logique DOS 3.3) avec
      pré-nibblisation en 35 × 6656 octets
- [ ] Loaders `.po` (ProDOS) / `.nib` / `.woz`
- [ ] Persistance des écritures (aujourd'hui le buffer nibble est jeté
      à l'éjection, le fichier n'est jamais touché)
- [ ] Drive 2, quart-pistes (jeux à protection lourde)

## 5. Audio speaker 1-bit

POM1 a **miniaudio + `AudioDevice`** réutilisable comme socle, mais aucun
générateur 1-bit (POM1 fait du SID 6581/8580 — modèle différent).

- [x] Compteur `getSpeakerToggleCount()` exposé
- [x] `AudioDevice` (miniaudio) porté depuis POM1 — mixer mono float32,
      taux échantillonnage négocié, sources enregistrables
- [x] Synthèse `SpeakerDevice` (`AudioSource`) — événements timestampés
      sub-instruction via `cycleCounter + cpu->getCurrentInstructionCycles()`,
      drainés à la fréquence d'échantillonnage négociée par
      `AudioDevice`. Cap 16 K events, catch-up auto si lag > 100 ms.
- [x] Filtre passe-bas 1-pôle ~5 kHz (cône speaker) + DC blocker pour
      éviter la dérive lors d'une absence de toggle prolongée
- [x] Volume + mute UI (slider dans la fenêtre Emulation, contrôle
      atomique audio-thread-safe)
- [x] Mix avec cassette (`AudioDevice::addSource(spk)` câblé dans
      `EmulationController`)
- [x] Test `tests/speaker_smoke` : silence, 1 kHz reconstruit,
      reset, mute

## 6. ROMs Apple II

- [x] Slot `roms/apple2.rom` (12 KB ou 16 KB) accepté par `loadAppleIIRom()`
- [x] Slot `roms/apple2_char.rom` accepté
- [ ] Sélecteur runtime II (Integer BASIC) vs II+ (Applesoft + Autostart)
- [ ] Character ROM Apple II : layout normal/inverse/flashing 2 Kb

## 7. Game I/O réel (paddles, joystick, boutons)

Implémenté dans POM2
(`JoystickInput` + `JoystickPanel_ImGui` + RC dans `Memory.cpp`).

- [x] Modèle de décharge RC : `$C064-$C067` reste à `0x80` tant que
      `(cycleCounter - paddleLatchCycle) < paddleValue × 11`, puis
      tombe à 0. `$C070` arme la latch. Constante 11 cycles ≈ pas RC
      Apple II — suffisant pour les jeux à paddle, pas une réplique
      PASCAL.
- [x] Mapping joystick host (GLFW, 16 slots polled, hot-plug) vers axes
      paddle 0/1, autobinding au premier pad présent, deadzone +
      invert configurables
- [x] Boutons `$C061-$C063` (PB0/PB1/PB2, "open-apple"/"closed-apple")
- [ ] Mapping souris (alternative aux pads — paddle 0/1 sur X/Y)
- [ ] PADL(2)/PADL(3) (second-stick rare, lus centrés à 127 actuellement)

## 8. Carte Language (Apple II+ → 64 K)

- [x] 16 KB RAM bank-switched derrière la ROM `$D000-$FFFF`
- [x] Soft switches `$C080-$C08F` (read/write enable, BSR, prewrite latch)
- [x] Banque 1 vs banque 2 sur `$D000-$DFFF` + 8 KB haut partagé
- [x] Indispensable pour ProDOS, Pascal, beaucoup de logiciels post-1980
- [x] Test `tests/language_card_smoke`

## 9. Reset / RDY / IRQ / VBL

- [x] Reset Apple II via vecteur `$FFFC`
- [x] `resetSoftSwitches()` force text/full/page1/lores au reset
- [ ] Color killer (logique Rev 1) — cosmétique, pas critique
- [ ] VBL synchrone : `$C019` (II+ tardif) ou test paddle 3. Beaucoup de
      jeux en dépendent.

## 10. Banque OOR / présence RAM variable
 Apple II/II+ :
présentations 4K / 8K / 16K / 24K / 32K / 36K / 48K via les *strapping
blocks*.

- [ ] À modéliser si on veut représenter la machine de 1977 d'origine
      (POM2 colle 48 K en dur aujourd'hui)

## 12. Cartes vidéo RVB tierces (Le Chat Mauve, Video-7 AppleColor)

Implémenté dans POM2 (`LeChatMauveCard`
+ `LeChatMauve_ImGui`) en slot 7, avec FIFO 2-bits piloté par AN3+80COL :

- [x] Carte branchée slot 7, décode DIRECT depuis l'octet HGR brut — pas
      de bit doubler MAME, pas de half-dot delay, pas de LUT artefact 7-bits.
      MSB = flag de banque palette (bank 0 = violet/vert, bank 1 = bleu/orange).
      6 couleurs HGR canoniques propres, sans fringing inter-octet.
- [x] FIFO 2-bits : front montant `$C05F` push le bit `$C00C`/`$C00D` courant
- [x] Modes BW560 / Mixed / Chunky / COL140 — sur standard HGR seul BW560 est
      visuellement distinct (force monochrome strict) ; les trois autres
      partagent le même rendu 6-couleurs (la séparation 16-couleurs n'apparaît
      qu'en DHGR)
- [x] Panneau Hardware → Le Chat Mauve (statut FIFO, override manuel, reset)
- [x] Menu Display → Le Chat Mauve (RGB) coexiste avec NTSC + Mono White /
      Green / Amber — 5 pipelines distincts, défaut NTSC (vrai Apple II)
- [x] Soft-switches `$C00C/$C00D` (80COL) et `$C05E/$C05F` (AN3) câblés
      dans `Memory::softSwitchAccess()` et diffusés via
      `SlotBus::broadcastVideoSwitch()`
- [ ] **DHGR (Double Hi-Res 560×192 + 16 couleurs avec 2 gris distincts)** —
      pré-requis bloquant : modéliser la RAM auxiliaire `$00-$BFFF` + soft
      switches 80STORE (`$C000/$C001`), RAMRD (`$C002/$C003`), RAMWRT
      (`$C004/$C005`), ALTZP (`$C008/$C009`). C'est en DHGR seulement que
      les motifs `0101`/`1010` deviennent #555555/#AAAAAA (signature
      esthétique French Touch / *Latecomer* / Extasie). Sans cela, les
      modes Mixed et COL140 partagent l'interprétation HGR-6-couleurs.
- [ ] **Mode texte couleur EVE ($C0B9)** — attributs foreground/background
      par caractère via banque aux dédiée. Bloqué par le même pré-requis
      RAM aux. Spécifique à la carte Eve ; la Féline ne le supporte pas.
- [ ] Carte Eve (variante 64 KB d'extension RAM intégrée + modes SPEC1/SPEC2/
      DASH/COL280) — palette historiquement pertinente mais incompatible
      Féline ; à n'envisager qu'après DHGR
- [ ] Carte Video-7 AppleColor RGB (équivalent américain, mêmes soft switches
      AN3+80COL avec encodage différent du FIFO)

## 11. Hors scope

- 80 colonnes / //e auxiliary memory : hors II/II+ d'origine
- Apple IIGS, ProDOS 16 : hors scope

---

## 13. Audit MAME (mai 2026) — gaps restants vs `mame/apple/`

Audit effectué contre `apple2video.cpp`, `apple2e.cpp`, `apple2common.cpp`
et `bus/a2bus/`. Les items déjà listés dans les sections 1-12 ci-dessus
sont rappelés ici avec leur priorité ; les **nouveaux** items
(mousetext, no-slot clock, 65C02 manquants, etc.) sont marqués 🆕.

### Tier 1 — Forte valeur, effort raisonnable

- [x] **VBL `$C019` scanline-accurate + IRQ mask** (cf. §9)
      ✓ 2026-05 : modèle 262 scanlines × 65 cycles, bit 7 réfléchit
      l'état video active (HIGH 0..191, LOW 192..261). IIe IRQ mask
      via `$C05A` (off) / `$C05B` (on) ; lecture `$C019` acquitte
      l'IRQ pending. Pinned by `tests/vbl_smoke_test.cpp`.

- [x] **Disk II : write-back + loader `.nib`** (cf. §4)
      ✓ 2026-05 : decoder GCR inverse (kGcrInverse + decode4and4 +
      6-and-2 reverse) → write-back .dsk/.do/.po au moment de
      l'eject. Loader `.nib` (35 × 6656 B raw nibbles) + write-back
      verbatim. **Opt-in via UI** (`writeBackEnabled`, default off)
      pour éviter la corruption silencieuse des fichiers source.
      Pinned by `tests/disk_writeback_smoke_test.cpp`. *.woz toujours
      non implémenté — format flux-based plus complexe ; à voir.*

- [x] **Character ROM 2 KB + ALTCHAR mousetext IIe** 🆕 (étend §6)
      ✓ 2026-05 : `Apple2Display::resolveGlyphRom` charge
      `roms/apple2_char.rom` (2 KB II/II+ ou 4 KB IIe).
      Range $00-$3F = inverse, $40-$7F = flashing, $80-$FF = normal.
      Avec un ROM 4 KB en IIe + ALTCHAR=on, codes $40-$7F basculent
      sur le bank mousetext (offset $800). Flash period 2 Hz
      (15 frames @ 60 fps, MAME parity). Fallback 5×7 conservé si
      pas de ROM.

### Tier 2 — Effort modéré, gain ciblé

- [ ] **Quart-pistes Disk II** (cf. §4)
      Demi-pistes OK ; quart-pistes non modélisés (jeux à protection
      lourde). MAME : `DISKII_FDC` modèle 4-phase complet. Débloque :
      Karateka original, Dragon Wars, Bard's Tale, Ultima II.
      Effort : moyen (~80-120 L dans `DiskIICard`).

- [x] **Mockingboard slot 4 (AY-3-8910 × 2 + 6522 VIA)** 🆕
      ✓ 2026-05-10 : `Mockingboard.h/.cpp` — 6522 VIA (ports A/B, T1
      one-shot/continuous, IFR/IER) + AY-3-8910 PSG (3 tone channels +
      noise + envelope). Inner `AudioSource` registered with
      `AudioDevice`. Pinned by `tests/mockingboard_smoke_test.cpp`.
      Slot ROM range used for MMIO (`$Cn00-$Cn0F` / `$Cn80-$Cn8F`) —
      added `slotRomWrite` to `SlotPeripheral` for this. Selectable
      from the Slot Configuration UI; not auto-plugged.

- [x] **Text flashing 2 Hz** 🆕
      ✓ 2026-05 : `kFlashHalfPeriodFrames = 15` (250 ms @ 60 fps =
      2 Hz cycle, MAME `frame_number() & 0x10` parity).

### Tier 3 — Petits ajouts opportunistes

- [ ] **No-slot clock / RTC** 🆕
      ProDOS time-of-day, timestamps fichiers. MAME :
      `bus/a2bus/nippelclock.cpp` ~80 L (MC146818) ou
      `a2thunderclock.cpp` ~150 L (uPD1990A). Effort : trivial.

- [ ] **65C02 instructions manquantes : SMB/RMB/BBR/BBS, WAI/STP** 🆕
      Rockwell + WDC additions. ~5 L par opcode dans `M6502.cpp`,
      mais **quasi-aucun jeu retail ne les utilise** (utilitaires
      système niche). Effort : trivial, impact cosmétique.

- [ ] **NTSC artifact LUT alternate (`color_mode = 1`)** 🆕
      MAME a deux variantes (`artifact_color_lut[2][128]`), POM2
      n'utilise que `[0]` (composite/NTSC primary). La seconde table
      diffère sur 8 entrées (rules `0010000` vs `0110000`). Effort :
      trivial, impact cosmétique sur les bordures de texte 40-col.

### Hors-scope confirmé après audit

- **Z-80 SoftCard** (`bus/a2bus/softcard.cpp`) — CP/M, niche
  business. DMA banking + Z80 CPU. Large effort, pas de jeux.
- **Mouse Card slot 4** (`bus/a2bus/mouse.cpp`) — 68705 µC + 6821 PIA.
  Rare sur II/IIe pour les jeux ; les paddles sufficient.
- **CFFA CompactFlash** (`bus/a2bus/cffa.cpp`) — IDE + EEPROM. POM2 a
  déjà le ProDOS HD slot 5 + host folder mount, qui couvre l'usage.
- **RAMWorks 1 MB** — IIe-only, ProDOS 3+ ; usage très rare.
- **Disk II 13-sector (Apple II 1977-1979)** — disquettes pré-DOS 3.3,
  software de test seulement.

---

Les blocs encore manquants pour un II+ "complet" : VBL `$C019`,
persistance des écritures Disk II, formats `.po`/`.nib`/`.woz`, sélecteur
preset Integer BASIC, character ROM 2 KB (normal/inverse/flashing), RAM
auxiliaire (pré-requis DHGR + Color TEXT EVE pour Le Chat Mauve).

---

## 14. Divergences vs MAME (audit Opus mai 2026)

10 sous-systèmes audités en parallèle contre MAME source-of-truth. Chaque
entrée = un écart **fonctionnel** (pas plomberie/style).

> **Caveat important** : aucun agent n'a pu fetcher le tree MAME live
> (WebFetch + Bash curl refusés). Analyses faites contre la connaissance
> interne MAME des agents. Les constantes exactes (e.g. 11.617 cycles paddle,
> `ay8910_default_levels[]`, `optimal_bit_timing` handling, valeurs PROM P6
> Disk II, ALU dispatch wozfdc) doivent être **re-vérifiées ligne-à-ligne
> contre `mame/master`** avant patch.

Légende sévérité : 🔴 Critical · 🟠 High · 🟡 Medium · 🟢 Low.

### 14.1 CPU 6502 / 65C02 (`M6502.cpp`)

- [ ] 🔴 D flag pas effacé sur entrée IRQ/BRK/NMI en mode 65C02
      (`M6502.cpp:95-103, 105-114, 656-695`). MAME `m65c02_device::do_exec_full`
      fait `P &= ~F_D` avant vector. POM2 reste en décimal si IRQ arrive
      avec D=1.
- [ ] 🔴 PLP/RTI n'imposent pas U=1 / B=0 sur le P pushé
      (`M6502.cpp:649-654, 697-702`). MAME force bit 5=1 et masque bit 4.
- [ ] 🔴 RMW sur `$C0xx` double-toggle les soft switches
      (`M6502.cpp:455-573, 927-945`). ASL/LSR/ROL/ROR/INC/DEC/TSB/TRB appellent
      `memRead` *puis* `memWrite`, deux dispatchs vers `softSwitchAccess`.
      Audible sur le speaker pour `INC $C030`.
- [ ] 🔴 Compteurs de cycles 65C02 multiples faux :
      - `JMP (abs)` = 6 (POM2=5), `JMP (abs,X)` = 6 (POM2=5)
      - RMW abs,X CMOS sans page-cross = 6 (POM2=7)
      - `BIT #imm` = 2 (POM2=1)
      - `STZ zp,X` = 4 (POM2=3)
      - RMBn/SMBn = 5 (POM2=6), BBR/BBS baseline = 5 (POM2=6)
      - BRK = 7 (POM2=9), RTI = 6 (POM2=7)
- [ ] 🟠 WAI ne sort pas à PC+1 sur IRQ masqué (I=1)
      (`M6502.cpp:1022-1031`). WDC : WAI sort sans vector si I=1.
- [ ] 🟠 JMP (abs) page-wrap bug appliqué en CMOS aussi (`M6502.cpp:173-181`).
      Le 65C02 corrige ce bug et coûte 6 cycles ; POM2 reproduit le bug NMOS.
- [ ] 🟠 Décimal 65C02 : N/V/Z non calculés sur ADC/SBC D=1 (`M6502.cpp:379-408`).
      +1 cycle manquant aussi.
- [ ] 🟠 BRK push wrong status + cycle count
- [ ] 🟡 STP réveillable par NMI dans POM2 (`M6502.cpp:1033-1039`).
      WDC : seul RESET sort de STP.
- [ ] 🟡 NMI traitée comme level dans POM2 (`M6502.cpp:1492-1495`).
      Réel : edge-triggered. Risque double-trigger côté Mockingboard.
- [ ] 🟡 `softReset` n'efface pas D (`M6502.cpp:1480-1485`). 65C02 le clear
      au reset hardware.

### 14.2 Disk II LSS (`DiskIICard.cpp`, `DiskImage.cpp`)

- [ ] 🔴 PULSE width = 1 cycle LSS (`DiskIICard.cpp:514-523`). MAME tient PULSE
      bas pendant toute la largeur d'une cellule. Casse protections cycle-précises
      (Spiradisc, RWTS-18, Locksmith calibration).
- [ ] 🟠 Spin-up moteur instantané (`DiskIICard.cpp:559-575`). MAME : ~500-1000 ms
      via attotime `mon_w`. Casse Locksmith RPM measurement.
- [ ] 🟠 `last_6502_write` legacy-gate seulement capturé sur `$C0ED`
      (`DiskIICard.cpp:823`). MAME : toute écriture `$C0Ex` la latche.
- [ ] 🟠 Phase magnets : si `activeDrive` change en plein step, l'autre lecteur
      ne reçoit pas le mask en cours (`DiskIICard.cpp:540-549`). MAME re-émet
      sur le drive nouvellement sélectionné.
- [ ] 🟠 `getNextTransition` sémantique `lower_bound` (= match-at-cursor)
      vs MAME strict-greater (`DiskImage.cpp:579-600`). Marche par compensation
      mais fragile au refactor.
- [ ] 🟡 Inflation sub-instruction via `getCurrentInstructionCycles()`
      (`DiskIICard.cpp:745-752`) au lieu du dispatch per-bus-cycle MAME.
      Cf. spec M6502 — confirmer la sémantique avant patch.
- [ ] 🟡 `writeBackEnabled` gate le LSS splice mais pas le legacy
      (`DiskIICard.cpp:614-627` vs legacyAdvance). Asymétrie.
- [ ] 🟡 Threshold flush pre-emptif : 30 events (POM2) vs 31 (MAME).
- [ ] 🟢 `lssCycle` préservé au reset (POM2) vs zéro MAME.

### 14.3 WOZ / Flux events (`DiskImage.cpp`)

- [ ] 🔴 **Quarter-tracks ignorés** (`DiskImage.cpp:373`). POM2 ne lit que
      `TMAP[t*4]` (35 entrées sur 160 → 78% perdues). Casse toutes les
      protections Roland Gustafsson (Spiradisc → Choplifter, Print Shop,
      Wings of Fury, Rescue Raiders), RWTS18 (Lode Runner, Karateka),
      Locksmith Fast Copy.
- [ ] 🟠 WOZ2 `optimal_bit_timing` (INFO offset +39) ignoré
      (`DiskImage.h:147`). Hard-codé à 32 ticks = 4 µs/cell. Casse WOZ
      à timing non-standard (3.0 µs, slow flux protection).
- [ ] 🟠 FLUX chunk (WOZ2 v2.1+) complètement skipped (`DiskImage.cpp:347`).
      Tout WOZ Applesauce 1.5+ avec FLUX-only lit des bits vides
      (Wings of Fury original, Captain Goodnight, Ankh).
- [ ] 🟡 `INFO.synchronized` et `INFO.cleaned` (offsets +3, +4) non consultés
      (`DiskImage.cpp:332-337`). Sync cross-track manquante.
- [ ] 🟡 WOZ1 `bytes_used` (@+6646) ignoré, hard-codé 6646 (`DiskImage.cpp:384`).
- [ ] 🟢 CRC32 jamais validé ni warné (`DiskImage.cpp:297-299`). Hand-edited /
      truncated files passent silencieusement.
- [ ] 🟢 `INFO.write_protected` lu mais jamais imposé (`DiskImage.cpp:194`).
      Footgun pour write-back futur — utiliser ce flag, pas `wozFormat`.

### 14.4 IIe memory paging (`Memory.cpp`)

- [ ] 🔴 Status reads `$C013-$C018, $C019, $C01E, $C01F` renvoient `0x00` dans
      les 7 bits bas (`Memory.cpp:625-649, 432-433`). MAME : floating-bus byte
      vidéo dans les bits bas. Utilisé par démos/protections pour timing.
      Idem `$C011, $C012, $C015, $C061-$C063, $C064-$C067`.
- [ ] 🟠 `$C001-$C00F` write-only sur HW réel — POM2 toggle aussi sur lecture
      (`Memory.cpp:438-440`).
- [ ] 🟠 ALTZP + Language Card : POM2 a des banques séparées main/aux mais une
      seule paire de latches partagés (`lcBank2Active`, `lcReadRam`,
      `lcWriteEnable` — `Memory.cpp:713-734`). Sather 5-5 : sélection
      indépendante main vs aux. Toggle `$C088` avec ALTZP on puis off
      change la sélection main → bug observable.
- [ ] 🟠 Reset Ctrl-Reset efface tout `iieMemMode` (`Memory.cpp:253-264`).
      Le vrai //e ne clear que `80STORE, RAMRD, RAMWRT, INTCXROM, SLOTC3ROM,
      HIRES, PAGE2, TEXT` ; **conserve** `ALTZP, 80COL, ALTCHAR, DHIRES` sur
      soft reset (seul power-on les efface).
- [ ] 🟠 VBL IRQ jamais asserté (`Memory.cpp:244-249`, commentaire évoque
      "IOUDIS non modélisé"). Casse music players IIe, mouse firmware.
      Modéliser IOUDIS (~3 lignes).
- [ ] 🟡 80COL dispatch redondant : `$C00C/$C00D` traités à la fois en
      `iieHandleSoftSwitch` (Memory.cpp:439) et dans le bloc standalone
      `low == 0x0C` (501-508). Gate le 2e sur `!iieMode`.
- [ ] 🟡 `$CFFF` ne désactive pas le slot actif quand INTCXROM=on
      (`Memory.cpp:854-862`). SlotBus reste latché.
- [ ] 🟢 Race condition : `iieMemRead`/`Write` lisent `display.page2`/`hiRes`
      sans `stateMutex` (`Memory.cpp:666, 672, 691, 700`). TSAN-flagged.

### 14.5 Display HGR / DHGR (`Apple2Display.cpp`)

- [ ] 🔴 `renderHiRes`/`renderLoRes`/`renderText` lisent `mem.data()` (main)
      **sans jamais consulter l'aux RAM** en IIe (`Apple2Display.cpp:380, 499,
      709`). Avec `80STORE+PAGE2+HIRES`, le scanner doit source de l'aux
      `$2000-$3FFF`. DHGR path le fait correctement, HGR/text/lo-res non.
- [ ] 🟠 Sélection de page incohérente : 80-col path force page 1 si 80STORE
      on (`Apple2Display.cpp:907`), 40-col path suit `page2` même si 80STORE
      (`:397, :513`). MAME : 80STORE force page 1 partout.
- [ ] 🟠 `artifact_color_lut` est `[2][128]` dans MAME (row 0 = TV composite,
      row 1 = RGB monitor tinted). POM2 utilise seulement row 0, pas d'enum
      pour row 1 (`Apple2Display.cpp:584-593`).
- [ ] 🟠 Palette Chat Mauve lo-res `#3300DD, #990000...` (`Apple2Display.cpp:474-491`)
      = POM2-original. Pas dans MAME. Documenter comme stylistique ou aligner.
- [ ] 🟡 Path HGR Chat Mauve écrit dans buffer 280-wide au lieu de 560
      (`Apple2Display.cpp:778-787`) → perd la netteté qui est la raison d'être
      de la carte.
- [ ] 🟡 Persistence buffer (afterglow mono) dimensionné 280×192 → DHGR mono
      (560) sans afterglow (`Apple2Display.cpp:107, 1087-1097`). Inconsistance UX.
- [ ] 🟢 `cursorOverlay` déclaré mais jamais dessiné (`Apple2Display.h:79-80`).
      Dead code à supprimer ou implémenter.
- [ ] 🟢 Stale claims dans CLAUDE.md à corriger :
      - "39 inter-byte seam fix-ups" : n'existe plus dans le code
      - "additive horizontal glow" : n'a jamais existé
      - "2 Hz flashing animation pending" : déjà implémenté (`:386, 410,
        423-424, 899, 923, 934`)
      - "//gs-corrected approximation" lo-res : en réalité MAME `apple2_palette`
        benrg NTSC

### 14.6 Mockingboard (`Mockingboard.cpp`)

- [ ] 🔴 Table d'amplitude AY pas la courbe log canonique
      (`Mockingboard.cpp:22-25`). MAME `ay8910_default_levels[]` (steps ≈3 dB,
      facteur √2). POM2 dévie jusqu'à 2× sur les indices 6-12. Comment cite
      `volumes_3level_8910` qui est en fait pour le 8913 3-level. Remplacer
      par la table 8910 canonique.
- [ ] 🟠 `T1LH` write clear `IFR.T1` (`Mockingboard.cpp:225`). **Incorrect** —
      WDC datasheet Table 4 : T1 IRQ cleared seulement par T1CH write et T1CL
      read. Supprimer la ligne `ifr &= ~IFR_T1`.
- [ ] 🟠 Envelope decoder shapes 10/12/14 (`\/\/`, `/|/|`, etc.) probablement
      incorrects (`Mockingboard.cpp:478-543`). MAME utilise une table 16
      shapes × 32 steps pré-calculée. Refondre sur lookup table.
- [ ] 🟡 T2 timer absent (`Mockingboard.cpp:227-229`). Casse driver speech
      d'Ultima IV (Echo+ daughter), démos FrenchTouch utilisant T2 pour
      sample playback.
- [ ] 🟡 Float tone counter aliase à périodes très courtes (1-3)
      (`Mockingboard.cpp:449-456`). MAME : compteur entier.
- [ ] 🟢 Port A read mask par DDR (devrait retourner pin state)
      (`Mockingboard.cpp:126-130`). Sans impact concret sur Mockingboard mais
      techniquement faux ; le test `mockingboard_smoke_test.cpp:87-95` pin
      la valeur incorrecte.

### 14.7 Super Serial Card (`SuperSerialCard.cpp`)

- [ ] 🔴 **Aucun câblage IRQ**. Pattern à copier de `Mockingboard.cpp:598, 679`.
      Casse ProTERM 3.x ("Use Interrupts"), firmware modem //c, MODEM.MGR,
      GS/OS SerialPort driver, drivers CP/M Z80.
- [ ] 🔴 Status read ne clear pas les bits sticky (`SuperSerialCard.cpp:266-276`).
      Vrai 6551 clear IRQ flag (bit 7) sur read — requis par tout driver
      IRQ-driven pour ACK.
- [ ] 🔴 Status write (soft reset) efface tout `cmdReg`
      (`SuperSerialCard.cpp:308-312`). MAME : `cmdReg &= ~0x1F` (préserve
      parity bits 5-7), clear overrun, clear IFR.
- [ ] 🟠 Bit 7 strippé en TX (`SuperSerialCard.cpp:297` `outByte = v & 0x7F`).
      Casse XMODEM, YMODEM, ZMODEM, Kermit-binary, BinSCII, ADTPro upload.
      Le strip bit 7 est une politique terminal, pas UART.
- [ ] 🟠 TDRE toujours = 1 (`SuperSerialCard.cpp:269`). Invisible sur poll,
      casse tout driver IRQ-driven (cf. ci-dessus).
- [ ] 🟠 Pas de tracking d'overrun (`SuperSerialCard.cpp:163-179`). Buffer
      RX plein → `pop_front` silencieux. Casse retransmit Kermit/XMODEM-CRC.
- [ ] 🟠 Echo mode (REM, command bit 4) stocké mais jamais appliqué.
      Casse diagnostic Apple "SSC TEST".
- [ ] 🟠 Control reg jamais consulté : baud rate, word length, stop bits
      stockés mais ignorés.
- [ ] 🟠 DIP-switch readback aux mauvais offsets (`SuperSerialCard.cpp:279-281`).
      Vrai SSC les expose dans le slot ROM `$Cn01, $Cn02, $Cn04, $Cn05`,
      pas dans le 6551 register space. `$C0A0-$C0A3` réel = latch 74LS259.
- [ ] 🟠 Pas de mirror A0-A1 only sur 6551 : `low4 = C/D/E/F` doit mirror
      `8/9/A/B`. Fix : `low4 &= 0x03` après détection ACIA window.
- [ ] 🟡 Signature slot ROM incomplète pour GS/OS / Pascal 1.1 : manque OS
      zero-byte ID block `$CnFB-$CnFF`, `$CnFE` low-nibble = device class.
- [ ] 🟡 Telnet IAC strip mange les `$FF` valides en RX 8-bit binaire.
      Proposer toggle "raw mode" dans le panneau.
- [ ] 🟡 LF→CR mapping appliqué uniquement sur keyboard sink, pas sur `rxBuf`
      (`SuperSerialCard.cpp:187-198`). Incohérence selon que le guest lit
      via clavier vs IN#2.

### 14.8 ThunderClock+ / uPD1990AC (`ClockCard.cpp`)

- [ ] 🟠 **Bug month nibble dans `shiftReg[4]`** (`ClockCard.cpp:79-80`).
      `(toBcd(month) & 0xF0) | (dow & 0x0F)` efface le mois pour mois 1-9
      (toBcd(5)=0x05, & 0xF0 = 0x00). Fix :
      `((toBcd(month) & 0x0F) << 4) | (dow & 0x0F)`.
      **Le test `clock_card_smoke_test.cpp:124` pin la valeur cassée** — à
      corriger aussi.
- [ ] 🟠 Seul `MODE_TIME_READ` implémenté (`ClockCard.cpp:13-16, 109-116`).
      Pas de TIME_SET (impossible de régler), pas de TP=64/256/2048 Hz
      (utilitaires interval-timing comme *Clockworks* ne tickent jamais).
- [ ] 🟠 Pinout simplifié — POM2 mappe directement sur `$C0n0` (`ClockCard.cpp:19-21`),
      le vrai ThunderClock+ passe par un 6522 VIA. ProDOS marche (driver
      hardcodé à `$D742`) ; tout soft écrivant les registres VIA `$C0n1-$C0nF`
      lit du 0xFF.
- [ ] 🟡 Pas de mode TEST, pas de CS gating, DATA_OUT "live" vs MAME qui
      latch sur edge CLK.
- [ ] 🟢 Pas de compteur interne 1 Hz — `std::time(nullptr)` à chaque STB.
      Casse déterminisme snapshot.
- [ ] 🟢 Slot ROM essentiellement vide (juste signature). Vrai ThunderClock+
      ROM contient ~250 octets de driver RTS-able utilisé par DOS 3.3
      patches et Applesoft.

### 14.9 Speaker + Cassette (`SpeakerDevice.cpp`, `CassetteDevice.cpp`)

- [ ] 🟠 `setSampleRate()` jamais câblé depuis `AudioDevice::getActualSampleRate()`
      (`SpeakerDevice.cpp:60-61`). Speaker reste sur `kSampleRate=44100` par
      défaut. Sur host Apple Silicon 48 kHz → tons **~8.8% trop aigus**.
      Vérifier wiring dans `MainWindow.cpp` / `EmulationController.cpp`.
- [ ] 🟠 Reconstruction square-wave naïve (`SpeakerDevice.cpp:136-139`).
      Snap+LP au lieu de l'intégration rectangle-area de MAME
      `spkrdev::sound_stream_update`. Pour toggles >sample_rate/4 (Karateka
      4-5 kHz, click effects), POM2 alias / perd de l'énergie.
- [ ] 🟠 Timestamp `cycleCounter + getCurrentInstructionCycles()`
      (`Memory.cpp:538-540`) : à confirmer que `getCurrentInstructionCycles()`
      donne l'offset au bus access exact, pas le total instruction.
      MAME : `machine().time()` snapshotté au moment précis.
- [ ] 🟠 `$C020` décodé 16-byte aliased `$C020-$C02F` (`Memory.cpp:549-552`).
      Sur IIe : **`$C028 = ALTCHAR write`** dans certaines configs. Risque
      de stomper ALTCHAR si l'ordre `softSwitchAccess` ne préempte pas.
      À vérifier.
- [ ] 🟡 DC blocker (p=0.995) non-MAME (`SpeakerDevice.cpp:108, 142-144`).
      MAME n'en a pas, encode symétrique ±level. Atténue contenu <12 Hz
      (inaudible mais divergence).
- [ ] 🟡 LP 5 kHz hard-coded "cone model" (`SpeakerDevice.h:74`). MAME laisse
      cela aux filtres machine-config externes (FILTER_RC). Son moins
      "snappy" sur transients.
- [ ] 🟡 Cassette seuil `±0.02` pré-extrait au load (vs MAME `> 0.0` à
      runtime). Rejette les rips faible volume.
- [ ] 🟡 Auto-rewind 500 ms si pas de poll `$C060` (`CassetteDevice.cpp:461-465`).
      MAME ne rewind jamais. Casse les loaders custom qui poll sporadiquement.

### 14.10 Soft switches + Paddle (`Memory.cpp`, `JoystickInput.cpp`)

- [ ] 🟠 Language Card prewrite latch armé sur **écritures** aussi
      (`Memory.cpp:713-734`). Vrai LC : prewrite armé seulement sur read
      (ou `BIT $C08x`) ; `STA $C08x` doit clear le latch. Casse les patterns
      `LDA $C081 / STA $C08B` (Sather 5-12, MAME `lc_w` n'arme pas prewrite).
      Fix : passer `isWrite` à `languageCardSwitchAccess`.
- [ ] 🟠 Paddle RC constant `paddleValue * 11` (`Memory.cpp:577`). Vrai RC
      Apple II = ~11.34 µs/step à 1.022727 MHz → **~11.617 cycles**.
      Erreur 8% → full deflection à ~92% de 255. Fix : `* 12` ou fixed-point
      `* 1162 / 100`.
- [ ] 🟠 `paddleValue[]` init à 128 (`Memory.cpp:267`) au lieu de 0 → 1408
      cycles de "hold" même sans joystick branché. Casse auto-test //e.
      MAME : 0 jusqu'à `setPaddle` host-driven.
- [ ] 🟠 PB3 / Shift-Key Mod manquant en mode IIe : `$C063` retourne
      `paddleButton[2]` inconditionnellement (`Memory.cpp:567`). Sur IIe
      avec jumper SHK, `$C063` est câblé sur Shift — utilisé par Lode Runner,
      Choplifter. Devrait OR `shiftHeld` en `iieMode`.
- [ ] 🟡 `$C070` PTRIG ne retourne pas floating bus (`Memory.cpp:582-585`)
      et n'alias pas sur tout `$C070-$C07F`. POM2 ne catch que `low == 0x70`.
- [ ] 🟡 `$C068-$C06F` décode manquant — sur IIe `$C068 = STATEREG`,
      `$C06C-$C06F = band-select`.

### 14.11 Actions prioritaires

Top 10 par ratio impact / effort :

1. **Bug month nibble ClockCard** (1 ligne + fix test) — §14.8
2. **Câbler `setSampleRate()` speaker** depuis `AudioDevice` — §14.9
3. **Câbler IRQ SSC** (copier pattern Mockingboard) — §14.7
4. **Retirer `& 0x7F` TX SSC** (débloque XMODEM/Kermit/ADTPro) — §14.7
5. **Retirer `ifr &= ~IFR_T1` Mockingboard T1LH** — §14.6
6. **Table amplitude AY 8910 canonique** — §14.6
7. **Paddle constant 11 → 12** + init 128 → 0 — §14.10
8. **LC prewrite gate sur `!isWrite`** — §14.10
9. **D flag clear sur IRQ 65C02** — §14.1
10. **Aux RAM dans renderHiRes/renderText/renderLoRes IIe** — §14.5

Effort plus lourd mais valeur élevée :
- Quarter-tracks WOZ (débloque Spiradisc/RWTS-18, ~120 L `DiskImage`) — §14.3
- Envelope shape table AY (refonte ~80 L) — §14.6
- Floating-bus dans low-7 bits des status reads (transverse) — §14.4
