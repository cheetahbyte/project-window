# Compositor benchmark

- Date: 2026-11-17 18:48:25 +0100
- Machine: Mac16,8, Apple M4 Pro
- macOS: 27.2.1
- Scene: desktop-builtin-2026-11-17.scene
- Display: 1800x1169 points, scale 2
- Windows: 199 (17 mapped)
- Client processes: 8
- Settle: 3 s, measure: 10 s

| State | Compositor footprint (MB) | Client footprint (MB) | Total footprint (MB) | Compositor CPU (%) | Client CPU (%) | Total CPU (%) | Frames/s |
|---|---:|---:|---:|---:|---:|---:|---:|
| idle | 118.1 | 2385.7 | 2503.8 | 0.0 | 0.0 | 0.0 | 0.0 |
| animate | 225.6 | 2385.9 | 2611.4 | 6.1 | 1.1 | 7.2 | 62.6 |
| move | 225.5 | 2385.9 | 2611.4 | 6.5 | 0.1 | 6.6 | 62.5 |

## Notes

Milestone 2 baseline: every window keeps a full buffer whether or not it is mapped, and any change redraws the whole frame.

- Client footprint is almost entirely window buffers. The scene's window sizes at scale 2 and 4 bytes per pixel add up to 2,358 MB for all 199 windows, of which the 17 mapped ones account for 317 MB.
- The compositor grows by about 107 MB once it starts presenting frames. One drawable at 3600 x 2338 pixels is about 34 MB, so this is consistent with a pool of three.
- Idle presents no frames and uses no measurable CPU in any process.
- CPU is user plus system time of our own processes. GPU time is not included, and the real WindowServer still composites our output window.

Not comparable one to one with the WindowServer profile (2,180 MB, external 5120 x 2880 display, 228 windows): this run used the built-in display, whose real scale is 1.68 rather than the 2 the scene assumes, and the clients are synthetic.
