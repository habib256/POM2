# POM2 — TODO

État au 2026-05-14. Backlog ouvert. Architecture → `CLAUDE.md` ;
historique → `git log`.

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

## Disques

- [ ] 🟠 **cc65-Chess.po hang après `LOADING CHESS`** — MLI READ
      retourne `trans_count = $646D` (= 25709, EOF complet) mais les
      151 derniers octets de CHESS manquent en RAM : `$A3F7-$A3FF`
      (queue de T11 sec $0A, ProDOS block 90 hi half) plus
      `$A400-$A4FF` tout entier (T11 sec $0C, ProDOS block 91 lo
      half). chess.system `JSR $A403` à `$4003` tape sur `$00` =
      BRK, vecteur `$FFFE/F` → Apple Monitor, écran figé. Repro :
      `disk_path=../disks/dsk/cc65-Chess.po`, `system_profile=iie`,
      `ai_control_enable=true`, GET `/mem?addr=41984&len=256` → tout
      à `$00`.
      Investigation 2026-05-14 : le bit stream est byte-perfect
      (décodeur stand-alone qui scanne `bitStream` directement,
      0/4096 octets de différence sur T11). La corruption apparaît
      uniquement au passage par la LSS state machine. Première piste
      (heuristique `isInFFRun` du `expandTrackBits` qui ajoutait 2
      cells zéro à toute paire `$FF $FF`, dont la paire encodant le
      checksum d'address field quand `vol ^ track ^ sector == $FF`)
      écartée : seuil porté à `≥5` → bit stream reste correct, mais
      le runtime LSS produit les mêmes 151 octets manquants. Donc
      le bug n'est pas dans le bit stream.
      Vraie cause suspectée : `DiskIICard` n'a qu'un compteur LSS
      global (`lssCycle`) remis à `cpuCycleTotal*2` à chaque
      `selectDrive` / `lssStart`. MAME wozfdc garde un
      `revolution_start_time` *par drive*, conservé au-travers des
      head steps intra-drive. Sans ça, des transitions
      track-to-track ou des catch-ups asymétriques décalent le
      shift register d'un cell ou deux après plusieurs révolutions
      — manifeste sur le dernier bloc lu d'une longue série (sec
      qui tombe au mauvais offset angulaire). Refactor non-trivial :
      split `lssCycle` → `revolutionStartLssCycle[drive]` + recalc
      à chaque `selectDrive` / `seekPhaseW`, en respectant le timing
      relatif MAME `floppy_image_device::set_write_splice` /
      `revolution_start_time`. Tests à étendre : `diskii_lss_smoke`
      + un round-trip `DiskIICard ↔ DiskImage` sur plusieurs
      révolutions et plusieurs head steps.
      Workaround temporaire : utiliser une copie HDV (cc65-Chess
      existe aussi en `.2mg`) sur le slot 5, ou skipper l'image en
      attendant le refactor.
- [ ] 🟡 **WOZ2 `optimal_bit_timing` (INFO+39) ignoré**. Hard-codé à
      32 ticks = 4 µs/cell. MAME ne le lit pas non plus au load mais
      utilise `track_size*2` ; vrai gap = placement `cellIdx*8+4` fixe.
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
- [ ] 🟢 **RAMWorks 1 MB** (IIe-only, ProDOS 3+).
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

## Hors scope

- 80-col / IIe-only sur machine sans ROM IIe
- IIgs / ProDOS 16
- Disk II 13-secteurs (Apple II 1977-1979, pré-DOS 3.3)
- Z-80 SoftCard CP/M
- CFFA CompactFlash (POM2 a déjà ProDOS HDV slot 5 + host folder)
