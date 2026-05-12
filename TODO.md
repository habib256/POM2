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

## 14. Divergences vs MAME (audit Opus, vérifié 2026-05-12)

10 sous-systèmes audités en parallèle contre MAME `master` (fetch via
Scrapling le 2026-05-12). Première passe avait été faite de mémoire et
livrait beaucoup de **faux positifs** ; cette section les a purgés et
ancre chaque finding restant sur une référence MAME `file:line` directe.

> **Note méthodologique** : sur les ~85 findings de la passe 1, ~20 ont
> été retirés comme faux positifs (POM2 matche déjà MAME) et ~25 ont été
> reformulés. Ce sont les findings **survivants** qui sont conservés
> ici. Les références MAME pointent vers `src/devices/cpu/m6502/`,
> `src/devices/machine/`, `src/devices/sound/`, `src/devices/bus/a2bus/`,
> `src/lib/formats/as_dsk.cpp` (formerly `woz_dsk.cpp`), et
> `src/mame/apple/`.

Légende sévérité : 🔴 Critical · 🟠 High · 🟡 Medium · 🟢 Low.

### 14.1 CPU 6502 / 65C02 (`M6502.cpp`)

MAME base CMOS = `w65c02` (pas `m65c02` qui n'existe pas), tables dans
`om6502.lst` / `ow65c02.lst`.

- [ ] 🔴 D flag pas effacé sur IRQ/BRK/NMI en mode 65C02
      (`M6502.cpp:95-103, 105-114, 656-695`). MAME `ow65c02.lst:259`
      `brk_c_imp` : `m_P = (m_P | F_I) & ~F_D;`. Trois call sites POM2
      à corriger (handleIRQ, handleNMI, BRK).
- [ ] 🔴 PLP/RTI ne forcent pas U=1 ET B=1 sur P pop
      (`M6502.cpp:649-654, 697-702`). MAME `om6502.lst:959` :
      `m_TMP = read(m_SP) | (F_B|F_E);` (OR avec `0x30`, pas mask).
      Importe parce que IRQ push B=0/U=1, donc sans le OR PLP/RTI
      perdent B.
- [ ] 🔴 RMW sur `$C0xx` triple-dispatche softSwitchAccess
      (`M6502.cpp:455-573, 927-945`). Réel : **1 read + 2 writes**
      (orig puis modifié — MAME `om6502.lst:161-164`). POM2 fait
      1 read + 1 write. Speaker toggle sur `INC $C030` fire 1× au
      lieu de 2×.
- [ ] 🟠 Compteurs de cycles 65C02 (POM2 / MAME) :
      - `JMP (abs)` 0x6C : 5 / **6** (`ow65c02.lst:387-395`)
      - `JMP (abs,X)` 0x7C : 5 / **6** (`ow65c02.lst:377-386`)
      - `BIT #imm` 0x89 : 1 / **2** (`ow65c02.lst:210-217`)
      - `STZ zp,X` 0x74 : 3 / **4** (`ow65c02.lst:739-744`)
      - RMBn / SMBn : 6 / **5** (`ow65c02.lst:497-504, 700-707`)
      - BBRn / BBSn baseline : 6 / **5** (`ow65c02.lst:168-181`)
      - BRK : 8 / **7** (`ow65c02.lst:234-261`)
      - RTI : 7 / **6** (`om6502.lst:1050-1059`)
- [ ] 🟠 WAI parke PC au lieu de continuer (`M6502.cpp:1022-1031`).
      MAME `ow65c02.lst:797-803` : WAI loop jusqu'à `!m_nmi_pending &&
      !m_irq_state` (réveille même si I=1, sans vector si masqué).
      Fix : NE PAS decrement PC ; consommer des cycles ; tomber sur
      PC+1 quand IRQ pin se lève.
- [ ] 🟠 JMP (abs) page-wrap bug NMOS appliqué en CMOS aussi
      (`M6502.cpp:173-181`). MAME splitté : `om6502.lst:649-656`
      buggy (NMOS) ; `ow65c02.lst:387-395` fixed (CMOS, +1 cycle —
      cf. cycles ci-dessus).
- [ ] 🟠 Décimal 65C02 : POM2 calcule N/V/Z mais sur intermédiaire
      binaire, pas sur A ajusté (`M6502.cpp:325-345, 379-408`). MAME
      `ow65c02.lst:11-14` : `if(F_D) { read_pc(); set_nz(m_A); }` —
      +1 cycle + NZ recomputé sur A ajusté final.
- [ ] 🟠 IRQ/NMI priorité inversée dans `step()`
      (`M6502.cpp:1499-1502`). POM2 check IRQ d'abord puis NMI. MAME
      `prefetch_end` (m6502.cpp:455) + `brk_c_imp` (ow65c02.lst:247)
      pick le **vector NMI** quand `m_irq_taken && m_nmi_pending`.
- [ ] 🟡 STP réveillable par NMI dans POM2 (`M6502.cpp:1033-1039`).
      MAME `ow65c02.lst:715-718` : seul `reset_c` (`:805`) sort.
- [ ] 🟡 `softReset` n'efface pas D (`M6502.cpp:1480-1485`). MAME
      `ow65c02.lst:814` : `m_P = (m_P | F_I) & ~F_D;` (CMOS-only ;
      NMOS `om6502.lst:1279` ne clear que I).

### 14.2 Disk II LSS (`DiskIICard.cpp`, `DiskImage.cpp`)

MAME ref `src/devices/machine/wozfdc.cpp`. **4 faux positifs purgés**
de la passe 1 (PULSE width, motor spin-up, phase magnets re-emit,
flush threshold 30 vs 31 — tous matchent MAME).

- [ ] 🟠 `last_6502_write` legacy-gate seulement capturé sur `$C0ED`
      (`DiskIICard.cpp:822-826`). MAME `wozfdc.cpp:181-186` :
      latche sur **toute** écriture `$C0Ex` inconditionnellement.
      Path LSS POM2 est OK (line 817).
- [ ] 🟠 `$C0EB` (drive 2 select) ne cycle pas `mon_w` (`DiskIICard.cpp:594-597`).
      MAME `wozfdc.cpp:229-241` : `mon_w(true)` sur l'ancien drive,
      `mon_w(false)` sur le nouveau (efface cache, reset
      `m_revolution_start_time`). POM2 ne fait que flipper
      `activeDrive` — intentionnel mais à documenter comme
      divergence MAME.
- [ ] 🟡 Inflation sub-instruction RAII (`DiskIICard.cpp:711-744`)
      vs dispatch per-bus-cycle MAME m6502. Documenté comme tradeoff ;
      audible si on porte de l'audio précis ou des protections
      cycle-sensibles.
- [ ] 🟡 `writeBackEnabled` gate le LSS splice mais pas le legacy
      (`DiskIICard.cpp:339, 466, 618`). Asymétrie : legacy mute la
      source file même en `writeBackEnabled=false`.
- [ ] 🟢 `lower_bound` vs MAME `upper_bound + cycles-1`
      (`DiskImage.cpp:579-600`). Équivalent par construction (cf.
      commentaire ligne 385-415) ; fragile au refactor.
- [ ] 🟢 `device_reset` MAME `wozfdc.cpp:85` re-anchor `cycles =
      time_to_cycles(machine().time())`. POM2 préserve `lssCycle`.
      Équivalent sous compteur monotonique CPU.
- [ ] 🟢 Pas de timer périodique 10 ms de catch-up (MAME
      `wozfdc.cpp:94`). POM2 sync seulement sur access ou
      `advanceCycles`. OK avec notre cadence per-instruction.

### 14.3 WOZ / Flux events (`DiskImage.cpp`)

MAME ref `src/lib/formats/as_dsk.cpp` class `woz_format` (le fichier
`woz_dsk.cpp` cité dans `DiskImage.cpp:214` n'existe **plus** dans
MAME master — renommé). **2 faux positifs purgés**.

- [ ] 🔴 **Quarter-tracks ignorés** (`DiskImage.cpp:373` + `DiskImage.h:223`
      `bitStream[kTracks=35]` + `DiskIICard.cpp:328, 383, 620, 637, 792`
      tous divisent `headQuarterTrack[d] / 4`). MAME `as_dsk.cpp:300-313`
      charge 141 sous-pistes avec `subtrack = trkid & 3` ; `floppy.cpp:835`
      garde `m_subcyl` séparé. Bug enjambe les deux fichiers (image + card).
      Casse Spiradisc (Choplifter / Print Shop / Wings of Fury / Rescue
      Raiders), RWTS18 (Lode Runner, Karateka), Locksmith Fast Copy.
- [ ] 🟠 FLUX chunk WOZ2 v2.1+ ignoré (`DiskImage.cpp:347`). MAME
      `as_dsk.cpp:287-290` lit `off_flux = u16le[info+46]` puis
      `:320-322` walk `load_flux_track` (8-bit delta ticks). Casse Wings
      of Fury original, Captain Goodnight, Ankh, Sundog.
- [ ] 🟠 CRC32 jamais validé (`DiskImage.cpp:297-299`). MAME
      `as_dsk.cpp:275-277` **rejette** le load sur mismatch
      (`if(crc != get_u32le(&img[8])) return false;`). Algorithme :
      `crc32r` reversed poly `0xedb88320` (`as_dsk.cpp:10-23`).
      POM2 accepte du garbage silencieusement.
- [ ] 🟡 WOZ2 `optimal_bit_timing` (INFO+39) ignoré (`DiskImage.h:147`).
      MAME aussi ne le lit pas au load — drive le pacing depuis
      `track_size*2` ticks via `flopimg.cpp:673`. Vrai gap : placement
      `cellIdx*8+4` fixe (`DiskImage.cpp:555`) ne varie pas par sous-piste.
- [ ] 🟡 Splice point WOZ1 (`+6650, u16, $FFFF=none`) ignoré
      (`DiskImage.cpp:381-398`). MAME `as_dsk.cpp:309` lit et passe à
      `set_write_splice_position`. Important pour write-back angulaire
      correct (sinon corruption FAT-like chains au format).
- [ ] 🟡 `info_version` bornes 1..3 non vérifiées (`DiskImage.cpp:332-337`).
      MAME `as_dsk.cpp:284-286` : `if(info_vers < 1 || info_vers > 3)
      return false;`.
- [ ] 🟢 `INFO.write_protected` lu mais jamais imposé (`DiskImage.cpp:194`).
      Footgun pour write-back futur — utiliser ce flag, pas `wozFormat`.
- [ ] 🟢 Commentaire stale (`DiskImage.cpp:214`) cite `woz_dsk.cpp` qui
      n'existe plus dans MAME master. Renommer en `as_dsk.cpp` /
      `class woz_format`.

**Faux positifs retirés** : `INFO.synchronized/cleaned` (MAME ne les
lit pas au load non plus — save-side only) ; WOZ1 `bytes_used` (MAME
utilise aussi `bit_count` comme borne autoritative).

### 14.4 IIe memory paging (`Memory.cpp`)

MAME ref `src/mame/apple/apple2e.cpp`. **2 faux positifs purgés**
(Ctrl-Reset preserve, VBL IRQ jamais asserté).

- [ ] 🟠 Status reads bits 0-6 = `0x00` au lieu de `m_transchar`
      (`Memory.cpp:625-649`). MAME `apple2e.cpp:1842-1871` `c000_r`
      retourne `(bit ? 0x80 : 0x00) | m_transchar` pour `$C011-$C01F` —
      les bits bas portent le **dernier char clavier**, pas le
      floating bus comme la passe 1 disait. Floating-bus réel seulement
      pour `$C061-$C067` (paddles/buttons) et `$C019` IIc mouse path
      (`:1972`).
- [ ] 🟠 `$C001-$C00F` write-only sur HW réel — POM2 toggle aussi sur
      lecture (`Memory.cpp:438-440`). MAME `apple2e.cpp:1821-1957`
      `c000_r` retourne `m_transchar | m_strobe` sur `$C001-$C00F`
      sans flipper de bit paging.
- [ ] 🟠 ALTZP + Language Card : latches partagés `lcBank2Active /
      lcReadRam / lcWriteEnable` (`Memory.cpp:713-734`). **MAME même
      modèle** (`apple2e.cpp:2801-2882` `lc_r/lc_w` testent `m_altzp`
      pour le storage mais `lc_update :1506-1561` gère les latches en
      single global). POM2 matche MAME. Sather 5-5 lirait "indépendant"
      mais MAME et POM2 partagent. Documenter, pas patcher.
- [ ] 🟠 `iieHandleSoftSwitch` ne broadcast pas `slots.broadcastVideoSwitch`
      pour `$C00C/$C00D` (80COL) en mode IIe. POM2 a un dispatch
      redondant (`Memory.cpp:439` + bloc standalone `:501-508`). Fix :
      gate le standalone sur `!iieMode` ET ajouter `broadcastVideoSwitch`
      dans `iieHandleSoftSwitch` pour Le Chat Mauve.
- [ ] 🟡 `$CFFF` ne désactive pas le slot actif quand INTCXROM=on
      (`Memory.cpp:854-862`). MAME `c800_r :2634-2653` : `if (offset
      == 0x7ff) { m_cnxx_slot = CNXX_UNCLAIMED; m_intc8rom = false; }`
      **toujours**, indépendamment d'INTCXROM. Fix : ajouter
      `if (addr == 0xCFFF) slots.deactivateExpansion();` avant le
      short-circuit internal-ROM.
- [ ] 🟡 `$C019` non gated sur `iieMode` (`Memory.cpp:452-463`). Sur
      II+, devrait retourner floating bus ; POM2 retourne `0x80`/`0x00`
      basé sur scanline. Gate sur `iieMode`.
- [ ] 🟢 Race : `iieMemRead`/`Write` lisent `display.page2`/`hiRes` sans
      `stateMutex` (`Memory.cpp:666, 672, 691, 700`). TSAN flag, en
      pratique bool-aligned.
- [ ] 🟢 `$C040` utility strobe pulse non modélisé. MAME `apple2e.cpp:1711-1716`
      `m_gameio->strobe_w(0); strobe_w(1);`. Pas de consumer POM2 actuel.
- [ ] 🟢 AN0-AN2 annunciators `$C058-$C05D` non drivés. MAME `:1750-1773`.

**Faux positifs retirés** :
- "Ctrl-Reset préserve ALTZP/80COL/ALTCHAR/DHIRES" : MAME
  `apple2e.cpp:1284-1347` clear bien **tout** sur Ctrl-Reset (Sather
  Fig 5.13 / 7.1 cités). POM2 matche.
- "VBL IRQ jamais asserté casse music players" : VBL IRQ est gated sur
  `m_isiic || m_isace500` (`apple2e.cpp:1617`) — feature IIc/IIc+/IIgs
  **seulement**. Sur IIe `$C05A/$C05B` ne setent jamais `m_vblmask`.
  POM2 matche MAME. Music players IIe ne sont pas cassés par ça.

### 14.5 Display HGR / DHGR (`Apple2Display.cpp`)

MAME ref `src/mame/apple/apple2video.cpp`. Palette + formules
interleave + DHGR ordering + rotl4b offset **tous byte-exacts** vs
MAME. Tous les claims existants confirmés + 5 NEW.

- [ ] 🔴 `renderHiRes`/`renderLoRes`/`renderText` lisent `mem.data()`
      (main) sans jamais consulter l'aux RAM en IIe (`Apple2Display.cpp:380,
      499, 709`). Avec `80STORE+PAGE2+HIRES`, le scanner doit source aux
      `$2000-$3FFF`. DHGR (`:1011-1012`) le fait. **Caveat** : MAME
      `hgr_update :766` partage la même limitation — donc divergence
      avec le HARDWARE, pas avec MAME. Fixer pour précision matérielle.
- [ ] 🟠 Sélection de page incohérente : 80-col path force page 1 si
      80STORE on (`Apple2Display.cpp:907`), 40-col path suit `page2`
      même si 80STORE (`:397, :513`). MAME `use_page_2()` (`apple2video.cpp:359`):
      `m_page2 && !m_80store` appliqué uniformément
      (text_update :671, lores :588, hgr :739, dhgr :799).
- [ ] 🟠 `artifact_color_lut[2][128]` : row 1 inutilisée
      (`Apple2Display.cpp:584-593`). MAME `apple2video.cpp:376` :
      row 1 = "medium-color biased" LUT, utilisée par `hgr_update :788`
      pour Video-7 RGB color-bias path ET dispatched via
      `composite_color_mode()`.
- [ ] 🟠 DHIRES annunciator sur HGR standard non honoré
      (`Apple2Display.cpp:706+`). MAME `apple2video.cpp:747` :
      `bit7_mask = m_dhires ? 0 : 0x80` — suppression du half-dot
      delay (Rev 0 emulation sans orange/bleu).
- [ ] 🟠 `composite_color_mode()` : seul mode 0 implémenté. MAME
      `apple2video.cpp:479-498` a 3 modes (0=composite artifact,
      1=medium-bias LUT row 1, 2=hard 4-bit square filter).
- [ ] 🟠 Palette Chat Mauve lo-res (`Apple2Display.cpp:474-491`,
      `#3300DD/#990000/...`) ET HGR (`:691-702`) = POM2-original.
      MAME n'a qu'une palette `apple2_palette[]`. Documenter
      comme stylistique.
- [ ] 🟡 Path HGR Chat Mauve écrit dans buffer 280-wide au lieu de 560
      (`Apple2Display.cpp:778-787`). Perd la netteté qui est la raison
      d'être de la carte. Cf. DHGR qui écrit dans `frame80`.
- [ ] 🟡 Persistence buffer (afterglow mono) dimensionné 280×192 →
      DHGR mono 560 sans afterglow (`Apple2Display.cpp:107, 1087-1097`).
      Inconsistance UX. Resize lazy ou documenter.
- [ ] 🟢 `monochrome_dhr_shift()` 1-px alignment manquant en DHGR mono
      (MAME `apple2video.cpp:460-471`). Cosmétique.
- [ ] 🟢 Pas de floating-TTL `empty_words[40] = {0x3fff,…}` pour rows
      hors-bounds (MAME `:751-758`). Jamais atteint en 48K+.
- [ ] 🟢 `cursorOverlay` déclaré mais jamais dessiné
      (`Apple2Display.h:79-80`). Dead code.
- [ ] 🟢 Stale claims CLAUDE.md à corriger :
      - "39 inter-byte seam fix-ups" : n'existe plus dans le code
      - "additive horizontal glow" : n'a jamais existé
      - "2 Hz flashing animation pending" : déjà implémenté
        (`:386, 410, 423-424, 899, 923, 935`)
      - "//gs-corrected approximation" lo-res : en réalité
        `apple2_palette[]` benrg NTSC byte-exact

### 14.6 Mockingboard (`Mockingboard.cpp`)

MAME refs `src/devices/machine/6522via.cpp`, `src/devices/sound/ay8910.cpp`.
**1 faux positif purgé** (T1LH IFR clear), 1 sévérité downgradée
(amplitude table).

- [ ] 🟠 Envelope decoder POM2 (`Mockingboard.cpp:478-543`) ne match
      pas MAME `ay8910.cpp:989-1020` + `ay8910.h:204-221`. MAME utilise
      un state machine **4-flag** (`attack/hold/alternate/holding`),
      pas une LUT 16×32. Bugs POM2 concrets :
      - ligne 491 `envStep = 16` halt position : MAME hold à step 0
        avec attack flipped, pas step 16
      - ligne 522 `rising = attack ^ (alt && secondHalf)` correct pour
        cont=1 mais branche cont=0 séparée est wrong ; MAME unifie en
        mappant cont=0 vers `hold=1, alternate=attack`
      → shapes 10 (`/\/\`), 12, 14 produisent des waveforms incorrects.
- [ ] 🟡 T2 timer absent (`Mockingboard.cpp:227-229`). MAME `6522via.cpp:73`
      `INT_T2=0x20`, lignes 97-114 implémentation complète (one-shot +
      PB6 pulse-count + IFR bit 5). Casse driver speech Ultima IV
      (Echo+ daughter), démos FrenchTouch sample playback.
- [ ] 🟡 Float tone counter aliase à périodes très courtes (1-3)
      (`Mockingboard.cpp:449-456`). MAME `ay8910.cpp:954-962` :
      compteur **entier** `count += 1; while (count >= period) { ... }`.
      Float ε accumule sur milliers de toggles → pitch drift.
- [ ] 🟢 Table amplitude AY (`Mockingboard.cpp:22-25`) dévie de **15-22%**
      vs MAME (pas 2× comme la passe 1 disait). MAME utilise un
      modèle de réseau résistif (`ay8910.cpp:631-637` `ay8910_param`
      Westcott 2001 + `:718-748` `build_single_table`), pas
      `ay8910_default_levels[]` qui n'existe pas. Si swap, valeurs
      canoniques `normalize=1` :
      ```
      {0.0000, 0.0105, 0.0154, 0.0223, 0.0321, 0.0468, 0.0635, 0.1061,
       0.1319, 0.2164, 0.2974, 0.3909, 0.5128, 0.6371, 0.8186, 1.0000}
      ```
- [ ] 🟢 Port A read mask par DDR (`Mockingboard.cpp:126-130`).
      MAME `6522via.cpp:464-468` : commentaire "port a in the real
      6522 does not mask off the output pins". Sans impact concret
      pour Mockingboard (PA = output uniquement) ; test
      `mockingboard_smoke_test.cpp:87-95` pin la valeur incorrecte.

**Faux positif retiré** : "T1LH write clear `IFR.T1`" — MAME
**fait pareil** (`6522via.cpp:746-749` `case VIA_T1LH:
clear_int(INT_T1)`). POM2 ligne 225 est correct, surtout pas
supprimer.

### 14.7 Super Serial Card (`SuperSerialCard.cpp`)

MAME refs `src/devices/machine/mos6551.cpp` + `src/devices/bus/a2bus/a2ssc.cpp`.
Tous claims confirmés + 6 NEW.

- [ ] 🔴 **Aucun câblage IRQ**. MAME `a2ssc.cpp:243` :
      `m_acia->irq_handler().set(FUNC(...acia_irq_w))`, `:369-385`
      `raise_slot_irq()/lower_slot_irq()` gated sur DIP `m_dswx & 4`.
      MAME `mos6551.cpp:135-150` `output_irq()` est l'asserter. Pattern
      à copier de `Mockingboard.cpp:598, 679`.
- [ ] 🔴 Status read ne clear pas `m_irq_state` (`SuperSerialCard.cpp:266-276`).
      MAME `mos6551.cpp:237-250` : `read_status()` fait `m_irq_state = 0;
      update_irq();`. SR_IRQ tracké via `output_irq` (`:142-147`) en
      reflet inversé du pin IRQ. Aussi : MAME mask TDRE sur read si
      `m_cts` (`:240-243`).
- [ ] 🔴 Status write (soft reset) efface tout `cmdReg`
      (`SuperSerialCard.cpp:308-312`). MAME `mos6551.cpp:264-270`
      `write_reset()` : `m_status &= ~SR_OVERRUN; m_irq_state &=
      ~(IRQ_DCD | IRQ_DSR); update_irq(); write_command(m_command &
      ~0x1f)` — clear bits 0-4 du command, **préserve parity bits 5-7**.
- [ ] 🟠 Bit 7 strippé en TX (`SuperSerialCard.cpp:297`
      `outByte = v & 0x7F`). MAME `mos6551.cpp:636-680` shift verbatim
      jusqu'à `m_wordlength` (bits ctrl 5-6). Casse XMODEM/YMODEM/
      ZMODEM/Kermit-binary/BinSCII/ADTPro upload.
- [ ] 🟠 TDRE toujours = 1 (`SuperSerialCard.cpp:269`). MAME
      `mos6551.cpp:259-263` `write_tdr` fait `m_status &= ~SR_TDRE`
      immédiatement ; remis à 1 par `transmitter_clock` (`:618`)
      au shift-out du start bit. Casse tout driver IRQ-driven.
- [ ] 🟠 Pas de tracking d'overrun (`SuperSerialCard.cpp:163-179`).
      MAME `mos6551.cpp:542-543` set SR_OVERRUN quand byte arrive avec
      RDRF déjà set. Casse retransmit Kermit / XMODEM-CRC.
- [ ] 🟠 Echo mode (REM, command bit 4) stocké mais jamais appliqué.
      MAME `mos6551.cpp:584-594` route m_rxd → output_txd. Casse
      diagnostic Apple "SSC TEST".
- [ ] 🟠 Control reg jamais consulté. MAME `mos6551.cpp:271-285`
      `write_control` dérive `m_wordlength`, `m_extrastop`,
      `m_rx_internal_clock`, baud divider via `update_divider()`
      (`:203-230`, table `:44-47`).
- [ ] 🟠 DIP-switch readback **mal placés** (`SuperSerialCard.cpp:279-281`).
      MAME `a2ssc.cpp:339-353` : DIPs en **device-select** `$C0n0-$C0n7`,
      avec bit 1 sélectionnant DSW1 (AND-mask actif-bas), bit 0 DSW2,
      bit 3 routant vers ACIA. POM2 mettait DIPs à `low4 = 1, 2, 3`
      avec duplication. Pas dans slot ROM.
- [ ] 🟠 Pas de mirror A0-A1 only sur 6551 : `low4 = C/D/E/F` doit
      mirror `8/9/A/B`. MAME `a2ssc.cpp:343` : `m_acia->read(offset &
      3)`. Fix : `low4 &= 0x03` après détection ACIA window.
- [ ] 🟠 `read_rdr` (data register read) doit clear PARITY_ERROR /
      FRAMING_ERROR / OVERRUN / RDRF. MAME `mos6551.cpp:231-236`.
- [ ] 🟠 DTR (command bit 0) drive side-effects. MAME `mos6551.cpp:290-292` :
      `output_dtr(!(bit 0))`, puis `m_rx_irq_enable = !(bit 1) &&
      !m_dtr`. Drop DTR → disable RX/TX-IRQ, reset RX state, txd
      forcé MARK.
- [ ] 🟠 DCD/DSR transitions doivent raise IRQ via `m_irq_state |=
      IRQ_DCD/IRQ_DSR` (MAME `mos6551.cpp:443-461`, gated sur `!m_dtr`).
      POM2 set SR_DCD/SR_DSR mais ne raise jamais. Casse modem dialer
      ProTERM "carrier detect".
- [ ] 🟡 Signature slot ROM incomplète pour GS/OS / Pascal 1.1 : manque
      Pascal 1.1 ID block `$CnFB-$CnFF` (`$CnFE` low-nibble = device
      class, `$CnFF` = entry-point offset). MAME ship le vrai
      `341-0065-a.bin` (2 KB, `a2ssc.cpp:68-69`).
- [ ] 🟡 Telnet IAC strip mange les `$FF` valides en RX 8-bit binaire.
      Proposer toggle "raw mode" dans le panneau.
- [ ] 🟡 LF→CR mapping appliqué uniquement sur keyboard sink, pas sur
      `rxBuf` (`SuperSerialCard.cpp:187-198`). Incohérence selon que
      le guest lit via clavier vs IN#2.
- [ ] 🟢 IRQ gate sur DIP : quand on câblera IRQ, gater sur un setting
      persistable (équivalent du SW2:6 "Interrupts"). MAME
      `a2ssc.cpp:373`.

### 14.8 ThunderClock+ / uPD1990AC (`ClockCard.cpp`)

MAME refs `src/devices/machine/upd1990a.cpp` + `src/devices/bus/a2bus/a2thunderclock.cpp`.
**1 faux positif purgé** (VIA pinout).

- [ ] 🟠 **Bug month nibble dans `shiftReg[4]`** (`ClockCard.cpp:79-80`).
      `(toBcd(month) & 0xF0) | (dow & 0x0F)` efface le mois pour mois
      1-9 (toBcd(5)=0x05, &0xF0=0x00). **Fix correct** (MAME
      `upd1990a.cpp:95`): `(month << 4) | (dow & 0x0F)` — **mois stocké
      en 4-bit binaire, pas BCD**. L'autre fix proposé `(toBcd(month)
      & 0x0F) << 4` casserait octobre-décembre (`toBcd(10)=0x10,
      &0x0F=0`). Test `clock_card_smoke_test.cpp:124` pin la valeur
      cassée `0x06` — devrait être `0x56` (mai/sat).
- [ ] 🟠 Seul `MODE_TIME_READ` implémenté (`ClockCard.cpp:13-16, 109-116`).
      MAME `upd1990a.h:56-74` liste 16 modes ; `upd1990a.cpp:194-289`
      implémente TIME_SET (shift→time), TP=64/256/2048 Hz (`:248-267`),
      TEST (`:274-286`). Pas de TIME_SET → impossible de régler l'horloge ;
      pas de TP → utilitaires interval-timing (*Clockworks*) ne tickent
      jamais.
- [ ] 🟠 `MODE_SHIFT` aussi non implémenté — POM2 shift sur **n'importe
      quel CLK rise** (`ClockCard.cpp:123-129`). MAME `upd1990a.cpp:312-327`
      ne shift que si `m_c == MODE_SHIFT`. Marche par accident parce
      que le driver ProDOS hardcodé est tolérant.
- [ ] 🟡 DATA_OUT live (`ClockCard.cpp:88-94`) vs MAME latch sur CLK
      edge en MODE_SHIFT (`upd1990a.cpp:312-327`) ; pré-loaded sur
      STB en TIME_READ (`:226-247`). Differs hors ProDOS.
- [ ] 🟡 `read_c0nx` ignore offset — POM2 retourne `0xFF` pour
      `$C0n1-$C0nF` (`ClockCard.cpp:89-93`). MAME `a2thunderclock.cpp:112-115`
      mirror DATA_OUT sur tous les 16 offsets.
- [ ] 🟢 Pas de compteur interne 1 Hz (`ClockCard.cpp:64-82`). MAME
      `upd1990a.cpp:67-68` alloue `m_timer_clock` à 1 Hz via XTAL/32768
      et avance via `advance_seconds()`. POM2 `std::time()` à chaque
      STB → casse déterminisme snapshot, et bloquerait TIME_SET si
      implémenté.
- [ ] 🟢 Slot ROM essentiellement vide (`ClockCard.cpp:134-155`, 256 B
      avec signature + NOPs). MAME `a2thunderclock.cpp:31-34` ship le
      vrai `thunderclock plus rom.bin` (2 KB) avec driver RTS-able
      utilisé par DOS 3.3 patches et Applesoft (via `read_c800`).

**Faux positif retiré** : "Pinout passe par un 6522 VIA". Le vrai
ThunderClock+ MAME (`a2thunderclock.cpp:71-75`) instancie **seulement**
un `UPD1990A`, pas de VIA. Bit layout POM2 match MAME exactly
(`:119-139`).

### 14.9 Speaker + Cassette (`SpeakerDevice.cpp`, `CassetteDevice.cpp`)

MAME refs `src/devices/sound/spkrdev.cpp` + `src/devices/imagedev/cassette.cpp`.
**3 faux positifs purgés** (setSampleRate non câblé, $C028=ALTCHAR,
DC blocker non-MAME).

- [ ] 🟠 Reconstruction square-wave naïve (`SpeakerDevice.cpp:136-139`).
      MAME `spkrdev.cpp:163-197` (`sound_stream_update`) +
      `update_interm_samples` (`:241-259`) + `finalize_interm_sample`
      (`:287-297`) implémente **rectangle-area integration** via
      attotime fractions, puis **4× oversampling + 64-tap windowed
      sinc anti-alias** (`get_filtered_volume :313-327`, `RATE_MULTIPLIER=4`,
      `FILTER_LENGTH=64`). POM2's snap+1-pole LP alias sur >sr/4
      (Karateka 4-5 kHz click sequences).
- [ ] 🟡 Cassette seuil `±0.02` pré-extrait au load
      (`CassetteDevice.cpp:723-749`) vs MAME runtime `> 0.0`
      (`apple2.cpp:362` : `(m_cassette->input() > 0.0 ? 0 : 0x80)`).
      Rejette les rips faible volume.
- [ ] 🟡 Auto-rewind 500 ms si pas de poll `$C060` (`CassetteDevice.cpp:461-465`).
      MAME ne rewind jamais (`cassette.cpp` `m_position` advance
      monotonique). Casse loaders custom polling sporadique.
- [ ] 🟡 LP 5 kHz hard-coded (`SpeakerDevice.h:74`). MAME utilise un
      **sinc 64-tap interne** à spkrdev (`spkrdev.cpp:109-133`,
      cutoff ≈ sr/4), pas un external FILTER_RC. Filtres
      qualitativement différents.
- [ ] 🟢 Timestamp `cycleCounter + getCurrentInstructionCycles()`
      (`Memory.cpp:538-540`) : `M6502.h:99-101` documente
      end-of-opcode (≤7 cycles, ~7 µs, sous-résolution audio à 44 kHz).
      Intentionnel et borné, pas un bug. MAME bus-cycle-exact via
      attotime mais sans impact audible.

**Faux positifs retirés** :
- "setSampleRate() jamais câblé → drift 8.8% sur 48 kHz" : câblé à
  `EmulationController.cpp:29` (`spk->setSampleRate(audioDev->getActualSampleRate())`
  avant `addSource`). Pas de drift.
- "$C028 = ALTCHAR write" : ALTCHAR est sur `$C00E/$C00F` (MAME
  `apple2e.cpp:2168-2172` et POM2 `Memory.cpp:610`). `$C028` sur IIe
  est bien cassette (MAME `apple2e.cpp:1682-1708` : "all models with
  a tape interface will respond to any of the $c02x addresses").
- "DC blocker non-MAME" : MAME `spkrdev.cpp:280-285` a le **strictement
  identique** filtre 1-pôle 0.995 (`filtered_volume = tempx - m_prevx
  + 0.995 * m_prevy`). POM2 reproduit exactement.

### 14.10 Soft switches + Paddle (`Memory.cpp`, `JoystickInput.cpp`)

MAME refs `src/mame/apple/apple2.cpp` + `apple2e.cpp`. **1 faux
positif purgé** (paddle constant 11→12), 1 reframé (STATEREG IIgs-only).

- [ ] 🟠 Language Card prewrite latch armé sur **écritures** aussi
      (`Memory.cpp:713-734`). MAME `ramcard16k.cpp:61-64` (II/II+) +
      `apple2e.cpp:1515-1520` (IIe) : "any write disables pre-write".
      Le `lcPrewrite = writeCandidate` ligne 728-729 fire depuis
      read (`:842`) ET write (`:890`). Fix : passer `isWrite` ;
      sur write, force `lcPrewrite = false`.
- [ ] 🟠 `paddleLatchCycle` init à 0 + `paddleValue` à 128
      (`Memory.h:267-269`). MAME `apple2.cpp:259` init
      `m_joystick_x1_time = 0` — timer **déjà expiré** au boot
      indépendamment de la valeur paddle (qui sera ce que l'host pousse).
      Au boot POM2 : 1408 cycles de "hold" parasite, casse auto-test
      //e. Fix : init `paddleLatchCycle` tel que le test elapsed soit
      au-dessus du threshold dès le boot (pas init de `paddleValue`).
- [ ] 🟠 PB3 / Shift-Key Mod manquant sur IIe `$C063`
      (`Memory.cpp:567`). MAME `apple2e.cpp:1908-1913` :
      `(sw2_r() || (shift_key_mod && (kbspecial & 0x06)==0) ? 0x80 : 0)
      | uFloatingBus7`. Idem manquant : `$C061` OR Open Apple
      (`kbspecial & 0x10`), `$C062` OR Solid Apple (`kbspecial & 0x20`).
- [ ] 🟠 IIe status reads `$C011-$C01F` doivent OR `m_transchar` dans
      les bits bas (`Memory.cpp:625-649`). Cf. §14.4 — même bug.
- [ ] 🟡 `$C000` non mirroré `$C001-$C00F` comme keyboard latch en mode
      II+ (`Memory.cpp:425`). MAME `apple2.cpp:548` `.mirror(0xf)`.
- [ ] 🟡 `$C010` bit 7 devrait refléter any-key-down en mode IIe
      (`Memory.cpp:429` retourne `kbLatch & 0x7F`). MAME `apple2e.cpp:1833` :
      `m_transchar | (m_anykeydown ? 0x80 : 0x00)`.
- [ ] 🟡 `$C068-$C06F` décode manquant — sur II+ ET IIe ils **mirror
      $C060-$C067** (MAME `apple2.cpp:554` `.mirror(0x8)` ;
      `apple2e.cpp:1889/1903/1909/1915/1919/1923/1927`). STATEREG et
      band-select sont IIgs-only, pas IIe.
- [ ] 🟡 `$C070` PTRIG ne retourne pas floating bus et n'alias pas
      `$C070-$C07F` (`Memory.cpp:582-585`). MAME `apple2.cpp:555`
      `map(0xc070, 0xc070).mirror(0xf)`. MAME `:395-411` aussi : 558
      monostable, strobe ignored if timer still running.
- [ ] 🟢 Floating-bus low 7 bits sur `$C061-$C067` (paddles + buttons).
      Cf. §14.4 — POM2 retourne `0x80`/`0x00` purs, MAME OR `uFloatingBus7`.

**Faux positifs retirés** :
- "Paddle RC constant 11 → 12" : MAME `apple2.cpp:247-248` utilise
  `attotime::from_nsec(10800)` = **11.045 cycles** à 1.022727 MHz.
  POM2's `* 11` est à 0.4% — pas 11.34/11.617. **Le fix `* 12`
  rendrait moins précis** ; si on veut affiner, utiliser
  `* 11045 / 1000`.
- "$C068-$C06F = STATEREG / band-select" : IIgs-only, pas IIe.
  POM2 a quand même un vrai bug : pas de mirror $C060-$C067.

### 14.11 Actions prioritaires (révisées 2026-05-12)

**Quick wins** (1-5 lignes, débloquent des cas concrets) :

1. **Month nibble ClockCard** : `shiftReg[4] = (month << 4) | (dow &
   0x0F)` ; corriger `clock_card_smoke_test.cpp:124` (attendu `0x56`).
   §14.8
2. **LC prewrite gate sur `!isWrite`** : passer `isWrite` à
   `languageCardSwitchAccess`, sur write `lcPrewrite = false`. §14.10
3. **CPU D flag clear sur IRQ/BRK/NMI CMOS** : ajouter `& ~F_D` dans
   `handleIRQ`, `handleNMI`, `BRK`. §14.1
4. **CPU PLP/RTI force U=1 ET B=1** : OR popped status avec `0x30`. §14.1
5. **`paddleLatchCycle` init pour que le test soit "expiré" au boot**
   (pas changer `paddleValue` 128). §14.10
6. **IRQ/NMI priorité** dans `step()` : check NMI **avant** IRQ. §14.1

**Mid-effort, high-value** :

7. **SSC : câbler IRQ** (copier pattern Mockingboard) +
   **clear `m_irq_state` sur status read** + **`cmdReg &= ~0x1F`
   sur status write** + **retirer `& 0x7F` en TX** + **mirror A0-A1
   only**. Débloque XMODEM/Kermit/ADTPro 8-bit binaire et tous les
   drivers IRQ-driven. §14.7
8. **CPU cycle counts CMOS** : table à corriger (JMP indirect+6 cycles
   avec fix page-wrap, BIT #imm=2, STZ zp,X=4, RMB/SMB/BBR/BBS=5,
   BRK=7, RTI=6). §14.1
9. **WAI à PC+1 sur IRQ masqué** : ne pas decrement PC, consommer
   des cycles, fall through sur IRQ même si I=1. §14.1
10. **RMW $C0xx triple dispatch** : MAME fait read + 2 writes ; POM2
    fait read + 1 write. Affecte speaker, paddle reset, etc. §14.1
11. **IIe status reads OR `m_transchar`** dans bits 0-6 (`$C011-$C01F`).
    Transverse §14.4 + §14.10.
12. **PB3 Shift-Key Mod + Open/Solid Apple** sur `$C061-$C063` en IIe.
    §14.10
13. **Aux RAM dans renderHiRes/renderText/renderLoRes** en
    `80STORE+PAGE2+HIRES`. Caveat : MAME `hgr_update:766` partage
    cette limitation, donc divergence avec le HARDWARE, pas MAME.
    §14.5

**Heavy lifts** (refonte ciblée) :

14. **Quarter-tracks WOZ** — étendre `bitStream[35]` → `[160]`,
    threader `headQuarterTrack[d]` (sans /4) à travers `DiskImage` +
    5 sites dans `DiskIICard`. Débloque Spiradisc / RWTS18 /
    Locksmith. §14.3
15. **FLUX chunk parser** (WOZ2 v2.1+). Débloque Wings of Fury orig,
    Captain Goodnight, Ankh. §14.3
16. **CRC32 validation** WOZ (`crc32r`, reject sur mismatch). §14.3
17. **AY envelope state machine 4-flag** (attack/hold/alternate/holding)
    pour shapes 10/12/14 corrects. §14.6
18. **Sinc 64-tap + 4× oversampling speaker** (port MAME
    `spkrdev.cpp:163-327`). Real MAME-grade audio. §14.9
19. **Composite color mode 1/2** (Video-7 RGB color-bias, hard
    4-bit) — déjà 50% (mode 0 OK). §14.5
20. **VIA T2 timer Mockingboard** — débloque Ultima IV speech
    driver. §14.6

**Faux positifs purgés de la passe 1** (à ne PAS appliquer) :
- `setSampleRate()` speaker (était déjà câblé)
- `ifr &= ~IFR_T1` Mockingboard T1LH (MAME fait pareil)
- Paddle constant 11 → 12 (MAME utilise 11.045)
- PULSE width 1 cycle Disk II (les deux pareil)
- Motor spin-up Disk II (MAME ne delay pas flux non plus)
- Phase magnets re-emit Disk II (MAME ne re-émet pas non plus)
- Flush threshold 30 vs 31 Disk II (les deux sont 30)
- Ctrl-Reset preserve ALTZP/80COL/etc. (MAME clear tout)
- VBL IRQ jamais asserté (VBL IRQ IIc-only dans MAME aussi)
- $C028 = ALTCHAR (ALTCHAR est en $C00E/F)
- DC blocker non-MAME (MAME a le même 0.995)
- ThunderClock+ pinout via 6522 VIA (pas de VIA dans MAME)
- WOZ `INFO.synchronized/cleaned`, `bytes_used` (MAME les ignore aussi
  au load)
- NMI level-triggered (POM2 est edge-triggered comme MAME)
