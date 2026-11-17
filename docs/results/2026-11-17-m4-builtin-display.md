# Milestone 4: occlusion culling and partial redraw

Same scene, machine, and display as the earlier runs, with `--release unmapped`. Three redraw modes, each a separate benchmark run on the new presentation path (the compositor's own pool of surfaces set as layer contents).

## Summary

| | Milestone 3 | `overdraw` | `full` | `damage` |
|---|---:|---:|---:|---:|
| Compositor CPU, animate (%) | 6.4 | 4.4 | 4.7 | 4.4 |
| Compositor CPU, move (%) | 6.2 | 4.6 | 4.8 | 4.6 |
| GPU per frame, animate (ms) | not measured | 0.625 | 0.617 | 0.287 |
| GPU per frame, move (ms) | not measured | 0.612 | 0.586 | 0.472 |
| Idle footprint (MB) | 452.8 | 447.9 | 446.5 | 448.3 |

- Presenting our own surfaces instead of Metal layer drawables lowered compositor CPU by about 28% (6.4% to about 4.5%) in every mode. A sample of the old path showed the cost was per-frame overhead in drawable acquisition, commit, and present, not drawing.
- Occlusion culling on its own (`full` against `overdraw`) changes GPU time by 1 to 4%, which is within run-to-run variation. The likely reason is that the GPU already discards hidden opaque fragments before shading them; that explanation is not verified.
- Partial redraw (`damage` against `full`) cuts GPU time per frame by 53% when a window's content animates and by 19% when a window moves.
- Footprint is unchanged: the pool settles at three display-sized surfaces, the same as the drawable pool it replaces.

## Correctness check

With `--verify-redraw` the compositor redraws the whole scene into a scratch surface after every partially redrawn frame and compares all pixels. On this scene (release build, `--release unmapped`): animate 313 frames checked, 0 mismatched; move 313 checked, 0 mismatched; reveal 10 checked, 0 mismatched.

The check is not reliable while window content animates, because clients draw into a single buffer and can change pixels they have not committed yet. Under the much slower sanitizer build, 4 of 23 animate frames mismatched on the small scene for that reason, while move and reveal frames all matched.

## Redraw: overdraw

| State | Compositor footprint (MB) | Client footprint (MB) | Total footprint (MB) | Compositor CPU (%) | Client CPU (%) | Total CPU (%) | Frames/s | GPU (ms/frame) | Reveals/s | Reveal mean (ms) | Reveal max since start (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| idle | 112.5 | 335.3 | 447.9 | 0.0 | 0.0 | 0.0 | 0.0 | - | 0.0 | - | - |
| animate | 220.0 | 335.4 | 555.5 | 4.4 | 1.0 | 5.4 | 62.5 | 0.625 | 0.0 | - | - |
| move | 220.1 | 335.5 | 555.6 | 4.6 | 0.1 | 4.7 | 62.5 | 0.612 | 0.0 | - | - |
| reveal | 220.0 | 335.7 | 555.7 | 0.4 | 0.9 | 1.3 | 2.0 | 3.983 | 1.0 | 8.20 | 10.55 |

## Redraw: full

| State | Compositor footprint (MB) | Client footprint (MB) | Total footprint (MB) | Compositor CPU (%) | Client CPU (%) | Total CPU (%) | Frames/s | GPU (ms/frame) | Reveals/s | Reveal mean (ms) | Reveal max since start (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| idle | 111.0 | 335.5 | 446.5 | 0.0 | 0.0 | 0.0 | 0.0 | - | 0.0 | - | - |
| animate | 218.2 | 335.4 | 553.7 | 4.7 | 1.0 | 5.7 | 62.5 | 0.617 | 0.0 | - | - |
| move | 220.1 | 335.5 | 555.6 | 4.8 | 0.1 | 5.0 | 62.4 | 0.586 | 0.0 | - | - |
| reveal | 220.0 | 335.6 | 555.6 | 0.5 | 0.8 | 1.2 | 2.0 | 1.226 | 1.0 | 7.41 | 9.18 |

## Redraw: damage

| State | Compositor footprint (MB) | Client footprint (MB) | Total footprint (MB) | Compositor CPU (%) | Client CPU (%) | Total CPU (%) | Frames/s | GPU (ms/frame) | Reveals/s | Reveal mean (ms) | Reveal max since start (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| idle | 112.7 | 335.6 | 448.3 | 0.0 | 0.0 | 0.0 | 0.0 | - | 0.0 | - | - |
| animate | 223.1 | 335.5 | 558.6 | 4.4 | 1.0 | 5.5 | 62.6 | 0.287 | 0.0 | - | - |
| move | 223.2 | 335.6 | 558.7 | 4.6 | 0.1 | 4.7 | 62.5 | 0.472 | 0.0 | - | - |
| reveal | 220.0 | 335.6 | 555.6 | 0.4 | 0.8 | 1.2 | 2.0 | 1.607 | 1.0 | 7.76 | 10.34 |

## Notes

- GPU time is the command buffer's start-to-end execution time. It varies noticeably between runs; the reveal rows rest on about 20 frames each and should not be compared.
- In `damage` mode every frame still loads and stores the whole target surface. Rendering the damaged area into a small scratch texture and copying only that region is a possible further saving; it has not been tried.
- The real WindowServer still composites our output window in full each frame, so none of this reduces its work.
