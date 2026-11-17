# Milestone 3: buffer release

Same scene, machine, and display as the milestone 2 run. Two release policies, each a separate benchmark run.

## Summary

| Policy | Idle footprint (MB) | Against milestone 2 | Reveal mean (ms) | Reveal max (ms) |
|---|---:|---:|---:|---:|
| Milestone 2 (keep everything) | 2,503.8 | | | |
| `unmapped` | 452.8 | -82% | 6.29 | 8.03 |
| `occluded` | 202.5 | -92% | 7.19 | 8.61 |

## Policy: unmapped

- Date: 2026-11-17 19:02:25 +0100
- Machine: Mac16,8, Apple M4 Pro
- macOS: 27.2.1
- Scene: desktop-builtin-2026-11-17.scene
- Display: 1800x1169 points, scale 2
- Windows: 199 (17 mapped)
- Client processes: 8
- Buffer release: unmapped
- Settle: 3 s, measure: 10 s

| State | Compositor footprint (MB) | Client footprint (MB) | Total footprint (MB) | Compositor CPU (%) | Client CPU (%) | Total CPU (%) | Frames/s | Reveals/s | Reveal mean (ms) | Reveal max since start (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| idle | 117.4 | 335.4 | 452.8 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | - | - |
| animate | 225.4 | 335.5 | 560.9 | 6.4 | 1.1 | 7.5 | 62.6 | 0.0 | - | - |
| move | 223.5 | 335.5 | 559.1 | 6.2 | 0.1 | 6.4 | 62.5 | 0.0 | - | - |
| reveal | 234.0 | 335.5 | 569.5 | 0.6 | 0.7 | 1.3 | 3.0 | 1.0 | 6.29 | 8.03 |

## Policy: occluded

- Date: 2026-11-17 19:03:19 +0100
- Machine: Mac16,8, Apple M4 Pro
- macOS: 27.2.1
- Scene: desktop-builtin-2026-11-17.scene
- Display: 1800x1169 points, scale 2
- Windows: 199 (17 mapped)
- Client processes: 8
- Buffer release: occluded
- Settle: 3 s, measure: 10 s

| State | Compositor footprint (MB) | Client footprint (MB) | Total footprint (MB) | Compositor CPU (%) | Client CPU (%) | Total CPU (%) | Frames/s | Reveals/s | Reveal mean (ms) | Reveal max since start (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| idle | 116.0 | 86.4 | 202.5 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | - | - |
| animate | 223.5 | 86.3 | 309.8 | 5.9 | 1.1 | 7.0 | 62.5 | 0.0 | - | - |
| move | 234.1 | 86.5 | 320.6 | 6.1 | 0.1 | 6.2 | 62.9 | 0.5 | 0.59 | 0.80 |
| reveal | 234.0 | 86.6 | 320.6 | 0.7 | 0.8 | 1.4 | 3.0 | 1.0 | 7.19 | 8.61 |

## Notes

- `unmapped` releases the buffers of the 182 windows that are not mapped. What remains in the clients (335 MB) is close to the 317 MB that the 17 mapped windows need.
- `occluded` also releases mapped windows that are fully covered. This figure is optimistic: every test window is opaque, while several windows in the captured scene are transparent overlays on the real desktop and would not hide what is under them.
- The `reveal` state hides and shows the scene's topmost ordinary window (3600 x 2126 pixels, about 31 MB) once a second. Reveal time runs from the server announcing the window visible to the client's commit of a freshly painted buffer; it does not include presenting the next frame. The clients paint a flat colour, so a real application redrawing real content would take longer.
- In the `move` state under `occluded`, the orbiting window uncovers and covers a small window, which accounts for the 0.5 reveals per second there.
- The demo client no longer allocates a buffer for a window it creates unmapped, whatever the policy. `--release never` therefore no longer reproduces the milestone 2 figure; the recorded milestone 2 run is the baseline.
- CPU and frame rate in the idle, animate, and move states are unchanged from milestone 2, as expected: this milestone does not touch drawing.
