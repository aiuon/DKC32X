"""65816 opcode table: OPS[op] = (mnemonic, mode).
Modes: imp acc immM immX imm8 dp dpx dpy dpi dpix dpiy dpil dpily abs absx absy absl abslx
       absi absix absil sr sriy rel8 rel16 blk"""
_T = """
00 BRK imm8|01 ORA dpix|02 COP imm8|03 ORA sr|04 TSB dp|05 ORA dp|06 ASL dp|07 ORA dpil|08 PHP imp|09 ORA immM|0A ASL acc|0B PHD imp|0C TSB abs|0D ORA abs|0E ASL abs|0F ORA absl
10 BPL rel8|11 ORA dpiy|12 ORA dpi|13 ORA sriy|14 TRB dp|15 ORA dpx|16 ASL dpx|17 ORA dpily|18 CLC imp|19 ORA absy|1A INC acc|1B TCS imp|1C TRB abs|1D ORA absx|1E ASL absx|1F ORA abslx
20 JSR abs|21 AND dpix|22 JSL absl|23 AND sr|24 BIT dp|25 AND dp|26 ROL dp|27 AND dpil|28 PLP imp|29 AND immM|2A ROL acc|2B PLD imp|2C BIT abs|2D AND abs|2E ROL abs|2F AND absl
30 BMI rel8|31 AND dpiy|32 AND dpi|33 AND sriy|34 BIT dpx|35 AND dpx|36 ROL dpx|37 AND dpily|38 SEC imp|39 AND absy|3A DEC acc|3B TSC imp|3C BIT absx|3D AND absx|3E ROL absx|3F AND abslx
40 RTI imp|41 EOR dpix|42 WDM imm8|43 EOR sr|44 MVP blk|45 EOR dp|46 LSR dp|47 EOR dpil|48 PHA imp|49 EOR immM|4A LSR acc|4B PHK imp|4C JMP abs|4D EOR abs|4E LSR abs|4F EOR absl
50 BVC rel8|51 EOR dpiy|52 EOR dpi|53 EOR sriy|54 MVN blk|55 EOR dpx|56 LSR dpx|57 EOR dpily|58 CLI imp|59 EOR absy|5A PHY imp|5B TCD imp|5C JML absl|5D EOR absx|5E LSR absx|5F EOR abslx
60 RTS imp|61 ADC dpix|62 PER rel16|63 ADC sr|64 STZ dp|65 ADC dp|66 ROR dp|67 ADC dpil|68 PLA imp|69 ADC immM|6A ROR acc|6B RTL imp|6C JMP absi|6D ADC abs|6E ROR abs|6F ADC absl
70 BVS rel8|71 ADC dpiy|72 ADC dpi|73 ADC sriy|74 STZ dpx|75 ADC dpx|76 ROR dpx|77 ADC dpily|78 SEI imp|79 ADC absy|7A PLY imp|7B TDC imp|7C JMP absix|7D ADC absx|7E ROR absx|7F ADC abslx
80 BRA rel8|81 STA dpix|82 BRL rel16|83 STA sr|84 STY dp|85 STA dp|86 STX dp|87 STA dpil|88 DEY imp|89 BIT immM|8A TXA imp|8B PHB imp|8C STY abs|8D STA abs|8E STX abs|8F STA absl
90 BCC rel8|91 STA dpiy|92 STA dpi|93 STA sriy|94 STY dpx|95 STA dpx|96 STX dpy|97 STA dpily|98 TYA imp|99 STA absy|9A TXS imp|9B TXY imp|9C STZ abs|9D STA absx|9E STZ absx|9F STA abslx
A0 LDY immX|A1 LDA dpix|A2 LDX immX|A3 LDA sr|A4 LDY dp|A5 LDA dp|A6 LDX dp|A7 LDA dpil|A8 TAY imp|A9 LDA immM|AA TAX imp|AB PLB imp|AC LDY abs|AD LDA abs|AE LDX abs|AF LDA absl
B0 BCS rel8|B1 LDA dpiy|B2 LDA dpi|B3 LDA sriy|B4 LDY dpx|B5 LDA dpx|B6 LDX dpy|B7 LDA dpily|B8 CLV imp|B9 LDA absy|BA TSX imp|BB TYX imp|BC LDY absx|BD LDA absx|BE LDX absy|BF LDA abslx
C0 CPY immX|C1 CMP dpix|C2 REP imm8|C3 CMP sr|C4 CPY dp|C5 CMP dp|C6 DEC dp|C7 CMP dpil|C8 INY imp|C9 CMP immM|CA DEX imp|CB WAI imp|CC CPY abs|CD CMP abs|CE DEC abs|CF CMP absl
D0 BNE rel8|D1 CMP dpiy|D2 CMP dpi|D3 CMP sriy|D4 PEI dp|D5 CMP dpx|D6 DEC dpx|D7 CMP dpily|D8 CLD imp|D9 CMP absy|DA PHX imp|DB STP imp|DC JML absil|DD CMP absx|DE DEC absx|DF CMP abslx
E0 CPX immX|E1 SBC dpix|E2 SEP imm8|E3 SBC sr|E4 CPX dp|E5 SBC dp|E6 INC dp|E7 SBC dpil|E8 INX imp|E9 SBC immM|EA NOP imp|EB XBA imp|EC CPX abs|ED SBC abs|EE INC abs|EF SBC absl
F0 BEQ rel8|F1 SBC dpiy|F2 SBC dpi|F3 SBC sriy|F4 PEA abs|F5 SBC dpx|F6 INC dpx|F7 SBC dpily|F8 SED imp|F9 SBC absy|FA PLX imp|FB XCE imp|FC JSR absix|FD SBC absx|FE INC absx|FF SBC abslx
"""
OPS = [None] * 256
for _ln in _T.strip().splitlines():
    for _e in _ln.split('|'):
        _o, _m, _md = _e.split()
        OPS[int(_o, 16)] = (_m, _md)
assert all(OPS)
FIXLEN = {'imp': 0, 'acc': 0, 'imm8': 1, 'dp': 1, 'dpx': 1, 'dpy': 1, 'dpi': 1, 'dpix': 1, 'dpiy': 1,
          'dpil': 1, 'dpily': 1, 'abs': 2, 'absx': 2, 'absy': 2, 'absl': 3, 'abslx': 3, 'absi': 2,
          'absix': 2, 'absil': 2, 'sr': 1, 'sriy': 1, 'rel8': 1, 'rel16': 2, 'blk': 2}

def oplen(op, m, x):
    """operand length; m, x = 1 for 8-bit"""
    md = OPS[op][1]
    if md == 'immM': return 1 if m else 2
    if md == 'immX': return 1 if x else 2
    return FIXLEN[md]
