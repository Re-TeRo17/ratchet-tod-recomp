# Settings and configuration reference

<a id="top"></a>

[Back to the README](../README.md) | [Features](FEATURES.md)

- [Settings](#settings) (all the `tod.ini` keys)
- [Language](#language)
- [Frame rate and shader stutter](#frame-rate-and-shader-stutter)
- [Installer, updates and moving the game folder](#installer-updates-and-moving-the-game-folder)
- [What is installed where](#what-is-installed-where)

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

## Language

The language is what the console would report: it selects the **text, the voices and the
subtitles** the disc offers for it. The installer asks for it; you can change it in F1 > System
(applies at the next start) or by editing `PS3_LANG` in `tod.ini`. Values: `english`, `english_gb`,
`french`, `german`, `spanish`, `italian`, `dutch`, `portuguese`, `swedish`, `norwegian`, `danish`,
`finnish`, `korean`, `chinese_t`, `japanese`. (Not every language has its own spoken dub; the game
falls back to what the disc contains.)

## Frame rate and shader stutter

- The game was made for 30 fps in many places. `TOD_FPS = 60` runs the game at 60 fps; above 60 the
  physics step is reduced and some effects may misbehave. Please report problems you see at 60 first.
- **Shaders are compiled before you play.** Since 1.2.0 the download includes `shaders\pipemap.bin`, a
  list of 1719 graphics pipelines (numbers only, nothing of the game). On the first start, or after a
  driver update, the game builds them once with a progress window (about 25 s), so new areas no longer
  stutter the first time an effect appears. The result is kept in `cache\`. (A cache itself is not
  shipped, because it would contain the game's own shaders.) Do not delete `cache\` unless
  asked to.

<sub><a href="#top">back to top</a></sub>

## Installer, updates and moving the game folder

The installer can be run again at any time (it replaces only the game files it installed). With the
game already installed, it can also just change the language.

The game tells you on the title screen and in F1 > System > Updates when a newer version is out
(`TOD_UPDATE_CHECK = 0` turns the check off; nothing is downloaded by itself). To keep the installed
game (`game\disc`) on another drive: close the game, move the `game\disc` folder there and write its
new path in `tod.ini`, for example `PS3_VFS_ROOT = D:\Games\ToD\disc`.

Command-line equivalent of the installer: `tod_setup.exe --install --base <folder> <iso or disc folder>`
(and `tod_setup.exe --check <file>` to see what the installer makes of a file).

<sub><a href="#top">back to top</a></sub>

## What is installed where

```
tod.exe, SDL2.dll, z.dll   the port (released with this download)
tod.default.ini            the default settings (from the download)
tod.ini                    your settings, made from tod.default.ini at the first start (also written by F1)
game\disc\                 a copy of your disc (PS3_GAME, PS3_DISC.SFB)   <- from your disc
elf\EBOOT.ELF              the decrypted game executable                  <- from your disc
cache\                     shader cache, built by the game while you play
gamedata\                  your saves
mods\                      optional, yours: files the game reads instead of its own (see FEATURES.md)
log\tod.log                the log of the last run (Play.bat), tod.previous.log the one before
```

The game executable is decrypted on your PC from your own disc. This download contains no part of
the game: no executable, no data, no shaders, no saves.

<sub><a href="#top">back to top</a></sub>
