#!/usr/bin/env python3
"""Write a DSY1 reference for a power-on CGB Gambatte BK2 with VBlank-driven frames."""

import argparse
from contextlib import ExitStack
import ctypes as ct
import hashlib
import io
import json
from pathlib import Path
import struct
import zipfile
import zlib


def replay(args):
    rom = args.rom.read_bytes()
    bios = args.bios.read_bytes()
    with zipfile.ZipFile(args.movie) as movie:
        header = dict(line.split(maxsplit=1) for line in
                      movie.read("Header.txt").decode("utf-8-sig").splitlines()
                      if len(line.split(maxsplit=1)) == 2)
        sync = json.loads(movie.read("SyncSettings.json"))["o"]
        if (header.get("Core") != "Gambatte" or header.get("IsCGBMode") != "1"
                or sync.get("EqualLengthFrames") is not False
                or (not args.bizhawk_23 and (sync.get("GBACGB") or sync.get("MulticartCompat")
                                           or sync.get("InitialDiv", 0) or sync.get("RTCInitialTime", 0)))
                or sync.get("RealTimeRTC")
                or any(header.get(key, "").lower() == "true" for key in
                       ("StartsFromSavestate", "StartsFromSaveRam", "StartsFromSaveram"))):
            raise ValueError("expected a power-on CGB Gambatte movie with VBlank-driven frames")
        masks = bytearray()
        expected_key = "LogKey:#Up|Down|Left|Right|Start|Select|B|A|Power|"
        found_key = False
        bits = (64, 128, 32, 16, 8, 4, 2, 1)
        with io.TextIOWrapper(movie.open("Input Log.txt"), encoding="utf-8-sig") as log:
            for line in log:
                line = line.rstrip("\r\n")
                if line == expected_key:
                    found_key = True
                elif line.startswith("|"):
                    row = line[1:-1]
                    if len(row) != 9 or row[8] != ".":
                        raise ValueError("unsupported BK2 input row or Power event")
                    if args.frames is None or len(masks) < args.frames:
                        masks.append(sum(bit for char, bit in zip(row, bits) if char != "."))
        if not found_key:
            raise ValueError("unexpected BK2 input layout")
    for key, data in (("SHA1", rom), ("GBC_Firmware_World", bios)):
        if hashlib.sha1(data).hexdigest().upper() != header.get(key, "").upper():
            raise ValueError(f"{key} does not match the BK2")

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
        mask = 0
        frame = 0
        if args.bizhawk_23:
            initial_time = sync.get("RTCInitialTime", 0)
            initial_div = sync.get("InitialDiv", 0) & 0xfffc
            flags = (2 if sync.get("GBACGB") else 0) | (4 if sync.get("MulticartCompat") else 0)
            if sync.get("ConsoleMode") == 1 or len(bios) != 0x900:
                raise ValueError("expected a CGB movie and a 2304-byte BIOS")
            if bind("load", integer, ptr, ct.c_char_p, uint, ct.c_longlong, uint, uint)(
                    core, rom, len(rom), 0, flags, initial_div):
                raise ValueError("Gambatte rejected the ROM")
            if bind("loadgbcbios", integer, ptr, ct.c_char_p)(core, bios):
                raise ValueError("Gambatte rejected the BIOS")
            callback_type = ct.CFUNCTYPE(uint)
            callback = callback_type(lambda: mask)
            clock = callback_type(lambda: frame * 4389 // 262144 + initial_time)
            bind("setinputgetter", None, ptr, callback_type)(core, callback)
            bind("setrtccallback", None, ptr, callback_type)(core, clock)
            run = bind("runfor", integer, ptr, ptr, ct.POINTER(uint))
            blit = bind("blitto", None, ptr, ptr, integer)
        else:
            if bind("loadbiosbuf", integer, ptr, ct.c_char_p, uint)(core, bios, len(bios)):
                raise ValueError("Gambatte rejected the BIOS")
            # BizHawk's force-CGB and read-only-SRAM flags for deterministic replay.
            if bind("loadbuf", integer, ptr, ct.c_char_p, uint, uint)(core, rom, len(rom), 17):
                raise ValueError("Gambatte rejected the ROM")
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
                    # The 2.3 ABI supplies a CPU counter. The newer ABI
                    # supplies a sample offset from the current runfor call.
                    when = r[0] if args.bizhawk_23 else elapsed_samples + r[0]
                    trace.write(f"{when} {r[1]:04x} {r[3]:02x} {r[8]:02x} "
                                f"{r[4]:02x}{r[5]:02x} {r[6]:02x}{r[7]:02x} "
                                f"{r[9]:02x}{r[10]:02x} {r[2]:04x} {r[11]}\n")

                set_trace = bind("settracecallback", None, ptr, trace_type)
            output.write(b"DSY1")
            for frame, mask in enumerate(masks, 1):
                if args.instructions:
                    if frame == args.trace_start:
                        set_trace(core, instruction)
                    elif frame == args.trace_end + 1:
                        set_trace(core, trace_type())
                samples = uint(35112)
                if args.bizhawk_23:
                    if run(core, audio, ct.byref(samples)) > 0:
                        blit(core, video, 160)
                else:
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
                        size = min(length.value, 0x7f) if name == "hram" else length.value
                        blocks[name] = ct.string_at(address, size)
                wram = blocks["wram_banks"]
                bank = read(core, 0xff70) & 7 or 1
                blocks["wram"] = wram[:0x1000] + wram[bank * 0x1000:(bank + 1) * 0x1000]
                if frame < args.from_frame:
                    continue
                if args.digest:   # same names and CRC-32 as DEEPEEL_DSY_DIGEST=1 on the native side
                    crc = lambda data: zlib.crc32(data).to_bytes(4, "little")
                    wr, vr = blocks["wram_banks"], blocks["vram"]
                    blocks = {"regs": blocks["regs"], "hram": blocks["hram"], "ie": blocks["ie"],
                              **{f"wram{i}_crc": crc(wr[i * 0x1000:(i + 1) * 0x1000]) for i in range(len(wr) // 0x1000)},
                              **{f"vram{i}_crc": crc(vr[i * 0x2000:(i + 1) * 0x2000]) for i in range(len(vr) // 0x2000)},
                              "oam_crc": crc(blocks["oam"]),
                              **({"sram_crc": crc(blocks["sram"])} if "sram" in blocks else {})}
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
    parser.add_argument("--bizhawk-23", action="store_true", help="use the Gambatte ABI from BizHawk 2.3")
    parser.add_argument("--instructions", type=Path, metavar="FILE",
                        help="also export pre-instruction time, PC, A, F, BC, DE, HL, SP, prefetch flag")
    parser.add_argument("--digest", action="store_true", help="write regs/hram/ie and CRC-32 of the large blocks only")
    parser.add_argument("--from-frame", type=int, default=0, metavar="N", help="write only frames >= N")
    parser.add_argument("--trace-start", type=int, default=1, metavar="FRAME")
    parser.add_argument("--trace-end", type=int, default=float("inf"), metavar="FRAME")
    args = parser.parse_args()
    if args.frames is not None and args.frames < 1:
        parser.error("--frames must be positive")
    if not 1 <= args.trace_start <= args.trace_end:
        parser.error("invalid instruction trace frame range")
    try:
        replay(args)
    except (ValueError, OSError, KeyError, AttributeError) as exc:
        parser.exit(1, f"{parser.prog}: {exc}\n")


if __name__ == "__main__":
    main()
