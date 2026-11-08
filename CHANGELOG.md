# Changelog

## 1.2.0 (7 November 2026)

**Faster, steadier, and no more freezes on Intel hybrid CPUs.** 1.2.0 spreads the renderer's work over
several threads, cuts video memory use at high settings, fixes the freezes on 12th-14th gen Intel CPUs
and adds a crash report and a detailed log to help with bug reports. Unzip it over 1.1: your settings
and saves are kept.

### Bug fixes

- **Freezes on Intel CPUs with P-cores and E-cores:** the game now runs on the P-cores only, so the
  `start "" /affinity ...` workaround is no longer needed. Thanks to TodeX for finding
  it ([#10](https://github.com/Re-TeRo17/ratchet-tod-recomp/issues/10)). AMD Ryzen and older Intel CPUs are
  not affected.
- **Long freezes in animation-heavy scenes:** a watchdog unsticks the character animation job when it stops
  answering, and a case where that job could not get a CPU core for seconds (a 3-4 second freeze at
  Sargasso) is fixed.
- **Shadows of Ratchet, enemies and allies flickering or jumping** in big fights after some time of play.
- **Alt-Tab in exclusive full screen** may cause a freeze or a crash; it now leaves full screen cleanly
  and comes back at the right size, at any resolution.
- Changing anti-aliasing or the display mode from F1 could freeze the renderer, and switching to exclusive
  full screen while playing could hang the window.
- **Above 60 fps:** an FPS limit above 60 now really applies (VSync is turned off above 60, so you may see
  tearing) ([#13](https://github.com/Re-TeRo17/ratchet-tod-recomp/issues/13)).
- With the FPS limit at 60 and VSync on, a frame was skipped now and then.
- Short stutters when Windows' video-memory figure jumped for a moment, and when render targets were
  created in the middle of a frame.
- Video memory may slowly fill up (gigabytes after a few minutes at 4K MSAA x8) in scenes that draw many
  passes without a depth buffer: fixed.
- Some animation settings written in `tod.ini` were ignored.
- In ultrawide the full-screen menu backgrounds were cut instead of covering the whole frame.
- On a CPU whose last core has only one logical processor, the audio thread could share it with another
  busy thread (one suspected cause of [#5](https://github.com/Re-TeRo17/ratchet-tod-recomp/issues/5));
  on a PC or virtual machine with a single logical processor no core is reserved for it any more.

### Performance

1.2.0 is mostly a performance release. Everything is on by default.

- **Shaders compiled before you play:** the download includes a map of 1719 graphics pipelines. On the
  first start (or after a driver update) the game builds them once, with a progress window (about 25 s on
  a 6-core CPU), so new areas no longer stutter the first time an effect appears. The map holds only
  numbers that describe pipelines, nothing of the game (`TOD_SHADER_PREWARM = 0` skips it).
- **Multi-threaded renderer:** the draws of each frame are prepared on three worker threads instead of
  one and closed in order, so the picture is identical. The lanes no longer wait for each other, and the
  thread that reads the game's graphics commands is much faster (`RSX_LANES` and the `RSX_LANES_*`,
  `RSX_WALK_*` keys).
- **Native SPU collision:** the game's collision jobs run inline on a native engine with vectorised maths,
  which helps big fights the most (`SPU_COLL_RING`).
- **About 0.8 GB less video memory** at 4K with MSAA x8: shadow maps, reflections and other offscreen
  passes no longer get an MSAA copy, render targets share large memory blocks, and MSAA copies and colour
  targets are pooled and reused (`RSX_MS_NO_SHADOW`, `RSX_RT_HEAP`, `RSX_MS_TWIN_*`, `RSX_SURF_POOL`).
- **Steadier video memory:** a governor releases idle targets near Windows' limit, and the budget is read
  smoothly so a momentary jump no longer triggers a release storm (`RSX_VRAM_GOV`).
- **Smoother frame pacing:** frames follow the monitor's real refresh instead of a timer, and resources
  are released outside the frame (`RSX_PACE_VBLANK`).
- **Measured** on a Ryzen 5 3600 + RTX 3070 at 4K (scale 3) with MSAA x8: 80% of the seconds at 59-60 fps
  in a 20-minute automated fight at Rykan V and 88% in a 14-minute play session at Sargasso, average
  59 fps, with no slowdown over time.
- **1.1 against 1.2.0** in the same heavy fight at Rykan V (3 minutes, all weapons, 4K MSAA x8, same exe
  with and without the new defaults, two runs each):

  | | 1.1 behaviour | 1.2.0 |
  |---|---|---|
  | Average fps | 47-51 | 57 |
  | Seconds at 59-60 fps | 32-36% | 56-61% |
  | Seconds below 45 fps | 26-30% | 2-3% |
  | Worst second | 0.3-26 fps (near-freeze) | 37-39 fps |
  | Peak video memory | 8.1-9.3 GB (more than the 8 GB card) | 6.2-6.4 GB |

Each key can be set to 0 on its own if it causes trouble on your PC.

### Display

- **Ultrawide (`TOD_ASPECT`, experimental):** the HUD and the menus stay 16:9 in the
  middle of the screen instead of being stretched ([#3](https://github.com/Re-TeRo17/ratchet-tod-recomp/issues/3)).
- **Thin black bars:** the game draws its picture 704 lines high inside a 720-line frame, which leaves
  thin bars at the top and bottom. This is how the original PS3 game looks too (TVs usually hid them
  with overscan). `RSX_CROP_BARS = 8` in `tod.ini` cuts the bars and stretches the picture 2.3% taller to
  fill the screen (off by default, to stay faithful to the PS3)
  ([#12](https://github.com/Re-TeRo17/ratchet-tod-recomp/issues/12)).
- The FPS limit can be changed while playing, and the detailed FPS counter also shows GPU load, CPU load
  and video memory.

### Bug reports

- **Crash report:** if the game crashes it now writes why in `log\tod_crash.txt`: the error code, the thread
  and where it happened (module and address, with the call stack), and the log says so. A minidump is also
  written with `TOD_MINIDUMP = 1`. Please attach them to your issue.
- **Detailed log:** next to the usual log the game writes `tod_v2_<date>.jsonl`, with fps, CPU, GPU,
  video memory and level every second and the cause of every stutter. Press **F7** to put a mark in it
  at the moment something goes wrong. `TOD_LOG_V2 = 0` turns it off.

### Other

- **Credits** tab in the F1 page, with the project's supporters.
- **Steam Deck / SteamOS:** the README now has an install guide through Proton, from a player's guide
  ([#18](https://github.com/Re-TeRo17/ratchet-tod-recomp/issues/18)).
- **Teleport menu (experimental):** turn it on in F1 > System > Teleport menu (or `TOD_TELEPORT = 1`), then
  **F5** opens a list of the warp points of the current level and a level switcher. Switching level keeps your
  inventory (the game restarts on that level). Meant for testing and bug reports, not for normal play; off by
  default.
- `TOD_PCORES = 0` turns off the P-core-only mode on hybrid Intel CPUs; the environment variable
  `TOD_PERF_DEFAULTS=0` turns off all the new performance defaults at once (1.1 behaviour).

### Known issues

- The weapon vendor screen and the weapons menu flicker briefly every 1.5 seconds (also in 1.1; being
  worked on).
- Very large fights (more than about 3000 draws per frame) can still drop to about 50 fps at 4K with
  MSAA x8.
- If Windows gives the game less video memory (other programs using the GPU), a short stutter can still
  happen when a heavy scene starts.
- The smoke of Clank's rocket boost can appear late the first time it is used.
- Ultrawide (`TOD_ASPECT`): the 3D picture is stretched to the wide shape, because the wider field of view
  does not apply yet (being fixed for 1.2.1).
- CPUs without AVX2 (Intel 3rd gen and older) are still not supported
  ([#7](https://github.com/Re-TeRo17/ratchet-tod-recomp/issues/7),
  [#15](https://github.com/Re-TeRo17/ratchet-tod-recomp/issues/15)); the game says so at start.

## 1.1 (4 November 2026)

**Smoother, faster, and your saves finally under control.** 1.1 brings a real save slot list,
the voices and sounds that were missing in some levels, less stutter, a faster renderer and
a pile of fixes for starting the game. Unzip it over 1.0: your settings and saves are kept.

### Bug fixes

- `Play.bat` failed with a red PowerShell error and the game did not start
  ([#5](https://github.com/Re-TeRo17/ratchet-tod-recomp/issues/5)).
- `tod.exe` closed at once without saying why (missing game files, CPU without AVX2...): it now
  shows a message with the reason ([#5](https://github.com/Re-TeRo17/ratchet-tod-recomp/issues/5)).
- Voices and ambient sounds were missing in some levels
  ([#1](https://github.com/Re-TeRo17/ratchet-tod-recomp/issues/1)).
- Started from a shortcut or a launcher, the game said "not installed" and the saves seemed gone.
- Unzipping a new version over the old one replaced your `tod.ini` settings.
- With two or more saves, loading took the oldest one and a new save overwrote the first slot.
- Short stutters in busy scenes when the vertex cache rebuilt itself in bursts.
- The installer did not say clearly when the disc image was decrypted with the wrong key.

### Saves

- **Pick your save slot.** Saving and loading now show the game's slot list, so you choose
  which save to load or overwrite, or make a new one.
- With the list turned off (`TOD_SAVE_PICKER = 0`), loading picks your newest save and a manual
  save makes a new slot.

### Sound

- **Voices and sounds that were missing are back.** Some levels stream their dialogue and
  ambient sound from separate files that 1.0 did not read (on Cobalia, for example, a lot of
  the dialogue was silent). They now play (`TOD_LEVEL_STREAMS`, on by default).

### Performance

- **Less stutter:** the vertex cache no longer rebuilds itself in bursts in busy scenes
  (`RSX_VCKEY_NODEF`, on by default).
- **A faster renderer:** the graphics thread now remembers texture setups and shader scans it
  has already worked out, about 2.5 ms per frame saved in Metropolis (`RSX_TEXMASK_MEMO`,
  `RSX_FPSCAN_MEMO`).
- **Faster collision:** two of the hottest collision loops are now hand-written for your CPU,
  about 1 ms per frame (`TOD_COLL_HAND`).
- **Profile-guided build:** the graphics code is compiled with a profile recorded while playing
  five planets, so the busiest paths run faster.
- Every new speed-up has a switch in `tod.ini` to turn it off, in case it causes trouble on
  your PC.

### Starting the game

- **`Play.bat` works again.** In 1.0 it stopped with a red PowerShell error on every Windows 10 / 11 PC
  (an empty argument list that Windows PowerShell 5.1 refuses), so the game did not start and no log was
  written. If the game now closes with an error right after the start, `Play.bat` says so and keeps the
  window open.
- **`tod.exe` explains why it cannot start** (game files missing, CPU without AVX2, ...) in a message box
  instead of a console window that closes at once.
- `tod.exe` finds its own folder when it is started from a shortcut or a launcher with another working
  folder (before, it said "not installed" and the saves seemed gone).
- **Updates keep your settings:** the download now holds `tod.default.ini` instead of `tod.ini`, so
  unzipping a new version over the old one does not replace your `tod.ini` (it is made from the default
  at the first start). Your saves (`gamedata\`) and shader cache (`cache\`) were never in the download.
- **`Update.bat`:** updates the port in one click. It checks GitHub, asks first, downloads the new
  version and replaces only the port's own files; `tod.ini`, saves, shader cache and the installed game
  are never touched.
- **Update notice:** at start the game asks GitHub whether a newer version exists and shows it on screen
  and in F1 > System > Updates (nothing is downloaded; `TOD_UPDATE_CHECK = 0` turns it off). The version
  is now shown in F1, in the log and in a corner of the title screen and main menu (`TOD_VERSION_TAG = 0`
  hides it).
- **Game files on another drive:** the installed game (the `game\disc` folder) can
  now live on any drive. Close the game, move the `game\disc` folder where you want, then write its
  new path in `tod.ini`, for example `PS3_VFS_ROOT = D:\Games\ToD\disc`.
- The installer says clearly when a disc image was decrypted with the wrong key (for example the US
  disc's key on the European disc).

### Playing

- **Skip a movie:** hold **Start** (or Enter, Esc or Space on the keyboard) for 2 seconds while a
  pre-rendered movie plays and it is skipped. It applies to the video cutscenes, not to the scenes
  the game engine renders. `TOD_FMV_SKIP = 0` in `tod.ini` turns it off. (It was already in 1.0,
  but nobody was told.)
- **Xbox impulse triggers:** on an Xbox One / Series controller the motors in the triggers now
  react to your weapon (`TOD_XB_TRIGGERS`, on by default, also in F1; not tested on many pads yet).

## 1.0 (first public release, 2 November 2026)

Development started on 20 October 2026; this is the first version published.

First public release of the port: a native Windows port of Ratchet & Clank: Tools of Destruction
(European release, BCES00052 v1.00), statically recompiled from the game's PowerPC and SPU code on the
ps3recomp toolkit. Direct3D 12 renderer, no emulation at run time. Nothing of the game is included in
the download: the installer (`Install.bat`, or `tod_setup.exe` on the command line) takes your own
decrypted disc (`.iso` or folder), checks Title ID, version and the SHA-256 of `EBOOT.BIN`, copies the
disc, makes `elf\EBOOT.ELF` from it and asks for the game language. `Play.bat` starts the game.

### Graphics

- **The 3D scene is now really 4K.** The game renders its scene at 960x704 and stretches it 4/3 in
  width afterwards, which made everything soft at any resolution. That stretch is undone
  (`RSX_HSTRETCH_FIX`, on by default; `0` restores the old behaviour).
- **Character ground shadows are back**, and the shadow bias is reworked (polygon offset applied in
  the vertex epilogue, and a faithful border for lookups outside the shadow map).
- Bloom halos fixed.
- The Apogee Space Station's holographic platforms and lasers no longer vanish when only sky or space
  is behind them (`RSX_PACK_FIX`, on by default; `0` restores the old behaviour).
- Internal resolution from 0.5x to 8x the game's 1280x720 (720p to 4K and beyond), window mode, VSync,
  FPS limit, FXAA / MSAA x2-x8, anisotropic filtering, optional ray-traced ambient occlusion
  (DXR 1.1 GPUs) and optional screen-space reflections (`RSX_SSR`, off by default).

### Performance

- Faster native work for the game's SPU jobs: round-toward-zero fast float on 17 graphics job
  modules, a native animation evaluator and native collision.
- The graphics command list is recorded on its own thread (`RSX_ENCODE_THREAD`, on by default).
- The vertex cache is four times larger (`RSX_VC_SLOTS`, default 32768): in Metropolis it used to be
  emptied every few frames, a hitch of about 35 ms each time.
- On a Ryzen 5 3600 + RTX 3070 at 4K with MSAA x8: Cobalia 60 fps, Pirate Base from about 52 to 58,
  the Imperial Fight Fest arena from 49 to 54, the opening of Metropolis from 24 to 31.
- About 23% fewer shader pipelines to compile, so less stutter on the first visit to an area. Shaders
  are compiled on first use and kept in `cache\`; the pipelines seen before are pre-built at start.

### Stability & hitches

- The autosave hitch is shorter: about 620 ms before, about 400 ms now.
- Music no longer starves (the streaming threads get a higher priority).
- Audio with a safety margin against crackle; low-latency output is optional.

### Options

- Settings in `tod.ini` and an in-game page (F1, or hold SELECT): resolution, window mode, VSync,
  FPS limit, anti-aliasing, FPS counter, game language and button prompts (PlayStation 3/4/5, Xbox,
  Switch, keyboard).
- **Controls** tab in the F1 page: every key and mouse button can be rebound (`KEY_*` in `tod.ini`),
  including keys for the camera (the right stick).
- **DualSense adaptive triggers** that follow the equipped weapon, with a kick at every shot; on a
  DualSense the right trigger fires (`TOD_DS_TRIGGERS`, `TOD_DS_TRIGGER_STRENGTH`, `TOD_DS_FIRE_R2`,
  all on by default and in F1 > System). Trigger effects adapted from RACiT (MIT).
- **Discord Rich Presence**: the current planet and the time played, with a link to this repository.
- **Mods folder**: a file in `mods\` next to `tod.exe` is read instead of the game file at the same
  path (`TOD_MODS`, on by default; reads only, saves are never touched).
- F1 > System > **Saves folder** opens the saves in Explorer; `PS3_SAVEDATA_ROOT` moves them. Saves use
  the same folder layout as RPCS3.
- The ray-traced options are marked EXPERIMENTAL in the F1 page.
- New, experimental and off by default (enable them in `tod.ini`):
  - `TOD_ASPECT = 21:9` (or `32:9`, `auto`): ultrawide. The 3D view widens; the HUD is stretched and
    cutscenes stay 16:9.
  - `TOD_MOUSE = 1` (or F1 > Controls): mouse camera, driven through the right stick (`TOD_MOUSE_SENS`,
    `TOD_MOUSE_INVERT_Y`, `TOD_MOUSE_WHEEL`).

### Known issues

See the README.
