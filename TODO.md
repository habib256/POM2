# POM2 — TODO

État 2026-05-27. Items résolus → `CHANGELOG.md`. Refs MAME → `DEV.md`.

**Format** : `🟠 high · 🟡 medium · 🟢 low` en tête d'item. Effort
indicatif en *italique*. Fichier/ligne en `backticks`. Lecture rapide :
[Quick wins](#quick-wins) puis [Backlog par sous-système](#backlog).

## Parité MAME ↔ POM2 (dashboard)

Référence canonique de ce qui est porté et avec quel niveau. Les
`Gaps connus` listés ici renvoient à des items détaillés dans le
[backlog](#backlog).

| #  | Sous-système                  | Parité           | Refs MAME / AppleWin                                                     | Gaps connus                                                                              |
| --- | ---------------------------- | ---------------- | ------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------- |
| 1  | M6502 / 65C02 / Rockwell / WDC | Verbatim         | `om6502.lst`, `ow65c02.lst`                                              | 🟢 $5C 8-cyc résiduel ; style hérité                                                     |
| 2  | Memory + IIe + RamWorks        | Partial-verbatim | `apple2e.cpp:1275-1299`, `a2eramworks3.cpp:108-115`                      | 🟠 god-object (Keyboard/PaddleInputs à extraire)                                         |
| 3  | Display HGR/DHGR/80-col        | Partial-verbatim | `apple2video.cpp:124-201`, `460-471`, `:751-758`                         | 🟢 mono DHGR 1-px, floating-TTL, per-scanline DHGR switch                                |
| 4  | SpeakerDevice                  | Verbatim         | `spkrdev.cpp:74-327`                                                     | —                                                                                        |
| 5  | CassetteDevice                 | POM2-original    | `apple2.cpp:362`                                                         | —                                                                                        |
| 6  | Mockingboard A/C (6522 + AY)   | Partial-verbatim | `ay8910.cpp:998-1015`, `:1077-1104`, `1309`                              | 🟢 Port A read mask par DDR ; 6522 subset (T2/SR/PCR)                                    |
| 6b | Mockingboard "C" Sound II      | POM2 + AppleWin  | `Mockingboard.h/.cpp` + `Via6522::setCa1NegativeEdge`                    | — (SSI263 à `$Cs40-$Cs44`, A/!R → VIA1.CA1)                                              |
| 7  | FloppySoundDevice              | Verbatim         | `floppy.cpp:1532-1620`, `:2925-3020`                                     | —                                                                                        |
| 8  | SlotBus + IRQ wire-OR          | POM2-original    | Pattern MAME slot bus                                                    | —                                                                                        |
| 9  | DiskImage                      | Partial-verbatim | `woz_dsk.cpp`, `flopimg.cpp:2017-2106`                                   | 🟡 WOZ1 splice TRK+6650 ; 🟢 .nib2/.app, half-tracked NIB (88)                           |
| 10 | DiskIICard                     | Partial-verbatim | `machine/wozfdc.cpp:264-291`, P6 PROM 341-0028-A                         | 🟢 sub-instruction RAII vs per-cycle ; Disk II hors snapshot délibéré                    |
| 11 | IWMDevice                      | Verbatim         | `machine/iwm.cpp:1-543`                                                  | 🟢 Q3 fast clock (Mac/IIgs only) ; window-size rounding                                  |
| 12 | SmartPortCard (//e Liron)      | POM2-original    | Spec SmartPort + Apple Tech Note                                         | 🟢 multi-partition ProDOS (CFFA3000)                                                     |
| 13 | SmartPortHub + Sony35Drive     | Verbatim         | `apple2e.cpp:638-679`, `mac_floppy.cpp`, `flopimg.cpp:512/967/2017-2106` | —                                                                                        |
| 14 | CFFA (MAME-faithful IDE)       | Verbatim         | `bus/a2bus/a2cffa.cpp`                                                   | 🟢 CHD = phase 2 ; pas de préservation média au switch de profil                         |
| 15 | ClockCard / ThunderClock+      | Partial-verbatim | `upd1990a.cpp:248-267`, `:312-327`                                       | 🟡 MODE_SHIFT lax ; 🟡 DATA_OUT live vs MAME latch ; 🟢 slot ROM mostly NOPs             |
| 16 | SuperSerialCard                | Partial-verbatim | `mos6551.cpp:46`, `:542-543`, `a2ssc.cpp:373`                            | 🟢 IRQ gate SW2:6 DIP non gated                                                          |
| 17 | MouseCard (MAME)               | Verbatim         | `bus/a2bus/mouse.cpp`, M68705 + MC6821                                   | 🟢 PIA out_a/b sans `scheduler.synchronize`                                              |
| 18 | MouseCard (AppleWin HLE)       | Verbatim         | AppleWin `source/MouseInterface.cpp`                                     | — (slot EPROM seul, MCU synthétisé)                                                      |
| 19 | Phasor (AE — 2×VIA, 4×AY)      | Verbatim         | MAME `a2bus/phasor.cpp` + AppleWin                                       | 🟢 EchoPlus mode (=7) routé comme Phasor natif ; stéréo deferred                         |
| 20 | SSI263 speech (chip model)     | AppleWin-faithful| AppleWin `source/SSI263.{h,cpp}` (MAME n'implémente pas)                 | 🟢 formant synth → PCM blob 62 phonèmes (AppleWin LGPL → GPL3)                           |
| 21 | EchoPlusCard (Street Elec)     | POM2-original    | Echo II/Echo+ hardware spec                                              | —                                                                                        |
| 22 | PrinterCard (parallèle synth)  | POM2-original    | Convention slot 1 Apple II + Pascal 1.1 sig                              | 🟡 export PDF deferred (`.txt` OK)                                                       |

## Quick wins

Ordre conseillé d'attaque — items à fort ratio impact/effort.

| # | Item                                    | Effort  | Pourquoi                                |
| - | --------------------------------------- | ------- | --------------------------------------- |
| 1 | WASM IDBFS settings persistence         | 2-4 h   | utilisateur web n'a pas de state        |
| 2 | Shader CRT post-process                 | 1-2 j   | polish visuel récurrent (V2/OE/μM8)     |
| 3 | WOZ1 splice point TRK+6650              | 1 j     | parité re-master Applesauce             |
| 4 | Memory god-object split                 | 2 j     | prérequis IIgs + réduit recompilations  |
| 5 | Debugger runtime glue (BP / watch / step) | 3-5 j | briques 80% là (Disassembler + MemView) |

## Backlog

Regroupé par sous-système. Sévérité encodée par 🟠/🟡/🟢 en tête d'item.

### [Memory] paging & RAM expansion

- 🟠 **God-object split** — extraire `Keyboard` (FIFO + strobe + paste)
  et `PaddleInputs` (RC + boutons + Open/Solid Apple) de `Memory.cpp`.
  `IIcPlusBank` déjà fait (`MemoryProfile`/`IIcClassProfile`).
  *Prérequis IIgs. ~2 j.*
- 🟡 **Saturn 128K LC** (Saturn Systems) — 16 banks ×16 KB sur LC
  `$D000-$FFFF`, switches `$C080-$C08F` slot-relatif. Refs MAME
  `bus/a2bus/a2memexp.cpp`. *2-3 j.*
- 🟡 **`Memory::memRead` hot path** — cascade `if` 7 niveaux
  (`Memory.cpp:1309-1437`). Table dispatch 256 entrées par page haute.
  Prérequis : extraction `IIcPlusBank`.
- 🟢 **Pascal LC dédiée** — variante 16 KB livrée avec Apple Pascal,
  différences mineures vs LC IIe (write-protect DIP). *1 j.*

### [Display] HGR / DHGR / 80-col

- 🟠 **Shader CRT post-process** — scanlines / phosphor glow / barrel
  curvature, fragment GLSL sur backbuffer `Apple2Display`, toggle UI.
  Présent dans Virtual ][, OpenEmulator, microM8, MAME. *1-2 j.*
- 🟡 **Pipeline NTSC analogique signal-level** — vs LUT actuelle.
  Composite YIQ, démodulation, courbe phosphore, bandwidth Y/I/Q.
  Référence académique OpenEmulator (cf. Zellyn Hunter explainer).
  Toggle UI vs LUT rapide. *5-10 j.*
- 🟡 **Beam-racing per-scanline composite** — recomposer le framebuffer
  scanline par scanline (mid-scanline mode switches). Per-scanline DHGR
  switch déjà côté code. Refs μM8 + OE. *3-5 j.*
- 🟡 **Eve Color text mode `$C0B9`** — variante Chat Mauve/Eve, FG/BG
  par caractère. Stub `LeChatMauve_ImGui.cpp:200`. *2 j.*
- 🟢 **Mode "smooth" sub-pixel interpolé** — bilinéaire/Lanczos sur
  HGR/DHGR, toggle UI. Inspiré microM8. *2 j.*
- 🟢 **DHGR mono 1-px alignment + floating-TTL `empty_words` +
  per-scanline mode switch** — cosmétique / hors-bounds.
- 🟢 **Le Chat Mauve EVE** (64 KB ext RAM + SPEC1/SPEC2/DASH/COL280),
  **Video-7 AppleColor RGB**, **Color killer Rev 1**,
  **Strapping RAM 4K→48K**.

### [Audio]

- 🟢 **8-bit DAC (Marczewski)** — latch slot 8-bit → DAC R-2R. Démos
  niche (Music Studio, trackers). Refs AppleWin `Card::CT_DX1`. *1 j.*
- 🟢 **Music Card MIDI Passport** — 6840 + 6850, Master Tracks Pro /
  Performer. Refs MAME `mc6840.cpp` + `acia6850.cpp`. *3 j.*
- 🟢 **Phasor stéréo** — mixer POM2 mono-only ; quand stéréo, pan L/R
  par AY-pair sur Phasor (et SSI263/Echo+).
- 🟢 **AY Port A read mask par DDR** (R14/R15) — academic.

### [Storage] disques & images

- 🟡 **WOZ1 splice point (TRK+6650)** — `DiskImage::setWriteSplice`
  est un stub (`DiskImage.cpp:381-398`) ; IWM call site câblé
  (`iwm.cpp:218-221`). Parité Applesauce re-master. *1 j.*
- 🟡 **SmartPort ProDOS multi-partition** — 1 image = 1 unit = 1
  volume aujourd'hui ; multi-volume CFFA3000-style non supporté.
- 🟢 **UI « Force DOS / Force ProDOS »** — backend prêt
  (`DiskImage::loadFile(path, SectorOrder)` à `DiskImage.cpp:212`),
  bouton manquant dans `DiskLibrary_ImGui` / `DiskController_ImGui`.
  Auto-détect (extension + content sniff vol-dir `0x400`/`0xB00`)
  couvre déjà 99 % des cas ; override manuel utile pour images
  ambiguës / non-standard / debug. *~30 min.*
- 🟢 **Half-tracked NIB (88)** + **Applesauce `.nib2`/`.app`** +
  **Disk II dans snapshot** — délibérément hors scope tant que
  WOZ couvre.
- 🟢 **Floppy Emu modes Dual-5.25" + Smartport-Unit-2** — hors scope
  v1 (4 modes principaux couverts).

### [Cards] cartes slot & périphériques

- 🟠 **Z-80 SoftCard + CP/M** — Microsoft SoftCard, Z-80B clipsé sur
  bus 6502, share RAM via mode-switch. Débloque ludothèque CP/M
  (BASIC-80, dBase II, Turbo Pascal, WordStar). Refs MAME
  `a2softcard.cpp` + Z-80 core. *10-15 j.*
- 🟢 **Grappler+ printer variant** — PrinterCard couvre 90 % ; manque
  les commandes d'impression d'écran HGR (CTRL-Y séquences). Refs
  MAME `a2grappler.cpp`. *2 j.*
- 🟢 **No-Slot Clock (NSC, DS1216E)** — clock qui se plogue sous une
  ROM (pattern recognition). Pour machines sans slot libre (//c).
  ThunderClock+ couvre le cas général. Refs MAME `ds1216.cpp`. *1 j.*
- 🟢 **SSC IRQ gate SW2:6 DIP** non implémenté (MAME `a2ssc.cpp:373`).
- 🟢 **ClockCard slot ROM** vide (256 B signature + NOPs). Vrai
  ThunderClock+ = 2 KB driver RTS-able DOS 3.3 / Applesoft.
- 🟡 **[P2] Liron / UniDisk 3.5 réelle (IWM en slot)** — stack déjà
  là (`IWMDevice` verbatim, `Sony35Drive`, GCR zoné, `SmartPortHub`).
  Reste `LironCard : SlotPeripheral` + ROM 343S0001.
  **Bloqueur** : aucun dump ROM public (MAME `a2iwm.cpp` *WANTED*).
  *~8-12 h hors sourcing ROM.*
- 🟢 **[P3] Apple II SCSI / High-Speed SCSI + CHD** — MAME
  `a2scsi.cpp` (NCR 5380) / `a2hsscsi.cpp` (53C80). Gros lift pour
  besoin niche (CFFA suffit). *~30-50 h.*
- 🟢 **UDC (Apple 1991)** — 4 baies hétérogènes (3.5"/5.25"/HDV).
- 🟢 **Slinky / RamFAST RAM disk** — utilité limitée vs RamWorks III.
- 🟢 **Apple 3.5" Controller IWM-level** — refactor IWMDevice attaché
  à un slot card (rare).

### [Cassette]

- 🟢 **WAV record/playback enrichi** — POM2 supporte .wav ; manque
  filtrage analogique tape (hiss, drop-out), VU-meter, timecode.
  Refs MAME `apple2.cpp` cassette. *2 j.*

### [Network]

- 🟠 **Uthernet I/II Ethernet TCP/IP** — débloque IRC/HTTP/telnet/FTP
  modernes. I = CS8900A NIC (`uthernet.cpp`) ; II = W5100 hardware
  stack (`uthernetii.cpp`). Backend host = libslirp ou TAP/TUN.
  *5-7 j.*

### [Printer]

- 🟡 **Export PDF** — `PrinterCard` spool + `.txt` OK ; reste renderer
  monospace ou libharu.

### [Input] joystick / paddles / mouse

- 🟡 **PADL(2)/PADL(3) binding host** — second stick centré 127
  (`JoystickInput.cpp:65-75`).
- 🟡 **Mapping souris → paddles** — paddle 0/1 sur axes X/Y de la
  souris host (alternative aux pads).

### [UI/UX]

- 🟢 **Layout par défaut plus aéré** — ImGui Docking ou
  `SetNextWindowPos` cascade adaptative.
- 🟢 **`isDuplicate` flagge cffa/smartport35 en double** dans la
  colonne d'assignation Slot Config — cosmétique.
- 🟢 **Touchscreen / virtual joystick on-screen** — joystick virtuel
  ImGui pour build WASM mobile (séparé du routage touch brut). Two
  thumb-sticks + boutons Open/Solid Apple. Inspiré microM8 / A2TS.
  *2 j.*

### [WASM]

- 🟡 **IDBFS settings persistence** — `/persistent` monté via IDBFS
  (`CMakeLists.txt:241`) mais `Settings.cpp` écrit dans `$HOME` ;
  `state.cfg` + `imgui.ini` ne survivent pas au reload. Router via
  `ResourcePaths` sous `__EMSCRIPTEN__`. *2-4 h.* ⭐ quick win
- 🟡 **File picker / drop-zone disks** — bundling build-time
  uniquement. Drop-zone HTML5 → `FS.writeFile('/uploads/…')` →
  `DiskIICard::insert`. *~1 j.*
- 🟢 **Touch input mobile** — GLFW3 sous Emscripten ne map pas
  touch → mouse hors-canvas. Wrapper JS `touchstart/move/end` →
  `Module._inject_mouse_*`.
- 🟢 **Audio worklet tuning** — miniaudio Web Audio fonctionne mais
  latence ~150 ms audible sur speaker click. Explorer
  `AudioWorkletNode` custom ou réduire le buffer.

### [Arch] refactor & tooling

- 🟡 **Config éclatée** — env vars `POM2_*` + CLI flags + `Settings`
  à centraliser dans un `Config` (env → CLI → Settings → defaults),
  lister env vars dans `--help`. *1 j.*
- 🟡 **`stateMutex` partagé CPU+UI** (`EmulationController.h:118`) —
  `MainWindow_Slots` prend ce lock pendant plug/unplug, risque
  jitter audio. Partitionner long terme.
- 🟡 **Namespace `pom2::` incohérent** — 54/105 fichiers top-level,
  `tests/` ne l'utilise pas. Migration mécanique.
- 🟢 **M6502 style hérité** — commentaires FR/EN, casts C-style,
  `void(void)`. `clang-format` + `clang-tidy modernize-*` ciblé.
- 🟢 **`*Card` raw pointers dans MainWindow** (`MainWindow.h:97-103`) —
  pas de notification quand SlotBus replug. Observer pattern ou
  `controller.slotBus().peripheral(N)`.

## Skips délibérés (documentés inline)

Divergences MAME conscientes, justifiées dans le code à l'endroit
concerné. Ne pas re-instruire sans relire le commentaire d'origine.

- 🟢 **`$C040` STRB pas gated `!//c`** (MAME `apple2e.cpp:1927`) —
  aucun sink wired.
- 🟢 **ClockCard DATA_OUT live** vs MAME latch sur CLK edge en
  MODE_SHIFT (`ClockCard.cpp:193-200`) — strict casserait stock
  ProDOS.
- 🟢 **MouseCard PIA out_a/b sans `scheduler.synchronize`** (MAME
  `mouse.cpp:280-294`) — pas de race firmware-visible.
- 🟢 **ClockCard offset model vs MAME `set_time`** — équivalent
  comportementalement tant que `timeFn()` lock-step.
- 🔁 **MAME path drift refresher** — repasser ~tous les 6 mois pour
  suivre les renommages upstream (récent : `wozfdc.cpp`
  `bus/a2bus → machine`).

## Hors scope

Choses qu'on ne fera pas, sauf demande explicite + ROI clair.

- **Apple IIgs / ProDOS 16** — nouveau projet (Mega II + FPI + GLU +
  Ensoniq DOC, *30-100 j*).
- **Apple ///** + SOS — niche, *20-40 j*.
- **Clones** Franklin / Laser / Pravetz / Basis 108 — *2-5 j/clone*,
  faible demande.
- **CFFA CompactFlash** — HDV + host folder suffit ; portage
  MAME-fidèle déjà couvert par P1 (CFFA fait), P2/P3 ci-dessus.

## Changelog

Voir [`CHANGELOG.md`](CHANGELOG.md).
