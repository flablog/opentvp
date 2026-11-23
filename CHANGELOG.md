# Changelog

Versions follow the `VERSION` file; a release is a `vX.Y.Z` tag. Frame numbers here count from 0, like the project settings (the TVPaint timeline shows +1).

## Unreleased

### Added
- `opentvp info` lists the sound tracks of the project and of each clip: file path (flagged when not found), offset, volume, mute and fades, as text and in the JSON (`sound`). TVPaint does not store the audio in the `.tvpp`, only the path of each file.
- Sound export: `opentvp render --sound` (and `opentvp_render.py --sound`) writes the mix of the clip's sound tracks as a 48 kHz stereo 24-bit WAV, the duration of the rendered frames (offset, volume, mute and fades as in TVPaint's movie export, to the sample; WAV sources only, other formats are reported and skipped). `--sound-map OLD=NEW` rewrites the path of the sound files. Also standalone in Python: `opentvp_sound.py`.
- `opentvp-play` plays the clips' sound tracks, in sync with the picture (in / out loop, clip to clip); **M** turns sound off and on. Windows: waveOut; Linux: ALSA loaded at run time.
- The build disables FMA contraction (`-ffp-contract=off`), so that C and Python compute the same floating-point results on every platform.

### Changed
- License: public domain (CC0 1.0) instead of the GPL v3 or later. Releases up to 0.4.0 stay available under the GPL.

## 0.4.0 — 2026-11-20

### Added
- `opentvp-play`, a separate player for Windows and Linux (X11): plays every clip of one or more `.tvpp` files through the camera, rendered in the background.
  - Timeline per clip with in / out points, clip selection and extension (Ctrl / Ctrl + Shift + arrows), clips hidden in TVPaint shown on demand (H).
  - Keyframes shown under the timeline with Shift held, snapping and keyframe-to-keyframe stepping.
  - Scrubbing by dragging on the picture; Alt + drag shows a frame ruler.
  - Whole canvas view with the camera frame (C, `--canvas`), half / full preview resolution (remembered), luminance display (L), fullscreen.
  - Drag and drop of `.tvpp` files (replace or append), empty start window waiting for files.
  - Copy of the current frame to the clipboard (Ctrl + C), file information (Ctrl + I), shortcuts (F1).
- GitHub release workflow, alongside the GitLab one.

### Changed
- The project is now named opentvp: `tvpp_info.py` / `tvpp_render.py` became `opentvp_info.py` / `opentvp_render.py`.
- Documentation and messages in English (`docs/USAGE.md`, `docs/LAYER_EXPORT.md`).

## 0.3.0 — 2026-10-21

First version: reads TVPaint Animation 11 `.tvpp` files without TVPaint.
- `opentvp info`: structure (resolution, frame rate, clips, layers, camera), as text or JSON.
- `opentvp render`: final render through the camera (position, zoom, rotation, motion blur).
- `opentvp render --layers [--keyframes]`: per-layer export, plus an exposure JSON.
- Single C executable for Windows, macOS (universal) and Linux (static), and a Python reference implementation with identical outputs.
