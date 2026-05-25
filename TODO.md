# POM2 — TODO

État 2026-05-24. Backlog **dédupliqué** : chaque item n'apparaît qu'une
seule fois, groupé par sévérité (🟠 high · 🟡 medium · 🟢 low), puis les
buckets non-bug (skips délibérés, propositions, hors scope). Tag
`[sous-système]` par item. Items résolus → `CHANGELOG.md`. Refs MAME →
`DEV.md`.

## Parité MAME ↔ POM2 (dashboard)

Snapshot transversal post-IWM (audit 2026-05-16). Vue d'ensemble par
sous-système ; le détail actionnable vit dans les sections par sévérité
ci-dessous.

| # | Sous-système | Parité | Refs MAME | Écarts / Bugs |
|---|---|---|---|---|
| 1 | M6502 / 65C02 / Rockwell / WDC | Verbatim | `om6502.lst`, `ow65c02.lst` | NMOS undocumented ANC/SBC-imm laissés NOP. Casts C-style + commentaires FR/EN hérités 🟢 |
| 2 | Memory + IIe + RamWorks | Partial-verbatim | `apple2e.cpp:1275-1299`, `a2eramworks3.cpp:108-115` | 🟠 god-object (reste Keyboard/PaddleInputs) ; //c+ extrait en `MemoryProfile` ✅ ; 🟡 `dataMutable()` contourne `writable[]` |
| 3 | Display HGR/DHGR/80-col | Partial-verbatim | `apple2video.cpp:124-201`, `460-471`, `:751-758` | 🟢 mono DHGR 1-px alignment, 🟢 floating-TTL `empty_words`, 🟢 per-scanline DHGR switch ; ChatMauve palette idx 5≢10 (divergence assumée vs AppleWin) |
| 4 | SpeakerDevice | Verbatim | `spkrdev.cpp:74-327` | Aucun écart connu |
| 5 | CassetteDevice | POM2-original | `apple2.cpp:362` (sign-flip) | Auto-rewind 500 ms désormais opt-in default-off ✅ (`3f42efc`) — aucun écart connu |
| 6 | Mockingboard (6522 + AY) | Partial-verbatim | `ay8910.cpp:998-1015`, `:1077-1104`, `1309` | AY tone counter entier ✅ (`3f42efc`) ; 🟢 Port A read mask par DDR absent, 6522 subset (T2/SR/PCR pas modélisés) |
| 7 | FloppySoundDevice | Verbatim | `floppy.cpp:1532-1620`, `:2925-3020` | Aucun écart connu |
| 8 | SlotBus + IRQ wire-OR | POM2-original | Pattern MAME slot bus | Aucun gap fonctionnel |
| 9 | DiskImage (.dsk/.po/.nib/.2mg/.woz) | Partial-verbatim | `woz_dsk.cpp`, `flopimg.cpp:2017-2106` | 🟡 WOZ1 splice TRK+6650 ignoré, 🟢 .nib2/.app non supportés, 🟢 half-tracked NIB (88) absent |
| 10 | DiskIICard (wozfdc + diskiing) | Partial-verbatim | `machine/wozfdc.cpp:264-291`, P6 PROM 341-0028-A | 🟢 sub-instruction inflation RAII vs MAME per-cycle (la LSS s'est avérée NON coupable du bug Mr. Robot — c'était `bootFromSlot` qui sautait l'autostart F8, voir § Medium) ; Disk II hors snapshot délibérément |
| 11 | IWMDevice | Verbatim | `machine/iwm.cpp:1-543` (audit 2026-05-16) | 🟢 Q3 fast clock (Mac/IIgs only) ; `read()` sans side-effects-disabled gate ; window-size round-down vs round-up choices |
| 12 | SmartPortCard (//e Liron) | POM2-original | Spec SmartPort + Apple Tech Note | 32 MB HDV + .2mg variantes pinnés ✅ ; 🟢 multiples partitions ProDOS (CFFA3000 style) absent |
| 13 | SmartPortHub + Sony35Drive | Verbatim | `apple2e.cpp:638-679`, `mac_floppy.cpp`, `flopimg.cpp:512/967/2017-2106` | Aucun gap connu |
| 14 | ClockCard / ThunderClock+ | Partial-verbatim | `upd1990a.cpp:248-267`, `:312-327` | TP tick rates 64/256/2048/4096 Hz + IRQ wiring ✅ (interval timers uPD4990A-serial-only, hors portée) ; 🟡 MODE_SHIFT lax (divergence assumée ProDOS) ; 🟡 DATA_OUT live vs MAME latch ; 🟢 slot ROM mostly NOPs |
| 15 | SuperSerialCard (6551 ACIA) | Partial-verbatim | `mos6551.cpp:46`, `:542-543`, `a2ssc.cpp:373` | LF→CR RX symétrique + raw-mode toggle ✅ (`3f42efc`) ; 🟡 Pascal 1.1 ID block `$CnFB-$CnFF` manquant ; 🟢 IRQ gate SW2:6 DIP non gated |
| 16 | MouseCard | Verbatim | `bus/a2bus/mouse.cpp`, M68705 + MC6821 | ✅ X axis vérifié OK (bits+`updateAxis` = MAME ; A2Desktop X tracke via `/mouse`) ; 🟡 sync curseur pixel-près host/guest delta-based ; 🟢 PIA out_a/b sans `scheduler.synchronize` |

## 🟠 High

- [x] **[MouseCard] Curseur X bloqué à ~8 px Apple — NON REPRODUCTIBLE,
      clos (2026-05-25).** Vérifié sur 4 fronts : parité headless X=Y=800,
      mapping bits + `updateAxis` identiques à MAME, clean-room firmware
      live (X≈Y), et **A2Desktop lui-même** via le nouvel endpoint
      `POST /mouse` (injection +150 identique → X=+134, Y=+95, donc X tracke
      *plus* que Y). L'hypothèse (c) updateAxis était fausse ; le symptôme
      était déjà résorbé (fix `kWidth` de `onMouseMove`) ou propre au clamp
      d'une appli. Détail → `CHANGELOG.md`. Reste ouvert l'item *distinct*
      « sync curseur pixel-près » en 🟡 Medium.
- [ ] **[Memory] god-object** (`Memory.cpp` / `Memory.h`). Reste à
      extraire `Keyboard` (FIFO + strobe + paste) et `PaddleInputs`
      (RC + boutons + Open/Solid Apple). L'`IIcPlusBank` est **fait** :
      toutes les spécificités //c/c+ ($C028 ROMBANK, IWM $C0E0-$C0EF,
      MIG $CC00/$CE00, INTCXROM forcé, alt firmware) sont extraites
      derrière `MemoryProfile` / `IIcClassProfile` (voir `CHANGELOG.md`
      + `DEV.md § MemoryProfile`). Prérequis profil IIgs ; réduit
      recompilations.

## 🟡 Medium

- [ ] **[Disques] WOZ1 splice point (TRK+6650) ignoré**
      (`DiskImage.cpp:381-398`). MAME passe via `set_write_splice` ;
      IWM call site câblé (2026-05-16) mais `DiskImage::setWriteSplice`
      reste un stub. Re-master parité Applesauce. **Effort : 1 j.**
- [ ] **[Disques] UI « Force DOS / Force ProDOS »** — backend prêt
      (`DiskImage::loadFile(path, SectorOrder)` à `DiskImage.cpp:212`),
      reste le bouton dans `DiskLibrary_ImGui` / `DiskController_ImGui`.
      **Effort : ~30 min.**
- [ ] **[SmartPort] ProDOS multi-partition** (feature, pas un test) —
      1 image = 1 unit = 1 volume aujourd'hui. Les images CFFA3000-style
      multi-volume (partition map → plusieurs units ProDOS) ne sont pas
      supportées. La capacité 32 Mo (65536 blocs), l'adressage bloc
      16-bit ($C0D1) et les variantes d'en-tête .2mg (data-offset ≠ 64,
      WP, trailer préservé au write-back) sont désormais **pinnés**
      (`hdv_mass_storage_smoke_test` + `hdv_writeback_smoke_test`).
