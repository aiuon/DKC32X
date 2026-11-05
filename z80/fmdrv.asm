; (I24) SOUND FM - the Z80 side: YM2612 (6 FM channels) + SN76489 from 7 command streams.
; Assembler: pasmo (devkit; z80/fmdrv.bin is shipped prebuilt, built from this file only - no game data).
;
; The 68000 does all bus work: it loads this driver, the song's instrument / envelope tables, and keeps
; the 7 stream rings full (LZ-expanded commands, whole commands only) - this CPU never touches the
; 68000 bus (no bank register: no ROM-access / PSG conflicts on the 32X, nothing to block during RV = 1).
; Timing: YM timer A (54 units = 1.014 ms) polled; ticks of the song every TK / 256 units (8.8 fixed,
; set by the 68000: 12.5 ms NTSC / PAL).  Every path polls the timer well within a period (3630 Z80 cycles; a 68000 bus request ~700).
;
; Stream commands (tools/audio/fm/fmpack.py):
;  00-3F WAIT n+1   40 lo hi WAIT n   42 END
;  50 inst env fh fl vol KEYON   51 fh fl FREQ   52 vol VOL   53 KEYOFF   54 ENV2   55 pan PAN
;  56 n b0..bn PSG (n+1 bytes)   57 d VOL+=d   58 d FREQ+=d   59-5B KEYOFF + WAIT 1-3
;  5C-5F d VOL+=d + WAIT 1-4   80-FF 1wwddddd FREQ+=d (-16..15) + WAIT ww

YM0     equ 4000h
YM1     equ 4001h
YM2     equ 4002h
YM3     equ 4003h
PSG     equ 7F11h

ENV     equ 0800h           ; env records, 7 bytes: 50 60 70 80 p60 p70 dull   (<= 146)
INST    equ 0C00h           ; instruments, 11 bytes: b0 cm follow (mul tl) x 4 (<= 32)
ST      equ 0D80h           ; stream states, 16 bytes each
RINGS   equ 1000h           ; 7 rings x 512 bytes
STACK   equ 1F00h
MB      equ 1F00h           ; mailbox (68000)
MB_CMD  equ MB+0            ; 1 START (rings / tables loaded, PEND / ACC set), 2 STOP, 3 FLAGS
MB_FLG  equ MB+1            ; bit 1 mono
MB_PEND equ MB+2            ; 16: ticks to play at once (a late start)
MB_ACC  equ MB+4            ; 24: timer accumulator (units x 256, signed: < 0 = before tick 0); a tick when >= TK
MB_TK   equ MB+7            ; 24: units x 256 a tick
MB_ALIVE equ MB+0Ah         ; ++ every tick
MB_STAT equ MB+0Bh          ; bit 0 playing, bit 1 a stream starved, bit 2 a timer period missed
MB_TICKN equ MB+0Ch         ; 16: ticks played
MB_ATT  equ MB+0Eh          ; master attenuation added to carriers (fade; 0 = full)
MB_WR   equ MB+10h          ; 7 x 16 ring write offsets (68000)
MB_RD   equ MB+20h          ; 7 x 16 ring read offsets (here)

S_WAIT  equ 0               ; 16
S_FN    equ 2               ; 16 last block << 11 | fnum (FREQ +d)
S_VOL   equ 4
S_INST  equ 5
S_ENV   equ 6
S_PAN   equ 7
S_ON    equ 8
S_PART  equ 9               ; 0 / 2: port offset (YM0 / YM2)
S_CC    equ 10              ; channel in its part 0-2
S_KC    equ 11              ; 0x28 code
S_NUM   equ 12              ; stream number

        org 0
        di
        im 1
        ld sp,STACK
        jp init

        org 38h
        ei                  ; (no interrupt is used)
        reti

init:   ld hl,6000h         ; the bank register away from the cartridge ($FF8000: the 32X tech bulletin's
        ld b,9              ;  PSG / ROM conflict) - this driver never reads the 68000 bus
