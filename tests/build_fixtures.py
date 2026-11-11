#!/usr/bin/env python3
"""Builds the synthetic GB and Z80 fixtures for the deepeel test suite.

Every fixture is a minimal ROM exercising one exploration behaviour:
  branch     - both sides of a conditional + a ROM->VRAM copy loop
  mbc5       - bank switching through MBC5 registers, cross-bank calls
  dma        - OAM DMA from a ROM source ($FF46)
  irq        - IE/IF set by hand (IRQ forking) + a VBlank handler
  jumptable  - JP HL through a pointer table, both handlers reachable
  calldispatch - inline table behind a plain call to a dispatcher routine
  copytable  - code-pointer table copied ROM->WRAM then indexed there
  rstdispatch - rst $28 jump table inline after the call site
  dispatch   - indexed JP HL (ld hl,TABLE; add hl,de; ld h,(hl); jp hl): the table's other entries proven code
  loop       - infinite loop, must be contained by the guards
  unreach    - a data blob no code ever touches
  closure    - code left unvisited by a tiny budget, reached by direct flow
  bankdata   - data read through the banked window of an MBC5 cart
"""
import sys, os

def rom(banks=2, mbc=0x00, ram=0x00, cgb=0x00, title=b"FIXT"):
    r = bytearray(banks * 0x4000)
    r[0x134:0x134 + len(title)] = title[:16]
    r[0x143] = cgb
    r[0x147] = mbc
    r[0x148] = {2: 0, 4: 1, 8: 2, 16: 3, 32: 4, 64: 5}.get(banks, 0)
    r[0x149] = ram
    return r

def w(r, addr, *bs):
    r[addr:addr + len(bs)] = bytes(bs)

def save(out, name, r):
    path = os.path.join(out, name + ".gb")
    with open(path, "wb") as f:
        f.write(r)
    print("wrote", path, len(r))

