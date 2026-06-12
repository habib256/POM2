# POM2 — Changelog

Historique des changements notables, organisé du plus récent au plus
ancien. Le `git log` reste la source canonique pour la mécanique
exacte ; ce fichier capture les **« pourquoi »** et les pièges qu'on
ne veut pas re-découvrir. Backlog actif → `TODO.md`. Implémentation
courante → `DEV.md`.

## 2026-06-12 (chasse aux bugs : audit complet validé sur l'oracle MAME)

Audit systématique des sous-systèmes (CPU/mémoire, vidéo, audio, stockage,
threading/cartes), chaque correctif validé contre les sources MAME (citations
fichier+ligne en commentaire) et épinglé par un test. Suite : 127/127.

- **Écritures disque LSS mal positionnées angulairement** (la plus grave —
  corruption silencieuse en config par défaut). `DiskImage::writeFlux`
  réduisait la fenêtre de splice avec un `startLssCycle % period` brut, alors
  que la lecture (`getNextTransition`) est ancrée sur `revolutionStart` (port
  de MAME `find_position`). L'ancre étant arbitraire (2×cpuCycleTotal au
  motor-on), chaque écriture bit-level atterrissait à `revStart mod period`
  cellules de là où le contrôleur venait de lire l'adresse — le champ data
  écrasait une autre zone de la piste. **Piège n°2 du même chemin** : le
  re-pack cellules→nibbles supposait 8 cellules/octet, mais la timeline de
  `expandTrackBits` ajoute +2 cellules de padding par $FF de sync — dérive
  ~4,75 nibbles/secteur sur une .dsk standard. Pourquoi aucun test ne le
  voyait : `diskii_lss_smoke_test::testLssWrite` **sautait explicitement
  l'assertion positionnelle**. `writeFlux` prend désormais l'ancre (même
  convention que la lecture), le re-pack marche la timeline paddée, et le pin
  positionnel est actif (`disk_writeflux_anchor` + LSS test renforcé).
- **DHGR : teintes tournées de 90° dans les démods composites OE (CPU+GPU).**
  Le déphasage sous-porteuse était appliqué **deux fois** (construction des
  tables sin/cos avec `(k+po)&3` ET indexation avec `(xi+po)&3`). Le
  commentaire du shader GLSL documentait la mauvaise conclusion : l'ancienne
  formule GPU (application simple) était la bonne, elle « divergeait » parce
  que le CPU était faux. HGR (po=0) n'était pas affecté — d'où une calibration
  « excellente » qui masquait le bug. Piège : `dhgr_phase_signal_test` épinglait
  le bug **tautologiquement** (son ancre répliquait la formule buggée) — le
  test dérive maintenant son attendu du chemin LUT MAME indépendant.
- **DLGR : motif nibble redémarré par demi-cellule de 7 points** au lieu de la
  phase absolue 14,318 MHz (`paintLoRes40` faisait déjà bien) — couleurs
  alternées par colonne. Pin : échantillons exacts en phase absolue (le test
  naïf `sig[i]!=sig[7+i]` est invalide : aux rotl4(1)=2 depuis x=0 et main 1
  depuis x=7 ≡ 3 (mod 4) donnent la **même séquence** à des phases différentes).
- **Sound II muet** : l'émulation SSI263 (registres+IRQ) était complète mais
  `fillAudioBuffer` ne mixait jamais `ssi_->fillAudio()` (seul EchoPlusCard le
  faisait). **VIA 6522** : l'accès ORA (reg 1) ne clearait pas IFR.CA1 (MAME
  `CLR_PA_INT()`) → IRQ speech coincée pour les drivers utilisant l'idiome
  standard ; premier coup de T1 à N+1 au lieu de N+3 (le biais +2 existait déjà
  pour T2, même rationale DIX). **Phasor natif** : décodage VIA MAME
  (`$Cs10`→VIA1, `$Cs80`→VIA2, `$Cs90`→broadcast les deux, rien à `$Cs00`).
  **AY** : enveloppe période 0 = double vitesse (MAME ne clamp pas à 1).
- **SSC/telnet** : `send()` sans `MSG_NOSIGNAL` → un pair qui coupe salement
  **tuait le process** (SIGPIPE) ; EAGAIN traité comme fatal + envois partiels
  silencieusement perdus (casse ADTPro/XMODEM) ; `accept()` bloquant non
  réveillé par `shutdown()` sur macOS/BSD (le même bug déjà documenté+corrigé
  dans AiControlServer — porté le pattern `poll()`).
- **« Apply » de Slot Config écrasait la config slots sur //c** — exactement le
  bug corrigé à la sortie le 2026-06-10, mais le chemin Apply n'avait pas la
  garde `builtInSlots`. **`applyProfile` sans verrou** : `stop()` n'attendait
  pas le parking du worker et la boucle de frame ne re-vérifiait pas `mode` →
  ROM/SlotBus/disques reconstruits pendant qu'une frame turbo tournait encore
  (UB). Le worker re-vérifie entre chunks de 4096 cycles et le switch attend
  `workerParked_`. `--speed` CLI clampé à 2 M comme l'AI server.
