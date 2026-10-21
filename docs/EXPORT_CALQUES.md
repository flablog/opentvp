# Export calque par calque — format du JSON

Ce format est commun à `tvpp.exe` (ou `tvpp-macos`, `tvpp-linux`) et à la version Python `tvpp_render.py`.

## Deux modes

```
# 1. Dessins seuls : une image par dessin + un JSON qui dit quand chacun est exposé
tvpp render plan.tvpp sortie --layers --keyframes

# 2. Toutes les frames : une image par frame et par calque, tenues comprises
tvpp render plan.tvpp sortie --layers
```

Choisir les calques : `--layers all` (défaut, calques visibles), `--layers "IMANI_LINE,IMANI_COLO_CTG"`, ou leurs numéros dans `tvpp info` (1 = calque du haut) : `--layers 2,3`. Pour inclure les calques masqués : `--include-hidden`.

Chaque calque a son propre dossier, nommé comme le calque. Si deux calques ont le même nom, le dossier est préfixé par leur numéro.

```
sortie/
  Clip_001_exposition.json         (ou _calques.json en mode toutes les frames)
  IMANI_LINE/IMANI_LINE_0000.png
  IMANI_LINE/IMANI_LINE_0002.png
  ...
```

## Ce que contient chaque image

- La feuille entière du projet (ex. 5990×3557), sans caméra, avec une couche alpha droite (non prémultipliée), au même arrondi que l'export de TVPaint.
- Les pixels bruts du calque : l'opacité et le mode de fusion ne sont pas appliqués, ils sont donnés dans le JSON.
- Les calques CTG donnent leur remplissage calculé, comme dans TVPaint.
- `--scale 0.5` réduit les images. `--camera` (mode toutes les frames uniquement) produit à la place chaque calque cadré par la caméra, à la taille de sortie, avec le flou de mouvement.

## Ce qu'est un dessin (mode `--keyframes`)

Un dessin, c'est une image distincte du calque. Une tenue ne crée pas de dessin, et un dessin réutilisé plus loin dans la timeline (même image ré-exposée) n'est exporté qu'une fois. C'est pourquoi le nombre de fichiers peut être inférieur au nombre de « dessins » que compte `tvpp info` (qui compte les instances). Les instances vides ne produisent pas de fichier.

Chaque fichier porte le numéro de la première frame où le dessin apparaît dans la plage exportée. Ainsi `IMANI_LINE_0012.png` est identique en mode dessins et en mode toutes les frames.

## Numérotation et plage

Les frames sont numérotées à partir de 0, comme dans les paramètres du projet (la timeline de TVPaint affiche +1). La plage par défaut va des marques in/out, ou à défaut couvre tout le clip. On peut la fixer avec `--frames 0-108`. Les comportements avant/après (répéter, ping-pong, tenir) sont appliqués dans cette plage.

## Le JSON

```json
{
 "source": "sq001_sh150_colo.tvpp", "clip": "Clip_001",
 "mode": "keyframes",            // ou "frames"
 "space": "canvas",              // ou "camera"
 "fps": 25.0,
 "canvas": {"width": 2304, "height": 1296},
 "image_size": {"width": 2304, "height": 1296},
 "frame_range": [0, 54],
 "alpha": "straight",
 "camera": {
   "output": {"width": 1920, "height": 1080, "fps": 25.0},
   "motion_blur": 0.0,
   "track": [{"frame": 0, "x": 1152.0, "y": 648.0, "rotation": 0.0, "zoom": 1.0}, ...]
 },
 "layer_order": "du haut vers le bas, comme dans TVPaint",
 "layers": [
  {
   "index": 3, "name": "IMANI_SCULPT", "folder": "IMANI_SCULPT",
   "type": "anim", "visible": true, "opacity": 1.0, "blending": "Color",
   "pre_behavior": "none", "post_behavior": "none", "start": 0, "end": 54,
   "drawings": [{"file": "IMANI_SCULPT/IMANI_SCULPT_0000.png", "first_frame": 0, "layer_frame": 0}, ...],
   "exposures": [{"start": 0, "duration": 2, "file": "IMANI_SCULPT/IMANI_SCULPT_0000.png"},
                 {"start": 2, "duration": 3, "file": "IMANI_SCULPT/IMANI_SCULPT_0002.png"}, ...],
   "key_frames": [0, 2, 5, 8, ...],
   "timeline": ["IMANI_SCULPT/IMANI_SCULPT_0000.png", "IMANI_SCULPT/IMANI_SCULPT_0000.png", ...]
  }
 ]
}
```

- `timeline` : pour chaque frame de la plage, le fichier à afficher, ou `null` quand le calque est vide. C'est le champ le plus simple pour un import (After Effects, Blender, Libreflow).
- `exposures` : la même information, regroupée en blocs (début, durée, fichier). Présent en mode `--keyframes`.
- `key_frames` : les frames où un nouveau dessin apparaît.
- `camera.track` : la caméra frame par frame (centre en pixels de la feuille, rotation en degrés, zoom TVPaint), pour refaire le cadrage dans le logiciel de compositing. La relation est : `sortie − centre_sortie = R(rotation) · (feuille − centre) / zoom`.

## Recomposer le plan

Pour recomposer le plan, on part du dernier calque de la liste (celui du bas). On empile les images en appliquant `opacity` et `blending`, puis on cadre l'ensemble avec `camera.track`. Cette recomposition est vérifiée. Elle redonne le rendu TVPaint de sh150 sur les 55 frames, et elle donne exactement le même résultat que le rendu final sur sh140.

## Vérification

Le mode `--keyframes` a été comparé à l'export de structure de TVPaint (`tv_ClipSaveStructure`) sur les 72 fichiers du corpus, soit 591 frames : toutes identiques au pixel près. Cela couvre les tenues, les instances vides, les comportements avant/après, les modes de fusion et les tuiles.
