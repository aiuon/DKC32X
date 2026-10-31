# DKC → 32X dev log (keep this current; it's the hand-off document)

## Status
- [x] M1 Jungle Hijinxs viewer. Works in PicoDrive, ares, and on MiSTer
  (user report 2026-10-16: 60fps, M 72% S 40% on MiSTer, minor flicker,
  see below).
- [x] M2 all-levels viewer + world-map level select + level effects (Torchlight + boss arenas paused
  by the user until they can test).
- [ ] M3 GAMEPLAY (in progress, see "M3 gameplay" at the end): the real game logic = DKC's 65816 code
  statically recompiled to C (tools/recomp), byte-exact vs snes9x in gameplay; sprites drawn by the
  SH-2s; world 1 playable. Goals from the user: 60 fps, physics/feel identical to the SNES, no
  added input lag. LEGAL: personal-use builds only; a release could only ever be a build tool.

## Environment setup (cloud container, rebuild if lost)
- SH-2 GCC: `apt install gcc-13-source binutils-sh-elf libgmp-dev libmpfr-dev libmpc-dev texinfo flex bison`,
  untar /usr/src/gcc-13/gcc-13.3.0.tar.xz, configure `--target=sh-elf --prefix=/opt/sh-elf
  --enable-languages=c --without-headers --with-newlib --disable-libssp --disable-nls --disable-threads
  --disable-shared --with-cpu=m2 --with-multilib-list=m2 --with-as=/usr/bin/sh-elf-as --with-ld=/usr/bin/sh-elf-ld`,
  `make all-gcc all-target-libgcc install-gcc install-target-libgcc` (~40 min on 2 cores).
- 68K: `apt install gcc-m68k-linux-gnu` (use -m68000 -ffreestanding -nostdlib; avoid 32-bit mul/div).
- SNES ref emulator: libretro/snes9x patched so retro_get_memory_data ids 0x1000=CGRAM,
  0x1001=FillRAM (regs), 0x1002=OAM, 0x1003=&PPU.BG[0] (12 bytes each: SCBase,HOfs,VOfs,BGSize,pad,NameBase,SCSize),
  0x1004=&DMA[0] (22 bytes each). Built at /opt/ref/snes9x/libretro/snes9x_libretro.so.
- Headless libretro harness: /opt/harness/harness core.so rom script outdir
  (script: run N [BTN,..] / shot NAME / dump NAME ID SIZE / state NAME / load NAME).
  Genesis via picodrive libretro: RETRO B=B, A=C, Y=A, START.
- 32X emulators: PicoDrive libretro (/opt/ref/libretro_picodrive, no FRT timer, lenient), ares
  (/opt/ref/ares/build/rundir/bin/ares, needs SDL3 built from source; run under Xvfb :99 with
  `--system "Mega 32X" --setting Video/Driver="OpenGL 3.2"`, screenshot with `import -window root`).
  ares emulates FRT: M1 measured M 46% / S 35%.
- SNES savestate inside Jungle Hijinxs: /opt/work/snes/lvl0.st (snes9x).
- Reference: Yoshifanatic1 DKC1 disassembly at /opt/ref/Yoshifanatic1_Donkey-Kong-Country-1-Disassembly
  (code in DKC1/Routine_Macros_DKC1.asm, asset table in DKC1/AsarScripts/AssetPointersAndFiles.asm).
  marsdev skeleton at /opt/ref/andwn_marsdev (MIT).

## ROM facts (USA 1.0, MD5 30c5f292ff4cbbfcc00fd8fa96c2de3b, HiROM, file offset = addr & 0x3FFFFF)
- BG gfx decompressor CODE_B89700: 0x80-byte dictionary (64 words), then cmds: 00=end,
  00nnnnnn=n literals, 01nnnnnn v=fill, 10nnnnnn lo hi=copy n from out[offset], 11iiiiii=dict word.
- GFX load lists DATA_B9A994 (index → list of (src24, vramdst16, size16)); vramdst bit15 = compressed.
- Tileset type table (CODE_818C66, index = LevelTypeID): layout ptr DATA_818B94, Map32 ptr DATA_818BBE,
  DATA_818BE8 (vram), DATA_818C04, DATA_818C2E, per-type code DATA_818C4A.
- Layout: column-major, 16 chunk rows per column (for horizontal levels at least), 16-bit entries:
  bit15 vflip, bit14 hflip, low 14 = chunk index. Chunk (Map32) = 4x4 SNES tilemap words (row-major,
  32 bytes). Chunk flip reverses tile order and XORs tile flip bits.
- Jungle: gfx D58FC0 (compressed → VRAM $3000, BG1+BG2 char base $3000), layout D90000,
  map32 D9A3C0, palette B9A1DC (128 colours → CGRAM 0, loaded by CODE_B999F1 X=$20).
  Jungle Hijinxs = layout cols 0..167. BG2 = layout cols 168..183 rows 0..7, built by CODE_B99412
  streaming level x $14F8..$16F8 into the 64-col tilemap (wraps!) → BG2 col c = level tile col
  672 + ((c+32)&63). BG2 scroll = (camX/2, camY/4). BG3: tilemap CDF337 (32x6 at BG3 row 8),
  2bpp chars CDF4B7 → VRAM $2000, palette 0. BG3 parallax (CODE_809AD4): bands lines 0-71/72-79/
  80-87/88+ scroll = X*(2..5)/16 (8-bit).
- HDMA (CODE_B98659): ch2 → CGRAM 0 sky = DATA_B996E6 (0x220 bytes, 5-byte entries count,0,0,lo,hi);
  ch1 → CGRAM 1 (BG3 fog colour) = DATA_B998EA (0xA0 bytes); ch3 → BG3 hscroll table at $0697.
- Colour math per entrance: DATA_B9D7AE[entrance*4] = CGADSUB, fixed colour word.
  Jungle Hijinxs (entrance $16): $C2 (subtract, half, BG2), fixed (4,4,4) → BG2 = (c-4)/2.
- RAM: entrance id $3E, Layer1 X $088B, Y $088F?, camera $1A62/$1A4C.

## 32X design (M1)
- 32X 8bpp packed, palette = SNES CGRAM (BGR555 identical), BG1 colours |0x8000 (priority over MD).
  Palette entry 0 = sky, rewritten per line by master SH-2 H-int (HCOUNT=0).
  Compositing rule (PicoDrive/ares): MD backdrop pixel → 32X always shown; else 32X shown only if
  palette prio bit set.
- Framebuffer: line table + 41x29 tile window, stride 336; fine scroll via line table + SFT reg.
  Full redraw each frame: master rows 0-13, slave rows 14-28. Master flips on VBLK rising edge, then
  posts COMM0=camx, COMM2=camy, COMM10=frame#. COMM4/COMM6 master↔slave token, COMM8 = pad.
- Genesis: H40 + shadow/highlight. Plane A = BG2 (low prio → shadowed = SNES half colour math),
  plane B = BG3, cell H-scroll. 68K VInt waits ≤400 loops for COMM10 change then writes scroll.
  68K H-int every line rewrites CRAM1 (BG3 colour) on lines where the table changes.
- Real-HW quirks learned from ares source: FB byte writes of 0x00 are ignored; FB writes dropped
  while a swap is pending (FEN); palette not writable during active display (ares stalls).

## Known issues / history
- v1.0 MiSTer: flicker 2-3x/s below trees + sky gradient. Cause (deduced from MiSTer RTL
  /opt/ref/s32x_mister): line effects counted interrupts; SH-2 H-int busy-waited on HBLK (register
  reads can stall behind FB FIFO -> merged H-ints -> line counter drift) and 68K VInt waited up to ~36
  lines for COMM10 (blocking H-ints). v1.1 fix: SH-2 line = FRT time since VInt (calibrated, fallback
  to counter when FRT dead e.g. PicoDrive); 68K H-int uses VDP V counter (0xC00008); master posts
  COMM0/2/10 during active display (slips a frame if line>=216) then flips at VBLK so the 68K VInt
  never waits. Sent v1.1 to user for MiSTer retest (pending confirmation).
- MiSTer RTL facts: palette writes stall until PEN (no drop); FB access stalls during FEN
  (refresh 40 cyc at H=0x158, fills); 32X HINT fires on HBLK rising edge, suppressed in VBLK unless HEN;
  SFT/mode latched at H>=0x157 each line; FS switches only in VBLK.
- Test tools: /opt/work/shots32x.sh rom outprefix warmup count interval (ares under Xvfb, crops
  690x530 game area). Don't name scripts with 'ares' (pkill).

## M2 survey (2026-10-17)
- SNES ground truth: /opt/work/survey/<entrance hex>/ (shot.ppm, vram, cgram, regs(FillRAM), bg, dma, ram,
  ppu.bin = whole snes9x SPPU struct: fixed colour @2776..2778, windows @2734.., BGMode @64).
  Captured by /opt/work/survey/run.sh <hex>: loads /opt/work/snes/wmap.st (world map), pokes RAM $40/$3E
  = entrance, presses START. Harness got `pokew addr val` / `unpoke`. Analysis: survey/analyze.py ->
  survey_levels.txt (copy in project).
- Main level entrances: Kongo: 16 JH, 0C Ropey, EA Reptile(Main1), BF Coral, 17 BarrelCannon, E0 boss.
  Mines: D9 Winky, 2E MineCartCarnage, 07 Bouncy, 31 StopGo, 42 Millstone, E1 boss. Vine: A5 Vulture,
  A4 TreeTop, D0 ForestFrenzy, 43 TempleTempest, 0D Orangutan, DE ClamCity, E5 boss. Glacier: 24 SnowBarrel,
  6D Slipslide, A7 IceAge, 3E Croctopus, 14 Torchlight, CE RopeBridge, E2 boss. Kremkroc: 40 OilDrum,
  2F TrickTrack, 18 Elevator, 22 PoisonPond, 27 MinecartMadness, 41 Blackout, E3 boss. Chimp: 30 TankedUp,
  12 ManicMincers, 0A MistyMine, 36 LoopyLights, 2B PlatformPerils, E4 boss. 68 Galleon.
  World maps: E6 Chimp, E7 Kremkroc, E8 Glacier, E9 Vine, EC Kongo, ED Mines (EB invalid).
- Effects found: mines = BG3 light layer on main, scene on sub, CGADSUB $24 (additive); Poison Pond BG3
  over scene $64 (half add); Misty Mine BG2 fog main + scene sub $22 + BG2HOFS HDMA; Slipslide $33 + CGRAM
  HDMA + INIDISP HDMA; caves $93 with HDMA window WH0 (light circle/vignette); underwater: HDMA BG1HOFS/
  VOFS(m3), BG2HOFS, BG3HOFS ripples; snow: HDMA BG3VOFS; Temple/Millstone: HDMA TM per line; Blackout
  $A7 fixed 30 (lights off); Galleon $BF + BG2 H/V HDMA; bosses BG2HOFS HDMA.
- Level geometry: all levels of a tileset share one long layout strip ($D3 = tileset base, same for all
  levels of the type). Horizontal levels: X range = [$1B21, $1B23+256] (RAM), Y 0..511 (16 chunk rows).
  e.g. JH 0..$1400, Ropey $1700..$3680, BarrelCannon $6360..$8C3F, Orangutan $3780..$5C60.
  Underwater (stream routine CODE_818168, $D3=E90000) and crystal cave (CODE_81831B, $D3=E101E5) are
  vertical; need their layout format. $1B15 = level effect/type index. $1B0F = stream routine.
  Per-type NMI scroll routines around CODE_80893x..: BG2 = X/2, Y/2 or Y/4 etc. ($1DF7 = BG3 VOFS).
- Debug: RAM $0535 = free movement debug flag.

## M2 architecture plan
- ROM budget: per tileset store 4bpp tiles + map32 + layout once; per level store remapped "virtual
  chunk" defs (chunk+flip combos -> baked tile idx|vflip) + layout + bake list (tile,pal,hflip); SH-2
  bakes 8bpp tiles into SDRAM at level load. Renderer resolves layout->chunk->tile per 8x8.
- 68K loads Genesis VRAM per level on SH-2 command (COMM), ROM >512KB via bank reg 0xA15104.
- Level select: world map images (entrances E6-ED dumps) + level list.
- Transparency plan: 8bpp blend via LUT[scene_idx][overlay_idx] -> palette entries 128-255 (free in
  viewer, no sprites) holding exact or k-means quantized blended colours; per-pixel only where overlay
  pixels are non-zero. Cross-chip blends (light over Genesis BG2) need BG2 on 32X (use overwrite FB
  region 0x24020000 so zero bytes skip = hardware transparency) - fill-rate cost, measure.
- Per-entrance NMI routine table DATA_80C5BD (pairs NMI,main; index = entrance). Full routine text in
  research_nmi.txt. Layer roles/scroll (P = playfield streamed from layout; X,Y = Layer1 pos $088B/$0895):
  808C6C Jungle(JH,Ropey,BarrelCanyon) / 808D43 Orangutan: P=BG1; BG2 x/2,y/4 (built from layout);
    BG3 band: VOFS=$1DF7, BG3SC=$1DF9 (rain swap), HDMA hscroll bands.
  808CDC Snow(SnowBarrel,IceAge): BG2 x/2,y/8; BG3 VOFS=$1DF7 + HDMA BG3VOFS (snowfall).
  808DB4 Underwater(Coral,Croctopus,Clam,Poison): BG2 x/2, y=-1; BG3 x*1.5, y=Y; HDMA ch5 BG1 H/V ripple,
    ch6 BG3 ripple, ch2 BG2HOFS.
  808E38 Cave(Bouncy,Torch,Elevator,Manic): BG2 x/2, min(Y,0x11F)/8; BG3 x*1.5, y=min(Y,0x11F) (foreground);
    WH0 window HDMA (fixed colour flicker from X).
  808F9C Walkway(Winky,TrickTrack,TankedUp,Platform): BG2 x/2, min(Y/8,31); BG3 lamps via 80B749/80B1BA.
  808FFE Misty: BG3 x/2, y=Y; BG2 fog via 80B490.  80905C MC Madness: BG2 x/2,min(Y/8,31).
  8090C3 MC Carnage: BG1 & BG2 = X,Y; BG3 x/2,y/2.  809137 StopGo / 809196 Loopy: BG2 x/2, y/2 (8-bit).
  8091F5 OilDrum / 809446 Blackout: P=BG2 (X,Y); BG1 = 2X (fg, X*2 with sub-pixel carry); BG3 x/2,min(Y/8,31).
  809326 Temple(Millstone,Temple): BG3 x/2,min(Y/8,31); torch anim DMA every 4 frames from D30000.
  8093B5 Slipslide: BG2 x/2,y/2; BG3 H=$1DF9 V=$1DFB; CGRAM 0-15 DMA from 7F0000 each frame.
  8094CC TreeTop/RopeBridge: BG2 x/2 (8-bit), min(Y/8,31); BG3 x/4.
  809543 Vulture/Forest: BG2 x/2, (Y-0x180)/8; BG3 x/4.   8095B6 bosses: BG2 HDMA ($0688=x/2,$068B=2x).
  809604 Galleon: BG3 wave V scroll.
- Factory: playfield is BG2 (BG1 is a 2x foreground in front of it). Validated: my layout model
  reproduces the playfield tilemap 100% for all horizontal levels (tools/validate_layout.py).
- Research tools in project: tools/asset_index.py, tools/recipe_finder.py (VRAM/CGRAM -> asset recipes,
  output research_recipes.txt), tools/validate_layout.py.
- tools/levels.json (generated by tools/make_levels_json.py from survey; names/addresses/numbers only)
  + tools/vvram.py rebuild each level's SNES VRAM/CGRAM from ROM. Verified vs dumps: chars 100% all
  levels; static BG maps 100% except: MC Carnage BG2 58% (2nd streamed layer at layout+$4B00,
  CODE_818B6B), Snow BG2 85%, Slipslide BG2 6% (unknown source), Torchlight BG3 0% (flashlight layer).
  Palettes 100% except runtime-computed: Ropey (night, CODE_80B099 halves jungle pal), IceAge, Torch,
  walkway/mine lamps (CGRAM 0-31 dynamic), Slipslide CGRAM 0-15 (DMA from 7F0000 per frame).
- Vertical layouts validated: underwater (E90000) and crystal (E101E5) = row-major 64 chunks wide.
- Reptile Rumble playable entrance is $01 (EA = map entry, doesn't load directly).
- World-map grids at BCF04B.. are NOT node coordinates (some other per-position table).
- 2026-10-17: tools/build_viewer.py (v2 builder) written: per-tileset pool/bake/vchunk tables, per-level
  grid, palettes, sky, 32X overlays (cave BG3 fg 1.5x, factory BG1 fg 2x), Genesis planes w/ shadow
  decision + 4-line palette packing, Python preview compositor (build/gen2/preview/<E>.png). Previews of
  16,07,40,BF,2F,0A,24,A4 match SNES shots. TODO: snow sky gradient (static c0 wrong), snow BG2 85%.
- Correct camera bounds: x_min=$1B23, x_max=$1B25 (Layer1 X range). Vertical levels: no Y bounds
  found yet -> free roam whole map (underwater 2048x12512, crystal 2048x3712).
- Engine v2 plan: COMM0 camx, COMM2 camy, COMM4 frame#, COMM6 cmd (SH2->68K load level 0x8000|idx,
  68K clears when done), COMM8 pad, COMM12/14 master/slave tokens. Genesis VRAM v2: tiles 0-0x9FFF,
  plane A 0xA000, B 0xC000 (64x64 max, plane size reg shared), sprites 0xE000, hscroll 0xE400.

## Session log
- 2026-10-17 resume #1: chat context was lost at usage limit but all files survived. State at loss:
  v2 engine (sh/main.c, md/main.c) builds to build/dkc_viewer.32x (3.5MB) + build/dbg.32x; symptom =
  black screen, suspected 68K not getting interrupts. v1 sources kept as sh/main_v1.c, md/main_v1.c.
  ALWAYS append here after each milestone/finding so a future resume can continue.
- 2026-10-17 FIXED black screen: not interrupts. md.ld put .data LMA at LOADADDR+SIZEOF(.text)
  = odd address (0x8809AF) while boot copies from __text_end = ALIGN(2) → RAM code shifted 1 byte →
  68K crashed on `jmp md_idle` (dbg_stage stuck at 2), master waited forever on COMM6. Fix: `. = ALIGN(4);
  __text_end = .;` inside .text. v2 now boots to world-map menu and loads levels in PicoDrive.
  Debug trick: dump 68K RAM via harness `dump x 2 0` (PicoDrive stores RAM as LE 16-bit words).
- NEXT: screenshot every level (menu nav script) → contact sheet, compare with SNES survey shots,
  then ares/MiSTer timing; then effects (transparency LUT, underwater ripple, snow).
- 2026-10-17 v2.0-alpha delivered (out/...v2.0-alpha.32x, contact sheet out/v2_alpha_all_levels.png).
  All 40 levels load from the menu in PicoDrive. Known gaps: Ropey/IceAge/Torchlight render black
  (runtime palettes), mine lamps + Poison Pond fog (transparency) missing, Slipslide palette anim,
  overlay levels slow (OilDrum/Blackout M~85-98%, caves ~80%: render_overlay is a per-pixel byte loop).
- 2026-10-17 BUG: SDRAM code was in section ".data" → GCC loop `.align` padding assembled as 0x0000
  (illegal instruction on SH-2) instead of NOP. Slave hung in render_overlay (found via 68K dbg_comm[]
  snapshot of COMM regs + COMM10 progress markers). Fix: SDRAM_CODE → section ".sdtext" ("ax"),
  68K RAM_CODE → ".ramtext"; linker scripts place them inside .data output section. Also affects real HW.
- Overlay renderer rewritten: tile-wise, long/word writes into overwrite region (0x24020000, HW skips
  zero bytes), per-slot rowmask (bit per non-empty row) computed at bake. Oil Drum (PicoDrive) M34 S30.
- load_level now clears pending_pal (queued menu palette was overwriting level CRAM).
- Test switches: EXTRA_SHFLAGS=-DAUTOLEVEL=n (boot straight into level n, autoscroll on).
  68K exports dbg_comm[8] (COMM0..14 snapshot each vblank) → `dump x 2 0`, LE words, addr from nm.
- 2026-10-17 runtime palettes (tools/build_viewer.py runtime_palette()): weather/fade engine pre-runs
  256 frames (CODE_80AF53) so first palette = fade TARGET. Ropey 0C (CODE_80AC8B): cols 16-127 =
  (r*5/16, g*5/8, b=g') [100/105 exact, rest ±1], sky lines 0-70 = DATA_B99658, rest black.
  Ice Age A7 (CODE_80AA42/80AABA): cols 0-31 = snow pal>>1, 32-127 = (c>>1)+5, cols 0-3 = DATA_B99B14.
  Both verified in PicoDrive vs SNES. Torchlight 14 still TODO (inverted pal + subtract math + WH0 window).
- ares timing (cycle-modelled FRT) overlay levels after fixes: OilDrum M60/S49, Blackout M82/S45,
  Reptile M63/S42, Bouncy M58/S40 — all 60fps. TODO: dynamic master/slave split (Blackout master heavy).
- 2026-10-17 underwater BG1 ripple DONE: measured in snes9x (/opt/work/rip): HDMA ch5 mode3 BG1 H/V,
  double-buffered tables 7F0200/7F0264, first band grows 1 line/frame → pattern slides down 1 line/frame.
  64-line cycle: (+1,+1) 22 lines, (0,0) 11, (+1,+1) 10, (0,0) 21. HDMAEN=$1B03 hi byte (0x24 = ch2+ch5).
  32X: line table per line (dy + even dx) + per-line SFT from sft_tab via master H-int (MiSTer RTL:
  SFT resampled while H>=0x157 through hblank, used at H=0x16; HINT = HBLK rise → write in H-int is safe).
  Level flag bit1 = water. BG2 ripple (ch2 → BG2HOFS, table 7F0390) NOT done: values look like 8.8
  ramps (0x7EC0,0x5E82,...) — needs proper decode.
- 2026-10-17 heavy snow (Ice Age A7) DONE: CODE_8089A0 weather level 5. BG3 is HIGH priority (in front
  of BG1) → 32X overlay (ov xm==4). Builder heavy_snow_overlay(): 8 frames from DATA_DECE74 (0x400 each,
  2bpp, 64 tiles), map TM_Layer3_Snow ($7C00, tiles 0-63 pal0 prio1), per-frame per-row flake lists
  (x,colour) packed after the map (sway[64] from DATA_808BA8, wobble[32], u32 frame offs[8], per frame
  u16 rowidx[257] + pairs). SH-2: HOFS=(frame+sway)&255 (sway: t=f&7F, mirror if &40, >>1, ~ if f&80),
  VOFS=(f&1)^1 + wobble[(line + (~f>>1))&31], anim frame (f&0xE)>>1. Renders only flake pixels per line
  (tile-row approaches were 30fps). ares: 60fps M61 S49. Other weather levels ($1DF1 2..4: BG2 animated
  snow DEEE74 at VRAM 5E00/5F00) NOT done (would be Genesis-side tile animation).

## M2b TRANSPARENCY DESIGN (2026-10-17, in progress)
- Problem: SNES colour math blends layers that we split across 32X (BG1) and Genesis (BG2) → can't
  blend across chips. Lit levels move ALL scene layers to the 32X ("comp mode", level flag bit2).
- Palette banks: scene quantized to S colours (q=0 reserved for backdrop, nonzero pixels map to >=1);
  light quantized to K levels; palette[k*S + q] = blend_op(light_k, scene_q) (S*K <= 252; 253-255 HUD).
  Lighting = add k*S to the pixel byte (no carry across bytes → 4 pixels per 32-bit add).
- Line compositor (per buffer line, SDRAM line buffer aligned to BG1 grid, 82 longs):
  1) base layer (e.g. BG2) via byte-shifted long stream (shift by (x&3) bytes with fixed shifts/xtrct),
  2) BG1 playfield merged (rowfull → long copy, else per-byte zero-skip),
  3) light line (pre-baked add-value tiles, same shifted stream) added as longs,
  4) 82 long writes to FB. Line table/SFT unchanged (fine scroll still via line table).
