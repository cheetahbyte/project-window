# Spec 02: The macOS look

Status: approved
Date: 2026-11-17

## Goal

Make windows in the standalone compositor look like macOS windows: transparency, rounded corners, shadows, and blurred backdrops, plus the open and close animation. Measure what each one costs, so that "looks the same, uses less" stays a number and not a claim.

This stage still runs as a normal app with our own test clients. It follows spec 01, whose four milestones are done.

## Why this before native apps

- It keeps us in the normal app, with no virtual machine and nothing changed on the system.
- Transparency makes the earlier numbers honest. Every window is opaque today, so the `occluded` release policy hides more than a real desktop would.
- Blur and shadows are where a compositor spends GPU time. The damage and buffer design from spec 01 has not been tested against them yet.

## Out of scope

- Native macOS apps and the private app protocol (later spec, in a virtual machine).
- Window contents: title bars, traffic lights, and toolbars are drawn by the app on macOS, so they are the client's job here as well. The demo client keeps its flat title strip.
- The Liquid Glass refraction and highlight effects. Blur here means blur, saturation, and tint.
- Minimise (genie), Mission Control, Spaces, full screen transitions: these belong to the Dock on macOS.
- The cursor, multiple displays, colour management, HDR.
- Arbitrary window shapes. Windows are rectangles or rounded rectangles.

## What is not known yet

These are measured in milestone 2, not assumed:

| Unknown | How it gets resolved |
|---|---|
| Shadow of a focused and an unfocused window: blur, offset, opacity | Capture a real window with its shadow (`screencapture -l` keeps the shadow with alpha) and fit our shadow to it |
| Corner radius of a standard window on macOS 27 | Measure from the same capture |
| Blur radius, saturation, and tint of a standard material | Capture a real window with a sidebar over a known backdrop and fit |
| Duration and curve of the open and close animation | Record the screen and measure frames |

## Design

### Transparency

- Buffers become premultiplied alpha. The compositor blends windows back to front where they are not opaque.
- New client message `setOpaqueRect` (window id, one rectangle in window points). It tells the server which part of the window is guaranteed opaque. Default: the whole window, as today. A translucent window sends an empty rectangle.
- Only a window's opaque part occludes what is under it. This feeds three things that exist already: which windows count as fully covered (buffer release), which pixels get drawn (culling), and the correctness check.
- Drawing order changes: opaque parts are still drawn only where visible; translucent parts are blended over whatever is beneath them, so damage under a translucent area redraws that area too.

### Rounded corners

- New client message `setCornerRadius` (window id, radius in points, limited to half the shorter side). The server masks the corners in the fragment shader with an antialiased edge, so clients do not have to draw corner alpha.
- The four corner squares stop counting as opaque.
- On macOS the app draws its own corner alpha. Doing it in the server is a deliberate difference: same picture, and the server knows the shape, which the shadow needs.

### Shadows

- The server draws the shadow, as on macOS. It is computed in the shader from the rounded rectangle's distance field, so there is no shadow texture to allocate, cache, or blur.
- Two parameter sets, focused and unfocused, fitted to the captures. The focused window is the one last clicked.
- A shadow reaches outside the window frame. Damage, visibility, and the display-edge clip use the frame grown by the shadow's extent. A shadow never occludes anything.
- Shadows can be switched off per window (`setShadow`), which menus and overlays need.

### Backdrop blur

- New client message `setBlurRect` (window id, one rectangle in window points, and a material: light or dark). One rectangle per window in this stage.
- The server renders what is beneath that rectangle, blurs it with a downsample and upsample chain, applies saturation and tint, and draws the window's translucent content over it.
- Damage rule: a change anywhere within the blur radius of a blur rectangle, on any window beneath it, damages the whole blurred rectangle.
- A window whose backdrop is not damaged reuses its last blurred result. This cache is the main lever for cost, and the benchmark measures it.

### Animation

- Mapping and unmapping a window run a short scale and fade, done by the server on the window's last buffer. The buffer of an unmapping window is kept until the animation ends, then released as now.
- While an animation runs the compositor draws every refresh; when none runs it goes back to drawing only on damage.

### Protocol and limits

All new messages are fixed-size and validated like the existing ones: rectangles inside the window, radius within its limit, material a known value. A violation closes the connection.

### Switching effects off

The compositor gets `--effects` with a list from `alpha`, `corners`, `shadows`, `blur`, `animation` (default: all). A feature that is off is ignored, not rejected, so the same clients run in every configuration and the benchmark can price each feature separately.

## Milestones

Each ends with something runnable, a number, and a commit.

1. **Transparency.** Premultiplied blending, `setOpaqueRect`, occlusion and culling by opaque parts. The demo client marks scene windows on a non-zero layer as translucent. Re-run the benchmark: this gives the honest figure for the `occluded` release policy.
2. **Measure macOS.** A small capture tool and a written record of the fitted values in the table above, with the captures they came from. No compositor changes.
3. **Corners and shadows.** `setCornerRadius`, `setShadow`, focus. Compare our window with the real capture: the measure is the mean absolute difference of the shadow and corner alpha, and the target is set once milestone 2 shows what the capture noise is.
4. **Backdrop blur.** `setBlurRect`, the blur chain, the damage rule, and the cache. Benchmark a scene with a blurred sidebar over a static backdrop and over an animating one.
5. **Animation.** Open and close, with the frame pacing change.
6. **Price list.** Benchmark the captured desktop with each effect switched on in turn, and record footprint, CPU, and GPU time per frame for each.

## Verification

- `--verify-redraw` stays the correctness check for every milestone: partial frames must match a full redraw exactly, including blended, masked, shadowed, and blurred pixels. It remains unreliable while window content animates.
- Sanitizer builds of every milestone run the small scene through the benchmark.
- Visual results are checked by capturing the compositor window and reading the image, and for corners and shadows by the difference measure in milestone 3.

## Conventions carried over

C++23, the strict warning flags, the sanitizer option, and the safe C++ rules used so far. Subagents do the parallel parts; each milestone is committed and pushed when it works.

## Risks

| Risk | Handling |
|---|---|
| Blending breaks the "draw each pixel once" property and GPU time rises | Milestone 1 measures it; opaque parts keep the culling path |
| The shader shadow does not match the real one closely enough | Milestone 3 decides by the difference measure; the fallback is a blurred shadow texture cached per window size |
| Backdrop blur damages large areas on small changes | The cache and the damage rule are measured in milestone 4 before anything is built on them |
| One rectangle per window is too little for real layouts | Accepted for this stage; a region message would need variable-size messages |
| Fitted values differ between macOS versions and appearance settings | The record in milestone 2 names the macOS version and appearance it was taken on |
