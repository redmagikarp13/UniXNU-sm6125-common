#!/usr/bin/env python3
"""Build libShengPlatformPower.a: guarded ApplePMGRNub provider for SM6125 AMFI contract."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/sheng-platform-power'
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))

from kernel_target import TRIPLE, check_object  # noqa: E402
from native_prelink import backed_offset, exports, macho  # noqa: E402

DONOR_KERNELCACHE_SHA256 = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
DONOR_SLIDE = 0x1000000

BINDINGS = {
    '__ZN9ApplePMGRC2EPK11OSMetaClass': 0xFFFFFFF0049F003C,
    '__ZN9ApplePMGRD2Ev': 0xFFFFFFF0049F019C,
    '__ZN9ApplePMGR10gMetaClassE': 0xFFFFFFF006ADF490,
    '__ZN9ApplePMGR7readAonEv': 0xFFFFFFF004A20FBC,
    '__ZN9ApplePMGR8writeAonEy': 0xFFFFFFF004A20FC4,
    '_sheng_original_pmgr_nub_create': 0xFFFFFFF004A22568,
    '_sheng_original_pmgr_nub_read': 0xFFFFFFF004A24ADC,
    '_sheng_original_pmgr_nub_write': 0xFFFFFFF004A230EC,
}


def require(cond: bool, message: str) -> None:
    if not cond:
        raise SystemExit(message)


def run(command: list, **kwargs) -> str:
    return subprocess.check_output([str(v) for v in command], timeout=120, **kwargs)


def find_llvm_objcopy() -> str:
    candidate = Path('/opt/homebrew/opt/llvm@22/bin/llvm-objcopy')
    if candidate.is_file() and os.access(candidate, os.X_OK):
        return str(candidate)
    resolved = shutil.which('llvm-objcopy')
    require(bool(resolved), 'llvm-objcopy not found (install Homebrew llvm@22)')
    return resolved


def find_donor_kernelcache() -> Path:
    candidates = [
        ROOT.parents[3] / 'ipsw/21H461/kernelcache.decompressed',
        ROOT / '.cache/firmware/kernelcache.macho/kernelcache.release.ipad7.decompressed',
    ]
    for path in candidates:
        if path.is_file():
            return path
    raise SystemExit('Donor 21H461 kernelcache.decompressed not found')


def verify_donor_contract(kc_path: Path) -> None:
    kc = kc_path.read_bytes()
    require(
        hashlib.sha256(kc).hexdigest() == DONOR_KERNELCACHE_SHA256,
        f'Donor kernelcache SHA-256 mismatch: {kc_path}',
    )
    meta = macho(kc)

    def read_slid(slid_va: int, size: int) -> bytes:
        at = backed_offset(meta['segments'], slid_va + DONOR_SLIDE, size, len(kc))
        return kc[at:at + size]

    def expect_words(slid_va: int, expected: tuple[int, ...], what: str) -> None:
        got = struct.unpack(f'<{len(expected)}I', read_slid(slid_va, 4 * len(expected)))
        require(got == expected, f'Donor {what} contract mismatch at {slid_va:#x}: {got}')

    expect_words(0xFFFFFFF004AAEB40, (0x94006C46, 0x321E0015, 0xAA1403E0, 0xAA1503E1, 0x94006C45), 'AMFI')
    expect_words(
        0xFFFFFFF004A24ADC,
        (0xF9404400, 0xB4000080, 0xF9400008, 0xF9450101, 0xD61F0020, 0x12800000, 0xD65F03C0),
        'ApplePMGRNub read',
    )
    expect_words(
        0xFFFFFFF004A230EC,
        (0xF9404400, 0xB4000080, 0xF9400008, 0xF9450502, 0xD61F0040, 0xD65F03C0),
        'ApplePMGRNub write',
    )
    expect_words(0xFFFFFFF004A20FBC, (0x12800000, 0xD65F03C0, 0xD65F03C0), 'ApplePMGR AON')
    require(read_slid(0xFFFFFFF0042DDE02, 13) == b'ApplePMGRNub\0', 'Donor ApplePMGRNub string mismatch')


def generate_headers() -> None:
    header = [
        '// Generated exact storage/slot ABI for ApplePMGR (ipados-17.7.11-21H461); private fields stay opaque.',
        '#pragma once',
        '#include <IOKit/IOPMGR.h>',
        'class ApplePMGR : public IOPMGR {',
        '  OSDeclareAbstractStructors(ApplePMGR)',
        '  unsigned char originalPrivate[0x4b8a8 - sizeof(IOPMGR)];',
        'public:',
    ]
    guards = []
    for slot in range(177, 340):
        if slot == 320:
            header.append('  virtual uint32_t readAon();')
        elif slot == 321:
            header.append('  virtual void writeAon(uint64_t);')
        else:
            header.append(f'  virtual void opaque{slot}() = 0;')
            guards.append(f'  void opaque{slot}() override {{ unavailable({slot}); }}')
    header.append('};')
    header.append('static_assert(sizeof(ApplePMGR) == 0x4b8a8, "Original provider storage");')
    (OUT / 'OriginalPMGRABI.h').write_text('\n'.join(header) + '\n', encoding='utf-8')
    (OUT / 'GuardedPMGRSlots.inc').write_text('\n'.join(guards) + '\n', encoding='utf-8')


def parse_symbols(raw: bytes, info: dict) -> list[tuple[str, int, int]]:
    sym_at = next(at for cmd, at, _ in info['commands'] if cmd == 2)
    at, count, strings, _ = struct.unpack_from('<4I', raw, sym_at + 8)
    result = []
    for i in range(count):
        ix, kind, _, _, address = struct.unpack_from('<IBBHQ', raw, at + i * 16)
        name = raw[strings + ix:raw.index(b'\0', strings + ix)].decode()
        result.append((name, address, kind))
    return result


def text_section(blob: bytes) -> bytes:
    section = next(
        s for seg in macho(blob)['segments'] for s in seg['sections'] if s['name'] == '__text'
    )
    return blob[section['fileoff']:section['fileoff'] + section['size']]


def main() -> None:
    kc_path = find_donor_kernelcache()
    verify_donor_contract(kc_path)
    generate_headers()

    script_dir = Path(__file__).resolve().parent
    power_dir = script_dir / 'platform-power'
    source = power_dir / 'Sm6125PlatformPower.cpp'
    obj = OUT / 'Sm6125PlatformPower.o'
    recipe_path = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    require(recipe_path.is_file(), f'Missing kernel compile record: {recipe_path}')
    record = json.loads(recipe_path.read_text().strip().rstrip(','))

    command, idx = [], 0
    arguments = record['arguments']
    while idx < len(arguments):
        arg = arguments[idx]
        if arg == record['file']:
            command.append(source)
        elif arg == '-o':
            command += ['-o', obj]
            idx += 1
        elif arg == '-Xclang' and arguments[idx + 1].startswith('-W'):
            idx += 1
        elif not arg.startswith('-W'):
            command.append(arg)
        idx += 1
    command += [
        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator',
        '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
        '-Wno-unnecessary-virtual-specifier',
        '-I', OUT, '-I', power_dir, '-I', ROOT / '.cache/xnu-work/osfmk',
    ]
    run(command, cwd=record['directory'])

    raw = obj.read_bytes()
    check_object(raw, 'Sm6125PlatformPower')
    info = macho(raw)
    symbols = parse_symbols(raw, info)
    vtable = next(va for name, va, _ in symbols if name == '__ZTV19Sm6125PlatformPower')
    relocations = {}
    for segment in info['segments']:
        for section in segment['sections']:
            if section['va'] <= vtable < section['va'] + section['size']:
                rel_at, rel_count = struct.unpack_from('<2I', raw, section['command_offset'] + 56)
                for i in range(rel_count):
                    offset, bits = struct.unpack_from('<2I', raw, rel_at + i * 8)
                    if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                        relocations[section['va'] + offset] = symbols[bits & 0xFFFFFF][0]

    entries = [relocations.get(vtable + 16 + i * 8) for i in range(340)]
    require(all(entries) and (vtable + 16 + 340 * 8) not in relocations, 'Expected exact 340-entry vtable')

    unstripped_path = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped'
    native_names = None
    iopmgr_vtable = None
    unstripped_bytes = b''
    unstripped_meta = None
    if unstripped_path.is_file():
        unstripped_bytes = unstripped_path.read_bytes()
        unstripped_meta = macho(unstripped_bytes)
        native_names = {n: va for va, ns in exports(unstripped_bytes, unstripped_meta).items() for n in ns}
        iopmgr_vtable = native_names.get('__ZTV6IOPMGR')

    for slot, name in enumerate(entries):
        if slot in (0, 1, 7) or (168 <= slot < 340 and slot not in (320, 321)):
            require('19Sm6125PlatformPower' in name, f'Own guard/lifecycle required at slot {slot}: {name}')
        elif slot in (320, 321):
            require('9ApplePMGR' in name, f'Expected inherited ApplePMGR AON method at slot {slot}: {name}')
        elif native_names is not None and iopmgr_vtable is not None:
            at = backed_offset(unstripped_meta['segments'], iopmgr_vtable + 16 + slot * 8, 8, len(unstripped_bytes))
            expected_va = struct.unpack_from('<Q', unstripped_bytes, at)[0]
            require(
                native_names.get(name) == expected_va,
                f'Native IOPMGR/IOService entry differs at slot {slot}: {name}',
            )

    undefined_pmgr = {n for n, _, kind in symbols if (kind & 0x0E) == 0 and 'ApplePMGR' in n}
    expected_pmgr = {
        '__ZN9ApplePMGRC2EPK11OSMetaClass',
        '__ZN9ApplePMGRD2Ev',
        '__ZN9ApplePMGR10gMetaClassE',
        '__ZN9ApplePMGR7readAonEv',
        '__ZN9ApplePMGR8writeAonEy',
    }
    require(undefined_pmgr == expected_pmgr, f'Unexpected ApplePMGR imports: {undefined_pmgr ^ expected_pmgr}')

    asm = OUT / 'original-bindings.S'
    asm.write_text(
        ''.join(f'.globl {name}\n.set {name}, {va:#x}\n' for name, va in sorted(BINDINGS.items())),
        encoding='utf-8',
    )
    run(['xcrun', 'clang', '-target', TRIPLE, '-Werror', '-Wno-incompatible-sysroot', '-c', asm, '-o', OUT / 'original-bindings.o'])

    late = OUT / 'Sm6125PlatformPower.late.o'
    objcopy = find_llvm_objcopy()
    run([
        objcopy,
        '--remove-section=__DATA,__mod_init_func',
        '--globalize-symbol=__GLOBAL__sub_I_Sm6125PlatformPower.cpp',
        obj,
        late,
    ])
    late_bytes = late.read_bytes()
    require(text_section(raw) == text_section(late_bytes), 'Late registration cannot change instructions')
    late_syms = parse_symbols(late_bytes, macho(late_bytes))
    init_sym = next((kind for name, _, kind in late_syms if name == '__GLOBAL__sub_I_Sm6125PlatformPower.cpp'), None)
    require(init_sym is not None and (init_sym & 0x01) == 1, 'Static init symbol must be globalized')

    archive = OUT / 'libShengPlatformPower.a'
    archive.unlink(missing_ok=True)
    run(['xcrun', 'ar', 'rcs', archive, late, OUT / 'original-bindings.o'])

    report = {
        'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
        'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
        'bindings': {name: hex(va) for name, va in sorted(BINDINGS.items())},
        'virtual_entries': 340,
        'mode': 'guarded ApplePMGRNub AMFI bridge for SM6125',
    }
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'PASS: Sm6125PlatformPower ({TRIPLE}, 340 vtable slots, {archive})')


if __name__ == '__main__':
    main()