- **2IMG : bit verrou = bit 31** (spec/CiderPress/AppleWin), pas bit 0 — les
  images verrouillées étaient inscriptibles ; le volume DOS lisait 0 au lieu
  de 254 sur dump verrouillé sans bit 8. Le test épinglait l'interprétation
  fausse (écrit d'après le code, pas d'après la spec — piège classique).
- **CPU NMOS : opcodes non documentés multi-octets** ($x3, $4B/$6B/$8B/$AB/$CB,
  $1B..$FB) dispatchés comme NOP 1 octet → désync du flux d'instructions (la
  classe exacte déjà corrigée pour $0B/$2B/$EB) ; $CB/$DB étaient remappés
  « WAI/STP indéfinis sur NMOS » alors que NMOS y met SBX #imm (2o) et DCP
  abs,Y (3o). NOP 1 octet : 1 cycle sur 65C02 (fetch seul, MAME ow65c02) vs
  2 sur NMOS ; WAI/STP 3 cycles. SBC décimal NMOS : V déterministe (différence
  binaire, MAME `do_sbc_d`) — l'ADC décimal le faisait déjà, V restait
  périmé côté SBC.
- **Mémoire IIe/II+** : $C010-$C01F est le miroir strobe clavier sur II/II+
  (MAME `.mirror(0xf)`, lecture OU écriture — `STA $C01x` ne clearait jamais) ;
  sur IIe toute écriture $C01x cleare (les lectures $C011-$C01F restent
  status-only). **Sentinelle $FE** : `iieReadStatus` renvoyait $FE pour « pas
  un status » — mais `0x80|transchar($7E '~')` == $FE est une lecture légitime
  → polls RDRAMRD envoyés au floating bus (OFF lu pendant ON). Signal hors
  bande désormais. **INTC8ROM** : s'arme sur tout accès $C3xx avec
  SLOTC3ROM=off **y compris sous INTCXROM=on** (UTAIIe 5-28) et sur le chemin
  écriture ; une écriture $C3xx ne vole plus la fenêtre $C800 à la carte
  légitime. **Événements vidéo en VBL** : estampillés ligne 192 (« fin de
  frame ») au lieu d'être clampés à 191 — un switch de mode jeté en VBL (la
  pratique canonique anti-tearing) ne peint plus de split parasite sur la
  dernière ligne visible.
