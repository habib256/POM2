# Re-validation parité vidéo / couleur / effets — POM2 (post-WIP)

**Date :** 2026-05-30 · **Étend :** [`video_parity_audit_2026-05-30.md`](video_parity_audit_2026-05-30.md)

Ce rapport est une **re-validation contre le code actuel (avec le WIP non commité)**, pas un nouvel
audit à blanc. Méthode : 1 agent par sous-système (POM2 post-WIP vs source d'origine MAME / AppleWin /
OpenEmulator pré-téléchargée dans `/tmp/pom2_refs/`), puis un vérificateur adverse par finding qui
re-lit les lignes exactes de la référence et recalcule la math (notch FIR, somme DC, trace d'un pixel),
plus **un agent dédié au bug couleur GPU vs CPU du mode OpenEmulator** demandé explicitement.

Sources d'origine confrontées : `mamedev/mame` `apple2video.cpp`, `AppleWin/AppleWin` `NTSC.cpp` +
`Video.cpp`, `openemulator/libemulation` `OpenGLCanvas.cpp` + `OEVector.cpp`.

---

## 0. Headline — bug couleur du mode OpenEmulator **GPU** vs **CPU**

> **✅ RÉSOLU (vérifié 2026-05-30, après le run).** Le code a été corrigé en parallèle : (1) `sharp` est
> désormais remappé `clamp((uSharpness-0.5)*2, 0, 1)`, donc au défaut `uSharpness=0.5` → `sharp=0` →
> `mix(chromaSoft, chromaSharp, 0) = chromaSoft` = le noyau CPU ; (2) la formule de phase GPU a été
> alignée sur le CPU (`((xi+po)&3 + po)&3·π/2`, `NtscPostProcessor.cpp:220-223`), corrigeant un écart
> **+90° en DHGR** ; (3) le commentaire périmé `sigmaC` est supprimé. Au défaut, GPU ≡ CPU (≤ 1 LSB) —
> luma, chroma, phase et matrice identiques. **Ne PAS hardcoder `wc = chromaSoft[a]`** : la moitié haute
> du curseur Sharpness (0.5→1.0) blende volontairement vers un kernel 2.0 MHz et doit rester. L'analyse
> ci-dessous documente la cause racine d'origine.

> Le chemin **CPU `renderCompositeOeCpu`** est l'oracle correct (« excellent »). Le chemin **GPU
> (shader)** déviait sur **une seule chose** : le noyau chroma (+ une phase DHGR décalée).

**Cause racine — mismatch du noyau chroma piloté par le `sharpness = 0.5` par défaut.**

Les deux chemins partagent **exactement** le même FIR luma, la même phase de sous-porteuse, les mêmes
axes de démodulation et la même matrice YUV→RGB. Ils divergent **uniquement** sur le noyau chroma FIR :

| Chemin | Noyau chroma | Référence |
|---|---|---|
| **CPU (oracle)** | `chromaK` = noyau **soft 0.6 MHz** OE-fidèle, **sans blend** | `src/Apple2Display.cpp:294-297`, utilisé `:319-320` |
| **GPU (bug)** | `mix(chromaSoft, chromaSharp, uSharpness)` avec **`uSharpness = 0.5`** par défaut | `src/NtscPostProcessor.cpp:200-203,212` ; défaut `src/NtscPostProcessor.h:35` |

Le gain DC est conservé (les deux noyaux somment à 2.000) → **pas** de désaturation ni de rotation de
teinte sur les aplats. En revanche le noyau GPU @0.5 laisse passer **beaucoup plus de chroma haute
fréquence** (recalculé par le vérificateur) :

| fréquence | \|H\| CPU | \|H\| GPU@0.5 | ratio |
|---|---|---|---|
| 0.72 MHz | 1.29 | 1.63 | 1.26× |
| 1.43 MHz | 0.30 | 0.96 | **3.2×** |
| 2.15 MHz | 0.01 | 0.42 | **45×** |

→ **bavures / franges de couleur aux bords de blocs et teintes altérées sur les traits colorés fins**
(1-2 dots de large), exactement le « problème de couleur » constaté. Les aplats sont identiques, les
bords et lignes fines non.

**Indice corroborant :** le commentaire `src/Apple2Display.cpp:271` (« Fixed sharpness 0.5 … sigmaC =
1.75 ») est **périmé** — vestige d'une ancienne implémentation gaussienne. Le code CPU réel (`:294-297`)
ne fait **aucun** blend → il équivaut à sharpness **0**, pas 0.5. Le défaut GPU de 0.5 n'a jamais été
resynchronisé après la réécriture en FIR.

**Tous les autres suspects écartés (vérifiés équivalents) :** alignement phase/texel (le `floor(fx)` du
shader annule le demi-texel — `NtscPostProcessor.cpp:169` ≡ `Apple2Display.cpp:316`), hue/BCS (hue=0 par
défaut, pas de double-spin, `MainWindow.cpp:2159-2160` neutralise bien pour le stack), matrice YUV→RGB
identique (`NtscPostProcessor.cpp:226-230` ≡ `Apple2Display.cpp:323-325`), fenêtre 17-tap centrée pareil,
pas de sRGB. **Le noyau chroma est la seule divergence.**

### Correctif recommandé — Option B (découplée, la plus sûre)

Forcer le démod GPU sur le noyau soft, indépendamment du curseur Sharpness (qui garde son sens spatial
côté `CrtEffectStack`). Dans `src/NtscPostProcessor.cpp:212` :

```glsl
// avant
float wc = mix(chromaSoft[a], chromaSharp[a], sharp);
// après
float wc = chromaSoft[a];   // chroma 0.6 MHz OE-fidèle — identique à renderCompositeOeCpu (pas de blend Sharpness)
```

Après ça le démod GPU est **pixel-équivalent** au CPU (≤ 1 LSB d'arrondi 8-bit). On peut ensuite
supprimer `chromaSharp` + la plomberie `sharp`/`uSharpness` du démod, ou la garder pour un futur réglage
explicite de bande chroma. **Et** corriger le commentaire périmé `src/Apple2Display.cpp:271`.

*Option A (minimale)* : passer le défaut `sharpness = 0.0f` (`src/NtscPostProcessor.h:35`) — mais ce même
param alimente l'unsharp spatial du `CrtEffectStack` (qui traite 0.5 comme neutre), donc Option A déplace
aussi ce réglage. **Option B est préférée** car elle élimine le couplage « un seul curseur, deux sens ».

### Comment confirmer
`tests/ntsc_oe_ab_tool.cpp` et `oe_signal_view.cpp` ne comparent **pas** CPU↔GPU. Vérif rapide : afficher
un écran HGR couleur à traits fins et basculer « Composite (OpenEmulator, GPU) » ↔ « (CPU) » (câblés pour
l'A/B à `MainWindow.cpp:1909-1916`) ; avant le fix ils diffèrent aux bords, après ils sont identiques.
Pour rigoureux : étendre `ntsc_oe_ab_tool.cpp` pour diffuser le même `signalBuf` dans
`renderCompositeOeCpu()` (→ `frame80`) et dans le GPU (`glReadPixels`), delta RGBA max ≤ 1 LSB après fix.

---

## 1. Parité par sous-système (post-WIP)

| Sous-système | Parité post-WIP | Verdict |
|---|---|---|
| OpenEmulator composite (démod) | **90 %** | WIP a fermé les 2 gros gaps ; reste le bug chroma GPU≠CPU (§0) |
| AppleWin NTSC (LUT IIR) | **97 %** | Bit-exact, aucune action (le seul écart est sub-LSB) |
| Effets CRT (glass) | **86 %** | WIP a fermé 4/6 gaps ; reste défauts par défaut + vignette ×4 + scanline |
| Le Chat Mauve / Video-7 | **93 %** | « mirroring AN3 » prouvé non-bug ; conforme |
| Phosphores mono | **80 %** | 1 vrai résidu (HGR mono 280/3-niveaux) ; teintes = goût assumé |
| Signal + lo-res / texte / DLGR | **95 %** | flash 16 frames + DLGR fermés ; DLGR pas câblé au chemin signal |

> `mame-lut` (ColorNTSC/CompMedium/Comp4Bit) : l'agent n'a pas rendu de sortie structurée, mais son gap
> phare (square-filter `composite_color_mode=2`) est **déjà fermé par le WIP** —
> `Apple2Display.cpp:1167-1169` (HGR) / `:1510-1513` (DHGR), confirmé par l'agent chatmauve.

---

## 2. Ce que le WIP (non commité) a déjà corrigé

- **OE démod** — remplace les 2 gaussiennes par 3 FIR 17-tap. Le vérificateur a **reconstruit
  indépendamment** la recette OE (`chebyshevWindow(17,50) × lanczosWindow`, realIDFT + symétrisation +
  normalisation, Fs=14318180, luma 2.0 MHz / chroma 0.6 MHz) : les coeffs `lumaK`/`chromaSoft`/
  `chromaSharp` **correspondent à 5 décimales**. `lumaK` somme à 1.0 et **notche réellement fs/4**
  (`|H(0.25)|=0.00199`, −3 dB ≈ 1.64 MHz). Ferme les gaps audit #108 (filtre gaussien) + #114 (dot-crawl).
- **CRT glass** — ajoute center-lighting/vignette (slot OE exact), `luminanceGain` post-glass (slot OE
  exact), masque Lottes dark/light (0.5/1.5, préserve la luminance moyenne), plancher de bruit
  persistance `−0.5/256` (OE exact). Ordre des effets = OE exact.
- **Le Chat Mauve** — documente la polarité AN3 (suit MAME), partage le fix square-filter.
- **Signal/texte** — flash demi-période **16 frames** (`Apple2Display.h:199`, ≡ MAME `frame_number()&0x10`),
  nouveau **renderLoResDouble** (DLGR) vérifié bit-pour-bit vs MAME `lores_update<Double>` + test
  `tests/dlgr_render_smoke_test.cpp` qui passe.
- **AppleWin / mono** — non touchés (et déjà à parité). Le DLGR couleur ne touche aucun de leurs chemins.

---

## 3. Plan priorisé — items encore ouverts

Trié par impact visuel (desc) puis effort (asc). Confiance = verdict adverse.

| # | Sous-système | Item | Sév. | Impact | Effort | Conf. |
|---|---|---|---|---|---|---|
| F0 | OE GPU | **Chroma GPU≠CPU (§0)** | — | **Élevé** (visible) | petit | — |
| F4 | CRT glass | Défauts CRT 0.25/0.5/0.4 vs OE 0.05/0.05/0 | medium | **Élevé** | petit | 0.90 |
| F3 | CRT glass | Vignette center-lighting **~4× trop forte** | medium | Moyen | petit | 0.95 |
| F7 | Mono | HGR mono 280 px / 3 niveaux vs 560 binaire AppleWin | low | Moyen | petit | 0.90 |
| F2 | CRT glass | Scanline cosinus vs **sin²** OE | low | Faible | petit | 0.90 |
| F6 | CRT glass | Masque : row-dim ×0.7 sur-assombrit (⚠ fix à nuancer) | low | Faible | petit | 0.86 |
| — | Signal | DLGR non câblé au chemin signal (OE/AppleWin → lo-res 40-col) | low | Moyen | moyen | — |
| F1 | AppleWin | Clamp double vs float — **NE PAS CHANGER** (sub-LSB, POM2 plus précis) | low | nul | — | 0.97 |
| F8/F9 | Mono | Teintes amber/green ≠ AppleWin — **intentionnel** (preset optionnel) | low | Faible | petit | 0.95 |

---

## 4. Fiches d'implémentation

### F0 — Chroma GPU = chroma CPU
Voir **§0** ci-dessus (fiche complète : cause, math, correctif Option B, validation).

### F4 — Défauts CRT alignés sur OE *(le plus gros gain visuel)*
`src/NtscPostProcessor.h:39,42,58`. POM2 ship 0.25 / 0.5 / 0.4 (scanlines / mask / persistence), OE
Apple II = ~0.05 / 0.05 / 0 (`OpenGLCanvas.cpp:1248,1017-1018,1024`). Les changements **porteurs** (1:1 OE) :
```cpp
scanlines        = 0.05f;   // ≡ OE displayScanlineLevel
persistence      = 0.0f;    // ≡ OE displayPersistence (Apple II)
// shadowMaskStrength ≈ 0.10f  (le masque dark/light est plus doux par unité que l'ancien primaire pur)
// barrel = 0.05f et luminanceGain = 1.0f sont déjà OK
```
**OU** documenter explicitement dans `DEV.md` que les défauts plus « punchy » sont un choix produit
assumé (actuellement c'est une divergence non documentée vs OE).

### F3 — Vignette center-lighting (×4 trop forte)
`src/CrtEffectStack.cpp:259-260`. POM2 : `cuv = vUv*2-1` ∈ [-1,1] ; OE : `qc = (texcoord-0.5)*barrelSize`
∈ [-0.5,0.5] (`OpenGLCanvas.cpp:122,134-135,1243-1244`). `cuv` vaut **2× qc** → l'exposant `dot()` est ×4.
```glsl
// avant : vec2 lighting = cuv * (1.0/uCenterLighting - 1.0);
// après : qc.x exact + y pondéré par l'aspect
vec2 qc = vec2(vUv.x - 0.5, (vUv.y - 0.5) / uAspect);   // ≡ OE qc (barrelSize = (1, 1/aspect))
vec2 lighting = qc * (1.0/uCenterLighting - 1.0);
// rgb *= exp(-dot(lighting, lighting));  (inchangé)
```
Correctif vérifié exact : `cuv.x*0.5 = vUv.x-0.5 = qc.x` d'OE. (Si l'uniform `uAspect` n'existe pas, un
simple `*0.5` corrige déjà l'erreur dominante sur x.)

### F7 — HGR mono natif 560 binaire *(seul vrai résidu mono, §4bis de l'audit)*
`src/Apple2Display.cpp:1208-1219`. POM2 moyenne par paires le flux 560 dots → `frame` 280 px à **3 niveaux**
(0/127/255). AppleWin Monitor (`NTSC.cpp:938,957-961`) sort le bit luma **binaire** brut en **560 dots**.
**Fix :** quand `effMode` ∈ {MonoWhite/Green/Amber}, rendre en 560 binaire dans `frame80` au lieu de la
moyenne 280 — c'est **une copie de la boucle DHGR-mono déjà livrée et testée** (`:1548-1557`), `stream[]`
est déjà le bitstream 560 binaire. Aligne single-HGR mono sur AppleWin **et** sur le propre chemin DHGR-mono.

### F2 — Scanline sin² (OE)
`src/CrtEffectStack.cpp:218-219`. POM2 : `beam = 0.5 + 0.5*cos(PI*outRow)`. OE
(`OpenGLCanvas.cpp:128-129`) : `scanline = sin(PI*texSize.y*q.y); c *= mix(1.0, scanline², level)`.
Comme `outRow = uv.y*srcSize.y*2`, l'argument `PI*outRow*0.5` **égale** `PI*texSize.y*q.y`. Passer en
`mix(1.0, s*s, uScanlines*scanAA)` (garder le terme `scanAA` fwidth, anti-moiré POM2 qu'OE n'a pas). Bonus :
recentre la bande sombre sur l'inter-ligne.

### F6 — Row-dim de masque ⚠ *(fix à nuancer)*
`src/CrtEffectStack.cpp:247-251`. Le `×0.7` horizontal empile un assombrissement qu'OE (`:131-132`, un seul
PNG triad `mix(1.0, mask, level)`) n'a pas. **Attention** (verdict `fixCorrect=false`) : supprimer le bloc
sèchement rend le mode *triad* visuellement identique au mode *aperture-grille*. Si on veut garder un gap
vertical, le rendre **neutre en luminance** (éclaircir les rangées sans gap) plutôt que de juste retirer.

### Open — DLGR sur le chemin signal
Le WIP ajoute le rendu DLGR couleur (`renderInternal`) mais **n'étend pas** `fillCompositeSignal` au DLGR :
sous OE/AppleWin, le DLGR émet encore du lo-res 40-col main-only. Le nouveau test ne couvre pas ce chemin.
À traiter si DLGR doit être correct sous les démodulateurs composite.

### F1 / F8 / F9 — Ne rien changer (documenté)
- **F1** (AppleWin clamp double vs float) : balayage 787 059 échantillons → 3 diffèrent, tous |Δ|=1 LSB.
  POM2 (double) est **plus** précis. Ne pas réintroduire le float d'AppleWin.
- **F8/F9** (amber `FF,B0,00` vs `FF,80,00` ; green `33,FF,33` vs `00,C0,00`) : déviations de goût
  assumées (green calibré P31). Garder ; éventuellement ajouter un preset « AppleWin-faithful » qui bascule
  vers les valeurs VT_MONO_*.

---

## 5. Méthode & limites

- Référence d'origine en cache local : `/tmp/pom2_refs/{mame,applewin,oe}/` (8 fichiers).
- L'agent `mame-lut` a échoué à produire une sortie structurée (gap phare déjà fermé par WIP — couvert).
- L'agent de synthèse du workflow a buté sur la **limite de session du compte** (`reset 17:50 Indian/
  Reunion`) — ce rapport a donc été assemblé à la main depuis les 9 findings vérifiés + l'agent GPU/CPU.
- Aucun fichier source modifié par les agents (lecture seule) ; le WIP en cours est intact.
