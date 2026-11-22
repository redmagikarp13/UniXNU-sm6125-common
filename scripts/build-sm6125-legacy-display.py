#!/usr/bin/env python3
"""Build libNativeLegacyDisplay.late.a against the audited iPadOS 17.7.11 (21H461) IOMobileFramebufferLegacy ABI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from kernel_target import TRIPLE, check_object  # noqa: E402

# Audited 358-slot vtable of IOMobileFramebufferLegacy in 21H461 (with slide -0x1000000).
LEGACY_VTABLE = [
    0xfffffff005786644, 0xfffffff005786658, 0xfffffff00783b1fc, 0xfffffff00783afe0, 0xfffffff00783b1d4, 0xfffffff00783b1c4,
    0xfffffff00783b210, 0xfffffff005786664, 0xfffffff007839214, 0xfffffff00783b058, 0xfffffff00783b0c0, 0xfffffff00783b0d0,
    0xfffffff007881910, 0xfffffff00783b46c, 0xfffffff005787030, 0xfffffff00789ae7c, 0xfffffff00789ad68, 0xfffffff00789ac54,
    0xfffffff00789d214, 0xfffffff00789d3ec, 0xfffffff00789b504, 0xfffffff005786e20, 0xfffffff00789a570, 0xfffffff00789b434,
    0xfffffff00789b6f4, 0xfffffff00789b758, 0xfffffff00789b7bc, 0xfffffff00789b840, 0xfffffff00789b8c0, 0xfffffff00789b948,
    0xfffffff00789b3ec, 0xfffffff00789b664, 0xfffffff00789b6ac, 0xfffffff00789b398, 0xfffffff00789b5bc, 0xfffffff00789b610,
    0xfffffff00789a844, 0xfffffff00789aa2c, 0xfffffff00789ab40, 0xfffffff00789a70c, 0xfffffff00789a774, 0xfffffff00789a7dc,
    0xfffffff00789a6a4, 0xfffffff0078abce8, 0xfffffff005792404, 0xfffffff00789d168, 0xfffffff00789d580, 0xfffffff00789d294,
    0xfffffff00789d2d0, 0xfffffff00789d4a8, 0xfffffff00789d46c, 0xfffffff00789d658, 0xfffffff00789d7c8, 0xfffffff00789d938,
    0xfffffff00789e2f4, 0xfffffff00789da4c, 0xfffffff00789de54, 0xfffffff00789dcfc, 0xfffffff00789dfa4, 0xfffffff00789e0f4,
    0xfffffff00789e194, 0xfffffff00789badc, 0xfffffff00789bb94, 0xfffffff00789c0bc, 0xfffffff00789c030, 0xfffffff00789bd24,
    0xfffffff00789be48, 0xfffffff00789bcd4, 0xfffffff00789bc44, 0xfffffff00789bf20, 0xfffffff00789bfc4, 0xfffffff00789c278,
    0xfffffff00789c968, 0xfffffff00789d14c, 0xfffffff0078a1288, 0xfffffff0078a5074, 0xfffffff0078a6be8, 0xfffffff0078a6c0c,
    0xfffffff0078b8398, 0xfffffff00578b0a8, 0xfffffff0078a7e90, 0xfffffff0078af994, 0xfffffff0078afb54, 0xfffffff0078ad2d8,
    0xfffffff0078a2458, 0xfffffff005787060, 0xfffffff0057872e8, 0xfffffff00578886c, 0xfffffff0078a79dc, 0xfffffff0078a7b3c,
    0xfffffff0078a7c60, 0xfffffff0078a7cbc, 0xfffffff0078a7d2c, 0xfffffff0078a7d44, 0xfffffff0078a79d4, 0xfffffff0078a7750,
    0xfffffff0078a41b4, 0xfffffff0078a4484, 0xfffffff0078a7970, 0xfffffff0078ab67c, 0xfffffff0078ab5c0, 0xfffffff0078a34ec,
    0xfffffff0078a34f8, 0xfffffff0078ae8e0, 0xfffffff0078aaabc, 0xfffffff0078ae688, 0xfffffff0078ae71c, 0xfffffff0078ae7b0,
    0xfffffff0078a18f0, 0xfffffff0078a1b8c, 0xfffffff0078a3508, 0xfffffff00578cb78, 0xfffffff0078a35ac, 0xfffffff0078a37f0,
    0xfffffff0078a35c0, 0xfffffff0078a35d4, 0xfffffff0078a38ac, 0xfffffff0078a3968, 0xfffffff0078a3bc0, 0xfffffff0078a3500,
    0xfffffff0078aed58, 0xfffffff0078aeda0, 0xfffffff0078aedfc, 0xfffffff0078aee40, 0xfffffff0078aee70, 0xfffffff0078af0e4,
    0xfffffff0078af258, 0xfffffff0078af760, 0xfffffff0078af814, 0xfffffff0078af894, 0xfffffff0078af914, 0xfffffff0078aec2c,
    0xfffffff0078aed4c, 0xfffffff0078a464c, 0xfffffff0078a49a4, 0xfffffff0078a4a40, 0xfffffff0078a45e0, 0xfffffff0078a45f4,
    0xfffffff0078a47ec, 0xfffffff0078a4a34, 0xfffffff0078ae934, 0xfffffff005789080, 0xfffffff0078aec60, 0xfffffff0078aec70,
    0xfffffff0078b0120, 0xfffffff0078b257c, 0xfffffff0078b2540, 0xfffffff0078b42a8, 0xfffffff0078b6e30, 0xfffffff0078b7dc4,
    0xfffffff0078b86a8, 0xfffffff0078b86b0, 0xfffffff0078b30ac, 0xfffffff0078b3dd4, 0xfffffff0078b81e4, 0xfffffff0057972b0,
    0xfffffff0078c0060, 0xfffffff0078c00ac, 0xfffffff0078c0104, 0xfffffff0078c0150, 0xfffffff0078c0158, 0xfffffff0078bd42c,
    0xfffffff0078bd438, 0xfffffff0078bec8c, 0xfffffff0078bede8, 0xfffffff0078bf6ec, 0xfffffff0078bf754, 0xfffffff0078c0160,
    0xfffffff00579fb24, 0xfffffff0057891d8, 0xfffffff00578c998, 0xfffffff00578cb00, 0xfffffff00578cb10, 0xfffffff00578cb90,
    0xfffffff00578cbf8, 0xfffffff005792774, 0xfffffff00578d998, 0xfffffff00578e290, 0xfffffff00578f224, 0xfffffff005791ffc,
    0xfffffff005798404, 0xfffffff005798538, 0xfffffff00579fb3c, 0xfffffff005797470, 0xfffffff00578cb20, 0xfffffff00579748c,
    0xfffffff0057980e0, 0xfffffff00578cbb4, 0xfffffff005798184, 0xfffffff0057982bc, 0xfffffff0057974ac, 0xfffffff0057978b0,
    0xfffffff005797d74, 0xfffffff005798014, 0xfffffff0057977c8, 0xfffffff005795bec, 0xfffffff005795c08, 0xfffffff005795c24,
    0xfffffff005795e00, 0xfffffff005795c60, 0xfffffff005795c44, 0xfffffff005795cb8, 0xfffffff005795c7c, 0xfffffff005795c9c,
    0xfffffff005795d10, 0xfffffff005795d5c, 0xfffffff005795d78, 0xfffffff005795de4, 0xfffffff005795e14, 0xfffffff0057960f0,
    0xfffffff005794058, 0xfffffff007a2adb8, 0xfffffff00579fb50, 0xfffffff007a2adb8, 0xfffffff005796b7c, 0xfffffff005796d0c,
    0xfffffff005796f0c, 0xfffffff0057893bc, 0xfffffff00578961c, 0xfffffff00578dde4, 0xfffffff00579ed9c, 0xfffffff00579edb4,
    0xfffffff00579edd0, 0xfffffff00579fb64, 0xfffffff00579fb98, 0xfffffff00579fbcc, 0xfffffff00579fc00, 0xfffffff00579edec,
    0xfffffff00579efb0, 0xfffffff0057973b8, 0xfffffff00579ae68, 0xfffffff00579a2a8, 0xfffffff00579a2fc, 0xfffffff005795cd8,
    0xfffffff005795cf4, 0xfffffff00579a318, 0xfffffff00579a338, 0xfffffff00579a354, 0xfffffff00579a370, 0xfffffff00579a2c4,
    0xfffffff00579a2e0, 0xfffffff00579fc28, 0xfffffff00578b390, 0xfffffff00578b450, 0xfffffff00579fc48, 0xfffffff007a2adb8,
    0xfffffff007a2adb8, 0xfffffff00579fc5c, 0xfffffff00579fc74, 0xfffffff00579fc8c, 0xfffffff00579fcfc, 0xfffffff00579fd10,
    0xfffffff00579fd24, 0xfffffff00579fd38, 0xfffffff00579fd54, 0xfffffff00579fd68, 0xfffffff00579fd80, 0xfffffff00579fd9c,
    0xfffffff00579fdb8, 0xfffffff00579fdd0, 0xfffffff00579fde8, 0xfffffff00579fdfc, 0xfffffff00579fe14, 0xfffffff00579fe2c,
    0xfffffff005796a40, 0xfffffff0057969f8, 0xfffffff00579fe44, 0xfffffff00579fe60, 0xfffffff00579fe78, 0xfffffff00579fe90,
    0xfffffff00579fea8, 0xfffffff00579fecc, 0xfffffff00579fedc, 0xfffffff00579fef8, 0xfffffff00579ff08, 0xfffffff00579ff24,
    0xfffffff00579ff40, 0xfffffff00579ff5c, 0xfffffff00579ff78, 0xfffffff00579ff94, 0xfffffff00579ffac, 0xfffffff00579ffc4,
    0xfffffff00579ffdc, 0xfffffff00579fffc, 0xfffffff0057a001c, 0xfffffff0057a004c, 0xfffffff0057a007c, 0xfffffff00579fa18,
    0xfffffff00579278c, 0xfffffff0057927a4, 0xfffffff005794030, 0xfffffff0057927c8, 0xfffffff0057927e0, 0xfffffff0057927b4,
    0xfffffff0057a0090, 0xfffffff005795b88, 0xfffffff005795bb4, 0xfffffff005795bd0, 0xfffffff0057a00bc, 0xfffffff005793694,
    0xfffffff00578d87c, 0xfffffff00579ea5c, 0xfffffff0057a00d0, 0xfffffff0057985e8, 0xfffffff0057a00e4, 0xfffffff005794040,
    0xfffffff007a2adb8, 0xfffffff007a2adb8, 0xfffffff0057a00f4, 0xfffffff00579c5ac, 0xfffffff00579c9d8, 0xfffffff00579c6c0,
    0xfffffff00579cf6c, 0xfffffff00579d228, 0xfffffff00579d584, 0xfffffff00579d85c, 0xfffffff00579ea44, 0xfffffff00579af20,
    0xfffffff00579af40, 0xfffffff0057a0108, 0xfffffff0057a0124, 0xfffffff0057a0138, 0xfffffff005798af4, 0xfffffff0057a014c,
    0xfffffff0057a0164, 0xfffffff00579a13c, 0xfffffff0057a017c, 0xfffffff0057a0190, 0xfffffff0057a01b0, 0xfffffff0057a01c8,
    0xfffffff0057a01e4, 0xfffffff0057a0200, 0xfffffff0057a0220, 0xfffffff00578cca8, 0xfffffff00578b2e8, 0xfffffff00578d91c,
    0xfffffff0057a023c, 0xfffffff007a2adb8, 0xfffffff007a2adb8, 0xfffffff00579e150, 0xfffffff005789698, 0xfffffff0057a0254,
    0xfffffff0057a026c, 0xfffffff005796524, 0xfffffff005797224, 0xfffffff007a2adb8, 0xfffffff00578a428, 0xfffffff0057941f8,
    0xfffffff0057942c0, 0xfffffff005794318, 0xfffffff0057a0288, 0xfffffff0057a029c, 0xfffffff0057a02ac, 0xfffffff0057a02bc,
    0xfffffff0057970cc, 0xfffffff0057a02d4, 0xfffffff0057971f8, 0xfffffff00579720c,
]

IOSERVICE_OVERRIDES = {
    14: ('void free() override;', '__ZN25IOMobileFramebufferLegacy4freeEv'),
    21: ('bool init(OSDictionary *properties = nullptr) override;', '__ZN25IOMobileFramebufferLegacy4initEP12OSDictionary'),
    44: ('IOReturn setProperties(OSObject *properties) override;', '__ZN25IOMobileFramebufferLegacy13setPropertiesEP8OSObject'),
    79: ('void systemWillShutdown(IOOptionBits specifier) override;', '__ZN25IOMobileFramebufferLegacy18systemWillShutdownEj'),
    85: ('IOService *probe(IOService *provider, SInt32 *score) override;', '__ZN25IOMobileFramebufferLegacy5probeEP9IOServicePi'),
    86: ('bool start(IOService *provider) override;', '__ZN25IOMobileFramebufferLegacy5startEP9IOService'),
    87: ('void stop(IOService *provider) override;', '__ZN25IOMobileFramebufferLegacy4stopEP9IOService'),
    111: ('IOWorkLoop *getWorkLoop() const override;', '__ZNK25IOMobileFramebufferLegacy11getWorkLoopEv'),
    141: ('IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) override;', '__ZN25IOMobileFramebufferLegacy13newUserClientEP4taskPvjPP12IOUserClient'),
    155: ('IOReturn setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice) override;', '__ZN25IOMobileFramebufferLegacy13setPowerStateEmP9IOService'),
}

NAMED_SLOTS = {
    184: ('virtual bool isMainDisplay();', '__ZN25IOMobileFramebufferLegacy13isMainDisplayEv'),
    188: ('virtual IOReturn getDotPitch(uint32_t *pitch);', '__ZN25IOMobileFramebufferLegacy11getDotPitchEPj'),
    213: ('virtual IOReturn supportedFrameInfo(uint32_t, uint32_t *count, char *key, uint32_t *mask);', '__ZN25IOMobileFramebufferLegacy18supportedFrameInfoEjPjPcS0_'),
    249: ('virtual void getDisplayDimensions(uint32_t *width, uint32_t *height);', '__ZN25IOMobileFramebufferLegacy20getDisplayDimensionsEPjS0_'),
    288: ('virtual bool startHardware();', '__ZN25IOMobileFramebufferLegacy13startHardwareEv'),
    290: ('virtual void kickSwapQueue();', '__ZN25IOMobileFramebufferLegacy13kickSwapQueueEv'),
    295: ('virtual void completeCurrentSwap();', '__ZN25IOMobileFramebufferLegacy19completeCurrentSwapEv'),
    296: ('virtual bool validateSwap(const OriginalLegacySwapRequest *request);', '__ZN25IOMobileFramebufferLegacy12validateSwapEPK25OriginalLegacySwapRequest'),
    301: ('virtual bool getRefreshAttributes(uint64_t *words, bool fixed);', '__ZN25IOMobileFramebufferLegacy20getRefreshAttributesEPyb'),
    337: ('virtual IOReturn collectCompletionReports(uint32_t, void *, uint32_t *, uint32_t);', '__ZN25IOMobileFramebufferLegacy24collectCompletionReportsEjPvPjj'),
    345: ('virtual bool cancelSwapsAfterTimeout();', '__ZN25IOMobileFramebufferLegacy23cancelSwapsAfterTimeoutEv'),
    354: ('virtual int applyStateRequest(OriginalLegacyStateRequest *, bool);', '__ZN25IOMobileFramebufferLegacy17applyStateRequestEP26OriginalLegacyStateRequestb'),
}

PURE_LEAVES = [211, 245, 246, 306, 307, 338]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def find_llvm_tool(name: str) -> str:
    candidate = Path('/opt/homebrew/opt/llvm@22/bin') / name
    if candidate.is_file() and os.access(candidate, os.X_OK):
        return str(candidate)
    resolved = shutil.which(name)
    if not resolved:
        raise SystemExit(f'{name} not found (install Homebrew llvm@22)')
    return resolved


def replay_compile(record_path: Path, source: Path, obj: Path, extra_flags: list[str]) -> None:
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


def generate_abi_header(path: Path) -> None:
    lines = [
        '// SPDX-License-Identifier: BSD-2-Clause-Patent',
        '// Generated by build-sm6125-legacy-display.py for iPadOS 17.7.11 (21H461).',
        '#pragma once',
        '#include <IOKit/IOService.h>',
        '#include <IOKit/IOMemoryDescriptor.h>',
        '#include <IOKit/IOWorkLoop.h>',
        '#include <stdint.h>',
        '',
        'static constexpr unsigned kLegacyVsyncClients = 5592;',
        'static constexpr unsigned kRequestRect0 = 17;',
        'static constexpr unsigned kRequestRect1 = 81;',
        'static constexpr unsigned kRequestMasks = 792;',
        'static constexpr unsigned kRequestOptions = 804;',
        'static constexpr unsigned kRequestDims = 816;',
        'static constexpr unsigned kRequestSurfaces = 2624;',
        '',
        'struct OriginalLegacyStateRequest {',
        '    uint32_t skipRequested, requested, skipPowered, powered;',
        '    uint8_t changed;',
        '    uint8_t pad[11];',
        '};',
        'struct OriginalLegacySwapRequest;',
        '',
        '#define NATIVE_LEGACY_LEAVES(M) \\',
        '    ' + ' '.join(f'M({s})' for s in PURE_LEAVES),
        '',
        'class IOMobileFramebufferLegacy : public IOService {',
        '    OSDeclareAbstractStructors(IOMobileFramebufferLegacy)',
        '    uint8_t iomfb_legacy_storage_[0x18a0 - sizeof(IOService)];',
        'public:',
    ]
    for s in sorted(IOSERVICE_OVERRIDES):
        lines.append(f'    {IOSERVICE_OVERRIDES[s][0]}')
    for s in range(168, 358):
        if s in NAMED_SLOTS:
            lines.append(f'    {NAMED_SLOTS[s][0]}')
        else:
            lines.append(f'    virtual void opaque{s}();')
    lines += [
        '',
        '    void installBootDimensions(uint32_t width, uint32_t height);',
        '    const OriginalLegacySwapRequest *pendingSwapRequest() const;',
        '    bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request);',
        '};',
        'static_assert(sizeof(IOMobileFramebufferLegacy) == 0x18a0, "IOMobileFramebufferLegacy 21H461 size");',
        '',
    ]
    path.write_text('\n'.join(lines), encoding='utf-8')


def generate_support_cpp(path: Path) -> None:
    path.write_text("""// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include "OriginalLegacyFramebufferABI.h"

