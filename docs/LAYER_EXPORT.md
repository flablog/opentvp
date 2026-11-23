# Layer-by-layer export — JSON format

This format is shared by `opentvp.exe` (or `opentvp-macos`, `opentvp-linux`) and by the Python version `opentvp_render.py`.

## Two modes

```
# 1. Drawings only: one image per drawing + a JSON saying when each one is exposed
opentvp render shot.tvpp out --layers --keyframes

# 2. Every frame: one image per frame and per layer, holds included
opentvp render shot.tvpp out --layers
```

Choosing layers: `--layers all` (default, visible layers), `--layers "IMANI_LINE,IMANI_COLO_CTG"`, or their numbers in `opentvp info` (1 = top layer): `--layers 2,3`. To include hidden layers: `--include-hidden`.

Each layer has its own folder, named after the layer. If two layers share a name, the folder is prefixed with the layer number.

```
out/
  Clip_001_exposure.json           (or _layers.json in every-frame mode)
  IMANI_LINE/IMANI_LINE_0000.png
  IMANI_LINE/IMANI_LINE_0002.png
  ...
```

## What each image contains

- The whole project canvas (e.g. 5990×3557), without the camera, with a straight (non-premultiplied) alpha channel, rounded the same way as TVPaint's export.
- The raw pixels of the layer: opacity and blending mode are not applied, they are given in the JSON.
- CTG layers give their computed fill, as in TVPaint.
- `--scale 0.5` downscales the images. `--camera` (every-frame mode only) instead produces each layer framed by the camera, at output size, with motion blur.

## What a drawing is (`--keyframes` mode)

A drawing is a distinct image of the layer. A hold does not create a drawing, and a drawing reused later in the timeline (same image exposed again) is exported only once. This is why the number of files can be lower than the number of "drawings" counted by `opentvp info` (which counts instances). Empty instances produce no file.

Each file is numbered after the first frame where the drawing appears in the exported range. So `IMANI_LINE_0012.png` is identical in drawings mode and in every-frame mode.

## Numbering and range

Frames are numbered from 0, as in the project settings (the TVPaint timeline shows +1). The default range runs from the in/out marks, or else covers the whole clip. It can be set with `--frames 0-108`. Pre/post behaviours (repeat, ping-pong, hold) are applied within that range.

## The JSON

```json
{
 "source": "sq001_sh150_colo.tvpp", "clip": "Clip_001",
 "mode": "keyframes",            // or "frames"
 "space": "canvas",              // or "camera"
 "fps": 25.0,
 "canvas": {"width": 2304, "height": 1296},
 "image_size": {"width": 2304, "height": 1296},
 "frame_range": [0, 54],
 "alpha": "straight",
 "camera": {
   "output": {"width": 1920, "height": 1080, "fps": 25.0},
   "motion_blur": 0.0,
   "track": [{"frame": 0, "x": 1152.0, "y": 648.0, "rotation": 0.0, "zoom": 1.0}, ...]
 },
 "layer_order": "top to bottom, as in TVPaint; composite starting from the last one",
 "layers": [
  {
   "index": 3, "name": "IMANI_SCULPT", "folder": "IMANI_SCULPT",
   "type": "anim", "visible": true, "opacity": 1.0, "blending": "Color",
   "pre_behavior": "none", "post_behavior": "none", "start": 0, "end": 54,
   "drawings": [{"file": "IMANI_SCULPT/IMANI_SCULPT_0000.png", "first_frame": 0, "layer_frame": 0}, ...],
   "exposures": [{"start": 0, "duration": 2, "file": "IMANI_SCULPT/IMANI_SCULPT_0000.png"},
                 {"start": 2, "duration": 3, "file": "IMANI_SCULPT/IMANI_SCULPT_0002.png"}, ...],
   "key_frames": [0, 2, 5, 8, ...],
   "timeline": ["IMANI_SCULPT/IMANI_SCULPT_0000.png", "IMANI_SCULPT/IMANI_SCULPT_0000.png", ...]
  }
 ]
}
```

- `timeline`: for each frame of the range, the file to display, or `null` when the layer is empty. This is the simplest field for an import (After Effects, Blender, Libreflow).
- `exposures`: the same information, grouped into blocks (start, duration, file). Present in `--keyframes` mode.
- `key_frames`: the frames where a new drawing appears.
- `camera.track`: the camera frame by frame (centre in canvas pixels, rotation in degrees, TVPaint zoom), to redo the framing in the compositing software. The relation is: `output − output_centre = R(rotation) · (canvas − centre) / zoom`.
- `sound` (with `--sound`, when the clip has sound tracks): `{"file": "Clip_001.wav", "start_frame": 0, "rate": 48000}`. The WAV is the mix of the clip's tracks, 48 kHz stereo 24-bit; its first sample is the start of `start_frame` (the first frame of the range), and it lasts exactly the range.

## Recompositing the shot

To recomposite the shot, start from the last layer in the list (the bottom one). Stack the images applying `opacity` and `blending`, then frame the result with `camera.track`. This recomposition is verified: it reproduces the TVPaint render of sh150 on all 55 frames, and gives exactly the same result as the final render on sh140.

## Verification

`--keyframes` mode was compared with TVPaint's structure export (`tv_ClipSaveStructure`) on the 72 corpus files, 591 frames in all: every one is pixel-identical. This covers holds, empty instances, pre/post behaviours, blending modes and tiles.
