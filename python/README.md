# opentvp in Python (reference implementation)

```
pip install -r requirements.txt          # numpy, Pillow (opentvp_info.py needs nothing)
python opentvp_info.py shot.tvpp [--json]
python opentvp_render.py shot.tvpp out [--layers [LIST]] [--keyframes] [--camera] ...
```

Options and outputs are the same as the executable's (`docs/USAGE.md`, `docs/LAYER_EXPORT.md`), with one extra option: `--jobs N`, which exports several layers in parallel (Linux and Mac only).

This is the version to change when trying out an idea. The C executable is the version that ships: both must produce identical outputs (`tests/run_all.sh`).
