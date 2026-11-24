# Crysis MTO

> [!WARNING]
> This mod has been pretty much entirely generated using AI;
> I won't and will never claim to have enough knowledge or expertise to do this
> kind of reverse-engineering by myself; I've played from the first level through
> Assault with it (5-6 levels) and so far spotted no major issues (on my machine ofc, if you
> have issues feel free to open an Issue).
> I've only tested it with the GOG version of the game so far (2.0.0.7), so no
> guarantees for other exes for now.
> If you're interested and want more (human generated) info, I have a blog post [here](https://slop-blog.enkhayzomachines.net/posts/crysis-multithreaded-overhaul) :)

A CPU-multithreading mod for Crysis (2007). The game runs its simulation, its scene traversal and every Direct3D call on one thread. This mod moves the renderer's work onto a thread of its own, spreads two of the main thread's jobs (the particle emitters' update and the characters' animation) over worker threads, and fixes two engine defects that cost frame rate. It ships as an ASI plugin, `optiperf.asi`, loaded by [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader); no game file is modified.

In play (4 minutes in Recovery, the same route twice), against the same 64-bit game with only the allocator, which it needs to run at all: **188 fps on average instead of 117** (×1.6), the slowest 1 % of frames at 122 fps instead of 73, and the time under 100 fps down from 35 % to 0.7 %. The scripted benchmarks show less, ×1.45 on average over eight levels: play has more of the frames that the main thread holds back.

Current release: **v1.0.0**

## Compatibility

Crysis 1.2.1 (build 6156) for Windows: the 32-bit game (`Bin32`) and the 64-bit game (`Bin64`), DX9 and DX10. The render thread and the worker threads need the 64-bit game with DX10; the 32-bit game and DX9 get the particle fix and the allocator. The 64-bit game needs Windows 10 or later.

| Release | Status |
| --- | --- |
| GOG | Reference build: benchmarked on eight levels, played from the first level through Assault |
| Steam | Same version number; not tested yet |

There are no version or hash checks. Before installing a hook, each part finds the game code it changes by its byte pattern and verifies it: offsets read from the matched code must agree wherever they appear, and the functions it calls are matched by their own patterns. If anything differs, only that part stays off, `optiperf.log` says why, and the game runs as usual. The release notes list the SHA-256 of the reference build's files that the supplied settings change. Mods that replace `CryGame.dll` (most single-player mods) haven't been tested with this version.

## What it does

| | Game builds | Effect (measured) |
|---|---|---|
| **Render thread** | 64-bit, DX10 | The renderer's main pass, the HUD and the present run on their own thread while the game works on its next frame. The opening dawn scene went from ~100 to 160–170 fps. |
| **Thread placement** | 64-bit, DX10 | The game's main thread and the render thread each get a physical core of their own, and the game's other threads are kept off those two. Left to Windows, both moved at most frames and often shared a core. +3 to +18 % fps on eight levels, about +9 % on average. |
| **Particle emitters on workers** | 64-bit, DX10 | The 3D engine's particle emitters are updated by worker threads and the main thread at once, instead of one after another on the main thread. +2 to +8 % fps on six levels, most on the effect-heavy ones (ice, ascension). |
| **Character animation on workers** | 64-bit, DX10 | The characters' poses (their forward kinematics) are made at once by worker threads at the end of the entity update, instead of in each character's own update. The game's logic reads each pose, and the movement its animation drives, one frame late; what is drawn is current, and the player's own character and weapons are unchanged. About 0.1–0.3 ms a frame in fights (estimated from a play session), less in the benchmarks. |
| **Particle fix** | all | Replaces a quadratic search in the renderer's particle batching. The ice benchmark: +27–31 % average fps, minimums 2.3–5.6× higher. |
| **Allocator** (mimalloc) | all | Removes the engine allocator's global lock, and makes the 64-bit game run (it crashed at level load). |

Measured on 8 cores of an i9-13900K (in a VM) with an RTX 4090, at 1080p and Very High settings. With the game held to fewer processors (village's benchmark, against the allocator alone on the same processors): 4 cores with both their threads ×1.42, one thread on each of 4 cores ×1.32, 2 cores ×1.25.

## Current status

Everything above is on by default in the supplied `optiperf.ini`, and each part can be turned off there on its own. The release package passed 15 timedemo launches through Ultimate ASI Loader (both games, DX9 and DX10, the frame rate display on, fewer processors) and a play-through up to Assault.

One behaviour differs from the game's own: with the animation on worker threads, the game's logic reads each character's pose one frame late. It wasn't noticeable in play; `[AnimUpdate] Mode=off` restores the game's order.

The main thread's own work still sets the frame rate. What is left to move off it (the scene's objects and terrain, onto workers) is a bigger project, for later.

