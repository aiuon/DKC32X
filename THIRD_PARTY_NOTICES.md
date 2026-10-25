# Third-party notices

Most of this repository is the DKC32X project's own work, under the licence in [LICENSE](LICENSE)
(PolyForm Noncommercial 1.0.0 with additional terms). The parts listed here come from, or are based
on, other people's work and keep **their own licences**. The additional terms in LICENSE do not
restrict anything these licences allow.

This repository contains **no data from Donkey Kong Country** (no code, graphics, sound, levels or
text from the game). Every byte of game data is read from the user's own ROM on the user's own PC
when the port is built.

Contents:

1. [32X startup code (sh/start.s)](#1-32x-startup-code-shstarts) - MIT
2. [32X security / boot code (md/blob.inc)](#2-32x-security--boot-code-mdblobinc) - Sega's, see below
3. [Donkey Kong Country 1 disassembly by Yoshifanatic1](#3-donkey-kong-country-1-disassembly-by-yoshifanatic1) - GPL-3.0 (tools/meta/layout.json.gz)
4. [libretro API header (tools/emu/libretro.h)](#4-libretro-api-header-toolsemulibretroh) - MIT
5. [Programs bundled with the Windows builder (tools/win/licenses/)](#5-programs-bundled-with-the-windows-builder-toolswinlicenses) - GPL-3.0 + GCC Runtime Library Exception, snes9x licence, PSF, BSD, HPND
6. [Emulator patches in devkit/patches/](#6-emulator-patches-in-devkitpatches)
7. [tools/emu/harness.exe](#7-toolsemuharnessexe)
8. [References used during development (not included)](#8-references-used-during-development-not-included)

---

## 1. 32X startup code (sh/start.s)

`sh/start.s` (ROM header, 32X "MARS" header, SH-2 start-up and exception code) is based on
**Chilly Willy's 32X support code** as distributed with **marsdev** by Andrew DeRosier
(<https://github.com/andwn/marsdev>, `examples/32x-skeleton/sh_src/mars_start.s`; marsdev describes
its 32X skeleton as based on ammianus's 32xHomebrew example). It has been heavily extended for this
project. marsdev is distributed under the MIT License:

```
MIT License

Copyright (c) 2023 Andrew DeRosier

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
```

## 2. 32X security / boot code (md/blob.inc)

`md/blob.inc` is the standard 32X 68000 security / start-up code (the "MARS Initial & Security
Program", sometimes called the ICD_MARS start-up blob) that the 32X BIOS requires at ROM offset
0x3F0. It was taken verbatim from marsdev's skeleton (`examples/32x-skeleton/md_src/md_start.s`).
It was originally written by **Sega** (it carries the text "Copyright SEGA ENTERPRISES,LTD. 1994").

It is included only because a 32X console refuses to start a cartridge without it; every 32X
program contains this same block, and including it is standard homebrew practice. It is **not**
covered by this project's licence, and no licence from Sega is claimed. The same block also ends
up inside the pre-built 68000 program `tools/prebuilt/md.bin` (built from `md/boot.s`, `md/main.c`
and `md/blob.inc`).

## 3. Donkey Kong Country 1 disassembly by Yoshifanatic1

<https://github.com/Yoshifanatic1/Donkey-Kong-Country-1-Disassembly> - licensed under the
**GNU General Public License v3.0**.

This project would not exist without it. The file formats, routines and memory layout of the game
were worked out by reading this disassembly, and the documentation and source comments throughout
this repository use its label names (for example `CODE_80C246`, `DATA_B996E6`, `GFX_Layer3_...`).

**tools/meta/layout.json.gz** is generated from the disassembly by `tools/layout.py build`
(developer step). It holds only metadata: 65816 instruction boundaries and operand sizes, label
names and addresses, the names and address ranges of the disassembly's asset table, and the layout
of the SPC700 sound engine. It contains no code or data bytes from the game (those are read from
the user's ROM). Because it is derived from the GPL-3.0 disassembly, **this file is provided under
the GNU General Public License version 3** (full text: [LICENSES/GPL-3.0.txt](LICENSES/GPL-3.0.txt)),
separately from the PolyForm-licensed code, which only reads it as a data file. Its corresponding
source is the disassembly linked above together with `tools/layout.py` and
`tools/recomp/disparse.py` in this repository.

## 4. libretro API header (tools/emu/libretro.h)

`tools/emu/libretro.h` is the unmodified libretro API header, used by the capture harness
`tools/emu/harness.c`. Its licence, quoted from the file:

```
The following license statement only applies to this libretro API header (libretro.h).

Copyright (C) 2010-2024 The RetroArch team

Permission is hereby granted, free of charge,
to any person obtaining a copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## 5. Programs bundled with the Windows builder (tools/win/licenses/)

The pre-built Windows builder (published on the Releases page, not stored in this repository's
source tree) bundles the following programs. Their licence texts and build notes are in
[tools/win/licenses/](tools/win/licenses/) and are shipped inside the builder's `licenses` folder.

| Component | In the builder | Licence | Source |
|---|---|---|---|
| GCC 13.3.0 and GNU binutils 2.42, target `sh-elf`, built for Windows (with gmp 6.3.0, mpfr 4.2.1, mpc 1.3.1) | `app\toolchain\sh-elf` | GPL-3.0 (`GPL-3.0.txt`); libgcc under the GCC Runtime Library Exception (`GCC-RUNTIME-EXCEPTION.txt`); gmp / mpfr / mpc (linked into the compiler) under the LGPL-3.0 (`LGPL-3.0.txt`) | unmodified upstream releases from <https://ftp.gnu.org/gnu/>, built with `toolchain-build_win_sh.sh`. If you pass the toolchain on, the GPL asks you to offer these source archives too. |
| snes9x (libretro core) with `snes9x-trace.patch` (snapshot / capture hooks used by the capture step) | `app\emu\snes9x_libretro.dll` | snes9x licence (`snes9x-LICENSE.txt`): free for **non-commercial** use only | <https://github.com/libretro/snes9x> + `tools/win/licenses/snes9x-trace.patch` |
| Python 3.12 (python-build-standalone) | `app\runtime\python` | Python Software Foundation License (`app\runtime\python\LICENSE.txt`) | <https://www.python.org/>, <https://github.com/astral-sh/python-build-standalone> |
| numpy 1.26 | `app\runtime\python` | BSD licence (in its `*.dist-info` folder) | <https://numpy.org/> |
| Pillow | `app\runtime\python` | HPND licence (in its `*.dist-info` folder) | <https://python-pillow.org/> |
| `harness.exe` (the small headless libretro loader) | `app\emu\harness.exe` | this project's own code, `tools/emu/harness.c` (see section 7) | this repository |

The snes9x licence is non-commercial; this is one more reason why nothing built from this project
may ever be sold. `tools/win/licenses/snes9x-trace.patch` is a modification of snes9x and is
distributed under the snes9x licence.

## 6. Emulator patches in devkit/patches/

`devkit/` holds the developer's test and measurement scripts (not needed to build the port).
`devkit/patches/` contains patches to third-party emulators that were used for testing:

- `ares.patch` - instrumentation for the **ares** emulator (<https://ares-emu.net/>,
  <https://github.com/ares-emulator/ares>, ISC licence): profiling, frame dumps, input scripts and
  a MiSTer-like timing model. It is a modification of ares (its context lines are ares code) and is
  distributed under ares's licence:

  ```
  ares

  Copyright (c) 2004-2025 ares team, Near et al

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
  ```
- `snes9x_trace.patch` - a development version of the snes9x trace hooks (see section 5),
  distributed under the snes9x licence (non-commercial).

The emulators themselves are not included.

## 7. tools/emu/harness.exe

`tools/emu/harness.exe` is `tools/emu/harness.c` (this project's own code, using the MIT-licensed
`libretro.h`) compiled for Windows with the mingw-w64 GCC 13 cross-compiler. Like every program
built with that compiler it contains the mingw-w64 C runtime start-up code (mingw-w64 runtime
licence: mostly public domain / ZPL, see <https://www.mingw-w64.org/>) and may contain parts of
libgcc (GPL-3.0 with the GCC Runtime Library Exception, which allows this).

## 8. References used during development (not included)

These were used to develop and test the port. **None of them is included in this repository**, and
no code from them is in it, except where listed above:

- **ares** (multi-system emulator; its SH-2 / 32X model was used for testing and profiling)
- **PicoDrive** (Mega Drive / 32X emulator, libretro core)
- **MiSTer S32X core** (FPGA 32X implementation; its RTL was read to understand timing on MiSTer)
- **snes9x** (SNES emulator; used as the reference the recompiled game is compared against, and
  for the build-time captures)
- **blargg's SPC_DSP** (from snes_spc / snes9x): `sh/spcdsp.c` / `sh/spcdsp.h` and
  `tools/audio/brr.py` are this project's own re-implementation of the S-DSP; the ADSR / GAIN
  envelope and BRR decoding rules follow blargg's SPC_DSP as a reference. No code was copied.
- **marsdev** (toolchain and examples, see sections 1 and 2)
- Yoshifanatic1's **DKC1 disassembly** (see section 3)

Thank you to everyone behind these projects.

---

*Trademarks: Nintendo, Rare, Donkey Kong Country, Sega, 32X, Mega Drive and Genesis are
trademarks of their respective owners. DKC32X is an unofficial, non-commercial fan project and is
not affiliated with or endorsed by any of them.*
