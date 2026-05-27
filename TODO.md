# POM2 — TODO

État 2026-05-26. Backlog dédupliqué, groupé par sévérité (🟠 high ·
🟡 medium · 🟢 low) puis buckets non-bug. Tag `[sous-système]`.
Items résolus → `CHANGELOG.md`. Refs MAME → `DEV.md`.

## Parité MAME ↔ POM2 (dashboard)


| #  | Sous-système                  | Parité          | Refs MAME                                                                | Gaps connus                                                                                                                               |
| ---- | -------------------------------- | ------------------ | -------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| 1  | M6502 / 65C02 / Rockwell / WDC | Verbatim         | `om6502.lst`, `ow65c02.lst`                                              | 🟢 $5C 8-cyc résiduel ; style hérité                                                                                                   |
| 2  | Memory + IIe + RamWorks        | Partial-verbatim | `apple2e.cpp:1275-1299`, `a2eramworks3.cpp:108-115`                      | 🟠 god-object (Keyboard/PaddleInputs à extraire)                                                                                         |
| 3  | Display HGR/DHGR/80-col        | Partial-verbatim | `apple2video.cpp:124-201`, `460-471`, `:751-758`                         | 🟢 mono DHGR 1-px, floating-TTL, per-scanline DHGR switch ; ChatMauve idx 5≢10 (vs AppleWin, délibéré)                                |
| 4  | SpeakerDevice                  | Verbatim         | `spkrdev.cpp:74-327`                                                     | —                                                                                                                                        |
| 5  | CassetteDevice                 | POM2-original    | `apple2.cpp:362`                                                         | —                                                                                                                                        |
| 6  | Mockingboard (6522 + AY)       | Partial-verbatim | `ay8910.cpp:998-1015`, `:1077-1104`, `1309`                              | 🟢 Port A read mask par DDR ; 6522 subset (T2/SR/PCR)                                                                                     |
| 7  | FloppySoundDevice              | Verbatim         | `floppy.cpp:1532-1620`, `:2925-3020`                                     | —                                                                                                                                        |
| 8  | SlotBus + IRQ wire-OR          | POM2-original    | Pattern MAME slot bus                                                    | —                                                                                                                                        |
| 9  | DiskImage                      | Partial-verbatim | `woz_dsk.cpp`, `flopimg.cpp:2017-2106`                                   | 🟡 WOZ1 splice TRK+6650 ; 🟢 .nib2/.app, half-tracked NIB (88)                                                                            |
| 10 | DiskIICard                     | Partial-verbatim | `machine/wozfdc.cpp:264-291`, P6 PROM 341-0028-A                         | 🟢 sub-instruction RAII vs per-cycle ; Disk II hors snapshot délibéré                                                                  |
| 11 | IWMDevice                      | Verbatim         | `machine/iwm.cpp:1-543`                                                  | 🟢 Q3 fast clock (Mac/IIgs only) ;`read()` sans side-effects-disabled gate (debugger lit RAM direct, jamais $C0Ex) ; window-size rounding |
| 12 | SmartPortCard (//e Liron)      | POM2-original    | Spec SmartPort + Apple Tech Note                                         | 🟢 multi-partition ProDOS (CFFA3000)                                                                                                      |
| 13 | SmartPortHub + Sony35Drive     | Verbatim         | `apple2e.cpp:638-679`, `mac_floppy.cpp`, `flopimg.cpp:512/967/2017-2106` | —                                                                                                                                        |
| 14 | CFFA (MAME-faithful IDE)       | Verbatim         | `bus/a2bus/a2cffa.cpp`                                                   | 🟢 CHD = phase 2 ; pas de préservation média au switch de profil                                                                        |
| 15 | ClockCard / ThunderClock+      | Partial-verbatim | `upd1990a.cpp:248-267`, `:312-327`                                       | 🟡 MODE_SHIFT lax (vs MAME strict, divergence ProDOS délibérée) ; 🟡 DATA_OUT live vs MAME latch ; 🟢 slot ROM mostly NOPs             |
| 16 | SuperSerialCard                | Partial-verbatim | `mos6551.cpp:46`, `:542-543`, `a2ssc.cpp:373`                            | 🟢 IRQ gate SW2:6 DIP non gated                                                                                                           |
| 17 | MouseCard (MAME)               | Verbatim         | `bus/a2bus/mouse.cpp`, M68705 + MC6821                                   |  🟢 PIA out_a/b sans`scheduler.synchronize`                                                                                               |
| 18 | MouseCard (AppleWin HLE)       | Verbatim         | AppleWin`source/MouseInterface.cpp`                                      | — (slot EPROM seul, MCU synthétisé)                                                                                                    |

## 🟠 High

- [ ] **[Memory] god-object** (`Memory.cpp/.h`). Reste à extraire
  `Keyboard` (FIFO + strobe + paste) et `PaddleInputs` (RC +
  boutons + Open/Solid Apple). `IIcPlusBank` déjà fait
  (`MemoryProfile`/`IIcClassProfile`). Prérequis profil IIgs ;
  réduit recompilations.

## 🟡 Medium

- [ ] **[Refactor] Cleanup `#if 0` Via6522/Ay3_8910 dans
  `Mockingboard.cpp`** (l. 92-452). Bloc de code déplacé vers
  `Via6522.h` + `Ay3_8910.h` ; le `#if 0` est dead-code en attente
  de revue avant suppression. Une fois validé, supprimer ~360
  lignes.
- [ ] **[SSI263] Phoneme PCM blob (audio v2)**. Chip model +
  EchoPlusCard structure complets (pin `ssi263_smoke` + `echoplus_card_smoke`,
  18+3 sub-tests), mais audio silencieux faute des 62 phonèmes
  pré-rendus. Deux paths : (a) importer `SSI263Phonemes.h` d'AppleWin
  (LGPL — implique POM2 distribué sous LGPL), (b) regénérer offline
  via espeak ou autre TTS libre. Décision license requise du
  mainteneur (POM2 n'a pas de LICENSE file actuellement).
- [ ] **[SSI263] MockingboardCard variant "Sound II"**. Ajouter un
  SSI263 optionnel à $C(s)40-$C(s)44 (et potentiellement $C(s)60
  pour la version dual-chip). Nécessite d'étendre `Via6522.h` pour
  modéliser CA1 edge → IFR.CA1 (le SSI263 A/!R drive CA1 sur
  MB-C). Plus invasif que Echo+ standalone, défèré au prochain
  commit. **Effort : ~1 j.**



- [ ] **[Disques] WOZ1 splice point (TRK+6650) ignoré**
  (`DiskImage.cpp:381-398`). IWM call site câblé
  (`iwm.cpp:218-221`), mais `DiskImage::setWriteSplice` reste
  un stub. Re-master parité Applesauce. **Effort : 1 j.**
- [ ] **[Disques] UI « Force DOS / Force ProDOS »** — backend prêt
  (`DiskImage::loadFile(path, SectorOrder)` à
  `DiskImage.cpp:212`), reste le bouton dans `DiskLibrary_ImGui`
  / `DiskController_ImGui`. **Effort : ~30 min.**
- [ ] **[SmartPort] ProDOS multi-partition** (feature) — 1 image =
  1 unit = 1 volume aujourd'hui. CFFA3000-style multi-volume
  non supporté. 32 Mo / 16-bit / `.2mg` variantes pinnés
  (`hdv_mass_storage_smoke` + `hdv_writeback_smoke`).
- [ ] **[Features] PADL(2)/PADL(3) binding host** (second stick,
  centrés 127, `JoystickInput.cpp:65-75`).
- [ ] **[Features] Mapping souris → paddles** : paddle 0/1 sur
  axe X/Y de la souris host (alternative aux pads).
- [ ] **[Features] Eve Color text mode (`$C0B9`)** — variante
  ChatMauve/Eve, attributs FG/BG par caractère. Stub
  `LeChatMauve_ImGui.cpp:200`.
- [ ] **[Features] Carte imprimante — export PDF**. La carte
  parallèle synthétique (`PrinterCard`) est en place (CHANGELOG
  2026-05-27) : spool host-side + UI « Save as .txt » + built-in
  slot 1 pour //c/+. Reste l'export PDF (renderer monospace ou
  libharu). Pin `printer_card_smoke` couvre déjà le ROM
  fingerprint + le flow CPU PR#1.
- [ ] **[Arch] Config éclatée** : env vars `POM2_*` + CLI flags +
  `Settings`. Centraliser dans un `Config` (env → CLI →
  Settings → defaults) et lister env vars dans `--help`.
  **Effort : 1 j.**
- [ ] **[Arch] stateMutex partagé CPU+UI**
  (`EmulationController.h:118`). MainWindow_Slots prend ce
  lock pendant plug/unplug — risque jitter audio. Partitionner
  long terme.
- [ ] **[Arch] Namespace `pom2::` incohérent** (54/105 fichiers
  top-level ; tests/ ne l'utilise pas). Migrer mécaniquement.
- [ ] **[Arch] `Memory::memRead` hot path** (cascade `if` 7
  niveaux, `Memory.cpp:1309-1437`). Table dispatch 256 entrées
  par page haute. Prérequis : extraction `IIcPlusBank`.
- [ ] **[WASM] IDBFS settings persistence**. Build WASM monte
  `/persistent` via IDBFS (`CMakeLists.txt:241`) mais `Settings.cpp`
  écrit toujours dans `$HOME` — `state.cfg` + `imgui.ini` ne
  survivent pas au reload. Router via `ResourcePaths` quand
  `__EMSCRIPTEN__`. **Effort : 2-4 h.**
- [ ] **[WASM] File picker / drop-zone disks** — bundling
  build-time uniquement (`POM2_WASM_BUNDLE_DISKS=ON` ou rien).
  Pas de mécanisme pour qu'un user upload un `.dsk`/`.woz`/`.hdv`
  dans le browser. Drop-zone HTML5 → `FS.writeFile('/uploads/…')` →
  `DiskIICard::insert`. **Effort : ~1 j.**

## 🟢 Low

- [ ] **[Floppy Emu] modes Dual-5.25" + Smartport-Unit-2** —
  hors scope v1 (4 modes principaux couverts).
- [ ] **[UI] Layout par défaut plus aéré** : ImGui Docking ou
  `SetNextWindowPos` cascade adaptative.
- [ ] **[UI] `isDuplicate` flagge cffa/smartport35 en double** dans
  la colonne d'assignation Slot Config — cosmétique, le plug
  reste single-instance par défaut.
- [ ] **[Audio] AY Port A read mask par DDR** (R14/R15 academic).
- [ ] **[SSC] IRQ gate SW2:6 DIP** non implémenté (MAME
  `a2ssc.cpp:373`).
- [ ] **[ClockCard] Slot ROM vide** (256 B signature + NOPs). Vrai
  ThunderClock+ = 2 KB driver RTS-able DOS 3.3 / Applesoft.
- [ ] **[DHGR] mono 1-px alignment**, **floating-TTL `empty_words`**,
  **per-scanline mode switching** — cosmétique / hors-bounds.
- [ ] **[Features] Le Chat Mauve EVE** (64 KB ext RAM + SPEC1/
  SPEC2/DASH/COL280), **Video-7 AppleColor RGB**, **Color
  killer Rev 1**, **Strapping RAM 4K→48K**.
- [ ] **[Disques] Half-tracked NIB (88)**, **Applesauce
  `.nib2`/`.app`**, **Disk II dans snapshot** — délibérément
  hors scope tant que WOZ couvre.
- [ ] **[Arch] M6502 style hérité** : commentaires FR/EN, casts
  C-style, `void(void)`. `clang-format` + `clang-tidy modernize-*` ciblé.
- [ ] **[Arch] `*Card` raw pointers dans MainWindow**
  (`MainWindow.h:97-103`). Pas de notification quand SlotBus
  replug. Observer pattern ou `controller.slotBus().peripheral(N)`.
- [ ] **[WASM] Touch input mobile**. GLFW3 sous Emscripten ne map
  pas touch → mouse hors-canvas ; tap sur menu ImGui ne fonctionne
  pas sur iPad/Android. Wrapper JS `touchstart/move/end` →
  `Module._inject_mouse_*`.
- [ ] **[WASM] Audio worklet tuning**. miniaudio backend Web Audio
  fonctionne mais latence par défaut (~150 ms) audible sur
  speaker click ; explorer un `AudioWorkletNode` custom ou
  réduire le buffer.

## Skips délibérés (documenté inline)

- 🟢 **[Memory] `$C040` STRB pas gated `!//c`** (MAME
  `apple2e.cpp:1927`) — aucun sink wired.
- 🟢 **[ClockCard] DATA_OUT live** vs MAME latch sur CLK edge en
  MODE_SHIFT (`ClockCard.cpp:193-200`) — strict casserait stock
  ProDOS.
- 🟢 **[MouseCard] PIA out_a/b sans `scheduler.synchronize`** (MAME
  `mouse.cpp:280-294`) — pas de race firmware-visible.
- 🟢 **[ClockCard] offset model vs MAME `set_time`** — équivalent
  comportementalement tant que `timeFn()` lock-step.
- 🔁 **[MAME] path drift refresher** ~6 mois pour suivre les
  renommages upstream (récent : `wozfdc.cpp` `bus/a2bus → machine`).

## Propositions / extensions

### SmartPort / stockage

- 🟢 **UDC (Apple 1991)** — 4 baies hétérogènes (3.5"/5.25"/HDV).
- 🟢 **Slinky / RamFAST RAM disk** — utilité limitée vs RamWorks III.
- 🟢 **Apple II SCSI Card** (670-0144) — ProDOSHardDiskCard couvre
  déjà le fonctionnel.
- 🟢 **Apple 3.5" Controller IWM-level** — refactor IWMDevice pour
  vivre attaché à un slot card (rare).

### Cartes MAME-fidèles (vraie ROM + bus émulé)

Complément aux modèles synthétiques (`DEV.md § ProDOSHardDiskCard` +
`§ SmartPortCard`). P1 (CFFA) **fait** (`CHANGELOG.md` 2026-05-24) ;
P2/P3 restent.

- [ ] 🟡 **P2 — Carte Liron / UniDisk 3.5 réelle (IWM en slot)**.
  Stack déjà là (`IWMDevice` verbatim, `Sony35Drive`, GCR zoné,
  `SmartPortHub`). Reste : `LironCard : SlotPeripheral` + ROM
  Liron (343S0001). **Bloqueur** : aucun dump ROM Liron public
  (MAME `a2iwm.cpp` « WANTED: no ROM dumps »). **Effort : ~8-12 h**
  hors sourcing ROM.
- [ ] 🟢 **P3 — Apple II SCSI / High-Speed SCSI + CHD**. MAME
  `a2scsi.cpp` (NCR 5380) / `a2hsscsi.cpp` (53C80). Gros lift
  pour besoin niche. **Effort : ~30-50 h.** Garder 🟢 sauf
  demande explicite CHD.

## Priorité conseillée (ROI)


| Rang | Item                          | Effort  | Impact                               |
| ------ | ------------------------------- | --------- | -------------------------------------- |
| 1    | [Memory] god-object split     | 2 j     | 🟠 Prépare IIgs + reduces recompile |
| 2    | [Disques] WOZ1 splice point   | 1 j     | 🟡 Applesauce remaster parity        |
| 3    | [Disques] UI Force DOS/ProDOS | ~30 min | 🟡 quick win, backend prêt          |

## Hors scope

- IIgs / ProDOS 16 · Z-80 SoftCard CP/M · CFFA CompactFlash (HDV +
  host folder suffit ; portage MAME-fidèle → P2/P3 ci-dessus).

## Changelog

Voir [`CHANGELOG.md`](CHANGELOG.md).
