#!/usr/bin/env python3
"""TDD contract tests for SM6125 kernel config and _PE_create_console disassembly."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tomllib
import unittest

SOC_DIR = Path(__file__).resolve().parents[1]
WORKSPACE = SOC_DIR.parent
CONFIG_PATH = SOC_DIR / 'configs/sm6125.config'
SOC_TOML_PATH = SOC_DIR / 'soc.toml'
BOARD_TOML_PATH = WORKSPACE / 'UniXNU-ginkgo/board.toml'
PATCH_SERIES_PATH = SOC_DIR / 'patches/xnu/series'
PHANDLE_ALIAS_PATCH = SOC_DIR / 'patches/xnu/iokit-fdt-phandle-alias.patch'

MOV_LOW_RE = re.compile(r'\bmovz?\s+[wx](\d+),\s*#0xc000\b', re.IGNORECASE)
MOVK_HIGH_RE = re.compile(r'\bmovk\s+[wx](\d+),\s*#0x3c0f,\s*lsl\s+#16\b', re.IGNORECASE)
INSN_PREFIX_RE = re.compile(r'^[0-9a-f]+:\s+[0-9a-f]+\s+', re.IGNORECASE)


def find_adrp_load_targets(disasm: str) -> set[int]:
    """Extract effective VAs referenced via ADRP + ADD/LDR/LDP in AArch64 disassembly."""
    targets: set[int] = set()
    pages: dict[str, int] = {}
    for raw_line in disasm.splitlines():
        line = INSN_PREFIX_RE.sub('', raw_line.split(';', 1)[0].strip())
        line = ' '.join(line.split())
        if not line:
            continue
        m_adrp = re.match(r'^adrp\s+(x\d+),\s*(?:0x)?([0-9a-f]+)\b', line, re.I)
        if m_adrp:
            pages[m_adrp.group(1).lower()] = int(m_adrp.group(2), 16)
            continue
        m_add = re.match(
            r'^add\s+(x\d+),\s*(x\d+),\s*#((?:0x)?[0-9a-f]+)\b', line, re.I
        )
        if m_add:
            dst, src = m_add.group(1).lower(), m_add.group(2).lower()
            imm = int(m_add.group(3), 0)
            if src in pages:
                targets.add(pages[src] + imm)
            pages.pop(dst, None)
            continue
        m_ldr = re.match(
            r'^ldr\w*\s+[wx](\d+),\s*\[(x\d+)(?:,\s*#((?:0x)?[0-9a-f]+))?\]',
            line,
            re.I,
        )
        if m_ldr:
            dst_num, src = m_ldr.group(1), m_ldr.group(2).lower()
            imm = int(m_ldr.group(3), 0) if m_ldr.group(3) else 0
            if src in pages:
                targets.add(pages[src] + imm)
            pages.pop(f'x{dst_num}', None)
            continue
        m_ldp = re.match(
            r'^ldp\s+[wx](\d+),\s*[wx](\d+),\s*\[(x\d+)(?:,\s*#((?:0x)?[0-9a-f]+))?\]',
            line,
            re.I,
        )
        if m_ldp:
            d1, d2, src = m_ldp.group(1), m_ldp.group(2), m_ldp.group(3).lower()
            imm = int(m_ldp.group(4), 0) if m_ldp.group(4) else 0
            if src in pages:
                targets.add(pages[src] + imm)
            pages.pop(f'x{d1}', None)
            pages.pop(f'x{d2}', None)
            continue
        if re.match(r'^blr?\b', line, re.I):
            for idx in range(19):
                pages.pop(f'x{idx}', None)
            continue
        if re.match(r'^(?:st\w*|cbn?z|tbn?z|cmp|cmn|tst|b(?:\.\w+)?)\b', line, re.I):
            continue
        m_write = re.match(r'^[a-z][a-z0-9.]*\s+[wx](\d+)\b', line, re.I)
        if m_write:
            pages.pop(f'x{m_write.group(1)}', None)
    return targets


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


def _parse_nm_symbols(kernel_path: Path) -> list[tuple[int, str]]:
    llvm_nm = _find_llvm_tool('llvm-nm')
    nm_out = subprocess.check_output([llvm_nm, '-n', str(kernel_path)], text=True)
    addrs: list[tuple[int, str]] = []
    for line in nm_out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            try:
                addrs.append((int(parts[0], 16), parts[2]))
            except ValueError:
                continue
    return addrs


def _lookup_symbol(addrs: list[tuple[int, str]], symbol: str, kernel_path: Path) -> int:
    start = next((addr for addr, name in addrs if name == symbol), None)
    if start is None:
        raise AssertionError(f'Symbol {symbol} not found in {kernel_path}')
    return start


def _disassemble_symbol(kernel_path: Path, symbol: str, addrs: list[tuple[int, str]] | None = None) -> str:
    llvm_objdump = _find_llvm_tool('llvm-objdump')
    sym_table = addrs if addrs is not None else _parse_nm_symbols(kernel_path)
    start = _lookup_symbol(sym_table, symbol, kernel_path)
    stop = next((addr for addr, _ in sym_table if addr > start), None)
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

    def test_find_adrp_load_targets_extracts_exact_va(self):
        snippet = '\n'.join([
            'fffffff00788bce4: d000135b \tadrp\tx27, 0xfffffff007af5000',
            'fffffff00788bce8: f9409f61 \tldr\tx1, [x27, #0x138]',
            'fffffff00788bcec: b0001348 \tadrp\tx8, 0xfffffff007af4000',
            'fffffff00788bcf0: 9122a108 \tadd\tx8, x8, #0x8a8',
            'fffffff00788bcf4: f9400108 \tldr\tx8, [x8]',
            'fffffff00788bcf8: d000135c \tadrp\tx28, 0xfffffff007af6000',
            'fffffff00788bcfc: d280001c \tmov\tx28, #0x0',
            'fffffff00788bd00: f9400380 \tldr\tx0, [x28, #0x10]',
        ])
        targets = find_adrp_load_targets(snippet)
        self.assertIn(0xFFFFFFF007AF5138, targets)
        self.assertIn(0xFFFFFFF007AF48A8, targets)
        self.assertNotIn(0xFFFFFFF007AF6010, targets)

    def test_series_includes_fdt_phandle_alias_patch(self):
        series_lines = [
            line.strip()
            for line in PATCH_SERIES_PATH.read_text(encoding='utf-8').splitlines()
            if line.strip() and not line.strip().startswith('#')
        ]
        self.assertIn('iokit-fdt-phandle-alias.patch', series_lines)
        self.assertTrue(PHANDLE_ALIAS_PATCH.is_file(), f'Missing patch: {PHANDLE_ALIAS_PATCH}')
        patch_text = PHANDLE_ALIAS_PATCH.read_text(encoding='utf-8')
        self.assertIn('iokit/Kernel/IODeviceTreeSupport.cpp', patch_text)
        self.assertIn('gIODTPHandleKey', patch_text)
        self.assertIn('"phandle"', patch_text)

    def test_built_kernel_pe_create_console(self):
        kernel = os.environ.get('SM6125_KERNEL_MACHO', '').strip()
        if not kernel:
            self.skipTest('SM6125_KERNEL_MACHO not set')
        kernel_path = Path(kernel)
        self.assertTrue(kernel_path.is_file(), f'Kernel not found: {kernel_path}')
        disasm = _disassemble_symbol(kernel_path, '_PE_create_console')
        self.assertEqual(check_pe_create_console_disasm(disasm, require_dt_probe=True), [])

    def test_sm6125_pmgr_config_and_wiring(self):
        parsed = parse_kconfig_fragment(CONFIG_PATH.read_text(encoding='utf-8'))
        self.assertEqual(parsed.get('CONFIG_XNU_SHENG_PMGR'), 'y')
        soc_toml = tomllib.loads(SOC_TOML_PATH.read_text(encoding='utf-8'))
        archives = (
            soc_toml.get('os_overrides', {})
            .get('ipados-17.7.11-21H461', {})
            .get('kernel', {})
            .get('archives', [])
        )
        self.assertIn('build-sm6125-platform-power.py', archives)
        board_toml = tomllib.loads(BOARD_TOML_PATH.read_text(encoding='utf-8'))
        kernel_args = board_toml.get('payload', {}).get('kernel_args', [])
        self.assertIn('pmgr=2', kernel_args)

    def test_built_kernel_make_reference_table_aliases_phandle(self):
        kernel = os.environ.get('SM6125_KERNEL_MACHO', '').strip()
        if not kernel:
            self.skipTest('SM6125_KERNEL_MACHO not set')
        kernel_path = Path(kernel)
        self.assertTrue(kernel_path.is_file(), f'Kernel not found: {kernel_path}')
        addrs = _parse_nm_symbols(kernel_path)
        phandle_key_va = _lookup_symbol(addrs, '_gIODTPHandleKey', kernel_path)
        disasm = _disassemble_symbol(kernel_path, '__ZL18MakeReferenceTablePK13OpaqueDTEntryb', addrs=addrs)
        self.assertIn(phandle_key_va, find_adrp_load_targets(disasm))

    def test_built_kernel_links_sm6125_platform_power(self):
        kernel = os.environ.get('SM6125_KERNEL_MACHO', '').strip()
        if not kernel:
            self.skipTest('SM6125_KERNEL_MACHO not set')
        kernel_path = Path(kernel)
        self.assertTrue(kernel_path.is_file(), f'Kernel not found: {kernel_path}')
        names = {name for _, name in _parse_nm_symbols(kernel_path)}
        self.assertIn('_sheng_pmgr_run', names)
        self.assertIn('__ZTV19Sm6125PlatformPower', names)

    def test_sm6125_emmc_ro_probe_wiring(self):
        parsed = parse_kconfig_fragment(CONFIG_PATH.read_text(encoding='utf-8'))
        extra_archives = parsed.get('CONFIG_XNU_EXTRA_LD_ARCHIVES', '').strip('"')
        self.assertIn('libSm6125EmmcProbe.a', extra_archives)
        soc_toml = tomllib.loads(SOC_TOML_PATH.read_text(encoding='utf-8'))
        archives = (
            soc_toml.get('os_overrides', {})
            .get('ipados-17.7.11-21H461', {})
            .get('kernel', {})
            .get('archives', [])
        )
        self.assertIn('build-sm6125-emmc-probe.py', archives)
        series_lines = [
            line.strip()
            for line in PATCH_SERIES_PATH.read_text(encoding='utf-8').splitlines()
            if line.strip() and not line.strip().startswith('#')
        ]
        self.assertIn('sm6125-emmc-probe-hook.patch', series_lines)

    def test_built_kernel_links_sm6125_emmc_ro_probe(self):
        kernel = os.environ.get('SM6125_KERNEL_MACHO', '').strip()
        if not kernel:
            self.skipTest('SM6125_KERNEL_MACHO not set')
        kernel_path = Path(kernel)
        self.assertTrue(kernel_path.is_file(), f'Kernel not found: {kernel_path}')
        addrs = _parse_nm_symbols(kernel_path)
        names = {name for _, name in addrs}
        self.assertIn('_sm6125_emmc_probe_run', names)
        self.assertIn('_sm6125_emmc_ident_gpt_run', names)
        bsd_init_disasm = _disassemble_symbol(kernel_path, '_bsd_init', addrs=addrs)
        self.assertRegex(bsd_init_disasm, r'\bbl\s+.*<_sm6125_emmc_probe_run>')

        load_re = re.compile(r'^(?:ld|cas|swp)', re.IGNORECASE)
        store_re = re.compile(r'^(?:st|cas|swp|ldadd|ldclr|ldeor|ldset)', re.IGNORECASE)
        writeback_re = re.compile(r'\]\s*,|\][!]', re.IGNORECASE)
        for sym, expected_load in (
            ('_sm6125_mmio_read8', 'ldrb'),
            ('_sm6125_mmio_read16', 'ldrh'),
            ('_sm6125_mmio_read32', 'ldr'),
        ):
            disasm = _disassemble_symbol(kernel_path, sym, addrs=addrs)
            loads = []
            for raw_line in disasm.splitlines():
                stripped = raw_line.strip()
                if (
                    not stripped
                    or stripped.endswith(':')
                    or stripped.startswith('/')
                    or stripped.startswith('Disassembly')
                ):
                    continue
                cleaned = INSN_PREFIX_RE.sub('', stripped.split(';', 1)[0].strip())
                cleaned = ' '.join(cleaned.split())
                if not cleaned:
                    continue
                mnemonic = cleaned.split()[0].lower()
                self.assertIsNone(store_re.match(mnemonic), f'Store instruction in {sym}: {cleaned}')
                if mnemonic.startswith('ld'):
                    self.assertIsNone(writeback_re.search(cleaned), f'Writeback load in {sym}: {cleaned}')
                    loads.append(mnemonic)
            self.assertEqual(loads, [expected_load], f'Unexpected loads in {sym}: {disasm}')

        for sym, expected_store in (
            ('_sm6125_mmio_write8', 'strb'),
            ('_sm6125_mmio_write16', 'strh'),
            ('_sm6125_mmio_write32', 'str'),
        ):
            disasm = _disassemble_symbol(kernel_path, sym, addrs=addrs)
            stores = []
            for raw_line in disasm.splitlines():
                stripped = raw_line.strip()
                if (
                    not stripped
                    or stripped.endswith(':')
                    or stripped.startswith('/')
                    or stripped.startswith('Disassembly')
                ):
                    continue
                cleaned = INSN_PREFIX_RE.sub('', stripped.split(';', 1)[0].strip())
                cleaned = ' '.join(cleaned.split())
                if not cleaned:
                    continue
                mnemonic = cleaned.split()[0].lower()
                self.assertIsNone(load_re.match(mnemonic), f'Load instruction in {sym}: {cleaned}')
                if mnemonic.startswith('st'):
                    self.assertIsNone(writeback_re.search(cleaned), f'Writeback store in {sym}: {cleaned}')
                    stores.append(mnemonic)
            self.assertEqual(stores, [expected_store], f'Unexpected stores in {sym}: {disasm}')


if __name__ == '__main__':
    unittest.main()
