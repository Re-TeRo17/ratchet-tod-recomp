# Changelog

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
