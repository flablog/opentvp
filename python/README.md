# opentvp in Python (reference implementation)

```
pip install -r requirements.txt          # numpy, Pillow (opentvp_info.py needs nothing)
python opentvp_info.py shot.tvpp [--json]
python opentvp_render.py shot.tvpp out [--layers [LIST]] [--keyframes] [--camera] ...
```

Options and outputs are the same as the executable's (`docs/USAGE.md`, `docs/LAYER_EXPORT.md`), with one extra option, `--jobs N`, which exports several layers in parallel (Linux and Mac only).

Sound: `--sound` also writes the mix of the clip's sound tracks as a WAV (`<prefix>.wav`, or `<clip>.wav` named in the JSON with `--layers`), and `--sound-map OLD=NEW` rewrites the path prefix of the sound files (for a render farm); a sound file not found is also looked for next to the `.tvpp`. Files other than WAV are decoded by ffmpeg when it is found (`--ffmpeg PATH`, otherwise the PATH, otherwise next to `opentvp_sound.py`). `--project-sound` also mixes the project's tracks (left out by TVPaint's clip export). Same WAV as the C, byte for byte. The mix alone: `python opentvp_sound.py shot.tvpp out.wav [--clip N] [--frames A-B] [--sound-map OLD=NEW]`. What the mix does: `opentvp_sound.py` and `docs/FORMAT.md` section 8.

This is the version to change when trying out an idea. The C executable is the version that ships: both must produce identical outputs (`tests/run_all.sh`).