- [ ] **[UI] MouseCard sync curseur pixel-près** non résolue. Tracking
      delta-based dérive en absolu. Investigation : MGTK `mouse_state:`
      en aux RAM (offset non identifié) ; Pearson scan main+aux 44
      waypoints sans candidat `|r|>0.7`. Reprise : désassembler
      A2Desktop v1.5 ou hook memory-write X/Y-corrélé.
- [ ] **[UI] LED de statut colorée par slot card** (vert / jaune WP /
      rouge erreur) en tête de chaque panel. Lisibilité immédiate.
- [ ] **[SSC] Pascal 1.1 ID block manquant** `$CnFB-$CnFF` (signature
      slot ROM).
- [ ] **[Features] PADL(2)/PADL(3) binding host** (second stick, centrés
      127, `JoystickInput.cpp:65-75`).
- [ ] **[Features] Mapping souris → paddles** : paddle 0/1 sur axe X/Y
      de la souris host (alternative aux pads).
- [ ] **[Features] Eve Color text mode (`$C0B9`)** — variante
      ChatMauve/Eve, attributs FG/BG par caractère. Stub
      `LeChatMauve_ImGui.cpp:200`.
- [ ] **[Arch] `Memory::dataMutable()` contourne `writable[]`**
      (`Memory.h:135,258`). Remplacer par `DebugWriteScope` RAII ou
      `writeRamUnchecked` avec assert `addr < 0xC000`. Évite écritures
      ROM silencieuses depuis debugger / snapshot. **Effort : 2 h.**
