#!/usr/bin/env python3
"""Host tests of virtual timer PPI DT parsing and GIC mask calculation."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'armcpu-timer-host'
    build = subprocess.run(
        ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
         str(ROOT / 'tests/armcpu_timer_host.c'), '-o', str(exe)],
        capture_output=True, text=True,
    )
    if build.returncode != 0:
        sys.stderr.write(build.stderr)
        raise SystemExit('FAIL: timer host tests (build)')
    result = subprocess.run([str(exe)], capture_output=True, text=True)
sys.stderr.write(result.stderr)
if result.returncode != 0:
    raise SystemExit('FAIL: timer host tests')
print(result.stdout.strip())
print('PASS: timer host tests')
