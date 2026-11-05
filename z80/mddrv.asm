; (I25) SOUND MD - "Genesis sound only": the Z80 side.  A sample player: every timer A period (7 units:
; 7610 Hz NTSC) the next entry of the 68000's ring: its 8-bit sample to the YM2612's DAC and at most one
; YM2612 / PSG register write.  The 68000 does everything else (it mixes the PCM voices, runs the songs' FM
; streams and the sound effects, and spreads the register writes over the samples from the one each tick
; falls on - the FM and the PCM cannot drift apart: they are the same entries).
; Assembler: pasmo (devkit; tools/prebuilt/mddrv.bin is shipped prebuilt - no game data in it).
;
; Ring: RING, 512 entries of 4 bytes: sample, cmd, reg, val.  sample 0 = not written yet (the 68000 writes
; 1-255, last; the Z80 clears it when played), cmd 0 = no register write, 2 = YM part I, 3 = part II, 4 = PSG
; byte (val) - the Z80 clears a cmd it played, so a plain entry costs the 68000 one byte.  The Z80 never
; touches the 68000 bus (bank register away, as fmdrv.asm).
;
; Timing (Z80 cycles, the longest path: an entry with a write) ~330 of the 402 a period: a 68000 bus hold of
; <= 16 bytes (~60 us) during it can make a period late, not lost.

YM0     equ 4000h
PSG     equ 7F11h
RING    equ 1000h           ; 1000h-17FFh (the wrap: res 3,h)
MB      equ 1F00h
MB_TAV  equ MB+0            ; 16: timer A value (the 68000's, before the reset ends)
MB_DRD  equ MB+2            ; 16: the next entry's address (the 68000 reads it: the room)
MB_STAT equ MB+4            ; bit 0: the ring ran dry (an entry not written in time)
MB_ALIVE equ MB+5           ; ++ every 64 entries
CNT     equ MB+6            ; periods in this timer B window
DEBT    equ MB+7            ; periods missed (their samples owed)
MB_LOST equ MB+8            ; (test reads) ++ every owed sample
STACK   equ 1F00h

        org 0
        di
        im 1
        ld sp,STACK
        jp init

        org 38h
        ei
        reti

init:   ld hl,6000h         ; the bank register away from the cartridge (fmdrv.asm)
        ld b,9
ib1:    ld (hl),1
        djnz ib1
        ld bc,YM0
        ld de,2B80h         ; DAC on
        call w0
        ld de,2200h         ; LFO off
        call w0
        ld hl,(MB_TAV)      ; timer A: 24h = bits 9-2, 25h = bits 1-0
        ld a,l
        and 3
        ld e,a
        srl h
        rr l
        srl h
        rr l
        ld d,24h
        push de
        ld e,l
        call w0
        pop de
        ld d,25h
        call w0
        ld hl,(MB_TAV)      ; timer B = 256 - units: 16 timer A periods (its unit is 16 of A's)
        ld de,768
        or a
        sbc hl,de
        ld d,26h
        ld e,l
        call w0
        ld de,273Fh         ; timers A and B loaded, their flags on (and reset)
        call w0
        ld c,2              ; part II: B6 = both speakers for channel 6 (the DAC)
        ld de,0B6C0h
        call w0
        ld c,0
        xor a
        ld (MB_STAT),a
        ld (CNT),a
        ld (DEBT),a
        ld hl,RING
        ld (MB_DRD),hl
        jp loop

; d = register, e = value, bc = the part's address port (4000h / 4002h)
w0:     ld a,(YM0)
        rlca
        jr c,w0
        ld a,d
        ld (bc),a
        inc c
        ld a,e
        ld (bc),a
        dec c
        ret

; ------------------------------------------------------------------------------- the loop
; hl = the next entry, bc = 4000h.  Timer B runs at 16 timer A periods: a window's periods the loop missed
; (a 68000 bus hold over a period) are owed (DEBT, <= 32) and played back to back as soon as there is time -
; the sample rate, so the music's tempo and the PCM's pitch, stays exact
loop:   ld a,(bc)           ; the timers
        bit 1,a
        jp nz,tmb           ; B: a window of 16 periods over
        rrca
        jr c,tma            ; A: a period
        ld a,(DEBT)         ; neither: a missed period's sample now
        or a
        jr z,loop
        ld a,(hl)
        or a
        jr z,loop           ; (not written yet: it waits)
        ex af,af'
        ld a,(DEBT)
        dec a
        ld (DEBT),a
        ex af,af'
        jr smp
tma:    ld a,(CNT)          ; a period: counted, timer A's flag reset (it runs on)
        inc a
        ld (CNT),a
sw1:    ld a,(bc)           ; (the YM busy from the last write)
        rlca
        jr c,sw1
        ld a,27h
        ld (bc),a
        inc c
        ld a,1Fh            ; (both timers on, A's flag reset)
        ld (bc),a
        dec c
        ld a,(hl)           ; the sample (0: not written yet)
        or a
        jr z,dry
smp:    ld (hl),0           ; (played: the 68000 may write it again)
        inc l
        ex af,af'
        ld e,(hl)           ; e = cmd
sw2:    ld a,(bc)
        rlca
        jr c,sw2
        ld a,2Ah            ; the DAC
        ld (bc),a
        inc c
        ex af,af'
        ld (bc),a
        dec c
        ld a,e              ; 0: no register write
        or a
        jr z,next
        ld (hl),0
        dec e
        inc l
        ld d,(hl)           ; reg
        inc l
        ld a,(hl)           ; val
        dec e
        jr z,wp1            ; 2: part I
        dec e
        jr z,wp2            ; 3: part II
        ld (PSG),a          ; 4: PSG
        jr next
wp2:    ld c,2
wp1:    ex af,af'
sw3:    ld a,(YM0)
        rlca
        jr c,sw3
        ld a,d
        ld (bc),a
        inc c
        ex af,af'
        ld (bc),a
        ld c,0
next:   ld a,l              ; the next entry (4 bytes; the ring wraps 1800h -> 1000h)
        or 3
        ld l,a
        inc hl
        res 3,h
        ld (MB_DRD),hl
        ld a,l
        or a
        jp nz,loop
        ld a,(MB_ALIVE)
        inc a
        ld (MB_ALIVE),a
        jp loop
dry:    ld a,(MB_STAT)      ; (the 68000 is late: the period passes without a sample - not owed)
        or 1
        ld (MB_STAT),a
        jp loop
tmb:    ld a,(bc)           ; timer B: its flag reset
        rlca
        jr c,tmb
        ld a,27h
        ld (bc),a
        inc c
        ld a,2Fh
        ld (bc),a
        dec c
        ld a,(CNT)          ; the window's missed periods owed
        ld e,a
        xor a
        ld (CNT),a
        ld a,16
        sub e
        jr c,tb1
        jr z,tb1
        ld e,a
        ld a,(DEBT)
        add a,e
        cp 33
        jr c,tb2
        ld a,32
tb2:    ld (DEBT),a
tb1:    jp loop
