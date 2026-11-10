# Crysis-MTO

A CPU-multithreading mod for Crysis (2007), version 1.2.1 (build 6156, the GOG and Steam releases). The game runs its simulation, its scene traversal and every Direct3D call on one thread. This mod moves the renderer's work onto a thread of its own, spreads two of the main thread's jobs (the particle emitters' update and the characters' animation) over worker threads, and fixes two engine defects that cost frame rate.

It ships as an ASI plugin, `optiperf.asi`, loaded by [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader). No game file is modified.

In play (4 minutes in Recovery, the same route twice), against the same 64-bit game with only the allocator, which it needs to run at all: **188 fps on average instead of 117** (×1.6), the slowest 1 % of frames at 122 fps instead of 73, and the time under 100 fps down from 35 % to 0.7 %. The scripted benchmarks show less, ×1.45 on average over eight levels: play has more of the frames that the main thread holds back.

## What it does

| | Game builds | Effect (measured) |
|---|---|---|
| **Render thread** | 64-bit, DX10 | The renderer's main pass, the HUD and the present run on their own thread while the game works on its next frame. The opening dawn scene went from ~100 to 160–170 fps. |
| **Thread placement** | 64-bit, DX10 | The game's main thread and the render thread each get a physical core of their own, and the game's other threads are kept off those two. Left to Windows, both moved at most frames and often shared a core. +3 to +18 % fps on eight levels, about +9 % on average. |
| **Particle emitters on workers** | 64-bit, DX10 | The 3D engine's particle emitters are updated by worker threads and the main thread at once, instead of one after another on the main thread. +2 to +8 % fps on six levels, most on the effect-heavy ones (ice, ascension). |
| **Character animation on workers** | 64-bit, DX10 | The characters' poses (their forward kinematics) are made at once by worker threads at the end of the entity update, instead of in each character's own update. The game's logic reads each pose, and the movement its animation drives, one frame late; what is drawn is current, and the player's own character and weapons are unchanged. About 0.1–0.3 ms a frame in fights (estimated from a play session), less in the benchmarks. |
| **Particle fix** | all | Replaces a quadratic search in the renderer's particle batching. The ice benchmark: +27–31 % average fps, minimums 2.3–5.6× higher. |
| **Allocator** (mimalloc) | all | Removes the engine allocator's global lock, and makes the 64-bit game run (it crashed at level load). |

Measured on 8 cores of an i9-13900K (in a VM) with an RTX 4090, at 1080p and Very High settings. With the game held to fewer processors (village's benchmark, against the memory fix alone on the same processors): 4 cores with both their threads ×1.42, one thread on each of 4 cores ×1.32, 2 cores ×1.25.

## Status

Version 1.0.0: everything above is on by default in `optiperf.ini`, and each part can be turned off there on its own. The main thread's own work still sets the frame rate; what is left to move off it (the scene's objects and terrain, onto workers) is a bigger project, for later.

To measure the frame rate, set `[Debug] FrameLog=on` in `optiperf.ini`: a line a second goes to `optiperf_frames.csv`, and the average and the 1 % and 0.1 % lows to `optiperf.log`.

## Install

Take a release zip, or build them (below): `optiperf-<version>-with-asi-loader.zip`, or `optiperf-<version>-plugin-only.zip` if the game already has an ASI loader (from another mod). Then follow its `optiperf-README.txt` ([src/dist/README.txt](src/dist/README.txt)): copy its `Bin32` and `Bin64` folders into the Crysis folder.

## Build

Needs Visual Studio 2022 or later with the C++ x86/x64 tools (CMake and Ninja come with it). CMake fetches mimalloc and MinHook at pinned tags.

```powershell
.\build.ps1 -Arch x86,x64        # build\x86, build\x64: optiperf.asi, optiperf.ini, the dev launcher, the tests
.\package.ps1 -Version 1.0.0     # the Release build, packaged as dist\optiperf-1.0.0-with-asi-loader.zip and -plugin-only.zip
```

[src/README.md](src/README.md) describes the code, the settings and the tests.

## License

MIT (see [LICENSE](LICENSE)). The plugin contains mimalloc and MinHook, and the zip with the loader Ultimate ASI Loader, under their own licenses (see each zip's `optiperf-THIRD-PARTY-NOTICES.txt`).
