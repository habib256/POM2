# Rapport de parité vidéo / couleur / effets — POM2

## 1. Objectif

Ce document consolide les audits de parité VÉRIFIÉS des sous-systèmes vidéo, couleur et effets de POM2 face à leurs sources de vérité respectives (MAME, AppleWin, OpenEmulator). L'objectif est la **quasi-parité** avec une **qualité maximale** : reproduire fidèlement le comportement matériel cité, tout en assumant les enrichissements délibérés et documentés (afterglow, anti-moiré analytique, deux gris Le Chat Mauve).

Chaque finding ci-dessous a été contre-vérifié contre le code POM2 et la source de référence, avec un verdict (`confirmed` / `adjusted` / `refuted`). Ce rapport ne retient que les findings `confirmed` et `adjusted` ; les findings `refuted` sont écartés et les points `uncertain` traités à part. Les sévérités affichées sont les **sévérités ajustées** issues de la vérification.

---

## 2. Tableau de synthèse

| Sous-système | Source réf | Parité % | Findings confirmés (crit/high/med/low) |
|---|---|---|---|
| MAME NTSC LUT (ColorNTSC / ColorComp4Bit) | MAME | 90 % | 0 / 1 / 1 / 3 |
| AppleWin NTSC (ColorAppleWin) | AppleWin | 97 % | 0 / 0 / 0 / 5 |
| **OpenEmulator composite (ColorCompositeOE)** | OpenEmulator | **72 %** | 0 / 0 / 2 / 6 |
| **Effets CRT (barrel / scanlines / masque / persistance)** | OpenEmulator + crt-lottes | **72 %** | 0 / 0 / 2 / 6 |
| Le Chat Mauve / Video-7 RGB | AppleWin | 88 % | 0 / 0 / 1 / 7 |
| **Phosphores monochromes (White / Green / Amber)** | AppleWin | **78 %** | 0 / 0 / 0 / 4 |
| Génération du signal composite 14.318 MHz | AppleWin + MAME | 96 % | 0 / 0 / 0 / 6 |
| Lo-res / texte / 80-col / MouseText / flash | MAME + AppleWin | 90 % | 0 / 0 / 0 / 5 |
| Palettes 16 couleurs (NTSC + Feline) | MAME + AppleWin | 99 % | 0 / 0 / 0 / 5 |

> **Chantiers principaux** (parité < 80 %) : **OpenEmulator composite (72 %)**, **Effets CRT (72 %)** et **Phosphores monochromes (78 %)**. Voir la note dédiée en fin de rapport.

---

## 3. Findings par sous-système

### 3.1 MAME NTSC LUT — ColorNTSC / ColorCompMedium / ColorComp4Bit (90 %)

#### ColorComp4Bit (square filter, `composite_color_mode=2`) dévie ~50 % de MAME — mauvaise origine de nibble + mauvaise rotation de phase  *(HIGH)*

- **POM2 vs origine** : dans une fenêtre `ContextBits=3`, POM2 extrait le nibble 4 dots comme `(w >> 3)` et le tourne de `absX` (HGR) / `absX+1` (DHGR). MAME utilise une fenêtre `ContextBits=1` avec `rotl4(w & 0x0f, x + is_80_column - 1)`. La fenêtre POM2 est décalée d'un dot vers la droite ET la rotation est décalée de +1 phase ; les deux ne se compensent pas. Le commentaire de code affirmant que « plier le -1 de MAME dans l'origine de fenêtre donne la même couleur » est prouvé faux.
- **Preuve** : `src/Apple2Display.cpp:1104-1121` (HGR), `:1451-1466` (DHGR) ; MAME `apple2video.cpp:487-494` + `rotl4` `:318-320`. Oracle randomisé : POM2 vs MAME = **49,98 % / 50,11 %** de pixels divergents (interior dots), sur les deux valeurs de `is_80`. Le correctif vérifié donne **0 / 2 208 000** divergence.
- **Recommandation** : remplacer les deux blocs square-filter.
  - HGR (`Apple2Display.cpp:1115-1117`) : `const unsigned nibble = (w >> (kContextBits - 1)) & 0x0Fu; loresIdx = rotl4b(static_cast<uint8_t>(nibble | (nibble << 4)), static_cast<unsigned>(absX - 1));`
  - DHGR (`Apple2Display.cpp:1456-1459`) : même nibble `(w >> (kContextBits-1))`, rotation `static_cast<unsigned>(absX)` (car `absX + 1 - 1`).
  - `rotl4b` gère un compte négatif via `(-count)&3`, donc `absX-1` à `absX=0` est sûr. Mettre à jour le commentaire trompeur `:1105-1114` / `:1452-1455` et pinner avec un smoke test square-filter MAME (analogue à `video7_parity_smoke_test`).

#### La doc de comparaison a dérivé du code pour ColorComp4Bit et affirme « aucune déviation »  *(MEDIUM)*

- **POM2 vs origine** : `docs/graphics_modes_comparison.md:105-113` documente le square filter comme `nibble = (window >> 3) & 0x0F; palette_idx = rotl4b(..., absX - 1)` et affirme « Déviations : aucune. Port littéral de MAME :486-493. » Or le code réel utilise une rotation `absX` (HGR) / `absX+1` (DHGR), pas `absX-1` — la doc ne correspond ni au code ni à MAME, et l'affirmation de parité bit-exacte est fausse (la formule du doc est mesurée ~75 % divergente).
- **Preuve** : `docs/graphics_modes_comparison.md:98-117` vs `src/Apple2Display.cpp:1116-1117`. Le doc cite aussi une plage de lignes périmée (`1030-1037`).
- **Recommandation** : après application du correctif square-filter ci-dessus, mettre à jour `docs/graphics_modes_comparison.md:105-113` vers `nibble = (window >> (kContextBits-1)) & 0x0F; palette_idx = rotl4b(nibble|(nibble<<4), absX + is_80 - 1)` ; alors seulement le label « port littéral de MAME :487-494 » devient exact. Ajouter la référence du nouveau smoke test à la ligne Tests (`:115`).

#### HGR sous-échantillonne 560 → 280 pixels par moyennage de paires ; MAME sort 560 natifs  *(LOW — ok-intentional)*