extern kmod_info_t g_kernel_kmod_info;

extern "C" {
void __GLOBAL__sub_I_NativeLegacyDisplay_cpp(void) __asm("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
IOService *native_legacy_display_allocate(void);
void __GLOBAL__sub_I_NativeKeyStoreEvents_cpp(void) __asm("__GLOBAL__sub_I_NativeKeyStoreEvents.cpp");
IOService *native_keyevents_allocate(void);
kern_return_t OSKextLoadKextWithIdentifier(const char *identifier);
}

static uint32_t g_legacy_stage = 0;
static uint32_t g_aks_stage = 0;
static IOService *g_pending_display = nullptr;
static IOService *g_legacy_display_instance = nullptr;
static IOService *g_legacy_display_provider = nullptr;
static IORegistryEntry *g_unit_scale_chosen = nullptr;
static bool g_unit_scale_added = false;
static bool g_unit_rotation_added = false;
extern "C" volatile uint32_t sm6125_disp_diag[10] = {};
extern "C" volatile uint32_t sm6125_aks_diag[4] = {};

static void finishUnitScalePolicy(bool keep) {
    if (g_unit_scale_chosen && !keep) {
        if (g_unit_scale_added) g_unit_scale_chosen->removeProperty("display-scale");
        if (g_unit_rotation_added) g_unit_scale_chosen->removeProperty("display-rotation");
    }
    OSSafeReleaseNULL(g_unit_scale_chosen);
    g_unit_scale_added = false;
    g_unit_rotation_added = false;
}

static bool publishOrMatchWord(IORegistryEntry *entry, const char *key, uint32_t value, bool &added) {
    OSObject *previous = entry->getProperty(key);
    if (previous) {
        OSData *data = OSDynamicCast(OSData, previous);
        uint32_t word = 0;
        if (!data || data->getLength() != sizeof(word)) return false;
        memcpy(&word, data->getBytesNoCopy(), sizeof(word));
        return word == value;
    }
    OSData *data = OSData::withBytes(&value, sizeof(value));
    bool result = data && entry->setProperty(key, data);
    OSSafeReleaseNULL(data);
    added = result;
    return result;
}

void IOMobileFramebufferLegacy::installBootDimensions(uint32_t width, uint32_t height) {
    uint8_t *self = reinterpret_cast<uint8_t *>(this);
    memcpy(self + 5808, &width, sizeof(width));
    memcpy(self + 5812, &height, sizeof(height));
}

const OriginalLegacySwapRequest *IOMobileFramebufferLegacy::pendingSwapRequest() const {
    const uint8_t *self = reinterpret_cast<const uint8_t *>(this);
    const OriginalLegacySwapRequest *head = nullptr;
    memcpy(&head, self + 5480, sizeof(head));
    return head;
}

bool IOMobileFramebufferLegacy::activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {
    auto self = reinterpret_cast<unsigned char *>(this);
    auto bytes = reinterpret_cast<const unsigned char *>(request);
    const void *current = nullptr, *head = nullptr, *tail = nullptr, *next = nullptr, *previous = nullptr;
    memcpy(&current, self + 5472, 8);
    memcpy(&head, self + 5480, 8);
    memcpy(&tail, self + 5488, 8);
    if (!request || current || head != request) return false;
    memcpy(&next, bytes + 2960, 8);
    memcpy(&previous, bytes + 2968, 8);
    if (previous != self + 5480) return false;
    if (next) {
        const void *link = self + 5480;
        memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next)) + 2968, &link, 8);
    } else if (tail != bytes + 2960) {
        return false;
    }
    memcpy(self + 5472, &request, 8);
    if (next) {
        memcpy(self + 5480, &next, 8);
    } else {
        const void *empty = nullptr, *emptyTail = self + 5480;
        memcpy(self + 5480, &empty, 8);
        memcpy(self + 5488, &emptyTail, 8);
    }
    return true;
}

