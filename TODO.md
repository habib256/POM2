# POM2 — TODO

Ce qui **manque dans POM1** (émulateur Apple-1 + cartes) et qu'il faut donc
bâtir spécifiquement pour avoir un Apple II / II+ précis dans POM2. POM1 ne
contient aucun équivalent natif pour ces blocs — son code peut servir de
socle (M6502, miniaudio, pattern PeripheralBus, LUT NTSC HGR de la GEN2)
mais pas de portage direct.

Légende : ✓ = déjà fait dans POM2, ❌ = à écrire, ~ = partiel.

## Infrastructure portée depuis POM1 (non Apple II spécifique)

- [x] **M6502** — porté + Klaus Dormann functional test passe en 0.46 s
      (`tests/klaus_6502_functional`, SHA-256 pinned, gate de non-régression)
- [x] **AudioDevice** + miniaudio + stb_vorbis vendoring
- [x] **CassetteDevice** (ACI POM1 → cassette Apple II native)
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

POM1 ne décode rien dans cette zone (Apple-1 utilise `$D010-$D012` pour PIA,
et `$C000/$C081` pour ACI). Le squelette est dans `Memory.cpp`, à compléter :

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

POM1 n'a aucun rendu Apple II natif. Sa `Screen_ImGui` rend 40×24 caractères
Apple-1 (1 ligne ASCII glissante), pas la trame entrelacée Apple II.

- [x] Texte 40×24 entrelacé Woz `addr = base + 0x80*(y%8) + 0x28*(y/8)`
- [x] **Attributs caractère** Apple II : bits 7-6 = normal/inverse, low
      6 bits = ASCII index (cf. `resolveGlyph` dans `Apple2Display.cpp`).
      Le clignotement 2 Hz reste à animer (rendu actuel = inverse statique).
- [x] **Lo-res 40×48 / 16 couleurs** : implémenté, palette //gs-corrected,
      partage entrelacé avec le texte (`renderLoRes`)
- [x] **Hi-res 280×192 + couleur NTSC** : pipeline 3 passes portée de la
      GEN2 d'Uncle Bernie :
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

POM1 a CFFA1 (ATA/IDE) et microSD (VIA 65C22) — rien à voir avec un Disk II.
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

POM1 livre `WozMonitor.rom` (256 B) + `basic.rom` (Integer BASIC **Apple-1**)
— binaires **différents** de l'Apple II :

- [x] Slot `roms/apple2.rom` (12 KB ou 16 KB) accepté par `loadAppleIIRom()`
- [x] Slot `roms/apple2_char.rom` accepté
- [ ] Sélecteur runtime II (Integer BASIC) vs II+ (Applesoft + Autostart)
- [ ] Character ROM Apple II : layout normal/inverse/flashing 2 KB
      (≠ `charmap.rom` Apple-1 de POM1)

## 7. Game I/O réel (paddles, joystick, boutons)

Apple-1 n'a pas de paddle ; POM1 n'a rien. Implémenté dans POM2
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

POM1 a JukeBox (paged ROM via `$CA00`) — modèle de banking **incompatible**.

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

POM1 a un mode "OOR strict" pour Apple-1 sans carte RAM. Apple II/II+ :
présentations 4K / 8K / 16K / 24K / 32K / 36K / 48K via les *strapping
blocks*.

- [ ] À modéliser si on veut représenter la machine de 1977 d'origine
      (POM2 colle 48 K en dur aujourd'hui)

## 12. Cartes vidéo RVB tierces (Le Chat Mauve, Video-7 AppleColor)

POM1 n'a rien (pas de carte vidéo). Implémenté dans POM2 (`LeChatMauveCard`
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

- [ ] **Mockingboard slot 4 (AY-3-8910 × 2 + 6522 VIA)** 🆕
      Carte son Apple II classique (POM1 n'a aucun générateur 8910 —
      à écrire from scratch). MAME : `bus/a2bus/mockingboard.cpp`.
      Débloque : musique Karateka intro, SFX Joust / Robotron /
      Ms. Pac-Man, démos sonores. Effort : large (300-500 L —
      VIA + AY-3-8910 PSG complet ; AudioSource pour mix).

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

## Récapitulatif "II/II+ correct" — réutilisable POM1 ?

| Bloc                                         | Statut POM2                                              |
|----------------------------------------------|----------------------------------------------------------|
| Soft switches `$C000-$C07F`                  | ✓ (KBD, speaker, vidéo, cassette, paddles RC, boutons)   |
| Texte 40×24 entrelacé + inverse/flashing     | ✓ (clignotement 2 Hz à animer)                            |
| Lo-res 16 couleurs                           | ✓ (palette //gs-corrected)                                |
| Hi-res NTSC + bit 7 palette + page 2 + mixed | ✓ porté GEN2 + seams + glow toggleable                    |
| Hi-res mono White / Green (P31) / Amber      | ✓ tints phosphore                                         |
| Slots `$C0nX` / `$CnXX` / `$C800` / `$CFFF`  | ✓ porté (`SlotBus` + `SlotPeripheral`)                    |
| Disk II                                      | ✓ slot 6, `.dsk`/`.do` DOS 3.3, read-only                 |
| Speaker 1-bit (synthèse)                     | ✓ porté + LP/DC + UI vol/mute                             |
| ROMs II / II+ + char ROM                     | ~ (Applesoft OK ; Integer + char layout 2 KB à venir)     |
| Game I/O (RC discharge)                      | ✓ paddles + buttons + joystick GLFW host                  |
| Cassette `$C020/$C060`                       | ✓ porté (CassetteDevice, deck UI, FA icons)               |
| Snapshot CPU+RAM+display                     | ✓ (`POM2SNAP`, Disk II exclu)                             |
| Carte RVB Le Chat Mauve (slot 7)             | ✓ HGR RGB ; DHGR + EVE Color TEXT en attente RAM aux      |
| Language Card                                | ✓ 16 KB LC, `$C080-$C08F`, prewrite latch                |
| Strapping RAM 4 K → 48 K                     | ❌                                                        |
| VBL `$C019` / annunciators `$C058-$C05F`     | ❌                                                        |

Le reste (M6502, Disassembler, MemoryViewer, shell ImGui, snapshot,
miniaudio) est déjà porté.

Les blocs encore manquants pour un II+ "complet" : VBL `$C019`,
persistance des écritures Disk II, formats `.po`/`.nib`/`.woz`, sélecteur
preset Integer BASIC, character ROM 2 KB (normal/inverse/flashing), RAM
auxiliaire (pré-requis DHGR + Color TEXT EVE pour Le Chat Mauve).
