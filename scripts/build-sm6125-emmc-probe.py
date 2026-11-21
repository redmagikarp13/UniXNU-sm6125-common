#!/usr/bin/env python3
"""Build libSm6125EmmcProbe.a with strict read-only MMIO disassembly and Mach-O audits."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/sm6125-emmc-probe'
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))

from kernel_target import check_object  # noqa: E402
from native_prelink import macho  # noqa: E402

READ_ACCESSOR_LOADS = {
    '_sm6125_mmio_read8': 'ldrb',
    '_sm6125_mmio_read16': 'ldrh',
    '_sm6125_mmio_read32': 'ldr',
}
STORE_MNEMONIC_RE = re.compile(r'^(?:st|cas|swp|ldadd|ldclr|ldeor|ldset)', re.IGNORECASE)
WRITEBACK_RE = re.compile(r'\]\s*,|\][!]', re.IGNORECASE)
INSN_PREFIX_RE = re.compile(r'^[0-9a-f]+:\s+[0-9a-f]+\s+', re.IGNORECASE)


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


def audit_object_sections_and_disasm(core_obj: Path, probe_obj: Path) -> None:
    text_end = 0
    for obj in (core_obj, probe_obj):
        raw = obj.read_bytes()
        check_object(raw, obj.stem)
        info = macho(raw)
        section_names = [s['name'] for seg in info['segments'] for s in seg['sections']]
        require('__mod_init_func' not in section_names, f'Unexpected __mod_init_func in {obj.name}')
        if obj == probe_obj:
            text_sec = next(s for seg in info['segments'] for s in seg['sections'] if s['name'] == '__text')
            text_end = text_sec['va'] + text_sec['size']

    llvm_nm = find_llvm_tool('llvm-nm')
    syms_out = subprocess.check_output([llvm_nm, '-n', '--defined-only', str(probe_obj)], text=True, timeout=60)
    text_syms: list[tuple[int, str]] = []
    defined_syms: list[str] = []
    for line in syms_out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            addr = int(parts[0], 16)
            kind, name = parts[1], parts[2]
            defined_syms.append(name)
            if kind in ('t', 'T') and addr < text_end:
                text_syms.append((addr, name))

    for sym in READ_ACCESSOR_LOADS:
        require(sym in defined_syms, f'Missing read accessor {sym} in {probe_obj.name}')
    mmio_writes = [s for s in defined_syms if 'mmio_write' in s.lower()]
    require(not mmio_writes, f'Unexpected MMIO write symbols in {probe_obj.name}: {mmio_writes}')

    llvm_objdump = find_llvm_tool('llvm-objdump')
    for sym, expected_load in READ_ACCESSOR_LOADS.items():
        start = next((addr for addr, name in text_syms if name == sym), None)
        require(start is not None, f'Text symbol missing for {sym}')
        stop = next((addr for addr, _ in text_syms if addr > start), text_end)
        disasm = subprocess.check_output(
            [llvm_objdump, '-d', f'--start-address={start:#x}', f'--stop-address={stop:#x}', str(probe_obj)],
            text=True,
            timeout=60,
        )
        insns: list[str] = []
        for raw_line in disasm.splitlines():
            stripped = raw_line.strip()
            if not stripped or stripped.endswith(':') or stripped.startswith('/') or stripped.startswith('Disassembly'):
                continue
            cleaned = INSN_PREFIX_RE.sub('', stripped.split(';', 1)[0].strip())
            cleaned = ' '.join(cleaned.split())
            if cleaned:
                insns.append(cleaned)
        require(bool(insns), f'No instructions decoded for {sym}')
        require(insns[-1].lower() == 'ret', f'{sym} must end with ret: {insns}')
        loads = []
        for insn in insns:
            mnemonic = insn.split()[0].lower()
            require(not STORE_MNEMONIC_RE.match(mnemonic), f'Store instruction forbidden in {sym}: {insn}')
            if mnemonic.startswith('ld'):
                require(not WRITEBACK_RE.search(insn), f'Writeback load forbidden in {sym}: {insn}')
                loads.append(mnemonic)
        require(loads == [expected_load], f'{sym} expected single [{expected_load}], got {loads}: {insns}')


def main() -> None:
    script_dir = Path(__file__).resolve().parent
    probe_dir = script_dir / 'emmc-probe'
    c_src = probe_dir / 'Sm6125EmmcReadOnly.c'
    cpp_src = probe_dir / 'Sm6125EmmcProbe.cpp'
    c_obj = OUT / 'Sm6125EmmcReadOnly.o'
    cpp_obj = OUT / 'Sm6125EmmcProbe.o'

    obj_root = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    c_record = obj_root / 'bsd/DEVELOPMENT/bsd_init.o.json'
    cpp_record = obj_root / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'

    common_warnings = [
        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator',
        '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
        '-Wno-unnecessary-virtual-specifier',
    ]
    replay_compile(c_record, c_src, c_obj, [*common_warnings, '-I', str(probe_dir)])
    replay_compile(cpp_record, cpp_src, cpp_obj, [*common_warnings, '-I', str(probe_dir)])

    audit_object_sections_and_disasm(c_obj, cpp_obj)

    archive = OUT / 'libSm6125EmmcProbe.a'
    archive.unlink(missing_ok=True)
    subprocess.check_call(['xcrun', 'ar', 'rcs', str(archive), str(c_obj), str(cpp_obj)], timeout=60)

    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    stamp = obj_root / '.sm6125-emmc-probe.sha256'
    if not stamp.is_file() or stamp.read_text(encoding='utf-8').strip() != digest:
        (obj_root / 'mach.development.bcm2837.unstripped').unlink(missing_ok=True)
        stamp.parent.mkdir(parents=True, exist_ok=True)
        stamp.write_text(digest + '\n', encoding='utf-8')

    report = {
        'c_source_sha256': hashlib.sha256(c_src.read_bytes()).hexdigest(),
        'cpp_source_sha256': hashlib.sha256(cpp_src.read_bytes()).hexdigest(),
        'archive_sha256': digest,
        'mode': 'strictly read-only GCC SDCC1 + SDHCI-MSM v5 snapshot probe for SM6125',
    }
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'PASS: Sm6125EmmcProbe ({archive} sha256 {digest})')


if __name__ == '__main__':
    main()
