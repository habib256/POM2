# POM2 — TODO

État 2026-05-16. Backlog ouvert. Sévérité 🟠 high · 🟡 medium · 🟢 low.
Historique des items résolus → `CHANGELOG.md`. Refs MAME → `DEV.md`.

## Audit MAME ↔ POM2 (2026-05-16)

Audit transversal post-IWM. Tableau, bugs concrets par ROI,
améliorations architecturales, classement ROI.

### Tableau récapitulatif de parité

| # | Sous-système | Parité | Refs MAME | Écarts / Bugs |
|---|---|---|---|---|
| 1 | M6502 / 65C02 / Rockwell / WDC | Verbatim | `om6502.lst`, `ow65c02.lst` | NMOS undocumented ANC/SBC-imm laissés NOP. Casts C-style + commentaires FR/EN hérités 🟢 |
| 2 | Memory + IIe + RamWorks | Partial-verbatim | `apple2e.cpp:1275-1299`, `a2eramworks3.cpp:108-115` | 🟠 god-object (521+1555 L), 🟠 spécificités //c+ qui fuient dans dispatcher, 🟡 `dataMutable()` contourne `writable[]` |
| 3 | Display HGR/DHGR/80-col | Partial-verbatim | `apple2video.cpp:124-201`, `460-471`, `:751-758` | 🟢 mono DHGR 1-px alignment, 🟢 floating-TTL `empty_words`, 🟢 per-scanline DHGR switch ; ChatMauve palette idx 5≢10 (divergence assumée vs AppleWin) |
| 4 | SpeakerDevice | Verbatim | `spkrdev.cpp:74-327` | Aucun écart connu |
| 5 | CassetteDevice | POM2-original | `apple2.cpp:362` (sign-flip) | 🟡 auto-rewind 500 ms inexistant chez MAME — casse loaders polling sporadique |
| 6 | Mockingboard (6522 + AY) | Partial-verbatim | `ay8910.cpp:1077-1104`, `1309`, `wozfdc.cpp` migration `bus/a2bus → machine` | 🟡 AY float tone counter alias sur périodes 1-3 (MAME entier), 🟢 Port A read mask par DDR absent, 6522 subset (T2/SR/PCR pas modélisés) |
| 7 | FloppySoundDevice | Verbatim | `floppy.cpp:1532-1620`, `:2925-3020` | Aucun écart connu |
| 8 | SlotBus + IRQ wire-OR | POM2-original | Pattern MAME slot bus | Aucun gap fonctionnel |
| 9 | DiskImage (.dsk/.po/.nib/.2mg/.woz) | Partial-verbatim | `woz_dsk.cpp`, `flopimg.cpp:2017-2106` | 🟡 WOZ1 splice TRK+6650 ignoré, 🟢 .nib2/.app non supportés, 🟢 half-tracked NIB (88) absent |
| 10 | DiskIICard (wozfdc + diskiing) | Partial-verbatim | `machine/wozfdc.cpp:264-291`, P6 PROM 341-0028-A | 🟢 sub-instruction inflation RAII vs MAME per-cycle (rare impact protections), Disk II hors snapshot délibérément |
| 11 | IWMDevice | Verbatim | `machine/iwm.cpp:1-543` (audit 2026-05-16) | 🟢 Q3 fast clock (Mac/IIgs only) ; `read()` sans side-effects-disabled gate ; window-size round-down vs round-up choices |
| 12 | SmartPortCard (//e Liron) | POM2-original | Spec SmartPort + Apple Tech Note | 🟡 32 MB ProDOS HDV pas pinné test explicite, 🟢 multiples partitions ProDOS (CFFA3000 style) absent |
| 13 | SmartPortHub + Sony35Drive | Verbatim | `apple2e.cpp:638-679`, `mac_floppy.cpp`, `flopimg.cpp:512/967/2017-2106` | Aucun gap connu |
| 14 | ClockCard / ThunderClock+ | Partial-verbatim | `upd1990a.cpp:248-267`, `:312-327` | 🟠 TP tick rates 64/256/2048/4096 Hz pas câblés ; 🟡 MODE_SHIFT lax (divergence assumée ProDOS) ; 🟡 DATA_OUT live vs MAME latch ; 🟢 slot ROM mostly NOPs |
| 15 | SuperSerialCard (6551 ACIA) | Partial-verbatim | `mos6551.cpp:46`, `:542-543`, `a2ssc.cpp:373` | 🟡 Pascal 1.1 ID block `$CnFB-$CnFF` manquant ; 🟡 telnet IAC strip mange `$FF` ; 🟡 LF→CR keyboard sink only ; 🟢 IRQ gate SW2:6 DIP non gated |
| 16 | MouseCard | Verbatim | `bus/a2bus/mouse.cpp`, M68705 + MC6821 | 🟠 X axis bloqué ~8 px Apple ; 🟡 sync curseur pixel-près host/guest delta-based ; 🟢 PIA out_a/b sans `scheduler.synchronize` |

### Bugs concrets par ROI

**🟠 Bugs sérieux (impact user)**

1. **MouseCard X-axis bloqué ~8 px** — observable via screenshot
   diff, dégrade A2Desktop. Investigation existante :
   `POM2_MOUSE_TRACE=1` + AI Control `/mem?bank=aux`. Hypothèses :
   SetMouse clamp tardif vs MCU idle gate. **Effort : 4-8 h**
   (instrumentation puis fix).
2. **ClockCard TP tick rates** — `SlotPeripheral::assertIrq` exposé,
   il manque les 4 diviseurs (64/256/2048/4096 Hz) + pulse
   rising-edge selon mode courant. **Effort : 2-3 h**, port
   mécanique de `upd1990a.cpp:248-267`. Pinnable par smoke test
   (count IRQs sur N cycles).

**🟡 Bugs moyens (impact niche, fix simple)**

3. **Mockingboard AY tone counter aliasing périodes 1-3** —
   `Mockingboard.cpp:449-456` utilise un float ; MAME utilise un
   compteur entier (`ay8910.cpp:998-1015`). Port verbatim, fix les
   PWM tricks (Cosmic Bouncer style). **Effort : 1-2 h**.
4. **SSC telnet IAC strip mange `$FF` valides** —
   `SuperSerialCard.cpp::swallowTelnetIac` trop agressif. Solution :
   toggle « raw mode » dans le panel SSC (XMODEM/Kermit). **Effort : 1 h**.
5. **SSC LF→CR pas appliqué sur rxBuf** —
   `SuperSerialCard.cpp:187-198`. Symétriser avec le keyboard sink.
   **Effort : 30 min**.
6. **Cassette auto-rewind 500 ms** — POM2-isme qui casse loaders
   polling sporadique. Solution : retirer le rewind ou gate-le
   derrière `cassette_auto_rewind=true` par défaut désactivé.
   **Effort : 1 h**.
7. **ClockCard DATA_OUT live vs MAME latch sur CLK edge en
   MODE_SHIFT** — `ClockCard.cpp:88-94`. Diverge hors ProDOS.
   **Effort : 1-2 h**, demande test pour ne pas casser le hack
   ProDOS shift-laxité.

**🟢 Bugs cosmétiques**

8. **MAME `wozfdc.cpp` path drift refresh** tous les ~6 mois — déjà
   fait, OK.
9. **`$C040` utility STRB pas gated `!//c`** — aucun sink wired, 0
   effet observable.
10. **MouseCard PIA propagation immédiate vs MAME
    `scheduler.synchronize` defer** — pas d'observation race
    firmware-visible.

### Améliorations architecturales

**A. Memory god-object split** (🟠 dette technique majeure).
Extraire 3 classes de `Memory` :
- `Keyboard` (FIFO + strobe + paste queue)
- `PaddleInputs` (RC paddle + boutons + IIe modifier keys
  $C061/$C062)
- `IIcPlusBank` (alt firmware + MIG + iicRomBank dispatch)

Pourquoi : (a) limiter la cascade de recompilations sur touch
`Memory.h`, (b) prépare l'ajout du profil IIgs sans plus de fuites,
(c) `Memory::memRead`/`memWrite` deviendra une table-dispatch propre
par page haute. **Effort : 2 jours**. Pré-requis avant toute
extension de profil future.

**B. Stratégie de profil pour les hooks //c+** (🟠 lié à A).
`iicHasAltBank` / `iicRomBank` / `migRead` testés en plein
dispatcher générique (`Memory.cpp:1338, 1370-1391`). Pattern cible :
`IProfileMemoryStrategy` injecté par `EmulationController`,
dispatcher reste profile-agnostic. À traiter conjointement avec A.

**C. Configuration unifiée** (🟡). 11 env vars `POM2_*` éparpillées
(grep `getenv` = 11 occurrences) + CLI flags + `Settings`. Ajouter
un `Config` central avec précédence env → CLI → Settings →
defaults, lister env vars dans `--help`. **Effort : 1 jour**.

**D. `Memory::dataMutable()` scope contrôlé** (🟡). Remplacer le
handle libre par `Memory::DebugWriteScope` RAII ou par
`writeRamUnchecked(addr, v)` qui asserte `addr < 0xC000`. Évite
écritures ROM silencieuses depuis debugger / snapshot. **Effort : 2 h**.

**E. Suppression progressive du `iie_*` boolean leakage** (🟢).
Cohérence namespace `pom2::` — ~20/89 fichiers seulement. Migration
mécanique en une passe.

**F. Disk II dans snapshot** (🟢). Déjà documenté comme exclu
délibéré. Quand quelqu'un veut y aller : image identity + head
position + per-track dirty bits.

### Classement ROI

| Rang | Item | Effort | Impact | Pour qui |
|---|---|---|---|---|
| 1 | Mockingboard AY tone counter entier | 1-2 h | 🟡 PWM tricks audibles | Audio fidelity |
| 2 | SSC LF→CR rxBuf | 30 min | 🟡 Terminal apps cleaner | SSC users |
| 3 | Cassette auto-rewind gate | 1 h | 🟡 Loaders custom OK | Cassette users |
| 4 | ClockCard TP tick rates | 2-3 h | 🟠 Clockworks utility | ClockCard users (niche) |
| 5 | Memory god-object split | 2 jours | 🟠 Prépare IIgs + reduces recompile | Dev velocity |
| 6 | MouseCard X axis stuck | 4-8 h | 🟠 A2Desktop usability | Mouse Card users |
| 7 | WOZ1 splice point | 1 jour | 🟡 Applesauce remaster parity | Disk imaging tools |

Pack « 3 premiers » : 3.5 h cumulés, 3 bugs 🟡 audibles/visibles
fixés en une session.

## Bugs sérieux

- [ ] 🟠 **MouseCard — curseur X bloqué à ~8 px Apple** alors que Y
      fonctionne sur toute la plage. Bbox screenshot-diff x=[0..5..8]
      même quand l'hôte traverse 600 px. Hypothèses : (a) A2Desktop
      clamp X via `SetMouse` post-init ; (b) firmware MCU idle loop
      ignore motion tant que `RAM $58 bit 0` non set par `ReadMouse`,
      A2Desktop ne pulse pas assez ; (c) bug subtil
      `MouseCard::updateAxis` X (PB0/PB1) vs Y (PB2/PB3). Outils en
      place : `POM2_MOUSE_TRACE=1`, AI Control `/mem?bank=aux`.
- [ ] 🟠 **ClockCard — TP tick rates non câblés** (64/256/2048/4096 Hz
      + interval timers). MAME `upd1990a.cpp:248-267`. La ligne IRQ
      slot-bus est exposée via `SlotPeripheral::assertIrq` ; reste à
      câbler les 4 diviseurs et pulser `assertIrq` au rythme TP.
      Utilitaires interval-timing (Clockworks) ne tickent jamais sans ça.
- [ ] 🟠 **`Memory` god-object** (521 + 1555 L). Extraire `Keyboard`
      (FIFO + strobe + paste), `PaddleInputs` (RC + boutons + Open/
      Solid Apple), `IIcPlusBank` (alt firmware + MIG + iicRomBank).
      Prérequis pour profil IIgs futur ; réduit recompilations.
- [ ] 🟠 **Spécificités //c+ qui fuient dans `Memory::memRead/Write`**
      (`Memory.cpp:1338,1370-1391`). Pattern cible : stratégie de
      profil (`IProfileMemoryStrategy`) injectée. À traiter avec
      `IIcPlusBank`.

## UI / UX

- [ ] 🟡 **MouseCard sync curseur pixel-près** non résolue. Tracking
      delta-based dérive en position absolue. Investigation menée :
      screen-holes lus par MCU mais pas par MGTK ; MGTK `mouse_state:`
      vit en aux RAM à offset `MGTKAuxEntry := $4000 +?` (non
      identifié) ; Pearson scan main+aux RAM sur 44 waypoints n'a
      trouvé aucun candidat `|r|>0.7`. Reprise : disassembler
      A2Desktop v1.5 ou memory-write hook X/Y-corrélé.
- [ ] 🟡 **LED de statut colorée par slot card** (vert / jaune WP /
      rouge erreur) en tête de chaque panel. Lisibilité immédiate.
- [ ] 🟢 **Layout par défaut plus aéré** : ImGui Docking ou
      `SetNextWindowPos` cascade adaptative selon nombre de cartes.

## Disques

- [ ] 🟡 **WOZ1 splice point (TRK+6650) ignoré**
      (`DiskImage.cpp:381-398`). MAME passe via `set_write_splice`.
      IWM call site câblé (2026-05-16) mais `DiskImage::setWriteSplice`
      reste un stub. Re-master parité Applesauce.
- [ ] 🟡 **UI « Force DOS / Force ProDOS »**. `loadFile(path,
      SectorOrder)` existe (`DiskImage.cpp:212`), pas de bouton UI.
- [ ] 🟡 **Mass storage SmartPort 32 MB ProDOS** — pinning explicite
      manquant : (a) boot HDV 32 MB sur //e, (b) compat headers .2mg
      DC42/Universal/CFFA, (c) ProDOS partitions multiples (1 image =
      1 unit = 1 volume aujourd'hui).
- [ ] 🟢 **Half-tracked NIB (88 tracks)**, **Applesauce `.nib2`/`.app`**,
      **Disk II dans snapshot** — délibérément hors scope tant que
      WOZ couvre.

## Audio

- [ ] 🟡 **AY float tone counter alias périodes 1-3**
      (`Mockingboard.cpp:449-456`). MAME utilise compteur entier
      (`ay8910.cpp:998-1015`). Manifeste sur tricks PWM.
- [ ] 🟡 **Cassette auto-rewind 500 ms** si pas de poll `$C060`
      (`CassetteDevice.cpp:461-465`). MAME ne rewind jamais. Casse
      loaders custom polling sporadique.
- [ ] 🟢 **AY Port A read mask par DDR** (`Mockingboard.cpp:126-130`).
      Sans impact concret.

## Super Serial Card

- [ ] 🟡 **Pascal 1.1 ID block manquant** `$CnFB-$CnFF` (slot ROM
      signature).
- [ ] 🟡 **Telnet IAC strip mange `$FF` en RX 8-bit binaire**.
      Toggle « raw mode » dans le panel SSC.
- [ ] 🟡 **LF→CR appliqué keyboard-only**, pas `rxBuf`
      (`SuperSerialCard.cpp:187-198`).
- [ ] 🟢 **IRQ gate SW2:6 DIP** non implémenté (MAME `a2ssc.cpp:373`).

## ClockCard / ThunderClock+

- [ ] 🟡 **DATA_OUT live** (`ClockCard.cpp:88-94`) vs MAME latch sur
      CLK edge en MODE_SHIFT. Diverge hors ProDOS.
- [ ] 🟢 **Slot ROM vide** (256 B signature + NOPs). Vrai
      ThunderClock+ = 2 KB driver RTS-able pour DOS 3.3 / Applesoft.

## DHGR / Display

- [ ] 🟢 **`monochrome_dhr_shift()` 1-px alignment** absent en DHGR mono
      (MAME `apple2video.cpp:460-471`). Cosmétique.
- [ ] 🟢 **Floating-TTL** `empty_words[40]` non modélisé pour rows hors
      bounds (MAME `:751-758`). Jamais atteint en 48 K+.
- [ ] 🟢 **DHGR per-scanline mode switching** — bottom-of-mixed utilise
      région 4-line statique.

## Features manquantes

- [ ] 🟡 **PADL(2)/PADL(3) binding host** (second stick, centrés à 127,
      `JoystickInput.cpp:65-75`).
- [ ] 🟡 **Mapping souris → paddles** : alternative aux pads, paddle
      0/1 sur axe X/Y de la souris host.
- [ ] 🟡 **Eve Color text mode (`$C0B9`)** — variante ChatMauve/Eve,
      attributs FG/BG par caractère. Stub `LeChatMauve_ImGui.cpp:200`.
- [ ] 🟢 **Le Chat Mauve EVE** (64 KB ext RAM + modes SPEC1/SPEC2/
      DASH/COL280), **Video-7 AppleColor RGB**, **Color killer Rev 1**,
      **Strapping RAM 4K→48K**.

## Architecture & qualité

- [ ] 🟡 **`Memory::dataMutable()` contourne `writable[]`**
      (`Memory.h:135,258`). Remplacer par `DebugWriteScope` RAII ou
      `writeRamUnchecked` avec assert `addr < 0xC000`.
- [ ] 🟡 **Config éclatée** : 11 env vars `POM2_*` + CLI flags +
      `Settings`. Centraliser dans un `Config` (env → CLI → Settings
      → defaults) et lister env vars dans `--help`.
- [ ] 🟡 **stateMutex partagé CPU+UI** (`EmulationController.h:118`).
      MainWindow_Slots prend ce lock pendant plug/unplug — risque
      jitter audio. Partitionner long terme.
- [ ] 🟡 **Namespace `pom2::` incohérent** (~20/89 fichiers). Migrer
      mécaniquement en une passe.
- [ ] 🟡 **`Memory::memRead` hot path** (cascade `if` 7 niveaux
      profondeur, `Memory.cpp:1309-1437`). Table dispatch 256 entrées
      par page haute. Prérequis : extraction `IIcPlusBank`.
- [ ] 🟢 **M6502 style hérité** : commentaires FR/EN, casts C-style,
      signatures `void(void)`. `clang-format` + `clang-tidy
      modernize-*` ciblé.
- [ ] 🟢 **`*Card` raw pointers dans MainWindow** (`MainWindow.h:97-103`).
      Pas de notification quand SlotBus replug. Observer pattern ou
      `controller.slotBus().peripheral(N)`.

## SmartPort — extensions possibles

- [ ] 🟢 **UDC (Apple 1991)** — 4 baies hétérogènes (3.5"/5.25"/HDV).
      99 % des configs réelles utilisaient Liron + Disk II en 2 slots.
- [ ] 🟢 **Slinky / RamFAST RAM disk** — trivial sur le papier mais
      utilité limitée vs RamWorks III.
- [ ] 🟢 **Apple II SCSI Card** (670-0144) — ProDOSHardDiskCard couvre
      déjà le besoin fonctionnel.
- [ ] 🟢 **Apple 3.5" Controller IWM-level** — refactor IWMDevice pour
      vivre attaché à un slot card (rare ; ex : « Mr. Robot » .woz Sony).

## MAME parity drifts (skip délibéré)

- [ ] 🟢 **`$C040` STRB pas gated `!//c`** (MAME `apple2e.cpp:1927`).
      Aucun sink wired → 0 effet observable.
- [ ] 🟢 **MouseCard PIA out_a/b sans `scheduler.synchronize`**
      (MAME `mouse.cpp:280-294`). Pas de race firmware-visible observée.
- [ ] 🟢 **ClockCard offset model vs MAME `set_time`**. Équivalent
      comportementalement tant que `timeFn()` avance en lock-step.
- [ ] 🟢 **MAME path drift refresher** tous les ~6 mois pour suivre
      renommages upstream (récent : `wozfdc.cpp` `bus/a2bus → machine`).

## Hors scope

- IIgs / ProDOS 16 · Disk II 13-secteurs (pré-DOS 3.3) · Z-80
  SoftCard CP/M · CFFA CompactFlash (HDV + host folder suffit).

## Changelog

Voir [`CHANGELOG.md`](CHANGELOG.md).
