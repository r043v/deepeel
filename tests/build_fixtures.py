#!/usr/bin/env python3
"""Builds the synthetic Game Boy fixtures for the memscan test suite.

Every fixture is a minimal ROM exercising one exploration behaviour:
  branch     - both sides of a conditional + a ROM->VRAM copy loop
  mbc5       - bank switching through MBC5 registers, cross-bank calls
  dma        - OAM DMA from a ROM source ($FF46)
  irq        - IE/IF set by hand (IRQ forking) + a VBlank handler
  jumptable  - JP HL through a pointer table, both handlers reachable
  loop       - infinite loop, must be contained by the guards
  unreach    - a data blob no code ever touches
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
    w(r, 0x220, 0x3E, 0x22, 0xEA, 0x00, 0x80, 0x76)  # handler A: vram $22, halt
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

if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else ".")