extern "C" {

__attribute__((noreturn)) void native_legacy_backend_guard(unsigned slot, void *display, void *caller) {
    panic("NativeLegacyDisplay: unimplemented IOMobileFramebufferLegacy slot %u (self=%p caller=%p)", slot, display, caller);
}

bool native_legacy_gop_mapping_observed(IOService *display, IOMemoryMap *map, uint32_t width, uint32_t height) {
    IOLog("NativeLegacyDisplay: GOP mapped %ux%u va=%p pa=0x%llx len=0x%llx display=%p\\n",
          width, height,
          map ? reinterpret_cast<void *>(map->getVirtualAddress()) : nullptr,
          map ? static_cast<unsigned long long>(map->getPhysicalAddress()) : 0ULL,
          map ? static_cast<unsigned long long>(map->getLength()) : 0ULL,
          display);
    unsigned enabled = 0;
    PE_parse_boot_argn("offline-legacy-unit-scale", &enabled, sizeof(enabled));
    if (!enabled) return true;
    unsigned phase = 0;
    PE_parse_boot_argn("offline-legacy-phase", &phase, sizeof(phase));
    PE_Video video = {};
    if (enabled < 1 || enabled > 3 || phase != 4 || g_unit_scale_chosen ||
        PE_current_console(&video) || video.v_width != width || video.v_height != height ||
        video.v_rotate || !map || map->getPhysicalAddress() != video.v_baseAddr) {
        return false;
    }
    g_unit_scale_chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (!g_unit_scale_chosen ||
        !publishOrMatchWord(g_unit_scale_chosen, "display-scale", enabled, g_unit_scale_added) ||
        !publishOrMatchWord(g_unit_scale_chosen, "display-rotation", video.v_rotate, g_unit_rotation_added)) {
        finishUnitScalePolicy(false);
        return false;
    }
    return true;
}

void native_legacy_registration_observed(IOService *display) {
    sm6125_disp_diag[0] = 7;
    IOLog("NativeLegacyDisplay: registered IOMobileFramebufferLegacy service %p\\n", display);
}

void native_legacy_swap_stage(unsigned) {}
void native_legacy_client_enter(task_t, uint32_t) {
    __atomic_fetch_add(&sm6125_disp_diag[1], 1u, __ATOMIC_RELAXED);
}
void native_legacy_client_observed(IOReturn status, IOUserClient *client) {
    sm6125_disp_diag[2] = static_cast<uint32_t>(status);
    if (status != kIOReturnSuccess) {
        IOLog("NativeLegacyDisplay: newUserClient failed status=0x%x client=%p\\n", status, client);
    }
}
void native_legacy_pm_observed(unsigned step, IOReturn status, unsigned long ordinal) {
    if (status != kIOReturnSuccess) {
        IOLog("NativeLegacyDisplay: PM step=%u status=0x%x ordinal=%lu\\n", step, status, ordinal);
    }
}

void native_legacy_frame_observed(unsigned, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {}
void native_legacy_frame_colors_observed(const uint32_t *, const uint64_t *, uint64_t) {}
void native_legacy_swap_validation_observed(bool accepted, unsigned reason, const void *, const void *surface) {
    if (accepted) {
        __atomic_fetch_add(&sm6125_disp_diag[3], 1u, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&sm6125_disp_diag[4], 1u, __ATOMIC_RELAXED);
        sm6125_disp_diag[5] = reason;
    }
    static uint32_t rejects = 0;
    if (!accepted && rejects < 8) {
        ++rejects;
        IOLog("NativeLegacyDisplay: swap rejected reason=%u surface=%p\\n", reason, surface);
    }
}
void native_legacy_surface_rejected(uint64_t w, uint64_t h, uint32_t fmt, uint32_t planes, uint64_t stride, uint64_t alloc, uint64_t len) {
    static uint32_t logs = 0;
    if (logs < 8) {
        ++logs;
        IOLog("NativeLegacyDisplay: surface reject %llux%llu fmt=0x%x planes=%u stride=%llu alloc=%llu len=%llu\\n",
              w, h, fmt, planes, stride, alloc, len);
    }
}
bool native_legacy_prepare_scanout(void) { return true; }
void native_legacy_transfer_observed(IOReturn status, const void *, uint64_t bytes) {
    __atomic_fetch_add(&sm6125_disp_diag[6], 1u, __ATOMIC_RELAXED);
    sm6125_disp_diag[7] = static_cast<uint32_t>(status);
    static uint32_t errs = 0;
    if (status != kIOReturnSuccess && errs < 8) {
        ++errs;
        IOLog("NativeLegacyDisplay: transfer failed status=0x%x bytes=%llu\\n", status, bytes);
    }
}
void native_legacy_copy_timing_observed(uint64_t, uint64_t) {}
void native_legacy_retirement_observed(IOService *) {
    __atomic_fetch_add(&sm6125_disp_diag[8], 1u, __ATOMIC_RELAXED);
}
bool native_legacy_gpu_offer(void *, IOMemoryDescriptor *, uint64_t, uint32_t, uint32_t, uint32_t) { return false; }
void native_legacy_gpu_fallback_observed(int) {}
void native_legacy_vsync_observed(unsigned ev, unsigned) {
    if (ev == 1) __atomic_fetch_add(&sm6125_disp_diag[9], 1u, __ATOMIC_RELAXED);
}

__attribute__((weak)) bool native_adreno_fences(uint32_t *, uint32_t *) { return false; }
__attribute__((weak)) bool native_adreno_fences_detail(uint32_t *, uint32_t *, uint32_t *) { return false; }
__attribute__((weak)) bool native_gpu_trace_enabled(void) { return false; }
__attribute__((weak)) void native_gpu_trace_record(uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {}
__attribute__((weak)) uint32_t native_ufs_dpu_map(unsigned, const uint64_t *, unsigned) { return 0; }
__attribute__((weak)) volatile uint32_t native_ufs_dpu_state = 0;

void native_keyevents_open_refused(void) {
    __atomic_fetch_add(&sm6125_aks_diag[3], 1u, __ATOMIC_RELAXED);
}

bool native_aks_state_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        unsigned value = 0;
        cached = PE_parse_boot_argn("aks", &value, sizeof(value)) && value == 1;
    }
    return cached == 1;
}

uint32_t native_aks_lock_state(void) {
    static int cached = -1;
    if (cached < 0) {
        unsigned value = 0;
        if (!PE_parse_boot_argn("aks-sls", &value, sizeof(value)) || value > 7) value = 0;
        cached = static_cast<int>(value);
    }
    return static_cast<uint32_t>(cached);
}

size_t native_aks_device_id(uint8_t *out, size_t capacity) {
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    OSData *id = chosen ? OSDynamicCast(OSData, chosen->getProperty("unique-chip-id")) : nullptr;
    size_t n = 0;
    if (id && id->getLength() >= 8 && id->getLength() <= capacity) {
        const uint8_t *bytes = static_cast<const uint8_t *>(id->getBytesNoCopy());
        bool nonzero = false;
        for (unsigned i = 0; i < id->getLength(); ++i) {
            out[i] = bytes[i];
            nonzero |= (bytes[i] != 0);
        }
        if (nonzero) n = id->getLength();
    }
    OSSafeReleaseNULL(chosen);
    return n;
}

void native_aks_opened(void) {
    __atomic_fetch_add(&sm6125_aks_diag[1], 1u, __ATOMIC_RELAXED);
}

void native_aks_call(uint32_t selector, int32_t result) {
    __atomic_fetch_add(&sm6125_aks_diag[2], 1u, __ATOMIC_RELAXED);
    static uint32_t errs = 0;
    if (result != kIOReturnSuccess && errs < 8) {
        ++errs;
        IOLog("AppleKeyStoreState: selector=%u failed rc=0x%x\\n", selector, result);
    }
}

static void native_keyevents_probe(void) {
    unsigned enabled = 0, original = 0;
    if (!PE_parse_boot_argn("offline-key-events", &enabled, sizeof(enabled)) || enabled != 1 || g_aks_stage) {
        return;
    }
    g_aks_stage = 1;
    sm6125_aks_diag[0] = 1;
    PE_parse_boot_argn("offline-keystore", &original, sizeof(original));
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (!chosen) {
        sm6125_aks_diag[0] = 91;
        return;
    }
    OSData *data = OSDynamicCast(OSData, chosen->getProperty("ephemeral-storage"));
    uint32_t ephemeral = 0;
    if (data && data->getLength() == sizeof(ephemeral)) {
        memcpy(&ephemeral, data->getBytesNoCopy(), sizeof(ephemeral));
    }
    OSSafeReleaseNULL(chosen);
    if ((ephemeral != 1 && !native_aks_state_enabled()) || original) {
        sm6125_aks_diag[0] = 92;
        return;
    }
    const OSSymbol *name = OSSymbol::withCString("AppleKeyStore");
    const bool absent = name && !OSMetaClass::getMetaClassWithName(name);
    OSSafeReleaseNULL(name);
    if (!absent) {
        sm6125_aks_diag[0] = 93;
        return;
    }
    IOService *provider = IOService::getResourceService();
    if (!provider) {
        sm6125_aks_diag[0] = 94;
        return;
    }
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        sm6125_aks_diag[0] = 95;
        return;
    }
    __GLOBAL__sub_I_NativeKeyStoreEvents_cpp();
    if (OSMetaClass::postModLoad(handle) != kOSReturnSuccess) {
        sm6125_aks_diag[0] = 95;
        return;
    }
    sm6125_aks_diag[0] = 2;
    IOService *service = native_keyevents_allocate();
    const bool initialized = service && service->init();
    const bool attached = initialized && service->attach(provider);
    const bool started = attached && service->start(provider);
    if (!started) {
        if (attached) service->detach(provider);
        OSSafeReleaseNULL(service);
        sm6125_aks_diag[0] = 96;
        IOLog("AppleKeyStoreState: failed to start service\\n");
        return;
    }
    sm6125_aks_diag[0] = 3;
    IOLog("AppleKeyStoreState: started service=%p lock_state=%u\\n", service, native_aks_lock_state());
}

