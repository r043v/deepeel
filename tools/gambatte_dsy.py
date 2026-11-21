#!/usr/bin/env python3
"""Write a DSY1 reference for a power-on CGB Gambatte BK2 with VBlank-driven frames."""

import argparse
from contextlib import ExitStack
import ctypes as ct
import hashlib
import json
from pathlib import Path
import struct
import zipfile


def replay(args):
    rom = args.rom.read_bytes()
    bios = args.bios.read_bytes()
    with zipfile.ZipFile(args.movie) as movie:
        header = dict(line.split(maxsplit=1) for line in
                      movie.read("Header.txt").decode("utf-8-sig").splitlines()
                      if len(line.split(maxsplit=1)) == 2)
        sync = json.loads(movie.read("SyncSettings.json"))["o"]
        log = movie.read("Input Log.txt").decode("utf-8-sig").splitlines()
        if (header.get("Core") != "Gambatte" or header.get("IsCGBMode") != "1"
                or sync.get("EqualLengthFrames") is not False
                or sync.get("GBACGB") or sync.get("MulticartCompat")
                or sync.get("InitialDiv", 0) or sync.get("RTCInitialTime", 0)
                or sync.get("RealTimeRTC")
                or any(header.get(key, "").lower() == "true" for key in
                       ("StartsFromSavestate", "StartsFromSaveRam", "StartsFromSaveram"))):
            raise ValueError("expected a power-on CGB Gambatte movie with VBlank-driven frames")
    for key, data in (("SHA1", rom), ("GBC_Firmware_World", bios)):
        if hashlib.sha1(data).hexdigest().upper() != header.get(key, "").upper():
            raise ValueError(f"{key} does not match the BK2")
    if "LogKey:#Up|Down|Left|Right|Start|Select|B|A|Power|" not in log:
        raise ValueError("unexpected BK2 input layout")
    rows = [line[1:-1] for line in log if line.startswith("|")]
    if any(len(row) != 9 or row[8] != "." for row in rows):
        raise ValueError("unsupported BK2 input row or Power event")

    lib = ct.CDLL(str(args.library.resolve()))
    ptr, uint, integer = ct.c_void_p, ct.c_uint, ct.c_int

    def bind(name, result, *params):
        fn = getattr(lib, "gambatte_" + name)
        fn.restype, fn.argtypes = result, params
        return fn

    create = bind("create", ptr)
    destroy = bind("destroy", None, ptr)
    core = create()
    if not core:
        raise ValueError("could not allocate Gambatte core")
    try:
        if bind("loadbiosbuf", integer, ptr, ct.c_char_p, uint)(core, bios, len(bios)):
            raise ValueError("Gambatte rejected the BIOS")
        # BizHawk's force-CGB and read-only-SRAM flags for deterministic replay.
        if bind("loadbuf", integer, ptr, ct.c_char_p, uint, uint)(core, rom, len(rom), 17):
            raise ValueError("Gambatte rejected the ROM")
        mask = 0
        callback_type = ct.CFUNCTYPE(uint, ptr)
        callback = callback_type(lambda _: mask)
        bind("setinputgetter", None, ptr, callback_type, ptr)(core, callback, None)
        run = bind("runfor", integer, ptr, ptr, integer, ptr, ct.POINTER(uint))
        area = bind("getmemoryarea", ct.c_bool, ptr, integer, ct.POINTER(ptr), ct.POINTER(integer))
        read = bind("cpuread", ct.c_ubyte, ptr, ct.c_ushort)
        get_regs = bind("getregs", None, ptr, ptr)
        video = (uint * (160 * 144))()
        audio = (ct.c_short * (2 * 40000))()
        regs = (integer * 10)()
        elapsed_samples = 0
        with ExitStack() as stack:
            output = stack.enter_context(args.output.open("wb"))
            if args.instructions:
                trace = stack.enter_context(args.instructions.open("w"))
                trace_type = ct.CFUNCTYPE(None, ct.POINTER(uint))

                @trace_type
                def instruction(r):
                    # Gambatte supplies pre-instruction registers and the
                    # sample offset within the current runfor call.
                    trace.write(f"{elapsed_samples + r[0]} {r[1]:04x} {r[3]:02x} {r[8]:02x} "
                                f"{r[4]:02x}{r[5]:02x} {r[6]:02x}{r[7]:02x} "
                                f"{r[9]:02x}{r[10]:02x} {r[2]:04x} {r[11]}\n")

                bind("settracecallback", None, ptr, trace_type)(core, instruction)
            output.write(b"DSY1")
            for frame, row in enumerate(rows[:args.frames], 1):
                mask = sum(bit for char, bit in zip(row, (64, 128, 32, 16, 8, 4, 2, 1)) if char != ".")
                samples = uint(35112)
                run(core, video, 160, audio, ct.byref(samples))
                elapsed_samples += samples.value
                get_regs(core, regs)
                pc, sp, a, b, c, d, e, f, h, l = regs
                blocks = {
                    "regs": struct.pack("<HHBBHHH", pc, sp, a, f & 0xf0, b << 8 | c, d << 8 | e, h << 8 | l),
                    "ie": bytes([read(core, 0xffff)]),
                }
                for name, index in (("wram_banks", 2), ("vram", 0), ("sram", 3), ("oam", 4), ("hram", 5)):
                    address, length = ptr(), integer()
                    if area(core, index, ct.byref(address), ct.byref(length)) and length.value:
                        blocks[name] = ct.string_at(address, length.value)
                wram = blocks["wram_banks"]
                bank = read(core, 0xff70) & 7 or 1
                blocks["wram"] = wram[:0x1000] + wram[bank * 0x1000:(bank + 1) * 0x1000]
                output.write(struct.pack("<IH", frame, len(blocks)))
                for name, data in blocks.items():
                    output.write(bytes([len(name)]) + name.encode("ascii") + struct.pack("<I", len(data)))
                    output.write(data)
    finally:
        destroy(core)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("library", "rom", "bios", "movie", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--frames", type=int)
    parser.add_argument("--instructions", type=Path, metavar="FILE",
                        help="also export pre-instruction sample time, PC, A, F, BC, DE, HL, SP, prefetch flag")
    args = parser.parse_args()
    if args.frames is not None and args.frames < 1:
        parser.error("--frames must be positive")
    try:
        replay(args)
    except (ValueError, OSError, KeyError, AttributeError) as exc:
        parser.exit(1, f"{parser.prog}: {exc}\n")


if __name__ == "__main__":
    main()
