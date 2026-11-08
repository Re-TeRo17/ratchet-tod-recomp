# Features

<a id="top"></a>

[Back to the README](../README.md) | [Settings reference](SETTINGS.md) | [Changelog](../CHANGELOG.md)

- [Graphics](#graphics)
- [Performance](#performance)
- [Audio and video](#audio-and-video)
- [Input](#input)
- [Experimental options (off by default)](#experimental-options-off-by-default)
- [Saves, language and installation](#saves-language-and-installation)

## Graphics

- Direct3D 12 renderer (feature level 12_0) driven by the console's RSX command stream.
- Internal resolution from 0.5x to 8x the game's 1280x720 (`RSX_SCALE`): 720p, 1080p, 1440p, 4K and
  beyond. It is independent of the window size.
- Anti-aliasing: FXAA or MSAA x2 / x4 / x8. Anisotropic filtering up to 16x.
- Display modes: windowed (any size), borderless window or exclusive fullscreen; VSync on or off.
- Optional, experimental **ray-traced ambient occlusion** (DXR 1.1: RTX 20 / RX 6000 series or newer),
  with three quality levels.
- Optional, experimental screen-space reflections (off by default, expensive at 4K).
- True 4K for the 3D scene: the game's 960x704 scene render is no longer stretched 4/3 in width
  (`RSX_HSTRETCH_FIX`, on by default), so Ratchet and the world are as sharp as the rest of the image.
- Character ground shadows restored and the shadow bias reworked (polygon offset in the vertex
  epilogue and a faithful border for lookups outside the shadow map).
- Rendering fixes made along the way for the bloom chain (the bloom halos are fixed), shadows and fog
  (see the [changelog](../CHANGELOG.md) and [Known issues](../README.md#known-issues) for what is still open).
- A frame counter (FPS, or detailed: frame time, 1% low, GPU and CPU load and video memory) that you
  can switch on.

## Performance

- A 60 fps target (`TOD_FPS`), with an optional cap above it.
- Native fast paths for two of the game's heaviest SPU jobs (skeletal animation and collision
  queries), so they run as plain native code instead of the lifted SPU version, and 17 graphics job
  modules use a round-toward-zero fast float path.
- Reference numbers for 1.2.0 (Ryzen 5 3600 + RTX 3070, 4K, MSAA x8). Each one is an automated
  3-minute fight with all the weapons, heavier than normal play:

  | Area | Average fps | Seconds at 59-60 fps |
  |---|---|---|
  | Cobalia | 59 | 85% |
  | Rykan V | 57 | 56-61% |
  | Apogee Space Station | 57 | 60% |
  | Metropolis (busiest fights, ~3700 draws per frame) | 54 | 44% |

  For comparison, 1.0 ran the opening of Metropolis at about 31 fps. In normal play the numbers are
  higher: a 14-minute session at Sargasso averaged 59 fps with 88% of the seconds at 59-60, and a
  20-minute fight test at Rykan V 59 fps with 80%. Lowering the resolution helps the heaviest scenes
  little, because they are limited by the CPU.
- The graphics command list is recorded on its own thread and the draws of each frame are prepared
  on three worker threads. The vertex cache is large enough for the busiest scenes (in 1.0 it was
  emptied every few frames in Metropolis, a hitch of about 35 ms each time).
- Shaders are built before you play: the download holds a map of the game's 1719 graphics pipelines
  (`shaders\pipemap.bin`), built once on the first start and kept in `cache\`, so new areas do not
  stutter the first time an effect appears. Pipelines seen while playing are added to the cache.
- The hitch when the game autosaves is shorter (about 620 ms before, about 400 ms now).
- 1.1 added: less stutter in busy scenes (the vertex cache no longer rebuilds in bursts), a faster
  graphics thread (about 2.5 ms per frame in Metropolis), faster collision (about 1 ms per frame)
  and a profile-guided build of the graphics code. Each one can be turned off in `tod.ini`.
- 1.2.0 added: the draws of each frame are prepared on three threads, the SPU collision jobs run on a
  native engine, render targets share memory blocks and offscreen passes no longer get an MSAA copy,
  frames follow the monitor's refresh, and the 1719 graphics pipelines are built before you play. See
  the [changelog](../CHANGELOG.md) for the measured numbers; `TOD_PERF_DEFAULTS=0` (environment variable)
  goes back to the 1.1 behaviour.
- The game's SPU code runs on your CPU cores, so a fast 6+ core CPU matters most.

## Audio and video

- Audio with a safety margin against crackle; an experimental low-latency output is optional.
  The music streaming threads now get enough priority, so the music no longer starves.
- The output follows Windows' default audio device when it changes (optional, on by default).
- Dialogue and ambient sound that some levels stream from separate files play since 1.1 (in 1.0
  much of Cobalia's dialogue, for example, was silent). `TOD_LEVEL_STREAMS = 0` turns it off.
- Cutscenes and movies play through the game's own video decoder.
- **Skip a movie:** hold **Start** on the pad (or **Enter**, **Esc** or **Space** on the keyboard) for
  2 seconds while a pre-rendered movie plays. This works for the video cutscenes (the ones the PS3
  streams from disc), not for the scenes rendered by the game engine. `TOD_FMV_SKIP = 0` turns it off.

## Input

- Controllers through SDL2 and XInput: Xbox, DualShock 4, DualSense, Switch Pro and others.
- The button icons in the game follow the pad you use (PlayStation 3/4/5, Xbox, Switch) or the
  keyboard.
- Keyboard and mouse: every key is rebindable in F1 > Controls (written to `tod.ini` as `KEY_*`),
  including keys for the camera (the right stick), unbound by default; the experimental mouse camera
  below is the other way to steer it. See [Controls](../README.md#controls).
- **Xbox impulse triggers** (1.1): on an Xbox One / Series controller the motor in the right trigger
  kicks at every shot. On by default; F1 > System > Xbox impulse triggers (`TOD_XB_TRIGGERS`).
- **DualSense adaptive triggers**: the right trigger follows the equipped weapon (a click for single
  shots, a hard pull for heavy weapons, a rattle for automatic fire, a steady drag for the
  flamethrower and the beams) with a kick at every shot, and goes slack in menus, in pause, in
  cutscenes and when the weapon is out of ammo. On a DualSense the right trigger (R2) fires and R1
  takes R2's role, so you fire with the trigger that has the motor (`TOD_DS_FIRE_R2 = 0` restores the
  PS3 layout). `TOD_DS_TRIGGERS`, `TOD_DS_TRIGGER_STRENGTH` and `TOD_DS_FIRE_R2` are also in F1 >
  System. The trigger effects are from [RACiT](https://github.com/HighDrexler/RACiT) (MIT).

## Experimental options (off by default)

Both are switched on in `tod.ini` (next to `tod.exe`): remove the `#` at the start of the line and
set the value, then restart the game (the mouse camera also from F1 > Controls, at once). They are not as well tested as the rest.

- **Ultrawide**: `TOD_ASPECT = 21:9` (or `32:9`, `43:18`, any `w:h` or ratio from 1.2 to 4, or `auto`).
  Also in F1 > Video > Aspect ratio. Without it a wide screen shows the 16:9 picture with black bars at
  the sides (not stretched). With it (since 1.2.1) the game shows a wider view: the same vertical field
  of view as 16:9, more of the scene at the sides, nothing stretched. `auto` takes the shape of the screen in borderless and fullscreen mode, and of
  `RSX_WINDOW` in windowed mode (1280x720 if unset, which is 16:9: nothing changes). For a 3440x1440
  monitor use `TOD_ASPECT = auto` or `43:18` (21:9 leaves about 40 px of bar on each side).
  `RSX_WINDOW` is used only in windowed mode. Use `RSX_SCALE` 2-3 (the horizontal resolution is
  1280 x `RSX_SCALE`). Read at start-up only. The HUD, the weapon wheel and the menus stay 16:9 in the
  middle of the screen (the pause menu background covers the whole width); pre-rendered movies stay 16:9 with bars.
- **Mouse camera**: `TOD_MOUSE = 1`, or F1 > Controls > Mouse camera. The mouse drives the right stick (camera and aim); left button =
  R1 (fire), right button = L1 (strafe / lock-on), middle button = Triangle (quick select), wheel =
  D-pad left/right, `Esc` = Start. `TOD_MOUSE_SENS` (default 1.0), `TOD_MOUSE_INVERT_Y` and
  `TOD_MOUSE_WHEEL = off` fine-tune it (the mouse buttons can be reassigned in F1 > Controls). Because it feeds the right stick, the top turning speed is
  the game's own Camera Speed option: raise it in the game's Options. F1 releases the cursor.

## Saves, language and installation

- **PC settings in the game's Options menu (1.2.1):** on the first start the game prepares the menu
  from your own copy of the game (about half a second, once; the copies go in `mods\`, the original files
  are never changed). The port's rows (resolution, window mode, aspect ratio, anti-aliasing, shadows,
  ray-traced AO, VSync and more, translated) then appear in Options, from the main menu and from the
  pause menu. `TOD_MENU = 0` in `tod.ini` turns it off. The F1 page has the same settings and more.

- **Save slot list (1.1):** saving and loading show the game's slot list, so you choose the slot.
  `TOD_SAVE_PICKER = 0` turns the list off: then loading picks your newest save and a manual save
  makes a new slot.
- Saves are kept next to the game, in `gamedata\dev_hdd0\home\00000001\savedata`, and are written by the game's own save code. F1 > System > **Saves folder** opens the folder; `PS3_SAVEDATA_ROOT` in `tod.ini` moves it. One `BCES00052_SAVE_<n>` folder per save, the same layout RPCS3 uses, so copying saves between the two should work (not tested yet).
- The game language (text, voices, subtitles) can be changed from the settings page.
- `Install.bat` / `tod_setup.exe` installs from a decrypted `.iso` or a disc folder; `Play.bat`
  starts the game and keeps the last two logs (`log\tod.log`, `log\tod.previous.log`).
- Shows Discord Rich Presence (current level and time played, with a GitHub button) while the Discord
  app is running; hide it in Discord's Settings > Activity Privacy.
- Mods folder (on by default, `TOD_MODS = 0` turns it off): a file in `mods\` next to `tod.exe` is read instead of the game file at the same path under `game\disc\`, e.g. `mods\PS3_GAME\USRDIR\built\movies\l01_s01_ntro_fmv\l01_s01_ntro_fmv.bik` replaces the intro movie (the `PS3_GAME\` level is optional). Reads only: saves and the game's own files are never touched, and deleting the mod file restores the original. Mods are your own files; the project ships none.

<sub><a href="#top">back to top</a></sub>
