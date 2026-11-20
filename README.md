# opentvp

Read, render and export TVPaint Animation 11 files (`.tvpp`) without TVPaint, from the command line, on a workstation or a headless server.

- `opentvp info shot.tvpp`: structure (resolution, frame rate, clips, layers, camera), as text or JSON.
- `opentvp render shot.tvpp out`: final composited render, through the camera.
- `opentvp render shot.tvpp out --layers [--keyframes]`: one folder per layer, with every frame or only the drawings, plus an exposure JSON.
- `opentvp-play shot.tvpp [shot2.tvpp ...]`: a separate, minimal player for every clip of the files (timeline per clip, in / out, hidden clips on demand, drag and drop to replace or append, starts empty to receive dropped files, whole canvas view with the camera frame, half / full resolution, fullscreen; F1 for the shortcuts, Ctrl+I for the file information), Windows and Linux.

The format was reverse-engineered (Flavio Perez, 2026) and is documented in [`docs/FORMAT.md`](docs/FORMAT.md).

## About this project

These tools were written to help TVPaint users automate their exports from the command line: in the background, without tying up TVPaint, and above all on a render farm when needed. They do not replace TVPaint in any way: TVPaint remains the animation tool.

**Compatibility.** The tools work with TVPaint Animation 11.x. They have not been checked against TVPaint 12 yet; that is coming.

**Reverse-engineered, so possibly fragile.** opentvp reads `.tvpp` files directly, from a format that was learned by reverse engineering. The file structure is not publicly documented, so any TVPaint update may break the tools at any time. If you notice a change, or a missing feature, please let me know by opening an issue.

**How it was made.** This code was vibe-coded with Claude Opus 5.5 (medium effort), and is provided as is, without warranty. Although I work at the Les Fées Spéciales studio, I wrote it on my own, with my personal Claude account: it does not engage the responsibility of the studio, which builds many tools and add-ons without AI. That is why this tool is not part of the studio's collection of shared tools.

That said, being able to query and export TVPaint files is something we, as a studio, have needed for years. LLMs gave me the opportunity to do work I would not otherwise have taken on, and since these tools are, in my opinion, so useful and practical, I am sharing them anyway.

I hope the TVPaint developers can take into account, in their future versions, the needs this exporter brings to light.

This code is free software: do what you like with it, within the terms of its license (below).

Happy animating!

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

1. Update `VERSION`, for example `0.4.0`, and add its section to [`CHANGELOG.md`](CHANGELOG.md).
2. Commit, then `git tag -a v0.4.0 -m "opentvp 0.4.0" && git push origin main v0.4.0`.
3. CI runs the tests, builds for every platform and publishes the release with the zips; the release notes are the version's section of the changelog.

C and Python share the same version: a release contains both.

## License

GPL v3 or later (`GPL-3.0-or-later`), see [LICENSE](LICENSE). Written by Flavio Perez.

Third-party code built into the binary: libdeflate (MIT, `c/third_party/libdeflate/COPYING`); for the static Linux binary, musl (MIT, `c/third_party/musl/COPYRIGHT`); for the player, the Spleen bitmap font (BSD 2-clause, `c/third_party/spleen/LICENSE`) and, on Linux, the X11 headers (MIT, `c/third_party/x11/COPYING`; libX11 itself is loaded at run time). The licenses are included in the release archives. Python dependencies: numpy (BSD), Pillow (MIT-CMU).
