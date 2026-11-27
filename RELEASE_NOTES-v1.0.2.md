# Crysis MTO v1.0.2

## What changed

Since v1.0.1, the mod works under [CryMP's client](https://github.com/crymp-net/crymp-client),
the community's multiplayer client (v27 tested: `CryMP-Client64.exe` and
`CryMP-Client32.exe`). v1.0.1 under it left the allocator unused and the system
snapshot off, and could crash with the render thread on:

- The allocator. CryMP's client brings its own memory functions, and the
  engine takes them from the executable before CrySystem's, so v1.0.1 replaced
  functions nothing called. v1.0.2 replaces the executable's when it has them:
  the allocator's blocks are zero-filled as CryMP's are, and the blocks CryMP
  made before the switch go back to its own functions. CryMP's own code frees
  some of the engine's blocks with its C runtime (its vehicles' parts, when a
  level unloads), so those are taken back too; without that, quitting to the
  menu corrupted the heap.
- The system snapshot, which gives the render thread the game's clock and
  camera as they were when the frame was handed over. CryMP replaces two of
  CrySystem's functions, and the snapshot's check refused them and stayed off:
  the render thread then rendered the water reflections itself, beside the
  main thread, and could crash.
- The particle emitters' worker threads stay off under CryMP's client, by
  themselves: it brings its own physics and sound, and the workers only know
  how to serialize the game's. `optiperf.log` says so.

With the game's own executable nothing changes: the allocator's new mode needs
an executable with its own memory functions, the particle workers' check
another module's physics or sound, and the snapshot finds CrySystem's
functions as before. `optiperf.ini` is the same as v1.0.1's: one kept from
v1.0.1 works as it is.

## Release archives

- `Crysis-MTO-v1.0.2-plugin-only.zip` contains the plugin and its
  configuration for both games (`Bin32`, `Bin64`), the project license,
  documentation, the notices of the libraries linked into the plugin, and
  payload hashes, for installations that already have an ASI loader.
  SHA-256:
  `8B99A1BE34809850BAAF223F45E9F10063C61D31FF27594032B69019C9E01328`
- `Crysis-MTO-v1.0.2-with-Ultimate-ASI-Loader-v9.7.4.zip` additionally
  contains the pinned loader for both games (as `version.dll`) and its MIT
  notice.
  SHA-256:
  `202F1255E72E76F516FF5C9CBEC2A3F837B2875416214DC3226041B19529EDEB`

## Validation

- Reference build: the GOG release of Crysis 1.2.1 (build 6156), as for
  v1.0.1. The SHA-256 of its files whose code or tables the supplied settings
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
- CryMP's client v27, as released (`CryMP-Client64.exe`, `CryMP-Client32.exe`).
- `optiperf.asi` SHA-256:
  - `Bin32`: `5C5015B61C804A3E990E573FAC05CAAA6854CFA323362214F8C7B8EE919EB4AD`
  - `Bin64`: `D0CFEB116E64C6DC05CCCB801D7847E2D885242AA9227F922B1B9344AE4A5EAA`
- Ultimate ASI Loader v9.7.4 SHA-256 (the release's `dinput8.dll`, bundled as
  `version.dll`):
  - x86: `EC2F4824ECA58DD40F425756A4A7CEC77B8E381F21D11D7C846EC4B339B617AB`
  - x64: `031A3E5576D91DCE1E438D36B9A3D462C7334AB4791990A8FF1E3DDC0E132DAF`
- Compilation with warnings enabled, and the allocator (both architectures,
  with the executable's memory functions and its C runtime's heap calls),
  call tracer and shadow pre-pass tests passed.
- The release package was installed into a fresh copy of the game's binaries
  and started through Ultimate ASI Loader, without the development launcher,
  in 15 timedemo launches with the HUD on, none with an exception:
  - the 32-bit game (DX9 and DX10) and the 64-bit game with DX9 kept the render
    thread and the worker threads off by themselves, with the particle fix and
    the allocator on;
  - the 64-bit game with DX10 ran the render thread, the two threads' cores and
    both worker parts on island, village, ice, harbor and ascension, and with
    the game's frame rate display on (`r_DisplayInfo 1`); every answer the
    render thread predicts was exact. Island ran at 234 fps and village at 138
    in the second pass, against 228 and 137 in v1.0.1's package test;
  - with the game limited to fewer processors (village's benchmark, against
    the allocator alone on the same processors, with 0.6-1.1 cores of other
    load on the machine): 4 cores with both their threads 137 against 96 fps
    (x1.43), one thread on each of 4 cores 115 against 92 (x1.25), 2 cores 110
    against 91 (x1.21). As in v1.0.1, the thread placement stayed off by
    itself in the last two, with one worker thread for each part.
- CryMP's client v27 on the same installation, with the package's files as
  installed (its online services pointed at a local address), DX10 and the
  HUD on, none with an exception:
  - the 64-bit client ran the render thread, the two threads' cores, the
    animation's workers and the allocator on the executable's memory
    functions; the particle emitters' workers stayed off by themselves, and
    the render thread rendered no reflection itself. Contact's benchmark ran
    at 228 fps and Recovery's at 121 in the second pass. In an earlier job
    with the development build, against CryMP with every part of the mod off:
    Contact 153 to 229 fps (x1.5), Recovery 87 to 114 (x1.32). Every answer
    the render thread predicts was exact, except the draw call and polygon
    counts, which it gives a frame late by design, and which CryMP's client
    reads every frame;
  - a level unloaded in the middle of Contact's benchmark (`disconnect`),
    loaded again, then the game quit: CryMP's C runtime freed 122 of the
    allocator's blocks, and they went back to the allocator;
  - the 32-bit client: Contact's benchmark at 142 fps in the second pass, and
    a level unloaded (`+disconnect`) with the same 122 blocks taken back,
    then a clean exit.
- Play sessions under CryMP's client (64-bit DX10, 1080p Very High, 8 cores of
  an i9-13900K, an RTX 4090): Contact and Recovery loaded from saves, each
  left to the menu, and a multiplayer server (Power Struggle on Mesa). The
  first session found the crash when a level unloads; the second, with the
  fix, was clean.
- The two archives contain the same `optiperf.asi`, `optiperf.ini` and loader
  files, byte for byte, as the package these tests ran.
