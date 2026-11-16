#!/usr/bin/env python3
"""Check fresh CLI reports without accepting leftovers from earlier runs."""
import pathlib
import re
import subprocess
import sys
import tempfile


def main():
    mode, binary, fixture_dir = sys.argv[1:]
    fixtures = pathlib.Path(fixture_dir)
    with tempfile.TemporaryDirectory(prefix="deepeel-report-") as directory:
        report = pathlib.Path(directory) / "report.txt"

        def run(arguments):
            report.unlink(missing_ok=True)
            subprocess.run([binary, *arguments, "--text", str(report)],
                           check=True, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, timeout=60)
            return report.read_text()

        if mode == "md_liveness":
            for fixture, pattern in (("fix_md_cpu_vram.md", r"section 1 000200-.* required hits .* live: video"),
                                     ("fix_md_pad.md", r"live:.*\binput\b"),
                                     ("fix_md_dbcc.md", r"live:$")):
                output = run(["md", "explore", str(fixtures / fixture), "--attract-frames", "0"])
                if not re.search(pattern, output, re.MULTILINE):
                    raise AssertionError(f"{fixture}: missing {pattern!r} in report")
                if fixture == "fix_md_pad.md":
                    for expected in (r"^SRAM: 0 instructions, 0 CPU reads$",
                                     r"^\s+A10002 PAD1_DATA\s+input\s+r=[1-9][0-9]*"):
                        if not re.search(expected, output, re.MULTILINE):
                            raise AssertionError(f"{fixture}: missing {expected!r} in report")
        else:
            raise ValueError(f"unknown check: {mode}")


if __name__ == "__main__":
    main()