- [ ] **[Arch] Config éclatée** : env vars `POM2_*` + CLI flags +
      `Settings`. Centraliser dans un `Config` (env → CLI → Settings →
      defaults) et lister les env vars dans `--help`. **Effort : 1 j.**
- [ ] **[Arch] stateMutex partagé CPU+UI** (`EmulationController.h:118`).
      MainWindow_Slots prend ce lock pendant plug/unplug — risque
      jitter audio. Partitionner long terme.
- [ ] **[Arch] Namespace `pom2::` incohérent** (54/105 fichiers
      top-level ; tests/ ne l'utilisent pas). Migrer mécaniquement en
      une passe.
- [ ] **[Arch] `Memory::memRead` hot path** (cascade `if` 7 niveaux,
      `Memory.cpp:1309-1437`). Table dispatch 256 entrées par page
      haute. Prérequis : extraction `IIcPlusBank`.

## 🟢 Low

- [x] **[UI] Slot Config hérité vs Slot Manager — fusion** (2026-05-25).
      Résolu en **fusionnant** les deux : le `Slot Manager` autonome a été
      supprimé (`SlotManager_ImGui.*` retiré) et sa zone médias intégrée
      dans la colonne droite de `Slot Configuration` (built-ins grisés à
      gauche, disques internes + ports montables à droite). Reste un écart
      mineur : `renderSlotConfigPanel`'s `isDuplicate` marque encore
      cffa/smartport35 en double dans la colonne d'assignation (seul
      `diskii` est multi-instance là) — acceptable, l'assignation reste
      single-instance pour ces cartes par défaut.
- [ ] **[Floppy Emu] modes Dual-5.25" + Smartport-Unit-2**. Le modèle
      `FloppyEmuDevice` couvre 4 modes (5.25 / 3.5 / Unidisk 3.5 /
      Smartport HD) ; les modes « Dual 5.25 » (2 lecteurs sur un
      contrôleur) et « Smartport Unit 2 » (astuce de boot daisy-chain
      IIgs) du vrai device sont hors scope v1. À reprendre si un cas
      d'usage concret apparaît.
- [ ] **[UI] Layout par défaut plus aéré** : ImGui Docking ou
      `SetNextWindowPos` cascade adaptative selon nombre de cartes.
- [ ] **[Audio] AY Port A read mask par DDR** (`Mockingboard.cpp`,
      R14/R15 « unused, academic »). Sans impact concret.
