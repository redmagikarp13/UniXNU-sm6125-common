// SPDX-License-Identifier: MIT
// Sonda GCC SDCC1 + SDHCI-MSM v5 (SM6125): modos 1 (RO), 2 (ident+GPT PIO), 3 (HS50/ADMA2/DDR52).
#include "Sm6125EmmcCore.h"
#include "Sm6125EmmcReadOnly.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IORegistryEntry.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <libkern/c++/OSData.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

extern "C" {

__attribute__((noinline)) uint8_t sm6125_mmio_read8(uintptr_t base, uint32_t offset)
{
  uint32_t value = 0;
  const uintptr_t addr = base + offset;
  __asm__ volatile("ldrb %w0, [%1]" : "=r"(value) : "r"(addr) : "memory");
  return static_cast<uint8_t>(value);
}

__attribute__((noinline)) uint16_t sm6125_mmio_read16(uintptr_t base, uint32_t offset)
{
  uint32_t value = 0;
  const uintptr_t addr = base + offset;
  __asm__ volatile("ldrh %w0, [%1]" : "=r"(value) : "r"(addr) : "memory");
  return static_cast<uint16_t>(value);
}

__attribute__((noinline)) uint32_t sm6125_mmio_read32(uintptr_t base, uint32_t offset)
{
  uint32_t value = 0;
  const uintptr_t addr = base + offset;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(addr) : "memory");
  return value;
}

__attribute__((noinline)) void sm6125_mmio_write8(uintptr_t base, uint32_t offset, uint8_t value)
{
  const uintptr_t addr = base + offset;
  __asm__ volatile("strb %w0, [%1]" : : "r"(static_cast<uint32_t>(value)), "r"(addr) : "memory");
}

__attribute__((noinline)) void sm6125_mmio_write16(uintptr_t base, uint32_t offset, uint16_t value)
{
  const uintptr_t addr = base + offset;
  __asm__ volatile("strh %w0, [%1]" : : "r"(static_cast<uint32_t>(value)), "r"(addr) : "memory");
}

__attribute__((noinline)) void sm6125_mmio_write32(uintptr_t base, uint32_t offset, uint32_t value)
{
  const uintptr_t addr = base + offset;
  __asm__ volatile("str %w0, [%1]" : : "r"(value), "r"(addr) : "memory");
}

static void sm6125_emmc_delay_us(uint32_t microseconds)
{
  IODelay(microseconds);
}

static uint64_t sm6125_emmc_now_us(void)
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000ULL;
}

}  // extern "C"

static const sm6125_mmio_ro_ops_t kReadOnlyMmioOps = {
    .read8 = sm6125_mmio_read8,
    .read16 = sm6125_mmio_read16,
    .read32 = sm6125_mmio_read32,
};

static const sm6125_emmc_hw_ops_t kIdentMmioOps = {
    .read8 = sm6125_mmio_read8,
    .read16 = sm6125_mmio_read16,
    .read32 = sm6125_mmio_read32,
    .write8 = sm6125_mmio_write8,
    .write16 = sm6125_mmio_write16,
    .write32 = sm6125_mmio_write32,
    .delay_us = sm6125_emmc_delay_us,
    .now_us = sm6125_emmc_now_us,
};

struct Sm6125DmaCacheCtx {
  IOBufferMemoryDescriptor *desc_md;
  IOBufferMemoryDescriptor *data_md;
};

static void dma_cache_clean_desc(void *ctx, size_t offset, size_t len)
{
  auto *c = static_cast<Sm6125DmaCacheCtx *>(ctx);
  if (c && c->desc_md) {
    c->desc_md->performOperation(kIOMemoryIncoherentIOStore, offset, len);
    __asm__ volatile("dsb sy" : : : "memory");
  }
}

static void dma_cache_flush_data(void *ctx, size_t offset, size_t len)
{
  auto *c = static_cast<Sm6125DmaCacheCtx *>(ctx);
  if (c && c->data_md) {
    c->data_md->performOperation(kIOMemoryIncoherentIOFlush, offset, len);
    __asm__ volatile("dsb sy" : : : "memory");
  }
}

static void dma_cache_invalidate_data(void *ctx, size_t offset, size_t len)
{
  auto *c = static_cast<Sm6125DmaCacheCtx *>(ctx);
  if (c && c->data_md) {
    c->data_md->performOperation(kIOMemoryIncoherentIOFlush, offset, len);
    __asm__ volatile("dsb sy" : : : "memory");
  }
}

__attribute__((noreturn)) static void panic_with_snapshot(const sm6125_emmc_ro_snapshot_t *snap)
{
  char buf[640];
  sm6125_emmc_ro_format_report(snap, buf, sizeof(buf));
  panic("%s", buf);
}