- Findings: Poison Pond 22: CGADSUB $64 = (main+sub)/2, main=BG3 fog (grays 2,5,7,9,11,14,16) or
  backdrop(0), sub=BG1+BG2 (73 colours). Fog coverage 92%. Misty Mine 0A: BG2 fog main (grays 0-3),
  sub BG1+BG3, $22 add. Walkway lamps (D9,30,2B,31,2F): BG3 cone main, $24 add (blue only, 0..10),
  per-line colours via HDMA ch2/ch3 (CGRAM 1-15) anchored to lamp object pos ($DD,$DF) → needs
  sprite data parsing (CODE_80B2xx/80B3xx); cone swing = pendulum (CODE_80B800) + BG3HOFS shear table.
  Torchlight 14: window WH0 HDMA cone, CGWSEL $22, subtract, inverted palettes. Blackout 41: $A7 fixed 30.
- Plan order: Poison Pond (static fog) → Misty Mine → lamps (needs lamp positions) → Torchlight.
- 2026-10-17 comp perf work (Poison Pond = AUTOLEVEL 29). Current design (sh/main.c render_comp +
  sh/comp.s generated by tools/gen_comp_asm.py, build.sh runs the generator):
  * comp levels: cache two-way mode (CCR 0x19) → 2KB on-chip RAM @0xC0000000 per CPU: BG1 line
    buffer (70 longs) + lrow_t ent arrays (base, BG1, light; 38 tiles × {addr, xor}).
  * 256-px window; whole line shifted by P = lightx&3 (line table + SFT) so light is long-aligned and
    read straight from tiles in the asm loop (out = F(base) [merge bg buf] + light → FB, 2 longs/tile,
    34 tiles/line). Overhang past 256 px hidden by Genesis plane A black border cells (builder sets
    gtiles/gmaps for comp, fixed scroll [3,0,3,0,0,FFFF]); comp palette has NO priority bit (HUD 253-255 do).
  * tiles row-pair-interleaved for comp (tile s row r at (r>>1)*COMP_RP + s*16 + (r&1)*8) — vflip via
    entry = addr + COMP_FLIPC + 1, xor ~0. Empty tiles remapped to ZERO_SLOT (MAX_SLOTS-1) at load.
  * BG1 ripple applied to BG1 only (per-line gx/gy + d); H-int disabled when sky constant (hint_mask).
  * Test flags: -DPROF (HUD row 2 = prof stages /100 ticks), -DCAMX=480 -DCAMY=2873 (fixed camera).
  * ares @fixed cam (master band, ticks ×8 = cycles): prep/overhead 9.2k, BG1 emit 11.2k, main 31.5k
    → ~52k ticks = 1.08 frame → 30fps. Per tile ≈ 42 instr + ~16 miss + 19 FB. Still too slow.
  * Ideas pending: per-pair BG1 class (empty/full/partial) to skip emit; unroll; load balance.
- 2026-10-17 v2.2: Poison Pond true transparency at 60fps in ares (58-60 autoscroll, M~90 S~86),
  60 in PicoDrive. Final design (all in sh/comp.s from tools/gen_comp_asm.py + render_comp in C):
  * comp_band (asm) = per-line control; state block in on-chip RAM at ONCHIP+300 longs (CS_* enum in
    main.c; r14/r13/r12 = fields 0-15/16-31/32-47). Callbacks cb_base/cb_light/cb_bg1 (C) every
    8 lines. GCC per-line C code was ~600 cycles/line of spills — the asm loop fixed most of it.
  * comp_kK_bB: no-BG1 lines.  comp_mix_{kb}{bb}_{kg}{bg} (64 variants, ~25KB SDRAM): per light-tile
    pair a class byte (COMP_CLS, end marker 3) → plain / opaque BG1 (BG1 replaces base) / partial
    (bit-trick merge). Tile classes from vch_ram bit14 (rowfull==0xFF at load).
  * base/light maps pre-converted to entry form (comp_map[2][4096], odd address = v-flip) → asm
    comp_prep copy. BG1: 64x32 tile ring (ring_ent/ring_cls) streamed by the master before each token
    (ring_update), read cache-through (slave coherence).
  * 33 light tiles/line suffice. Dynamic split (sh->split, ±2 lines/frame from master vs slave ticks).
  * H-int off when sky constant (hint_mask), comp → 0 + CCR 0x11 on return to menu.
  * HUD for comp: hud_text_fast (long writes, x%4==0).
  * MiSTer RTL: SH-2 framebuffer writes go through a 4-word FIFO (VDP.sv) → cheaper than ares's
    8 cycles/long; expect more headroom on MiSTer.
  * PROF build (-DPROF) row 2: master band, preps, BG1 prep+classes, mix, plain/partial % (code).
  * Contact sheet: python3 /opt/work/contact.py ROM build/gen2/pack.bin out.png (PicoDrive, via menu).
- NEXT: Misty Mine fog (0A, BG2 fog main + BG1/BG3 sub, add) → lamps → Torchlight → Blackout; BG2 ripple;
  weather snow anim; refresh build-tool zip at the end (no game data).
- 2026-10-17 v2.3: Misty Mine fog (0A) DONE, 58-60 ares, 60 PicoDrive. Builder COMP[0x0A]: base BG3 (2bpp),
  light BG2 fog, op add, S=31 K=8, lgroup 'vpair' (level = v>>1), wave 'misty' → light extra block
  {nlt, waveflag, tiles, sway[64], S, K, qcol[S], vrep2[K]}; level flag bit3. Engine: misty_frame()
  emulates CODE_80B547 (band table shifts down 1 band/frame, top = X*1.25 + w, Y + w) and the intensity
  fade (I +1 every 32 frames to 31; misty_palette() recomputes banks, uploaded in vblank).
  * comp_band is now per line: light {H,V} per 4-line band (comp_wave[56][2]; PP fills constant), P = H&3,
    prep keys = row<<8|tile0 with tile offset 0..3 (BOFF/LOFF fields 46/47).
  * Misty H rounded to even: per-line SFT changes via H-int land 2 lines early in PicoDrive (measured
    with a flat-vs-wave diff test) → only word-granular line-table shifts. (Underwater ripple in non-comp
    water levels still uses per-line SFT — may have the same 2-line error; check.)
  * levels.json fix: GFX_Layer2_Fog run now [57344, 7456, 0] (tile 0 was missing → black squares).
  * Memory: ovmap_ram (u16) and comp_map (u32) share map_mem (in-place conversion); __end ~0x0603DEE0,
    limit 0x0603E000 — nearly full! PROF build needs gen arg 'prof' (path counters only with 'prof2').
  * Comp HUD drawn by the 68K on plane A (font tiles from builder hud_font_tiles(), md fx=2); 32X sends
    FPS / M% / S% multiplexed in COMM10 (bit15 on, bits 13-12 field). 68K-side vblank counting was wrong
    in ares (counted 2 vints/frame?) so FPS comes from the 32X.
  * present(): skip threshold 216 → 222. Balance uses measured line-table ticks + 300.
  * Test flags: -DNOSCROLL, -DMISTY_I0, -DMISTY_FLAT, -DDEBUGLINE (Y field = line at present), -DOLDHUD,
    -DNOHUD, -DSHOWSLOTS.
- 2026-10-17 v2.3b: Misty fog waviness fix: all 4-line bands take the TOP band's pixel parity
  (comp_wave[i][0] += (h ^ h0) & 1) so one global SFT works; built, not yet delivered when music began.
- 2026-10-17 MUSIC (in progress). DKC = Rare SPC700 engine (disasm DKC1/SPC700/SPC700_Engine_DKC1.asm).
  * tools/audio/spc2x.py: static translator asm → Python (emit_py) / C (emit_c) → sh/spceng_gen.c
    (+ sh/spceng.h spc_t {ram[0x600], rom ptr (ARAM image), dsp[128], port[4], dspw cb}).
    Song start: spc_exec(CODE_1071) then ram[0x24]=1; tick = spc_exec(CODE_064F) (runs to CODE_05FE),
    period 4*$EC samples @32kHz. DSP writes verified IDENTICAL to snes9x (patched libretro: mem id
    0x1006 ARAM, 0x1007 DSP regs, 0x1008/9 DSP write log), and Python == C.
  * tools/audio/spcrun.py (py runtime), song.py (events/samples → .ev/.smp), brr.py (BRR → 8-bit+gain),
    render_test.c (PC WAV through sh/spcdsp.c = S-DSP subset mixer: 8 voices, ADSR/GAIN, echo+FIR).
  * Song = whole upload; header at ARAM $12A0 (8 track ptrs + 2 bytes). Level→music via WRAM $0523
    (music ID, defines Misc_Defines_DKC1.asm). Capture: /opt/work/music/cap1.sh <entrance> →
    m/<e>_a.{bin,wram} at 200 frames after level start (the 1566-frame captures can be death/map music!).
    15 songs: DKIslandSwing(0c,0d,16) CaveDweller(01,07,12,14,18) Misty(0a,31,36) Aquatic(22,3e,bf,de)
    Northern(24,a7) MineCart(27,2e) LifeInMines(2b,2f,30,d9) FearFactory(40,41) Temple(42,43)
    Gangplank(68) IceCave(6d) Treetop(a4,ce) ForestFrenzy(a5,d0) BossBoogie(e0-e5) SimianSegue(map, e6-ed).
