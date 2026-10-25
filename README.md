# DKC32X - Donkey Kong Country for the Sega 32X (build tool)

DKC32X turns **your own** Donkey Kong Country (SNES) cartridge dump into a ROM that runs on the
**Sega 32X**: in emulators, on MiSTer, and on a real Mega Drive / Genesis with a 32X and a flash cart.
The whole game is there: logos, title, file select, the island maps, every level, the final boss,
the ending and the credits, with music and battery saves.

> **This repository contains no game data and no ROMs.** It is a build tool. Everything that
> belongs to the original game is read from the ROM file you give it, on your own computer, and
> the 32X ROM it writes is for your own personal use only.

Written by **Claude** (Anthropic's AI model) with **[aiuon](https://github.com/aiuon)**, who
directed the project and play-tested every build. See [Credits](#credits).

### Why this exists

This project was made out of love for Donkey Kong Country, and out of a nerdy fascination with the
question of whether a game built around the SNES's hardware could run faithfully on wildly different
hardware: two 23 MHz SH-2 processors, a Mega Drive video chip and a 68000. It is not a product. **No
one may make money from it, and no one may distribute ROMs built with it** (see [Legal](#legal) and
[Licence](#licence)).

---

## Contents

- [Legal](#legal) - please read this first
- [What you need](#what-you-need)
- [Using the Windows builder](#using-the-windows-builder)
- [Playing](#playing)
- [Building from source (Linux)](#building-from-source-linux)
- [How it works](#how-it-works)
- [Status and known issues](#status-and-known-issues)
- [Repository layout](#repository-layout)
- [Contributing](#contributing)
- [Credits](#credits)
- [Licence](#licence)

---

## Legal

*This section explains the intent of the project in plain words. It is not legal advice. If you
are unsure whether something is allowed where you live, ask someone qualified.*

- **No game data, no ROMs.** This repository and the Windows builder contain no code, graphics,
  sound, levels or text from Donkey Kong Country. Every byte of game data comes from the ROM file
  you supply, and the conversion happens on your own PC. A scanner (`tools/scanbundle.py`) checks
  every release for data copied from the game.
- **Trademarks.** Nintendo, Rare, Donkey Kong Country, Sega, 32X and Mega Drive/Genesis are
  trademarks of their owners. This is an unofficial, non-commercial fan project. It is not
  affiliated with or endorsed by Nintendo, Rare, Sega or anyone else.
- **You need your own cartridge.** You must own an original Donkey Kong Country cartridge and
  make the ROM dump from it yourself. Whether making and using such a personal copy is allowed
  depends on the law where you live; it is your responsibility to check.
- **Please don't ask for ROMs.** Nobody here will send you one, and we can't help you find one.
  Issues, pull requests, comments or discussions that contain ROMs or links to ROMs will be
  deleted.
- **Don't share what you build.** The `.32x` file the tool writes is a converted copy of your
  cartridge. It is for your own use. Don't upload, sell or share it, or patches or save states
  made from it. Everyone builds their own from their own cartridge.
- **No money may be made from this.** No selling, no paid downloads, no bundling with paid
  products or hardware (pre-loaded flash carts, SD cards, consoles), no ad-supported
  re-uploads, no crowdfunding or patron rewards. This is a condition of the licence (see
  [Licence](#licence)).
- **Third-party parts** (the 32X start-up code, the standard 32X boot code originally written by
  Sega that every 32X cartridge must contain, the libretro header, the emulator patches, the
  toolchain and emulator core bundled with the Windows builder, and the metadata derived from
  Yoshifanatic1's GPL-3.0 disassembly) keep their own licences. They are listed in
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

## What you need

- A ROM dump of **Donkey Kong Country, USA, revision 1.0** made from your own cartridge.
  - Accepted files: `.sfc`, `.smc`, `.swc`, `.fig` or `.bin`, with or without the 512-byte copier
    header, or a `.zip` that contains one of those.
  - The md5 of the headerless 4 MB image must be `30c5f292ff4cbbfcc00fd8fa96c2de3b`.
  - **Revisions 1.1 and 1.2 and other regions are not supported yet.** The tool reads the
    cartridge's own header, tells you which version you have, and builds nothing if it isn't
    supported.
- To play: an emulator with 32X support, a MiSTer, or a 32X console with a flash cart
  (see [Playing](#playing)).

## Using the Windows builder

The easiest way. Nothing needs installing.

1. Download the **DKC32X Builder** zip from this repository's **Releases** page.
2. Unzip the whole folder somewhere you can write to (Desktop, Documents, a USB stick). Don't run
   it from inside the zip.
3. **Drag your ROM file onto `DKC32X Builder.exe`** (or double-click it, press *Browse...*, pick
   your ROM and press *Convert*). There is also `Convert (drag ROM here).bat`, which does the same
   in a text window.
4. Wait a few minutes (about 2 minutes on a slow dual-core PC). When it is done,
   `<your ROM name> (32X).32x` appears next to your ROM.

If something goes wrong:

- *"This version is not supported yet"*: your dump is not USA 1.0 (see above).
- *"checksum mismatch"*: the dump is probably damaged. Dump the cartridge again.
- Windows SmartScreen may warn about an unknown app: choose *More info* -> *Run anyway*, or use the
  `.bat` file.
- Antivirus software can slow the compile step down a lot. If it takes much longer than 10
  minutes, try adding an exclusion for the builder folder.
- After a failed run, `last conversion log.txt` in the builder folder has the whole log.

What the builder does, step by step: it checks your ROM; runs it headless in the bundled snes9x
emulator to capture the start of every level; recompiles the game's 65816 code to SH-2 C code and
compiles it with the bundled GCC; converts the levels, graphics and music; and writes the 32X ROM.

## Playing

Play the `.32x` in an emulator with 32X support (**ares**, **PicoDrive**), on **MiSTer** (S32X core),
or on a **real 32X** with a flash cart. The ROM is 14 MB, so it fits flash carts with a 15 MB
limit (Mega EverDrive Pro). A 6-button pad is recommended.

| Mega Drive pad | Does |
|---|---|
| A | Run / roll (SNES Y) |
| B | Jump (SNES B) |
| C | SNES A |
| Start | Start / pause |

- Switch on and the whole game starts. Hold **Start while switching on** to get the level viewer
  menu instead (pick any level directly).
- The pause menu also has *exit to the map* and options (for example frame skip, 60FPS MODE,
  ripple effects and level select).
- Saves (Cranky's / Candy's barrels, the file select) are kept in the cartridge's battery memory.
  In an emulator that is the `.srm` / `.sav` file next to the ROM; on MiSTer, mount a save file
  when you load the ROM.
- The same ROM works on NTSC (60 Hz) and PAL (50 Hz) consoles. On PAL it keeps the game at its
  normal speed (60 game frames a second, shown as 50 pictures a second).

## Building from source (Linux)

This is what the Windows builder does, without the bundled programs. You need:

- **Python 3** with **numpy** and **Pillow** (tkinter only for the optional window, `tools/gui.py`).
- An **SH-2 cross GCC** (`sh-elf-gcc` with binutils `sh-elf-as`, `sh-elf-objcopy`, `sh-elf-nm`).
  No C library is needed (everything is built with `-nostdlib`, linked with `-lgcc`). The Windows
  builder uses GCC 13.3.0 and binutils 2.42; marsdev's toolchain scripts build a suitable one.
  `tools/build.py` looks in `/opt/sh-elf/bin` (or `--toolchain DIR` / `DKC_TOOLCHAIN`), then on
  your `PATH`.
- *Optional:* `m68k-linux-gnu-gcc` with binutils, to rebuild the 68000 side from `md/`. Without it
  (or with `DKC_PREBUILT_MD=1`) the pre-built `tools/prebuilt/md.bin` is used.
- For the capture step: the **snes9x libretro core with the trace hooks**, and the small headless
  loader in `tools/emu/harness.c`.

Build the capture programs once:

```sh
# the headless libretro loader
mkdir -p emu
gcc -O2 -o emu/harness tools/emu/harness.c -ldl

# snes9x libretro core + trace hooks (tested with libretro/snes9x commit fae2fea0)
git clone https://github.com/libretro/snes9x ../snes9x
git -C ../snes9x checkout fae2fea08f74180759ef540ee94259213f503480
git -C ../snes9x apply "$PWD/devkit/patches/snes9x_trace.patch"
make -C ../snes9x/libretro -j4
cp ../snes9x/libretro/snes9x_libretro.so emu/
```

Then convert your ROM in one step (check ROM -> capture -> build):

```sh
python3 tools/convert.py "Donkey Kong Country (USA).sfc"
# -> "Donkey Kong Country (USA) (32X).32x" next to the ROM
```

`convert.py` looks for `emu/harness` and `emu/snes9x_libretro.so` in the repository folder
(override with `DKC_HARNESS` / `DKC_SNES9X_TRACE`). Its work files go to `work/` and `build/` and
are deleted when it is done (`--keep` keeps them).

The two steps can also be run separately, which is handy when you change the code and only want
to rebuild:

```sh
python3 tools/romcheck.py "Donkey Kong Country (USA).sfc" work/rom.sfc
DKC_SURVEY=work/survey python3 tools/capture.py work/rom.sfc work --core emu/snes9x_libretro.so --harness emu/harness
DKC_SURVEY=work/survey DKC_PREBUILT_MD=1 python3 tools/build.py work/rom.sfc out.32x --snaps work/snaps
```

Never run two builds at the same time in one folder (they share `build/`). More developer
detail (verification, profiling, the test macros) is in [docs/HANDBOOK.md](docs/HANDBOOK.md),
section 5. The older `build.sh` is the developer's original single-world build script; it expects
the development machine's paths and is kept for reference.

## How it works

A short overview; [docs/HANDBOOK.md](docs/HANDBOOK.md) has the full picture.

- **The game's own code runs on the 32X.** DKC's 65816 code is statically recompiled from your ROM
  to C (`tools/recomp/`) and compiled for the SH-2. Routines that no trace reached run in a small
  65816 interpreter (`tools/recomp/r65interp.c`). The recompiled game is checked to produce the
  same work RAM, byte for byte and frame by frame, as the SNES game in snes9x, so the controls and
  physics are the original ones.
- **Two SH-2 CPUs (23 MHz each).** The master runs the game logic and part of the drawing; the
  slave draws and plays the music. Sprites are drawn in software by both CPUs.
- **Graphics.** The SNES playfield (BG1) is drawn into the 32X's 8-bit frame buffer; the
  backgrounds BG2 and BG3 go on the Mega Drive's planes A and B, using shadow mode to reproduce the
  SNES colour maths. The 32X palette uses the SNES colours directly (both are 15-bit). The 68000
  drives the Mega Drive video chip and handles the battery saves.
- **Music.** The game's SPC700 sound driver is translated to C from your ROM at build time and runs
  on the slave SH-2, together with an emulation of the SNES sound chip (S-DSP: samples, envelopes,
  echo), played through the 32X's PWM sound.
- **Level starts** are snapshots made by running your ROM in a headless snes9x at build time.
- **Frame rate.** The game always runs at its normal speed. When a scene is too heavy for one
  picture per frame, the port runs two game frames per picture instead of slowing down. The pause
  menu's 60FPS MODE splits the work over both CPUs differently (more pictures in many levels, but
  one frame of extra input lag; off by default).
- The 32X picture is 320 pixels wide, so you see a little more of each level at the sides than on
  the SNES.

## Status and known issues

The complete game is playable from power-on to the credits. Known open issues
([docs/HANDBOOK.md](docs/HANDBOOK.md), section 8):

- Poison Pond: one line of sprite pixels is repeated at the ends of the ripple bands
  (workaround: RIPPLE OFF in the pause menu).
- Slow picture rate on MiSTer in Ropey Rampage's rain section (about 20 pictures a second) and in
  Torchlight Trouble (15-18). The game speed is still correct.
- Coral Capers / Necky's Nuts: a small fish or Necky could show a wrong tile for one frame (not
  reproduced since the 60FPS MODE fix in round I18).
- Millstone Mayhem: about 22 pictures a second on MiSTer while a big millstone is on screen.
- Only the USA 1.0 revision is supported (1.1 and 1.2 move code and data around; see docs/NOTES.md).
- The K. Rool fight has not been played to the end by the automated test bot.

Every bug that was reported and fixed so far is written up in [docs/BUGLOG.md](docs/BUGLOG.md)
(symptom, cause, fix, how it was verified). [docs/NOTES.md](docs/NOTES.md) is the full design and
measurement history.

**Please note:** the maintainer does not monitor issues regularly, so replies can take a long time
or not come at all.

## Repository layout

| Path | What it is |
|---|---|
| `sh/` | SH-2 side: start-up, rendering, music player, sprite drawing, game runtime |
| `md/` | 68000 side: Mega Drive video planes, H-interrupt effects, saves, boot code |
| `tools/` | the build tool (Python): ROM check, capture, recompiler, asset converters, ROM writer |
| `tools/recomp/` | the 65816 -> C recompiler and its runtime / interpreter |
| `tools/audio/` | SPC700 driver translator, BRR sample decoder, music packer |
| `tools/emu/` | the headless libretro loader used for the captures (`harness.c`, Windows `harness.exe`) |
| `tools/win/` | Windows builder launcher, `.bat`, user README and the licences it ships with |
| `tools/meta/layout.json.gz` | metadata from the DKC1 disassembly (GPL-3.0, see notices) |
| `tools/prebuilt/md.bin` | the pre-built 68000 program |
| `devkit/` | developer test, verification and profiling scripts, emulator patches (not needed to build) |
| `docs/` | HANDBOOK (start here), BUGLOG, NOTES |
| `build.sh` | the developer's original build script (reference) |

## Contributing

Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md). In short: contributions are
accepted under the same licence as the project, and **never attach ROM data** (ROMs, dumps, save
states, captured files) to an issue or pull request. Screenshots and logs are fine.

## Credits

- **Code and documentation:** written by **Claude**, Anthropic's AI model, with
  **[aiuon](https://github.com/aiuon)**, who directed the project and play-tested every build.
  Most entries in the bug log started as one of aiuon's detailed test reports.
- **Yoshifanatic1's [Donkey Kong Country 1 disassembly](https://github.com/Yoshifanatic1/Donkey-Kong-Country-1-Disassembly)**
  (GPL-3.0): the map that made the port possible. The docs and source comments use its label
  names (such as `CODE_80C246`).
- **Chilly Willy** and **marsdev** (Andrew DeRosier): the 32X start-up code this port's start-up
  is based on (MIT).
- **blargg**: SPC_DSP, the reference for the sound chip's envelope and sample rules (re-implemented
  here, not copied).
- **snes9x**, **ares**, **PicoDrive** and the **MiSTer S32X core**: used to build, test and measure
  the port. They are not included in this repository.
- **libretro**: the API used to run snes9x headless.

Full licence details: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Licence

The project's own code and documentation are licensed under the
**[PolyForm Noncommercial License 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0)**,
granted only together with **additional terms** (see [LICENSE](LICENSE)):

- no commercial use of any kind;
- you must not distribute ROM images (`.32x` or any other format), patches or save states made with
  this software or containing data from the original game;
- a pre-built Windows builder may only be passed on unmodified, free of charge and with its licence
  files.

`tools/meta/layout.json.gz` is under the **GPL-3.0** ([LICENSES/GPL-3.0.txt](LICENSES/GPL-3.0.txt)),
and the other third-party parts keep their own licences ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

Required Notice: Copyright (c) 2026 aiuon (https://github.com/aiuon)