def build(out):
    # Ambiguous Sega image: Codemasters signature, code that banks slot zero.
    z80 = bytearray([0xff]) * 0x10000
    w(z80, 0, 0xc3, 0x00, 0x01)
    w(z80, 0x100, 0x3e, 2, 0x32, 0, 0, 0x76)
    w(z80, 0x8105, 0x21, 0xcd, 0xab, 0x76)
    w(z80, 0x7fe6, 1, 0, 0xff, 0xff)
    with open(os.path.join(out, "fix_sega_mapper.sms"), "wb") as f:
        f.write(z80)

    # SMS: YM2413 writes (key-on register $20, instrument register $30, rhythm $0E)
    z80 = bytearray([0xff]) * 0x8000
    w(z80, 0, 0xc3, 0x00, 0x01)
    w(z80, 0x100, 0x3e, 0x20, 0xd3, 0xf0, 0x3e, 0x10, 0xd3, 0xf1,
      0x3e, 0x30, 0xd3, 0xf0, 0xd3, 0xf1, 0x3e, 0x0e, 0xd3, 0xf0, 0xd3, 0xf1, 0x76)
    with open(os.path.join(out, "fix_sms_ym2413.sms"), "wb") as f:
        f.write(z80)

    # SMS: a routine in ROM ($0200) copied verbatim to RAM ($C000) then called (rtcode ROM source)
    z80 = bytearray([0xff]) * 0x8000
    w(z80, 0, 0xc3, 0x00, 0x01)                   # jp $0100
    w(z80, 0x100, 0x21, 0x00, 0x02, 0x11, 0x00, 0xc0, 0x06, 0x03)   # ld hl,$0200; ld de,$c000; ld b,3
    w(z80, 0x108, 0x7e, 0x12, 0x23, 0x13, 0x05, 0x20, 0xf9)          # loop: ld a,(hl); ld (de),a; inc hl; inc de; dec b; jr nz
    w(z80, 0x10f, 0xcd, 0x00, 0xc0, 0x76)                            # call $c000; halt
    w(z80, 0x200, 0x3e, 0x42, 0xc9)                                  # ld a,$42; ret
    with open(os.path.join(out, "fix_sms_ramsrc.sms"), "wb") as f:
        f.write(z80)

    # --- branch: cp/jr z with both sides + copy loop -------------------------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)          # nop; jp $0150
    w(r, 0x150, 0x3E, 0x01)                      # ld a,1
    w(r, 0x152, 0xFE, 0x01)                      # cp 1
    w(r, 0x154, 0x28, 0x06)                      # jr z,$015c
    w(r, 0x156, 0x06, 0x02)                      # ld b,2   (not-taken side)
    w(r, 0x158, 0x18, 0x0A)                      # jr $0164
    w(r, 0x15C, 0xCD, 0x80, 0x01)                # call $0180 (copy loop)
    w(r, 0x15F, 0x18, 0x03)                      # jr $0164
    w(r, 0x164, 0x76)                            # halt
    w(r, 0x180, 0x21, 0x00, 0x03)                # ld hl,$0300 (rom data)
    w(r, 0x183, 0x11, 0x00, 0x80)                # ld de,$8000 (vram)
    w(r, 0x186, 0x0E, 0x10)                      # ld c,16
    w(r, 0x188, 0x7E, 0x12, 0x23, 0x13, 0x0D, 0x20, 0xF9)  # loop: ld a,(hl); ld (de),a; inc hl; inc de; dec c; jr nz
    w(r, 0x18F, 0xC9)                            # ret
    w(r, 0x40, 0xD9)                             # reti
    w(r, 0x300, *range(1, 17))                   # 16 bytes of "tile" data
    save(out, "fix_branch", r)

    # --- streams: a decoder called with HL from a pointer table (S6) ---------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)          # nop; jp $0150
    w(r, 0x150, 0x21, 0x00, 0x03, 0xCD, 0x00, 0x02)   # ld hl,$0300; call $0200
    w(r, 0x156, 0x21, 0x10, 0x03, 0xCD, 0x00, 0x02)   # ld hl,$0310; call $0200
    w(r, 0x15C, 0x76)                            # halt
    w(r, 0x200, 0x2A, 0xB7, 0x20, 0xFC, 0xC9)    # decoder: ld a,(hl+); or a; jr nz,-4; ret
    w(r, 0x300, 1, 2, 3, 0)                      # stream 0 (played)
    w(r, 0x310, 5, 0)                            # stream 1 (played)
    w(r, 0x330, 7, 8, 0)                         # stream 2 (only in the table)
    w(r, 0x400, 0x00, 0x03, 0x10, 0x03, 0x30, 0x03)   # pointer table
    save(out, "fix_streams", r)

    # --- streams across banks: 3-byte (bank, ptr) table, a decoder fed through the banked window (S6)
    r = rom(4, mbc=0x01)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 1, 0xEA, 0x00, 0x20, 0x21, 0x00, 0x40, 0xCD, 0x00, 0x02)   # bank 1; hl=$4000; call
    w(r, 0x15B, 0x3E, 2, 0xEA, 0x00, 0x20, 0x21, 0x10, 0x40, 0xCD, 0x00, 0x02)   # bank 2; hl=$4010; call
    w(r, 0x166, 0x76)
    w(r, 0x200, 0x2A, 0xB7, 0x20, 0xFC, 0xC9)
    w(r, 0x4000, 1, 2, 3, 0)                     # bank 1 stream (played)
    w(r, 0x8010, 5, 0)                           # bank 2 stream (played)
    w(r, 0xC020, 7, 8, 0)                        # bank 3 stream (only in the table)
    w(r, 0x400, 1, 0x00, 0x40, 2, 0x10, 0x40, 3, 0x20, 0x40)   # (bank, ptr) table
    save(out, "fix_streams_bank", r)

    # --- sgb: one MLT_REQ-like command packet written on P1 ($FF00) ---------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)          # nop; jp $0150
    pkt = bytearray(16); pkt[0] = (0x11 << 3) | 1
    seq = [0x00, 0x30]                           # reset pulse, idle
    for i in range(128):
        seq += [0x10 if (pkt[i >> 3] >> (i & 7)) & 1 else 0x20, 0x30]
    seq += [0x20, 0x30]                          # stop bit
    code = bytearray()
    for v in seq:
        code += bytes([0x3E, v, 0xE0, 0x00])     # ld a,v; ldh ($00),a
    code += bytes([0x76])                        # halt
    w(r, 0x150, *code)
    save(out, "fix_sgb", r)

    # --- smc: code built in WRAM, executed, then one of its bytes is rewritten -
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)          # nop; jp $0150
    w(r, 0x150, 0x21, 0x00, 0xC0, 0x36, 0x00, 0x23, 0x36, 0xC9)   # ld hl,$C000; ld (hl),0; inc hl; ld (hl),$C9
    w(r, 0x158, 0xCD, 0x00, 0xC0)                # call $C000 (nop; ret)
    w(r, 0x15B, 0x21, 0x00, 0xC0, 0x36, 0x3C)    # ld hl,$C000; ld (hl),$3C  (rewrites an executed byte)
    w(r, 0x160, 0x76)                            # halt
    save(out, "fix_smc", r)

    # --- mbc5: switch to bank 2, call, switch to bank 3, call ----------------
    r = rom(4, mbc=0x19)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 0x02)                      # ld a,2
    w(r, 0x152, 0xEA, 0x00, 0x20)                # ld ($2000),a -> bank 2
    w(r, 0x155, 0xCD, 0x00, 0x40)                # call $4000 (bank 2 code)
    w(r, 0x158, 0x3E, 0x03)                      # ld a,3
    w(r, 0x15A, 0xEA, 0x00, 0x20)                # ld ($2000),a -> bank 3
    w(r, 0x15D, 0xCD, 0x00, 0x40)                # call $4000 (bank 3 code)
    w(r, 0x160, 0x76)                            # halt
    # bank 2 code (file offset 2*0x4000): writes a VRAM byte, returns
    w(r, 0x8000, 0x3E, 0x12)                     # ld a,$12
    w(r, 0x8002, 0xEA, 0x00, 0x80)               # ld ($8000),a
    w(r, 0x8005, 0xC9)                           # ret
    # bank 3 code (file offset 3*0x4000): writes another VRAM byte, returns
    w(r, 0xC000, 0x3E, 0x34)                     # ld a,$34
    w(r, 0xC002, 0xEA, 0x01, 0x80)               # ld ($8001),a
    w(r, 0xC005, 0xC9)                           # ret
    save(out, "fix_mbc5", r)

    # --- mbc1m1: MBC1 mode 1 remaps the ROM0 window ($0000-$3FFF) --------------
    # 1 MiB (64 banks).  Mode 1 + upper register 1 makes the window show bank
    # $20 while the code runs there: the instruction after the switch lives in
    # bank $20 at the same address.
    r = rom(64, mbc=0x01)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 0x01)                      # ld a,1
    w(r, 0x152, 0xEA, 0x00, 0x60)                # ld ($6000),a -> mode 1
    w(r, 0x155, 0xEA, 0x00, 0x40)                # ld ($4000),a -> upper=1: window shows bank $20
    w(r, 0x158, 0x3E, 0x56)                      # (bank 0 copy, never reached once switched)
    w(r, 0x20 * 0x4000 + 0x158, 0x3E, 0x78)      # bank $20: ld a,$78
    w(r, 0x20 * 0x4000 + 0x15A, 0xEA, 0x02, 0x80) # ld ($8002),a
    w(r, 0x20 * 0x4000 + 0x15D, 0x76)            # halt
    save(out, "fix_mbc1m1", r)

    # --- closure: code the budget never reaches, found by direct flow ---------
    r = rom(4, mbc=0x19)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 0x02)                      # ld a,2
    w(r, 0x152, 0xEA, 0x00, 0x20)                # ld ($2000),a -> bank 2
    w(r, 0x155, 0xCD, 0x00, 0x40)                # call $4000
    w(r, 0x158, 0xC3, 0x30, 0x40)                # jp $4030 (bank 2, cross-bank edge)
    w(r, 0x8000, 0x3E, 0x01)                     # $4000 ld a,1
    w(r, 0x8002, 0xFE, 0x01)                     # cp 1
    w(r, 0x8004, 0x28, 0x03)                     # jr z,$4009
    w(r, 0x8006, 0x06, 0x02)                     # ld b,2   (not-taken side)
    w(r, 0x8008, 0xC9)                           # ret
    w(r, 0x8009, 0x0E, 0x03)                     # ld c,3
    w(r, 0x800B, 0xCD, 0x20, 0x40)               # call $4020
    w(r, 0x800E, 0xC9)                           # ret
    w(r, 0x8020, 0x3E, 0x55)                     # ld a,$55
    w(r, 0x8022, 0xEA, 0x00, 0x80)               # ld ($8000),a
    w(r, 0x8025, 0xC9)                           # ret
    w(r, 0x8030, 0x3E, 0x09)                     # $4030 ld a,9
    w(r, 0x8032, 0xC9)                           # ret
    save(out, "fix_closure", r)

    # --- keyirq: IE arms only the joypad IRQ; the explorer must simulate
    # key presses, nothing else can raise IF bit 4 --------------------------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 0x10)                      # ld a,$10 (joypad bit)
    w(r, 0x152, 0xEA, 0xFF, 0xFF)                # ld ($FFFF),a -> IE = joypad
    w(r, 0x155, 0x76)                            # halt (IME 0: nothing dispatches)
    save(out, "fix_keyirq", r)

    # --- p1poll: keys read straight out of $FF00 (polling, no IRQ); the
    # explorer must fork on the read and press each key ---------------------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 0x20)                      # ld a,$20 (select button keys)
    w(r, 0x152, 0xE0, 0x00)                      # ldh ($00),a  P1 select
    w(r, 0x154, 0xF0, 0x00)                      # ldh a,($00)  poll P1
    w(r, 0x156, 0xE6, 0x08)                      # and $08      Start pressed? (0 = yes)
    w(r, 0x158, 0xCA, 0x70, 0x01)                # jp z,$170    (only with the fork)
    w(r, 0x15B, 0x18, 0xFE)                      # jr -2: poll again (natural path)
    w(r, 0x170, 0x3E, 0x99)                      # ld a,$99
    w(r, 0x172, 0xEA, 0x00, 0x80)                # ld ($8000),a
    w(r, 0x175, 0x76)                            # halt
    save(out, "fix_p1poll", r)

    # --- dma: OAM DMA from ROM source ----------------------------------------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 0x30)                      # ld a,$30 -> source page $3000
    w(r, 0x152, 0xE0, 0x46)                      # ldh ($46),a  (OAM DMA)
    w(r, 0x154, 0x76)                            # halt
    w(r, 0x3000, *([0xAA] * 0xA0))               # 160 bytes of sprite data
    save(out, "fix_dma", r)

    # --- irq: IE/IF set by hand, EI, then the handler via IRQ fork -----------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 0x01)                      # ld a,1
    w(r, 0x152, 0xE0, 0xFF)                      # ldh ($FF),a  IE = vblank
    w(r, 0x154, 0xE0, 0x0F)                      # ldh ($0F),a  IF = vblank pending
    w(r, 0x156, 0xFB)                            # ei
    w(r, 0x157, 0x00)                            # nop (ime effective here)
    w(r, 0x158, 0x76)                            # halt
    w(r, 0x40, 0x3E, 0x77, 0xEA, 0x00, 0x80, 0xD9)  # ld a,$77; ld ($8000),a; reti
    save(out, "fix_irq", r)

    # --- jumptable: jp hl through a table, both handlers ---------------------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 0x05)                      # ld a,5
    w(r, 0x152, 0xFE, 0x05)                      # cp 5
    w(r, 0x154, 0x28, 0x03)                      # jr z,$0159
    w(r, 0x156, 0x21, 0x10, 0x02)                # ld hl,$0210 (handler B)
    w(r, 0x159, 0x21, 0x20, 0x02)                # ld hl,$0220 (handler A)
    w(r, 0x15C, 0xE9)                            # jp hl
    w(r, 0x210, 0x3E, 0x11, 0xEA, 0x00, 0x80, 0x76)  # handler B: vram $11, halt
    # handler A: walks the pointer table at $0230, then jp hl -> handler C
    w(r, 0x220, 0x21, 0x30, 0x02)                # ld hl,$0230
    w(r, 0x223, 0x7E)                            # ld a,(hl)   a = low byte
    w(r, 0x224, 0x23)                            # inc hl
    w(r, 0x225, 0x66)                            # ld h,(hl)   h = high byte
    w(r, 0x226, 0x6F)                            # ld l,a      hl = pointer
    w(r, 0x227, 0xE9)                            # jp hl
    w(r, 0x230, 0x40, 0x02)                      # pointer -> $0240
    w(r, 0x240, 0x3E, 0x33, 0xEA, 0x00, 0x80, 0x76)  # handler C: vram $33, halt
    save(out, "fix_jumptable", r)

    # --- dispatch: indexed jp hl, three table entries, one taken ---------------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x11, 0x02, 0x00)                # ld de,2  (index 1, entry two)
    w(r, 0x153, 0x21, 0x30, 0x02)                # ld hl,$0230
    w(r, 0x156, 0x19)                            # add hl,de
    w(r, 0x157, 0x2A)                            # ld a,(hl+)
    w(r, 0x158, 0x66)                            # ld h,(hl)
    w(r, 0x159, 0x6F)                            # ld l,a
    w(r, 0x15A, 0xE9)                            # jp hl
    w(r, 0x230, 0x50, 0x02, 0x60, 0x02, 0x70, 0x02)   # table: $0250 $0260 $0270
    w(r, 0x250, 0x3E, 0x01, 0x76)                # entry 0 (never taken)
    w(r, 0x260, 0x3E, 0x02, 0x76)                # entry 1 (taken)
    w(r, 0x270, 0x3E, 0x03, 0x76)                # entry 2 (never taken)
    save(out, "fix_dispatch", r)

    # --- dispatch3: indexed jp hl through a stride-3 table (addr, addr, bank);
    # the taken entry lives in bank 1: only the stride-3 proof reaches it ------
    r = rom(2, mbc=0x19)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 0x01, 0xEA, 0x00, 0x20)    # ld a,1; ld ($2000),a  (bank 1 in)
    w(r, 0x155, 0x11, 0x03, 0x00)                # ld de,3  (index 1, stride 3)
    w(r, 0x158, 0x21, 0x30, 0x02)                # ld hl,$0230
    w(r, 0x15B, 0x19)                            # add hl,de
    w(r, 0x15C, 0x2A)                            # ld a,(hl+)
    w(r, 0x15D, 0x66)                            # ld h,(hl)
    w(r, 0x15E, 0x6F)                            # ld l,a
    w(r, 0x15F, 0xE9)                            # jp hl
    # stride-3 table: $0250@0 (ROM0, bank 0), $4250@1 (window, bank 1), $0260@0
    w(r, 0x230, 0x50, 0x02, 0x00, 0x50, 0x42, 0x01, 0x60, 0x02, 0x00)
    w(r, 0x250, 0x3E, 0x01, 0x76)                # entry 0 bank 0 (never taken): ld a,1; halt
    w(r, 0x4250, 0x3E, 0x02, 0xEA, 0x00, 0x80, 0x76)  # entry 1 bank 1 (taken): vram $02, halt
    w(r, 0x260, 0x3E, 0x03, 0x76)                # entry 2 bank 0 (never taken)
    save(out, "fix_dispatch3", r)

    # --- residual: a dispatch whose sibling entry is 60 NOPs (the 48-instruction
    # decode window refuses it, so the table stays unproven); the taken handler
    # ends in an unconditional jp right before the sibling: only the residual
    # targeting (predecessor + forced entry) reaches it --------------------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x11, 0x00, 0x00)                # ld de,0  (index 0, entry one)
    w(r, 0x153, 0x21, 0x40, 0x02)                # ld hl,$0240
    w(r, 0x156, 0x19)                            # add hl,de
    w(r, 0x157, 0x2A)                            # ld a,(hl+)
    w(r, 0x158, 0x66)                            # ld h,(hl)
    w(r, 0x159, 0x6F)                            # ld l,a
    w(r, 0x15A, 0xE9)                            # jp hl
    w(r, 0x240, 0x50, 0x02, 0x58, 0x02, 0xA0, 0x02, 0xA0, 0x02)  # 4-word table: detected
                                                 # as a pointer table, so the pointer scan
                                                 # evicts its targets (only the residual
                                                 # targeting can reach the sibling)
    w(r, 0x250, 0x3E, 0x01, 0xEA, 0x00, 0x80, 0xC3, 0xA0, 0x02)  # taken: ld a,1; ld ($8000),a; jp $02A0
    w(r, 0x258, *([0x7C, 0x7D, 0x7E, 0x7F] * 15))            # sibling: 60x ld a,h / ld a,l (no transfer in the
                                                 # decode window, not a 00/FF fill, and no uniform
                                                 # word run: no pad or pointer-table inference)
    w(r, 0x294, 0x76)                            # sibling halt
    w(r, 0x2A0, 0x76)                            # taken handler's landing halt
    save(out, "fix_residual", r)

    # --- rstdispatch: rst $28 jump table inline after the call site (SML2 idiom) -----
    r = rom(2)
    w(r, 0x28, 0x87, 0xE1, 0x5F, 0x16, 0x00, 0x19, 0x5E, 0x23, 0x56, 0xD5, 0xE1, 0xE9)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 0x01, 0xEF)                # ld a,1; rst $28
    w(r, 0x153, 0x60, 0x02, 0x70, 0x02, 0x80, 0x02)   # inline table: three entries
    w(r, 0x260, 0x3E, 0x01, 0x76)                # entry 0 (never taken)
    w(r, 0x270, 0x3E, 0x02, 0x76)                # entry 1 (taken)
    w(r, 0x280, 0x3E, 0x03, 0x76)                # entry 2 (never taken)
    save(out, "fix_rstdispatch", r)

    # --- calldispatch: the same inline-table dispatcher reached by a plain `call` --
    r = rom(2)
    w(r, 0x200, 0x87, 0xE1, 0x5F, 0x16, 0x00, 0x19, 0x5E, 0x23, 0x56, 0xD5, 0xE1, 0xE9)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 0x01, 0xCD, 0x00, 0x02)    # ld a,1; call $0200
    w(r, 0x155, 0x60, 0x02, 0x70, 0x02, 0x80, 0x02)
    w(r, 0x260, 0x3E, 0x01, 0x76)
    w(r, 0x270, 0x3E, 0x02, 0x76)
    w(r, 0x280, 0x3E, 0x03, 0x76)
    save(out, "fix_calldispatch", r)

    # --- copytable: a pointer table copied ROM->WRAM, then indexed there ----------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x21, 0x30, 0x02, 0x11, 0x00, 0xC0, 0x06, 0x06, 0x2A, 0x12, 0x13, 0x05, 0x20, 0xFA)
    w(r, 0x15E, 0x21, 0x00, 0xC0, 0x2A, 0x66, 0x6F, 0xE9)   # ld hl,$C000; ld a,(hl+); ld h,(hl); ld l,a; jp hl
    w(r, 0x230, 0x50, 0x02, 0x60, 0x02, 0x70, 0x02)
    w(r, 0x250, 0x3E, 0x01, 0x76)
    w(r, 0x260, 0x3E, 0x02, 0x76)
    w(r, 0x270, 0x3E, 0x03, 0x76)
    save(out, "fix_copytable", r)

    # --- lywait: wait on LY == $64 (not one of the stock probe values), then code ----
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0xF0, 0x44, 0xFE, 0x64, 0x20, 0xFA)   # L: ldh a,($44); cp $64; jr nz,L
    w(r, 0x156, 0x3E, 0x07, 0x76)                     # reached only when LY == $64
    save(out, "fix_lywait", r)

    # --- hlwait: wait on TIMA through ld a,(hl) with HL=$FF05 ----------------------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x21, 0x05, 0xFF, 0x7E, 0xFE, 0x37, 0x20, 0xFB)  # ld hl,$FF05; L: ld a,(hl); cp $37; jr nz,L
    w(r, 0x158, 0x3E, 0x08, 0x76)
    save(out, "fix_hlwait", r)

    # --- loop: infinite tight loop -------------------------------------------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x18, 0xFE)                      # jr -2 (self)
    save(out, "fix_loop", r)

    # --- unreach: code, untouched data blob, more code -----------------------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 0x99, 0xEA, 0x00, 0x80, 0x76)  # vram $99, halt
    for i in range(0x100):                       # data blob nobody touches
        r[0x200 + i] = (i * 7 + 3) & 0xFF
    w(r, 0x400, 0x3E, 0x88, 0xEA, 0x01, 0x80, 0x76)  # unreachable code
    save(out, "fix_unreach", r)

    # --- bankdata: data read through the MBC5 banked window ------------------
    r = rom(4, mbc=0x19)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 0x02)                      # ld a,2
    w(r, 0x152, 0xEA, 0x00, 0x20)                # ld ($2000),a -> bank 2
    w(r, 0x155, 0x21, 0x00, 0x40)                # ld hl,$4000 (bank 2 offset 0)
    w(r, 0x158, 0x11, 0x00, 0x80)                # ld de,$8000
    w(r, 0x15B, 0x0E, 0x08)                      # ld c,8
    w(r, 0x15D, 0x7E, 0x12, 0x23, 0x13, 0x0D, 0x20, 0xF9)  # copy loop
    w(r, 0x164, 0x76)                            # halt
    w(r, 0x8000, *range(1, 9))                   # 8 bytes of data in bank 2 (file offset)
    save(out, "fix_bankdata", r)

    # --- vblank: HALT waits the session clock, VBlank handler, JOYP branch ---
    # The forking explorer cannot press keys: only play sessions take both
    # JOYP branches (worker 0 holds A, worker 1 holds START).
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 0x01)                      # ld a,1
    w(r, 0x152, 0xE0, 0xFF)                      # ldh ($FF),a  IE = vblank
    w(r, 0x154, 0xFB)                            # ei
    w(r, 0x155, 0x00)                            # nop (IME effective here)
    w(r, 0x156, 0x76)                            # main: halt  (session clock wakes it)
    w(r, 0x157, 0x3E, 0x10)                      # ld a,$10    select button keys
    w(r, 0x159, 0xE0, 0x00)                      # ldh ($00),a
    w(r, 0x15B, 0xF0, 0x00)                      # ldh a,($00) read JOYP
    w(r, 0x15D, 0x1F)                            # cpl         active-low -> 1 = pressed
    w(r, 0x15E, 0xE6, 0x01)                      # and $01     A pressed?
    w(r, 0x160, 0xFE, 0x01)                      # cp 1
    w(r, 0x162, 0x20, 0x03)                      # jr nz,+3
    w(r, 0x164, 0xCD, 0x80, 0x01)                # call $0180  path A
    w(r, 0x167, 0xF0, 0x00)                      # ldh a,($00)
    w(r, 0x169, 0x1F)                            # cpl
    w(r, 0x16A, 0xE6, 0x08)                      # and $08     START pressed?
    w(r, 0x16C, 0xFE, 0x08)                      # cp 8
    w(r, 0x16E, 0x20, 0x03)                      # jr nz,+3
    w(r, 0x170, 0xCD, 0xA0, 0x01)                # call $01A0  path B
    w(r, 0x173, 0xC3, 0x56, 0x01)                # jp main
    w(r, 0x40, 0x3E, 0x77, 0xEA, 0x00, 0x80, 0xD9)  # vblank handler: vram $77, reti
    w(r, 0x180, 0x3E, 0xAA, 0xEA, 0x02, 0x80, 0xC9)  # path A: vram $AA
    w(r, 0x1A0, 0x3E, 0xBB, 0xEA, 0x03, 0x80, 0xC9)  # path B: vram $BB
    save(out, "fix_vblank", r)

    # --- copy1: a pure 1:1 ROM->VRAM copy loop (verbatim proof) ---------------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x21, 0x00, 0x40)                # ld hl,$4000 (bank 1 data)
    w(r, 0x153, 0x11, 0x00, 0x80)                # ld de,$8000
    w(r, 0x156, 0x01, 0x00, 0x01)                # ld bc,$0100
    w(r, 0x159, 0x2A)                            # loop: ld a,(hl+)
    w(r, 0x15A, 0x12)                            # ld (de),a
    w(r, 0x15B, 0x13)                            # inc de
    w(r, 0x15C, 0x0B)                            # dec bc
    w(r, 0x15D, 0x78)                            # ld a,b
    w(r, 0x15E, 0xB1)                            # or c
    w(r, 0x15F, 0x20, 0xF8)                      # jr nz,$0159
    w(r, 0x161, 0x76)                            # halt
    for i in range(0x100):                       # the "tile data" in bank 1
        r[0x4000 + i] = (i * 13 + 5) & 0xFF
    save(out, "fix_copy1", r)

    # --- wramexec: a 16-byte routine copied ROM->WRAM, then called there -------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x21, 0x00, 0x40)                # ld hl,$4000
    w(r, 0x153, 0x11, 0x00, 0xC0)                # ld de,$C000
    w(r, 0x156, 0x01, 0x10, 0x00)                # ld bc,$0010
    w(r, 0x159, 0x2A, 0x12, 0x13, 0x0B, 0x78, 0xB1, 0x20, 0xF8)   # copy loop
    w(r, 0x161, 0xCD, 0x00, 0xC0)                # call $C000 (the copy)
    w(r, 0x164, 0x76)                            # halt
    w(r, 0x4000, 0x3E, 0x42, 0xEA, 0x00, 0xD0, 0xC9)   # ld a,$42 ; ld ($D000),a ; ret
    save(out, "fix_wramexec", r)

    # --- wramgen: the routine is stored XOR $5A and decoded while copied to WRAM:
    # no verbatim ROM source, but the producer region is the data read -----------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x21, 0x00, 0x40)                # ld hl,$4000
    w(r, 0x153, 0x11, 0x00, 0xC0)                # ld de,$C000
    w(r, 0x156, 0x06, 0x06)                      # ld b,6
    w(r, 0x158, 0x00)
    w(r, 0x159, 0x2A, 0xEE, 0x5A, 0x12, 0x13, 0x05, 0x20, 0xF8)   # ld a,(hl+) ; xor $5A ; ld (de),a ; inc de ; dec b ; jr nz
    w(r, 0x161, 0xCD, 0x00, 0xC0)                # call $C000 (the decoded routine)
    w(r, 0x164, 0x76)                            # halt
    w(r, 0x4000, *[b ^ 0x5A for b in (0x3E, 0x42, 0xEA, 0x00, 0xD0, 0xC9)])
    save(out, "fix_wramgen", r)

    # --- lywait: a vblank wait loop on LY; LY is static for the explorer, only the
    # event probe (LY = 144 variant) gets out of it to the code at $0180 ----------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0xF0, 0x44)                      # wait: ldh a,($44)
    w(r, 0x152, 0xFE, 0x90)                      # cp 144
    w(r, 0x154, 0x20, 0xFA)                      # jr nz,wait
    w(r, 0x156, 0xCD, 0x80, 0x01)                # call $0180 (after vblank)
    w(r, 0x159, 0x76)                            # halt
    w(r, 0x180, 0x3E, 0x77, 0xEA, 0x00, 0x80, 0xC9)   # ld a,$77 ; ld ($8000),a ; ret
    save(out, "fix_lywait", r)

    # --- jptable: ld hl,table ; ld a,[hl+] ; ld h,[hl] ; ld l,a ; jp hl.  Only the first
    # entry runs; the other two handlers are reached by the S4 operand table scan ---
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x21, 0x70, 0x01, 0x2A, 0x66, 0x6F, 0xE9)
    w(r, 0x170, 0x80, 0x01, 0x90, 0x01, 0xA0, 0x01)
    for t in (0x180, 0x190, 0x1A0):
        w(r, t, 0x3C, 0x00, 0x18, 0xFE)          # inc a ; nop ; jr $
    save(out, "fix_jptable", r)

    # --- jptable_de: base in DE, index in HL (ld de,tbl ; ld hl,0 ; add hl,de ; ld a,(hl+) ; ld h,(hl) ; ld l,a ; jp hl) ---
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x11, 0x70, 0x01, 0x21, 0x00, 0x00, 0x19, 0x2A, 0x66, 0x6F, 0xE9)
    w(r, 0x170, 0x80, 0x01, 0x90, 0x01, 0xA0, 0x01)
    for t in (0x180, 0x190, 0x1A0):
        w(r, t, 0x3C, 0x00, 0x18, 0xFE)
    save(out, "fix_jptable_de", r)

    # --- provde: pointer read through DE (ld de,tbl ; ld a,(de) ; ld l,a ; inc de ; ld a,(de) ; ld h,a ; jp hl)
    # followed by an unreachable-by-execution branch: the jp hl target is proven statically (S3 BC/DE) ---
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x11, 0x70, 0x01, 0x1A, 0x6F, 0x13, 0x1A, 0x67, 0xE9)
    w(r, 0x170, 0x80, 0x01)
    w(r, 0x180, 0x3C, 0x00, 0x18, 0xFE)
    save(out, "fix_provde", r)

    # --- provbyte: the pointer devised byte by byte through BC — S3 byte-granular.
    # The fork: A comes from an IO read (unknown), so the z side's ld b,a leaves
    # B unknown and its jp hl stays unresolved (negative witness, "1 partial");
    # the nz side devises HL from ld b,$01/ld c,$90 and its jp hl is proven -> $0190 ---
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0xF0, 0x44, 0xFE, 0x00, 0x28, 0x06, 0x06, 0x01, 0x0E, 0x90,
       0x18, 0x03, 0x78, 0x0E, 0x90, 0x60, 0x69, 0xE9)
    w(r, 0x190, 0x3C, 0x00, 0x18, 0xFE)
    save(out, "fix_provbyte", r)

    # --- hdma: CGB general-purpose HDMA, 256 bytes $4000 -> $8000 -------------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 0x40, 0xE0, 0x51, 0xAF, 0xE0, 0x52, 0xAF, 0xE0, 0x53, 0xAF, 0xE0, 0x54,
       0x3E, 0x0F, 0xE0, 0x55, 0x76)
    for i in range(0x100):
        r[0x4000 + i] = (i * 7 + 3) & 0xFF
    save(out, "fix_hdma", r)

    # --- cgbbank: VBK=1 then SVBK=3 (only a CGB-mode core observes them) -------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 0x01, 0xE0, 0x4F, 0x3E, 0x03, 0xE0, 0x70, 0x76)
    save(out, "fix_cgbbank", r)

    # --- apu: a ROM byte stored to wave RAM, another to NR12 -------------------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0xFA, 0x00, 0x40, 0xE0, 0x30, 0xFA, 0x01, 0x40, 0xE0, 0x12, 0x76)
    r[0x4000] = 0x5A
    r[0x4001] = 0xC4
    save(out, "fix_apu", r)

    # --- probes: serial start + MBC3 RTC register select -----------------------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 0x41, 0xE0, 0x01, 0x3E, 0x81, 0xE0, 0x02,
       0x3E, 0x08, 0xEA, 0x00, 0x40, 0xF0, 0x04, 0xF0, 0x41, 0xF0, 0x44, 0x76)
    save(out, "fix_probes", r)

    # --- pal: a ROM byte stored to BCPD ----------------------------------------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0xFA, 0x00, 0x40, 0xE0, 0x69, 0x76)
    r[0x4000] = 0x1F
    save(out, "fix_pal", r)

    # --- copyx: same loop but every byte is XOR-transformed -------------------
    # (a decompressor shape: the ranges match, the bytes do not)
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x21, 0x00, 0x40)                # ld hl,$4000
    w(r, 0x153, 0x11, 0x00, 0x80)                # ld de,$8000
    w(r, 0x156, 0x01, 0x00, 0x01)                # ld bc,$0100
    w(r, 0x159, 0x2A)                            # loop: ld a,(hl+)
    w(r, 0x15A, 0xEE, 0xFF)                      # xor $FF   (the transformation)
    w(r, 0x15C, 0x12)                            # ld (de),a
    w(r, 0x15D, 0x13)                            # inc de
    w(r, 0x15E, 0x0B)                            # dec bc
    w(r, 0x15F, 0x78)                            # ld a,b
    w(r, 0x160, 0xB1)                            # or c
    w(r, 0x161, 0x20, 0xF6)                      # jr nz,$0159
    w(r, 0x163, 0x76)                            # halt
    for i in range(0x100):
        r[0x4000 + i] = (i * 29 + 11) & 0xFF
    save(out, "fix_copyx", r)

    # --- nonsense: jump into echo RAM, jump into verbatim data, RET with a
    # stack pointer outside WRAM/HRAM — each is one nonsense eviction ---------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    # echo RAM: jp $E000 (2 bytes of 0xFF there -> the slide guard would eat
    # it, so seed a real opcode: copy code to WRAM through the echo?  no —
    # the echo hit itself is the eviction, before anything runs)
    w(r, 0x150, 0x21, 0x00, 0xE0)                # ld hl,$E000
    w(r, 0x153, 0xE9)                            # jp (hl)
    w(r, 0x154, 0x76)                            # halt
    save(out, "fix_nsecho", r)

    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    # verbatim data: copy $0200..$02FF to VRAM (proven verbatim), then a
    # second forced path jumps into it
    w(r, 0x150, 0x21, 0x00, 0x02)                # ld hl,$0200
    w(r, 0x153, 0x11, 0x00, 0x80)                # ld de,$8000
    w(r, 0x156, 0x01, 0x00, 0x01)                # ld bc,$0100
    w(r, 0x159, 0x2A)                            # loop: ld a,(hl+)
    w(r, 0x15A, 0x12)                            # ld (de),a
    w(r, 0x15B, 0x13)                            # inc de
    w(r, 0x15C, 0x0B)                            # dec bc
    w(r, 0x15D, 0x78)                            # ld a,b
    w(r, 0x15E, 0xB1)                            # or c
    w(r, 0x15F, 0x20, 0xF8)                      # jr nz,$0159
    w(r, 0x161, 0x21, 0x00, 0x02)                # ld hl,$0200
    w(r, 0x164, 0xE9)                            # jp (hl)   -> into proven data
    for i in range(0x100):
        r[0x200 + i] = (i * 31 + 7) & 0xFF
    save(out, "fix_nsdata", r)

    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    # RET with the stack pointer in ROM: senseless return
    w(r, 0x150, 0x31, 0x00, 0x01)                # ld sp,$0100
    w(r, 0x153, 0xC9)                            # ret
    w(r, 0x154, 0x76)                            # halt
    save(out, "fix_nsstack", r)

