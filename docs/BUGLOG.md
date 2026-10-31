# DKC 32X gameplay port - bug log

One entry per bug: symptom (as reported), root cause, fix, how it was verified, and what to
watch for in other worlds.  Newest last.  (NOTES.md keeps the design / measurement history.)

## Tools used to find these bugs (sim3 = host build of the 32X game runtime, /opt/work/sim3)
- `./sim ENT FRAMES` runs the game from the level-start snapshot of entrance ENT.
  Env: SCRIPT (pad script), POKE=frame:addr:val (0 = every frame; $0535=1 = DKC's free-movement
  debug flag), TP=start,every,dx (move DK right), GOTO=frame:ent (load entrance via the cheat path),
  WAILOG, SPAWNLOG, BGPLOG, SKYLOG, PALLOG, RDWATCH, WRAMDUMP.
- Build variants: sim_vs (-DVRAM_SHADOW: byte-exact VRAM copy; spr_check compares our OBJ tile
  pointers and, with CKCHK=1, the chunky tile copies against it), -DWH_CHECK -DROMB_CHECK (reads of
  WRAM pages not resident / ROM pages missing from the blob; "SCRATCH page" = a load-only WRAM page
  aliasing the tile cache used while a level is on screen), SCRIBBLE=1 (randomise the tile cache
  every frame), sim_fi (-DR65_FORCEI: FORCEI=1 runs everything in the interpreter - compare
  WRAMDUMP files to prove an HLE / translation byte-exact).
- 32X debug builds: -DDBG_FREE=frames,dx (free movement + teleport right), -DDBG_GOTO=f,ent,
  -DAUTOLEVEL=n, -DGAMEDEMO, -DPROFT, -DJ3S; ares knobs ARES_SDFILL=30 ARES_UNCSD=11 = MiSTer-like.

- Builds: /opt/work/wb.sh OUT [flags] = the user's ROM (EXTRA_SHFLAGS=-DTCACHE, GAMEOPT=-DR65_OOL,
  LEVELS="16 0C 01 BF 17 E0" - plain ./build.sh without these overflows the SDRAM); ub.sh / mb.sh =
  autoplay / profiling variants.

## World 1

### B1  Sprite colours garbage ("scrambled" Zingers, psychedelic Diddy at dusk, Ropey all garbage when the rain clears)  [test 12]
- Cause: WRAM pages classed "load-only scratch" (aliasing the master's BG tile cache) are used in
  levels: $7F2779.. = the sprite palette buffer (palette upload ring sources), 7F08-7F13 = palette
  fade buffers (CODE_B6B324), 7E:D100.  Every tile bake overwrote them.
- Fix: tools/recomp/data/whcls.txt class 1 for 0D1, 108-113, 127-133.  Master tile cache 512 -> 448
  entries for SDRAM.
- Verified: sim SCRATCH check over 6 levels x 15 sweeps + autoplay: none left.
- Other worlds: re-run the SCRATCH sweep for every new level (and bonus rooms).

### B2  Sprite tiles mismatched while animating (worse after dying)  [test 13]
- Cause: spr_prepare converted changed tiles to the chunky copy BEFORE applying the game's queued
  graphics uploads ($170F) -> one frame stale.
- Fix: convert after the queue (sh/sprites.c).  Verified: sim CKCHK, 0 stale tiles (old order 40+).

### B3  Blank columns past the level end / Coral areas  [test 12, 13]
- The 32X view is 64 px wider (right).  View stops at min($1B25, camera block max) - 64; sprites
  shift by the difference (spr_xoff).  Camera blocks: HLE of CODE_80C246 keeps CODE_B8A853's
  [$50,$52] (byte-exact: sim_fi).  Eased 3 px/frame between blocks.
- Left: blocks with < 64 px of camera travel can still show up to 64 px of the neighbouring map.

### B4  Necky on the hill throws nuts straight down  [test 14]
- Cause: the throw table DATA_B68EE0 (x / y speeds, page DA3 = ROM 0x368C00) was not in the ROM
  blob -> read as zeros (speed 0: the nut just falls).  The profiles never saw a throw.
- Fix: mkgame BLOB_BANKS: every page of the banks holding code (C0,C1,F5,F6,F8,F9,FB-FF; +213 KB).
- Other worlds: same mechanism protects all sprite tables in those banks.

### B5  Croctopus invisible (Coral Capers)  [test 14]
- Cause: build.sh SPR_EXCLUDE listed Croctopus (graphics left out for ROM space, wrongly thought
  to be outside world 1).
- Fix: removed from the list.  Other worlds: SPR_EXCLUDE must shrink as worlds are added (ROM
  banking makes room).

### B6  Slippa (snake) hiss sounds like a whine  [test 13]
- Cause: the S-DSP noise generator was not emulated (KillSlippa SFX $1D: NON = $20, FLG & $1F rate);
  the voice played its BRR sample at pitch instead.
- Fix: sh/spcdsp.c 15-bit LFSR at the FLG rate, NON voices mixed after the voice jobs (not in them),
  key-on allowed without a sample.  Verified: ~12k zero crossings/s on the hiss.
- Other worlds: any SFX with NON set now plays noise (wind, explosions?).

### B7  60 FPS mode = slow motion on MiSTer  [test 13]
- Cause: the frame-skip estimate ignored the master's band help and the post-flip work, so the
  pipelined mode never caught up missed vblanks.
- Fix: in pipe mode, always run the owed game frames (fsteps = min(owed, 3) when owed >= 2).
  Measured ares-M (MiSTer-like): 60 game frames/s at 30 shown.

### B8  Ripple effect gone in water levels  [user request, test 14]
- Re-enabled (rip_on, pause menu RIPPLE): sprites follow the per-line shift (spr_rip_pat, tile8r)
  and the restore ORs the previous row's marks (sprites move with the wave).

### B9  Bonus rooms: wrong graphics / music, day->night inside, day again after  [test 13]
- Cause: rooms were not built; the reload kept the level's own graphics (wrong camera region).
- Fix: rooms are pack levels (make_levels_json.py ROOMS + ALIASES; room_of[entrance] -> level),
  stored in 512 KB SSF2 banks (window 7, set_bank) with their snapshots.  Ropey rooms 1C/1B use
  the storm palette/rain like their level.
- B9b  Rooms far right in a level's map (Reptile 48, Barrel 46/5D, animal rooms at x 44800..48640)
  showed garbage: camx was (int16_t)($88B - spr_xoff) -> negative above 32767.  Now int32.
- Verified: ares captures of every world-1 room vs the snes9x survey shots.
- Other worlds: rooms list per world in make_levels_json.py ROOMS; survey them (/opt/work/survey/run.sh).

### B10  Music "sticky" (Coral music on the map after dying, no bonus music)  [test 13]
- Cause: the 32X chose the song from the level, never from the game.
- Fix: music_follow() each game frame: playing = $051D ? $0523 : silence (CODE_B990CE sets both,
  CODE_B990E7 clears them); mus_hdr_t.id_song[32] maps the DKC music id to the pack song.
  The pack has the songs of the built levels + bonus/death/victory/game over/cabin/map songs.
- Verified: sim SONGLOG: level -> 11 death -> 0C map -> level; bonus rooms 10.

### B11  Animal buddy bonus stages (3 tokens)  [test 13 question]
- Winky / Expresso were in SPR_EXCLUDE (invisible animal), Cranky/Candy/Funky too (now included).
- The bonus item table (CODE_B8B580: 16-byte groups at 7E:C000, bitmaps from 7E:D000) needs up to
  C0-CA and D0-DD in the animal rooms (up to 166 groups / 1678 words, max over ALL entrances);
  C1-C3 / D2-DD were scratch (tile cache alias) or missing: tokens invisible / corrupt.
  Fix: WRAM class 16 (whcls.txt) = pages aliased on the comp levels' BG ring (ring_ent, 8 KB,
  unused outside comp levels), also restored from snapshots.  SDRAM had no room for 22 more pool pages.
