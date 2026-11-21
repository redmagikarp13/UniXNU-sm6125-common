#!/usr/bin/env python3
"""Contract test: ginkgo DTS -> DTB -> ADT -> ArmCpuTimer.h parses /arm-io/timer PPI 19."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

SOC_DIR = Path(__file__).resolve().parents[1]
WORKSPACE = SOC_DIR.parent
BOARD_DTS = WORKSPACE / 'UniXNU-ginkgo/dts/ginkgo.dts'
BOARD_TOML = WORKSPACE / 'UniXNU-ginkgo/board.toml'
UNIXNU_DIR = WORKSPACE / 'UniXNU'
OS_PROFILE = UNIXNU_DIR / 'profiles/os/ipados-17.7.11-21H461/profile.toml'
TIMER_HEADER_DIR = SOC_DIR / 'drivers/ArmCpu'


def parse_adt_node(blob, off=0):
    nprops, nchildren = struct.unpack_from('<II', blob, off)
    off += 8
    props = {}
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


def walk_adt(node, path=''):
    name = node['props']['name'].rstrip(b'\0').decode()
    current = f'{path}/{name}' if path or name != 'device-tree' else ''
    result = {current or '/': node['props']}
    for child in node['children']:
        result.update(walk_adt(child, current))
    return result


def main():
    with tempfile.TemporaryDirectory(prefix='sm6125-adt-timer-') as tmp_dir:
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
                sys.executable, str(UNIXNU_DIR / 'scripts/make-adt.py'),
                '--dtb', str(dtb),
                '--os-profile', str(OS_PROFILE),
                '--board', str(BOARD_TOML),
                '--unique-chip-id', '0123456789abcdef',
                '--output', str(adt),
            ],
            env=env, check=True,
        )
        root, end = parse_adt_node(adt.read_bytes())
        assert end == adt.stat().st_size, f'ADT trailing bytes: {end} != {adt.stat().st_size}'
        nodes = walk_adt(root)
        timer = nodes.get('/arm-io/timer')
        assert timer is not None, f'/arm-io/timer missing from ADT nodes: {sorted(nodes)}'
        assert timer.get('compatible') == b'arm,armv8-timer\0', f"unexpected compatible: {timer.get('compatible')!r}"
        interrupts_raw = timer.get('interrupts', b'')
        assert interrupts_raw and len(interrupts_raw) % 4 == 0, f'invalid interrupts bytes: {len(interrupts_raw)}'

        byte_list = ', '.join(f'0x{b:02x}' for b in interrupts_raw)
        harness = tmp / 'harness.c'
        harness.write_text(
            '#include <stdint.h>\n'
            '#include <stdio.h>\n'
            '#include <string.h>\n'
            '#include "ArmCpuTimer.h"\n'
            'int main(void) {\n'
            f'    const uint8_t raw[] = {{ {byte_list} }};\n'
            '    uint32_t cells[16] = {0};\n'
            '    if (sizeof(raw) > sizeof(cells)) return 2;\n'
            '    memcpy(cells, raw, sizeof(raw));\n'
            '    const uint32_t ppi = armcpu_virtual_timer_ppi_from_dt(cells, sizeof(raw) / sizeof(uint32_t));\n'
            '    const uint32_t mask = armcpu_sgi_ppi_enable_mask(ppi);\n'
            '    printf("ppi=%u mask=0x%08x\\n", ppi, mask);\n'
            '    return 0;\n'
            '}\n'
        )
        exe = tmp / 'harness'
        subprocess.run(
            ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', f'-I{TIMER_HEADER_DIR}', str(harness), '-o', str(exe)],
            check=True,
        )
        out = subprocess.check_output([str(exe)], text=True).strip()
        assert out == 'ppi=19 mask=0x00080001', f'unexpected output: {out!r}'
        print(f'PASS: ADT /arm-io/timer -> {out}')


if __name__ == '__main__':
    main()
