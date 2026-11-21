#!/usr/bin/env python3
"""Build libSm6125EmmcProbe.a with read-only MMIO disasm, IOStorageFamily vtable, and Mach-O audits."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/sm6125-emmc-probe'
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))

from kernel_target import check_object  # noqa: E402
from native_prelink import macho  # noqa: E402

IOSTORAGEFAMILY_REVISION = 'e45fbe30a7879e0b16e26dddb78538f1b00e0bff'

READ_ACCESSOR_LOADS = {
    '_sm6125_mmio_read8': 'ldrb',
    '_sm6125_mmio_read16': 'ldrh',
    '_sm6125_mmio_read32': 'ldr',
}
WRITE_ACCESSOR_STORES = {
    '_sm6125_mmio_write8': 'strb',
    '_sm6125_mmio_write16': 'strh',
    '_sm6125_mmio_write32': 'str',
}
LOAD_MNEMONIC_RE = re.compile(r'^(?:ld|cas|swp)', re.IGNORECASE)
STORE_MNEMONIC_RE = re.compile(r'^(?:st|cas|swp|ldadd|ldclr|ldeor|ldset)', re.IGNORECASE)
WRITEBACK_RE = re.compile(r'\]\s*,|\][!]', re.IGNORECASE)
INSN_PREFIX_RE = re.compile(r'^[0-9a-f]+:\s+[0-9a-f]+\s+', re.IGNORECASE)


def require(cond: bool, message: str) -> None:
    if not cond:
        raise SystemExit(message)


def load_helper(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


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


def extract_symbol_insns(disasm: str) -> list[str]:
    insns: list[str] = []
    for raw_line in disasm.splitlines():
        stripped = raw_line.strip()
        if not stripped or stripped.endswith(':') or stripped.startswith('/') or stripped.startswith('Disassembly'):
            continue
        cleaned = INSN_PREFIX_RE.sub('', stripped.split(';', 1)[0].strip())
        cleaned = ' '.join(cleaned.split())
        if cleaned:
            insns.append(cleaned)
    return insns


def parse_symbols(raw: bytes, info: dict) -> list[tuple[str, int, int]]:
    sym_at = next(at for cmd, at, _ in info['commands'] if cmd == 2)
    at, count, strings, _ = struct.unpack_from('<4I', raw, sym_at + 8)
    result = []
    for i in range(count):
        ix, kind, _, _, address = struct.unpack_from('<IBBHQ', raw, at + i * 16)
        name = raw[strings + ix:raw.index(b'\0', strings + ix)].decode()
        result.append((name, address, kind))
    return result


def extract_vtable_slots(raw: bytes, info: dict, vtable_sym: str, expected_slots: int) -> list[str]:
    symbols = parse_symbols(raw, info)
    vtable = next((va for name, va, _ in symbols if name == vtable_sym), None)
    require(vtable is not None, f'Missing vtable symbol {vtable_sym}')
    relocations = {}
    for segment in info['segments']:
        for section in segment['sections']:
            if section['va'] <= vtable < section['va'] + section['size']:
                rel_at, rel_count = struct.unpack_from('<2I', raw, section['command_offset'] + 56)
                for i in range(rel_count):
                    offset, bits = struct.unpack_from('<2I', raw, rel_at + i * 8)
                    if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                        relocations[section['va'] + offset] = symbols[bits & 0xFFFFFF][0]
    slots = [relocations.get(vtable + 16 + i * 8) for i in range(expected_slots)]
    require(
        all(slots) and (vtable + 16 + expected_slots * 8) not in relocations,
        f'{vtable_sym} must have exact {expected_slots} slots',
    )
    return slots


def text_section(blob: bytes) -> bytes:
    section = next(
        s for seg in macho(blob)['segments'] for s in seg['sections'] if s['name'] == '__text'
    )
    return blob[section['fileoff']:section['fileoff'] + section['size']]


def make_late_object(obj_path: Path, source_filename: str) -> Path:
    late_path = obj_path.with_suffix('.late.o')
    objcopy = find_llvm_tool('llvm-objcopy')
    init_sym = f'__GLOBAL__sub_I_{source_filename}'
    subprocess.check_call(
        [
            objcopy,
            '--remove-section=__DATA,__mod_init_func',
            f'--globalize-symbol={init_sym}',
            str(obj_path),
            str(late_path),
        ],
        timeout=60,
    )
    orig_bytes = obj_path.read_bytes()
    late_bytes = late_path.read_bytes()
    require(text_section(orig_bytes) == text_section(late_bytes), f'Late objcopy altered __text in {obj_path.name}')
    late_info = macho(late_bytes)
    late_syms = parse_symbols(late_bytes, late_info)
    kind = next((k for name, _, k in late_syms if name == init_sym), None)
    require(kind is not None and (kind & 0x01) == 1, f'{init_sym} not globalized in {late_path.name}')
    return late_path


def audit_storage_vtable(
    base_obj: Path,
    block_obj: Path,
    vtable_sym: str = '__ZTV15Sm6125EmmcBlock',
    class_prefix: str = '15Sm6125EmmcBlock',
    class_label: str = 'Sm6125EmmcBlock',
) -> int:
    dwarf = subprocess.check_output(
        ['xcrun', 'dwarfdump', '--name=IOBlockStorageDevice', '--show-children', str(base_obj)],
        text=True,
        timeout=60,
    )
    class_size = re.search(
        r'DW_AT_name\s+\("IOBlockStorageDevice"\)[\s\S]{0,200}?DW_AT_byte_size\s+\((0x[0-9a-f]+)\)',
        dwarf,
    )
    require(
        class_size is not None and int(class_size.group(1), 16) == 0x90,
        f'Expected sizeof(IOBlockStorageDevice) == 0x90, got {class_size.group(1) if class_size else None}',
    )

    base_raw = base_obj.read_bytes()
    block_raw = block_obj.read_bytes()
    base_slots = extract_vtable_slots(base_raw, macho(base_raw), '__ZTV20IOBlockStorageDevice', 190)
    block_slots = extract_vtable_slots(block_raw, macho(block_raw), vtable_sym, 190)
    require(base_slots.count('___cxa_pure_virtual') == 14, 'Expected 14 pure-virtual slots in IOBlockStorageDevice')
    require('___cxa_pure_virtual' not in block_slots, f'{class_label} left an unimplemented pure-virtual slot')

    overrides = 0
    for idx, (expected, actual) in enumerate(zip(base_slots, block_slots)):
        own = actual.startswith((f'__ZN{class_prefix}', f'__ZNK{class_prefix}'))
        if expected == '___cxa_pure_virtual':
            require(own, f'Slot {idx} is pure virtual in IOBlockStorageDevice and not overridden by {class_label}')
        elif actual != expected:
            require(own, f'Slot {idx} mismatch: expected {expected} or {class_label} override, got {actual}')
        if own:
            overrides += 1
    return overrides


def audit_object_sections_and_disasm(final_objs: list[Path], probe_obj: Path) -> None:
    text_end = 0
    for obj in final_objs:
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

    for sym in (*READ_ACCESSOR_LOADS, *WRITE_ACCESSOR_STORES):
        require(sym in defined_syms, f'Missing MMIO accessor {sym} in {probe_obj.name}')

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
        insns = extract_symbol_insns(disasm)
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

    for sym, expected_store in WRITE_ACCESSOR_STORES.items():
        start = next((addr for addr, name in text_syms if name == sym), None)
        require(start is not None, f'Text symbol missing for {sym}')
        stop = next((addr for addr, _ in text_syms if addr > start), text_end)
        disasm = subprocess.check_output(
            [llvm_objdump, '-d', f'--start-address={start:#x}', f'--stop-address={stop:#x}', str(probe_obj)],
            text=True,
            timeout=60,
        )
        insns = extract_symbol_insns(disasm)
        require(bool(insns), f'No instructions decoded for {sym}')
        require(insns[-1].lower() == 'ret', f'{sym} must end with ret: {insns}')
        stores = []
        for insn in insns:
            mnemonic = insn.split()[0].lower()
            require(not LOAD_MNEMONIC_RE.match(mnemonic), f'Load instruction forbidden in {sym}: {insn}')
            if mnemonic.startswith('st'):
                require(not WRITEBACK_RE.search(insn), f'Writeback store forbidden in {sym}: {insn}')
                stores.append(mnemonic)
        require(stores == [expected_store], f'{sym} expected single [{expected_store}], got {stores}: {insns}')


def generate_root_policies_c(probe_dir: Path, out_c: Path) -> dict:
    pol_dir = probe_dir / 'policies/21H461'
    os_dir = pol_dir / 'OS'
    sys_dir = pol_dir / 'SystemOS'
    for d in (os_dir, sys_dir):
        for fn in ('manifest.header.bin', 'trusted-key.bin', 'verification.json', 'manifest.json'):
            require((d / fn).is_file(), f'Missing VerifiedRoot policy artifact: {d / fn}')

    os_key = (os_dir / 'trusted-key.bin').read_bytes()
    sys_key = (sys_dir / 'trusted-key.bin').read_bytes()
    require(len(os_key) == 395 and os_key == sys_key, 'OS and SystemOS must share a 395-byte owner public key')
    key_sha = hashlib.sha256(os_key).hexdigest()

    os_hdr = (os_dir / 'manifest.header.bin').read_bytes()
    sys_hdr = (sys_dir / 'manifest.header.bin').read_bytes()
    require(len(os_hdr) == 544 and len(sys_hdr) == 544, 'VerifiedRoot signed headers must be 544 bytes')

    os_ver = json.loads((os_dir / 'verification.json').read_text(encoding='utf-8'))
    sys_ver = json.loads((sys_dir / 'verification.json').read_text(encoding='utf-8'))
    os_man = json.loads((os_dir / 'manifest.json').read_text(encoding='utf-8'))
    sys_man = json.loads((sys_dir / 'manifest.json').read_text(encoding='utf-8'))

    require(
        os_ver['real_c_signature_and_block_verification'] and not os_ver['original_image_modified'] and
        os_ver['volume_uuid'] == '04769D26-2CA0-4825-B443-E2E5F88300F2' and
        os_ver['trusted_key_sha256'] == key_sha and
        os_hdr[56:88].hex() == os_ver['image_sha256'] == os_man['image_sha256'] and
        os_hdr[88:120].hex() == os_man['hash_table_sha256'],
        'OS VerifiedRoot policy failed build-time integrity check',
    )
    require(
        sys_ver['real_c_signature_and_block_verification'] and not sys_ver['original_image_modified'] and
        sys_ver['volume_uuid'] == '8846FE78-2910-4297-AF97-D59BB6F8FC16' and
        sys_ver['trusted_key_sha256'] == key_sha and
        sys_hdr[56:88].hex() == sys_ver['image_sha256'] == sys_man['image_sha256'] and
        sys_hdr[88:120].hex() == sys_man['hash_table_sha256'],
        'SystemOS VerifiedRoot policy failed build-time integrity check',
    )

    def fmt_bytes(blob: bytes) -> str:
        return ', '.join(f'0x{b:02x}u' for b in blob)

    c_text = f"""// Generated by build-sm6125-emmc-probe.py — DO NOT EDIT.
