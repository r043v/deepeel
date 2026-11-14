import subprocess
import sys

result = subprocess.run([sys.argv[1], "explore", sys.argv[2], "--legacy",
                         "--max-total", "1000", "--no-closure"],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                        text=True, timeout=60)
assert result.returncode == 0, result.stderr
assert "packed queue image corrupt" not in result.stderr, result.stderr
assert "exploration: 0 steps" not in result.stdout, result.stdout