- [ ] **[SSC] IRQ gate SW2:6 DIP** non implémenté (MAME `a2ssc.cpp:373`).
- [ ] **[ClockCard] Slot ROM vide** (256 B signature + NOPs). Vrai
      ThunderClock+ = 2 KB driver RTS-able pour DOS 3.3 / Applesoft.
- [ ] **[DHGR] `monochrome_dhr_shift()` 1-px alignment** absent en mono
      (MAME `apple2video.cpp:460-471`). Cosmétique.
- [ ] **[DHGR] Floating-TTL** `empty_words[40]` non modélisé pour rows
      hors bounds (MAME `:751-758`). Jamais atteint en 48 K+.
- [ ] **[DHGR] per-scanline mode switching** — bottom-of-mixed utilise
      région 4-line statique.
- [ ] **[Features] Le Chat Mauve EVE** (64 KB ext RAM + modes SPEC1/
      SPEC2/DASH/COL280), **Video-7 AppleColor RGB**, **Color killer
      Rev 1**, **Strapping RAM 4K→48K**.
- [ ] **[Disques] Half-tracked NIB (88 tracks)**, **Applesauce
      `.nib2`/`.app`**, **Disk II dans snapshot** — délibérément hors
      scope tant que WOZ couvre.
- [ ] **[Arch] M6502 style hérité** : commentaires FR/EN, casts C-style,
      signatures `void(void)`. `clang-format` + `clang-tidy
      modernize-*` ciblé.
- [ ] **[Arch] `*Card` raw pointers dans MainWindow**
      (`MainWindow.h:97-103`). Pas de notification quand SlotBus replug.
      Observer pattern ou `controller.slotBus().peripheral(N)`.

## Tests de pin dus

- [ ] Smoke tests pour les quick-wins audio/IO résolus dans `3f42efc`
      (convention « MAME source of truth ») : **[Mockingboard]** AY
      tone counter entier, **[SSC]** LF→CR + raw-mode, **[Cassette]**
      auto-rewind opt-in.

## Skips délibérés (won't-fix, documenté inline)

- 🟢 **[Memory] `$C040` STRB pas gated `!//c`** (MAME `apple2e.cpp:1927`).
  Aucun sink wired → 0 effet observable.
- 🟢 **[ClockCard] DATA_OUT live** vs MAME latch sur CLK edge en
  MODE_SHIFT (`ClockCard.cpp:193-200`). Gating strict casserait les
  reads ProDOS stock — hack délibéré, documenté inline.
- 🟢 **[MouseCard] PIA out_a/b sans `scheduler.synchronize`** (MAME
  `mouse.cpp:280-294`). Pas de race firmware-visible observée.
- 🟢 **[ClockCard] offset model vs MAME `set_time`** — équivalent
  comportementalement tant que `timeFn()` avance en lock-step.
- 🔁 **[MAME] path drift refresher** tous les ~6 mois pour suivre les
  renommages upstream (récent : `wozfdc.cpp` `bus/a2bus → machine`).

## Propositions / extensions

### SmartPort
- 🟢 **UDC (Apple 1991)** — 4 baies hétérogènes (3.5"/5.25"/HDV). 99 %
  des configs réelles utilisaient Liron + Disk II en 2 slots.
- 🟢 **Slinky / RamFAST RAM disk** — trivial sur le papier mais utilité
  limitée vs RamWorks III.
- 🟢 **Apple II SCSI Card** (670-0144) — ProDOSHardDiskCard couvre déjà
  le besoin fonctionnel.
- 🟢 **Apple 3.5" Controller IWM-level** — refactor IWMDevice pour vivre
  attaché à un slot card (rare ; ex : « Mr. Robot » .woz Sony).

### Cartes de stockage MAME-fidèles (2026-05-22)

Complément « vraie carte » au modèle synthétique (lignée AppleWin)
documenté `DEV.md § ProDOSHardDiskCard (HDV)` + `§ SmartPortCard`.
Objectif : exécuter le **firmware réel** des cartes du commerce
par-dessus des **puces de bus émulées**, comme MAME — fidélité
matérielle plutôt que suffisance fonctionnelle. Revisite le « Hors
scope » CFFA et les lignes SCSI / Apple 3.5 IWM-level ci-dessus :
ces items les remplaceraient si la fidélité prime sur la simplicité.