#include <stddef.h>
#include <stdint.h>

const uint8_t sm6125_verified_root_key[395] = {{ {fmt_bytes(os_key)} }};
const size_t sm6125_verified_root_key_len = 395u;
const uint64_t sm6125_verified_root_min_generation = 1ULL;

const uint8_t sm6125_verified_os_header[544] = {{ {fmt_bytes(os_hdr)} }};
const size_t sm6125_verified_os_header_len = 544u;

const uint8_t sm6125_verified_systemos_header[544] = {{ {fmt_bytes(sys_hdr)} }};
const size_t sm6125_verified_systemos_header_len = 544u;
"""
    out_c.write_text(c_text, encoding='utf-8')
    return {
        'trusted_key_sha256': key_sha,
        'os_header_sha256': hashlib.sha256(os_hdr).hexdigest(),
        'systemos_header_sha256': hashlib.sha256(sys_hdr).hexdigest(),
        'os_table_sha256': os_man['hash_table_sha256'],
        'systemos_table_sha256': sys_man['hash_table_sha256'],
    }


def main() -> None:
    script_dir = Path(__file__).resolve().parent
    probe_dir = script_dir / 'emmc-probe'
    ro_src = probe_dir / 'Sm6125EmmcReadOnly.c'
    core_src = probe_dir / 'Sm6125EmmcCore.c'
    shng_src = probe_dir / 'Sm6125ShngRoot.c'
    cpp_src = probe_dir / 'Sm6125EmmcProbe.cpp'
    blk_src = probe_dir / 'Sm6125EmmcBlock.cpp'
    ram_dir = (ROOT / 'drivers/RamBlock').resolve()
    if not (ram_dir / 'NativeRamBlock.cpp').is_file():
        for parent in ROOT.parents:
            if (parent / 'drivers/RamBlock/NativeRamBlock.cpp').is_file():
                ram_dir = (parent / 'drivers/RamBlock').resolve()
                break
    ram_src = ram_dir / 'NativeRamBlock.cpp'
    require(ram_src.is_file(), f'Missing NativeRamBlock.cpp under {ram_dir}')
    vr_kit = ROOT / 'kits/VerifiedRoot'
    if not (vr_kit / 'src/verified_root.c').is_file():
        for parent in ROOT.parents:
            if (parent / 'kits/VerifiedRoot/src/verified_root.c').is_file():
                vr_kit = parent / 'kits/VerifiedRoot'
                break
    vr_src = (vr_kit / 'src/verified_root.c').resolve()
    crypto_src = (vr_kit / 'adapters/xnu_crypto.c').resolve()
    vr_inc = (vr_kit / 'include').resolve()
    pol_src = OUT / 'Sm6125RootPolicies.c'
    policy_report = generate_root_policies_c(probe_dir, pol_src)

    storage_dir = (ROOT / 'refer/IOStorageFamily').resolve()
    headers_helper = load_helper('prepare_storage_headers', ROOT / 'scripts/prepare-storage-headers.py')
    headers_helper.prepare(storage_dir, IOSTORAGEFAMILY_REVISION, OUT / 'headers')

    base_cpp = OUT / 'IOBlockStorageDevice.cpp'
    original_base = (storage_dir / 'IOBlockStorageDevice.cpp').read_text(encoding='utf-8')
    macro = 'OSDefineMetaClassAndAbstractStructors(IOBlockStorageDevice,IOService)'
    require(original_base.count(macro) == 1, 'Pinned IOBlockStorageDevice registration macro changed')
    base_cpp.write_text(
        original_base.replace(
            macro,
            '''
extern "C" const OSMetaClass *sm6125_emmc_storage_parent(void);
OSDefineMetaClassWithInit(IOBlockStorageDevice, IOService, )
IOBlockStorageDevice::MetaClass::MetaClass()
    : OSMetaClass("Sm6125EmmcBlockStorageBase", sm6125_emmc_storage_parent(), sizeof(IOBlockStorageDevice)) {}
OSDefineAbstractStructors(IOBlockStorageDevice, IOService)
OSDefineOperatorMethods(IOBlockStorageDevice)
''',
        ),
        encoding='utf-8',
    )

    ro_obj = OUT / 'Sm6125EmmcReadOnly.o'
    core_obj = OUT / 'Sm6125EmmcCore.o'
    shng_obj = OUT / 'Sm6125ShngRoot.o'
    vr_obj = OUT / 'verified_root.o'
    crypto_obj = OUT / 'xnu_crypto.o'
    pol_obj = OUT / 'Sm6125RootPolicies.o'
    cpp_obj = OUT / 'Sm6125EmmcProbe.o'
    base_obj = OUT / 'IOBlockStorageDevice.o'
    blk_obj = OUT / 'Sm6125EmmcBlock.o'
    ram_obj = OUT / 'NativeRamBlock.o'

    obj_root = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    c_record = obj_root / 'bsd/DEVELOPMENT/bsd_init.o.json'
    cpp_record = obj_root / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'

    common_warnings = [
        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator',
        '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
        '-Wno-unnecessary-virtual-specifier',
    ]
    storage_includes = [
        '-I', str(OUT / 'headers/include'),
        '-I', str(probe_dir),
        '-I', str(vr_inc),
        '-I', str(ROOT / '.cache/xnu-work/osfmk'),
    ]

    replay_compile(c_record, ro_src, ro_obj, [*common_warnings, '-I', str(probe_dir)])
    replay_compile(c_record, core_src, core_obj, [*common_warnings, '-I', str(probe_dir)])
    replay_compile(c_record, shng_src, shng_obj, [*common_warnings, '-I', str(probe_dir)])
    replay_compile(c_record, vr_src, vr_obj, [*common_warnings, '-I', str(vr_inc)])
    replay_compile(c_record, crypto_src, crypto_obj, [*common_warnings, '-I', str(vr_inc)])
    replay_compile(c_record, pol_src, pol_obj, [*common_warnings])
    replay_compile(cpp_record, cpp_src, cpp_obj, [*common_warnings, '-I', str(probe_dir)])
    replay_compile(
        cpp_record,
        base_cpp,
        base_obj,
        [*common_warnings, '-Wno-deprecated-declarations', '-Wno-error', *storage_includes],
    )
    replay_compile(
        cpp_record,
        blk_src,
        blk_obj,
        [*common_warnings, '-Wno-deprecated-declarations', *storage_includes],
    )
    replay_compile(
        cpp_record,
        ram_src,
        ram_obj,
        [*common_warnings, '-Wno-deprecated-declarations', '-Wno-error', '-I', str(ram_dir), *storage_includes],
    )

    check_object(base_obj.read_bytes(), base_obj.stem)
    check_object(blk_obj.read_bytes(), blk_obj.stem)
    check_object(ram_obj.read_bytes(), ram_obj.stem)
    overrides = audit_storage_vtable(base_obj, blk_obj)
    ram_overrides = audit_storage_vtable(
        base_obj, ram_obj, '__ZTV14NativeRamBlock', '14NativeRamBlock', 'NativeRamBlock',
    )

    base_late_obj = make_late_object(base_obj, 'IOBlockStorageDevice.cpp')
    blk_late_obj = make_late_object(blk_obj, 'Sm6125EmmcBlock.cpp')
    ram_late_obj = make_late_object(ram_obj, 'NativeRamBlock.cpp')

    final_objs = [
        ro_obj, core_obj, shng_obj, vr_obj, crypto_obj, pol_obj, cpp_obj,
        base_late_obj, blk_late_obj, ram_late_obj,
    ]
    audit_object_sections_and_disasm(final_objs, cpp_obj)

    archive = OUT / 'libSm6125EmmcProbe.a'
    archive.unlink(missing_ok=True)
    subprocess.check_call(
        ['xcrun', 'ar', 'rcs', str(archive), *(str(o) for o in final_objs)],
        timeout=60,
    )

    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    stamp = obj_root / '.sm6125-emmc-probe.sha256'
    if not stamp.is_file() or stamp.read_text(encoding='utf-8').strip() != digest:
        (obj_root / 'mach.development.bcm2837.unstripped').unlink(missing_ok=True)
        stamp.parent.mkdir(parents=True, exist_ok=True)
        stamp.write_text(digest + '\n', encoding='utf-8')

    report = {
        'ro_source_sha256': hashlib.sha256(ro_src.read_bytes()).hexdigest(),
        'core_source_sha256': hashlib.sha256(core_src.read_bytes()).hexdigest(),
        'shng_source_sha256': hashlib.sha256(shng_src.read_bytes()).hexdigest(),
        'cpp_source_sha256': hashlib.sha256(cpp_src.read_bytes()).hexdigest(),
        'blk_source_sha256': hashlib.sha256(blk_src.read_bytes()).hexdigest(),
        'ram_source_sha256': hashlib.sha256(ram_src.read_bytes()).hexdigest(),
        'iostorage_revision': IOSTORAGEFAMILY_REVISION,
        'verified_root_policies': policy_report,
        'vtable_slots_checked': 190,
        'vtable_overrides': overrides,
        'ram_vtable_overrides': ram_overrides,
        'archive_sha256': digest,
        'mode': 'SM6125 GCC SDCC1 + SDHCI-MSM v5 probe, VerifiedRoot OS+SystemOS, and NativeRamBlock Data store',
    }
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'PASS: Sm6125EmmcProbe+Block+Data ({archive} sha256 {digest}, 190 vtable slots, {overrides}+{ram_overrides} overrides)')


if __name__ == '__main__':
    main()
