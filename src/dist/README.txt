Crysis MTO v@VERSION@
=====================

A CPU-multithreading mod for Crysis (2007). The game runs its simulation, its scene drawing and every Direct3D
call on one thread; this mod moves the renderer's work onto a thread of its own, updates the particle effects and
the characters' animation on worker threads, and fixes two engine defects that cost frame rate. It is an ASI
plugin (optiperf.asi); no game file is modified.

Supported game builds
---------------------
Crysis 1.2.1 (build 6156) for Windows: the 32-bit game (Bin32) and the 64-bit game (Bin64), DX9 and DX10. Played
and benchmarked with the GOG release; the Steam release has the same game files and was tested with benchmarks
(below). The 64-bit game needs Windows 10 or later. The release notes list the SHA-256 of the tested build's files
that the mod changes.

The Steam release comes with Crytek's original launchers instead of C1-Launcher, which the GOG release ships:
* With them, the ASI loader needs version.ini beside version.dll (in the archive with the loader since v1.0.3):
  without it, the loader never loads the plugin and there is no optiperf.log. With a loader of your own (Ultimate
  ASI Loader), set DontLoadFromDllMain=0 under [GlobalSets] in its ini.
* Steam's Play button asks which launch option to start: "Play Crysis" is the 32-bit game (Bin32\Crysis.exe),
  which gets the particle fix and the allocator only.
* The unnamed option below it is the 64-bit game, which crashed about 10 seconds after it started in our tests,
  with or without this mod, and the same with crysis64.exe started directly: a SecuROM check in the 64-bit
  CrySystem.dll fails and turns on the processor's floating-point exceptions. C1-Launcher
  (https://github.com/ccomrade/c1-launcher) skips that check: replace Bin32\Crysis.exe and Bin64\Crysis.exe with
  its two files, then start Bin64\Crysis.exe. That is the GOG release's setup, and the whole mod works with it.

CryMP's client (the community's multiplayer client, https://github.com/crymp-net/crymp-client; v27 tested)
works from v1.0.2: install the mod the same way and start CryMP-Client64.exe. Everything works as with the game's
own executable, except the particle emitters' worker threads, which stay off by themselves (CryMP brings its own
physics and sound). CryMP-Client32.exe gets the particle fix and the allocator, as the 32-bit game does. Versions
before 1.0.2 can crash under it with the render thread on.

There are no version or hash checks. Before changing anything, each part of the mod finds the game code it changes
by its byte pattern and checks it. If something differs (another mod changed it, a different game version), that
part stays off, optiperf.log says why, and the game runs as usual. Mods that replace CryGame.dll (most
single-player mods) haven't been tested with this version.

Choose an installation
----------------------
Archive with Ultimate ASI Loader:
  If the game has no ASI loader, extract the archive into the Crysis folder (the one that contains Bin32, Bin64
  and Game), or copy its Bin32 and Bin64 folders there. Each of the two gets version.dll, version.ini,
  optiperf.asi and optiperf.ini; nothing is overwritten. The bundled version.dll is Ultimate ASI Loader
  @UAL_VERSION@ (x86 in Bin32, x64 in Bin64); version.ini holds its one setting (see the Steam release above).

  If the game already has an ASI loader, do not overwrite it: copy only optiperf.asi and optiperf.ini into Bin32
  and Bin64. If another mod's version.dll is not an ASI loader, rename this one to winmm.dll instead, and
  version.ini to winmm.ini (both names work).

Plugin-only archive:
  Requires an ASI loader already installed in Bin32 and Bin64 (for example Ultimate ASI Loader as version.dll or
  winmm.dll, from another mod). Extract the archive into the Crysis folder, or copy optiperf.asi and optiperf.ini
  into Bin32 and Bin64. Keep each folder's two files together.

Other ASI plugins can share the loader: they go next to optiperf.asi.

