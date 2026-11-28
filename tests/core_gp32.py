#!/usr/bin/env python3
"""GP32 kernel contract on generated applications: both sides of a branch, both
entries of a computed jump, Thumb code and an input register are covered, and a
self-unpacking application is followed past its unpacker."""
import os, re, struct, subprocess, sys, tempfile

tool, fixtures = sys.argv[1], sys.argv[2]
with tempfile.TemporaryDirectory() as work:
    def run(name, *options, source=None):
        code = os.path.join(work, name + ".code")
        text = os.path.join(work, name + ".txt")
        out = subprocess.run([tool, "gp32", "explore", source or os.path.join(fixtures, name + ".gxb"), "--jobs", "1",
                              "--code", code, "--text", text, *options],
                             check=True, capture_output=True, text=True, cwd=work).stdout
        classes = {}
        with open(code) as f:
            for line in f:
                fields = line.split()
                if len(fields) == 4 and not line.startswith("#"):
                    start, length = int(fields[0], 16), int(fields[1], 16)
                    for at in range(start, start + length):
                        classes[at] = (fields[2], fields[3])
        return out, classes

    def expect(classes, offsets, mode, what):
        for at in offsets:
            got = classes.get(at)
            assert got and got[0] in ("natural", "forced", "static") and got[1] == mode, (what, hex(at), got)

    out, classes = run("fix_gp32_branch", "--probe-frames", "4", "--random", "0", "--attract-frames", "0",
                       "--max-steps", "2000", "--max-total", "200000")
    expect(classes, (0x1c, 0x28, 0x2c, 0x34, 0x38, 0x3c, 0x4c, 0x54, 0x58, 0x60, 0x64), "arm", "ARM path")
    expect(classes, (0x70, 0x72), "thumb", "Thumb routine")
    assert "$15600030" in out and "PEDAT" in out, "the input register is not reported"

    assert "static register references: 1 of 170" in out, out

    # every registry entry: an application reads each address from a table, and
    # the exploration reports all of them; a store through a base register is
    # also found by the static sweep.
    listing = subprocess.run([tool, "regs", "gp32"], check=True, capture_output=True, text=True).stdout
    addresses = [int(m.group(1), 16) for m in re.finditer(r"^\s+([0-9a-f]{8})-[0-9a-f]{8}\s", listing, re.M)]
    assert len(addresses) == 170, len(addresses)
    base = 0x0c000000
    words = [0xe59f0020,   # 1c ldr r0, =table
             0xe3a02000 | len(addresses),   # 20 mov r2, #170
             0xe4901004,   # 24 ldr r1, [r0], #4
             0xe5913000,   # 28 ldr r3, [r1]
             0xe2522001,   # 2c subs r2, r2, #1
             0x1afffffb,   # 30 bne 24
             0xe59f400c,   # 34 ldr r4, =TCFG0
             0xe5943004,   # 38 ldr r3, [r4, #4]
             0xeafffffe,   # 3c b .
             0x00000000,   # 40 unused
             base + 0x4c,  # 44 literal: the table
             0x15100000,   # 48 literal: TCFG0
             *addresses]
    size = 0x1c + 4 * len(words)
    header = [0xea000005, base] + [base + size] * 5
    registers = os.path.join(work, "registers.gxb")
    with open(registers, "wb") as f:
        f.write(struct.pack("<%dI" % (len(header) + len(words)), *header, *words))
    out, classes = run("registers", "--probe-frames", "4", "--random", "0", "--attract-frames", "0",
                       "--max-steps", "4000", "--max-total", "100000", source=registers)
    assert "register accesses: 170 of 170" in out, out
    assert "static register references: 2 of 170" in out, out

    out, classes = run("fix_gp32_unpack", "--random", "0", "--attract-frames", "0",
                       "--max-total", "40000000", "--max-steps", "40000000")
    assert "self-unpacking application" in out, out
    assert "SDRAM snapshot" in out, out
    expect(classes, (0x1c, 0x20, 0x3c, 0x4c, 0x60), "arm", "unpacker stub")
    expect(classes, (0x100000, 0x100000 + 4 * 1199), "arm", "unpacked routine")
print("ok")