ib1:    ld (hl),1
        djnz ib1
        xor a
        ld (MB_CMD),a
        ld (MB_STAT),a
        ld hl,ST            ; stream states: number, part, channel, key code
        ld b,7
        ld c,0
is1:    push bc
        push hl
        ld e,l
        ld d,h
        ld b,16
is2:    xor a
        ld (de),a
        inc de
        djnz is2
        pop hl
        pop bc
        push hl
        pop ix
        ld (ix+S_NUM),c
        ld a,c
        cp 3
        jr c,is3
        sub 3
        ld (ix+S_CC),a
        ld (ix+S_PART),2
        or 4
        ld (ix+S_KC),a
        jr is4
is3:    ld (ix+S_CC),a
        ld (ix+S_KC),a
is4:    ld de,16
        add hl,de
        inc c
        djnz is1
        call silence
        ; timer A = 970 (54 units = 1.01 ms), loaded and running; its flag polled
        ld b,24h
        ld c,0F2h
        call w0
        ld b,25h
        ld c,02h
        call w0
        ld b,27h
        ld c,15h
        call w0
        ld b,2Bh            ; DAC off: channel 6 is FM
        ld c,0
        call w0
        ld b,22h            ; LFO off
        ld c,0
        call w0

; ------------------------------------------------------------------------------------- main loop
main:   call poll
        ld a,(MB_CMD)
        or a
        call nz,command
        ld a,(MB_STAT)
        and 1
        jr z,main
        ld hl,(MB_PEND)
        ld a,h
        or l
        jr z,main
        dec hl
        ld (MB_PEND),hl
        call tick
        jr main

; the 68000's commands
command:
        cp 1
        jr nz,cm2
        ld hl,MB_STAT       ; START: the 68000 set up rings, tables, PEND, ACC
        set 0,(hl)
        res 1,(hl)
        res 2,(hl)
        ld hl,0
        ld (MB_TICKN),hl
        ld ix,ST            ; streams on, waits 0 (their first commands at tick 0)
        ld b,7
        ld de,16
cs1:    ld (ix+S_ON),1
        ld (ix+S_WAIT),0
        ld (ix+S_WAIT+1),0
        ld (ix+S_FN),0
        ld (ix+S_FN+1),0
        add ix,de
        djnz cs1
        jr cmdone
cm2:    cp 2
        jr nz,cmdone        ; (3 FLAGS: just read on use)
        ld hl,MB_STAT       ; STOP
        res 0,(hl)
        call silence
cmdone: xor a
        ld (MB_CMD),a
        ret

; all FM keys off, fast release; PSG silent
silence:
        ld b,28h
        ld e,0
sl1:    ld a,e
        cp 3
        jr z,sl2
        cp 7
        jr z,sl3
        ld c,e
        call w0
        push de
        ld a,e              ; RR 15 on every slot (the key-off ends at once)
        and 3
        ld d,a
        ld a,e
        and 4
        jr nz,slp1
        ld a,80h
        add a,d
        call rr0
        jr slp2
slp1:   ld a,80h
        add a,d
        call rr1
slp2:   pop de
        ld b,28h
sl2:    inc e
        jr sl1
sl3:    ld a,9Fh
        ld (PSG),a
        ld a,0BFh
        ld (PSG),a
        ld a,0DFh
        ld (PSG),a
        ld a,0FFh
        ld (PSG),a
        ret
rr0:    ld b,4              ; a = 80h + channel: the 4 slots' D1L/RR = 0Fh
rr0a:   push bc
        ld b,a
        ld c,0Fh
        call w0
        ld a,b
        add a,4
        pop bc
        djnz rr0a
        ret
rr1:    ld b,4
rr1a:   push bc
        ld b,a
        ld c,0Fh
        call w2
        ld a,b
        add a,4
        pop bc
        djnz rr1a
        ret

