# Agent — parité couleur OE GPU vs CPU

**Statut** : chantier actif (2026-05-30)  
**Référence** : `ColorCompositeOECpu` = démodulation excellente ; `ColorCompositeOE` (GPU) doit lui être pixel-identique à réglages neutres.

## Symptôme

En mode **Composite NTSC (OpenEmulator)** GPU, les couleurs NTSC (artefacts HGR/DHGR) divergent du mode **… CPU** alors que les deux consomment le même `signalBuf` 560×192.

## Architecture

```
fillCompositeSignal() → signalBuf
         ├─ ColorCompositeOECpu → renderCompositeOeCpu() → frame80 → screenTexture
         └─ ColorCompositeOE    → NtscPostProcessor (GLSL) → demodTex → CrtEffectStack
```

Fichiers clés :

| Fichier | Rôle |
|---------|------|
| `src/Apple2Display.cpp` | `renderCompositeOeCpu()` — référence CPU |
| `src/NtscPostProcessor.cpp` | Shader GLSL démod-only |
| `src/MainWindow.cpp` | Présentation + CRT (`drawScreenImage`) |
| `tests/oe_demod_gpu_cpu_parity_test.cpp` | Parité CPU ↔ formule GPU (sans GL) |

## Causes identifiées (par l'agent d'audit)

| # | Cause | Sévérité | Correctif |
|---|-------|----------|-----------|
| 1 | **Phase DHGR GPU** : `π/2·(floor(fx)+po)` ≠ CPU `π/2·((k+po)&3)` avec `k=(xi+po)&3` | **Critique (DHGR)** | ✅ Shader aligné sur CPU (`NtscPostProcessor.cpp`) |
| 2 | **CRT asymétrique** : OECpu passait par la branche « non-OE » avec hue/sharpness RGB | **Haute (CRT on)** | ✅ Branche `oeFamily` + CRT OECpu neutre |
| 3 | **Hue / Sharpness** : appliqués en démod GPU seulement | Moyenne | Comparer avec `ntsc_hue=0`, `ntsc_sharpness=0.5` |
| 4 | **Fallback LUT** si shader GL indisponible | Moyenne | Bannière UI (TODO) |
| 5 | Outils diag (`oe_signal_view`) encore gaussiens | Basse | Mettre à jour vers FIR YUV |

## Correctifs appliqués (2026-05-30)

1. **Phase shader GPU** — formule identique à `renderCompositeOeCpu()` :
   ```glsl
   int k = (xi + uPhaseOffset) & 3;
   int phaseIdx = (k + uPhaseOffset) & 3;
   float phase = PI * 0.5 * float(phaseIdx);
   ```

2. **MainWindow** — `oeFamily = oeGpu || oeCpu` ; CRT OECpu avec `hue=0`, `sharpness=0.5` (comme GPU).

3. **Test** — `oe_demod_gpu_cpu_parity` : max ΔRGB ≤ 1 sur HGR + DHGR.

## Checklist comparaison visuelle

1. Machine → **Composite NTSC (OpenEmulator)** vs **… CPU**
2. CRT Settings : `hue=0`, `sharpness=0.5`, PAL off
3. CRT Effects : OFF pour isoler la démod, puis ON pour valider la pile CRT unifiée
4. Scènes : bandes HGR `$55/$2A`, DHGR IIe 80COL+DHIRES, mixed HGR

## Prochaines étapes (backlog agent)

- [ ] Test GL readback (`NtscPostProcessor::process` + `glReadPixels`) en `EXCLUDE_FROM_ALL`
- [ ] Factoriser FIR dans `OeDemodConstants.h` (éviter dérive shader/CPU)
- [ ] Porter hue/sharpness sur `renderCompositeOeCpu()` ou documenter « GPU-only knobs »
- [ ] Fallback auto → OECpu si `!ntscFx->available()`
- [x] Mettre à jour `oe_signal_view.cpp` (référence gaussienne → FIR YUV + phaseOffset)

## Tests CI

```bash
cd build && ctest -R "oe_demod_gpu_cpu|dhgr_phase_signal|display_golden" --output-on-failure
```