- **POM2 vs origine** : `renderHiRes` décode 560 sous-pixels puis moyenne les paires adjacentes en frame 280-large (`Apple2Display.cpp:1131-1133`, `avgRgb` `Apple2VideoDecode.h:123-129`). MAME écrit toujours 560 pixels natifs (`out[col*14+b]`), laissant la limitation de bande au scaling aval. DHGR (560 dots directs) correspond à MAME.
- **Preuve** : `src/Apple2Display.cpp:1131-1133` ; les runs de couleur unie restent bit-exacts (moyenne de deux valeurs identiques), seuls les liserés mono-pixel aux transitions diffèrent.
- **Recommandation** : intentionnel et documenté (limitation optique de bande chroma d'un vrai CRT, `Apple2VideoDecode.h:121-122`). Acceptable. Pour parité maximale, on pourrait rendre HGR en buffer 560-large comme DHGR et laisser le scaling faire la limitation — optionnel. **Aucune action requise.**

#### Remplissage floating-bus MAME (mots = `0x3fff`) non modélisé en HGR  *(LOW — missing-feature)*

- **POM2 vs origine** : `buildHgrWordRow` lit toujours `ram[rowAddr+col]` ; POM2 alloue toujours la RAM complète, donc aucune branche « adresse > ram_mask ». MAME remplit les rangées hors-masque avec `empty_words[40]` de `0x3fff` (« socket RAM vide → entrée TTL flotte haut »).
- **Preuve** : MAME `apple2video.cpp:750-762` ; aucune branche analogue dans `src/Apple2VideoDecode.h:89-102`.
- **Recommandation** : très basse priorité — ne concerne que les configs à RAM réduite / délibérément non peuplées, que POM2 ne modélise pas. **Laisser tel quel.**

#### Palette lo-res Le Chat Mauve / Video-7 diverge intentionnellement de MAME (idx 5 ≢ 10)  *(LOW — ok-intentional)*

- **POM2 vs origine** : `ChatMauveRGB` utilise `kChatMauveLoResPalette` avec deux gris distincts (idx 5 = olive `0x9f977e`, idx 10 = mauve `0x78687f`), sourcés d'AppleWin `PaletteRGB_Feline`. MAME réutilise `apple2_palette[]` où idx 5 = idx 10 = `0x808080`.
- **Preuve** : `Apple2Display.cpp:758-796` (commentaire explicite) ; MAME `apple2_palette` idx5==idx10==`0x808080` ; `DEV.md:225-226`.
- **Recommandation** : amélioration de fidélité délibérée et documentée vs MAME (les deux gris teintés du vrai board). **Conserver.**

---

### 3.2 AppleWin NTSC — ColorAppleWin (97 %)

Tous les findings sont `confirmed` en sévérité **LOW** ; aucun bug de math couleur. Le port d'`initChromaPhaseTables` et des quatre filtres IIR est fidèle au coefficient près.

#### Clamp/scale RGB final en double (POM2) vs float (AppleWin) — arrondi sub-LSB  *(LOW — deviation)*

- **POM2 vs origine** : `yiqToRgb` garde tout en double (`const double fr = clamp01(y + 0.956*i + 0.621*q); r = (uint8_t)(fr*255.0);`). AppleWin calcule la matrice en double puis caste en **float** AVANT clamp et `*255.0f` ; le cast intermédiaire peut changer l'octet tronqué de 1 LSB.
- **Preuve** : `AppleWinNtsc.cpp:121-126` vs AppleWin `NTSC.cpp:839-844` + `878-880`, `clampZeroOne` signature `NTSC.cpp:312`.
- **Recommandation** : pour la bit-exactness, répliquer le float intermédiaire (`float fr = clampZeroOne_f((float)(...)); r = (uint8_t)(fr * 255.0f);`). Différence ≤ 1 LSB ; le double POM2 est sans doute plus précis. **Basse priorité.**

#### Tables B&W (`g_aBnWMonitor` / `g_aBnwColorTV`) non construites  *(LOW — missing-feature)*

- **POM2 vs origine** : POM2 ne construit que les 3 LUTs couleur (Monitor / Color-TV / Idealized). AppleWin remplit en plus les tables luma-only B&W qui pilotent son composite VT_MONO.
- **Preuve** : `AppleWinNtsc.cpp:77-79` (3 hue tables seulement) vs AppleWin `NTSC.cpp:813-822`.
- **Recommandation** : intentionnel et inoffensif — POM2 a ses propres modes MonoWhite/Green/Amber dédiés. **Aucune action**, sauf si un vrai sous-mode « B&W composite avec ringing » est un jour souhaité.

#### État statique des filtres reporté entre appels (AppleWin) vs zéro-init par build (POM2)  *(LOW — ok-intentional)*

- **POM2 vs origine** : POM2 instancie des filtres frais zéro-initialisés par build (`Iir2 fSignal, fChroma, ...`). Les taps d'AppleWin sont `static`, zéro-init seulement au premier lancement — un second appel d'init réutilise les taps résiduels (quirk latent, non délibéré).
- **Preuve** : `AppleWinNtsc.cpp:142` + note `87-92` vs AppleWin `NTSC.cpp:943-944` (2 call sites `:2026` & `:2120`).
- **Recommandation** : aucun changement. POM2 reproduit exactement le chemin PRIMAIRE d'AppleWin et est déterministe ; il est de fait plus correct ici.

#### Sous-mode Tv : blend 50 % frame précédente absent d'AppleWin  *(LOW — ok-intentional)*

- **POM2 vs origine** : POM2 moyenne chaque canal 50/50 avec la même scanline de la frame précédente. AppleWin n'a aucun terme temporel sur ce chemin ; sa douceur est SPATIALE (interpolation inter-scanlines intra-frame).
- **Preuve** : `AppleWinNtsc.cpp:250-265` vs AppleWin `NTSC.cpp:1044` ; `DEV.md:387-389`.
- **Recommandation** : approximation stylistique délibérée et documentée. Pour coller au look TV réel d'AppleWin, envisager plutôt une interpolation verticale intra-frame. **Garder derrière le toggle Tv**, basse priorité.

#### Sous-mode Idealized (boost chroma ×1.6) — POM2-only, correctement signalé  *(LOW — ok-intentional)*

- **POM2 vs origine** : 3e LUT réutilisant la luma Monitor (y0) avec I/Q ×1.6. AppleWin n'a pas ce mode.
- **Preuve** : `AppleWinNtsc.cpp:73, 191-195` ; explicitement « Not part of the AppleWin port » + `DEV.md:390-391`.
- **Recommandation** : **aucune action de parité.** N'affecte ni Monitor ni Tv.

---

### 3.3 OpenEmulator composite — ColorCompositeOE (72 %) — CHANTIER PRINCIPAL

> Note de cadrage : la doc interne (`DEV.md:248-302`) déclare explicitement cette implémentation « OpenEmulator-inspired/style » réécrite depuis la spec publique NTSC + le notebook explainer, **sans copie du code libemulation**. Les recommandations « port ligne-à-ligne » ci-dessous sont donc à comprendre comme **optionnelles, si l'objectif est une parité OE stricte**, et non comme des corrections de bug.

#### Filtre passe-bas de démodulation : deux gaussiennes au lieu du FIR Chebyshev×Lanczos d'OE (bandes erronées)  *(MEDIUM — quality-gap, sévérité ajustée de high)*

- **POM2 vs origine** : POM2 utilise deux gaussiennes 17-taps (`sigmaY=0.8` luma, `sigmaC=1.75` chroma, pilotable au slider). OE utilise une fenêtre `chebyshevWindow(17,50)` normalisée puis `lanczos(17, fc)` par canal, avec luma fc = 2,0 MHz et chroma fc = 0,6 MHz. Mesures recalculées : POM2 luma -3dB = 2,37 MHz, chroma -3dB = 1,09 MHz (OE : 1,65 / 0,64 MHz) → chroma ~1,8× trop large, color bleed plus marqué.
- **Preuve** : `NtscPostProcessor.cpp:187-206`, `Apple2Display.cpp:275-310` vs OE `OpenGLCanvas.cpp:793-825` + `screenEmu.js:512-513`. (NB : l'écart « 3,6× » de l'audit initial mélangeait MHz et gain ; le gain réel POM2 à f=0.10 est ~0,55, pas 1,09.)
- **Recommandation** : précalculer côté CPU les trois kernels 17-taps OE — `w = chebyshevWindow(17,50)` normalisé, `wy = normalize(w·lanczos(17, 2.0e6/14318180))`, `wu=wv = normalize(w·lanczos(17, 0.6e6/14318180))·2` — et les passer en uniformes `c0..c8` pour la forme séparable OE. Corrige d'un coup la fuite de sous-porteuse en luma ET la chroma trop large. **À cadrer comme amélioration optionnelle vers parité-OE exacte.**

#### Le chemin luma ne rejette pas la sous-porteuse couleur (pas de notch) → dot-crawl / crosstalk luma-chroma  *(MEDIUM — quality-gap, sévérité ajustée de high)*

- **POM2 vs origine** : la gaussienne luma étroite (`sigmaY=0.8`) laisse passer ~45 % de la sous-porteuse f=0.25 dans Y (`|H(0.25)| = 0.455`) → scintillement sur zones colorées. Le Lanczos luma d'OE (fc=0.1397) annule pratiquement la sous-porteuse (`|H(0.25)| = 0.002`). Côté chroma, POM2 rejette correctement (`sigmaC=1.75 → |H(0.25)| = 0.023`), donc pas de crosstalk symétrique.
- **Preuve** : `Apple2Display.cpp:275` / `NtscPostProcessor.cpp:187` vs OE `OpenGLCanvas.cpp:818`.
- **Recommandation** : adopter le kernel Lanczos luma OE (finding précédent) règle intrinsèquement ce point. Élargir la gaussienne (`sigmaY ~1.6`) sur-floute la luma — la bonne solution est le FIR Lanczos. Dégradation de qualité, pas défaut bloquant.

#### Décalage de phase chroma 0 vs colorburst -33° d'OE (roue de teinte tournée)  *(LOW — deviation intentionnelle, sévérité ajustée de medium)*

- **POM2 vs origine** : phase = `π/2·floor(fx)` (offset 0), sans terme -33°. OE référence la chroma au colorburst -33° (`p.x=0.9083` cycle). POM2 a été **délibérément recalibré sur la LUT d'artefact MAME** (commentaire inline « Probe-calibrated against the MAME LUT: with the YUV matrix the correct phase offset is 0 »).
- **Preuve** : `NtscPostProcessor.cpp:185-186, 200` ; `Apple2Display.cpp:288-294` vs OE `screenEmu.js:363` + `OpenGLCanvas.cpp:68`.
- **Recommandation** : déviation intentionnelle documentée — **ne pas changer le code.** Documenter explicitement dans `DEV.md` que « la phase OE est volontairement abandonnée au profit d'une teinte alignée MAME », et ne l'ajuster que si la repro couleur OE exacte (et non MAME) devient l'objectif. (NB : la valeur `0.9162 rad` citée dans l'audit est incohérente avec `2π·33/360=0.5760 rad` ; recalculer si jamais on l'implémente.)

#### Phase couleur par ligne (phaseInfo) et alternance PAL différentes d'OE  *(LOW — deviation)*

- **POM2 vs origine** : grille de phase globale (dépend de `floor(fx) mod 4`, identique chaque ligne) ; PAL inverse le signe V/Q sur lignes impaires via `palQSign`. OE pilote via une texture 1D `phaseInfo` par ligne. Pour l'Apple II NTSC (`phaseAlternation=[false]`, colorburst constant), les deux se réduisent au même comportement constant-phase non-alterné → **fonctionnellement équivalents**.
- **Preuve** : `NtscPostProcessor.cpp:172-177, 203` vs OE `screenEmu.js:544` + `OpenGLCanvas.cpp:69`.
- **Recommandation** : OK pour l'Apple II NTSC (intentionnel). Si une émulation PAL OE exacte est un jour voulue, piloter la négation V depuis un flag `p.y` par ligne et utiliser la vraie sous-porteuse PAL (4 433 618,75 Hz). **Basse priorité.**

#### Gain chroma ×2.0 conforme à OE ; pas de double application avec saturation  *(LOW — ok-intentional)*

- **POM2 vs origine** : U, V ×2.0 dans la boucle d'accumulation, conforme à `wu/wv.normalize()*2` d'OE. La saturation vit en aval dans `CrtEffectStack.cpp:202`, vérifiée absente de la passe demod.
- **Preuve** : `NtscPostProcessor.cpp:202-203`, `Apple2Display.cpp:308-309` vs OE `OpenGLCanvas.cpp:822,825`.
- **Recommandation** : **aucun changement** — gain correct, pas de double comptage.

#### Matrice décodeur et coefficients YUV→RGB exacts OE  *(LOW — ok-intentional)*

- **POM2 vs origine** : `r = Y + 1.139883·V; g = Y - 0.394642·U - 0.580622·V; b = Y + 2.032062·U` — identique à l'`OEMatrix3` d'OE dans les DEUX chemins (GPU et CPU). C'est le point de parité le plus solide.
- **Preuve** : `NtscPostProcessor.cpp:218-222`, `Apple2Display.cpp:313-315` vs OE `OpenGLCanvas.cpp:910-912`.
- **Recommandation** : **aucun changement.**

#### Alignement sous-porteuse π/2 par dot (0.25 cycle/sample) correct  *(LOW — ok-intentional)*

- **POM2 vs origine** : `phase = π/2·floor(fx)` → 0.25 cycle/sample sur la grille 4×fsc. OE : `NTSC_FSC/NTSC_4FSC = 0.25` exact. L'alignement 4× (dot clock Apple II = colorburst×4) est exactement correct.
- **Preuve** : `NtscPostProcessor.cpp:200`, `Apple2Display.cpp:292` vs OE `screenEmu.js:183-184`.
- **Recommandation** : **aucun changement.**

#### Signal composite en 0.0/1.0 dur ; OE Apple II = blackLevel 0 / whiteLevel 1 (sans gamma) — correspond  *(LOW — ok-intentional)*

- **POM2 vs origine** : `signalBuf` = `0x00/0xFF` → `0.0/1.0`, aucun gamma. OE Apple II ImageInfo : blackLevel=0, whiteLevel=1, signal linéaire, aucun gamma. Les deux sont bilevel (pas de niveaux luma intermédiaires de rise-time analogique) → ils concordent.
- **Preuve** : `Apple2Display.cpp:305` vs OE `screenEmu.js:539-540`.
- **Recommandation** : **aucun changement** — signal linéaire 0..1 sans gamma fidèle à OE.

---

### 3.4 Effets CRT — barrel / scanlines / masque / persistance / B-C-S (72 %) — CHANTIER PRINCIPAL

#### Le decay de persistance n'est pas normalisé au frame-rate et omet le plancher de bruit OE  *(LOW — deviation, sévérité ajustée de high)*

- **POM2 vs origine** : `rgb = max(rgb, prev * clamp(uPersistence, 0.0, 0.98))` — la valeur slider EST le facteur de rétention par frame, sans conversion en constante de temps (`p/(1/60+p)`) ni plancher `-0.5/256`. OE traite le slider comme une constante de temps phosphore et soustrait `-0.5/256` pour amener les traînées faibles à zéro.
- **Preuve** : `src/CrtEffectStack.cpp:257` vs OE `OpenGLCanvas.cpp:1023-1025` + `:140`. Le modèle par-frame est documenté (`NtscPostProcessor.h:37-39`, `DEV.md:274`) — simplification assumée, pas un bug.
- **Recommandation** : côté C++ calculer `level = persistence / (1.0f/60.0f + persistence)` et passer ça comme `uPersistence` ; dans le shader, `rgb = max(rgb, prev * uPersistence - 0.5/256.0)` ; garder le clamp 0.98 comme garde-fou. **Amélioration de fidélité optionnelle.**

#### Défauts scanline / shadow-mask bien plus lourds que la référence OE  *(MEDIUM — deviation)*

- **POM2 vs origine** : défauts POM2 `scanlines=0.25`, `shadowMaskStrength=0.5`, `persistence=0.4` (look fortement strié/masqué/smeary). Les profils Apple II color d'OE : `displayScanlineLevel=0.05`, `displayShadowMaskLevel=0.05`, `displayPersistence=0` (barrel 0.05 correspond). Divergence **non documentée** vis-à-vis de la source de vérité citée. Nuance : le masque POM2 utilise des primaires pures hard, pas un PNG photographique soft, donc 0.5 ici n'est pas directement comparable à 0.05 OE (autre modèle de masque — voir finding suivant).
- **Preuve** : `src/NtscPostProcessor.h:39,42,58` vs OE `AppleColor Composite Monitor IIe.xml`.
- **Recommandation** : pour correspondre à OE out-of-the-box, mettre `scanlines=0.05f`, `shadowMaskStrength=0.05f`, `persistence=0.0f` (garder barrel 0.05f). Si un défaut plus marqué est un choix produit délibéré, le documenter dans `DEV.md` comme ok-intentional — en l'état c'est une divergence non documentée.

#### Le shadow-mask multiplie des primaires pures (1,0,0) au lieu d'un triplet dark/light — sur-sature et sur-assombrit  *(MEDIUM — quality-gap)*

- **POM2 vs origine** : `maskColor = vec3(1,0,0)/(0,1,0)/(0,0,1)` puis `atten = mix(vec3(1.0), maskColor, strength)`, plus `atten *= mix(1.0, 0.6, strength)` sur la rangée triad. À strength=1, deux canaux sur trois tombent à 0 → virage dur vers la primaire + chute de luminosité. OE échantillonne un vrai PNG soft (`mix(vec3(1.0), mask, 0.05)`) ; crt-lottes utilise `maskDark=0.5 / maskLight=1.5` (canaux off atténués à 0.5, canal allumé boosté à 1.5 → luminance moyenne préservée).
- **Preuve** : `src/CrtEffectStack.cpp:235-244` vs OE `OpenGLCanvas.cpp:131-132` + Lottes `lottes.glsl:348-364`.
- **Recommandation** : adopter la convention dark/light Lottes : `vec3 mask = vec3(maskDark); if(phase==0) mask.r=maskLight; ...; rgb *= mix(vec3(1.0), mask, strength)` avec `maskDark~0.5`, `maskLight~1.5`. Stoppe les canaux off à zéro et l'assombrissement global. Garder le `maskAA` existant ; régler dark/light par type de masque.

#### Profil de scanline cosinus au lieu du sin² d'OE (et de la gaussienne Lottes)  *(LOW — quality-gap)*

- **POM2 vs origine** : `beam = 0.5 + 0.5*cos(PI*outRow)` (= cos²(PI·x/2), vallée raised-cosine période 2) appliqué via `rgb *= 1.0 - uScanlines*(1.0-beam)*scanAA`. OE : `sin²` période 1, appliqué via `mix(1.0, scanline², level)`. Deux écarts réels : profil cos² vs sin² (équivalents à un déphasage près), et surtout la convention de mélange (`1 - level*(1-beam)` vs `mix`). Le `scanAA` via `fwidth` est une amélioration POM2 (anti-moiré) qu'OE n'a pas.
- **Preuve** : `src/CrtEffectStack.cpp:216-217` vs OE `OpenGLCanvas.cpp:128-129`, Lottes `lottes.glsl:218-220`.
- **Recommandation** : pour parité OE : `s = sin(PI*outRow*0.5); rgb *= mix(1.0, s*s, uScanlines*scanAA)`. Optionnellement offrir un faisceau gaussien Lottes (`exp2(hardScan*pow(abs(dist),shape))`, hardScan~-8, shape~3) pour une qualité supérieure à OE. **Conserver le `scanAA`.**

#### Pas de terme center-lighting (vignette) — OE applique `exp(-dot(lighting,lighting))`  *(LOW — missing-feature)*

- **POM2 vs origine** : aucun terme vignette/center-lighting ; luminosité plate sauf le edge-mask barrel. OE assombrit vers les bords ; mais son défaut Apple II color est `centerLighting=1.0` → `exp(0)=1` → **off par défaut** (seuls les profils mono europlus à 0.5 le rendent visible).
- **Preuve** : `src/CrtEffectStack.cpp:128-261` (absent), `NtscPostProcessor.h:25-75` (pas de champ) vs OE `OpenGLCanvas.cpp:134-135`.
- **Recommandation** : ajouter `centerLighting` à NtscParams (défaut 1.0 = off), shader `vec2 lighting = cuv*(1.0/centerLighting - 1.0); rgb *= exp(-dot(lighting,lighting));` après le masque (ordre OE). **Basse priorité** (défaut OE off).

#### Pas d'étage luminance-gain — OE applique un gain post-glass (jusqu'à 1.5)  *(LOW — missing-feature)*

- **POM2 vs origine** : luminosité gérée seulement par `uBrightness` additif + `uContrast` ; aucun gain multiplicatif post-glass pour compenser la luminosité perdue aux scanlines/masque. OE : `c *= luminanceGain` après scanline/mask/lighting (défaut 1.0 Apple II color, 1.5 europlus).
- **Preuve** : `src/CrtEffectStack.cpp:200` vs OE `OpenGLCanvas.cpp:137`.
- **Recommandation** : ajouter `luminanceGain` à NtscParams (défaut 1.0, range 1..2), `rgb *= uLuminanceGain` juste après la multiplication shadow-mask (avant persistance/edge-mask). **À implémenter de pair avec l'allègement des défauts** (findings ci-dessus), car c'est précisément ce gain qui re-brille l'image assombrie par scanlines/masque.

#### Effets appliqués en espace gamma ; crt-lottes fait la math faisceau/masque en lumière linéaire  *(LOW — amélioration au-delà d'OE, sévérité ajustée)*

- **POM2 vs origine** : tous les multiplies opèrent sur les valeurs sRGB sans linéarisation. **Important** : le shader d'affichage d'OpenEmulator travaille AUSSI en espace gamma — ce n'est donc PAS une déviation vis-à-vis d'OE (POM2 et OE sont alignés). C'est uniquement une amélioration potentielle au-delà d'OE, vers crt-lottes (qui fait `ToLinear`/`ToSrgb`).
- **Preuve** : `src/CrtEffectStack.cpp:200-257` vs Lottes `lottes.glsl:203,423,160-186`.
- **Recommandation** : pour une qualité maximale, encadrer la math faisceau/masque/persistance par `pow(rgb,2.2)` … `pow(rgb,1/2.2)`. **Amélioration hors-parité** à étiqueter clairement comme telle (gate par flag), pas une correction de parité.

#### Sharpness redéfinie comme unsharp-mask spatial sur RGB — déviation intentionnelle documentée  *(ok-intentional, aucune action)*

- **POM2 vs origine** : la sharpness dans CrtEffectStack est un unsharp/soften 4-tap neutre à 0.5, car la sémantique OE (bande-passante chroma) n'a pas de sens sur un framebuffer RGB déjà décodé. Le chemin demod OE garde la vraie sémantique (`sigmaC = mix(2.5, 1.0, sharpness)`).
- **Preuve** : `src/CrtEffectStack.h:16-20`, `:168-179` ; `src/NtscPostProcessor.cpp:188`.
- **Recommandation** : **aucun changement requis** — déviation justifiée et documentée pour le stack universel à entrée RGB.

#### Barrel en distorsion carrée normalisée vs forme additive aspect-scaled d'OE — quasi-équivalent  *(LOW — ok-intentional)*

- **POM2 vs origine** : `cuv = vUv*2-1; r2 = dot(cuv,cuv); buv = cuv*(1.0 + uBarrel*r2)` — quadratique symétrique en coords normalisées sans pondération d'aspect, plus un edge-mask AA `fwidth` qu'OE n'a pas. OE scale par `barrelSize` (aspect-correct). Même famille r² ; seul écart mineur : courbure H/V différente sur fenêtre non carrée.
- **Preuve** : `src/CrtEffectStack.cpp:149-158` vs OE `OpenGLCanvas.cpp:122-124`.
- **Recommandation** : polish optionnel — pondérer `cuv.x *= aspect` avant `r2`. **Garder l'edge-mask soft `fwidth`** (amélioration réelle vs le clamp dur d'OE). Largement équivalent.

