OUTPUT_ARCH(sh)
MEMORY {
	rom (rx) : ORIGIN = 0x02000000, LENGTH = 0x00400000
	ram (wx) : ORIGIN = 0x06000000, LENGTH = 0x0003E800   /* then the master stack (4 KB - 16, I13: the front-end screens with sprites used 1.9 of 2 KB), the slave's (2 KB) */
}
SECTIONS {
	.text 0x02000000 : AT(0x00000000) {
		EXCLUDE_FILE(*g65_*.o *game_tab.o *game_data.o) *(.text .text.*)
		EXCLUDE_FILE(*g65_*.o *game_tab.o *game_data.o) *(.rodata .rodata.*)
		. = ALIGN(0x10);
		_etext = .;
	} > rom
	_stext = SIZEOF(.text);
	/* the game code (recompiled 65816) and its data, after the pack */
	.gtext GAME_BASE : AT(GAME_BASE - 0x02000000) {
		_gtext_start = .;
		*g65_*.o(.text .text.* .rodata .rodata.*)
		*game_tab.o(.text .text.* .rodata .rodata.*)
		*game_data.o(.text .rodata .gtext)
		. = ALIGN(0x10);
	}
	.data 0x06000000 : AT(LOADADDR(.text) + SIZEOF(.text)) {
		*(.data .data.*)
		. = ALIGN(0x10);
		EXCLUDE_FILE(*g65_*.o) *(.sdtext .sdtext.*)
		. = ALIGN(0x10);
	} > ram
	_sdata = SIZEOF(.data);
	/* (I12) the recompiled game code that runs from SDRAM: m_main copies its image itself before anything
	   runs it.  (I17) The image sits in world bank 0 (window 6 at power-on, SDGAME_LMA = 0x300000 + its
	   offset there: finalize.py puts it there with a header m_main checks); without SDGAME_LMA it follows
	   the .data image in the first 256 KB as before. */
	.sdgame 0x06000000 + SIZEOF(.data) : AT(DEFINED(SDGAME_LMA) ? SDGAME_LMA : LOADADDR(.data) + SIZEOF(.data)) {
		_sdgame_start = .;
		*g65_*.o(.sdtext .sdtext.*)
		. = ALIGN(0x10);
		_sdgame_end = .;
	} > ram
	_sdgame_load = LOADADDR(.sdgame);
	.bss 0x06000000 + SIZEOF(.data) + SIZEOF(.sdgame) : {
		__bss_start = .;
		*(.bss .bss.* COMMON)
		. = ALIGN(0x10);
		__end = .;
	} > ram
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
