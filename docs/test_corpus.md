# Corpus de test « cas limites » — logiciels réels pour valider l'émulation

> Liste curée de programmes (démos cycle-exactes, disquettes protégées,
> suites CPU) qui torturent les recoins de l'Apple II : synchro CPU↔vidéo
> au cycle près, flux magnétique brut, timings 6502 et IRQ. Sert de
> **backlog de tests manuels / d'intégration** au-delà des `ctest` unitaires.
>
> Origine : échange Gemini (2026-06) re-vérifié et croisé avec les
> sous-systèmes POM2 réels. Quand un programme cible un `Gap connu` du
> [dashboard de parité](../TODO.md#parité-mame--pom2-dashboard), le numéro `#`
> est cité. **Statut** = ce que POM2 fait *aujourd'hui*, pas une promesse.
>
> ⚠️ Toutes les images de jeux commerciaux sont **fournies par l'utilisateur**
> (comme les ROMs). Ce document ne référence aucun binaire ; il décrit *quoi
> tester et pourquoi c'est dur*.

> **⭐ Référence prioritaire — [DIX](https://github.com/Fr3nchT0uch/DIX/)**
> (anthologie French Touch, 29+ min de démos Apple II 2014–2024, sources GPLv3).
> C'est le **banc d'essai le plus complet** pour viser la perfection de
> l'émulation : vapor lock / bus flottant, bascules vidéo mid-scanline,
> Mockingboard + IRQ VIA, 128 KB aux, SmartPort/Liron + Unidisk 800 KB, cadence
> PAL 50 Hz. Si DIX tourne sans glitch, l'émulateur est au niveau « démo
> cycle-exacte » ; si DIX casse, le corpus ci-dessous indique *quel* sous-système
> creuser.

## Sommaire

- [1. Précision CPU↔vidéo (synchro au cycle)](#1-précision-cpuvidéo-synchro-au-cycle)
- [2. Enfer du contrôleur Disk II (flux / WOZ)](#2-enfer-du-contrôleur-disk-ii-flux--woz)
- [3. CPU & quirks matériels](#3-cpu--quirks-matériels)
- [4. Audio / Mockingboard (IRQ VIA)](#4-audio--mockingboard-irq-via)
- [Annexe — le Vapor Lock en détail](#annexe--le-vapor-lock-en-détail)
- [Corrections vs la source d'origine](#corrections-vs-la-source-dorigine)

---

## 1. Précision CPU↔vidéo (synchro au cycle)

L'Apple II n'a **pas de timer vidéo dédié ni d'IRQ VBL** (sur II/II+ ; le //e
ajoute `$C019` RDVBL en lecture seule). Toute la synchro fine repose sur le
**bus flottant** : lire une adresse I/O non pilotée renvoie le dernier octet
posé par le scanner vidéo (effet capacitif TTL). Voir
[annexe Vapor Lock](#annexe--le-vapor-lock-en-détail).

| Programme | Ce qu'il torture | Pourquoi c'est un cas limite | Statut POM2 |
|---|---|---|---|
| **deater — « megademos »** (Vince « deater » Weaver, `deater.net/weave/vmwprod`) | Vapor lock : détecte le VBL en bouclant sur une lecture `$C0xx` non pilotée jusqu'à lire un octet repère écrit en RAM vidéo. | Si la vidéo est un framebuffer rendu de façon asynchrone en fin de frame au lieu d'entrelacer lectures CPU et scanner au cycle, la boucle ne « verrouille » jamais → écran figé / glitché. | ✅ Base solide : `Memory::floatingBus()` est un **port verbatim** de MAME `apple2video.cpp:124-201 scanner_address`, indexé sur le `cycleCounter` global → l'adresse scanner suit le faisceau au cycle. Pinné par `floatingbus_page2_smoke_test` + `beam_race_composite_test`. À valider sur une vraie megademo. |
| **[DIX](https://github.com/Fr3nchT0uch/DIX/)** — anthologie French Touch (29+ min, //e / //c PAL, sources GPLv3) | **Suite d'intégration tout-en-un** : vapor lock, mid-scanline, DHGR/NTSC, Mockingboard, 128 KB aux, Unidisk 800 KB via Liron/SmartPort. Regroupe *Mad Effect*, *Plasmagical*, *Wave* et les autres prods FT récentes. | Un seul disque qui enchaîne les cas limites des §1–4 ; la référence à viser avant de déclarer l'émulation « parfaite ». **Requiert PAL 50 Hz (pas NTSC).** | 🟡 **Priorité #1**. Rendu mid-scanline ✅ (cf. ligne suivante). **Bloqueur #1 = timing PAL 50 Hz absent** (voir analyse datée ci-dessous). Sondes : `dix_modpage_split`, `horizontal_split*`, `dhgr_phase_signal`, `floatingbus_page2_smoke`. |
| **Productions « French Touch »** (ex. *Mad Effect*, *Plasmagical*, *Wave* — incluses dans DIX) | Changements de **mode vidéo en milieu de scanline** (mid-scanline : TEXT↔HGR, PAGE1↔2, lo↔hi-res entre deux cycles d'une même ligne). | Exige un 6502 découpé en **vrais sous-cycles d'accès** : un opcode exécuté atomiquement (effets appliqués en un bloc) décale le commutateur de 1-2 cycles → bandes de couleur mal placées. | ✅ **Rendu intra-ligne fait (2026-06-09)** : `Apple2Display::renderBeamRacing` rejoue le log d'events au **byte-column** près (`frameCycleToPos`), splits horizontaux TEXT/HGR/LORES/DHGR/80-col **et** PAGE1↔2 / ALTCHAR sur la même ligne, en RGBA *et* signal composite. Sondes : `horizontal_split`, `horizontal_split_composite`, `horizontal_split_560`, `dix_modpage_split`, `dhgr_phase_signal`, `artifact_phase_probe`. *Le cycle de transition exact au character-clock reste un raffinement.* Détail → `DEV.md` § Beam-racing. |
| **Démos DHGR / `dapple`-like + tests d'artefact NTSC** | Ordre d'évaluation des soft-switches DHGR (`80STORE`/`PAGE2`/`HIRES`/`AN3`) et frangeage couleur (artifacting NTSC par entrelacement de signaux). | Valide l'ordre exact des switches Le Chat Mauve (FIFO AN3 → `$C05E/F`) et la démodulation composite. | ✅/🟡 Pipeline NTSC composite (`NtscPostProcessor`, `AppleWinNtsc`) + chemins CPU/GPU. Couvert par `dhgr_render_smoke_test`, `oe_demod_gpu_cpu_parity_test`, `display_golden_hash_test`. Gap résiduel : mono DHGR 1-px, floating-TTL (`#3`). |

### Analyse DIX au niveau source — 2026-06-09

Lecture de la source GPLv3 ([Fr3nchT0uch/DIX](https://github.com/Fr3nchT0uch/DIX/),
ex. `MADEF2/main.a`) pour cadrer la validation. La boucle phare (`INT_ROUT1`,
page-alignée, jouée sur la dernière ligne VBL) fait, **chaque scanline de
65 cycles** et sur 6 lignes :

```asm
MODPAGE0  LDA $C054,X          ; PAGE1/PAGE2 mid-ligne (X = offset de scroll)
MODLINE0  LDA $C056 (×11)      ; HIRES mid-ligne, ~44 cycles
```

Elle est **synchronisée par une IRQ Timer-2 du Mockingboard** :
`DEFAULT_SYNC_TIMER = 7479 ; IRL machines PAL`.

Conséquences pour POM2, séparées proprement :

1. **Rendu mid-scanline (PAGE/HIRES/mode) — ✅ FAIT.** Le beam-racing par
   byte-column rejoue ces bascules à la bonne colonne. **Bug trouvé + corrigé
   en cours de validation** : les painters RGBA (`renderText/HiRes/LoRes`)
   relisaient `mem.getDisplayState()` en interne → la sélection **PAGE2** (et
   `ALTCHAR`) utilisait l'état de *fin de frame*, pas celui de la bande. Corrigé
   en passant le `state` par bande aux painters (le chemin composite le faisait
   déjà). Pinné par `dix_modpage_split` (la technique MODPAGE exacte : page 1 à
   gauche, page 2 à droite, même ligne).
2. **IRQ Timer-2 Mockingboard — ✅ supporté** (`Via6522` T2 one-shot phase-2,
   `IFR_T2`/`t2Counter`). L'IRQ de sync *se déclenche*.
3. **Timing machine PAL 50 Hz — ❌ BLOQUEUR #1.** POM2 est **NTSC seul**
   (`kScanlinesPerFrame = 262`, 17045 cyc/frame ; le « PAL » du
   `NtscPostProcessor` n'est qu'un mode couleur shader, pas le timing machine).
   `DEFAULT_SYNC_TIMER=7479` et la géométrie 312 lignes PAL placent l'effet
   verticalement et cadencent la musique pour 50 Hz ; sur 262 lignes NTSC,
   l'effet est mal positionné / roule et le tempo est ~20 % trop rapide. **C'est
   le pré-requis pour une vraie validation DIX bout-en-bout** (à ajouter au
   backlog comme machine-timing PAL : 312 lignes, 1.0157 MHz, refresh 50 Hz).

Boot : DIX boote en **800 KB ProDOS via Unidisk/SmartPort** (`boot_unidisk.a`) ;
le SmartPort host-served de POM2 (slot 5 sur //e/c) couvre ce chemin. *(Test
visuel en direct non réalisable dans l'environnement agent headless — pas
d'affichage GLFW ; validation faite au niveau source + sondes unitaires.)*

---

## 2. Enfer du contrôleur Disk II (flux / WOZ)

L'émulation **secteur logique** (`.dsk`, `.po`) ne suffit pas : ces titres
exigent le **flux magnétique brut** (`.woz`) + le comportement du moteur
pas-à-pas et de la rotation 300 RPM.

| Programme | Protection | Pourquoi c'est un cas limite | Statut POM2 |
|---|---|---|---|
| **Captain Goodnight and the Islands of Fear** (Broderbund) | **Spiradisc** : données écrites sur une **spirale continue** (piste `$01`→`$0E`), pas en cercles concentriques. | Le contrôleur doit suivre les déplacements de tête **« à la volée »** pendant que le flux défile ; un LSS qui resynchronise par piste plante au boot. | 🟡 LSS event-driven + WOZ bit-stream présents (`DiskIICard`, `DiskImage`, `#9/#10`). Demi-pistes gérées ; suivi spiral continu **à valider** sur image WOZ réelle. Tests proches : `woz_bit_timing_smoke_test`, `diskii_lss_smoke_test`. |
| **Prince of Persia** (Broderbund / Roland Gustafsson) | **RWTS18** : quarter-tracks, sync-bytes modifiés, timing-bits / weak bits. | La vitesse de rotation, l'espacement des sync-nibbles et l'interprétation des weak bits doivent être cohérents avec les cycles 6502 → sinon échec de lecture des pistes protégées. | 🟡 WOZ + timing bit-cell event-driven (cf. `CLAUDE.md` *« disk-turbo »* + `emuCycles`). Weak/fake bits dépendent du master WOZ. Pinné côté flux : `woz_writeflux_smoke_test`, `woz_bit_timing_smoke_test`. `Gap #9` : WOZ1 splice TRK+6650. |
| **Disquettes « bus flottant comme RNG »** (protections Beagle Bros, certaines démos) | Utilisent l'octet du bus flottant comme graine aléatoire. | Exige une réplication **bit-exacte** du compteur scanner (HBL inclus, « $1000 phantom row »). | ✅ Géré par le port verbatim `floatingBus()` (cf. commentaire `Memory.cpp:1486+`). C'est précisément le cas d'usage cité dans le code. |

---

## 3. CPU & quirks matériels

Le socle doit être irréprochable **avant** que les démos vidéo ne passent.

| Programme | Ce qu'il valide | Statut POM2 |
|---|---|---|
| **Klaus Dormann — `6502_functional_test`** | Juge de paix 6502 : franchissement de page (+1 cycle), flag décimal (D) exact, etc. | ✅ `test_klaus_6502` **PASSE**. Binaire auto-téléchargé + SHA256 vérifié (`tests/CMakeLists.txt`). |
| **Klaus Dormann — `65C02_extended_opcodes_test`** | Opcodes étendus 65C02 (BBR/BBS/RMB/SMB, `STZ`, `(zp)`, etc.). | ✅ `test_klaus_65c02` **PASSE** @ `$24F1` (cf. `DEV.md` §CPU). |
| **Suites « illegal opcodes » NMOS** (visual6502-derived) | Comportement des opcodes non documentés du 6502 NMOS. | 🟢 Partiellement — `#1` note un *« $5C 8-cyc résiduel »*. Couvre surtout le sous-ensemble utilisé en pratique. Compléter via `cpu_cycle_count_test`. |

> 113 tests `ctest` au total (Klaus 6502+65C02, `cpu_cycle_count`, disque,
> vidéo, audio…). Cf. `TODO.md` Quick-win #5 (CI GitHub Actions headless).

---

## 4. Audio / Mockingboard (IRQ VIA)

Stress-test des **IRQ matérielles** : les timers des VIA 6522 du Mockingboard
ne doivent ni désynchroniser le bus principal, ni rater leur acquittement.

| Programme | Ce qu'il torture | Statut POM2 |
|---|---|---|
| **Ultima V: Warriors of Destiny** (Origin) | Musique Mockingboard pilotée par IRQ timer VIA en continu pendant le jeu. | ✅/🟡 Mockingboard A/C (2×VIA + 2×AY) verbatim (`#6`, `ay8910.cpp`, `Via6522`). IRQ wire-OR via `SlotBus` (`#8`). À écouter en conditions réelles. |
| **Music Construction Set / Willy Byte / Rescue Raiders** (titres Mockingboard confirmés) | Séquençage AY-3-8910 + cadence IRQ. | ✅/🟡 Même chemin que ci-dessus. Bon banc d'essai pour la justesse des timers T1/T2. |
| **Phasor / SSI263 (parole)** | 2×VIA + 4×AY (Phasor), synthèse formant SSI263. | ✅ `PhasorCard` verbatim (`#19`) ; SSI263 AppleWin-fidèle (`#20`). |

---

## Annexe — le Vapor Lock en détail

Solution **purement logicielle** au manque d'IRQ VBL sur II/II+. Mécanique,
de la physique au C++ POM2 :

1. **Bus partagé (entrelacement Φ0/Φ1).** Pas de VRAM dédiée : CPU (6502) et
   scanner vidéo partagent la même RAM. Sur un cycle 1 MHz, la **phase basse**
   sert le scanner (génère les pixels), la **phase haute** sert le CPU. À chaque
   µs, le bus transporte d'abord une donnée vidéo, puis une donnée CPU.
2. **Bus flottant (capacitance TTL).** Quand aucun composant ne pilote le bus
   (lecture d'une I/O vide, ex. mirroirs `$C050-$C05F` pendant le VBL), les
   lignes conservent ~½ µs par capacité parasite (~50 pF) la **dernière valeur**
   — celle posée par le scanner juste avant.
3. **Algorithme.** Le programme écrit un motif repère (p. ex. `$FF` isolé) dans
   un coin de la RAM vidéo, puis boucle serré sur la lecture du bus flottant.
   Dès qu'il relit `$FF`, il connaît la **position exacte du faisceau** à ce
   cycle → synchro « verrouillée ». Le VBL se détecte car le scanner cesse de
   lire la RAM vidéo structurée.
4. **Piège émulateur.** Si la durée d'`ExecuteCycle()` n'est pas exacte (cycle
   pénalité d'un `BCC`/`BCS` franchissant une page oublié, etc.), le CPU dérive
   face au scanner et le lock lâche après quelques scanlines → glitches/crash.
   L'alignement doit être **parfait**.

**Côté POM2.** `Memory::floatingBus()` (`src/Memory.cpp:1484+`) calcule
l'adresse scanner à partir du `cycleCounter` global (65 cycles/ligne ×
262 lignes/frame), port **verbatim** de MAME `apple2video.cpp scanner_address`.
Les lectures de soft-switches non pilotés (`#define ... floatingBus()` aux
`Memory.cpp:1053/1121/1143/1262/1270/1481`) renvoient cet octet. C'est la
fondation qui rend le vapor lock *possible* ; reste à le prouver de bout en
bout sur une megademo (test d'intégration à ajouter).

---

## Corrections vs la source d'origine

La conversation d'origine contenait quelques imprécisions, corrigées ici :

- **« Megademo par Deater (Peter Ferrie) »** → **deater = Vince Weaver**.
  Peter Ferrie (alias *qkumba*) est une autre personne (cracks/analyses de
  protections, distinct des megademos deater). Ne pas confondre.
- **« Skyfox … Mockingboard »** → *Skyfox* (Ariolasoft/EA) sort surtout par le
  **haut-parleur**, pas le Mockingboard. Remplacé par des titres Mockingboard
  **confirmés** (Ultima V, Music Construction Set, Rescue Raiders, Willy Byte).
- **VBL.** L'absence d'IRQ VBL vaut pour **II/II+** ; le **//e** expose `$C019`
  RDVBL en lecture (toujours pas d'IRQ). Précisé dans §1.
