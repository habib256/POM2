# POM2 — TODO

État 2026-05-16. Backlog ouvert. Sévérité 🟠 high · 🟡 medium · 🟢 low.
Historique des items résolus → `CHANGELOG.md`. Refs MAME → `DEV.md`.

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
