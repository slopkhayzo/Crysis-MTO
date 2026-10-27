# Crysis-MTO

A CPU-multithreading mod for Crysis (2007), version 1.2.1 (build 6156, the GOG and Steam releases). The game runs its simulation, its scene traversal and every Direct3D call on one thread. This mod moves the renderer's work onto a thread of its own, and fixes two engine defects that cost frame rate.

It ships as an ASI plugin, `optiperf.asi`, loaded by [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader). No game file is modified.

## What it does

| | Game builds | Effect (measured) |
|---|---|---|
| **Render thread** | 64-bit, DX10 | The renderer's main pass, the HUD and the present run on their own thread while the game works on its next frame. The opening dawn scene went from ~100 to 160–170 fps. |
| **Particle fix** | all | Replaces a quadratic search in the renderer's particle batching. The ice benchmark: +27–31 % average fps, minimums 2.3–5.6× higher. |
| **Allocator** (mimalloc) | all | Removes the engine allocator's global lock, and makes the 64-bit game run (it crashed at level load). |

Measured on 8 cores of an i9-13900K (in a VM) with an RTX 4090, at 1080p and Very High settings.

## Status

The engine fixes and the render thread are done. The render thread is on by default in `optiperf.ini`. Next is the main thread's own work (the scene traversal, the game's update), which now sets the frame rate.

## Install

Take a release zip, or build one (below). Then follow its `optiperf-README.txt` ([src/dist/README.txt](src/dist/README.txt)): copy its `Bin32` and `Bin64` folders into the Crysis folder.

## Build

Needs Visual Studio 2022 or later with the C++ x86/x64 tools (CMake and Ninja come with it). CMake fetches mimalloc and MinHook at pinned tags.

```powershell
.\build.ps1 -Arch x86,x64        # build\x86, build\x64: optiperf.asi, optiperf.ini, the dev launcher, the tests
.\package.ps1 -Version 0.4.0     # the Release build, packaged as dist\optiperf-0.4.0.zip
```

[src/README.md](src/README.md) describes the code, the settings and the tests.

## License

MIT (see [LICENSE](LICENSE)). The package also contains Ultimate ASI Loader, mimalloc and MinHook, under their own licenses (see its `optiperf-THIRD-PARTY-NOTICES.txt`).
