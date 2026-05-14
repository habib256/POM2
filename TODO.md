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

## Disques

- [ ] 🟡 **cc65-Chess.po hang après `LOADING CHESS`** — symptôme :
      MLI READ retourne `trans_count = $646D` (EOF complet) mais les
      151 derniers octets de CHESS manquent (`$A3F7-$A4FF` à `$00`),
      chess.system `JSR $A403` tape sur `$00` = BRK → Monitor.
      Investigation 2026-05-15 : le bit stream est byte-perfect ET
      la LSS state machine *livre* les bons nibbles. Instrumenté
      `POM2_DEBUG_DISK_STREAM=/tmp/foo.csv` qui capture chaque
      `$C0EC` ; comparaison nibble-pour-nibble entre les 343 octets
      delivered par la LSS sur sec $0A/$0B/$0C/$0E de T11 et le
      buffer nibblisé interne : 0/343 diffs pour chaque secteur.
      → la corruption se produit DOWNSTREAM de la LSS. Suspects
      restants : (a) ProDOS RWTS 6-and-2 decoder côté CPU qui rate
      la première paire data-field-prologue+nibble pour sec $0C
      (timing 32-cycle boucle déroulée vs sub-instruction
      inflation RAII de POM2 — cf. l'item « Inflation
      sub-instruction RAII » plus bas ; les deux symptômes
      pointent dans la même direction) ; (b) cc65-chess.system
      loader qui pourrait demander 25600 B au lieu de 25709 B,
      cas où ProDOS s'arrêterait pile à `$A3F7` (à creuser via
      trap du MLI READ). Les deux fixes 2026-05-15 (kSyncMinRun=5
      + revolutionStartLssCycle per-drive) sont *nécessaires* mais
      *pas suffisants* pour cc65-Chess — ils ont par contre
      débloqué Shamus et H.E.R.O. sur `mario.dsk`. Workaround :
      copie HDV slot 5 ; cc65-Chess existe aussi en `.2mg`.
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

## Hors scope

- 80-col / IIe-only sur machine sans ROM IIe
- IIgs / ProDOS 16
- Disk II 13-secteurs (Apple II 1977-1979, pré-DOS 3.3)
- Z-80 SoftCard CP/M
- CFFA CompactFlash (POM2 a déjà ProDOS HDV slot 5 + host folder)