For either installation:
1. Close Crysis before copying files.
2. Start the game as usual (GOG Galaxy, Steam or Crysis.exe, or CryMP's CryMP-Client64.exe). The render thread and
   the worker threads need the 64-bit game with DX10. GOG Galaxy's Play button starts the 32-bit DX9 game: choose
   "Crysis x64 DX10" from its launch options instead. On Steam, "Play Crysis" starts the 32-bit game too; the
   Steam release's 64-bit game needs C1-Launcher (see Supported game builds). The 32-bit game and DX9 get the
   particle fix and the allocator only.

Usage
-----
The supplied configuration (optiperf.ini, in Bin32 and in Bin64) turns everything on; no key press is needed:
* the render thread: the renderer's main pass, the HUD and the frame's present run on a thread of their own while
  the game already works on its next frame;
* the game's main thread and the render thread on a physical core each, with the game's other threads kept off
  those two (with four cores or more; with fewer it stays off by itself);
* the particle emitters' update and the characters' animation on worker threads;
* the particle fix, which removes a slow search the renderer runs for every particle effect on screen (smoke,
  snow, explosions);
* the allocator (mimalloc), which removes the engine allocator's global lock and makes the 64-bit game run:
  unpatched, it crashes when a level loads.

In a 4-minute play session in Recovery (the same route twice; 64-bit DX10, 1080p Very High, an 8-core i9-13900K,
an RTX 4090), against the same game with only the allocator: 117 fps on average before, 188 fps after, and the
slowest 1% of frames at 122 fps instead of 73.

Each part can be turned off in optiperf.ini: [ParticleBatch] Mode=off; [Allocator] Mode=off; the render thread
with [ShadowPrepass] FlushThread=main, or everything it prepares with [ShadowPrepass] Mode=off; the two threads'
own cores with [ShadowPrepass] FlushThreadPlacement=default; the worker threads with [ParticleUpdate] Mode=off and
[AnimUpdate] Mode=off.

To measure the frame rate, set [Debug] FrameLog=on: a line a second goes to optiperf_frames.csv next to the log
(the frames, the longest one, the frames paused in a menu), and optiperf.log gets the average, the 1% and 0.1%
lows and the longest frame every 10 seconds and when the game ends. Menus and loading screens aren't counted.

Safety and scope
----------------
The visuals are the game's own, and the game's logic runs as in the game, with one exception: with the animation
on worker threads, the game's logic reads each character's pose, and the movement its animation drives, as they
were one frame earlier. What is drawn is current, your own character and weapons are not affected, and it wasn't
noticeable in play testing; [AnimUpdate] Mode=off puts back the game's own order. Everything else only changes how
fast the game's own code runs, and on which thread.

Diagnostics
-----------
The plugin writes optiperf.log beside optiperf.asi: what was patched, why a part stayed off, and, if the game
crashes or shows an error, where (with the engine's call chain). Check it if the mod doesn't seem to work; if
there is no optiperf.log at all, the loader didn't load the plugin (with the Steam release: version.ini). With
any game it should say "particle batch fix installed" and "allocator replaced by mimalloc"; with the 64-bit game
in DX10 also "flush thread: overlap" and, once a level is running, "the outer flush runs on a worker", "the main
thread on core", "particle update: mode=on" and "anim update: mode=on". Under CryMP's client the allocator's line
reads "its six replaced by mimalloc" instead, and the particle update says later that it is off ("physics or sound
comes from another module than the game's own").

Uninstallation
--------------
Close the game and remove optiperf.asi and optiperf.ini from Bin32 and Bin64. The optiperf.log diagnostic file
(and optiperf_frames.csv, if you turned the frame log on) may also be removed.

Do not remove version.dll if it existed before this mod or if another plugin depends on it. Remove the bundled
loader (version.dll and version.ini) only when you installed that exact file for this mod and no other ASI plugin
needs it.

Third-party components
----------------------
optiperf.asi contains mimalloc (by Microsoft) and MinHook (by Tsuda Kageyu), both under the MIT License. The
explicitly named archive with the loader also includes Ultimate ASI Loader @UAL_VERSION@ by ThirteenAG under the
MIT License. See THIRD-PARTY-NOTICES.txt.

Misc
----
For more game fixes, visit:
https://slop-blog.enkhayzomachines.net/fixes :)
