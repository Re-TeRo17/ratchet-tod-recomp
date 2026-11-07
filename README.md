<div align="center">

<h1><img src="docs/images/banner.png" alt="Ratchet &amp; Clank: Tools of Destruction Recompiled" width="100%"></h1>

**Ratchet & Clank: Tools of Destruction for PC** (*Ratchet & Clank Future: Tools of Destruction*):
a native Windows port of the PlayStation 3 game, built by static recompilation. No emulator,
Direct3D 12, up to 4K and 60 fps.

![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-blue)
![Renderer](https://img.shields.io/badge/renderer-Direct3D%2012-green)
![Version](https://img.shields.io/badge/version-1.2.0-orange)
![Game](https://img.shields.io/badge/game-BCES00052%20(EU)-lightgrey)
![License](https://img.shields.io/badge/license-MIT-yellow)

[![Get it on RecompLauncher](https://recomplauncher.com/assets/press/launcher/badge-dark.svg)](https://recomplauncher.com/game/ratchet-tod-recomp)
[![Verified developer on RecompStore](https://recompstore.com/assets/press/verified-dark.svg)](https://recompstore.com/dev/re-tero17)

<img src="docs/screenshots/01_metropolis_kerwan.jpg" alt="Metropolis, Kerwan, running natively on PC at 4K" width="100%">

<a href="https://github.com/Re-TeRo17/ratchet-tod-recomp/releases"><img src="docs/images/nav_download.png" alt="Download" width="250"></a>
<a href="#installing"><img src="docs/images/nav_install.png" alt="Install" width="250"></a>
<a href="#settings"><img src="docs/images/nav_settings.png" alt="Settings" width="250"></a>
<a href="#known-issues"><img src="docs/images/nav_known_issues.png" alt="Known issues" width="250"></a>
<a href="#reporting-bugs"><img src="docs/images/nav_report_bug.png" alt="Report a bug" width="250"></a>
<a href="#support-the-project"><img src="docs/images/nav_support.png" alt="Support" width="250"></a>

</div>

A native PC port of **Ratchet & Clank: Tools of Destruction** (Ratchet & Clank Future: Tools of
Destruction, PlayStation 3, European release **BCES00052**) for Windows, made by static
recompilation of the game's code on the [ps3recomp](https://github.com/sp00nznet/ps3recomp)
toolkit.

The game's PowerPC (PPU) and SPU code is translated to C/C++ ahead of time and built into a normal
executable, with the PS3's system libraries replaced by native implementations and the RSX graphics
by Direct3D 12. There is no emulation at run time.

**Version 1.2.0 is out** (7 November 2026): no more freezes on Intel hybrid CPUs, a multi-threaded
renderer, less video memory and shaders built before you play. See the [changelog](CHANGELOG.md).
Thank you to the project's [supporters](#supporters)!
Version 1.1 (4 November 2026) added a save slot list and the missing level voices. Version 1.0 was the
first public release (2 November 2026; development started on 20 October 2026). The game is
playable from the first planet onwards, but it is not finished: see [Status](#status) and
[Known issues](#known-issues) before you start.

**Nothing of the game is in this download.** You need your own copy of the game (the disc). The
installer takes it from there.

## Highlights

- **Native, not emulated.** The game's code is recompiled ahead of time into a Windows executable.
- **Any resolution, and the 3D scene really is 4K.** The internal resolution is a free multiple of
  the game's 1280x720: 1080p, 1440p, 4K and beyond, with MSAA or FXAA and 16x anisotropic filtering.
  The game renders its 3D scene at 960x704 and stretches it afterwards; in 1.0 that stretch is
  undone, so the scene is sharp at 4K instead of being enlarged by a third in width.
- **Character ground shadows are back**, with reworked shadow bias.
- **New in 1.2.0: much steadier frame rate.** The renderer works on several threads, the collision
  code runs natively and video memory use is about 0.8 GB lower at 4K with MSAA x8. In a heavy fight at
  Rykan V the average went from 47-51 to 57 fps and the seconds below 45 fps from 26-30% to 2-3%.
- **New in 1.2.0: no stutter the first time an effect appears.** The shaders are built once, before
  you play (about 25 s on the first start).
- **New in 1.2.0: no more freezes on Intel 12th-14th gen CPUs** (the game runs on the P-cores), and a
  crash report plus a detailed log to make bug reports easier.
- **New in 1.1: pick your save slot.** Saving and loading show the game's own slot list, so you
  choose which save to load, overwrite or create.
- **New in 1.1: the missing voices are back.** Dialogue and ambient sound that some levels stream
  from separate files now play.
- **A 60 fps target** (the original runs at 30 fps in many places). On a Ryzen 5 3600 + RTX 3070 at
  4K with MSAA x8, 1.2.0 averages 59 fps on Cobalia, 57-58 in fights on Rykan V and on the Apogee Space
  Station, and 54 in the busiest fights in Metropolis (it was about 31 there in 1.0). See
  [Performance](#performance) and [Known issues](#known-issues).
- **Optional ray-traced ambient occlusion** on DXR 1.1 graphics cards.
- **In-game settings page** (F1, or hold SELECT on a controller): resolution, window mode, VSync,
  FPS limit, anti-aliasing, game language, button prompts, and a **Controls** tab to rebind every
  key and mouse button.
- **DualSense adaptive triggers** that follow the weapon in Ratchet's hands (click, heavy pull,
  rattle, flamethrower drag), with a kick at every shot; on by default, adjustable in F1.
- **In-game Cheats tab** (F1): god mode, infinite ammo, bolts, all weapons, the game's own cheats;
  off at every start.
- **One-click installer** that builds the game folder from your own decrypted disc dump and checks
  that it is the right disc. Nothing of the game is shipped.
- **Game language of your choice** (text, voices and subtitles that the disc offers), and button
  prompts that follow your controller: PlayStation 3/4/5, Xbox, Switch or keyboard.
- **Shaders built before you play:** the 1719 graphics pipelines the game uses are compiled once on
  the first start (about 25 s, with a progress window) and kept in `cache\`, so new areas do not
  stutter the first time an effect appears.
- **Discord Rich Presence**: the planet you are on and the time played, while Discord is running.
- **Experimental, off by default:** ultrawide (21:9 / 32:9, in `tod.ini`) and mouse camera (F1 >
  Controls); see [Experimental options](#experimental-options-off-by-default).
- **Free and open source** (MIT). The repository holds the port's own code and the scripts to
  build it from your own disc.

## Screenshots

<table>
  <tr>
    <td width="50%"><img src="docs/screenshots/01_metropolis_kerwan.jpg" alt="Metropolis (Kerwan)"><br><sub><b>Metropolis (Kerwan)</b> - the opening city, under attack</sub></td>
    <td width="50%"><img src="docs/screenshots/02_meridian_city.jpg" alt="Meridian City"><br><sub><b>Meridian City</b> - meteors over the capital</sub></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/screenshots/03_arena_imperial_fight_fest.jpg" alt="Imperial Fight Fest"><br><sub><b>Imperial Fight Fest</b> - close combat in the arena</sub></td>
    <td width="50%"><img src="docs/screenshots/04_rykan_v.jpg" alt="Rykan V"><br><sub><b>Rykan V</b> - the planet's fiery depths</sub></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/screenshots/05_space_combat.jpg" alt="Space combat"><br><sub><b>Space combat</b> - a ringed gas giant and nebula</sub></td>
    <td width="50%"><img src="docs/screenshots/06_kerchu_city.jpg" alt="Kerchu City"><br><sub><b>Kerchu City</b> - the pirate-raided city under fire</sub></td>
  </tr>
</table>

<sub>Captured at 3840x2160 internal 4K (RSX_SCALE=3), MSAA x8, 16x anisotropic filtering, then
downscaled and saved as JPEG for this page.</sub>

### Videos

<table>
  <tr>
    <td width="50%"><a href="https://www.youtube.com/watch?v=9Zxq7EdYPfs"><img src="https://img.youtube.com/vi/9Zxq7EdYPfs/hqdefault.jpg" alt="Ratchet & Clank Tools of Destruction on PC with ray tracing in 4K (video)"></a><br><sub><b>4K with ray tracing</b> - RTX 5080 + Ryzen 7 9800X3D, by ANGEL4EVA</sub></td>
    <td width="50%"><a href="https://www.youtube.com/watch?v=scb1zhBkfrg"><img src="https://img.youtube.com/vi/scb1zhBkfrg/hqdefault.jpg" alt="Ratchet & Clank Future: Tools of Destruction PC Recomp on an RTX 3070 (video)"></a><br><sub><b>On an RTX 3070</b> - by danxo</sub></td>
  </tr>
</table>

<sub>Both videos were recorded with version 1.0. Made a video with the port? Open an issue with the
link and it can be added here.</sub>

## Features

### Graphics

- Direct3D 12 renderer (feature level 12_0) driven by the console's RSX command stream.
- Internal resolution from 0.5x to 8x the game's 1280x720 (`RSX_SCALE`): 720p, 1080p, 1440p, 4K and
  beyond. It is independent of the window size.
- Anti-aliasing: FXAA or MSAA x2 / x4 / x8. Anisotropic filtering up to 16x.
- Display modes: windowed (any size), borderless window or exclusive fullscreen; VSync on or off.
- Optional **ray-traced ambient occlusion** (DXR 1.1: RTX 20 / RX 6000 series or newer), with three
  quality levels.
- Optional screen-space reflections (off by default, expensive at 4K).
- True 4K for the 3D scene: the game's 960x704 scene render is no longer stretched 4/3 in width
  (`RSX_HSTRETCH_FIX`, on by default), so Ratchet and the world are as sharp as the rest of the image.
- Character ground shadows restored and the shadow bias reworked (polygon offset in the vertex
  epilogue and a faithful border for lookups outside the shadow map).
- Rendering fixes made along the way for the bloom chain (the bloom halos are fixed), shadows and fog
  (see the [changelog](CHANGELOG.md) and [Known issues](#known-issues) for what is still open).
- A frame counter (FPS, or detailed: frame time, 1% low, GPU and CPU load and video memory) that you
  can switch on.

### Performance

- A 60 fps target (`TOD_FPS`), with an optional cap above it.
- Native fast paths for two of the game's heaviest SPU jobs (skeletal animation and collision
  queries), so they run as plain native code instead of the lifted SPU version. In 1.0 the animation
  evaluator and the collision code are faster, and 17 graphics job modules use a round-toward-zero
  fast float path.
- Reference numbers for 1.2.0 (Ryzen 5 3600 + RTX 3070, 4K, MSAA x8). Each one is an automated
  3-minute fight with all the weapons, heavier than normal play:

  | Area | Average fps | Seconds at 59-60 fps |
  |---|---|---|
  | Cobalia | 59 | 85% |
  | Rykan V | 57-58 | 56-66% |
  | Apogee Space Station | 57 | 60% |
  | Metropolis (busiest fights, ~3700 draws per frame) | 54 | 44% |

  For comparison, 1.0 ran the opening of Metropolis at about 31 fps. In normal play the numbers are
  higher: a 14-minute session at Sargasso averaged 59 fps with 88% of the seconds at 59-60, and a
  20-minute fight test at Rykan V 59 fps with 80%. Lowering the resolution helps the heaviest scenes
  little, because they are limited by the CPU.
- In 1.0 the graphics command list is recorded on its own thread, and the vertex cache is large
  enough for the busiest scenes (in Metropolis it used to be emptied every few frames, a hitch of
  about 35 ms each time).
- A pipeline (PSO) and shader cache in `cache\`: pipelines seen before are pre-built at start, to
  avoid compile stutter. About 23% fewer shader pipelines need compiling in 1.0, so the first visit
  to an area stutters less.
- The hitch when the game autosaves is shorter (about 620 ms before, about 400 ms now).
- New in 1.1: less stutter in busy scenes (the vertex cache no longer rebuilds in bursts), a faster
  graphics thread (about 2.5 ms per frame in Metropolis), faster collision (about 1 ms per frame)
  and a profile-guided build of the graphics code. Each one can be turned off in `tod.ini`.
- New in 1.2.0: the draws of each frame are prepared on three threads, the SPU collision jobs run on a
  native engine, render targets share memory blocks and offscreen passes no longer get an MSAA copy,
  frames follow the monitor's refresh, and the 1719 graphics pipelines are built before you play. See
  the [changelog](CHANGELOG.md) for the measured numbers; `TOD_PERF_DEFAULTS=0` (environment variable)
  goes back to the 1.1 behaviour.
- The game's SPU code runs on your CPU cores, so a fast 6+ core CPU matters most.

### Audio and video

- Audio with a safety margin against crackle; an experimental low-latency output is optional.
  The music streaming threads now get enough priority, so the music no longer starves.
- The output follows Windows' default audio device when it changes (optional, on by default).
- Dialogue and ambient sound that some levels stream from separate files play since 1.1 (in 1.0
  much of Cobalia's dialogue, for example, was silent). `TOD_LEVEL_STREAMS = 0` turns it off.
- Cutscenes and movies play through the game's own video decoder.
- **Skip a movie:** hold **Start** on the pad (or **Enter**, **Esc** or **Space** on the keyboard) for
  2 seconds while a pre-rendered movie plays. This works for the video cutscenes (the ones the PS3
  streams from disc), not for the scenes rendered by the game engine. `TOD_FMV_SKIP = 0` turns it off.

### Input

- Controllers through SDL2 and XInput: Xbox, DualShock 4, DualSense, Switch Pro and others.
- The button icons in the game follow the pad you use (PlayStation 3/4/5, Xbox, Switch) or the
  keyboard.
- Keyboard and mouse: every key is rebindable in F1 > Controls (written to `tod.ini` as `KEY_*`),
  including keys for the camera (the right stick), unbound by default; the experimental mouse camera
  below is the other way to steer it. See [Controls](#controls).
- **Xbox impulse triggers** (1.1): on an Xbox One / Series controller the motor in the right trigger
  kicks at every shot. On by default; F1 > System > Xbox impulse triggers (`TOD_XB_TRIGGERS`).
- **DualSense adaptive triggers**: the right trigger follows the equipped weapon (a click for single
  shots, a hard pull for heavy weapons, a rattle for automatic fire, a steady drag for the
  flamethrower and the beams) with a kick at every shot, and goes slack in menus, in pause, in
  cutscenes and when the weapon is out of ammo. On a DualSense the right trigger (R2) fires and R1
  takes R2's role, so you fire with the trigger that has the motor (`TOD_DS_FIRE_R2 = 0` restores the
  PS3 layout). `TOD_DS_TRIGGERS`, `TOD_DS_TRIGGER_STRENGTH` and `TOD_DS_FIRE_R2` are also in F1 >
  System. The trigger effects are from [RACiT](https://github.com/HighDrexler/RACiT) (MIT).

### Experimental options (off by default)

Both are switched on in `tod.ini` (next to `tod.exe`): remove the `#` at the start of the line and
set the value, then restart the game (the mouse camera also from F1 > Controls, at once). They are not as well tested as the rest.

- **Ultrawide**: `TOD_ASPECT = 21:9` (or `32:9`, or `auto` to take the shape of the window). The 3D
  view widens (same vertical field of view, you see more at the sides). Set the window to the same
  shape, e.g. `RSX_WINDOW = 3440x1440`, and use `RSX_SCALE` 2-3. Since 1.2.0 the HUD and the 2D menus
  stay 16:9 in the middle of the screen; cutscenes stay 16:9 with bars.
- **Mouse camera**: `TOD_MOUSE = 1`, or F1 > Controls > Mouse camera. The mouse drives the right stick (camera and aim); left button =
  R1 (fire), right button = L1 (strafe / lock-on), middle button = Triangle (quick select), wheel =
  D-pad left/right, `Esc` = Start. `TOD_MOUSE_SENS` (default 1.0), `TOD_MOUSE_INVERT_Y` and
  `TOD_MOUSE_WHEEL = off` fine-tune it (the mouse buttons can be reassigned in F1 > Controls). Because it feeds the right stick, the top turning speed is
  the game's own Camera Speed option: raise it in the game's Options. F1 releases the cursor.

### Saves, language and installation

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

## Requirements

| | |
|---|---|
| OS | Windows 10 or 11, 64-bit |
| CPU | **AVX2 and FMA** (Intel Haswell / 4th gen Core or newer, any AMD Ryzen). On older CPUs the game shows a message at start saying the CPU is not supported. A fast 6+ core CPU matters most: the game's SPU code runs on your CPU cores. |
| GPU | Direct3D 12 (feature level 12_0). Optional ray-traced ambient occlusion needs DXR 1.1 (RTX 20 / RX 6000 or newer). 8 GB of video memory is comfortable at 1080p-1440p. |
| RAM | 16 GB recommended |
| Disk | About 24 GB free for the installed game files, plus a little for shader caches and saves |
| Software | [Microsoft Visual C++ Redistributable 2015-2022 (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe) (most PCs already have it) |
| A controller | Recommended. Any XInput or SDL2-supported pad works (Xbox, DualShock 4, DualSense, Switch Pro...). Keyboard and mouse work too: every key is rebindable in F1 > Controls, camera keys included. |
| The game | **Your own copy of the European disc, BCES00052, version 1.00**, see below |

Intel 12th gen and newer hybrid (P/E core) CPUs: since 1.2.0 the game runs on the P-cores only, which
fixes the freezes of issue #10 (`TOD_PCORES = 0` turns it off). Not tested: AMD GPUs and Intel GPUs. The port was
developed on a Ryzen 5 3600 with an RTX 3070. Reports from other hardware are very welcome.

## Which game files work

- **Ratchet & Clank: Tools of Destruction, European release, Title ID `BCES00052`, disc version
  `01.00`.** The disc is the multi-language one (English, Japanese, French, German, Spanish,
  Italian, Dutch, Portuguese, Swedish, Norwegian, Danish, Finnish, Chinese, Korean).
- Other regions (the US, Japanese and Asian releases) and the digital (PSN) versions are
  **not supported**: the port is built from the European executable. The installer checks the Title
  ID and the SHA-256 of `EBOOT.BIN` and refuses anything else.
- No game update is needed or used. Only the disc.

The installer needs the disc **decrypted**, either as

- a **decrypted `.iso`**, or
- the **folder that contains `PS3_GAME`** (a disc dump copied to a folder).

How to get one from your own disc and a Blu-ray drive that can read PS3 discs:
[PS3 Disc Dumper](https://github.com/13xforever/ps3-disc-dumper) produces a decrypted dump. A raw
redump-style `.iso` is still disc-encrypted: decrypt it first with the disc's key (for example with
ps3dec and the `.dkey` file) and then use the result. If you pick
an encrypted image, the installer tells you so and refuses it.

## Installing

1. Download `tod-recomp-1.2.0-win64.zip` from the
   [Releases](https://github.com/Re-TeRo17/ratchet-tod-recomp/releases) page and unzip it
   anywhere you like **except** `Program Files` (the game writes its saves and shader cache next to
   `tod.exe`). About 24 GB must be free on that drive.
2. Run **`Install.bat`**. (You can also drag the `.iso` or the disc folder onto `Install.bat`.)
   - Choose your decrypted `.iso` (button ".iso...") or the disc folder ("Folder...").
   - The installer checks that it is the right disc and says so in green.
   - Choose the **game language** (default English; you can change it later in the game with F1).
   - Press **Install**. It copies the disc into the `game` folder and makes `elf\EBOOT.ELF` from the
     disc's `EBOOT.BIN`. It takes a few minutes. Nothing leaves your PC.
3. Press **Play** (or run **`Play.bat`** any time later).

Windows SmartScreen may warn the first time because the program is not signed: "More info", then
"Run anyway". Some antivirus programs flag unsigned freshly built executables; the source code is
public if you want to check or build it yourself.

The installer can be run again at any time (it replaces only the game files it installed). With the
game already installed, it can also just change the language.

**Updating from an older version:**

From 1.1 on, run **`Update.bat`** in the game folder: it checks GitHub, asks before doing anything,
downloads the new version and replaces only the port's files. To update by hand instead (and to go
from 1.0 to 1.2.0, since 1.0 has no `Update.bat`):

1. Close the game.
2. Open the 1.2.0 zip. Inside there is a folder `tod-recomp-1.2.0`: copy **everything inside it** into
   your game folder (the one with `tod.exe`). When Windows asks, choose **Replace the files in the
   destination**.
3. Start the game with `Play.bat` as usual.

You do **not** need to run `Install.bat` again: the installed game (`game\`, `elf\`) stays as it is.
Your settings (`tod.ini`), your saves (`gamedata\`) and your shader cache (`cache\`) are kept,
because the download holds `tod.default.ini` instead of `tod.ini` and has no saves or cache in it.

The game tells you on the title screen and
in F1 > System > Updates when a newer version is out (`TOD_UPDATE_CHECK = 0` turns the check off;
nothing is downloaded by itself). To keep the installed game (`game\disc`) on another drive: close the game, move the
`game\disc` folder there and write its new path in `tod.ini`, for example
`PS3_VFS_ROOT = D:\Games\ToD\disc`.

Command-line equivalent: `tod_setup.exe --install --base <folder> <iso or disc folder>` (and
`tod_setup.exe --check <file>` to see what the installer makes of a file).

### Steam Deck / SteamOS (Proton, community guide)

There is no native Linux build, but the Windows build runs through Proton. These steps come from a
player's guide ([#18](https://github.com/Re-TeRo17/ratchet-tod-recomp/issues/18), tested on an ASUS ROG
Ally X with SteamOS and GE-Proton 11-12); they are not official support. See also
[#9](https://github.com/Re-TeRo17/ratchet-tod-recomp/issues/9).

1. Install GE-Proton (for example with ProtonUp-Qt from Desktop Mode).
2. Unzip the port into a folder and put your decrypted `.iso` next to `tod_setup.exe`.
3. In Desktop Mode open Konsole in that folder and run the installer through Proton (change the
   GE-Proton folder name and the `.iso` name to yours):

   ```bash
   cd "/home/deck/Games/tod-recomp"
   STEAM_COMPAT_DATA_PATH="$PWD/.steam-compat-data" STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.steam/root" STEAM_COMPAT_APP_ID=1848820163 "$HOME/.steam/root/compatibilitytools.d/GE-Proton11-12-x86_64/proton" run "$PWD/tod_setup.exe" --install --machine --base "$PWD" "$PWD/your_game.dec.iso"
   ```

   `STEAM_COMPAT_DATA_PATH` keeps the Proton prefix inside the folder, `--base "$PWD"` installs into
   that folder.
4. Add `tod.exe` to Steam as a Non-Steam Game, set its compatibility tool to GE-Proton in the game's
   Properties, and start it from Gaming Mode.

### What is installed where

```
tod.exe, SDL2.dll, z.dll   the port (released with this download)
tod.default.ini            the default settings (from the download)
tod.ini                    your settings, made from tod.default.ini at the first start (also written by F1)
game\disc\                 a copy of your disc (PS3_GAME, PS3_DISC.SFB)   <- from your disc
elf\EBOOT.ELF              the decrypted game executable                  <- from your disc
cache\                     shader cache, built by the game while you play
gamedata\                  your saves
mods\                      optional, yours: files the game reads instead of its own (see Features)
log\tod.log                the log of the last run (Play.bat), tod.previous.log the one before
```

The game executable is decrypted on your PC from your own disc. This download contains no part of
the game: no executable, no data, no shaders, no saves.

## Settings

Edit `tod.ini` (every line is explained inside), or press **F1** in the game (on a controller: hold
**SELECT** for two seconds) to open the settings page: internal resolution, window mode, VSync, FPS
limit, anti-aliasing, anisotropic filtering, FPS counter, ray-traced ambient occlusion, the **game
language**, and the button prompts (PlayStation 3/4/5, Xbox, Switch, keyboard). "Apply and save"
writes `tod.ini`; some rows apply at once, others ("requires restart") at the next start.

Defaults of this release: 1080p internal resolution (`RSX_SCALE = 1.5`), MSAA x4, anisotropic x16,
borderless window, VSync on, 60 fps target, English.

| Key | Meaning |
|---|---|
| `RSX_SCALE` | Internal resolution as a multiple of 1280x720: 1 = 720p, 1.5 = 1080p, 2 = 1440p, 3 = 4K (range 0.5 to 8). This is the biggest performance lever. |
| `RSX_AA` | `off`, `fxaa`, `msaa2`, `msaa4`, `msaa8`. MSAA x8 at 4K is heavy and not recommended below an RTX 3080. |
| `RSX_ANISO` | Anisotropic filtering, 1 to 16 (default 16). |
| `RSX_DISPLAY`, `RSX_WINDOW` | `windowed` (with a size such as `1920x1080`), `borderless`, `fullscreen`. |
| `RSX_VSYNC` | 1 = sync to the monitor (default), 0 = off. |
| `TOD_FPS` | A cap (30, 60, 120...) or `lock60`. Above 60 VSync is switched off by itself and the physics uses a smaller time step. 60 is what the port is tuned for. |
| `TOD_FPS_COUNTER` | 0 = off, 1 = FPS, 2 = detailed (FPS, frame time, 1% low of the last 5 s, GPU and CPU load, video memory). |
| `RT_AO`, `RT_AO_QUALITY` | Optional ray-traced ambient occlusion (DXR 1.1 GPU), off by default; quality 0 (low), 1 (medium, default) or 2 (high). |
| `RSX_SSR` | Optional screen-space reflections, off by default (costs a lot at 4K). |
| `TOD_PROMPTS` | Button icons: `auto` (follows the last used device), `ps3`, `ps4`, `ps5`, `xbox`, `switch`, `keyboard`. |
| `PS3_LANG` | Game language, see below. |
| `AUDIO_LATENCY_MS`, `AUDIO_LOWLAT` | Audio safety margin (default 32 ms) and the experimental low-latency output (default off, the stable choice). |
| `TOD_SAVE_PICKER` | 1.1: the save slot list when saving and loading (on by default; 0 = off). |
| `TOD_LEVEL_STREAMS` | 1.1: the dialogue and ambient sound some levels stream from separate files (on by default; 0 = off). |
| `RSX_VCKEY_NODEF`, `RSX_TEXMASK_MEMO`, `RSX_FPSCAN_MEMO`, `TOD_COLL_HAND` | 1.1 speed-ups, all on by default. If something looks wrong on your PC, set one to 0 at a time and tell me which one helped. |
| `TOD_PERF_DEFAULTS` | 1.2.0 speed-ups (multi-threaded renderer, native collision, video-memory savings, frame pacing), all on by default. `TOD_PERF_DEFAULTS=0` as an environment variable goes back to 1.1; each one also has its own key (`RSX_LANES`, `RSX_RT_HEAP`, `RSX_VRAM_GOV`, `RSX_PACE_VBLANK`...). |
| `TOD_PCORES` | 1.2.0: on Intel CPUs with P-cores and E-cores the game runs on the P-cores only (on by default; 0 = off). |
| `TOD_SHADER_PREWARM` | 1.2.0: 0 skips building the shipped pipelines before the game starts. |
| `RSX_CROP_BARS` | 1.2.0: 8 cuts the thin black bars at the top and bottom (as on the PS3) and stretches the picture 2.3% taller (off by default). |
| `TOD_UPDATE_CHECK`, `TOD_VERSION_TAG` | 1.1: check for a newer version at start, and the version shown on the title screen (both on; 0 = off). |
| `PS3_VFS_ROOT` | Where the installed game is, if you moved the `game\disc` folder to another drive (for example `D:\Games\ToD\disc`). Empty = `game\disc` next to `tod.exe`. |

### Language

The language is what the console would report: it selects the **text, the voices and the
subtitles** the disc offers for it. The installer asks for it; you can change it in F1 > System
(applies at the next start) or by editing `PS3_LANG` in `tod.ini`. Values: `english`, `english_gb`,
`french`, `german`, `spanish`, `italian`, `dutch`, `portuguese`, `swedish`, `norwegian`, `danish`,
`finnish`, `korean`, `chinese_t`, `japanese`. (Not every language has its own spoken dub; the game
falls back to what the disc contains.)

### Frame rate and shader stutter

- The game was made for 30 fps in many places. `TOD_FPS = 60` runs the game at 60 fps; above 60 the
  physics step is reduced and some effects may misbehave. Please report problems you see at 60 first.
- **Shaders are compiled before you play.** Since 1.2.0 the download includes `shaders\pipemap.bin`, a
  list of 1719 graphics pipelines (numbers only, nothing of the game). On the first start, or after a
  driver update, the game builds them once with a progress window (about 25 s), so new areas no longer
  stutter the first time an effect appears. The result is kept in `cache\`. (A cache itself is not
  shipped, because it would contain the game's own shaders.) Do not delete `cache\` unless
  asked to.

## Controls

A controller is recommended; it is read through SDL2 (and XInput), so most pads work. The game's
own control scheme is used; on a DualSense the right trigger fires (see [Input](#input)). The
default keyboard layout is below; every key can be changed in **F1 > Controls** (Enter or click a
row, then press the new key), and keys for the camera (the right stick) can be added there:

| PS3 | Keyboard |
|---|---|
| D-pad / left stick | Arrow keys |
| Cross / Circle / Square / Triangle | `Z` / `X` / `A` / `S` |
| L1 / R1 | `Q` / `W` |
| L2 / R2 | `1` / `2` |
| Start / Select | `Enter` / `Tab` |
| Settings page | `F1` (controller: hold Select for 2 s) |
| Quit | the window's close button, `Alt+F4` or `Shift+Esc` (`Esc` alone does not quit) |

## Status

| Area | State |
|---|---|
| Boot, menus, saving and loading | Works |
| Playing through the planets | Works. Every level loads; the early game has been played a lot, the later planets less |
| Frame rate | 60 fps in most places at 1080p. At 4K with MSAA x8 on an RTX 3070 / Ryzen 5 3600 (1.2.0): average 59 fps over a 14-minute session at Sargasso, 57 fps in a heavy fight at Rykan V; the busiest fights (more than about 3000 draws per frame) can drop to about 50 fps |
| Graphics | Direct3D 12 renderer, any internal resolution, MSAA/FXAA, anisotropic filtering, optional ray-traced ambient occlusion. A Vulkan renderer exists but is experimental (`RSX_BACKEND = vulkan`) |
| Audio and music | Works. Low-latency output is off by default for stability |
| Cutscenes / movies | Play (the game's own video decoder) |
| Controllers | Works (Xbox, PlayStation, Switch pads); the button icons follow the pad. DualSense adaptive triggers per weapon (not yet tested on many pads) |
| Keyboard and mouse | Works; every key rebindable in F1 > Controls. The mouse camera is experimental and off by default |
| Save data | Works. Saves are in `gamedata\dev_hdd0\home\00000001\savedata` (F1 > System > Saves folder opens it; `PS3_SAVEDATA_ROOT` in `tod.ini` moves it), one `BCES00052_SAVE_<n>` folder per save in the layout RPCS3 uses. Copying saves to and from RPCS3 should work but is not tested yet. Not compatible with a real console (saves are not encrypted) |
| Online / PSN features | Not available (the game has none that matter) |
| Other regions / PSN versions | Not supported |
| Linux / Steam Deck | No native build; the Windows build runs through Proton (see [Steam Deck / SteamOS](#steam-deck--steamos-proton-community-guide)) |

## Known issues

Version 1.2.0 is playable from the first planet onwards, but a few rough edges remain.
Everything below is on the list for the next updates. If you run into something that is not here,
please [report it](#reporting-bugs).

- **The very busiest fights can still drop below 60 fps.** On the reference PC (Ryzen 5 3600 + RTX
  3070, 4K with MSAA x8) most of the game holds 59-60 fps, but scenes with more than about 3000 draws
  per frame (big fights in Metropolis, for example) can drop to about 50. They are limited by the CPU,
  so a lower resolution helps them little.
- **Video memory:** if other programs use a lot of the GPU's memory, Windows gives the game less, and a
  short stutter can happen when a heavy scene starts.
- **Weapon vendor and weapons menu:** they flicker briefly every 1.5 seconds (being worked on).
- **Clank's rocket boost:** its smoke can appear late the first time it is used.
- **Short pauses** on autosave (about 0.4 s, down from 0.6 s) and while a level or a cutscene loads.
- **Audio:** an occasional short crackle, mostly while the frame rate dips or an area loads. Raising
  `AUDIO_LATENCY_MS` in `tod.ini` (for example to 48) may help.
- **Water:** a few surfaces can be invisible or incomplete (for example the large round pool on
  Fastoon), and underwater can show as a flat colour.
- **Shadows:** the character ground shadows are back in 1.0, but an occasional artefact can still
  appear in some scenes. (Small plants and crates casting no shadow is how the PS3 original looks
  too.)
- **Apogee holograms:** the platforms and lasers that vanished against the sky are fixed in 1.0. If
  you still find one, please tell me where.
- **Very long sessions:** a rare freeze has been reported after many hours of play. 1.2.0 fixes two
  causes of freezes (Intel hybrid CPUs, and the character animation job getting stuck); if it still
  happens to you, please send `log\tod.log` and `log\tod_v2_*.jsonl`. Saving now and then is a good
  habit.
- **CPUs without AVX2** (Intel 3rd gen and older) are not supported yet
  ([#7](https://github.com/Re-TeRo17/ratchet-tod-recomp/issues/7),
  [#15](https://github.com/Re-TeRo17/ratchet-tod-recomp/issues/15)).
- **Experimental options**, off by default: ultrawide (`TOD_ASPECT`: the 3D view widens, the
  HUD and menus stay 16:9 in the middle and cutscenes stay 16:9) and the mouse camera (`TOD_MOUSE`: it drives
  the right stick, so its top speed is the game's Camera Speed option).
- **Hardware not tested yet:** AMD and Intel GPUs. Reports are very welcome.

The current list of reported problems is on
[GitHub Issues](https://github.com/Re-TeRo17/ratchet-tod-recomp/issues). Please read it before
filing a new report.

## Roadmap

What comes next, roughly in this order:

- The port's settings in the game's own Options menu (next update, 1.2.1).
- A rock-solid 60 fps in the busiest fights too.
- Fixes for the visual problems listed under [Known issues](#known-issues), starting with the
  flicker in the weapon vendor screen.
- A simpler installer, without the `.bat` files.
- More polish, and fixes for what you report.

## Reporting bugs

Please use [GitHub Issues](https://github.com/Re-TeRo17/ratchet-tod-recomp/issues) and the
bug report form. The more of this you include, the faster it can be fixed:

1. **What happened**, what you expected, and **where** (planet / level name, what you were doing).
2. **A screenshot or short clip** if it is visual (Windows' Win+Alt+PrtScn or Xbox Game Bar, or
   `Win+Shift+S`).
3. **Your setup**: Windows version, CPU, GPU and driver version, RAM, the release version (1.2.0), and
   which disc dump you used (decrypted `.iso` or folder).
4. **Your settings**: attach `tod.ini` (resolution scale, MSAA, FPS limit, language...).
5. **The log**: `log\tod.log` from right after the problem (before you start the game again: the
   previous run is kept as `log\tod.previous.log`). If the game crashed or froze, the last lines are
   the most useful part. Also attach `log\tod_v2_<date>.jsonl` (the detailed log: press **F7** in game
   at the moment something goes wrong to mark it) and, if the game crashed, `log\tod_crash.txt`.

Do **not** attach game files, disc images or saves from other people. A `gamedata\...\savedata`
folder is fine if it is your own and the bug depends on it.

## Building from source

This repository contains the port's own code (runtime glue, installer, settings page, tools) and the
build scripts. It does **not** contain any code or data generated from the game: the game's PowerPC
(PPU) and SPU code is translated from **your own** `EBOOT.ELF` on your machine, into the git-ignored
folders `recompiled\` and `spu\`. You need your own copy of the disc (European release, BCES00052,
v1.00) to build a working `tod.exe`.

The released `tod.exe` is built the same way (plus the optional step 7 below).

### 1. Tools

| | |
|---|---|
| Windows 10/11 x64 | The build is Windows-only (Direct3D 12). |
| Visual Studio 2022 or newer, or its Build Tools | "Desktop development with C++" workload with the Windows SDK (provides the MSVC headers, `lib.exe`, `vcvars64.bat`). |
| [LLVM](https://releases.llvm.org) with `clang-cl` | The lifted code is compiled with clang-cl (MSVC cl.exe is not supported for it). In `PATH` or `C:\Program Files\LLVM`. |
| [CMake](https://cmake.org) 3.20+ and [Ninja](https://ninja-build.org) | `winget install Kitware.CMake Ninja-build.Ninja` |
| Python 3.11+ | For the lifter and the tools (a virtual environment is created below). |
| [vcpkg](https://vcpkg.io) | `vcpkg install zlib:x64-windows sdl2:x64-windows`. Expected in `C:\vcpkg`, or set `VCPKG_ROOT`. |
| Git | |

Plan for about 16 GB of RAM (the generated source files are 30-50 MB each) and about 5 GB of disk for
`recompiled\`, `spu\` and `build\`. From a clean checkout the whole procedure (steps 3-6) took about
16 minutes on a Ryzen 5 3600 (6 cores): PPU lift ~7 min, SPU lift <1 min, compile and link ~8 min.

### 2. Get the source

`ps3recomp` (the runtime this port is built on) is a git submodule, a fork of
[soyjxck/ps3recomp](https://github.com/soyjxck/ps3recomp) that carries this port's runtime changes:

```powershell
git clone --recurse-submodules https://github.com/Re-TeRo17/ratchet-tod-recomp.git
cd ratchet-tod-recomp
```

(If you already cloned without `--recurse-submodules`: `git submodule update --init`.)

### 3. Python environment

```powershell
python -m venv .venv
.venv\Scripts\pip install -r ps3recomp\requirements.txt
```

### 4. The game executable (`elf\EBOOT.ELF`)

The lifter works on the decrypted ELF of the game's `EBOOT.BIN`. Take it from your own **decrypted**
disc dump (the folder that contains `PS3_GAME`, or a decrypted `.iso`: see
[Which game files work](#which-game-files-work)). Either

- use the installer of the release (`tod_setup.exe --install --base . <iso or disc folder>`), or the
  built `tod.exe --install <iso or disc folder>` once you have one; both also copy the disc into
  `game\disc`, which you need to play; or
- decrypt the file yourself: `.venv\Scripts\python tools\self2elf.py <path to PS3_GAME\USRDIR\EBOOT.BIN> elf\EBOOT.ELF`.

The result must be the ELF with SHA-256 `cd2c37351e840fb3...` (the installer checks this for you).
`elf\` is git-ignored.

### 5. Generate the code from the ELF

```powershell
powershell -ExecutionPolicy Bypass -File tools\lift_ppu.ps1   # PPU: elf\EBOOT.ELF -> recompiled\ (minutes, ~600 MB of C++)
powershell -ExecutionPolicy Bypass -File tools\lift_spu.ps1   # SPU: policy module, 21 job modules, 2 raw collision programs -> spu\
```

`lift_ppu.ps1` runs `ps3recomp\tools\ppu_loader.py` and `ppu_lifter.py` (function discovery, then
translation to C++) and computes `--code-end` from the ELF's section headers. `lift_spu.ps1` extracts
the SPU programs embedded in the ELF, lifts them with `ps3recomp\tools\spu_lifter.py` and writes the
registries the build links. Both are re-runnable (`lift_spu.ps1 -Force` redoes everything).

### 6. Configure and build

```powershell
tools\build_win.bat            # = configure + build; output: build\tod.exe, SDL2.dll, z.dll, tod.ini
```

`tools\build_win.bat` finds Visual Studio (`vswhere`), clang-cl, CMake, Ninja and vcpkg, then runs

```
cmake -S . -B build -G Ninja -DRECOMP_DIR=<repo>/recompiled -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build
```

Useful options: `-DSPU_DIR=<dir>` (where the lifted SPU images are, default `spu\`),
`-DPS3RECOMP_DIR=<dir>` (use another ps3recomp checkout), `-DTOD_SYMBOLS=OFF` (no debug info; much
smaller `tod.exe`), `-DTOD_CONSOLE=OFF` (windowed program, no console), `-DTOD_X86_LEVEL=` /
`-DTOD_SPU_X86_LEVEL=` (the default `x86-64-v3` needs AVX2/FMA; `x86-64-v2` is the SSE4.1 level for
the SPU code). `build_win.bat build` repeats only the compile step. Build output paths are not
embedded in the executable (`-fmacro-prefix-map`, `/PDBALTPATH`).

### 7. Optional: dynamic SPU overlays (speed)

Some of the game's SPU code is loaded at run time and cannot be found in the ELF. Without extra
work those regions run in ps3recomp's SPU interpreter (correct, but slower). The release build has
them lifted too, from regions captured while playing:

```powershell
$env:SPU_DUMP_UNLIFTED = "spu_dumps"        # then play through the game's levels once; the runtime saves the regions it had to interpret
build\tod.exe
.venv\Scripts\python -I tools\lift_dynovl.py --dumps spu_dumps --out spu      # lift them; writes spu\dynovl_register.c
tools\build_win.bat                          # re-run CMake: the new images are picked up
```

The release also uses two **native fast paths** for the game's heaviest SPU jobs: skeletal animation
(`SPU_HLE_WID0`, on by default; `=0` turns it off) and the collision tickets (`SPU_COLL_SYNC`, on by default;
`=0` turns it off). They need source files that are a mechanical translation of SPU functions of the game
(`ps3recomp\runtime\spu\anim_gen_*.inc`, `spu_coll_l2_gen_*.inc`, `spu_coll_l2_img.inc`, `spu_coll_sync_regs.inc`) and
small hooks inserted into the lifted jobs. All of that is game-derived, so it is not in the repository: without it
CMake switches the collision path off by itself (message `TOD_COLL_SYNC: generated collision tables not present`)
and the animation code refuses every command, so a build from this source runs the game's own lifted animation and
collision SPU programs instead (same result, slower: about 2.4 ms per frame in animation and about 1.4 ms in collision
in dense scenes such as Ardolis).

The release's lifted job modules are also post-processed with `tools\rz109_apply.py <in spu_recomp.c> <out spu_recomp.c>`
(float-heavy loops and functions of the SPURS job modules run with the host's round-toward-zero float operations;
`RZ109=0` turns it off at run time, `--check` compares every operation with the exact path). The patched modules are
generated code, so they are not published either; without the step the exact path is used.

`spu_dumps\` is git-ignored (it is game code). Other optional post-lift helpers, only for profiling and
debugging: `tools\patch_ppu_timeline.py` (TOD_TIMELINE hooks), `tools\patch_ppu_hints.py` (PPU pause
hints), `tools\fntrace_patch.py` (function tracing).

### 8. Run

Install the game next to the built executable (`build\`), as the release does:

```powershell
cd build
.\tod.exe --install ..\path\to\disc.iso      # or the disc folder; --check <file> to see what it would do
.\tod.exe
```

or copy `build\tod.exe`, `SDL2.dll`, `z.dll`, `tod.ini` and the files of `release\` into a folder
and use `Install.bat` / `Play.bat` as in the release.

### What is where

```
src\                  the port: main.cpp (loader, frame clock), install\ (disc installer), settings.cpp (tod.ini),
                      ui\ (F1 settings page, Dear ImGui), patches\ (button prompts, SIXAXIS), input\, kernel\, stubs\
tools\                lift_ppu.ps1, lift_spu.ps1, lift_dynovl.py, tod_spu.py, self2elf.py, build_win.bat, build_setup.bat (tod_setup.exe), ...
tools\analysis\       small read-only analysis helpers used while reverse engineering (need your own extracted data)
release\              Install.bat, Play.bat and the PowerShell scripts shipped in the release zip
docs\                 developer notes (partly in Italian): SPU inventory, raw SPU collision programs, frame rate, camera, textures...
third_party\imgui\    Dear ImGui 1.92.9b (MIT)
ps3recomp\            git submodule: the recompiler and runtime (fork of soyjxck/ps3recomp, MIT)
```

## Credits

- [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet: the recompiler, the runtime and
  the PS3 library implementations. This port's runtime is a fork of
  [soyjxck/ps3recomp](https://github.com/soyjxck/ps3recomp).
- [dod3-recomp](https://github.com/soyjxck/dod3-recomp) by soyjxck: the model of this port's
  installer, settings file and settings page.
- [RPCS3](https://rpcs3.net): the reference for much of the PS3's behaviour.
- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut: the settings page.
- [RACiT](https://github.com/HighDrexler/RACiT) by HighDrexler: the DualSense trigger effects and the
  Discord Rich Presence code are adapted from it (MIT).
- Other third-party components and their licences: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Legal

This project is not affiliated with or endorsed by Sony Interactive Entertainment, Insomniac Games
or any other rights holder. *Ratchet & Clank* and *Tools of Destruction* and all of the game's
assets belong to their owners. None are included in this download or repository, and the port only
runs from a copy of the game you own. You are responsible for dumping your own disc legally in your
country.

The code of this project is released under the [MIT License](LICENSE). It builds on ps3recomp (MIT)
and includes Dear ImGui (MIT) and, through ps3recomp, an ATRAC3 decoder under the LGPL-2.1+; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Support the project

If this is useful to you, you can [buy me a coffee](https://buymeacoffee.com/tero17). It is
entirely optional: the port is and stays free.

<a href="https://buymeacoffee.com/tero17"><img src="docs/images/support_coffee.png" alt="Buy me a coffee" width="300"></a>

- Bugs and questions: [GitHub Issues](https://github.com/Re-TeRo17/ratchet-tod-recomp/issues)
- Downloads: [Releases](https://github.com/Re-TeRo17/ratchet-tod-recomp/releases)

### Supporters

A big thank you to the people who support the project on Buy Me a Coffee:

- **7GP**

## A note from the developer

Ratchet & Clank was my first video game series, and I have loved its PlayStation 3 chapters ever
since. It always bothered me to see them stuck on a single, ageing console, so I wanted everyone to
be able to enjoy this wonderful series at its best: natively on PC, at high resolution and 60 fps.

This is a passion project, made in my spare time, so I will keep improving it whenever I can. If you
would like to support it, you can [buy me a coffee](https://buymeacoffee.com/tero17).

A big thank you to [dod3-recomp](https://github.com/soyjxck/dod3-recomp) and
[RACiT](https://github.com/HighDrexler/RACiT): ideas and code from both went into the latest fixes of
this port.

Once this one is finished, I would love to bring the other PlayStation 3 Ratchet & Clank games to PC
as well.

Thank you for playing.
