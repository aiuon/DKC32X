DKC32X Builder
==============

Turns your own Donkey Kong Country (SNES) ROM into a Sega 32X ROM.
This folder contains no game data. Everything Nintendo-owned is taken
from the ROM file you give it, on your own PC. Nothing is downloaded and
nothing needs installing.

HOW TO USE
----------
1. Unzip the whole folder somewhere you can write to (Desktop, Documents,
   a USB stick). Don't run it from inside the zip.
2. Do one of these:
     - Double-click "DKC32X Builder.exe", press Browse..., pick your ROM,
       then press Convert.
     - Drag your ROM file onto "DKC32X Builder.exe".
     - Drag your ROM file onto "Convert (drag ROM here).bat" (text-only).
3. Wait a few minutes (about 2 minutes on a slow dual-core PC). When
   it finishes, "<your ROM name> (32X).32x" appears next to your ROM.

Play the .32x in an emulator with 32X support (ares, PicoDrive) or on
real hardware / MiSTer. A 6-button pad is recommended. The .32x is
14 MB, so it fits flash carts with a 15 MB limit (Mega EverDrive Pro).

PLAYING
-------
Switch on and the whole game starts: logos, title, file select, the
island maps, every level, the final boss and the ending with the credits.
Pad: A = run / roll (SNES Y), B = jump (SNES B), C = SNES A, Start =
Start (pause; the pause menu also has "exit to the map" and the options).
Your saves (Cranky / Candy barrels, the file select) are kept in the
cartridge's battery memory. In an emulator that is the .srm / .sav file
next to the ROM; on MiSTer mount a save file when loading the ROM.
Hold Start while switching on for the level viewer menu instead (pick
any level directly).

The same .32x works on NTSC (60 Hz) and PAL (50 Hz) consoles: it
detects a PAL console and keeps the game at its normal speed (60 game
frames a second, shown as 50 pictures a second).

ROM FILES IT ACCEPTS
--------------------
- .sfc, .smc, .swc, .fig or .bin, with or without the 512-byte copier
  header, or a .zip containing one of those.
- Supported so far: Donkey Kong Country, USA, revision 1.0.
  The tool reads the cartridge's own header and tells you which version
  you have. Other versions (USA 1.1 / 1.2) and regions are not supported
  yet; if yours isn't supported, the window says so (with its md5) and
  nothing is built.

IF SOMETHING GOES WRONG
-----------------------
- "This version is not supported yet": see above.
- "checksum mismatch": the dump is probably damaged. Re-dump the cart.
- Windows SmartScreen may warn about an unknown app. Choose
  "More info" -> "Run anyway". Or use the .bat file.
- Antivirus can slow the compile step a lot. If it takes much longer
  than 10 minutes, try adding an exclusion for this folder.
- After a failed run, "last conversion log.txt" in this folder has the
  whole log. Send it along with your report.
- Anything else: the window (or the .bat window) shows the step that
  failed.

WHAT IT DOES
------------
1. Checks the ROM.
2. Runs it headless in a bundled snes9x emulator (app\emu) to capture the
   start of every level.
3. Recompiles the game's 65816 code to SH-2 C code and compiles it with
   the bundled GCC (app\toolchain).
4. Converts the levels, graphics and music, and writes the 32X ROM.

The resulting .32x is a converted copy of your cartridge: it's for your
own use. Please don't share it.

CREDITS
-------
snes9x (libretro core): snes9x licence, non-commercial.
GCC and binutils: GPL. Python: PSF licence. numpy: BSD. Pillow: HPND.
The 32X startup code is based on Chilly Willy's, as distributed with
marsdev (MIT).
