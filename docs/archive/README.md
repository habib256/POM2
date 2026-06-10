# Archive — documents de travail historiques

Snapshots ponctuels d'efforts terminés, conservés pour la **provenance** (le
« pourquoi » d'un correctif, la méthode d'audit). Ils ne sont **pas** maintenus :
leurs références de lignes peuvent être périmées et leurs conclusions sont déjà
repliées dans les docs vivants. Ne pas s'y fier comme état courant.

| Fichier | Quoi | État → où vit l'info à jour |
|---|---|---|
| `video_parity_audit_2026-05-30.md` | Audit parité vidéo/couleur/effets (9 findings, TOP-10 actions) | Gaps phares **résolus** (square-filter, OE GPU/CPU). Doc vivant : [`../graphics_modes_comparison.md`](../graphics_modes_comparison.md). Résidus → `TODO.md` § [Display] *Raffinements parité CRT*. |
| `video_parity_revalidation_2026-05-30.md` | Re-validation post-WIP de l'audit ci-dessus + fiches d'implémentation F0–F9 | Idem. Les fiches F2/F3/F4/F6/F7 encore ouvertes sont listées dans `TODO.md` § [Display]. |
| `oe_gpu_cpu_parity.md` | Notes d'agent sur le bug couleur OE GPU≠CPU | **Résolu + pinné** `oe_demod_gpu_cpu_parity`. Détail vivant : `DEV.md` § Composite NTSC shader + `CHANGELOG.md` 2026-05-30. |

Historique canonique complet → `git log` + `CHANGELOG.md`.
