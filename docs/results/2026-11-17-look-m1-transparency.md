# Look milestone 1: transparency

Same scene, machine, and display as the earlier runs, `--redraw damage`. Scene windows on a layer above 0 are now drawn as translucent overlays with no opaque part; 4 of the 17 mapped windows are such overlays.

## Summary

| Run | Idle footprint (MB) | Compositor CPU, animate (%) | GPU per frame, animate (ms) | GPU per frame, move (ms) |
|---|---:|---:|---:|---:|
| `--release unmapped` | 446.6 | 4.5 | 0.252 | 0.513 |
| `--release occluded` | 205.1 | 4.5 | 0.280 | 0.544 |
| `--release occluded --effects none` | 199.3 | 4.9 | 0.269 | 0.598 |

- The `occluded` figure held up. With overlays no longer hiding anything it is 205 MB, against 199 MB when every window is treated as opaque. The earlier worry that opaque test windows made it optimistic was mostly unfounded for this scene: the mapped overlays are small, and what covers other windows is three nearly full-screen ordinary windows.
- Blending costs nothing measurable here: CPU and GPU time per frame are within run-to-run variation of the opaque runs.
- Remaining caveat for `occluded`: ordinary windows (layer 0 and below) are still treated as opaque everywhere. Real ones have rounded corners and sometimes translucent areas.

## Correctness

- Partial redraw against full redraw, sanitizer build, small scene, `--release occluded`: move 23 frames checked, 0 mismatched; reveal 6 checked, 0 mismatched. Animate mismatched 6 of 23, for the known reason that the check is unreliable while content animates.
- Blended colours were compared with hand-computed values at four points in a capture and matched exactly.

## Run: unmapped

- Buffer release: unmapped
- Redraw: default
- Effects: default

| State | Compositor footprint (MB) | Client footprint (MB) | Total footprint (MB) | Compositor CPU (%) | Client CPU (%) | Total CPU (%) | Frames/s | GPU (ms/frame) | Reveals/s | Reveal mean (ms) | Reveal max since start (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| idle | 110.9 | 335.8 | 446.6 | 0.0 | 0.0 | 0.0 | 0.0 | - | 0.0 | - | - |
| animate | 223.3 | 335.7 | 558.9 | 4.5 | 1.0 | 5.5 | 62.6 | 0.252 | 0.0 | - | - |
| move | 235.4 | 335.9 | 571.3 | 4.5 | 0.1 | 4.6 | 62.4 | 0.513 | 0.0 | - | - |
| reveal | 220.0 | 335.8 | 555.8 | 0.5 | 0.8 | 1.3 | 2.0 | 1.023 | 1.0 | 7.69 | 9.11 |

## Run: occluded

- Buffer release: occluded
- Redraw: default
- Effects: default

| State | Compositor footprint (MB) | Client footprint (MB) | Total footprint (MB) | Compositor CPU (%) | Client CPU (%) | Total CPU (%) | Frames/s | GPU (ms/frame) | Reveals/s | Reveal mean (ms) | Reveal max since start (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| idle | 112.7 | 92.4 | 205.1 | 0.0 | 0.0 | 0.0 | 0.0 | - | 0.0 | - | - |
| animate | 223.1 | 92.5 | 315.6 | 4.5 | 1.0 | 5.5 | 62.5 | 0.280 | 0.0 | - | - |
| move | 223.2 | 92.3 | 315.5 | 4.8 | 0.1 | 4.9 | 62.6 | 0.544 | 0.5 | 0.49 | 0.82 |
| reveal | 220.0 | 92.6 | 312.6 | 0.5 | 0.9 | 1.4 | 2.0 | 1.595 | 1.0 | 8.99 | 10.94 |

## Run: occluded-none

- Buffer release: occluded
- Redraw: default
- Effects: none

| State | Compositor footprint (MB) | Client footprint (MB) | Total footprint (MB) | Compositor CPU (%) | Client CPU (%) | Total CPU (%) | Frames/s | GPU (ms/frame) | Reveals/s | Reveal mean (ms) | Reveal max since start (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| idle | 112.7 | 86.6 | 199.3 | 0.0 | 0.0 | 0.0 | 0.0 | - | 0.0 | - | - |
| animate | 222.7 | 86.7 | 309.4 | 4.9 | 1.0 | 5.9 | 62.2 | 0.269 | 0.0 | - | - |
| move | 221.6 | 86.7 | 308.3 | 5.8 | 0.1 | 5.9 | 62.5 | 0.598 | 0.5 | 0.69 | 1.50 |
| reveal | 219.9 | 86.9 | 306.8 | 0.5 | 0.9 | 1.4 | 2.0 | 1.280 | 1.0 | 9.18 | 10.59 |