- **Divers validés** : STATUS du HDV renvoie le compte de blocs en X/Y (le
  crash BITSY déjà corrigé côté SmartPortCard) ; volume 32 Mio exactement
  65 536 blocs clampé $FFFF (lu 0 avant) ; `writeBackEnabled` propagé aux
  images au restore de snapshot ; ClockCard ne perd plus l'heure au Ctrl-Reset
  (uPD1990AC sur pile) ; `/mouse` de l'AI server reconnaît la souris HLE
  AppleWin (défaut //c) ; VBL souris en cycles profil (PAL 20313) ; horloge
  PAL : provenance documentée honnêtement (verrouillée ligne 15625×65 par
  conception ; MAME = 1 016 966 — écart 0,13 % assumé, même classe que
  l'approximation « device clocks stay NTSC »).

## 2026-06-10 (//c : gel CPU NMOS, Chat Mauve arrière, config slots préservée)

- **« POM2 plante quand je sélectionne le profil Apple //c (1984) »** — c'était
  un **gel CPU**, pas un segfault. Diagnostic : la config de l'utilisateur avait
  `cpu_mode_override=nmos` (réglage collant, posé une fois sur un II+).
  `resolveCpuMode` renvoyait donc **toujours NMOS**, y compris pour le //c — qui
  a un **65C02 soudé**. La ROM //c exécute des opcodes 65C02 (`LDA (zp)`=$B2…)
  qui **décodent en KIL sur NMOS** (`M6502::Hang` = `PC--` → boucle infinie) →
  CPU figé → écran mort = « planté ». (Non reproductible en headless car le
  symptôme est l'émulation figée, pas un crash process ; isolé en analysant
  `M6502.cpp` + repro de la bascule mid-frame.) **Fix** : `resolveCpuMode`
  n'honore un override **NMOS** que si le profil est NMOS par défaut
  (II/II+///e-unenh) ; les machines **65C02-only** (//c, //c+, //e enhanced,
  variantes PAL) tournent toujours en CMOS. Le menu Machine→CPU **grise** « NMOS
  6502 » sur ces profils. Répond aussi à « le CPU doit basculer NMOS↔65C02 selon
  le profil //e/enhanced ». Vérifié : //c résout `CPU = 65C02` malgré
  l'override.
- **Le Chat Mauve sur //c (connecteur arrière).** Le //c prenait l'**« Adaptateur
  IIc »** Le Chat Mauve sur son port vidéo DB-15 (cf. fenarinarsa.com/?p=1370 +
  CLAUDE.md § profils). POM2 ignorait toute carte sur un profil `noPhysicalSlots`.
  **Fix** : exception pour `chatmauve` — la carte RVB se branche sur les //c-class
  (c'est un adaptateur vidéo, pas une carte de slot périphérique). Le panneau Slot
  Config offre un combo **{(vide), Le Chat Mauve RGB (rear connector)}** sur //c/+
  (rien d'autre n'est branchable ; le check de doublon limite à un adaptateur).
  **Le profil « Apple //c PAL (Le Chat Mauve) » câble désormais la carte en dur**
  (built-in **sl7** = « Adaptateur IIc ») — il portait le nom sans brancher la
  carte. Sur ce profil le combo des autres slots est grisé (un seul adaptateur),
  et un `chatmauve` user redondant ailleurs est ignoré (pas de double carte).
- **Config slots écrasée à la sortie sur //c.** `persistSettings` sauvait le
  mapping **live** `slotCards`, donc quitter sur //c écrivait les built-ins forcés
  (`mouseaw`…) par-dessus le choix utilisateur (`slot_4_card=mockingboard` perdu
  au retour sur //e). **Fix** : ne pas persister les slots forcés par le profil
  (built-ins + slots vidés par `noPhysicalSlots`, sauf le Chat Mauve
  user-contrôlable) — le réglage utilisateur reste intact. Même classe que
  [[pom2-cffa-profile-switch-drop]]. Vérifié : `slot_4_card=mockingboard`
  préservé après sortie propre sur //c.

## 2026-06-10 (DROL cut-scene : lectures $C050-$C057 → bus flottant)

- **Hang de la cut-scene DROL.** Scan de l'image disque : les overlays de
  cut-scene (offsets 0x14359/0x143d5/0x14be0 de `Drol.dsk`) se synchronisent
  par `LDX #$02 / LDA $C050 / CMP #$80 / BNE / DEX / BPL` — trois lectures
  consécutives du **scanner vidéo** via un soft-switch d'affichage. POM2
  renvoyait un 0 dur sur les lectures `$C050-$C057` → boucle infinie (le hang
  historique de LinApple ; AppleWin l'a corrigé en 1.13.0 en implémentant le
  bus flottant). **Fix** : une lecture `$C050-$C057` bascule le mode ET
  renvoie `floatingBus()` (MAME `apple2.cpp do_io` fait pareil) ; idem pour le
  speaker `$C030-$C03F` (latch non pilotée). **Piège évité** : le bloc tenait
  `stateMutex` et `floatingBus()` le reprend — travail scopé avant le return.
  Pinné par la section (d) de `vapor_lock` (la boucle exacte du jeu verrouille
  sur une page HGR remplie de $80, et l'effet de bord TEXT-off est préservé).

## 2026-06-10 (DROL : flips double-buffer vs beam-racing ; Chat Mauve : décodage AppleWin)

- **Flicker DROL (page-flips non synchronisés)** — tous modes d'affichage,
  NTSC comme PAL. Diagnostic par sonde sur le vrai disque
  (`tests/drol_probe.cpp`, WOZ) : DROL flippe `$C054/$C055` toutes les
  ~4 frames à des positions **dérivantes** (23/31 flips en zone visible) — du
  double-buffering libre, PAS du beam-racing (son flipper est auto-modifiant
  en `$6138` ; le bus flottant de DROL ne sert qu'à la cut-scene, cf. AppleWin
  1.13.0 « fixed the hang at Drol's cut-scene »). **Pourquoi ça clignotait** :
  le replay beam-racé peint la bande au-dessus du flip depuis la page que le
  jeu **redessine déjà** — POM2 lit la RAM au rendu, pas au passage du
  faisceau → sprites à moitié effacés. (Le vrai faisceau lisait la page
  encore intacte ; avant la publication par frame, ces events étaient souvent
  perdus → rendu pleine-page « propre » par accident.) **Fix** : dans
  `forEachBeamSegment`, une frame dont les events PAGE2 vont tous dans le
  MÊME sens = flip de buffer → page finale appliquée à toute la frame (= la
  RAM réellement affichable) ; une frame qui flippe dans les DEUX sens (DIX
  MODPAGE : page 1 à gauche, page 2 à droite de la même ligne) garde le
  replay exact. Pinné `drol_pageflip_render` ; `dix_modpage_split` inchangé.
- **Les 6 painters 560-wide relisaient l'état live** (`renderText80`,
  `renderDhgr`, `renderLoResDouble`, `renderTextChatMauveFgBg`,
  `renderHgrDuochrome`, `renderHiResChatMauve80` → `mem.getDisplayState()`
  interne au lieu du `state` de bande) — même classe de bug que celle déjà
  corrigée sur les painters legacy : les splits mid-frame PAGE2/ALTCHAR
  étaient ignorés en 560 (c'est pour ça que « Chat Mauve ne clignotait
  pas » : il masquait les flips). Signatures threadées, état passé partout.
- **Résolution HGR Chat Mauve** : le décodage couleur écrasait TOUT en blocs
  de paires alignées (1 couleur / 4 dots = 140 effectif → image « molle »).
  Porté l'algo AppleWin `RGBMonitor.cpp UpdateHiResRGBCell` : un pixel n'est
  COULEUR que s'il forme un motif isolé 010/101 avec ses voisins (couleur de
  sa paire alignée, 2 dots) ; tout le reste est noir/blanc **à la pleine
  résolution 280 px** — les runs blancs (texte, contours de sprites)
  retrouvent leur piqué, fidèle à la vraie carte RVB. Goldens `*/hgr*/
  chatmauve` régénérés ; sémantique pinnée par `le_chat_mauve_smoke` +
  `display_persistence_smoke` mis à jour.

## 2026-06-10 (Beam-racing PAL : publication par frame vidéo + vitesses 1× par standard)

- **Publication du log d'évènements vidéo par frame vidéo** (le hand-off
  50/60 Hz). L'ancien modèle ouvrait le log à chaque tick CPU du worker
  (`beginVideoEventFrame`) et l'UI le **volait** au vsync (`takeVideoEvents`
  fermait le bracket) ; tout évènement enregistré entre le take UI et le tick
  suivant était **silencieusement perdu** (`recordVideoEvent` no-op bracket
  fermé). **Pourquoi ça comptait** : en PAL le worker tourne à 50 Hz et l'UI à
  60 Hz → battement systématique à 10 Hz : ~1 rendu UI sur 6 retombait dans le
  même tick et recevait un log *vide* (→ `renderInternal`, zéro split), les
  autres un log *partiel* — les effets mid-scanline French Touch (*Mad
  Effect*, DIX) clignotaient et perdaient des bandes. Aucun test ne le voyait
  (ils bracketent de façon synchrone). **Fix** : enregistrement continu ;
  `Memory::advanceCycles` **publie** `{état de début de frame, events}` au
  franchissement de chaque frontière de frame vidéo (65 × 262 NTSC / 312 PAL
  cycles — aligné sur la géométrie du scanner, pas sur le budget 17045/20313
  du worker) ; `takeVideoEvents` renvoie une **copie** de la dernière frame
  publiée, re-rendable à volonté par l'UI 60 Hz. Le bracket synchrone reste
  disponible pour les tests (`legacyEventBracket_`). Un reset purge les deux
  logs (sinon replay fantôme contre l'état essuyé). Bonus : le chemin WASM
  (qui n'appelait jamais `beginVideoEventFrame`) gagne le beam-racing.
  Pinné `video_event_publish`.
- **`$C019`/VBL suit le standard vidéo** : la détection d'edge VBL
  (`advanceCycles`) et la lecture `$C019` utilisaient un 262 lignes codé en
  dur ; une démo PAL qui mesure la période VBL voyait une frame de 17030
  cycles pendant que le bus flottant balayait 20280 — deux machines
  contradictoires. Pinné par l'extension de `pal_timing` (§ 4 : lignes
  262–311 = VBL sous PAL, wrap à 312).
- **Vitesse 1×/2×/4× dérivée du standard actif** (toolbar, preset `/speed`
  de l'AI server, restauration du turbo disque re-seedée par `applyProfile`).
  **Pourquoi** : les 17045 codés en dur faisaient tourner une machine PAL à
  17045 × 50 Hz = 852 kHz (−16 %) au premier clic « 1× » — effets qui roulent,
  musique Mockingboard molle. Reste assumé : `MouseCardAppleWin::kCyclesPerVbl`
  = 17045 (pacing VBL de la HLE souris à 60 Hz même en PAL — sans effet sur
  les démos, à retraiter avec le port //c).

## 2026-06-01 (Release v0.7)

- **Bump de version v0.6 → v0.7.** Mise à jour de la chaîne de version dans
  les **5 emplacements canoniques** recensés par `CLAUDE.md` § Version string
  locations : `CMakeLists.txt` (`project(... VERSION 0.7 ...)`, qui pilote aussi
  `CPACK_PACKAGE_VERSION` + le nom de l'archive `build_dist.sh`), `src/main.cpp`
  (bannière console + titre de fenêtre initial), `src/MainWindow_Slots.cpp`
  (titre runtime qui écrase celui de `main.cpp` une fois le profil résolu — au
  constructeur **et** au switch de profil), `src/MainWindow.cpp` (dialogue
  *About*), et `README.md` (titre). `CLAUDE.md` lui-même mis à jour
  (`Current release: **v0.7**`). **Pourquoi le noter** : la version vit dans
  des chaînes dupliquées non dérivées d'une source unique, donc tout bump doit
  toucher ces points en bloc sous peine de dérive (titre fenêtre vs About vs
  paquet). Source unique CMake → header générée = item de backlog séparé.

## 2026-05-31 (Composite : beam-racing du signal + courbe phosphore)

- **Beam-racing du signal composite.** `fillCompositeSignal` lisait *un seul*
  `getDisplayState()` de fin de frame : les switches d'affichage mid-scanline
  (text↔graphics, page flip, DHGR on/off) étaient invisibles dans **tous** les
  modes composite (`ColorCompositeOE` GPU, `ColorCompositeOECpu`,
  `ColorAppleWin`) — seuls les modes LUT bénéficiaient du beam-racing
  (`renderBeamRacing` côté RGBA). **Pourquoi ça comptait** : une démo qui passe
  du texte au HGR à mi-écran s'affichait entièrement en HGR sous OE/AppleWin.
  **Fix** : `render()` prend le log d'évènements **une seule fois** et le passe
  aux deux consommateurs ; `fillCompositeSignal(mem, events)` rejoue le log
  bande par bande (zéro `signalBuf` → `getDisplayStateAtFrameStart()` →
  `paintSignalBand(y0,y1)` qui réutilise le même clipping `bandRows`/
  `bandScanlines` que `renderInternalBand`). Le `state` peint est un local
  *mutable* pour que les helpers (capturés par référence) voient chaque switch.
  Log vide → `paintSignalBand(0,192)` = octet-pour-octet l'ancien dispatch
  (goldens OE GPU/CPU inchangés). **Pièges** : `signalPhaseOffset_` reste une
  constante par-frame (dernière bande graphique gagne → split HGR↔DHGR
  mid-frame approximé) ; lo-res clip au block-row (4 lignes), comme le path
  RGBA. Épinglé `beam_race_composite` (frame text→HGR à la scanline 96 :
  bande haute = waveform texte, bande basse = waveform HGR, et **pas** HGR en
  haut comme le bug pré-fix).
- **Courbe phosphore (CRT gamma).** Le pipeline NTSC signal-level (déjà
  complet : FIR Y@2.0 MHz / chroma@0.6 MHz, YUV→RGB, PAL line-phase) n'avait
  pas de réponse phosphore. Ajout d'un `phosphorGamma` (power-law par canal
  `rgb^γ`) dans `CrtEffectStack`, **après** BCS et **avant** scanlines/mask
  (le masque atténue la lumière que le phosphore a déjà produite). γ = 1.0 =
  identité → **aucun golden/parité touché** ; γ > 1 creuse les ombres, γ < 1
  les relève. C'est la moitié *luminance* du modèle phosphore ; `persistence`
  est la moitié *temporelle*. Slider « Phosphor curve (gamma) » 0.6–2.6,
  persisté `ntsc_phosphor_gamma`. NB : effet *glass*, donc actif sous OE en
  permanence et sous les autres modes quand « CRT effects on all modes » est on.

## 2026-05-31 (Vue 3D voxel — phases 0+1 ; bouton rewind toolbar)

- **Vue 3D voxel — refonte « Voxel Cube » fidèle à MicroM8 (correctif).**
  La première version extrudait **hauteur = luminance** sur un écran posé
  **à plat** (plan XZ) : pixels brillants transformés en stalactites, angle
  catastrophique, voxels difformes. Scrapé MicroM8 (`paleotronic.com` Quick
  Start + Features) : le mode « Voxel Cube Color » dresse l'écran **debout**
  (moniteur, plan XY) et donne à **chaque pixel un cube de même épaisseur**
  extrudé vers le spectateur sur **+Z** (« Voxel Depth »). La hauteur n'est
  **jamais** liée à la luminance ; le relief par couleur (`colorShift`,
  « Z-axis 3D offset ») est une **option** (défaut 0 → dalle plate qu'on
  tourne pour voir l'épaisseur).
  - **Géométrie** : cube footprint XY + profondeur Z ; colonne→X, ligne→Y
    (ligne 0 = haut) ; plan **4:3 réel** (2.0 × 1.5) pour garder la forme des
    pixels Apple II. `heightScale`→`voxelDepth` (0.06), `+colorShift`.
  - **Caméra** : défauts quasi de face + léger 3/4 (azimut 0.32 / élévation
    0.20 / distance 2.8 / fovY ~40°), cible recentrée à l'origine. **Orbite
    au glisser-gauche + zoom molette** (dans `drawScreenImage`, mute
    `voxelCam_`). `voxel3d_math` reste vert (la math caméra est inchangée).
  - **Suite (même jour)** : (1) **haut/bas inversés** — la présentation
    FBO→`ImGui::Image` est un miroir vertical (comme les passes NTSC 2D) ;
    pré-flip de `gl_Position.y` dans le vertex shader. (2) **Résolution
    native** — `gridW/gridH` pilotés par `display->width()/height()`
    (280/560 × 192) → un voxel par pixel Apple II (avant : 140×96, moitié de
    l'info perdue) ; `voxelDepth`/`colorShift` passés en **unités de cellule**
    pour rester constants entre 280 et 560 de large. (3) **Relief par couleur
    activé** (`colorShift` 8 cellules, pondéré luminance) → « pop » pin-art
    demandé. (4) **Indépendant du CRT** — le voxel tappe l'image couleur
    **avant** `CrtEffectStack` via un handle `voxelSrcTex` séparé (sinon
    scanlines/masque/barrel se retrouvaient cuits dans les 50k cubes).
    (5) **Pan/strafe au bouton du milieu** — `OrbitCamera::pan` glisse la
    cible dans le plan droite/haut de la caméra, échelle en unités-monde/pixel
    pour un suivi 1:1 (orbite = glisser-gauche, zoom = molette). (6) **Moiré
    supprimé** — cubes **jointifs** (`cubeFill` 0.9→1.0 : un aplat redevient
    une dalle continue, fin de la grille d'interstices) **+ supersampling**
    (`superSample` 2× : FBO rendu à 2×, mip-chain, minify trilinéaire par
    ImGui → anti-aliasing sans resolve MSAA).
  - **Phase 3 — panneau de réglage** (`renderVoxelSettingsWindow`, View ▸
    « 3D voxel settings… ») : sliders live `voxelDepth` / `colorShift` /
    `cubeFill` / `superSample` (poussé à **3×** par défaut) / `ambient`, +
    boutons Reset view / Reset settings. Le renderer est **possédé dès le
    chargement des settings** (ctor sans GL) pour que le panneau et les clés
    `voxel_*` (persistées) se branchent directement sur `voxel3d_`, même avant
    d'activer la vue 3D. La résolution de grille reste auto (= écran).
  - **P4 — garde-fou perf WASM** : sous `__EMSCRIPTEN__`, `process()` plafonne
    `superSample ≤ 2` + FBO ≤ 2048² (réduit le facteur jusqu'à tenir) et
    `MainWindow` plafonne `gridW ≤ 280` (divise par 2 la géométrie DHGR 560).
    Natif inchangé (`ss ≤ 4`, 8192²). Compile WASM/WebGL2 revérifiée.
  - **Bonus fidélité — Mono + profondeur par index de couleur** : case `mono`
    (« Voxel Cube Mono », sortie grise, relief conservé) et `perColorDepth`
    (snap au plus proche des 16 couleurs lo-res `kVoxelPalette` → relief
    discret par couleur au lieu de la luminance continue). Boucle de recherche
    16-itérations dans le vertex shader (par instance, pas par pixel) ;
    `glUniform3fv` ajouté au loader. Persistés `voxel_mono` /
    `voxel_percolor_depth`. P5 (tie-in rewind) **différé** sur demande.
  - **Fix molette en WASM** : le port GLFW d'Emscripten ne livre pas les
    events `wheel` à ImGui (`io.MouseWheel` restait à 0 → zoom 3D inopérant
    dans le navigateur). Ajout d'un `emscripten_set_wheel_callback("#canvas")`
    dans `main.cpp` qui alimente `io.AddMouseWheelEvent` (même échelle que le
    backend ImGui) — choix chirurgical pour ne pas toucher au sizing canvas du
    shell (vs `ImGui_ImplGlfw_InstallEmscriptenCallbacks` qui hooke aussi le
    resize/fullscreen).

- **Bouton rewind dans la toolbar** (à gauche de Pause, en miroir de Step à
  droite) : `ICON_FA_BACKWARD_FAST`, **maintenir = rewind-live** (même geste
  que `F6` / la barre Devices ▸ Rewind). Grisé tant qu'il n'y a pas d'historique.
  `F6` et le bouton partagent un seul edge-tracker (`driveRewindHold`).

- **Vue 3D voxel façon MicroM8 — fondations (phases 0+1).**
  - **Pourquoi / archi** : extruder l'écran en cubes (hauteur = luminance du
    pixel) avec caméra orbitale. Choix clé : c'est un **axe de vue orthogonal**,
    pas un `HiResMode` — un render-pass qui consomme la **texture RGBA déjà
    décodée** (n'importe quel mode couleur + NTSC/CRT), exactement comme
    `CrtEffectStack`. Universel, gratuit pour tous les modes.
  - **Phase 0 — `Mat4.h`** : Vec3 + Mat4 column-major (perspective/lookAt/
    multiply) + `OrbitCamera` (azimut/élévation/distance → view-proj). Aucune
    dépendance (pas de glm). Épinglé `voxel3d_math` (entrées perspective, base
    orthonormée lookAt, projection de la cible au centre).
  - **Phase 1 — `Voxel3DRenderer.{h,cpp}`** : cubes **instanciés**
    (`glDrawElementsInstanced`, ~13 k pour 140×96), hauteur+couleur par
    **vertex texture-fetch** de la framebuffer, ombrage par **dérivées
    d'écran** (pas d'attribut normal → reste sur le seul `aPos` lié par le
    helper shader partagé). FBO **avec depth** (les passes 2D n'en ont pas).
    Même pattern lazy-init + sauvegarde/restore d'état GL que `NtscPostProcessor` ;
    compatible WebGL2/GLES3 (instancing + VTF + dérivées, pas de geometry
    shader). Toggle **View ▸ « 3D voxel view »** (persisté `show_3d_voxel`),
    branché dans `drawScreenImage` avant le blit final.
  - **À suivre** : caméra orbitale au drag souris + zoom (P2), panneau de
    réglages + éclairage (P3), paliers de résolution / heightfield (P4), tie-in
    rewind « figer + orbiter » (P5). Le rendu GL se vérifie en lançant l'app
    (pas de hash golden — la math caméra est, elle, testée).

## 2026-05-31 (Rewind — codec delta, UI, état disque, cas lourds : phases 2→5)

- **Rewind façon MicroM8 complété (phases 2 à 5).** Le socle (phases 0+1,
  ci-dessous) stockait des snapshots pleins ; ces phases le rendent
  utilisable en vrai. Tout épinglé : `rewind_delta`, `rewind_transport`,
  `rewind_slot_state` (+ `rewind_roundtrip` inchangé = filet de régression
  de l'API).
  - **Phase 2 — codec delta XOR + keyframes** (`RewindBuffer`) : une keyframe
    pleine tous les `keyframeInterval` frames (défaut 120 ≈ 2 s), deltas XOR
    entre (uniquement les spans modifiés, coalescés sur gaps < 16 o). 30 s
    passent de ~315 Mo à ~10 Mo. Reconstruction = keyframe la plus proche ≤ i
    + XOR des deltas. Éviction **rebase-on-evict** : le front reste toujours
    une keyframe (le delta suivant est promu avant de drop). API publique
    inchangée → `rewind_roundtrip` (phase 1) passe tel quel = preuve de
    non-régression. *Pourquoi keyframes+delta plutôt que reverse-delta seul :
    XOR est sa propre inverse, donc un seul sens de delta sert le scrub
    bidirectionnel, et les keyframes bornent le coût de seek aléatoire.*
  - **Phase 3 — UI + transport + rewind-live** : `Rewind_ImGui` (Devices ▸
    Rewind) — toggle Record, timeline, transport |< / << (hold) / <| / |> /
    resume, slider de durée d'historique ; `F6` = hold-to-rewind-live partout
    (gesture MicroM8). Restore servi **worker parké** : `rewindBeginScrub()`
    met Stopped puis `waitUntilParked()` attend `workerParked_` (posé dans
    l'attente CV Stopped du worker) → un restore UI ne peut pas être écrasé
    par la frame Running en vol (la branche Running épuise tout son budget
    avant de re-checker le mode). `rewindEndAndResume` restaure + `truncateAfter`
    (jette le futur abandonné) + relance. Ring vidé au `coldBoot`.
  - **Phase 4 — état cartes slot** : `SlotPeripheral::append/loadSnapshotState`
    (no-op par défaut) ; `DiskIICard` sérialise son état mécanique + LSS
    (quarter-track tête, moteur, aimants de phase, registre data, séquenceur,
    timing rotationnel — **pas** le média ni les PROMs). `MachineSnapshot`
    écrit des sections `SLOTn` **uniquement si `includeSlots=true`** (le
    rewind opt-in ; l'API AI-control `/snapshot` garde son contrat « disque
    exclu » — un fichier d'archive peut survivre à un changement de média).
    Le restore route vers la carte du slot (magic+version → une carte
    étrangère ignore un blob qui n'est pas le sien) et tolère leur absence. Un
    rewind pendant une I/O disque ne laisse plus la tête sur le mauvais
    nibble. Round-trip machine complète bit-à-bit (incl. SLOT6) épinglé.
  - **Durcissement post-revue** (revue multi-agents) : (a) course du handshake
    de park corrigée — `setMode(non-Stopped)` efface `workerParked_` côté
    setter, sinon un resume→rescrub rapide lisait un flag périmé ; (b)
    `DiskIICard::loadSnapshotState` borne `activeDrive` (garde d'index) ; (c)
    slider d'historique désactivé pendant le scrub (l'éviction décalerait les
    index) ; (d) backend mémoire `SnapshotIO` réécrit en streambuf zéro-copie
    (`VectorOutBuf`/`ArrayInBuf`) — supprime le double-copy via `stringstream`
    à chaque capture (~21 Mo/s sur IIe, bien plus avec RamWorks).
  - **Phase 5 — cas lourds** : budget mémoire `maxBytes_` (défaut 256 Mio) en
    plus du cap de frames → RamWorks (~10 Mo/keyframe) borné (moins d'historique
    plutôt que RAM qui explose). `flushAudioForRewind()` (reset speaker) à
    chaque restore → un saut temporel est silencieux, pas un « pop ». Capture
    branchée aussi dans `tickFrame()` (chemin mono-thread WASM).
  - **Chips audio sérialisés** (clôture du gap audio) : `MockingboardCard` et
    `PhasorCard` sérialisent l'état registre/timer de leurs `Via6522` (24 o) +
    `Ay3_8910` (34 o) via le hook `SlotPeripheral` — helpers `append/loadSnapshot`
    partagés par les deux cartes, packing LE mutualisé dans `ByteIO.h`. La
    musique survit donc à un rewind (pas seulement le flush speaker). L'AY est
    un modèle-registres (la synthèse dérive des 16 registres) → restaurer les
    registres restaure le son exactement. Épinglé `rewind_audio_state`
    (round-trip machine complète bit-à-bit incl. Mockingboard).
  - **Parole SSI263 sérialisée** : `Ssi263::append/loadSnapshot` (30 o : 5
    registres + curseur de lecture des phonèmes), câblé dans la variante Sound II
    de `MockingboardCard` → la parole survit aussi au rewind. Couvert par
    `rewind_audio_state` (bloc Sound II).
  - **Écritures disque annulées au rewind** (Phase 6) : snapshot DiskIICard
    passé en v2 — il embarque les buffers de pistes nibble pour les disques
    chargés, physiquement inscriptibles et non-WOZ
    (`DiskImage::append/loadMediaSnapshot`), donc une écriture disque est
    annulée par un rewind. Le codec delta garde le coût ~nul tant qu'aucune
    piste n'est écrite ; les caches de lecture se redérivent des nibbles
    restaurés. Disques read-only / WOZ / vides = 1 octet de flag. Épinglé
    `rewind_disk_write` (COW média + écriture-via-carte annulée bout-en-bout).
  - **Gap restant** : écritures sur WOZ inscriptible non annulées (WOZ stocke
    ses bits dans `wozRaw`, store distinct ; les originaux WOZ sont en général
    write-protected). Suivi propre si besoin. Détail → `DEV.md` § Rewind.

## 2026-05-31 (Rewind — fondations, phases 0+1)

- **Rewind façon MicroM8 — socle capture/restore d'état (sans UI).**
  - **Pourquoi** : enregistrer l'état machine en continu pour permettre
    un retour arrière dans le temps (scrub/step-back). Choix d'archi :
    ring-buffer de snapshots d'état (façon RetroArch) plutôt que rejeu
    déterministe des entrées — découplé du hot-path CPU, robuste, et
    réutilise `SnapshotIO` tel quel. Le delta/keyframe (pour réduire le
    coût ~175 Ko/frame) est la phase 2 ; ici on stocke des snapshots
    pleins pour valider la boucle capture→restore bit-à-bit.
  - **Phase 0 — `SnapshotIO` backend mémoire** : `SnapshotWriter(vector&)`
    / `SnapshotReader(ptr,len)` à côté du backend fichier existant, via un
    `std::stringstream` lié à un membre `std::ostream&`/`std::istream&`
    (toute la logique sections/longueurs réutilisée). Format binaire
    identique entre les deux backends. Épinglé : `snapshot_memory_roundtrip`
    (round-trip + parité octet-pour-octet vs le writer fichier).
  - **`MachineSnapshot.{h,cpp}`** : extraction de la séquence canonique
    `CPU`/`MEM`/`MEX` hors d'`AiControlServer` (qui maigrit de ~63 lignes).
    Source unique de vérité partagée par l'API AI-control ET le rewind, donc
    plus de divergence possible. Le durcissement sécurité reste : gate de
    longueur 16 o sur la section CPU (over-read d'un blob forgé, « round 10
    #3 ») + cap MEX 16 Mio → `RestoreResult{false,…}` (l'API renvoie
    toujours 400). Couvert par `ai_control_server_smoke` (aucune régression).
  - **Phase 1 — `RewindBuffer.{h,cpp}`** : ring `std::deque` de snapshots
    pleins, éviction oldest-first au-delà de `maxFrames` (défaut 1800 ≈ 30 s
    @ 60 Hz), `restore(i)` / `restoreToCycle(cycle)`. Capture branchée à la
    frontière de frame quiescente du `workerLoop` (après budget CPU + tick
    IWM), gardée par `enabled()` avant la prise de `stateMtx` → coût nul
    quand désactivé (défaut). Épinglé : `rewind_roundtrip` (round-trip
    bit-à-bit + éviction + seek `restoreToCycle`).
  - **Gaps assumés cette phase** : état cartes/disque hors snapshot (rewind
    pendant I/O disque laisse la tête où le sim live l'a posée → phase 4 :
    hook `SlotPeripheral` + état lecteur `DiskIICard`) ; chips audio
    désync ; pas d'UI (phase 3) ; WASM non câblé (phase 5). Détail →
    `DEV.md` § Rewind / time-travel.

## Antérieur (≤ 2026-05-30) — archivé

Les entrées du 2026-05-30 jusqu'au 2026-05-14 (pré-v0.7) sont déplacées dans
[`docs/archive/CHANGELOG-2026-05.md`](docs/archive/CHANGELOG-2026-05.md) pour
garder ce fichier focalisé sur le cycle courant. Historique complet → `git log`.
