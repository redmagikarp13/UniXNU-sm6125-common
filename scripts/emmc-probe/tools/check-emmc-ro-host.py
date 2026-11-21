#!/usr/bin/env python3
"""Host ASan+UBSan tests for SM6125 strictly read-only eMMC/GCC probe core."""
import subprocess
import sys
import tempfile
from pathlib import Path

PROBE_DIR = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'emmc-ro-host'
    build = subprocess.run(
        [
            'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
            '-fsanitize=address,undefined',
            f'-I{PROBE_DIR}',
            str(PROBE_DIR / 'Sm6125EmmcReadOnly.c'),
            str(PROBE_DIR / 'tests/emmc_ro_host.c'),
            '-o', str(exe),
        ],
        capture_output=True, text=True,
    )
    if build.returncode != 0:
        sys.stderr.write(build.stderr)
        raise SystemExit('FAIL: emmc-ro host tests (build)')
    result = subprocess.run([str(exe)], capture_output=True, text=True)
sys.stderr.write(result.stderr)
if result.returncode != 0:
    raise SystemExit('FAIL: emmc-ro host tests')
print(result.stdout.strip())
print('PASS: emmc-ro host tests')
