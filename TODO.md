# POM2 — TODO

État au 2026-05-15. Backlog ouvert. Orientation → `CLAUDE.md` ;
notes d'implémentation profondes → `DEV.md` ; historique → `git log`.

Légende sévérité : 🟠 High · 🟡 Medium · 🟢 Low. Sections regroupées
par sous-système, triées par poids des items 🟠.

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

## Display

- [ ] 🟢 **`monochrome_dhr_shift()` 1-px alignment manquant** en DHGR
      mono (MAME `apple2video.cpp:460-471`). Cosmétique.
- [ ] 🟢 **Floating-TTL** `empty_words[40]={0x3fff,…}` pour rows
      hors-bounds non modélisé (MAME `:751-758`). Jamais atteint en
      48K+.
- [ ] 🟢 **DHGR per-scanline mode switching** — bottom-of-mixed utilise
      une région 4-line statique.

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

## Changelog (résolu cette session, 2026-05-14/15)

**SmartPortCard 3.5" pluggable slot //e** : nouveau
`SmartPortCard.{h,cpp}` (~280 L, slot 5 par défaut) qui expose les 2
`Disk35Image` d'`EmulationController` en block-device ProDOS via slot
ROM signée + dispatcher $Cn50 + soft-switches $C0nX. Driver
$Cn50 examine `$43` bit-7 pour router drive 1 / drive 2. PR#N boote
drive 1. Dropdown slot config "SmartPort 3.5\"". Pinné par
`smartport_card_smoke_test.cpp` (ROM signature, drive-select,
streaming I/O, write-back roundtrip).

**Sons mécaniques 3.5"** : `Sony35Drive` reçoit un
`FloppySoundSink*` (réutilise le `FloppySoundDevice` du Disk II) ;
`seekPhaseW(uint8_t, uint64_t emuCycles)` propagé via
`SmartPortHub` depuis `IWMDevice::emuCycles()`. `strobeWriteRegister`
émet step / motor on / motor off / click avec stamping cycle CPU
émulé (parité MAME). `mount35` / `eject35` côté
`EmulationController` émettent un click via `emitInsertClick()`.

**Disk35 panel UX** : LED moteur rouge par drive, marqueur `* ` sur
les images chargées, librairie 540 px, write-back checkbox per-drive
+ indicateur "(unsaved)", left-click = mount + cold boot, right-click
= menu contextuel drive 1/2 × boot/no-boot. `Disk35Image::isWriteBackEnabled()`
ajouté pour parité accessor avec Disk II.


**cc65-Chess.po hang après `LOADING CHESS`** : résolu 2026-05-15. Les
fixes round 1 (`kSyncMinRun=5` + `revolutionStartLssCycle` per-drive)
combinés aux corrections de chemin de boot (B1-B5) suffisent
finalement — le jeu démarre proprement, le `JSR $A403` ne tape plus
sur `$00`. Le pinning est implicite (boot `cc65-Chess.po` jusqu'à
prompt d'ouverture du moteur d'échecs).


**Floppy sound step cadence** mesurée en cycles CPU émulés (MAME
`floppy_sound_device::step` via `machine().time()`,
`floppy.cpp:1532-1620`), pas en frames audio. En turbo 60× toutes
les ~80 pulses du PROM de boot tombaient dans le même buffer audio
de 5 ms → `audioFrameCounter_` constant → fallback `STEP_1_1` avec
`stepPos_=0` réinitialisé par event → l'attaque de `step_1_1` (5 ms)
rejouée buffer après buffer = buzz / son haché qui ressemblait
vaguement à un sample du `roms/floppy_samples/`. `FloppySoundSink::
step()` prend maintenant `emuCycles`, `DiskIICard::seekPhaseW` passe
son `cpuCycleTotal`. Pinné par
`floppy_sound_smoke_test.cpp::testRapidStepsNoHang` (140 events
`~10 ms` émulés, no buffer between → `audioInSeek()=true`) +
`testSameCycleStepsClampGracefully` (50 events au même cycle → floor
1 ms → SEEK_2MS @ pitch 2.0).

