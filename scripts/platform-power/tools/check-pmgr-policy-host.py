#!/usr/bin/env python3
"""Host tests of SM6125 platform power boot-arg selection policy."""
import subprocess
import sys
import tempfile
from pathlib import Path

POWER_DIR = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'pmgr-policy-host'
    build = subprocess.run(
        [
            'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
            '-fsanitize=address,undefined',
            f'-I{POWER_DIR}',
            str(POWER_DIR / 'tests/pmgr_policy_host.c'),
            '-o', str(exe),
        ],
        capture_output=True, text=True,
    )
    if build.returncode != 0:
        sys.stderr.write(build.stderr)
        raise SystemExit('FAIL: pmgr policy host tests (build)')
    result = subprocess.run([str(exe)], capture_output=True, text=True)
sys.stderr.write(result.stderr)
if result.returncode != 0:
    raise SystemExit('FAIL: pmgr policy host tests')
print(result.stdout.strip())
print('PASS: pmgr policy host tests')