void native_legacy_display_probe(void) {
    if (g_legacy_stage) return;
    g_legacy_stage = 1;
    sm6125_disp_diag[0] = 1;
    void *meta_handle = OSMetaClass::preModLoad("com.apple.iokit.IOMobileGraphicsFamily");
    if (!meta_handle) {
        IOLog("NativeLegacyDisplay: OSMetaClass::preModLoad failed\\n");
        return;
    }
    __GLOBAL__sub_I_NativeLegacyDisplay_cpp();
    const OSReturn post_rc = OSMetaClass::postModLoad(meta_handle);
    if (post_rc != kOSReturnSuccess) {
        sm6125_disp_diag[5] = static_cast<uint32_t>(post_rc);
        IOLog("NativeLegacyDisplay: OSMetaClass::postModLoad failed (%d)\\n", post_rc);
        return;
    }
    sm6125_disp_diag[0] = 2;

    PE_Video video = {};
    if (PE_current_console(&video) || !video.v_baseAddr || !video.v_width || !video.v_height ||
        video.v_depth != 32 || video.v_rotate || video.v_rowBytes < video.v_width * 4) {
        IOLog("NativeLegacyDisplay: no boot console video available\\n");
        return;
    }

    IOService *display = native_legacy_display_allocate();
    if (!display || !display->init()) {
        OSSafeReleaseNULL(display);
        IOLog("NativeLegacyDisplay: allocate/init failed\\n");
        return;
    }
    sm6125_disp_diag[0] = 3;
    g_pending_display = display;
}

