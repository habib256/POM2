# POM2 — Changelog

Historique des changements notables, organisé du plus récent au plus
ancien. Le `git log` reste la source canonique pour la mécanique
exacte ; ce fichier capture les **« pourquoi »** et les pièges qu'on
ne veut pas re-découvrir. Backlog actif → `TODO.md`. Implémentation
courante → `DEV.md`.

## 2026-05-24

- **Infra de release Linux** (branche `release-infra`). Nouveau résolveur
  d'assets `ResourcePaths` (`.h/.cpp`) : centralise la recherche des assets
  *bundlés* (roms/, fonts/, floppy_samples/) avec, en plus des racines
  historiques relatives au CWD (`.`, `..`, `../..` — comportement dev
  inchangé), des racines **relatives à l'exécutable** (`<exe>`, `<exe>/..`,
  `<exe>/../share/POM2`) et la racine **utilisateur** `~/.local/share/POM2`.
  Indispensable pour qu'un binaire *installé* (AppImage, `.deb` → `/usr/bin`,
  tarball lancé par chemin absolu) trouve ses fichiers. Câblé sur les
  chokepoints : `RomLoader::probe*`, `MainWindow::firstExistingPath`,
  `CharRomCatalog::resolveCharRomPath`, le probe ROM CFFA, les boucles
  inline disk2/LSS/mouse/char/auto-détect, et les polices de `main.cpp`.
  **Pourquoi** : tous les sites répétaient `X` / `../X` / `../../X`, qui ne
  marche que si le CWD est la racine du dépôt — ce qu'une release casse.
  Les 76 tests passent inchangés. CMake gagne `install()` (bin/ +
  share/POM2) + **CPack** (TGZ relocatable + DEB shlibdeps) ; `build_dist.sh`
  produit `DIST/POM2-v0.6-linux-x86_64.{tar.gz,deb}` (+ AppImage si
  `linuxdeploy` présent). **Les ROMs Apple ne sont jamais incluses**
  (copyright) — un `roms/README.txt` indique où les déposer. Pièges :
  la cible CMake GUI est `pom2_imgui` (sortie `POM2`) — `make POM2` est un
  no-op, le script build via `--target pom2_imgui` ; `pom2_headless` et les
  tests compilent `RomLoader.cpp` donc linkent aussi `ResourcePaths.cpp`.

- **Release v0.6** — jalon regroupant CFFA 2.0 (carte IDE MAME-fidèle),
  Floppy Emu (BMOW), Slot Manager consolidé, rendu Video-7 / Le Chat Mauve
  complet, et le support Disk II 13-secteurs (DOS 3.1/3.2). Version bumpée
  dans `main.cpp` (bannière + titre fenêtre), `MainWindow.cpp` (À propos),
  `CMakeLists.txt` (`project VERSION`) et `CLAUDE.md`.

- **Floppy Emu (BMOW) — modèle du gadget SD/OLED** (`floppy_emu_smoke`).
  Nouveaux `FloppyEmuDevice` + `FloppyEmu_ImGui` modélisant le Big Mess o'
  Wires Floppy Emu : carte SD + OLED 128×64 + 3 boutons (PREV/NEXT/SELECT)
  qui *devient* un lecteur. POM2 émule déjà tous les types de lecteurs que
  l'Emu peut présenter — la valeur ajoutée est donc le comportement *propre*
  du gadget : le **mode d'émulation** persistant (sa NVRAM ; 4 modes 5.25 /
  3.5 / Unidisk 3.5 / Smartport HD), l'**explorateur de fichiers SD** (borné
  à la racine, filtré par mode), les **favoris** (`favdisks.txt` + directive
  `automount`), le workflow de swap. Le montage réel est **routé par
  MainWindow** vers les cartes existantes (`DiskIICard` / unités
  `SmartPortCard`) — le device choisit l'image + le mode, rien d'autre. Le
  cœur est volontairement agnostique UI/émulateur (zéro ImGui / MainWindow /
  SlotBus) → testable en isolation (filtrage de format, navigation SD,
  parsing favdisks). La « carte SD » est le dossier `floppyemu/`, **séparé**
  des dossiers Disk Library (`disks/` `disks35/` `hdv/`) car l'Emu est sa
  propre carte. Réf : manuel BMOW Floppy Emu Model C §3 + §5. Menu : Devices
  → Floppy Emu. Réglages `floppyemu_mode` / `floppyemu_sd_root` /
  `show_floppy_emu`. Hors scope v1 : modes Dual-5.25 et Smartport-Unit-2
  (boot daisy-chain IIgs). Voir `DEV.md § Host control center`.

