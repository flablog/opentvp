# opentvp — read and render TVPaint 11 files without TVPaint

A single executable, no installation, no dependencies (≈ 220 KB). A separate player, `opentvp-play`, is described below.
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
opentvp info   file.tvpp            readable structure (resolution, fps, clips, layers, camera, sound tracks)
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
| `--sound` | also writes the mix of the clip's sound tracks: `NAME.wav` (with `--layers`: `<clip>.wav`, named in the JSON) |
| `--sound-map OLD=NEW` | the sound files are looked for under NEW instead of OLD (repeatable), e.g. on a render farm |
| `--project-sound` | with `--sound`: also mixes the project's sound tracks (TVPaint leaves them out when it exports a clip). The clips follow one another on the project timeline: clip 2 starts where clip 1 ends |
| `--ffmpeg PATH` | ffmpeg used for sound files other than WAV (MP3, AAC / M4A, AIFF, FLAC, MOV…). Default: the first `ffmpeg` on the PATH, otherwise one next to `opentvp`. Without ffmpeg these files are reported and skipped; WAV files never need it |
| `--png-level N` | PNG compression 0-12 (default 6) |

## Player: opentvp-play

A separate executable, so that `opentvp` stays as small as it is: `opentvp-play.exe` (Windows) and `opentvp-play-linux` (Linux desktop, X11 or XWayland). Not available on macOS yet.

```
opentvp-play [file.tvpp file2.tvpp ...] [--clip N [--frames A-B]] [--canvas] [--scale S] [--blur-samples N] [--threads N]
```

It plays every clip of every file given, one after the other, in the order of the command line and then of each project, rendered through the camera exactly like `opentvp render`, in a loop at the project frame rate. Clips hidden in TVPaint are left out; **H** adds them to the timeline (purple names) or removes them again, without losing the frames already loaded. `--clip N` plays a single clip of a single file, hidden or not, and only then can `--frames` choose its range. Each clip plays its in/out marks, otherwise all its visible content. Files may have different sizes and frame rates: each clip plays at its own project frame rate. On Windows, you can also drop `.tvpp` files onto `opentvp-play.exe`. Started without a file (a double-click on the executable), the window shows *Drop a .tvpp file here* and waits for files dropped on it.

**Drag and drop.** Drag `.tvpp` files from the file explorer onto the player window: the window splits in two. Drop on the top half, **Replace current timeline**, to close the open files and clear the memory before opening the new ones; drop on the bottom half, **Append to timeline**, to add them after the current timeline. A file that cannot be opened is reported in a message and the rest is kept.

**Whole canvas.** **C** shows the current clip without the camera framing: the whole canvas, with what lies outside the camera frame darkened by half, so the framing stays visible while the camera moves (its frame at the start of each frame, without motion blur). C again goes back to the camera view. Only that clip changes, and its frames are rendered again; its timeline name says `canvas`. `--canvas` opens every clip this way, dropped files included.

**Preview resolution.** The button at the bottom left of the bar (`50%`) opens a menu: full resolution (100 %, the camera size) or half resolution (50 %, the default: 4 times faster to load and 4 times less memory). Changing it renders the frames again. The choice is remembered for the next time, in `%APPDATA%\opentvp\play.ini` (Windows) or `~/.config/opentvp/play.ini` (Linux); `--scale` overrides it for one session without saving it.

Frames are rendered in the background and kept in memory: playback starts with the first frames. Loading always continues from the current frame, inside the in/out selection first. **F1 shows the keyboard shortcuts in the player, Ctrl+I the file information**: canvas and camera size, frame rates (with a warning when TVPaint would resample), author and date, duration, preview resolution and memory used, every clip (range, frames, duration, marks, layers) and, for the current clip, its camera, background and any blending mode played as Color.

**Timeline.** One section per clip, with its name and its number of frames (click a name to play only that clip). Light grey = frames in memory, dark = not loaded yet, blue = played. Orange brackets show the in / out points; outside them the timeline is darker and playback loops between them. When playback catches up with frames that are not loaded yet, it waits and the play / pause button turns red; the window title shows the loading percentage. Holding Shift shows the keyframes as yellow dots under the timeline: the frames where a visible layer shows another drawing than on the previous frame (a held drawing coming back after another one counts again), plus the first frame of each clip. They are found when the file is opened, without decoding any picture.

