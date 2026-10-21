# Format .tvpp (TVPaint Animation 11)

Établi par rétro-ingénierie sur TVPaint 11.7.6, à partir du corpus `corpus/` (fichiers générés par script George + vérité terrain exportée par TVPaint) et de plans de production. Tout ce qui suit est vérifié par les tests, sauf mention contraire.

Conventions : entiers big-endian, frames numérotées à partir de 0 (la timeline de TVPaint affiche +1).

## 1. Arborescence de sections

Le fichier est un arbre de sections :

```
GUID (16 octets) | taille u64 BE | contenu (taille octets)
```

Le type d'une section se lit sur les 4 premiers octets du GUID :

| id | rôle |
|---|---|
| `3384780e` | racine |
| `3385553a` | en-tête projet ; liste clé/valeur à l'offset 0x30 du fichier |
| `338631b2` | scène (« shot ») |
| `3388da98` | en-tête de scène : `Drawings` (nombre de clips), `Name` |
| `3389b846` | clip |
| `3387e34a` | en-tête de clip (clé/valeur) |
| `33871154` | corps du clip : IFF `FORM…TVPPXS24` |

**Listes clé/valeur** : u32 nombre de paires, puis pour chaque paire deux chaînes (u16 longueur en caractères + UTF-16 BE).

- Projet : `Width`, `Height`, `FrameRate`, `Camera.Width`, `Camera.Height`, `Camera.FrameRate`, `PixelAspectRatio`, `Author`, `LastChangeDate`…
- Clip : `Name`, `Hidden`, `MarkIn`, `MarkInPosition`, `MarkOut`, `MarkOutPosition`, `ColorIdx`, `Action`, `Dialog`, `Note`.

## 2. Corps du clip (IFF)

Blocs : tag 4 caractères ASCII + taille u32 BE + contenu. Une taille impaire peut être suivie d'un octet de bourrage.

En tête de clip : `DGBL`, `DPEL`, `DLOC`, `BGMD` (8 octets), `ARAT`, `CRLR`, `BGP1`/`BGP2` (couleurs de fond RGBA ; différentes = damier), `ANNO`, `FRAT` (fps × 1000), `FILD`, `MARK`, `XSHT`, `TLNT`.

Puis, pour chaque calque, de haut en bas :

| bloc | contenu |
|---|---|
| `LNAM` | nom (cp1252) |
| `LNAW` | nom (UTF-8, prioritaire) |
| `LRHD` | en-tête de calque (104 octets) ; `LRSH` = même structure, calque CTG |
| `DBOD` | première image (PackBits) |
| `SRAW` | une entrée par frame suivante du calque |
| `LRSR` | (CTG) début de la série « résultat » (remplissage calculé), mêmes `DBOD`/`SRAW` ensuite |
| `LEXT` | texte INI : `[images auid]`, `[images name]`, CTG : `[scribbles source pos]` (positions 0-based des calques sources), `[scribblescolors]`, `[scribblestextures]`, `[result images auid]`… |
| `UDAT`, `STCK`, `XSRC` | non utilisés |

En fin de clip : `FCFG`, INI du clip (`[audio]`, `[cameradata]`, `[guideline]`, `[guiobjectcolors]`, `[info]`…). INI projet : `[guigroup]` (27 groupes de couleur : noms et couleurs).

### Blocs compressés `ZCHK` (fichiers de production)

```
tag interne (4) | taille interne (4) | "czmp" | u32 taille totale | u32 nombre de blocs
puis par bloc : u32 méthode (100 = zlib) | u32 taille brute | u32 taille compressée | données
```

Blocs de 64 Ko. Une fois décompressé, on obtient le bloc interne complet (tag + taille + contenu).

## 3. En-tête de calque `LRHD`

| offset | type | champ |
|---|---|---|
| 4 | u32 | première frame |
| 8 | u32 | dernière frame |
| 12 | u32 | nombre de frames (0 = calque image, visible de début à fin) |
| 19 | u8 | opacité 0–255 |
| 28 | u32 | drapeaux : bit 0 masqué, bit 4 verrouillé |
| 44 | u16 | comportement après |
| 52 | u16 | comportement avant |
| 60 | u32 | mode de fusion |

Comportements : 0 aucun, 1 répéter, 2 ping-pong, 3 tenir. Répéter et ping-pong sont ancrés au début du calque, y compris vers l'arrière.

Modes de fusion : 0 Color, 1 Behind, 2 Erase, 3 Shade, 4 Light, 5 Colorize, 6 Hue, 7 Add, 8 Sub, 9 Multiply, 10 Screen, 11 Replace, 12 Copy, 15 Difference, 16 Divide, 17 Overlay, 18 Light2, 19 Shade2, 20 HardLight, 21 SoftLight, 22 GrainExtract, 23 GrainMerge, 24 Sub2, 25 Darken, 26 Lighten, 27 Value, 28 Saturation.

## 4. Pixels

BGRA, alpha prémultiplié.

**PackBits** (DBOD, tuiles), ligne par ligne, sur des pixels de 4 octets :
- n ≥ 128 : répéter 257 − n fois le pixel suivant ;
- sinon : n + 1 pixels littéraux.

**Entrées SRAW** : le premier u32 donne le type.

| type | sens |
|---|---|
| `0x40` | nouvelle image, en tuiles |
| `6` | tenue : prolonge l'image précédente |
| `5` | nouvelle instance vide |
| `2` | nouvelle instance qui réutilise l'image de la frame du calque donnée par le u32 à l'offset 4 (ex. `00000002 00000021` → frame 33) |

Il y a une entrée par frame de la timeline du calque. La frame 0 du calque est le `DBOD` s'il existe ; sinon c'est la première entrée `SRAW`, appliquée à une image vide.