---

### 3.5 Le Chat Mauve / Video-7 RGB (88 %)

#### Le FIFO AN3 clocke 80COL directement (convention MAME) — opposé du !80COL d'AppleWin, donc le même logiciel sélectionne des modes en miroir vs AppleWin  *(MEDIUM — deviation, sévérité ajustée de high)*

- **POM2 vs origine** : `clockFifo` fait `fifo = ((fifo<<1)|dataBit)&0b11` avec `dataBit = eightyColLatched` (LEVEL 80COL brut). Enum BW560=0b00 … COL140=0b11 — numérotation verbatim de MAME `an3_w`. AppleWin clocke l'INVERSE (`!80COL`), donc ses numéros de mode sont le bit-inverse de POM2/MAME. **Désaccord de référence réel** : MAME et AppleWin divergent sur la polarité de la donnée AN3.
- **Preuve** : `LeChatMauveCard.cpp:53-57` + `le_chat_mauve_smoke_test.cpp:88` vs MAME `apple2video.cpp:252-253` (POM2 = MAME) et AppleWin `RGBMonitor.cpp:1087-1088` (inversé). Choix documenté `DEV.md:214-226`, header `LeChatMauveCard.h:18-28`.
- **Recommandation** : garder la polarité MAME (référence Video-7 activement maintenue, patent-derived) — **aucun changement comportemental.** Ajouter un commentaire dans `clockFifo` citant les deux : `// MAME apple2video.cpp:253 clocks 80COL directly; AppleWin RGBMonitor.cpp:1088 clocks !80COL — we follow MAME. Mode numbers are therefore the bit-inverse of AppleWin's g_rgbMode.`

