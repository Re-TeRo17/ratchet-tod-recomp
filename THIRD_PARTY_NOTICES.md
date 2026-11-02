# Third-party notices

Ratchet & Clank: Tools of Destruction Recompiled includes or links the following. Each keeps its own licence.

| Component | Used for | Licence |
|---|---|---|
| [ps3recomp](https://github.com/sp00nznet/ps3recomp) (a modified fork is the `ps3recomp` submodule), (c) 2026 Ned Heller / sp00nznet | The runtime, the PS3 library implementations and the RSX draw engine | MIT (text below) |
| [dod3-recomp](https://github.com/soyjxck/dod3-recomp) by soyjxck and contributors | The settings loader (`tod.ini`), the first-run installer (`src/install/`) and the settings page, adapted | MIT (text below) |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.92.9b, (c) 2014-2026 Omar Cornut (`third_party/imgui`) | The in-game settings page | MIT (text below) |
| ATRAC3 / ATRAC3+ decoder from FFmpeg, as extracted by [PPSSPP](https://github.com/hrydgard/ppsspp) (`ps3recomp/third_party/at3_standalone`) | Decoding the game's ATRAC audio | **LGPL-2.1-or-later**, see below |
| stb_image, stb_image_write, stb_truetype by Sean Barrett (in `ps3recomp/libs`) | PNG/JPEG decoding and font rendering for the PS3 library implementations | Public domain / MIT (see each file) |
| [SDL2](https://libsdl.org), (c) 1997-2025 Sam Lantinga (`SDL2.dll`) | Controllers | zlib licence |
| [zlib](https://zlib.net), (c) 1995-2024 Jean-loup Gailly and Mark Adler (`z.dll`) | Decompression | zlib licence |
| Microsoft Visual C++ runtime, DirectX 12 / DXGI / D3DCompiler | Not distributed here: they come with Windows and with the Redistributable you install | Microsoft terms |

## The ATRAC3 decoder (LGPL-2.1-or-later)

`tod.exe` contains the LGPL-licensed ATRAC3/ATRAC3+ decoder, statically linked. As the LGPL asks, the complete
corresponding source code of the program (including that decoder, unmodified, in its own directory) is published at
https://github.com/Re-TeRo17/ratchet-tod-recomp and in the `ps3recomp` fork it points to, together with the build
scripts, so that you can rebuild `tod.exe` with a modified decoder. The licence text:
https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html (also in `ps3recomp/third_party/at3_standalone/COPYING.LGPLv2.1`
in the source).

## Button prompt images

The button-prompt atlases for other pads (`src/res/prompts`, embedded in `tod.exe`) are drawn from scratch by
`tools/prompt_atlas.py`; only a few text labels ("L1", "OPTIONS", "Enter"...) are rasterised from Segoe UI Bold.
None of the game's art is used.

## Keys

`src/install/keys_builtin.h` holds the publicly known PS3 application key set for SELF key revision 0x0001 (as in
RPCS3's key vault). It is used only by the installer, to turn the player's own `EBOOT.BIN` into `EBOOT.ELF` on the
player's PC. No game data is distributed.

## Licence texts

### ps3recomp (MIT)

MIT License

Copyright (c) 2026 Ned Heller / sp00nznet

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### dod3-recomp (MIT)

MIT License

Copyright (c) 2026 soyjxck and the dod3-recomp contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### Dear ImGui (MIT)

The MIT License (MIT)

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### SDL2 and zlib (zlib licence)

This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held liable
for any damages arising from the use of this software.

Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it
and redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you
   use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
