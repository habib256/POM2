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
- [ ] Game I/O `$C061-$C067` + latch `$C070` : la décharge RC en cycles
      n'est pas modélisée (signature présente, logique de discharge absente)
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
- [ ] **Aucune carte n'est encore branchée** — l'infra est prête, la
      première carte (Disk II en slot 6) reste à écrire

## 4. Disk II

POM1 a CFFA1 (ATA/IDE) et microSD (VIA 65C22) — rien à voir avec un Disk II.
À écrire intégralement :

- [ ] ROM de boot Disk II (256 B au slot 6 typiquement, le P5/P6 Woz)
- [ ] Séquenceur d'état P5/P6 (16 états × 8 entrées)
- [ ] Encodage 6-and-2 nibble
- [ ] Phases moteur pas-à-pas `$C0n0-$C0n7`
- [ ] Sélection drive 1/2, motor on/off, head load
- [ ] Loaders `.dsk` / `.do` / `.po` / `.nib` / `.woz`

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

Apple-1 n'a pas de paddle ; POM1 n'a rien.

- [ ] Modèle de décharge RC : le bit 7 de `$C064-$C067` reste à 1 pendant
      `~11 µs × valeur_paddle` après écriture sur `$C070`, puis tombe à 0
- [ ] Mapping souris/joystick host vers axes paddle 0/1
- [ ] Boutons `$C061-$C063` (touches pomme ouverte/fermée sur II+ via
      `$C061/$C062`)

## 8. Carte Language (Apple II+ → 64 K)

POM1 a JukeBox (paged ROM via `$CA00`) — modèle de banking **incompatible**.

- [ ] 16 KB RAM bank-switched derrière la ROM `$D000-$FFFF`
- [ ] Soft switches `$C080-$C08F` (read/write enable, BSR, prewrite latch)
- [ ] Banque 1 vs banque 2 sur `$D000-$DFFF`
- [ ] Indispensable pour ProDOS, Pascal, beaucoup de logiciels post-1980

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

## 11. Hors scope

- 80 colonnes / //e auxiliary memory : hors II/II+ d'origine
- Apple IIGS, ProDOS 16 : hors scope

---

## Récapitulatif "II/II+ correct" — réutilisable POM1 ?

| Bloc                                         | Réutilisable POM1 ?                          |
|----------------------------------------------|----------------------------------------------|
| Soft switches `$C000-$C07F`                  | ❌                                            |
| Texte 40×24 entrelacé + inverse/flashing     | ✓ (clignotement 2 Hz à animer)                |
| Lo-res 16 couleurs                           | ✓ (palette //gs-corrected)                    |
| Hi-res NTSC + bit 7 palette + page 2 + mixed | ✓ porté GEN2 + seams + glow toggleable        |
| Slots `$C0nX` / `$CnXX` / `$C800` / `$CFFF`  | ✓ porté (`SlotBus` + `SlotPeripheral`)        |
| Disk II                                      | ❌                                            |
| Speaker 1-bit (synthèse)                     | ✓ porté + LP/DC + UI vol/mute                 |
| ROMs II / II+ + char ROM                     | ❌ (binaires)                                 |
| Game I/O (RC discharge)                      | ❌                                            |
| Cassette `$C020/$C060`                       | ✓ porté (CassetteDevice, deck UI, FA icons)   |
| Language Card                                | ❌                                            |
| Strapping RAM 4 K → 48 K                     | ❌                                            |

Le reste (M6502, Disassembler, MemoryViewer, shell ImGui, snapshot,
miniaudio) est déjà porté ou portable depuis POM1 sans difficulté.