- **Slot Manager — panneau bus consolidé + `MountableMediaCard`**
  (`slot_multi_card_smoke`). Nouveau `SlotManager_ImGui` : une seule fenêtre
  liste tous les slots (1-7 + ligne AUX 80-col en IIe), la carte par slot
  (dropdown ou badge built-in verrouillé), et — pour toute carte exposant des
  baies média — mount / eject / write-back / type-select / boot inline par
  baie. **Pourquoi** : le bus est multi-instance (plusieurs cartes bloc,
  plusieurs SmartPort) mais les anciens panneaux par-carte supposaient chacun
  un pointeur global unique. Le Slot Manager se construit depuis une
  **énumération du SlotBus** (`bus.peripheral(s)` +
  `dynamic_cast<MountableMediaCard*>`), donc reste correct quel que soit le
  nombre de cartes plugguées. `MountableMediaCard.h` = mix-in **côté hôte**
  (pas une préoccupation bus, même motif que `ProDOSBlockCard`) :
  `bayCount/bayInfo/mountBay/ejectBay/setBayWriteBack` + type-select
  (`bayTypeOptions/setBayType`). `ProDOSBlockCard` l'implémente comme une
  baie fixe → HDV + CFFA l'obtiennent gratuitement ; `SmartPortCard`
  l'implémente sur ses 2 unités (type empty / 3.5" / hdv). `SlotCardCatalog.h`
  extrait la liste unique `kCardTypes` + les sondes ROM
  (`mouseRomsPresent`/`cffaRomPresent`), partagée entre le panneau Slot Config
  hérité et le Slot Manager. Multi-instance élargie : le Slot Manager autorise
  `diskii`/`cffa`/`smartport35` en plusieurs slots (persistance par slot). Les
  panneaux par-carte détaillés subsistent pour l'état profond (LED de piste,
  moteur, Disk Library) via « Open detailed panel ». Menu : Machine → Slot
  Manager. Réglage `show_slot_manager`. Voir `DEV.md § Host control center`.

- **SmartPortHdvUnit — adossé au `Block512Backing` partagé**. L'unité HDV
  d'une carte SmartPort réimplémentait son propre stockage ; elle est
  désormais un fin adaptateur sur le `Block512Backing` commun (le store
  derrière `ProDOSHardDiskCard` / `CffaCard`) → envelope 2IMG, suivi des
  blocs sales, WP médium et write-back opt-in obtenus sans duplication. Chaque
  `SmartPortUnit` possède ses octets (fin du `thread_local`). Pinné par
  `smartport_mixed_units_smoke`.

- **Rendu Video-7 / Le Chat Mauve complet (4 rgbmodes DHGR + texte
  foreground-background)**. `Apple2Display::renderDhgr` consulte désormais le
  mode FIFO AN3 de la carte (`LeChatMauveCard::currentMode()`) pour choisir
  l'un des quatre rgbmodes de MAME `apple2video.cpp` `dhgr_update()` :
  `COL140`(3, couleur 140-cellules, déjà présent), `Mixed`(1, le MSB de chaque
  octet source choisit couleur vs mono 7-points), `Chunky160`(2, `aux+(main<<8)`
  → quatre pixels 4-bit de 3 points, 480 centrés dans 560) et `BW560`(0, mono
  strict). Nouveau `renderTextChatMauveFgBg` : mode **texte couleur Video-7**
  (texte 40-col + AN3 actif) — le code char vient de la RAM principale, les
  couleurs fg/bg par cellule de l'octet aux à la même adresse (quartet haut =
  fg, bas = bg ; glyphe 7 bits doublé en 14 points). Porté verbatim de MAME
  (`dhgr_update` `:885-980`, `render_line_color_array` `:571-583`, `text_update`
  `:788-791`), palette Feline AppleWin conservée (deux gris distincts). Le
  refactor de `COL140` est bit-identique à l'ancien chemin (les tests existants
  passent inchangés). Épinglé par `tests/video7_parity_smoke_test.cpp` (les 4
  rgbmodes + fg/bg comparés ligne par ligne — 560 points — à un oracle MAME
  `dhgr_update` autonome embarqué dans le test). 72 tests OK.

