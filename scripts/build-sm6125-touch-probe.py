#!/usr/bin/env python3
"""Build libSm6125TouchProbe.a (QcomGeniSpi + Sm6125NvtProbe + Sm6125TouchProbe) with ARMv8.0-A audits."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/sm6125-touch-probe'
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))

from kernel_target import check_object  # noqa: E402
from native_prelink import macho  # noqa: E402


def require(cond: bool, message: str) -> None:
    if not cond:
        raise SystemExit(message)


def find_llvm_tool(name: str) -> str:
    candidate = Path('/opt/homebrew/opt/llvm@22/bin') / name
    if candidate.is_file() and os.access(candidate, os.X_OK):
        return str(candidate)
    resolved = shutil.which(name)
    require(bool(resolved), f'{name} not found (install Homebrew llvm@22)')
    return resolved


def find_geni_spi_dir() -> Path:
    candidate = (ROOT / 'drivers/QcomGeniSpi').resolve()
    if (candidate / 'GeniSpi.c').is_file():
        return candidate
    for parent in ROOT.parents:
        alt = (parent / 'drivers/QcomGeniSpi').resolve()
        if (alt / 'GeniSpi.c').is_file():
            return alt
    raise SystemExit('Missing QcomGeniSpi/GeniSpi.c in tree or parent UniXNU checkout')


def replay_compile(record_path: Path, source: Path, obj: Path, extra_flags: list[str]) -> None:
    require(record_path.is_file(), f'Missing kernel compile record: {record_path}')
    record = json.loads(record_path.read_text().strip().rstrip(','))
    command, idx = [], 0
    arguments = record['arguments']
    while idx < len(arguments):
        arg = arguments[idx]
        if arg == record['file']:
            command.append(str(source))
        elif arg == '-o':
            command += ['-o', str(obj)]
            idx += 1
        elif arg == '-Xclang' and idx + 1 < len(arguments) and arguments[idx + 1].startswith('-W'):
            idx += 1
        elif not arg.startswith('-W'):
            command.append(arg)
        idx += 1
    command += extra_flags
    subprocess.check_call(command, cwd=record['directory'], timeout=120)


def main() -> None:
    script_dir = Path(__file__).resolve().parent
    probe_dir = script_dir / 'touch-probe'
    geni_dir = find_geni_spi_dir()
    geni_src = geni_dir / 'GeniSpi.c'
    nvt_src = probe_dir / 'Sm6125NvtProbe.c'
    cpp_src = probe_dir / 'Sm6125TouchProbe.cpp'
    for src in (geni_src, nvt_src, cpp_src):
        require(src.is_file(), f'Missing source file: {src}')

    obj_root = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    c_record = obj_root / 'bsd/DEVELOPMENT/bsd_init.o.json'
    cpp_record = obj_root / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'

    common_warnings = [
        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator',
        '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
        '-Wno-unnecessary-virtual-specifier',
    ]
    inc_flags = ['-I', str(probe_dir), '-I', str(geni_dir)]

    geni_obj = OUT / 'GeniSpi.o'
    nvt_obj = OUT / 'Sm6125NvtProbe.o'
    cpp_obj = OUT / 'Sm6125TouchProbe.o'

    replay_compile(c_record, geni_src, geni_obj, [*common_warnings, '-I', str(geni_dir)])
    replay_compile(c_record, nvt_src, nvt_obj, [*common_warnings, '-I', str(probe_dir)])
    replay_compile(cpp_record, cpp_src, cpp_obj, [*common_warnings, *inc_flags])

    objs = [geni_obj, nvt_obj, cpp_obj]
    for obj in objs:
        raw = obj.read_bytes()
        check_object(raw, obj.stem)
        info = macho(raw)
        sec_names = [s['name'] for seg in info['segments'] for s in seg['sections']]
        require('__mod_init_func' not in sec_names, f'Unexpected __mod_init_func in {obj.name}')

    archive = OUT / 'libSm6125TouchProbe.a'
    archive.unlink(missing_ok=True)
    subprocess.check_call(['xcrun', 'ar', 'rcs', str(archive), *(str(o) for o in objs)], timeout=60)

    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    stamp = obj_root / '.sm6125-touch-probe.sha256'
    if not stamp.is_file() or stamp.read_text(encoding='utf-8').strip() != digest:
        (obj_root / 'mach.development.bcm2837.unstripped').unlink(missing_ok=True)
        stamp.parent.mkdir(parents=True, exist_ok=True)
        stamp.write_text(digest + '\n', encoding='utf-8')

    report = {
        'geni_spi_sha256': hashlib.sha256(geni_src.read_bytes()).hexdigest(),
        'nvt_probe_sha256': hashlib.sha256(nvt_src.read_bytes()).hexdigest(),
        'touch_probe_sha256': hashlib.sha256(cpp_src.read_bytes()).hexdigest(),
        'archive_sha256': digest,
        'mode': 'SM6125 QUPv3 SE2 SPI + NT36672A Chip ID probe (sm6125-touch=1..3)',
    }
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'PASS: Sm6125TouchProbe ({archive} sha256 {digest})')


if __name__ == '__main__':
    main()
