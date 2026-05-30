# POM2 — Changelog

Historique des changements notables, organisé du plus récent au plus
ancien. Le `git log` reste la source canonique pour la mécanique
exacte ; ce fichier capture les **« pourquoi »** et les pièges qu'on
ne veut pas re-découvrir. Backlog actif → `TODO.md`. Implémentation
courante → `DEV.md`.

## 2026-05-30 (DHGR parity)

- **DHGR phase + beam-racing + goldens DLGR.**
  - **Phase DHGR signal** : `signalPhaseOffset()` (= 1 en DHGR HGR, 0 sinon)
    appliqué au démod OE GPU (`uPhaseOffset`), OE CPU (`renderCompositeOeCpu`)
    et AppleWin (`renderLine`/`renderFrame`). Aligné sur MAME
    `rotl4b(_, absX+1)`. Épinglé par `dhgr_phase_signal`.
  - **Beam-racing** : journal d'événements vidéo horodatés dans `Memory`
    (`VideoEvent`, `$C050-$C057` / `$C05E-$C05F` / 80COL), snapshot par frame
    (`beginVideoEventFrame` dans `EmulationController`), rendu par bandes
    (`renderInternalBand` + fallback rapide si log vide). Logging actif
    seulement pendant une frame émulée (tests headless non affectés).
  - **Goldens DLGR** : scènes `iie/dlgr` et `iie/dlgrmixed` dans
    `display_golden_hash_test`.
  - **Docs** : `DEV.md`, `docs/video_parity_audit_2026-05-30.md`,
    `docs/graphics_modes_comparison.md` — DLGR implémenté, sharpness neutre
    à 0.5, phase DHGR corrigée.

- **Routage affichage — DLGR signal + beam-racing paging.**
  - **`fillCompositeSignal::paintLoResDouble`** : le signal composite
    interleave aux/main (rotl4) comme `renderLoResDouble` — corrige DLGR
    en ColorCompositeOE / AppleWin / OE-CPU (auparavant main-only via
    `paintLoRes40`). Goldens `iie/dlgr/signal` et `iie/dlgrmixed/signal`
    mis à jour ; `dlgr_render_smoke` vérifie la parité signal.
  - **Beam-racing** : événements `EightyStore` et `AltChar` journalisés sur
    `$C000-$C00F` pour que le replay mid-frame respecte PAGE2 sous 80STORE
    et le jeu de caractères 80-col.
  - **`paintHgr`** : masque MSB fixe `0xFF` (HGR standard) — le bit DHIRES
    ne doit pas masquer bit 7 hors mode DHGR affiché.

- **Parité couleur OE GPU ↔ CPU (agent `docs/oe_gpu_cpu_parity.md`).**
  - **Phase shader GPU** alignée sur `renderCompositeOeCpu()` (formule
    `(k+po)&3`, pas `floor(fx)+po`) — corrige les teintes DHGR/HGR erronées
    en mode `ColorCompositeOE`.
  - **MainWindow** : branche CRT unifiée `oeFamily` (GPU + CPU) avec hue/sharp
    neutralisés post-démod.
  - **Test** : `oe_demod_gpu_cpu_parity` (ΔRGB ≤ 1, HGR + DHGR, sans GL).
  - **`oe_signal_view`** : référence CPU gaussienne remplacée par FIR YUV
    (`renderCompositeOeCpu`), `phaseOffset` CLI, stats max/mean Δ vs readback GL.

- **Reset II/II+ : affichage préservé sur F12.**
  - `hardReset()` appelait `resetSoftSwitches()` (TEXT forcé, plus d'artefacts
    NTSC) au lieu du chemin warm MAME. Corrigé : F11 et F12 utilisent
    `resetSoftSwitchesWarm()` sur II/II+ ; seuls cold boot / changement de
    profil réinitialisent TEXT. Pin : `system_profile_smoke`.

- **TEXT sans NTSC en ColorCompositeOECpu (et AppleWin).**
  - Le mode texte passait par `fillCompositeSignal` + démod CPU → franges
    colorées au boot. Désormais `renderInternal` (mono net) pour TEXT plein
    écran ; démod réservée au graphique (+ `patchMixedTextBand` en mixed).
    Pin : `text_oecpu_crisp`.

## 2026-05-30

- **Parité OpenEmulator : filtres de démodulation exacts + reliquats du
  rapport.** Source OE récupérée via scrapling (le cœur `libemulation` est un
  sous-module séparé `openemulator/libemulation`).
  - **Filtres FIR OE exacts** (`NtscPostProcessor.cpp` + `Apple2Display.cpp`).
    Portage des fonctions `OEVector::chebyshevWindow`/`lanczosWindow` (méthode
    realIDFT) au config *AppleColor Composite Monitor IIe* (luma 2.0 MHz,
    chroma 0.6 MHz, Y'UV). **Luma** : notch fs/4 `|H(0.25)|` = **0.002**, −3 dB
    **1.64 MHz** (= OE). **Chroma** : kernel OE exact (`|H(0.25)|` = 0.0004,
    −3 dB 0.64 MHz) ; le curseur Sharpness mélange désormais soft-OE (0.6 MHz,
    parité exacte à 0) ↔ net (2.0 MHz). Remplace les gaussiennes approximatives.
  - **Vignette / center-lighting** (`centerLighting`, défaut 1.0 = plat) :
    formule OE `exp(−dot(cuv·(1/cl−1)))`, nouveau slider.
  - **Plancher de bruit persistance** (`−0.5/256`, OE) : les traînées faibles
    s'éteignent jusqu'au noir au lieu de stagner.
  - **Double Lo-Res (DLGR)** : nouveau `renderLoResDouble` (80 cellules,
    aux+main, `rotl4(NIBBLE(aux),1)` — MAME `lores_update<Double>`). Épinglé
    par `dlgr_render_smoke`.
  - **Polarité AN3 commentée** (`LeChatMauveCard.cpp`) : MAME clocke 80COL,
    AppleWin !80COL → numéros de mode bit-inverses (attendu, documenté).
  - *Non porté (volontaire)* : linear-light (OE est aussi en gamma → hors
    parité) ; teintes phosphores mono (choix esthétiques assumés).
- **Audit de parité vidéo (MAME/AppleWin/OpenEmulator) + correctifs.** Audit
  multi-agents de 9 sous-systèmes vidéo/couleur/effets (rapport :
  `docs/video_parity_audit_2026-05-30.md`). Correctifs appliqués :
  - **🔴 Bug `ColorComp4Bit` (square filter) corrigé** — l'origine de nibble
    (`>>kContextBits` au lieu de `>>kContextBits-1`) et la rotation de phase
    (`absX`/`absX+1` au lieu de `absX-1`/`absX`) divergeaient de MAME sur
    ~50 % des dots. Correction couplée → **bit-exact contre un oracle MAME**
    (0/2,2 M dots), confirmée visuellement (teintes alignées sur ColorNTSC).
    Hashes `*/4bit` du golden régénérés. (`Apple2Display.cpp:1104,1451`.)
  - **OE composite : notch de sous-porteuse en luma.** Le passe-bas luma OE
    passe d'une gaussienne (qui laissait fuir ~46 % de la sous-porteuse fs/4 →
    dot-crawl) à un **FIR 17-taps Dolph-Chebyshev(50 dB)×sinc @ 2 MHz** qui
    notche fs/4 (`|H(0.25)|` 0.46→0.05). Chroma reste gaussienne (rejet fs/4
    déjà bon). Appliqué shader GPU + chemin CPU. (`NtscPostProcessor.cpp`,
    `Apple2Display.cpp`.)
  - **Shadow-mask dark/light (Lottes)** : triplet 0.5/1.5 qui préserve la
    luminance moyenne au lieu d'écraser 2/3 canaux à 0 (sur-assombrissement).
  - **Gain de luminance post-glass** (`luminanceGain`, nouveau slider, défaut
    1.0) pour re-éclairer sous scanlines/masque. Défauts CRT volontairement
    punchy assumés et documentés (DEV.md).
  - **Cadence de flash 15→16 frames** (`Apple2Display.h`) — aligne exactement
    MAME-IIe `& 0x10` + AppleWin (était ~6,7 % trop rapide).
- **Curseur Hue fonctionnel sur tous les modes (plus seulement OpenEmulator).**
  La teinte n'était appliquée que dans le shader de démodulation OE ; pour les
  autres modes (AppleWin, ColorNTSC, Chat Mauve…) le framebuffer est déjà en
  RGB et passe par `CrtEffectStack`, qui **ignorait** le knob hue → curseur
  sans effet. Ajout d'une rotation de chroma dans `CrtEffectStack` (RGB→YUV
  BT.601, rotation U/V de `hue·π`, YUV→RGB matrice OpenEmulator — même
  convention que `NtscPostProcessor`, donc comportement identique partout).
  En mode OE la teinte reste appliquée au démodulateur ; le hue passé à
  `CrtEffectStack` est mis à 0 pour éviter une double rotation. Bannière du
  panneau « CRT Settings » corrigée (elle prétendait à tort que les knobs
  n'agissaient que sur OE). Sharpness et PAL restent OE-only (étapes de
  démodulation, sans objet sur du RGB). (`src/CrtEffectStack.{h,cpp}`,
  `src/MainWindow.cpp`.)