- ROM pages no profile read: BLOB_FILES (LevelData/SpriteData/*, the snow tileset of the Rambi room D2).
- Verified: sim_sc map-path entry to D2/A6/6C/D3 (no MISSING / SCRATCH), ares: tokens + counter,
  Winky / Rambi / Enguarde / Expresso visible; the token -> room path (3rd Winky token in 17) works.
- Open: A6 (Enguarde, ~130 sprites) runs ~20 fps on ares (master: logic 54% + sprite prep 41% +
  bands 40% of a frame per shown frame); D2 sky is pink (the SNES shows BG pixels of colour $5A0C
  there - snow tileset sky layer, same in the Snow Barrel Blast preview).
- Other worlds: comp levels must not have class-16 users (none of the item-heavy rooms is comp).

### B12  Second Kongo Jungle map screen shows map 1  [test 13]
- Cause: one picture per world.  The game's map screen index is $02 (CODE_80E225; 0 world map,
  1/2 Kongo Jungle, 3/4 Monkey Mines, 5/6 Gorilla Glacier, 7/8 Kremkroc, 9/10 Chimp Caverns,
  11/12 Vine Valley), tables DATA_80E176 (gfx) / 80E1AA (BG1 map) / 80E1DE (palette).
- Fix: pack map table (off_maps, 13 x {img, pal, bank}) built straight from the ROM tables for the
  world map + both screens of each built world; main.c shows the screen $02 names (re-setup when
  it changes while on a map).

### B13  ROM space (resident area must end below 0x380000)
- BG graphics only DMA'd to the SNES VRAM are dropped (BLOB_DROP Graphics/GFX_Layer): WRAM
  byte-identical over 6 levels + map + bonus (sim3 WRAMDUMP, two blobs).  BLOB_BANKS only adds
  pages holding something besides incbin files.  GAME_BASE aligned to 4 KB (was 64 KB).
- Margin after world 1 incl. cabins' characters: ~60 KB.  For more worlds: per-world sprite pages in a
  second banked window (6) with the snes_pg table at its start (plan in NOTES.md).

### B14  Cranky / Funky / Candy screens were black with sprites  [test 13]
- The screens are game modes outside the level loop ($3E = EE-F3 Cranky, F4-F9 Funky, FA-FF Candy).
- Fix: pictures built from ROM assets (build_viewer SCREENS recipes: BG1 (+ Cranky's BG2 sky) gfx,
  map, palette - found by matching snes9x VRAM dumps to the disassembly's files), shown like a map
  screen.  The dialogue is BG3 map $7400 rows 16-31 with ASCII tile numbers: sprites.c keeps a shadow
  (VRAM DMA + $2118 writes), main.c cab_text() draws changed cells with GFX_Sprite_AreaNameFont
  (colour 1 white, 2 black edge), rows 23+ 12 lines apart from line 176 (the SNES spaces them by HDMA),
  in front of the sprites (the SNES adds BG3 onto BG1 and OBJ).
- Known differences: the SNES fades lines in with HDMA'd colours (green while appearing); white here.
  Cranky's clouds (BG2) do not scroll.  Candy's save writes the game's SRAM copy only (not the cart).

### B15  Kong's banana hoard (JH, walk left at the start)  [new]
- NMI type 808F28 added (BG2 x = camera/2, y 4), room 47 built (4C, the full hoard after the last
  boss, has a BG2 banana pile not handled yet).

### B16  Narrow rooms showed the map beyond them (single-screen bonus rooms, the hoard)
- When the camera range ($1B25 - $1B23, or the camera block) is under 64 px, the view is centred on
  the room and black bars (CRAM 96, opaque black in game levels) cover both sides.

### B17  One garbage frame between map and level (the map picture in the level's colours)
- load_level wrote the level palette long before the canvas was redrawn.  Now the 32X layer is off
  (DISPMODE 0) during a load and goes back on at the first flip of the new level.

### B18  Back in Jungle Hijinxs from a bonus room past the sunset: day again, then a new fade
- A (re)load past the darkness x now starts at the dark palette (wx_first) - no fade.

### B19  Music pack size / resident ROM
- Only the songs of the built levels + jingles/cabins (MUSIC_SONGS from the built levels, EXTRA list).

## World 2 (Monkey Mines) - separate test ROM (/opt/work/w2b.sh)

How a world is added (the recipe, world 2 took ~1 hour of machine time):
1. Level-start snapshots: /opt/work/gp/capsnap.sh EE for each level entrance (snes9x from the world map).
2. CPU / WRAM profiles: FREERUN=1 MEMTRACK=mem.txt WHSTAT=wh.txt ./hostrun ROM snaps/EE.wai 2000 1
   (in /opt/work/gp) for each level -> tools/recomp/data/memprof_wN.txt; compare the WHSTAT classes
   with whcls.txt (pages used in gameplay must be pool (1) or ring (16); load-only = 8; unknown =
   wram_dummy, shared by all unknown pages - never leave a used page out).
3. Rooms: make_levels_json.py derives them from the disassembly's entrance names (<Level>_Enter* =
   rooms, _Exit* / _CheckpointBarrel / _Warp / _Main<n> = aliases); survey each (survey/run.sh EE).
4. Sprite characters: /tmp/claude-0/py/sprdata.py lists the sprite types of each level from the
   disassembly's level sprite data (script labels -> NorSpr ids); SPR_EXCLUDE the rest (ROM space).
5. Build (LEVELS = the world's levels + the animal rooms), then sim3 / sim_sc sweeps per level and per
   room (map path: SCRIPT=mapgo2.txt + POKE $40/$3E): MISSING pages must all be dropped BG data
   (GFX_Layer*, map / game over / intro screens, SPC samples), no SCRATCH, no dummy WRAM.
6. ares autoplay captures of every level (w2u.sh) and a rooms tour (DBG_GOTO) vs the survey shots.

### W2-1  WRAM pages new in world 2
- 7E:2000/2400/2800/2C00 used in gameplay (were scratch), 7E:CB00-CF00 and FD00 read in loads with
  older data (item tables: ring alias), 7E:3000 load-only (was unknown -> wram_dummy).

### W2-2  Level menu: a build without world-1 levels started on an empty world (n % 0)
- The menu starts on the first world with levels and skips empty ones.

### Open (world 2)
- Stop & Go Station's light states (red / green tint) need a check against the SNES.
- One ROM per world for now: the resident area cannot hold two worlds' data (see NOTES.md, world banks).

## Feedback on test 15 / w2 test 1 (2026-10-19 evening) - open list, worked in this order
F1  Bananas (items) only appear inside the SNES 256 px, not in the 32X's extra 64 px (all levels).
F2  Cranky's dialogue: letters partly missing at tile edges, flashing (BG3 h offset -4).
F3  Coral Capers start: tiles turn black near the wall before the hidden banana area; in the hidden
    area the right side shows misaligned graphics / an empty space.
F4  Very Gnawty: black bar at the top; "a blank horizontal bar sometimes" in other levels too.
F5  Winky's Walkway: the light cone's swing is out of sync with the lamp sprite.
F6  Mine Cart Carnage: background visible through the tracks.
F7  Stop & Go: red/green switch on a timer, not by the barrels; HUD / pause menu text black there
    (and in other lighting-effect levels).
F8  Millstone Mayhem: the millstones (BG2 objects) are invisible.
F9  Mine Cart Carnage: some sprites (bananas / HUD banana icon / carts) are corrupt tiles.
F10 Winky's Walkway: enemies / barrels sometimes in wrong colours with blue (lamp colours?).
F11 MiSTer, world 2 ROM: after beating Winky's Walkway the world map picture is corrupt.
Hint from the user: DKC reuses sprites / tilesets with other palettes (ROM space).

### Fixes for the test-15 list (status per item; newest at the bottom)
- F1 FIXED: the item (banana) system culls on the SNES screen: CODE_B8B580/B8B918 compare the
  item x with camera + 0x100 / + 0x10F and the 16-px "visible column" bitmap at 7E:D000 is built
  for 256 px.  tools/recomp/rompatch.py (WS_ITEMS=1, default on) widens the compares to +0x140 /
  +0x14F, 0x0107 -> 0x0147 at 38BA11, and XBA ASL -> XBA LSR at 38BA90 / 38BAF3 (bitmap index).
  sim: bananas past x 256 drawn ("oam wide" count 0 -> 7 in JH at the first banana row).
- F2 FIXED: cabin text redrawn per BG3 row (cab_text1 before the sprites, cab_text2 after);
  marks are read before they are cleared; neighbour cells redrawn (the 4 px h offset).  Cranky's
  sky now uses TM_Layer2_IntroSky from offset 0 (the old black notch) - needs a look in ares.
- F10 FIXED (needs ares check): the lamp shade colours were 32X CRAM 238-249 (+ dot 250) = SNES
  sprite palettes 6/7 colours 14/15 and 0-9 -> sprites using palette 6/7 took the lamp's blues.
  Now the shade is k-means'd to 8 colours at CRAM 128 + 16 k (each sprite palette's colour 0,
  never drawn); main.c lamp_row draws v >= 128 && !(v & 15).  build_viewer.py LAMP_NSH = 8.
  SAME RISK: glimmer (Slipslide Ride, world 4) GLOW0 = 240-251 = sprite palette 7.  Check before
  world 4 ships (sprite palette 7 used there? else move to BG colour-0 slots 16..112).
- F10 (2nd, the real cause): sh lamp_frame() still wrote the cone flicker colours to 32X CRAM
  228-237 (= sprite palette 6 colours 4-13) whenever the flicker step changed, although the cone
  has been on the Genesis since v3.2 -> blue sprites.  Call removed (main loop: `if (lamp_n) ;`).
- F5 FIXED: the SNES swing is CODE_80B891 (BG3 HOFS HDMA table 7F:0200/0500, one frame ahead):
  p = ($28*2)&0x7F folded at 0x40, v = DATA_808BA8[p]>>1, ~v when ($28*2)&0x80, W = v>>1;
  table line j (0 = apex, 24 rows below the cone image origin) HOFS += (W>>4) + j*W/256.
  Period 128 frames (the old table: a fitted 126-frame sine on a free counter).  Verified bit-exact
  vs 140 snes9x frames (/tmp/claude-0/lampsw: table == emu($28 - 1)).  The 64-byte sine goes in
  the cone block (old swing[126] slot), md update_scroll computes W from C_FRAME; the SH-2's
  present() sets C_FRAME = $28 - 1 in lamp levels (also syncs the md flicker, CODE_80B800 = $28).
  ares autoplay (build/w2u_walk.32x): cone apex stays on the swinging lamp.
- F7a FIXED: Stop & Go / Loopy Lights followed an estimate (view centre crossing a barrel +
  the script duration).  lt_frame now reads the game's own state, $0579 bit $200 (what
  CODE_80B206 / CODE_80B38B use), when game_on; the estimate stays only for Torchlight (wb[6]).
- F7b FIXED: HUD / options text black in palette-effect levels: ws_frame's wx_pal has colour 0 of
  each BG palette = 0 and the wx_dirty upload wrote CRAM 1-127, i.e. also CRAM 96 / 112 = the
  in-game HUD's text colours (hud_off_c / hud_on_c).  The upload skips 96 / 112 in game mode.
- F8 FIXED (perf open): the millstone is SNES BG2 = one 256x112 picture (map 32x14 at VRAM $7000,
  tiles at word $2000, palette 2) that CODE_809278 places from the tracked millstone ($DD,$DF):
  HOFS = camx+$80-$DD (hidden: $100), VOFS = $DF-$1C0+camy, TM HDMA at 7E:0687 only hides the
  wrapped copy; it turns by DMAing one of 4 tilemaps (DATA_F95B5E, every 4 frames, k = (~$28>>2)&3
  on frames with $28&3 = 3).  builder millstone_block (kind 7, entrances 42/43 only): tilemaps,
  per-tilemap opaque-cell masks + row spans, 277 chunky tiles.  SH-2: drawn as 8x8 pseudo-sprites
  under the OAM sprites (sprites.c spr_ms_draw, own LUT for BG palette 2), data copied to SDRAM
  (map_mem+17920, each CPU purges its cache once per load: spr_ms_gen).  Picture at 32X
  x = $DD-camx-$80 (+spr_xoff), y = $1C0-$DF-camy-1; shown in the 32X's extra 64 columns too.
  Restore: canvas tiles inside the predicted picture area are not restored in job 3 (ms_defer,
  map_mem+17408); cv_band restores them unless fully covered by opaque cells of the current
  tilemap (spr_ms_cover).  Marks = per-row non-empty spans.
  PERF: ares, picture fully on screen (test -DMSTEST: forced at 60,30): F30 (profile 64 0 28 42 06:
  canvas +21, sprites +23 vs F60 baseline 31 4 7 19 6).  Tried: ROM->SDRAM (-4), skip covered
  restores, all millstone restores in the band phase (job-3 wait 21 -> 0), span marks.  Ideas
  left: 8bpp pre-baked rows (no LUT), split the picture between the CPUs explicitly.
  NOTE: never run two build.sh at once (shared build/ dir: the binaries got mixed up).
- F9 (open, not reproduced): sim_vs CKCHK=1 at 2E: 52400 OBJ tiles compared in 1100 frames, no stale
  chunky rows; SCRIBBLE=1 + free movement + TP (2600 frames, camx up to ~3600, cart levels don't
  free-move far): no SCRATCH / WH / sprite errors.  MISSING pages there are all intentional drops
  (world-map layer gfx 06:8000-D7FF when DK dies -> map, BG3 mine gfx F4:8400, debug font F9:DC00).
  Next: find where in the level the user saw it (ask for the spot / an ares state), or script a
  cart run with jumps to get past x 3600.
- F11 (speculative, needs MiSTer): ares is fine, so likely cache/timing.  Two hardenings:
  set_bank waits (bounded, <3 vblanks) until the slave is idle (s_job_now == 0) before the 68K
  switches window 7 (a slave band / pipe frame could still read the old bank), and map_setup
  bumps the epoch unconditionally (the slave purged only when the bank changed).  If it persists:
  ask the user for a photo (32X picture vs Genesis layer garbage) - md lamp mode (lc_on, 64x64
  planes, S/H off) is cleared on every load (md main.c 338), so the Genesis side looks safe.
- F6 (open, analysed): 2E is mode 1, TM=$17 (BG1+BG2+BG3+OBJ), CGADSUB=$24 (add on BG3+backdrop,
  fixed colour 0 = no visible change).  SNES layers (snes9x dump at x 1400, /tmp/claude-0/mc):
  BG1 = track rails + support pillars (5800, name 2000), BG2 = same scroll as BG1 (7800, name 2000)
  holds the dark ballast strip under the track, BG3 = the dark lattice wall at x/2 (6800, name 6000).
  The builder's 'mine' types (809137 / 809196) map only BG2 -> Genesis plane A; BG3 is dropped.
  Next: confirm which layer our lattice is (Genesis plane dump in ares) and whether the ballast
  strip (BG2) or BG3 is the one missing under the rails; then add the missing layer (plane B)
  with BG3's x/2 scroll.
- All-worlds ROM step 1 (worlds 1+2 banked) works; see NOTES "window-6 world banks".  The sim
  check found and fixed: animal rooms' pages in one world bank only (now resident), and the sim
  itself linked the common-only blob (all banked-build sim runs before that fix were invalid).

## Feedback 2026-10-20 (test 16): w12 build on MiSTer/ares + first builder ROM in ares
User wants: PAL = normal speed (done).  After the fixes: the COMPLETE game (title screen, saving,
game over, final boss + ending).  Worried compaction causes regressions -> regression checks.
Screens in uploads (2026-10-20 03:26 *.jpg); ares state of the Coral Capers corruption:
uploads/67ce882b-dkc32x_gameplay_w12_test1.bs1.
G1  Priorities: when the Kongs enter a bonus room / exit a level they walk IN FRONT of the graphics
    (door / exit hut should cover them).
G2  Winky's Walkway: lamps have a non-animated copy behind the animated (swinging) lamp.
G3  Level end: the blank area past the level end shows for a second before the screen scrolls back.
G4  Cave level (pink crystal cave, world 1; first hidden bonus room = timed jumps through 3 barrels):
    after exiting that room all bananas are garbage tiles; also the platform tiles near the entrance
    to get the frog are missing.  After entering + leaving the frog's room the bananas are fine again.
G5  After beating a level (Kongs' dance) the screen flashes black for a split second.
G6  Stop & Go Station: stop/go barrels sometimes garbage; ~1/3 in bananas garbage (no bonus room);
    the bonus room entrance garbage (fine after leaving the room); corruption starts exactly when a
    red-eyed Rockkroc comes into view (2 screens); Rockkroc's platform is invisible.
G7  Millstone Mayhem: the tire uncovered via the hidden bottom barrel is garbage until touched;
    garbage bananas there too.
G8  Riding Winky, Diddy's DK barrel can't be opened (check SNES behaviour).
G9  Necky's Nuts: one tile in Necky's face missing/garbled in one animation frame.
G10 Very Gnawty defeated: the screen cuts to the SNES width + a line or two of junk at the bottom.
G11 Sometimes a frame of glitchy graphics when a level starts.
G12 World map: the Kongs walk very slowly -> faster (even faster than the SNES).
G13 BUILDER ROM regression: Snow Barrel Blast sky pink + blocks around the mountain edges (again).
G14 BUILDER ROM: Poison Pond (level menu) froze on a garbled screen.
G15 Coral Capers black corruption (= F3) - ares state attached.
### Fixes for the test-16 list
- G13 FIXED (builder data): levels.json vram_runs started 0x10-0x30 bytes late in 139 runs (the
  recipe finder missed each asset's first tile(s)): BG3 tile 0 of the snow set ($C000, the solid
  sky colour $5A0C = palette 6 colour 1) was empty -> plane B sky transparent (pink colour 0) and
  the edge tiles' sky pixels merged to cyan.  Extended every run backwards while the asset bytes
  match the survey VRAM (/tmp/claude-0/runfix.py, backup /tmp/claude-0/levels_pre_runfix.json).
  Also fixes the first tiles of lamp light / TM_Layer3_* / bananas (viewer) etc.  Not a capture
  (auto vs hand) difference: surveys + weather.npz are byte-identical.
- G14 (hang FIXED, sprites open): comp perf variant (Poison Pond 545 fog tiles, Misty Mine 2 x 652)
  copied its fog tiles into tiles_ram = the small TCACHE tile cache in game builds (29.5 KB) and
  ran over comp_palbuf / game_on etc. -> game_frame without init -> error -> hang_show.  Now
  draw_fog_row reads fog_tiles = tiles_ram when they fit, else the pack (ROM).  OPEN: cv 2/3 (perf
  canvas) draws NO sprites (sh->spr = game_on && cv == 1) -> PP / MM need sprites in that mode;
  also PP turned grey after walking right (palette fade / comp palette?).  World 4/5 work.
  Debug tools made for this: -DHANGANY (watchdog also during loads, hang_show turns the 32X on),
  -DLOADSTAGE (LDST/LDST2 markers in COMM 10, read back from the 68000's dbg_comm via a PicoDrive
  68K RAM dump: harness 'dump r 2 65536', words byte-swapped), and /opt/ref/pd_dbg = PicoDrive with
  PD_SH2LOG=file (per-frame master/slave PC, PR, SR; HARNESS_OPTS=picodrive_drc=disabled).
  /opt/work/bv.sh OUT SNAPDIR "flags" (build.py variant), /opt/work/pd.sh ROM DIR "script;..."
- G6 FIXED (+G4 likely): world-bank page violations found with a new sweep (/opt/work/wsweep.py = every
  main level, /opt/work/rsweep.py = every bonus room via GOTO; sim_vs + TPGRID free-move grid over the
  whole level at 4-5 heights; /opt/work/classify.py maps the pages to incbin files; NOTE sim messages
  go to stderr).  MinePlatform (Rockkroc's platform, Stop & Go) and CavePlatform (Bouncy Bonanza = the
  pink cave with Winky, and MCM bonus 3) were only in other worlds' banks -> zero pages -> a size-0
  sprite DMA (64 KB) trashed the OBJ tiles: garbage bananas / barrels / bonus entrance, invisible
  platform.  world_chars: +MinePlatform/CavePlatform (w2), +CavePlatform (w5); BLOB_FILES: QueenB_Fly1
  for w2/5/6 bonus rooms (Zinger frames?), 6:LevelData/JungleLevels.bin (Loopy Lights bonus 2),
  AreaNameFont; memprof_all +A7E/A7F = UNK_E9F724 (read by the underwater vertical streaming 818168
  in Coral Capers / Clam City at f 31: was zero -> maybe F3/G15).  PicoDrive sweep of Stop & Go
  (DBG_FREE): bananas fine, Rockkroc's platform drawn.
- Stop & Go BG2 (the lamp wall in the gaps, palette 1): colours were 0 (not in the CGRAM recipe;
  the capture's palette 1 holds the lamp HDMA's last line) -> runtime_palette: 809137/809196 BG2
  colours 17-31 = DATA_B9A2FC (as walkway levels).  Gaps now show the wall + lamps (brighter than the
  SNES - check the light keyframes).
- survey_cgram(): colours the CGRAM recipe does not cover come from the capture (capture.py now dumps
  cgram.bin per survey).  Previews: OUG bonus rooms (were black), SBB / Ice Age bonus rooms, Torchlight
  bonus 1, Loopy Lights bonus 1 now like the SNES; Crystal cave (C6) sky too bright (HDMA gradient).
- Regression tool: build_viewer previews of all levels (DKC_FLAGS=--preview, /tmp/claude-0/reg/{A,B,C})
  + PIL diff; compare against survey shot.ppm.
- G14 sprites: perf canvas (cv 2, Poison Pond) now draws the game's sprites (sh->spr and the sprite
  pass allow cv 2).  Misty Mine (cv 3, wave bands) still has NO sprites - needs a sprite pass through
  the per-band line table; the fidelity comp path (-DCOMPFID) is broken in TCACHE builds (garbage).
- G2 FIXED (likely): in game levels the game draws the swinging lamp itself (sprite CeilingLight); the
  canvas still baked the builder's static lamp shade -> a second, still lamp.  lamp_row skips the shade
  pixels when lamp_noshade (= game_on at the lamp setup).
- G3 FIXED: the wide view's offset at a level's right end (spr_xoff) eased in at 3 px/frame after the
  camera limit arrived, showing the blank columns past the level for ~20 frames.  Now never past
  cur_lv->xmax (the builder's level end - 64): d = max(eased, camx - xmax) at once.
- G12 DONE (untested on a real map walk): map screen + MapDK's X/Y speed ($0E89 / $0EF1, disassembly
  OWSpr10_MapDK) non-zero -> MAP_FAST = 3 game frames per picture; the step loop's early break allows
  extra steps on the map (WAI 80E90E).
- G1 FIXED (logic; JH exit shown): OBJ priority.  sim OAM stats: normal play = priority 3; walking into
  the exit hut / doors the Kongs get priority 2 (behind BG1 high-priority tiles); 0/1 never seen.
  Builder: per-vchunk 16-bit mask of BG1 high-priority non-empty tiles (Tileset.vprio, tileset record
  + off_vprio).  sprites.c obj_behind/tile8_behind (ROM code - SDRAM is full, 416 bytes over when it
  was SDRAM_CODE): priority 0/1 only where the canvas pixel is 0; priority 2 skips pixels of
  high-priority tiles that are opaque.  spr_prio_grid/mask/gw/gh set per level (0 in perf levels).
- Misty Mine (cv 3) entered from another level: white/black block garbage at the top + bottom of the
  32X fog layer (not the Genesis: -DNO32XDISP test; not the palette: PicoDrive PD_PALDUMP).  Not from
  HUD text, sprite marks, FB contents or the level-fx state (all tried).  MM -> MM reload and NOGAME
  are clean -> depends on the game running.  OPEN (with MM sprites).
- G9 Necky's Nuts: reproduced in PicoDrive (one 8x8 tile at the eye in the head-down pose); the sim's
  SPRCHK (VRAM shadow vs our OBJ tiles + chunky rows) finds nothing -> ROM-only: suspect cache /
  aliasing of a WRAM sprite source.  OPEN.
- G8 (frog + DK barrel): the game logic is the original code -> should match the SNES; not checked.
- G5 / G11 (black flash after the dance, a glitchy frame on entry): INIDISP brightness is not emulated
  (fades are black/full) - needs a brightness fade (32X CRAM + Genesis CRAM scaling).  OPEN.
- G10 (Very Gnawty defeated: SNES width + junk lines): probably the narrow-room pillars (camera range
  < 64 px wider than the view) after the boss; not reproduced.  OPEN.
- Regression suite: /opt/work/regress.sh NAME [flags] = AUTOCYCLE ROM (every level in turn, game on),
  PicoDrive shots every 50 frames (240), diff vs /tmp/claude-0/regress/base -> NAME_diff.png.
  Deterministic.  Baseline = state after the fixes above (2026-10-20 05:00).

## B-REINIT (2026-10-20, found while testing the ending) - every level load restarted the game state
- Symptom: in the complete game the hoard after the ending went to Jungle Hijinxs instead of the
  credits ($563 = 4C, $565 = 16 six frames into the hoard).  Every level / bonus room the game loaded
  re-initialised the 65816 state from that entrance's level-start snapshot (lives, bananas, progress,
  position after a bonus room ...).
- Cause: a load-progress marker was inserted as `if (!game_reload) LDST(3); game_level(...)` - the
  `if` guarded only the marker (LDST is a statement), so game_level -> game_init(snapshot) always ran.
  In the bundle delivered 2026-10-20 05:14.
- Fix: braces.  Found with a -DDBGRING build (per game frame: $1C/$3E/WAI/$563/$565/last key into a
  ring in SDRAM) + the PicoDrive debug core's new PD_SDDUMP/PD_SDFRAMES (SDRAM dumps; PicoDrive keeps
  SDRAM byte-swapped per 16-bit word).

## B-EARLYJOB (2026-10-20) - the slave's early canvas job ran on front-end screens
- Symptom: the credits (drawn by fescr.c after the hoard, a level) showed the top ~75 lines flat
  brown: the VRAM map mirror (tc_sdata) rows 0-9 were zeros.
- Cause: the per-frame "early" slave job (restore the canvas under the old sprites, uses the tile
  cache = tc_sdata) was started on every game frame while cvb[] was still valid from the last level -
  also on blanked / front-end frames, where tc_sdata is lent to fescr.c (map mirror, line buffers).
  Affects every front-end screen entered from a level (GAME OVER, the ending speech, the credits).
- Fix: only with !game_fblank && !fe_vm.  Found by dumping SDRAM (PD_SDDUMP) and comparing the
  mirror with the captured VRAM.

## B-FE-MISC (2026-10-20, the ending)
- Front-end screens were drawn one line too high (line y = SNES scanline y + 1): fixed in fescr.c
  (BG rows, HDMA lines, mode 7); host render now matches snes9x exactly.
- VRAM DMAs in mode 0 to $2118/$2119 (low or high bytes only) were dropped (the credits' map
  attribute fill): handled as CPU writes.
- The credits setup loads in the same frame that sets $3E = 5E: its map / graphics / table pages
  were read from the hoard's world bank (zeros) -> resident (memprof_fe.txt).
- The hoard (4C) as a level showed its banana pile as yellow fragments: the bananas are BG2
  high-priority tiles in front of BG1; drawn by fescr.c now (entrance-keyed front-end screen).
- Front-end screens were very slow under big sprites (Cranky's intro / speech ~7 fps game): see the
  NOTES speed entry (partial redraws, clean line copies, per-line BG2/BG3 scroll signatures).
- The sides beside the SNES picture showed the backdrop colour (cream behind the credits) and
  sprites walking in from x >= 256: black CRAM 128 + wiped after the sprites.

# H-series: user's full playthrough report (2026-10-20, ares unless noted)
Screenshots in /root/.claude/uploads/77201e1a-0573-5bea-b59a-643538a338dc/ (e28f149c, f21d26f1,
e1bdf2dc ... c4eb6cbd).
- H1  ROM is 16 MB: Mega EverDrive Pro supports <= 15 MB -> shrink (dedup / bank packing).
- H2  Power-on: a frame of night-time jungle graphics flashes.
- H3  No music on the logos (SNES has it).
- H4  Rare logo grid lines flicker / glitchy lines while it animates.
- H5  The full-size Rare logo flashes for one frame when the zoomed logo reaches its smallest size.
- H6  Cranky intro (DK jumps in): plays the jungle level song instead of the intro music.
- H7  Intro: as Cranky's platform cross-fades to the jungle, black bars along its outline.
- H8  Intro TNT explosion: white horizontal bars instead of a fade to white.
- H9  Sometimes the intro only runs once: after the title fades, blank screen.
- H10 Player select: the text flashes horizontal bands of different brightness.
- H11 World maps: DK / Diddy map sprites miss tiles.
- H12 Jungle Hijinxs: entering DK's house shows the jungle level (tire, balloon), not the house.
- H13 Coral Capers: the small fish has a corrupt frame; black corruption flashes near the first
      octopus / hidden banana and later after the red balloon (screens with Enguarde).
- H14 Animal-token bonus stage: when it ends you're back at the START of the level.
- H15 Candy's save: missing tiles on Candy / barrel; jumping in the barrel shows letters
      "PQPQXTUTU RSRSZVWVW PQtu RSVW" instead of the save-slot select.
- H16 World 1 boss (Very Gnawty): flashing black corruption after the boss is beaten.
- H17 Mine Cart Carnage: the track's transparent parts show the far background (wall missing).
- H18 World 2 cave level: banana tiles still corrupt.
### H-series fixes (2026-10-20 evening / night)
- H1 FIXED: bank packing best-fit DECREASING by tile-set group (build_viewer: levels sharing a tile
  set are placed together, largest groups / levels first) -> 14 level banks instead of 17;
  ROM 13.5 MB (14155776 bytes).  Was 15 MB exactly after the first best-fit change.
- H2-H10: see NOTES (front-end renderer; verified in PicoDrive).
- H11 FIXED: map_setup / fe_setup clear spr_prio_mask (the last level's BG1 priority mask hid the map
  Kongs' and Candy's priority-2 pixels).
- H12 FIXED: DK's house (entrance 5C) and the empty hoard (47) are entrance-keyed front-end screens
  like the full hoard (capture.py ent_pre; 15 front-end screens).  GOTO 5C -> house, 0E -> JH outside.
- H14 = SNES behaviour: the token bonus returns to $0565 = $7EF9FC[level] (CODE_BF9EF8) = the level's
  last star barrel (or its start).  The return path itself checked (D2 with $0565 = 25 -> JH star
  barrel, as snes9x).  (The delivered build already had the B-REINIT fix.)
- H15 FIXED: Candy's barrel -> the save-slot screen has NMI F894 (= the file select, captured): main.c
  runs fe_find() in cabins too (the cabins' own NMI F142 has no entry).  Candy's sprite holes = H11.
- H13 / H16 FIXED (black bars / staircases):
  * Coral Capers: camera blocks narrower than 64 px of travel (shafts: 937-944, 1344) took the
    narrow-room path = black pillars (32 px each side) flashing on and off at block changes.  Pillars
    now only when the whole level ($1B23-$1B25) is that narrow (bonus rooms); blocks show the map.
  * Very Gnawty beaten: the game locks the camera ($1B23 = $1B25) -> pillars + the 3 px/frame easing:
    pillar_draw writes black INTO the canvas, which stays valid -> the eased view slid black stripes
    into the picture.  Pillar changes now invalidate both canvas buffers and the view jumps (no ease).
  * Found with -DCAMRING (main.c: per picture c / $1B23 / $1B25 / block / d / pillars / game frames)
    + pd_dbg SDRAM dumps; -DGAMEBOT (run right, unstick) and -DGAMEBOSS=id (jump onto sprite id) test
    inputs; Enguarde start: snapshot with $055D = 1, $0516 = 0C (snaps_eng).  Gnawty HP = $14F9,x.
  * OPEN: the small fish's one-frame wrong tile (not reproduced).
- H17 FIXED: Mine Cart Carnage BG2 = a second layout at layout + 0x4B00 (CODE_818B6B, same scroll as
  BG1: the bed between the rails).  build_viewer bakes it under BG1: composite bake entries (flag 2:
  top bake << 19 | vflip << 18 | under << 5 | vflip << 4), sh/main.c tc_composite.  MAX_BAKE grew
  (1891 -> 2338): game builds no longer size the slot map by it (TCACHE: unused) - SDRAM end 0603EC50.
- H18 FIXED: the rock debris (FlyingRock / SmallFlyingRock) is preloaded when a level is entered from a
  bonus room's exit; only world 1's bank had it -> garbage OBJ tiles (bananas, DK) after Bouncy
  Bonanza's bonus 1 etc.  world_chars: all worlds.  Found with /opt/work/xsweep.py (every entrance
  alias GOTO'd in sim_vs, WORLD / MISSING page report).
- PERF FIXED (all levels with BG1 high-priority tiles): every priority-2 sprite took obj_behind's
  per-pixel path (a variable shift = __ashrsi3 call per pixel): Very Gnawty's arena ran at 15-20 fps
  game speed (slave 31 % in obj_behind).  Now only 8x8 tiles touching a high-priority cell go
  per-pixel (prio_any), the loop has no variable shifts -> the arena at full speed.
- PERF FIXED: underwater levels ran at exactly half speed until the first death (pictures took 2
  vblanks while the frame-skip estimates said they fit).  3 late pictures in a row -> catch up.

# I-series: user report 2026-10-20 (after the H build)
- I1  Systemic: previous areas' state leaks into new ones (asks for a general fix).
- I2  Music: exit to map via the pause menu, then the level select -> the previous level's song.
- I3  Misty Mine: no sprites at all; the fog has tile corruption (screenshot f3bc5b25).
- I4  After exiting Coral Capers, the map has an invisible horizontal line through sprites (bca9b009).
- I5  Intro: Rare logo grid artifacts remain; Nintendo logo glimmers stretched horizontally (a6dcd592);
      platform -> trees cross-fade uneven per area; TNT fade to white still white lines (6a19e079).
- I6  Save: the save made with the previous build loaded fine, but after using the level select the
      save file is gone (b67ffae5: empty slots).
- I7  60 FPS mode: better in places, worse in others (many enemies?).
### I-series fixes (2026-10-21)
- I1 ROOT CAUSES of "the last area's data leaks into the next":
  * STACK OVERFLOWS (found with -DSTACKCHK paint + pd_dbg SDRAM dumps): the slave's 2 KB stack was used
    to the last byte (the music mixer runs in its DMA interrupt on top of the canvas jobs: spcdsp_mix
    had 1100 bytes of locals) and ran into the master's stack top (m_main's own variables); the
    master's stack (1 KB at 0603F400-F800) went 824 bytes below its area into the RAM the linker
    fills (only ~360 bytes before the last globals).  Fixes: mixer work buffers static / cache RAM
    only (silence on the first frame), shared_t copies in m_main / s_main static, master stack 2 KB
    (mars.ld RAM ends at 0603F000), master tile cache 448 -> 444 entries for the SDRAM.  Measured
    now: master 1512 of 2032 bytes, slave 1320 of 2048.  Canaries (0603F000, 0603F7F0) checked every
    loop: a broken one stops with the STUCK page (STK line) instead of corrupting memory quietly.
  * render_state_reset(): every screen setup (level, map, cabin, front-end, level menu) resets the
    sprite renderer's state (priority mask, ripple pattern, millstone, x offset, front-all, pillars,
    the chunky tile copy in map_mem).
  * I2 the level select started a level's snapshot after its song upload: music_follow kept the
    previous level's uploaded song -> game_level sets game_apu_song from $0523.
  * I6 a level start from the menu unpacked the snapshot's 2 KB save RAM (the capture's: empty) over
    the player's saves; the next save write wrote them to the cartridge -> game_keep_sram.
- I4 = the ripple pattern (Coral Capers' wave bands) left on for the map's sprites: fixed by the reset.
- I3 Misty Mine: no sprites (canvas mode 3 had no sprite pass): spr_draw_lt draws them through the
  wavy line table, the fog under them is redrawn from rectangles kept per buffer (in map_mem's last
  KB); the fog palette upload stops at CRAM 127 in the game (it overwrote the sprite palettes); the
  fog corruption = the sprites' chunky tile copy left attached in map_mem (the fog map's memory).
- I5 front-end renderer (fescr.c):
  * Nintendo logo shine: its lines are modes 1 and 5 -> per-line BG scroll signatures were off (all
    224 lines redrawn when the shine jumped) - modes 1/3/5 now (hi-res 16-px tiles in fe_rowflag).
  * Pictures that run out of time are no longer shown half old / half new (the slices = "stretched
    glimmers", the cross-fade "by parts"): held and finished in the next calls (FE->hold, at most 4,
    the palette still updated); front-end catch-up up to 12 game frames.
  * TNT flash: main screen = backdrop only, the scene on the subscreen added -> drawn as the main
    screen, the brightening backdrop added through the palette (FE->subm): a smooth fade to white.
  * Cross-fade with sprites shown (no colour sums): main / sub checkerboard where neither is dark.
  * Rare logo (952 colour sums needed, 126 CRAM entries): "brighter of the two" for the rest,
    recorded per line (FE_OVF) and redrawn when one of its colours changes brightness level.
- I7 60 FPS mode: the slave alone drew Jungle Hijinxs at 30 pictures a second (normal mode 60).  The
  mode now measures both (vblanks per 64 pictures) and keeps the faster one per level.
- I8 REGRESSION in the I build: no music after the logos.  The I1 change dropped the mixer's SDRAM work
  buffers "used only before the cache switch" - wrong: the cache RAM (two-way mode) is only used in the
  comp levels, everywhere else the mixer needs them -> silence.  Buffers back (static, 512 B), the
  SDRAM for them from the master tile cache (444 -> 436 entries).  Audio now checked in every test
  run (harness 'audio file' -> peak level), not only pictures.
- I9 REGRESSION in the I build (user, MiSTer): entering Misty Mine from the map showed a wall of tiled
  foreground graphics, then the STUCK page (M STAGE 5, M PC 06000C78).  The I3 sprite pass keeps its
  per-buffer fog rectangles + count in map_mem's last KB; other screens use that memory (the CPU-written
  OBJ tiles at map_mem + 16 KB) and left junk there -> the first frames redrew junk rectangles (tile
  wall) and spr_draw_lt overran.  Fix: count zeroed at the fog setup, every rectangle and count
  validated before use (bad -> skipped, too many -> the buffer marked invalid = full redraw).
  Reproduced with -DPOISONMAP (render_state_reset fills map_mem with junk): old build wall + STUCK,
  fixed build fine; 34-level poisoned tour clean.
- I10 Torchlight Trouble (found by the poisoned tour, present since the gameplay build - also in the
  H build): the beam's lit area was pink / blue blocks, the Kongs cyan.  The beam was made for the
  level viewer (lit copy of the BG colours at CRAM 128+); in the game those are the sprite palettes.
  Also the beam was a fixed wedge with a timer bob - the game's follows Squawks (apex = his position,
  top edge slope per wing pose), and the beam switched on at a fixed x, not when Squawks is freed.
  Now (game levels): the game's own window HDMA table ($7F1C28, CODE_819370) gives the lit x per
  line (game_beam_lines); CRAM 1-127 = the lit colours, dark pixels remapped (bm_dmap) to 13 spare
  CRAM entries holding the scene's dark colours (c - 15: black + a few reds, merged); every sprite
  palette is inverted by the game (the scene is the subscreen the beam subtracts) -> shown as
  (30,30,31) - c; the 68000's highlight sprites (BG2 lit in the beam) move to the game's apex
  (COMM10 = 0x4000 | x/2 << 8 | y, y 0xFF = off).  Known differences: sprites outside the beam stay
  lit (SNES: dark), the Genesis BG2 outside the beam is shadowed to half brightness (SNES: darker),
  and the highlight sprites keep the template's slope (the 32X layer is exact).

### I11 fixes (2026-10-21, user's round after the I build)
- Torchlight Trouble: the background flashed random colours (jumping / turning) and the beam went
  horizontal.  The game's window table holds two edges per line (left-facing Squawks: WH0 = 0, WH1 =
  edge) - only one was read.  Now both (game_beam_lines: kind 1 left-open, 2 right-open, 3 mixed), soft
  edges, the extra 32X columns extrapolated; the Genesis BG2 is no longer lit at all (user: "the
  background does not have to be lit") - no more palette swaps on it.
- Misty Mine: sprites are drawn behind the mist (spr_draw_lt keeps fog dots 1-127, the sprite shows
  between them, as the SNES adds the fog over OBJ); mist at its thickest less opaque (misty_palette
  g = 3 + ((v * I * 21) >> 9)); the pause menu shows on the pseudo-transparency levels (fog levels:
  pm_open with cv 1-3, the other buffer invalidated).
- Hanging lamps: beams missing a chunk = plane cell streaming aliasing in the 64x64 plane (lamps 512 px
  apart shared cells; the want window was too wide) - window cx + 22 > vx - 1 && cx < vx + 41, aliased
  lamps dropped after a clear.  Stop & Go Station's lamp cones (green / red, switched by the lights
  state, script C5E9) are NOT done yet.
- Mine backgrounds "NES-like" (Mine Cart Carnage style dark levels): the Genesis planes of lamp / dark
  levels are dithered (4x4 Bayer between the two nearest 9-bit levels) instead of rounded to few
  colours; lamp levels keep palette line 3 for the cones (lines 0-2 for the BG).
- Things vanishing past the SNES width (ropes in Forest Frenzy, objects at the screen edge): the
  game's activation / despawn windows are widened for the 320-pixel screen (rompatch.py, bytes
  verified): spawn edges BDF595 / BDFB8E (+0x120 -> +0x160), particle wrap B69A69 / B69A9E / B69AB5
  (0x140 -> 0x180), on-screen activation BED5B2 (0xF0 -> 0x130); ropes = HLE of CODE_80A7ED
  (hle_rope_col: x range -80..320, OAM x bit 8).  The pushed tyre: not located yet (needs the place).
  Walk-off exits are deliberately not widened.
- Final boss (Gang-Plank Galleon): the island stayed put - BG2 is scrolled per line by the game
  (CODE_809A6E table $7F0200: lines 0-63 and 97-127 from camx / 4 with a per-line slope, 64-96 = 0).
  md fx 6 reproduces it with line scroll on plane A.
- Ending save state (.bs5): ares could load it after a v146 -> v145 patch, but the SH-2 crashed - the
  state's ROM is a different build; the ending was tested from the Gang-Plank snapshot (-DENDTEST).
- PERFORMANCE (main issue: Cranky's cabin, the credits "skipping frames").  Measured in ares with
  FEDBG bars (now in the left border: no line redrawn for them): the speech took 10-16 vblanks a
  picture, the credits 2-4.  Causes and fixes (sh/fescr.c unless noted):
  * the line signatures were evaluated up to five times per line and picture -> once (FE_LSC /
    FE_LSC2 / FE_LSCA), and a line with the registers and map rows of the line above reuses them
    (HDMA write masks from fe_lines_regs); fe_lines_regs works 32 lines at a time.
  * Cranky's clouds (BG2, 72 lines, ~25-34k cycles a line from the cartridge) are now the Genesis
    plane B: build_viewer fe_bg2_plane (FE_GPLANE = {EEA3}) packs them as a plane block next to the
    screen's VRAM; the 68000 loads it (command 0x9E00 | screen), scrolls it with BG2's offsets (posted
    as C_CAMX / C_CAMY) and fades it with the brightness (weather keys black -> palette, k / 16 in
    COMM10).  The 32X leaves those pixels see-through and never redraws a line for them.  The plane
    is checked against the game's BG2 map (mirror) - a different map -> drawn by the 32X again.
  * the sprites: the slave draws the lower part (job 2, split at half the sprite cells; it is idle on
    these screens); restores under the last sprites copy only the 8-pixel columns marked.
  * blank characters are skipped before their bytes are read (FE_CHST shared with fe_rowflag), and a
    4-bpp row with no pixels before decoding.
  * text written into a map (FE_DIRTY per buffer and 32-word map row, from the VRAM mirror hooks):
    only those columns redrawn (fe_dirty_range).  (The speech's text appears by HDMA - BG3 switched on
    two lines a frame - those lines are drawn whole.)
  * the scroll-only budget starts after the signature pass (it was used up before the first line).
  Result (ares): the speech and the in-game Cranky cabins 1 vblank a picture (60 fps) while the text
  stands, 2 while it is revealed; the credits 1 (occasionally 2).  Host renders (sim3 fehost) of the
  logos, title, file select, credits and hoard are byte-identical to before.

# I12: user report 2026-10-21 (after the I11 build)
- Torchlight from the level select started paused, Start did not close the menu (mashing did, then
  it came back); very slow, "drops a lot of frames whenever you change direction"; after leaving it
  the map sprites had wrong colours; a "corrupt tile" (screenshot: checkerboard strip next to DK);
  slowdowns everywhere, the intro "2 fps at points" even in ares.
### I12 fixes (2026-10-21)
- Start: (a) the level menu's Start (and the Start held while switching on - it picked the menu's first
  level at once: prev_pad now starts as the pad at boot) is not passed to the game until released
  (start_block); (b) the game only accepts Start / Select after 16 frames without them (CODE_809AFF,
  $1E33 counts down): our pause menu's "resume" is queued (pm_req) until the game takes it; (c) while
  paused with the menu unchanged no new pictures are drawn (pm_frz: the game ran at 4-6 vblanks a
  frame drawing the same picture).
- Map colours after Torchlight: render_state_reset clears the beam state (bm_game kept the dark palette
  mapping).
- The checkerboard strip: the beam's soft (dithered) edge.  Hard edges now, as the SNES window.
- Torchlight speed (ares, beam on: BG 42-49 % -> ~21 %, dark parts BG 17-21 %):
  * the beam per picture line: the master works out the changed pixel spans (bm_spans_make, its own
    edge history of the last two pictures, open sides at fixed far values) and each CPU redraws only
    those in its band (bm_spans_draw) - turning round redrew every row of both buffers before;
  * the tile cache holds Torchlight's tiles dark (tc_fill maps through bm_dmap) and the lit ones under
    their own key (tile_ptr_lit, tag | 0x8000) - the lit pixels were decoded per row;
  * the sprites use the chunky tile copy in Torchlight too (map_mem's lower half; the beam tables moved
    to the upper half).
  The 60 FPS mode pipeline was tried for Torchlight and left off (PicoDrive slower, ares no better).
- Intro (front-end renderer, sh/fescr.c; ares FEDBG measurements):
  * Rare logo: every line was redrawn every frame (224) - lines with colour math had all colours'
    darkness in their signature, and the wireframe lights a colour or two each frame.  Now per line the
    colours whose darkness it looked at (FE_DUSED; fe_dh_fix puts the drawn line's own set into its
    stored signature).  15-35 lines a frame, ~3x faster.  On mode 7 screens with the colour sums used
    up (no "brighter of two" records there) all colours count, as before.
  * Nintendo logo: the "brighter of two" re-checks (60-80 lines every frame from the glow) are spread
    over 4 pictures; while a picture is held unfinished BG2-4 keep the scroll it started with (the smoke
    jumps twice every 9 frames: a held picture was redrawn over and over).  ~2 fps -> ~3-4 fps in ares;
    still the slowest screen (150 lines of logo + smoke per jump, ~35k cycles a line).
  * per pixel colour math: last pair + a per-picture pair table (FE_PRC) and colour brightness table
    (FE_BRT); 8 pixels written as words also when not on a word boundary (scrolled BGs); hi-res lines
    skip blank characters; mode 7 without rotation reads one map row per line.
  * the title wipe ran interpreted 65816 code (CODE_80DAE5 / 80DC94 and their continuations 80DAF6 /
    80DCBE, ~3.6k instructions a frame): translated, in SDRAM (the resident ROM is full - 0x2FFDC0 of
    0x300000).  WRAM byte-identical to the interpreted build over 4900 frames (sim3).
  * FEDBG builds wrote CRAM once per drawn line (a palette write waits for the blank in ares): their
    measurements were too slow.

### I13 fixes (2026-10-23, user's round after the I12 build)
User report: "the first level didn't even load" (STUCK page: M STAGE 3, ENT 0016, M PC 020113E4),
"flashing random graphics between loading maps/stages".
- STUCK entering Jungle Hijinxs from the map (I12 regression, reproduced in PicoDrive with the same
  STUCK page): fescr.c's sprite job on the slave (I11) used the token COMM(C_TOKEN) ^ 0x8000; the
  slave answered with that value in C_DONE, and m_main's next wait for an idle slave without a job
  of its own in between (the "give" wait of the first level frame) waited for its own last token
  forever.  Now one counter (main.c job_token; m_main's `token` is a macro for it).
- Map screens: map_setup set the map's sky colour (transparent: the Genesis backdrop) before the
  new picture was drawn - for a few frames the last screen's picture showed as white / brown
  silhouettes (file select -> island map, island -> Kongo Jungle's map; I11 too).  Black (opaque
  0x8000, CRAM + sky lines) while both buffers are drawn, the palette + sky right after the flip.
- Level starts: the 68000 switched the Genesis display on as soon as the planes were loaded - the
  planes showed alone, half built, for 2-4 frames, then one frame with the load-time (day) colours
  before the weather keyframe.  The display now stays off until the SH-2's second picture of the
  level (md_vint counts C_FRAME changes: disp_hold), and the 32X layer goes on at that same flip
  (disp_pending = 2).
- Level loads (death, bonus rooms, level changes): the 32X layer went off at once, the old level's
  Genesis planes stayed on until the 68000 got the new level's command (several frames of the old
  background alone).  load_level first sends 68000 command 9D00 (display off, backdrop black) and
  waits for it.  tools/prebuilt/md.bin rebuilt.
- Our level menu (pause menu LEVEL SELECT, I12 regression): the frozen pause menu (pm_frz, I12)
  skipped the rest of the loop, go_menu was never seen - LEVEL SELECT did nothing.  Also its sky /
  palette (pending_sky) are set with the menu's first picture, the screen black until then.
- Verified: PicoDrive power-on -> file select -> map -> Jungle Hijinxs -> pause -> exit to map;
  ares (MiSTer model, scripted pad: ARES_PADSCRIPT, frame dumps: ARES_DUMP) every frame of
  boot -> map -> level -> map -> level: no frame with the old picture / planes alone; regress tour.

### I14 fixes (2026-10-24, user's round after the I13 build)
User report (MiSTer + ares, 8 screenshots): progress lost after pausing in level 2, rope tops
invisible, Torchlight pause menu closing by itself and stutter when Squawks turns, Poison Pond sprite
line / RIPPLE OFF incomplete, snow weather changes slow, Ropey night -> day slow + background not all
there + gradient jumps, Funky black board + slow plane-barrel jump, Misty mist doubling + menu text
lines doubled, Temple Tempest bonus room sprites on black, "no music for level" builder warnings.
- PROGRESS LOST (major): pausing while holding run (Genesis A) = A + Start = the level viewer's menu
  shortcut, also live in the complete game; the menu's Start then began Jungle Hijinxs from a fresh
  state.  A + Start now only in the viewer (`!full_game`); the complete game reaches our menu only via
  the pause menu's LEVEL SELECT.
- Rope tops invisible (all straight ropes, Slipslide Ride): hle_rope_col (I11 HLE) used WLW / WLW_SET
  (gbr-relative word access) on COMPUTED odd addresses.  The SH-2 needs aligned words: PicoDrive /
  MiSTer read the aligned word (wrong bytes), ares raises an address error; the host sim tolerates it
  (hid it).  Byte-wise W16L / W16L_SET in that function.  ares got ARES_MISALIGN=file (logs every
  misaligned SH-2 access: PC, address, size) - the I14 bot build is scanned with it.
- Torchlight pause menu closing at once: with 2 logic frames per picture the game took the pausing
  Start in the picture's 2nd logic frame, one picture before the menu code saw `press`, which then
  queued a resume.  The Start that opened the menu is ignored until released (pm_hold).
- Torchlight stutter on turns (ares MiSTer model, per-picture phase HUD -DPROFT -DPICPROF): one
  picture took 6-10 frames when the beam swung round.  Cause: bm_spans_draw worked line by line and
  fetched each tile's dark AND lit versions on every line; in the direct-mapped tile caches the two
  keys evict each other, so a turn re-baked the screen's tiles up to 8 times.  Now per tile row (both
  versions fetched once, copied), and the drawn beam edges follow the game's at most 48 px a picture
  (the open sides as -/+400): the beam slides round over a few pictures, each picture's redraw bounded.
  Turning test (12 turns): longest picture 12 -> 6 vblanks, no freeze.  (I14's first attempt - holding
  the old beam for 3 pictures - did not help and is gone.)
- Poison Pond RIPPLE OFF: the option only stopped the 32X layer's sprite wobble; the 68000 kept the
  BG1 plane ripple.  68000 level command bit 0x2000 (no BG1 ripple) + the SH-2's comp ripple off.
  The 1-line sprite seam at ripple band ends is still open (sprites share the rippled canvas).
- Snow weather transitions: the game's palette-fade routines ran interpreted (CODE_80AA42 / 80A992 +
  helpers, ~4000 instructions per step); translated (byte-exact).  MiSTer model logic 74 -> 68 %.
- Ropey Rampage, night -> day:
  * slow: the game's darkness fade (CODE_80B0CF: a 280-entry palette lerp, CODE_80B118 / 80B16F and
    their loops 80B11B / 80B172) ran in the interpreter every 8th frame - ~4000 instructions = several
    frames on MiSTer each step.  Translated, plus the other direction (80B0E8 / 80B0F4 / 80B13F /
    80B14B: the Jungle Hijinxs / Orang-utan Gang sunsets) and BCF230 / BCF23C / BCF23F / 80EEBF /
    80EED8.  sim3 WRAM byte-exact (0C / 16 / 0D to the end with free movement + teleport, and the
    20-entrance verify2 set).  ROM: the 7 Torchlight keys added earlier in I14 (no measured gain) were
    dropped again to make room (resident end 0x2FFAD0).
  * "background not all there": the distant treeline (BG3 map $2400 as a second plane B map) never
    came up after the storm - the 68000 tested `wx_nkey == 2` for jungle levels, and Ropey has 3
    keyframes (night, day, lightning).  Now a jungle flag (wx_jungle).  Matches snes9x's picture.
  * haze bands jumping: the 68000's BG3 gradient table is double-buffered (the H-int read a half
    rewritten table).
- Funky's Flights: cabin text used CRAM 254 / 255 = SNES sprite palette 7 colours 14 / 15 (Funky's
  board black).  The text now uses the cabin picture's own brightest / darkest colours (cab_colours).
  The plane barrel ran in slow motion: cabins had no catch-up logic frames - now as on other screens.
- Misty Mine: the fog line table added a row for odd (x + y) to keep the checkerboard phase - bands
  whose parity changed moved by a row: lines doubled / dropped, flickering with the waves.  Removed.
  With the pause menu open the table is flat (the menu's text was cut / doubled by the bands).
- Temple Tempest bonus 2 (53), rooms C1 / C3 / B8: the map can't start them, so their survey was
  taken from the wrong state (sprites on black).  capture.py JUMP_ROOMS: the parent level, then the
  game's own NMI-pointer path into the room ($3E / $40 / $2E / $565 = room, $1C = 806F).
- Builder "warning: no music for level ..": LEVEL_MUSIC is only the level viewer's song table; the
  complete game plays whatever the game uploads.  Now one note line.
- Verified: verify2 (20 entrances x 3000 frames x 2 inputs) byte-exact vs the I12 sim; ares MiSTer
  model: Ropey end vs snes9x picture, Torchlight turning test, Misty fog + pause menu, Poison Pond.

### I15 (2026-10-24): Barrel Cannon Canyon - a barrel missing, no way on
User report + ares save state (entrance 77 = the midway barrel, DK in the barrel cannon at x 33792):
"there is a missing barrel so I can't continue".
- Read the state: ares v145 state (our ares accepts it with ARES_ANYVER=1, loads with --save-state 1
  next to a ROM of the same name); SDRAM image at file offset 0x123B (16-bit words swapped); the user's
  ROM = our build byte for byte, so plain14.elf symbols apply (wram_lo, wram_hp -> full 128 KB WRAM).
- BCC's sprite list (ROM FD981C): the group at list entries 8A-8C = the barrel cannon DK was in
  (C989), plus 9EC1 and the vertically moving barrel C9A3 at x 33836.  Spawn flags $192B+8B/8C = 0:
  the moving barrel was gone while its group leader stayed spawned, so it never came back.
- The user's WRAM injected into a snes9x state (pages from wram_lo / the pool, $1C = 80AB): the real
  game behaves the same from there - the damage was done earlier.
- CAUSE: tools/recomp/rompatch.py WIDESCREEN widened the game's sprite SPAWN / KEEP-ALIVE windows and
  "enemy acts while on screen" range by 64 px (I11 / earlier) - those are game logic.  snes9x A/B
  (same inputs, original ROM vs patched ROM): the patched game diverged within seconds in BCC.
- FIX: only display code stays widened (OAM builder x culls, item drawing, the falling leaves / snow
  flake wrap); spawn / keep-alive / on-screen windows are the SNES's again.  snes9x A/B with the
  remaining patches: Kong + sprite tables + spawn flags identical (10 entrances x 60 input blocks);
  the only differences come from SNES lag frames (the wider OAM pass costs CPU time on the SNES; the
  32X has no lag frames) and the particles' own x.  Cost: objects appear 32 px from the 32X's right
  edge (where the SNES spawns them), some enemies stand still there until they reach the SNES view.
- "Anything else missing?": every main level (40) swept in sim3 with free movement over a 64 x 64 px
  grid of the whole level (TPGRID, WORLDBANK = its world, ROMB_CHECK, WH_CHECK): no missing game
  pages, no world-bank / dummy / scratch WRAM use; the only page misses are the intentionally dropped
  map / layer-3 graphics (death -> map screen, snow levels).  Native vs interpreter (FORCEI) WRAM per
  frame: identical but for the known stack residue byte $1F6 and the rope HLE's OAM / scratch bytes.
- Note for the user: the old save state still holds the broken barrel group; leaving the level and
  re-entering it (or dying) respawns it.  Save states are tied to one build.

### I16 (2026-10-24): edge pop-in, hidden-item holes, Misty Mine dither
User report on I15: objects popped in out of thin air near the right edge, ground holes of hidden
items showed before their covering sprite; Misty Mine's mist dither broken by the wave ("misaligns the
dithering pixels"), wants Poison Pond's clean alternation.
- Edges: I15 had put the game's spawn windows back (camx - $20 .. camx + $120), so objects appeared
  32 px inside the 32X's right edge.  Now the 32X VIEW is centred on the SNES picture: SNES x -32 .. 287
  (main.c WS_CENTER = 32: the view x = camera - 32, shrinking to 0 at the level's left end, growing to
  64 at the right end as before).  Those columns are exactly the game's spawn window: nothing appears
  inside the picture (objects can still show half of themselves for a frame at the very edge, as on
  the SNES at its edges), and the game logic stays the SNES's (snes9x A/B with the display patches:
  identical).  Display patches (rompatch.py _ws_display): OAM builder culls (both sides), item drawing
  down to x - 47 (ADC $10F -> $16F, the left column clip $0F -> $2F); the particle wrap patch is gone
  (the game's own wrap range is the new view).  Sprites keep spr_xoff (SNES x + 32).
- Misty Mine: the checkerboard is baked into the fog canvas; bands (4 lines) whose ROW offset had the
  other parity showed the inverted pattern -> the dither cut into strips moving with the wave.  Band
  rows now take band 0's row parity (as the columns already did: band-to-band differences in 2-line /
  2-pixel steps), and the fog origin's column + row parity is held even with a 1-px nudge (Poison
  Pond's trick), so every band shows one pattern and each framebuffer shows its own phase: a clean
  60 Hz alternation (measured on ares dumps: all bands the same phase, flipping every frame).  The cv3
  sprite restore rectangles still used the I13 parity row: fixed.

### I16 optimization run (2026-10-24): interpreted 65816 code -> native SH-2
User request: convert the code that still runs in the interpreter to native code, testing every step.
- Measure: sim3 coverage = 309 jobs (every main level with the pad script, teleports and a free-movement
  grid, every room / bonus alias, front-end menu + idle, ending); sim_fc (XC_PROF) counts interpreted
  instructions per entry key, now EXCLUSIVE (r65lib.c interp_c: nested interpreted calls count for the
  callee).  Gameplay frames (picture on), debug free-movement keys excluded.
- Before (I16 start): 17.9 M interpreted instructions in gameplay frames (31.9 M in all frames).
  Worst rooms ~975 per frame (Millstone Mayhem bonus 2, Vulture Culture bonus 1, Slipslide bonus 2:
  ~150 % of a MiSTer frame at ~600 cycles each), Oil Drum Alley bonus 3 735, Orang-utan Gang bonus 1 616.
- After: 1.0 M in gameplay frames (0.42 M of it the front-end menu screen), 14.9 M in all frames.
  Worst room now the Enguarde bonus at 29 per frame (~4 %); every other level / room <= 15.
- Steps (each one: build, BADJUMP scan of the generated code, host regression = per-frame WRAM hash
  of all 309 jobs vs the previous step's sim: identical every time):
  1. batch 1 (I16 morning): 27 -> 4.7 M (old inclusive count).  2. batch 2: 60 continuation keys.
  3. The interpreter now hands off at JML (r65interp.c 0x5C returns a JUMPKEY like JML [abs]): the
     sprite behaviour JML table at BF8363.. used to keep the whole called routine interpreted although
     the targets are ordinary functions; with the hand-off a translated target runs native.  The JML
     targets (B6D29A ...) + BFEA09 (data/entries.txt: entered from a pointer table no trace saw) +
     5 stubs translated.  4. batch 3 (119 keys) and 5. batch 4 (20 continuation keys).
- Room for it (resident ROM was full): (a) DUP pages: 25 resident sprite-graphics pages of the common
  characters are now in every world bank at the same offset (mkgame.py world_split, worldbanks.py
  unchanged - its bank lists start with the DUP pages): window 6 always shows one of the 7 world banks,
  so these pages are always visible at one fixed address, exactly like resident ones (verified: same
  table entry + same bytes in all 7 banks).  Limited by the fullest bank (world 5: 2 KB left).
  (b) game_pg_world[4096] (host-sim page checks) only in host builds (#ifdef R65_HOST_SIM): 4 KB.
  Resident end 0x2FFD50.
- Robustness: md_setup now also sets the mapper's window 6 to bank 6 (a reset while in another world
  kept that world's bank while the SH-2 assumed bank 6).
- Checks: ares viewer run through worlds 4 -> 5 -> others on the MiSTer model (Kong sprites right after
  every world-bank switch); ares GAMEBOT 9000 frames MiSTer model: no one-frame flashes; ARES_MISALIGN
  8000 frames: none; PicoDrive GAMEBOT 12000 frames + STACKCHK: master 1936 / 4064 bytes (unchanged),
  slave 1364 / 2048.
- MiSTer model (ares, bench.sh with DBG_GOTO into the room): Millstone Mayhem bonus 2 master total
  236 % -> 142 % of a frame, Vulture Culture bonus 1 232 % -> 138 % (old = I16-start translate list +
  -DR65_NO_JMLHANDOFF).  Main-level demos ~1 % (little interpreted code was left there).
- Left interpreted: the front-end menu screen (80BA2C, its own renderer; analysis gaps -> BADJUMP), the
  resume point 808159 inside 808131 (mid-function return), ~100 small keys (~0.5 M over the coverage,
  ~42 KB of code - no resident ROM left).

### I17 (2026-10-25): 60FPS MODE (option 1), sprite list on the slave (option 2), missed jumps, swap wait
User request: "Do option 1 and 2 ... option 1 should be selectable ... The 1 frame input delay seems to
really mess me up if the frames drop (I miss jumps etc!) ... make everything ultra stable".
Measurements: devkit/perf/perf.py (PERFREC builds, MiSTer-model ares, perf_rec read from an SDRAM dump),
picture equality devkit/perf/pic_eq.py (+ peq2/peq3 style alignment: ares dumps miss single frames).
- B-PIPEWAIT (bug, since I7): `if (give) LDST2(13); while (...)` - LDST2 is a statement in test builds
  and empty in user builds, but the dangling `if` made the WAIT unconditional in every build: in 60FPS
  MODE the master waited for the slave's whole picture before its next logic - no pipeline at all.
  Same pattern at LDST2(9).  Braces.  (60FPS MODE in JH, MiSTer model, no frame skip: 41 vs 36 fps.)
- B-FUSEDROWS: the fused job's master rows were overwritten by the generic `mrows = window` branch
  (the master redid every row the slave also drew).  Only matters for -DFUSED / 60FPS MODE splits.
- 60FPS MODE: sprite list (spr_prepare) runs after the slave's kick (the slave starts its BG earlier,
  its sprite bands wait for pipe_mbg); the master helps with sprite bands only while it can finish
  before the picture's deadline (helping past it delayed the next logic frame); the early-job time
  checks moved to where the master waits for that job anyway.
- Option 2 (sprite list / tile conversions on the slave): split spr_prepare_a / _b; measured in the
  normal mode: no gain (the slave is on the critical path after the logic) - kept as the 60FPS MODE
  deferral above.
- B-PADSKIP (the missed jumps): with frame skip two logic frames ran back to back and both read the
  pad register: a press that lasted one vblank could fall between two pictures and was lost (also
  hold lengths were quantized).  Now the 68000 numbers its pad samples (COMM8 high byte), the master
  queues every vblank's sample (V interrupt + pad_take), each logic frame takes the next one (SNES
  order), the last logic frame of a picture always the newest; samples passed over when the game is
  behind give their new button presses to the next one.  PADTEST build (68000 sends B for one vblank
  in 32), MiSTer model: JH 15/15 (old 14/15), level 2 18/18 (old 14/18), JH 60FPS MODE 14/14 (old 11/14).
- FLIP_ASYNC (normal mode speed, no added lag): present() waited for the vblank of the swap - ~half
  a frame per picture, 20-30 % of the master in the levels below 60.  Gameplay pictures with nothing
  for the swap's vblank but the shift register now ask for the swap during the display (it happens at
  the next vblank, the V interrupt sets the shift) and the master runs the next logic meanwhile.
  Nothing draws until the swap is seen: fb_sync() after the logic (and in wait_flip / present /
  load_level / the menu), the slave's early job 3 and job 5 wait for it themselves.  Pictures with
  palette work, ripple, the first pictures of a level etc. stay synchronous.  Frame skip after
  gameplay pictures catches up every vblank (no snowballing without the vblank wait): 60 logic
  frames a second.  MiSTer model, frame skip on, normal mode: JH 29 -> 40 fps, level 2 22 -> 38,
  level 4 26 -> 44, level 13 29 -> 49 (game speed 59.3-60).  Also in 60FPS MODE (its swap overlaps
  the next picture's setup and logic; the slave's job 5 waits).
- Sky: the JH sunset table / rain + snow sky fades were written into sky_ram while the previous
  picture was still on screen (lines below the write showed the new sky for one frame); now they go to
  sky_next and present() copies them right after the swap.
- 60FPS MODE auto fallback: the normal mode is chosen unless the 60FPS MODE is >= 10 % faster.
- Picture equality (no frame skip): async vs synchronous swap identical in levels 2, 12, 14 (fast
  and MiSTer model; only frames the ares dump missed); JH identical below row 110 (above: the
  68000's sunset haze, whose table update timing follows the SH-2's command timing).
- sdgame image: PERFREC builds overflowed the first 256 KB - the SDRAM code image now lives in world
  bank 0 (0x36F000, header + checksum, finalize.py), m_main checks header + copy (red screen if bad).
- B-RESETBTN (found in I17's checks, pre-existing since the first build): the console's reset button
  restarts the 68000 (hot start: it waits for the SH-2s' boot handshake M_OK / S_OK) but the SH-2s
  only get a VRES interrupt - they ignored it and played on, the 68000 never answered a command
  again: STUCK page a few seconds later (PicoDrive soft reset; real console; MiSTer / ares reset =
  power-on, not affected).  start.s m_vres / s_vres (ROM code): the slave stops its DMA and parks,
  the master copies the SDRAM image from ROM again, clears the COMM registers, both post the
  handshake and run the cold start.  PicoDrive: reset in Jungle Hijinxs -> Rareware logo -> the game
  again, twice in a row (I16 build: STUCK).
### I18 (2026-10-25): user's round after the I17 build
User report: Winky's Walkway dither + dark tiles in the lamp cones, a "jumpy" camera in the water
levels, tires vanishing a few pixels off screen, Millstone Mayhem frame rate, flashing sprite tile parts
in the Rambi bonus, the pause menu rippling in water levels, small tile glitches (fish, Necky).
- B-LAMPDITHER (Winky's Walkway, Tanked Up Trouble, Platform Perils): since I11 the lamp levels ran
  without shadow/highlight (the cone on plane A had to be at full brightness) and the dark wall (BG2,
  SNES 5-bit values 0-4 per channel) was ordered-dithered between the Genesis normal levels 0 and 52 -
  a coarse dot pattern all over the wall.  And Bayer 4x4 puts its 8 lower thresholds all on one
  checkerboard half: the cone's 50 % dots cover one half per frame, so inside the cone the wall showed
  only its "below" pixels - dark tiles.  The cone's half was chosen by the game frame's parity in the H
  scroll, so with frame skip (2 game frames a picture) it stayed on one half for seconds.
  Now: S/H on (md: 0x8C89 in lamp levels too), the wall shadowed = the finer levels 0/29/52/70/..,
  dithered between those (a faint pattern; the browns right on average); cells with colours above
  the shadow range (the lanterns' flames, LAMP_BRIGHT5) normal = high priority (in front of the cone,
  as before), dithered between the normal levels; the cone colours at the shadow levels (the brightest
  core loses a little: blue 20 -> 16 of 31); the dither matrix = Bayer with rows 2-3 rotated by one
  (both checkerboard halves get every other threshold); the cone's other half every VBLANK by a 1-line
  V scroll of plane A (68000, independent of the game frame).  Checked: offline render of D9's BG2 vs
  the SNES colours (scratchpad wkren.py / wksnes.py), ares MiSTer-model shots of D9, 30, 2B.
- B-WATERCAM (the "jumpy" water camera, from I16's camera code): the 32X view (320 wide, d = 32 px
  of extra view each side) followed the game's camera blocks (hle_cam_clamp ranges) at every block
  change - in the water levels the blocks change often while swimming, so the view stopped at block
  ends and the d target jumped, then slid 3 px a picture.  Now only NARROW blocks (< 64 px of travel,
  the I16 case: rooms the SNES view barely scrolls in) are used, and a narrow block stays in use until
  the camera leaves it (sticky); otherwise the level's own range.  camlog (CAMLOG build): Coral Capers
  d = 32 constant over the swim.
- B-MENURIPPLE: the pause menu was drawn into the canvas, which the water ripple's line table shifts
  per band; the line table and the sprites' ripple are flat while the menu is open (60FPS MODE too).
- Tires (Stop & Go Station: rolling one to the high barrel): NOT a port bug.  Objects are removed by
  the game's own keep-alive window, camera x - 32 .. camera x + 288 (the SNES view + 32 px each
  side); the 32X view (camx - 32 .. camx + 288) is exactly that window, so a tire disappears just as
  it leaves the 32X picture - on the SNES the same tire disappears 32 px outside ITS picture.  Host
  sim A/B (WIDESCREEN=0 vs 1, 9 levels x 4000 frames): gameplay WRAM identical (only the pose table
  0x0CDD-0x0D44 differs) - the physics and despawn rules are the SNES's.  Pushing a tire out of the
  view and walking back loses it on the SNES as well (the game's design).
- B-PIPETILES (flashing sprite tile parts, 60FPS MODE): the slave draws picture N while the master's
  logic N+1 runs; levels without the chunky tile copy (overlays: snow, rain, lamps, the fog, the
  animal bonus rooms' snow) drew from the live obj_tile table, which N+1's VRAM DMAs re-point - parts
  of the sprites showed the next frame's graphics.  The renderers now use obj_tile_dr, a copy taken
  in spr_prepare_b (and at level init / chunky detach).  Rambi bonus reached in the port with
  DBG_GOTO=f,0xD2 plus $1E15 = $0402 (DATA_81D102: the animal-bonus flag the map path sets).
  The fish (Coral Capers) / Necky (Necky's Nuts) one-tile glitches: the VRAM-shadow sim still finds
  no wrong tile in normal play; the same race is the likely cause when they were seen in 60FPS MODE.
- Investigated, no change: GFX_Layer1_ShadedDebugFont (B9:DB45, uploaded to OBJ VRAM $0800 in every
  level) has its middle page dropped with the BG graphics (BLOB_DROP) - the sim with that page zeroed
  (scratchpad sim_z, ZEROPG=E77): no visible sprite uses those tiles (0 SPRCHK differences in 16, E1,
  DE, 31, D9, 30, D2).  Kept dropped: the resident ROM is 688 bytes below its 3 MB limit.
- Millstone Mayhem (performance): the millstone (BG2 picture, 8x8 pseudo-sprites, sprites.c) read its
  palette LUT and opaque masks UNCACHED (4 uncached SDRAM reads per 8-pixel row); now cached, with
  the per-CPU cache purge once per level load (spr_ms_sync, once per cv_band); fully opaque cells
  inside the screen take ms_tile8_opq (no transparency tests, h flip decided per cell).  MSTEST
  (picture always fully on screen), MiSTer model: 20.8 -> 22.5 fps (frame skip), 60FPS MODE 23.8;
  MSNODRAW bound 27.9 - the rest is the canvas restore under the picture and the logic (~14 ms a
  game frame here).  No bigger lever without memory for pre-baked rows (SDRAM full).
### I18b (2026-10-26): MiSTer STUCK entering Jungle Hijinxs' first bonus room
User photo: STUCK page, M STAGE 3, ENT 0006, M PC 0200EFA8 = load_level's final wait for the 68000
(COMM6 bit 15 never cleared); ares fine.  Not reproduced (ares, PicoDrive, MiSTer-model ares).
- Found on the way: the game writes its SRAM during play - $6057 = $61, then $60 again (sim log,
  e.g. JH frames 2247 / 3957): content unchanged, but game_sram_dirty was set, so a full cartridge
  save ran at the next screen-off - typically a bonus room's entry.  On MiSTer the cartridge SRAM is
  mapped over the whole upper 2 MB of the cart while it is enabled (rtl/CART/cart.sv SRAM_EN: any
  access with A21 set), for the SH-2s too; and the 68000's sram_xfer waited forever with interrupts
  off if the SH-2 gave up its side of the handshake (2 s timeouts) - every later command then hung,
  exactly the photo's state.  Fixes: (1) sram_save compares an FNV hash of the 2 KB with what the
  cartridge holds (after the load / the last save) and skips unchanged data; (2) it starts only with
  the 68000 idle (no command pending); (3) md sram_xfer gives up after ~4 s without progress and
  then clears COMM6 only if it still holds its own 0x9FF0.  Saves with new data still write
  (SRAMPOKE test: PicoDrive SRAM dump with the changed byte, checksum right).
- STUCK page: a second column with the 68000's side - CMD (COMM6), C2 / C4 (the SRAM handshake),
  VBL (the 68000's vblank-numbered pad sample) - refreshed every half second (the page loops in the
  V interrupt): a counting VBL = the 68000's interrupts still run.
### I18c (2026-10-26): MiSTer - garbage tiles after starting a new game
User photos (MiSTer): the player-select screen without its tiles, then Cranky's intro and Jungle
Hijinxs with scrambled tiles; Start (a reload) fixed it.  ares fine.
- Cause (MiSTer core, rtl/CART/cart.sv): while the cartridge SRAM is on ($A130F1 = 1) EVERY access
  with cart A21 set - $200000-$3FFFFF - goes to the SRAM, for the SH-2s too: the level / front-end
  banks (windows 6 / 7) and the resident SNES pages above 2 MB.  A new game writes new save data,
  the save ran at the next screen-off while a slave job (front-end picture) was still reading its
  tiles from window 7 - and the SRAM stayed on for the whole word-by-word handshake (tens of ms).
  The garbage stayed in the tile caches.
- Fix: md sram_xfer hands the data over through a 2 KB buffer in 68000 RAM with the SRAM OFF; the
  SRAM is on only for the 2 KB burst (read for a load, write for a complete save: a few ms).
  sram_save waits for an idle slave (no job, C_DONE == C_TOKEN) as well as an idle 68000 (else it
  tries again at the next screen-off) and purges both CPUs' caches afterwards (epoch).
  PicoDrive: save (SRAMPOKE: the changed byte, checksum right) and load (a second run started from
  that SRAM flips the loaded byte back) both work.
- Music refill counters (music_halfs / music_skips / music_maxdt, sh/music.c) for the "garbled
  victory fanfare on MiSTer" report - see NOTES I18c.
### I18d (2026-10-29): mapper writes with RV = 1 (32X hardware manual)
The user sent Sega's 32X Hardware Manual (1994).  Section 4.1 ("ROM access competition"): the
cartridge bank registers $A130F1-$A130FF (the SSF2 mapper this ROM uses for windows 6 / 7 and the
SRAM switch) are to be written with RV = 1 (DREQ control $A15107 bit 0).  Emulators and the MiSTer
core accept them either way; real hardware / flash carts might not.  md mapper_w: RAM code, interrupts
off, RV = 1, the write, RV = 0.  It must NOT be inlined into ROM code: with RV = 1 the cartridge sits at
$000000 for the 68000 and $880000 is gone - the first build inlined it into md_setup and ares (which
models that) fetched garbage: the SH-2s never saw window 6 = bank 6 (red screen).  PicoDrive does not
model it.  The SH-2s' ROM reads wait while RV = 1 (a few cycles).  Checked: ares (normal and MiSTer
model) boots and loads levels from several banks, PicoDrive SRAM save + load, MiSTer-model bot run.
### I19 (2026-10-29): performance round + Genesis background dither
User: "push the architecture to its absolute screaming limits" (all the small optimizations together),
dithering for the Genesis-drawn backgrounds where the SNES colour is missing - without fighting the
checkerboard transparencies that scroll over them.  ("Rolling framebuffer": already in since I3 -
the 352x256 wrapping canvas with line-table scrolling, BUGLOG I3.)
- B-LATESWAP (performance): a picture finished in the vblank or in the last lines before it took the
  synchronous swap path - the master waited for the NEXT vblank (the 68000 has already taken the
  camera for this one), a whole display period idle.  SYNCWHY (test counters per reason, present2):
  in Jungle Hijinxs 1 picture in 7 went that way.  Now: wait for the display to start, then the
  asynchronous request (swap at the next vblank) and go on with the next logic frame.  The picture
  is shown at the same vblank as before; only the master's idle time goes.
- B-RIPPLESYNC (performance): the water levels (ripple: a per-line shift table read by the slave's
  H interrupt) never used the I17 asynchronous swap - every picture waited for a vblank (Croctopus
  Chase 20 fps on MiSTer).  The shift table now changes in the V interrupt of the swap's vblank,
  like the line-0 shift register (af_sft next to af_shift; the slave's H interrupt reads sft_show
  uncached).  The next picture's table is written only after its fb_sync (the swap happened).
- Interpreted 65816 code (IKEYPROF survey, levels 1-33 to frame 2400): Stop & Go Station ran a
  per-frame table lookup (81:9B78, ~20 instructions, ~600 MiSTer cycles each = ~4 % of the master)
  in the interpreter - translated.
- More interpreted code translated (data/lowrom.txt, placed in the first 256 KB of the ROM): the
  level-load lighting / palette table builders (0.3 - 2.3 s of MiSTer time per level start in 14
  levels: Orang-utan Gang, Oil Drum Alley, Blackout Basement, Stop & Go Station, the snow levels,
  Torchlight Trouble, Slipslide Ride, Vulture Culture, Forest Frenzy, Winky's Walkway, Trick Track
  Trek, Mine Cart Madness, Tanked Up Trouble) and every key with >= 400 interpreted instructions in 2400 frames of a
  level (Temple Tempest, Bumble B Rumble, Croctopus Chase, ...).  IKEYPROF after: e.g. Orang-utan Gang
  264 K -> 3.5 K interpreted instructions to frame 2400, Oil Drum Alley 151 K -> 0.1 K.  Host check:
  devkit/cov/reg.py, all 309 coverage jobs, per-frame WRAM identical to the I18d translation set.
- B-MUSGAP (MiSTer music dropouts): a refill that overran its buffer (the DMA ran dry for a moment)
  was followed by a whole buffer (5.8 ms) of silence to catch up - every dropout was twice as long
  as needed.  Now the silence only comes after two late refills in a row (or one over two buffers).
  MiSTer model, 40 s of JH: 13 late refills, 0 silent buffers (before: 14 silent buffers).
- The SPC700 engine (generated from the ROM at build time) compiled -Os: 28 -> 13 KB, refill times
  unchanged (MUSHIST histograms).
- Measured (devkit/perf/perf.py, MiSTer model, frame skip on; I18d -> I19): JH 36.9 -> 42.0,
  Croctopus Chase 20.0 -> 34.7, Millstone Mayhem 34.9 -> 45.4, Winky's Walkway 35.5 -> 40.1, all
  ten levels in HANDBOOK 7.
- B-PLANEDITHER: the Genesis planes (BG2 / BG3 of most levels) are 4 lines x 15 colours, so many
  SNES colours (especially after the SNES colour math) map to a distant palette entry - Coral
  Capers' sand was magenta, the water a flat saturated blue.  build_viewer pd_index: per tile pixel,
  if the nearest entry of the tile's palette line is off by more than PD_MIN_ERR, the best PAIR of
  entries of the same line is mixed with the parity-balanced 4x4 matrix (I18 B-LAMPDITHER: both
  checkerboard halves get every other threshold, so a 50 % checkerboard overlay at any scroll
  position always shows both colours of the mix in the right proportion), weighted against far-apart
  pairs (PD_SPREAD: grain).  No new tiles, colours or palette lines (the per-pixel choice stays in
  the tile's own line; tile counts unchanged over all 115 levels).  Off in the lamp / weather / day-
  cycle levels (their own dither and palette animation).  Offline (scratchpad pdprev.py, blurred
  error vs the SNES colours): Coral Capers 143 -> 36, Temple Tempest 79 -> 30-59, Forest Frenzy 13 -> 6-8.
  PLANE_DITHER=0 in the environment turns it off at build time.
### I20 (2026-10-29): background colours again, Blackout Basement lights
User: the I19 dither looked awful (repeating noise over large areas, far-apart colour pairs); wanted
only plain checkerboards between close shades (Super Donkey Kong 99's look) or else the nearest colour;
asked whether shadow / highlight could give the Genesis more colours.  Blackout Basement: "the lights
don't go out, instead the sprites disappear".  Objects still vanish near the screen edge (open).
- B-PLANESH (more colours): S/H was already on in the ordinary levels, but a plane was shadowed only as
  a whole and only when both planes were dark (max SNES channel <= 15.5).  Now per cell: a cell is
  shadowed (low priority: half brightness, the finer dark levels 0 29 52 70 87 101 116 130 instead
  of 0 52 87 116) when its colours are at least as close that way; bright cells stay normal.  A pixel
  is shadowed only when both planes' cells there are low priority, so with two planes this needs
  plane B all dark (B shadowed, A per cell; A cells with transparent pixels shadowed too); otherwise
  the old rule.  When the colours no longer fit the 4 lines, the closest ones are merged (up to 24);
  failing that the level keeps the old palette.  No new tiles.  Also in the light-switch levels
  (Stop & Go, Loopy Lights, Blackout Basement), not in the weather / day-cycle / lamp / beam levels.
  Coral Capers: the sand brown instead of magenta, the water muted like the SNES; the caves, the
  temple, the factory and Tree Top Town keep their dark detail instead of going black.
- B-DITHER2: the I19 4x4 ordered mix of any two line colours is gone.  A checkerboard is used only
  between the two Genesis levels around the SNES value (one level apart per channel), only when the
  nearest colour is clearly off (weighted error > 1500) and the mix much closer; both colours are
  added to the cell's palette line (if they do not fit, plain colours).  With the shadow levels this
  is rare.
- B-BLACKOUT: CGADSUB $A7 subtracts the fixed colour from BG1-3 and the backdrop, never the sprites:
  4 with the lights on, 30 off (snes9x: COLDATA $E4 / $FE, CGRAM unchanged).  The captures held a
  dark frame's 30, which the converter's colour math skips (CGWSEL bit 1), so the port always showed
  the lit colours.  Now a light-switch level (LIGHTS[0x41] + its rooms): keys -4 / -30, the state
  from the game's last all-channel $2132 write (game_coldata, >= 16 = dark).  The sprites stay.
  The "vanishing sprites" part was not reproduced (ares runs of the I19 build: the Kongs left the
  picture only by falling into a pit); with the lights going out now, the sprites stay as on the SNES.
### I21 (2026-10-30): Galleon island, water camera, Ropey trees, object range, menu font, Rare logo
User: "looks much better".  Final boss: the DK island in the distance still drifts from its shadow on
the water.  The underwater camera jumps again (save state from Coral Capers).  Ropey Rampage: the back
trees are one colour.  Make the object range (things vanishing at the screen edge) a setting for a
playthrough test.  Smaller pause menu text.  The Rare logo's lines look bad, run slow and push the
Nintendo logo far behind the fanfare.  More optimization.
- B-GALLEON3: the island is a SNES sprite (drawn by the 32X, placed in the 32X view, camx - d), its
  shadow is BG2 (the Genesis plane, scrolled for the game camera camx): the two were d pixels apart
  times (1 - parallax factor).  The view offset d (spr_xoff 0..127) now goes to the 68000 in C_FRAME
  bits 9-15 (cframe(); the lamp levels keep their game-frame counter there) and md update_scroll()
  computes every plane's H scroll for the game camera and subtracts d (all fx: line, band and cell
  scroll tables).  Every background now lines up with the SNES picture at SNES x 0; the Galleon island
  sits on its shadow.  40-level ares sweep I20 vs I21: only parallax offsets moved.
- B-WATERCAM2: hle_cam_xmin / xmax change while swimming (the game's camera blocks); I16's narrow-block
  rule adopted a range the moment it was narrow and the left clamp used the block's start, so the view
  offset jumped 12 px at block changes (user's state: frames 46, 63, 359).  Now a narrow block is
  used only after its range stayed the same for 8 camera frames, and the left clamp uses the level's
  own start.  Replay of the logged camera (camlog) with the old / new rules: jumps 6 -> 0.
- B-ROPEYTREES: the storm palette (CODE_80AC8B: r 5/16, g 5/8, b = g) was then put through the
  level's colour math (BG2 - fixed (4, 4, 4), halved) - snes9x shows the storm's BG2 colours
  unmodified (96 % of the treetop pixels are palette colours), so the treetops fell to 1-2 Genesis
  levels: one flat teal.  genesis_planes(day={'nomath': (1, 2)}): the storm and lightning keyframes
  skip the math; the treetops now have 4-5 shades (pdprev: 2 -> 10 colours on the plane).
- B-OBJRANGE (option): pause menu OBJECT RANGE = WIDE (default) / SNES.  WIDE moves the game's
  sprite spawn / keep-alive windows (SNES: camx - 32 .. camx + 288, its view + 32 each side) to the
  32X view + 32 each side: camx - d - 32 .. camx - d + 352.  41 immediates (rompatch WS_TUNE: bank BD
  spawn windows, B6:9A5F keep-alive, BE:D5B2 "acts while on screen") read imm + r65_ws[kind] in the
  translated code; the interpreter adds the same (r65_ws_tab).  SNES = r65_ws 0 = the original game
  logic (host regression 309/309 WRAM-identical).  I15's broken barrel group came from widening the
  spawn windows but not the keep-alive check (objects killed inside the spawn window never came
  back); WIDE moves both together.  Game logic change: for the user's playthrough test.
- B-MENUFONT: the pause menu in a 5 x 7 font (6 px characters, 10 px lines; 8 items in a 136 x 102
  box, was 7 in 152 x 130).
- B-RARE: the Rare logo (NMI BC7B, mode 3: BG1 8bpp wireframe whose colour index is the drawing
  order, BG2 the logo, colour math) went through the general screen renderer: two layers and colour
  sums every frame, ~30 pictures a second, the game logic behind the real-time music.  Now its own
  effect (fescr.c rare_fx, data from build_viewer fe_rare_fx in the screen's bank): the strokes are
  drawn in the game's order (its lit palette entries) as lines in the logo's own colours with a
  white-hot point, a diagonal shine sweep fills the logo, the gold turns once through the spectrum,
  and the game's mode 7 zoom away is drawn by rare_m7 (the logo's box only, every picture).  60
  pictures a second, asynchronous swaps, colours written in the vblank.
- B-FBZERO: a BYTE write of 0 to the 32X frame buffer is not stored (ares; the overwrite rule) - the
  zoom's erase left trails.  Effect rows are written as words.
- B-FEDEBT: front-end screens catch up at most 12 logic frames per picture; the Nintendo logo's set-up
  and first picture took ~22 vblanks, so the game (and its screens) fell behind the music for good.
  The rest is now carried to the next pictures.  fe_setup's mode 7 box scan (48 K cartridge reads)
  runs only for mode 7 screens; the 68000 clears its planes with a VDP DMA fill (was a 16 K-word CPU
  loop, ~3 frames the SH-2 waited at every screen change).  Nintendo logo: ~41 -> ~6 frames after
  the SNES timing.
### I21b (2026-10-30): level backgrounds slide into place at the start
User: in every level the background shifts into place as you start walking - only the boss fight
needed the I21 change.
- B-PLANESLIDE: I21's plane scroll H = f(camx + d) - d follows the view offset d, which eases from 0
  (level start: the view stops at the level's left edge) to 32 over the first steps - so every
  parallax layer slid by d (1 - its speed) while the foreground stood still.  Now only the Galleon
  boss fight (entrance 68, md fx 6: the island sprite over its shadow on the plane) gets d (sh
  cframe / cf_view, md update_scroll dd = 0 unless fx 6); every other level scrolls exactly as in I20.
### I21c (2026-10-30): Galleon scroll, Ropey tree brightness, Manic Mincers room, Misty mist, STATS
User: Galleon - the island stays on its shadow but the whole background shifts / auto-scrolls at the
playfield's edges; Ropey's back trees as bright as the foreground trees; a Manic Mincers bonus room
(jump on the Klaptraps) with no graphics; Misty Mine's mist bands jump back and forth every other
frame; wants the frame rate / SH-2 / 68000 load readout back as a pause menu option, small, in a corner.
- B-GALLEON4: the I21 / I21b plane scroll used the live view offset d (0..64), which changes at the
  playfield's edges - the whole sea and sky slid.  Now a constant 32 (md update_scroll(.., 32) for fx 6:
  the planes depend only on the view camera) and the island SPRITE is drawn 32 px right of the SNES
  picture instead of d (sprites.c spr_fixed_x: OBJ tiles 140-14F, palette 6, priority 2, Galleon boss
  only) - island and shadow band both fixed on screen, as on the SNES.
- B-ROPEYTREES2: I21's "no colour math" for the storm was wrong - the snes9x check had sampled BG1
  (foreground) palms; the back trees ARE darkened by the math (0-57 of 255, user's SNES shot).  Now the
  math again, then a 1.5x lift (genesis_planes day['lift']): 3 dark shades (0 / 52 / 87) instead of I20's
  single one, darker than the foreground.
- B-MMBONUS: entrance 05 (Manic Mincers' Klaptrap room) cannot be started from the map: the game went
  back to Jungle Hijinxs, so both the builder's capture and the research survey (levels.json layout /
  graphics recipe) were of the wrong scene - the room showed its sprites on black.  capture.py
  JUMP_ROOMS gets 05 (via its level, the game's NMI-pointer path; jump rooms captured the old way by
  an earlier builder are taken again: jump.ok), levels.json entry 05 regenerated alone from a new
  survey.  All 76 bonus-room entrances checked with the map start (snes9x, $3E after 300 frames):
  only 05, 53, B8, C3 do not start - the last three were already jump rooms.
- B-MISTWAVE: the fog bands took the top band's pixel parity (one global 1-px shift) plus a 1-px
  nudge for the checkerboard; the top band drifts a pixel every other frame, so every band jumped a
  pixel and back with it.  Now each band's column and row offset is rounded to an even value on its
  own: the checkerboard keeps one phase and a band moves only when its own wave crosses 2 px.
- STATS (pause menu, off by default): "F60 M95 S20 G24" at the bottom left in the 8 x 12 HUD font -
  pictures a second, master / slave SH-2 load, 68000 load.  The 68000 measures its V interrupt and idle
  work (VDP V counter, + an estimate per H interrupt) per 60 frames and sends the % 1 bit a vblank in
  C_PAD bits 15 / 14 (the pad sample number is now 6 bits).

### I21d (2026-10-30): Manic Mincers sprite colours, Misty Mine mist
User: after leaving Manic Mincers' Klaptrap room, a tree-stump platform in completely wrong colours
(and wrong colours in a TNT barrel's explosion), OBJECT RANGE WIDE; Misty Mine's mist still tears /
bounces between bands - "use the exact same misty foreground from Poison Pond ... just the auto-scroll".
- B-PALOVF: the screenshot's stump AND the bananas above it were drawn in the Blue Krusha's colours
  (matched against every sprite palette in the ROM: BC:89C2).  Cause, in the game itself: the SNES has 8
  sprite palettes; the allocator (CODE_BDF1F4) with all 8 in use returns palette 0 WITHOUT counting the
  new user (CODE_BDF22B, the debug "out of palettes" marker), but that object releases palette 0 when it
  goes.  Two such objects and palette 0 (the bananas') was freed while the bananas still used it; the next
  palette loaded (the Krusha's) went there and recoloured the bananas and every object that had been
  given palette 0.  On the SNES's 256-px windows Rare never runs out; WIDE keeps more objects (and
  palettes) alive, and the Klaptrap room's palettes are kept over the room change for its fade.
  Fixes: (1) rompatch PALFIX - the overflow now counts its user (INC $1AE3 in place of the debug
  display's LDA $8C / STA $1E35; r65.py learns the new instruction boundaries via rompatch.dis_fix), so
  palette 0 can no longer be freed under its users; (2) main.c: with 7 of the 8 palettes in use the
  OBJECT RANGE windows go back to the SNES ones until no more than 5 are in use (at mid-level the SNES
  windows are exactly the 32X's 320 columns).  Host check: 8 palettes forced full in the sim - the
  overflowed barrel now holds a count on palette 0 (2 -> 4 instead of 3).  reg.py: 309/309 coverage jobs
  WRAM-identical to I21 (the patch only acts when the palettes run out).
- B-MISTWAVE2: the game's Misty fog is one offset for the whole layer now, as Poison Pond's: the SNES's
  scroll rates (x 1.25, y 1) plus its wave's steady drift (half a pixel a frame, down-right); the
  per-band wave and its sway are gone, so no band can tear or bounce against another.  (x + y) kept
  even as in Poison Pond's layer (the checkerboard keeps its screen phase).  No fade-in: full density
  from the start (its palette steps forced a synchronous flip every 32 frames for 16 s).

### I21e (2026-10-31): sprite tiles, foreground in front of sprites, mist wave
User: the colours and the mist look great; wants a mist wave that leaves the checkerboard alone; Loopy
Lights' hanging lamps (light cones) missing and low-colour backgrounds; Tanked Up Trouble's background
dithered and without the lamp's brightness changes; sprites in front of the foreground (screenshot:
Manic Mincers' black trunk); persistent one-tile sprite glitches - a swinging rope (until swung on), the
Rambi crate, Necky's face before / after spitting a nut.
- B-STRADDLE (the tile glitches, "once and for all"): an OBJ tile is read as 32 bytes from the address of
  its first byte.  In the all-worlds ROM a 1 KB ROM page sits either in its world's bank (window 6) or in
  the resident blob, so for a tile that crosses a page boundary between the two the last bytes came from
  an unrelated page: one corrupt tile in every sprite frame whose graphics cross such a boundary.  Found
  by logging every OBJ DMA source over all 40 levels in the host sim and resolving each crossing tile
  through the built ROM's own page tables (devkit/scripts/tilechk.py): 230 wrong tiles, e.g. the Rambi
  crate (GFX_AnimalBuddyBox_Rambi, tile 6), the swinging rope's rest pose (GFX_SwingingRope_Pose2:
  page 580 resident, 581 in the world banks), Very Gnawty, Klump, Kritter, Necky, Master Necky, Slippa,
  Rockkroc frames.  The host sim could never show it (its blob holds every page in order).  Fix:
  mkgame.py keeps a resident 64-byte copy of the bytes around each such boundary inside / at the edge of
  sprite graphics (126 boundaries, 8 KB), sprites.c vram_dma points crossing tiles into it
  (straddle_ptr).  After: 0 wrong tiles but the debug font's (never shown).
- B-FGFRONT: with FOREGROUND ON the canvas drew the cave foreground overlay BEFORE the sprites, so
  every sprite covered it.  Now (ov_front) after each sprite band the overlay is drawn again in the
  canvas cells the sprites touched (the overwrite region keeps its transparent pixels): the caves'
  BG3 foreground (mode 1 BG3 priority) is in front of every sprite as on the SNES; in the factory levels
  (BG1 foreground, x 2 scroll) the OBJ priority 3 sprites (the Kongs) are drawn once more on top.
  FOREGROUND OFF leaves these layers out altogether (performance mode, as before).
- Misty Mine: the wave is back as horizontal 2-pixel steps per 4-line band (the SNES's sway, one band
  per frame down the screen): even columns keep the checkerboard's phase on every line, rows never
  move.  RIPPLE OFF in the pause menu = the flat mist.
- Not done (NOTES I21e): Loopy Lights' light cones - BG1 covers the whole screen there, so the
  cone (the SNES subtracts 3 outside it) would have to be drawn by the 32X, whose 127 BG colours have no
  room for a second, brighter set; a Genesis version (BG2, S/H) was built and dropped - BG2 is not
  visible behind BG1.  Background colours (Loopy, Tanked Up Trouble): the Genesis's darkest levels are
  0 / 29 / 52 of 255 even with shadow, DKC's cave walls sit below that - flat (wrong hues) or dithered;
  not a memory / CPU limit.

### I21f (2026-10-31): lamp-level walls by flicker, PicoDrive boot freeze, millstone snapshot
User report (I21e build): no tile glitches.  Remaining: Winky's Walkway type levels' background still
"ugly repeating dithering tiles that aren't checkerboard-only" - try the flickering trick, a bit of
dithering only by the checkerboard rule; Millstone Mayhem "sprite priorities are all out of order still"
(screenshot: background cells over the millstone ring).
- B-LAMPFLICK (Winky's Walkway D9, Tanked Up Trouble 30, Platform Perils 2B): the I11 / I18 Bayer 4x4
  dots are gone.  The 68000 now swaps CRAM lines 0-2 between two tables every vblank (md flag 256,
  CRAM B after the extras).  A wall colour between two Genesis levels gets two CRAM entries, one per
  checkerboard parity, each a pair (frame A value, frame B value); per channel 4 samples (2 pixels x
  2 frames) take the level above in the order (even, A), (odd, B), (odd, A), (even, B): half way =
  a checkerboard that inverts every frame (constant brightness), 1/4 and 3/4 = one half alternates.
  Every frame is a plain 1-px checkerboard; on a CRT / at 60 Hz the eye sees the colour in between -
  9 shades between the shadow levels 0 / 29 / 52 instead of 3 (the wall's browns: 8,8,0 / 16,8,0 /
  25,16,0 / 33,16,0 of 255 were olive / black patches).  Palette lines: cells grouped by SNES palette
  and shadow / normal, a group's spare lines filled most-used colours first (build_viewer fl_alloc):
  D9 uses 13 + 15 entries.  LAMP_FLICK=0 (builder env) = the old dots; FL_QUARTER=0 = halves only.
  The lamp cones (line 3) are untouched.  Screenshots catch one frame: they show the checkerboard.
- B-PDBOOT (found while testing the above): the new 68000 binary hung PicoDrive at the Nintendo
  screen (STUCK page, M STAGE 3, master waiting for the slave's first job).  pd_dbg instrumented: the
  slave's first run after its boot wait is a catch-up of ~437 k cycles; music_init started the PWM
  timer (an interrupt every sample, slave PWM mask bit on - PicoDrive needs it to clock DREQ1) inside
  it, and every PWM interrupt went PicoDrive's "nested" path (p32x_update_irls -> sh2_end_run) that
  shortens the slice using the SR in memory - stale in a recompiled run (start-of-run cycle count,
  I = 2): the slice went to ~ -55 M cycles, the slave stood still for minutes.  Which builds hit it
  depends on the boot timing (the 68000 binary's size): 1 of 50-105 tested 68000 sizes with the I21e
  code, so any earlier build could have had it.  Fix (sh/main.c s_main, music.c): the music starts
  with the first song request or job (the slave stays awake for the request: no COMM poll sleep), and
  I = 6 before the PWM timer runs.  After: 0 of 105 boot timings; the intro music starts as before
  (audio dump: from 3 s).  The SH-2 interpreter and ares never showed it.
- B-MSSNAP (Millstone Mayhem / Temple Tempest): the millstone (SNES BG2, high priority) was drawn
  from spr_ms_on / x0 / y0 / k, which the master rewrites in every logic frame - in 60 FPS MODE while
  the slave still draws the previous picture (position, rotation tilemap and opaque masks of two
  different frames; restores computed for one, picture drawn at another).  Now the master stores each
  picture's position / tilemap per frame buffer (spr_ms_cq[bi]) and both the picture and its restores
  use that.  Layer order checked against snes9x (VRAM dump at a millstone frame: BG2 tiles all high
  priority, BG1 all low, the pot and pillars are BG1): BG1 < millstone < OBJ priority 2 / 3, OBJ 0 / 1
  behind it - the 32X draws the same in PicoDrive and ares (MiSTer model, 22 fps, frame skip; 60 FPS
  MODE): the pot passes behind the stone, the Gnawty and the Kongs in front.  The reported cells over
  the ring were not reproduced in those runs (NOTES I21f).

### I21g (2026-10-31): lamp flicker steady at any frame rate, cone lines, Mine Cart Madness tracks
User report (I21f build): Winky's Walkway's flicker "works well in theory" but becomes a flashing mess
whenever frames drop (60 FPS MODE a bit better); the lamp light's dither makes dark vertical bars flash
over the flickering wall; Mine Cart Madness's tracks completely black; the intro's platform -> jungle
cross-fade crawls and barely fades (answered: NOTES I21g).
- B-FLICKSTALL: the 68000 did swap the CRAM every vblank, but the wall's checkerboard lives in plane
  B's coordinates: a plane scroll by an odd number of pixels swaps its halves on screen.  With the
  plane moving 1 px at irregular moments (the parallax plane at half speed, the 32X's picture rate) a
  pixel's colour stayed for 2 vblanks now and then - 15 Hz patches, worse with frame drops.  Now the
  table shown = vblank count XOR the parity of plane B's H + V scroll for that frame (md_vint reads the
  camera once and works the parity out with update_scroll's own formula): every screen pixel
  alternates every vblank.  Measured (PicoDrive, bot running, 40 consecutive frames, the checker's
  screen phase): before "+-+-----+++---++++----++--+++--++++++++-", after strictly "+-+-+-...".
- B-CONEBARS: the Genesis cone was a checkerboard of light dots flipped every vblank (1-line V scroll)
  - at the same rate as the wall, so its gaps always showed the same wall phase; with the swing's
  per-line skew a different one per line band: bars that came and went.  Now (lamp levels with the
  flicker) the cone is every other LINE, flipped every vblank: whole lines hide both checkerboard
  halves alike, the gaps show the wall's real mix.  (A static checkerboard cone would still bias the
  1/4 - 3/4 mixes; the halves-only wall was tried - flat olive, the browns gone.)
- B-MCMTRACKS (Mine Cart Madness): the walkway levels' rule "palette 0 colours 1-15 = the lamp light,
  black away from lamps" (build_viewer runtime_palette) also blacked out Mine Cart Madness's palette 0
  - its BG1 mine cart tracks.  That level has no hanging lamps (snes9x: palette 0 = DATA_B9A2DE in
  every frame): it keeps those colours now.

