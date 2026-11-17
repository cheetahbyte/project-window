# Spec 01: Standalone compositor

Status: approved
Date: 2026-11-17

## Goal

Build a compositor that runs as a normal macOS app and composites windows from our own test clients. Use it to find out how low memory and CPU can go for a desktop shaped like the one we profiled.

This spec covers the first stage only. It runs on top of the real WindowServer, needs no elevated rights, and changes nothing on the system.

## Baseline to beat

Measured on this machine (M4 Pro, one 5120 x 2880 display, macOS 27.2.1), WindowServer after almost 3 days:

| Measure | Value |
|---|---|
| Footprint | 2,180 MB (peak 3,949 MB) |
| Of which graphics buffers | about 1,610 MB |
| Of which small heap allocations | 259 MB |
| CPU, light activity | about 7% of one core in user space, 21% in Activity Monitor |
| Windows | 228 total, 20 on screen |

The profile points at two levers: which buffers are kept for windows nobody can see, and how much per-window work happens on each frame.

## Out of scope

- Running native macOS apps. That needs the private app protocol and comes in a later spec, tested in a virtual machine.
- Matching the macOS look (shadows, rounded corners, blur, animations). Next spec.
- Driving the physical display directly.
- Multiple displays, colour management, HDR, variable refresh.
- Automated tests. Verification is the benchmark in milestone 2.

## Architecture

C++23, built with CMake (Makefile generator; Ninja is not installed) with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`. `-DPW_SANITIZE=ON` adds AddressSanitizer and UndefinedBehaviorSanitizer; benchmarks use a build without them. AppKit and Metal are Objective-C interfaces, so the window and the renderer are Objective-C++ files; everything else is plain C++. No dependencies beyond the macOS SDK.

Three executables and two static libraries.

| Part | Role |
|---|---|
| `compositor` | The server. Owns one window with a Metal layer that stands in for the display. Holds the window list, receives client buffers, draws. |
| `protocol` (library) | Message layouts, validation, and the Mach transport: `bootstrap_register`, `bootstrap_look_up`, send and receive with a port descriptor. Shared by server and clients. |
| `clientkit` (library) | What a client links. Connects, creates windows, draws into a shared buffer, commits with a damage rectangle. |
| `demo-client` | Test client. Hosts one or many windows with static, animated, or idle content. |
| `bench` | Launches a scene, drives it, and records footprint and CPU. |

### Transport

Clients find the server by name through the per-user Mach bootstrap namespace and exchange Mach messages. Buffers are IOSurfaces, passed as Mach ports inside those messages.

Verified by a spike on this machine: a server registered a name with `bootstrap_register`, a second process looked it up, sent an IOSurface port, and the server read the pixel the client wrote. `bootstrap_register` is deprecated; it works here without a launchd job, which keeps the system untouched. If it stops working, the fallback is a launchd agent, which would need your permission.

### Protocol

Fixed-layout messages with a message identifier, one direction each.

The server treats every client message as untrusted: it checks the received size against the layout for that identifier, and checks window ids, frames, scale, and damage rectangles against limits before use. A message that fails a check is dropped and the connection is closed.

Client to server:

| Message | Payload |
|---|---|
| `hello` | reply port |
| `createWindow` | window id, frame in points, scale |
| `attachBuffer` | window id, IOSurface port |
| `commit` | window id, damage rectangle in pixels |
| `destroyWindow` | window id |
| `setMapped` | window id, mapped or not; an unmapped window keeps its buffer but is not drawn |
| `moveWindow` | window id, new origin |
| `sync` | serial |

Server to client:

| Message | Payload |
|---|---|
| `welcome` | port for this connection; the client sends everything after `hello` to it |
| `configure` | window id, new frame |
| `visibility` | window id, visible or hidden |
| `input` | window id, pointer event in window coordinates |
| `synced` | serial of the `sync` it answers, sent once everything before that `sync` is processed |

Window ids are chosen by the client and scoped to its connection, so creating a window needs no round trip.

### Rendering

- Metal, one textured quad per window, each IOSurface wrapped as a texture without copying.
- Shaders are compiled at build time with the `metal` tool into a library file placed next to the executable.
- Frames are drawn only when something changed. A static scene draws nothing.
- The display stand-in is a 2560 x 1440 point window at 2x, so pixel counts match the real display.

### Input

Enough to exercise the compositor: click raises a window, drag moves it, other pointer events are forwarded to the window under the cursor. No keyboard.

## Milestones

Each ends with something runnable and a number.

1. **Windows on screen.** Server, `clientkit`, and `demo-client` showing several windows that can be raised and dragged. Full-frame redraw on any change.
2. **Benchmark.** `bench` builds a scene from the captured desktop (228 windows with their real sizes, 20 visible), then records total footprint and CPU across the compositor and all clients in three states: idle, one window animating, one window being dragged. Results go to `docs/results/`.
3. **Buffer policy.** The server tells clients when a window becomes hidden or fully covered. The client releases that buffer and redraws when the window is revealed. Re-run the benchmark. The compositor's `--release never|unmapped|occluded` option selects which windows count as hidden, and the benchmark's `reveal` state measures how long a client takes to supply a buffer again.
4. **Damage and occlusion.** Redraw only the changed region, and skip windows fully covered by opaque ones. Re-run the benchmark.

## How results compare to the baseline

The comparison is indicative, not exact, for three reasons:

- Our output window is itself composited by the real WindowServer, so the real one keeps running and its cost does not go away at this stage.
- Our clients are synthetic. Real apps have layer trees, video, and effects that cost more.
- We count buffers in whichever process owns them and report the sum, so shifting memory from server to client is not counted as a saving.

## Risks

| Risk | Handling |
|---|---|
| Releasing hidden buffers makes revealing a window slow | Measure reveal latency in milestone 3; if poor, try keeping a downscaled or compressed copy |
| Partial redraw is awkward with a Metal layer, whose drawables rotate | Milestone 4 decides between a persistent scene texture and tracking drawable age, based on measurement |
| 228 windows in one client process hides per-process costs | `bench` spreads windows over several client processes |

## Layout

```
CMakeLists.txt
src/protocol/
src/clientkit/
src/compositor/
src/demo-client/
src/bench/
shaders/
docs/specs/
docs/results/
```
