# opentvp

Read, render and export TVPaint Animation 11 files (`.tvpp`) without TVPaint, from the command line, on a workstation or a headless server.

- `opentvp info shot.tvpp`: structure (resolution, frame rate, clips, layers, camera), as text or JSON.
- `opentvp render shot.tvpp out`: final composited render, through the camera.
- `opentvp render shot.tvpp out --layers [--keyframes]`: one folder per layer, with every frame or only the drawings, plus an exposure JSON.
- `opentvp-play shot.tvpp [shot2.tvpp ...]`: a separate, minimal player for every clip of the files (timeline per clip, in / out, hidden clips on demand, drag and drop to replace or append, starts empty to receive dropped files, whole canvas view with the camera frame, half / full resolution, fullscreen; F1 for the shortcuts, Ctrl+I for the file information), Windows and Linux.

The format was reverse-engineered (Les Fées Spéciales, 2026) and is documented in [`docs/FORMAT.md`](docs/FORMAT.md).

## Download

Binaries are in the **Releases**: `opentvp-X.Y.Z-windows.zip`, `-macos.zip` (universal Apple Silicon + Intel), `-linux.zip` (static), and `opentvp-python-X.Y.Z.zip`. The user guide is [`docs/USAGE.md`](docs/USAGE.md).

## Repository layout

| folder | content |
|---|---|
| `c/` | the shipped tools: `opentvp.c`, the player `opentvp-play.c` (includes `opentvp.c`), libdeflate (MIT), X11 headers (MIT); `build.sh`, `package.sh` |
| `python/` | reference implementation (prototyping), same outputs as the C |
| `corpus/` | George scripts that generate the test files, and those files with TVPaint ground truth |
| `tests/` | comparison of both implementations with TVPaint ground truth |
| `docs/` | format, user guide, export JSON format |

## Development

```
pip install -r python/requirements.txt ziglang
./c/build.sh            # native binary in c/build/
./tests/run_all.sh      # corpus: C and Python
./c/build.sh all        # + opentvp.exe, opentvp-macos, opentvp-linux
```

## Releasing

1. Update `VERSION`, for example `0.4.0`.
2. Commit, then `git tag v0.4.0 && git push --tags`.
3. CI runs the tests, builds for every platform and publishes the release with the zips.

C and Python share the same version: a release contains both.

## License

GPL v3 or later (`GPL-3.0-or-later`), see [LICENSE](LICENSE). © 2026 Les Fées Spéciales.

Third-party code built into the binary: libdeflate (MIT, `c/third_party/libdeflate/COPYING`); for the static Linux binary, musl (MIT, `c/third_party/musl/COPYRIGHT`); for the player, the Spleen bitmap font (BSD 2-clause, `c/third_party/spleen/LICENSE`) and, on Linux, the X11 headers (MIT, `c/third_party/x11/COPYING`; libX11 itself is loaded at run time). The licenses are included in the release archives. Python dependencies: numpy (BSD), Pillow (MIT-CMU).