- [x] 🟡 **P1 — Cœur ATA/IDE + carte CFFA** — **FAIT (2026-05-24)**. MAME
      `bus/a2bus/a2cffa.cpp` porté.
      1. ✅ `AtaBlockDevice` — taskfile (IDENTIFY/READ/WRITE SECTOR(S),
         LBA28) sur `Block512Backing` (backing partagé extrait de
         `ProDOSHardDiskCard`). Réutilisable CFFA + Vulcan + Zip + Focus.
         Pin : `ata_block_device_test`.
      2. ✅ `CffaCard : SlotPeripheral, ProDOSBlockCard` — ROM réel
         (`cffa20ee02.bin` / `cffa20eec02.bin`), `$C0nX` → ATA (latch
         8↔16-bit), `$CnXX`/`$C800` ROM ; cité vs `a2cffa.cpp`.
      3. ✅ `.hdv` / `.2mg` LBA brut conservé ; **CHD = phase 2**.
      4. ✅ Intégration GUI : type de slot « cffa » (gaté ROM),
         persistance `cffa_slotN_*`, réutilise la HDV Library via
         `ProDOSBlockCard` / `hdvDevice()`. CFFA `$Cn07=$3C` ⇒ boot F8
         natif. Pin : `cffa_card_smoke` (ROM-gated, décode via Memory).
      - **Reste** : oracle MAME boot `.2mg` réel
        (`mame apple2ee -sl7 cffa2`, romset prêt `~/mame_roms/cffa2/`) ;
        CHD ; préservation média CFFA au switch de profil.

- [ ] 🟡 **P2 — Carte Liron / UniDisk 3.5 réelle (IWM en slot)**.
      Remplace le driver bloc synthétique de `SmartPortCard` par le
      vrai chemin GCR/IWM en slot //e. Le gros du stack existe déjà :
      `IWMDevice` (verbatim MAME `iwm.cpp`), `Sony35Drive` + GCR zoné,
      `SmartPortHub`. Reste : `LironCard : SlotPeripheral` câblant ce
      stack à un slot + ROM Liron (343S0001).
      **Bloqueur** : aucun dump ROM Liron public — MAME `a2iwm.cpp`
      note « WANTED: there are no ROM dumps from this card ». Sans ROM,
      rester synthétique. Recoupe « Apple 3.5 Controller IWM-level »
      ci-dessus. **Effort : ~8-12 h** hors sourcing ROM.

- [ ] 🟢 **P3 — Apple II SCSI / High-Speed SCSI + CHD**. MAME
      `a2scsi.cpp` (NCR 5380) / `a2hsscsi.cpp` (53C80). Émuler la puce
      NCR + protocole SCSI + lecteur CHD : gros lift pour besoin niche
      (ProDOSHardDiskCard couvre déjà le fonctionnel). **Effort :
      ~30-50 h.** Garder 🟢 sauf demande explicite CHD.

Ordre conseillé : **P1** (compat `.2mg` conservée + carte la plus
répandue, débloque le vrai firmware) → **P2** si un dump Liron
apparaît → **P3** seulement si CHD requis.

## Priorité conseillée (ROI)

| Rang | Item | Effort | Impact | Pour qui |
|---|---|---|---|---|
| 1 | [Memory] god-object split | 2 jours | 🟠 Prépare IIgs + reduces recompile | Dev velocity |
| 2 | [Disques] WOZ1 splice point | 1 jour | 🟡 Applesauce remaster parity | Disk imaging tools |
| 3 | [Disques] UI Force DOS/ProDOS | ~30 min | 🟡 quick win, backend prêt | Disk wranglers |

## Hors scope

- IIgs / ProDOS 16 · Z-80 SoftCard CP/M · CFFA CompactFlash (HDV + host
  folder suffit ; portage MAME-fidèle proposé → § Cartes de stockage
  MAME-fidèles).

## Changelog

Voir [`CHANGELOG.md`](CHANGELOG.md).