## Build

Needs Visual Studio 2022 or later with the C++ x86/x64 tools (CMake and Ninja come with it). CMake fetches mimalloc and MinHook at pinned tags.

```powershell
.\build.ps1 -Arch x86,x64        # build\x86, build\x64: optiperf.asi, optiperf.ini, the dev launcher, the tests
```

[src/README.md](src/README.md) describes the code, the settings and the tests.

## Package

After building and testing the plugin, create the two release archives with:

```powershell
.\package.ps1 -Version 1.0.0
```

The script builds the Release binaries (`-SkipBuild` packages those already in `build\release`), downloads Ultimate ASI Loader v9.7.4 and checks its pinned hashes, and writes to `dist\` the plugin-only archive and the archive with the loader, each with payload hashes (`SHA256SUMS.txt`) and a `.sha256` file beside it. The loader's notice goes only into its archive; the notices of mimalloc and MinHook, which are linked into the plugin, into both. It refuses to overwrite an existing release archive.

## Install

1. Close Crysis.
2. Take the archive with the loader (`Crysis-MTO-v1.0.0-with-Ultimate-ASI-Loader-v9.7.4.zip`), unless the game already has an ASI loader in `Bin32` and `Bin64`: then the plugin-only one (`Crysis-MTO-v1.0.0-plugin-only.zip`).
3. Extract it into the Crysis folder (the one that contains `Bin32`, `Bin64` and `Game`). `Bin32` and `Bin64` each get `optiperf.asi` and `optiperf.ini`, and from the archive with the loader `version.dll`. If the game already has a loader, don't overwrite it: copy only `optiperf.asi` and `optiperf.ini`. If another mod's `version.dll` isn't an ASI loader, rename this one to `winmm.dll` (both names work).
4. Start the game as usual. The render thread and the worker threads need the 64-bit game with DX10: GOG Galaxy's Play button starts the 32-bit DX9 game, so choose "Crysis x64 DX10" from its launch options.

The plugin writes `optiperf.log` beside `optiperf.asi`: what was patched, why a part stayed off, and, if the game crashes or shows an error, where.

## Configuration

Edit `optiperf.ini` (in `Bin32` and in `Bin64`) while the game is closed. The supplied one turns everything on; without a line, a setting is off.

- `[ParticleBatch] Mode=off`, `[Allocator] Mode=off`: the two engine fixes off.
- `[ShadowPrepass] FlushThread=main`: the render thread off (what prepares the renderer's work stays on); `[ShadowPrepass] Mode=off` leaves the renderer alone entirely.
- `[ShadowPrepass] FlushThreadPlacement=default`: Windows places the main thread and the render thread.
- `[ParticleUpdate] Mode=off`, `[AnimUpdate] Mode=off`: the particle emitters and the animation back on the main thread.
- `[Debug] FrameLog=on`: the frame rate, a line a second in `optiperf_frames.csv`, and the average and the 1 % and 0.1 % lows in `optiperf.log`.

[src/README.md](src/README.md) lists every setting.

## Uninstall

Close the game and remove these files from `Bin32` and `Bin64`:

- `optiperf.asi`
- `optiperf.ini`
- `optiperf.log` (and `optiperf_frames.csv`, if the frame log was on)

Do not remove `version.dll` if it existed before this mod or if another ASI plugin uses it.

## Technical outline

The mod is an ASI plugin for both games. On the 64-bit game with DX10 it runs the renderer's main pass, the HUD and the present on a thread of its own: before the hand-off it prepares, copies or records everything the renderer would otherwise ask the rest of the engine for while drawing, and the game's next frame goes on beside it, its calls into the renderer taken, answered or made to wait. [src/README.md](src/README.md) describes each part, its settings and its tests.

The repository contains the mod's source and its build and package scripts. Release archives contain only the built plugin, its configuration, documentation, hashes, notices and, only in the explicitly named archive, the third-party ASI loader. No Crysis file is distributed.

## License

MIT (see [LICENSE](LICENSE)). The plugin contains mimalloc and MinHook, and the archive with the loader Ultimate ASI Loader, under their own licenses (see each archive's `THIRD-PARTY-NOTICES.txt`).
