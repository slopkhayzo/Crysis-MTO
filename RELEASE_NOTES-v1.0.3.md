# Crysis MTO v1.0.3

## What changed

Since v1.0.2, the mod loads with the Steam release of Crysis. Its game files
are the GOG release's, byte for byte, but it comes with Crytek's original
launchers instead of C1-Launcher, and with those Ultimate ASI Loader never
loaded the plugin: the game ran as it is, with no `optiperf.log`.

- The loader comes into the game with `CrySystem.dll`, which Crytek's
  launchers load only inside the game's startup, and by default it waits for
  a call from the launcher that never comes. The archive with the loader now
  has `version.ini` beside `version.dll` in `Bin32` and `Bin64`
  (`[GlobalSets] DontLoadFromDllMain=0`), which makes the loader load the
  plugin at once. If you renamed the loader to `winmm.dll`, name the file
  `winmm.ini`; with a loader of your own, set that line in its ini.
- With GOG's C1-Launcher and CryMP's client the plugin now starts a little
  earlier, while `CrySystem.dll` loads; nothing else changes there.
- Steam's Play button asks which launch option to start: "Play Crysis" is the
  32-bit game, which gets the particle fix and the allocator. The unnamed
  option is the 64-bit game, which crashed about 10 seconds after it started
  in our tests, with or without the mod, and the same with `crysis64.exe`
  started directly: a SecuROM check in its 64-bit `CrySystem.dll` fails and
  turns on the processor's floating-point exceptions. C1-Launcher
  (https://github.com/ccomrade/c1-launcher), which the GOG release ships,
  skips that check; with it, the whole mod works on the Steam release as on
  GOG's. The READMEs describe this.

`optiperf.asi`, `optiperf.ini` and the loader are byte-identical to v1.0.2's:
an installed v1.0.2 only needs `version.ini` added beside `version.dll`.

## Release archives

- `Crysis-MTO-v1.0.3-plugin-only.zip` contains the plugin and its
  configuration for both games (`Bin32`, `Bin64`), the project license,
  documentation, the notices of the libraries linked into the plugin, and
  payload hashes, for installations that already have an ASI loader.
  SHA-256:
  `D0D4BBC8E90BCDB0172C3ED2EE08A5F02E7B793022C8F54182C3084A8D5A4960`
- `Crysis-MTO-v1.0.3-with-Ultimate-ASI-Loader-v9.7.4.zip` additionally
  contains the pinned loader for both games (as `version.dll`), its settings
  (`version.ini`) and its MIT notice.
  SHA-256:
  `B7EF4DD36B8296D36E7B94B7F22124DA7876FA8BDD4A0BC1FBA8B8BC952CCEFB`

## Validation

- Reference build: the GOG release of Crysis 1.2.1 (build 6156), as for
  v1.0.2. The SHA-256 of its files whose code or tables the supplied settings
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
  The Steam release's `Bin32` and `Bin64` game DLLs are byte-identical to
  these; its launchers differ.
- `optiperf.asi` SHA-256 (v1.0.2's):
  - `Bin32`: `5C5015B61C804A3E990E573FAC05CAAA6854CFA323362214F8C7B8EE919EB4AD`
  - `Bin64`: `D0CFEB116E64C6DC05CCCB801D7847E2D885242AA9227F922B1B9344AE4A5EAA`
- Ultimate ASI Loader v9.7.4 SHA-256 (the release's `dinput8.dll`, bundled as
  `version.dll`):
  - x86: `EC2F4824ECA58DD40F425756A4A7CEC77B8E381F21D11D7C846EC4B339B617AB`
  - x64: `031A3E5576D91DCE1E438D36B9A3D462C7334AB4791990A8FF1E3DDC0E132DAF`
- `version.ini` SHA-256 (both folders):
  `330D05806D57E7FF57E4ED39B45524E02E5134CA750EAFA0C981007C108FC993`
- The binaries are v1.0.2's, as built and tested for it.
- The release package was installed into a fresh copy of the GOG release's
  binaries and started through Ultimate ASI Loader, without the development
  launcher, in 15 timedemo launches with the HUD on, none with an exception.
  In every one the loader loaded the plugin while `CrySystem.dll` loaded:
  - the 32-bit game (DX9 and DX10) and the 64-bit game with DX9 kept the render
    thread and the worker threads off by themselves, with the particle fix and
    the allocator on;
  - the 64-bit game with DX10 ran the render thread, the two threads' cores and
    both worker parts on island, village, ice, harbor and ascension, and with
    the game's frame rate display on; every answer the render thread predicts
    was exact, apart from the polygon counts on island, which it gives a frame
    late by design. Island ran at 149 fps and village at 124 in the second
    pass, against 234 and 138 in v1.0.2's test, with 1.0-1.5 cores of other
    load on the machine this time. Back to back in the same conditions, with
    and without `version.ini` (A-B-B-A-A-B-B-A, island): 185 against 191 fps,
    the adjacent pairs +3 %, +3 %, 0 % and -18 % (one slow run): no
    consistent difference;
  - with the game limited to fewer processors (village's benchmark, against
    the allocator alone on the same processors): 4 cores with both their
    threads 114 against 91 fps (x1.25), one thread on each of 4 cores 119
    against 95 (x1.25), 2 cores 108 against 91 (x1.18). As before, the thread
    placement stayed off by itself in the last two.
- CryMP's client v27 on the same installation, none with an exception: the
  64-bit client's Contact benchmark at 193 fps and Recovery's at 105 in the
  second pass, with the same parts on as with v1.0.2; a level unloaded and
  loaded again, then the game quit: CryMP's C runtime freed 122 of the
  allocator's blocks and they went back to it, as with v1.0.2; the 32-bit
  client's Contact benchmark at 118 fps, and `+disconnect`, clean.
- The Steam release, its install copied and the archive extracted into it: the
  loader loaded the plugin with Steam's 32-bit launcher (Contact 140 fps and
  ice 85 in the second pass; that launcher starts the DX10 renderer) and with
  its 64-bit ones (`crysis64.exe` started directly, and Steam's SecuROM
  launcher), where the game then crashed about 10 seconds in on SecuROM's
  check, as it does without the mod. v1.0.2's archive never loaded the plugin
  there.
- A play session with the GOG release through the loader with `version.ini`
  (64-bit DX10, a few minutes in a level, quit from the menu): clean, every
  answer the render thread predicts exact, no exception.
- The two archives contain the same `optiperf.asi`, `optiperf.ini`, loader and
  `version.ini` files, byte for byte, as the package these tests ran.