- **`bootFromSlot` réplique l'init zéro-page du Monitor F8 (corrige Mr. Robot
  4am)**. `EmulationController::bootFromSlot` (raccourci `coldBoot + PC=$Cn00`
  utilisé par le kiosk/CLI, les boutons « Boot » de la Disk Library ET « Boot
  HDV ») saute le cold-start du Monitor F8 — donc n'initialisait PAS la
  zéro-page texte/I-O que les routines écran du Monitor attendent. Concret :
  CLREOP (`$FCA0`) boucle sur `CPY WNDWDTH($21)` ; avec `$21` laissé à 0 elle
  **déborde la ligne et écrase le loader en `$0800`** → le crack Mr. Robot 4am
  (`.dsk` **et** `.woz`) calait (boot1 effacé → `$A0`, moteur tournant). Le
  même disque **bootait via l'autostart naturel**. Fix : après `clearRam`,
  `bootFromSlot` initialise la fenêtre texte (`$20-$23`), `CH/CV` (`$24/$25`),
  `BASL/BASH` (`$28/$29`=$0400), `INVFLG` (`$32`), et les hooks `CSW/KSW`
  (`$36-$39` → COUT1/KEYIN) — valeurs vérifiées contre un dump autostart→BASIC.
  Corrige **tous** les chemins (kiosk, GUI Disk Library, HDV) tout en gardant
  le ciblage de slot. Diagnostic via **MAME oracle** (flatpak `apple2p`, romset
  reconstitué depuis `roms/` — [[reference_mame_oracle]]) : MAME bootait
  l'image, POM2 non ; diff de trace POM2↔MAME puis bootFromSlot↔autostart →
  confounds éliminés un par un (profil, turbo, image, opcodes illégaux, phase
  d'échantillonnage, moniteur ROM, long-cycle, LSS) jusqu'à isoler l'init F8.
  Mr. Robot affiche son écran-titre ; Choplifter + Total Replay (HDV) bootent
  toujours (71/71 tests verts). Le **bug CPU INC/DEC** trouvé en chemin (cf.
  ci-dessous) était réel mais distinct — gardé.

## 2026-05-23

- **M6502 — INC/DEC mémoire : 1 cycle manquant corrigé** (`cpu_cycle_count`).
  `INC`/`DEC` en zp/abs/zp,X/abs,X chargeaient 1 cycle de trop peu (le
  cycle « dummy » read-modify-write : `INC abs` = 5 au lieu de 6). Les
  autres RMW (`ASL/LSR/ROL/ROR/TSB/TRB`) étaient déjà corrects (`+3`) ;
  seuls `INC`/`DEC` faisaient `+2`. Klaus ne l'attrapait pas (il teste les
  *résultats*, pas le *timing*). Trouvé via un diff de trace cycle-à-cycle
  contre MAME (`apple2p`, oracle reconstitué) sur le boot d'un loader
  sensible au timing : les cycles du boot0 collent désormais exactement à
  MAME. Réf MAME `om6502.lst` / `ow65c02.lst` (INC mem = 5/6/6/7). Affecte
  NMOS **et** 65C02. Nouveau test `cpu_cycle_count` épingle les timings RMW
  (premier test de *comptage de cycles* — leur absence avait laissé passer
  le bug). Voir `TODO.md § High [DiskII]` pour le contexte (Mr. Robot 4am).