#### Décodage HGR standard RGB par table de paires alignées au lieu de l'évaluation par-pixel 101/010 d'AppleWin  *(LOW — quality-gap, sévérité ajustée de medium)*

- **POM2 vs origine** : `renderHiResChatMauve80` et la branche `renderHiRes` ChatMauve décodent HGR en PAIRES alignées paires (`code = pixels[p]|(pixels[p+1]<<1); rgb = kChatMauveHGR[msb][code]`), peignant les deux pixels de la paire de la même couleur sans test de voisinage. Un dot isolé peint toujours une couleur saturée. AppleWin décide par pixel : couleur seulement si motif 101 (`0x140`) ou 010 (`0x80`) avec les voisins, sinon B&W pur.
- **Preuve** : `Apple2Display.cpp:1281-1293` / `1051-1062` vs AppleWin `RGBMonitor.cpp:560-631`. *Correction de l'audit* : le commentaire `kChatMauveHGR` (`Apple2Display.cpp:903-939`) n'affirme PAS la parité MAME — il attribue correctement les valeurs à AppleWin Feline ; la critique de doc initiale était infondée.
- **Recommandation** : pour fidélité HGR maximale, porter `UpdateHiResRGBCell` (dwordval 28-bit, fenêtre glissante 2-bit, émettre couleur seulement si `(dwordval&0x1C0)==0x140 || ==0x80`, sinon B&W). Élimine la sur-saturation des dots isolés. **Polish optionnel** (impact visuel étroit : seulement les dots isolés en HGR standard).

