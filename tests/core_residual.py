import json
import pathlib
import subprocess
import sys
import tempfile

exe, rom, compare = sys.argv[1:]
with tempfile.TemporaryDirectory() as d:
    summary = pathlib.Path(d) / "summary.json"
    image = pathlib.Path(d) / "map.txt"
    run = subprocess.run([exe, "explore", rom, "--core", "--max-total", "1000",
                          "--no-closure", "--summary", str(summary), "--map", str(image)],
                         capture_output=True, text=True, timeout=60)
    assert run.returncode == 0, run.stderr
    data = json.loads(summary.read_text())
    assert data["instruction_starts_total"] > 0
    assert data["instruction_starts_omitted"] == 0
    assert len(data["instruction_starts"]) == data["instruction_starts_total"]
    assert any(row["index"] == 0x100 and row["natural"] for row in data["instruction_starts"])
    classes = image.read_text().splitlines()[1]
    m = data["map"]
    assert len(classes) == data["rom_size"]
    assert m["residual_omitted"] == 0
    assert m["residual_ranges"] == len(m["residual"])
    rebuilt = set()
    for row in m["residual"]:
        assert row["len"] == row["hi"] - row["lo"] + 1
        for offset in range(row["lo"], row["hi"] + 1):
            ix = row["bank"] * 16384 + offset
            assert ix not in rebuilt
            rebuilt.add(ix)
    assert rebuilt == {i for i, c in enumerate(classes) if c == "0"}
    assert m["residual_bytes"] == len(rebuilt)
    result = subprocess.run([sys.executable, compare, str(summary), str(summary)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)["became_classified_bytes"] == 0
    # Starts are part of coverage.bin and must survive a stopped/resumed run.
    state = pathlib.Path(d) / "state"
    first, resumed = pathlib.Path(d) / "first.json", pathlib.Path(d) / "resumed.json"
    def explore(output, resume=False):
        cmd = [exe, "explore", rom, "--core", "--state-dir", str(state),
               "--max-total", "1000", "--summary", str(output)]
        if resume:
            cmd.append("--resume")
        run = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        assert run.returncode == 0, run.stderr
        doc = json.loads(output.read_text())
        assert doc["instruction_starts_omitted"] == 0
        return {(row["index"], kind) for row in doc["instruction_starts"]
                for kind in ("natural", "forced") if row[kind]}
    old = explore(first)
    new = explore(resumed, True)
    assert old and old <= new
