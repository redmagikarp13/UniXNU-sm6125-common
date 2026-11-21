#!/usr/bin/env python3
"""TDD contract tests for SM6125 ADT CPU topology compatibility with kernel limits."""
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
EXPECTED_TIMEBASE_HZ = 19200000


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


def validate_adt_cpu_topology(
    nodes: dict[str, dict[str, bytes]],
    *,
    max_cpus: int = 8,
    max_cpu_clusters: int = 1,
) -> list[str]:
    """Validate /cpus/cpu* ADT nodes against XNU ml_parse_cpu_topology() limits."""
    errors: list[str] = []
    cpus = sorted(
        (path, props)
        for path, props in nodes.items()
        if path.startswith('/cpus/cpu') and path != '/cpus'
    )
    if not cpus:
        return ['No /cpus/cpu* nodes found in ADT']
    if len(cpus) > max_cpus:
        errors.append(f'CPU count {len(cpus)} exceeds MAX_CPUS={max_cpus}')

    running_cpus = 0
    max_cluster_phy_id = max_cpu_clusters - 1
    for path, props in cpus:
        if props.get('state') == b'running\0':
            running_cpus += 1
        reg = props.get('reg')
        if reg is None or len(reg) != 4:
            errors.append(f'{path}: missing or invalid 4-byte reg property')
        tb = props.get('timebase-frequency')
        if tb is None or len(tb) != 4 or struct.unpack('<I', tb)[0] != EXPECTED_TIMEBASE_HZ:
            errors.append(f'{path}: timebase-frequency must be 4-byte LE {EXPECTED_TIMEBASE_HZ}')
        if 'cluster-type' in props and max_cpu_clusters == 1:
            ct_raw = props['cluster-type']
            ct_val = struct.unpack('<I', ct_raw)[0] if len(ct_raw) == 4 else None
            if ct_val == ord('P'):
                errors.append(
                    f"{path}: cluster-type='P' incompatível com MAX_CPU_CLUSTERS=1 "
                    f'(phys_cluster_id=1 > MAX_CPU_CLUSTER_PHY_ID={max_cluster_phy_id})'
                )
            else:
                errors.append(
                    f'{path}: cluster-type present while MAX_CPU_CLUSTERS={max_cpu_clusters}'
                )

    if running_cpus != 1:
        errors.append(f'Expected exactly 1 running boot CPU, found {running_cpus}')
    return errors


def _make_cpu_props(reg: int, *, state: bytes = b'waiting\0', cluster_type: str | None = None) -> dict[str, bytes]:
    props = {
        'reg': struct.pack('<I', reg),
        'state': state,
        'timebase-frequency': struct.pack('<I', EXPECTED_TIMEBASE_HZ),
    }
    if cluster_type is not None:
        props['cluster-type'] = struct.pack('<I', ord(cluster_type))
    return props


class Sm6125AdtTopologyTests(unittest.TestCase):
    def test_rejects_p_cluster_when_max_cpu_clusters_is_one(self):
        nodes = {
            '/cpus': {},
            '/cpus/cpu0': _make_cpu_props(0x000, state=b'running\0', cluster_type='E'),
            '/cpus/cpu4': _make_cpu_props(0x100, state=b'waiting\0', cluster_type='P'),
        }
        errors = validate_adt_cpu_topology(nodes, max_cpus=8, max_cpu_clusters=1)
        self.assertTrue(
            any('cluster-type' in err and 'MAX_CPU_CLUSTERS=1' in err for err in errors),
            errors,
        )

    def test_accepts_eight_smp_cpus_without_cluster_type(self):
        nodes: dict[str, dict[str, bytes]] = {'/cpus': {}}
        regs = [0x0, 0x1, 0x2, 0x3, 0x100, 0x101, 0x102, 0x103]
        for idx, reg in enumerate(regs):
            nodes[f'/cpus/cpu{idx}'] = _make_cpu_props(
                reg, state=b'running\0' if idx == 0 else b'waiting\0'
            )
        self.assertEqual(validate_adt_cpu_topology(nodes, max_cpus=8, max_cpu_clusters=1), [])

    def test_rejects_missing_boot_cpu_or_excess_cpus(self):
        no_running = {
            '/cpus': {},
            '/cpus/cpu0': _make_cpu_props(0x0, state=b'waiting\0'),
        }
        err_no_boot = validate_adt_cpu_topology(no_running, max_cpus=8, max_cpu_clusters=1)
        self.assertTrue(any('boot CPU' in err for err in err_no_boot), err_no_boot)

        too_many: dict[str, dict[str, bytes]] = {'/cpus': {}}
        for idx in range(9):
            too_many[f'/cpus/cpu{idx}'] = _make_cpu_props(
                idx, state=b'running\0' if idx == 0 else b'waiting\0'
            )
        err_excess = validate_adt_cpu_topology(too_many, max_cpus=8, max_cpu_clusters=1)
        self.assertTrue(any('MAX_CPUS=8' in err for err in err_excess), err_excess)

    def test_compiled_ginkgo_adt_topology_matches_kernel_limits(self):
        with tempfile.TemporaryDirectory(prefix='sm6125-adt-topo-') as tmp_dir:
            tmp = Path(tmp_dir)
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
            blob = adt.read_bytes()
            root, end = parse_adt_node(blob)
            self.assertEqual(end, len(blob))
            nodes = walk_adt(root)
            self.assertEqual(
                validate_adt_cpu_topology(nodes, max_cpus=8, max_cpu_clusters=1),
                [],
            )


if __name__ == '__main__':
    unittest.main()
