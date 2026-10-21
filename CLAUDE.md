# opentvp — reading and rendering TVPaint files without TVPaint

A Les Fées Spéciales project: read `.tvpp` files (TVPaint Animation 11) without TVPaint, to render and export shots on a headless server. The format was reverse-engineered: **read `docs/FORMAT.md` before touching decoding or rendering.**

## Layout

- `c/`: the shipped tool (`opentvp`, `opentvp.exe`, `opentvp-macos`). A single source file, `c/opentvp.c`, plus `c/third_party/libdeflate` (MIT). Build: `c/build.sh` (native) or `c/build.sh all` (Windows, universal Mac, static Linux, via `pip install ziglang`).
- `python/`: reference implementation, slower but easy to change. `opentvp_info.py` (structure, no dependencies), `opentvp_render.py` (rendering and per-layer export, numpy + Pillow), `proto.py` (pixel decoding). New features are prototyped here, then ported to C.
- `corpus/`: `george/` holds the scripts that generate the corpus in TVPaint; `data/` holds the resulting files, with TVPaint ground truth (`*_gt.csv`, `*_gt.layers/`, `render_*.png`). `camera_refs.json` maps each camera file to its renders.
- `tests/`: `run_all.sh` runs everything. `check_layers.py` compares the per-layer export with ground truth (must stay at 0 failures). `check_camera.py` compares camera renders with `baseline_camera.json` (no regression).
- `docs/`: `FORMAT.md` (specification), `USAGE.md` (executable user guide), `LAYER_EXPORT.md` (JSON format).

## Rules

- C and Python outputs must stay pixel-identical (images and JSON) and print the same messages. Every change is made on both sides, or the README says which one is ahead.
- Production files (sq…_sh… shots) never go into the repository. Test them with `tests/compare_render.py shot.tvpp refs_folder`.
- George scripts: the output folder always goes through a `base = "C:/projets/nebuleuse/TECH/test_tvpp/sorties"` variable at the top, never hard-coded. Mind the pitfalls listed at the end of `docs/FORMAT.md`.
- The user counts frames from 1 in the TVPaint timeline; the tools count from 0, like the project settings. Say which one every time a frame number comes up.
- Project language: English (messages, docs, comments). Corpus folder names and George scripts stay in French. The user may write in French.
- Version: `VERSION` file at the root, injected into the binary and read by the Python. A release = a `vX.Y.Z` tag; CI builds and publishes the binaries.

## Open leads

- Resampling when the camera frame rate ≠ the project frame rate (cause of the errors in `baseline_camera.json`).
- Residual camera model error (≤ 0.7 % of the movement).
- Validation of blending modes other than Color; unsupported modes (Shade, Light, Hue, Colorize, SoftLight…).
- TVPaint 12: regenerate the corpus with the same scripts and compare.
- Sound, reference and folder layers: not studied.
- After Effects import (ExtendScript reading the exposure JSON).
