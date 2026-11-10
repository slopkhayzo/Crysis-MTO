optiperf - performance fixes for Crysis (2007)
===============================================

Version 1.0.0. For Crysis 1.2.1 (the GOG and Steam releases). Works with the 32-bit and the 64-bit game, DX9 and
DX10; the biggest gains need the 64-bit game with DX10.


WHAT IT DOES

* Particle fix: removes a very slow search the game runs for every particle effect on screen. In snow, smoke
  and explosion-heavy scenes this was costing up to half of every frame. Measured in the built-in ice benchmark
  (Benchmark_CPU2): about +30% average fps, and the worst moments are 2-3x faster (minimums went from ~20 fps
  to ~65 fps). Scenes without many particles are unchanged.

* New memory allocator (mimalloc) for the engine. This makes the 64-bit version playable: unpatched, it crashes
  as soon as a level loads on current Windows and drivers. It also prepares the engine for multithreading work.

* Render thread (64-bit game with DX10 only): the renderer's main pass, then the HUD and the frame's present, run on
  a thread of their own while the game already works on its next frame beside them. In play this took the opening
  dawn scene from ~100 fps to 160-170 fps, with later scenes up to ~300 fps. With four CPU cores or more, the game's
  main thread and the render thread also get a core each, and the game's other threads are kept off those two:
  a further 3-18% fps in the benchmarks. On the 32-bit game or DX9 it stays off by itself, and the other fixes still
  apply.

* Particle effects and character animation on several threads (64-bit game with DX10 only): the game updates its
  particle emitters, and poses its characters, one after another on its main thread. Both are now spread over
  worker threads and the main thread at once. The particle update gains most in effect-heavy scenes (+4 to +8% in
  the ice, harbor and ascension benchmarks), the animation in fights, where many characters are close.

* A log file (optiperf.log) that records what was patched and, if the game crashes or shows an error, where.

All of it together, in a 4-minute play session in Recovery (the same route twice; 64-bit DX10, 1080p Very High,
an 8-core i9-13900K, an RTX 4090), against the same game with only the new allocator (the 64-bit game doesn't run
without it): 117 fps on average before, 188 fps after. The slowest 1% of frames went from 73 to 122 fps, and the
time spent under 100 fps from a third of the session to almost none.

The game itself doesn't change: same visuals, same behaviour, with one small exception. With the animation on
several threads, the game's logic (not what you see) reads each character's pose, and the movement its animation
drives, as they were one frame earlier. Your own character and weapons are not affected, and it wasn't noticeable
in play testing; [AnimUpdate] Mode=off puts back the game's own order. Everything else only changes how fast the
game's own code runs, and on which thread.


INSTALL

There are two zips with the same plugin. Take the one "with-asi-loader", unless your game already has an ASI
loader in Bin32 and Bin64 (for example Ultimate ASI Loader as version.dll or winmm.dll, from another mod): then
take the "plugin-only" one, which leaves the loader out. Without a loader the plugin isn't loaded, and nothing
changes.

1. Open your Crysis folder (the one that contains Bin32, Bin64 and Game).
2. Copy the Bin32 and Bin64 folders from this package into it. New files in each folder:
     version.dll   (Ultimate ASI Loader - it loads the plugin; only in the zip with the loader)
     optiperf.asi  (the fixes)
     optiperf.ini  (settings)
   Nothing is overwritten.
3. Start the game as usual (GOG Galaxy, Steam or Crysis.exe). 32-bit or 64-bit, DX9 or DX10 all work; the
   render thread needs the 64-bit game (Bin64\Crysis.exe) with DX10, the faster renderer on modern PCs.
   GOG Galaxy's Play button starts the 32-bit DX9 game: for the render thread, choose "Crysis x64 DX10" from
   its launch options instead.

To check it works, look at Bin32\optiperf.log (or Bin64\optiperf.log) after starting the game: it should say
"particle batch fix installed" and "allocator replaced by mimalloc"; with the 64-bit game in DX10, also
"flush thread: overlap" and, once a level is running, "the outer flush runs on a worker", (the placement)
"the main thread on core", "particle update: mode=on" and "anim update: mode=on".


UNINSTALL

Delete optiperf.asi, optiperf.ini and optiperf.log from Bin32 and Bin64, and version.dll if it came with this
package (and optiperf_frames.csv, if you turned the frame log on).


SETTINGS (optiperf.ini)

Each fix can be turned off on its own ([ParticleBatch] Mode=off, [Allocator] Mode=off; the render thread with
[ShadowPrepass] FlushThread=main, or everything it prepares with [ShadowPrepass] Mode=off; the two threads' own
cores with [ShadowPrepass] FlushThreadPlacement=default; the particle emitters' and the animation's worker threads
with [ParticleUpdate] Mode=off and [AnimUpdate] Mode=off). Every fix first checks that the game code it changes is
exactly what it expects; if not (another mod changed it, a different game version), that fix stays off and
optiperf.log says why. The game itself always runs.

To measure the frame rate, set [Debug] FrameLog=on: a line a second goes to optiperf_frames.csv next to the log
(the frames, the longest one, the frames paused in a menu), and optiperf.log gets the average, the 1% and 0.1%
lows and the longest frame every 10 seconds and when the game ends. Menus and loading screens aren't counted.


COMPATIBILITY

* Another mod already uses version.dll: if that is Ultimate ASI Loader, take the plugin-only zip; if not, rename
  this package's version.dll to winmm.dll (both names work).
* Other ASI plugins can share the same loader: put them next to optiperf.asi.
* Mods that replace CryGame.dll (most single-player mods) are fine.
* The 64-bit fix needs Windows 10 or later.


CREDITS

Ultimate ASI Loader by ThirteenAG (in the zip with the loader), mimalloc by Microsoft, MinHook by Tsuda Kageyu -
see optiperf-THIRD-PARTY-NOTICES.txt.
