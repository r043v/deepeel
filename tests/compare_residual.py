import json
import pathlib
import subprocess
import sys
import tempfile

tool = pathlib.Path(sys.argv[1])


def report(ranges):
    rows = [{"bank": b, "lo": lo, "hi": hi, "len": hi - lo + 1} for b, lo, hi in ranges]
    return {"map": {"residual": rows, "residual_ranges": len(rows),
                    "residual_bytes": sum(r["len"] for r in rows)}}


with tempfile.TemporaryDirectory() as d:
    a, b = pathlib.Path(d) / "a.json", pathlib.Path(d) / "b.json"
    a.write_text(json.dumps(report([(0, 0, 9), (1, 0, 3)])))
    b.write_text(json.dumps(report([(0, 3, 6), (0, 12, 13), (1, 2, 5)])))
    def run():
        return subprocess.run([sys.executable, str(tool), str(a), str(b)], capture_output=True, text=True)
    result = run()
    assert result.returncode == 0, result.stderr
    data = json.loads(result.stdout)
    assert data["became_classified_bytes"] == 8
    assert data["became_unknown_bytes"] == 4
    assert data["identity_verified"] is False
    invalid = report([(0, 0, 2), (0, 2, 4)])
    b.write_text(json.dumps(invalid))
    assert run().returncode != 0
    invalid = report([(0, 0, 2)])
    invalid["map"]["residual_omitted"] = 1
    b.write_text(json.dumps(invalid))
    assert run().returncode != 0
    invalid["map"].pop("residual_omitted")
    invalid["map"]["residual_ranges"] = 2
    b.write_text(json.dumps(invalid))
    assert run().returncode != 0
    first, second = report([]), report([])
    first["rom_size"], second["rom_size"] = 32768, 65536
    a.write_text(json.dumps(first))
    b.write_text(json.dumps(second))
    assert run().returncode != 0