; ------------------------------------------------------------------------------------- the timer
; timer A overflowed: reset its flag, ACC += 54 x 256; a tick (PEND++) when ACC >= TK
poll:   ld a,(YM0)
        rrca
        ret nc
        push bc
        push hl
        push de
        ld b,27h
        ld c,15h
        call w0r
        ld hl,(MB_ACC)      ; 24-bit ACC += 3600h (54 units x 256)
        ld a,h
        add a,36h
        ld h,a
        ld a,(MB_ACC+2)
        adc a,0
        ld (MB_ACC+2),a
        ld (MB_ACC),hl
        bit 7,a             ; (negative: the start is still ahead)
        jr nz,pl1
        ; ACC >= TK ?
        ld e,a              ; e:h:l = ACC
        ld a,l
        ld bc,(MB_TK)
        sub c
        ld l,a
        ld a,h
        sbc a,b
        ld h,a
        ld a,(MB_TK+2)
        ld d,a
        ld a,e
        sbc a,d
        jr c,pl1            ; below
        ld (MB_ACC),hl
        ld (MB_ACC+2),a
        ld hl,(MB_PEND)
        inc hl
        ld (MB_PEND),hl
        ld a,h              ; (more than a few pending: a period was missed - flag it)
        or a
        jr z,pl1
        ld hl,MB_STAT
        set 2,(hl)
pl1:    pop de
        pop hl
        pop bc
        ret