- **ClockCard — interruptions TP (Timing-Pulse) câblées** (`clock_card_smoke`,
  5 nouveaux cas). La sortie TP de l'uPD1990AC bascule à une cadence
  sélectionnable et la ThunderClock+ la route (via une bascule d'activation)
  vers la ligne IRQ du slot — source d'interruption périodique pour les
  utilitaires horloge/scheduler, qui ne tickaient jamais avant. *Rates* =
  source de vérité MAME `upd1990a.cpp:248-257` (modes TP) + `:176-181`
  (REGISTER_HOLD) : diviseurs 512/128/16/8 sur le XTAL 32.768 kHz →
  **64/256/2048/4096 Hz** (modes 4-7) ; SHIFT/TIME_SET/TIME_READ laissent
  TP inchangé (comportement normal-mode MAME). `programTpTimer()` décode le
  mode latché sur front montant STB, `setTpRate()` convertit en cycles
  CPU/bascule, `advanceCycles()` pilote la bascule et lève l'IRQ sur chaque
  front montant. *Câblage IRQ* = **POM2-original** (MAME `a2thunderclock.cpp`
  ne lie jamais `tp_callback`) suivant le **manuel ThunderClock Plus ch. V** :
  `$C0n0` bit 6 (`$40`) = latch d'activation ; tout accès device-select
  efface la requête (le latch persiste → tick continu) ; lecture `$C0n0`
  bit 5 = drapeau « interrupt asserted » (polling manuel 5-3) ; RESET
  désactive. **Hors portée** : les interval timers (1/10/30/60 s, modes
  8-15) exigent la commande série 4-bit de l'uPD4990A, inatteignable sur
  l'uPD1990AC parallèle de la carte. Voir `DEV.md § ProDOS clock card`.
- **Disk II 13-secteurs (pré-DOS 3.3) — DOS 3.2 boote** (`dos32_boot_trace`,
  `d13_roundtrip_smoke`). Support complet des disquettes DOS 3.1/3.2/3.2.1 :
  détection `.d13` / 116480 o → `ImageKind::Dos32_13` (+ détection 13s des
  `.nib` par scan `D5 AA B5`), encode/decode GCR **5-and-3**
  (`nibblizeTrack13`/`decodeTrack13`, port verbatim MAME
  `formats/ap2_dsk.cpp` `a2_13sect_format` — `kTranslate5`, prologue
  adresse `D5 AA B5`, champ data 411 nibbles), write-back,
  `sectorsPerTrack_`/`is13Sector()`. `DiskIICard` sert la boot PROM
  **341-0009** (`roms/disk2_13.rom`) quand une 13s est montée
  (`serving13_`), force la LSS bit-level, et **lit via le séquenceur P6
  16-secteurs** (341-0028) — la LSS est agnostique de l'encodage, le
  décodage 5-and-3 est logiciel (boot/RWTS) ; le dump brut 341-0010 ne
  pilote pas `lssSync`. Un vrai master **DOS 3.2 boote** (`DOS32STD.d13`)
  via l'Autostart ][+ jusqu'au prompt `]` (greeting « LANGUAGE NOT
  AVAILABLE » = HELLO Integer sur II+ Applesoft). Round-trip encode↔decode
  pinné byte-pour-byte sur 35 pistes ; boot pinné end-to-end. Images de
  test dans `disks/dsk/` (DOS32STD.d13, DOS32PLS.D13, dos32std.nib…).
- **HDV mass-storage — pinning 32 MB** (`hdv_mass_storage_smoke_test`).
  Nouveau smoke test couvrant les trous restants du stockage de masse
  ProDOS : borne de capacité (65536 blocs = 32 MiB acceptés, 65537
  rejetés, fichier non-multiple de 512 rejeté), adressage bloc **16-bit**
  (l'octet haut du sélecteur `$C0D1` — un volume 32 Mo a besoin de blocs
  jusqu'à 65535, les autres tests ne touchaient que les blocs 0-1), et
  les `.2mg` avec data-offset ≠ 64. La préservation en-tête/trailer/WP
  au write-back était déjà pinnée par `hdv_writeback_smoke_test`. Reste
  hors scope : les images **multi-partition** (CFFA3000-style, 1 image =
  N volumes ProDOS) — feature, pas un test. Voir `DEV.md § HDV`.
- **Memory — stratégie de profil //c-class extraite** (`MemoryProfile`).
  Toutes les spécificités //c / //c+ qui fuyaient dans le dispatcher
  générique `Memory::memRead/memWrite` (alt-firmware `$C028` ROMBANK,
  IWM on-board `$C0E0-$C0EF`, MIG `$CC00/$CE00`, INTCXROM forcé, alt
  firmware sur `$C100-$FFFF`) passent derrière une interface
  `MemoryProfile` (`MemoryProfile.h` + `MemoryProfile_IIcClass.{h,cpp}`).
  `Memory::iicProfile_` est non-null **uniquement** sur //c/c+
  (créé/détruit dans `loadAppleIIRom`) ; II/II+/IIe → un seul
  `if (iicProfile_)` sur le hot path, **zéro virtual call**. `migRead`/
  `migWrite` (MAME `apple2e.cpp:532-624`) déplacés verbatim dans le
  profil. Les façades `setIWM/setSmartPortHub/setIWMAuthoritative`
  forwardent vers le profil (câblage des tests inchangé = garde-fou).
  Supprime les prédicats `isIIcClass`/`isIIcPlus`/`iicHasAltBank`
  éparpillés ; prérequis du split `Memory` god-object restant
  (`Keyboard`/`PaddleInputs`) et du futur profil IIgs. Vérifié : 62/62
  ctest + boot //c (U4boot.dsk → écran-titre) + banner //c+. Voir
  `DEV.md § MemoryProfile`.

## 2026-05-16

- **Cassette auto-rewind opt-in** (`3f42efc`). L'auto-rewind 500 ms
  (POM2-only, MAME ne rewind jamais) cassait les loaders custom à
  polling sporadique de `$C060` (Penguin Software fast loaders, etc.).
  Désormais gaté derrière `autoRewindEnabled`, **default off**
  (`CassetteDevice.cpp:470`). Setter `setAutoRewindEnabled(bool)`,
  toggle UI dans le panel Cassette.
- **SSC — LF→CR symétrique sur RX** (`3f42efc`). La normalisation
  CR LF → CR + bare LF → CR + drop NUL est appliquée **une fois**
  sur `scratch[]` brut avant que les bytes ne soient livrés à `rxBuf`
  ET au `keyboardSink` (`SuperSerialCard.cpp:91-104, 225-246`).
  Avant : les terminal apps lisant via `$C0A8` voyaient des paires
  CRLF et des LF orphelins.
- **SSC — raw mode toggle** (`3f42efc`). Nouveau toggle `rawMode_`
  qui bypasse `swallowTelnetIac` ET `normalizeLineEndings` côté RX
  pour les protocoles binaires 8-bit (XMODEM/Kermit/ADTPro). UI
  toggle dans le panel SSC, persisté via `ssc_raw_mode`.
- **Mockingboard — compteur de tons AY entier** (`3f42efc`). Les
  compteurs de phase des 3 canaux de tons (et du bruit) passent de
  `float` à `uint16_t` + accumulateur fractionnaire séparé (MAME
  `ay8910.cpp:998-1015` utilise des compteurs uint). Le float pur
  accumulait une erreur d'arrondi dans la boucle de résolve
  `while (counter >= period)`, ce qui faisait dériver/aliaser les
  périodes 1-3 — les tricks PWM (style Cosmic Bouncer) sonnaient faux.
  Compteur entier ⇒ plus de dérive (`Mockingboard.cpp:495, 607-617`).
  NB : pas encore pinné par un test (synthèse sur le thread audio).
- **SmartPortCard — refactor multi-unités** (`7db6861`). Chaque
  carte SmartPort possède maintenant ses propres `SmartPortUnit`
  polymorphes (3.5" Sony 800K ou ProDOS HDV au choix par unité), avec
  buffers de bloc par unité (plus de `thread_local`). Per-unit settings
  `smartport_slotN_unitK_{type,path,writeback}`. Pinné par
  `smartport_card_smoke_test.cpp` + `smartport_mixed_units_smoke_test.cpp`.
- **Disk Library — locale char ROM + eject** (`71cec5d`). Sélection
  de locale pour le character ROM dans la Disk Library + bouton
  d'éjection.
- **Audio peak monitoring** (`af4fad5`, `fad8e13`). `AudioDevice`
  suit les peaks par source (chaque carte) en plus du master peak.
  Diagnostic des saturations / cards qui dominent le mix.
- **DiskII multi-instances (option C)** (`5f7f209`). `MainWindow`
  héberge `std::vector<DiskIICard*> diskCards` ; permet
  `Disk II slot 6 + Disk II slot 4` (4 drives 5.25" sur un //e).
  Per-slot settings `disk_path_slotN`.
- **Shamus DHGR — entrée erronée retirée**. Shamus est un jeu HGR
  (Synapse 1982) qui tourne correctement ; l'item TODO précédent était
  basé sur une confusion HGR/DHGR ou déjà résolu silencieusement.

## 2026-05-14 / 2026-05-15

### UI

- **UI Toolbar + Disk Library unifiée**. Nouveau `Toolbar_ImGui` pinné
  sous la menu bar (10 boutons d'accès rapide + 2 combos), nouveau
  `DiskLibrary_ImGui` (panneau 3-onglets avec recherche + tri + table
  + markers d'images montées + click→insert/mount + boot). Toggles
  persistés.

### SmartPort 3.5" pluggable

- **SmartPortCard slot //e**. Nouveau `SmartPortCard.{h,cpp}`, slot 5
  par défaut, expose les 2 `Disk35Image` d'`EmulationController` en
  block-device ProDOS via slot ROM signée + dispatcher $Cn50 +
  soft-switches $C0nX. Driver $Cn50 examine `$43` bit-7 pour router
  drive 1 / drive 2. PR#N boote drive 1. Pinné par
  `smartport_card_smoke_test.cpp`.
- **Sons mécaniques 3.5"**. `Sony35Drive` reçoit un `FloppySoundSink*`
  (réutilise le `FloppySoundDevice` du Disk II). `seekPhaseW(uint8_t,
  uint64_t emuCycles)` propagé via `SmartPortHub` depuis
  `IWMDevice::emuCycles()`. `strobeWriteRegister` émet step / motor on
  / motor off / click avec stamping cycle CPU émulé (parité MAME).
- **Disk35 panel UX**. LED moteur rouge par drive, marqueur `* ` sur
  les images chargées, write-back checkbox per-drive, left-click =
  mount + cold boot, right-click = menu contextuel drive 1/2 × boot/
  no-boot.

### Disques

- **cc65-Chess.po hang après `LOADING CHESS`** résolu (2026-05-15).
  Les fixes round 1 (`kSyncMinRun=5` + `revolutionStartLssCycle`
  per-drive) combinés aux corrections de chemin de boot (B1-B5)
  suffisent — le `JSR $A403` ne tape plus sur `$00`.
- **Floppy sound step cadence** mesurée en cycles CPU émulés (MAME
  `floppy_sound_device::step` via `machine().time()`,
  `floppy.cpp:1532-1620`), pas en frames audio. **Pourquoi** : en
  turbo 60×, toutes les ~80 pulses du PROM de boot tombaient dans le
  même buffer audio de 5 ms → `audioFrameCounter_` constant →
  fallback `STEP_1_1` avec `stepPos_=0` réinitialisé par event →
  l'attaque de `step_1_1` (5 ms) rejouée buffer après buffer = buzz /
  son haché. `FloppySoundSink::step()` prend maintenant `emuCycles`,
  `DiskIICard::seekPhaseW` passe son `cpuCycleTotal`. Pinné par
  `floppy_sound_smoke_test.cpp` (`testRapidStepsNoHang` +
  `testSameCycleStepsClampGracefully`).

### MAME-parity round 1 (Mockingboard / IIe / //c)

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
  (`revolutionStartLssCycle[2]` + `kNeverRev`). MAME
  `wozfdc.cpp:264-291` + `floppy.cpp:809-839 mon_w`.
- **Floating bus** port verbatim `scanner_address` (MAME
  `apple2video.cpp:124-201`). addend0=0x0D + h-carries + v_4/v_3
  doublés dans addend2 + HBL phantom row $1000 gated sur !//e.
- **LOW-1** AY LFSR re-seed sur PB2=0 reset (MAME `ay8910.cpp:1309`
  `m_rng=1`) via cross-thread `ayResetCount_` signal.
- **LOW-2** VIA T1 continuous reload `+2` → `+3` (MAME `IFR_DELAY`).
- **LOW-3** AY register strobe fire sur tout call (drop debounce).
- **LOW-4** AY `reset_w` wipe regs 0..13 (R14/R15 préservés, MAME
  `ay8910_reset_ym`).

### RamWorks III

- Port verbatim `a2eramworks3.cpp`, jusqu'à 8 MB, pinné par
  `ramworks_smoke_test.cpp`.

### Coordination boot paths

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
