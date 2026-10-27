optiperf - performance fixes for Crysis (2007)
===============================================

For Crysis 1.2.1 (the GOG and Steam releases). Works with the 32-bit and the 64-bit game, DX9 and DX10.


WHAT IT DOES

* Particle fix: removes a very slow search the game runs for every particle effect on screen. In snow, smoke
  and explosion-heavy scenes this was costing up to half of every frame. Measured in the built-in ice benchmark
  (Benchmark_CPU2): about +30% average fps, and the worst moments are 2-3x faster (minimums went from ~20 fps
  to ~65 fps). Scenes without many particles are unchanged.

* New memory allocator (mimalloc) for the engine. This makes the 64-bit version playable: unpatched, it crashes
  as soon as a level loads on current Windows and drivers. It also prepares the engine for multithreading work.

* Render thread (64-bit game with DX10 only): the renderer's main pass, then the HUD and the frame's present, run on
  a thread of their own while the game already works on its next frame beside them. In play this took the opening
  dawn scene from ~100 fps to 160-170 fps, with later scenes up to ~300 fps. On the 32-bit game or DX9 it stays off
  by itself, and the other fixes still apply.

* A log file (optiperf.log) that records what was patched and, if the game crashes or shows an error, where.

Nothing in the game itself changes: same visuals, same behaviour. The fixes only change how fast the game's own
code runs, and on which thread.


INSTALL

1. Open your Crysis folder (the one that contains Bin32, Bin64 and Game).
2. Copy the Bin32 and Bin64 folders from this package into it. Three new files are added to each folder:
     version.dll   (Ultimate ASI Loader - it loads the plugin)
     optiperf.asi  (the fixes)
     optiperf.ini  (settings)
   Nothing is overwritten.
3. Start the game as usual (GOG Galaxy, Steam or Crysis.exe). 32-bit or 64-bit, DX9 or DX10 all work; the
   render thread needs the 64-bit game (Bin64\Crysis.exe) with DX10, the faster renderer on modern PCs.
   GOG Galaxy's Play button starts the 32-bit DX9 game: for the render thread, choose "Crysis x64 DX10" from
   its launch options instead.

To check it works, look at Bin32\optiperf.log (or Bin64\optiperf.log) after starting the game: it should say
"particle batch fix installed" and "allocator replaced by mimalloc"; with the 64-bit game in DX10, also
"flush thread: overlap" and, once a level is running, "the outer flush runs on a worker".


UNINSTALL

Delete version.dll, optiperf.asi, optiperf.ini and optiperf.log from Bin32 and Bin64.


SETTINGS (optiperf.ini)

Each fix can be turned off on its own ([ParticleBatch] Mode=off, [Allocator] Mode=off; the render thread with
[ShadowPrepass] FlushThread=main, or everything it prepares with [ShadowPrepass] Mode=off). Every fix first checks
that the game code it changes is exactly what it expects; if not (another mod changed it, a different game
version), that fix stays off and optiperf.log says why. The game itself always runs.


COMPATIBILITY

* Another mod already uses version.dll: rename this package's version.dll to winmm.dll (both names work).
* Other ASI plugins can share the same loader: put them next to optiperf.asi.
* Mods that replace CryGame.dll (most single-player mods) are fine.
* The 64-bit fix needs Windows 10 or later.


CREDITS

Ultimate ASI Loader by ThirteenAG, mimalloc by Microsoft, MinHook by Tsuda Kageyu - see
optiperf-THIRD-PARTY-NOTICES.txt.