; register writes: b = register, c = value.  w0: part I, w2: part II, wp: the stream's part (ix) - each
; polls the timer first (a KEYON's 30 writes take longer than a timer period)
w0:     call poll
w0r:    ld a,(YM0)
        rlca
        jr c,w0r
        ld a,b
        ld (YM0),a
        ld a,c
        ld (YM1),a
        ret
w2:     call poll
w2r:    ld a,(YM0)
        rlca
        jr c,w2r
        ld a,b
        ld (YM2),a
        ld a,c
        ld (YM3),a
        ret
wp:     ld a,(ix+S_PART)
        or a
        jr z,w0
        jr w2
; wpc: register b + the stream's channel
wpc:    ld a,b
        add a,(ix+S_CC)
        ld b,a
        jr wp

; ------------------------------------------------------------------------------------- a tick
tick:   ld hl,(MB_TICKN)
        inc hl
        ld (MB_TICKN),hl
        ld hl,MB_ALIVE
        inc (hl)
        ld ix,ST
        ld b,7
tk1:    push bc
        ld a,(ix+S_ON)
        or a
        jr z,tk3
        ld l,(ix+S_WAIT)
        ld h,(ix+S_WAIT+1)
        ld a,h
        or l
        jr z,tk2
        dec hl
        ld (ix+S_WAIT),l
        ld (ix+S_WAIT+1),h
        ld a,h
        or l
        jr nz,tk3
tk2:    call run            ; this stream's commands up to its next WAIT
tk3:    call poll
        ld de,16
        add ix,de
        pop bc
        djnz tk1
        ret

; ring access: hl = ring base + read offset; the offset in (MB_RD + 2 s)
; avail: z = the ring is empty
ringp:  ld a,(ix+S_NUM)
        add a,a
        ld e,a
        ld d,0
        ld hl,MB_RD
        add hl,de
        ret                 ; hl -> RD[s]; WR[s] = hl - 16
avail:  call ringp
        ld e,(hl)
        inc hl
        ld d,(hl)           ; de = rd
        ld bc,-17
        add hl,bc           ; hl -> WR[s]
        ld a,(hl)
        inc hl
        ld h,(hl)
        ld l,a              ; hl = wr
        or a
        sbc hl,de
        ld a,h
        and 1
        or l
        ret
; rb: a = the next byte of the stream (ix), the read offset advanced
rb:     call poll
        push hl
        push de
        push bc
        call ringp
        ld e,(hl)
        inc hl
        ld d,(hl)           ; de = rd
        push hl
        ld a,(ix+S_NUM)     ; ring base = RINGS + s x 512
        add a,a
        add a,10h
        ld h,a
        ld l,0
        add hl,de
        ld c,(hl)           ; the byte
        inc de
        ld a,d
        and 1
        ld d,a
        pop hl
        ld (hl),d
        dec hl
        ld (hl),e
        ld a,c
        pop bc
        pop de
        pop hl
        ret

; run: commands until a WAIT (or the ring is empty: stall a tick - flagged)
run:    call avail
        jr nz,rn1
        ld (ix+S_WAIT),1
        ld (ix+S_WAIT+1),0
        ld hl,MB_STAT
        set 1,(hl)
        ret
rn1:    call poll            ; (every command: a long run must not miss a timer period)
        call rb
        cp 40h
        jr nc,rn2
        inc a               ; WAIT n+1
        ld (ix+S_WAIT),a
        ld (ix+S_WAIT+1),0
        ret
rn2:    jr nz,rn3
        call rb             ; WAIT16
        ld (ix+S_WAIT),a
        call rb
        ld (ix+S_WAIT+1),a
        ret
rn3:    cp 80h
        jp nc,c_fd
        cp 42h
        jr nz,rn4
        ld (ix+S_ON),0      ; END
        ret
rn4:    cp 50h
        jp z,c_keyon
        cp 51h
        jp z,c_freq
        cp 52h
        jp z,c_vol
        cp 53h
        jp z,c_keyoff
        cp 54h
        jp z,c_env2
        cp 55h
        jp z,c_pan
        cp 56h
        jp z,c_psg
        cp 57h
        jp z,c_vold
        cp 58h
        jp z,c_fd8
        jr c,rnbad
        cp 5Ch
        jp c,c_kow
        cp 60h
        jp c,c_vdw
rnbad:  ld (ix+S_ON),0      ; (unknown: stop the stream)
        ret
; setw: WAIT a ticks (a > 0), the run is over
setw:   ld (ix+S_WAIT),a
        ld (ix+S_WAIT+1),0
        ret

; pointers: iy = instrument record, hl = envelope record of the stream
instp:  ld a,(ix+S_INST)    ; iy = INST + 11 x inst
        ld l,a
        ld h,0
        ld e,l
        ld d,h
        add hl,hl
        add hl,de           ; x3
        add hl,hl
        add hl,hl           ; x12
        or a
        sbc hl,de           ; x11
        ld de,INST
        add hl,de
        push hl
        pop iy
        ld a,(ix+S_ENV)     ; hl = ENV + 7 x env
        ld l,a
        ld h,0
        ld e,l
        ld d,h
        add hl,hl
        add hl,hl
        add hl,hl           ; x8
        or a
        sbc hl,de           ; x7
        ld de,ENV
        add hl,de
        ret

slotof: db 0,8,4,12

; carrier TL: c = min(127, tl + vol + master attenuation)
cartl:  add a,(ix+S_VOL)
        jr c,ct1
        ld c,a
        ld a,(MB_ATT)
        add a,c
        jr c,ct1
        cp 128
        jr nc,ct1
        ld c,a
        ret
ct1:    ld c,127
        ret

c_keyon:
        call rb
        ld (ix+S_INST),a
        call rb
        ld (ix+S_ENV),a
        call rb
        ld c,a              ; fh: pan << 6 | block << 3 | fnum >> 8
        and 3Fh
        ld (ix+S_FN+1),a
        ld a,c
        rlca
        rlca
        and 3
        ld (ix+S_PAN),a
        call rb
        ld (ix+S_FN),a
        call rb
        ld (ix+S_VOL),a
        ; key off
        ld b,28h
        ld c,(ix+S_KC)
        call w0
        call instp          ; iy = instrument, hl = envelope
        push hl
        ld b,0B0h
        ld c,(iy+0)
        call wpc
        ld de,0             ; d = slot k, e unused
        pop hl
        ld (envp),hl
        ld a,(iy+1)
        ld (cmask),a
        push iy
        pop hl
        inc hl
        inc hl
        inc hl              ; hl -> (mul, tl) x 4
        ld (opp),hl
        xor a
        ld (slotk),a
ko1:    call poll
        ld a,(slotk)        ; o = slot offset + channel
        ld e,a
        ld d,0
        ld hl,slotof
        add hl,de
        ld a,(hl)
        add a,(ix+S_CC)
        ld (slo),a
        ld hl,(opp)         ; MUL
        ld c,(hl)
        inc hl
        ld a,(hl)           ; tl
        inc hl
        ld (opp),hl
        ld (tlk),a
        ld a,(slo)
        add a,30h
        ld b,a
        call wp
        ld a,(cmask)        ; carrier?
        ld c,a
        ld a,(slotk)
        inc a
        ld b,a
        ld a,c
ko2:    rrca
        djnz ko2
        jr nc,komod
        ld a,(tlk)          ; carrier: TL + vol, the envelope
        call cartl
        ld a,(slo)
        add a,40h
        ld b,a
        call wp
        ld hl,(envp)
        ld c,(hl)
        ld a,(slo)
        add a,50h
        ld b,a
        call wp
        ld hl,(envp)
        inc hl
        ld c,(hl)
        ld a,(slo)
        add a,60h
        ld b,a
        call wp
        ld hl,(envp)
        inc hl
        inc hl
        ld c,(hl)
        ld a,(slo)
        add a,70h
        ld b,a
        call wp
        ld hl,(envp)
        inc hl
        inc hl
        inc hl
        ld c,(hl)
        ld a,(slo)
        add a,80h
        ld b,a
        call wp
        jp konext
komod:  ld hl,(envp)        ; modulator: TL + dull
        ld de,6
        add hl,de
        ld a,(tlk)
        add a,(hl)
        cp 128
        jr c,km1
        ld a,127
km1:    ld c,a
        ld a,(slo)
        add a,40h
        ld b,a
        call wp
        ld a,(iy+2)         ; follow the note's envelope?
        or a
        jr z,kmfix
        ld hl,(envp)        ; 50 = e50, 60 = p60, 70 = p70, 80 = (p60 ? F0 : 0) | rr
        ld c,(hl)
        ld a,(slo)
        add a,50h
        ld b,a
        call wp
        ld hl,(envp)
        ld de,4
        add hl,de
        ld c,(hl)
        ld a,(slo)
        add a,60h
        ld b,a
        call wp
        ld hl,(envp)
        ld de,5
        add hl,de
        ld c,(hl)
        ld a,(slo)
        add a,70h
        ld b,a
        call wp
        ld hl,(envp)
        ld de,3
        add hl,de
        ld a,(hl)
        and 0Fh
        ld c,a
        ld hl,(envp)
        ld de,4
        add hl,de
        ld a,(hl)
        or a
        jr z,km2
        ld a,c
        or 0F0h
        ld c,a
km2:    ld a,(slo)
        add a,80h
        ld b,a
        call wp
        jr konext
kmfix:  ld c,1Fh            ; a static modulator: AR 31, no decay, the release
        ld a,(slo)
        add a,50h
        ld b,a
        call wp
        ld c,0
        ld a,(slo)
        add a,60h
        ld b,a
        call wp
        ld c,0
        ld a,(slo)
        add a,70h
        ld b,a
        call wp
        ld hl,(envp)
        inc hl
        inc hl
        inc hl
        ld a,(hl)
        and 0Fh
        ld c,a
        ld a,(slo)
        add a,80h
        ld b,a
        call wp
konext: ld a,(slotk)
        inc a
        ld (slotk),a
        cp 4
        jp nz,ko1
        call poll
        call panw
        call fnw
        ld b,28h            ; key on
        ld a,(ix+S_KC)
        or 0F0h
        ld c,a
        call w0
        jp run

panw:   ld a,(MB_FLG)       ; B4: pan (mono: both)
        and 2
        ld c,0C0h
        jr nz,pw1
        ld a,(ix+S_PAN)
        rrca
        rrca
        and 0C0h
        ld c,a
pw1:    ld b,0B4h
        jp wpc
fnw:    ld c,(ix+S_FN+1)    ; A4 then A0
        ld b,0A4h
        call wpc
        ld c,(ix+S_FN)
        ld b,0A0h
        jp wpc

c_freq: call rb
        and 3Fh
        ld (ix+S_FN+1),a
        call rb
        ld (ix+S_FN),a
        call fnw
        jp run
c_fd:   ld c,a              ; 1 ww ddddd: FREQ += d (5 bits signed), then WAIT ww
        and 1Fh
        bit 4,a
        jr z,fd0
        or 0E0h
fd0:    push bc
        call fdadd
        pop bc
        ld a,c
        rlca
        rlca
        rlca
        and 3
        jp z,run
        jp setw
c_fd8:  call rb             ; 58 d: FREQ += d (signed byte)
        call fdadd
        jp run
fdadd:  ld e,a              ; S_FN += a (signed), A4 / A0 written
        ld d,0
        bit 7,a
        jr z,fd1
        dec d
fd1:    ld l,(ix+S_FN)
        ld h,(ix+S_FN+1)
        add hl,de
        ld (ix+S_FN),l
        ld (ix+S_FN+1),h
        jp fnw
c_kow:  sub 58h             ; 59-5B: KEYOFF, WAIT 1-3
        push af
        ld b,28h
        ld c,(ix+S_KC)
        call w0
        pop af
        jp setw
c_vdw:  sub 5Bh             ; 5C-5F d: VOL += d, WAIT 1-4
        push af
        call rb
        add a,(ix+S_VOL)
        ld (ix+S_VOL),a
        call carriers
        pop af
        jp setw

c_vold: call rb
        add a,(ix+S_VOL)
        ld (ix+S_VOL),a
        jr vol1
c_vol:  call rb
        ld (ix+S_VOL),a
vol1:   call carriers
        jp run
; carriers: rewrite the carriers' TL (vol / master attenuation changed)
carriers:
        call instp
        ld a,(iy+1)
        ld (cmask),a
        push iy
        pop hl
        inc hl
        inc hl
        inc hl
        inc hl              ; hl -> tl0
        ld (opp),hl
        xor a
        ld (slotk),a
cr1:    ld a,(cmask)
        rrca
        ld (cmask),a
        jr nc,cr2
        ld hl,(opp)
        ld a,(hl)
        call cartl
        ld a,(slotk)
        ld e,a
        ld d,0
        ld hl,slotof
        add hl,de
        ld a,(hl)
        add a,40h
        add a,(ix+S_CC)
        ld b,a
        call wp
cr2:    ld hl,(opp)
        inc hl
        inc hl
        ld (opp),hl
        ld a,(slotk)
        inc a
        ld (slotk),a
        cp 4
        jr nz,cr1
        ret

c_keyoff:
        ld b,28h
        ld c,(ix+S_KC)
        call w0
        jp run
c_env2: call instp          ; carriers' 60 / 70 = the post-ramp decay
        ld (envp),hl
        ld a,(iy+1)
        ld (cmask),a
        xor a
        ld (slotk),a
e21:    ld a,(cmask)
        rrca
        ld (cmask),a
        jr nc,e22
        ld a,(slotk)
        ld e,a
        ld d,0
        ld hl,slotof
        add hl,de
        ld a,(hl)
        add a,(ix+S_CC)
        ld (slo),a
        ld hl,(envp)
        ld de,4
        add hl,de
        ld c,(hl)
        ld a,(slo)
        add a,60h
        ld b,a
        call wp
        ld hl,(envp)
        ld de,5
        add hl,de
        ld c,(hl)
        ld a,(slo)
        add a,70h
        ld b,a
        call wp
e22:    ld a,(slotk)
        inc a
        ld (slotk),a
        cp 4
        jr nz,e21
        jp run
c_pan:  call rb
        ld (ix+S_PAN),a
        call panw
        jp run
c_psg:  call rb             ; n + 1 raw bytes
        inc a
        ld b,a
ps1:    call rb
        ld (PSG),a
        djnz ps1
        jp run

; scratch
envp:   dw 0
opp:    dw 0
cmask:  db 0
slotk:  db 0
slo:    db 0
tlk:    db 0