Fix MAME-parity round 1 (Mockingboard / IIe / //c) — pinned par tests
correspondants :

- **HIGH-1** Mockingboard AY tone/noise/envelope rate ×2 (clock/16 →
  clock/8 + noise prescaler MAME `ay8910.cpp:1086-1104`). Toutes les
  musiques Mockingboard (Ultima IV, Nox Archaist, Total Replay,
  Skyfox) jouent enfin à la bonne fréquence — avant, 1 octave trop
  bas + enveloppes 2× trop lentes.
- **HIGH-2** $C019 RDVBLBAR clear-on-read divergence (Apple IIc Tech
  Note #9 : « does not reset »). Retiré le clear-IRQ-on-read de
  `Memory.cpp`.
- **MED-1** //c $C02x ROMBANK range étendu de $C028 seul à
  $C020-$C02F entier (MAME `apple2e.cpp:1894-1922` `(offset & 0xf0)
  == 0x20`). Cassette toggle gated sur `!iicHasAltBank`.
- **MED-2** Drive-switch revolution_start per-drive
  (`revolutionStartLssCycle[2]` + `kNeverRev` + offset dans
  `getNextTransition`). MAME `wozfdc.cpp:264-291` + `floppy.cpp:
  809-839 mon_w`. À vérifier que cc65-Chess hang est résolu par ça.
- **Floating bus** port verbatim `scanner_address` (MAME
  `apple2video.cpp:124-201`). addend0=0x0D + h-carries + v_4/v_3
  doublés dans addend2 + HBL phantom row $1000 gated sur !//e.
  Impact RNG byte pour softs qui poke $C019/$C04x en VBL.
- **LOW-1** AY LFSR re-seed sur PB2=0 reset (MAME `ay8910.cpp:1309`
  `m_rng=1`) via cross-thread `ayResetCount_` signal.
- **LOW-2** VIA T1 continuous reload `+2` → `+3` (MAME `IFR_DELAY`).
- **LOW-3** AY register strobe fire sur tout call (drop debounce),
  diagnostic counters restent en edge-only.
- **LOW-4** AY `reset_w` wipe regs 0..13 (R14/R15 préservés, MAME
  `ay8910_reset_ym`).
- **LOW-5/6** doc Mockingboard.h wiring (PB0=BC1/PB1=BDIR/PB2=RESET)
  + CLAUDE.md citations `wozfdc.cpp` (moved `bus/a2bus/` → `machine/`).

Avant ce round : **RamWorks III** ajouté (port verbatim
`a2eramworks3.cpp`, jusqu'à 8 MB, pinned).

Coordination des chemins de boot solidifiée :

- **B1** AI server dangling pointers pendant `applyProfile` →
  `aiServer.detach()` sous `stateMutex`, handlers null-check sous
  verrou, `attach()` ré-armé après re-plug.
- **B2** Disques inserted mid-session perdus sur profile switch →
  lit `diskCard->getDiskPath()` / `hdvCard->getImagePath()` au lieu
  du setting persisté.
- **B3** Use-after-free deferred-actions thread → tracké + cancel
  atomic + join avant teardown.
- **B4** Reset RamWorks bank → 0 (MAME `device_reset` parity).
- **B5** RamWorks gate strict sur profil `AppleIIe` (pas //c/c+ qui
  n'ont pas d'aux slot).

## SmartPort — extensions possibles

La carte SmartPort 3.5" branchable en slot //e est en place
(`SmartPortCard`, 2 unités block-device, smoke test pinné). Pistes
restantes pour étendre la couverture matérielle :

- [ ] 🟡 **DiskII multi-instances dans plusieurs slots**. Aujourd'hui
      `MainWindow_Slots.cpp` interdit le doublon de type ; lever cette
      contrainte uniquement pour `"diskii"` permet la config historique
      `Disk II slot 6 + Disk II slot 4` (= 4 drives 5.25" sur le même
      //e). Demande un refactor mineur de `diskCard` en
      `std::vector<DiskIICard*>` et per-slot settings (`disk_path_slotN`).
      C'est l'**option C** discutée 2026-05-15 — la plus accessible,
      ROI immédiat pour les utilisateurs DOS 3.3 multi-volumes.
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
- [ ] 🟢 **Caches read/write per-block dans `SmartPortCard`** —
      le code actuel utilise `static thread_local` pour les buffers
      de bloc lu/écrit ; OK pour un seul thread CPU mais à
      remplacer par des membres de classe si on multi-instance la
      carte. Latent jusqu'à ce qu'on plug 2 SmartPort différentes.

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
