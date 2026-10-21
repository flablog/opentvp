# opentvp — read and render TVPaint 11 files without TVPaint

A single executable, no installation, no dependencies (≈ 220 KB).
`opentvp.exe` for 64-bit Windows 10/11, `opentvp-macos` for Mac (universal binary: Apple Silicon M1–M4 and Intel, macOS 11 and later), `opentvp-linux` (static) for a Linux server.

### First run on a Mac

The binary is not signed with an Apple developer account, so macOS blocks it the first time. In the Terminal, once:

```
cd path/to/opentvp
xattr -d com.apple.quarantine opentvp-macos
chmod +x opentvp-macos
./opentvp-macos info file.tvpp
```

## Commands

```
opentvp info   file.tvpp            readable structure (resolution, fps, clips, layers, camera)
opentvp info   file.tvpp --json     the same as JSON (+ frame-by-frame camera track)
opentvp render file.tvpp folder     final composited render, like "Save sequence" through the camera
opentvp render file.tvpp folder --layers --keyframes    one folder per layer, one image per drawing + exposure JSON
opentvp render file.tvpp folder --layers                one folder per layer, every frame
```

`render` options:

| option | effect |
|---|---|
| `--clip N` | clip to render (default 1) |
| `--frames A-B` | frame range, numbered from 0 as in the project settings (the timeline shows +1). Default: in/out marks, otherwise all visible content |
| `--bg project` / `--bg transparent` / `--bg #RRGGBB` | background (default: project background colour, black outside the canvas). The pre-0.4 spelling `--bg projet` still works |
| `--scale 0.5` | downscaled render (quick preview) |
| `--blur-samples N` | camera motion blur samples (default 8) |
| `--threads N` | number of cores used (default: all) |
| `--prefix NAME` | PNG prefix (default: clip name) → `NAME_0000.png`, … |
| `--png-level N` | PNG compression 0-12 (default 6) |

## Layer-by-layer export

| option | effect |
|---|---|
| `--layers` | all visible layers, each in its own folder |
| `--layers "IMANI_LINE,IMANI_COLO_CTG,3"` | only these layers (name, or number given by `opentvp info`) |
| `--keyframes` | one image per drawing (holds and reused drawings are not duplicated) + exposure JSON |
| `--camera` | (without `--keyframes`) each layer framed by the camera, at output size, motion blur included |
| `--include-hidden` | include hidden layers |
| `--skip-empty` | no transparent image when the layer is empty |
| `--json NAME` | JSON file name (default: `<clip>_exposure.json` or `<clip>_layers.json`; before 0.4: `_exposition.json` / `_calques.json`) |

Images are RGBA PNGs with straight alpha, at canvas size, holding the raw pixels of the layer. Opacity, blending mode, pre/post behaviours and the camera track are given in the JSON.

Files are numbered after the first frame where the drawing appears. `IMANI_LINE_0012.png` is therefore the same file in drawings mode and in every-frame mode.

In the JSON, `timeline` gives, for each frame, the file to display (`null` if the layer is empty). `exposures` gives the same information as blocks (start, duration, file). The full format is described in `LAYER_EXPORT.md`.

`opentvp --version` prints the version.

Examples:

```
opentvp.exe render "C:/projects/nebuleuse/sq001_sh150_colo.tvpp" "C:/renders/sh150" --prefix sh150
opentvp.exe render "C:/projects/nebuleuse/sq001_sh140_colo.tvpp" "C:/export/sh140" --layers --keyframes
```

## What is supported

- TVPaint 11 files (tested on 11.7.6), including compressed production files.
- Image, anim and CTG layers (the computed fill is read back, not recomputed).
- Exposure, holds, reused frames, pre/post behaviours (repeat, ping-pong, hold).
- Visibility, opacity, blending modes Color (Normal), Behind, Erase, Multiply, Screen, Add, Darken, Lighten, Difference, Overlay.
- Camera: points, zoom, rotation, speed profile (Linear, Curve, Polynomial), motion blur.
- In/out marks, camera output resolution and frame rate.

## Measured accuracy (compared with TVPaint renders)

- sq001_sh150 (CTG, 55 frames): identical within 1/255 on every frame.
- sq001_sh140 (5990×3557, camera zooming out ×2.7): still shots 0.3 % of pixels differ (stroke edges, filtering), moving shots up to 1 % (slight camera track offset, < 0.7 % of the movement).
- Camera corpus: translations, profiles, blur: ≤ 0.5 % of pixels differ.
- Layer export: pixel-identical to TVPaint's structure export on the 72 corpus files (591 frames), and to the Python version in every mode (drawings, every frame, camera, scale).

## Speed (2-core test machine, faster on a workstation)

- Final render sh150 (2304×1296 → 1920×1080): ≈ 0.35 s/frame.
- Final render sh140 (5990×3557 → 1920×1080, 17 visible layers): ≈ 1.4 s/frame, ≈ 2 GB of memory.
- Layer export, drawings only: sh150 4 s (152 drawings); sh140 1 min 45 s (434 drawings, 38 MB, < 1 GB of memory).
- Layers are processed in parallel, one per core. `--threads N` lowers that number if memory runs short (≈ 4 × the size of a canvas image per layer in progress).

## Known limitations

- Unsupported blending modes (Shade, Light, Hue, Colorize, Soft Light…): rendered as Color (Normal), with a warning.
- If the camera frame rate differs from the project's, TVPaint resamples; opentvp renders at the project frame rate (warning in `info`).
- Checkerboard background (two colours): rendered on the first colour.
- Not tested yet: TVPaint 12 files, sound / reference / folder layers.
- Files are read entirely into memory: allow the size of the .tvpp + ~100 MB per large active layer.

## Building it yourself

See the repository README: `c/build.sh` (native binary) or `c/build.sh all` (all platforms, with `pip install ziglang`).
