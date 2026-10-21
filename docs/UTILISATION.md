# tvpp — lire et rendre des fichiers TVPaint 11 sans TVPaint

Un seul exécutable, sans installation ni dépendance (≈ 220 Ko).
`tvpp.exe` pour Windows 10/11 64 bits, `tvpp-macos` pour Mac (binaire universel : Apple Silicon M1–M4 et Intel, macOS 11 et plus), `tvpp-linux` (statique) pour un serveur Linux.

### Premier lancement sur Mac

Le binaire n'est pas signé par un compte développeur Apple, macOS le bloque donc au premier lancement. Dans le Terminal, une seule fois :

```
cd chemin/vers/tvpp
xattr -d com.apple.quarantine tvpp-macos
chmod +x tvpp-macos
./tvpp-macos info fichier.tvpp
```

## Commandes

```
tvpp info   fichier.tvpp            structure lisible (résolution, fps, clips, calques, caméra)
tvpp info   fichier.tvpp --json     la même chose en JSON (+ trajectoire caméra frame par frame)
tvpp render fichier.tvpp dossier    rendu final composé, comme « Enregistrer la séquence » via la caméra
tvpp render fichier.tvpp dossier --layers --keyframes    un dossier par calque, une image par dessin + JSON d'exposition
tvpp render fichier.tvpp dossier --layers                un dossier par calque, toutes les frames
```

Options de `render` :

| option | effet |
|---|---|
| `--clip N` | clip à rendre (défaut 1) |
| `--frames A-B` | plage de frames, numérotées depuis 0 comme dans les paramètres du projet (la timeline affiche +1). Défaut : marques in/out, sinon tout le contenu visible |
| `--bg projet` / `--bg transparent` / `--bg #RRGGBB` | fond (défaut : couleur de fond du projet, noir hors de la feuille) |
| `--scale 0.5` | rendu réduit (preview rapide) |
| `--blur-samples N` | échantillons du flou de mouvement caméra (défaut 8) |
| `--threads N` | nombre de cœurs utilisés (défaut : tous) |
| `--prefix NOM` | préfixe des PNG (défaut : nom du clip) → `NOM_0000.png`, … |
| `--png-level N` | compression PNG 0-12 (défaut 6) |

## Export calque par calque

| option | effet |
|---|---|
| `--layers` | tous les calques visibles, chacun dans son dossier |
| `--layers "IMANI_LINE,IMANI_COLO_CTG,3"` | seulement ces calques (nom, ou numéro donné par `tvpp info`) |
| `--keyframes` | une image par dessin (les tenues et les dessins réutilisés ne sont pas dupliqués) + JSON d'exposition |
| `--camera` | (sans `--keyframes`) chaque calque cadré par la caméra, à la taille de sortie, flou de mouvement compris |
| `--include-hidden` | inclure les calques masqués |
| `--skip-empty` | pas d'image transparente quand le calque est vide |
| `--json NOM` | nom du JSON (défaut : `<clip>_exposition.json` ou `<clip>_calques.json`) |

Les images sont des PNG RGBA en alpha droit, à la taille de la feuille, avec les pixels bruts du calque. L'opacité, le mode de fusion, les comportements avant/après et la trajectoire caméra sont donnés dans le JSON.

Les fichiers portent le numéro de la première frame où le dessin apparaît. `IMANI_LINE_0012.png` est donc le même fichier en mode dessins et en mode toutes les frames.

Dans le JSON, `timeline` donne pour chaque frame le fichier à afficher (`null` si le calque est vide). `exposures` donne la même information en blocs (début, durée, fichier). Le format complet est décrit dans `EXPORT_CALQUES.md`.

`tvpp --version` affiche la version.

Exemples :

```
tvpp.exe render "C:/projets/nebuleuse/sq001_sh150_colo.tvpp" "C:/rendus/sh150" --prefix sh150
tvpp.exe render "C:/projets/nebuleuse/sq001_sh140_colo.tvpp" "C:/export/sh140" --layers --keyframes
```

## Ce qui est géré

- Fichiers TVPaint 11 (testé sur 11.7.6), y compris les fichiers compressés de production.
- Calques image, anim, CTG (le remplissage calculé est relu, pas recalculé).
- Exposition, tenues, frames réutilisées, comportements avant/après (répéter, ping-pong, tenir).
- Visibilité, opacité, modes de fusion Normal, Derrière, Effacer, Multiplier, Écran, Ajouter, Obscurcir, Éclaircir, Différence, Incrustation.
- Caméra : points, zoom, rotation, profil de vitesse (Linéaire, Courbe, Polynômial), flou de mouvement.
- Marques in/out, résolution et cadence de sortie de la caméra.

## Précision mesurée (comparaison avec les rendus TVPaint)

- sq001_sh150 (CTG, 55 frames) : identique à 1/255 près sur toutes les frames.
- sq001_sh140 (5990×3557, caméra qui dézoome ×2,7) : plans fixes 0,3 % de pixels différents (bords de traits, filtrage), plans en mouvement jusqu'à 1 % (léger décalage de trajectoire caméra, < 0,7 % du mouvement).
- Corpus caméra : translations, profils, flou : écarts ≤ 0,5 % de pixels.
- Export par calque : identique au pixel près à l'export de structure de TVPaint sur les 72 fichiers du corpus (591 frames), et à la version Python dans tous les modes (dessins, toutes les frames, caméra, échelle).

## Vitesse (machine de test à 2 cœurs, sera plus rapide sur une station)

- Rendu final sh150 (2304×1296 → 1920×1080) : ≈ 0,35 s/frame.
- Rendu final sh140 (5990×3557 → 1920×1080, 17 calques visibles) : ≈ 1,4 s/frame, ≈ 2 Go de mémoire.
- Export par calque, dessins seuls : sh150 4 s (152 dessins) ; sh140 1 min 45 (434 dessins, 38 Mo, < 1 Go de mémoire).
- Les calques sont traités en parallèle, un par cœur. `--threads N` limite ce nombre si la mémoire manque (≈ 4 × la taille d'une image de la feuille par calque en cours).

## Limites connues

- Modes de fusion non gérés (Ombre, Lumière, Teinte, Coloriser, Lumière douce…) : rendus en Normal, avec un avertissement.
- Si la cadence de la caméra diffère de celle du projet, TVPaint rééchantillonne ; tvpp rend à la cadence du projet (avertissement dans `info`).
- Fond en damier (deux couleurs) : rendu sur la première couleur.
- Pas encore testé : fichiers TVPaint 12, calques son / référence / dossier.
- Les fichiers sont lus en entier en mémoire : prévoir la taille du .tvpp + ~100 Mo par grand calque actif.

## Compiler soi-même

Voir le README du dépôt : `c/build.sh` (binaire natif) ou `c/build.sh all` (toutes les plateformes, avec `pip install ziglang`).
