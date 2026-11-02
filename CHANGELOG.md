# Changelog

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
