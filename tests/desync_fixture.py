import struct, sys
def w(path, tweak):
    with open(path, "wb") as f:
        f.write(b"DSY1")
        for fr in range(6):
            wram = bytearray(64); wram[0x10] = 0xAA
            if tweak and fr >= 4: wram[0x10] = 0xAB
            for nm, d in (("regs", bytes([fr] * 8)), ("wram", bytes(wram))):
                pass
            f.write(struct.pack("<IH", fr, 2))
            for nm, d in (("regs", bytes([fr] * 8)), ("wram", bytes(wram))):
                f.write(bytes([len(nm)]) + nm.encode() + struct.pack("<I", len(d)) + d)
w(sys.argv[1] + "/dsy_a.dsy", False); w(sys.argv[1] + "/dsy_b.dsy", True)