**Counter.** First the frame in the current clip, counted from 0 as in the project settings (the TVPaint timeline shows +1), then the position in the whole playback / the total number of frames (counted from 1).

| key / control | effect |
|---|---|
| Space or K, play / pause button | play / pause |
| click or drag on the timeline | go to a frame |
| drag left / right on the picture | move through the frames by as much as the same movement on the timeline; pauses playback, and the frame reached stays when the button is released (with Shift: snaps to the keyframes) |
| Alt + drag on the picture | frame ruler where you clicked: one tick per frame (12 px each), keyframes in yellow, a long line at each clip start, a taller tick at the starting frame; the current frame in blue with, in big, its frame in the clip (from 0, as in the project settings: the TVPaint timeline shows +1) and, small above it, the offset since the click. With Shift: from one keyframe to the next. Pauses playback; releasing stays on the frame (on Linux, the window manager may keep Alt + drag to move windows) |
| Left / Right arrows, mouse wheel | one frame back / forward |
| Home / End | go to the in / out point |
| Ctrl + Left / Right, click on a clip name | play only the previous / next clip (sets in / out on it) |
| Ctrl + Shift + Left / Right | extend the in / out selection by one clip to the left / right (a partly selected clip is completed first) |
| Shift + Left / Right | previous / next keyframe |
| Shift held | show the keyframes; clicking or dragging on the timeline snaps the playhead to the nearest one |
| I or [ | in point at the current frame |
| O or ] | out point at the current frame |
| \ | reset in / out: play everything |
| H | show / hide the clips hidden in TVPaint |
| C | current clip: whole canvas with the camera frame shown / camera view |
| L | luminance: pictures shown in grey levels (Rec. 709 luma) / in colour; display only, nothing is rendered again |
| M | sound on / off |
| sound button in the bar (speaker) | opens the sound panel: volume slider, **Scrub sound** and **Mute (M)**; the mouse wheel over the button or the panel sets the volume (5 % steps); the volume and scrub sound are remembered in `play.ini` |
| Ctrl + I | file information |
| Ctrl + C | copy the current frame to the clipboard, as shown: preview resolution, camera or canvas view, grey levels with L (Windows: image and PNG, still there after quitting; Linux: PNG, available while the player is open unless a clipboard manager keeps it) |
| F, F11, fullscreen button, double-click on the picture | fullscreen on / off (the picture is resized to fit) |
| Esc | close the help or the information / leave fullscreen |
| Q | quit |
| F1 or ? | show / hide the shortcuts |

Keys are read as typed characters, so `[`, `]` and `\` work on any keyboard layout (AltGr on AZERTY). In fullscreen, the bar and the cursor hide after 2 seconds of playback without moving the mouse. If playback then has to wait for frames that are not loaded yet, the bar comes back on its own (red play / pause button, loading shown on the timeline) and hides again 2 seconds after playback is smooth again.

**Sound.** Each clip plays its own sound tracks and the project's, mixed as `opentvp render --sound --project-sound` does (the clips follow one another on the project timeline). The mix is prepared when the file is opened; a sound file that cannot be found is reported in the terminal and Ctrl+I, and the clip plays without it. Files other than WAV need ffmpeg, as for `render --sound` (`--ffmpeg PATH`, otherwise the PATH, otherwise next to the player). Sound follows normal playback, including the in / out loop and the passage from one clip to the next; it stops while playback is paused, scrubbed (drag, Alt + drag) or waiting for frames, and starts again on the displayed frame. With **Scrub sound** (in the sound panel), every frame shown outside playback (dragging on the timeline or the picture, Alt + drag, arrow keys, wheel, keyframe jumps) plays its own sound, one frame long: the sound of a frame can be checked while stepping through the animation. **M** turns it off and on. Windows: default output device; Linux: ALSA (`libasound.so.2`, loaded at run time; without it the player runs silently).

In memory, a 1920×1080 camera takes ≈ 2 MB per frame at half resolution and ≈ 8 MB at full resolution; if memory runs out, the player keeps the frames already loaded and says so.

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