void native_legacy_display_late_probe(void) {
    native_keyevents_probe();
    if (!g_pending_display && !g_legacy_display_instance) {
        native_legacy_display_probe();
    }
    IOService *display = g_pending_display;
    if (!display) return;
    g_pending_display = nullptr;

    const kern_return_t ks = OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface");
    if (ks != KERN_SUCCESS) {
        sm6125_disp_diag[0] = 91;
        sm6125_disp_diag[5] = static_cast<uint32_t>(ks);
        IOLog("NativeLegacyDisplay: IOSurface kext load failed (%d)\\n", ks);
        display->release();
        return;
    }

    OSDictionary *matching = OSDictionary::withCapacity(1);
    {
        const OSObject *names[] = {
            OSString::withCString("IOCoreSurfaceRoot"),
            OSString::withCString("IOSurfaceRoot"),
        };
        OSArray *list = (names[0] && names[1]) ? OSArray::withObjects(names, 2) : nullptr;
        if (!matching || !list || !matching->setObject(gIONameMatchKey, list)) {
            OSSafeReleaseNULL(matching);
        }
        OSSafeReleaseNULL(list);
        for (const OSObject *name : names) {
            if (name) name->release();
        }
    }
    IOService *surface = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    OSSafeReleaseNULL(matching);
    if (!surface) {
        sm6125_disp_diag[0] = 92;
        IOLog("NativeLegacyDisplay: timed out waiting for IOSurfaceRoot\\n");
        display->release();
        return;
    }
    OSSafeReleaseNULL(surface);

    PE_Video video = {};
    if (PE_current_console(&video) || !video.v_baseAddr || !video.v_width || !video.v_height ||
        video.v_depth != 32 || video.v_rotate || video.v_width > 16384 || video.v_height > 16384 ||
        video.v_rowBytes < video.v_width * 4 || video.v_rowBytes > UINT64_MAX / video.v_height) {
        display->release();
        return;
    }

    IOService *nub = new IOService;
    IODeviceMemory *memory = IODeviceMemory::withRange(video.v_baseAddr, video.v_rowBytes * video.v_height);
    OSArray *ranges = OSArray::withCapacity(1);
    IOService *parent = IOService::getResourceService();
    IORegistryEntry *displayTreeParent = nullptr;
    bool nubAttached = false, nubStarted = false, dtAttached = false, attached = false, started = false;

    if (nub && memory && ranges && parent && nub->init()) {
        ranges->setObject(memory);
        nub->setDeviceMemory(ranges);
        nub->setName("NativeGOPFramebuffer");
        nubAttached = nub->attach(parent);
        if (nubAttached) nubStarted = nub->start(parent);
        if (nubStarted) {
            displayTreeParent = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
            const char type[] = "display-subsystem";
            OSData *data = OSData::withBytes(type, sizeof(type));
            if (displayTreeParent && data && nub->setProperty("device_type", data)) {
                nub->setName("gop-display", gIODTPlane);
                dtAttached = nub->attachToParent(displayTreeParent, gIODTPlane);
            }
            OSSafeReleaseNULL(data);
            if (!dtAttached) {
                sm6125_disp_diag[0] = 93;
                IOLog("NativeLegacyDisplay: failed to attach display-subsystem under /arm-io\\n");
            } else {
                sm6125_disp_diag[0] = 4;
                SInt32 score = 0;
                const bool probed = (display->probe(nub, &score) == display);
                if (!probed) {
                    sm6125_disp_diag[0] = 94;
                    IOLog("NativeLegacyDisplay: probe rejected nub\\n");
                } else {
                    attached = display->attach(nub);
                    if (!attached) {
                        sm6125_disp_diag[0] = 95;
                        IOLog("NativeLegacyDisplay: attach failed\\n");
                    } else {
                        sm6125_disp_diag[0] = 5;
                        started = display->start(nub);
                        if (started) {
                            g_legacy_display_instance = display;
                            g_legacy_display_provider = nub;
                            sm6125_disp_diag[0] = 10;
                            finishUnitScalePolicy(true);
                            OSSafeReleaseNULL(displayTreeParent);
                            OSSafeReleaseNULL(ranges);
                            OSSafeReleaseNULL(memory);
                            IOLog("NativeLegacyDisplay: started %lux%lu @ 0x%lx (display-subsystem)\\n",
                                  video.v_width, video.v_height, video.v_baseAddr);
                            return;
                        }
                        sm6125_disp_diag[0] = 99;
                        IOLog("NativeLegacyDisplay: start failed\\n");
                    }
                }
            }
        }
    }

    finishUnitScalePolicy(false);
    if (started) display->stop(nub);
    if (attached) display->detach(nub);
    if (dtAttached && displayTreeParent) nub->detachFromParent(displayTreeParent, gIODTPlane);
    if (nubStarted) nub->stop(parent);
    if (nubAttached) nub->detach(parent);
    OSSafeReleaseNULL(displayTreeParent);
    OSSafeReleaseNULL(ranges);
    OSSafeReleaseNULL(memory);
    OSSafeReleaseNULL(nub);
    display->release();
}

}  // extern "C"
""", encoding='utf-8')


def generate_bindings_asm(path: Path) -> None:
    lines = [
        '.text',
        '.globl __ZTV25IOMobileFramebufferLegacy',
        '.set __ZTV25IOMobileFramebufferLegacy, 0xfffffff005f38560',
        '.globl __ZN25IOMobileFramebufferLegacy10gMetaClassE',
        '.set __ZN25IOMobileFramebufferLegacy10gMetaClassE, 0xfffffff006b35940',
        '.globl __ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass',
        '.set __ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass, 0xfffffff0057865cc',
        '.globl __ZN25IOMobileFramebufferLegacyD2Ev',
        '.set __ZN25IOMobileFramebufferLegacyD2Ev, 0xfffffff005786618',
        '.globl _native_legacy_surface_descriptor',
        '.set _native_legacy_surface_descriptor, 0xfffffff004b9ead0',
        '.globl _native_legacy_surface_width',
        '.set _native_legacy_surface_width, 0xfffffff004b9ebbc',
        '.globl _native_legacy_surface_height',
        '.set _native_legacy_surface_height, 0xfffffff004b9ebc4',
        '.globl _native_legacy_surface_stride',
        '.set _native_legacy_surface_stride, 0xfffffff004b9ebd4',
        '.globl _native_legacy_surface_allocation',
        '.set _native_legacy_surface_allocation, 0xfffffff004b9ebfc',
        '.globl _native_legacy_surface_format',
        '.set _native_legacy_surface_format, 0xfffffff004b9ebac',
        '.globl _native_legacy_surface_planes',
        '.set _native_legacy_surface_planes, 0xfffffff004b9ebf4',
        '.globl _native_legacy_vsync_notify',
        '.set _native_legacy_vsync_notify, 0xfffffff005796720',
        '.globl _native_legacy_set_refresh_properties',
        '.set _native_legacy_set_refresh_properties, 0xfffffff00579ebb8',
    ]
    for s, (_, sym) in sorted(IOSERVICE_OVERRIDES.items()):
        lines.append(f'.globl {sym}')
        lines.append(f'.set {sym}, {LEGACY_VTABLE[s]:#x}')
    for s in range(168, 358):
        if s in NAMED_SLOTS:
            sym = NAMED_SLOTS[s][1]
        else:
            sym = f'__ZN25IOMobileFramebufferLegacy9opaque{s}Ev'
        lines.append(f'.globl {sym}')
        lines.append(f'.set {sym}, {LEGACY_VTABLE[s]:#x}')
    path.write_text('\n'.join(lines) + '\n', encoding='utf-8')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, default=ROOT)
    args = parser.parse_args()
    tree = args.tree.resolve()

    out = tree / 'build/native-legacy-display'
    out.mkdir(parents=True, exist_ok=True)
    src = tree / 'drivers/GopFramebuffer/NativeLegacyDisplay.cpp'
    if not src.is_file():
        raise SystemExit(f'Missing GopFramebuffer source: {src}')

    abi_h = out / 'OriginalLegacyFramebufferABI.h'
    support_cpp = out / 'NativeLegacyDisplaySupport.cpp'
    bindings_s = out / 'original-bindings.S'
    generate_abi_header(abi_h)
    generate_support_cpp(support_cpp)
    generate_bindings_asm(bindings_s)

    record_path = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    if not record_path.is_file():
        raise SystemExit(f'Missing kernel compile record: {record_path}')

    common_flags = [
        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
        '-Wno-c99-designator', '-Wno-reorder-init-list',
        '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier',
        '-I', str(tree / '.cache/xnu-work/osfmk'),
        '-I', str(out),
        '-I', str(src.parent),
        '-DNATIVE_LEGACY_CANCEL_SWAPS=1',
    ]

    obj = out / 'NativeLegacyDisplay.o'
    replay_compile(record_path, src, obj, common_flags)
    check_object(obj.read_bytes(), 'NativeLegacyDisplay.o')

    llvm_objcopy = find_llvm_tool('llvm-objcopy')
    late_obj = out / 'NativeLegacyDisplay.late.o'
    subprocess.check_call([
        llvm_objcopy,
        '--remove-section=__DATA,__mod_init_func',
        '--globalize-symbol=__GLOBAL__sub_I_NativeLegacyDisplay.cpp',
        str(obj),
        str(late_obj),
    ], timeout=60)

    support_obj = out / 'NativeLegacyDisplaySupport.o'
    replay_compile(record_path, support_cpp, support_obj, common_flags)
    check_object(support_obj.read_bytes(), 'NativeLegacyDisplaySupport.o')

    aks_src = tree / 'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp'
    if not aks_src.is_file():
        raise SystemExit(f'Missing AppleKeyStoreState source: {aks_src}')
    aks_obj = out / 'NativeKeyStoreEvents.o'
    replay_compile(record_path, aks_src, aks_obj, common_flags)
    check_object(aks_obj.read_bytes(), 'NativeKeyStoreEvents.o')

    aks_late_obj = out / 'NativeKeyStoreEvents.late.o'
    subprocess.check_call([
        llvm_objcopy,
        '--remove-section=__DATA,__mod_init_func',
        '--globalize-symbol=__GLOBAL__sub_I_NativeKeyStoreEvents.cpp',
        str(aks_obj),
        str(aks_late_obj),
    ], timeout=60)

    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    bindings_obj = out / 'original-bindings.o'
    subprocess.check_call([
        'xcrun', 'clang', '-target', TRIPLE, '-isysroot', sdk,
        '-c', str(bindings_s), '-o', str(bindings_obj),
    ], timeout=60)

    archive = out / 'libNativeLegacyDisplay.late.a'
    archive.unlink(missing_ok=True)
    subprocess.check_call([
        'xcrun', 'ar', 'rcs', str(archive),
        str(late_obj), str(aks_late_obj), str(support_obj), str(bindings_obj),
    ], timeout=60)

    report = {
        'source_sha256': {
            str(src.relative_to(tree)): sha256(src),
            str(aks_src.relative_to(tree)): sha256(aks_src),
            str((src.parent / 'NativeLegacyRequest.h').relative_to(tree)): sha256(src.parent / 'NativeLegacyRequest.h'),
            str((src.parent / 'NativeHalfCodes.h').relative_to(tree)): sha256(src.parent / 'NativeHalfCodes.h'),
            str((src.parent / 'NativeGOPWire.h').relative_to(tree)): sha256(src.parent / 'NativeGOPWire.h'),
            str((src.parent / 'NativeGpuTrace.h').relative_to(tree)): sha256(src.parent / 'NativeGpuTrace.h'),
        },
        'generated_abi_sha256': sha256(abi_h),
        'compiled_object_sha256': sha256(obj),
        'late_object_sha256': sha256(late_obj),
        'aks_object_sha256': sha256(aks_obj),
        'aks_late_object_sha256': sha256(aks_late_obj),
        'archive_sha256': sha256(archive),
        'virtual_entries': len(LEGACY_VTABLE),
    }
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'PASS: built {archive} (ARMv8.0-A clean, 358-slot 21H461 ABI)')


if __name__ == '__main__':
    main()
