# Corpus

Ce sont des fichiers `.tvpp` minuscules, générés dans TVPaint 11.7.6 par les scripts de `george/`. Chaque fichier n'isole qu'une seule propriété du format. À côté de chaque fichier se trouve ce que TVPaint en exporte lui-même :

- `*_gt.csv` et `*_gt.layers/` : `tv_ClipSaveStructure` (une image PNG par instance et par calque) ;
- `render_*_NNNN.png` : rendu à travers la caméra (`tv_ProjectSaveSequence … "camera"`).

| dossier | contenu | script |
|---|---|---|
| `01_bases` | projet vide, carrés, résolution, fps, renommage, opacité, fusion | `make_tvpp_corpus.grg` |
| `02_images_calques` | plusieurs images, modifications d'un pixel, tailles non multiples de 64, calques masqués | `make_tvpp_corpus2.grg` |
| `03_tuiles_fusion` | tuiles de bord, tuiles identiques, tous les modes de fusion | `make_tvpp_corpus3.grg` |
| `04_expositions` | tenues, deux par deux, instances vides, décalage de calque, noms d'instance | `make_tvpp_corpus4.grg` |
| `05_comportements` | avant/après : aucun, répéter, ping-pong, tenir | `make_tvpp_corpus5.grg` |
| `06_plusieurs_clips` | deux clips dans un projet | (manuel) |
| `07_camera_base` | translation, zoom, rotation, un point, points inégaux | `make_tvpp_cam.grg` |
| `08_camera_points_doubles` | points répétés (tenues de caméra) | `make_tvpp_cam2.grg` |
| `09_camera_trajets` | trajets à 3–5 points, marques | `make_tvpp_cam3.grg` |
| `10_camera_duree_visible` | durée = calques visibles (masqués et marques ignorés) | `make_tvpp_cam5.grg` |
| `11_camera_cadence` | durées de 10 à 120 frames, projet à 24 ou 25 i/s | `make_tvpp_cam6.grg` |
| `12_camera_profils_flou` | profils de vitesse (linéaire, courbe, polynômial) et flou de mouvement | `make_tvpp_cam7_base.grg` + réglages à la main + `make_tvpp_cam7_render.grg` |

Autres scripts :

- `make_tvpp_cam4.grg` (S2–S4) échantillonne `tv_CameraInterpolation` dans des chaînes du projet. C'est ce qui a servi à établir la loi de répartition du temps ; ses résultats ne sont pas dans `data/`.
- `make_tvpp_ctg_base.grg` et `make_tvpp_ctg_gt.grg` (C01–C06) forment un corpus CTG manuel. Il n'a pas été produit, parce qu'un plan de production a suffi ; il est à reprendre pour un corpus CTG public.
- `render_reference.grg` rend un plan de production à travers la caméra, pour `tests/compare_render.py`.

Pour regénérer le corpus, par exemple avec TVPaint 12 : ouvrir chaque script, régler `base` sur un dossier vide, le lancer dans TVPaint, puis comparer avec `python tests/check_layers.py dossier` et `tests/check_camera.py`.