#### Le mode DHGR Mixed omet le BW-bit-repeat cross-byte et la continuation de cellule couleur partielle d'AppleWin  *(LOW — quality-gap, sévérité ajustée de medium)*

- **POM2 vs origine** : la branche Mixed/COL140 décode chaque fenêtre 28-bit à plat (par dot), sans état inter-cellule. AppleWin maintient `g_dhgrLastCellIsColor/g_dhgrLastBit` à travers les cellules (cellule couleur qui déborde dans un octet BW → se termine ; run BW qui croise un octet couleur → répète son dernier bit). **POM2 correspond exactement à MAME ici** (`apple2video.cpp:946-977`, port verbatim documenté) — AppleWin diverge.
- **Preuve** : `Apple2Display.cpp:1537-1576` vs AppleWin `RGBMonitor.cpp:739-741,804-816` et MAME `apple2video.cpp:867-877` (POM2 = MAME).
- **Recommandation** : POM2 est fidèle à sa source de vérité (MAME). Pour fidélité maximale au vrai board IIc spécifiquement en Mixed, on pourrait porter la state machine 7-cellules/2-octets d'AppleWin. Si on reste MAME-fidèle, ajouter un commentaire notant la divergence AppleWin en Mixed. COL140 pur et BW560 pur sont déjà bit-exacts. **Basse priorité.**

#### Palette Feline verbatim d'AppleWin (deux gris distincts)  *(ok-intentional, aucune action)*

- **POM2 vs origine** : `kChatMauveLoResPalette` = port byte-pour-byte de `PaletteRGB_Feline` (16/16 vérifiés), idx5=olive, idx10=mauve ; `kChatMauveHGR` = 6 couleurs Feline. MAME collapse les deux gris à `0x808080`.
- **Preuve** : `Apple2Display.cpp:779-796, 940-951` vs AppleWin `RGBMonitor.cpp:148-180` ; pinné par `le_chat_mauve_smoke_test.cpp:213-258`.
- **Recommandation** : **aucun changement** — la marque deux-gris est la raison d'être du mode.

#### COL140 (rotl4) et Chunky160 = ports bit-exacts de MAME  *(ok-intentional, aucune action)*

- **POM2 vs origine** : COL140 `rgbCardPalette[((nib<<1)|(nib>>3))&0xF]` == `rotl4(nib,1)` MAME ; color_mask `0x7F/0x3F80/0x1FC000/0xFE00000` == MAME ; Chunky160 layout (40 marges + 40 cols × 4×3 dots) == MAME. Seule la palette diffère (Feline vs apple2_palette).
- **Preuve** : `Apple2Display.cpp:1515-1530, 1560-1571` vs MAME `apple2video.cpp:817-835, 870-875`.
- **Recommandation** : **aucun changement** — ports verbatim.

#### Idx 5 ≡ 10 NON appliqué (gris distincts) — correspond à AppleWin, diverge intentionnellement de MAME/NTSC  *(ok-intentional, aucune action)*

- **POM2 vs origine** : sous carte RGB, gris distincts ; sous NTSC, `kLoResPalette` les garde tous deux à `0xFF808080`. Le collapse est un artefact de filtre chroma NTSC, absent du board RGB numérique.
- **Preuve** : `Apple2Display.cpp:745,750,785,790` vs AppleWin NTSC `0x808080` / Feline distincts.
- **Recommandation** : **aucun changement** — comportement correct des deux côtés.

#### Texte fg/bg coloré ne modélise pas le chemin Video7_SL7 (inverse-char-as-B&W / couleur fixe)  *(LOW — missing-feature)*

- **POM2 vs origine** : `renderTextChatMauveFgBg` source toujours fg=high-nibble / bg=low-nibble depuis l'AUX (équivalent `g_nTextFBMode=1`), gaté par Eve `$C0B8/$C0B9`. AppleWin SL7 rend les chars inverses en B&W (fg=15,bg=0) ou utilise des couleurs de texte fixes par switch. Mais SL7 est une **carte différente** (Video7_SL7) que POM2 ne prétend pas émuler.
- **Preuve** : `Apple2Display.cpp:692-731` vs AppleWin `RGBMonitor.cpp:938-955` ; `DEV.md:232-236`.
- **Recommandation** : POM2 modélise correctement le chemin Feline F/B. **Optionnel** : ajouter un flag de variante carte + répliquer `RGBMonitor.cpp:948-953` si SL7 est un jour souhaité. Non requis pour la parité Feline.

#### Eve HGR Duochrome mirroite correctement AppleWin (fg=hi-nibble, bg=lo-nibble depuis AUX)  *(ok-intentional, aucune action)*

- **POM2 vs origine** : `renderHgrDuochrome` — image depuis MAIN, attr depuis AUX, fg/bg par nibbles, 7 bits doublés en 2 dots ; armé par Eve `$C0BB`. Le `+= 12` d'AppleWin est équivalent à l'indexation directe du bloc lo-res 16-entrées POM2.
- **Preuve** : `Apple2Display.cpp:1307-1332` vs AppleWin `RGBMonitor.cpp:970-977,990-991` ; pinné `le_chat_mauve_smoke_test.cpp:331-406`.
- **Recommandation** : **aucun changement.**

---

### 3.6 Phosphores monochromes — White / Green P31 / Amber (78 %) — CHANTIER PRINCIPAL

> Note : les deux écarts de teinte ci-dessous sont **réels** mais **intentionnels et documentés** (`graphics_modes_comparison.md` §7/§8 admet explicitement « teinte empirique, variante AppleWin »). Le commentaire du *code* (`Apple2Display.cpp:880-892`) n'attribue PAS ces teintes à AppleWin — la fausse attribution n'existe que dans l'audit, pas dans le code. Sévérités ajustées de medium → low, recommandations de parité stricte **écartées** (à conserver uniquement comme variantes POM2 assumées).

#### Teinte amber : POM2 (0xFF,0xB0,0x00) vs AppleWin VT_MONO_AMBER (0xFF,0x80,0x00)  *(LOW — deviation intentionnelle)*

- **POM2 vs origine** : canal vert 0xB0 (176) au lieu de 0x80 (128) → ambre plus jaune-citron. AppleWin = ambre orangé classique. Écart numérique réel.
- **Preuve** : `Apple2Display.cpp:892` vs AppleWin `NTSC.cpp:1974-1978` ; `graphics_modes_comparison.md` §8 (« empirique, pas de référence formelle »).
- **Recommandation** : **conserver tel quel** comme variante esthétique POM2 assumée et documentée (recommandation de parité stricte écartée). Si une parité AppleWin stricte est un jour voulue, mettre `g=0x80`.