static bool dt_compatible_has(IORegistryEntry *entry, const char *target)
{
  OSData *compat = OSDynamicCast(OSData, entry->getProperty("compatible"));
  if (!compat || !compat->getLength()) {
    return false;
  }
  const char *bytes = static_cast<const char *>(compat->getBytesNoCopy());
  const unsigned int len = compat->getLength();
  const size_t target_len = strlen(target);
  unsigned int pos = 0;
  while (pos < len) {
    unsigned int end = pos;
    while (end < len && bytes[end] != '\0') {
      end++;
    }
    if (end < len && (end - pos) == target_len && memcmp(bytes + pos, target, target_len) == 0) {
      return true;
    }
    pos = end + 1;
  }
  return false;
}

static bool read_dt_geometry(uint64_t *out_gcc_pa, uint64_t *out_sdhci_pa)
{
  IORegistryEntry *soc = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
  IORegistryEntry *gcc = IORegistryEntry::fromPath("/arm-io/gcc", gIODTPlane);
  IORegistryEntry *sdhci = IORegistryEntry::fromPath("/arm-io/sdhci1", gIODTPlane);
  bool ok = false;

  if (soc && gcc && sdhci &&
      (dt_compatible_has(gcc, "qcom,gcc-sm6125") || dt_compatible_has(gcc, "qcom,gcc-trinket")) &&
      dt_compatible_has(sdhci, "qcom,sdhci-msm-v5")) {
    OSData *ranges = OSDynamicCast(OSData, soc->getProperty("ranges"));
    OSData *gcc_reg = OSDynamicCast(OSData, gcc->getProperty("reg"));
    OSData *sdhci_reg = OSDynamicCast(OSData, sdhci->getProperty("reg"));
    uint64_t bus[3] = {0, 0, 0};
    uint64_t gcc_pair[2] = {0, 0};
    uint64_t sdhci_pair[2] = {0, 0};
    if (ranges && ranges->getLength() == sizeof(bus) &&
        gcc_reg && gcc_reg->getLength() >= sizeof(gcc_pair) &&
        sdhci_reg && sdhci_reg->getLength() >= sizeof(sdhci_pair)) {
      memcpy(bus, ranges->getBytesNoCopy(), sizeof(bus));
      memcpy(gcc_pair, gcc_reg->getBytesNoCopy(), sizeof(gcc_pair));
      memcpy(sdhci_pair, sdhci_reg->getBytesNoCopy(), sizeof(sdhci_pair));
      ok = sm6125_emmc_translate_region(
               bus, gcc_pair[0], gcc_pair[1], SM6125_GCC_MIN_REGION_SIZE, out_gcc_pa) &&
           sm6125_emmc_translate_region(
               bus, sdhci_pair[0], sdhci_pair[1], SM6125_SDHCI_MIN_REGION_SIZE, out_sdhci_pa);
    }
  }

  OSSafeReleaseNULL(sdhci);
  OSSafeReleaseNULL(gcc);
  OSSafeReleaseNULL(soc);
  return ok;
}

static bool map_mmio_window(
    uint64_t phys_addr,
    uint64_t length,
    IOMemoryDescriptor **out_desc,
    IOMemoryMap **out_map)
{
  IOMemoryDescriptor *desc =
      IOMemoryDescriptor::withPhysicalAddress(phys_addr, length, kIODirectionInOut);
  if (!desc) {
    return false;
  }
  IOMemoryMap *map = desc->map(kIOMapAnywhere | kIOMapInhibitCache);
  if (!map || !map->getVirtualAddress()) {
    OSSafeReleaseNULL(map);
    OSSafeReleaseNULL(desc);
    return false;
  }
  *out_desc = desc;
  *out_map = map;
  return true;
}

