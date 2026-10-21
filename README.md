# tvpp

Lire, rendre et exporter des fichiers TVPaint Animation 11 (`.tvpp`) sans TVPaint, en ligne de commande, sur un poste ou un serveur sans interface.

- `tvpp info plan.tvpp` : structure (résolution, cadence, clips, calques, caméra), en texte ou en JSON.
- `tvpp render plan.tvpp sortie` : rendu final composé, à travers la caméra.
- `tvpp render plan.tvpp sortie --layers [--keyframes]` : un dossier par calque, avec toutes les frames ou seulement les dessins, plus un JSON d'exposition.

Le format a été établi par rétro-ingénierie (Les Fées Spéciales, 2026) et il est documenté dans [`docs/FORMAT.md`](docs/FORMAT.md).

## Télécharger

Les binaires se trouvent dans les **Releases** : `tvpp-X.Y.Z-windows.zip`, `-macos.zip` (universel Apple Silicon + Intel), `-linux.zip` (statique), et `tvpp-python-X.Y.Z.zip`. Le mode d'emploi est dans [`docs/UTILISATION.md`](docs/UTILISATION.md).

## Contenu du dépôt

| dossier | contenu |
|---|---|
| `c/` | l'outil livré : `tvpp.c` + libdeflate (MIT) ; `build.sh`, `package.sh` |
| `python/` | implémentation de référence (prototypage), mêmes sorties que le C |
| `corpus/` | scripts George qui génèrent les fichiers de test, et ces fichiers avec la vérité terrain TVPaint |
| `tests/` | comparaison des deux implémentations avec la vérité terrain TVPaint |
| `docs/` | format, mode d'emploi, format du JSON d'export |

## Développer

```
pip install -r python/requirements.txt ziglang
./c/build.sh            # binaire natif dans c/build/
./tests/run_all.sh      # corpus : C et Python
./c/build.sh all        # + tvpp.exe, tvpp-macos, tvpp-linux
```

## Publier une version

1. Mettre à jour `VERSION`, par exemple `0.4.0`.
2. Committer, puis `git tag v0.4.0 && git push --tags`.
3. La CI lance les tests, compile pour toutes les plateformes et publie la release avec les zips.

Le C et le Python partagent la même version : une release contient les deux.