#### Teinte green : POM2 (0x33,0xFF,0x33) vs AppleWin VT_MONO_GREEN (0x00,0xC0,0x00)  *(LOW — deviation intentionnelle)*

- **POM2 vs origine** : POM2 ajoute un plancher R/B 0x33 (désature/éclaircit) et plafonne le vert à 0xFF ; AppleWin = vert pur plafonné à 0xC0. Écart réel ; le commentaire code cite la chromaticité P31 (CIE x=0.280, y=0.595), pas AppleWin.
- **Preuve** : `Apple2Display.cpp:891` vs AppleWin `NTSC.cpp:1979-1983` ; `graphics_modes_comparison.md` §7 (« empirique, calibré visuellement, variante AppleWin »).
- **Recommandation** : **conserver tel quel** comme variante POM2 documentée (recommandation de parité stricte écartée).

#### Modèle de persistance/decay phosphor absent d'AppleWin (enrichissement POM2)  *(LOW — ok-intentional)*

- **POM2 vs origine** : POM2 ajoute `merged = max(target, prev*decay)` (decay 0.96 amber ~25 frames, 0.85 green ~3 frames, 0.00 white). AppleWin n'a AUCUN modèle de persistance mono (mise à l'échelle statique seulement). Le `0.9647813115` d'AppleWin est LUMCOEF2, un coeff luma COMMENTÉ, sans rapport.
- **Preuve** : `Apple2Display.cpp:1146-1162` (HGR), `1486-1499` (DHGR) ; `graphics_modes_comparison.md:209`.
- **Recommandation** : **aucune correction de parité** — enrichissement POM2 documenté au-delà d'AppleWin. (La « note de transparence » de l'audit affirmant une fausse attribution AppleWin dans le commentaire est inexacte et écartée.)

#### Math d'application de teinte : POM2 ÷255 arrondi vs AppleWin ÷256 (>>8)  *(LOW — deviation)*

- **POM2 vs origine** : `(tint.r * merged + 127) / 255` (arrondi, mappe 0xFF→0xFF exactement) vs AppleWin `(bnw * tint) >> 8` (troncature : (255×255)>>8 = 254, léger assombrissement sub-LSB).
- **Preuve** : `Apple2Display.cpp:1164-1166, 1500-1502` vs AppleWin `NTSC.cpp:1005-1007`.
- **Recommandation** : écart sub-LSB ; POM2 est en fait plus exact. **Ne PAS copier le >>8** (artefact fixed-point de perf). Garder ÷255. Aucune action.

#### Uncertain → voir §4bis (luminance HGR mono)

---

## Note sur les points `uncertain` / `refuted`

