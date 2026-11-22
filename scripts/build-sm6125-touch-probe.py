#!/usr/bin/env python3
"""Build libSm6125TouchProbe.a (QcomGeniSpi + Sm6125GeniFifo + Sm6125NvtProbe + Sm6125TouchProbe + Sm6125HidInput)."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/sm6125-touch-probe'
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))

from kernel_target import TRIPLE, check_object  # noqa: E402
from native_prelink import macho  # noqa: E402

# Audited 207-slot vtable of IOHIDDevice in iPadOS 17.7.11 (21H461, IOHIDFamily-2031.120.4, slide -0x1000000).
IOHID_VPTR = 0xFFFFFFF005D045C0
IOHID_C2 = 0xFFFFFFF004AD94F8
IOHID_D2 = 0xFFFFFFF004AD9518
IOHID_GMETA = 0xFFFFFFF006AE6FA0
IOHID_VTABLE = [
    0xFFFFFFF004AD951C, 0xFFFFFFF004AD9520, 0xFFFFFFF00785F83C, 0xFFFFFFF00785F620, 0xFFFFFFF00785F814, 0xFFFFFFF00785F804,
    0xFFFFFFF00785F850, 0xFFFFFFF004AD9524, 0xFFFFFFF00785D854, 0xFFFFFFF00785F698, 0xFFFFFFF00785F700, 0xFFFFFFF00785F710,
    0xFFFFFFF004B0B55C, 0xFFFFFFF00785FAAC, 0xFFFFFFF004AD95FC, 0xFFFFFFF0078BF4BC, 0xFFFFFFF0078BF3A8, 0xFFFFFFF0078BF294,
    0xFFFFFFF0078C1854, 0xFFFFFFF0078C1A2C, 0xFFFFFFF0078BFB44, 0xFFFFFFF004AD9590, 0xFFFFFFF0078BEBB0, 0xFFFFFFF004ADCED0,
    0xFFFFFFF0078BFD34, 0xFFFFFFF0078BFD98, 0xFFFFFFF0078BFDFC, 0xFFFFFFF0078BFE80, 0xFFFFFFF0078BFF00, 0xFFFFFFF0078BFF88,
    0xFFFFFFF0078BFA2C, 0xFFFFFFF0078BFCA4, 0xFFFFFFF0078BFCEC, 0xFFFFFFF0078BF9D8, 0xFFFFFFF0078BFBFC, 0xFFFFFFF0078BFC50,
    0xFFFFFFF0078BEE84, 0xFFFFFFF0078BF06C, 0xFFFFFFF0078BF180, 0xFFFFFFF0078BED4C, 0xFFFFFFF0078BEDB4, 0xFFFFFFF0078BEE1C,
    0xFFFFFFF0078BECE4, 0xFFFFFFF0078D0328, 0xFFFFFFF0078BED40, 0xFFFFFFF0078C17A8, 0xFFFFFFF0078C1BC0, 0xFFFFFFF0078C18D4,
    0xFFFFFFF0078C1910, 0xFFFFFFF0078C1AE8, 0xFFFFFFF0078C1AAC, 0xFFFFFFF0078C1C98, 0xFFFFFFF0078C1E08, 0xFFFFFFF0078C1F78,
    0xFFFFFFF0078C2934, 0xFFFFFFF0078C208C, 0xFFFFFFF0078C2494, 0xFFFFFFF0078C233C, 0xFFFFFFF0078C25E4, 0xFFFFFFF0078C2734,
    0xFFFFFFF0078C27D4, 0xFFFFFFF0078C011C, 0xFFFFFFF0078C01D4, 0xFFFFFFF0078C06FC, 0xFFFFFFF0078C0670, 0xFFFFFFF0078C0364,
    0xFFFFFFF0078C0488, 0xFFFFFFF0078C0314, 0xFFFFFFF0078C0284, 0xFFFFFFF0078C0560, 0xFFFFFFF0078C0604, 0xFFFFFFF0078C08B8,
    0xFFFFFFF0078C0FA8, 0xFFFFFFF0078C178C, 0xFFFFFFF0078C58C8, 0xFFFFFFF0078C96B4, 0xFFFFFFF004ADB0E4, 0xFFFFFFF0078CB24C,
    0xFFFFFFF0078DC9D8, 0xFFFFFFF0078E47A4, 0xFFFFFFF0078CC4D0, 0xFFFFFFF004ADAEA4, 0xFFFFFFF004ADAEB0, 0xFFFFFFF0078D1918,
    0xFFFFFFF0078C6A98, 0xFFFFFFF0078C5834, 0xFFFFFFF004AD98EC, 0xFFFFFFF004ADAEBC, 0xFFFFFFF0078CC01C, 0xFFFFFFF0078CC17C,
    0xFFFFFFF0078CC2A0, 0xFFFFFFF004ADC3AC, 0xFFFFFFF004ADC824, 0xFFFFFFF004ADC93C, 0xFFFFFFF0078CC014, 0xFFFFFFF0078CBD90,
    0xFFFFFFF0078C87F4, 0xFFFFFFF0078C8AC4, 0xFFFFFFF0078CBFB0, 0xFFFFFFF0078CFCBC, 0xFFFFFFF0078CFC00, 0xFFFFFFF004ADB4CC,
    0xFFFFFFF0078C7B38, 0xFFFFFFF0078D2F20, 0xFFFFFFF0078CF0FC, 0xFFFFFFF0078D2CC8, 0xFFFFFFF0078D2D5C, 0xFFFFFFF0078D2DF0,
    0xFFFFFFF0078C5F30, 0xFFFFFFF0078C61CC, 0xFFFFFFF0078C7B48, 0xFFFFFFF0078C7BBC, 0xFFFFFFF0078C7BEC, 0xFFFFFFF0078C7E30,
    0xFFFFFFF0078C7C00, 0xFFFFFFF0078C7C14, 0xFFFFFFF0078C7EEC, 0xFFFFFFF0078C7FA8, 0xFFFFFFF0078C8200, 0xFFFFFFF0078C7B40,
    0xFFFFFFF0078D3398, 0xFFFFFFF0078D33E0, 0xFFFFFFF0078D343C, 0xFFFFFFF0078D3480, 0xFFFFFFF0078D34B0, 0xFFFFFFF0078D3724,
    0xFFFFFFF0078D3898, 0xFFFFFFF0078D3DA0, 0xFFFFFFF0078D3E54, 0xFFFFFFF0078D3ED4, 0xFFFFFFF0078D3F54, 0xFFFFFFF0078D326C,
    0xFFFFFFF004ADCCB4, 0xFFFFFFF0078C8C8C, 0xFFFFFFF0078C8FE4, 0xFFFFFFF0078C9080, 0xFFFFFFF0078C8C20, 0xFFFFFFF0078C8C34,
    0xFFFFFFF0078C8E2C, 0xFFFFFFF0078C9074, 0xFFFFFFF004ADC978, 0xFFFFFFF0078D3224, 0xFFFFFFF0078D32A0, 0xFFFFFFF0078D32B0,
    0xFFFFFFF0078D4760, 0xFFFFFFF0078D6BBC, 0xFFFFFFF0078D6B80, 0xFFFFFFF0078D88E8, 0xFFFFFFF0078DB470, 0xFFFFFFF0078DC404,
    0xFFFFFFF0078DCCE8, 0xFFFFFFF0078DCCF0, 0xFFFFFFF0078D76EC, 0xFFFFFFF0078D8414, 0xFFFFFFF0078DC824, 0xFFFFFFF0078E464C,
    0xFFFFFFF0078E46A0, 0xFFFFFFF0078E46EC, 0xFFFFFFF0078E4744, 0xFFFFFFF0078E4790, 0xFFFFFFF0078E4798, 0xFFFFFFF0078E1A6C,
    0xFFFFFFF0078E1A78, 0xFFFFFFF0078E32CC, 0xFFFFFFF0078E3428, 0xFFFFFFF0078E3D2C, 0xFFFFFFF0078E3D94, 0xFFFFFFF0078E47A0,
    0xFFFFFFF004ADC3A0, 0xFFFFFFF004ADC3A8, 0xFFFFFFF004ADB680, 0xFFFFFFF004ADCFDC, 0xFFFFFFF004ADCFE4, 0xFFFFFFF004ADCFEC,
    0xFFFFFFF004ADCFF4, 0xFFFFFFF004ADCFFC, 0xFFFFFFF004ADD004, 0xFFFFFFF004ADD00C, 0xFFFFFFF004ADD014, 0xFFFFFFF004ADD06C,
    0xFFFFFFF007A5EC70, 0xFFFFFFF004ADD0C4, 0xFFFFFFF004ADD160, 0xFFFFFFF004ADD294, 0xFFFFFFF004ADDC54, 0xFFFFFFF004ADDC44,
    0xFFFFFFF004ADDC68, 0xFFFFFFF004ADDD74, 0xFFFFFFF004ADDEA8, 0xFFFFFFF004ADDF34, 0xFFFFFFF004ADDF50, 0xFFFFFFF004ADDF68,
    0xFFFFFFF004ADE02C, 0xFFFFFFF004ADE184, 0xFFFFFFF004ADE1A4, 0xFFFFFFF004ADE1C4, 0xFFFFFFF004ADE1CC, 0xFFFFFFF004ADE1D4,
    0xFFFFFFF004ADE6D0, 0xFFFFFFF004ADE75C, 0xFFFFFFF004ADD378, 0xFFFFFFF004ADE890, 0xFFFFFFF004ADEA2C, 0xFFFFFFF004ADEB38,
    0xFFFFFFF004ADECA8, 0xFFFFFFF004ADF42C, 0xFFFFFFF004ADF434,
]


def generate_hid_abi_header(path: Path) -> None:
    lines = [
        '// Generated from 21H461 IOHIDFamily-2031.120.4 ABI; opaque private storage only.',
        '#pragma once',
        '#include <IOKit/IOService.h>',
        '#include <IOKit/IOMemoryDescriptor.h>',
        'class IOHIDDevice : public IOService {',
        '  OSDeclareAbstractStructors(IOHIDDevice)',
        '  unsigned char originalPrivate[0xd0 - sizeof(IOService)];',
        'public:',
        '  using IOService::setProperty;',
        '  IOReturn Dispatch(IORPC) override;',
        '  void free() override;',
        '  bool init(OSDictionary *properties = nullptr) override;',
        '  bool setProperty(const OSSymbol *, OSObject *) override;',
        '  bool willTerminate(IOService *, IOOptionBits) override;',
        '  IOReturn configureReport(IOReportChannelList *, IOReportConfigureAction, void *, void *) override;',
        '  IOReturn updateReport(IOReportChannelList *, IOReportUpdateAction, void *, void *) override;',
        '  bool start(IOService *) override;',
        '  void stop(IOService *) override;',
        '  bool handleOpen(IOService *, IOOptionBits, void *) override;',
        '  void handleClose(IOService *, IOOptionBits) override;',
        '  bool handleIsOpen(const IOService *) const override;',
        '  bool matchPropertyTable(OSDictionary *, SInt32 *) override;',
        '  IOReturn message(UInt32, IOService *, void *) override;',
        '  IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) override;',
    ]
    for i in range(168, 207):
        if i == 171:
            lines.append('  virtual OSString *newTransportString() const;')
        elif i == 173:
            lines.append('  virtual OSString *newProductString() const;')
        elif i == 180:
            lines.append('  virtual IOReturn newReportDescriptor(IOMemoryDescriptor **) const = 0;')
        elif i == 181:
            lines.append('  virtual IOReturn handleReport(IOMemoryDescriptor *, unsigned, IOOptionBits);')
        else:
            lines.append(f'  virtual __attribute__((deprecated("Unqualified prototype"))) void opaque{i}();')
    lines.append('};')
    lines.append('static_assert(sizeof(IOHIDDevice) == 0xd0, "Original IOHIDDevice instance size");')
    path.write_text('\n'.join(lines) + '\n', encoding='utf-8')


def generate_hid_bindings(obj_path: Path, asm_path: Path) -> None:
    raw = obj_path.read_bytes()
    om = macho(raw)
    st = next(p for c, p, _ in om['commands'] if c == 2)
    sy, count, strings, _ = struct.unpack_from('<4I', raw, st + 8)
    symbols = []
    for i in range(count):
        ix, kind, _, _, va = struct.unpack_from('<IBBHQ', raw, sy + i * 16)
        symbols.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), va, kind))
    vt = next(va for name, va, _ in symbols if name == '__ZTV14Sm6125HidInput')
    relocations = {}
    for segment in om['segments']:
        for sec in segment['sections']:
            if sec['va'] <= vt < sec['va'] + sec['size']:
                ro, rc = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
                for i in range(rc):
                    offset, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                    if (bits >> 27) & 1 and (bits >> 28) == 0 and ((bits >> 25) & 3) == 3:
                        relocations[sec['va'] + offset] = symbols[bits & 0xFFFFFF][0]
    compiled = [relocations.get(vt + 16 + i * 8) for i in range(207)]
    require(all(compiled) and (vt + 16 + 207 * 8) not in relocations, 'Sm6125HidInput vtable must be exactly 207 slots')
    bindings = {
        '__ZN11IOHIDDeviceC2EPK11OSMetaClass': IOHID_C2,
        '__ZN11IOHIDDeviceD2Ev': IOHID_D2,
        '__ZN11IOHIDDevice10gMetaClassE': IOHID_GMETA,
        '__ZTV11IOHIDDevice': IOHID_VPTR - 16,
    }
    owned = []
    for i, name in enumerate(compiled):
        if name.startswith(('__ZN14Sm6125HidInput', '__ZNK14Sm6125HidInput')):
            owned.append(i)
        elif '11IOHIDDevice' in name:
            bindings[name] = IOHID_VTABLE[i]
    require(owned == [0, 1, 7, 14, 86, 87, 111, 171, 173, 180], f'Unexpected Sm6125HidInput owned slots: {owned}')
    undefined = {n for n, _, k in symbols if k == 1 and '11IOHIDDevice' in n}
    require(undefined == set(bindings), f'Unbound IOHIDDevice symbols: {undefined ^ set(bindings)}')
    asm_path.write_text(''.join(f'.globl {n}\n.set {n}, {a:#x}\n' for n, a in sorted(bindings.items())), encoding='utf-8')


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


def find_touch_fw_bin() -> Path | None:
    env_path = os.environ.get('SM6125_TOUCH_FW_BIN', '').strip()
    if env_path:
        path = Path(env_path).expanduser().resolve()
        require(path.is_file(), f'SM6125_TOUCH_FW_BIN not found: {path}')
        return path
    candidate = (ROOT.parent / 'vendor-firmware/novatek_ts_tianma_fw.bin').resolve()
    if candidate.is_file():
        return candidate
    return None


def generate_fw_c(out_c: Path, fw_path: Path | None) -> tuple[bool, str | None]:
    if fw_path is None:
        out_c.write_text(
            '#include <stdint.h>\n'
            'extern const uint8_t sm6125_touch_fw_data[1];\n'
            'extern const uint32_t sm6125_touch_fw_size;\n'
            'extern const char sm6125_touch_fw_name[];\n'
            'const uint8_t sm6125_touch_fw_data[1] = {0};\n'
            'const uint32_t sm6125_touch_fw_size = 0u;\n'
            'const char sm6125_touch_fw_name[] = "none";\n',
            encoding='utf-8',
        )
        return False, None

    blob = fw_path.read_bytes()
    require(len(blob) == 118784, f'Unexpected touch firmware size in {fw_path}: {len(blob)}')
    require(blob[0x1affd:0x1b000] == b'NVT', f'Missing NVT signature at 0x1affd in {fw_path}')
    digest = hashlib.sha256(blob).hexdigest()

    lines = [
        '#include <stdint.h>',
        f'extern const uint8_t sm6125_touch_fw_data[{len(blob)}];',
        'extern const uint32_t sm6125_touch_fw_size;',
        'extern const char sm6125_touch_fw_name[];',
        f'const uint32_t sm6125_touch_fw_size = {len(blob)}u;',
        f'const char sm6125_touch_fw_name[] = "{fw_path.name}";',
        f'const uint8_t sm6125_touch_fw_data[{len(blob)}] = {{',
    ]
    for off in range(0, len(blob), 16):
        chunk = ', '.join(f'0x{b:02x}' for b in blob[off:off + 16])
        lines.append(f'  {chunk},')
    lines.append('};')
    out_c.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return True, digest


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
    fifo_src = probe_dir / 'Sm6125GeniFifo.c'
    nvt_src = probe_dir / 'Sm6125NvtProbe.c'
    btn_src = probe_dir / 'Sm6125Buttons.c'
    cpp_src = probe_dir / 'Sm6125TouchProbe.cpp'
    hid_src = probe_dir / 'Sm6125HidInput.cpp'
    for src in (geni_src, fifo_src, nvt_src, btn_src, cpp_src, hid_src):
        require(src.is_file(), f'Missing source file: {src}')

    fw_path = find_touch_fw_bin()
    fw_src = OUT / 'Sm6125TouchFw.c'
    fw_embedded, fw_sha256 = generate_fw_c(fw_src, fw_path)

    hid_abi_h = OUT / 'OriginalHidDeviceABI.h'
    generate_hid_abi_header(hid_abi_h)

    obj_root = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    c_record = obj_root / 'bsd/DEVELOPMENT/bsd_init.o.json'
    cpp_record = obj_root / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'

    common_warnings = [
        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator',
        '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
        '-Wno-unnecessary-virtual-specifier',
    ]
    inc_flags = ['-I', str(probe_dir), '-I', str(geni_dir), '-I', str(OUT)]

    geni_obj = OUT / 'GeniSpi.o'
    fifo_obj = OUT / 'Sm6125GeniFifo.o'
    nvt_obj = OUT / 'Sm6125NvtProbe.o'
    btn_obj = OUT / 'Sm6125Buttons.o'
    fw_obj = OUT / 'Sm6125TouchFw.o'
    cpp_obj = OUT / 'Sm6125TouchProbe.o'
    hid_obj = OUT / 'Sm6125HidInput.o'
    hid_late_obj = OUT / 'Sm6125HidInput.late.o'
    hid_bind_s = OUT / 'original-hid-bindings.S'
    hid_bind_obj = OUT / 'original-hid-bindings.o'

    replay_compile(c_record, geni_src, geni_obj, [*common_warnings, '-I', str(geni_dir)])
    replay_compile(c_record, fifo_src, fifo_obj, [*common_warnings, *inc_flags])
    replay_compile(c_record, nvt_src, nvt_obj, [*common_warnings, '-I', str(probe_dir)])
    replay_compile(c_record, btn_src, btn_obj, [*common_warnings, '-I', str(probe_dir)])
    replay_compile(c_record, fw_src, fw_obj, common_warnings)
    replay_compile(cpp_record, cpp_src, cpp_obj, [*common_warnings, *inc_flags])
    replay_compile(cpp_record, hid_src, hid_obj, [*common_warnings, *inc_flags])

    for obj in (geni_obj, fifo_obj, nvt_obj, btn_obj, fw_obj, cpp_obj, hid_obj):
        raw = obj.read_bytes()
        check_object(raw, obj.stem)
        if obj != hid_obj:
            info = macho(raw)
            sec_names = [s['name'] for seg in info['segments'] for s in seg['sections']]
            require('__mod_init_func' not in sec_names, f'Unexpected __mod_init_func in {obj.name}')

    llvm_objcopy = find_llvm_tool('llvm-objcopy')
    subprocess.check_call([
        llvm_objcopy,
        '--remove-section=__DATA,__mod_init_func',
        '--globalize-symbol=__GLOBAL__sub_I_Sm6125HidInput.cpp',
        str(hid_obj),
        str(hid_late_obj),
    ], timeout=60)

    generate_hid_bindings(hid_obj, hid_bind_s)
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    subprocess.check_call([
        'xcrun', 'clang', '-target', TRIPLE, '-isysroot', sdk,
        '-c', str(hid_bind_s), '-o', str(hid_bind_obj),
    ], timeout=60)

    objs = [geni_obj, fifo_obj, nvt_obj, btn_obj, fw_obj, cpp_obj, hid_late_obj, hid_bind_obj]

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
        'geni_fifo_sha256': hashlib.sha256(fifo_src.read_bytes()).hexdigest(),
        'nvt_probe_sha256': hashlib.sha256(nvt_src.read_bytes()).hexdigest(),
        'touch_probe_sha256': hashlib.sha256(cpp_src.read_bytes()).hexdigest(),
        'fw_embedded': fw_embedded,
        'fw_sha256': fw_sha256,
        'archive_sha256': digest,
        'mode': 'SM6125 QUPv3 SE2 SPI + NT36672A Chip ID + host-download/points probe (sm6125-touch=1..4)',
    }
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'PASS: Sm6125TouchProbe ({archive} sha256 {digest}, fw_embedded={fw_embedded})')


if __name__ == '__main__':
    main()
