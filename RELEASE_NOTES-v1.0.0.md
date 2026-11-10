# Crysis MTO v1.0.0

## What changed

Since v0.5.0:

- The particle emitters' update runs on worker threads. The 3D engine updated
  every particle emitter one after another on the game's main thread; worker
  threads and the main thread now update them at once, while the rest of the
  engine's particle update (which emitters are still alive, where they are
  drawn) keeps the game's order. The sounds and physics the emitters start
  still go one at a time. +2 to +8 % fps in six levels' benchmarks, most on the
  effect-heavy ones (ice, ascension).
- The characters' animation runs on worker threads. Each character's forward
  kinematics (its bone pose) was made in its own update, inside the entity
  system's update on the main thread; it now waits for the end of that update,
  where worker threads and the main thread make all of them at once, the
  post-processes (IK, attachments) after them on the main thread. The game's
  logic reads each pose, and the movement its animation drives, one frame
  late; what is drawn is current, and the player's own character and
  first-person weapons are unchanged. It saves about 0.1-0.3 ms a frame in
  fights, and it wasn't noticeable in play.
- `[Debug] FrameLog=on` (off by default) logs the frame rate: a line a second
  in `optiperf_frames.csv`, and the average, the 1 % and 0.1 % lows and the
  longest frame in `optiperf.log`.
- The release comes as two archives, with and without Ultimate ASI Loader,
  each with the project license and payload hashes.

The new parts are on in the supplied `optiperf.ini` (`[ParticleUpdate]
Mode=on`, `[AnimUpdate] Mode=on`). Without a line a setting is off, so replace
an `optiperf.ini` kept from v0.5.0. Either part can be turned off there on its
own.

## Release archives

- `Crysis-MTO-v1.0.0-plugin-only.zip` contains the plugin and its
  configuration for both games (`Bin32`, `Bin64`), the project license,
  documentation, the notices of the libraries linked into the plugin, and
  payload hashes, for installations that already have an ASI loader.
  SHA-256:
  `ED8E0E32595978B2FDB626B0300F6A3615A243ED90F99D33B901119D4E68243A`
- `Crysis-MTO-v1.0.0-with-Ultimate-ASI-Loader-v9.7.4.zip` additionally
  contains the pinned loader for both games (as `version.dll`) and its MIT
  notice.
  SHA-256:
  `3694563952FE41D398678A86BEC3206B5E837A73C27E8AC96CCB5DBD8E652018`

## Validation

- Reference build: the GOG release of Crysis 1.2.1 (build 6156). The SHA-256
  of its files whose code or tables the supplied settings change:

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
  - `Bin32`: `B917B7875C66B114D11BC1B8E194D27014597CC0D99686E71A1A32A1DE08A4EF`
  - `Bin64`: `CFF1421CD7581C89705C30C10BFEA5F72462E7E49CB702410F330CDE60EA7DEB`
- Ultimate ASI Loader v9.7.4 SHA-256 (the release's `dinput8.dll`, bundled as
  `version.dll`):
  - x86: `EC2F4824ECA58DD40F425756A4A7CEC77B8E381F21D11D7C846EC4B339B617AB`
  - x64: `031A3E5576D91DCE1E438D36B9A3D462C7334AB4791990A8FF1E3DDC0E132DAF`
- Compilation with warnings enabled, and the allocator, call tracer and shadow
  pre-pass tests passed.
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
    the allocator alone on the same processors): 4 cores with both their
    threads 158 against 111 fps (x1.42), one thread on each of 4 cores 148
    against 111 (x1.32), 2 cores 133 against 106 (x1.25). The thread placement
    stayed off by itself in the last two.
- A 4-minute play session in Recovery, the same route twice (64-bit DX10,
  1080p Very High, 8 cores of an i9-13900K, an RTX 4090), against the same game
  with only the allocator, which the 64-bit game needs to run at all:

  | | Allocator only | v1.0.0 |
  | --- | ---: | ---: |
  | Average | 117 fps | 188 fps |
  | 1 % low | 73 fps | 122 fps |
  | 0.1 % low | 55 fps | 95 fps |
  | Time under 100 fps | 35 % | 0.7 % |

  The scripted benchmarks show less, x1.45 on average over eight levels: play
  has more of the frames that the main thread holds back.
- The release package was played from the first level through Assault. It
  worked well.
- The two archives contain the same `optiperf.asi`, `optiperf.ini` and loader
  files, byte for byte, as the package these tests ran.
