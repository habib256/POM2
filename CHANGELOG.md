# POM2 — Changelog

Historique des changements notables, organisé du plus récent au plus
ancien. Le `git log` reste la source canonique pour la mécanique
exacte ; ce fichier capture les **« pourquoi »** et les pièges qu'on
ne veut pas re-découvrir. Backlog actif → `TODO.md`. Implémentation
courante → `DEV.md`.

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
