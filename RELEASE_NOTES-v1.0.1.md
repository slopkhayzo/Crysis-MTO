# Crysis MTO v1.0.1

## What changed

Since v1.0.0, three fixes to the render thread (the 64-bit game with DX10).
Each was in every release with the render thread on, since v0.3.0:

- The sun's shadow maps are redrawn as often as in the original game. Up to
  half of them weren't: the next frame's scene, built beside the render
  thread, asked for a map again with the same value the render thread was
  about to clear, and the request was lost. The shadows of moving vehicles and
  characters were a frame behind every other frame. Drawing those maps again
  costs some of v1.0.0's speed, which came partly from skipping them: in
  back-to-back benchmark runs about 4 % on island and 8-13 % on village (the
  render thread draws the maps, and the main thread prepares their casters,
  about 0.2 ms a frame).
- Plants no longer flicker. The render objects of the next frame's scene,
  made beside the render thread, started from one template in the fields the
  game doesn't reset, instead of from the renderer's object in their place. A
  plant whose bending had faded took another object's bend in some frames,
  and thin parts of plants appeared and went.
- Distant bushes and palms drawn as billboards no longer lose their sun shadow
  in single frames, which showed most near sunrise and sunset. The part that
  gives the render thread the game's console variables as they were when the
  frame was handed over stayed off whenever the render thread was on, so the
  render thread read them live. The 3D engine sets `e_shadows` to 0 for a
  moment before it builds each frame's scene, and billboards drawn in that
  moment got no shadow.

`optiperf.ini` is the same as v1.0.0's: one kept from v1.0.0 works as it is.

## Release archives

- `Crysis-MTO-v1.0.1-plugin-only.zip` contains the plugin and its
  configuration for both games (`Bin32`, `Bin64`), the project license,
  documentation, the notices of the libraries linked into the plugin, and
  payload hashes, for installations that already have an ASI loader.
  SHA-256:
  `C83C136EA10A4B41868E50CED6AD42D124A519E10CAAEB9DB86351B4394DF227`
- `Crysis-MTO-v1.0.1-with-Ultimate-ASI-Loader-v9.7.4.zip` additionally
  contains the pinned loader for both games (as `version.dll`) and its MIT
  notice.
  SHA-256:
  `77C8EDA2E043F9A2FC6DCE7FFC4877307F88BB7A2916A1F8EAF313BBF72ACBED`

## Validation

- Reference build: the GOG release of Crysis 1.2.1 (build 6156), as for
  v1.0.0. The SHA-256 of its files whose code or tables the supplied settings
  change:

  | File | SHA-256 |
  | --- | --- |
  | `Bin32\CrySystem.dll` | `89872EF3D86DEBD8A189EF703150EF775ED45CA16041FF8DF76734B7AE9C29C8` |
  | `Bin32\CryRenderD3D9.dll` | `C162697AC29EB82C1813F5F611C16F34E059AB71E4F2071368A0F60FD5AE784C` |
  | `Bin32\CryRenderD3D10.dll` | `1E0FC6E60FEB394EEB0EA5E4221227B539FC712A78A5B7B575640BBFF4C316C2` |
  | `Bin64\CrySystem.dll` | `F54900B261705ECC27F25F9310ADD669599A3AD2AD722919472DA45B6A621DAB` |
  | `Bin64\CryRenderD3D9.dll` | `008337B240C12EB6A15D78CF539FDEDC422915BA41BEC9146B1F53E77D138C40` |
  | `Bin64\CryRenderD3D10.dll` | `5BF35593A54DD77D9EB2397F9F98BABD63084F2AAFDD40A4D4C8BC297B19D9EA` |
  | `Bin64\Cry3DEngine.dll` | `AFA321AA307535149BBC08ABF8814C826DD720069DAD6D81837005BB1A84EC60` |
  | `Bin64\CryAnimation.dll` | `017D85E22FD1D25CA4F423695F8233C089CE81B8CE003C85732BCCD5917B1057` |
  | `Bin64\CryEntitySystem.dll` | `83B113206B917E4F1D064F9B994C42C25F16DC018F3CD596BDF4BE0D604AD1BD` |
  | `Bin64\CryGame.dll` | `7323A5DFD7147A40F3E359D007FD5B4537BB8054CF4AD314D234E83CF1C0818A` |
  | `Bin64\CryPhysics.dll` | `6BD9450999795E43917114F7BB38B3C7D82DF4643CB87FB7B57AD28B23D1F13E` |
  | `Bin64\CrySoundSystem.dll` | `ABE76E9FABA8212A9AB93FAC3F7FB772AEFAD82318B90DB9490E891D53B1AD21` |

  These identify the tested build; they are not checked. The plugin finds the
  code it changes by byte pattern and leaves off any part whose code differs.
