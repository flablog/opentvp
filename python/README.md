# tvpp en Python (implémentation de référence)

```
pip install -r requirements.txt          # numpy, Pillow (tvpp_info.py n'a besoin de rien)
python tvpp_info.py plan.tvpp [--json]
python tvpp_render.py plan.tvpp sortie [--layers [LISTE]] [--keyframes] [--camera] ...
```

Les options et les sorties sont les mêmes que celles de l'exe (`docs/UTILISATION.md`, `docs/EXPORT_CALQUES.md`), avec une option en plus : `--jobs N`, qui exporte plusieurs calques en parallèle (Linux et Mac seulement).

C'est la version à modifier pour essayer une idée. L'exe C est la version à livrer : les deux doivent produire des sorties identiques (`tests/run_all.sh`).
