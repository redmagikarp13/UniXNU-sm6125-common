#!/usr/bin/env python3
"""TDD contract tests for SM6125 eMMC/GCC nodes in hardware DTS and compiled ADT."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

SOC_DIR = Path(__file__).resolve().parents[1]
WORKSPACE = SOC_DIR.parent
BOARD_DTS = WORKSPACE / 'UniXNU-ginkgo/dts/ginkgo.dts'
BOARD_TOML = WORKSPACE / 'UniXNU-ginkgo/board.toml'
UNIXNU_DIR = WORKSPACE / 'UniXNU'
OS_PROFILE = UNIXNU_DIR / 'profiles/os/ipados-17.7.11-21H461/profile.toml'


def parse_adt_node(blob: bytes, off: int = 0) -> tuple[dict, int]:
    """Decode one recursive node from an Apple DeviceTree (ADT) binary blob."""
    nprops, nchildren = struct.unpack_from('<II', blob, off)
    off += 8
    props: dict[str, bytes] = {}
    for _ in range(nprops):
        raw_name, length = struct.unpack_from('<32sI', blob, off)
        off += 36
        name = raw_name.split(b'\0', 1)[0].decode()
        props[name] = blob[off:off + length]
        off += (length + 3) & ~3
    children = []
    for _ in range(nchildren):
        child, off = parse_adt_node(blob, off)
        children.append(child)
    return {'props': props, 'children': children}, off


def walk_adt(node: dict, path: str = '') -> dict[str, dict[str, bytes]]:
    """Flatten an ADT tree into a mapping of node path -> property bytes."""
    name = node['props']['name'].rstrip(b'\0').decode()
    current = f'{path}/{name}' if path or name != 'device-tree' else ''
    result = {current or '/': node['props']}
    for child in node['children']:
        result.update(walk_adt(child, current))
    return result


def decode_reg_pairs(raw: bytes) -> list[tuple[int, int]]:
    """Decode ADT reg byte property as (offset, size) little-endian u64 pairs."""
    if len(raw) == 0 or len(raw) % 16 != 0:
        raise AssertionError(f'Invalid ADT reg length: {len(raw)}')
    return [struct.unpack_from('<QQ', raw, i) for i in range(0, len(raw), 16)]


def translate_arm_io(ranges_raw: bytes, pairs: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """Translate /arm-io child reg pairs into (phys_addr, size) using /arm-io/ranges."""
    if len(ranges_raw) != 24:
        raise AssertionError(f'Expected 24-byte /arm-io/ranges, got {len(ranges_raw)}')
    bus_child, bus_phys, bus_size = struct.unpack('<QQQ', ranges_raw)
    result = []
    for off, size in pairs:
        if off < bus_child or off - bus_child > bus_size or size > bus_size - (off - bus_child):
            raise AssertionError(f'Child range ({off:#x}, {size:#x}) out of /arm-io window ({bus_child:#x}, {bus_size:#x})')
        result.append((bus_phys + (off - bus_child), size))
    return result


class Sm6125AdtEmmcTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory(prefix='sm6125-adt-emmc-')
        tmp = Path(cls._tmp.name)
        dtb = tmp / 'hardware.dtb'
        adt = tmp / 'kernel.macho.adt'
        subprocess.run(
            ['dtc', '-I', 'dts', '-O', 'dtb', '-i', str(SOC_DIR / 'dts'), '-o', str(dtb), str(BOARD_DTS)],
            check=True,
        )
        env = dict(os.environ, PYTHONPATH=str(UNIXNU_DIR / 'scripts'))
        subprocess.run(
            [
                sys.executable,
                str(UNIXNU_DIR / 'scripts/make-adt.py'),
                '--dtb', str(dtb),
                '--os-profile', str(OS_PROFILE),
                '--board', str(BOARD_TOML),
                '--unique-chip-id', '0123456789abcdef',
                '--output', str(adt),
            ],
            env=env,
            check=True,
        )
        root, end = parse_adt_node(adt.read_bytes())
        assert end == adt.stat().st_size
        cls.nodes = walk_adt(root)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def test_arm_io_ranges_cover_gcc_sdhci_and_gic(self):
        arm_io = self.nodes.get('/arm-io')
        self.assertIsNotNone(arm_io)
        bus_child, bus_phys, bus_size = struct.unpack('<QQQ', arm_io['ranges'])
        self.assertEqual(bus_child, 0)
        self.assertEqual(bus_phys, 0x01000000)
        self.assertEqual(bus_size, 0x0F000000)

        intc = self.nodes.get('/arm-io/intc')
        self.assertIsNotNone(intc)
        intc_phys = translate_arm_io(arm_io['ranges'], decode_reg_pairs(intc['reg']))
        self.assertEqual(intc_phys, [(0x0F200000, 0x10000), (0x0F300000, 0x100000)])

    def test_arm_io_gcc_node_covers_sdcc1_block(self):
        arm_io = self.nodes['/arm-io']
        gcc = self.nodes.get('/arm-io/gcc')
        self.assertIsNotNone(gcc, f'/arm-io/gcc missing from ADT: {sorted(self.nodes)}')
        compat = gcc.get('compatible', b'').rstrip(b'\0').split(b'\0')
        self.assertIn(b'qcom,gcc-sm6125', compat)
        gcc_phys = translate_arm_io(arm_io['ranges'], decode_reg_pairs(gcc['reg']))
        self.assertEqual(gcc_phys, [(0x01400000, 0x001F0000)])
        base, size = gcc_phys[0]
        self.assertLessEqual(base + 0x38000 + 0x1000, base + size)

    def test_arm_io_sdhci1_node_registers_and_interrupts(self):
        arm_io = self.nodes['/arm-io']
        intc = self.nodes['/arm-io/intc']
        sdhci = self.nodes.get('/arm-io/sdhci1')
        self.assertIsNotNone(sdhci, f'/arm-io/sdhci1 missing from ADT: {sorted(self.nodes)}')
        self.assertEqual(sdhci.get('compatible'), b'qcom,sdhci-msm-v5\0')
        sdhci_phys = translate_arm_io(arm_io['ranges'], decode_reg_pairs(sdhci['reg']))
        self.assertEqual(sdhci_phys, [(0x04744000, 0x1000), (0x04745000, 0x1000)])
        intc_phandle = struct.unpack('<I', intc['AAPL,phandle' if 'AAPL,phandle' in intc else 'phandle'])[0]
        self.assertEqual(struct.unpack('<I', sdhci['interrupt-parent'])[0], intc_phandle)
        intr = sdhci.get('interrupts', b'')
        self.assertEqual(len(intr), 24)
        self.assertEqual(struct.unpack('<6I', intr), (0, 0x15C, 0, 0, 0x160, 0))


if __name__ == '__main__':
    unittest.main()