- `optiperf.asi` SHA-256:
  - `Bin32`: `850A08094F65256573E9F36DD89A758AB9757C0249C0458B0BB70B9AD6264DA4`
  - `Bin64`: `864103F126DDCAE55A5C70021094749502D1E3E8DE074715AF92509FBBA5E3D5`
- Ultimate ASI Loader v9.7.4 SHA-256 (the release's `dinput8.dll`, bundled as
  `version.dll`):
  - x86: `EC2F4824ECA58DD40F425756A4A7CEC77B8E381F21D11D7C846EC4B339B617AB`
  - x64: `031A3E5576D91DCE1E438D36B9A3D462C7334AB4791990A8FF1E3DDC0E132DAF`
- Compilation with warnings enabled, and the allocator, call tracer and shadow
  pre-pass tests passed.
- The fixes, in timedemos with the HUD on (64-bit DX10):
  - shadow maps: on eight levels the render thread redraws the same maps as
    the game in its own order (island: 15,064 of 18,000, against 8,050 with
    v1.0.0), and every prediction of which maps it redraws held;
  - plants: of the vegetation's render objects made beside the render thread
    on island, those left with another object's bend went from 7,998 to 345,
    where the camera's movement changed them;
  - billboards: a still timedemo recorded where they strobed (Contact's beach)
    and replayed at 6:20 in the morning with the time of day held: 4.5-13.9 %
    of the frames had the hillside's billboards brighter before, none after,
    and the hillside matched the render thread turned off frame by frame.
- The release package was installed into a fresh copy of the game's binaries
  and started through Ultimate ASI Loader, without the development launcher,
  in 15 timedemo launches with the HUD on, none with an exception:
  - the 32-bit game (DX9 and DX10) and the 64-bit game with DX9 kept the render
    thread and the worker threads off by themselves, with the particle fix and
    the allocator on;
  - the 64-bit game with DX10 ran the render thread, the two threads' cores and
    both worker parts on island, village, ice, harbor and ascension, and with
    the game's frame rate display on (`r_DisplayInfo 1`); every answer the
    render thread predicts was exact;
  - with the game limited to fewer processors (village's benchmark, against
    the allocator alone on the same processors, with 0.5-1 core of other load
    on the machine): 4 cores with both their threads 140 against 108 fps
    (x1.30), one thread on each of 4 cores 134 against 108 (x1.24), 2 cores 122
    against 101 (x1.21). The thread placement stayed off by itself in the last
    two.
- Against v1.0.0, the two releases' plugins alternated in the same
  installation (v1.0.0, v1.0.1, v1.0.1, v1.0.0; two passes each, 0.6-0.9 core
  of other load): island 203 against 212 fps (-4.6 %), village 124 against
  142 (-13 %; -8 % leaving out the run a compiler ran beside). v1.0.1 redraws
  the shadow maps v1.0.0 skipped: island 15,064 of 18,000 maps against 7,719
  and 7,845, village 20,288 of 24,510 against 16,372 and 16,981.
- Play sessions in Contact (64-bit DX10, 1080p Very High, 8 cores of an
  i9-13900K, an RTX 4090), with each fix as it came: the vehicles' shadows,
  the plants and the distant billboards no longer flickered.
- The two archives contain the same `optiperf.asi`, `optiperf.ini` and loader
  files, byte for byte, as the package these tests ran.
