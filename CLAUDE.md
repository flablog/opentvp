# tvpp — lecture et rendu de fichiers TVPaint sans TVPaint

Projet des Fées Spéciales : lire les `.tvpp` (TVPaint Animation 11) sans TVPaint, pour rendre et exporter les plans sur un serveur, sans interface. Le format a été établi par rétro-ingénierie : **lire `docs/FORMAT.md` avant de toucher au décodage ou au rendu.**

## Organisation

- `c/` : l'outil livré (`tvpp`, `tvpp.exe`, `tvpp-macos`). Un seul fichier source, `c/tvpp.c`, plus `c/third_party/libdeflate` (MIT). Compilation : `c/build.sh` (natif) ou `c/build.sh all` (Windows, Mac universel, Linux statique, via `pip install ziglang`).
- `python/` : implémentation de référence, plus lente mais facile à modifier. `tvpp_info.py` (structure, aucune dépendance), `tvpp_render.py` (rendu et export par calque, numpy + Pillow), `proto.py` (décodage des pixels). Les nouvelles fonctions se prototypent ici, puis se portent en C.
- `corpus/` : `george/` contient les scripts qui génèrent le corpus dans TVPaint ; `data/` contient les fichiers produits, avec la vérité terrain TVPaint (`*_gt.csv`, `*_gt.layers/`, `render_*.png`). `camera_refs.json` associe chaque fichier caméra à ses rendus.
- `tests/` : `run_all.sh` lance tout. `check_layers.py` compare l'export par calque à la vérité terrain (doit rester à 0 échec). `check_camera.py` compare les rendus caméra à `baseline_camera.json` (pas de régression).
- `docs/` : `FORMAT.md` (spécification), `UTILISATION.md` (mode d'emploi de l'exe), `EXPORT_CALQUES.md` (format du JSON).

## Règles

- Les sorties du C et du Python doivent rester identiques au pixel près (images et JSON). Toute évolution se fait des deux côtés, ou le README dit lequel est en avance.
- Les fichiers de production (plans sq…_sh…) ne vont jamais dans le dépôt. On les teste avec `tests/compare_render.py plan.tvpp dossier_refs`.
- Scripts George : le dossier de sortie passe toujours par une variable `base = "C:/projets/nebuleuse/TECH/test_tvpp/sorties"` en tête, jamais en dur. Respecter les pièges listés à la fin de `docs/FORMAT.md`.
- L'utilisateur compte les frames depuis 1 dans la timeline TVPaint ; les outils comptent depuis 0, comme les paramètres du projet. Le préciser à chaque fois qu'on parle d'un numéro de frame.
- Langue du projet : français (messages, docs, commentaires).
- Version : fichier `VERSION` à la racine, injecté dans le binaire et lu par le Python. Une release = un tag `vX.Y.Z` ; la CI compile et publie les binaires.

## Pistes ouvertes

- Rééchantillonnage quand la cadence caméra ≠ la cadence projet (cause des écarts dans `baseline_camera.json`).
- Écart résiduel du modèle caméra (≤ 0,7 % du mouvement).
- Validation des modes de fusion autres que Color ; modes non gérés (Shade, Light, Hue, Colorize, SoftLight…).
- TVPaint 12 : regénérer le corpus avec les mêmes scripts et comparer.
- Calques son, référence, dossiers : non étudiés.
- Import After Effects (script ExtendScript lisant le JSON d'exposition).