static bool alloc_dma_buf(
    IOBufferMemoryDescriptor **out_desc_md,
    IOBufferMemoryDescriptor **out_data_md,
    Sm6125DmaCacheCtx *ctx,
    sm6125_emmc_dma_buf_t *out_dma)
{
  constexpr size_t kDescBytes = 4096u;
  constexpr size_t kXferBytes = 32768u;
  constexpr size_t kCanaryBytes = 4096u;
  constexpr size_t kDataTotal = kXferBytes + kCanaryBytes;
  constexpr mach_vm_address_t kLow4GMask = 0x00000000FFFFF000ULL;
  constexpr IOOptionBits kOpts =
      static_cast<IOOptionBits>(kIODirectionInOut) |
      static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);

  IOBufferMemoryDescriptor *desc_md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task, kOpts, kDescBytes, kLow4GMask);
  IOBufferMemoryDescriptor *data_md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task, kOpts, kDataTotal, kLow4GMask);
  if (!desc_md || !data_md) {
    OSSafeReleaseNULL(data_md);
    OSSafeReleaseNULL(desc_md);
    return false;
  }
  if (desc_md->prepare(kIODirectionInOut) != kIOReturnSuccess) {
    OSSafeReleaseNULL(data_md);
    OSSafeReleaseNULL(desc_md);
    return false;
  }
  if (data_md->prepare(kIODirectionInOut) != kIOReturnSuccess) {
    desc_md->complete(kIODirectionInOut);
    OSSafeReleaseNULL(data_md);
    OSSafeReleaseNULL(desc_md);
    return false;
  }

  IOByteCount desc_seg = 0;
  IOByteCount data_seg = 0;
  const addr64_t desc_pa = desc_md->getPhysicalSegment(0, &desc_seg, kIOMemoryMapperNone);
  const addr64_t data_pa = data_md->getPhysicalSegment(0, &data_seg, kIOMemoryMapperNone);
  auto *desc_va = static_cast<uint8_t *>(desc_md->getBytesNoCopy());
  auto *data_va = static_cast<uint8_t *>(data_md->getBytesNoCopy());
  if (!desc_va || !data_va || !desc_pa || !data_pa ||
      desc_seg < kDescBytes || data_seg < kDataTotal ||
      desc_pa > (0xFFFFFFFFULL - kDescBytes + 1ULL) ||
      data_pa > (0xFFFFFFFFULL - kDataTotal + 1ULL)) {
    data_md->complete(kIODirectionInOut);
    desc_md->complete(kIODirectionInOut);
    OSSafeReleaseNULL(data_md);
    OSSafeReleaseNULL(desc_md);
    return false;
  }

  ctx->desc_md = desc_md;
  ctx->data_md = data_md;
  memset(out_dma, 0, sizeof(*out_dma));
  out_dma->desc_va = desc_va;
  out_dma->desc_pa = desc_pa;
  out_dma->desc_size = kDescBytes;
  out_dma->data_va = data_va;
  out_dma->data_pa = data_pa;
  out_dma->data_size = kDataTotal;
  out_dma->canary_offset = kXferBytes;
  out_dma->canary_size = kCanaryBytes;
  out_dma->cache_clean_desc = dma_cache_clean_desc;
  out_dma->cache_flush_data = dma_cache_flush_data;
  out_dma->cache_invalidate_data = dma_cache_invalidate_data;
  out_dma->cache_ctx = ctx;

  *out_desc_md = desc_md;
  *out_data_md = data_md;
  return true;
}