- **Luminance HGR mono (signal_gen / mono_phosphor)** — *REFUTÉ, écarté du corps du rapport.* L'audit affirmait que le chemin mono d'AppleWin passe par un filtre luma IIR (LUMA_GAIN/0/1) produisant un dégradé lissé + ringing, et recommandait d'imiter ce filtre. Vérification : `VT_MONO_*` consomme `g_aBnWMonitor`, générée par `brightness = clampZeroOne((float)z)` où `z = (s & 0x800)` — le **bit brut retardé** (binaire 0/255), **non filtré**. Le résultat luma filtré `y1` va exclusivement à `g_aBnwColorTV` (chemin Color-TV, type vidéo différent). L'audit a confondu le chemin Monitor (binaire) avec Color-TV (filtré). Un écart réel subsiste (AppleWin Monitor = 560 dots binaires pleine résolution ; POM2 = moyenne de paires → 280 px à 3 niveaux), mais la prémisse de la recommandation est fausse : **ne pas suivre la reco** (AppleWin Monitor ne filtre pas la luma ; ironiquement le moyennage 2-tap POM2 adoucit davantage que le Monitor binaire d'AppleWin).
- Le finding `oe_composite` « gamma space » a été **reclassé** : ce n'est pas une déviation vs OE (OE est aussi en gamma), seulement une amélioration optionnelle vers Lottes — voir §3.4.

---

### 3.7 Génération du signal composite 14.318 MHz (96 %)

Tous les findings `confirmed`, sévérité **LOW** — couche de base critique commune à OE + AppleWin, essentiellement à parité. Un seul point matériel (decode-side) ressort.

#### ~~Le decode OE/AppleWin utilise une origine de phase absolue unique~~ **RÉSOLU (2026-05-30)** — `signalPhaseOffset` DHGR +1

- **Correctif** : `Apple2Display::signalPhaseOffset()` (= 1 quand IIe + 80COL + HIRES + DHGR) propagé au shader OE (`uPhaseOffset`), au demod CPU (`renderCompositeOeCpu`) et à AppleWin (`renderLine`/`renderFrame`). Épinglé par `dhgr_phase_signal`.
- **Preuve** : `NtscPostProcessor.cpp` (`floor(fx)+uPhaseOffset`), `Apple2Display.cpp` (`signalPhaseOffset_`), `AppleWinNtsc.cpp` (`(x+phase)&3`).

#### Sérialisation HGR (bit-doubling + half-dot delay) bit-pour-bit fidèle à MAME/AppleWin  *(LOW — ok-intentional)*

- **POM2 vs origine** : `makeBitDoubler` `t[i]=t[i>>1]*4+(i&1)*3` == MAME `bit_doubler` ; half-dot delay avec carry `last_output_bit` inter-colonnes == MAME `hgr_update:771-774`. **0 mismatch** sur 2000 rangées aléatoires + table complète.
- **Preuve** : `Apple2VideoDecode.h:35-102` vs MAME `apple2video.cpp:321-333,770-774` + AppleWin `NTSC.cpp:983-998`.
- **Recommandation** : **aucun changement** — parité vérifiée, garder le smoke test pinné.

#### Sérialisation lo-res = décalage 2-dot even/odd de MAME exact  *(LOW — ok-intentional)*

- **POM2 vs origine** : `(nibble>>(absX&3))&1` ; comme 14 mod 4 = 2, la phase auto-avance de 2 sur colonnes impaires == `>>2` de MAME. **0 mismatch** sur 8960.
- **Preuve** : `Apple2Display.cpp:1698-1721` vs MAME `lores_update:650-651`.
- **Recommandation** : aucun changement. Optionnel : commenter l'équivalence 14 mod 4 = 2 pour éviter qu'un futur lecteur ne « corrige » vers la forme littérale MAME.

#### Signal composite DHGR (interleave aux/main 7+7, pas de half-dot delay) == MAME  *(LOW — ok-intentional)*

- **POM2 vs origine** : aux bits 0-6 → dots base+0..6, main bits 0-6 → base+7..13, pas de half-dot delay == MAME `dhgr_update:842`. AppleWin chaîne en plus `g_nLastColumnPixelNTSC` entre cellules ; POM2 suit le modèle per-line de MAME (cohérent avec son decode DHGR MAME-LUT).
- **Preuve** : `Apple2Display.cpp:1673-1688` vs MAME `apple2video.cpp:842`.
- **Recommandation** : **aucun changement** pour parité MAME.

#### Flash-mask / glyph doubling / split aux-main texte == MAME/AppleWin  *(LOW — ok-intentional)*

- **POM2 vs origine** : `paintText40` double 7 dots → 14/cell ; `paintText80` aux=even (gauche), main=odd (droite) == MAME aux-low/main-high ; flash XOR 0x7F sur glyphe 7-bit avant doubling == AppleWin doubling-puis-XOR. Pas de half-dot delay sur texte (conforme aux deux).
- **Preuve** : `Apple2Display.cpp:1606-1650` vs MAME `text_update:687-694` + AppleWin `NTSC.cpp:1482-1485`.
- **Recommandation** : **aucun changement.**

#### `signalBuf` 1-bit (0x00/0xFF) — pas de mise en forme analogique avant le démodulateur  *(LOW — ok-intentional)*

- **POM2 vs origine** : tous les `paint*` écrivent 0x00/0xFF (square wave 14.318 MHz pur). Le band-limiting vit dans les décodeurs (IIR AppleWin / gaussiennes OE), pas dans la source — exactement le modèle hardware réel (flux digital de dots → étages analogiques filtrants).
- **Preuve** : `Apple2Display.cpp:1665` ; `AppleWinNtsc.cpp:159` ; `NtscPostProcessor.cpp:193-206`.
- **Recommandation** : **aucun changement** — correct par conception.

---

### 3.8 Lo-res / texte / 80-col / MouseText / flash (90 %)

#### Demi-période de flash = 15 frames au lieu de 16 (fréquence légèrement trop rapide)  *(LOW — deviation)*

- **POM2 vs origine** : `kFlashHalfPeriodFrames=15` → cycle 30 frames = 2.00 Hz. MAME IIe (`frame_number() & 0x10`) et AppleWin (`(++counter & 0xF)==0`) basculent tous les **16** frames → cycle 32 frames ≈ 1.875 Hz. Le commentaire POM2 prétend matcher `& 0x10` mais 15 ≠ 16.
- **Preuve** : `Apple2Display.h:198`, `Apple2Display.cpp:656` vs MAME `apple2video.cpp:1050` + AppleWin `NTSC.cpp:372-373`.
- **Recommandation** : changer `kFlashHalfPeriodFrames` de 15 à 16 dans `Apple2Display.h:198` (aligne exactement MAME-IIe + AppleWin). Mettre à jour le commentaire `:653-655` (demi-période 16, cycle 32 frames, ~1.87 Hz) qui sinon reste mensonger. Correctif trivial, ~6,7 % trop rapide aujourd'hui.

#### ~~Double Lo-Res (80-col lo-res) non implémenté~~ **RÉSOLU (2026-05-30)** — `renderLoResDouble` + goldens

- **Correctif** : `renderLoResDouble` sous `iie80 && !hiRes && dhgr`, smoke `dlgr_render_smoke`, goldens `iie/dlgr` / `iie/dlgrmixed`.

#### Flash II/II+ piloté par compteur de frames au lieu de l'horloge murale (555)  *(LOW — ok-intentional)*

- **POM2 vs origine** : un seul chemin flash pour tous les modèles, verrouillé sur la cadence UI (~60 Hz). MAME II/II+ pur utilise `(machine().time()*4).seconds() & 1` (timer 555 indépendant). AppleWin assume le même compromis (TODO reconnu).
- **Preuve** : `Apple2Display.cpp:656` ; `DEV.md:185-186`.
- **Recommandation** : acceptable et documenté. Tant que l'UI tourne à 60 Hz, le gain visuel d'un pilotage par cycles CPU est nul. **Laisser tel quel**, combiner avec le correctif 15→16.

#### Police 5×7 de secours grossière quand aucune ROM caractère n'est chargée  *(LOW — quality-gap)*

- **POM2 vs origine** : sans ROM, `kAscii5x7` (5 pixels centrés dans 7), minuscules repliées, $61-$7F vides, MouseText/ALTCHAR no-op. MAME/AppleWin exigent une vraie ROM caractère (pas de fallback synthétique) → rendu pixel-exact.
- **Preuve** : `Apple2Display.cpp:629, 495, 498-500` vs MAME `text_update:578-582`.
- **Recommandation** : déviation documentée raisonnable (fournir `roms/apple2_char.rom` pour la fidélité). Optionnel : étendre `kAscii5x7` aux minuscules/ponctuation, ou embarquer une table csbits //e libre pour qu'ALTCHAR/MouseText marche sans dump. Sinon conserver.

#### Repli minuscules→majuscules par charset<4096 dans le chemin csbits  *(LOW — ok-intentional)*

- **POM2 vs origine** : `lookupCsbitsGlyph` remappe a-z→A-Z (efface bit 5) pour ROM < 4096. MAME indexe directement sans remap, comptant sur le bon dump par modèle. Pour un dump 2 KB II/II+ sans minuscules, afficher la majuscule est le moindre mal.
- **Preuve** : `Apple2Display.cpp:545-550` (commentaire « mirroring the IIe firmware's own fallback ») vs MAME `text_update:578-580`.
- **Recommandation** : acceptable et documenté. **Aucune action** ; garder le commentaire justificatif.

---

### 3.9 Palettes 16 couleurs — NTSC/MAME + Feline (99 %)

Parité quasi parfaite ; tous les findings `confirmed` LOW. Seul défaut réel : des ancres de lignes périmées dans la doc.

#### Références de lignes périmées dans `docs/graphics_modes_comparison.md`  *(LOW — quality-gap)*

- **POM2 vs origine** : le doc renvoie vers `Apple2Display.cpp:611-628` (kLoResPalette) et `:651-668` (kChatMauveLoResPalette) ; les définitions réelles sont à `739-756` et `779-796` (décalage ~128 lignes après le refactor display-layering). D'autres ancres du même doc dérivent aussi (LUT `:57` → 759-789, kChatMauveHGR `:139` → 940-951).
- **Preuve** : `docs/graphics_modes_comparison.md:61,379-380` vs `Apple2Display.cpp:739,779`.
- **Recommandation** : remplacer `611-628`→`739-756` et `651-668`→`779-796` (lignes 61, 379-380), et corriger les autres ancres dérivées. Envisager des **ancres symboliques** plutôt que numériques pour éviter la dérive future.

#### kLoResPalette (NTSC/MAME) — 16/16 index identiques, IIgs-calibré confirmé  *(ok-intentional)*

- **POM2 vs origine** : `kLoResPalette` (ABGR) = port byte-pour-byte vérifié 16/16 de `apple2_palette[]` MAME (idx5=idx10=`0x808080`, etc.). Le label « IIGS-corrigé » du doc et le label « MAME » du code désignent la même table (« Apple II Video Display Theory »).
- **Preuve** : `Apple2Display.cpp:739-756` vs MAME `apple2video.cpp:886-904`.
- **Recommandation** : aucun correctif. Optionnel : un commentaire réconciliant les deux labels.

#### kChatMauveLoResPalette (Feline) — 16/16 identiques à AppleWin  *(ok-intentional)*

- **POM2 vs origine** : 16/16 vérifiés vs `PaletteRGB_Feline[]` (mapping `LoresResColors` = identité), idx5 olive / idx10 mauve.
- **Preuve** : `Apple2Display.cpp:779-796` vs AppleWin `RGBMonitor.cpp:164-179`.
- **Recommandation** : **aucun correctif.**

#### Index 5 ≠ 10 en Chat Mauve vs collapse 0x808080 de MAME — déviation intentionnelle vérifiée  *(ok-intentional)*

- **POM2 vs origine** : deux gris distincts sous carte RGB (la marque Chat Mauve), collapse sous NTSC. Source MAME confirmée : chemin Video-7 réutilise `apple2_palette[]` → collapse.
- **Preuve** : `DEV.md:225-226`, `graphics_modes_comparison.md:162`.
- **Recommandation** : **conserver tel quel.**

#### kChatMauveHGR (6 couleurs HGR) — identiques à AppleWin Feline  *(ok-intentional)*

- **POM2 vs origine** : magenta/green/blue/orange Feline vérifiés 4/4 ; cohérent avec la palette lo-res (même capture).
- **Preuve** : `Apple2Display.cpp:940-951` vs AppleWin `RGBMonitor.cpp:152-155`.
- **Recommandation** : **aucun correctif.**

---

## 4. TOP 10 actions prioritaires

Triées par rapport impact/effort pour la parité maximale.

| # | Action | Fichier cible | Changement précis | Sous-système | Effort | Impact |
|---|---|---|---|---|---|---|
| 1 | **Corriger le square filter ColorComp4Bit** | `src/Apple2Display.cpp:1115-1117` (HGR), `:1456-1459` (DHGR) | Nibble `(w >> (kContextBits-1))`, rotation `absX-1` (HGR) / `absX` (DHGR) ; corriger le commentaire `:1105-1114`. Pin smoke test MAME. | MAME NTSC LUT | Faible | **Élevé** (corrige ~50 % des pixels en mode 2) |
| 2 | **Cadence de flash 15 → 16** | `src/Apple2Display.h:198` | `kFlashHalfPeriodFrames = 16` + corriger commentaire `:653-655`. | Lo-res / texte | Trivial | Moyen (aligne exactement MAME-IIe + AppleWin) |
| 3 | **Réaligner les défauts CRT sur OE** | `src/NtscPostProcessor.h:39,42,58` | `scanlines=0.05f`, `shadowMaskStrength=0.05f`, `persistence=0.0f` (garder barrel 0.05f) — OU documenter le choix punchier dans `DEV.md`. | Effets CRT | Trivial | Moyen (look out-of-the-box conforme OE) |
| 4 | **Shadow-mask dark/light (Lottes)** | `src/CrtEffectStack.cpp:235-244` | `mask = vec3(0.5)`, canal allumé `=1.5`, `rgb *= mix(vec3(1.0), mask, strength)` ; garder `maskAA`. | Effets CRT | Faible | Moyen (préserve la luminance, stoppe sur-saturation/assombrissement) |
| 5 | **Ajouter luminance-gain post-glass** | `src/NtscPostProcessor.h` + `src/CrtEffectStack.cpp` (après masque) | Champ `luminanceGain` (défaut 1.0) + `rgb *= uLuminanceGain` avant persistance/edge-mask. | Effets CRT | Faible | Moyen (re-brille sous scanlines/masque, synergie avec #3/#4) |
| 6 | **Porter le FIR Chebyshev×Lanczos d'OE** | `src/NtscPostProcessor.cpp:187-206` + host-side | Précalculer 3 kernels 17-taps (`chebyshev(17,50)·lanczos`, fc luma 2.0 MHz / chroma 0.6 MHz, ×2), passer en `c0..c8`. | OE composite | Moyen | **Élevé** (corrige notch luma + chroma trop large, parité 72→~90 %) |
| 7 | **Normaliser le decay de persistance + plancher de bruit** | `src/CrtEffectStack.cpp:257` + C++ | `level = p/(1/60+p)` côté C++ ; shader `max(rgb, prev*uPersistence - 0.5/256.0)`. | Effets CRT | Faible | Faible-Moyen (constante de temps physique OE) |
| 8 | **Mettre à jour `graphics_modes_comparison.md`** | `docs/graphics_modes_comparison.md:61,105-117,139,379-380` | Corriger la formule square-filter (post-#1), les ancres périmées (→ 739-756/779-796/940-951), passer aux ancres symboliques. | Doc / palettes / MAME LUT | Faible | Faible (confiance/maintenabilité) |
| 9 | **Commenter la polarité AN3 (MAME vs AppleWin)** | `src/LeChatMauveCard.cpp:53-57` | Ajouter le commentaire `// MAME clocks 80COL directly; AppleWin clocks !80COL — we follow MAME, mode numbers bit-inverse.` | Le Chat Mauve | Trivial | Faible (évite confusion future en trace AppleWin) |
| ~~10~~ | ~~**Implémenter Double Lo-Res (DLGR)**~~ **FAIT** | `renderLoResDouble` + `dlgr_render_smoke` + goldens | — | Lo-res / texte | — | — |

---

## 5. Déjà fidèle — NE PAS TOUCHER

Sous-systèmes et chemins vérifiés à parité, à laisser intacts :

- **Palettes 16 couleurs (99 %)** : `kLoResPalette` (16/16 == MAME apple2_palette[]), `kChatMauveLoResPalette` et `kChatMauveHGR` (== AppleWin Feline). Aucun écart RGB/ordre/arrondi/byte-order.
- **Génération du signal composite 14.318 MHz (96 %)** : bit-doubling, half-dot delay HGR (0 mismatch), sérialisation lo-res 2-dot, interleave DHGR 7+7, glyph doubling / flash / split aux-main texte, `signalBuf` 1-bit. Couche de base critique — garder les smoke tests pinnés.
- **AppleWin NTSC (97 %)** : `initChromaPhaseTables`, les 4 filtres IIR, tous les coefficients, la topologie, le build loop, le special-casing white/black/grey. Aucun bug de math couleur. Le zéro-init des filtres par build est de fait *plus* correct qu'AppleWin.
- **OE composite — points de parité forts** : matrice décodeur YUV→RGB (1.139883 / -0.394642 / -0.580622 / 2.032062, exacte), alignement sous-porteuse π/2 (0.25 cycle/sample), gain chroma ×2.0, signal linéaire 0..1 sans gamma. **Ne pas toucher** — ce sont les fondations identitaires de la couleur OE.
- **Le Chat Mauve — ports verbatim MAME** : COL140 (`rotl4` + color_mask), Chunky160 (layout 40+40×4×3+40), DHGR Mixed (modèle per-line MAME), Eve HGR Duochrome, idx 5≠10 sous RGB / collapse sous NTSC. Palette Feline (deux gris distincts) — marque délibérée du board.
- **Phosphores mono — structure + math d'échelle** : structure luminance×teinte multiplicative, blanc neutre (0xFF,0xFF,0xFF exact), persistance/afterglow (enrichissement POM2 documenté), `÷255` arrondi (plus exact qu'AppleWin `>>8` — ne PAS copier le `>>8`).
- **Lo-res / texte — chemins fidèles** : palette lo-res, mapping nibble→bloc, interleave de ligne texte/lo-res, gating du flash ($40-$7F + ALTCHAR off), routage MouseText/ALTCHAR, interleave 80-col (aux=pair/main=impair), pré-flip ROM `loadCharRom`. Flash piloté par frame (parité MAME-IIe + AppleWin) et repli minuscules→majuscules : déviations documentées acceptables.
- **Sharpness spatiale, barrel normalisé + edge-mask `fwidth`, scanAA `fwidth`, espace gamma** (CRT) : déviations intentionnelles documentées ou améliorations POM2 vs OE. Le `scanAA`/`edge-mask` analytiques sont des plus-values à conserver.
- **OE phase 0 recalibrée MAME** : déviation intentionnelle (teinte alignée MAME) — ne pas réintroduire le -33° OE sauf objectif de repro OE strict. À documenter, pas à modifier.

---

## 6. Note sur les chantiers principaux (parité < 80 %)

Trois sous-systèmes sont sous le seuil de 80 % et constituent les chantiers prioritaires vers la quasi-parité :

1. **OpenEmulator composite (72 %)** — le cœur du gap est le **filtre passe-bas de démodulation** (deux gaussiennes au lieu du FIR Chebyshev×Lanczos d'OE) : la luma ne notche pas la sous-porteuse (~45 % de fuite → dot-crawl) et la chroma est ~1,8× trop large. Le **porter** (action TOP #6) remonterait la parité à ~90 %. À cadrer comme amélioration optionnelle, car l'implémentation est explicitement « OE-inspired » réécrite depuis la spec, pas un port libemulation. La phase -33° est une déviation MAME assumée, à documenter et non à corriger.

2. **Effets CRT (72 %)** — gaps **quantitatifs, non structurels**. Le triptyque le plus rentable : **réaligner les défauts** (#3), **shadow-mask dark/light** (#4) et **luminance-gain** (#5) — à implémenter ensemble, car le gain compense l'assombrissement allégé. Le decay de persistance normalisé (#7) et les manques vignette/linear-light sont secondaires (souvent off par défaut chez OE, ou améliorations au-delà d'OE).

3. **Phosphores monochromes (78 %)** — paradoxalement le moins « actionnable » : les deux écarts de teinte (amber, green) sont **réels mais intentionnels et documentés** (variantes esthétiques POM2 assumées), donc à **conserver** plutôt qu'à corriger. Le seul vrai gap résiduel est la résolution HGR mono (280 px à 3 niveaux vs 560 dots binaires AppleWin) — mais la recommandation de filtre luma associée a été **réfutée** (AppleWin Monitor ne filtre pas la luma). Ce sous-système est donc essentiellement « à parité de principe » ; son score reflète surtout des choix de teinte délibérés, pas des défauts à corriger.