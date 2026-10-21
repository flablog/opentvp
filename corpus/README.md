# Corpus

Tiny `.tvpp` files, generated in TVPaint 11.7.6 by the scripts in `george/`. Each file isolates a single property of the format. Next to each file is what TVPaint itself exports from it:

- `*_gt.csv` and `*_gt.layers/`: `tv_ClipSaveStructure` (one PNG per instance and per layer);
- `render_*_NNNN.png`: render through the camera (`tv_ProjectSaveSequence … "camera"`).

Folder names are in French, as generated; the table gives their content.

| folder | content | script |
|---|---|---|
| `01_bases` | empty project, squares, resolution, fps, renaming, opacity, blending | `make_tvpp_corpus.grg` |
| `02_images_calques` | several images, one-pixel changes, sizes that are not multiples of 64, hidden layers | `make_tvpp_corpus2.grg` |
| `03_tuiles_fusion` | edge tiles, identical tiles, every blending mode | `make_tvpp_corpus3.grg` |
| `04_expositions` | holds, on twos, empty instances, layer offset, instance names | `make_tvpp_corpus4.grg` |
| `05_comportements` | pre/post: none, repeat, ping-pong, hold | `make_tvpp_corpus5.grg` |
| `06_plusieurs_clips` | two clips in one project | (manual) |
| `07_camera_base` | translation, zoom, rotation, single point, uneven points | `make_tvpp_cam.grg` |
| `08_camera_points_doubles` | repeated points (camera holds) | `make_tvpp_cam2.grg` |
| `09_camera_trajets` | 3–5 point paths, marks | `make_tvpp_cam3.grg` |
| `10_camera_duree_visible` | duration = visible layers (hidden layers and marks ignored) | `make_tvpp_cam5.grg` |
| `11_camera_cadence` | durations from 10 to 120 frames, project at 24 or 25 fps | `make_tvpp_cam6.grg` |
| `12_camera_profils_flou` | speed profiles (linear, curve, polynomial) and motion blur | `make_tvpp_cam7_base.grg` + manual settings + `make_tvpp_cam7_render.grg` |

Other scripts:

- `make_tvpp_cam4.grg` (S2–S4) samples `tv_CameraInterpolation` into project strings. This is what established the time distribution law; its results are not in `data/`.
- `make_tvpp_ctg_base.grg` and `make_tvpp_ctg_gt.grg` (C01–C06) form a manual CTG corpus. It was not produced, because a production shot was enough; it should be picked up again for a public CTG corpus.
- `render_reference.grg` renders a production shot through the camera, for `tests/compare_render.py`.

To regenerate the corpus, for instance with TVPaint 12: open each script, set `base` to an empty folder, run it in TVPaint, then compare with `python tests/check_layers.py folder` and `tests/check_camera.py`.