- 2026-10-17 v2.4 MUSIC ON THE 32X (done; per-level songs + map song in the menu).
  * Builder: tools/audio/mkmusic.py rebuilds each song's ARAM from the ROM exactly like the SNES uploads
    (engine DATA_8AA36A -> $04E0, SFX block DATA_C92D95, global samples DATA_8AB245 + music set
    DATA_8AB20D[id] -> dir $3200 / data $3300, sample-id->SRCN map -> $04E0, song DATA_8AB159[id]).
    The map keeps stale entries: replicate history (BOOT_MAP {0x10: 2}, title set 09, map set 0C).
    Verified: engine DSP-write streams from ROM-built ARAM == from snes9x snapshots for 7/15 songs; the
    rest differ only by leftover per-channel pitch/vibrato RAM ($7C+x/$8C+x/$0100+x) from the previous
    song (history dependent in the real game too).  LEVEL_MUSIC maps entrances (from WRAM $0523).
    Pack: header off 24 = off_music; block 'MUS1' (see mkmusic.py docstring); samples 16-bit BE, 15-bit
    range, pre-filtered [a,1-2a,a] a=0.13 (approximates the gaussian interpolation's lowpass).
  * Runtime: sh/musplay.c (song start: ram[0..5FF] from pack, rom = image - 0x600, CODE_1071, $24=1;
    engine tick every 4*$EC 32k-samples), sh/spcdsp.c (S-DSP subset), sh/mixv.s (voice loop, linear
    interp, gain ramps, mixjobs, expand2), sh/mixout.s (half-rate echo with 2-tap FIR = the two oldest
    entries of a line lengthened by the FIR delay, 8-bit echo line, lowpass 7/8, PWM conversion),
    sh/music.c (slave: PWM 22050 Hz, TM=1 + RTP -> DREQ1 -> DMAC ch1 long writes to 0x20004034,
    CHCR 0x18E5, double buffer 2x128 frames, DMA-end IRQ vector 63 (slave VBR TRAPA slot) at IPRA 13;
    slave SR I=6 + slave PWM int-mask bit on because PicoDrive only clocks DREQ1 when some CPU enables
    the PWM IRQ).  Echo line shares memory with slotmap (load-only): music_play(0xFF) before loading.
  * SH-2 gotchas found: GCC calls libgcc for every signed x>>n (n != 1,16) and variable shifts -> use
    dmuls (inline asm mulhi), biased logical shifts; ROM code in a 2 KB cache is very slow -> keep hot
    code in .sdtext.  ares counts 1 cycle/instr + 12/60 cycles per SDRAM/ROM line fill.
  * Cost (ares, FRT ticks per 128-frame refill = 16.7k per period): ~3.2k-8.7k in normal levels
    (song dependent, 8-voice Aquatic worst) -> all normal levels still 60 (Blackout Basement 49-60).
    Render split now dynamic for normal levels too (split_row, master/slave balance).
  * Comp levels (Poison Pond, Misty Mine): "light" mode = voices at half rate + expand2 interpolation,
    uncached sample reads, mix buffers in on-chip RAM 0xC0000600 (only while CCR two-way).  Still
    ~30 fps with music (both CPUs were ~90% before).  B+A toggles music (restores 60 fps there).
  * Memory: RAM limit 0x0603F400, master stack 1 KB (0x0603F400-F800), slave 2 KB.
  * Debug: -DMUSDEBUG (menu: refills/s, ticks; level HUD: split row, refill avg, stack free),
    -DNOMIX, -DFAKESMP, -DMUSPROF2.  PC: tools/audio/player_test.c (little-endian pack via
    'mkmusic.py ROM out.bin le').  Exact-DSP reference renderer: /opt/work/refdsp (snes9x SPC_DSP).
  * Release-tool TODO: sh/spceng_gen.c is generated from the disassembly (Rare's code) - a shareable
    tool must translate the engine from the user's ROM (DATA_8AA36A) at build time, not ship it.
- 2026-10-17 v2.5a fixes (user report after v2.4 on MiSTer):
  * Jungle gradient flicker: 68K H-int read VCNT (may already show the next line) -> count H-ints since
    vint (hcount) instead.
  * Walkway / Mine Cart Madness BG2 black: colours 17-31 = DATA_B9A2FC dimmed by lamp flicker
    (CODE_80B749/80B800, DMA from $7F0002) -> runtime_palette uses B9A2FC - 1 per component.
    Lamp light cones (BG3 main + add, needs lamp object positions) still TODO.
  * Slipslide BG2: CODE_B992B2 copies the BG1 map rendered at Y=0 (64x32) to $7000 -> LAYOUT_BG rule
    0xE101E5 (w=64). Glimmer (BG3 NintendoLogoShine added, palette 0 cycling) TODO.
  * Temple: BG2 is the millstone sprite layer -> not drawn.
  * Treehouse/forest: BG2/BG3 HOFS written as 8 bits -> scroll mode 4 ((v>>s)&0xFF) in md/sh/py.
  * Genesis S/H rule: priority (non-shadow) plane A cells un-shadow plane B under them even where A is
    transparent -> if A is not shadowed, B is not either (Tree Top Town blocky "transparent" tiles).
  * Ice Age Alley BG2 snow: weather>=2 re-streams VRAM $5E00 (16 tiles) every odd frame from DATA_DEEE74
    and the map's snow cells point at t0..t0+15 (static recipe has t0+16..). Builder emits ganim slots
    (md hdr word 15 = count, data after the maps: u16 tile + 8x32 bytes); 68K uploads in vint.
  * World map sky: master H-int writes sky_ram to CRAM 0; menu now sets it to the world palette colour 0.
- v2.5b: music modes (B+A cycles AUTO/LIGHT/OFF; AUTO switches to half-rate voices when a level
  drops below 57 fps, music_light shared var read by the slave each refill). Comp "performance" mode
  (B+C, default off): BG1 drawn unfogged through the overwrite region after the comp pass, unlit
  palette bank at U = K*S (PP S=36, Misty S=28; U in level flags bits 8-15; bake_add for grid slots).
  In ares it is SLOWER (PP 40-54 fps vs 59-60) - FB overwrite pass costs more than the integrated
  merge; kept as an option for MiSTer testing. Walkway-type palette: colours 0-15 = 0 (lamp light
  HDMA / backdrop black, CODE_80B1BA) - fixed Mine Cart Madness green specks (sky 0x160).
- v2.5c: render_overlay: per-band alignment hoisted (STRIDE % 4 == 0), row bitmask walk (rev8 for
  v-flip), aligned long writes inline -> Oil Drum Alley M63/S52 -> M57/S46 (ares, no music).
- 2026-10-17 v2.6: INCREMENTAL CANVAS for normal levels without overlays (30 of 40 levels).
  Each FB keeps a world-anchored canvas: grid tile (tx,ty) at canvas line (8ty mod CV_H=240), byte
  8tx+CV_XOFF(64), pitch CV_P=344 (43 tiles). Vertical scroll wraps via the line table, horizontal
  scroll moves line starts (levels < 19k px wide, room for ~47k). Per buffer: valid window cvb[b].w;
  each frame draw N \ (V ∩ N) strips + tiles under last frame's HUD (cvb[b].hud). Full redraw (level
  start / jumps) split master/slave via sh->srect; otherwise the slave is not even synced (give=0),
  so it only mixes music. write_line_table_cv (ripple included), hud_text_cv (via the back buffer's
  line table). Master ~15% in Coral (was ~50-60% both CPUs). Overlay levels (nov>0) and comp levels
  keep the old full-redraw path (sh->cv = 0). Test switches: -DNOCANVAS, -DAUTOPATH (camera path),
  -DAUTOCYCLE=n (next level every n frames), -DCVCHECK (samples screen pixels vs level data; 0
  errors over all levels in PicoDrive), -DCVPROF (render/linetable/HUD ticks).
- MiSTer memory facts (from /opt/ref/s32x_mister RTL): SDRAM 0x06000000-0x06007FFF is FPGA BRAM
  (ddram.sv ramh_ram_sel), the rest is DDR3 behind a 4 x 32-byte read cache shared by both SH-2s;
  writes go through a small FIFO and invalidate the cached line. Cart ROM is in SDRAM (sdram.sv
  port 1). FB is BRAM unless dual SDRAM. -DBENCH = memory timing ROM (dkc32x_memtest.32x sent to the
  user; ares numbers: loop 5.0, SDRAM fill 17, ROM fill 64.5, ROM unc 18.9, FB long 12.9, word 8.9).
- v2.6b: canvas for overlay levels too (all but Ice Age Alley's heavy snow and the 2 comp levels).
  Per frame each CPU runs cv_band(rows): strips/HUD/restore ∩ its tile rows, then the overlay on
  those rows' screen lines (render_overlay_cv, canvas addressing, writes stay in x -7..327). Restore
  = BG1 tiles marked under this buffer's previous overlay pass: cv_ovd[buf][row&31][2] (64 col bits),
  built from cv_mmask|cv_smask after sync. cv arrays live in map_mem's upper 16 KB (comp-only).
  Master/slave row split = split_row (balanced). Old render_overlay only in -DNOCANVAS(OV) builds.
  Reptile Rumble (ares, no music) M63/S52 -> M31-39/S21-32. Oil Drum with music M44-68/S37-61.
  MAX_SLOTS/MAX_VCH margins cut to +2 (SDRAM was full). Verified identical to the old renderer in
  PicoDrive: 2 shots x 40 levels (AUTOCYCLE + AUTOPATH) + 40-frame paths in Oil Drum and Reptile.
- NEXT (2026-10-17 after v2.6b): waiting on the user's MiSTer memtest photo + "Coral with music off"
  test + B+C (comp perf mode) test. Ideas: comp levels on MiSTer (16 fps) - dithered/flicker fog as
  an overlay over a canvas scene; per-voice half-rate mixing for low-pitch voices (step < 0.5:
  Misty 87%, DKIsland 42%, Aquatic only 20% of voice-frames); 68K could run the sequencer (~10% of
  the music cost only); lamp cones via Genesis S/H highlight sprites (+ lit BG1 palette copy);
  Slipslide glimmer = BG3 glow (NintendoLogoShine-style routine: $1DF5-$1DFD, palette $7F0000 grey
  fade over 8 frames, position random per 8-frame window, colour math $33).
- 2026-10-17 MiSTer memtest results (cycles x10, pass 2): loop+hit 80 (ares 50), SDRAM line fill
  302 (BRAM 302, same: the cost is the SH bus, not DDR3), ROM fill 604, SDRAM uncached read 294 (!,
  ares 50), ROM uncached 263, SDRAM writes ~117 (cheap), write+read 370, FB long write 83 (~free),
  FB overwrite 63, FB word 114, COMM read 216, ROM fill while the slave reads ROM 440. => on MiSTer
  reads (cache fills ~22 extra cycles, uncached ~22) dominate; FB writes are cheap; avoid uncached
  SDRAM reads in hot loops.
- v2.6c fixes: md sval applied the vertical sub/clamp to the horizontal scroll (treehouse BG2 froze
  after x=0xF8, Rope Bridge never moved, cave backgrounds froze at x=0x11F, forest offset by 0x180).
  levels.json vram_runs: 27 levels lost the first 32 bytes of an asset (matching bug) -> extended
  (fixer /tmp/claude-0/fixruns.py logic: extend while asset == survey VRAM); treehouse BG2 tile 0x73
  was empty -> "see-through ladder" in Tree Top Town. 'ROM:xxxxxx' raw-ROM runs (vvram.build_vram)
  for forest BG3 tiles DMA'd past the asset end. Underwater BG2 perspective (CODE_819741): 68K fx=3,
  line H-scroll (reg 8B=03), 8.8 accumulator per line, lines 80-143 = 0. Canvas overlay marks now
  cached per CPU (each clears its own; master reads the slave's uncached).
- v2.7: Poison Pond PERFORMANCE VARIANT (default; B+C = old true-transparency comp mode):
  BG1 on Genesis plane A streamed by the 68000 (fx 4: pf_update/pf_col/pf_row from md2's grid +
  Genesis vch table; window 42x30 cells in the 64x32 plane), BG2 on plane B with the underwater
  perspective, line H-scroll (plane A ripple x only). The BG3 fog on the 32X as canvas mode 2
  (draw_fog_row: 32x32 wrapping map in ovmap_ram[0], tiles in tiles_ram, colour 0 -> black idx 1),
  checkerboard dither, phase = buffer index (the two FBs alternate -> temporal + spatial dither).
  (fog + scene)/2 == average of the checkerboard: exact for the SNES half-add. Fog ripple uses
  (2,0) offsets (dither_ripple) so the checkerboard phase never breaks. Builder: perf_variant()
  (genesis_planes(ents_override, noshadow)), md_block(b, pf) fx 4, LEVEL_REC 136 (+off_md2,
  off_fog), 68K cmd bit 14 = variant. ares: PP 60 fps M19% S0% with music (was ~30).
  Misty Mine: comp only for now (old BG1-overdraw perf mode retired).
- v2.7b: Misty Mine performance variant (default). Builder pf_add_variant: additive grey fog ->
  ordered dither (Bayer 4x4, density g(v)/FOG_W, FOG_W 16 grey, scaled by the fade-in I via 32X
  palette index 1), baked per phase (2nd FB pattern shifted 1 px; fog block kind 1 = n keys, map,
  pal, 2n tiles, sway[64]). SH canvas mode 3: misty_frame's comp_wave bands applied through the
  line table (write_line_table_cv3), window = union of bands (cv3_rect), cv_xbase rebases fog x by
  multiples of 256 (full redraw). CV_P 352, CV_H 256 now (fog vertical period). 68K fx 5 = streamed
  BG1 + plain plane B, full-screen H scroll mode. Planes for perf variants: _planes_clustered
  (k-means palette lines, tiles assigned to best line; shadow mode for dark scenes (<4% pixels
  >140) -> finer dark levels). ares: Misty 60 fps M17% S0% with music (was 30 / 99%).
- v2.7c (user: PP tearing band when scrolling up; dither visible while scrolling; Misty dither bad on
  MiSTer -> wants the PP method): 68K fx4/5 write VSRAM + H-scroll first, then stream; plane keeps
  one cell of look-ahead on every side (rows ty0-1..ty0+29, cols tx0-1..tx0+41). Dither now
  screen-anchored: mode 2 shifts the layer by e = (gx+gy)&1 px, mode 3 shifts each band by
  (x+y)&1 lines -> on screen pixel (x,y) is fog iff (x+y+buffer) even, verified alternating every
  frame while scrolling (-DFOGDEBUG = magenta fog). Misty: checkerboard like PP, palette 1+v = grey
  2*(v*I>>5)+3 (fade kept), v<3 transparent.
- v2.8: Slipslide glimmer. Builder glimmer_block (level off_fog reused, lflags 0x10, kind 2): BG3
  NintendoLogoShine map as white dots (32X index 252), ordered-dither density = peak grey of the BG3
  palette ramp (p0..3 peaks 1-5/5-9/9-17/19-30), 2 phases. SH: overlays 0/1 = phases (sh->glow,
  sh->ovcap = restore capability; drawn only when sh->nov), glow_frame emulates the game's timer:
  wait 0x60-0x9F frames, 4 glimmers x 8 frames, random position (base -64..32 + rand&63, y formula
  from the routine), dot grey by t&7 {0,0,22,27,31,27,22,0} (dim steps skipped). CRAM[252] at present.
- Weather research: Ropey Rampage $1DF1=7 (rain) whole level, ends at x~13600 -> 3 -> 1. Snow Barrel
  Blast 0 -> 1 at x~2000 -> 2 at x~2590 (barrel blocked further teleport). Teleport trick in snes9x:
  pokew 0xB1B x / 0xBC3 y (DK slot 1) + unpoke every step (harness poke array overflows otherwise).
  Jungle-night NMI (808D43): BG3 = rain, VOFS from $1DF7, BG3SC from $1DF9 (map switching). Palette
  fades: $7F0000 buffer DMA'd when $1E01 set. Jungle Hijinxs: no weather/palette change found (sky
  identical at x 250 and 4090).
- v2.8b: hanging lamp cones (Winky's Walkway, Tanked Up Trouble, Platform Perils). Lamp objects =
  sprite-list entries with spawn script 0xC5DF (lists: 8-byte entries u16 type, x, y, script;
  SPR_* ranges in AssetPointersAndFiles.asm). Lamp apex screen y = 504 - obj_y - camy_snes
  (CODE_80B3D7), x centre = obj_x - camx - 3.5. Cone + shade captured from snes9x on the user's ROM
  (tools/capture/lamp_capture.py -> build/lamp.npz: lamp flag $0579=0 toggles the cone, diff = blue
  light), mirrored left half. Overlay kind 3 (u16 n,3,nlamps,0, lamps, pal[12] -> CRAM 238-249,
  map[2][28][22], tiles), 64x64 overlay map, dots CRAM 250 (2,2,31) at density L/31 (L>=2), shade
  opaque. lamp_frame picks the first visible lamp, (sx0+sy0)&1 keeps the dither phase on screen.
  Release-tool TODO: rebuild cone from BG3 HangingLampLight map + HDMA colour tables instead of capture.
- v2.8c: Ropey Rampage rain + end-of-level weather. Weather triggers are sprite-list entries of type 8
  (u16 8, x, y, routine in bank BD): FD3C/FD46 rain on/off, FD50/FD63 dark on/off, FDA9/FDB3 lightning
  on/off, FD76..FD99 set state 0..5 (Snow Barrel / snow levels). The game runs them when the entry enters
  the spawn window (x AND y - teleport tests were inconsistent because of y). Ropey: 13856 lightning
  off, 13888 rain off, 13920 dark off. Rain off (CODE_818FF2): $1DF5 keeps +8/frame to $300, BG3SC
  -> $2A (map $2800 = empty, stacked above the rain map) => rain's top edge sweeps down 8 px/frame
  for 32 frames. Dark off: $1DFD=$20 + CODE_B6B324 = linear fade night -> day (DATA_B9A1DC cols
  17-127, verified = snes9x CGRAM after fade) in 32 steps x 8 frames; sky -> normal jungle sky table.
  Builder: rain_block kind 4 = u16 n,4,rain_off_x,dark_off_x,col[3],0, gen CRAM night[64], day[64],
  32X day pal[128], day sky[224], pixels. Genesis plane built from the DAY palette, night CRAM = mean
  night colour per entry (genesis_planes(day={'cg':..}) fills day['cram']); the Ropey build-time cg
  is black (runtime palette), so cg_day comes from DATA_B9A1DC.
  SH: wx_frame() (trigger = camx + 320 >= x), lerps CRAM 1-127 (written in present) + sky_ram, posts
  COMM10 = 0x5A00 | step; 68K lerps its 64 CRAM entries (tables copied to RAM at load - pack_ptr
  switches the ROM bank!). Rain list restore now always runs (also before full redraws); when the
  sweep ends both canvases are repainted (cvb valid = 0). Rain line table moved to map_mem (+31808):
  the master stack is only ~1.3 KB above __end (0x0603F2D0 .. 0x0603F800) - an 896-byte local
  array crashed the SH2 (32X layer never appeared). -DRAINCHECK: HUD shows rows with rain-coloured
  pixels left after the restore. -DSTARTX=x: autoscroll from x. -DNORAIN.
- v2.9: snow weather (Snow Barrel Blast 0->5, Ice Age Alley 5->0). Type-8 triggers FD76..FD99 set
  state 0-5 (Snow Barrel x 2281/2840/5120/6221/7528, Ice Age 17312(5) 18291(4) 19270(3) 20249(2)
  21228(1) 22208(0)). Viewer model: state = last trigger with x <= camx + 320 (works both ways).
  Palette keyframes per state = snes9x capture (tools/capture/weather_capture.py -> build/weather.npz,
  CGRAM after each 8x8-frame fade; personal-use like lamp.npz; release TODO: run CODE_80A9DF/80AAF9/
  80AC0B/80AB3A/80AB92/80ABCF in Python). States: 1 = BG3 colours halved (dusk), 2 = + BG2 snow
  tiles ($5E00, frame every odd frame), 3 = BG3 grey ((r+g+b)/4+6), 4 = BG3 flat grey 16 + BG1 snow
  tiles ($5F00, every frame; BG1 cells pattern (y&3)*4+(x&3), pal 2), 5 = blizzard (BG3 -> $7C00 map,
  BG1/BG2 colours 32-127 greyed). Fades linear 8 steps x 8 frames.
  Builder: planes A = BG2, B = BG3 mountains ($7800; Ice Age gets Snow Barrel's BG3 chars 0-63),
  'S' = BG1 snow pattern -> second plane B map at VRAM $8000 (68K flips reg 4 once a fade into >= 4
  is done; scroll = camera; high priority over BG2, S/H off for these levels: reg $8C = $81). The
  level data's BG2 map points at the $5F00 copies: remapped -16 (game map uses $5E00). genesis_planes
  now keys colours by their value in EVERY keyframe (tuple), pre-places each SNES palette's colour
  set, and merges the closest colour pairs until no tile lacks a full line (7 merges here); anim
  specs can be a list (group 2 = 2nd spec, tile | 0x8000). Genesis CRAM keyframes [6][64] in block
  kind 5 (u16 0,5,start,ntrig,nkey,shift,0,0, (x,state)[ntrig], gen[6][64], 32X pal[6][128], sky[6]).
  68K: COMM10 = 0x4000 | from<<11 | to<<8 | step (Ropey now 0 -> 1 of 32 steps), keyframes copied
  to RAM; anim groups enabled by state (blank once when off). SH: ws_frame(); state 5 switches to
  the Ice Age blizzard path (heavy overlay + cv 0 full redraws), leaving 5 back to the canvas (both
  buffers repainted). Slipslide glimmer branch now requires block kind 2 (it used to grab any block).
  Data pack moved to ROM 0x40000 (was 0x100000: the ROM went over 4 MB); 68K-read blocks kept inside
  one 1 MB bank. ares: Snow Barrel blizzard 60 fps M75 S64.
- v2.9b: sunsets. Jungle Hijinxs (trigger FD50 "dark on" at x 4992) and Orang-utan Gang (14829) fade
  day -> the storm-night palette (= jungle_night(): Ropey's CODE_80AC8B formula; verified 106/111
  vs snes9x, rest +-1) with the night sky (DATA_B99658 lines 0-70, black below), 32 x 8 frames. The
  BG3 colour gradient HDMA table ($1D51, from DATA_B998EA) goes to black at night: block kind 4
  word 7 = 1 -> the 68K fades bg3col[224] toward black (recomputed in md_idle, not in the vint).
  Kind 4 block now means "jungle darkness change" (n = 0 rain pixels for JH/OG); JUNGLE_WX lists the
  sprite lists. Earlier "no sunset found" was a wrong test (teleport stopped at x 4090 < 4992).
- v2.9c: light switches. NorSpr63 LightSwitchBarrel (scripts '00 82 <base> 75 13 <dur> 00 80':
  base E6F1 = Stop & Go barrel, E6D7 = Loopy On barrel; dur = frames) -> $0579 |= $300, $1E21 = dur;
  the NorSpr64 controller counts down and clears $100, then $200 (lights state). Stop & Go
  (CODE_80B206): main = BG3 + backdrop, sub = scene, add: backdrop $0060 (GO, +g3) / $0002 (STOP,
  +r2). Loopy (CODE_80B38B): sub-screen empty -> fixed colour subtracted, 9 (off) / 3 (on) on
  BG1/BG2/OBJ. Block kind 6 (u16 0,6,n,flicker,2,0,0,0,(x,dur)[n],gen[2][64],32X[2][128],sky[2]);
  SH lt_frame(): view centre (camx+160) crossing a barrel -> switched for dur frames (Loopy: 24-frame
  flicker out), fed as target into the ws_ fade engine with shift 0 (instant). Lamp BG3 light rays
  of these levels still not drawn.
- v2.9d: Torchlight Trouble. Palette = DATA_B9A01C (cols 17-127) inverted at run time; main =
  backdrop (16,16,16) or Squawks' BG3 beam ramp (cols 1-15, (30,30,31) centre) minus the inverted
  scene -> outside the beam t - 15 (only bright reds survive), inside t - (1,1,0). Viewer: LIGHTS
  entry with fixed trigger at Squawks' crate (x 29128, duration 0xFFFF = permanent), keys from the
  ROM palette; the BG3 beam overlay dropped (no per-pixel beam - would need ~10k pixel RMW/frame or
  BG1 on the Genesis; possible future: tile-granular beam via a 2nd palette bank in the canvas).
- v3.0 (MiSTer feedback round 2):
  * 32X/MiSTer quirk: BYTE writes of 0 to the framebuffer are ignored (VDP.sv line 513: normal region
    upper/lower byte written only if non-zero or both lanes) - the old rain restore left every drop
    over transparent sky. Rain now saves/restores whole words. FB reads wait for the write FIFO.
  * Rain split over both CPUs ("rain phase" after the canvas: shared job 1; master lines 0-111,
    slave 112-223, own save lists in map_mem, see RAIN_LIST); pixel list per map row (row start
    table + u16 x | colour<<8) so each CPU reads only its rows. ares: 60 fps M52 S33 (was 99%).
  * Orang-utan Gang: dark-on = SUNSET (not night): captured (weather.npz sunset_0D_*: CGRAM, $1B41
    sky HDMA, $1D51 BG3 haze HDMA). Kind 4 block gets a 224-word BG3 haze target after skyD (rain
    pixels now at rb+712); the 68K lerps bg3col toward it (JH: black).
  * Ropey day: BG3 switches to the $2400 treeline map (= JH's BG3): second plane B map 'S' with the
    JH BG3 scroll, 68K flips it in when the day fade starts (k > 0).
  * 68K H-int gradient: resync hcount with VCNT (MiSTer's first H-int is at V=$1FF, i.e. before
    line 0; lost/extra H-ints shifted the haze band into the black end of the table).
- v3.1 (MiSTer feedback round 3: blizzard speed, lamps, glimmer, Torchlight):
  * Snow Barrel blizzard on the Genesis: weather state 5 draws BG3's blizzard ($7C00 map, 64 tiles x
    8 frames from DATA_DECE74, CODE_808B1F) on BOTH planes (md flag 16 'H': first tile, sway[64], map
    for VRAM $6000, frames); the 68K uploads a frame every other vblank and writes full-screen H/V
    scroll from the sway (regs 82/84 -> $6000, 8B00 while on). Key 5's 32X colours have no priority
    bit, so the flakes show in front of BG1; the BG2 trees are the flat sky grey by then. The SH-2
    keeps its canvas (no overlay, no full redraws). ws_heavy() is a no-op now.
  * Lamps (kind 3, Winky 6 / Tanked Up 33 / Platform Perils 37): no overlay any more. One cone
    image per level (u8 [224][176]: shade colours 238-249, light level 228-237), origin at level
    (lamp x - 91, 481 - lamp y). lamp_row() bakes it into the canvas while render_rect draws BG1
    tiles: shade pixels always, cone pixels only where BG1 is transparent and only every other
    pixel in level space ((x + y + buffer) even: a 50% checkerboard, the other half in the other
    framebuffer). CRAM 228-237 = unlit scene mean + 2 x light x (level - flicker d) (CODE_80B800),
    light colour per level (block: w[3] in 1/16 after lvl[10]: blue (0,0,16), Platform Perils warm
    (16,9,0)); levels with no light left (l <= 0) get colour 0 without priority = the Genesis shows.
    Cost: only tiles entering the window (ares M 14-29%, was 82-99%). The SNES lamps do not swing
    (checked in snes9x: static cone, brightness flicker only). lamp.npz now also holds on/off frames
    of 30 and 2B (light colour + scene).
  * Slipslide glimmer: tiles single phase (367 instead of 2 x 367 - the 2-phase set overflowed
    MAX_SLOTS: bake 1780 + 734 > 2218 -> the garbage blocks on MiSTer); render_overlay_glow() (ROM)
    masks every other byte as it draws (glow_rows, mask flips per row, buffer parity). Dots = mean
    scene (BG2) + 2 x peak x step (CRAM 240-251, glow_frame).
  * Torchlight: Squawks' beam. Colours 1-127 = DATA_B9A01C (1-15 = the beam ramp, also BG1 palette
    0's colours: the old 17-127 left the bottom rock black). The wedge (SNES window: top edge -0.2375
    px/px, lower-left +0.25) from a fixed apex (64, 110) bobbing +-3 px with the frame number
    (t = (f >> 2) & 31 triangle, (t * 7 >> 4) - 3, same on both CPUs):
    - 32X: CRAM 1-127 dark (t - 15), 129-253 lit copies (t - (1,1,0)); BG1 pixels right of
      L(y) = ax + max((ay - y) * 1078, (y - ay) * 64) >> 8 get colour | 0x80 (bm_lit: non-zero bytes
      only). beam_row() draws tiles that way (also used by render_rect), beam_pass() per tile row
      redraws [min(old, new dark bound), max(old, new lit bound)) - state per buffer and row in
      map_mem + 20480 (uncached: rows move between the CPUs). The slave always helps (give).
    - Genesis: plane B low priority (shadowed by S/H), 44 highlight sprites (32x32, palette 3
      colour 14) covering the wedge (md flag 32 'L': nspr, ntiles, first tile, 0, SAT[n][4] at bob
      0, tiles after the plane tiles); the 68K writes the SAT with the bob when the frame changes,
      on once COMM10's switch target is 1 (Squawks' crate, x 29128). Shadow = t/2 outside the beam
      (S/H cannot reach t - 15). ares: 60 fps M 24-30% S 5-17%.
  * RAM: SDRAM is nearly full (~850 bytes left): lamp_row / glow_rows / beam_* / cv_band are ROM
    code (noinline), render_overlay_glow is a ROM copy of render_overlay_cv.
  * Builder accident: a slice edit cut pf_add_variant .. lamp_block; restored from the .pyc
    (Misty Mine fog block verified byte-identical against v3.0).
  * Debug: -DBEAMON (Torchlight beam from the start), CAMX/CAMY fixed camera.
- v3.2 (MiSTer feedback round 4):
  * Snow blizzard (state 5): only plane B shows the blizzard map ($6000, high priority) - plane A
    keeps BG2's trees (the SNES keeps them, fogged). Plane B H scroll = the blizzard's sway written
    into its column of the cell scroll table (0x8F20 stride), V scroll into VSRAM word 1. Key 5's
    32X colours keep their priority bit (the trees must stay behind BG1), so the flakes pass behind
    BG1 - the one compromise (32X priority is per colour, not per Genesis plane).
  * Weather CRAM: the 68K computes the lerped palette into wx_buf and writes it at the very start of
    the next vblank (it used to write CRAM late in the interrupt -> CRAM dots on hardware/MiSTer:
    likely the "glitchy flashing" during Ropey's night -> day fade). Lamp colours also first.
  * Lamps (walkway levels 6/33/37): the light cone moved to Genesis plane B (S/H off for these
    levels, md flag 64 'C'): 64x64-cell planes, BG2 on plane A low priority, cone cells high
    priority, streamed per lamp by the 68K (lc_frame: 8 cell rows per frame, cleared when out of
    view). Tiles = the captured cone per pixel (levels 3-10 -> line 3 colours 1-10), 50% checker;
    the other phase every other frame by a 1-px H scroll. SWING: the SNES lamps do swing (snes9x,
    frame by frame: skew about row 25, period ~126 frames, -14..+18 px at row 140) -> per-line H
    scroll of plane B, slope table swing[126] (1/256 px per row) from the nearest lamp's row.
    Colours [d][level] precomputed (Genesis 3-bit), d = the SH-2's flicker formula on C_FRAME.
    Lamp origins snapped to the 8-px grid (the 32X shade moves with them, <= 4 px); the 32X image
    holds only the shade (rows = block word 3).
  * Slipslide glimmer: the 32X overlay is gone. The 68K runs the glimmer timer itself (xorshift16
    RNG, same 0x60-0x9F wait / 4 x 8 frames / position formula) and shows the glow map's sparkle
    cores (BG3 palettes 1-3; palette 0's faint halo dropped) as S/H highlight sprites (32x32 blocks,
    15 sprites, Bayer density = peak / 30, high priority because Slipslide's planes are) over the
    BG2 crystals (= the purple mass; the 32X BG1 covers them elsewhere). Fade: steps 27-31 every
    frame, 18 every other; the dither pattern moves 1 px per frame. md flag 128 'G'.
  * Torchlight: checked in ares with the sprites drawn solid (BEAMPIX=1) and the 32X lit palette
    white / dark red (-DBEAMDBG): both edges are pixel-exact and line up; the stalactites at the top
    are Genesis plane B. Added a 4-px checkerboard soft edge on both layers (BM_SOFT; 32X phase per
    framebuffer). Sprite attr stays low priority there (planes are low priority).
  * Debug: BEAMPIX=n (builder env: beam/glimmer sprite pixel value, 14 = highlight), -DBEAMDBG,
    -DNOBOB (SH and EXTRA_MDFLAGS), -DGLDBG (68K: glimmer always on), -DPALDBG (32X layer red),
    EXTRA_MDFLAGS in build.sh.
- v3.3: lamps: BG2 moved to plane B, the cone to plane A (both low priority, A in front); BG2 cells
  with a bright pixel (Genesis level >= 4: the lanterns) high priority = in front of the cone (a
  dot replaces the scene pixel, so over a bright lantern it darkened it; now the lantern cell stays
  as it is). Torchlight MiSTer stair-steps: not reproducible in ares or PicoDrive (both pixel-exact).
  Test ROM dkc32x_v33_beamgrid.32x: BEAMPIX=99 + -DBEAMGRID (68K) + -DBEAMON: the beam sprites drawn
  opaque, full tiles blue, partial (edge) tiles red / green by tile parity.
- v3.4:
  * Poison Pond fog (perf variant, canvas mode 2): snes9x shows BG3 H and V scroll = camera + t/4
    (a slow diagonal drift, 1 px per 4 frames on both axes), not x * 1.5: CV_GX/CV_GY mode 2 =
    camera + pp_drift (frame_no >> 2).
  * Single-screen boss arenas (Necky's Nuts E1, Dumb Drum E3, Necky's Revenge E4: x_min == x_max):
    camera fixed at x_min - 32 (the 256-px arena centred), grid cut at x_max + 256 (the level data
    beyond is garbage; the Genesis background fills the 32-px margins).
  * Snow foreground flakes on the 32X (flakes over BG1) tried and dropped: per flake ~250 SH-2
    cycles in ares (FB writes + save/restore), 560 flakes over the ground = +60% frame time; MiSTer
    runs our loads ~2x slower than ares (Torchlight HUD: ares M30 vs MiSTer M46-63). Variants tried:
    FB-read save (rain style), tile-data restore values, cell redraw, word writes, balanced split.
  * Torchlight on MiSTer: the beam-grid test shows stair-stepped 32x32 sprite columns with the
    partial (edge) tiles as full squares; ares/PicoDrive are pixel-exact. Test ROM with no tile
    sharing between sprites (BEAMNODEDUP=1) sent.
- v3.5: Torchlight beam sprites never share tiles (key = block position; 704 tiles at 256): the
  MiSTer core drew 32x32 sprites that share a tile set wrongly (stair-stepped columns); the no-dedup
  test ROM looked right on the user's MiSTer. Poison Pond fog back to parallax (user preference:
  depth over SNES accuracy): 1.5 x the camera on BOTH axes, no drift (the v3.4 drift version
  looked broken to the user). The rectangular open-water areas around Y 2100-2300 are the level's
  own layout (static and moving cameras give identical planes; Y 2872 matches the SNES).

## M3 gameplay (recompiled game logic) - 2026-10-18

LEGAL: everything generated from the ROM (build/g65/*, build/game/*, snes9x-captured snapshots,
lamp/weather.npz, spceng_gen.c) is personal-use only and must never ship; a release = build tool.

### Pipeline
- Traces: instrumented snes9x (/opt/ref/snes9x_trace, TRWAI=cb dumps WRAM+SRAM+regs+pad at the first
  WAI of each frame; TRDIR collects per-PC M/X/DB/D states + indirect edges) -> /opt/work/gp/covd
  (state.txt, edges.txt, hle.txt); tools/recomp/data/ holds the copies used by build.sh.
- tools/recomp/r65.py ROM tracedir outdir --translate data/translate.txt --files N --sdram list
  --hot data/hot.txt: trace + static discovery (validated against the disassembly's instruction
  boundaries, disparse.py), one C function per routine entry (key = mode<<24 | pc24, mode = (P&0x30)>>4).
  JSR/JSL push the real return bytes on the emulated stack and call the C function; a returned key
  that is not the expected return site unwinds (`return r`). RTS/RTL return the key built from the
  pulled bytes. WAITKEY 0x40000000 (WAI), ERRKEY 0x20000000, JUMPKEY 0x80000000 (indirect/tail
  transfers, trampolined by xrun so the C stack does not grow). Static tail calls are `return F();`.
  Interprocedural flag liveness (N Z C V); decimal mode is never used by DKC.
  Untranslated keys run in r65interp.c (same key protocol). xcall: 256-entry direct-mapped cache in
  front of a binary search of the dispatch table.
- Locals: n / v / db are function locals (l_n, l_v, l_db; written back around calls and returns):
  every byte store to WRAM aliases all globals in C, so globals were reloaded after each store.
  a x y s z c are global register variables r8-r13 on the SH-2 (r14 = frame pointer, GCC may use it).
- Memory macros (r65rt.h): WL = WRAM 0-1FFF (wram_lo, SDRAM, 16-aligned so WLW/WLW_SET do a word
  access + swap.b at constant even addresses), WH = high WRAM via wram_hp[512] page table, ROMB via
  snes_pg[4096] 1 KB pages into snes_blob. rd*_b0/_sys inline fast path + out-of-line slow path
  (LIKELY hints: the cold path leaves the hot code stream); _sysr = ROM-likely variant (constant base
  >= 0x8000). The ROM code fetch (cart, 60-cycle line fills on ares/MiSTer) dominates the cost, so
  code SIZE matters: hot functions (data/hot.txt, >= 2% of gameplay frames) -O2, the rest COLD_FN
  (optimize("Os")).
- HLE (data/hle.txt, sh/game_hle.c; optional 3rd column = mode): 8AB4E6/8AB4FF SPC uploads, 80A203
  OAM clear loop (mode 0 only; HLE functions must not use r65_mx - it is stale inside generated code).
- Host checks (/opt/work/gp): mk.sh [r65 args] builds hostrun; `CONT=1 ./hostrun ROM ref.wai N start`
  compares WRAM byte-exact per frame against a snes9x capture (refcap.sh ENT SEED FRAMES; delete the
  ~100 MB .wai after use). FREERUN=1 ./hostrun ROM snaps/XX.wai N 1 = the GAMEDEMO script without a
  reference. Env: PCCOUNT=file (per-PC + per-function .func counts, free-run too), IPROF, MEMTRACK=file
  (R/W/L pages), WHSTAT=file (high WRAM page classes), FRAMELOG=whcls (per frame: WAI site, INIDISP,
  load-only page use). Interpreted entry keys -> /tmp/claude-0/ikeys.txt (append to translate.txt).
  Gameplay matches snes9x exactly; transitions differ slightly (frame counter at WAI 80E90D).
- /opt/work/sim3 (sh mk.sh; ./sim ENT FRAMES): the 32X runtime (game.c + sprites.c + mkgame tables)
  on the host, with the tile-cache clobbering of the real build; must match hostrun's camx.
  SIMF="-DVRAMLOG" (VRAM DMAs + PPU base regs), "-DROMB_CHECK" (missing ROM pages), "-DR65_FORCEI"
  + FORCEI=1 (everything interpreted), "-DR65_ITRACE" (sim.c r65_itrace(pc) hook in the interpreter).
  SCRIPT=harness script (run N BUTTONS lines) drives the pad, SCALE=x stretches the run lengths
  (32X frames != game frames: the PicoDrive s4 run matched about SCALE=0.85).

### 32X runtime (sh/game.c, sh/sprites.c, sh/main.c)
- Level start = snes9x snapshot at a gameplay WAI (personal use): mkgame.py packs registers, WRAM-lo +
  resident pool pages + 2 KB SRAM (LZ, lz_pack/lz_unpack), OBJ VRAM 16 KB, CGRAM. game_level() picks
  it by entrance. game_frame(pad): NMI push, run65_safe from 00A968 until the main loop's WAI.
- WRAM pages (data/whcls.txt from WHSTAT runs): class 1 gameplay (19 pages) + 2 read by later loads
  (47) -> wram_pool (SDRAM, 66 pages); class 8 = load-only scratch (133 pages) ALIAS THE TILE CACHE
  (tiles_ram): they are only used while the screen is dark. game.c tracks INIDISP; a frame that ends
  dark (forced blank or brightness 0) sets game_fblank; main.c then keeps the last picture and, once
  the game turns the screen on, calls load_level() again (game_reload: no snapshot) for the level of
  entrance $3E. Unknown pages -> wram_dummy. NOTE: re-profile (WHSTAT) when adding worlds.
- SNES ROM pages: memprof.txt R pages (all six world-1 levels, 2000 free-run frames each) + whole
  disassembly data blocks (<= 16 KB) + code pages + ALL frames of every sprite character (build.sh
  SPR_EXCLUDE = characters never seen in world 1: Cranky, K. Rool, Expresso, Candy, Funky, Queen B,
  Master Necky, Croctopus, Manky, Krusha, Winky, mine cart, Rockkroc) + whole level terrain files
  with >= 2 pages read (LevelData/*Levels.bin) - BLOB_DROP (SPC sample / music data: the uploads
  are HLE'd; pages where a block starts are kept, the sound code reads each sample's start).
  World 1 = 2205 pages, ROM ~4050 KB of 4096. The full game will not fit: plan a mapper / compression.
  A page missing from the blob reads as zeros: the life balloon's pose header read 0 tiles, the
  game queued a size-0 sprite DMA (= 64 KB on the SNES) which overwrote all OBJ tiles from $1A00
  (garbage bananas / HUD). Check with the sim: SIMF=-DROMB_CHECK (r65mem.h romb_chk) prints the
  first read of every missing page with the 65816 key / PC.
- Sprites: OAM from WRAM $0200/$0400 (OBSEL 0), obj_tile[512] -> 4bpp source tiles (VRAM DMA hook),
  palettes via the $1A8F ring; drawn after the logic over the canvas (overwrite region), split between
  master and slave by tile rows; spr_marks restore the canvas next time that buffer is drawn.
- Pad: Genesis A -> SNES Y (run/roll), B -> B (jump), C -> A, Start; START+A leaves to the map menu.
- SDRAM: full build __end 0x0603a850 (~19 KB free below the 1 KB master stack region 0x0603F400).
  STACKCHK build: the game's deepest master stack use is ~1150 bytes below 0x0603F800 (> 1 KB!).

### Performance (ares, GAMEDEMO = run right + jump script, average game logic % of a frame, f60-359)
48% (start) -> 43 (ROM fast paths, xcall cache) -> 41 (word access WLW) -> 37 (more translated, OAM
clear HLE, -Os for cold code). FPS 31-60 with sprites + BG. Profiles (PROFH build: H-int PC sampling
of the game code in 512-byte buckets, 4 pages of top buckets shown at frame 360; map with
sh-elf-nm): OAM builders BBAADF/BBA849/BBA594 ~15%, helpers (SDRAM) ~22%, long tail. MiSTer is
~1.5-2x slower than ares for this kind of code -> still far from 60 there.
Ideas: native C for the OAM builders (byte-exact vs hostrun), smaller generated code, more SDRAM
(code), overlapping the BG strips with the logic on the slave.

### Debug flags
-DGAMEDEMO (scripted input + avg), -DPROFH (profiler), -DSTACKCHK, -DMAPMEM_SMALL (old experiment),
-DAUTOLEVEL=n, -DNOGAME. build.sh env: LEVELS, SNAPDIR, SDRAMLIST, HOTLIST, GAMEOPT.

### 2026-10-18 later: the real bottleneck is instruction fetch (ares RECOMPILER mode hides it!)
- ares's SH-2 recompiler (default) charges 1 cycle per instruction and never models instruction
  cache fills; only its interpreter does (Developer/ForceInterpreter: ROM fill 60, SDRAM fill 12
  cycles per 16-byte line). MiSTer: ROM 60, SDRAM 30. All earlier "G%" numbers were recompiler mode.
  Use /opt/work/shotsi.sh (interpreter) or /opt/work/imeasure.sh ROM name (HUD crop after the
  GAMEPROF window) for realistic numbers.
- Patched ares (/opt/ref/ares: md/m32x/sh7604.cpp, sh2 cache.cpp): ARES_SHPROF=file dumps per-PC
  master cycles, instruction counts and cache fills (code ROM/SDRAM, data ROM/SDRAM, other);
  ARES_PHASE_ADDR=hex = only while that SDRAM word != 0 (main.c ares_phase, -DGAMEPROF; -DPHASE_SPR =
  the sprite phase instead of the game logic); ARES_SHPROF_SLAVE=1 profiles the slave.
  /opt/work/aprof.sh ROM out.txt secs [phase_addr] (SLAVE=1 env), tools in /opt/work/tools:
  aprof_sum.py (by function), fills.py (gen/helpers/other), fillmap.py (by 65816 insn kind, needs
  -g: GAMEOPT="... -g"), fillcum.py / sdpick2.py (SDRAM code selection by fills per byte).
- Interpreter-mode JH (GAMEDEMO frames 60-359, % of a frame): logic 109 (ROM code) -> 101 (compact
  calls) -> 92 (R65_OOL) -> 45-49 with ~73 KB of the hottest code in SDRAM.
- Changes: r65_call2/3 + r65_callx2/3 (push return bytes, run, 0 if back at the expected site),
  r65_rts/rtl helpers, n/v write-back by flag liveness (entry_live of the callee / live after);
  -DR65_OOL (GAMEOPT): dynamic-address WRAM/sys accessors out of line (o_rd16_sys...), loop-heavy
  functions (pcprof: >= 2 executions per instruction per frame) keep inline _I variants; cold labels
  (__attribute__((cold))) on never-executed code in hot functions (data/pcprof.txt); GBR = wram_lo on
  both CPUs (WL via @(disp,GBR)); hot functions in SDRAM: data/sdram.txt (sdpick2.py from an
  interpreter-mode profile; budget = free SDRAM - ~5 KB stack margin, see __end).
- -DTCACHE (game builds): no tiles_ram / vch_ram. BG tiles are baked on demand into per-CPU direct
  mapped caches by bake index (master 512 x 64 B = the WRAM scratch alias, slave 256), vch read from
  the pack (sh->vch), row masks by bake index. Freed ~78 KB of SDRAM for game code.
- The slave's early job (3, posted before game_frame): restore the canvas under this buffer's old
  sprites, and draw the strips of the predicted window (camera + last move; kept only within 1 tile
  of the old and the real window: canvas 32 x 44 tiles for a 30 x 42 window). Sprite band split is
  balanced by the two CPUs' last band times.
- Transition screens: DKC shows the world map (WAI 80E90E) after a death; the renderer now holds
  the last picture whenever the game is not in its main loop (WAI 8080F8) or the screen is dark,
  and reloads the level graphics (entrance $3E) once it is back (the scratch pages may be live in
  those screens). TODO: draw the map screen itself; brightness fades.
- Sound effects: game port0 changes -> music_sfx(cmd = port1, param = port2) queue, drained by the
  slave's refill -> mp_cmd (cmd < $FC: engine routine SPC_CODE_1173 with a = cmd, x = param;
  $FD -> ram[$ED]; $FC -> ram[$25]).
- Map screen (WAI 80E90E, after a death / leaving a level): map_frame draws the world-map image and
  the map sprites (spr_draw_band with the canvas offsets), restoring the image under the old marks.
- VRAM DMA fixes: fixed-source DMAs (the level-load clear 80A8B2 -> 0, n 10000) map every tile to a
  fill tile; ROM sources are looked up per 32-byte tile (the blob only has the pages in use, a
  source continues elsewhere after a 1 KB boundary).

### 2026-10-18 (test 3 feedback: MiSTer FPS 32, caves 16-20, M 99%, G ~50%)
- Widescreen (tools/recomp/rompatch.py, applied to the ROM image by r65.py and mkgame.py; env
  WIDESCREEN=0 = off): the 32X shows SNES x 0..319 (64 px more on the right; screen x = SNES x).
  The level sprite spawn / keep-alive windows (bank BD, camx - $20 + $140) are widened by 64 px,
  the OAM builder culls (BBA8E0 $160 -> $1A0, BBA910 $1B0 -> $1F0); sprites.c reads OAM x
  256..319 as on screen (9-bit x < 320).  Enemies now appear ~64 px earlier relative to the camera
  (the only gameplay difference).  Spawns at rel ~348 (sim SPAWNLOG=1).
- Black Kongs after bonus rooms / black map Kong: present() uploaded pending_pal (which blanks CRAM
  128-252) after the sprite palettes; the level palette load (reload) did not re-mark spr_pal.
- Map font (CPU writes to VRAM $0800-$0DFF during the map transition): game_ppu_hook now follows
  $2118/$2119 (VMAIN increments) and stores OBJ tiles written this way in spr_rwbuf (= map_mem,
  borrowed while no level is shown; spr_rw_release() at load_level).
- Screens without a picture (Cranky's cabin, Funky, Candy: WAI != 80E90E): sprites on black instead of
  the world map image (map_black).  TODO: real support needs their graphics + ROM space.
- Ropey Rampage lightning: game mode mirrors the game's BG CGRAM writes (sprites.c spr_bgpal, colours
  1-127; the pack palette = SNES CGRAM 0-127 there) and decodes the sky from its HDMA table at $1B41
  (game_hdma_sky); the Genesis planes get a 3rd keyframe (lightning, from DATA_B9A1DC as r/2, g/2,
  min(2b, 31)) appended to the rain block; COMM10 key 2 while $1B44 = the flash sky colour.
- Sky H-int moved to the slave (slave_hint / slave_vint; music DMA irq lowered to level 9 so it
  cannot block them): master T 95 -> 91 (ares interpreter, JH).  -DMASTER_SKY = old behaviour.
- Native OAM builder (game_hle.c hle_oam_build, CODE_BBA849 incl. the 6 pose variants and the DMA
  queue): WRAM byte-exact vs the pure interpreter (sim FORCEI=1, WRAMDUMP) over 6 levels x 1500
  frames + the s4 script.
- ROM: build.sh SPR_EXCLUDE / BLOB_DROP; all sprite characters except non-world-1 ones; whole level
  terrain files; ~4050 KB of 4096.
- Tools: ares ARES_SDFILL=n (SDRAM line fill cycles, MiSTer-like); /opt/work/mb.sh NAME IDX [flags]
  (GAMEDEMO+GAMEPROF measurement build); sim: SFXLOG, SKYLOG, BGPLOG, SPAWNLOG, WAILOG, WATCHSPR
  (R65_ITRACE), RDWATCH=lo-hi (ROMB_CHECK: PCs reading a ROM range), WRAMDUMP=file.
- Logic speedups (ares interpreter, JH GAMEDEMO f60-359): G 47 -> 44, T 95 -> 88: native OAM builder
  in SDRAM (game_hle.c: hle_oam_build, hle_oam_pose845; the translated BBA849/BBAADF/BBAC7E/BBA845 left
  sdram.txt), 'dw' accesses (r65.py: absolute < $2000 with an untraced DB -> WRAM lo when DB is a
  system bank or $7E, checked inline), rd*_sys / rd*_l_i inline ROM paths for pointer idioms
  (LDA $0002,y with Y in the ROM half), no libgcc variable shifts in the HUD / mark / overlay loops
  (bit32[], shr8()).  sdram.txt trimmed by 12 entries to fit (__end 0603EA50).
- Caves (Reptile Rumble, BG3 foreground overlay at 1.5x): restore under the old overlay pass moved to
  the slave's early job, rows adaptive (sh->ov_rows vs the logic time); B 43 -> 38, T 117 -> 115.
  The overlay redraw is still most of the cost (draw_tile_row / put_row8).  -DNO_EARLY_OV, -DJ3HUD
  (HUD L field = the slave's job 3 time), -DPHASE_BG (ares profiler: BG phase).
- Ripple levels (Coral Capers) had no sprites at all (the sprite pass skipped ripple): now drawn
  (they wobble with BG1 by the ripple's 1 px).
- Test 5 (2026-10-18 evening): JH user-like build (1 HUD line, GAMEDEMO) M ~79% avg in interpreter
  ares (was ~83 in test 4, ~95 in test 3).  Changes: sprites from a chunky tile copy (spr_ck =
  map_mem lower 16 KB for levels without overlays / rain / lamps / beam; converted lazily on first
  draw: ck_dirty; h flip = nibble reverse), compact visible-sprite list built once by the master
  (spr_build_list), sprite marks gathered locally, sprite split balanced on the master's wait, the
  slave writes the canvas line table in job 2 (lt_do), HUD restore in job 3 when it fits (hud_early),
  one HUD line in game builds (HUD2 / GAMEPROF / PROFT = two), HUD M / S % averaged over ~16 frames,
  sdram.txt re-picked (sdpick2 on the new logic profile).  spr_rwbuf moved to map_mem + 16 KB.
- Measuring: /opt/work/ub.sh NAME IDX (user-like GAMEDEMO build) + /opt/work/umeasure.sh (6 HUD crops);
  /opt/work/mb.sh ... -DPROFT: HUD line 2 = % of a frame between stamps: [start..logic] [logic +
  spr_prepare] [wait for job 3] [prep + BG band + line table] [sprites] [HUD..present] [present].
  JH (2 HUD lines): 00 44 05 08 21 09 31.
- Test 6: auto frame skip (main.c fsteps / fs_vc / fs_l / fs_r / fskip).  EMA estimates (1/8) of the
  logic per game frame (fs_l) and the rest per picture (fs_r); when one game frame + the render no longer
  fits (fs_l + fs_r > 94 % of a frame) and the last picture took 2 (3) vblanks, run 2 (3) game frames
  before the next picture: the game keeps its speed at 30 (20) pictures/s instead of slowing down (all
  game frames run: same physics, no added input lag).  Single late frames are not caught up (the
  catch-up frame would be late too).  Menu: B toggles (SKIP ON / OFF, bottom left).  Rain moves by
  fs_done per picture.  Builds: -DNO_FSKIP (mb.sh measurement builds use it), -DFS_FORCE=2 (test:
  always 2), -DFSHUD (HUD S field = game frames per 60 vblanks).  ares: Reptile Rumble 30 pictures /
  60 game frames per s (was 30-44 / 30-44); Ropey (rain) 21-35 pictures, 40-60 game frames.
- M % now from frt_since (frames longer than 1.36 frames wrapped the 16-bit FRT difference).
- Slave profile (JH, ares): idle 40 %, draw_tile_row 10, tc_fill 5.8, cv_restore_spr 4, music ~27.
  Job 3 (restores + predicted strips) takes ~ as long as the logic (J3HUD: 44 vs G 45).
  Master sprite phase: tile8 50 %, spr_draw_band 20, waiting for the slave 17, ck_conv 13.
- Tried: all ROM-resident generated code at -Os (empty hot list): G 45 -> 55 (worse).  A WRAM layout
  with odd 16-bit words native: no (static even accesses WLW 1912 + 932 outnumber odd ones 881 + 651;
  code only 2 % smaller).
- ROM space (bonus rooms, Cranky, the full game): the 4 MB cart is full (~9 KB left).  SSF2 mapper
  test ROM tools/banktest (build.sh -> build/banktest.32x, 8 MB, "BNK" b markers at the end of every
  512 KB bank; the 68000 maps bank 9 into window 7 via A130FF): 4 bars, green with bank + 1 squares =
  OK (SH-2 before / after, 68000 before / after).  PicoDrive and ares: all four OK (SH-2 reads follow
  the 68000's bank registers).  MiSTer: sent to the user (cart.sv banks VA = S32X_CA when rom_sz > 2 MB).
  Plan if it works: windows 0-6 fixed (code, shared data), window 7 = the current level's pack / pages.
- Hot / cold split (r65.py, R65_SPLIT=0 disables, R65_SPLIT_T = runs per frame counted as cold): code
  of the SDRAM functions never run in the gameplay profile becomes separate ROM functions entered by a
  tail call (62 entries); SDRAM code 7.8 KB smaller, re-picked (sdpick2, budget 69800, __end 0x0603EFA0;
  keep >= ~0x400 below 0x0603F400: the master stack dips ~130 bytes below its 1 KB).  G 45 -> 43.
  R65_SPLIT_T=0.02 overflows the ROM (cold copies duplicate the hot tails).
- Test 7: + frame skip reads the pad again for the extra game frame; SDRAM hot/cold split + re-pick
  (G -2); sprite rows skip all-transparent FB words; the screen goes black while the game's screen is
  off (was the last picture, scrambled by the next screen's palette: garbage flash entering a level);
  GAME OVER (WAI 80DF7C): the words on black, then the title screen (WAI 80CF2B) -> our level menu;
  game-level HUD colours moved to CRAM 96 / 112 (BG colour 0 of palettes 6 / 7, unused): the game's
  sprite palette 7 is CRAM 241-255 and overwrote 253-255 (the grey / blue HUD box, Coral Capers).
  NOTE: rain colours 245-247 (Ropey) are sprite palette 7 colours 5-7 too.
- Tried: fused per-CPU band job (job 5: each CPU its band's cv_band + sprites, slave top band + line
  table + HUD; -DFUSED): ares T 86 vs 85, no gain.  -Os for SDRAM code (SD_OS): G 56 (worse).
  Dynamic dispatch (XC_PROF): 39 xcalls / frame - negligible.  I/O writes: ~53 / frame.
- 2026-10-18 night, user test 7 on MiSTer: bank test PASSES (SSF2 banking works for the SH-2s too).
  JH F21 M98 S27, Ropey F20, Coral F20, RR F23 (frame skip on): the MiSTer is ~2x slower than ares.
  RTL (rtl/ddram.sv): 32X SDRAM 0x06000000-0x06007FFF = BRAM, rest DDR3 behind a 4 x 32 B read cache;
  but the earlier memtest showed BRAM and DDR fills cost the same (~30 cycles: the SH bus), uncached
  SDRAM reads ~29 (ares 5), cached loop 1.6x slower than ares.  "CPU Turbo" option is not wired.
  ares knobs: ARES_SDFILL=30 (line fill), ARES_UNCSD=11 (extra cycles per uncached SDRAM word read,
  new: ares_in_fill flag in cache.cpp / bus-internal.cpp).  JH (2 HUD lines) T 84 -> 112 with both:
  logic G 44 -> 68 (fills: ~3300 SDRAM line fills / frame in the logic, 2/3 code), S 21 -> 27.
  -Os for game code still worse (G 80).  Logic cycle split (M model): calls / returns ~20% (JSR/JSL/
  RTS/RTL + r65_call / rts helpers + xcall), OAM path (hle_oam_build + oam_pose + spr_prepare) ~13%.
- Test 8: sprite rows / list read cached (the master converts the changed tiles in spr_prepare, the
  slave purges their lines: spr_slave_sync; CK_LMAX list, else whole-cache purge) - M model T 112 ->
  108; hang watchdog (master V-int: 300 vblanks without a main-loop iteration in a level, or a game
  error -> "STUCK" page with dbg_stage, slave job, last xcall key, interp PC, WAI, entrance, frame,
  error, and both SH-2 PCs from the V-int stubs mv_stub / sv_stub in start.s; map PCs with the ELF
  saved next to the ROM: /opt/work/dkc32x_gameplay_w1_test8.elf); Coral: no ripple in game builds
  (the sprites share BG1's layer; -DGAME_RIPPLE); HUD "F60 M98 S27"; SDRAM budget 68200 (sdram.txt).
  Measurement ROMs (GAMEDEMO, PROFT, NO_FSKIP, J3S = S field is the slave's job 3): line 2 = % of a
  frame [0 .. logic] [logic] [wait job 3] [prep + BG] [sprites] [HUD] [present].
- Bonus lock-up (user, JH): not reproduced (sim + ares: entrances $06 / $1A and back, falling out of
  bonus 1 - fine).  Test hooks: sim GOTO=frame:entrance,...; 32X -DDBG_GOTO=f,ent,... (game.c
  game_debug_jump: runs CODE_80806F = the cheat-code entrance loader instead of the next NMI).
- User MiSTer measurement (test 8 ROMs, PROFT line 2 = start / logic / wait j3 / BG / sprites / HUD /
  present, % of a frame): jungle F30 M98 S(j3)70: 00 73 11 11 27 08 65; cave F30 S49: 00 66 07 52 32 08 30.
  ares-M (SDFILL 30, UNCSD 11) jungle: 68 / 03 / 06 / 23 / 09 - close for the master, the slave is
  slower on MiSTer (music: light mode read samples UNCACHED = 29 cycles each there).
  STUCK pages (both): master stage 4 waiting for the slave, slave job 0 with its PC in mixout (music
  interrupt): the music refill no longer fit its 5.8 ms buffer -> the slave never left the interrupt.
  Happens once a song starts while music_light is on (frame rate < 57: after a death, a bonus exit).
- Test 9: samples always cached (smp_or 0); overload guard in music_irq (refill > 70 % of the buffer
  time -> half-rate voices for 512 refills; > 100 % -> the next buffer silent); the slave purges its
  whole cache before its sprite band (spr_slave_sync: CCR CP) instead of per-line purges (sprite tile
  glitches on MiSTer while moving: the slave skipped purges on frames it drew no sprites, or the
  MiSTer purge differs); frame skip margins: 1 step if L + R <= 97 %, 3 steps only if 2 L + R > 100 %
  x 2 (MiSTer jungle 2 L + R = 192 % -> 30 pictures/s instead of 20).
  Leaf-call stats (R65_LEAFSTATS): only ~94 JSR / frame - the "call overhead" in the M profile is
  cache misses at function boundaries, not call count.
- User MiSTer test 9 measurements: cave F29 S(j3)66: 00 66 07 53 33 08 40; jungle F30 S81: 00 72 11 11 27 08 66.
  Sprite tiles still scrambled while moving -> not the slave's cache.  Test 10: the chunky sprite tiles
  and the sprite list are read / written UNCACHED again (as in test 7, which had no glitch); the master
  still converts the changed tiles once per frame (spr_prepare).  Suspect: MiSTer CACHE.sv write-hit
  update condition (CACHE_AREA && HIT && CCR.CE && (!IBREQ || IBUS_END)) - possibly a data write hit
  that does not update the cached line (same-CPU staleness).  spr_slave_sync is a no-op now.
- Test 10: menu C toggles "FG ON / OFF" (cave foreground overlays, default OFF = performance mode;
  A / Start enter the level).  Reptile Rumble without the foreground: ares T 115 -> 75 (60 fps),
  M model 99.
- User MiSTer test 10: cave (FG off) F34 S54: 00 70 05 08 28 08 70; jungle F30 S82: 00 73 10 11 30 08 63.
  Sprite scramble still there (uncached reads too) + Diddy's colours "psychedelic" when JH turns dusk.
- Test 11:
  * wait_flip drains the 32X VDP frame buffer write FIFO first (a FB read waits for FIFO empty in
    MiSTer VDP.sv); MiSTer swaps FS at once in vblank and FIFO entries drain into whichever buffer is
    the draw buffer then -> late sprite / HUD writes landed in the wrong buffer: unrestored leftovers
    = "mismatched tiles while moving, gone when paused".
  * ROM page coverage: sim free-movement sweeps (POKE=0:535:1 = DKC's FreeMovementDebugFlag $0535,
    scripts RIGHT / RIGHT+UP / RIGHT+DOWN then LEFT, ROMB_CHECK + WH_CHECK) over the 6 levels: 17
    missing pages added to memprof (JH dusk: sprite spawn scripts B5D543.. - likely Diddy's palette;
    JH B68EE0 movement tables, Ropey 19E800, Coral 006947 code page / 25B219..).  /tmp sweep.sh.
    No dummy-WRAM-page reads.  sim: TP=start,every,dx (move slot 1 = DK), POKE=frame:addr:val.
  * ROM space: worlds without levels in the build share one map picture (pack 1100 -> 818 KB).
  * In-level options menu while the game is paused ($0579 bit 6): RESUME / FRAME SKIP / FOREGROUND
    (rebuilds the renderer: game_reload + load_level) / EXIT TO MAP (sets the level's cleared bit
    $0583+level ($0563, EA -> 1) and sends Select - the game's own exit; the bit is restored on the map)
    / LEVEL SELECT (our menu).  text_cv / box_cv draw through the line table (CRAM 96 / 112).
- 2026-10-19 user (test 10 in ares on their PC: smooth; test 11 on MiSTer): sprite scramble still there
  (also in ares), JH dusk Diddy colours, Ropey: the whole screen garbage once the rain clears, Coral
  STUCK ERR 208059B9 (xcall to 80:59B9 = I/O space, ENT BF, after a hidden banana near the start; not
  reproduced in the sim), blank columns past the level's right end / Coral.
- ROOT CAUSE (all three glitches): WRAM pages classed load-only "scratch" (they alias the master's tile
  cache, tiles_ram) are used during levels: $7F2779.. = the SPRITE PALETTE BUFFER (pages 7F27-7F33:
  palette ring $1A8F sources -> sprites in noise colours "scrambled", Diddy psychedelic), 7F08-7F13 =
  palette fade buffers (CODE_B6B324: 7F04A0 / 7F0D58; Ropey's night -> day fade wrote 7F0D-7F10 ->
  tile cache = garbage pixels AND colours), 7E:D100.  The tile cache overwrites them whenever it bakes.
  Found with sim3: SCRATCH check in wh_chk (scratch page used while the level is on screen), "scratch
  written" message now lists the pages, SCRIBBLE=1 (random tile cache every frame), sim_vs =
  -DVRAM_SHADOW (byte-exact VRAM copy, spr_check: our OBJ tile pointers vs VRAM: all equal).
  Fix: whcls.txt class 1 for 0D1, 108-110, 111-113, 127-133 (89 pool pages, 110 scratch).  SDRAM: master
  tile cache 512 -> 448 entries (index bake & 511, >= 448 folded -128; shared_t.tcn), tiles_ram +1280
  gone, pose_buf 784 -> __end 0603ECD0.  m_main: red screen if game_nscratch * 256 > the cache data.
  Re-verified: free-movement sweeps (5 pads x 3 teleports) + 8000-frame autoplay, all 6 levels: no
  scratch page used in levels (BF hits = the out-of-level garbage run only).
- Rain colours in game levels: CRAM 16 / 32 / 48 (BG colour 0 of palettes 1-3, never a BG pixel) - 245-247
  are the game's sprite palette 7 (red rain at Ropey's end).  rain_ci[].
- Wide view at a level's right end: the game's camera range is $1B23 .. $1B25 (CODE_809C8F); the 32X
  view (64 px wider on the right) now stops at $1B25 - 64 and the sprites move right by the difference
  (spr_xoff in spr_build_list).  OAM builder culls widened 64 px to the left too (rompatch: ADC $30 ->
  $70 / CMP $1E0, big: ADC $58 -> $98 / CMP $230; the HLE reads both from the ROM).  Narrow camera
  ranges (< 64 px of travel, bonus rooms) keep the blank part.
- 60 FPS MODE (pause menu, default off; -DPIPE_DEFAULT=1): fused job 5 with every row and every sprite
  on the slave for game frame N while the master runs the logic of N + 1; the picture is presented after
  that logic (pipe_pend: wait C_DONE, present).  +1 frame of input lag.  Sprite bands of 16 lines are
  shared: the slave takes them from the top, the master from the bottom once its logic is done (pb_next
  / pb_low / pb_go; a band taken by both = same pixels).  The master takes the sky H-int.  (Tried: the slave
  drawing with the master's 448-entry cache - not clearly faster, and unsafe: the game's load-only
  WRAM pages alias that cache and a load can start in the logic frame the slave is drawing over.)  Only the fused levels (cv 1, no
  ripple / rain / overlays / lamps, spr_ck on); others run the normal path.  Frame skip in this mode:
  R = max(slave job - L, 0) + 1000.  ares (interpreter): JH M 51-59 S 55-68 F 57-60 (normal M 69-81);
  ares-M (MiSTer model): F 35-56 (normal F 28-41) - the slave is the limit there (music ~28 %, BG +
  restore under the old sprites ~25-30 %, sprites).
- JH bonus rooms (sim GOTO 06/08/1A/6E/25): missing ROM page F68 (FDA03C) added; A00 (E80000 = the
  language select text) only via the debug entrance path.
- Test 12 = all of the above (/opt/work/dkc32x_gameplay_w1_test12.32x + .elf).
- User test 12: sprite TILES still mismatched (colours fine now), worse after dying; 60FPS MODE on MiSTer =
  slow motion (S ~40 %); Coral blank / "misaligned" strips; feels slower; small noisy tiles.
- REAL sprite-tile bug (since test 8): spr_prepare converted the changed chunky tiles BEFORE applying the
  game's queued uploads ($170F) -> tiles changed by this frame's queue were drawn stale (last frame's
  graphics) until the next frame: mismatched pieces while animating, never while paused.  Now converted
  after the queue.  sim: CKCHK=1 attaches a chunky buffer, spr_check compares every visible tile's
  chunky rows with the shadow VRAM: old order 40+ stale per level, new order 0 (6 levels x 6000 frames).
- 60 FPS mode frame skip: the estimate was max(logic, slave) - but the master also draws sprite bands
  and waits; iterations took 2 vblanks with 1 logic step = slow motion.  Now R = the measured time from
  the logic's end to the picture being ready (pipe_rest) + 1500.
- Camera blocks: CODE_80C246 HLE (hle_cam_clamp, byte-exact vs the interpreter: sim_fi FORCEI WRAMDUMP)
  keeps CODE_B8A853's block range [$50, $52]; the view limit = min($1B25, block max) - 64 (eased 3 px /
  frame when the block changes).  Coral blocks e.g. 0..704, 0..864, 864..944.  JH etc. do not use it
  (falls back to $1B23 / $1B25).  Blocks with < 64 px of travel still show up to 64 px past the block.
- Speed check (ares-M, PROFT, JH): current logic 71 % vs 68 % for the test-9 measure ROM, rest equal.
  The wider OAM cull costs nothing measurable (WS_LEFT=0 build: same G / T).
- Test 13 = the above.

## 2026-10-19 world-1 completion plan (user asleep until 11:00; log every fix in BUGLOG.md)
User list: Necky nuts straight down (B4 fixed), bonus rooms graphics + music, day/night after a bonus,
snake hiss (B6 fixed: DSP noise), Croctopus invisible (B5 fixed), ripple back (B8), map screen 2 of
world 1, sticky music (song follows the game), animal bonus stages, flashing bits, 60FPS MODE slow
motion on MiSTer (B7 fixed).  ROM is full (4 MB) -> SSF2 banking:
- Window 7 (cart 0x380000-0x3FFFFF, SH-2 0x02380000, 68K via $A15104 bank 3 at 0x980000) = a level
  bank (512 KB, physical bank 7..15 of an 8 MB ROM), switched by the 68K (A130FF) on SH-2 request
  (C_CMD 0xA000 | bank).  Both SH-2 caches purged after a switch.
- Banked: per level/room pack data (tileset copy, grid, palettes, overlays, md blocks, weather) +
  its snapshot WRAM part; world map images.  Resident: code, pack tables, font, music, gtext.
- Snapshot OBJ VRAM -> resident dedup tile pool (game_obj_pool, 469 tiles for 6 levels) + per-entrance
  index tables (sprites.c SNAP_TILE).
- Rooms: every world-1 entrance surveyed (/opt/work/survey/EE); rooms = unique layouts; pack has an
  entrance -> room table; load_level picks the room by $3E.

## 2026-10-19 ROM space for more worlds (decision record)
- MiSTer S32X core (rtl/CART/cart.sv): SSF2 banking is supported for all 7 windows (6-bit banks, up to
  32 MB) when the ROM is > 2 MB; the 32X side's cart accesses go through the same mapper.  ares and
  PicoDrive run the 8 MB banked test 14.
- Resident (below the banked windows) today: 0x40000 code + pack (music 414 KB) + compiled game code
  (~390 KB) + SNES pages (2.4 MB: DK + Diddy + animals + cabins ~1.2 MB of sprite graphics, the world's
  enemies, level files, tables).  World 1 alone ends at 0x372180 of 0x380000.
- Plan for one ROM with all worlds: cart window 6 = a per-world bank with its own snes_pg table at
  0x02300000 (linker defsym; game_tab.c guards the host table with SNES_PG_BANKED) + that world's
  enemy graphics and level files; resident <= 0x300000.  mkgame.py world_split() (WORLD_PROFILES)
  is written: world 1 = 411 KB incl. 89 KB of compressed BG graphics, which an HLE of the graphics
  list walker CODE_B9A924 could skip (dest >= $2000 = BG VRAM, not used by the 32X).
  Open: character lists per world (DMA logs miss statically loaded ones - use the level sprite data
  types, sprdata.py), music split (resident jingles + per-world songs), the per-song ARAM images are
  12.5 KB each and mostly identical (engine code) - dedup would save ~8 KB per song.
- Until then: one ROM per world (w2b.sh), same code.
- 2026-10-19 late (test16 / w2 test2 delivered): F1 items culling, F2 cabin text, F5 lamp swing =
  CODE_80B891 from $28 (C_FRAME = $28-1 in lamp levels), F7 lights from $0579 bit $200 + HUD
  CRAM 96/112 kept, F8 millstone pseudo-sprites (block kind 7; perf: F30 with the full picture on
  screen in ares), F10 lamp CRAM moved to sprite colour-0 slots + lamp_frame CRAM writes removed.
  After delivery: F11 hardening (set_bank waits for an idle slave; map_setup bumps the epoch).
  Open: F3, F4, F6 (layer analysis in BUGLOG), F9 (not reproduced in sim), millstone perf.
  Pitfall: never run two build.sh at once (shared build/ tree).
- Music pack dedup (all-worlds ROM space): song ARAM images 0..$31FF in 64-byte chunks, one shared
  pool; song record off_img = u16 chunk table [200], off_ram = the pool; sh/spceng.h spc_rdx reads
  pool[ctab[a>>6]*64 + (a&63)] (a >= $3200 -> 0), musplay.c fills ram[0x600] from chunks 0-23.
  24 songs: images 300 KB -> 79 KB, pack 553 KB -> 328 KB.  Verified: player_test renders (20 s of
  songs 0/3/7/12) byte-identical to the old format (scratchpad mus/old vs mus/new).
  World 1 ROM: GAME_BASE 0xAA000 -> 0x89000.

## 2026-10-19 night: all-worlds ROM - window-6 world banks WORKING for worlds 1+2 (/opt/work/w12b.sh)
- Layout: resident <= 0x300000 (code, pack, game code, common SNES pages); window 6 (0x300000) =
  world bank (physical bank 6 = world 0's; others after the level banks); window 7 = level bank.
  Each world bank = snes_pg[4096] (u32 BE SH-2 pointers, linked at 0x02300000 via
  -DSNES_PG_BANKED + defsym _snes_pg) + that world's pages.  tools/worldbanks.py builds the banks
  after the link (needs _snes_blob/_zero_page from sh.elf); finalize.py places them.
- Runtime: set_bank6() (68K cmd 0xA100|b -> 0xA130FD; waits for an idle slave, purges caches,
  epoch++).  Bank chosen by: menu level start (level flags bits 16-21), load_level (safety),
  map_setup (mapscr_t pad[0] / world_t pad[0]).  Levels are only entered from their own world's
  map, so following the map screen is enough.  build_viewer writes the bank numbers at the end
  of write_pack from build/game/worlds.json.
- mkgame world_split: WORLD_PROFILES 'K:cpu:dma'; unclaimed needed pages -> resident (the DMA logs
  miss characters: Croctopus, Chomps...); WORLD_RESIDENT = page profiles read from every world
  (tools/recomp/data/memprof_rooms.txt: the 4 animal token rooms, 596 pages, sim PAGELOG).
  BLOB_FILES 'K:path' = world K only.  Music dedup + GFXSKIP (below) made it fit.
- GFXSKIP (rompatch.py, default on): graphics-list entries (CODE_B9A924) that decompress BG
  graphics (VRAM >= $2000, not the cabin text $7600-$77FF) become a 2-byte DMA; temple's direct
  JSL B896FC (E73BDE) NOPed.  mkgame drops those 13 files.  World 2 bank 669 -> 451 KB.
- sim3 checks (rebuild with ./mksc.sh after every build; sim/blobs.s now links
  snes_blob_host.bin = every page): WORLDBANK=i reports reads of pages neither resident nor in
  bank i; PAGELOG=file (+PAGELOG_F0/F1 frame window) logs pages read.  Verified 0 violations:
  every w1/w2 level (2000 frames, TP), every animal room from both worlds (GOTO, 2 TP patterns).
  (A first round found real ones: rooms' data was in one world's bank only.)
- ares: JH (bank 6) and Winky's Walkway (bank 12) autoplay from the banked ROM fine.
- OPEN: map loads read pages before map_setup switches the bank (profile a map load / walk
  between worlds on the map; Funky's flights); worlds 3-6 profiles; Galleon (no world).
- Sizes (w1+w2): resident end 0x2E9930 (~90 KB free), banks: w1 281 KB, w2 451 KB.
- WORLD_CHARS (tools/world_chars.py -> tools/recomp/data/world_chars.json): per-world character
  lists from the level sprite lists (sprdata.json, NorSpr -> graphics group aliases).  Unclaimed
  resident pages 211 -> 128 (the rest: worlds 3-6 characters).  Map-screen pages resident too
  (memprof_maps.txt: PAGELOG over the map visits after deaths in all 12 levels, 362 pages).
  w1+w2: banks 358 / 416 KB, resident end 0x2E4130 (~110 KB free).  Re-verified: 0 WORLD reads.
- 2026-10-19 night, all-worlds build (/opt/work/allb.sh, 16 MB, 17 level banks + 7 world banks incl.
  the Galleon = "world 7" with K. Rool; resident end 0x2E3820): sim_sc checks over all 40 levels
  (WORLDBANK per world, 2400 frames, TP): no game errors, no dummy WRAM, no SCRATCH; 6 WORLD reads
  fixed in world_chars EXTRA (MasterNecky in w3 Clam City, CavePlatform w4, MinePlatform w6);
  MISSING = intentional drops (map / intro / game-over screen graphics, fog) except
  Palettes/FGBG_UnknownPalette9.bin (level inits, all worlds -> plain BLOB_FILES) and
  Tilemaps/Map32/UnderwaterLevels.bin (Croctopus Chase: 2 pages -> BLOB_FILES 1:,3:,4:).
  These fixes are in the scripts; not rebuilt/verified yet.
- Worlds 3-6: snapshots (gp/capsnap.sh) + hostrun profiles memprof_w3..w7 (w7 = Galleon 68);
  WRAM: 7E:BC00-BF00 -> class 10, 7F:1400/1500/1D00/1E00 -> 9 (pool), 7E:3100-3F00 + 7F:1F00 -> 8;
  SDRAM: the last 7 sdram.txt functions moved to ROM (backup /tmp/claude-0/sdram_pre_w36.txt).
- DELIVERY (decided 2026-10-20, after the all-worlds build worked): stop handing over built ROMs /
  ELFs (each one is a complete converted copy of the commercial game).  The user builds from their
  own ROM with the build tool - their own stated model for anything beyond personal tests.  The
  package = sources + scripts only: exclude build/, captured data (lamp.npz, snapshots, surveys),
  and code generated from the game (build/g65; sh/spceng_gen.c must be generated from the user's
  ROM at build time).  Keep test builds / emulator captures inside the workspace.
- ares smoke test of the all-worlds ROM (autoplay, 22 s): Snow Barrel Blast F60, Oil Drum Alley F59,
  Tanked Up Trouble F56, Galleon running (HUD F03 right after the load - recheck), Vulture
  Culture showed the map (demo died) - recheck.
- Build-tool package v2 delivered (scratchpad pkg/dkc32x_buildtool_v2.tar.gz, 615 KB, 121 files):
  sources + scripts + metadata only; pipeline/ = harness.c, capsnap.sh, run.sh, allb.sh, allu.sh
  (paths via DKC_* env), snes9x_trace.patch (my trace hooks); BUILD.md.  Excluded: build/, out/,
  sh/spceng_gen.c (build.sh regenerates it from DKC_DISASM - byte-identical), sh/comp.s.
  Checked: no file contains a 48-byte run of the ROM.  tools/paths.py holds the DKC_* defaults.

## 2026-10-20: Windows build tool work (user: Win10, portable, GUI / drag-drop, any ROM version, PAL)
- Disassembly dependency REMOVED from the build: tools/layout.py builds tools/meta/layout.json.gz
  (342 KB: instruction boundaries + operand sizes, labels, refs, incbin map, asset table, SPC
  engine items + an opcode format table learnt from the listing vs the ROM).  disparse.parse()
  without a path uses it (mnemonics read from the user's unpatched ROM); mkgame's six scans,
  asset_index and spc2x (ROM mode: `spc2x.py ROM c out.c`) use it.  Verified: full build with
  the disassembly folder renamed away -> byte-identical 16 MB ROM, identical spceng_gen.c.
  (Generated C comments now show mnemonics, not disassembly text.)  Regenerate the layout only
  when the analysis changes: `python3 tools/layout.py build DKC1_DIR`.
- tools/build.py = cross-platform build (all-worlds settings, parallel compile, progress '##' lines,
  prebuilt 68K side tools/prebuilt/md.bin, toolchain dir --toolchain / DKC_TOOLCHAIN, PATH
  fallback).  Verified byte-identical to allb.sh/build.sh.  tools/romcheck.py first (header /
  zip / version via the cartridge header, md5 list SUPPORTED).  Windows sh-elf toolchain:
  canadian cross in /tmp/claude-0/xsrc (build_win_sh.sh -> /opt/sh-elf-win; gmp dfsg tarball
  needed a stub doc/Makefile.in).
- tools/capture.py: every emulator capture from the ROM alone (cold boot + fixed button script to
  the Kongo Jungle map -> wmap.st; 114 surveys (vram.bin + bg.bin only - all the build reads);
  40 level-start snapshots (trace core, TRWAI); lamp.npz + weather.npz).  46 s on 2 CPUs.  One core
  is enough: plain and trace snes9x give identical dumps.  Auto captures differ from the old
  hand-made ones (other start state: frame counter / RNG / Kongs) -> not byte-identical ROMs, but
  the ROM built purely from them passes the sim checks on all 40 levels (only known BG drops) and
  plays in ares (menu, Slipslide Ride F60).
- Portable Windows bundle (tools/mkbundle.py -> DKC32X_Builder/): "DKC32X Builder.exe" (tools/win/launcher.c,
  mingw -mwindows -municode, starts app\runtime\python\pythonw.exe app\tools\gui.py [dropped file]),
  "Convert (drag ROM here).bat" (python.exe app\tools\convert.py), README.txt, licenses/ (GPL/LGPL/GCC RLE,
  snes9x licence + trace patch, toolchain build script, SOURCES.txt).  app/ = sh, md, tools (minus
  banktest, spceng_gen.c, comp.s, old drafts), emu/ (harness.exe, snes9x_libretro.dll), toolchain/sh-elf
  (/opt/sh-elf-win), runtime/python (/tmp/claude-0/winpy/python).  All Windows exes import only system
  DLLs (KERNEL32/ADVAPI32/msvcrt).
- tools/convert.py = romcheck -> capture -> build in one go (work/ under app; deleted unless --keep);
  tools/gui.py = tkinter window (romcheck.identify on file choice, Convert, progress from '##' lines,
  CREATE_NO_WINDOW so no console windows, "Show the 32X file").
- tools/scanbundle.py ROM DIR: no >= 32-byte non-trivial ROM run in any file (+ .gz/.zip contents + hex
  lists in text).  Positive-tested.  Whole bundle: our files CLEAN; 2 coincidental hits in third-party
  binaries (python unicodedata.pyd 53 B, numpy's msvcp140.dll 32 B) - tables, not game data.
- Wine 9 testing quirks (NOT problems on real Windows 10): numpy 2.x needs ucrtbase crealf (missing) ->
  bundle now ships numpy 1.26.4 (also friendlier to old Windows); Python 3.12 shutil.copy uses
  CopyFile2 (missing in Wine 9) -> test copy only gets a sitecustomize that deletes _winapi.CopyFile2;
  Windows python needs stdin/stdout to be real handles under Wine (`< /dev/null | cat`).
- capture/lamp_capture.py + weather_capture.py: os.system('cp') -> shutil.copy; temp dirs under
  paths.TMP (default tempfile.gettempdir()); astype(np.int64) (Windows int = int32).
- Wine end-to-end (2 CPUs): GUI launched with a dropped headered .smc ("[!]" in the name) -> 16 MB ROM
  byte-identical to the Linux auto build (/tmp/claude-0/all_auto.32x), 102 s total.  Fixes found on the
  way: mkgame snapshot args 'path:EE' broke on 'C:\' (now rsplit on a trailing hex entrance).
- PAL (2026-10-20): the ROM is region-free already (header "JUE", no 68K region check).  SH-2:
  pal = !(32X VDP reg 0x4100 bit 15); gtick (60 Hz ticks) = vcount + 1 extra every 5th vblank on
  PAL; the game loop's owed = gtick delta -> fsteps up to 3 (owed > 6 = load/pause, ignored);
  frame_ticks 57348 on PAL; FPS counters per 50 vblanks; music_light threshold 48 fps on PAL;
  music PWM cycle + refill budget from the PAL SH-2 clock 22.801467 MHz (music.c SH2_HZ).
  -DPALSLOW = 50 Hz pace (no extra ticks), -DFORCEPAL = test.  68K (md.bin rebuilt, prebuilt
  updated): md_pal = VDP status bit 0; glow + weather-snow tile animations run twice every 5th
  vblank.  Header title now "DONKEY KONG COUNTRY 32X (PERSONAL BUILD)".
  Test: PicoDrive harness HARNESS_OPTS=picodrive_region=Europe, -DFSHUD build: NTSC F60 S60,
  PAL F50 S60 (60 game frames per second at 50 pictures).  ares NTSC boots to the menu as before.
- Bundle trims: toolchain exes stripped, share/include dropped (62 MB); python: no _test*, sqlite,
  tix, idlelib, test, ensurepip.  Zip 64 MB.
- Final bundle check (fresh unzip under Wine, "Convert (drag ROM here).bat" with a headered .smc inside a
  .zip): 102 s, ROM byte-identical to the Linux build of the same sources (/tmp/claude-0/pal_final.32x).
  Bug found: trimming the toolchain with ignore_patterns('include') also dropped gcc's own
  lib/gcc/sh-elf/13.3.0/include (stdint.h) -> only the TOP-level share/include are skipped now.
  Wine: run .bat with `wine "X.bat" args` (wine cmd /c mangles quoted names).
- Delivered: scratchpad DKC32X_Builder.zip (64 MB).  Rebuild: tools/mkbundle.py OUT --python
  /tmp/claude-0/winpy/python --toolchain /opt/sh-elf-win --core /tmp/claude-0/s9xwin/libretro/snes9x_libretro.dll
  --harness tools/emu/harness.exe --launcher (tools/win/launcher.c built with mingw), then zip.

## 2026-10-20: complete game - first steps
- Power-on snapshot: capture.py boot_snapshot -> snaps/100.wai = the first WAI in the main wait loop
  80C0B1 (frame ~76, after the boot code's SPC upload; trace core TRWAI=cb between 'dump x 0x1010 1'
  and 'dump z 0x1016 8').  mkgame key 100 (3 hex digits) -> build/game/snap_100.bin; build_viewer
  packs it resident, pack header off_boot (bytes 44-47).  main.c: hold START at power-on (or
  -DFULLGAME) -> load_level(0) for a renderer state, game_init(boot), game_on: the game runs its own
  flow; screens without pictures show their sprites on black (map / cabins keep their pictures).
- sim3 from power-on (sim_vs 100 N, SCRIPT pad): Rare logo / Nintendo / intro 80C0B2, title 80CF2B,
  80F421 (file select), 80F34F (player select), 80FBBD, 80E163 (opening), map 80E90E (ent EB) at
  f 1522 with START,START,B,B,B; B on the map selects JH (ent 16) but did not enter yet.
  Found: dummy WRAM pages 7F:23-26 at the first map (key B89333) - need classes; WORLD reads of
  title graphics (decompressor B999AD) - BG data, harmless unless rendered.
- Screen plan: pictures per screen from the capture (like maps) + live BG3 text (like cabins) for the
  file / player select; sprites live.  Saving: SRAM (2 KB) - SSF2 + cart SRAM to be designed.
- SRAM research (2026-10-20, from ares / PicoDrive / MiSTer sources):
  header $1B0 "RA",$F8,$20, start $00200001, end $00200FFF (2 KB odd bytes; $1B3 bit 6 must be 0 =
  not EEPROM).  68K only: byte write $A130F1=1, save $A15104, set it to 2, SRAM byte n at $900001+2n
  (move.b only), restore $A15104, $A130F1=0.  RV=0.  NEVER write $A130F9 (SSF2 slot 4): PicoDrive then
  loses SRAM reads.  MiSTer: while $A130F1=1 the whole cart $200000-$3FFFFF reads SRAM (resident pack
  + windows 6/7 gone) -> both SH-2s must stay off cart ROM during the access; a 68K $A130Fx write during
  an SH-2 ROM cycle may be lost.  SH-2 SRAM access: not portable (none in PicoDrive).  ares: SRAM only
  via $900000 window with RV=0 (direct $200001 needs RV=1).  Fresh SRAM: $FF ares, $00 PicoDrive,
  undefined MiSTer -> magic + checksum; libretro skips writing an all-zero .srm.  MiSTer needs a save
  file mounted when loading the ROM (64 KB .sav, words {FF,byte}).
- Front-end screens (2026-10-20): TM shows no OBJ on title / file select / player select / (c) /
  Nintendo / Rare (only the Cranky intro TM 13 has sprites, uploaded from ROM pointers).  All VRAM
  for a screen is written in its setup frame (forced blank) before $1C changes; nothing after (in the
  menu flow tested).  Map bases 7000-7FFF.  tools/snesppu.py (mode 1/5/7 BG renderer -> CGRAM index
  picture) matches snes9x for title / file select / player select.  Player select highlight = HDMA
  ch1 (to be decoded; color math CGADSUB 25 + fixed colour).  Credits = a LEVEL (entrance 5E, stage
  with the cast walking + names); ending: K. Rool -> full hoard (4C) -> credits (5E) -> JH.
- Front-end renderer DONE (sh/fescr.c, included by main.c, ROM code): logos (mode 3/7/5 + HDMA
  per-line BG mode / TS / colour math), Cranky's intro (sprites + black-main add rule), title, (c),
  file select (text from the save data via the VRAM mirror), player select (BG2 glow added: exact
  colour sums in CRAM 128-254), then the game's own map.  PicoDrive: whole flow boot -> map works.
  * tools/capture.py fe_capture: power-on + FE_SCRIPT, per ($1C, BG mode after HDMA) the VRAM of the
    first fully-bright dump -> survey/fe/NNNN_M.vram + fe.json (3 s).  build_viewer: banked 64 KB
    VRAM per screen + resident table (pack header off_fe, header now 52 bytes).
  * sprites.c: fe_vm (VRAM 7000-7FFF mirror, every DMA / CPU write) + FE_TRK (BG scroll, mode 7 regs,
    COLDATA) live in tc_sdata (slave tile cache) while main.c lends it (fe_lend: blanked / map /
    cabin / front-end frames, slave idle; fe_take in load_level).  Cabin dialogue now read from the
    mirror (spr_bg3txt removed), cab_drawn in tc_sdata+8320 -> SDRAM __end 0x603EC20 (stack 3 KB).
  * game.c: game_ppu(a) (last written PPU/CPU/DMA reg), game_hdma_run (all enabled HDMA channels,
    modes 0-7, direct / indirect -> callback per write), game_debug_jump always on.
  * Boot: the power-on snapshot is taken AFTER the logo setup (its regs / HDMA not in the snapshot)
    -> main.c re-runs CODE_80BA2C (the reset code's logo setup) with game_debug_jump at full-game
    start.  Its loops were interpreted from ROM (4 s first frame): translate.txt += the front-end
    function keys AND the continuation keys (labels after calls whose returns come back through the
    dispatcher) found iteratively with sim_xc (XC_PROF, XCFILE) -> 1113 translated.
  * Host test of the renderer: /opt/work/sim3/fehost.c (FEHOST=rom.32x FESHOT=f,f.. -> /tmp/claude-0/feh/
    fNNNN.ppm; FESIG=1 prints the redraw signature per frame; JUMP0=0080BA2C runs the logo setup).
    sim env FEVM=1, SIGCHK, ICOUNT, FECHK added.
  * Bugs found on the way: master stack overflow into wram_pool (DMA clearing 7F pages smashed the
    loop counter) -> SDRAM freed (above); redraw ping-pong between the two frame buffers (pair table
    reset invalidated the other buffer each frame) -> FE->psig; multiplier use of M7A/M7B changed
    the signature every frame -> M7 regs only count when a line is mode 7; mode 7 drawn only inside
    the box of non-blank map tiles.
  * WRAM: 7F:23-26 = class 8 (scratch) for the first map (B89333).  memprof_fe.txt (WORLD_RESIDENT):
    the front-end tilemap source pages + player select table page BED.
- Front-end, round 2 (2026-10-20): PicoDrive whole flow logos -> intro -> title -> file select (cursor
  highlight moves) -> player select -> map; SNES A (Genesis C) on the map enters JH.
  * Speed: redraws now per LINE (FE_LSIG[2][224] in map_mem's lower half - the sprites' chunky copy is
    detached on these screens, load_level attaches it again): line sig = global (vgen, scroll of the
    BGs shown, BG regs, black-colour mask if sprites) + the line's per-line registers (+ the dark
    colour masks on colour-math lines).  Lines are drawn within ~2 vblanks per picture (vcount
    budget; FRT does not run in PicoDrive) continuing next frame at FE->ynext; mode 7 lines and the
    first 2 pictures of a screen are always complete.  Mode 7: odd lines repeat the even line above
    (line table), box map + chars copied to SDRAM (map_mem+2560 / +4608).  Tile rows merged with
    32-bit lane masks (fe_lanemask, FE_W for the host).  fsteps up to 3 on front-end screens.
  * Colour sums: never reset during a screen (pairs are index pairs, colours recomputed per frame);
    full table -> the brighter of main / sub; sub colours with r+g+b < 8 are not added, dark main
    + sub = the sub's own index (no pair).
  * Pages: sim WORLDBANK=0 over the front-end found world-banked pages B49 B4A 480 9BA 9CE C40 905
    (Rare logo maps etc.) -> memprof_fe.txt.  Run this check after any front-end change.
  * translate.txt: 6 continuation keys produced BADJUMP (analysis lacks code after them, e.g. 80E055
    in the player select setup -> game error 2080E055) -> removed (interpreted); listed in a comment.
    Check new keys: grep BADJUMP in their generated function (script in this session).
  * FE state (tc_sdata+16128, <= 256 B) is cleared in fe_setup (it is lent memory: garbage
    otherwise - the host test had zeroed statics and hid this).
  * Test aids: -DFEDBG (bars: vblanks / game frames / lines per picture), -DFEDBG2 (per-line regs in
    the left border), host FE_LINE_BUDGET (FEB=-DFE_LINE_BUDGET=20 sh mkvs.sh), FECOUNT, FELDIFF.
  * regress.sh fe1: levels fine (timing-shifted frames only) -> new baseline (old in regress/base_old).
- Battery saves (2026-10-20): header $1B0 "RA" F8 20, $200001-$201FFF.  68000 md/main.c sram_xfer
  (command 0x9F01 save / 0x9F02 load, 0x9FF0 = ready): A130F1=1, A15104=2, byte n at $900001+2n,
  word handshake COMM0 data / COMM2 seq (0xFFFF end) / COMM4 ack, interrupts off.  SH-2 main.c
  sram_save / sram_load: layout "DK32", sum16, ~sum16, 2048 data bytes (1028 words); load at full-game
  power-on (after game_init(boot), into the game's sram[] if magic + checksum hold); save when
  game.c game_sram_dirty (any changed SRAM byte) has been quiet for 20 frames on a non-level frame
  (slave idle; MiSTer maps SRAM over $200000-$3FFFFF meanwhile).  PicoDrive: saved (srm odd bytes
  "DK32..") and loaded back (harness: new 'loadmem FILE ID' command, Linux harness rebuilt with
  -I tools/emu).  -DSRAMTEST: forces a save at boot + line 7 bar = valid save loaded.
  tools/prebuilt/md.bin updated.
- Game over / ending (2026-10-20, in progress):
  * GAME OVER = front-end screen DEF9 (capture run 2: wmap + lives FFFF, JH, RIGHT 1800) - PicoDrive
    -DGOTEST (lives forced FFFF) shows it.  main.c's old GAME OVER branch skipped when full_game+off_fe.
  * Ending (sim JUMPF=30:00B884D7 = after K. Rool): Cranky speech EEA3 (front-end screen, ~1950 frames,
    capture run 3 via snes9x_trace TRJUMP=400:00B884D7 from the Galleon map) -> entrance 4C (full hoard,
    levels.json type hoard, Kongo) -> ~370 frames later entrance 5E = credits (a main-loop "level", $1C
    80AB / nmi 8096C4 not a known TYPE -> to be drawn by fescr, keyed by entrance) -> after the credits
    the game loops intro / attract (NMI 80CCB6 <-> 80DAE5).  Credits need 116 pages (memprof_w7 + all).
  * snes9x_trace TRJUMP="frame:key": at that frame the CPU is redirected to key (24-bit pc, mode bits)
    from 00:A968 (cpuexec.cpp).  Linux .so rebuilt; Windows DLL + licenses patch NOT yet.
  * levels.json appended 4C (hoard full) + 5E (credits); backup /tmp/claude-0/levels_pre_end.json.
- The ending WORKS in PicoDrive (2026-10-20, -DENDTEST = Gang-Plank Galleon's snapshot + CODE_B884D7 at
  game frame 30; -DBOSSTEST without the jump): Cranky's speech -> the full hoard (DK cheering) ->
  "CAST OF CHARACTERS" (cast walking + names) -> the staff roll -> back to the Rare logo / intro loop.
  * Credits (entrance 5E) and the full hoard (4C) are entrance-keyed front-end screens: capture runs 4/5
    (fe.json 'ent' + 'world'; table mode byte = 0x80 | the world's window-6 bank, patched by
    build_viewer).  fescr.c fe_find: in the main loop -> by $3E; main.c draws them from the fblank load
    branch (game_fblank stays set, no load_level).  The hoard as a LEVEL drew its bananas wrong: they
    are BG2 high-priority tiles in front of BG1 (the level renderer has BG2 behind the 32X layer).
  * main.c dark/blank frames (full game): window 6 = the bank of the entrance in $3E (fe entry's world
    or its level's), set before the game's next frame loads it.  The credits set $3E = 5E and load in
    the SAME frame -> their setup pages are resident (memprof_fe.txt: 3DF map, 3F9-3FF, 8C0-8C4, 8FF).
  * Credits pages / sprite characters: tools/recomp/data/dma_w7.txt = the sim's DMALOG over the whole
    ending (all cast characters -> world 7's bank, 42 KB left), memprof_w7 + 116 pages.
  * sprites.c: VRAM DMAs in mode 0 to $2118 / $2119 (one register, e.g. the credits' map attribute
    fill) were ignored -> handled as CPU writes (mirror, VMAIN).  spr_front_all: sprites behind BG1
    that colour math adds from the subscreen (the credits' names) drawn in front.
  * fescr.c is now 1 line lower: display line y = SNES scanline y + 1 (BG row y + 1 + VOFS, HDMA line
    y + 1) - matches snes9x exactly (was one line off on every front-end screen).
  * Sides outside the 256-px picture: CRAM 128 = black (FE_BORDER); colour sums moved to 129-254;
    sprites in the sides (SNES x >= 256 / < 0) wiped after drawing.
  * Speed (FEDBG bars: vblanks / game frames / lines / line time / sprite time): lines cost ~25k cycles
    each (cartridge char reads, 3 layers) -> (1) under the last sprites only their columns, from a
    clean copy of the line kept in the frame buffer's free space (FE_CLEAN, lines 12-211, 51 KB);
    (2) BG2 / BG3 scroll only in the signatures of lines whose map row has non-blank chars
    (fe_rowflag: char blank states + row flags cached per vgen in map_mem 14848-16127) - the speech's
    clouds no longer redraw the whole screen; (3) changed lines under sprites past the time budget:
    the stale clean copy (redrawn later); (4) fe_same() in the step loop (no HDMA run per extra
    step); fsteps up to 6 on these screens.  Speech: 5-6 vblanks a picture, game at full speed.
  * Profiling tools: pd_dbg core (/opt/ref/pd_dbg) PD_SH2LOG (master/slave PC per frame -> symbol
    histogram, /tmp/claude-0/pdp/prof.py), PD_SDDUMP=prefix PD_SDFRAMES=n PD_SDFROM=f (SDRAM 256 KB +
    frame buffers + 32X palette per dump; SDRAM is byte-swapped per 16-bit word).  -DDBGRING (main.c:
    per game frame $1C/$3E/WAI/$563/$565/key into a ring), -DFEDBGL (early-lend counters).
    sim3: WWATCH=addr (WRAM word changes), MAPWATCH (VRAM 7000-713F changes + fblank / dark),
    HOOKLOG (fe_lend_hook calls), sim_vlog = -DVRAMLOG build.
  * fe_lend_hook (sprites.c -> fescr.c fe_lend_early): a map write while not lent and the game's
    INIDISP shadow $51A is dark -> lend at once (slave idle) so that frame's writes are mirrored.
  * Full game: show_hud = 0 (no frame-rate line).
- COMPLETE GAME IS NOW THE DEFAULT (2026-10-20): power on -> the game; hold START at power-on -> the
  level viewer menu (-DFULLGAME: never the menu, -DVIEWERMENU: always).  README.txt "PLAYING".
  capture.py MAIN + 4C (hoard snapshot, viewer only).  Windows snes9x DLL rebuilt with TRJUMP
  (/tmp/claude-0/s9xwin: cpuexec.cpp / tracehook.* copied from /opt/ref/snes9x_trace, make
  platform=win CC=x86_64-w64-mingw32-gcc CXX=..g++), licenses/snes9x-trace.patch regenerated (diff -ruN
  snes9x snes9x_trace).  Bundle /tmp/claude-0/bundle2 -> scratchpad DKC32X_Builder.zip (66 MB);
  scanbundle: only the known unicodedata.pyd coincidence.  Wine end-to-end (.bat, 134 s): ROM
  byte-identical to the Linux build (12 front-end screens incl. the ending speech via TRJUMP).
  PicoDrive: boot -> logos -> intro -> title -> file select -> player select -> map -> JH (no HUD);
  ares: intro OK.  regress.sh end1: levels unchanged (timing shifts only) -> new base (old: base_fe1).
- Mirror "unset" words (2026-10-20): fe_lend fills FE_VM with 0xFFFF (FE_VM_UNSET); readers fall back
  to the captured VRAM for those words (fe_vword, fe_tiles map reads, fe_rowflag, CAB_CH), fe_vwr
  merges a byte write into 0 instead.  Needed since the early lend: a block written only in part
  (the speech's text rows) showed the tile cache's old bytes in the rest ("ooo" rows round the text).
- FINAL this round: bundle /tmp/claude-0/bundle2 -> scratchpad DKC32X_Builder.zip (delivered);
  Wine .bat run 140 s -> /tmp/claude-0/wfinal.32x boots in PicoDrive to JH.  Test ROMs:
  /tmp/claude-0/fce.32x (-DFULLGAME -DENDTEST, the ending), fgo2.32x (-DGOTEST, GAME OVER).
- Open: K. Rool fight not played through on the 32X (needs input; ENDTEST jumps past it); the
  front-end screens draw ~10-15 pictures/s under big sprites (game speed right); MiSTer untested
  for the new front-end code.

## 2026-10-20 evening: H-series fixes (user playthrough, BUGLOG H1-H18)
- H1 ROM size: build_viewer into_bank = best fit (least growth, then fullest bank) over all banks
  (was next-fit) -> 16 level banks instead of 18; finalize.py pads to whole 512 KB banks only (no
  power-of-2) -> 14848 KB (EverDrive Pro limit 15 MB).  regress.sh h1: pictures unchanged.
  RESIDENT SPACE IS NEARLY FULL: resident end 0x2FF1E0 of 0x300000 (music 340 KB, SNES pages 1998 KB).
- H3/H6 music: the complete game follows the song the game UPLOADS (game_hle.c hle_apu_upload: the
  pointer -> DATA_8AB159 index = game_apu_song, game_apu_song_n counts uploads -> restart);
  $0523 stays 0 (= DK Island Swing) on the logos (SplashScreenFanfare 0A, uploaded by the logo setup)
  and the intro / title (TitleTheme 09).  mkmusic EXTRA_OFF emptied: + TitleTheme, Fanfare,
  CreditsConcerto (+21 KB).  sim3 APULOG=1 logs uploads.
- H2: full-game power-on: load_level(0) with load_noplanes (68000 clears instead of loading JH's
  planes) + an explicit clear.
- H5 (full Rare logo flash): fe_setup flipped both buffers with a plain line table -> showed stale
  pictures (mode 7 leaves odd lines undrawn: the reveal's logo).  fe_setup no longer flips; fe_frame's
  first two pictures paint the sides.  Found with -DPATHRING (main.c: per path / fe_setup / picture
  a ring entry {frames, path, value, vcount}) + PD_SDDUMP frame buffer renders.
- H4/H10 (flicker on palette cycling / fades, no sprites): FE->sdark sticky (a colour once lit stays
  "lit"), so pair decisions stop flipping.
- H7/H8 (intro cross-fade bars, TNT shake): sprites + colour math use "main colour DARK (r+g+b<8) ->
  the sub pixel", main transparent -> always the sub; per-line record FE_LUSED (map_mem+4608,
  non-mode-7 screens) of the main colours a line's math looked at -> only their lines redraw when
  they turn dark (dsig/sdark no longer in obj screens' signatures).  Pictures with <= 120 changed
  lines ignore the time budget (one consistent picture); bigger changes dissolve in a scattered line
  order (j * 67 mod 224).  fe_steady: a BG scroll that jumps back to its value of 2 pictures ago (the
  TNT earthquake, 2 px every frame) keeps its steady value (FE_SCRL map_mem+2336).
- H9: DAE5 = the title screen of the attract loop's 2nd round (not captured before: blank screen) ->
  capture run 6 (power-on, 4400 idle frames).  13 front-end screens.
- Sim tools added: fehost two frame buffers (like the 32X), FEWIN (window regs), FESIG prints TM/TS/
  CGA/INIDISP/COLDATA, WWATCH, MAPWATCH, HOOKLOG, APULOG.

## 2026-10-20 night: H11-H18 + speed + size (see BUGLOG "H-series fixes")
- ROM 13.5 MB (14 level banks): build_viewer places levels best-fit decreasing by tile-set group.
- New front-end screens 47 (empty hoard) and 5C (DK's house): 15 in all.  Cabins run fe_find() too
  (Candy's save-slot screen = F894).
- Narrow camera blocks inside levels no longer get black pillars; pillar changes invalidate the canvas.
- Mine Cart Carnage BG2 (layout + 0x4B00) baked under BG1 (composite bake entries, flag 2).
- FlyingRock / SmallFlyingRock in every world bank (bonus-room exits).
- Speed: obj_behind only per-pixel for 8x8 tiles touching BG1 high-priority cells (was every
  priority-2 sprite in levels with such tiles); late-picture catch-up (3 pictures of 2+ vblanks).
- Test macros (main.c, all #ifdef): GAMEBOT, GAMEBOSS=id (+GAMEBOSSHP=n), CAMRING, DBG_GOTO with
  FULLGAME ENDTEST BOSSTEST.  sim3: /opt/work/xsweep.py (every entrance alias, page violations).
- New game: the island overview waits for START (the SNES does too).
- Bundle /tmp/claude-0/bundle3 -> scratchpad DKC32X_Builder.zip (68 MB); scanbundle clean (the known
  unicodedata.pyd coincidence); Wine (.bat under `script` for a console, DISPLAY=:99) 193 s, ROM
  byte-identical to the Linux convert.py run.
- Open: Coral Capers small fish one-frame wrong tile (not reproduced); K. Rool fight not played to
  the end by a bot (renders; the ending after it verified with ENDTEST); seabed colours (Coral) a bit
  lighter than the SNES's colour math.

## 2026-10-21: I-series (see BUGLOG): stacks, state reset, Misty sprites, front-end holds
- SDRAM map now: .data/.bss up to 0603F000 (mars.ld), master stack 0603F000-F7F0 (canary 16 B at each
  end), slave stack 0603F800-06040000.  __end 0603EF70.  STACKCHK builds paint both stacks (s_main /
  boot) - measure with pd_dbg PD_SDDUMP and scan for 0xA5A5A5A5 (master) / 0x5A5A5A5A (slave).
- game_stats.frames is word 0 of game_stats (not 3: that is dmas).  dbg_presents counts pictures.
- PATHRING builds hung at boot before the stack move (the ring pushed the globals into the master
  stack) - should work again now.
- I9/I10 (2026-10-21 late): Misty Mine STUCK fixed (map_mem last-KB rectangles validated); Torchlight
  beam rebuilt for the game (BUGLOG I10).  map_mem in beam levels: 4096 bm_dmap[128], 4224 bm_lines,
  4672 slot colours, 4704 setup scratch (lower 16 KB: no chunky sprite copy in beam levels), 20480
  BM_ST, 20864 BM_LN[224] (uncached, copied while the slave is idle).  __end 0603EFB0 (80 bytes left).
- Test macros: -DPOISONMAP (junk in map_mem at every screen setup: finds state that leaks between
  screens), -DBOTPRE=x (GAMEBOT: walk to level x, jump, wait - Torchlight: 29071 frees Squawks).
  SNES reference: harness_linux + snes9x_trace, Torchlight beam state saved as tlsnes/beam.st; the
  window table = WRAM 0x11C28 (224 x [1, WH0, WH1]).
- Possible follow-up: darken sprite pixels outside the beam (post-pass over the sprite rects).

## 2026-10-21: I11 (see BUGLOG "I11 fixes")
- map_mem during front-end screens (fescr.c): lower half as before (FE_LSIG 0, FE_MENT 2048, FE_SCRL
  2336, M7MAP 2560 / LUSED 4608, OVF 8192, LGEN 11776, DGEN 12032, LVL 12928, LSIG2 13184, HPREV
  14080, FKEY 14096, FOK 14104, CHST 14848, ROWC 15872); upper half (I11): FE_LSC 16384, FE_LSC2
  17280, FE_LSCA 17728, FE_LSIGA 18624 (-> 20416), FE_DIRTY 22528 [2][128] (after Torchlight's beam
  tables 20480-22272).  sprites.c fe_dirty points at FE_DIRTY from fe_setup until fe_take / fe_lend.
- Front-end Genesis plane: pack fe table = u32 n, n x (u16 nmi, u8 mode, u8 bank, u32 off_vram), then
  n x u32 off_md (plane block in the screen's VRAM bank, md_block layout, plane B only).  68000
  command 0x9E00 | i (md/main.c fe_plane -> load_md, the level loader's plane part).  fescr: FE->gplane
  masks BG2 out of the lines / signatures / frame key, CRAM 0 + sky_ram without the priority bit,
  COMM10 = 0x4000 | 1 << 8 | brightness (0-16), present(BG2 hofs - 32, BG2 vofs).  tools/prebuilt/md.bin
  must be rebuilt with md/main.c (the bundle's prebuilt 68000 binary).
- FE sprites on two CPUs: slave job 2 with sgx 32, sgy 0, split = spr_balance_split(); the token is
  COMM(C_TOKEN) ^ 0x8000 (never one of main.c's own counting tokens).  spr_front_all is read uncached.
- Test tooling: -DFEDBG bars are in the left border now (x 0-31; /tmp/claude-0/bars2.py reads ares
  shots: vblanks / picture, game frames, lines, full, partial, copies, setups, fe_line and signature
  ticks, changed / deferred lines).  sim3 fehost: FESMALL (scroll deferral), FEPART (partial / full
  redraw reasons); /tmp/claude-0/fecmp.sh compares host renders of the ending, credits and boot
  screens between sim_ref and sim_xc (FEROM=rom).  ares profiles of a phase: aprof.sh ROM out 120
  <address of _fe_vm> (only while a front-end screen is lent), per source line with a -g build
  (same ROM bytes) + /tmp/claude-0/pline.py.
- The in-game Cranky cabins are front-end screen 11 too (NMI EEA3: the ending speech's capture);
  Funky's / Candy's use map_frame (static picture + sprites + cab_text).
- Sim3: Cranky's cabin from the map = POKE $3E/$40 = EE over frames 1302-1440 of the power-on run
  (JUMP0=0080BA2C, START every 100 frames).  sim_xc must be rebuilt (mkxc.sh) after every build.sh
  (stale g65 objects -> wrong interpreter path, err 2080E92A).
- I12 bundle: /tmp/claude-0/bundle8 (bundle7 with app/sh, md, tools refreshed; tools/win left out), scanbundle
  clean but for unicodedata.pyd, Wine ROM byte-identical to the Linux convert.py run (lin_i12.32x).
- Bundle /tmp/claude-0/bundle7 (bundle6 with app/sh, md, tools refreshed via mkbundle.copytree, all
  __pycache__ removed) -> scratchpad DKC32X_Builder.zip (66 MB).  scanbundle: only the unicodedata.pyd
  coincidence.  Wine run: cd DKC32X_Builder && wine cmd /c call "Convert (drag ROM here).bat" Z:\...\dkc.sfc
  under `script` (DISPLAY=:99), 211 s, ROM byte-identical to the Linux convert.py run (lin_i11.32x).
  tools/prebuilt/md.bin refreshed (it was still the 2026-10-20 one: missing the I10 / I11 68000 work).
## 2026-10-21: I12 (see BUGLOG "I12 fixes")
- map_mem upper half in Torchlight: BMS span list 17408, BM_ST 20480 (viewer), BM_LN 21376, bm_dmap
  24576, bm_lines 24704, bm_slotcol 25600, BM_TMP 25664 (-> 27840), BMH [CV_H][6] 27840 (-> 30912);
  the lower 16 KB is the sprites' chunky copy (spr_ck) in every game level now.
- Front-end (fescr.c) upper half additions: FE_DUSED [224][8] 23552 (-> 30720), FE_DHC [224] 30720,
  FE_PRC [128] 31616, FE_BRT [256] 32128 (-> 32384).  They overlap Torchlight's / fog tables: front-end
  screens only (no level loaded); a level's tables are rebuilt by load_level.
- Tile cache: tc_fill(p, slot, tag) sets the tag itself; tag bit 15 = Torchlight's lit tile.
- SDRAM: sdram.txt lost FBFD656 (996 B, the last entry) and gained 80DAE5 / 80DC94 / 80DAF6 / 80DCBE
  (title wipe); __end 0x603ED40 (704 B free).  Resident ROM end 0x2FFDC0 (576 B free of 3 MB):
  translating more needs space first (SDRAM-placed functions do not use resident ROM).
- tc_composite (Mine Cart Carnage's composite tiles) is ROM code now (SDRAM was full).
- Tools: ares rebuilt with ARES_SHPROF_EVERY (dump interval; aprof.sh EVERY=); -DFEPHASE=n sets
  ares_phase while front-end screen n is shown (profile of one screen); /tmp/claude-0/bars3.py reads the
  FEDBG bars of the shots32x crop (vblanks / 2, lines / 16, fe_line frames / 2 scales); hot.py /
  pdiff.py (profile windows); /tmp/claude-0/ikiter.sh (IKEYPROF PicoDrive run, interpreted keys);
  sim3 fehost FEMODES=f (per-line modes / layers), FEBG3COV (smoke coverage, test).
## 2026-10-23: I13 (optimization run + the user's I12 round; see BUGLOG "I13 fixes")
- SDRAM layout: the recompiled game code picked for SDRAM (sdram.txt) is its own section .sdgame
  (mars.ld) right after .data's image in the first 256 KB; the BIOS copies only .data, m_main copies
  .sdgame first thing (uncached, read back and compared, up to 4 rounds; sdgame_fixes counts words
  that needed a second copy), then purges the cache.  sdram.txt re-picked (sdpick3, budget 67500 +
  the title wipe's 4): ~83 % of the code line fills.  First 256 KB: ~3.9 KB left (finalize.py checks).
- Master stack 4 KB (0603E800-F7F0, mars.ld RAM ends at 0603E800): STACKCHK measured 1916 bytes
  (front-end screens with sprites: m_main 736 + fe_frame 604 + spr_draw_band 496 + ...) - the
  old 2 KB stack had 100 bytes left.  Slave 1140 of 2048.  __end 0603BD60.
- Logic speed-ups (all WRAM byte-exact: sim3 simva vs simo = the I12 sources' sim, 44 entrances x
  3000 frames x 2 inputs, and vs the interpreter except the known rope / stack-residue bytes):
  * r65rt.h R65_SLIM (default with R65_OOL): the inline RD8/16_SYS_I keep only the WRAM path, the
    rest calls o_rd*_sys (SDRAM game code 5.5 % smaller);
  * r65mem.h ROMB: page pointer as (o >> 8) & 0x3FFC (3 SH-2 instructions instead of 6) - SH only,
    the host keeps the plain index (8-byte pointers);
  * r65.py: JMP (abs) / JML [abs] through a WRAM vector read it directly (was an mrd16 call);
  * xcall: 512-entry key+function cache (one line per lookup), multiplicative hash (the xor fold
    collided ~8 % of the time -> binary search);
  * game_hle.c: hle_spr_sort (CODE_B88344, the sprite draw-order passes), pose_src (pose data read
    in place inside a 1 KB page instead of copied).
- ares MiSTer model (bench.sh, GAMEPROF HUD G/T % of a frame) I12 -> I13: JH 73/129 -> 70/121,
  Reptile 69/111 -> 65/105, Coral 65/144 -> 61/139, Snow 78/123 -> 74/115, Forest Frenzy 54/95.  (GAMEPROF ROMs draw two
  HUD lines: ~5 % of T.)  The user build runs 2 logic frames per picture when 1 + render > 94 %.
- Tools: ares (rebuilt) ARES_PADSCRIPT=file (harness-style "run N BUTTONS", Genesis names, from
  power-on), ARES_DUMP=dir (+_FROM/_TO/_EVERY/_HALF: frames as PPM), ARES_QUIT_AT=frame;
  /tmp/claude-0/ares_run.sh ROM PAD DIR FROM TO EVERY QUIT [mister]; tr/cuts.py (scene cuts),
  tr/ana.py (cuts / one-frame flashes of a dump, keeps frames around them), tr/blk.py (sheets of every
  black -> picture transition); arun2.sh (bench capture that waits for the HUD).  GAMEBOT builds now
  press Start / walk right outside levels too (full game from power-on: menus, maps, deaths, game over).
- I13 bundle: /tmp/claude-0/bundle9 (bundle8 + app/sh, md, tools via mkbundle.copytree, no tools/win /
  __pycache__) -> scratchpad DKC32X_Builder.zip.  scanbundle: only the unicodedata.pyd coincidence.
  Wine (.bat under `script`) 151 s, ROM byte-identical to the Linux convert.py run (lin_i13f.32x =
  u13f.32x, the ROM tested in PicoDrive / ares).
## 2026-10-24: I14 (see BUGLOG "I14 fixes"; HANDBOOK.md = the consolidated hand-off file)
- HANDBOOK.md: goals / legal rules / architecture / memory maps / tools / pitfalls / open list / the
  60 fps trade-offs, in one file.  devkit/: copies of the dev scripts that lived in /tmp/claude-0 and
  /opt/work (sim3 sources, gp scripts, verify / bench / ares scripts, frame analysis) + the ares and
  snes9x_trace patches (not part of the bundle).
- map_mem (Torchlight): bm_shown [224][2] at 30912 (-> 31808) = the beam edges actually drawn (slide
  toward the game's bm_lines by BM_STEP = 48 px a picture; -DNO_BMRATE = old behaviour).
- MiSTer cost model of the interpreter: ~600 cycles per interpreted 65816 instruction (ROM-resident
  interpreter, cache misses) - even 50 instructions a frame are ~8 % of the master.  Keep translating
  hot keys; check with IKEYPROF (PicoDrive SDRAM dumps every N frames: PD_SDFRAMES=N) or sim3
  sim_xcp (XC_PROF).  Free-movement tests ($0535) run debug-only code (80A1C0, BCB38E, BFC63E):
  don't translate those.
- Resident ROM is the limit for translations: 0x2FFAD0 of 0x300000 now.
- Test aids: -DPROFT -DPICPROF = this picture's phases in tenths of a frame (+ full-redraw flag and
  beam span lines); -DDBG_TP=start,every,dx[,end]; -DRIP_DEFAULT=0/1; harness (/opt/harness) got a
  byte `poke ADDR VAL` (pokew writes a word: $0536 got clobbered).  SNES reference of a level's end:
  harness + plain snes9x, wmap.st, entrance poke, `poke 0x535 1` + `pokew 0xB1B X` steps
  (/tmp/claude-0/ropesnes/s.txt).  Ropey's level-start snapshot is at x 6025 (not 0).
- Ropey rain section (MiSTer model): F20, logic ~65-75 %, master render ~97 % (rain phase waits for
  the slave + rain_half); the slave is ~45 % idle.  Next lever: the whole rain on the slave.
- I14 bundle: /tmp/claude-0/bundle10 (bundle9 + app/sh, md, tools via mkbundle.copytree, no tools/win /
  __pycache__, + app/docs = HANDBOOK / BUGLOG / NOTES) -> scratchpad DKC32X_Builder.zip (66 MB).
  scanbundle: only the unicodedata.pyd coincidence.  Wine .bat run: ROM byte-identical to the Linux
  convert.py run (lin14.32x, md5 48013ce1...).  Checks: verify2 20 entrances byte-exact; ares GAMEBOT
  9000 frames (MiSTer model): no one-frame flashes, clean black -> picture cuts; ARES_MISALIGN: none;
  STACKCHK master 1936 / 4064, slave 1140 / 2048 (Torchlight turning: 1340).
## 2026-10-24: I15 - widescreen spawn windows removed (BUGLOG "I15")
- rompatch.py: _ws_logic (spawn / keep-alive / enemy-on-screen windows, only with WS_LOGIC=1 for tests)
  vs _ws_display (OAM builder culls, items, particle wrap: always).  The game logic is the SNES's again.
- Tools: ares ARES_ANYVER=1 (load other-version states), --save-state N (the .bsN next to the ROM);
  user state -> WRAM: SDRAM at state offset 0x123B (swap 16-bit), wram_hp from the ROM at the ELF's
  _wram_hp; injection into a snes9x state (/tmp/claude-0/bcc/user2.st recipe: WRAM block at 0x10B97
  of a harness `state`, $1C = 80AB); /tmp/claude-0/bcc/ab2.sh ENT (snes9x A/B of ROM patches from a
  shared in-level state, PATCHED=rom); /tmp/claude-0/sweep15/run.py (all levels, TPGRID page sweep).
- I15 bundle: /tmp/claude-0/bundle11 (bundle10 + rompatch.py + docs) -> scratchpad DKC32X_Builder.zip;
  scanbundle: only unicodedata.pyd; Wine .bat ROM byte-identical to the Linux convert.py run (lin15.32x).

## 2026-10-24: I16 - centred view, Misty dither, interpreter -> native (BUGLOG "I16", "I16 optimization run")
- Coverage tools (copies in devkit/cov): /tmp/claude-0/cov/run.py SIM OUTDIR (309 jobs from jobs.py,
  FCFILE / XCFILE / PAGELOG per job), agg.py DIR OUT.pkl (exclusive interpreted counts per key: xg
  gameplay, xa all, runs), reg.py REF NEW OUT (per-frame WRAM hash of every job on two sims: "ok N"),
  badjump.py [g65dir] (translated functions with BADJUMP: only the 3 old 80E2D6 ones), dis.py PC MODE N
  (65816 disassembly with the rompatch'ed ROM).  sim3 scripts: mkfc.sh (sim_fc = XC_PROF + FCOUNT_PROF),
  mkfcb.sh (same from /tmp/claude-0/g65_base with -DR65_NO_JMLHANDOFF = the I16 start), mkfi3.sh
  (FORCEI profile: every key interpreted, exclusive counts - 4 h for all jobs), mkref.sh G65DIR OUT.
- Selection: probe build with all candidates in translate.txt (finalize fails over 3 MB, but build/sh.elf
  has every function size: copy it), then greedy by instructions per byte (+16 B dispatch entry) into
  the free resident ROM.  Keys with no owner in build/g65/owner.txt are not analyzer entries: add them
  to data/entries.txt first.  Keys whose owner is another function are resume points (skip).
- Translate list history: /tmp/claude-0/translate_i16_base.txt (start), _b1, _b2, _b3a (JML targets),
  _b3b, _b4 = translate_final.txt.  Sims: simbase, simb1, simb2, simb3a, simb3b, simva (final).
- Remaining interpreted (gameplay): 80BA2C front-end menu 0.42 M, 808159 resume 62 K, then keys < 20 K
  each; candidates + sizes in /tmp/claude-0/cov/probe4.pkl / ps4.pkl (needs ~42 KB).
- More resident room later: WORLD_RESIDENT pages (197 KB, read from every world: token rooms) could
  also be DUP pages if the world banks had room (world 5 is full); per-level window-6 banks would lift
  that limit.
- pkill -f PATTERN kills the calling shell when its command line contains PATTERN (it did, twice):
  use pgrep -f "PATTERN " / kill by pid.
- I16 bundle: /tmp/claude-0/bundle12 (bundle11 + app/sh, md, tools via mkbundle.copytree, docs) ->
  scratchpad DKC32X_Builder.zip.  scanbundle: only unicodedata.pyd.  Linux convert.py run from the
  bundle sources (/tmp/claude-0/linapp16, no toolchain dir) = lin16.32x md5 3668e929... = the plain
  build.py ROM; Wine .bat run (wtest16): byte-identical.

## 2026-10-25: SH-2 cache study (after I16) - findings, nothing changed in the build
- Logic-phase profile, JH demo, MiSTer model (ares ARES_SDFILL=30 ARES_UNCSD=11, ForceInterpreter,
  ARES_PHASE_ADDR = _ares_phase; /tmp/claude-0/dup/plg.txt with -g ELF /tmp/claude-0/bench/pg_0.elf,
  /opt/work/allb_g.sh = allb.sh with GAMEOPT -g): ~2400 frames, 528 M cycles.
  gen code 53 % (54 M SH-2 insns, 4.13 M code fills from SDRAM + 0.56 M from ROM, 1.69 M SDRAM data
  fills): at 30 / 60 cycles a fill ~80 % of the generated code's time is line fills - one code fill
  per ~13 instructions: the per-sprite code STREAMS through the 4 KB cache (no reuse inside a frame).
  helpers 11 % (o_rd16_sys 4.5, mrd16 3.7, o_wr16_sys 1.8, xcall 1.5, r65_call2/3 2.5, rts/rtl 1.6);
  other 36 % (spr_prepare 7.0 - ~15 tile conversions + list per frame, hle_oam_build 3.9,
  oam_pose 3.0, io_wr 2.6, m_main 2.5 (ROM, c/i 18.8), hle_spr_sort 1.1, vram_dma 1.1).
- By 65816 instruction (/tmp/claude-0/genkind.py, outermost -g line): LDA 25 %, calls / returns
  (entry + JSR + JSL + RTS + RTL + JMP) ~29 %, STA / ADC / SBC / CMP ~5 % each.  Static SDRAM code:
  LDA 18.7 %, STA 8.8, RTS 6.7 (flag write-back + r65_rts), ADC 6, JSR 5.6 (/tmp/claude-0/genbytes.py).
  Literal-pool address loads: r65_db 312, r65_n 275, r65_v 168, o_rd16_sys 198, o_wr16_sys 134 sites.
- Known SH-2 tricks vs this port: hot code in SDRAM (done, .sdgame 65 KB), cache-through for shared
  data (done), per-line / whole purges (done), 2 KB cache RAM (two-way mode) used by the comp renderer;
  for the logic it would capture <= 12-18 % of code fills while halving the cache (net loss expected).
  -falign-* = 0.03 % of the code (the nops are delay slots).  A source-keyed memo for tile conversions:
  <1 % of a frame (only ~15 conversions a frame; sim3 -DCKSTAT counts them, hit rate 30-60 %).
- Remaining levers (estimates): CPU-state block addressed by GBR (n / v / db + helper table before
  wram_lo; ~5 % smaller SDRAM code -> ~1-2 % of the logic; every GBR path is untested by the host sim:
  R65_NO_GBR); spr_prepare on the slave in parallel with the BG prep (~4 % of a master frame); the big
  one stays the 60FPS MODE pipeline (HANDBOOK 9) - input-lag trade-off = the user's call.
## 2026-10-25: I17 - 60FPS MODE pipeline, pad queue, FLIP_ASYNC (BUGLOG "I17")
- devkit/perf/perf.py ROM ELF OUT.json level:pipe:flags ... (PERFREC -DNOHUD build; flags 1 no frame
  skip, 2 60FPS MODE forced, 4 fused job in the normal mode, 8 no async swap): MiSTer-model ares
  (ForceInterpreter, ARES_SDFILL=30 ARES_UNCSD=11) to ARES_QUIT_AT 2400, SDRAM dumped (ares patch
  ARES_SDDUMP), perf_rec decoded: fps, game speed, picture-interval histogram, master phases per
  picture, slave idle, pad presses (PADTEST), async pictures, lost vblanks by reason.
  Several ares at once need `--setting Input/Defocus=Allow` (an unfocused ares pauses).
- devkit/perf/pic_eq.py ROM ELF LEVEL pipe:flags pipe:flags [F0 F1] (MISTER=1 for the MiSTer model):
  distinct-picture sequences of two configs.  ares dumps miss single frames now and then: align with
  difflib (scratchpad peq2.py / peq3.py Y0 = only rows >= Y0; JH / BCC haze rows are timing-dependent
  68000 output) and look at replace blocks only.  Row 237 (border) differs by one pixel between runs.
- PADTEST: -DPADTEST in EXTRA_MDFLAGS (68000 sends B for one vblank in 32) + PERFREC; -DPAD_OLD on the
  SH side = the I16 behaviour (register read per logic frame).
- Results (MiSTer model): tables in BUGLOG I17 / HANDBOOK 7.  Old baseline sweep (I17 pipeline fix,
  no async): /tmp/claude-0/perf/sweep2.log; async: asy4.log (normal), asy5.log (60FPS MODE).
- Builds for tests: DKC_SURVEY=work/survey python3 tools/build.py work/rom.sfc OUT --snaps work/snaps
  --extra-shflags="..." (md rebuilt unless DKC_PREBUILT_MD; refresh tools/prebuilt/md.bin from a plain
  build after 68000 changes - the bundle's Wine build uses it).
- Checks (I17 final sources): host sim verify2 standard set 3000 frames vs the I16 sim (simva_i16):
  32/32 byte-exact.  ares MiSTer-model GAMEBOT 9000 frames, normal mode and 60FPS MODE default
  (tr/botrun.sh bot17 / bot17p): one weak "flash" each (score 7: a 1 px camera step sampled by frame
  skip, no stale picture).  ares fast GAMEBOT + NO_FSKIP, sync (FLIP_ASYNC=0) vs async, 8800 frames
  (scratchpad peq4.py / fl.py): same pictures below the haze rows except the timing-dependent
  front-end animations; the flash-like events identical in both.  ARES_MISALIGN 8000 frames: none
  (the "illegal slot 0xaffe" line is in every run since I14).  PicoDrive GAMEBOT 12000 frames +
  STACKCHK: master 1948 / 4064, slave 1368 / 2048.  PicoDrive reset (/tmp/claude-0/hr/harness_r =
  harness + "reset" command): restart to the Rareware logo, game again (I16: STUCK).
- I17 bundle: /tmp/claude-0/bundle13 (bundle12 + app/sh, md, tools via mkbundle.copytree, docs) ->
  scratchpad DKC32X_Builder.zip.  scanbundle: only unicodedata.pyd.  Linux convert.py run from the
  bundle sources (/tmp/claude-0/linapp17) = lin17.32x md5 9bd3c21c... = the plain build.py ROM; Wine
  .bat run (wtest17, 204 s): byte-identical.
## 2026-10-25: I18 - lamp S/H, water camera, 60FPS MODE tile race, millstone, v1.1 / v1.2, GitHub
- Lamp levels: details BUGLOG B-LAMPDITHER.  Offline checks without a ROM build (0.4 s a level):
  scratchpad wkren.py (build_level(D9), plane B rendered as the Genesis shows it, SH=1: per-cell
  shadow) / wksnes.py (BG2 in the SNES colours) / wkq.py (quantizer comparisons on the SNES render).
  build_viewer: genesis_planes(normal=True) = lamp mode: pi['bright'] (tiles with a channel >
  LAMP_BRIGHT5 = 17), tsh(pi, k) per-tile shadow, tdither(pi, k).  md: S/H on in lamp levels, lc_ph.
- Water camera: BUGLOG B-WATERCAM.  CAMLOG build (cam_log[128][4] ring) + scratchpad camlog.py.
- Pause menu ripple: write_line_table_cv / spr_rip_pat flat while pm_open (both modes checked:
  DBG_PAD=500,506,0x1000 in Coral Capers, 250 frames identical).
- Test inputs found this round: DBG_GOTO=f,0xD2 + DBG_POKER=f,f+2,0x1E15,0x402 = the real Rambi
  bonus (the animal bonuses need $1E15 = DATA_81D102[entrance] = $0402; the sim: GOTO=200:D2
  POKE=200:1E15:402,201:1E15:402).  Millstone: -DMSTEST (fixed position), -DMSNODRAW.
  Host-sim page checks: WORLDBANK=k (pages outside world k's bank), DMALOG (OBJ upload sources) vs
  build/game/worlds.json - all 40 main levels: only the ShadedDebugFont page (never visible).
- Millstone profile (ares interpreter, ARES_SHPROF, MiSTer model, MSTEST no frame skip): master
  tile8 23 % -> 19 %, waits for the slave 17 %, present2 8 %, draw_tile_row 6 %, spr_ms_cover 4 %;
  slave idle 41 %, music ~25 %.  Ideas left: canvas restore under the picture (~1/3 of its cost),
  pre-baked 8bpp rows (needs ~18 KB SDRAM that is not there).
- US v1.1 / v1.2 (the user's dumps, headerless 4 MB): v1.1 differs from v1.0 in 7441 bytes - code
  moved within banks 80, 81, B5, B6, B8, B9, BC-BF, data banks unchanged; v1.2 differs in ~567 K
  bytes across most banks (code and data shifted).  The tool hard-codes v1.0 addresses in many places:
  the recompiler's entry / translate / hot / sdram / pcprof lists, ~200 HLE address literals
  (sh/game_hle.c), rompatch.py patch sites, build_viewer's ROM tables (DATA_* reads), level / sprite
  list addresses, capture.py's breakpoints, the page profiles (memprof*, dma_w*), and the disassembly
  metadata (layout.json, v1.0 labels).  Support = an address map per revision (align the code by
  matching instruction streams, data by content) applied to every list + re-validation of all
  levels (host sim vs snes9x with that revision).  A delta patch to v1.0 is not an option (it would
  ship game bytes).  Status: v1.0 only; romcheck names the revision it finds and stops.
- Genesis-only DKC (user asked about a "2 days ago" report): nothing found that is a real port.  What
  exists: a Genesis tech demo by Pyron / TiagoSC (three-layer DKC-style scene, Oct 2025, Time
  Extension article), the bootleg "Super Donkey Kong 99" (Genesis, original game), DKC1Recomp
  (elliotttate, PC static recompilation of v1.0; widescreen via the game's own object activation /
  culling ranges - the same approach as I16's spawn window).  Nothing to borrow for the 32X.
- GitHub: repository prepared at /tmp/claude-0/gh/DKC32X (sync script /tmp/claude-0/gh/sync.sh:
  mirrors sh/ md/ tools/ devkit/ + docs, scanbundle before every commit; PolyForm Noncommercial 1.0.0
  + additional terms, THIRD_PARTY_NOTICES.md).  The session cannot push (no GitHub account linked to
  it; github.com is reachable for git only through the repository proxy).
- Checks (I18 final sources, plain build md5 c909431a...): host sim verify2 16 entrances x pad/tp
  3000 frames vs simva_i16: 32/32 byte-exact.  ares MiSTer-model GAMEBOT 9000 frames, normal and
  60FPS MODE default (bot18 / bot18p): one weak flash each (score 7, the JH camera step known from
  I17).  (devkit/tr/botrun.sh now passes Input/Defocus=Allow: a second ares paused the first.)
  ARES_MISALIGN 8000 frames: none (the old 0xaffe line).  PicoDrive GAMEBOT 12000 frames +
  STACKCHK: master 1964 / 4064, slave 1364 / 2048.  PicoDrive reset: logo -> game again.
- I18 bundle: /tmp/claude-0/bundle14 (bundle13 + app/sh, md, tools via mkbundle.copytree, docs,
  README.txt 14 MB / versions) -> scratchpad DKC32X_Builder.zip.  scanbundle: only unicodedata.pyd.
  Linux convert.py from the bundle sources (/tmp/claude-0/linapp18, 114 s) = the plain build.py ROM
  (md5 c909431a...); Wine .bat run (wtest18, 138 s): byte-identical.
- I18b (MiSTer STUCK at JH bonus 1): BUGLOG "I18b".  Tools: scratchpad-free host check of SRAM writes
  (copy of sh/game.c with a printf at the SRAM store, sim BOT=0): the game toggles $6057 during play.
  MiSTer core source /opt/ref/s32x_mister rtl/CART/cart.sv: SRAM_EN = (SRAM_BANK || ...) && A21 &&
  !CE0 - with 0xA130F1 = 1 the whole cart range 0x200000-0x3FFFFF is SRAM for every bus master.
  Test flags: -DSRAMPOKE=f (flip a save byte at game frame f), -DDBG_HANG (watchdog page at 400).
- I18b checks: plain build md5 1c4d611f...; Linux convert.py from bundle15 = Wine .bat run = that ROM;
  MiSTer-model GAMEBOT 9000 frames (bot18b): no flash events; PicoDrive GAMEBOT 12000 frames fine;
  DBG_HANG page shows the live 68000 column (VBL counting).
- I18c music on MiSTer (user: "victory fanfare garbled"): scratchpad musstat.py (MiSTer-model ares,
  SDRAM dump: music_halfs / music_skips / music_maxdt, -DMUSHIST refill-time histograms, -DMUSRESET=f
  counts from game frame f).  25 s of gameplay: every refill runs at half rate (the adaptive
  music_light: levels below 57 fps), typical refill 25-50 % of a 5.8 ms buffer, a tail to 1.6x
  (9-13 underruns = short silent buffers, 50-100 "slow refill" events).  The I17 sources measure the
  same (not a regression).  The music's CPU cost is the limit on MiSTer; open (HANDBOOK 8).
- I18c checks: plain build md5 457a361e...; see BUGLOG I18c for the SRAM tests.
  I18c: Linux convert.py from bundle16 = Wine .bat = 457a361e...; MiSTer-model GAMEBOT 9000 frames (bot18c): no flash events.
- I18d, 32X Hardware Manual (user's PDF, text in /tmp/claude-0/hwman/man.txt): timing table 4.4 -
  cart ROM 6-15 waits a word for the SH-2 (a 16-byte line fill ~64-136 cycles), frame buffer write 1
  wait min with a 4-word FIFO (5 clocks when full), FB read 5-12 waits, palette 1 line wait when the
  VDP is busy, VDP registers 5 waits, system registers 1, SDRAM line fill 12 clocks / 8 words (real
  hardware; the MiSTer core is slower: the model uses 30), writes 2 clocks a word.  Auto Fill (VDP,
  up to 256 words, 7 + 3 x length cycles, no SH-2 FB access meanwhile) is not used by the port.
  Applied: mapper writes with RV = 1 (BUGLOG I18d).
  I18d: plain build md5 817e7458...; MiSTer-model GAMEBOT 9000 frames (bot18d): no flash events.
## 2026-10-29: I19 - late / ripple pictures async, interpreted level-load code, plane dither (BUGLOG "I19")
- Perf builds (as I17): DKC_SURVEY=work/survey python3 tools/build.py /opt/work/dkc.sfc OUT --snaps
  work/snaps --extra-shflags "-DPERFREC -DNOHUD" (work/snaps + work/survey = the builder's own
  captures: 15 front-end screens; the older /tmp/claude-0/e2e2 set has only 8).  Baseline I18d:
  /tmp/claude-0/i18/base19.txt; I19 runs p19a.txt (late + ripple async).
- -DSYNCWHY: sync_why[16] counts per present2 reason (0 eligible, 1 sky_next, 2 ripple, 3 sft_show,
  4 disp_pending, 5 pending_pal, 6 spr_pal_dirty, 7 bm_cram, 8 comp_pal, 9 wx_dirty, 10 lamp,
  11 glow, 12 VBLK at entry, 13 line >= 208, 14 not a gameplay picture); read with scratchpad
  symdump.py ROM ELF FRAMES sync_why:16 levels... (two frame counts subtracted = a window).
  After I19, JH frames 1200-2000: palettes ~1 %, 208-221 window ~6 % (that sync wait is short).
- -DIKEYPROF survey (scratchpad symdump.py ... ikp_key:48,ikp_n:48,ikp_c:48 + ikread.py): gameplay
  interpreted code is small everywhere except Stop & Go Station (81:9B78 per frame); the big items
  were level-load lighting / palette table builders (fblank frames - the I16 coverage counted only
  frames with a picture, xg, so they were never picked): 81:9A07 / 81:9A72 / 81:9AF2, B9:921D,
  B9:9100 / B9:8AF4, B9:8597 (81:98D8), 80:ADA2, B9:92B2, B9:9362, B9:87B7 (81:98B9), 80:AC0B,
  81:97DD, B8:A7E7 - 10 K - 60 K interpreted instructions each (~600 MiSTer cycles apiece).
- Room: the first 256 KB of the ROM (SH-2 code + .data image) has had ~58 KB free since I17 moved
  the SDRAM code image into world bank 0.  New r65.py --lowrom data/lowrom.txt: those functions get
  LOWROM_FN = section .lowtext (-Os), linked into .text (sh/mars.ld): 59 functions, 34 KB; the
  first 256 KB now has ~39 KB free (build/sh.bin 222 KB).  The resident game region (pack .. 3 MB)
  only grows by the 16-byte dispatch entries: 0x2FFE70, 400 bytes free.  The SPC engine
  (build/spceng_gen.c) at -Os: 28 -> 13 KB (MUSHIST refill histograms unchanged).
- Regression for translations: scratchpad-free host sims from two generated-code dirs (r65.py with
  the old / new translate list -> /opt/work/sim3/mkref.sh G65DIR NAME), devkit/cov/reg.py REF NEW OUT
  (309 coverage jobs, per-frame WRAM hashes).
- I19 checks (final sources): plain build md5 32359c09...; Linux convert.py from bundle18
  (/tmp/claude-0/linapp19, 222 s) and the Wine .bat run (wtest19, 272 s): byte-identical to it.
  scanbundle: only unicodedata.pyd.  Host reg.py: 309/309 jobs WRAM-identical to the I18d
  translation set.  MiSTer-model GAMEBOT 9000 frames, normal (bot19) and 60FPS MODE (bot19p): one
  weak "flash" each at 7109 (score 7) = the whole picture 1 px left for one frame and back (all
  layers alike: the game camera, as the I17 runs; not a stale or mismatched picture).  PicoDrive
  GAMEBOT 12000 frames + STACKCHK (scratchpad pdstk.sh): master 1960 / 4080, slave 1364 / 2048.
  PicoDrive reset at 3000: Cranky's intro again.  ARES_MISALIGN 8000 frames: only the old 0xaffe line.
  Water levels: async pictures vs FLIP_ASYNC off (pic_eq 22 0:1 / 0:9) differ only where the runs'
  game states diverge (pad timing) - the ripple bands line up in both.
## 2026-10-29: I20 - per-cell shadow on the Genesis planes, checkerboard dither, Blackout lights (BUGLOG "I20")
- build_viewer.genesis_planes(sh_cells=True) from the ordinary level path and the LIGHTS path (not
  the beam): cell_err(pi, k, sh) = weighted error of the cell's pixels at the ideal normal / shadow
  levels; bright = cells where normal wins.  allocate() fallbacks: pairs off -> up to 24 colour merges
  (canon) -> the old shadow flags.  Env: PLANE_SH=0 (off), PLANE_DITHER=0, PD_MIN_ERR (1500), PD_GAIN.
- dpair(pi, base, v, sh): per channel the bracketing levels (lv8 = shadow / normal table); a channel is
  split when the midpoint is closer than the nearest level; pixel = high colour on ((i >> 3) ^ i) & 1.
- GPSTAT=1: per plane maxc / shadow / cells / dark cells and GPMIS (misfits, colours) per level.
  Before I20 most planes used 5-35 of the 60 Genesis colours (9-bit colour collapses DKC's palettes);
  per-cell shadow raises that (Coral 9 -> 18, the factory 5 -> 13, Tree Top Town 33 -> 41).
- Offline preview: scratchpad pdprev.py ENT OUT.png (SNES colours | Genesis planes as shown, per-cell
  shadow from the map priority bit) - PLANE_SH / PLANE_DITHER / PD_* env apply.
- SNES reference runs: the harness 'load' / 'state' paths are relative to the output directory (an
  absolute path segfaulted it): copy work/wmap.st into the run folder; pokew 0x40 / 0x3E = entrance.
  Memory ids: 2 WRAM, 4096 CGRAM, 4097 PPU register fill (last written values; $2132 = COLDATA).
- Blackout: snes9x frames every 10 (scratchpad-free: /tmp/claude-0/i20/bb), lights off ~60 frames at
  a time; WRAM has no clean flag for it, the register write is the signal (game.c game_coldata).
- I20 checks: plain build md5 297be58b...; Linux convert.py from bundle19 (linapp20, 176 s) and the
  Wine .bat run (wtest20, 236 s): byte-identical.  40-level ares shot sweep I19 vs I20 at frame 700
  (/tmp/claude-0/i20/sw sheet_*.png): only the Genesis planes' colours changed, no broken planes.
  MiSTer-model GAMEBOT 9000 frames (bot20): one weak flash (5970: the whole picture 1 px for one
  frame, all layers alike - the game camera, as in I17 / I19).  PicoDrive GAMEBOT 12000 + STACKCHK:
  master 1960 / 4080, slave 1364 / 2048.  ARES_MISALIGN 8000 frames: only the 0xaffe line.
## 2026-10-30: I21 - Galleon island, water camera, Ropey trees, OBJECT RANGE, menu font, Rare logo (BUGLOG "I21")
- Galleon: C_FRAME = picture counter | view offset d << 9 (sh/main.c cframe(); lamp levels keep the
  game frame there, lc_on on the 68000 side); md update_scroll(camx, camy, d) works out every plane's
  H scroll for the game camera (camx + d) and subtracts d.  Galleon's BG2 band 60-99 (zero H scroll
  in the SNES HDMA table) is now d instead of 0.
- Water camera: camlog (the user's .bs1 + the I20 ROM: F, camx, $1B23/$1B25, hle_cam_xmin/xmax,
  hle_cam_frame, spr_xoff every other frame) replayed in Python with the old / new view-offset rules:
  6 jumps > 4 px -> none.  (A new ROM cannot run the user's state: the SDRAM code image differs.)
- OBJECT RANGE: rompatch.WS_TUNE {pc: kind}; r65.py emits ((imm + r65_ws[kind]) & 0xFFFF) for those
  16-bit immediates and appends r65_ws_tab (pc | kind << 24) to game65_00.c; r65interp.c adds the same
  for interpreted code (banks B6 / BD / BE, only while r65_ws is non-zero).  main.c sets r65_ws before
  each picture's logic steps: L = clamp(spr_xoff, 0, 64), width 64, R = 64 - L (in levels, WIDE).
  The bank BD windows with $80 / $200 and $C0 / $280 (big objects) already reach 96+ px past the
  32X view: unchanged.  Vertical windows unchanged (same picture height).
- Ropey storm colour math: snes9x CGRAM + picture at the level start - 96 % of the treetop pixels
  are storm-palette colours as they are, 0 % match (c - 4) / 2.  The jungle day levels (same CGADSUB
  $C2) were not changed.
- Rare logo: SNES timeline (snes9x, frames from power-on): 74 screen on, 76-186 two strokes lit one
  entry a frame each (CGRAM 32-143 and 144-255: white, $7E10, then green $01C0), 196 the logo's BG2
  colours come on (flash), 228-282 the wires go off 4 entries a frame, ~300 mode 7 zoom away, 348
  the Nintendo logo.  The port follows the game's palette (lit entries, logo colours on) and its
  frame counter from the fill on.  Data block 15 KB in the BC7B_3 screen's bank (the resident ROM
  had no room: 3 MB limit).
- ares 32X: byte writes of 0 to the frame buffer are dropped (the mode 7 zoom's erase); sync
  present() = 30 pictures a second at best (waits for the vblank after the next).
- fe timing: per-iteration ring (vcount, game frames, map_mode) showed one iteration of 23 vblanks
  at the Nintendo logo: fe_setup 6 + the first hi-res picture 16 (fe debt now carries the frames the
  12-step cap dropped).
- Resident ROM: with the builder's 15 front-end screens the resident region ended 96 bytes over the
  3 MB limit (the OBJECT RANGE code) - r65_ws_tab moved to .lowtext and four front-end / start-up
  functions (80:D199, 80:CCB6, 80:C093, 80:F31F) added to data/lowrom.txt: resident end 0x2FCBB0
  (13 KB free), sh.bin 239 KB of 256 KB.
- Optimization survey: IKEYPROF (MiSTer model, 1500 frames) - gameplay interpreted code is down to
  < 300 instructions per level (levels 0-5 checked; the I19 work).  Ropey (MiSTer model, F21-27):
  the master waits ~12 % for the slave (C_DONE) and ~9 % in present2's vblank wait; SYNCWHY: 55 of 307
  pictures synchronous for sky_next / wx_dirty (the storm's palette changes); rain_half 10 % (an FB
  read per rain pixel for its restore list).  Not changed this round.
- I21 checks: plain build md5 95537cce...; Linux convert.py from bundle21 (linapp21, 136 s) and the
  Wine .bat run (wtest21, 173 s): byte-identical.  Host reg.py: 309/309 jobs WRAM-identical to the
  I19 translation set (r65_ws = 0).  40-level ares sweep I20 vs I21 at frame 700: only parallax
  offsets moved.  MiSTer-model GAMEBOT 9000 frames (bot21): no flash events.  PicoDrive GAMEBOT 12000
  + STACKCHK: master 1964 / 4080, slave 1364 / 2048; PicoDrive reset at 3000: the Rare logo again.
  ARES_MISALIGN 8000 frames: none.  scanbundle: only unicodedata.pyd.
- I21b: d in C_FRAME only for entrance 68 (cf_view, set in load_level); md uses it only for fx 6, the
  frame counter unmasked elsewhere (as I20).  The 68000 scroll code with dd = 0 is the I20 code line
  for line (diff against bundle19).  Checks: plain build md5 44307aa1...; linapp21 / wtest21
  byte-identical; Galleon boss shot (island on its shadow); PicoDrive GAMEBOT 12000 + STACKCHK
  1964 / 1364.
- I21c: Galleon island OAM (boss start): tiles 140-148, attr 2D (priority 2, palette 6, N); K. Rool
  palette 7, the Kongs 5.  Ropey lift previews: STORM_LIFT env (1.0 / 1.25 / 1.5 / 1.75) with scratchpad
  pdprev.py; 1.5 chosen.  The "purple" Ropey frames seen while testing were lightning flashes (random
  timing), not a palette bug.  Research survey 05 redone (old one in /opt/work/survey_05_old); levels.json
  backup /tmp/claude-0/levels_pre_i21c.json (only entry 105 differs).
- C_PAD (68000 -> SH-2): bits 0-7 pad, 8-13 vblank sample number (mod 64), 14 = first bit of the 68000
  load, 15 = data bit.  ARES checks: plain md5 ac5e9dfe...; linapp21 (fresh captures) / wtest21
  byte-identical; MiSTer-model GAMEBOT 9000 (bot21c): no flash events; PicoDrive GAMEBOT 12000 +
  STACKCHK 1960 / 1364.
- I21d: the stump colours were identified by matching the screenshot's colours (5-bit) against every
  15-colour window in bank BC: best = DATA_BC89C2 (Sprite_BlueKrusha) for both the stump and the
  bananas.  Manic Mincers slot table at its start: 86F2 (bananas) / 849A (DK) / 8440 (Diddy) / 8A1C /
  881E (tire = mincers) / 872E (barrel); the stump = DATA_BC88D2 (Sprite_CavePlatform, CODE_B5854C).
  Sim (devkit copy of /opt/work/sim3/sim.c) gained env tests: WS=L,W (OBJECT RANGE windows), PALOVF
  (slot high-water mark + $1E3D overflow marker), PALCHK (port colours vs the slot's ROM palette),
  PALREUSE (a live object's slot reassigned - false positives from sprite 44, the off-screen spawner
  placeholder that keeps a stale attribute), SLOTLOG.  Bots never reached an overflow on their own (they
  die at the first mincer); POKE of 4 dummy slots forces one.  $0535 = 1 / 2 (the free movement
  debug) does not move the Kong in snes9x with a plain d-pad; 2 also stops sprite spawning.
- Misty perf variant (fog3): misty_frame's flat path returns before the old wave code (still used by
  the non-perf compositor path comp_fog).  mw_I = 31 at setup.
- I21e: devkit/scripts/tilechk.py ROM32 ELF SNESROM LOGDIR - every OBJ tile that crosses a 1 KB page
  boundary in the DMA logs (sim DMALOG, one file per entrance), read as the 32X does (world bank table
  from build/gen2/worldbanks.bin, resident blob, straddle windows) vs the SNES ROM.  Logs: sim_vs
  DMALOG=dir/EE.txt with BOT=0 and TP=100,90,96, 4000 frames each.  DMALOG now also lists dropped
  (not tile-aligned) OBJ DMAs as "ODD" (none in the 40 levels).
- I21e Loopy Lights research: the lamp = sprite-list script C5F3 (22 lamps, y 248-348); CODE_80B38B
  subtracts fixed colour 9 / 3 (lights off / on) from BG1, BG2, OBJ and puts BG3 (the cone, colour 0) on
  the sub screen only with the lights on and the nearest lamp ($DD, $DF) within -64..320 of the view;
  BG3 H-scroll per line from HDMA 7F:0200 / 0500 (-66 .. -57: a slight lean), CGRAM per line by two
  HDMA channels (7E:2000 / 2224).  snesppu: BG1 covers 100 % of the picture at the level start.  The
  Genesis S/H cone (plane A high-priority transparent cells, BG2 low priority, S/H with the lights)
  worked in the planes but BG2 is never visible there - reverted (sim / md notes in the transcript).
- I21e Tanked Up Trouble: the SNES wall is (0..6)/31 per channel; the Genesis shadow levels 0 / 29 / 52
  give either flat red / olive patches (no dither, LAMP_DITHER test) or the Bayer dots (current).  The
  lamp flicker changes the scene by ~10 % (snes9x frame means 26 -> 31, mostly the cone): below one
  Genesis level on the wall.
- FG_DEFAULT (test macro): the FOREGROUND option's start value.
## 2026-10-31: I21f - lamp-level flicker, PicoDrive boot freeze, millstone snapshot (BUGLOG "I21f")
- build_viewer genesis_planes(flick_out={}): flick = lamp levels (normal, no keyframes).  fpair() ->
  ((even A, even B), (odd A, odd B)) CRAM words + error gain (thresholds FL_MIN_ERR 150 / FL_GAIN 0.8,
  FL_QUARTER 1); fl_alloc() plans the 3 lines (groups = plane x SNES palette x shadowed, spare lines by
  least weighted error, exact key counting); fkey(pi, base, v, sh, parity, cell); a flipped tile's
  parity = h ^ v flip (tile key gets the flip bit).  GPSTAT=1 prints the line plan ("FL line").
  build_level returns gcram2; md_block flag 256 + 64 words.  md/main.c: fl_a / fl_b / fl_ph, md_vint
  writes CRAM 0-47 first thing every vblank.  Preview: scratchpad pdflk.py ENT OUT B (MODE=A / B / avg).
- PicoDrive debugging: /opt/ref/pd_dbg core instrumented (PD_SH2LOG lines now with SH-2 state, poll
  address, COMM12/14, m68krcycles_done, DMA1 CHCR/TCR; stderr BADRUN / BADEND / BADSYNC / BADPOLL /
  BADSCHED - BADSYNC lines ~110 k are normal frame syncs).  Interpreter test: HARNESS_OPTS=
  "picodrive_drc=disabled".  68000 variants spliced into a ROM at 0x3F0 (same slot size):
  /tmp/claude-0/i21f/mdv/mk.sh, sweep2.sh (boot timing sweep).
- Music start: s_main waits (awake, a 16-iteration pause between COMM reads) for a song request or the
  first token, then music_init; music.c sets I = 6 before PWM_CTRL.  music_play still waits for the
  slave's ack (up to 2 M loops).
- Millstone: spr_ms_cq[2][4] (canvas x, y, on, k per buffer) written at the picture setup (main.c,
  cq block); spr_ms_draw takes position / tilemap from it (cells = opq - 4 * 448 + k * 448), cv_band
  from cq[cvbi].  SNES reference for Millstone Mayhem: harness from work/wmap.st, pokew 0x40 / 0x3E 66,
  START, then "run 16 RIGHT,Y; run 14 RIGHT,Y,B" x 40 reaches the first millstone (second life);
  dumps at /tmp/claude-0/i21f/ms_snes (BGMODE 09, TM 17, BG2 map all high priority).  32X: GOTO
  0x42 + GAMEBOT reaches it (ares / PicoDrive shots /tmp/claude-0/i21f/pms2, ar_ms, ar_ms3).
  DBG_FREE (free movement) does not spawn the millstones.
- I21f checks: plain build md5 8f3b1a9f...; host reg.py 309/309 WRAM-identical to I21e; PicoDrive
  GAMEBOT 12000 + STACKCHK master 1968 / 4080, slave 1372 / 2048; boot sweep 0 / 105.