def build_techniques_8bit(out):
    # SM83 (GB) technique fixtures 3/4/7 on work RAM $C1xx..$C3xx
    def gb(name, code, table=None):
        r = rom(2)
        w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
        r[0x150:0x150 + len(code)] = code
        if table:
            r[0x220:0x220 + len(table)] = table
        save(out, name, r)
    gb("fix_gb_save", bytes.fromhex("fa00c1 feaa 2004 3e01 1802 3e02 18fe"))
    gb("fix_gb_rng", bytes.fromhex("fa00c2 ee55 ea00c2 feaa 2802 18f2 3e01 18fe"))
    gb("fix_gb_index", bytes.fromhex("fa00c3 5f 1600 212002 19 7e fe03 2804 3e01 18fe 3e02 18fe"), bytes([0, 0, 0, 3] + [0] * 12))
    # HuC6280 (PCE) technique fixtures: MPR1 = work RAM ($2000..), code at $E000, table at $E020
    def pce(name, code, table=None):
        p = bytearray(0x4000)
        pro = bytes.fromhex("a9f8 5302")
        p[0:len(pro) + len(code)] = pro + code
        if table:
            p[0x20:0x20 + len(table)] = table
        p[0x1FFE], p[0x1FFF] = 0x00, 0xE0
        with open(os.path.join(out, name + ".pce"), "wb") as f:
            f.write(p)
        print("wrote", name)
    pce("fix_pce_save", bytes.fromhex("ad0020 c9aa d004 a901 8002 a902 80fe"))
    pce("fix_pce_rng", bytes.fromhex("ad0020 4955 8d0020 c9aa f003 4c04e0 a901 80fe"))
    pce("fix_pce_index", bytes.fromhex("ad0021 aa bd20e0 c903 f004 a901 8002 a902 80fe"), bytes([0, 0, 0, 3] + [0] * 12))

