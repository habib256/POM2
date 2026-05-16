# POM2 — TODO

État au 2026-05-16. Backlog ouvert. Orientation → `CLAUDE.md` ;
notes d'implémentation profondes → `DEV.md` ; historique → `git log`.

Légende sévérité : 🟠 High · 🟡 Medium · 🟢 Low. Sections regroupées
par sous-système, triées par poids des items 🟠.

## UI / UX (retours utilisateur 2026-05-15)

L'émulateur fonctionne dans le fond — le user a explicitement remonté
que le polish UI est ce qui reste à faire (le reste est cosmétique).

- [x] ~~🟠 **Toolbar de raccourcis sous la barre de menus**.~~ Fait
      2026-05-15. `Toolbar_ImGui.{h,cpp}` ~200 L. Boutons compacts
      Font Awesome : Power-cycle, Soft reset, Hard reset, Pause/Run
      (icône change), Step (grisé quand running), combo Speed (1×
      / 2× / 4× / MAX), combo Profile (II / II+ / //e / //c / //c+),
      Insert disk (primary), Eject-all (gris si rien chargé),
      Screenshot, Memory viewer (tinted-active toggle). Pin via
      `SetNextWindowPos(0, ImGui::GetFrameHeight())`. Toggle
      `Window → Toolbar` persisté `show_toolbar` (on par défaut).
- [x] ~~🟠 **Panneau « Disk Library » unifié**.~~ Fait 2026-05-15.
      `DiskLibrary_ImGui.{h,cpp}` ~360 L. 3 onglets (5.25 / 3.5 /
      HDV) avec compte d'entrées dans le titre, recherche
      `InputTextWithHint`, tri Name/Size/Date, `BeginTable` 3 colonnes
      (name + size + date), `* ` marker pour les images actuellement
      montées (croise toutes les cartes DiskII plugged + les 2 Disk35
      + HDV). Left-click = insert/mount + boot ; right-click =
      context menu (drive 1/2 pour 3.5", boot/no-boot pour les autres).
      Scan filesystem ré-utilisable via bouton Refresh.
      `Hardware → Devices → Disk Library` toggle persisté
      `show_disk_library`. Câblage des actions dans
      `MainWindow::renderDiskLibraryWindow()` — chaque action passe
      par le path déjà rodé (controller->mount35 + bootFromSlot ou
      coldBoot selon profile/card, diskCard->insertDisk + coldBoot,
      hdvCard->loadImage + bootFromSlot(getSlot())).
- [ ] 🟡 **Mouse Card — sync curseur pixel-près host/guest non
      résolue** (héritée 2026-05-15, déplacée ici pour proximité avec
      le reste UX). Le tracking actuel est delta-based : 1 px hôte →
      1 px Apple en vitesse (ratio `display.width() / widgetW` dans
      `MainWindow::onMouseMove`), avec accumulation sub-pixel et
      clamp résidu à ±127 ticks par event. Fonctionne mais les deux
      curseurs dérivent en position absolue. Reprendre depuis
      l'investigation détaillée dans la section [Mouse Card](#mouse-card)
      ci-dessous (l'enquête sur le data segment MGTK reste valable).
- [ ] 🟡 **Statut de carte plus visible**. Quand un disque manque dans
      un slot, le panel affiche "No disk inserted" ; pas évident à
      repérer en un coup d'œil. Une LED de status colorée (vert =
      OK, jaune = WP/aucun disque, rouge = erreur) en tête de chaque
      panneau de slot rendrait la situation lisible sans lire.
- [ ] 🟢 **Layout par défaut plus aéré**. Avec 2+ Disk II + Disk 3.5"
      + HDV, les fenêtres se superposent au lancement. Soit ImGui
      Docking par défaut, soit `SetNextWindowPos` plus malin
      (cascade adaptative selon le nombre de cartes plugged).

## DHGR / Display

- [ ] 🟢 **`monochrome_dhr_shift()` 1-px alignment manquant** en DHGR
      mono (MAME `apple2video.cpp:460-471`). Cosmétique.
- [ ] 🟢 **Floating-TTL** `empty_words[40]={0x3fff,…}` pour rows
      hors-bounds non modélisé (MAME `:751-758`). Jamais atteint en
      48K+.
- [ ] 🟢 **DHGR per-scanline mode switching** — bottom-of-mixed utilise
      une région 4-line statique.

## ClockCard / ThunderClock+

- [ ] 🟠 **TP tick rates (64/256/2048/4096 Hz + interval timers)** non
      implémentés. MAME `upd1990a.cpp:248-267`. La ligne IRQ slot-bus
      est exposée via `SlotPeripheral::assertIrq(bool)` (cf. CLAUDE.md
      « IRQ aggregation ») ; reste à câbler les 4 diviseurs dans
      `ClockCard` et à pulser `assertIrq` au rythme TP. Utilitaires
      interval-timing (Clockworks) ne tickent jamais sans ça.
- [ ] 🟡 **DATA_OUT live** (`ClockCard.cpp:88-94`) vs MAME latch sur
      CLK edge en MODE_SHIFT. Diverge hors ProDOS.
- [ ] 🟢 **Slot ROM essentiellement vide** (256 B signature + NOPs).
      Vrai ThunderClock+ ROM = 2 KB avec driver RTS-able pour DOS 3.3 /
      Applesoft.

## Super Serial Card

- [ ] 🟡 **Signature slot ROM incomplète** pour GS/OS / Pascal 1.1 :
      manque Pascal 1.1 ID block `$CnFB-$CnFF`.
- [ ] 🟡 **Telnet IAC strip mange les `$FF` valides en RX 8-bit
      binaire**. Proposer toggle « raw mode » dans le panneau.
- [ ] 🟡 **LF→CR mapping** appliqué uniquement sur keyboard sink, pas
      sur `rxBuf` (`SuperSerialCard.cpp:187-198`).
- [ ] 🟢 **IRQ gate sur DIP** : MAME `a2ssc.cpp:373` n'arme l'IRQ que
      si SW2:6 (Interrupts) est on. POM2 expose toujours.

## Mouse Card

- [ ] 🟡 **Sync curseur pixel-près host/guest non résolue** (2026-05-15).
      Le tracking actuel est delta-based : 1 px hôte → 1 px Apple en
      vitesse (ratio `display.width() / widgetW` dans
      `MainWindow::onMouseMove`), avec accumulation sub-pixel et clamp
      résidu à ±127 ticks par event. Fonctionne mais les deux curseurs
      dérivent en position absolue parce qu'on n'a pas accès à la
      position vraie du curseur dans A2Desktop pour corriger.
      Investigation menée :
      - **Screen-holes standard** (`$0478+slot/$04F8+slot/$0578+slot/
        $05F8+slot`) : la firmware MCU les utilise via `LDA $0438,Y`
        avec Y=slot*16, MAIS A2Desktop's MGTK ne les lit pas — elle
        appelle `ReadMouse` du slot ROM et copie les valeurs dans son
        propre data segment.
      - **MGTK data segment** : `mouse_state:` (mouse_x word, mouse_y
        word, status byte) + `cursor_pos:` (xcoord word, ycoord word).
        Chargé à `MGTKAuxEntry := $4000` en **AUX RAM** d'après
        `src/desktop/desktop.inc` du repo `a2stuff/a2d` (disasm
        Apple II Desktop). Offset exact de `mouse_state` dépend du
        link layout — n'a pas pu être identifié.
      - **Pearson scan complet** (main + aux RAM $0000-$BFFF) avec
        host cursor déplacé sur 44 waypoints couvrant les 4 coins +
        diagonales + croix : **0 candidat à `|r| > 0.7`**. Probablement
        parce que le curseur X côté Apple ne se déplace presque pas
        (~5 px Apple pour 600 px hôte) — voir TODO suivant.
      - **Slot ROM 6502 et MCU sources** récupérés dans `roms/` ;
        firmware Apple-copyright (341-0269.bin, 341-0270-c.bin) gérée
        comme port MAME fidèle (cf. `MouseCard.cpp`).
      Reprendre l'investigation soit en disassemblant l'offset de
      `mouse_state` depuis le binaire A2DeskTop release v1.5, soit en
      ajoutant un memory-write hook qui détecte le premier byte
      pair-corrélé avec target X/Y après que la firmware MCU a posté
      sa première ReadMouse.

- [ ] 🟠 **Curseur X bloqué à ~8 px Apple pour de grandes motions
      hôte** alors que Y fonctionne sur toute la plage. Symptôme
      visible via screenshot-diff (`/tmp/m_*.ppm` after host moves
      full-width) : bbox cursor x = [0..5..8] même quand l'hôte
      traverse les 600 px du widget. Pas de régression dans
      `MouseCard.cpp` côté MCU — les deux axes utilisent
      `updateAxis()` symétrique. Hypothèses :
      - A2Desktop a posé un clamp X étroit (rectangle window-local)
        via `SetMouse` après initialisation ;
      - La firmware MCU est dans son idle loop ($0403/$0405/$0409/
        $0469/$03F3) qui ignore la motion tant que `RAM $58 bit 0`
        n'est pas set par la commande `ReadMouse`, et A2Desktop ne
        pulse `ReadMouse` pas assez souvent ;
      - Bug subtil dans `MouseCard::updateAxis` pour X (PB0/PB1) vs
        Y (PB2/PB3) — pourtant `mouse_card_quadrature_smoke_test.cpp`
        passe.
      Outils utiles laissés en place : `POM2_MOUSE_TRACE=1` (logs
      MCU PC + transitions PIA), `POM2_AUTO_BOOT_HDV=N` (auto-boot
      slot 5), `POM2_AUTO_QUIT=N`. Extension AI Control HTTP
      `/mem?addr=...&len=...&bank=aux` pour lire aux RAM.

## Disques

- [x] ~~🟡 **WOZ2 `optimal_bit_timing` (INFO+39) ignoré**.~~ Fait
      2026-05-15. Lu dans `loadWoz` quand `info_version >= 2`
      (clamp [8, 64] sur la valeur), stocké dans
      `DiskImage::optimalBitTiming`, exposé via
      `lssCyclesPerCell()` qui pilote `expandTrackFlux` et
      `trackPeriod`. WOZ1 et non-WOZ gardent le défaut 32 (= 8 LSS
      cycles/cell). Pinné par `woz_bit_timing_smoke_test.cpp`
      (obt 32/40/28 + WOZ1 fallback).
- [ ] 🟡 **WOZ1 splice point (TRK +6650)** ignoré
      (`DiskImage.cpp:381-398`). MAME passe via `set_write_splice`.
      Maintenant que WOZ write-back est en place ce hint compte vraiment
      (re-master parité avec l'imager Applesauce).
- [ ] 🟡 **UI « Force DOS / Force ProDOS »** pour le manual override
      path. `loadFile(path, SectorOrder)` existe
      (`DiskImage.cpp:212`) mais aucun bouton UI ne l'invoque. Utile
      quand l'auto-détection content-driven se trompe sur une image
      atypique.
- [ ] 🟢 **Inflation sub-instruction RAII** (`DiskIICard.cpp:711-744`)
      vs dispatch per-bus-cycle MAME m6502. Tradeoff documenté ; rare
      impact sur protections cycle-sensibles.
- [ ] 🟢 **Half-tracked NIB (88 tracks)** non supporté. Locksmith /
      David-DOS bury protections sur half-tracks ; WOZ couvre déjà ce
      besoin via TMAP donc priorité basse.
- [ ] 🟢 **Applesauce `.nib2` / `.app`** (bit-shift, multi-track-
      per-byte) non supportés. Hors scope tant que WOZ couvre les
      protections.
- [ ] 🟢 **Disk II dans le snapshot `POM2SNAP`** délibérément exclu
      aujourd'hui (`CLAUDE.md` « Snapshot »). Reprise après reload
      nécessite identité de l'image + position de tête + dirty bits par
      piste.

## Audio

- [ ] 🟡 **AY float tone counter aliase à périodes très courtes (1-3)**
      (`Mockingboard.cpp:449-456`). MAME utilise compteur entier.
      Inaudible à période normale, manifeste sur tricks PWM.
- [ ] 🟡 **Cassette auto-rewind 500 ms** si pas de poll `$C060`
      (`CassetteDevice.cpp:461-465`). MAME ne rewind jamais. Casse les
      loaders custom polling sporadique.
- [ ] 🟢 **Port A read mask par DDR** (`Mockingboard.cpp:126-130`).
      Devrait retourner pin state. Sans impact concret.

## Features manquantes

- [ ] 🟡 **PADL(2) / PADL(3) binding host** (second stick, actuellement
      centrés à 127 — `JoystickInput.cpp:65-75`).
- [ ] 🟡 **Mapping souris → paddles** : alternative aux pads, paddle 0/1
      sur axe X/Y de la souris host (la Mouse Card route déjà la souris
      vers `$C0n0` mais pas vers `$C064/65`).
- [ ] 🟡 **Eve Color text mode (`$C0B9`)** — attributs FG/BG par
      caractère via banque aux dédiée (variante Chat Mauve / Eve). Stub
      UI dans `LeChatMauve_ImGui.cpp:200`.
- [ ] 🟢 **Video-7 AppleColor RGB** — équivalent américain de Le Chat
      Mauve avec même AN3+80COL mais encodage FIFO différent.
- [x] ~~🟢 **RAMWorks 1 MB** (IIe-only, ProDOS 3+).~~ Fait 2026-05-14
      en port verbatim MAME `a2eramworks3.cpp` — 1/4/8/16/48/128 banks
      (jusqu'à 8 MB). Setting `ramworks_banks`. Pinné par
      `ramworks_smoke_test.cpp`. Cf. CLAUDE.md « RamWorks III ».
- [ ] 🟢 **Le Chat Mauve EVE variante** (64 KB extension RAM intégrée +
      modes SPEC1 / SPEC2 / DASH / COL280).
- [ ] 🟢 **Color killer** (logique Rev 1) — cosmétique.
- [ ] 🟢 **Strapping RAM 4K → 48K** — modéliser la machine de 1977
      d'origine (POM2 colle 64 K en dur aujourd'hui).

## MAME parity drifts (audit 2026-05-15)

Items relevés par l'audit ligne-à-ligne MAME ↔ POM2 mais skippés
volontairement. Cf. section « Divergences » CLAUDE.md pour les
divergences intentionnelles (DHGR Video-7 palette, ThunderClock
MODE_SHIFT lax, etc.) — celles-ci ne sont **pas** des bugs, leur
"fix" régresserait quelque chose qui marche.

- [ ] 🟢 **$C040 utility STRB pas gated sur !//c** — MAME
      `apple2e.cpp:1927` pulse STRB seulement quand `!m_isiic`. POM2
      retourne 0 inconditionnellement. Pas de sink wired aujourd'hui
      donc 0 effet observable. À fixer quand une carte avec STRB
      arrivera (joystick paddles externes, etc.).
- [ ] 🟢 **MouseCard PIA out_a/b sans `scheduler.synchronize`** —
      MAME `mouse.cpp:280-294` defer la propagation PIA→MCU d'un
      quantum scheduler. POM2 propage immédiatement
      (`MouseCard.cpp:159-177`). Aucune race firmware-visible
      observée ; un read tight après un write Port A pourrait voir
      la nouvelle valeur 1 instruction MCU plus tôt qu'en silicium.
- [ ] 🟢 **ClockCard offset model vs MAME `set_time`** —
      `ClockCard.cpp:113-145` stocke un delta vs `timeFn()`. MAME
      copie shift_reg dans `m_time_counter[]` et tick à 1 Hz via
      `m_timer_clock`. Équivalent comportementalement tant que
      `timeFn()` avance en lock-step avec le wall clock. Refactor
      pour rien sauf si on ajoute un mode `--frozen-clock` pour
      tests reproductibles.
- [ ] 🟢 **Mockingboard.cpp:33-34 path drift** — MAME `wozfdc.cpp`
      a bougé `bus/a2bus/` → `machine/` ; doc déjà à jour (CLAUDE.md
      + comments DiskIICard.h/cpp) mais audit refresher utile tous
      les ~6 mois pour suivre les renommages MAME upstream.


## Changelog

Voir [`CHANGELOG.md`](CHANGELOG.md) pour l'historique complet des changements résolus.

## SmartPort — extensions possibles

La carte SmartPort 3.5" branchable en slot //e est en place
(`SmartPortCard`, 2 unités block-device, smoke test pinné). Pistes
restantes pour étendre la couverture matérielle :

- [ ] 🟡 **Mass storage SmartPort 32 MB ProDOS** — vérification de
      couverture. Beaucoup de softs modernes (ex: Total Replay II)
      exigent « un device mass storage qui supporte les appels
      SmartPort et peut monter une image ProDOS de 32 MB ». Sur du
      vrai hardware c'est ce que fournissent **CFFA3000 / Floppy Emu /
      MicroDrive / Xdrive / SP\[\]SD** ; côté POM2 c'est rempli par
      `SmartPortCard` + `SmartPortHdvUnit` (slot 5 par défaut, jusqu'à
      2 units, 65535 blocks × 512 B = 32 MB).
      À pinner explicitement :
      (a) boot d'une HDV 32 MB via `SmartPortCard` sur //e
          (`tests/smartport_card_smoke_test.cpp` couvre ProDOS HDV
          générique mais pas le cas 32 MB exact) ;
      (b) compat .2mg ProDOS-order avec header DC42 / Universal /
          variantes CFFA (header 64 B vs 0 B selon outil) — passer
          par `DiskImage::loadFile` qui sniff déjà mais ajouter un
          test pinné sur les 3 variantes de header ;
      (c) ProDOS partitions multiples (CFFA3000 expose plusieurs
          volumes ProDOS sur une seule carte) — non supporté
          aujourd'hui (1 image = 1 unit = 1 volume).

- [x] ~~🟡 **DiskII multi-instances dans plusieurs slots**.~~ Fait
      2026-05-15 (option C, commit `5f7f209`). `MainWindow.h` héberge
      `std::vector<DiskIICard*> diskCards` ; `MainWindow_Slots.cpp`
      itère le vecteur (lignes 337-594). `diskCard` reste comme alias
      du primary (= `diskCards.front()` ou nullptr). Per-slot settings
      `disk_path_slotN`. Permet `Disk II slot 6 + Disk II slot 4` (4
      drives 5.25" sur un //e). Pinné implicitement par les smoke
      tests existants `disk_*` + le runtime UI.
- [ ] 🟢 **UDC (Universal Disk Controller, Apple 1991)** — carte
      « SmartPort 4 baies hétérogènes » : chaque baie peut être
      `Disk35Image` (3.5"), `DiskImage` (5.25") ou
      `ProDOSHardDiskCard`-backed (block device générique). Refactor
      du protocole `SmartPortCard` pour typer chaque unité ; ProDOS
      voit jusqu'à 4 devices sur le slot. ROM UDC pas dans le domaine
      public — partir de la spec SmartPort + reverse depuis Apple
      Tech Note #SmartPort. Faible demande en pratique : 99% des
      configs réelles utilisaient Liron + Disk II en 2 slots séparés.
- [ ] 🟢 **Slinky / RamFAST RAM disk SmartPort**. Une carte avec
      512 KB-32 MB de RAM exposés en device-bloc SmartPort. Trivial
      sur le papier (juste un buffer en `std::vector<uint8_t>` +
      protocole bloc identique à `SmartPortCard`) mais utilité
      limitée vs RamWorks III déjà supporté.
- [ ] 🟢 **Apple II SCSI Card** (Apple Part 670-0144). Branche un
      vrai `Apple Hard Disk 20SC` ou n'importe quel SCSI ID. ROM
      Apple-copyright disponible mais le protocole SCSI complet est
      lourd ; pour ProDOS l'équivalent fonctionnel est déjà
      `ProDOSHardDiskCard` (.hdv block device sans le SCSI réel
      par dessus).
- [ ] 🟢 **Apple II Disk 3.5" Controller — passage IWM-level**.
      `SmartPortCard` actuel est block-device (passe par
      `Disk35Image::readBlock/writeBlock`). Le vrai Liron embarque
      un IWM ; passer le slot card par le `IWMDevice` +
      `Sony35Drive` existants permettrait de jouer des images flux
      copy-protected sur //e (rare mais existe — ex: « Mr. Robot »
      en .woz Sony). Demande de refactoriser `IWMDevice` pour qu'il
      puisse vivre attaché à un slot card et pas seulement au //c+
      on-board.
- [x] ~~🟢 **Caches read/write per-block dans `SmartPortCard`**.~~ Fait
      2026-05-16 dans le refactor `7db6861` ("enhanced unit management").
      Plus de `thread_local` dans `SmartPortCard.{h,cpp}` ; chaque
      unité possède ses propres buffers de bloc. Multi-instance OK.

## Architecture & qualité du code (audit 2026-05-15)

Items transversaux relevés lors de la revue d'architecture
2026-05-15. Pas de bug fonctionnel à la clé — c'est de la dette qui
freine l'ajout de nouveaux profils Apple II et augmente le coût de
chaque changement de header.

- [ ] 🟠 **`Memory` god-object (`Memory.h` 521 L, `Memory.cpp` 1 555 L)**.
      Responsabilités empilées : RAM/ROM + bitmap `writable[]`,
      pagination IIe, RamWorks III, Language Card, soft-switches,
      RC paddle, FIFO clavier + paste queue, callback speaker,
      dispatch slot bus, IWM, fenêtre MIG //c+, firmware alternatif
      //c+. Extraire au minimum `Keyboard` (FIFO + strobe + paste),
      `PaddleInputs` (RC + buttons + IIe modifier keys), et
      `IIcPlusBank` (alt firmware + MIG + iicRomBank). Réduit la
      surface publique de `Memory.h` et limite les recompilations
      en cascade.
- [ ] 🟠 **Spécificités //c+ qui fuient dans `Memory::memRead`/
      `memWrite`**. `iicHasAltBank`, `iicRomBank`, `iicAltFirmware`,
      `migRead/migWrite` sont testés en plein dispatcher générique
      (`Memory.cpp:1338,1370-1391`). À chaque nouveau profil (IIgs,
      future variante) cette cascade va s'épaissir. Pattern cible :
      stratégie de profil (`IProfileMemoryStrategy`) injectée par
      `EmulationController`, le dispatcher générique reste ignorant
      du modèle. À traiter conjointement avec l'extraction
      `IIcPlusBank` ci-dessus.
- [x] ~~🟠 **`MainWindow.h` inclut 20+ headers (chaque carte + chaque
      panneau ImGui + ImGui lui-même)**.~~ Fait 2026-05-15.
      MainWindow.h ne dépend plus que de `M6502.h` + `imgui.h` ;
      cartes/panels/controller forward-declared, membres value-type
      passés en `unique_ptr` (constructeur/destructeur out-of-line).
      Mesures : touch `CassetteDeck_ImGui.h` → 2 TUs recompilés ;
      touch `MainWindow.h` → 4 TUs (irréductible : les 3 .cpp
      MainWindow + main.cpp). main.cpp + MainWindow_Slots.cpp
      adaptés en conséquence ; build + 57 tests verts.
- [ ] 🟡 **Encapsulation `Memory` trouée par `dataMutable()` /
      `auxDataMutable()`** (`Memory.h:135,258`). N'importe quel
      appelant peut écrire en ROM en contournant le bitmap
      `writable[]`. Utile pour le debugger et `SnapshotIO`, mais
      sans contrôle d'accès c'est une porte ouverte aux régressions
      silencieuses. Remplacer par un handle scopé
      (`Memory::DebugWriteScope`) ou par des accesseurs nommés
      (`writeRamUnchecked(addr, v)` qui asserte `addr < 0xC000`).
- [ ] 🟡 **Configuration éclatée sur 3 canaux indépendants** : env
      vars (`POM2_TRACE_IIE_REBOOT`, `POM2_IWM_AUTHORITATIVE`,
      `POM2_IWM_LEGACY_DATA_PATH`, `POM2_DEBUG_DISK_STREAM`,
      `POM2_MOUSE_TRACE`, `POM2_AUTO_BOOT_HDV`, `POM2_AUTO_QUIT`,
      `POM2_CPU_CLOCK_HZ` — `grep -n getenv *.cpp` = 11
      occurrences), flags CLI (`CliDispatcher`), fichier
      `Settings`. Aucun registre central, l'utilisateur doit
      grepper pour découvrir les env vars. Ajouter un `Config`
      unique avec l'ordre de précédence env → CLI → Settings →
      defaults, et lister les env vars dans `--help`.
- [ ] 🟡 **Granularité du `stateMutex`** (`EmulationController.h:118`).
      Un seul lock partagé entre CPU et UI suffit aujourd'hui parce
      que l'UI ne lock que pour des snapshots courts. Mais
      `MainWindow_Slots.cpp:281,386,426,447,494` prend ce même lock
      pendant des opérations de re-câblage de cartes (plug/unplug,
      restartEmulationFromSettings), pendant lesquelles le CPU est
      gelé — risque de jitter audio si un panneau ImGui devient
      lourd. Cible long terme : partitionner en (a) lock CPU/Memory,
      (b) ring audio lock-free, (c) état UI cohérent par snapshot.
      Préalable : auditer chaque `stateMutex` lock pour mesurer la
      durée typique de tenue.
- [ ] 🟡 **Cohérence namespace `pom2::`** : ~20 / 89 fichiers
      seulement l'utilisent. Les classes historiques (`M6502`,
      `Memory`, `MainWindow`, `Apple2Display`, `DiskIICard`,
      `MockingboardCard`, `CassetteDevice`, `SuperSerialCard`,
      `ClockCard`, `MouseCard`, `LeChatMauveCard`,
      `ProDOSHardDiskCard`) sont en namespace racine. Mélange
      visible dans `EmulationController.h` (`Memory mem;` à côté de
      `std::unique_ptr<pom2::IWMDevice>`). Migrer en une passe
      mécanique vers `pom2::` partout — ne pas faire moitié-moitié.
- [ ] 🟡 **`Memory::memRead` hot path complexe** (`Memory.cpp:1309-1437`).
      Profondeur d'imbrication jusqu'à 7, dispatch en cascade de
      `if`. Appelé ~1 M fois/seconde × 60× en mode turbo. Une table
      de dispatch indexée par page haute (256 entrées de
      `MemoryHandler*`) serait plus lisible **et** probablement plus
      rapide. Préalable : extraire les responsabilités //c+ (item
      🟠 ci-dessus) sinon la table de 256 entrées devient une
      table de 256 entrées × 5 profils.
- [ ] 🟢 **Style hérité dans `M6502`** : commentaires bilingues
      FR/EN (`M6502.cpp:48-49,71,79,98`), casts C-style
      `(unsigned short)` (`M6502.cpp:88-94`), signatures `void(void)`
      (`M6502.h:42-45`), aggregate init manuelle dans les
      constructeurs au lieu de `= default` + initialiseurs
      in-class. Cosmétique mais détonne du reste du projet.
      Passe `clang-format` + `clang-tidy modernize-*` ciblée sur
      `M6502.{h,cpp}`.
- [ ] 🟢 **Pointeurs nus `*Card` dans `MainWindow`**
      (`MainWindow.h:97-103`). 7 pointeurs `// non-owning, owned by
      SlotBus`. Pas de notification quand SlotBus libère / replug.
      `restartEmulationFromSettings()` doit penser à les remettre à
      jour manuellement — fragile en cas d'évolution. Soit
      observers pattern via `SlotBus::setReplugListener`, soit
      passer par `controller.slotBus().peripheral(N)` à chaque
      utilisation (un peu plus de typing mais zéro risque de
      dangling).

## Hors scope

- 80-col / IIe-only sur machine sans ROM IIe
- IIgs / ProDOS 16
- Disk II 13-secteurs (Apple II 1977-1979, pré-DOS 3.3)
- Z-80 SoftCard CP/M
- CFFA CompactFlash (POM2 a déjà ProDOS HDV slot 5 + host folder)
