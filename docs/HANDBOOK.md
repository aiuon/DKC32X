# DKC 32X - project handbook (consolidated knowledge, 2026-10-25)

This is the one-file summary of everything needed to keep working on the port: goals, rules,
architecture, memory maps, tools, pitfalls and the open list. NOTES.md (design / measurement
history) and BUGLOG.md (one entry per bug, newest last) hold the details; this file points into
them. Keep this file current when something structural changes.

---------------------------------------------------------------------------------------------------
## 1. Goals and rules

- Donkey Kong Country (SNES, USA 1.0) on the Sega 32X: the game's own 65816 code statically
  recompiled to SH-2 C, graphics / levels / music converted for 32X + Mega Drive hardware.
- Targets: MiSTer (S32X core), ares, PicoDrive. MiSTer is the strictest and slowest.
- User goals: 60 fps; controls and physics identical to the SNES (no added input lag); the complete
  game; "bulletproof with no glitches"; memory handling 100 % solid.
- LEGAL (user, verbatim): "I only want the 32x rom for myself to try on my 32x and emulators. If there
  ever comes a point where I'd consider releasing it for others, it should only ship as a build tool
  and avoid any legal trouble." / "I want to keep this project nice & legal."
  * Deliver ONLY the builder bundle (sources, scripts, toolchain, emulator core for capture). Never a
    built .32x / .elf, never captured data (snapshots, surveys, lamp.npz, weather.npz), never code
    generated from the game (build/g65, sh/spceng_gen.c, sh/comp.s).
  * Scan every bundle: `python3 tools/scanbundle.py /opt/work/dkc.sfc DIR` (no >= 32-byte ROM run in
    any file). Known harmless hit: python's unicodedata.pyd (53 B coincidence).
  * devkit/ (this repo's dev scripts + emulator patches) is not part of the bundle.
- Keep BUGLOG.md (every user-reported bug: symptom, cause, fix, verification) and NOTES.md current.

---------------------------------------------------------------------------------------------------
## 2. Hardware model (what bites)

- Two SH-2s at 23 MHz (master / slave), 4 KB cache each (write-through). Cartridge ROM fetches are
  slow line fills (~60 cycles on ares-M / MiSTer): CODE SIZE and ROM data reads dominate. SDRAM is
  faster but 256 KB total. The 68000 (7.6 MHz) drives the Mega Drive VDP (planes A / B, sprites off).
- MiSTer model in ares: `ARES_SDFILL=30 ARES_UNCSD=11` + `--setting Developer/ForceInterpreter=true`.
  ares's recompiler hides instruction-fetch cost - always measure with the interpreter.
- SH-2 16/32-bit accesses MUST be aligned. PicoDrive / MiSTer silently read the aligned word, ares
  raises an address error; the host sim tolerates it (hid the I14 rope bug). WLW / WLW_SET (gbr-relative
  mov.w) are only valid for EVEN constant addresses. Use byte access for computed addresses.
  Check: ares `ARES_MISALIGN=file` logs every misaligned SH-2 access (PC, address, size, write).
- 32X VDP frame buffer writes go through a FIFO; MiSTer swaps buffers at once in vblank, so drain the
  FIFO before the flip (wait_flip does). Late writes land in the wrong buffer.
- SRAM (battery saves): 68000 only (A130F1 = 1, A15104 = 2, byte n at $900001 + 2n). While A130F1 = 1
  MiSTer maps SRAM over the whole $200000-$3FFFFF: both SH-2s must be off cart ROM. Never write
  A130F9 (PicoDrive loses SRAM).
- SSF2 mapper: windows 6 / 7 (A130FD / A130FF) banked; ROM <= 15 MB (EverDrive Pro limit).
- PAL: region-free ROM; SH-2 detects PAL from VDP reg 0x4100 bit 15 and runs 60 game frames / s on
  50 pictures (gtick extra every 5th vblank).

---------------------------------------------------------------------------------------------------
## 3. Architecture

### 3.1 Game logic (recompiled 65816)
- tools/recomp/r65.py: trace-guided + static discovery, one C function per routine entry (key =
  mode << 24 | pc24). Untranslated keys run in r65interp.c. JSR/JSL push real return bytes; WAI /
  error / jump keys as in NOTES "M3 gameplay / Pipeline".
- data/translate.txt: extra entry keys to translate (interpreted keys found with sim_fc XC_PROF,
  exclusive counts; procedure: NOTES "I16"). Check new keys for BADJUMP in their generated function
  (devkit/cov/badjump.py). data/entries.txt: entries no trace reached (pointer tables) - an
  interpreted key with no owner in build/g65/owner.txt must be added there before it can be
  translated. The interpreter hands off at JML / JMP (ind) / JML [ind] (JUMPKEY): a translated
  target always runs native (-DR65_NO_JMLHANDOFF = the old JML for A/B tests). data/hle.txt: HLE replacements
  (sh/game_hle.c). data/hot.txt: -O2 functions (rest -Os). data/sdram.txt: functions placed in SDRAM
  (.sdgame section, sdpick3.py, budget 67500 bytes).
- Every translation / HLE must be WRAM byte-exact per frame vs the reference sim (see 5.2).
- Level start = snes9x snapshot (personal-use capture from the user's ROM at build time).
- WRAM: wram_lo (0-1FFF, SDRAM), high WRAM via wram_hp[512] page table: class 1 gameplay pages ->
  wram_pool (SDRAM); class 8 load-only scratch pages ALIAS THE MASTER TILE CACHE (only usable while
  the screen is dark). A wrongly classed page = garbage tiles / colours (see NOTES "ROOT CAUSE").
  Re-profile (sim WHSTAT, SCRATCH check) after adding content.
- SNES ROM reads: snes_pg[4096] 1 KB pages; resident pages + window-6 world bank pages. A missing page
  reads zeros (-> e.g. size-0 DMA = 64 KB of garbage). sim ROMB_CHECK / WORLDBANK find them.

### 3.2 Rendering
- 32X layer (8bpp frame buffer) = BG1 (+ sprites drawn by the SH-2s over it); Genesis plane A / B =
  BG2 / BG3 (shadow mode for SNES colour math). Palette = SNES CGRAM (BGR555) with priority bit.
- Per level a "render mode" (cv): 0 classic tile rows, 1 fused canvas (most levels), 2 / 3 fog
  canvases (Misty / Poison Pond transparency), Torchlight beam (bm_*), rain (Ropey), snow weather,
  lamps (walkway levels), ripple (water levels, BG1 per-line shift + Genesis H-scroll), overlays.
- Lamp levels (I18): S/H on; BG2 (wall) shadowed and dithered between the shadow levels (matrix =
  Bayer with rows 2-3 rotated: both checkerboard halves get every other threshold); lantern cells
  (colours above the shadow range) high priority, normal levels; the cone = plane A 50 % dots at the
  shadow levels, the other half every vblank by a 1-line V scroll (68000 lc_ph).
- Sprites: OAM from WRAM, obj_tile[512] -> 4bpp source tiles, chunky copy (spr_ck) in map_mem lower
  16 KB, drawn by both CPUs in 16-line bands, restore marks for the next picture.  Without spr_ck the
  renderers read obj_tile_dr (I18: a copy made in spr_prepare_b - the 60FPS MODE draws picture N while
  logic N+1 re-points obj_tile).
- Front-end screens (logos, title, file select, map overlays, cabins, ending, credits, game over):
  sh/fescr.c line renderer from captured VRAM + live VRAM mirror (fe_vm, lent tile cache).
- Map screens: captured pictures + live sprites (map_frame), cabins with cab_text.
- 68000 (md/main.c): plane loads (cmd 0x8000 | idx; 0x4000 = variant; 0x2000 = no BG1 ripple),
  0x7FFF clear planes, 0x9D00 display off + backdrop black (before every level load), 0x9E00 | i
  front-end plane, 0x9F01 / 02 SRAM save / load, 0xA0xx / 0xA1xx bank switches. disp_hold keeps the
  Genesis display off until the SH-2's 2nd picture of a level (SH disp_pending = 2). BG3 haze lines
  double-buffered (bg3buf[2][224], swapped in vint).

### 3.3 CPU job protocol
- Master runs logic (game_frame), BG work and its sprite share, sky H-int.  Flips (I17 FLIP_ASYNC):
  gameplay pictures with nothing for the swap's vblank but the shift register request the swap
  during the display and the master goes on with the next logic; nothing draws until the swap is
  seen (fb_sync after the logic, in wait_flip / present / load_level; the slave's jobs 3 and 5 poll
  FBCTL themselves).  Pictures with palette / sky / ripple / first-picture work stay synchronous.
  Never touch the frame buffer while a swap is pending (ares drops the access, MiSTer writes into
  the picture about to be shown).
- Pad (I17): the 68000 writes COMM8 = sample number << 8 | pad every vblank; the master queues the
  samples (V interrupt + pad_take) and every logic frame takes its own (frame skip runs 2-3 logic
  frames back to back: before, they shared one register value and one-vblank presses were lost).
- Slave: rendering jobs + music (SPC700 + DSP emulation, PWM out; ~28-30 % of the slave on MiSTer).
- Jobs: master writes job, COMM(C_TOKEN = 12) = ++job_token; slave answers COMM(C_DONE = 14) = token.
  ONE counter (main.c job_token, `#define token job_token`, fescr.c uses ++job_token too). Two
  counters = a wait for a token nobody answers (I13 STUCK).
- set_bank / set_bank6 wait for an idle slave, purge caches, bump the epoch.

### 3.4 Game flow
- Power-on: the complete game (power-on snapshot 100.wai) -> logos, intro, title, file select, map,
  levels, ending, credits. Hold START at power-on = level-viewer menu. -DFULLGAME / -DVIEWERMENU.
- Music follows the song the GAME uploads (hle_apu_upload). The viewer menu's LEVEL_MUSIC table only
  covers menu levels (builder prints one note for rooms without a menu song - harmless).
- Pause menu (game paused, $0579 bit 6): RESUME / FRAME SKIP / 60FPS MODE / FOREGROUND / EXIT TO MAP
  / RIPPLE / LEVEL SELECT. Start opening the pause must be released before Start closes it (pm_hold).
  A + Start = viewer shortcut only in viewer builds.

---------------------------------------------------------------------------------------------------
## 4. Memory and ROM maps

### 4.1 SDRAM (256 KB, 0x06000000)
- .data, then .sdgame (copied by m_main uncached, read back + compared, up to 4 rounds,
  sdgame_fixes), then .bss up to 0x0603E800 (__end ~0x0603BD60, check the link map).
- Master stack 0x0603E800-0x0603F7F0 (4 KB, canaries 16 B at each end, measured peak 1916 B).
- Slave stack 0x0603F800-0x06040000 (2 KB, measured peak 1140 B).
- STACKCHK builds paint both stacks (0xA5A5A5A5 master / 0x5A5A5A5A slave); read with PicoDrive
  pd_dbg PD_SDDUMP (SDRAM byte-swapped per 16-bit word).

### 4.2 map_mem (32 KB shared scratch; owner depends on the screen)
- Game levels: lower 16 KB = sprite chunky copy (spr_ck).
- Torchlight (upper half): BMS span list 17408, BM_ST 20480, BM_LN 21376, bm_dmap 24576, bm_lines
  24704, bm_slotcol 25600, BM_TMP 25664, BMH [CV_H][6] 27840, bm_shown [224] 30912 (-> 31808, I14).
- Front-end screens: FE_* tables (NOTES I11 / I12 lists), overlap the level tables - front-end only,
  rebuilt by load_level.

### 4.3 ROM
- First 256 KB: 68000 code, SH-2 boot + code, .data image, .sdgame image (finalize.py checks; ~3.9 KB
  free).
- Pack at 0x40000 (music, level data, front-end tables, palettes); game code at GAME_BASE; resident
  SNES pages. Resident end must stay <= 0x300000 (0x2FFD50 after I16 and I18 - FULL, 688 bytes left;
  SDRAM-placed code does not use resident ROM; a 1 KB page more made the I18 build fail).
- DUP pages (I16): resident sprite pages of common characters copied into EVERY world bank at the
  same offset (first after the table) - always visible at one address like resident pages; as many
  as the fullest world bank allows (mkgame.py world_split, WORLD_DUP=0 turns it off).
- 0x300000 window 6 = world bank (snes_pg table at 0x02300000 + the world's pages); window 7 = level
  banks (best-fit by tile-set group). ROM 14 MB (14680064 bytes, I18).

### 4.4 32X palette conventions
- CRAM 0 = sky (per-line H-int), 1-127 BG, 128-255 sprites; 253-255 = SNES sprite palette 7
  (don't use them for HUD / text: Funky's board went black when cab text wrote 254 / 255, I14).
- Rain colours: CRAM 16 / 32 / 48 in game levels. Pause menu text CRAM 96 / 112. Cabin text uses the
  brightest / darkest existing colours (cab_colours).

---------------------------------------------------------------------------------------------------
## 5. Tools

### 5.1 Build
- User path: `python3 tools/convert.py ROM` (romcheck -> capture -> build). Windows bundle runs the same
  via "Convert (drag ROM here).bat" / "DKC32X Builder.exe".
- Dev: `DKC_SURVEY=work/survey DKC_PREBUILT_MD=1 python3 tools/build.py work/rom.sfc OUT.32x --snaps
  work/snaps --extra-shflags="-D..."`. Never run two builds at once (shared build/ tree).
  /opt/work/allb.sh OUT "flags" = same, used by bench.sh.
- tools/prebuilt/md.bin = the 68000 side shipped in the bundle: rebuild + copy after md/main.c changes.
- tools/levels.json = hand-maintained survey/recipe table. Do NOT regenerate wholesale with
  make_levels_json (changes ~90 entries); splice single entries. Backups in /tmp/claude-0/*.json.
- tools/capture.py: every capture from the ROM (snapshots, surveys, lamp / weather). Rooms the map
  cannot start: JUMP_ROOMS (parent level 400 frames, then poke $3E/$40/$2E/$565 = room, $1C = 0x806F
  for one frame = the game's NMI pointer path).

### 5.2 Verification
- sim3 (/opt/work/sim3, sources copied to devkit/sim3): host build of the 32X runtime. mkva.sh ->
  simva (native, VRAM shadow), mkvb.sh -> simvb (everything interpreted), simo = the I12 sources' sim
  (reference). Rebuild after every build (stale g65 objects -> wrong results).
- devkit/scripts/verify2.sh "ENTS" FRAMES REF: WRAM byte-exact per frame, pad script + teleport inputs.
  Standard set: 14 16 0C 01 BF 17 E0 D9 2E 07 31 42 24 6D A7 43 (3000 frames). Rooms without a
  snapshot (C1 C3 53 ...) are not testable this way.
- sim_xcp (XC_PROF): interpreted instruction counts per key -> candidates for translate.txt.
- regress.sh (picture tour), xsweep.py (every entrance alias: page violations), roomtest.sh.
- ares: ARES_PADSCRIPT (harness-style "run N BUTTONS"), ARES_DUMP / _FROM / _TO / _EVERY / _HALF (PPM
  frames), ARES_QUIT_AT, ARES_MISALIGN, ARES_SHPROF_EVERY (profile). ares_run.sh ROM PAD DIR FROM TO
  EVERY QUIT [mister]. tr/ana.py, cuts.py, blk.py (flashes / cuts / black transitions in a dump).
- bench.sh TAG "IDX..." = GAMEDEMO + GAMEPROF + NO_FSKIP builds, MiSTer-model ares, HUD crop
  (row 1: F fps, M master %, S slave %; row 2: G logic, B BG strips, L, S sprites, T master total, %
  of a frame, game frames 60-359). Baseline I13: /tmp/claude-0/bench/fin2_*.png.
- PicoDrive libretro + /opt/harness/harness (script: run / shot / dump / state / load / loadmem).
  pd_dbg core: PD_SH2LOG (PC histograms), PD_SDDUMP.
- STACKCHK, POISONMAP (junk in map_mem at every screen setup), GAMEBOT (bot plays from power-on).
- Test macros in main.c: AUTOLEVEL=n, GAMEDEMO, GAMEPROF, PROFT + PICPROF (per-picture phases), RIP_DEFAULT,
  DBG_PAD=f0,f1,bits,..., DBG_TP=start,every,dx[,end],
  DBG_POKE / DBG_POKER, DBG_GOTO, MAPDBG, ENDTEST, BOSSTEST, GOTEST, FEDBG, PATHRING, DBGRING, NO_BMRATE.
- Patches for the instrumented emulators: devkit/patches/ares.patch, snes9x_trace.patch.

### 5.3 Delivery checklist
1. Full regression: verify2 set, regress tour, ares transition run (dumps, no flashes), GAMEBOT run,
   STACKCHK, ARES_MISALIGN scan.
2. Bundle: copy the previous bundle, refresh app/sh, app/md, app/tools with mkbundle.copytree (drop
   tools/win, __pycache__), HANDBOOK / BUGLOG / NOTES.
3. scanbundle; Wine run of the .bat (WINEPREFIX=/tmp/claude-0/wineprefix, under `script`) must give a
   ROM byte-identical to the Linux convert.py run (DKC_HARNESS=/tmp/claude-0/harness_linux
   DKC_SNES9X_TRACE=/opt/ref/snes9x_trace/libretro/snes9x_libretro.so DKC_PREBUILT_MD=1). Afterwards
   restore build/lamp.npz and build/weather.npz (the auto capture overwrites them).
4. Zip -> scratchpad DKC32X_Builder.zip, send, short summary per reported bug.

---------------------------------------------------------------------------------------------------
## 6. Pitfalls learned the hard way (each one was a shipped bug)

- Unaligned 16/32-bit SH-2 access (ropes' upper half invisible, I14).
- Two job token counters (STUCK at the first level, I13).
- Load-only WRAM pages aliasing the tile cache used in a level (scrambled sprites / colours).
- Missing ROM pages read as zero (size-0 DMA = 64 KB garbage).
- Stack overflow into .bss (2 KB master stack had 100 B left; now 4 KB with canaries).
- Lent memory (map_mem / tc_sdata) is not zeroed: clear state at setup (host sim zeroes statics).
- Frame buffer FIFO not drained before the flip (late writes in the wrong buffer on MiSTer).
- Palette / sky set before the picture is drawn (old picture flashes in new colours).
- `if (x) LDST2(n); while (...)` - a test macro in front of a loop makes the `if` cover only the
  macro: the wait ran in every mode (I17: the 60FPS MODE never pipelined).  Braces around such pairs.
- Per-line tables the interrupts read (sky_ram) must change at the swap, not while the old picture
  is still on screen (I17 sky_next).
- The reset button restarts only the 68000; the SH-2s get a VRES interrupt and must restart
  themselves (I17 start.s m_vres / s_vres) - ares and MiSTer never raise it, test in PicoDrive
  (harness with retro_reset).
- HLE functions must not use r65_mx (stale inside generated code).
- Sprite palette 7 (CRAM 253-255) is game data, not free HUD colours.
- ROM patches must never touch game logic (I15): the widescreen patch had widened the sprite spawn /
  keep-alive windows - the game diverged from the SNES and Barrel Cannon Canyon lost a barrel.  Any
  patch to the game's code: A/B it in snes9x (same inputs, original vs patched ROM, compare the sprite
  tables; /tmp/claude-0/bcc/ab2.sh).  Display-only patches: OAM builder culls, item drawing, particles.
- Pause / menu inputs: the Start that opened a menu must be released before it can close it.
- Pause menu frozen loop must still see go_menu / level select.
- Builder: never regenerate levels.json wholesale; two builds at once corrupt build/.
- Direct-mapped tile caches: two versions of a tile (Torchlight dark / lit) fetched alternately per
  line evict each other - fetch per tile, copy, then draw all its lines (I14 turn stutter).
- Interpreted 65816 code is ~600 cycles an instruction on MiSTer: profile with IKEYPROF / XC_PROF.
  Before I16 some bonus rooms ran ~975 interpreted instructions a frame (> a whole MiSTer frame).
- pkill -f PATTERN also kills the shell whose command line contains PATTERN (use pgrep + kill PID).
- Free-movement tests ($0535 = 1) run the game's debug code (cursor sprite, 80A1C0 / BFC63E): their
  profiles are not gameplay profiles.
- A 68000 test written for N keyframes breaks when a level gains a key (Ropey's lightning = 3 keys hid
  the treeline): use explicit level-kind flags.
- (I18) Anything per-frame on the Genesis side that must alternate (checkerboards, dither phases)
  goes by the vblank, never by the game frame: with frame skip the game frame's parity can stay the
  same for seconds (the lamp cones' dark tiles).
- (I18) An ordered dither under a 50 % checkerboard overlay: Bayer's lower half of the thresholds
  sits on one checkerboard half - the overlay hides exactly the "above" pixels.
- (I18) Data shared by both CPUs and written only at a level load can be read cached if each CPU
  purges once per load (spr_ms_gen / spr_ms_sync); uncached reads in an inner loop cost ~11 cycles
  each on MiSTer (the millstone LUT).
- (I18b) Any 68000 <-> SH-2 handshake must be able to give up on BOTH sides: the 68000 waited
  forever (interrupts off) in the SRAM transfer after the SH-2 timed out - every later command hung.
  On MiSTer, cartridge SRAM enabled = the whole cart range 0x200000-0x3FFFFF is SRAM for all CPUs.
  The game writes its SRAM during play without changing it: saves go by content (hash), not writes.
- (I18d) 68000 code that sets RV = 1 must run from RAM (the cart moves to $000000 for the 68000; ares
  models it, PicoDrive does not).  The SSF2 mapper registers are written with RV = 1 (md mapper_w).
- (I18) The 60FPS MODE overlaps the slave's picture N with the master's logic N+1: everything the
  slave reads for N must be a copy taken before N+1 starts (spr_list, spr_ck, obj_tile_dr).
- (I19) Interpreted code that runs only during screen-off frames (level loads: the lighting /
  palette table builders) was invisible to the I16 coverage, which counted frames with a picture
  (xg) - 0.3 - 2.3 s of MiSTer time per level start.  IKEYPROF on the real ROM counts every frame.
- (I19) Room for translated code: the resident game region ends at 3 MB and GAME_BASE follows the
  pack, so shrinking SH-2 code does not move it.  The first 256 KB has its own room (~38 KB after
  I19): put functions there with data/lowrom.txt (LOWROM_FN).  Each translated key also adds a
  16-byte dispatch entry to the resident region (~0.4 KB free there now).
- (I20) Genesis S/H: a pixel is shadowed only if both planes' cells there are low priority (even
  transparent ones), and a high-priority plane B cell is drawn over a low plane A cell: per-cell
  shadow works for one plane, or for two when plane B is shadowed throughout.
- (I20) The harness's load / state file names are relative to its output folder.
- (I19) Asynchronous swaps: anything per-line the interrupts read (the ripple shift table) must
  switch in the V interrupt of the swap's vblank (af_sft next to af_shift), and the next picture
  may rewrite the old table only after its fb_sync.

---------------------------------------------------------------------------------------------------
- A BYTE write of 0 to the 32X frame buffer is not stored (ares, as the overwrite area's rule): erase
  with word / long writes (I21 B-FBZERO).
- The synchronous present() waits for the vblank after the next one (the swap request lands in the
  vblank it waited for): any screen that needs 60 pictures a second presents asynchronously
  (present2(..., 1), palettes written after waiting for the vblank - fescr.c rfx_show).
- Rooms the map cannot start (05, 53, B8, C3) need capture.py JUMP_ROOMS AND a research survey taken
  the same way - check new rooms' $3E after the map start (I21c B-MMBONUS).
- In I21 tests an ARES_PADSCRIPT Start did not open the pause menu in PERFREC builds (cause not
  looked into); check menus in a plain build or offline (a mock-up from the font table).

## 7. Performance picture (MiSTer model, I13 -> I14)

- I19 (devkit/perf/perf.py, frame skip on, fps; I18d -> I19): JH 36.9 -> 42.0, Reptile Rumble
  38.1 -> 41.6, Barrel Cannon Canyon 46.2 -> 48.1, Winky's Walkway 35.5 -> 40.1, Stop & Go 32.5 -> 34.8,
  Millstone Mayhem 34.9 -> 45.4, Tree Top Town 46.6 -> 49.7, Forest Frenzy 53.3 -> 55.0, Croctopus
  Chase 20.0 -> 34.7, Mine Cart Madness 49.8 -> 52.3; 60FPS MODE JH 38.1 -> 41.9, BCC 45.5 -> 48.7,
  TTT 48.2 -> 49.8, MCM 48.8 -> 52.5 (game speed 59.7-60.1).  From: late pictures swapped
  asynchronously, ripple levels on the async swap, interpreted code translated.  Level loads: the
  interpreted lighting tables took 0.3 - 2.3 s of MiSTer time per level start (Orang-utan Gang ~2.3 s,
  Oil Drum Alley / Blackout Basement ~1.3 s, Stop & Go ~1.5 s, the snow levels ~1.2 s) - now native.
  Layout noise: the same sources with code shifted (e.g. the SPC engine -O2 vs -Os) differ by up to
  ~0.8 fps in a level; compare builds over several levels, not one.
- I18 Millstone Mayhem, MSTEST (millstone fully on screen all the time), frame skip: 20.8 -> 22.5
  fps (cached LUT + masks, opaque-cell blit); 60FPS MODE 23.8; not drawing it at all: 27.9; no
  millstone: ~38.
- I17 (devkit/perf/perf.py, frame skip on, fps / game speed): the swap wait was 45-65 % of a frame
  per picture.  Normal mode before -> after FLIP_ASYNC: JH 29 -> 40, level idx 2 22 -> 38, 4 26 -> 44,
  6 29 -> 37, 9 23 -> 32, 13 29 -> 49, 14 50 -> 54, 30 27 -> 51 (game speed 59.2-60).  60FPS MODE
  (forced, async swap too): JH 38.5, 4 49.6, 13 45.9, 30 53.2 - about the normal mode's numbers now
  (the slave is its limit); the automatic fallback keeps the normal mode unless 60FPS MODE is
  >= 10 % faster.

- Logic (G, % of a frame): JH 70, Reptile 65, Coral 61, Snow 74, Forest Frenzy 54. Master total T
  105-139 % -> the user build runs 2 logic frames per picture when logic + render > 94 % (30 pictures,
  game at full 60 Hz speed).
- Hot spots: master interpreter ~5.7 %, tile8 7 %, bm_spans ~6.5 % (Torchlight); slave music ~30 %,
  beamg_row 13.6 %, tc_fill 6.6 %. Ropey rain: rain_half ~7 % on each CPU.
- Interpreter cost on MiSTer: ~600 cycles per interpreted 65816 instruction (the interpreter is
  ROM code and misses the cache) - 50 instructions a frame cost ~8 % of the master. Hot keys go into
  translate.txt (resident ROM permitting).
- Torchlight: F15-18 in the MiSTer model. Turning was a 6-10 frame freeze (tile cache thrash in
  bm_spans_draw); now per tile row + edges sliding 48 px a picture: longest picture 6 vblanks.
- Ropey Rampage: F20 in the rain (logic ~70 %, master render ~97 %, slave ~45 % idle). The darkness
  fade's interpreted bursts are gone (translated). Next lever: the whole rain on the slave.
- I16 (interpreter -> native): bonus rooms with the sprite JML-table behaviours (Millstone Mayhem
  bonus 2, Vulture Culture bonus 1, ...) went from T 236 / 232 % to 142 / 138 % (DBG_GOTO bench, MiSTer
  model); main levels ~1 % (they had little interpreted code left).  Over the sim coverage the
  interpreted instructions in gameplay frames fell from 17.9 M to 1.0 M (0.42 M = the front-end menu).
- The rest of G (logic, 60-75 % of a frame) is native code missing the 4 KB cache (c/i 4-7 in
  ARES_SHPROF on the game functions) - the next lever is code placement / size, not the interpreter.
- 60 fps analysis and options: section 9.

---------------------------------------------------------------------------------------------------
## 8. Status / open list (I14 - I19 rounds, 2026-10-29)

Fixed in I14 (details in BUGLOG "I14 fixes"):
- Pause in a later level reset progress to level 1: pausing while holding run (Genesis A) was
  A + Start = the level viewer's menu shortcut; Start there restarted Jungle Hijinxs. A + Start now
  only works in the viewer; the full game reaches our menu via the pause menu's LEVEL SELECT.
- Rope upper parts invisible (unaligned word access in hle_rope_col).
- Torchlight pause menu closing at once (pm_hold); turn stutter (per-tile-row beam spans + sliding
  edges).
- Poison Pond RIPPLE OFF now also stops the 68000's BG1 ripple (cmd bit 0x2000).
- Snow weather fades and Ropey / Jungle Hijinxs / Orang-utan Gang darkness fades translated (they ran
  interpreted in bursts every 8 frames).
- Ropey: the distant treeline after the storm (68000 wx_jungle), haze table double-buffered.
- Funky: sprite palette 7 no longer overwritten by cabin text; catch-up frames in cabins.
- Misty Mine: line table without the parity row (no doubled fog lines), flat under the pause menu.
- Temple Tempest bonus 2 (53) and rooms C1 / C3 / B8: real surveys via the NMI jump capture.
- Builder: one note instead of 75 "no music for level" warnings (viewer menu table only).
- I15: Barrel Cannon Canyon missing barrel (widescreen spawn windows removed, see BUGLOG).
- I16: no pop-in at the edges (the 32X view is centred on the SNES picture: SNES x -32 .. 287 = the
  game's spawn window), hidden-item holes no longer show before their cover; Misty Mine dither = one
  checker phase per framebuffer (60 Hz alternation like Poison Pond); interpreter -> native run
  (BUGLOG "I16 optimization run"), DUP world-bank pages, window 6 reset at power-on / reset.
- I17: missed short presses with frame skip (pad sample queue), normal mode 8-90 % more pictures
  (asynchronous swap), 60FPS MODE pipeline fixed (it never overlapped before) + async swap, sky
  tables applied at the swap (BUGLOG "I17").
- I18: lamp levels' wall dither / dark tiles in the cones (S/H, BUGLOG B-LAMPDITHER), the water
  levels' jumpy camera (narrow camera blocks only), the pause menu no longer ripples, flashing sprite
  tile parts in 60FPS MODE (obj_tile_dr), Millstone ~8 % faster.  Tires: SNES-identical (BUGLOG).
  US v1.1 / v1.2: not supported (see NOTES I18 for the analysis).
- I19: performance round (HANDBOOK 7, BUGLOG I19): async swap for late and ripple pictures, level-load
  and per-frame interpreted code translated (data/lowrom.txt), one late music refill no longer adds a
  buffer of silence; Genesis plane dither (B-PLANEDITHER).
- I20: Genesis planes per-cell shadow (more dark shades, no new tiles), the I19 dither replaced by
  rare neighbour-level checkerboards, Blackout Basement's lights go out (BUGLOG "I20").
- I21: Galleon boss: planes at a constant view offset 32, the island sprite fixed with them (I21b/c: the
  live offset made backgrounds slide), water camera blocks need
  8 stable frames, Ropey's storm trees without colour math, pause menu OBJECT RANGE (WIDE default /
  SNES), small menu font, the Rare logo's own 60 fps effect + zoom, front-end catch-up carried over,
  plane clears by VDP DMA fill (BUGLOG "I21").
Open:
- OBJECT RANGE WIDE is a game-logic change (spawn + keep-alive windows moved together, rompatch
  WS_TUNE / r65_ws): waiting for the user's playthrough.  If a level breaks (I15: a barrel group in
  Barrel Cannon Canyon lost a member when only the spawn windows were widened), SNES restores the
  original logic; consider a per-level exception list.
- The Nintendo logo's first picture (hi-res mode 5, every line) still takes ~16 vblanks; the game
  catches up afterwards (fe debt), the picture appears ~0.1 s late.
- Poison Pond: one line of sprite pixels repeated at ripple band ends (sprites share the rippled
  canvas); workaround RIPPLE OFF.
- Ropey rain section F20 on MiSTer (game speed fine); Torchlight F15-18.
- Coral small fish / Necky one-frame tile (not reproduced; possibly the 60FPS MODE race fixed in
  I18); K. Rool fight not bot-played to the end.
- Millstone Mayhem with a millstone on screen: ~22 pictures a second on MiSTer (game speed fine).
- MiSTer music: always half-rate voices in levels below 57 fps, and ~10 short dropouts a 40 s stretch
  (refills over a 5.8 ms buffer; NOTES I18c).  I19: a single late refill no longer adds a whole
  buffer of silence (only two in a row do), so each dropout is the short gap alone.  The rare 1.6x refills
  look like song starts (mp_start clears the echo buffer and runs the engine init).

---------------------------------------------------------------------------------------------------
## 9. 60 fps: trade-offs of a dual-processor redesign

(I17) The normal mode with FLIP_ASYNC (section 3.3) now gives most of what 60FPS MODE gave, without
its extra frame of input lag; 60FPS MODE stays a pause-menu option (off by default).
What exists: 60FPS MODE (pause menu, off by default) = a two-stage pipeline in the fused levels:
the slave draws game frame N (all rows + sprites) while the master runs the logic of N + 1; the
master helps with sprite bands when its logic is done. Cost: +1 frame of input lag. Measured: ares
(fast) JH 57-60 fps, MiSTer model 35-56 fps - the SLAVE is the limit (music ~28 %, BG + restore under
the old sprites ~25-30 %, sprites).

Why not simply "split everything over both CPUs":
- The game logic is one 65816 thread. It must stay on one CPU to stay byte-exact (physics / feel
  identical to the SNES). It costs 55-75 % of a frame on MiSTer by itself.
- So 60 fps needs: master = logic (+ little else) <= ~95 %, slave = ALL drawing + music <= ~95 %.
  Today drawing alone is 50-80 % of a CPU in ordinary levels and much more in Torchlight / weather /
  fog levels; music is ~28-30 % of the slave. Heavy levels don't fit on any split.
- Music: there is no third processor that can do it. The 68000 cannot emulate the SPC700 + DSP
  (8 voices of sample decoding, interpolation, echo) at 7.6 MHz; pre-rendered PCM songs would need
  tens of MB of ROM (we are at 13.5 of 15 MB) and still need live sound effects mixed in. MUSIC
  LIGHT (fewer voices / lower rate, already in the menu) is the only cheap lever.
- Pipelining always adds one frame (16.7 ms) of input latency vs the SNES-like path. The user's goal
  "no added input lag" conflicts with it, hence an option, not the default.
- Every render mode (rain, snow, beam, fog, ripple, lamps, overlays, front-end screens) needs its own
  pipelined version: the slave must draw from a frozen copy of everything the logic changes (scroll,
  OAM, palette ring, VRAM uploads, weather state). Each copy costs SDRAM, and SDRAM is nearly full.

Can it be bulletproof?
- Correctness yes: the logic stays byte-exact either way; the pipeline only moves WHEN pixels are
  drawn. Made safe by (1) an explicit per-frame render snapshot double-buffer (no shared mutable
  state between the CPUs), (2) the existing job token protocol, (3) sim checks: pipelined pictures
  must equal non-pipelined pictures frame for frame. The known hazard (load-only WRAM aliasing the
  master tile cache) means the slave must never use the master cache.
- Frame rate no: 60 fps cannot be guaranteed in every level on MiSTer. A robust design keeps the
  frame-skip fallback (game speed stays 60 Hz, pictures drop to 30) so heavy scenes never slow down.

Recommended order (each step measurable, nothing risky to the logic):
1. Keep shaving the logic (translations, HLE of hot routines, SDRAM placement) - every % helps both
   modes and has no lag cost.
2. Cheaper drawing where it is the limit (Torchlight beam, weather, fog) - also helps both modes.
3. Extend 60FPS MODE to the other render modes one by one, each with the sim picture-equality check.
4. Optional auto MUSIC LIGHT when the slave is the limit.