def md_rom(size=0x1000):
    r = bytearray(size)
    r[0:4] = (0x00FFFE00).to_bytes(4, "big")      # initial SSP
    r[4:8] = (0x200).to_bytes(4, "big")           # reset vector
    for i in range(2, 64):                        # every other vector: reset code
        r[i * 4:i * 4 + 4] = (0x200).to_bytes(4, "big")
    r[0x100:0x110] = b"SEGA MEGA DRIVE "
    r[0x120:0x130] = b"DEEPEEL FIXTURE "
    r[0x1F0:0x1F3] = b"JUE"
    return r

def save_md(out, name, r):
    path = os.path.join(out, name + ".md")
    with open(path, "wb") as f:
        f.write(r)
    print("wrote", path, len(r))

def build_md(out):
    header = bytearray(64)
    header[:16] = b"Gens Movie TESTA"
    header[20:22] = b"33"
    with open(os.path.join(out, "fix_md_movie.gmv"), "wb") as f:
        f.write(header + bytes([0x7F, 0xFF, 0xFF, 0xF7, 0xFF, 0xFF]))
    import zipfile
    with zipfile.ZipFile(os.path.join(out, "fix_md_movie.bk2"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("Header.txt", "MovieVersion BizHawk v2.0\nPlatform GEN\nSHA1 00000000000000000000000000000000\n")
        z.writestr("Input Log.txt", "[Input]\nLogKey:#Power|Reset|#P1 Up|P1 Down|P1 Left|P1 Right|P1 A|P1 B|P1 C|P1 Start|P1 X|P1 Y|P1 Z|P1 Mode|\n"
                   + "|..|............|............|\n" * 3 + "|..|.......S....|............|\n" + "[/Input]\n")
    header[22] = 0x40
    with open(os.path.join(out, "fix_md_movie_state.gmv"), "wb") as f:
        f.write(header + bytes([0xFF, 0xFF, 0xFF]))
    # --- s4: moveq #0,d0 ; jmp 4(pc,d0.w) -> table of two bra.w handlers; only entry 0 runs ----
    r = md_rom()
    r[0x200:0x202] = bytes([0x70, 0x00])                 # moveq #0,d0
    r[0x202:0x206] = bytes([0x4E, 0xFB, 0x00, 0x04])     # jmp 4(pc,d0.w): base = $204 + 4 = $208
    r[0x208:0x20C] = bytes([0x60, 0x00, 0x00, 0x10])     # bra.w $21A
    r[0x20C:0x210] = bytes([0x60, 0x00, 0x00, 0x18])     # bra.w $226
    r[0x21A:0x21C] = bytes([0x4E, 0x71]); r[0x21C:0x21E] = bytes([0x60, 0xFE])
    r[0x226:0x228] = bytes([0x4E, 0x71]); r[0x228:0x22A] = bytes([0x4E, 0x71]); r[0x22A:0x22C] = bytes([0x60, 0xFE])
    save_md(out, "fix_md_s4", r)
    # --- s4 with jsr: moveq #0,d0 ; jsr 4(pc,d0.w) -> same table; only entry 0 runs ----
    r = md_rom()
    r[0x200:0x202] = bytes([0x70, 0x00])
    r[0x202:0x206] = bytes([0x4E, 0xBB, 0x00, 0x04])     # jsr 4(pc,d0.w)
    r[0x208:0x20C] = bytes([0x60, 0x00, 0x00, 0x10])
    r[0x20C:0x210] = bytes([0x60, 0x00, 0x00, 0x18])
    r[0x21A:0x21C] = bytes([0x4E, 0x71]); r[0x21C:0x21E] = bytes([0x60, 0xFE])
    r[0x226:0x228] = bytes([0x4E, 0x71]); r[0x228:0x22A] = bytes([0x4E, 0x71]); r[0x22A:0x22C] = bytes([0x60, 0xFE])
    save_md(out, "fix_md_s4j", r)
    # --- s4 with an address register: lea $210,a0 ; moveq #0,d0 ; jmp 0(a0,d0.w) -> table at $210 ----
    r = md_rom()
    r[0x200:0x206] = bytes([0x41, 0xF9, 0x00, 0x00, 0x02, 0x10])   # lea $00000210,a0
    r[0x206:0x208] = bytes([0x70, 0x00])                           # moveq #0,d0
    r[0x208:0x20C] = bytes([0x4E, 0xF0, 0x00, 0x00])               # jmp 0(a0,d0.w)
    r[0x210:0x214] = bytes([0x60, 0x00, 0x00, 0x08])               # bra.w $21A
    r[0x214:0x218] = bytes([0x60, 0x00, 0x00, 0x10])               # bra.w $226
    r[0x21A:0x21C] = bytes([0x4E, 0x71]); r[0x21C:0x21E] = bytes([0x60, 0xFE])
    r[0x226:0x228] = bytes([0x4E, 0x71]); r[0x228:0x22A] = bytes([0x4E, 0x71]); r[0x22A:0x22C] = bytes([0x60, 0xFE])
    save_md(out, "fix_md_s4a", r)
    # --- word-offset table: moveq #0,d0 ; move.w $20C(pc,d0.w),d1 ; jmp $20C(pc,d1.w) ----
    r = md_rom()
    r[0x200:0x202] = bytes([0x70, 0x00])                           # moveq #0,d0
    r[0x202:0x206] = bytes([0x32, 0x3B, 0x00, 0x08])               # move.w 8(pc,d0.w),d1  (base $204+8 = $20C)
    r[0x206:0x20A] = bytes([0x4E, 0xFB, 0x10, 0x04])               # jmp 4(pc,d1.w)        (base $208+4 = $20C)
    r[0x20C:0x210] = bytes([0x00, 0x08, 0x00, 0x10])               # offsets: -> $214, -> $21C
    r[0x214:0x216] = bytes([0x4E, 0x71]); r[0x216:0x218] = bytes([0x60, 0xFE])
    r[0x21C:0x21E] = bytes([0x4E, 0x71]); r[0x21E:0x220] = bytes([0x60, 0xFE])
    save_md(out, "fix_md_s4w", r)
    # --- pointer table behind a bare jmp (a1): movea.l 8(pc,d0.w),a1 ; jmp (a1) ----
    r = md_rom()
    r[0x200:0x202] = bytes([0x70, 0x00])                           # moveq #0,d0
    r[0x202:0x206] = bytes([0x22, 0x7B, 0x00, 0x08])               # movea.l 8(pc,d0.w),a1 (base $204+8 = $20C)
    r[0x206:0x208] = bytes([0x4E, 0xD1])                           # jmp (a1)
    r[0x20C:0x214] = bytes([0, 0, 0x02, 0x14, 0, 0, 0x02, 0x1C])   # longs: $214, $21C
    r[0x214:0x216] = bytes([0x4E, 0x71]); r[0x216:0x218] = bytes([0x60, 0xFE])
    r[0x21C:0x21E] = bytes([0x4E, 0x71]); r[0x21E:0x220] = bytes([0x60, 0xFE])
    save_md(out, "fix_md_s4p", r)
    # --- same, base in a2 (not a0): lea $210,a2 ; moveq #0,d0 ; movea.l 0(a2,d0.w),a1 ; jmp (a1) ----
    r = md_rom()
    r[0x200:0x206] = bytes([0x45, 0xF9, 0x00, 0x00, 0x02, 0x10])   # lea $210,a2
    r[0x206:0x208] = bytes([0x70, 0x00])                           # moveq #0,d0
    r[0x208:0x20C] = bytes([0x22, 0x72, 0x00, 0x00])               # movea.l 0(a2,d0.w),a1
    r[0x20C:0x20E] = bytes([0x4E, 0xD1])                           # jmp (a1)
    r[0x210:0x218] = bytes([0, 0, 0x02, 0x20, 0, 0, 0x02, 0x28])   # longs: $220, $228
    r[0x220:0x222] = bytes([0x4E, 0x71]); r[0x222:0x224] = bytes([0x60, 0xFE])
    r[0x228:0x22A] = bytes([0x4E, 0x71]); r[0x22A:0x22C] = bytes([0x60, 0xFE])
    save_md(out, "fix_md_s4p2", r)
    # --- pointer through a data register: move.l 0(a2,d0.w),d1 ; movea.l d1,a1 ; jmp (a1) ----
    r = md_rom()
    r[0x200:0x206] = bytes([0x45, 0xF9, 0x00, 0x00, 0x02, 0x10])   # lea $210,a2
    r[0x206:0x208] = bytes([0x70, 0x00])                           # moveq #0,d0
    r[0x208:0x20C] = bytes([0x22, 0x32, 0x00, 0x00])               # move.l 0(a2,d0.w),d1
    r[0x20C:0x20E] = bytes([0x22, 0x41])                           # movea.l d1,a1
    r[0x20E:0x210] = bytes([0x4E, 0xD1])                           # jmp (a1)
    r[0x210:0x218] = bytes([0, 0, 0x02, 0x20, 0, 0, 0x02, 0x28])   # longs: $220, $228
    r[0x220:0x222] = bytes([0x4E, 0x71]); r[0x222:0x224] = bytes([0x60, 0xFE])
    r[0x228:0x22A] = bytes([0x4E, 0x71]); r[0x22A:0x22C] = bytes([0x60, 0xFE])
    save_md(out, "fix_md_s4p3", r)
    # --- one copy more: move.l 0(a2,d0.w),d1 ; move.l d1,d2 ; movea.l d2,a1 ; jmp (a1) ----
    r = md_rom()
    r[0x200:0x206] = bytes([0x45, 0xF9, 0x00, 0x00, 0x02, 0x10])   # lea $210,a2
    r[0x206:0x208] = bytes([0x70, 0x00])                           # moveq #0,d0
    r[0x208:0x20C] = bytes([0x22, 0x32, 0x00, 0x00])               # move.l 0(a2,d0.w),d1
    r[0x20C:0x20E] = bytes([0x24, 0x01])                           # move.l d1,d2
    r[0x20E:0x210] = bytes([0x22, 0x42])                           # movea.l d2,a1
    r[0x210:0x212] = bytes([0x4E, 0xD1])                           # jmp (a1)  (table moves to $214)
    r[0x214:0x21C] = bytes([0, 0, 0x02, 0x20, 0, 0, 0x02, 0x28])
    r[0x200:0x206] = bytes([0x45, 0xF9, 0x00, 0x00, 0x02, 0x14])   # lea $214,a2
    r[0x220:0x222] = bytes([0x4E, 0x71]); r[0x222:0x224] = bytes([0x60, 0xFE])
    r[0x228:0x22A] = bytes([0x4E, 0x71]); r[0x22A:0x22C] = bytes([0x60, 0xFE])
    save_md(out, "fix_md_s4p4", r)
    # --- Am already at the entry: lea $210,a2 ; movea.l 0(a2),a1 ; jmp (a1) -> entries $220, $228 ----
    r = md_rom()
    r[0x200:0x206] = bytes([0x45, 0xF9, 0x00, 0x00, 0x02, 0x10])   # lea $210,a2
    r[0x206:0x20A] = bytes([0x22, 0x6A, 0x00, 0x00])               # movea.l 0(a2),a1
    r[0x20A:0x20C] = bytes([0x4E, 0xD1])                           # jmp (a1)
    r[0x210:0x218] = bytes([0, 0, 0x02, 0x20, 0, 0, 0x02, 0x28])
    r[0x220:0x222] = bytes([0x4E, 0x71]); r[0x222:0x224] = bytes([0x60, 0xFE])
    r[0x228:0x22A] = bytes([0x4E, 0x71]); r[0x22A:0x22C] = bytes([0x60, 0xFE])
    save_md(out, "fix_md_s4p5", r)
    # --- longloop: a 5000-iteration boot loop before the idle loop (visit cap must not cut played sessions) ----
    r = md_rom()
    r[0x200:0x204] = bytes([0x30, 0x3C, 0x13, 0x88])     # move.w #5000,d0
    r[0x204:0x206] = bytes([0x51, 0xC8]); r[0x206:0x208] = bytes([0xFF, 0xFE])   # loop: dbf d0,loop
    r[0x208:0x20A] = bytes([0x60, 0xFE])                 # bra.s *
    save_md(out, "fix_md_longloop", r)
    # --- dma: VDP regs, then a ROM->VRAM DMA of 16 words from $000400 ---------
    r = md_rom()
    pc = 0x200
    for v in (0x8114, 0x9310, 0x9400, 0x9500, 0x9602):   # DMA on, len=$10, src=$400>>1
        r[pc:pc + 8] = bytes([0x33, 0xFC, v >> 8, v & 0xFF, 0x00, 0xC0, 0x00, 0x04])
        pc += 8
    r[pc:pc + 10] = bytes([0x23, 0xFC, 0x40, 0x00, 0x00, 0x80, 0x00, 0xC0, 0x00, 0x04])
    pc += 10
    r[pc:pc + 2] = bytes([0x60, 0xFE])                   # bra.s * : bounded by the budget
    for i in range(0x20):
        r[0x400 + i] = i
    save_md(out, "fix_md_dma", r)
    # Start 16 bytes before the end of ROM, so the DMA faults halfway through.
    r[0x21A:0x21C] = bytes([0x95, 0xF8])
    r[0x222:0x224] = bytes([0x96, 0x07])
    save_md(out, "fix_md_dma_fault", r)
    # Four words copied by MOVE.W directly from ROM to the VDP data port.
    r = md_rom()
    code = bytes.fromhex("33fc 8f02 00c0 0004 23fc 4000 0000 00c0 0004 "
                         "41f9 0000 0400 7003 33d8 00c0 0000 51c8 fff8 60fe")
    r[0x200:0x200 + len(code)] = code
    r[0x400:0x408] = bytes.fromhex("1234 5678 9abc def0")
    save_md(out, "fix_md_cpu_vram", r)
    r = md_rom()
    code = bytes.fromhex("33fc 8f02 00c0 0004 23fc 4000 0000 00c0 0004 "
                         "41f9 0000 0400 43f9 00c0 0000 7203 3018 3280 51c9 fffa 60fe")
    r[0x200:0x200 + len(code)] = code
    r[0x400:0x408] = bytes.fromhex("1234 5678 9abc def0")
    save_md(out, "fix_md_cpu_vram_reg", r)
    code = bytes.fromhex("33fc 8f02 00c0 0004 23fc 4000 0000 00c0 0004 "
                         "41f9 0000 0400 43f9 00c0 0000 7203 3018 4e71 3280 51c9 fff8 60fe")
    r[0x200:0x200 + len(code)] = code
    save_md(out, "fix_md_cpu_vram_gap", r)
    # A 4-byte routine ($0400) copied word by word to work RAM, then called there (rtcode ROM source).
    r = md_rom()
    code = bytes.fromhex("41f9 0000 0400 43f9 00ff 0000 7201 32d8 51c9 fffc 4eb9 00ff 0000 60fe")
    r[0x200:0x200 + len(code)] = code
    r[0x400:0x404] = bytes.fromhex("7042 4e75")
    save_md(out, "fix_md_ramsrc", r)
    # Generated code: a ROM word is read, inverted, and stored to RAM, then an immediate rts is stored; called there.
    r = md_rom()
    code = bytes.fromhex("41f9 0000 0400 3010 4640 33c0 00ff 0000 33fc 4e75 00ff 0002 4eb9 00ff 0000 60fe")
    r[0x200:0x200 + len(code)] = code
    r[0x400:0x402] = bytes.fromhex("8fbd")
    save_md(out, "fix_md_ramgen", r)
    # taint through exg and movem: word read from ROM, swapped between registers, stored by movem, executed
    r = md_rom()
    code = bytes.fromhex("41f9 0000 0400 43f9 00ff 0000 3210 c342 4891 0004 4eb9 00ff 0000 60fe")
    r[0x200:0x200 + len(code)] = code
    r[0x400:0x402] = bytes.fromhex("4e75")
    save_md(out, "fix_md_ramexg", r)
    # A 2 MiB+ cartridge with SRAM overlay: execute and read SRAM without
    # accidentally attributing those accesses to the covered ROM bytes.
    r = md_rom()
    r.extend(bytes(0x200004 - len(r)))
    code = bytes.fromhex("13fc 0001 00a1 30f1 33fc 60fe 0020 0000 "
                         "1039 0020 0002 4ef9 0020 0000")
    r[0x200:0x200 + len(code)] = code
    r[0x200000:0x200004] = bytes.fromhex("4e71 1234")
    save_md(out, "fix_md_sram_overlay", r)

    # --- smc: code written to work RAM, executed, then rewritten ------------------
    r = md_rom()
    code = bytes.fromhex("33fc 4e71 00ff 0000 33fc 4e75 00ff 0002 4eb9 00ff 0000 "
                         "33fc 4e71 00ff 0000 60fe")
    r[0x200:0x200 + len(code)] = code
    save_md(out, "fix_md_smc", r)

    # --- fork: tst.w d0 / bne.s (not taken naturally) / both sides end in bra.s * -
    r = md_rom()
    r[0x200:0x20C] = bytes([0x4A, 0x40, 0x66, 0x04, 0x4E, 0x71, 0x60, 0xFE, 0x4E, 0x71, 0x60, 0xFE])
    save_md(out, "fix_md_fork", r)
    # Two natural ROM reads from one instruction in a counted loop.
    r = md_rom()
    r[0x200:0x20E] = bytes([0x70, 0x01, 0x32, 0x39, 0, 0, 0x04, 0x00,
                            0x51, 0xC8, 0xFF, 0xF8, 0x60, 0xFE])
    save_md(out, "fix_md_reader", r)
    r[0x20C:0x214] = bytes.fromhex("3239 0000 0400 60fe")
    save_md(out, "fix_md_reader_overlap", r)
    # The second branch's alternative returns to an already executed PC with
    # a different D0. Address-only eviction must not discard the snapshot.
    r = md_rom()
    r[0x200:0x20E] = bytes([0x70, 0x00, 0x4A, 0x00, 0x66, 0x0A,
                            0x70, 0x01, 0x4A, 0x00, 0x67, 0xF6, 0x60, 0xFE])
    r[0x210:0x212] = bytes([0x60, 0xFE])
    save_md(out, "fix_md_revisit", r)
    # Poll controller 1 repeatedly; probes apply to the next poll.
    r = md_rom()
    r[0x200:0x208] = bytes([0x10, 0x39, 0x00, 0xA1, 0x00, 0x03, 0x60, 0xF8])
    save_md(out, "fix_md_pad", r)
    # Continue only when Right is read as pressed (active low).
    r = md_rom()
    r[0x200:0x210] = bytes([0x10, 0x39, 0x00, 0xA1, 0x00, 0x03,
                            0x08, 0x00, 0x00, 0x03, 0x66, 0xF4,
                            0x4E, 0x71, 0x60, 0xFE])
    save_md(out, "fix_md_pad_effect", r)
    # Six-button detection: pulse TH three times, then read with TH low; bits 0-3 read
    # as zero only on a six-button pad.  The probes apply to the next poll, hence the loop.
    r = md_rom()
    code = bytearray()
    for v in (0, 0x40, 0, 0x40, 0, 0x40, 0):
        code += bytes([0x13, 0xFC, 0x00, v, 0x00, 0xA1, 0x00, 0x03])
    # moveq #0,d0 ; move.b $A10003,d0 ; andi.w #15,d0 ; move.b 8(pc,d0.w),d1 ; bra loop
    # no branch depends on the pad (a branch forks both sides): the data byte read tells
    code += bytes([0x70, 0x00, 0x10, 0x39, 0x00, 0xA1, 0x00, 0x03, 0x02, 0x40, 0x00, 0x0F,
                   0x12, 0x3B, 0x00, 0x08, 0x60, 0xB6, 0x4E, 0x71, 0x4E, 0x71])
    r[0x200:0x200 + len(code)] = code
    r[0x24E] = 0xAA; r[0x251] = 0xBB
    save_md(out, "fix_md_pad6", r)
    r = md_rom()
    r[0x200:0x206] = bytes([0x10, 0x39, 0x00, 0xA2, 0x00, 0x00])
    save_md(out, "fix_md_fault", r)
    r = md_rom()
    r[0x200:0x20C] = bytes([0x26, 0x7C, 0x00, 0x00, 0x12, 0x34,
                            0x10, 0x39, 0x00, 0xA2, 0x00, 0x00])
    save_md(out, "fix_md_fault_reg", r)
    r = md_rom()
    r[0x200:0x20E] = bytes([0x33, 0xFC, 0x12, 0x34, 0x00, 0xFF, 0x10, 0x00,
                            0x10, 0x39, 0x00, 0xA2, 0x00, 0x00])
    save_md(out, "fix_md_watch", r)
    r = md_rom()
    r[0x200:0x20E] = bytes([0x4A, 0x40, 0x66, 0x04, 0x4E, 0x71, 0x60, 0xFE,
                            0x10, 0x39, 0x00, 0xA2, 0x00, 0x00])
    save_md(out, "fix_md_fork_fault", r)
    # --- dbcc: moveq #0,d0 / dbf d0,* / nop / bra.s * ---------------------------
    r = md_rom()
    r[0x200:0x20A] = bytes([0x70, 0x00, 0x51, 0xC8, 0xFF, 0xFE, 0x4E, 0x71, 0x60, 0xFE])
    save_md(out, "fix_md_dbcc", r)
    # --- loop: a single bra.s * (loop bound must report saturation) ------------
    r = md_rom()
    r[0x200:0x202] = bytes([0x60, 0xFE])
    save_md(out, "fix_md_loop", r)
    # --- indirect: movea.l #$300,a0 / jmp (a0) -> bra.s * at $300 -------------
    r = md_rom()
    r[0x200:0x208] = bytes([0x20, 0x7C, 0x00, 0x00, 0x03, 0x00, 0x4E, 0xD0])
    r[0x300:0x302] = bytes([0x60, 0xFE])
    save_md(out, "fix_md_ind", r)
    # --- padding: nop then a long run of zero words (ori.b #0,d0 slide) --------
    r = md_rom()
    r[0x200:0x202] = bytes([0x4E, 0x71])
    save_md(out, "fix_md_zeros", r)
    # --- dispatch: never-read table {$300,$310,$320} of code pointers at $400 ---
    r = md_rom()
    r[0x200:0x202] = bytes([0x60, 0xFE])
    for t in (0x300, 0x310, 0x320):
        r[t:t + 6] = bytes([0x70, 0x01, 0x4E, 0x71, 0x60, 0xFE])
    r[0x400:0x40C] = bytes([0, 0, 3, 0, 0, 0, 3, 0x10, 0, 0, 3, 0x20])
    save_md(out, "fix_md_disp", r)
    # --- static: beq.s taken (fall-through never forked with --fork-cap 0) + an unread
    # pointer table at $400 whose targets are not code ---
    r = md_rom()
    r[0x200:0x20A] = bytes([0x70, 0x00, 0x67, 0x04, 0x70, 0x01, 0x4E, 0x71, 0x60, 0xFE])
    for i, t in enumerate((0x500, 0x504, 0x508, 0x50C, 0x510)):
        r[0x400 + i * 4:0x404 + i * 4] = t.to_bytes(4, "big")
    r[0x3F8:0x400] = bytes([0x11]) * 8
    r[0x500:0x520] = bytes([0xAA]) * 0x20
    save_md(out, "fix_md_static", r)
    # --- siblings: DMA of $400 (16 words) + a longword table {$400,$600,$800} read at $300
    r = md_rom()
    pc = 0x200
    for v in (0x8114, 0x9310, 0x9400, 0x9500, 0x9602):
        r[pc:pc + 8] = bytes([0x33, 0xFC, v >> 8, v & 0xFF, 0x00, 0xC0, 0x00, 0x04])
        pc += 8
    r[pc:pc + 10] = bytes([0x23, 0xFC, 0x40, 0x00, 0x00, 0x80, 0x00, 0xC0, 0x00, 0x04])
    pc += 10
    r[pc:pc + 6] = bytes([0x20, 0x39, 0x00, 0x00, 0x03, 0x00])   # move.l ($300).l,d0
    pc += 6
    r[pc:pc + 2] = bytes([0x60, 0xFE])
    r[0x300:0x30C] = bytes([0, 0, 4, 0, 0, 0, 6, 0, 0, 0, 8, 0])
    for i in range(0x20):
        r[0x400 + i] = i
    save_md(out, "fix_md_sib", r)
    # --- vint: enable V-int (reg1 bit 5), vector 30 -> $300, main loop bra.s * --
    r = md_rom()
    r[30 * 4:30 * 4 + 4] = (0x300).to_bytes(4, "big")
    r[0x200:0x208] = bytes([0x33, 0xFC, 0x81, 0x64, 0x00, 0xC0, 0x00, 0x04])
    r[0x208:0x20C] = bytes([0x46, 0xFC, 0x20, 0x00])        # move #$2000,sr (unmask)
    r[0x20C:0x20E] = bytes([0x60, 0xFE])
    r[0x300:0x302] = bytes([0x4E, 0x73])                 # rte
    save_md(out, "fix_md_vint", r)
    r[0x208:0x20C] = bytes([0x60, 0xFE, 0x4E, 0x71])     # keep reset SR interrupt mask
    save_md(out, "fix_md_vint_masked", r)

def build_md_techniques(out):
    # 68000 technique fixtures (3 save signature, 4 volatile byte, 7 index replay) on work RAM $FFC100..
    def prog(name, code, table=None):
        r = md_rom()
        r[0x200:0x200 + len(code)] = code
        if table:
            r[0x230:0x230 + len(table)] = table
        save_md(out, name, r)
    prog("fix_md_save", bytes.fromhex("1039 00ffc100 0c00 00aa 6604 7001 6002 7002 60fe"))
    prog("fix_md_rng", bytes.fromhex("1039 00ffc200 0a00 0055 13c0 00ffc200 0c00 00aa 6702 60e8 7001 60fe"))
    prog("fix_md_index", bytes.fromhex("1039 00ffc300 0280 0000 000f 41f9 00000230 1230 0000 0c01 0003 6704 7001 60fe 7002 60fe"),
         bytes([0, 0, 0, 3] + [0] * 12))

def build_md_z80(out):
    # 68000 stops the Z80 bus, copies a 13-byte Z80 program from ROM $400 into
    # Z80 RAM, releases the bus. The Z80 reads ROM $320 through the bank window
    # and writes it to the YM2612 DAC (register $2A).
    r = md_rom()
    code = bytes.fromhex("33fc 0100 00a1 1100 33fc 0100 00a1 1200 "
                         "41f9 0000 0400 43f9 00a0 0000 700c 12d8 51c8 fffc "
                         "33fc 0000 00a1 1100" + " 4e71" * 64 + " 1039 00a0 0000 60fe")
    r[0x200:0x200 + len(code)] = code
    r[0x400:0x40D] = bytes.fromhex("3e2a 32 0040 3a 2083 32 0140 18fe")
    r[0x320] = 0xAB
    save_md(out, "fix_md_z80", r)

def build_z80_family(out):
    # one Z80 branch program per Sega/MSX target: ld a,(RAM) / or a / jr z (both sides) /
    # out (VDP data) / idle loop. SMS, Game Gear and SG-1000 start at $0000, MSX at the
    # INIT of an "AB" cartridge header ($4000).
    prog = bytes([0x3A, 0x00, 0xC0, 0xB7, 0x28, 0x04, 0x06, 0x01, 0x18, 0x02, 0x06, 0x02,
                  0x78, 0xD3, 0xBE, 0x18, 0xFE])
    for ext in ("sms", "gg", "sg"):
        z = bytearray([0xFF]) * 0x8000
        z[0:len(prog)] = prog
        with open(os.path.join(out, "fix_z80_branch." + ext), "wb") as f:
            f.write(z)
    # two conditional sites forking to the same target with the same RAM: technique 8
    # (semantic cells) drops the second forced fork
    cells = bytes([0xB7, 0x20, 0x06, 0x20, 0x04, 0, 0, 0, 0, 0x18, 0xFE])
    z = bytearray([0xFF]) * 0x8000
    z[0:len(cells)] = cells
    with open(os.path.join(out, "fix_z80_cells.sms"), "wb") as f:
        f.write(z)
    # save-signature check: ld a,($C100) / cp $AA / jr nz (technique 3: a valid save flips it)
    sv = bytearray([0xFF]) * 0x8000
    code = bytes([0x3A, 0x00, 0xC1, 0xFE, 0xAA, 0x20, 0x04, 0x3E, 0x01, 0x18, 0x02, 0x3E, 0x02, 0x18, 0xFE])
    sv[0:len(code)] = code
    with open(os.path.join(out, "fix_z80_save.sms"), "wb") as f:
        f.write(sv)
    # volatile byte: loop reads ($C200) twice around an inc, then tests it (technique 4)
    rg = bytearray([0xFF]) * 0x8000
    code = bytes([0x3A,0x00,0xC2, 0xEE,0x55, 0x32,0x00,0xC2, 0xFE,0xAA, 0x28,0x02, 0x18,0xF2, 0x3E,0x01, 0x18,0xFE])
    rg[0:len(code)] = code
    with open(os.path.join(out, "fix_z80_rng.sms"), "wb") as f:
        f.write(rg)
    # table read indexed by a RAM byte (technique 7): only index 3 reaches the cp $03 match
    ix = bytearray([0xFF]) * 0x8000
    code = bytes([0x3A,0x00,0xC3, 0x5F, 0x16,0x00, 0x21,0x20,0x00, 0x19, 0x7E, 0xFE,0x03, 0x28,0x04, 0x3E,0x01, 0x18,0xFE, 0x3E,0x02, 0x18,0xFE])
    ix[0:len(code)] = code
    ix[0x20:0x30] = bytes([0,0,0,3] + [0] * 12)
    with open(os.path.join(out, "fix_z80_index.sms"), "wb") as f:
        f.write(ix)
    # arithmetic on a RAM byte (technique 1): ld a,($C400) / sub 7 / and $0F / jr z; only RAM = 7 takes the jr
    sv1 = bytearray([0xFF]) * 0x8000
    code = bytes([0x3A,0x00,0xC4, 0xD6,0x07, 0xE6,0x0F, 0x28,0x04, 0x3E,0x01, 0x18,0x02, 0x3E,0x02, 0x18,0xFE])
    sv1[0:len(code)] = code
    with open(os.path.join(out, "fix_z80_solve.sms"), "wb") as f:
        f.write(sv1)
    # HALT then idle; NMI handler at $66 (the SMS Pause button): needs the Pause NMI probe
    nm = bytearray([0xFF]) * 0x8000
    nm[0:3] = bytes([0x76, 0x18, 0xFD])
    nm[0x66:0x6B] = bytes([0x3E, 0x77, 0x32, 0x00, 0xC0]); nm[0x6B:0x6D] = bytes([0xED, 0x45])
    with open(os.path.join(out, "fix_z80_pause.sms"), "wb") as f:
        f.write(nm)
    # pad poll: in a,($DC) / bit 4,a (button 1) / jr z; both outcomes need a pad probe
    pad = bytearray([0xFF]) * 0x8000
    code = bytes([0xDB, 0xDC, 0xCB, 0x67, 0x28, 0x04, 0x3E, 0x01, 0x18, 0x02, 0x3E, 0x02, 0x18, 0xFE])
    pad[0:len(code)] = code
    with open(os.path.join(out, "fix_z80_pad.sms"), "wb") as f:
        f.write(pad)
    # IM 1 vblank interrupt: VDP reg1 enables the frame IRQ, EI, idle; handler at $38 (IRQ fork)
    irq = bytearray([0xFF]) * 0x8000
    boot = bytes([0x31, 0xF0, 0xDF, 0x3E, 0x20, 0xD3, 0xBF, 0x3E, 0x81, 0xD3, 0xBF,
                  0xED, 0x56, 0xFB, 0x18, 0xFE])
    irq[0:len(boot)] = boot
    irq[0x38:0x38 + 8] = bytes([0x3E, 0x55, 0x32, 0x00, 0xC0, 0xFB, 0xED, 0x4D])
    with open(os.path.join(out, "fix_z80_irq.sms"), "wb") as f:
        f.write(irq)
    # a 32-byte ROM stream expanded 1:2 into RAM: technique 6 (decompressor detection)
    dec = bytes([0x21, 0x00, 0x01, 0x11, 0x00, 0xC0, 0x06, 0x20,
                 0x7E, 0x23, 0x12, 0x13, 0x12, 0x13, 0x10, 0xF8, 0x18, 0xFE])
    z = bytearray([0xFF]) * 0x8000
    z[0:len(dec)] = dec
    z[0x100:0x120] = bytes(range(1, 33))
    with open(os.path.join(out, "fix_z80_decomp.sms"), "wb") as f:
        f.write(z)
    m = bytearray([0xFF]) * 0x8000
    m[0:2] = b"AB"
    m[2:4] = (0x4010).to_bytes(2, "little")
    m[4:16] = bytes(12)
    m[0x10:0x10 + len(prog)] = prog
    with open(os.path.join(out, "fix_z80_branch.msx"), "wb") as f:
        f.write(m)
    # MSX: V9938 palette ($9A) and indirect register ($9B) writes
    m = bytearray([0xFF]) * 0x8000
    m[0:2] = b"AB"
    m[2:4] = (0x4010).to_bytes(2, "little")
    m[4:16] = bytes(12)
    m[0x10:0x18] = bytes([0x3E, 0x0F, 0xD3, 0x9A, 0xD3, 0x9B, 0x18, 0xFE])
    with open(os.path.join(out, "fix_msx_v9938.msx"), "wb") as f:
        f.write(m)

def build_nes_pce(out):
    # NES NROM-256 (2 x 16 KiB PRG + 8 KiB CHR): LDA $9000 / BNE (both sides) /
    # STA $4014 (OAM DMA from page $80); vectors -> $8000
    rom = bytearray(16 + 2 * 0x4000 + 0x2000)
    rom[0:4] = b"NES\x1a"
    rom[4], rom[5] = 2, 1
    code = bytes([0xAD, 0x00, 0x90, 0xD0, 0x05, 0xA9, 0x11, 0x4C, 0x0C, 0x80, 0xA9, 0x22, 0x85, 0x10,
                  0xA9, 0x80, 0x8D, 0x14, 0x40, 0x4C, 0x13, 0x80])
    rom[16:16 + len(code)] = code
    rom[16 + 0x1000] = 0x01
    for v in (0xFFFA, 0xFFFC, 0xFFFE):  # vectors live at the end of the last 16 KiB PRG bank
        o = 16 + 0x4000 + (v - 0xFFFA) + 0x3FFA
        rom[o:o + 2] = bytes([0x00, 0x80])
    with open(os.path.join(out, "fix_nes_branch.nes"), "wb") as f:
        f.write(rom)
    # 6502 technique fixtures (3 save signature, 4 volatile byte, 7 index replay): RAM load at $8000
    def nes_prog(name, code, table=None, nmi=None, handler=None):
        n = bytearray(16 + 2 * 0x4000 + 0x2000)
        n[0:4] = b"NES\x1a"
        n[4], n[5] = 2, 1
        n[16:16 + len(code)] = code
        if table:
            n[16 + 0x20:16 + 0x20 + len(table)] = table
        for v in (0xFFFA, 0xFFFC, 0xFFFE):
            q = 16 + 0x4000 + (v - 0xFFFA) + 0x3FFA
            n[q:q + 2] = bytes([0x00, 0x80])
        if nmi is not None:
            n[16 + nmi:16 + nmi + len(handler)] = handler
            q = 16 + 0x4000 + 0x3FFA
            n[q:q + 2] = bytes([nmi & 0xFF, 0x80 | (nmi >> 8)])
        with open(os.path.join(out, name), "wb") as f:
            f.write(n)
    # NMI enabled, spin; the handler counts frames (IRQ forks and timing jitter, techniques 2/9)
    nes_prog("fix_nes_nmi.nes", bytes([0xA9,0x80, 0x8D,0x00,0x20, 0x4C,0x05,0x80]), nmi=0x10, handler=bytes([0xE6,0x10, 0x40]))
    nes_prog("fix_nes_save.nes", bytes([0xAD,0x00,0x03, 0xC9,0xAA, 0xD0,0x05, 0xA9,0x01, 0x4C,0x0E,0x80, 0xA9,0x02, 0x4C,0x0E,0x80]))
    nes_prog("fix_nes_rng.nes", bytes([0xAD,0x00,0x03, 0x49,0x55, 0x8D,0x00,0x03, 0xC9,0xAA, 0xF0,0x03, 0x4C,0x00,0x80, 0xA9,0x01, 0x4C,0x11,0x80]))
    nes_prog("fix_nes_index.nes", bytes([0xAD,0x00,0x04, 0xAA, 0xBD,0x20,0x80, 0xC9,0x03, 0xF0,0x05, 0xA9,0x01, 0x4C,0x0D,0x80, 0xA9,0x02, 0x4C,0x12,0x80]),
             bytes([0,0,0,3] + [0] * 12))
    # NES PPU status poll: LDA $2002 / BMI done / JMP poll; done stores and idles (technique 2)
    n = bytearray(16 + 2 * 0x4000 + 0x2000)
    n[0:4] = b"NES\x1a"
    n[4], n[5] = 2, 1
    code = bytes([0xAD, 0x02, 0x20, 0x30, 0x03, 0x4C, 0x00, 0x80, 0xA9, 0x55, 0x85, 0x10, 0x4C, 0x0C, 0x80])
    n[16:16 + len(code)] = code
    for v in (0xFFFA, 0xFFFC, 0xFFFE):
        o = 16 + 0x4000 + (v - 0xFFFA) + 0x3FFA
        n[o:o + 2] = bytes([0x00, 0x80])
    with open(os.path.join(out, "fix_nes_ppupoll.nes"), "wb") as f:
        f.write(n)
    # NES pad reader: strobe $4016, read A, branch, loop (pad probes fork per button)
    n = bytearray(16 + 2 * 0x4000 + 0x2000)
    n[0:4] = b"NES\x1a"
    n[4], n[5] = 2, 1
    code = bytes([0xA9, 0x01, 0x8D, 0x16, 0x40, 0xA9, 0x00, 0x8D, 0x16, 0x40,
                  0xAD, 0x16, 0x40, 0x29, 0x01, 0xD0, 0x03, 0xEA, 0xEA, 0x4C, 0x00, 0x80])
    n[16:16 + len(code)] = code
    for v in (0xFFFA, 0xFFFC, 0xFFFE):
        q = 16 + 0x4000 + (v - 0xFFFA) + 0x3FFA
        n[q:q + 2] = bytes([0x00, 0x80])
    with open(os.path.join(out, "fix_nes_pad.nes"), "wb") as f:
        f.write(n)
    # NES indirect jump: pointer in RAM -> $8010, jmp ($0200); target spins
    n = bytearray(16 + 2 * 0x4000 + 0x2000)
    n[0:4] = b"NES\x1a"
    n[4], n[5] = 2, 1
    code = bytes([0xA9, 0x10, 0x8D, 0x00, 0x02, 0xA9, 0x80, 0x8D, 0x01, 0x02, 0x6C, 0x00, 0x02])
    n[16:16 + len(code)] = code
    n[16 + 0x10:16 + 0x10 + 3] = bytes([0x4C, 0x10, 0x80])
    for v in (0xFFFA, 0xFFFC, 0xFFFE):
        q = 16 + 0x4000 + (v - 0xFFFA) + 0x3FFA
        n[q:q + 2] = bytes([0x00, 0x80])
    with open(os.path.join(out, "fix_nes_ind.nes"), "wb") as f:
        f.write(n)
    # NES CPU-loop copy: $2006 <- $2000, then 16x (lda $8100,x / sta $2007 / inx)
    n = bytearray(16 + 2 * 0x4000 + 0x2000)
    n[0:4] = b"NES\x1a"
    n[4], n[5] = 2, 1
    code = bytes([0xA9, 0x20, 0x8D, 0x06, 0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20, 0xA2, 0x00,
                  0xBD, 0x00, 0x81, 0x8D, 0x07, 0x20, 0xE8, 0xE0, 0x10, 0xD0, 0xF5, 0x4C, 0x17, 0x80])
    n[16:16 + len(code)] = code
    for i in range(16):
        n[16 + 0x100 + i] = 0x40 + i
    for v in (0xFFFA, 0xFFFC, 0xFFFE):
        q = 16 + 0x4000 + (v - 0xFFFA) + 0x3FFA
        n[q:q + 2] = bytes([0x00, 0x80])
    with open(os.path.join(out, "fix_nes_cpucopy.nes"), "wb") as f:
        f.write(n)
    # NES CPU loop ROM -> WRAM: 16 bytes from $8100 to $0300
    nw = bytearray(n)
    cw = bytes([0xA2, 0x00, 0xBD, 0x00, 0x81, 0x9D, 0x00, 0x03, 0xE8, 0xE0, 0x10, 0xD0, 0xF5, 0x4C, 0x0D, 0x80])
    nw[16:16 + 64] = bytes(64)
    nw[16:16 + len(cw)] = cw
    with open(os.path.join(out, "fix_nes_wramcopy.nes"), "wb") as f:
        f.write(nw)
    # NES chain ROM -> WRAM ($0200) -> OAM DMA
    nc = bytearray(nw)
    nc[16 + 5:16 + 8] = bytes([0x9D, 0x00, 0x02])
    nc[16 + 13:16 + 13 + 8] = bytes([0xA9, 0x02, 0x8D, 0x14, 0x40, 0x4C, 0x12, 0x80])
    with open(os.path.join(out, "fix_nes_oamchain.nes"), "wb") as f:
        f.write(nc)
    # NES CPU loop ROM -> SRAM ($6000) and ROM -> APU ($4000..)
    ns = bytearray(nw)
    ns[16 + 5:16 + 8] = bytes([0x9D, 0x00, 0x60])
    with open(os.path.join(out, "fix_nes_sramcopy.nes"), "wb") as f:
        f.write(ns)
    na = bytearray(nw)
    na[16 + 5:16 + 8] = bytes([0x9D, 0x00, 0x40])
    with open(os.path.join(out, "fix_nes_apucopy.nes"), "wb") as f:
        f.write(na)
    # NES copy siblings: the loader reads the pointer word at $8200 first; the
    # table ($8100, $8300, $8400) names the other 16-byte sources
    n2 = bytearray(n)
    code2 = bytes([0xAD, 0x00, 0x82]) + code[:-3] + bytes([0x4C, 0x1A, 0x80])
    n2[16:16 + len(code2)] = code2
    n2[16 + 0x200:16 + 0x200 + 6] = bytes([0x00, 0x81, 0x00, 0x83, 0x00, 0x84])
    with open(os.path.join(out, "fix_nes_sibling.nes"), "wb") as f:
        f.write(n2)
    # NES provenance: lda $8100 / sta $00 / lda $8101 / sta $01 / jmp ($0000);
    # the table at $8100 is ($8020, $8030, $8040), only the first is taken
    n3 = bytearray(16 + 2 * 0x4000 + 0x2000)
    n3[0:4] = b"NES\x1a"
    n3[4], n3[5] = 2, 1
    code3 = bytes([0xAD, 0x00, 0x81, 0x85, 0x00, 0xAD, 0x01, 0x81, 0x85, 0x01, 0x6C, 0x00, 0x00])
    n3[16:16 + len(code3)] = code3
    n3[16 + 0x100:16 + 0x106] = bytes([0x20, 0x80, 0x30, 0x80, 0x40, 0x80])
    for t in (0x20, 0x30, 0x40):
        n3[16 + t:16 + t + 3] = bytes([0x4C, t, 0x80])
    for v in (0xFFFA, 0xFFFC, 0xFFFE):
        q = 16 + 0x4000 + (v - 0xFFFA) + 0x3FFA
        n3[q:q + 2] = bytes([0x00, 0x80])
    with open(os.path.join(out, "fix_nes_prov.nes"), "wb") as f:
        f.write(n3)
    # NES bank dispatch: UxROM, 4 banks; the fixed bank idles at $C000 and holds a
    # word table ($8010, $8020, $8030) at $C100; banks 0 and 1 hold code at all
    # three targets, bank 2 only at $8010, bank 3 (the fixed bank) none
    d = bytearray(16 + 4 * 0x4000)
    d[0:4] = b"NES\x1a"
    d[4], d[5], d[6] = 4, 0, 0x20
    for b, ts in ((0, (0x10, 0x20, 0x30)), (1, (0x10, 0x20, 0x30)), (2, (0x10,))):
        for t in ts:
            q = 16 + b * 0x4000 + t
            d[q:q + 3] = bytes([0x4C, t, 0x80])
    o = 16 + 3 * 0x4000
    d[o:o + 3] = bytes([0x4C, 0x00, 0xC0])
    d[o + 0x100:o + 0x106] = bytes([0x10, 0x80, 0x20, 0x80, 0x30, 0x80])
    for v in (0xFFFA, 0xFFFC, 0xFFFE):
        q = o + 0x3FFA + (v - 0xFFFA)
        d[q:q + 2] = bytes([0x00, 0xC0])
    with open(os.path.join(out, "fix_nes_dispatch.nes"), "wb") as f:
        f.write(d)
    # NES context loop: INC $10 / LDA #0 / BEQ back; every pass differs only in RAM,
    # so the exact dedup keeps all of them and a context cap closes the surplus
    k = bytearray(16 + 2 * 0x4000)
    k[0:4] = b"NES\x1a"
    k[4], k[5] = 2, 0
    k[16:16 + 6] = bytes([0xE6, 0x10, 0xA9, 0x00, 0xF0, 0xFA])
    for v in (0xFFFA, 0xFFFC, 0xFFFE):
        q = 16 + 0x4000 + 0x3FFA + (v - 0xFFFA)
        k[q:q + 2] = bytes([0x00, 0x80])
    with open(os.path.join(out, "fix_nes_ctx.nes"), "wb") as f:
        f.write(k)
    # NES cross-window context loop: the main loop in the $8000 window only forks (lda #0 / beq), the RAM
    # counter is bumped by a subroutine in the $C000 window, so the loop's own window never reads RAM
    k = bytearray(16 + 2 * 0x4000)
    k[0:4] = b"NES\x1a"
    k[4], k[5] = 2, 0
    k[16:16 + 7] = bytes([0x20, 0x00, 0xC0, 0xA9, 0x00, 0xF0, 0xF9])
    k[16 + 0x4000:16 + 0x4000 + 3] = bytes([0xE6, 0x10, 0x60])
    for v in (0xFFFA, 0xFFFC, 0xFFFE):
        q = 16 + 0x4000 + 0x3FFA + (v - 0xFFFA)
        k[q:q + 2] = bytes([0x00, 0x80])
    with open(os.path.join(out, "fix_nes_ctx2.nes"), "wb") as f:
        f.write(k)
    # NES jam: a KIL opcode after a short run ends the path (the 6502 has no HALT)
    q = bytearray(16 + 2 * 0x4000)
    q[0:4] = b"NES\x1a"
    q[4], q[5] = 2, 0
    q[16:16 + 4] = bytes([0xA9, 0x01, 0x02, 0xEA])
    for v in (0xFFFA, 0xFFFC, 0xFFFE):
        o2 = 16 + 0x4000 + 0x3FFA + (v - 0xFFFA)
        q[o2:o2 + 2] = bytes([0x00, 0x80])
    with open(os.path.join(out, "fix_nes_jam.nes"), "wb") as f:
        f.write(q)
    # NES static closure: jsr $8020 whose subroutine a 2-step budget never reaches
    n = bytearray(16 + 2 * 0x4000 + 0x2000)
    n[0:4] = b"NES\x1a"
    n[4], n[5] = 2, 1
    code = bytes([0x20, 0x20, 0x80, 0x4C, 0x00, 0x80])
    n[16:16 + len(code)] = code
    sub = bytes([0xA9, 0x01, 0xD0, 0x02, 0xEA, 0xEA, 0x60])
    n[16 + 0x20:16 + 0x20 + len(sub)] = sub
    for v in (0xFFFA, 0xFFFC, 0xFFFE):
        q = 16 + 0x4000 + (v - 0xFFFA) + 0x3FFA
        n[q:q + 2] = bytes([0x00, 0x80])
    with open(os.path.join(out, "fix_nes_static.nes"), "wb") as f:
        f.write(n)
    # NES picture: palette ($0F, $30), 64 nametable tiles of tile 1 (solid colour 1), rendering on
    n = bytearray(16 + 2 * 0x4000 + 0x2000)
    n[0:4] = b"NES\x1a"
    n[4], n[5] = 2, 1
    code = bytes([0xA9, 0x3F, 0x8D, 0x06, 0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20,
                  0xA9, 0x0F, 0x8D, 0x07, 0x20, 0xA9, 0x30, 0x8D, 0x07, 0x20,
                  0xA9, 0x20, 0x8D, 0x06, 0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20, 0xA2, 0x00,
                  0xA9, 0x01, 0x8D, 0x07, 0x20, 0xE8, 0xE0, 0x40, 0xD0, 0xF6,
                  0xA9, 0x00, 0x8D, 0x05, 0x20, 0x8D, 0x05, 0x20, 0xA9, 0x0A, 0x8D, 0x01, 0x20])
    code += bytes([0x4C, 0x00 + (16 + 0) , 0x80])
    n[16:16 + len(code)] = code
    # jump to itself (the 4C is the last instruction): patch its address
    at = len(code) - 3
    code = code[:at + 1] + bytes([at & 0xFF, 0x80])
    n[16:16 + len(code)] = code
    n[16 + 2 * 0x4000 + 16:16 + 2 * 0x4000 + 24] = bytes([0xFF] * 8)   # CHR tile 1, plane 0
    for v in (0xFFFA, 0xFFFC, 0xFFFE):
        q = 16 + 0x4000 + (v - 0xFFFA) + 0x3FFA
        n[q:q + 2] = bytes([0x00, 0x80])
    with open(os.path.join(out, "fix_nes_picture.nes"), "wb") as f:
        f.write(n)
    # NES pointer table: three executed subroutines, then the table of their
    # addresses is read as data ($8100: $8040, $8050, $8060)
    n = bytearray(16 + 2 * 0x4000 + 0x2000)
    n[0:4] = b"NES\x1a"
    n[4], n[5] = 2, 1
    code = bytes([0x20, 0x40, 0x80, 0x20, 0x50, 0x80, 0x20, 0x60, 0x80,
                  0xAD, 0x00, 0x81, 0xAD, 0x02, 0x81, 0xAD, 0x04, 0x81, 0x4C, 0x12, 0x80])
    n[16:16 + len(code)] = code
    for a in (0x40, 0x50, 0x60):
        n[16 + a] = 0x60
    for k, a in enumerate((0x40, 0x50, 0x60)):
        n[16 + 0x100 + 2 * k:16 + 0x100 + 2 * k + 2] = bytes([a, 0x80])
    for v in (0xFFFA, 0xFFFC, 0xFFFE):
        q = 16 + 0x4000 + (v - 0xFFFA) + 0x3FFA
        n[q:q + 2] = bytes([0x00, 0x80])
    with open(os.path.join(out, "fix_nes_table.nes"), "wb") as f:
        f.write(n)
    # NES MMC1 (4 x 16 KiB PRG): the same pointer table in the fixed last bank, $C100 -> $C040/$C050/$C060
    m1 = bytearray(16 + 4 * 0x4000)
    m1[0:4] = b"NES\x1a"
    m1[4], m1[5], m1[6] = 4, 0, 0x10
    o = 16 + 3 * 0x4000
    code = bytes([0x20, 0x40, 0xC0, 0x20, 0x50, 0xC0, 0x20, 0x60, 0xC0,
                  0xAD, 0x00, 0xC1, 0xAD, 0x02, 0xC1, 0xAD, 0x04, 0xC1, 0x4C, 0x12, 0xC0])
    m1[o:o + len(code)] = code
    for a in (0x40, 0x50, 0x60):
        m1[o + a] = 0x60
    for k, a in enumerate((0x40, 0x50, 0x60)):
        m1[o + 0x100 + 2 * k:o + 0x100 + 2 * k + 2] = bytes([a, 0xC0])
    for v in (0xFFFA, 0xFFFC, 0xFFFE):
        q = o + 0x3FFA + (v - 0xFFFA)
        m1[q:q + 2] = bytes([0x00, 0xC0])
    with open(os.path.join(out, "fix_nes_mmc1table.nes"), "wb") as f:
        f.write(m1)
    # NES UxROM cross-bank pointer table: the table sits in bank 0 ($8100) and
    # its words ($8140/$8150/$8160) target code in bank 1 — the game selects
    # the target bank before the indirect jump, so no own-bank resolution
    # exists.  Bank 0 reads the table as data, bank 1 executes the targets.
    xb = bytearray(16 + 4 * 0x4000)
    xb[0:4] = b"NES\x1a"
    xb[4], xb[5], xb[6] = 4, 0, 0x20
    code = bytes([0xAD, 0x00, 0x81, 0xAD, 0x02, 0x81, 0xAD, 0x04, 0x81, 0x4C, 0x09, 0x80])
    xb[16:16 + len(code)] = code
    for k, a in enumerate((0x40, 0x50, 0x60)):
        xb[16 + 0x100 + 2 * k:16 + 0x100 + 2 * k + 2] = bytes([a, 0x81])
    o = 16 + 0x4000
    code = bytes([0x20, 0x40, 0x81, 0x20, 0x50, 0x81, 0x20, 0x60, 0x81, 0x4C, 0x09, 0x80])
    xb[o:o + len(code)] = code
    # CPU $8140/$8150/$8160 in bank 1 = bank offsets 0x140/0x150/0x160
    for a in (0x140, 0x150, 0x160):
        xb[o + a] = 0x60
    o = 16 + 3 * 0x4000
    xb[o:o + 3] = bytes([0x4C, 0x00, 0xC0])
    for v in (0xFFFA, 0xFFFC, 0xFFFE):
        q = o + 0x3FFA + (v - 0xFFFA)
        xb[q:q + 2] = bytes([0x00, 0xC0])
    with open(os.path.join(out, "fix_nes_xbanktable.nes"), "wb") as f:
        f.write(xb)
    # NES padding slide: lda #1; jmp $8100 into a zero-filled PRG
    n = bytearray(16 + 2 * 0x4000 + 0x2000)
    n[0:4] = b"NES\x1a"
    n[4], n[5] = 2, 1
    code = bytes([0xA9, 0x01, 0x4C, 0x00, 0x81])
    n[16:16 + len(code)] = code
    for v in (0xFFFA, 0xFFFC, 0xFFFE):
        q = 16 + 0x4000 + (v - 0xFFFA) + 0x3FFA
        n[q:q + 2] = bytes([0x00, 0x80])
    with open(os.path.join(out, "fix_nes_slide.nes"), "wb") as f:
        f.write(n)
    # NES UxROM (4 x 16 KiB PRG, CHR-RAM): the fixed bank spins at $C000; banks 0-2
    # hold code reachable only by an explicit entry or --seed-banks
    u = bytearray(16 + 4 * 0x4000)
    u[0:4] = b"NES\x1a"
    u[4], u[5], u[6] = 4, 0, 0x20
    for b in range(3):
        c = bytes([0xA9, b + 1, 0x85, 0x10, 0x4C, 0x04, 0x80])
        u[16 + b * 0x4000:16 + b * 0x4000 + len(c)] = c
    o = 16 + 3 * 0x4000
    u[o:o + 3] = bytes([0x4C, 0x00, 0xC0])
    for v in (0xFFFA, 0xFFFC, 0xFFFE):
        q = o + 0x3FFA + (v - 0xFFFA)
        u[q:q + 2] = bytes([0x00, 0xC0])
    with open(os.path.join(out, "fix_nes_uxrom.nes"), "wb") as f:
        f.write(u)
    # PC Engine 16 KiB HuCard (same program as test_pce): MPR setup, WRAM branch, VDC write
    p = bytearray(0x4000)
    code = bytes([0x78, 0xD4, 0xA9, 0xFF, 0x53, 0x01, 0xA9, 0xF8, 0x53, 0x02, 0xA9, 0x01, 0x53, 0x04,
                  0xAD, 0x00, 0x20, 0xD0, 0x05, 0xAD, 0x00, 0x40, 0x80, 0x03, 0xAD, 0x08, 0x40,
                  0x03, 0x02, 0xAD, 0x10, 0x40, 0x8D, 0x02, 0x00, 0x80, 0xFE])
    p[0:len(code)] = code
    for i in range(0x20):
        p[0x2000 + i] = 0x40 + i
    p[0x1FFE], p[0x1FFF] = 0x00, 0xE0
    with open(os.path.join(out, "fix_pce_branch.pce"), "wb") as f:
        f.write(p)
    # PC Engine VDC vblank interrupt: IRQ1 unmasked, CLI, spin; the handler at $E040 acknowledges and counts
    p = bytearray(0x4000)
    code = bytes([0x78, 0xD4, 0xA9, 0xFF, 0x53, 0x01, 0xA9, 0xF8, 0x53, 0x02,
                  0x03, 0x05, 0x13, 0x08, 0x23, 0x00, 0xA9, 0x00, 0x8D, 0x02, 0x14, 0x58, 0x80, 0xFE])
    p[0:len(code)] = code
    p[0x40:0x46] = bytes([0xAD, 0x02, 0x00, 0xE6, 0x00, 0x40])
    for v in (0x1FF6, 0x1FF8, 0x1FFA, 0x1FFC):
        p[v], p[v + 1] = 0x40, 0xE0
    p[0x1FFE], p[0x1FFF] = 0x00, 0xE0
    with open(os.path.join(out, "fix_pce_irq.pce"), "wb") as f:
        f.write(p)
    # PC Engine: writes to the CD-ROM2 interface ($180D ADPCM, $1800 CD) through the hardware page
    p = bytearray(0x4000)
    code = bytes([0x78, 0xD4, 0xA9, 0xFF, 0x53, 0x01, 0xA9, 0x00, 0x8D, 0x0D, 0x18, 0x8D, 0x0E, 0x18,
                  0x8D, 0x00, 0x18, 0x80, 0xFE])
    p[0:len(code)] = code
    p[0x1FFE], p[0x1FFF] = 0x00, 0xE0
    with open(os.path.join(out, "fix_pce_adpcm.pce"), "wb") as f:
        f.write(p)
    # PC Engine: SuperGrafx VPC ($0008) and second VDC ($0010) writes
    p = bytearray(0x4000)
    code = bytes([0x78, 0xD4, 0xA9, 0xFF, 0x53, 0x01, 0xA9, 0x00, 0x8D, 0x08, 0x00, 0x8D, 0x10, 0x00,
                  0x8D, 0x12, 0x00, 0x80, 0xFE])
    p[0:len(code)] = code
    p[0x1FFE], p[0x1FFF] = 0x00, 0xE0
    with open(os.path.join(out, "fix_pce_sgx.pce"), "wb") as f:
        f.write(p)
    # PC Engine padding slide: LDA #1 ; JMP $E020 into a zero-filled run
    w = bytearray(0x4000)
    w[0:5] = bytes([0xA9, 0x01, 0x4C, 0x20, 0xE0])
    w[0x1FFE], w[0x1FFF] = 0x00, 0xE0
    with open(os.path.join(out, "fix_pce_slide.pce"), "wb") as f:
        f.write(w)
    # PC Engine context loop: WRAM at MPR1, INC zp / LDA #0 / BEQ back; only the RAM differs
    z = bytearray(0x4000)
    z[0:11] = bytes([0x78, 0xA9, 0xF8, 0x53, 0x02, 0xE6, 0x10, 0xA9, 0x00, 0xF0, 0xFA])
    z[0x1FFE], z[0x1FFF] = 0x00, 0xE0
    with open(os.path.join(out, "fix_pce_ctx.pce"), "wb") as f:
        f.write(z)
    # PC Engine copy siblings: reads a table word, block-moves 32 bytes from $E200 to the VDC;
    # the table ($E100) also names $F000 and $F800 (32-byte streams never copied, far apart)
    g = bytearray(0x4000)
    code = bytes([0x78, 0xD4, 0xA9, 0xFF, 0x53, 0x01, 0xAD, 0x00, 0xE1, 0xAD, 0x01, 0xE1,
                  0xE3, 0x00, 0xE2, 0x02, 0x00, 0x20, 0x00, 0x80, 0xFE])
    g[0:len(code)] = code
    g[0x100:0x106] = bytes([0x00, 0xE2, 0x00, 0xF0, 0x00, 0xF8])
    for t, base in ((0x200, 0x11), (0x1000, 0x41), (0x1800, 0x71)):
        for k in range(32):
            g[t + k] = base + k * 3
    g[0x1FFE], g[0x1FFF] = 0x00, 0xE0
    with open(os.path.join(out, "fix_pce_sibling.pce"), "wb") as f:
        f.write(g)
    # PC Engine indirect jumps: JMP ($E100) then LDX #2 ; JMP ($E100,X), two distinct targets
    ij = bytearray(0x4000)
    ij[0:3] = bytes([0x6C, 0x00, 0xE1])
    ij[8:13] = bytes([0xA2, 0x02, 0x7C, 0x00, 0xE1])
    ij[0x10:0x12] = bytes([0x80, 0xFE])
    ij[0x100:0x104] = bytes([0x08, 0xE0, 0x10, 0xE0])
    ij[0x1FFE], ij[0x1FFF] = 0x00, 0xE0
    with open(os.path.join(out, "fix_pce_ind.pce"), "wb") as f:
        f.write(ij)
    # PC Engine dispatch table: never read, three words naming code under window $E000
    dt = bytearray(0x4000)
    dt[0:2] = bytes([0x80, 0xFE])
    dt[0x300:0x306] = bytes([0x40, 0xE0, 0x60, 0xE0, 0x80, 0xE0])
    for off in (0x40, 0x60, 0x80):
        dt[off:off + 4] = bytes([0xA9, 0x01, 0x80, 0xFE])
    dt[0x1FFE], dt[0x1FFF] = 0x00, 0xE0
    with open(os.path.join(out, "fix_pce_disp.pce"), "wb") as f:
        f.write(dt)
    # PC Engine provenance: pointer loaded from a ROM table into zero page, then JMP ($2020)
    pv = bytearray(0x4000)
    code = bytes([0xA9, 0xF8, 0x53, 0x02, 0xAD, 0x00, 0xE1, 0x85, 0x20, 0xAD, 0x01, 0xE1, 0x85, 0x21, 0x6C, 0x20, 0x20])
    pv[0:len(code)] = code
    pv[0x20:0x22] = bytes([0x80, 0xFE])
    pv[0x100:0x106] = bytes([0x20, 0xE0, 0x40, 0xE0, 0x00, 0x00])   # second word: a neighbour in the same table
    pv[0x40:0x44] = bytes([0xA9, 0x01, 0x80, 0xFE])
    pv[0x1FFE], pv[0x1FFF] = 0x00, 0xE0
    with open(os.path.join(out, "fix_pce_prov.pce"), "wb") as f:
        f.write(pv)

if __name__ == "__main__":
    build_nes_pce(sys.argv[1] if len(sys.argv) > 1 else ".")
    build_z80_family(sys.argv[1] if len(sys.argv) > 1 else ".")
    build_md_z80(sys.argv[1] if len(sys.argv) > 1 else ".")
    build_md_techniques(sys.argv[1] if len(sys.argv) > 1 else ".")
    build_techniques_8bit(sys.argv[1] if len(sys.argv) > 1 else ".")
    build_md(sys.argv[1] if len(sys.argv) > 1 else ".")
    build(sys.argv[1] if len(sys.argv) > 1 else ".")