- **Effet barrel : fin des moirées / lignes parasites.** La distorsion barrel
  déforme les UV de façon non-linéaire ; les motifs haute-fréquence (scanlines
  période 2 lignes-source, masque d'ombre période 3) dépassaient alors le
  Nyquist de la cible aux endroits où la courbure compresse l'image → aliasing
  en bandes de moiré. Correction en deux temps dans `CrtEffectStack` :
  (1) la passe d'effets est désormais rendue à la **résolution écran native**
  — `MainWindow::drawScreenImage()` calcule la taille cible en amont et la
  passe à `process(src, srcW, srcH, dstW, dstH)` (les dims source ne servent
  plus qu'à la *fréquence* des motifs via `uSrcSize`) ; ImGui blitte ensuite
  1:1, sans second ré-échantillonnage ; (2) le shader **anti-aliase
  analytiquement** scanlines et masque via `fwidth()` — là où la courbure
  comprime (donc là où ça moirerait), la modulation se fond doucement vers le
  neutre. Scanlines en faisceau `cos` lisse (au lieu d'un `fract` dur) et bord
  barrel adouci par un masque `fwidth`. Diagnostic : `tests/crt_barrel_view`.
  (`src/CrtEffectStack.{h,cpp}`, `src/MainWindow.cpp`.)
- **`ColorAppleWin` : couleur restaurée — port fidèle de `NTSC.cpp`.** Le
  mode AppleWin NTSC ne sortait quasiment **aucune couleur** (aplats en
  damier noir/blanc, teinte seulement sur les bords). Cause : l'ancienne
  approximation gaussienne calculait la luma avec une fenêtre trop étroite
  (σ=1.5) pour notcher la sous-porteuse fs/4 ; la luma absorbait donc la
  sous-porteuse, et la démod `signal − luma` annulait la chroma à
  l'intérieur des aplats stables — seuls les transitoires de bord
  survivaient. Remplacé par un **port ligne-à-ligne** de
  `AppleWin source/NTSC.cpp::initChromaPhaseTables` : trois filtres IIR
  2-pôles (signal passe-bas, chroma **passe-bande @ fs/4**, luma passe-bas),
  sur-échantillonnage ×2, démod en quadrature `/8`, matrice FCC, cas
  spéciaux blanc/noir/gris. Coefficients et matrice cités depuis
  `NTSC.cpp:115-132 / 833-841`. `CYCLESTART = 45°` aligne les teintes sur
  la référence MAME sans calibration. Tables séparées Monitor (luma y0) /
  Color-TV (comb y1). Garde anti-régression : `$2A` plein doit être saturé
  (`applewin_ntsc_smoke`). Captures `docs/img/total_replay_09/10/11`
  régénérées. (`src/AppleWinNtsc.{h,cpp}`.)
- **OpenEmulator composite accessible en CPU *et* GPU dans l'UI.** Nouveau
  mode `ColorCompositeOECpu` : la même démodulation Y/I/Q OpenEmulator,
  calculée **sur le CPU** dans le framebuffer (comme AppleWin) au lieu du
  shader GLSL — disponible sans chemin shader et pour comparer les deux.
  Menu *Display → Color pipeline* : « Composite (OpenEmulator, GPU) » et
  « … CPU ». `render()` démodule `signalBuf` → frame80 (`renderCompositeOeCpu`,
  poids gaussiens + 4 phases sous-porteuse pré-calculés → pas de trig dans la
  boucle pixel) ; MainWindow le présente comme un framebuffer normal (la pile
  `CrtEffectStack` s'applique toujours si « CRT effects on all modes »).
  Même phase +1.5π que le GPU → couleurs identiques. Vérifié au runtime sur
  le vrai shader GLSL (outil `oe_signal_view` : démod du signal TR réel =
  herbe verte / maisons magenta, conforme NTSC).

- **Inversion des couleurs d'artefact NTSC corrigée (4-bit, OpenEmulator,
  AppleWin).** Retour utilisateur : « NTSC (MAME) et NTSC medium parfaits,
  les autres ont une inversion d'artefact NTSC ». Diagnostic : seuls les
  modes couleur **non-LUT** étaient affectés ; oracle = `ColorNTSC`
  (palette MAME, confirmée correcte). Vérifié visuellement en rendant le
  splash HGR de Total Replay dans chaque mode (`render_total_replay_modes`,
  comparaison à la référence NTSC : herbe verte / maisons magenta).

  - **4-bit square (`ColorComp4Bit`)** — la rotation de phase était `absX-1`
    (HGR) / `absX` (DHGR) au lieu de `absX` / `absX+1` (= la rotation du
    chemin LUT mode 0, la référence). Erreur d'1 dot/90° : le violet de
    `$01` sortait orange. Dérivé à la main puis confirmé contre MAME
    `apple2video.cpp` (`rotl4(w & 0x0f, x + is_80_column - 1)` : le `-1`
    de MAME compense sa propre origine de fenêtre `w&0x0f`, alors que la
    fenêtre POM2 porte `kContextBits` de contexte gauche → l'extraction
    `(w>>kContextBits)&0x0f` était correcte, c'était la rotation qui devait
    suivre mode 0). Épinglé : 5 entrées 4-bit de `display_golden_hash`
    re-baseline.
  - **OpenEmulator (`ColorCompositeOE` GPU + `ColorCompositeOECpu`)** — la
    cause racine était la **mauvaise matrice de décodage** : le shader
    démodulait U/V (chroma·sin, chroma·cos, comme OpenEmulator) mais les
    passait à une matrice **YIQ** (axes tournés ~33° par rapport à YUV) → roue
    des teintes mal orientée. Un offset de phase pouvait rattraper vert/violet
    mais **jamais** bleu/orange (le bleu sortait orange). Source de vérité :
    OpenEmulator `libemulation/.../OpenGLCanvas.cpp` — chroma =
    composite·(sin φ, cos φ) puis matrice **YUV→RGB**
    `R=Y+1.139883·V ; G=Y−0.394642·U−0.580622·V ; B=Y+2.032062·U`. Adoptée
    verbatim, offset de phase **0** (calibré par balayage : les 4 couleurs
    matchent la LUT MAME à ±10 RGB — VIOLET (233,28,255)≈(230,40,255),
    BLUE (18,150,255)≈(25,144,255), etc.). **Deuxième bug, GPU uniquement** :
    le shader échantillonnait au centre du texel (`vUv.x·size = px+0.5`) mais
    référençait la **phase** de la sous-porteuse à `px+0.5` au lieu de l'indice
    entier `px` → décalage d'un demi-échantillon = 45°, qui re-tournait la roue
    (bleu→vert, orange→magenta) côté GPU alors que le CPU était correct. Fix :
    `phase = π/2·floor(fx)` (échantillonnage au centre conservé, phase à
    l'entier). Vérifié sur le **vrai shader GLSL** (`oe_signal_view`,
    readback glReadPixels = démod CPU à ≤1 LSB) : bandes violet/vert/bleu/orange
    correctes.
  - **AppleWin (`ColorAppleWin`)** — décodeur IIR-LUT séparé (registre 12-bit,
    fenêtre centrée, sat ×10) : la matrice YUV ne lui convient pas (aucune
    phase ne récupère vert/violet). Laissé en YIQ + `g_phaseShift` **+90°**
    (`buildChromaLut`/`buildIdealizedLut`) qui redresse vert/violet sur contenu
    réel ; bleu/orange restent approximatifs (mode « scaffold » documenté).
    Hook diagnostic `AppleWinNtsc::rebuildForPhase()`.

  **Pas une régression du refactor d'affichage** : golden + A/B prouvaient le
  comportement préservé — ces bugs étaient pré-existants, révélés en testant
  les modes. Outils de diag ajoutés (EXCLUDE_FROM_ALL, hors ctest) :
  `artifact_probe` (sweep phase × matrice vs LUT NTSC), `oe_signal_view`
  (vrai GLSL sur signal dumpé), sweep dans `render_total_replay_modes`.

## 2026-05-29

- **Affichage — réorganisation en couches (Phase 0 + 1a).** Préparation du
  passage à une architecture d'affichage en couches (décodage couleur /
  démodulation composite / **effets CRT activables indépendamment du mode**),
  aujourd'hui bloquée parce que `HiResMode` mélange trois axes orthogonaux
  (algorithme de décodage, phosphore mono, et effets — ces derniers
  verrouillés au seul chemin `ColorCompositeOE`). Deux étapes sans aucun
  changement de rendu :

  **Phase 0 — filet de non-régression.** Nouveau `test_display_golden`
  (`display_golden_hash`) : fige un hash FNV-1a du framebuffer pour le
  produit croisé {8 scènes //e + 3 ][+} × {7 pipelines couleur entiers} +
  le générateur de signal composite (88 chemins épinglés). Couvre les
  décodeurs déterministes-entiers ; `ColorAppleWin` exclu du golden (LUT
  bâtie en flottant → quantif. dépendante de l'hôte, déjà couvert par
  `applewin_ntsc_smoke` + son signal d'entrée golden-hashé ici). Régénérable
  via `POM2_GOLDEN_RECORD=1`. *Piège trouvé en l'écrivant* : sur //e les
  soft-switches de pagination `$C00C/$C00D` (80COL) ne répondent qu'aux
  **écritures** (`Memory.cpp:892`) — un `memRead` laisse `eightyCol` à 0 et
  les scènes 80-col/DHGR retombent silencieusement sur le chemin 40-col/HGR.

  **Phase 1a — extraction des primitives de décodage NTSC.** Le cœur porté
  de MAME (bit doubler, `kArtifactColorLut`, `rotl4b`, `buildHgrWordRow`,
  `buildBitStream`, `avgRgb`) sort de l'anonymous-namespace de
  `Apple2Display.cpp` vers `Apple2VideoDecode.h` (`namespace pom2::a2v`,
  header-only `inline constexpr` — évite de câbler un nouveau .cpp dans les
  ~12 cibles qui compilent `Apple2Display.cpp`). Partagé verbatim par HGR /
  DHGR / générateur de signal. Golden + 6 tests d'affichage verts, GUI build
  OK. Doc périmée corrigée au passage (`signal()`/`signalProduced()` :
  le lo-res EST sérialisé ; `kChatMauveHGR` : la DHGR Chat Mauve EST
  modélisée, dans `renderDhgr`).

  **Phase 1b — unification des glyphes texte.** Les 6 copies de la logique
  glyphe→masque-7-bits (renderText, renderText80, renderTextChatMauveFgBg,
  + paintText40/80 du générateur de signal) fusionnées dans un unique
  `glyphRows7()`. Oracle renforcé avant la chirurgie : nouvelle scène
  `textcolorcm` qui couvre le chemin texte couleur Chat Mauve fg/bg
  (96 chemins épinglés au total). Byte-for-byte identique.

  **Phase 2 (lite) — découplage du phosphore.** `kPhosphors[(int)mode]`
  (indexé par la valeur entière de l'énum `HiResMode`, d'où l'obligation
  d'ajouter tout nouvel enumerator en fin de liste) remplacé par un
  `phosphorFor(mode)` à switch explicite + constantes nommées. Amorce de
  l'axe « teinte » indépendant du décodeur couleur. Identique.

  **Phase 3 — pile d'effets CRT universelle (la fonctionnalité phare).**
  Nouveau `CrtEffectStack` (`.h/.cpp`) : passe GLSL RGBA→RGBA qui applique
  scanlines / masque d'ombre / barrel / persistance / brightness-contrast-
  saturation sur le framebuffer de **n'importe quel** mode couleur (Color
  NTSC MAME, mono, Chat Mauve, AppleWin) — plus seulement le chemin OE qui
  cuisait ses effets dans son shader de démodulation. Réutilise `NtscParams`
  (un seul panneau « CRT Settings » pilote les deux). Câblé dans
  `drawScreenImage` pour les modes non-OE, **opt-in** (menu Display → « CRT
  effects on all modes », persistant `crt_effects_all_modes`) et
  passthrough-safe (échec d'init du shader → framebuffer brut, aucun
  plantage). **Vérifié au runtime** : GUI lancée sous X, shader compilé
  (`[INFO] CRT: Universal CRT effect stack ready`), rendu correct en
  ColorNTSC, zéro erreur GL.

  **Phase 4 — suppression du double-rendu AppleWin + unification OE.** Deux
  volets :

  *(a) Double-rendu AppleWin.* En `ColorAppleWin`, `render()` appelait
  `renderInternal()` (colorisation LUT complète de frame80) PUIS
  `AppleWinNtsc::renderFrame` qui réécrivait intégralement frame80 → 1ʳᵉ passe
  100 % jetée. `render()` saute désormais `renderInternal` pour AppleWin
  (fallback défensif conservé). `++frameCounter` déplacé vers `render()` pour
  que FLASH continue d'avancer malgré le skip. Golden byte-identique
  (non-AppleWin) ; AppleWin vérifié au runtime.

  *(b) Unification OE dans la pile d'effets partagée.* `NtscPostProcessor`
  devient **démod-pur** (récupération couleur Y/I/Q + sharpness + hue + PAL,
  sortie 1×, texture unique sans ping-pong) ; tout le « verre CRT »
  (scanlines / masque / barrel / persistance / BCS) est retiré du shader OE et
  vit désormais UNIQUEMENT dans `CrtEffectStack`. `drawScreenImage` chaîne
  `démod → CrtEffectStack` pour OE (toujours actif, pour préserver le look),
  exactement comme les autres modes — **une seule implémentation d'effets**,
  fin de la triple-persistance / double-scanlines.

  *Preuve d'équivalence* (nouvel outil GL hors-ctest `ntsc_oe_ab`,
  `glReadPixels` A/B avant/après sur le serveur GL) : démod pur **max 1 LSB**,
  scanlines **max 1 LSB**, BCS **≤10 LSB** (ordre clamp-vs-grade dû à
  l'intermédiaire 8-bit), barrel re-situé au stade image (visuellement
  équivalent ; les deux configs barrel=0 restent ≤10 LSB → l'écart de la config
  « full » est purement la dé-superposition pixel du barrel). OE **vérifié au
  runtime** (les deux shaders compilent, rendu correct, zéro erreur GL).

  **Phase 5 — aplatissement de `renderInternal`.** L'arbre de décision (≈7
  sorties anticipées) mêlant mode vidéo / machine / carte / mode couleur est
  réorganisé : les deux branches Chat-Mauve-HGR quasi-identiques (Duochrome vs
  décode 6-couleurs, qui ne différaient que par le renderer du haut) fusionnées
  en un seul bloc ; DHGR mixed/plein replié sur l'idiome `hiResEnd =
  mixed ? 160 : 192` ; sections nommées (RGB-card HGR / colour-text / IIe
  80-col / legacy 280) qui se lisent de haut en bas. Chaque branche reste 1:1
  avec l'originale → golden **byte-identique** (les chemins chatmauve hgr /
  hgrmixed / dhgr / textcolorcm de l'oracle couvrent la fusion).

  **Phase 6 — aspect du pixel / présentation.** Le pixel Apple II n'est pas
  carré (la zone active 280×192 remplissait un écran 4:3). `drawScreenImage`
  propose désormais trois modes de présentation (menu *Display → Aspect
  ratio*, persistant `aspect_mode`) : **Square** (1:1 logique ≈ 1.46, défaut
  net), **4:3 (CRT)** (étire à la forme d'un vrai moniteur), **Integer**
  (carré mais multiple entier, sans scintillement de mise à l'échelle
  fractionnaire). Le calcul travaille en dimensions logiques, donc indépendant
  de la texture présentée (280 / 560 / 2× OE). Vérifié au runtime (les trois
  modes rendent correctement, aucune erreur GL).

  **Refactor d'affichage en couches : Phases 0→6 livrées.** L'objectif initial
  — *des couches d'effets CRT activables, en aval de n'importe quel mode
  couleur* — est atteint : un seul étage `CrtEffectStack` partagé par OE,
  AppleWin et tous les décodeurs CPU. **Reste optionnel** (changement UX, pas
  mécanique) : scinder le `HiResMode` public en `ColorPipeline` +
  `CompositeBackend` + `EffectParams` et refondre le menu Display en groupes
  pipeline/backend/effets avec shim de migration `settings.json` — à faire
  avec une validation de la disposition du menu (le modèle plat actuel +
  panneau CRT Settings reste pleinement fonctionnel).

- **Audit multi-agents : 25 bugs corrigés (1 critical, 1 high, 8 medium,
  15 low)**. Audit systématique des 32 sous-systèmes (un agent par
  domaine) suivi d'une vérification adversariale de chaque finding
  (8 faux positifs écartés — comportements corrects/intentionnels laissés
  intacts). Build propre, **99/99 tests verts**.

  **🔴 Critical — freeze à 1 clic.** `File → Reload ROM` se
  self-deadlockait : le handler appelait `hardReset()` *à l'intérieur* du
  `lock_guard(stateMutex)`, or `hardReset()` re-verrouille ce même mutex
  non-récursif (UB → deadlock du thread UI puis du worker CPU). Fix :
  fermer le scope du lock avant `hardReset()`, comme tous les autres
  sites reset/boot du fichier. `MainWindow.cpp`.

  **🟠 High — détection fin-de-phonème SSI263.** En `MODE_IRQ_DISABLED`
  (DR1:0=00), `Ssi263::advance()` ne posait jamais A/!R (D7) : il
  `return`ait avant. Or AppleWin (source de vérité — MAME n'a pas de
  SSI263) lève D7 à la complétion **quel que soit le mode** ; seul l'IRQ
  *hôte* est gated. Les drivers qui scrutent D7 en mode 00 voyaient le bit
  bloqué à 0 + la machine d'état figée (≠ « répétition silencieuse » que
  prétendait le commentaire). Fix : `aRequest_=true` inconditionnel,
  `return irqEnabled()` (ne gate que l'IRQ). `ssi263_smoke`
  (`testIrqDisabledMode`), l'enum `Ssi263.h` et `DEV.md` mis à jour — ils
  épinglaient/décrivaient l'ancien comportement bugué.

  **🟡 Medium.**
  - **Disk II — boucle LSS non bornée** : insérer une disquette pendant
    que le moteur tourne à vide (LSS bit-level actif) laissait `lssCycle`
    figé tandis que `cpuCycleTotal` montait → le 1er `$C0EC` après
    insertion faisait un catch-up de millions d'itérations PROM (freeze
    multi-s). `insertDisk` resync désormais `lssCycle` comme `lssStart`.
  - **M68705 (souris) — IRQ timer** modélisé en latch set-only au lieu de
    la ligne level-sensitive de MAME : masquer (TIM=1) ou acquitter
    (TIR=0) le TCR ne dé-assertait pas le bit pending → ré-entrée parasite
    de l'ISR. Ajout du `else` qui dé-asserte.
  - **Floppy Emu — `goUp()`** sautait à la racine depuis tout
    sous-dossier dès que le SD root finissait par `/` (séparateur final →
    composant vide en fin d'itération de `lexically_normal`). Strip du
    séparateur dans `setSdRoot`.
  - **FloppySound — data race** : `samplesLoaded_` (bool non-atomique)
    publié par `loadSamples()` alors que le callback audio tourne déjà →
    lecture OOB possible sur ARM. Rendu `std::atomic` (release/acquire),
    pinné par static_assert.
  - **Slot Config — draft périmé** : `draftInited` (static jamais remis à
    false) conservait les assignations de l'ancien profil après un
    changement de machine, et **Apply les persistait**. Promu en membre
    `slotDraftInited_`, réinitialisé par `applyProfile` /
    `restartEmulationFromSettings`.
  - **Phasor — /RESET natif** routé via le chip-select au lieu de resetter
    la paire AY inconditionnellement (MAME `via_out_b` traite /RESET hors
    `chip_sel`). /RESET déplacé avant le decode chip-select.
  - **Sony 3.5" — over-read 1 octet** dans `nibblesFromCells` quand
    l'alignement wrap tombait sur une queue toute-à-zéro (`cells[n]`).
    Garde `if (pos==n) return` ajoutée (le chemin `goto found` l'avait
    déjà).
  - **Sony 3.5" — perte de données** : l'eject déclenché par le firmware
    (strobe IWM 0x7) ne flushait pas les blocs write-back. `saveDirty()`
    avant `eject()`, comme le chemin UI.

  **🟢 Low** — races bénignes/latentes & inexactitudes matérielles :
  cassette `playbackActive` atomique ; garde de taille `--load`/`--paste`
  + try/catch sur le thread CLI différé (un `/dev/zero` faisait
  `std::terminate`) ; flag V binaire en ADC décimal **CMOS** (carry-in
  capturé avant écrasement par le carry BCD) ; longueurs des NOP CMOS non
  documentés au désassembleur ($44/$54/$5C/$DC/$FC/$x2…) ; strobe CA2 en
  lecture port-A (MC6821) ; single-step perdu sur race `requestStep`
  (récupération auto) ; budget par frame en `int64` (overflow si
  `--speed`≈INT_MAX) ; échappement `\s`/`\t` des espaces de bord dans
  Settings (round-trip des chemins) ; écriture paddle sous `stateMutex` ;
  lecture `keyReady` $C010 réutilise `kbLatch` (déjà sous lock) ; reload
  T1 continu borné en arithmétique (Via6522) ;
  `glBindAttribLocation(aPos→0)` avant link, + ajout de l'entrée au loader
  GL maison ; `std::call_once` pour l'init des LUTs AppleWin NTSC ;
  open-bus à `0xFF` sur device-select de slot vide (SlotBus, cohérent avec
  slot/expansion ROM).

## 2026-05-28

- **Audit markadev/AppleII-RevEng : 3 nouvelles cartes + correction de
  nommage Echo+**. Suite à l'analyse du dépôt
  https://github.com/markadev/AppleII-RevEng/ (reverse-engineering Apple
  II avec dumps ROM, schémas KiCad, datasheets), trois ajustements
  intégrés dans POM2 :
  1. **`EchoPlusCard` renommé "Cricket / Echo (SSI263)"**. L'audit
     confirme que le vrai *Street Electronics ECHO+* contient 2×
     AY-3-8913 + TMS5220, pas un SSI263 (cf. index.md du dossier
     `Street-Electronics-Corp-ECHO+`). La carte SSI263 existante de POM2
     modélise en réalité la lignée **Cricket** de Street Electronics. La
     clé catalogue `"echoplus"` reste inchangée pour la compat
     `settings.json` ; seule l'étiquette user-visible et les commentaires
     d'en-tête sont mis à jour. Pas d'impact comportemental.
  2. **`ClockCard` accepte le vrai dump Thunderware U9 v1.3**. Probe
     automatique de `roms/thunderclock_u9_v1.3.bin` (alias :
     `thunderclock_u9.bin`, `thunderclock.rom`,
     `Thunderware_REV_1.3_ROM_U9.bin`). Accepte 256 B (slot ROM seul)
     ou 2 KB (slot ROM + miroir $C800-$CFFF de la même puce, conforme
     au câblage Thunderware où la même EPROM 2 KB est décodée dans les
     deux fenêtres). Validation de la signature ProDOS
     `$08/$28/$58/$70` aux offsets 0/2/4/6 — fallback vers la ROM
     synthétique si la signature est absente ou si la taille n'est ni
     256 ni 2048 octets. Source : markadev /
     `Thunderware-Thunderclock-Plus/Thunderware_REV_1.3_ROM_U9.bin`.
  3. **`EchoPlusTMS5220Card` (scaffold)** — nouvelle carte clé
     catalogue `"echoplus_tms"`, modèle minimaliste du vrai Echo+
     (2× AY-3-8913 + TMS5220) avec decode de registre stub à
     $Cs00-$Cs0F : permet la détection logicielle sans crasher le bus.
     Cores LPC10 TMS5220 et synthèse AY-3-8913 différés (le second
     attend l'extraction d'un helper synth partagé depuis
     Mockingboard/Phasor). Audio silencieux en v1.
  4. **`GrapplerCard` (Orange Micro)** — nouvelle carte clé catalogue
     `"grappler"`, parallel printer ROM-gated. Charge
     `roms/grappler_plus.bin` (4 KB) si présent : les 256 premiers
     octets exposent `$CnXX`, les 2 KB inférieurs sont miroir dans
     `$C800-$CFFF` (assez pour la fingerprint de détection logicielle).
     Sans dump, fallback synth ROM identique à `PrinterCard` →
     `PR#n` continue à fonctionner. Source : markadev /
     `Orange-Micro-Grappler+`. Bank-switch des 2 KB supérieurs non
     modélisé (TODO).
  Tests : `grappler_card_smoke` ajouté (stub ROM fingerprint + spool +
  gate de taille à 4 KB). `clock_card_smoke`, `echoplus_card_smoke`,
  `printer_card_smoke` inchangés (zéro régression).

- **Le Chat Mauve Eve — Color TEXT master enable + HGR Duochrome**.
  Le panneau Slot 7 listait deux fonctionnalités Eve "hors scope" :
  livrées. **$C0B8/$C0B9** = bascule maître du Color TEXT (par défaut
  ON pour préserver la pipeline Video-7 historique ; un strobe `LDA
  $C0B8` la coupe et la TEXT 40-col + AN3 retombe sur le rendu mono
  IIe standard). **$C0BA/$C0BB** = HGR Duochrome (off par défaut) :
  bitmap dans MAIN $2000-$3FFF comme du HGR normal, métadonnées
  couleur par octet dans AUX à l'offset image (quartet haut = fg
  index palette lo-res, quartet bas = bg). Nouveau path
  `Apple2Display::renderHgrDuochrome` qui dessine en frame80 à 560
  natif (chaque pixel HGR doublé en 2 dots). La dispatch dans
  `Memory::softSwitchAccess` broadcast les accès `$C0B8-$C0BB` via
  `slots.broadcastVideoSwitch` (lecture ET écriture — le rapport dit
  "écriture" mais les conventions Apple II acceptent les deux ; ne
  coûte rien d'être tolérant). Settings persistés
  (`chatmauve_color_text`, `chatmauve_hgr_duochrome`). Pin :
  `le_chat_mauve_smoke` sections #6 ($C0B8/9 toggle) + #7 (Duochrome
  fg/bg + fallback 6-couleurs après $C0BA). Tests IIe Memory restent
  verts (la broadcast s'ajoute sans modifier la routing existante).

- **Le Chat Mauve — option compat Dragon Wars (`invertBit7`)**. Le RPG
  *Dragon Wars* encode son DHGR Mixed avec le bit 7 inversé par rapport
  à la spec brevetée Video-7/Le Chat Mauve : la zone interface texte
  passe en bouillie monochrome 560 et la zone graphique en couleur 140
  illisible si le décodeur applique la spec à la lettre. Nouvel état
  `LeChatMauveCard::invertBit7_` (off par défaut = strict brevet) ;
  quand activé, XOR `0x80` est appliqué aux octets sources avant
  l'extraction de la bank-flag (HGR 6-couleurs) et du sélecteur
  color/mono (DHGR Mixed). N'affecte ni COL140 (bit 7 ignoré), ni BW560
  (bit 7 ignoré), ni Chunky160 (où bit 7 = données, pas un flag). CLI
  `--rgb-card-invert-bit7[=on|off]` (parité avec AppleWin), checkbox UI
  dans le panneau Slot 7, clé Settings `chatmauve_invert_bit7`
  persistante. Pin : `le_chat_mauve_smoke` section #5 — `$01` à
  col 0 vire de Feline MAGENTA à Feline BLUE quand le flag passe à on,
  et inversement pour `$81`. Panneau ImGui mis à jour : DHGR /
  Color-TEXT fg/bg ne sont plus listés comme "hors scope" (livrés
  précédemment) ; seule la famille Eve (`$C0B9` Color TEXT lock + HGR
  Duochrome) reste explicitement hors scope.

## 2026-05-27

- **Mockingboard Sound II — SSI263 intégré à MB**. Nouveau
  `Variant::SoundII` du `MockingboardCard` : la carte vanilla A/C
  (2× VIA + 2× AY) reçoit en plus un SSI263 à `$Cs40-$Cs44`. Le
  slot ROM decode est ajusté pour carve $40-$4F hors de la mirror
  VIA1 (matche le hardware réel — l'address-decoder du Sound II
  est plus strict que celui de l'A/C). A/!R du SSI263 (inversé sur
  hardware réel) drive VIA1.CA1 ; nouvelle méthode
  `pom2::Via6522::setCa1NegativeEdge()` set `IFR.CA1` quand
  `PCR.0 == 0` (config par défaut des stock Sound II drivers,
  AppleWin-parity). Une fois `IER.CA1` enabled par le driver, le
  slot IRQ s'asserte → handler dequeue le phonème suivant.
  Catalog clé `mockingboard_c` (label "Mockingboard C (Sound II)")
  ajoutée, dispatch dans MainWindow accepte les deux variants
  via paramètre `Variant`. Panel UI Mockingboard étend une section
  SSI263 en bas du window quand `hasSsi263()`. Pin
  `mockingboard_smoke::testSoundIIVariantSSI263` : variant AC
  n'expose pas de SSI263, variant SoundII oui ; writes à $40-$44
  atteignent le chip ; advance past duration → `IFR.CA1` latche ;
  IER.CA1 + 2e phonème → slot IRQ. 5 tests Mockingboard tous verts
  (zero régression sur A/C). **Cleanup `#if 0` Via6522/Ay3_8910**
  appliqué en même temps : Mockingboard.cpp 1014 → 653 lignes
  (−361 = exactement le bloc dead-code de l'extraction Phasor).

- **SSI263 audio — phoneme PCM blob (62 phonèmes, ~313 KB)**. Livraison
  B du SSI263, branchée sur la chip emulation livrée précédemment
  dans le même jour. Import verbatim de `SSI263Phonemes.h` d'AppleWin
  (LGPL, compat avec POM2 GPL3 → la combinaison s'exécute sous GPL3).
  Nouvelle paire `Ssi263PhonemeData.h/.cpp` : header expose
  `kPhonemeInfo[62]` (table offset/length) + `kPhonemeData[156566]`
  (uint16_t PCM signé à 22050 Hz natif) dans namespace
  `pom2::ssi263_data`. `Ssi263::fillAudio` lit le sample à
  l'offset courant, scale par AMP register (R3[3:0]), resample
  vers le host rate via curseur linéaire (pas de filtre — la
  source est déjà band-limited par l'analog output du chip réel),
  loop sur fin de phonème jusqu'au DURPHON suivant ou power-down.
  Power-down (CTL=1) et `FILFREQ=$FF` (silence sentinel)
  squelchent. `EchoPlusCard::AudioSrc::fillAudioBuffer` consomme
  `Ssi263::fillAudio` sous le mutex parent + scale par volume
  master. Pin `ssi263_smoke` étendu : phoneme $05 audio
  RMS = 0.18 (range −0.51 à +0.50 — vraie speech) ; power-down +
  $FF filter sentinel produisent silence exact. Tous tests audio
  cards verts (mockingboard ×5, phasor ×1, printer ×1, ssi263 ×1,
  echoplus ×1).

- **SSI263 speech synth — chip model + Echo+ card (audio silent v1)**.
  Nouveau `pom2::Ssi263` (header + cpp) : modèle Silicon Systems Inc.
  SSI263A partagé, 5 registres ($00-$04), mode soft-bits (00 IRQ
  disabled / 01 frame imm. infl. / 10 phon. imm. infl. / 11 phon.
  trans. infl.), CTL power-down/run, formule de durée parité
  AppleWin (`ms = ((16-(rate>>4))*4096/1023) * (4-(dur>>6))` → 4 ms
  à 256 ms), A/!R bit avec ack via writes $00/$01/$02. **Pas de
  référence MAME** (vérifié 2026-05-27 — aucun `ssi263*` dans
  `src/devices/sound`) ; AppleWin `source/SSI263.cpp` est la ref
  canonique, code POM2 indépendant. Nouvelle carte `EchoPlusCard`
  (Street Electronics Echo+/Echo II) : SSI263 standalone à
  $Cs00-$Cs04, A/!R → slot IRQ direct (pas de 6522), open bus pour
  le reste du slot ROM. Catalog clé `"echoplus"`, slot 4 par défaut,
  pluggable libre. Audio panel Devices → Echo+ avec mode courant,
  state CTL, A/!R, phonème courant, countdown durée (cycles + ms),
  dump 5 registres.
  **v1 audio = silencieux** : MMIO + IRQ timing complets et pinnés
  (`ssi263_smoke` 6 sub-tests + `echoplus_card_smoke` 3 sub-tests),
  mais le blob PCM de 62 phonèmes (~313 KB) sera importé dans un
  commit suivant. POM2 étant GPL3 et AppleWin étant LGPL,
  l'import de `SSI263Phonemes.h` est compatible licence. Pourquoi
  ship silent ici : isoler les hooks bus-protocol (« mon jeu
  Echo+ détecte-t-il la carte ? ») du travail data-import (« les
  phonèmes sonnent-ils correctes ? »). Le 2e commit pourra se
  concentrer sur la qualité audio sans risquer la régression du
  protocole bus. **MockingboardCard variant Sound II** (SSI263 à
  $C(s)40, IRQ via VIA1.CA1) : différée au prochain commit —
  nécessite d'étendre `Via6522.h` pour modéliser le CA1 edge.

- **Phasor (Applied Engineering) — skeleton dual-mode**. Nouvelle carte
  `PhasorCard` : 2 × 6522 VIA + 4 × AY-3-8913 (12 voix en natif),
  mode soft-switch runtime au $C0(8+s)X qui flippe entre
  PH_Mockingboard (compat — 1 AY actif par VIA) et PH_Phasor (natif —
  2 AYs par VIA via chip-select PB3/PB4 actif-bas, clock chip ×2),
  plus PH_EchoPlus (7) routé comme natif en v1. Décode mode :
  `if (offset & 0x8) mode &= ~0x7; mode |= offset & 0x7;` — matche
  AppleWin `Mockingboard.cpp`. Décode chip-select :
  `chip_sel = (~(pb >> 3)) & 3` (4 cas : none / primary / secondary
  / broadcast) — matche MAME `a2bus/phasor.cpp`. Boot en MB-compat
  pour que les jeux Mockingboard tournent unchanged sur un slot
  Phasor. **Audio synth 4-AY mono mix** (livré dans la foulée, même
  commit) : `PhasorCard::AudioSrc` snapshot les 4 register banks +
  reset/env-write counts + `clockScale()` sous le mutex parent, puis
  tourne le synth MAME-parity par chip (tone counter integer + accum
  fractionnel, noise LFSR 17-bit avec prescale, envelope 4-flag state
  machine avec retrigger sur R13). Mix mono divisé par 12 (4 chips ×
  3 channels × peak 1.0) → peak Phasor-native = 1.0 avant la knob
  volume. `clockScale ×2` applique au step-rate des compteurs : mêmes
  register values = notes une octave plus haut en natif (Phasor
  double le chip clock, pas les périodes AY). En MB-compat seul
  AY1+AY3 sonnent → ~6 dB plus bas qu'un vrai Mockingboard
  (compensé via la knob ; le divisor dynamique clipperait en natif
  full-amp). Pin `phasor_card_smoke` étendu : RMS mix 4-AY > 0.05
  avec mute path → 0, et zero-crossing freq estimator confirme
  ratio pitch Phasor/MB = 2.011 sur period $200 (cible 2.0).
  **UI panel** (Devices → Phasor) : bannière mode color-coded (gris MB
  / vert Phasor / bleu EP) + clock multiplier + nombre d'AYs actifs,
  2 cols VIA telemetry, 4 cols AY register banks avec décodage
  ch A/B/C period + volume. Les colonnes AY1/AY3 portent un tag
  "(MB-compat: silent)" en mode MB pour expliquer pourquoi ces bancs
  restent zéro malgré un music driver actif.
  **Refactor préalable** : `Via6522` et `Ay3_8910` (jusque-là
  privées nichées dans `MockingboardCard`) extraites vers
  `Via6522.h` + `Ay3_8910.h` (namespace `pom2::`, header-only,
  inline). MockingboardCard utilise les types extraits via alias
  `using` au file scope ; le code dead-stub reste dans un bloc
  `#if 0` (l. 92-452) en attente de revue avant suppression (TODO
  [Refactor] cleanup). 5 tests Mockingboard tous verts après
  extraction (mockingboard_smoke + sync + 4am_detect +
  irq_delivery + iie_irq) — aucune régression. **UI panel
  Phasor** non livrée dans ce commit (modeste, ~30 min, item
  TODO).

- **Carte imprimante parallèle synthétique (`PrinterCard`)**. Nouvelle
  carte slot-pluggable qui capture les bytes envoyés via `PR#n` dans
  un spool host-side, sauvegardable en `.txt` depuis Devices →
  Printer. **Pourquoi synthétique (pas de portage MAME `a2parprn`)** :
  la vraie carte parallèle pilotait un PIA 6821 avec STROBE/BUSY
  bit-bangés vers une imprimante physique — émuler ça sans imprimante
  câblée n'a aucun observable côté Apple II au-delà de "le byte est
  parti". Modèle synthétique (lignée `ProDOSHardDiskCard`) : 256 B de
  slot ROM dont le seul rôle est de hooker CSWL/CSWH ($36/$37) sur un
  trampoline 4-byte qui STA dans notre port de données ; le reste vit
  côté C++ dans un `std::vector<uint8_t>`. **Pourquoi built-in slot 1
  pour //c et //c+** : matche le « Save as PDF » du print dialog
  macOS — un PR#1 depuis BASIC remplit un fichier directement, zéro
  config. Divergent de MAME (qui keep une 2e SSC à $C100 sur //c
  réel) mais cohérent avec la philosophie POM2 (Mouse au sl4 du //c
  où MAME a Mockingboard). Sur II / II+ / //e la carte reste
  pluggable libre via Slot Configuration (catalogue clé `"printer"`).
  **PDF deferred** — le `.txt` couvre 90 % du use case (copy-paste,
  édition, impression hôte) ; un export PDF monospace ou via libharu
  pourra arriver en suivi. Pin `printer_card_smoke` : ROM fingerprint
  (PR#n trampoline + Pascal autodetect + handler $Cn31) + spool
  data-port semantics + CPU-driven `PR#1` + 3 COUT-style writes.

- **WASM : doc consolidée** (TODO + DEV + CHANGELOG). Les 4 gaps
  connus (IDBFS settings persistence, file drop-zone, touch mobile,
  audio worklet tuning) qui vivaient uniquement dans
  `dist/wasm/README.md` migrent vers `TODO.md` (sections 🟡/🟢
  `[WASM]`). `DEV.md` gagne une section « WebAssembly (browser
  build) » qui documente la branche EMSCRIPTEN du `CMakeLists` (l.
  212-276), les compile-out gates `SuperSerialCard::startListening`
  (l. 153 etc) et `AiControlServer::start` (l. 381-430), et la
  règle pour les futurs éditeurs : *new socket calls must have a
  no-op WASM branch returning a safe sentinel*. Pas de CI WASM
  encore — un refactor sur ces 6 fichiers (main, MainWindow,
  EmulationController, AiControlServer, SuperSerialCard,
  CMakeLists) doit toujours être suivi d'un `./build_wasm.sh
  --clean` manuel.

## 2026-05-26

- **WASM : portage browser (Emscripten) — MVP single-threaded**.
  Nouvelle cible build via `./build_wasm.sh` (driver + dev server
  COOP/COEP) → `dist/wasm/{index.html, POM2.{js,wasm,data}}`.
  Branche `EMSCRIPTEN` dans `CMakeLists.txt:212-276` :
  `-sUSE_GLFW=3 -sUSE_WEBGL2=1`, mémoire 128 MiB growable, IDBFS
  (`-lidbfs.js`) monté `/persistent` pour future persistance, asset
  bundle ROMs+fonts par défaut (disks opt-in via
  `-DPOM2_WASM_BUNDLE_DISKS=ON`). **Pourquoi single-threaded** :
  pas de SAB → pas de COOP/COEP requis → déployable sur
  n'importe quel host statique (GitHub Pages, S3, …) sans
  configurer les headers cross-origin. Le CPU worker thread
  natif est remplacé par `EmulationController::tickFrame()`
  appelé depuis la boucle RAF dans `main.cpp` ; coût pratique
  nul car miniaudio Web Audio tourne dans un worklet géré par
  le browser de toute façon. **Compile-out gates** via
  `#ifdef __EMSCRIPTEN__` : `SuperSerialCard` TCP listener
  (`:153`, `:203`, `:227`, `:241`, `:366`) et `AiControlServer`
  HTTP listener (`:381-430`) deviennent des no-ops retournant
  `false`/sentinelles — les symboles restent déclarés (link
  intact), seule l'impl host-side dégrade ; l'ACIA SSC continue
  d'être émulée côté Apple II. **Pourquoi compile-out plutôt
  que stub comportemental** : pas de sockets POSIX dans le
  sandbox browser, et émuler une stack telnet/HTTP en JS pour
  un MVP n'apporte rien — la GUI reste accessible pour driver
  ces cartes. **`pom2_headless` skipped sous EMSCRIPTEN**
  (`CMakeLists.txt:332`) — pas de TCP, pas de TTY.
  Documentation : `dist/wasm/README.md` (utilisateur),
  `DEV.md § WebAssembly` (developer). Gaps connus → `TODO.md`
  (IDBFS settings, file drop-zone, touch mobile, audio
  worklet). **Pas de CI** — refactors de `main.cpp`,
  `MainWindow.cpp`, `EmulationController.cpp`,
  `AiControlServer.cpp`, `SuperSerialCard.cpp` ou
  `CMakeLists.txt` doivent être suivis d'un
  `./build_wasm.sh --clean` manuel.

- **Memory : `dataMutable()` raw pointer supprimé (footgun debugger /
  snapshot)**. L'accesseur retournait `mem.data()` sans aucun garde —
  un poke debugger ou un futur consommateur (mem editor, restore path
  externe) pouvait silencieusement clobber le ROM ($D000-$FFFF), le
  Monitor ($F800-$FFFF) ou les ROM slots ($C100-$C7FF), avec aucun
  diagnostic ; `restoreMainRam` se protégeait déjà via `writable[]`
  mais rien n'imposait ce passage. Remplacé par deux accesseurs
  narrow : `writeRamUnchecked(addr, val)` (`assert(addr < 0xC000)`,
  bypass paging IIe vers main bank) pour les pokes ciblés, et
  `loadFlatTestImage(src, len)` (`assert(testMode == true)`) pour le
  bulk-load 64 KB des suites Klaus (où l'image inclut volontairement
  la zone ROM-mirror et la mémoire entière doit se comporter en RAM
  flat). 4 callers (tous tests) migrés ; suite verte
  (`test_snapshot_state_roundtrip`, `test_klaus_6502`, `test_klaus_65c02`).
  Bénéfice : le trap d'écriture ROM silencieuse est soit empêché à la
  compilation (pas de pointeur), soit asserté à l'exécution. Item TODO
  🟡 « [Arch] dataMutable() contourne writable[] » clos.

- **Mouse Card : seconde variante AppleWin HLE (`mouseaw`)**. À côté de la
  carte MAME-faithful existante (`mouse`, qui émule le MC68705P3 cycle-par-
  cycle à partir de `mouse_341-0269.bin`), nouvelle classe
  `MouseCardAppleWin` portée verbatim depuis AppleWin
  `source/MouseInterface.cpp` (CMouseInterface). Le MC6821 PIA reste réel
  (réutilise notre chip model) mais la partie MCU est *synthétisée* en C++
  : les écritures dans `$C0nX` passent par `On6821_A` / `On6821_B`, le
  byte de commande sélectionne un opcode (`MOUSE_SET / READ / SERV / CLEAR
  / POS / INIT / CLAMP / HOME / TIME`) et l'état mouse (X, Y, clamps,
  boutons, IRQ status) est calculé directement à partir des entrées hôte.
  Conséquence pratique : la variante AppleWin n'a besoin que de la slot
  EPROM `mouse_341-0270-c.bin` — pas de ROM mask 341-0269. Le routage UI
  (`MainWindow::onMouseMove/Button` + boucle de sync absolue via les
  screen holes `$0478+s` / `$0578+s` / `$04F8+s` / `$05F8+s` / `$07F8+s`)
  est partagé : les deux cartes exposent le même `setHostMouse(rawX, rawY,
  button)`. Plug via Slot Configuration → « Mouse (AppleWin HLE) ». Pinné
  par `mouse_card_applewin_smoke` (slot-ROM bank-select + BIT5 write-
  strobe handshake → MOUSE_INIT canned $FF reply remonté sur PRA).

## 2026-05-25 — BUGJAM

- **M6502 : NMOS undoc 2-byte ops $0B/$2B/$EB traités en 1 octet → desync PC**.
  $0B/$2B = ANC #imm, $EB = USBC #imm sont 2 octets sur NMOS ; la table 65C02
  les laissait en NOP 1-octet, donc en mode NMOS l'octet opérande était
  redécodé comme opcode. Fix : `setCpuMode(NMOS)` les remappe en NOP 2-octets /
  2-cycles (`UnoffImm`). Pin : `cpu_cycle_count_test` (PC += 2).
- **M6502 : cycles des NOP 65C02 non documentés**. Les handlers génériques
  `Unoff2`/`Unoff3` facturaient 3/5 cycles ; les vrais timings varient : NOP
  #imm = 2, NOP zp,X ($54/$D4/$F4) = 4, NOP abs,X ($DC/$FC) = 4 (NOP zp $44 = 3
  reste correct). Les *nombres d'octets* étaient déjà bons (aucun desync) —
  c'est purement la précision-cycle pour ces opcodes quasi-jamais exécutés.
  Nouveaux handlers `UnoffImm`/`UnoffZpX`/`UnoffAbs4`. ($5C = 8 cyc laissé tel
  quel : divergent CMOS/NMOS, jamais exécuté.) Pin : `cpu_cycle_count_test`.
- **Non-bugs confirmés par les tests** (signalés par le bugjam, vérifiés
  *intentionnels*) : (a) lecture ATA hors-plage renvoie des zéros « no crash »
  — comportement délibéré, épinglé par `ata_block_device_test` (« reads as
  zeros ») ; (b) le parseur 2IMG de `Block512Backing` accepte un champ
  header-length à 0 (offset 64) là où `DiskImage` exige ≥ 52 — la version
  permissive de Block512 accepte *plus* d'images réelles, durcir casserait des
  `.hdv`/`.2mg` valides (image WP du test). Les deux laissés inchangés.
- **Mockingboard : sync VIA en fin d'instruction sur-comptait d'une
  instruction**. `Memory::advanceCycles` fait `cycleCounter += cycles` AVANT de
  dispatcher aux slots, mais `M6502::step()` n'a pas encore remis `cpu->cycles`
  à zéro → `getCycleCountNow()` (= cycleCounter + cpu->cycles) dépassait le vrai
  « now » de `cycles`. La carte avançait ses VIA ~k cycles en avance, puis le
  prochain accès MMIO mid-instruction tombait dans le court-circuit
  `now <= lastSyncCycle_` — perdant la précision-cycle là où la lazy-sync devait
  justement la garantir (T1/T2/IFR pour Nox/Skyfox/Broadside). Fix :
  `advanceCycles` synchronise vers `getCycleCountNow() - cycles`. Pin :
  `mockingboard_sync_smoke::testNoEndOfStepOvershoot` (différentiel CPU-step vs
  batch ; échoue sans le fix : 958≠960).
- **Mockingboard : 6522 effaçait IFR.T1 sur écriture T1L-H ($07)**. Sur un vrai
  6522/W65C22 le flag T1 n'est effacé que par lecture de T1C-L ou écriture de
  T1C-H ($05) — jamais par T1L-H ($07). Le commentaire prétendait l'inverse.
  Une IRQ T1 pendante était indûment annulée si un driver ne mettait à jour que
  l'octet haut du latch. Fix : retrait du `ifr &= ~IFR_T1` du cas T1L-H.
- **Memory : lectures des soft-switches $C050-$C05F renvoyaient 0 au lieu du
  floating bus**. Les commutateurs d'affichage + annonciateurs ne pilotent pas
  le bus de données → une LECTURE doit renvoyer l'octet du scanner vidéo
  (comme les chemins paddle/catch-all `isWrite ? 0 : floatingBus()`), pas 0.
  Du code RNG/anti-copie échantillonne ces registres en attendant des bits de
  poids faible non-déterministes. Fix aligné sur la convention du fichier.
- **Audit sanitizer (ASan+UBSan)** : suite complète (90/90) propre, zéro erreur
  mémoire/UB sur les chemins testés.
- (Non-bug confirmé : `Mouseapps Apple2.hdv` « UNABLE TO LOAD PRODOS » = l'image
  n'a pas de fichier de démarrage `PRODOS`/`*.SYSTEM` ; POM2 exécute fidèlement
  le boot1 du disque.)
- **SSC : bloc Pascal 1.1**. L'SSC publiait les octets d'identification
  (`$Cn05=$38`/`$Cn07=$18`/`$Cn0B=$01`/`$Cn0C=$31`) mais pas la table
  d'entrée du protocole firmware Pascal 1.1 → une appli Pascal détectait la
  carte puis sautait dans le remplissage NOP. Ajout de la table en
  **`$Cn0D-$Cn10`** (offsets bas de PINIT/PREAD/PWRITE/PSTATUS — pas
  `$CnFB-$CnFF`, la note du TODO était erronée) + les 4 routines dans
  `buildRom()`, layout & convention d'appel calqués sur le vrai ROM SSC
  (carry=prêt pour PSTATUS, X=0, char 7 bits en A pour PREAD). Pin :
  `ssc_acia_smoke::testPascalIdBlock`.
- **UI : LED de statut par carte**. Nouveau helper partagé `StatusLed.h`
  (gris vide / vert OK / jaune write-protect / **rouge erreur** + tooltip) qui
  unifie les pastilles jusque-là dupliquées (Slot Configuration, SmartPort) et
  ajoute l'état erreur manquant. Câblé en tête des panels HDV, Disk II, 3.5"
  (par lecteur, rouge sur échec de montage via `lastError`) et SmartPort (par
  unité). Lisibilité immédiate de l'état média. (Les LED sont de l'UI hôte
  ImGui, donc invisibles via `/screen.ppm` qui ne capture que l'écran Apple II.)
- **`.hdv` 800K refusé au boot** (« unrecognised disk image ») —
  `classifyDiskForSlot` exigeait `sz > 819200`, donc un `.hdv` de pile 800K
  (1600 blocs, AppleWorks_AW.hdv) tombait en `Unknown`. Un `.hdv` est sans
  ambiguïté un volume disque dur → classé `Hdv` à toute taille 512-alignée ;
  `.2mg` garde le partage 3.5"/HDV par taille. Pin : `cli_kiosk_test`.
- **Tests de pin (dette `3f42efc`)** : `mockingboard_smoke` mesure désormais la
  fondamentale du compteur de tons AY (période 64 → 998.6 Hz vs 998.8 attendu,
  garde le refactor float→entier) ; `ssc_acia_smoke` pin le flag raw-mode
  (défaut OFF + toggle ; LF→CR + IAC FSM déjà couverts) ; `cassette_wav_tail`
  pin l'auto-rewind (défaut OFF + toggle) ; `ai_control_server_smoke` pin
  l'endpoint `/mouse` (lookup carte, compteur 8 bits, clamp ±127, absolu,
  reset, 503/405).
- **`/status` rapportait le mauvais profil** (`Apple ][+` en tournant //e) —
  `MainWindow` figeait le label AI dans le ctor avec `activeProfile` encore au
  défaut, AVANT résolution ; quand le profil sauvé == auto-probe, `applyProfile`
  (qui rafraîchit le label) est sauté. Fix : rafraîchir le label après la
  résolution du profil.
- **Build : `cmake --build . --target POM2` ne faisait rien** (la vraie cible est
  `pom2_imgui`, `POM2` n'est que l'`OUTPUT_NAME`) — ajout d'un alias phony
  `add_custom_target(POM2 DEPENDS pom2_imgui)` (hors `ALL`, donc zéro impact sur
  le build normal).
- **HDV/CFFA : images 32 Mio (65536 blocs) refusées — correctif off-by-one**.
  Le garde de `Block512Backing` rejetait tout `.hdv`/`.2mg` de **plus de
  65535 blocs**, ce qui bloquait au chargement les dumps 32 Mio **exactement
  65536 blocs** (ex. `A2DeskTop-GIST.hdv` → boot kiosk « disk boot failed:
  more than 65535 ProDOS blocks »). Le raisonnement de « round-8 #6 » (« bloc
  65536 inadressable ») confondait **nombre de blocs** et **index max** : un
  numéro de bloc ProDOS est 16 bits → index max `$FFFF` → une image peut
  contenir **65536 blocs** (index 0..`$FFFF`), tous atteints par le
  `selectedBlock` (uint16_t) de la carte. Le STATUS du driver ROM ne renvoie
  d'ailleurs **aucun** compte de blocs (`LDA #$00; CLC; RTS` ; ProDOS lit
  `total_blocks` dans le volume), donc aucun débordement 16 bits à craindre.
  * **Fix** : `kMaxBlocks` `0xFFFF`→`0x10000` ; garde « > 65536 ⇒ reject » ;
    static_assert reformulé sur l'index max. Pin mis à jour :
    `hdv_mass_storage_smoke_test` (65535 ✓, **65536 ✓**, 65537 ✗).
- **MouseCard « axe X bloqué à ~8 px » — non reproductible, item clos**.
  Vérifié sur 4 fronts : (1) `mouse_card_axis_parity_test` headless X=Y=800 ;
  (2) le mapping bits PB + `updateAxis` sont **identiques** à MAME
  `mouse.cpp` (X `dir=$01/clk=$02`, Y `dir=$04/clk=$08`) ; (3) clean-room
  firmware live (rampes égales → X≈Y) ; (4) **A2Desktop lui-même** (l'appli du
  rapport) : injection identique +150 → X=+134, Y=+95 (X tracke *plus* que Y).
  Le symptôme historique était soit déjà corrigé (fix `kWidth` de
  `onMouseMove`), soit propre au clamp d'une appli — pas un bug de la carte.
- **AiControlServer : endpoint `POST /mouse`**. Injection souris pilotable par
  agent : `{"dx","dy"}` (delta Apple-cursor signé, ±127/appel), `{"x","y"}`
  absolu, `{"btn"}`, `{"reset"}`. Atteint la `MouseCard` via le SlotBus,
  maintient un compteur 8 bits continu (comme `MainWindow::onMouseMove`), et
  permet de tester les applis souris en tête-à-tête avec `/screen.ppm` +
  `/mem` (trous écran ProDOS). A servi à clore l'item MouseCard ci-dessus.
- **//c / //c+ : banner corrompu au reboot avec plusieurs périphériques —
  correctif**. Régression du SmartPort embarqué : exposer mon stub bloc à
  `$C500` (à la place du **vrai** firmware SmartPort interne du //c) cassait
  l'autostart du ROM //c dès qu'un disque Disk II (slot 6) ET un média
  SmartPort (slot 5) coexistaient — un ProDOS booté depuis le Disk II (ex.
  le navigateur BITSY) énumérait le slot 5, appelait des points d'entrée
  `$C5xx` que le vrai firmware fournit mais pas mon stub → exécution erronée
  → la page texte se remplissait de fragments de code du stub (« fE1DMS@HP »
  au lieu de « Apple //c », `finalPC=$FD23` moniteur). Trace headless :
  `tests/iic_dual_boot_trace.cpp`.
  * **Fix** : flag « armé » (`Memory::setIicSmartPortArmed`). Le stub `$C500`
    n'est exposé à travers le masque INTCXROM **que** lorsque
    `EmulationController::bootFromSlot` l'arme (boot explicite GUI/CLI) ;
    **tout reset/cold-boot le désarme**. Donc l'autostart du //c voit
    toujours son **vrai** firmware → reboot propre (boot Disk II / banner),
    et le SmartPort se boote via le bouton « Boot » de la Disk Library / Slot
    Configuration. Conséquence : un média SmartPort persisté ne se réamorce
    pas tout seul au reboot (utiliser « Boot »).
  * Corrections de fond du stub au passage : `$Cn07` `$3C`→`$01` ($3C = la
    signature **Disk II**, qui faisait passer le slot 5 pour un 2ᵉ Disk II)
    et **STATUS ProDOS** complet (renvoie le nombre de blocs en X/Y via
    `$C0n5/$C0n6`) — un STATUS incomplet corrompait un scanner de volumes.
  Vérifié à l'écran (//c) : reboot Disk II+SmartPort propre (BITSY), boot
  explicite 3.5" OK (JEUX). 89 tests passent. Voir `project_iic_smartport_boot`.

- **[UI] Fusion Slot Manager → Slot Configuration**. Le panneau autonome
  *Slot Manager* est **supprimé** (`SlotManager_ImGui.{h,cpp}` retirés) ;
  sa fonctionnalité de baies montables est repliée dans une **2ᵉ colonne**
  de *Slot Configuration* (Machine → Slot Configuration). À gauche :
  assignation carte par slot, avec **tout l'intégré grisé/verrouillé**
  (AUX 80-col + built-ins du profil — sur //c/+/c+ : SSC, Mouse, SmartPort,
  Disk II). À droite : **disques internes + ports montables** construits par
  un parcours **live** du SlotBus — baies `MountableMediaCard` (SmartPort
  2 unités avec type-select, CFFA + HDV 1 baie) **et** lecteurs `DiskIICard`
  (5.25" internes), chacun avec montage/éjection/write-back/**Boot slot**.
  `persistMediaBay` (ex-lambda du Slot Manager) promu membre. Visibilité du
  panneau désormais persistée (`show_slot_config`). Vérifié à l'écran en //c.
  89 tests passent.

- **//c / //c+ : boot 3.5" ET HDV via SmartPort embarqué**. Symptôme : en
  profil //c, les images 3.5" et HDV ne bootaient pas depuis la Disk Library
  (le 5.25" oui). Diagnostic en cascade :
  * Les ROM de slot sont **masquées sur tout //c-class** (`IIcClassProfile::
    forcesIntCxRom()` toujours `true`) → une carte slot (`ProDOSHardDiskCard`,
    `SmartPortCard`, `CffaCard`) n'est **jamais bootable** (`$Cn00` lit la ROM
    interne, pas la carte). Auto-brancher une carte slot (l'astuce kiosk
    `ensureHdvCardForBoot`) est donc inopérant en //c-class.
  * **MAME ne modélise pas** le 3.5"/SmartPort sur le //c simple : sa carte
    `A2BUS_IWM` (`bus/a2bus/a2iwm.cpp`, machines apple2c0/c3/c4) n'a que **deux
    lecteurs 5.25"** ("WANTED: there are no ROM dumps"). Seul le //c+ a un 3.5"
    (IWM+MIG+Sony porté en `SmartPortHub`), mais son **boot 3.5" n'a jamais
    marché** (état documenté : "banner + 5.25" only" ; vérifié → "UNABLE TO
    FIND A BOOTABLE DISK ONLINE"). Donc aucun //c-class ne bootait 3.5"/HDV.
  * **Solution** (pas un portage MAME — il n'y a rien de fidèle à porter pour
    le //c simple) : un **SmartPort servi par l'hôte** au slot 5 intégré. Le
    vrai //c y avait son firmware SmartPort ; POM2 substitue un **stub bloc**
    (la `SmartPortCard` déjà prouvée sur //e). `Memory::memRead` perce un trou
    unique à `$C500-$C5FF` (bank 0) à travers le masque INTCXROM **ssi** la
    carte slot-5 a un média (`SlotPeripheral::exposesIicOnboardRom` — garde
    contre un boot autostart d'une carte vide → JMP $0801 sur du vide). Le
    device-select (`$C0D0-$C0DF`) n'étant jamais masqué, le protocole bloc
    `$C0D0-$C0D4` atteignait déjà le bus. Câblage : `routeMount35` utilise la
    carte SmartPort sur **tous** les profils (plus seulement non-//c+) ;
    `routeMountHdv`/`ensureHdvCardForBoot` routent le HDV //c-class vers le
    SmartPort slot 5 (jamais une cffa/hdv slot — masquée). Profil //c : ajout
    de `smartport35` en slot 5 intégré (le //c+ l'avait déjà). `bootFromSlot(5)`
    boote. Vérifié à l'écran : //c & //c+ × 3.5"+HDV (JEUX, Total Replay, ARCHON).
    Pin : `tests/iic_onboard_smartport_test.cpp` (+ `system_profile` mis à jour).
    88 tests passent. Détails : `project_iic_smartport_boot`.

- **Bug hunt round 10 — correctifs (CPU 65C02 / snapshot / Settings / Le Chat
  Mauve)**. Audit des sous-systèmes restants : couche HTTP AiControlServer,
  cœur CPU M6502, décode soft-switch / Language-Card / RamWorks de Memory,
  sérialisation de snapshot, Le Chat Mauve + Settings. **AiControlServer**
  (testé ASan — pas de crash/OOB distant) et **paging Memory** (LC + RamWorks)
  ressortent **propres**. 90 tests passent (nouveau pin : `settings_roundtrip`).
  * **[CPU] flag V de `SBC` décimal sur 65C02 faux** (`M6502.cpp` `SBC`) —
    l'overflow était calculé depuis l'accumulateur **ajusté BCD** au lieu de la
    différence binaire ; diverge du matériel (MAME `w65c02`) sur **2400**
    entrées. Recalculé depuis `(uint8_t)tmp` (binaire) → identique au matériel
    sur les 131072 entrées. NMOS inchangé. Pin : `cmos_6502_smoke`.
  * **[CPU] `(zp,X)` sous-compté d'1 cycle** (`M6502::IndZeroX`) — les 8 ops
    `(zp,X)` (LDA/STA/ADC/AND/CMP/EOR/ORA/SBC) prenaient 5 cycles au lieu de
    **6** (lecture-bidon du pointeur zero-page non indexé manquante). +1.
    Pin : `cpu_cycle_count`.
  * **[CPU] `ASL/LSR/ROL/ROR abs,X` sur 65C02 sur-comptés** (`M6502.cpp`) —
    facturés 7 cycles fixes ; le 65C02 prend **6** (7 seulement si page
    franchie). Nouvelle adresse `RmwAbsX` ; `INC/DEC abs,X` restent 7 sur les
    deux CPU. Pin : `cpu_cycle_count`.
  * **[Snapshot] section CPU : garde de longueur trop laxiste** (`AiControlServer.cpp`
    chargement) — la garde était `len >= 9` mais le lecteur consomme **16**
    octets → un snapshot forgé/tronqué (9–15 o) lisait jusqu'à 7 octets au-delà
    de la section (compteur de cycles / mode CPU corrompus, silencieux ;
    atteignable via l'API localhost). Garde `len >= 16` + docstring corrigée
    (les lignes IRQ/NMI ne sont pas persistées — signaux transitoires). Pin :
    `ai_control_server_smoke` (section CPU courte → ignorée).
  * **[Settings] valeur contenant `#` tronquée au rechargement** (`Settings.cpp`)
    — `load` coupait après le premier `#` n'importe où, mais `save` écrivait
    brut : un chemin `/home/u/My#Disks/jeu.dsk` rechargeait en `/home/u/My`
    (montage cassé silencieux). `#` n'est désormais un commentaire qu'en début
    de ligne. Pin : `settings_roundtrip`.
  * **[Settings] valeur contenant un saut de ligne corrompait le fichier** —
    échappement `\\`/`\n`/`\r` à l'écriture, dés-échappement à la lecture →
    round-trip sans perte pour toute chaîne. Pin : `settings_roundtrip`.
  * **[Settings] écriture atomique défaite à l'échec de flush** (`save`) — le
    stream était testé AVANT `close()` ; une erreur différée (disque plein)
    renommait alors le `.tmp` tronqué par-dessus la bonne config. `flush()`+
    `close()`+vérification avant le `rename`.
  * **[Le Chat Mauve] décalage FIFO parasite au reset** (`LeChatMauveCard`) —
    `an3Prev` initialisé à `false` ; un `$C05F` isolé après reset était vu
    comme front montant et clockait la FIFO. AN3 démarre HAUT (DHIRES off,
    comme MAME `device_reset` `m_dhires=false`) → `an3Prev = true`. Pin :
    `le_chat_mauve_smoke`.
  * **Vérifié propre** : AiControlServer (parsing HTTP/JSON, bornes mem
    peek/poke, cycle de vie sockets, threading — testé ASan), décode
    soft-switch Memory (machine LC write-enable, combinaisons paging IIe,
    banques RamWorks, lectures de statut).

- **Bug hunt round 9 — correctifs (Disk II / profils / threading)**. Audit des
  sous-systèmes restants : contrôleur Disk II + LSS, parsers d'images disque,
  bus de slots + agrégation IRQ, `EmulationController` (horloge/threads),
  machinerie de profils (`applyProfile`). **Parsers `DiskImage`** (WOZ/DSK/NIB/
  2MG, robustesse sur fichiers forgés/tronqués) et **`SlotBus`/`SlotPeripheral`**
  (bornes de slot, wire-OR IRQ, cycle de vie) ressortent **propres**. 89 tests
  passent (nouveau pin : `disk_wp` ; `diskii_lss` étendu).
  * **[Disk II] écriture d'une disquette protégée en écriture** (`DiskIICard.cpp`
    sites d'écriture + `DiskImage` `writeFlux`/`writeNibbleAt`/`saveDirty`) — les
    sites ne testaient que `writeBackEnabled`, pas la **WP physique du médium**
    (WOZ INFO+2 / flag 2IMG). `isWriteProtected() = fileWriteProtected ||
    !writeBackEnabled` reportait bien la WP au logiciel, mais un logiciel qui
    l'ignore et écrit quand même corrompait le buffer puis **écrasait le fichier
    source** à l'éjection. Sur vrai matériel la WP **inhibe le courant
    d'écriture** : `writeFlux`/`writeNibbleAt`/`saveDirty` honorent désormais
    `fileWriteProtected` (nouveau getter `isFileWriteProtected()`). Pin :
    `disk_wp` (le médium WP n'est muté ni en RAM ni sur disque).
  * **[Disk II] spin-down bloqué quand le lecteur sélectionné est vide**
    (`DiskIICard::advanceCycles`) — le `return` anticipé sur `!isLoaded` était
    AVANT le décompte du délai d'arrêt moteur, donc un motor-off sur un lecteur
    vide laissait `motorOn`/`active` coincés indéfiniment (LED + son). Le chemin
    données reste gardé par la présence du média, mais le spin-down tourne
    désormais quoi qu'il arrive. Pin : `diskii_lss` (cas spin-down sans disque).
  * **[Disk II] getters par-lecteur non bornés** (`DiskIICard.h`) — index `drive`
    non validé → accès hors bornes des tableaux `images[]`/`headQuarterTrack[]`/
    `trackPos[]`. Bornés (défaut sûr hors plage), cohérent avec insert/eject.
    Pin : `diskii_lss` (assertions OOB).
  * **[Profils] `applyProfile` appliquait les slots intégrés du MAUVAIS profil**
    (`MainWindow_Slots.cpp`) — `plugSlotsFromSettings()` (étape 7) lit
    `activeProfile` pour forcer les cartes built-in, mais `activeProfile = p`
    n'était posé qu'à l'étape 12. Switch VERS //c/c+ → cartes embarquées (SSC/
    souris/SmartPort/Disk II) non forcées (pas de contrôleur de boot, y compris
    au démarrage où le ctor appelle `applyProfile(saved)`) ; switch DEPUIS //c/c+
    → fuite des built-in dans un II+/IIe propre. `activeProfile = p` remonté en
    étape 0.
  * **[Threading] lecture inter-threads non verrouillée des registres CPU**
    (`MainWindow.cpp`, panneau Emulation) — PC/A/X/Y/SP + compteur de cycles lus
    sans verrou pendant que le worker les écrit sous `stateMtx` (data race / UB ;
    tous les autres lecteurs prennent le verrou). Snapshot sous `stateMutex()`.
  * **[Profils] RamWorks III désactivé sur le //e Unenhanced** — la grille ne
    testait que `AppleIIe` (Enhanced) ; le //e 1983 a le même bus aux. Étendu aux
    deux variantes //e.
  * **[Profils] RAM auxiliaire non effacée au cold-switch VERS un profil IIe** —
    `clearRam()` n'efface l'aux que si `iieMode` est déjà vrai, mais tournait
    AVANT `setIIEMode`. Réordonné : `setIIEMode` → `clearRam` → `resetSoftSwitches`
    (et `setIIEMode` reste avant `loadAppleIIRom`).
  * **[Threading] `stop()` ne bloque pas jusqu'à l'arrêt du worker** (latent) —
    documenté : les appelants exigeant l'exclusivité après `stop()` doivent
    prendre `stateMutex()` (c'est le verrou, pas `stop()`, qui sérialise).
  * **Note** : les correctifs profils/threading vivent dans `MainWindow*` (classe
    GUI sans harnais de test unitaire) — validés par compilation + suite verte +
    vérification structurelle, comme les autres changements MainWindow du repo.

- **Bug hunt round 8 — correctifs (SmartPort / sécurité ProDOS / threading
  audio / ATA)**. Audit des sous-systèmes encore vierges : pile bloc SmartPort,
  backing bloc partagé + ATA/HDV, chemin audio (Speaker/Cassette), entrées
  (joystick/paddles/souris), moteur d'affichage. **Entrées** et **affichage**
  ressortent **propres** (seuls des commentaires périmés). 87 tests passent
  (3 nouvelles cibles : `smartport_io_error`, `prodos_decode_safety`,
  `speaker_overflow`).
  * **[Sécurité — ProDOS synth] traversée de chemin au write-back**
    (`ProDOSVolume.cpp` `decodeVolumeToFolder`) — le nom de chaque entrée de
    répertoire était lu depuis l'image **inscriptible par l'invité** et joint
    au dossier hôte sans validation : un nom forgé `../PWNED` s'échappait du
    bac à sable quand le write-back est actif (même classe que les évasions
    symlink/`goUp` corrigées R4/R6). Nouveau garde `isHostSafeProDOSName`
    (rejette séparateurs, NUL, `.`/`..`, hors `[A-Za-z0-9.]`) appliqué aux
    deux sites de jonction (fichier + sous-répertoire). Pin :
    `prodos_decode_safety`.
  * **[SmartPort] READBLOCK/WRITEBLOCK silencieusement « réussis »**
    (`SmartPortCard.cpp`) — un `readBlock`/`writeBlock` hors-plage renvoyait
    `CLC` (succès) à ProDOS : lecture → buffer de `0xFF` + carry clair, et
    désync du flux d'octets ; écriture → données perdues sans erreur. La carte
    verrouille désormais un bit d'erreur I/O ($C0n4 bit 0) ; les routines ROM
    lecture/écriture le testent et renvoient carry-set ($27). Le flux reste en
    phase (cache 0xFF). L'allongement de la routine de lecture (34→45 o) a
    décalé le bloc d'écriture → opérande `BEQ write` recalculé `$2E`→`$39`
    (revalidé par `smartport_write_dispatch`). Pin : `smartport_io_error`.
  * **[SmartPort] sélection d'unité par masque, buffer d'écriture non purgé**
    (`SmartPortCard.cpp`) — `$C0n0` utilisait `& (kMaxUnits-1)` (faux si
    kMaxUnits non puissance de 2) → `% kMaxUnits` + `static_assert` ; et ne
    purgeait pas `writeBufPrimed_`/`readCacheValid_` → un write à moitié
    streamé pouvait commiter des octets périmés. Les deux corrigés.
  * **[Audio — Cassette] data race sur les drapeaux de mode**
    (`CassetteDevice`) — `audioStreamMode` (lu sans verrou sur le thread audio,
    muté sur le thread UI) et le compteur de rampe (touché sous deux mutex
    différents) rendus `std::atomic` (comme `playbackPaused`/`muted`). Pin :
    `static_assert` de discipline threading dans le constructeur (revertir vers
    un type nu casse la compilation).
  * **[Stockage — ATA] écriture sur médium write-protected « réussie »**
    (`AtaBlockDevice.cpp`) — une commande WRITE accordait DRQ puis jetait
    silencieusement les données (`flushBufferToSector`) sur un backing WP, sans
    `ERR`. ATA-1 : abort avec `ERR`+`DF` en statut et `ABRT` dans le registre
    d'erreur, pas de DRQ. Atteignable via une image 2IMG WP sous CFFA. Pin :
    `ata_block_device` (cas WP).
  * **[Stockage] plafond de blocs off-by-one** (`Block512Backing.h`) —
    `kMaxBlocks` acceptait 65536 blocs alors que les numéros de bloc ProDOS
    sont 16 bits (max 65535, et `selectedBlock` est un `uint16_t`). Ramené à
    `0xFFFF` (+ `static_assert`). Pin : `hdv_mass_storage` (frontière 65535
    accepté / 65536 rejeté).
  * **[Audio — Speaker] perte de toggle à l'overflow inversait la parité**
    (`SpeakerDevice.cpp`) — le haut-parleur est une bascule 1 bit : chaque
    événement est un flip de parité. Jeter UN toggle à l'overflow inversait le
    niveau reconstruit de tous les échantillons suivants → on jette désormais
    par **paires**. Pin : `speaker_overflow`. + suppression de l'accesseur mort
    `getAudioCpuCursor()` (lecture 64-bit déchirée, zéro appelant).
  * **Vérifié propre** : entrées (timing RC paddle = MAME `apple2.cpp`,
    quadrature souris MAME-fidèle), moteur d'affichage (interleave HGR/texte,
    DHGR aux/main, lookup char-ROM 2K/4K), bornes lecture/écriture du backing
    bloc, dispatch/persistance/propriété SmartPort.

- **Bug hunt round 7 — correctifs (SnapshotIO / clavier-paste / char-ROM /
  désassembleur)**. Audit des sous-systèmes restants : I/O snapshot,
  pipeline clavier/coller, chargeur de ROM de caractères, désassembleur du
  Memory Viewer. Le cœur NMOS du désassembleur ressort **correct** ; seul le
  jeu 65C02 manquait. 84 tests passent (2 nouvelles cibles : `disasm_cmos`,
  `char_rom`).
  * **[SnapshotIO] longueur de section non bornée → crash** (`SnapshotIO.cpp`
    `nextSection`) — un `.snap` forgé (ou tronqué) déclarant une longueur de
    section gigantesque pilotait une allocation/`readBytes` dimensionnée par
    le fichier lui-même (`bad_alloc` → crash, ou over-read sur section
    tronquée). La taille du fichier est désormais relevée à l'ouverture et
    **chaque** section est rejetée si `sectionEnd > fileSize` (ou overflow).
    Une seule borne couvre crash + over-read pour tous les consommateurs.
    Atteignable via l'API AI-control (localhost). Défense en profondeur
    additionnelle : cap 16 Mio sur la section MEX (`AiControlServer.cpp`) +
    `try/catch` autour de `handleClient` dans le worker. Pin :
    `snapshot_io_smoke` (4 cas malformés : longueur géante, longueur>fichier,
    longueur tronquée, taille exacte acceptée).
  * **[Clavier] frappe live pendant un coller écrasait la FIFO**
    (`Memory::queueKey`) — une touche arrivant en plein coller faisait
    `lastKey=…` et clobberait l'octet de coller latché (saut de file). Elle
    est maintenant **appendée** derrière le coller si une file est en cours,
    et garde le comportement matériel « dernière touche gagne » sinon. Pin :
    `paste_smoke`.
  * **[Clavier] un reset n'abandonnait pas le coller en cours**
    (`Memory::resetSoftSwitches` IIe + `resetSoftSwitchesWarm` II/II+) — la
    `pasteQueue` survivait à F11/F12 et continuait à se vider après le reset.
    `pasteQueue.clear()` ajouté aux deux chemins. Pin : `paste_smoke`.
  * **[Clavier] cap du coller par-appel et non sur la file vivante**
    (`Memory::pasteText` + `pasteRawKeys`) — des collers répétés pouvaient
    faire croître `pasteQueue` sans borne (DoS mémoire via AI-control /
    presse-papiers). Le cap `kPasteMaxChars` est désormais calculé contre
    `pasteQueue.size() + (keyReady?1:0)`. Pin : `paste_smoke`.
  * **[Clavier] coller minuscules sur ][/][+** (`Memory::pasteText`) — le
    clavier II/II+ n'a pas de minuscules ; `a-z` → `A-Z` au coller quand
    `!iieMode` (un vrai clavier ne peut émettre `$61-$7A`), les claviers
    IIe-class gardent la casse. Pin : `paste_smoke`.
  * **[char-ROM] dump 8K accepté puis rendu en garbage** (`Memory::loadCharRom`)
    — seuls 2K (II/II+) et 4K (IIe) ont un chemin de normalisation ; toute
    autre taille était stockée brute et dessinée en glyphes corrompus. Le
    gate rejette désormais proprement tout ce qui n'est pas 2K/4K (8K, taille
    impaire, vide) et vide `characterRom` sur lecture courte. Pin :
    `char_rom`.
  * **[Désassembleur] jeu 65C02 décodé en `???` 1 octet** (`Disassembler6502`)
    — l'émulateur tourne en 65C02 par défaut mais le désassembleur était
    NMOS-only ; les opcodes CMOS sortaient en `???` de 1 octet, **désynchro-
    nisant** tout le listing Disasm (piège classique des BBR/BBS 3 octets
    traités comme 1). Ajout d'une table CMOS (TSB/TRB/STZ/BRA/PHX/PHY/PLX/PLY/
    INC A/DEC A/(zp)/JMP (abs,X)/RMB/SMB/BBR/BBS/WAI/STP…) sélectionnée par un
    flag `cmos` câblé depuis `MainWindow` via `MemoryViewer::setCmosMode`
    (poussé chaque frame selon `M6502::getCpuMode`). Pin : `disasm_cmos`
    (mnémonique + longueur, dont la longueur 3 octets des BBR/BBS, + repli
    NMOS → `???`).

- **Bug hunt round 6 — correctifs (Floppy Emu / SSC threading / ProDOS / son)**.
  Audit des sous-systèmes encore vierges (gadget BMOW Floppy Emu, ClockCard,
  pont TCP/telnet SSC, décode ProDOS, son disque). ClockCard ressort
  **propre** (seul un octet d'année superflu dans le shift, inoffensif pour
  ProDOS). 82 tests passent.
  * **[Floppy Emu] évasion du sandbox SD par symlink** (`FloppyEmuDevice.cpp`
    `listing`) — `directory_iterator`/`is_directory` suivent les liens ;
    skip des symlinks pour rester dans `floppyemu/`. Pin : `floppy_emu_smoke`.
  * **[Floppy Emu] `goUp` testait la LONGUEUR de chaîne** (pas un préfixe) —
    un frère plus long que la racine échappait ; check par composants.
  * **[Floppy Emu] taille d'un favori manquant = garbage** (`file_size` `ec`
    ignoré → `(uint64_t)-1`) → 0. Pin : `floppy_emu_smoke`.
  * **[Floppy Emu] curseur du browser non remis à 0 au changement de listing**
    (entrée/sortie de dossier, toggle Favoris, switch de mode) →
    surbrillance sur une entrée périmée (`FloppyEmu_ImGui.cpp`).
  * **[SSC] machine à états IAC telnet persistante** (`SuperSerialCard.cpp`
    `processTelnetRx`) remplace le filtre sans état : gère `IAC SB … IAC SE`
    (sous-négociation longueur variable — la rafale NAWS d'un client telnet
    fuyait ~5 octets en RX) ET une séquence IAC coupée entre deux `recv()`.
    Pin : `ssc_acia_smoke`.
  * **[SSC] data race sur la ligne IRQ** — le thread TCP appelait
    `setIrqLine`/`assertIrq` (RMW non-atomiques) en concurrence avec le
    thread CPU. `M6502::IRQ`/`irqSourceMask` rendus atomiques + l'IRQ du
    worker est *marshalée* sur le thread CPU via `SuperSerialCard::
    advanceCycles` (assertIrq devient CPU-thread-only).
  * **[SSC] race de cycle de vie des sockets** — le thread UI fermait
    `clientFd`/`listenFd` pendant que le worker était dans `recv`/`accept`
    (use-after-close / double-close). fds atomiques ; l'UI ne fait que
    `shutdown()` (réveil), le worker est seul à `close()`, `listenFd` fermé
    après `join()`.
  * **[ProDOS] round-trip de nom de fichier non idempotent** (décode) — un
    nom à extension conservée (ex. `NOTES.DATA`) accumulait `.bin` à chaque
    sauvegarde ; fichier sans extension → `.bin`. Fix : extensionless →
    type 0x00 (typeless) à l'encode, `0x00` → "" au décode, et pas d'ajout
    d'extension si le nom en a déjà une. Round-trip lossless pour les cas
    courants. (Chemin atteint via le write-back host-folder synth.)
  * **[ProDOS] trous de fichier sparse tronquaient** (décode, défensif) — une
    entrée d'index `0x0000` faisait `break` au lieu de zéro-remplir +
    continuer. Corrigé (non atteignable sur volumes synth — 0 bloc libre —
    mais correct si le décode est un jour étendu aux images réelles).
    Seedling `eof>512` → warn.
  * **[Son disque] `kSeekJoinMs == kSeekTimeoutMs == 100`** →
    chevauchement : un flux de pas à la cadence-join max pouvait déclencher
    le timeout entre deux pas → clic « atterrissage » parasite en plein
    seek ; + bande morte 50-100 ms (gap classé seek mais sans sample →
    clic). `kSeekJoinMs` abaissé à 50 (= plage de `pickSeekSample`), timeout
    100 > join → plus de chevauchement, plus de bande morte (résultat
    audible inchangé). (`FloppySoundDevice.h`)
  * **Reporté** : Floppy Emu `automount` (jamais câblé — *feature*, pas un
    bug de correction ; wire-up délicat à l'ordre de boot) ; garde anti-cycle
    de répertoire ProDOS (le plafond de profondeur 16 empêche déjà le hang ;
    défensif sur image corrompue, impossible sur volume synth) ; fichiers
    *tree* ProDOS (>128 Ko) déjà skippés proprement.

- **Bug hunt round 5 — 9 correctifs (affichage / persistance / cassette)**.
  Audit des dernières zones non couvertes (pipeline vidéo, parseurs de
  formats disque non-WOZ2, orchestration boot/slot/persistance MainWindow,
  cassette + joystick). Les parseurs disque sont ressortis **propres** (aucun
  over-read sur taille/offset fournis par le fichier). 82 tests passent.
  1. **[Affichage] texte-40-col / lo-res / HGR simple lisaient l'AUX sous
     80STORE+PAGE2** (`Apple2Display.cpp`). Le scanner vidéo //e ne
     multiplexe l'aux qu'en 80-col/DHGR ; en 40-col (80COL off) il lit
     TOUJOURS la page 1 PRINCIPALE. La base page1/page2 est déjà choisie par
     `videoTextPage2/videoHgrPage2` (= `page2 && !80store`, MAME
     `use_page_2()`) ; lire cette base depuis l'aux était un bug montrant du
     garbage aux pour les programmes qui font du page-flip 80STORE hors
     80-col. 4 redirections supprimées (renderText/renderLoRes/renderHiRes/
     renderHiResChatMauve80). Pin : `hgr_render` (MAIN=blanc, AUX=noir).
  2. **[GUI] le mode kiosque n'était pas read-only pour 3.5"/HDV**
     (`MainWindow.cpp` routeMount35/routeMountHdv) — `settings->save()`
     inconditionnel écrasait `state.cfg` depuis un lancement kiosque. Gardé
     sur `!kiosk_`.
  3. **[GUI] `POM2 game.hdv` (sans --kiosk) fuyait une carte fantôme dans la
     config sauvée** — `ensureHdvCardForBoot` est « non persisté » par
     contrat, mais `~MainWindow` écrivait `slot_N_card="hdv"` + `hdv_path`.
     Slot auto-provisionné tracé (`autoProvisionedHdvSlot_`) et exclu de la
     persistance.
  4. **[GUI] persistance CFFA multi-instance** — `~MainWindow` ne sauvait que
     la carte CFFA primaire ; une 2e carte perdait son image. Boucle sur
     `blockCards()` (comme DiskII).
  5. **[GUI] `restartEmulationFromSettings` perdait le média live** — un
     Insert menu / mount HDV/CFFA ne met à jour que la carte live (clés
     écrites au shutdown), donc un « Apply » de Slot Config reconstruisait
     depuis des clés périmées → disque monté perdu. Snapshot du média live
     dans les settings avant le teardown (symétrique avec `applyProfile`).
  6. **[Cassette] dernière impulsion de chaque bande WAV/MP3 perdue**
     (`CassetteDevice.cpp` `pcmToDurations`) — une durée n'était émise que
     SUR un changement de signe ; le segment tenu après le dernier
     zéro-crossing (leader final / demi-bit) n'était jamais converti. Flush
     de la queue après la boucle (symétrique avec le trailer 0,1 s du save).
     Pin : `cassette_wav_tail`.
  - **LOWs** : `diskDialogTargetSlot` remis à -1 à la fermeture du popup ;
    sniff ProDOS `next >= 280` (vol 280-blocs = blocs 0..279) ; valeur paddle
    centrée 127→128 (cohérence avec la valeur live + défaut Memory).
  - **Reporté** : SmartPort multi-instance demi-supporté (pas de persistance
    au shutdown — MED, involved) ; `loadAudioStream` mode flux mort (jamais
    dispatché — décision feature, pas un bug de correction) ; overload
    `loadFile(path,order)` Force-secteur-ordre limité au 143360 brut (latent,
    aucun appelant UI).

- **Bug hunt round 4 — 8 correctifs (serveur HTTP AI / souris / VIA / banking //c)**.
  Audit des sous-systèmes restants (serveur de contrôle HTTP, MC6821 + souris,
  6522 VIA, banking //c/c+). Le **bug X de la souris a été circonscrit** : la
  carte (MC6821 + quadrature + M68705) est fidèle à MAME (prouvé end-to-end
  contre le vrai firmware) — l'asymétrie X/Y est dans le GUI. 81 tests passent.
  1. **[AI HTTP] `/speed` gèle l'émulateur** (`AiControlServer.cpp`).
     `cycles_per_frame` (un `long`) était casté en `int` sans borne sup →
     `9999999999`→1,4 G cycles/frame (UI gelée) ; `4294967296`→0 (CPU en
     pause). Bornage [1, 2 000 000] + rejet 400 hors plage. Pin :
     `ai_control_server_smoke`.
  2. **[AI HTTP] `jsonGetString` matchait la clé n'importe où** (substring
     `body.find("\"key\"")`) → une valeur contenant le nom d'un autre champ
     détournait le match (toutes les routes). Réécrit en scan structurel qui
     saute les valeurs-chaîne (la clé ne matche qu'en position de clé, suivie
     de `:`). Pin : `ai_control_server_smoke`.
  3. **[AI HTTP] évasion du jail cwd via symlink** (sauvegarde snapshot).
     `weakly_canonical` rend un symlink final lexicalement (dans cwd) mais
     `ofstream` le suit hors du jail → écriture de fichier arbitraire. Rejet
     du symlink final (`symlink_status`). Pin : `ai_control_server_smoke`.
  4. **[GUI souris] X 2× trop sensible en 80-col/DHGR** (`MainWindow.cpp`).
     Le widget est toujours dessiné en `kWidth`(280)×`kHeight`(192), mais
     `sxRatio` divisait par `display->width()`=560 en DHGR (vs 192 pour Y) →
     X suivait 2× plus vite que Y en 80-col (là où tourne A2Desktop). Utilise
     `kWidth/kHeight` pour les deux axes. (Le « stuck à 8 px » résiduel est
     très probablement le clamp X d'A2Desktop, hors émulateur.)
  5. **[AI HTTP] `/status` échantillonnait CPU et disque sous deux locks**
     séparés → snapshot incohérent (le worker relâche le lock entre chunks
     de 4096 cycles). Lecture sous un seul `lock_guard`.
  6. **[VIA] `t1FireArmed` init membre (`true`) ≠ `reset()` (`false`)**
     (`Mockingboard.cpp`) — landmine latente (ctor reset, inoffensif
     aujourd'hui). Aligné à `false`.
  7. **[//c] décode IOUDIS incomplet** (`Memory.cpp`). MAME : sur //c TOUT
     `$C078/A/C/E` est SETIOUDIS et tout `$C079/B/D/F` CLRIOUDIS ; POM2 ne
     décodait que `$C078/E` / `$C079/F`. Décodage par parité sur
     `$C078-$C07F`.
  8. **[//c] reset MIG non gardé sur `isPlus_`** (`MemoryProfile_IIcClass.cpp`).
     MAME garde le reset MIG sur `m_isiicplus` ; un //c 32 K (rev 0/3/4) a une
     banque alt mais pas de MIG. Gardé sur `isPlus_`.
  - **[#4/#5/#6/#7/#8]** correctifs GUI / refactor / banking LOW non pinnés
    unitairement (intégration MainWindow, cohérence concurrente, profil //c
    coûteux à monter) — couverts en régression (iic/iicplus boot traces).
  - **Reporté sans corriger** (avec raisons) : 6522 SR/CA-CB/PCR non modélisés
    (features hors scope, déjà dans TODO) ; reload T1 continu `+3` vs `+2`
    (disputé, impact <0,5 % sub-audible — ne pas toucher un timing fonctionnel
    sans oracle MAME cycle-trace) ; `jsonGetInt` hex négatif + DoS mono-thread
    (mineurs).

- **Bug hunt round 3 — 9 correctifs (CLI / audio / 6805 souris)**. Audit des
  sous-systèmes restants (cœur 6805 du MC68705P3, pile audio, profils + CLI,
  synthèse ProDOS + RamWorks). La synthèse ProDOS et RamWorks sont ressorties
  **propres** (re-vérifiées vs spec + 3 images réelles). 81 tests passent.
  1. **[CLI] `parseAddr16` adresses + garbage** (`CliDispatcher.cpp`). Décodage
     incohérent (branche décimale gatée sur la longueur > 4) + `std::stol` sans
     vérif de consommation totale → `"12ZZ"`→$12, `"  5"`→$5 acceptés
     silencieusement. Réécrit : adresses **hex** (convention Apple II, comme
     `README` « hex address »), préfixe `$`/`0x` optionnel, premier caractère
     hex obligatoire, token entièrement consommé, plage $0000-$FFFF. Pin :
     `cli_kiosk`.
  2. (avec #1) rejet du garbage / hors-plage.
  3. **[CLI] `--preset iie-u` injoignable** (`CliDispatcher.{h,cpp}` +
     `main.cpp`). `CliPreset` n'avait pas `AppleIIeUnenhanced` et
     `parsePresetName` aucun cas — alors que `profileFromKey` (menu) et la doc
     les listent. Ajout de la valeur enum, des alias
     (`iie-u`/`iieunenhanced`/`apple2e-1983`/`//e-u`) et du `case` du switch
     `main.cpp`. Pin : `cli_kiosk`.
  4. **[CLI] `--ii-plus` écrasé par le profil persistant** (`MainWindow` ctor).
     `forceIIPlus` n'était honoré que dans l'auto-probe legacy ; le rattrapage
     « saved profile » réappliquait ensuite un `iie/iic` sauvegardé. Gardé : le
     profil persistant est ignoré quand `forceIIPlus`.
  5. **[CLI] `--35-disk1/2` sans gate de profil** (`main.cpp mount35Cli`).
     Montait dans le hub 3.5" on-board même hors //c+ (contrat documenté //c+
     only). Gate `currentProfile()==AppleIIcPlus` + warning sinon.
  6. **[Audio] requeue d'événements speaker en ordre inversé**
     (`SpeakerDevice.cpp`). `push_front` en k croissant inversait la queue des
     événements résiduels (fin de buffer) → invariant ascendant rompu →
     glitches de timing de clic. Reverse-iteration.
  7. **[Audio] enveloppe AY non re-déclenchée sur réécriture R13 même valeur**
     (`Mockingboard.{h,cpp}`). Un vrai AY-3-8910 redémarre l'enveloppe à
     CHAQUE écriture R13 ; POM2 ne re-init que sur valeur changée. Le thread
     audio ne voit pas une écriture même-valeur via le snapshot de registres →
     compteur monotone `ayEnvWriteCount_` (calque de `ayResetCount_`). Pin :
     `mockingboard_smoke` (la sortie est AC-couplée comme la vraie carte, donc
     le test mesure le transitoire de rampe).
  8. **[6805] CLR mémoire faisait une lecture parasite** (`M68705P3.cpp`).
     MAME `clr` est write-only (ARGADDR) ; POM2 passait par `rmw_mem` (rdmem
     d'abord) → sur une adresse de port ($00/$01/$02) ça déclenchait une
     lecture (synthèse de front quadrature côté souris). Helper `clr_mem`
     write-only pour `$3F/$6F/$7F`. Pin : `m68705_decode`.
  9. **[6805] MUL ($42) absent** → décodé en NOP. Opcode HMOS réel (X:A = X×A,
     efface H et C). Ajouté. Pin : `m68705_decode`.
  - **[#4/#5/#6]** correctifs GUI/arrondi non pinnés unitairement (intégration
    MainWindow / chemin de fin de buffer) — couverts en régression.

- **Bug hunt round 2 — 6 correctifs (stockage / FDC / snapshot)**. Audit des
  sous-systèmes non couverts au round 1 (IWM, pile 3.5"/SmartPort, cartes
  bloc ProDOS, glue système). 81 tests passent.
  1. **[SmartPort] WRITE_BLOCK firmware exécutait la routine READ**
     (`SmartPortCard.cpp` `buildRom`). Erreur d'offset d'un octet dans le
     dispatch ProDOS `$Cn50` : `BEQ write` encodé `$22` (saute à
     l'offset 53 = milieu de la routine READ) au lieu de `$2E` (offset 65 =
     routine WRITE). Toute écriture ProDOS via le firmware (la carte
     s'annonce write-capable, `$CnFE=$13`, et ProDOS dispatche par
     `$CnFF=$50`) **lisait** le bloc dans le buffer appelant, n'écrivait
     rien sur le média, et renvoyait « succès » → perte de données
     silencieuse + corruption du buffer. Pin CPU-driven :
     `smartport_write_dispatch`. (Les pins existants n'exécutaient que le
     protocole brut `$C0nX`, jamais le dispatch `$Cn50`.)
  2. **[Sony 3.5"] tête bloquée en pas-vers-track-0** (`Sony35Drive.cpp`
     `strobeWriteRegister`). `directionIn_` n'était jamais remis à false :
     reg 0x0 → inward, reg 0x4 → eject, **aucun chemin outward** (la branche
     cyl+1 du step était morte). Réaligné sur MAME `mac_floppy_device::
     seek_phase_w` : reg 0x0 = DirNext (cyl+1, outward), reg 0x4 = DirPrev
     (cyl-1, vers track 0), reg 0x7 = StartEject. Les tracks 1-79 du 3.5"
     étaient inatteignables via le protocole. Pin : `sony_seek_direction`.
  3. **[Snapshot] restore incomplet** (`AiControlServer` + `M6502` +
     `Memory` + `SnapshotIO`). Le load (a) jetait A/X/Y/P/SP (M6502 n'avait
     que `setProgramCounter`) et (b) la section MEM ne couvrait que les
     64 Ko principaux visibles — aux RAM, Language-Card RAM, banques
     RamWorks, soft-switches de pagination (`iieMemMode`) et DisplayState
     n'étaient jamais sérialisés. Tout programme IIe/aux/LC restaurait du
     garbage. Ajout des setters registres M6502, de
     `Memory::appendSnapshotState`/`loadSnapshotState` (nouvelle section
     « MEX », format **v2** — les v1 se chargent toujours) et de
     `restoreMainRam` (respecte `writable[]` → la ROM n'est plus écrasée).
     Pin : `snapshot_state_roundtrip`.
  4. **[IWM] délai async `+14` non mis à l'échelle** (`IWMDevice.cpp`).
     POM2 fait tourner la FSM IWM sur l'horloge CPU et divise les
     constantes de fenêtre MAME (base 7,16 MHz) par 7 ; ce `+14` était resté
     brut → le timer « data devient stale → 0 » des lectures 3.5" async se
     déclenchait ~7× trop tard. 14/7 = 2.
  5. **[CFFA] octets de config EEPROM non patchés** (`CffaCard.cpp`
     `loadRom`). MAME `a2cffa.cpp` device_start() force `m_rom[0x800]=13`,
     `m_rom[0x801]=13` à chaque boot (le dump brut porte `0x04`/`0x00`,
     `$C801=0` désactive le 2e connecteur) ; POM2 chargeait verbatim → le
     firmware scannait une EEPROM différente de l'oracle MAME. Pin ajouté à
     `cffa_card_smoke`.
  6. **[IWM] sous-dépassement `halfWindowSize() - 7`** (`IWMDevice.cpp`,
     FSM d'écriture). `halfWindowSize()` mis à l'échelle ∈ {1,2} moins `7`
     brut sous-dépassait en `uint64_t`. 7/7 = 1 ; corrige aussi les deux
     offsets de chargement `+7` → `+1`.
  - **[#4/#6]** pinnés au niveau régression (`iwm_device_smoke` +
    `iic`/`iicplus` boot traces) — un test de timing dédié exigerait un
    harnais flux-replay disproportionné.

## 2026-05-24

- **Bug hunt — 5 correctifs de parité (audit multi-sous-systèmes)**. Tous
  pinnés ; 78 tests passent.
  1. **[CPU] Cycles d'entrée IRQ/NMI perdus** (`M6502.cpp` `step`).
     `handleIRQ`/`handleNMI` accumulaient les 7 cycles d'entrée dans
     `cycles`, mais `executeOpcode()` réinitialise `cycles = 1` juste après
     → les 7 cycles étaient **écrasés** avant `advanceCycles`. Chaque
     interruption coûtait donc **0 cycle**, désynchronisant toutes les
     horloges dérivées de `cycleCounter` (VBL, timers de slot type
     Mockingboard 6522 T1, cassette) sur le logiciel piloté par IRQ. Corrigé
     en capturant le coût d'entrée puis en le réinjectant après
     l'instruction. Même classe que le bug RMW de Mr. Robot. Pin :
     `cpu_cycle_count` (IRQ + NMI = 7 + instr).
  2. **[Disques] `writeFlux` largeur de cellule codée en dur à 8 LSS**
     (`DiskImage.cpp`). Le chemin de lecture espace les cellules à
     `i*lssCyclesPerCell()`, mais le write-back divisait par `8`. Pour un
     WOZ2 dont `optimal_bit_timing != 32` (cyc ≠ 8) les transitions étaient
     éparpillées dans les mauvaises cellules → **corruption silencieuse** de
     la piste au write-back. (Le test `woz_writeback` fixe `info[39]=32`,
     d'où l'angle mort.) Corrigé via `lssCyclesPerCell()` dans les deux
     branches. Pin : `woz_writeflux_smoke` (obt=40/28 round-trip).
  3. **[CPU] `INC A`/`DEC A` 65C02 ($1A/$3A) facturés 4 cycles** au lieu de
     2 (`M6502.cpp`) : le corps ajoutait `+2` en plus du fetch + `Imp`.
     Aligné sur INX/DEX (corps = 0). Pin : `cpu_cycle_count`.
  4. **[SSC] CR NUL telnet → CR parasite** (`SuperSerialCard.cpp`
     `normalizeLineEndings`). RFC 854 : un retour-chariot nu arrive en
     `CR NUL`. Le NUL exécutait `prevCR = false` **avant** d'être supprimé,
     si bien qu'un `CR NUL LF` produisait `CR CR` au lieu de `CR` (ENTER
     doublé en session telnet interactive). Corrigé en supprimant le NUL en
     premier ; fonction rendue `static` publique pour le test. Pin :
     `ssc_acia_smoke`.
  5. **[Memory] Floating bus ignore 80STORE** (`Memory.cpp` `floatingBus`).
     Le scanner vidéo n'honore PAGE2 que si 80STORE est OFF
     (MAME `apple2video.cpp` `use_page_2() = page2 && !80store`) ; avec
     80STORE ON, PAGE2 redirige la sélection de banque aux et non la page
     affichée. `floatingBus` lisait donc la mauvaise page DRAM sous tout
     programme 80-col/DHGR. Pin : `floatingbus_page2_smoke`.

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
