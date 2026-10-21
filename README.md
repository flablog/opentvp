# opentvp

Read, render and export TVPaint Animation 11 files (`.tvpp`) without TVPaint, from the command line, on a workstation or a headless server.

- `opentvp info shot.tvpp`: structure (resolution, frame rate, clips, layers, camera), as text or JSON.
- `opentvp render shot.tvpp out`: final composited render, through the camera.
- `opentvp render shot.tvpp out --layers [--keyframes]`: one folder per layer, with every frame or only the drawings, plus an exposure JSON.

The format was reverse-engineered (Les Fées Spéciales, 2026) and is documented in [`docs/FORMAT.md`](docs/FORMAT.md).

## Download

Binaries are in the **Releases**: `opentvp-X.Y.Z-windows.zip`, `-macos.zip` (universal Apple Silicon + Intel), `-linux.zip` (static), and `opentvp-python-X.Y.Z.zip`. The user guide is [`docs/USAGE.md`](docs/USAGE.md).

## Repository layout

| folder | content |
|---|---|
| `c/` | the shipped tool: `opentvp.c` + libdeflate (MIT); `build.sh`, `package.sh` |
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

Third-party code built into the binary: libdeflate (MIT, `c/third_party/libdeflate/COPYING`); for the static Linux binary, musl (MIT, `c/third_party/musl/COPYRIGHT`). The licenses are included in the release archives. Python dependencies: numpy (BSD), Pillow (MIT-CMU).
