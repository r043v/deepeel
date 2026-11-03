#!/usr/bin/env python3
"""Builds the synthetic GB and Z80 fixtures for the deepeel test suite.

Every fixture is a minimal ROM exercising one exploration behaviour:
  branch     - both sides of a conditional + a ROM->VRAM copy loop
  mbc5       - bank switching through MBC5 registers, cross-bank calls
  dma        - OAM DMA from a ROM source ($FF46)
  irq        - IE/IF set by hand (IRQ forking) + a VBlank handler
  jumptable  - JP HL through a pointer table, both handlers reachable
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

    # --- hdma: CGB general-purpose HDMA, 256 bytes $4000 -> $8000 -------------
    r = rom(2)
    w(r, 0x100, 0x00, 0xC3, 0x50, 0x01)
    w(r, 0x150, 0x3E, 0x40, 0xE0, 0x51, 0xAF, 0xE0, 0x52, 0xAF, 0xE0, 0x53, 0xAF, 0xE0, 0x54,
       0x3E, 0x0F, 0xE0, 0x55, 0x76)
    for i in range(0x100):
        r[0x4000 + i] = (i * 7 + 3) & 0xFF
    save(out, "fix_hdma", r)

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
    header[22] = 0x40
    with open(os.path.join(out, "fix_md_movie_state.gmv"), "wb") as f:
        f.write(header + bytes([0xFF, 0xFF, 0xFF]))
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
    # A 2 MiB+ cartridge with SRAM overlay: execute and read SRAM without
    # accidentally attributing those accesses to the covered ROM bytes.
    r = md_rom()
    r.extend(bytes(0x200004 - len(r)))
    code = bytes.fromhex("13fc 0001 00a1 30f1 33fc 60fe 0020 0000 "
                         "1039 0020 0002 4ef9 0020 0000")
    r[0x200:0x200 + len(code)] = code
    r[0x200000:0x200004] = bytes.fromhex("4e71 1234")
    save_md(out, "fix_md_sram_overlay", r)

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
    m = bytearray([0xFF]) * 0x8000
    m[0:2] = b"AB"
    m[2:4] = (0x4010).to_bytes(2, "little")
    m[4:16] = bytes(12)
    m[0x10:0x10 + len(prog)] = prog
    with open(os.path.join(out, "fix_z80_branch.msx"), "wb") as f:
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

if __name__ == "__main__":
    build_nes_pce(sys.argv[1] if len(sys.argv) > 1 else ".")
    build_z80_family(sys.argv[1] if len(sys.argv) > 1 else ".")
    build_md_z80(sys.argv[1] if len(sys.argv) > 1 else ".")
    build_md(sys.argv[1] if len(sys.argv) > 1 else ".")
    build(sys.argv[1] if len(sys.argv) > 1 else ".")