extern "C" void sm6125_emmc_probe_run(void)
{
  uint32_t arg_val = 0;
  const bool has_arg = PE_parse_boot_argn("sm6125-emmc-probe", &arg_val, sizeof(arg_val));
  const uint32_t mode = sm6125_emmc_probe_select_mode(has_arg, arg_val);
  if (mode == SM6125_EMMC_PROBE_MODE_OFF) {
    return;
  }

  sm6125_emmc_ro_snapshot_t snap;
  memset(&snap, 0, sizeof(snap));

  if (mode == SM6125_EMMC_PROBE_MODE_INVALID) {
    snap.err = SM6125_EMMC_RO_ERR_INVALID_MODE;
    panic_with_snapshot(&snap);
  }

  if (!read_dt_geometry(&snap.gcc_pa, &snap.sdhci_pa) ||
      snap.gcc_pa > (UINT64_MAX - SM6125_GCC_SDCC1_PAGE_OFFSET - SM6125_GCC_SDCC1_WINDOW_SIZE)) {
    snap.err = SM6125_EMMC_RO_ERR_ADT_INVALID;
    panic_with_snapshot(&snap);
  }

  if (mode == SM6125_EMMC_PROBE_MODE_IDENT_GPT || mode == SM6125_EMMC_PROBE_MODE_FAST_BENCH) {
    IOMemoryDescriptor *gcc_desc = nullptr;
    IOMemoryMap *gcc_map = nullptr;
    IOMemoryDescriptor *sdhci_desc = nullptr;
    IOMemoryMap *sdhci_map = nullptr;
    if (!map_mmio_window(
            snap.gcc_pa + SM6125_GCC_SDCC1_PAGE_OFFSET,
            SM6125_GCC_SDCC1_WINDOW_SIZE,
            &gcc_desc,
            &gcc_map)) {
      snap.err = SM6125_EMMC_RO_ERR_MAP_GCC;
      panic_with_snapshot(&snap);
    }
    if (!map_mmio_window(snap.sdhci_pa, SM6125_SDHCI_MIN_REGION_SIZE, &sdhci_desc, &sdhci_map)) {
      OSSafeReleaseNULL(gcc_map);
      OSSafeReleaseNULL(gcc_desc);
      snap.err = SM6125_EMMC_RO_ERR_MAP_SDHCI;
      panic_with_snapshot(&snap);
    }

    const uintptr_t gcc_va = static_cast<uintptr_t>(gcc_map->getVirtualAddress());
    const uintptr_t sdhci_va = static_cast<uintptr_t>(sdhci_map->getVirtualAddress());
    char buf[768];

    if (mode == SM6125_EMMC_PROBE_MODE_IDENT_GPT) {
      sm6125_emmc_ident_report_t rep;
      memset(&rep, 0, sizeof(rep));
      sm6125_emmc_ident_gpt_run(&kIdentMmioOps, gcc_va, sdhci_va, &rep);
      OSSafeReleaseNULL(sdhci_map);
      OSSafeReleaseNULL(sdhci_desc);
      OSSafeReleaseNULL(gcc_map);
      OSSafeReleaseNULL(gcc_desc);
      sm6125_emmc_ident_format_report(&rep, buf, sizeof(buf));
      panic("%s", buf);
    }

    IOBufferMemoryDescriptor *desc_md = nullptr;
    IOBufferMemoryDescriptor *data_md = nullptr;
    Sm6125DmaCacheCtx dma_ctx = {};
    sm6125_emmc_dma_buf_t dma = {};
    const bool has_dma = alloc_dma_buf(&desc_md, &data_md, &dma_ctx, &dma);

    sm6125_emmc_fast_report_t fast_rep;
    memset(&fast_rep, 0, sizeof(fast_rep));
    sm6125_emmc_fast_bench_run(
        &kIdentMmioOps, gcc_va, sdhci_va, has_dma ? &dma : nullptr, &fast_rep);

    if (has_dma) {
      data_md->complete(kIODirectionInOut);
      desc_md->complete(kIODirectionInOut);
      OSSafeReleaseNULL(data_md);
      OSSafeReleaseNULL(desc_md);
    }
    OSSafeReleaseNULL(sdhci_map);
    OSSafeReleaseNULL(sdhci_desc);
    OSSafeReleaseNULL(gcc_map);
    OSSafeReleaseNULL(gcc_desc);
    sm6125_emmc_fast_format_report(&fast_rep, buf, sizeof(buf));
    panic("%s", buf);
  }

  IOMemoryDescriptor *gcc_desc = nullptr;
  IOMemoryMap *gcc_map = nullptr;
  if (!map_mmio_window(
          snap.gcc_pa + SM6125_GCC_SDCC1_PAGE_OFFSET,
          SM6125_GCC_SDCC1_WINDOW_SIZE,
          &gcc_desc,
          &gcc_map)) {
    snap.err = SM6125_EMMC_RO_ERR_MAP_GCC;
    panic_with_snapshot(&snap);
  }
  const bool gcc_ok = sm6125_emmc_ro_read_gcc(
      &kReadOnlyMmioOps, static_cast<uintptr_t>(gcc_map->getVirtualAddress()), &snap);
  OSSafeReleaseNULL(gcc_map);
  OSSafeReleaseNULL(gcc_desc);

  if (!gcc_ok) {
    snap.err = SM6125_EMMC_RO_ERR_MAP_GCC;
    panic_with_snapshot(&snap);
  }

  if (!sm6125_emmc_gcc_gate(
          snap.gcc_bcr, snap.gcc_apps_cbcr, snap.gcc_ahb_cbcr, snap.gcc_ice_cbcr, &snap.gate_flags)) {
    snap.err = SM6125_EMMC_RO_ERR_CLOCK_GATED;
    panic_with_snapshot(&snap);
  }

  IOMemoryDescriptor *sdhci_desc = nullptr;
  IOMemoryMap *sdhci_map = nullptr;
  if (!map_mmio_window(snap.sdhci_pa, SM6125_SDHCI_MIN_REGION_SIZE, &sdhci_desc, &sdhci_map)) {
    snap.err = SM6125_EMMC_RO_ERR_MAP_SDHCI;
    panic_with_snapshot(&snap);
  }
  const bool sdhci_ok = sm6125_emmc_ro_read_sdhci(
      &kReadOnlyMmioOps, static_cast<uintptr_t>(sdhci_map->getVirtualAddress()), &snap);
  OSSafeReleaseNULL(sdhci_map);
  OSSafeReleaseNULL(sdhci_desc);

  snap.err = sdhci_ok ? SM6125_EMMC_RO_ERR_NONE : SM6125_EMMC_RO_ERR_MAP_SDHCI;
  panic_with_snapshot(&snap);
}
