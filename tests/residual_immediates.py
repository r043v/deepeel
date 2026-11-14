import json
import pathlib
import subprocess
import sys
import tempfile

tool = sys.argv[1]
with tempfile.TemporaryDirectory() as tmp:
    rom = bytearray(0x8000)
    classes = list("3" * len(rom))
    rom[0x100:0x103] = b"\x21\x23\x41"
    classes[0x100:0x103] = "111"
    classes[0x123] = classes[0x4123] = "0"
    rom[0x110:0x113] = b"\x21\x23\x41"
    classes[0x110:0x113] = "333"  # static bytes are not observed execution
    rom[0x120:0x123] = b"\x11\x00\xc0"  # RAM is not a ROM address
    classes[0x120:0x123] = "111"
    r, m = pathlib.Path(tmp) / "fixture.gb", pathlib.Path(tmp) / "fixture.map"
    r.write_bytes(rom)
    m.write_text("gb map v1\n" + "".join(classes) + "\n")
    result = subprocess.run([sys.executable, tool, str(r), str(m), "--limit", "1"], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    data = json.loads(result.stdout)
    assert data["candidates_total"] == 2 and data["candidates_omitted"] == 1
    assert data["candidates"][0]["site_index"] == 0x100
    assert data["candidates"][0]["target_index"] == 0x123
    summary = pathlib.Path(tmp) / "fixture.json"
    summary.write_text(json.dumps({"platform": "gb", "engine": "kernel", "rom_size": len(rom),
                                   "instruction_starts": [{"index": 0x110}],
                                   "instruction_starts_total": 1, "instruction_starts_omitted": 0}))
    filtered = subprocess.run([sys.executable, tool, str(r), str(m), "--summary", str(summary)], capture_output=True, text=True)
    assert filtered.returncode == 0, filtered.stderr
    assert json.loads(filtered.stdout)["candidates_total"] == 0
    summary.write_text(json.dumps({"platform": "gb", "engine": "kernel", "rom_size": len(rom),
                                   "instruction_starts": [{"index": 0x100}],
                                   "instruction_starts_total": 1, "instruction_starts_omitted": 0}))
    filtered = subprocess.run([sys.executable, tool, str(r), str(m), "--summary", str(summary)], capture_output=True, text=True)
    assert json.loads(filtered.stdout)["candidates_total"] == 2
    # Nearby observed loads suggest, but cannot prove, a shared HL value.
    rom[0x130:0x136] = b"\x26\x41\x00\x2e\x23\x00"
    rom[0x140:0x144] = b"\x26\x41\x1e\x23"  # unrelated registers
    classes[0x130:0x132] = classes[0x133:0x135] = "11"
    classes[0x140:0x144] = "1111"
    r.write_bytes(rom)
    m.write_text("gb map v1\n" + "".join(classes) + "\n")
    rows = [{"index": i} for i in (0x130, 0x133, 0x140, 0x142)]
    summary.write_text(json.dumps({"platform": "gb", "engine": "kernel", "rom_size": len(rom),
                                   "instruction_starts": rows, "instruction_starts_total": len(rows),
                                   "instruction_starts_omitted": 0}))
    cmd = [sys.executable, tool, str(r), str(m), "--summary", str(summary), "--split-immediates"]
    split = subprocess.run(cmd, capture_output=True, text=True)
    assert split.returncode == 0, split.stderr
    found = json.loads(split.stdout)
    assert found["candidates_total"] == 2
    assert all(x["high_site_index"] == 0x130 and x["low_site_index"] == 0x133
               and x["pattern"] == "split ld hl" for x in found["candidates"])
    limited = json.loads(subprocess.run(cmd + ["--limit", "1"], capture_output=True, text=True, check=True).stdout)
    assert limited["candidates_total"] == 2 and limited["candidates_omitted"] == 1
    rom[0x150:0x154] = b"\x2e\x23\x26\x41"  # reverse order
    rom[0x160:0x162] = b"\x26\x41"
    rom[0x169:0x16b] = b"\x2e\x23"  # nine bytes apart
    classes[0x150:0x154] = "1111"
    classes[0x160:0x162] = classes[0x169:0x16b] = "11"
    r.write_bytes(rom)
    m.write_text("gb map v1\n" + "".join(classes) + "\n")
    rows = [{"index": i} for i in (0x150, 0x152, 0x160, 0x169)]
    summary.write_text(json.dumps({"platform": "gb", "engine": "kernel", "rom_size": len(rom),
                                   "instruction_starts": rows, "instruction_starts_total": len(rows),
                                   "instruction_starts_omitted": 0}))
    reverse = json.loads(subprocess.run(cmd, capture_output=True, text=True, check=True).stdout)
    assert reverse["candidates_total"] == 2
    assert all(x["high_site_index"] == 0x152 and x["low_site_index"] == 0x150
               for x in reverse["candidates"])
    assert subprocess.run([sys.executable, tool, str(r), str(m), "--split-immediates"], capture_output=True).returncode != 0
    m.write_text("wrong map\n")
    assert subprocess.run([sys.executable, tool, str(r), str(m)], capture_output=True).returncode != 0
