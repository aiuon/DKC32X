OUTPUT_ARCH(m68k)
MEMORY {
	rom (rx) : ORIGIN = 0x00880000, LENGTH = 512K
	ram (wx) : ORIGIN = 0x00FF0000, LENGTH = 64K
}
SECTIONS {
	.text 0x008803F0 : {
		*(.text.keepboot)
		*(.text .text.*)
		*(.rodata .rodata.*)
		. = ALIGN(4);
		__text_end = .;
	} > rom
	.data 0x00FF0000 : AT(LOADADDR(.text) + SIZEOF(.text)) {
		*(.data .data.*)
		*(.ramtext .ramtext.*)
		. = ALIGN(2);
	} > ram
	__data_size = SIZEOF(.data);
	.bss : {
		*(.bss .bss.* COMMON)
		. = ALIGN(2);
	} > ram
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
