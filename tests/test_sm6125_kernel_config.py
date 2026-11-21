#!/usr/bin/env python3
"""TDD contract tests for SM6125 kernel config and _PE_create_console disassembly."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest

SOC_DIR = Path(__file__).resolve().parents[1]
CONFIG_PATH = SOC_DIR / 'configs/sm6125.config'

MOV_LOW_RE = re.compile(r'\bmovz?\s+[wx](\d+),\s*#0xc000\b', re.IGNORECASE)
MOVK_HIGH_RE = re.compile(r'\bmovk\s+[wx](\d+),\s*#0x3c0f,\s*lsl\s+#16\b', re.IGNORECASE)


def parse_kconfig_fragment(text: str) -> dict[str, str]:
    """Parse KEY=VALUE pairs from a Kconfig fragment (last assignment wins)."""
    values: dict[str, str] = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith('#') or '=' not in line:
            continue
        key, val = line.split('=', 1)
        values[key.strip()] = val.strip()
    return values


def check_pe_create_console_disasm(disasm: str, *, require_dt_probe: bool = True) -> list[str]:
    """Verify _PE_create_console does not access BCM2837 fixed PA 0x3c0fc000."""
    errors: list[str] = []
    lower = disasm.lower()
    low_regs = set(MOV_LOW_RE.findall(disasm))
    high_regs = set(MOVK_HIGH_RE.findall(disasm))
    if '0x3c0fc000' in lower or (low_regs & high_regs):
        errors.append(
            'BCM2837 fixed panic buffer address 0x3c0fc000 found in _PE_create_console'
        )
    if require_dt_probe and '_SecureDTLookupEntry' not in disasm:
        errors.append(
            'Missing _SecureDTLookupEntry in _PE_create_console (CONFIG_XNU_DT_PANIC_REGION not active)'
        )
    return errors


def _find_llvm_tool(name: str) -> str:
    candidate = Path('/opt/homebrew/opt/llvm@22/bin') / name
    if candidate.is_file() and os.access(candidate, os.X_OK):
        return str(candidate)
    resolved = shutil.which(name)
    if resolved:
        return resolved
    raise FileNotFoundError(f'LLVM tool not found: {name}')


def _disassemble_symbol(kernel_path: Path, symbol: str) -> str:
    llvm_nm = _find_llvm_tool('llvm-nm')
    llvm_objdump = _find_llvm_tool('llvm-objdump')
    nm_out = subprocess.check_output([llvm_nm, '-n', str(kernel_path)], text=True)
    addrs: list[tuple[int, str]] = []
    for line in nm_out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            try:
                addrs.append((int(parts[0], 16), parts[2]))
            except ValueError:
                continue
    start = next((addr for addr, name in addrs if name == symbol), None)
    if start is None:
        raise AssertionError(f'Symbol {symbol} not found in {kernel_path}')
    stop = next((addr for addr, _ in addrs if addr > start), None)
    if stop is None:
        raise AssertionError(f'No following symbol after {symbol} in {kernel_path}')
    return subprocess.check_output(
        [llvm_objdump, '-d', f'--start-address={start:#x}', f'--stop-address={stop:#x}', str(kernel_path)],
        text=True,
    )


class Sm6125KernelConfigTests(unittest.TestCase):
    def test_sm6125_config_requires_no_pan_and_dt_panic_region(self):
        parsed = parse_kconfig_fragment(CONFIG_PATH.read_text(encoding='utf-8'))
        self.assertEqual(parsed.get('CONFIG_XNU_NATIVE_PAN'), 'n')
        self.assertEqual(parsed.get('CONFIG_XNU_DT_PANIC_REGION'), 'y')

    def test_checker_rejects_bcm2837_fixed_panic_paddr(self):
        bad_disasm = '\n'.join([
            'fffffff00794468c <_PE_create_console>:',
            'fffffff0079446a0: 52980000      mov     w0, #0xc000',
            'fffffff0079446a4: 72a781e0      movk    w0, #0x3c0f, lsl #16',
            'fffffff0079446a8: 52880001      mov     w1, #0x4000',
            'fffffff0079446ac: 97eab2df      bl      0xfffffff0073f1228 <_ml_io_map_wcomb>',
        ])
        errors = check_pe_create_console_disasm(bad_disasm)
        self.assertTrue(any('0x3c0fc000' in err for err in errors), errors)

    def test_checker_accepts_dt_panic_region_disasm(self):
        good_disasm = '\n'.join([
            'fffffff00794468c <_PE_create_console>:',
            'fffffff0079446a4: 94000010      bl      0xfffffff0079415d8 <_SecureDTLookupEntry>',
            'fffffff007944740: 17e9e159      b       0xfffffff0073bcca4 <_initialize_screen>',
        ])
        self.assertEqual(check_pe_create_console_disasm(good_disasm), [])

    def test_built_kernel_pe_create_console(self):
        kernel = os.environ.get('SM6125_KERNEL_MACHO', '').strip()
        if not kernel:
            self.skipTest('SM6125_KERNEL_MACHO not set')
        kernel_path = Path(kernel)
        self.assertTrue(kernel_path.is_file(), f'Kernel not found: {kernel_path}')
        disasm = _disassemble_symbol(kernel_path, '_PE_create_console')
        self.assertEqual(check_pe_create_console_disasm(disasm, require_dt_probe=True), [])


if __name__ == '__main__':
    unittest.main()
