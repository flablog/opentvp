# opentvp — reading and rendering TVPaint files without TVPaint

A personal project of Flavio Perez (not a Les Fées Spéciales project, see the README): read `.tvpp` files (TVPaint Animation 11) without TVPaint, to render and export shots on a headless server. The format was reverse-engineered: **read `docs/FORMAT.md` before touching decoding or rendering.**

## Layout

- `c/`: the shipped tool (`opentvp`, `opentvp.exe`, `opentvp-macos`). A single source file, `c/opentvp.c`, plus `c/third_party/libdeflate` (MIT). The player `c/opentvp-play.c` is a separate binary that `#include`s `opentvp.c` (with `OPENTVP_NO_MAIN`) and adds a minimal window layer: Win32 GDI (+ OLE drop target), or X11 loaded with `dlopen` (+ XDND) (headers vendored in `c/third_party/x11`). Its text uses the Spleen bitmap font, generated into `c/third_party/spleen/spleen.h` by `gen_font.py`. No macOS window layer yet: it needs a Mac to build and test. Build: `c/build.sh` (native) or `c/build.sh all` (Windows, universal Mac, static Linux, via `pip install ziglang`).
- `python/`: reference implementation, slower but easy to change. `opentvp_info.py` (structure, no dependencies), `opentvp_render.py` (rendering and per-layer export, numpy + Pillow), `proto.py` (pixel decoding). New features are prototyped here, then ported to C.
- `corpus/`: `george/` holds the scripts that generate the corpus in TVPaint; `data/` holds the resulting files, with TVPaint ground truth (`*_gt.csv`, `*_gt.layers/`, `render_*.png`). `camera_refs.json` maps each camera file to its renders.
- `tests/`: `run_all.sh` runs everything. `check_layers.py` compares the per-layer export with ground truth (must stay at 0 failures). `check_camera.py` compares camera renders with `baseline_camera.json` (no regression).
- `docs/`: `FORMAT.md` (specification), `USAGE.md` (executable user guide), `LAYER_EXPORT.md` (JSON format).

## Rules

- C and Python outputs must stay pixel-identical (images and JSON) and print the same messages. Every change is made on both sides, or the README says which one is ahead. The player is C only.
- Production files (sq…_sh… shots) never go into the repository. Test them with `tests/compare_render.py shot.tvpp refs_folder`.
- George scripts: the output folder always goes through a `base = "C:/projets/nebuleuse/TECH/test_tvpp/sorties"` variable at the top, never hard-coded. Mind the pitfalls listed at the end of `docs/FORMAT.md`.
- The user counts frames from 1 in the TVPaint timeline; the tools count from 0, like the project settings. Say which one every time a frame number comes up.
- Project language: English (messages, docs, comments). Corpus folder names and George scripts stay in French. The user may write in French.
- Version: `VERSION` file at the root, injected into the binary and read by the Python. Every user-visible change gets a line under `## Unreleased` in `CHANGELOG.md`.
- Release: rename `Unreleased` to `X.Y.Z — date`, bump `VERSION`, commit, then push a `vX.Y.Z` tag. Only tags trigger the GitHub CI (`.github/workflows/ci.yml`: tests, build, release whose notes are the version's changelog section); `.gitlab-ci.yml` is kept for a GitLab mirror. No release for doc-only changes.
- Git: GitHub `flablog/opentvp`, `main` is the published branch. Features are developed on their own branch (`player`, `writer`) and reach `main` only when the user says so.
- License: public domain, CC0 1.0. New files start with `SPDX-License-Identifier: CC0-1.0`, no copyright line. Third-party code under `c/third_party/` keeps its own license and notices.

## Open leads

- Resampling when the camera frame rate ≠ the project frame rate (cause of the errors in `baseline_camera.json`).
- Residual camera model error (≤ 0.7 % of the movement).
- Validation of blending modes other than Color; unsupported modes (Shade, Light, Hue, Colorize, SoftLight…).
- TVPaint 12: regenerate the corpus with the same scripts and compare.
- Sound, reference and folder layers: not studied.
- After Effects import (ExtendScript reading the exposure JSON).