**Nouvelle image (0x40)** :

```
u32 type | u32 taille de tuile (64) | u32 thumb_size | u16? largeur, hauteur de la vignette
vignette : thumb_size − 8 octets après l'en-tête de 16 octets
u32 nombre de tuiles
pour chaque tuile (64×64, partielles au bord droit / bas) :
    u32 taille > 0 : tuile PackBits
    u32 0 : u32 source, u32 index  — source 1 = tuile `index` de l'image précédente,
                                     source 0 = tuile `index` de l'image en cours
```

**Calques CTG** : en-tête `LRSH`. La série principale contient les traits de construction ; le remplissage calculé est stocké après `LRSR` (souvent sans `DBOD` : la série résultat part d'une image vide). Le rendu utilise la série résultat.

## 5. Caméra (`[cameradata]` dans `FCFG`)

Clés : `pathcount`, `mpoints-i-x`, `mpoints-i-y`, `mpoints-i-rotation`, `mpoints-i-zoomfactor`, `mmode`, `mmotionblur` (flottant, ex. 0.5), `mprofile_prof_linear` (1 Linéaire, 0 Courbe, 2 Polynômial), `mprofile_prof_numpoint`, `mprofile_prof_pointN` = « t p ».

Un projet neuf n'a aucun point : la caméra est alors centrée, au zoom 1.

**Cadrage** (coordonnées continues, y vers le bas, angle en degrés) :

```
sortie − centre_sortie = R(+rotation) · (projet − centre_caméra) / zoom
```

La taille de sortie est `Camera.Width × Camera.Height`. Un zoom > 1 montre une zone plus grande (dézoom). Hors de la feuille, le rendu est noir.

**Répartition du temps entre les points** :
- Pour chaque segment i, Lᵢ est la distance en x/y. Avec n segments : w₀ = L₀, wᵢ = (n − 1)·Lᵢ₋₁ + Lᵢ. Si tous les poids sont nuls (zoom ou rotation seuls), les poids sont égaux.
- À l'intérieur d'un segment, x, y, rotation et zoom sont interpolés linéairement.

**Temps** :
- N = étendue des calques visibles (calques masqués et marques ignorés).
- Frame f → t = f / (N − 1), puis u = profil(t).

**Profils** :
- Linéaire.
- Courbe : Hermite cubique, tangentes Catmull-Rom `(y[i+1] − y[i−1]) / (x[i+1] − x[i−1])`, tangentes nulles aux extrémités. Avec deux points, c'est une droite.
- Polynômial : interpolation de Lagrange.

**Flou de mouvement** : moyenne des cadrages sur `[t, t + flou / (N − 1)]` (fenêtre vers l'avant).

**Limites connues** :
- Écart résiduel du modèle de ≤ 0,7 % du mouvement par rapport à `tv_CameraInterpolation`.
- Quand `Camera.FrameRate` ≠ `FrameRate`, TVPaint rééchantillonne le temps au rendu ; ce n'est pas encore modélisé. C'est le cas de la plupart des fichiers du corpus caméra (projet 24 i/s, caméra 25 i/s) : c'est l'écart de 3 à 8 % qui apparaît dans `tests/baseline_camera.json`. À 25/25 (N050_fps25, N117_fps25, N120_fps25), l'écart est nul.

## 6. Rendu

- Les marques in/out limitent les frames rendues (bornes incluses).
- Fond : `BGP1` sur la feuille, noir en dehors. Si `BGP2` est différent de `BGP1`, c'est un damier : le rendu utilise `BGP1` et affiche un avertissement.
- Calques visibles, du bas vers le haut, avec leur opacité et leur mode de fusion. Color est validé ; Behind, Erase, Multiply, Screen, Add, Darken, Lighten, Difference et Overlay sont implémentés avec les formules W3C, mais pas encore validés contre TVPaint.
- Rééchantillonnage : bilinéaire, comme Pillow, sur les pixels prémultipliés.
- Alpha droit pour l'export : arrondi `(c × 510 + a) / (2a)`, identique à l'export PNG de TVPaint.

## 7. Pièges George (pour étendre le corpus)

- `LOCAL` : variables séparées par des espaces, sans virgules.
- Pas d'expression dans les arguments d'une commande : calculer dans une variable d'abord.
- Le programme principal doit être placé avant les `FUNCTION`.
- Les arguments de fonction sont coupés aux virgules, même entre guillemets. `tv_WriteProjectString` échoue avec des espaces ou des virgules.
- `tv_LayerCreate` crée un calque image sous le calque courant (`tv_LayerAnim lid` pour le passer en anim).
- Sauvegarder avec `tv_SaveProject`. Pour valider le dernier trait : `tv_UpdateUndo`, puis créer et supprimer un calque temporaire.
- `RectFill` d'un pixel ne dessine rien. `tv_LayerBlendingMode lid "Mode"` (sans l'id, la commande est ignorée).
- `tv_ProjectSaveSequence chemin "camera" début fin` rend à travers la caméra. TVPaint remplace les chiffres du nom de fichier par le numéro de frame : utiliser des noms sans chiffres.
- `tv_CameraInsertPoint idx x y angle échelle`. `tv_CameraInterpolation u` renvoie « x y angle échelle » (utiliser `PARSE result a b c d`).
- `tv_MarkIn n "set"`, `tv_MarkOut n "set"`, `tv_LayerDisplay lid "off"`, `tv_LayerLock lid "on"`, `tv_LayerPreBehavior` / `tv_LayerPostBehavior lid "repeat|pingpong|hold|none"`.
- `tv_ClipName` ne renomme pas le premier clip ; `tv_ClipNew "nom"` fonctionne.
- Le dossier de sortie des scripts est défini par la variable `base` en tête de script, jamais en dur.
