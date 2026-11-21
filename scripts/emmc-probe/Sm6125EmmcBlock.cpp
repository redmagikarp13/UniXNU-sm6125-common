// SPDX-License-Identifier: MIT
// Driver de bloco somente-leitura IOBlockStorageDevice para eMMC SM6125 (SDCC1 DDR52 + 32-bit ADMA2 + VerifiedRoot).
#include "Sm6125EmmcCore.h"
#include "Sm6125EmmcReadOnly.h"
#include "Sm6125ShngRoot.h"
#include "verified_root.h"
#include <IOKit/IOBSD.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOService.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOMedia.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/crypto/sha2.h>
#include <libkern/libkern.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <sys/param.h>
#include <sys/types.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>

extern "C" {

extern kmod_info_t g_kernel_kmod_info;
kern_return_t OSKextLoadKextWithIdentifier(const char *kext_id);

extern const struct vr_crypto vr_xnu_crypto;
extern const uint8_t sm6125_verified_root_key[VR_KEY_BYTES];
extern const size_t sm6125_verified_root_key_len;
extern const uint64_t sm6125_verified_root_min_generation;
extern const uint8_t sm6125_verified_os_header[VR_HEADER_BYTES + VR_SIGNATURE_BYTES];
extern const size_t sm6125_verified_os_header_len;
extern const uint8_t sm6125_verified_systemos_header[VR_HEADER_BYTES + VR_SIGNATURE_BYTES];
extern const size_t sm6125_verified_systemos_header_len;

extern volatile int native_owner_root_accepted;
extern volatile int native_owner_root_apple_autherr;
extern volatile int native_owner_cache_stage;
extern volatile int native_owner_cache_status;
extern volatile int native_owner_cache_native_union;
extern char native_owner_cache_device_path[64];
extern volatile int native_owner_boot_manifest_status;
extern uint8_t native_owner_boot_manifest_hash[48];

int native_verified_block_root_authorized(dev_t device);
int native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48]);
int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size);
void sm6125_emmc_post_mount_check(int cache_error, int launchd_ok, int dyld_ok, int dsc_ok);

uint8_t sm6125_mmio_read8(uintptr_t base, uint32_t offset);
uint16_t sm6125_mmio_read16(uintptr_t base, uint32_t offset);
uint32_t sm6125_mmio_read32(uintptr_t base, uint32_t offset);
void sm6125_mmio_write8(uintptr_t base, uint32_t offset, uint8_t value);
void sm6125_mmio_write16(uintptr_t base, uint32_t offset, uint16_t value);
void sm6125_mmio_write32(uintptr_t base, uint32_t offset, uint32_t value);

void sm6125_ioblockstoragedevice_static_init(void)
    __asm__("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
void sm6125_emmc_block_static_init(void)
    __asm__("__GLOBAL__sub_I_Sm6125EmmcBlock.cpp");

static const OSMetaClass *g_sm6125_storage_parent = nullptr;

const OSMetaClass *sm6125_emmc_storage_parent(void)
{
  return g_sm6125_storage_parent;
}

}  // extern "C"

namespace {

constexpr size_t kBounceBufBytes = 32768u;
constexpr size_t kDmaDescBytes = 4096u;
constexpr size_t kDmaCanaryBytes = 4096u;
constexpr size_t kDmaDataTotal = kBounceBufBytes + kDmaCanaryBytes;
constexpr char kOsVolumeUuid[] = "04769D26-2CA0-4825-B443-E2E5F88300F2";
constexpr char kSystemOsVolumeUuid[] = "8846FE78-2910-4297-AF97-D59BB6F8FC16";

static void blk_delay_us(uint32_t microseconds)
{
  IODelay(microseconds);
}

static uint64_t blk_now_us(void)
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000ULL;
}

static void blk_sha256(const uint8_t *data, size_t len, uint8_t out_digest[32])
{
  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, data, len);
  SHA256_Final(out_digest, &ctx);
}

static const sm6125_emmc_hw_ops_t kBlockHwOps = {
    .read8 = sm6125_mmio_read8,
    .read16 = sm6125_mmio_read16,
    .read32 = sm6125_mmio_read32,
    .write8 = sm6125_mmio_write8,
    .write16 = sm6125_mmio_write16,
    .write32 = sm6125_mmio_write32,
    .delay_us = blk_delay_us,
    .now_us = blk_now_us,
};

struct Sm6125SharedEmmcCtrl {
  IOLock *gate;
  IOMemoryDescriptor *gcc_desc;
  IOMemoryMap *gcc_map;
  IOMemoryDescriptor *sdhci_desc;
  IOMemoryMap *sdhci_map;
  IOBufferMemoryDescriptor *desc_md;
  IOBufferMemoryDescriptor *data_md;
  uint8_t *bounce_buf;
  sm6125_emmc_ctrl_t core;
  uint32_t block_mode;
  volatile uint32_t io_requests;
  volatile uint32_t io_errors;
  volatile uint32_t write_rejects;
  volatile uint32_t vr_verified_blocks;
  volatile uint32_t vr_integrity_errors;
  volatile uint64_t io_sectors;
  volatile uint64_t io_time_us;
  char os_bsd[16];
  char sysos_bsd[16];
};

static Sm6125SharedEmmcCtrl *g_shared_ctrl = nullptr;
static bool g_classes_registered = false;

static void shared_clean_desc(void *ctx, size_t offset, size_t len)
{
  auto *c = static_cast<Sm6125SharedEmmcCtrl *>(ctx);
  if (c && c->desc_md) {
    c->desc_md->performOperation(kIOMemoryIncoherentIOStore, offset, len);
    __asm__ volatile("dsb sy" : : : "memory");
  }
}

static void shared_flush_data(void *ctx, size_t offset, size_t len)
{
  auto *c = static_cast<Sm6125SharedEmmcCtrl *>(ctx);
  if (c && c->data_md) {
    c->data_md->performOperation(kIOMemoryIncoherentIOFlush, offset, len);
    __asm__ volatile("dsb sy" : : : "memory");
  }
}

static void shared_invalidate_data(void *ctx, size_t offset, size_t len)
{
  auto *c = static_cast<Sm6125SharedEmmcCtrl *>(ctx);
  if (c && c->data_md) {
    c->data_md->performOperation(kIOMemoryIncoherentIOFlush, offset, len);
    __asm__ volatile("dsb sy" : : : "memory");
  }
}

static const OSMetaClass *lookup_meta(const char *name_cstr)
{
  const OSSymbol *sym = OSSymbol::withCString(name_cstr);
  const OSMetaClass *meta = sym ? OSMetaClass::getMetaClassWithName(sym) : nullptr;
  OSSafeReleaseNULL(sym);
  return meta;
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

static bool alloc_ctrl_dma(Sm6125SharedEmmcCtrl *ctrl, sm6125_emmc_dma_buf_t *out_dma)
{
  constexpr mach_vm_address_t kLow4GMask = 0x00000000FFFFF000ULL;
  constexpr IOOptionBits kOpts =
      static_cast<IOOptionBits>(kIODirectionInOut) |
      static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);

  IOBufferMemoryDescriptor *desc_md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task, kOpts, kDmaDescBytes, kLow4GMask);
  IOBufferMemoryDescriptor *data_md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task, kOpts, kDmaDataTotal, kLow4GMask);
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
      desc_seg < kDmaDescBytes || data_seg < kDmaDataTotal ||
      desc_pa > (0xFFFFFFFFULL - kDmaDescBytes + 1ULL) ||
      data_pa > (0xFFFFFFFFULL - kDmaDataTotal + 1ULL)) {
    data_md->complete(kIODirectionInOut);
    desc_md->complete(kIODirectionInOut);
    OSSafeReleaseNULL(data_md);
    OSSafeReleaseNULL(desc_md);
    return false;
  }

  ctrl->desc_md = desc_md;
  ctrl->data_md = data_md;
  memset(out_dma, 0, sizeof(*out_dma));
  out_dma->desc_va = desc_va;
  out_dma->desc_pa = desc_pa;
  out_dma->desc_size = kDmaDescBytes;
  out_dma->data_va = data_va;
  out_dma->data_pa = data_pa;
  out_dma->data_size = kDmaDataTotal;
  out_dma->canary_offset = kBounceBufBytes;
  out_dma->canary_size = kDmaCanaryBytes;
  out_dma->cache_clean_desc = shared_clean_desc;
  out_dma->cache_flush_data = shared_flush_data;
  out_dma->cache_invalidate_data = shared_invalidate_data;
  out_dma->cache_ctx = ctrl;
  return true;
}

static bool select_window_policy_header(
    const char *window_name,
    const uint8_t **out_hdr,
    size_t *out_hdr_len)
{
  if (!window_name || !out_hdr || !out_hdr_len) {
    return false;
  }
  if (strcmp(window_name, "OS") == 0) {
    *out_hdr = sm6125_verified_os_header;
    *out_hdr_len = sm6125_verified_os_header_len;
    return true;
  }
  if (strcmp(window_name, "SystemOS") == 0) {
    *out_hdr = sm6125_verified_systemos_header;
    *out_hdr_len = sm6125_verified_systemos_header_len;
    return true;
  }
  return false;
}

}  // namespace

class Sm6125EmmcBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(Sm6125EmmcBlock)

 private:
  Sm6125SharedEmmcCtrl *ctrl_ = nullptr;
  uint64_t baseLba_ = 0;
  uint64_t sectorCount_ = 0;
  bool online_ = false;
  bool requireVerified_ = false;
  bool verifiedReady_ = false;
  vm_offset_t verifiedStorage_ = 0;
  vm_size_t verifiedStorageBytes_ = 0;
  size_t verifiedEnvelopeBytes_ = 0;
  vr_view verifiedView_ = {};
  char windowName_[32] = {};
  char productName_[48] = {};

 public:
  bool configureWindow(
      Sm6125SharedEmmcCtrl *ctrl,
      const char *name,
      uint64_t base_lba,
      uint64_t sector_count,
      bool require_verified)
  {
    if (!ctrl || !ctrl->core.online || !name || sector_count == 0 ||
        base_lba >= ctrl->core.sec_count ||
        sector_count > ctrl->core.sec_count - base_lba) {
      return false;
    }
    ctrl_ = ctrl;
    baseLba_ = base_lba;
    sectorCount_ = sector_count;
    online_ = true;
    requireVerified_ = require_verified;
    verifiedReady_ = false;
    strlcpy(windowName_, name, sizeof(windowName_));
    snprintf(productName_, sizeof(productName_), "SM6125 eMMC %s", windowName_);
    setName(productName_);
    setProperty("SM6125Window", windowName_);
    return true;
  }

  bool configureVerifiedWindow(
      Sm6125SharedEmmcCtrl *ctrl,
      const sm6125_shngroot_entry_t &win_entry,
      const sm6125_shngroot_entry_t *vroot_entry)
  {
    if (!ctrl || !vroot_entry ||
        win_entry.flags != SM6125_SHNGROOT_FLAG_RO_WINDOW ||
        vroot_entry->flags != SM6125_SHNGROOT_FLAG_OBJECT ||
        vroot_entry->block_size != VR_BLOCK_BYTES ||
        (win_entry.offset & 511ULL) != 0ULL ||
        (win_entry.size & 511ULL) != 0ULL ||
        (vroot_entry->offset & 511ULL) != 0ULL) {
      return false;
    }
    const uint64_t win_base = ctrl->core.userdata_first + (win_entry.offset / 512ULL);
    const uint64_t win_secs = win_entry.size / 512ULL;
    if (!configureWindow(ctrl, win_entry.name, win_base, win_secs, true)) {
      return false;
    }

    const uint8_t *hdr = nullptr;
    size_t hdr_len = 0;
    if (!select_window_policy_header(win_entry.name, &hdr, &hdr_len) ||
        hdr_len != (VR_HEADER_BYTES + VR_SIGNATURE_BYTES) ||
        sm6125_verified_root_key_len != VR_KEY_BYTES) {
      return false;
    }

    vr_view hdr_view = {};
    if (vr_open_header(
            hdr,
            hdr_len,
            sm6125_verified_root_key,
            sm6125_verified_root_min_generation,
            &vr_xnu_crypto,
            &hdr_view) != VR_OK) {
      return false;
    }

    if (hdr_view.image_bytes != win_entry.size ||
        vroot_entry->size != hdr_view.block_count * 32ULL ||
        memcmp(hdr_view.image_sha256, win_entry.sha256, 32u) != 0 ||
        memcmp(hdr + 88u, vroot_entry->sha256, 32u) != 0) {
      return false;
    }

    const size_t envelope_bytes = hdr_len + static_cast<size_t>(vroot_entry->size);
    const vm_size_t table_pages = round_page(static_cast<vm_size_t>(envelope_bytes));
    const vm_size_t alloc_bytes = table_pages + PAGE_SIZE;
    vm_offset_t storage = 0;
    if (kmem_alloc(
            kernel_map,
            &storage,
            alloc_bytes,
            static_cast<kma_flags_t>(KMA_KOBJECT | KMA_DATA),
            VM_KERN_MEMORY_SECURITY) != KERN_SUCCESS ||
        !storage) {
      return false;
    }

    auto *storage_bytes = reinterpret_cast<uint8_t *>(storage);
    memcpy(storage_bytes, hdr, hdr_len);

    const uint64_t vroot_base_lba = ctrl->core.userdata_first + (vroot_entry->offset / 512ULL);
    const uint64_t total_vroot_bytes = vroot_entry->size;
    uint64_t copied_bytes = 0;
    uint64_t lba_cursor = vroot_base_lba;

    IOLockLock(ctrl->gate);
    while (copied_bytes < total_vroot_bytes) {
      const uint64_t rem_bytes = total_vroot_bytes - copied_bytes;
      const uint64_t rem_sectors = (rem_bytes + 511ULL) / 512ULL;
      const uint32_t chunk_sectors = static_cast<uint32_t>(rem_sectors > 64ULL ? 64ULL : rem_sectors);
      const size_t copy_len = static_cast<size_t>(
          rem_bytes < static_cast<uint64_t>(chunk_sectors) * 512ULL
              ? rem_bytes
              : static_cast<uint64_t>(chunk_sectors) * 512ULL);
      const int32_t rrc = sm6125_emmc_ctrl_read_sectors(
          &ctrl->core, lba_cursor, chunk_sectors, ctrl->bounce_buf);
      if (rrc != SM6125_EMMC_RC_OK) {
        IOLockUnlock(ctrl->gate);
        kmem_free(kernel_map, storage, alloc_bytes);
        return false;
      }
      memcpy(storage_bytes + hdr_len + copied_bytes, ctrl->bounce_buf, copy_len);
      copied_bytes += copy_len;
      lba_cursor += chunk_sectors;
    }
    IOLockUnlock(ctrl->gate);

    auto *key_copy = storage_bytes + table_pages;
    memcpy(key_copy, sm6125_verified_root_key, VR_KEY_BYTES);

    if (vm_map_protect(
            kernel_map,
            storage,
            storage + alloc_bytes,
            VM_PROT_READ,
            TRUE) != KERN_SUCCESS) {
      kmem_free(kernel_map, storage, alloc_bytes);
      return false;
    }

    vr_view full_view = {};
    if (vr_open(
            storage_bytes,
            envelope_bytes,
            key_copy,
            sm6125_verified_root_min_generation,
            &vr_xnu_crypto,
            &full_view) != VR_OK ||
        full_view.image_bytes != win_entry.size) {
      return false;
    }

    /* Cryptographically verify block 0 (4 KiB = 8 sectors) before publishing the device. */
    IOLockLock(ctrl->gate);
    const int32_t b0_rc = sm6125_emmc_ctrl_read_sectors(&ctrl->core, win_base, 8u, ctrl->bounce_buf);
    const int vr_b0 = (b0_rc == SM6125_EMMC_RC_OK)
        ? vr_verify_block(&full_view, 0ULL, ctrl->bounce_buf, VR_BLOCK_BYTES, &vr_xnu_crypto)
        : VR_BLOCK;
    IOLockUnlock(ctrl->gate);
    if (vr_b0 != VR_OK) {
      return false;
    }
    __atomic_fetch_add(&ctrl->vr_verified_blocks, 1u, __ATOMIC_RELAXED);

    verifiedStorage_ = storage;
    verifiedStorageBytes_ = alloc_bytes;
    verifiedEnvelopeBytes_ = envelope_bytes;
    verifiedView_ = full_view;
    verifiedReady_ = true;
    return true;
  }

  bool authenticatesVolume(const char *uuid, uint8_t *manifestDigest) const
  {
    if (!uuid || !online_ || !verifiedReady_ || !verifiedStorage_ || !verifiedView_.hashes) {
      return false;
    }
    static const char kHex[] = "0123456789ABCDEF";
    char expected[37];
    unsigned at = 0;
    for (unsigned i = 0; i < 16u; ++i) {
      if (i == 4u || i == 6u || i == 8u || i == 10u) {
        expected[at++] = '-';
      }
      expected[at++] = kHex[verifiedView_.volume_uuid[i] >> 4u];
      expected[at++] = kHex[verifiedView_.volume_uuid[i] & 0x0Fu];
    }
    expected[at] = '\0';
    if (strcmp(uuid, expected) != 0) {
      return false;
    }
    if (manifestDigest) {
      SHA384_CTX ctx;
      SHA384_Init(&ctx);
      SHA384_Update(
          &ctx,
          reinterpret_cast<const void *>(verifiedStorage_),
          verifiedEnvelopeBytes_);
      SHA384_Final(manifestDigest, &ctx);
    }
    return true;
  }

  IOReturn doEjectMedia(void) override
  {
    return kIOReturnUnsupported;
  }

  IOReturn doFormatMedia(UInt64 byteCapacity) override
  {
    (void)byteCapacity;
    return kIOReturnNotWritable;
  }

  UInt32 doGetFormatCapacities(UInt64 *capacities, UInt32 capacitiesMaxCount) const override
  {
    (void)capacities;
    (void)capacitiesMaxCount;
    return 0;
  }

  char *getVendorString(void) override
  {
    return const_cast<char *>("Qualcomm-Micron");
  }

  char *getProductString(void) override
  {
    return productName_[0] ? productName_ : const_cast<char *>("SM6125 eMMC S0J9K8");
  }

  char *getRevisionString(void) override
  {
    return const_cast<char *>("1.0");
  }

  char *getAdditionalDeviceInfoString(void) override
  {
    return const_cast<char *>("SDCC1 DDR52 ADMA2 VerifiedRoot RO");
  }

  IOReturn reportBlockSize(UInt64 *blockSize) override
  {
    if (!blockSize) {
      return kIOReturnBadArgument;
    }
    *blockSize = 512;
    return kIOReturnSuccess;
  }

  IOReturn reportEjectability(bool *isEjectable) override
  {
    if (!isEjectable) {
      return kIOReturnBadArgument;
    }
    *isEjectable = false;
    return kIOReturnSuccess;
  }

  IOReturn reportMaxValidBlock(UInt64 *maxBlock) override
  {
    if (!maxBlock || !online_ || sectorCount_ == 0) {
      return kIOReturnBadArgument;
    }
    *maxBlock = sectorCount_ - 1;
    return kIOReturnSuccess;
  }

  IOReturn reportMediaState(bool *mediaPresent, bool *changedState) override
  {
    if (!mediaPresent) {
      return kIOReturnBadArgument;
    }
    *mediaPresent = online_;
    if (changedState) {
      *changedState = false;
    }
    return kIOReturnSuccess;
  }

  IOReturn reportRemovability(bool *isRemovable) override
  {
    if (!isRemovable) {
      return kIOReturnBadArgument;
    }
    *isRemovable = false;
    return kIOReturnSuccess;
  }

  IOReturn reportWriteProtection(bool *isWriteProtected) override
  {
    if (!isWriteProtected) {
      return kIOReturnBadArgument;
    }
    *isWriteProtected = true;
    return kIOReturnSuccess;
  }

  IOReturn doSynchronizeCache(void) override
  {
    return online_ ? kIOReturnSuccess : kIOReturnNotReady;
  }

  IOReturn doSynchronize(UInt64 block, UInt64 nblks, IOStorageSynchronizeOptions options) override
  {
    (void)block;
    (void)nblks;
    (void)options;
    return doSynchronizeCache();
  }

  IOReturn getWriteCacheState(bool *enabled) override
  {
    if (!enabled) {
      return kIOReturnBadArgument;
    }
    *enabled = false;
    return kIOReturnSuccess;
  }

  IOReturn setWriteCacheState(bool enabled) override
  {
    (void)enabled;
    return kIOReturnUnsupported;
  }

  IOReturn doAsyncReadWrite(
      IOMemoryDescriptor *buffer,
      UInt64 block,
      UInt64 nblks,
      IOStorageAttributes *attributes,
      IOStorageCompletion *completion) override
  {
    (void)attributes;
    if (!buffer || !completion || !completion->action || !nblks || !online_ || !ctrl_ ||
        block >= sectorCount_ || nblks > sectorCount_ - block ||
        nblks > UINT64_MAX / 512ULL || buffer->getLength() < nblks * 512ULL) {
      return kIOReturnBadArgument;
    }
    if (buffer->getDirection() != kIODirectionIn) {
      __atomic_fetch_add(&ctrl_->write_rejects, 1u, __ATOMIC_RELAXED);
      return kIOReturnNotWritable;
    }
    if (requireVerified_ && !verifiedReady_) {
      __atomic_fetch_add(&ctrl_->vr_integrity_errors, 1u, __ATOMIC_RELAXED);
      return kIOReturnNotPrivileged;
    }

    uint64_t actual_bytes = 0;
    uint64_t read_sectors_total = 0;
    IOReturn status = kIOReturnSuccess;

    IOLockLock(ctrl_->gate);
    const uint64_t t0 = blk_now_us();

    if (verifiedReady_) {
      const uint64_t image_bytes = sectorCount_ * 512ULL;
      uint64_t cur_byte = block * 512ULL;
      const uint64_t end_byte = cur_byte + nblks * 512ULL;

      while (cur_byte < end_byte) {
        const uint64_t first_vr_blk = cur_byte / VR_BLOCK_BYTES;
        const uint64_t last_vr_blk = (end_byte - 1ULL) / VR_BLOCK_BYTES;
        uint64_t vr_blk_count = last_vr_blk - first_vr_blk + 1ULL;
        constexpr uint64_t kMaxVrBlocksPerBatch = kBounceBufBytes / VR_BLOCK_BYTES;
        if (vr_blk_count > kMaxVrBlocksPerBatch) {
          vr_blk_count = kMaxVrBlocksPerBatch;
        }

        const uint64_t batch_start_byte = first_vr_blk * VR_BLOCK_BYTES;
        uint64_t batch_end_byte = batch_start_byte + vr_blk_count * VR_BLOCK_BYTES;
        if (batch_end_byte > image_bytes) {
          batch_end_byte = image_bytes;
        }
        const uint64_t batch_lba = batch_start_byte / 512ULL;
        const uint32_t batch_sectors =
            static_cast<uint32_t>((batch_end_byte - batch_start_byte) / 512ULL);

        const int32_t rc = sm6125_emmc_ctrl_read_sectors(
            &ctrl_->core, baseLba_ + batch_lba, batch_sectors, ctrl_->bounce_buf);
        if (rc != SM6125_EMMC_RC_OK) {
          __atomic_fetch_add(&ctrl_->io_errors, 1u, __ATOMIC_RELAXED);
          status = kIOReturnIOError;
          break;
        }

        for (uint64_t b = 0; b < vr_blk_count; ++b) {
          const uint64_t blk_idx = first_vr_blk + b;
          const uint64_t blk_off = b * VR_BLOCK_BYTES;
          const uint64_t abs_off = batch_start_byte + blk_off;
          const size_t blk_len = static_cast<size_t>(
              (abs_off + VR_BLOCK_BYTES <= batch_end_byte)
                  ? VR_BLOCK_BYTES
                  : (batch_end_byte - abs_off));
          if (vr_verify_block(
                  &verifiedView_,
                  blk_idx,
                  ctrl_->bounce_buf + blk_off,
                  blk_len,
                  &vr_xnu_crypto) != VR_OK) {
            __atomic_fetch_add(&ctrl_->vr_integrity_errors, 1u, __ATOMIC_RELAXED);
            status = kIOReturnNotPrivileged;
            break;
          }
          __atomic_fetch_add(&ctrl_->vr_verified_blocks, 1u, __ATOMIC_RELAXED);
        }
        if (status != kIOReturnSuccess) {
          break;
        }

        const uint64_t slice_end = (end_byte < batch_end_byte) ? end_byte : batch_end_byte;
        const size_t slice_off = static_cast<size_t>(cur_byte - batch_start_byte);
        const IOByteCount slice_len = static_cast<IOByteCount>(slice_end - cur_byte);
        if (buffer->writeBytes(
                static_cast<IOByteCount>(actual_bytes),
                ctrl_->bounce_buf + slice_off,
                slice_len) != slice_len) {
          __atomic_fetch_add(&ctrl_->io_errors, 1u, __ATOMIC_RELAXED);
          status = kIOReturnIOError;
          break;
        }

        cur_byte = slice_end;
        actual_bytes += slice_len;
        read_sectors_total += batch_sectors;
      }
    } else {
      uint64_t done_sectors = 0;
      while (done_sectors < nblks) {
        const uint64_t rem = nblks - done_sectors;
        const uint32_t chunk = static_cast<uint32_t>(rem > 64ULL ? 64ULL : rem);
        const uint32_t chunk_bytes = chunk * 512u;
        const int32_t rc = sm6125_emmc_ctrl_read_sectors(
            &ctrl_->core, baseLba_ + block + done_sectors, chunk, ctrl_->bounce_buf);
        if (rc != SM6125_EMMC_RC_OK) {
          __atomic_fetch_add(&ctrl_->io_errors, 1u, __ATOMIC_RELAXED);
          status = kIOReturnIOError;
          break;
        }
        if (buffer->writeBytes(
                static_cast<IOByteCount>(actual_bytes),
                ctrl_->bounce_buf,
                chunk_bytes) != chunk_bytes) {
          __atomic_fetch_add(&ctrl_->io_errors, 1u, __ATOMIC_RELAXED);
          status = kIOReturnIOError;
          break;
        }
        done_sectors += chunk;
        actual_bytes += chunk_bytes;
        read_sectors_total += chunk;
      }
    }

    const uint64_t t1 = blk_now_us();
    if (status == kIOReturnSuccess) {
      __atomic_fetch_add(&ctrl_->io_requests, 1u, __ATOMIC_RELAXED);
      __atomic_fetch_add(&ctrl_->io_sectors, read_sectors_total, __ATOMIC_RELAXED);
      if (t1 > t0) {
        __atomic_fetch_add(&ctrl_->io_time_us, t1 - t0, __ATOMIC_RELAXED);
      }
    }
    IOLockUnlock(ctrl_->gate);

    completion->action(completion->target, completion->parameter, status, actual_bytes);
    return kIOReturnSuccess;
  }
};

OSDefineMetaClassAndStructors(Sm6125EmmcBlock, IOBlockStorageDevice)
static_assert(sizeof(IOBlockStorageDevice) == 0x90, "Expected 0x90 IOBlockStorageDevice ABI");

namespace {

static int verified_volume_query(dev_t device, const char *expectedUUID, uint8_t *manifestDigest)
{
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity(2);
  OSNumber *mjr = OSNumber::withNumber(
      static_cast<unsigned>(major(static_cast<uint32_t>(device))), 32);
  OSNumber *mnr = OSNumber::withNumber(
      static_cast<unsigned>(minor(static_cast<uint32_t>(device))), 32);
  if (!matching || !properties || !mjr || !mnr) {
    OSSafeReleaseNULL(mnr);
    OSSafeReleaseNULL(mjr);
    OSSafeReleaseNULL(properties);
    OSSafeReleaseNULL(matching);
    return 0;
  }
  const bool ready = properties->setObject(kIOBSDMajorKey, mjr) &&
                     properties->setObject(kIOBSDMinorKey, mnr) &&
                     matching->setObject("IOPropertyMatch", properties);
  mnr->release();
  mjr->release();
  properties->release();
  OSIterator *iterator = ready ? IOService::getMatchingServices(matching) : nullptr;
  matching->release();
  if (!iterator) {
    return 0;
  }

  bool authenticated = false;
  unsigned matches = 0;
  unsigned depth = 0;
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    if (!media) {
      continue;
    }
    if (++matches != 1u) {
      authenticated = false;
      break;
    }
    OSString *uuid = OSDynamicCast(OSString, media->getProperty(kIOMediaUUIDKey));
    if (!uuid || uuid->getLength() != 36u ||
        (expectedUUID && strcmp(expectedUUID, uuid->getCStringNoCopy()) != 0)) {
      continue;
    }
    for (IOService *provider = media; provider && depth < 64u;
         provider = provider->getProvider()) {
      ++depth;
      Sm6125EmmcBlock *block = OSDynamicCast(Sm6125EmmcBlock, provider);
      if (!block) {
        continue;
      }
      authenticated = block->authenticatesVolume(uuid->getCStringNoCopy(), manifestDigest);
      break;
    }
  }
  iterator->release();
  if (!authenticated || matches != 1u) {
    if (manifestDigest) {
      bzero(manifestDigest, SHA384_DIGEST_LENGTH);
    }
    return 0;
  }
  return 1;
}

}  // namespace

extern "C" int native_verified_block_root_authorized(dev_t device)
{
  return verified_volume_query(device, nullptr, nullptr);
}

extern "C" int native_verified_block_manifest_sha384(
    dev_t device,
    const char *uuid,
    uint8_t digest[48])
{
  if (!digest) {
    return 0;
  }
  bzero(digest, SHA384_DIGEST_LENGTH);
  if (!uuid || strlen(uuid) != 36u) {
    return 0;
  }
  return verified_volume_query(device, uuid, digest);
}

extern "C" int native_verified_block_volume_device(
    const char *uuid,
    dev_t *device,
    char *path,
    size_t size)
{
  if (!uuid || !device || !path || size < 6u || strlen(uuid) != 36u) {
    return 0;
  }
  *device = NODEV;
  path[0] = '\0';
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity(1);
  OSString *wanted = OSString::withCString(uuid);
  if (!matching || !properties || !wanted) {
    OSSafeReleaseNULL(wanted);
    OSSafeReleaseNULL(properties);
    OSSafeReleaseNULL(matching);
    return 0;
  }
  const bool ready = properties->setObject(kIOMediaUUIDKey, wanted) &&
                     matching->setObject("IOPropertyMatch", properties);
  wanted->release();
  properties->release();
  OSIterator *iterator = ready ? IOService::getMatchingServices(matching) : nullptr;
  matching->release();
  if (!iterator) {
    return 0;
  }

  unsigned matches = 0;
  bool authenticated = false;
  dev_t candidate = NODEV;
  char candidatePath[64] = {};
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    if (!media) {
      continue;
    }
    if (++matches != 1u) {
      authenticated = false;
      break;
    }
    OSString *identity = OSDynamicCast(OSString, media->getProperty(kIOMediaUUIDKey));
    OSString *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
    OSNumber *mjr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMajorKey));
    OSNumber *mnr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMinorKey));
    if (!identity || strcmp(identity->getCStringNoCopy(), uuid) != 0 ||
        !name || !mjr || !mnr || !name->getLength() ||
        name->getLength() >= sizeof(candidatePath) - 6u ||
        name->getLength() >= size - 6u ||
        mjr->unsigned64BitValue() > 255ULL ||
        mnr->unsigned64BitValue() > 0xFFFFFFULL) {
      continue;
    }
    candidate = static_cast<dev_t>(
        makedev(mjr->unsigned32BitValue(), mnr->unsigned32BitValue()));
    if (!native_verified_block_root_authorized(candidate)) {
      continue;
    }
    snprintf(candidatePath, sizeof(candidatePath), "/dev/%s", name->getCStringNoCopy());
    authenticated = true;
  }
  iterator->release();
  if (!authenticated || matches != 1u) {
    return 0;
  }
  *device = candidate;
  strlcpy(path, candidatePath, size);
  return 1;
}

namespace {

static const char *blk_mode_name(uint32_t mode)
{
  switch (mode) {
    case SM6125_EMMC_FAST_MODE_HS50_PIO: return "hs50-pio";
    case SM6125_EMMC_FAST_MODE_HS50_ADMA: return "hs50-adma";
    case SM6125_EMMC_FAST_MODE_DDR52_ADMA: return "ddr52-adma";
    default: return "none";
  }
}

static bool has_sm6125_ancestry(IOService *service, bool *out_saw_driver)
{
  bool saw_drv = false;
  unsigned depth = 0;
  for (IOService *cur = service; cur && depth < 32u; cur = cur->getProvider(), depth++) {
    if (cur->metaCast("IOBlockStorageDriver") != nullptr) {
      saw_drv = true;
    }
    if (OSDynamicCast(Sm6125EmmcBlock, cur) != nullptr) {
      if (out_saw_driver && saw_drv) {
        *out_saw_driver = true;
      }
      return true;
    }
  }
  return false;
}

struct IokitStorageObservation {
  bool saw_driver;
  uint32_t media_count;
  uint32_t bsd_count;
  uint32_t part_count;
  char first_bsd[16];
  char target_bsd[16];
  char target_uuid[40];
  char os_bsd[16];
  char sysos_bsd[16];
  bool os_auth;
  bool sysos_auth;
};

static void observe_iokit_storage(bool has_shngroot, IokitStorageObservation *obs)
{
  memset(obs, 0, sizeof(*obs));
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  if (!matching) {
    return;
  }
  OSIterator *iter = IOService::getMatchingServices(matching);
  matching->release();
  if (!iter) {
    return;
  }

  while (OSObject *obj = iter->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, obj);
    if (!media) {
      continue;
    }
    if (!has_sm6125_ancestry(media, &obs->saw_driver)) {
      continue;
    }
    obs->media_count++;
    OSBoolean *whole = OSDynamicCast(OSBoolean, media->getProperty(kIOMediaWholeKey));
    const bool is_whole = (whole == kOSBooleanTrue);
    if (!is_whole) {
      obs->part_count++;
    }
    OSString *bsd = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
    if (bsd && bsd->getLength() > 0) {
      obs->bsd_count++;
      if (obs->first_bsd[0] == '\0' && is_whole) {
        strlcpy(obs->first_bsd, bsd->getCStringNoCopy(), sizeof(obs->first_bsd));
      }
    }
    OSString *uuid = OSDynamicCast(OSString, media->getProperty(kIOMediaUUIDKey));
    const char *mname = media->getName();
    if (!has_shngroot) {
      if (mname && strcmp(mname, "userdata") == 0 && bsd && bsd->getLength() > 0) {
        strlcpy(obs->target_bsd, bsd->getCStringNoCopy(), sizeof(obs->target_bsd));
        if (uuid && uuid->getLength() > 0) {
          strlcpy(obs->target_uuid, uuid->getCStringNoCopy(), sizeof(obs->target_uuid));
        }
      }
    } else if (uuid && uuid->getLength() == 36 && bsd && bsd->getLength() > 0) {
      const char *ustr = uuid->getCStringNoCopy();
      if (strcmp(ustr, kOsVolumeUuid) == 0) {
        strlcpy(obs->os_bsd, bsd->getCStringNoCopy(), sizeof(obs->os_bsd));
        strlcpy(obs->target_bsd, bsd->getCStringNoCopy(), sizeof(obs->target_bsd));
        strlcpy(obs->target_uuid, ustr, sizeof(obs->target_uuid));
      } else if (strcmp(ustr, kSystemOsVolumeUuid) == 0) {
        strlcpy(obs->sysos_bsd, bsd->getCStringNoCopy(), sizeof(obs->sysos_bsd));
      }
    }
  }
  iter->release();

  if (has_shngroot) {
    dev_t dev = NODEV;
    char dev_path[64] = {};
    if (obs->os_bsd[0] != '\0' &&
        native_verified_block_volume_device(kOsVolumeUuid, &dev, dev_path, sizeof(dev_path)) == 1) {
      obs->os_auth = true;
    }
    if (obs->sysos_bsd[0] != '\0' &&
        native_verified_block_volume_device(kSystemOsVolumeUuid, &dev, dev_path, sizeof(dev_path)) == 1) {
      obs->sysos_auth = true;
    }
  }
}

static void noop_completion(void *target, void *parameter, IOReturn status, UInt64 actualByteCount)
{
  (void)target;
  (void)parameter;
  (void)status;
  (void)actualByteCount;
}

static bool verify_write_protection_refusal(Sm6125EmmcBlock *dev)
{
  uint8_t dummy[512] = {};
  IOMemoryDescriptor *out_md =
      IOMemoryDescriptor::withAddress(dummy, sizeof(dummy), kIODirectionOut);
  if (!out_md) {
    return false;
  }
  IOStorageCompletion comp = {
      .target = nullptr,
      .action = noop_completion,
      .parameter = nullptr,
  };
  const IOReturn rc = dev->doAsyncReadWrite(out_md, 0, 1, nullptr, &comp);
  out_md->release();
  return rc == kIOReturnNotWritable;
}

static const sm6125_shngroot_entry_t *find_shngroot_entry(
    const sm6125_shngroot_manifest_t &manifest,
    const char *name,
    uint32_t expected_flags)
{
  for (uint32_t i = 0; i < manifest.entry_count; ++i) {
    if (manifest.entries[i].flags == expected_flags &&
        strcmp(manifest.entries[i].name, name) == 0) {
      return &manifest.entries[i];
    }
  }
  return nullptr;
}

static Sm6125EmmcBlock *publish_raw_window_device(
    Sm6125SharedEmmcCtrl *ctrl,
    const char *name,
    uint64_t base_lba,
    uint64_t sector_count)
{
  IOService *platform = IOService::getPlatform();
  if (!platform) {
    return nullptr;
  }
  Sm6125EmmcBlock *dev = new Sm6125EmmcBlock;
  if (!dev) {
    return nullptr;
  }
  if (!dev->init(nullptr) || !dev->configureWindow(ctrl, name, base_lba, sector_count, false)) {
    dev->release();
    return nullptr;
  }
  if (!dev->attach(platform)) {
    dev->release();
    return nullptr;
  }
  if (!dev->start(platform)) {
    dev->detach(platform);
    dev->release();
    return nullptr;
  }
  dev->registerService();
  return dev;
}

static Sm6125EmmcBlock *publish_verified_window_device(
    Sm6125SharedEmmcCtrl *ctrl,
    const sm6125_shngroot_entry_t &win_entry,
    const sm6125_shngroot_entry_t *vroot_entry)
{
  IOService *platform = IOService::getPlatform();
  if (!platform) {
    return nullptr;
  }
  Sm6125EmmcBlock *dev = new Sm6125EmmcBlock;
  if (!dev) {
    return nullptr;
  }
  if (!dev->init(nullptr) || !dev->configureVerifiedWindow(ctrl, win_entry, vroot_entry)) {
    dev->release();
    return nullptr;
  }
  if (!dev->attach(platform)) {
    dev->release();
    return nullptr;
  }
  if (!dev->start(platform)) {
    dev->detach(platform);
    dev->release();
    return nullptr;
  }
  dev->registerService();
  return dev;
}

}  // namespace

extern "C" void sm6125_emmc_block_late_init(void)
{
  uint32_t arg_val = 0;
  const bool has_arg = PE_parse_boot_argn("sm6125-emmc-block", &arg_val, sizeof(arg_val));
  const uint32_t mode = sm6125_emmc_block_select_mode(has_arg, arg_val);
  if (mode == SM6125_EMMC_BLOCK_MODE_OFF) {
    return;
  }
  if (mode == SM6125_EMMC_BLOCK_MODE_INVALID) {
    panic("SM6125-EMMC-BLK ok=0 err=invalid-mode arg=%u", arg_val);
  }
  const bool diag_or_mount_diag =
      (mode == SM6125_EMMC_BLOCK_MODE_DIAG || mode == SM6125_EMMC_BLOCK_MODE_MOUNT_DIAG);

  const kern_return_t kext_rc = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
  const OSMetaClass *parent = lookup_meta("IOBlockStorageDevice");
  if (kext_rc != KERN_SUCCESS || !parent || parent->getClassSize() != 0x90u) {
    if (diag_or_mount_diag) {
      panic("SM6125-EMMC-BLK ok=0 err=iostorage-abi krc=%d sz=%u",
            kext_rc, parent ? parent->getClassSize() : 0u);
    }
    IOLog("Sm6125EmmcBlock: IOStorageFamily unavailable (rc=%d)\n", kext_rc);
    return;
  }

  if (!g_classes_registered) {
    g_sm6125_storage_parent = parent;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
      if (diag_or_mount_diag) {
        panic("SM6125-EMMC-BLK ok=0 err=premodload");
      }
      return;
    }
    sm6125_ioblockstoragedevice_static_init();
    sm6125_emmc_block_static_init();
    const OSReturn post_rc = OSMetaClass::postModLoad(handle);
    if (post_rc != kOSReturnSuccess) {
      if (diag_or_mount_diag) {
        panic("SM6125-EMMC-BLK ok=0 err=postmodload rc=%d", post_rc);
      }
      return;
    }
    g_classes_registered = true;
  }

  uint64_t gcc_pa = 0;
  uint64_t sdhci_pa = 0;
  if (!read_dt_geometry(&gcc_pa, &sdhci_pa) ||
      gcc_pa > (UINT64_MAX - SM6125_GCC_SDCC1_PAGE_OFFSET - SM6125_GCC_SDCC1_WINDOW_SIZE)) {
    if (diag_or_mount_diag) {
      panic("SM6125-EMMC-BLK ok=0 err=dt-geometry");
    }
    return;
  }

  auto *ctrl = static_cast<Sm6125SharedEmmcCtrl *>(IOMallocZero(sizeof(Sm6125SharedEmmcCtrl)));
  uint8_t *shng_hdr = static_cast<uint8_t *>(IOMallocZero(SM6125_SHNGROOT_HEADER_BYTES));
  if (!ctrl || !shng_hdr) {
    if (diag_or_mount_diag) {
      panic("SM6125-EMMC-BLK ok=0 err=alloc-ctrl");
    }
    return;
  }
  ctrl->block_mode = mode;
  ctrl->gate = IOLockAlloc();
  ctrl->bounce_buf = static_cast<uint8_t *>(IOMallocZero(kBounceBufBytes));
  if (!ctrl->gate || !ctrl->bounce_buf ||
      !map_mmio_window(
          gcc_pa + SM6125_GCC_SDCC1_PAGE_OFFSET,
          SM6125_GCC_SDCC1_WINDOW_SIZE,
          &ctrl->gcc_desc,
          &ctrl->gcc_map) ||
      !map_mmio_window(
          sdhci_pa,
          SM6125_SDHCI_MIN_REGION_SIZE,
          &ctrl->sdhci_desc,
          &ctrl->sdhci_map)) {
    if (diag_or_mount_diag) {
      panic("SM6125-EMMC-BLK ok=0 err=map-mmio");
    }
    return;
  }

  sm6125_emmc_dma_buf_t dma = {};
  const bool has_dma = alloc_ctrl_dma(ctrl, &dma);
  const uintptr_t gcc_va = static_cast<uintptr_t>(ctrl->gcc_map->getVirtualAddress());
  const uintptr_t sdhci_va = static_cast<uintptr_t>(ctrl->sdhci_map->getVirtualAddress());

  const int32_t init_rc = sm6125_emmc_ctrl_init(
      &ctrl->core, &kBlockHwOps, gcc_va, sdhci_va, has_dma ? &dma : nullptr);
  if (init_rc != SM6125_EMMC_RC_OK || !ctrl->core.online) {
    if (diag_or_mount_diag) {
      panic("SM6125-EMMC-BLK ok=0 err=ctrl-init step=%u rc=%d",
            ctrl->core.step, init_rc);
    }
    return;
  }
  g_shared_ctrl = ctrl;

  /* Read first 4096 bytes (8 sectors) of userdata to probe SHNGROOT manifest. */
  const uint64_t ud_sectors = ctrl->core.userdata_last - ctrl->core.userdata_first + 1ULL;
  const uint64_t ud_bytes = ud_sectors * 512ULL;
  const int32_t hdr_read_rc = sm6125_emmc_ctrl_read_sectors(
      &ctrl->core, ctrl->core.userdata_first, 8u, shng_hdr);

  sm6125_shngroot_manifest_t manifest;
  memset(&manifest, 0, sizeof(manifest));
  int32_t shng_rc = SM6125_SHNGROOT_RC_BAD_ARG;
  if (hdr_read_rc == SM6125_EMMC_RC_OK) {
    shng_rc = sm6125_shngroot_parse(shng_hdr, ud_bytes, blk_sha256, &manifest);
  }
  IOFree(shng_hdr, SM6125_SHNGROOT_HEADER_BYTES);
  shng_hdr = nullptr;

  const bool has_shngroot = (shng_rc == SM6125_SHNGROOT_RC_OK);
  uint32_t published_devs = 0;
  Sm6125EmmcBlock *first_dev = nullptr;

  if (has_shngroot) {
    /* Publish SystemOS first and OS last so SystemOS is already ready when root mounts. */
    static const char *const kPublishOrder[2] = {"SystemOS", "OS"};
    for (unsigned ord = 0; ord < 2u; ++ord) {
      const char *win_name = kPublishOrder[ord];
      const sm6125_shngroot_entry_t *win_ent =
          find_shngroot_entry(manifest, win_name, SM6125_SHNGROOT_FLAG_RO_WINDOW);
      if (!win_ent) {
        continue;
      }
      char vroot_name[32] = {};
      snprintf(vroot_name, sizeof(vroot_name), "%s.vroot", win_name);
      const sm6125_shngroot_entry_t *vroot_ent =
          find_shngroot_entry(manifest, vroot_name, SM6125_SHNGROOT_FLAG_OBJECT);
      Sm6125EmmcBlock *dev = publish_verified_window_device(ctrl, *win_ent, vroot_ent);
      if (!dev) {
        if (diag_or_mount_diag) {
          panic("SM6125-EMMC-BLK ok=0 err=vroot-open win=%s shng_v=%u",
                win_name, manifest.version);
        }
        return;
      }
      if (!first_dev) {
        first_dev = dev;
      }
      published_devs++;
    }
  } else if (mode == SM6125_EMMC_BLOCK_MODE_DIAG) {
    /* Before flashing userdata-root.img, publish whole read-only eMMC so Apple's
     * IOBlockStorageDriver + IOGUIDPartitionScheme + IOMediaBSDClient attach and parse real GPT! */
    first_dev = publish_raw_window_device(ctrl, "whole-emmc", 0ULL, ctrl->core.sec_count);
    if (first_dev) {
      published_devs = 1u;
    }
  } else {
    if (diag_or_mount_diag) {
      panic("SM6125-EMMC-BLK ok=0 err=no-shngroot rc=%d", shng_rc);
    }
    IOLog("Sm6125EmmcBlock: no valid SHNGROOT manifest in userdata (rc=%d)\n", shng_rc);
    return;
  }

  const bool wr_refused = first_dev ? verify_write_protection_refusal(first_dev) : false;

  IokitStorageObservation obs = {};
  for (unsigned poll = 0; poll < 160u; poll++) {
    observe_iokit_storage(has_shngroot, &obs);
    if (has_shngroot) {
      if (obs.saw_driver && obs.os_auth && obs.sysos_auth) {
        break;
      }
    } else {
      if (obs.saw_driver && obs.part_count >= 30u && obs.target_bsd[0] != '\0') {
        break;
      }
    }
    IOSleep(50);
  }
  strlcpy(ctrl->os_bsd, obs.os_bsd, sizeof(ctrl->os_bsd));
  strlcpy(ctrl->sysos_bsd, obs.sysos_bsd, sizeof(ctrl->sysos_bsd));

  if (mode == SM6125_EMMC_BLOCK_MODE_PUBLISH) {
    return;
  }

  const uint32_t reqs = __atomic_load_n(&ctrl->io_requests, __ATOMIC_RELAXED);
  const uint32_t errs = __atomic_load_n(&ctrl->io_errors, __ATOMIC_RELAXED);
  const uint32_t wrej = __atomic_load_n(&ctrl->write_rejects, __ATOMIC_RELAXED);
  const uint32_t vr_blks = __atomic_load_n(&ctrl->vr_verified_blocks, __ATOMIC_RELAXED);
  const uint32_t vr_errs = __atomic_load_n(&ctrl->vr_integrity_errors, __ATOMIC_RELAXED);
  const uint64_t secs = __atomic_load_n(&ctrl->io_sectors, __ATOMIC_RELAXED);
  const uint64_t us = __atomic_load_n(&ctrl->io_time_us, __ATOMIC_RELAXED);
  const uint32_t kib = static_cast<uint32_t>((secs * 512ULL) / 1024ULL);
  const uint32_t kibs = (us > 0ULL)
      ? static_cast<uint32_t>(((secs * 512ULL / 1024ULL) * 1000000ULL) / us)
      : 0u;

  const bool all_ok =
      published_devs > 0u &&
      wr_refused &&
      wrej >= 1u &&
      errs == 0u &&
      vr_errs == 0u &&
      reqs > 0u &&
      obs.saw_driver &&
      obs.media_count >= 2u &&
      obs.bsd_count >= 2u &&
      (has_shngroot ? (obs.os_auth && obs.sysos_auth && vr_blks > 0u)
                    : (obs.target_bsd[0] != '\0'));

  if (mode == SM6125_EMMC_BLOCK_MODE_MOUNT_DIAG && all_ok) {
    /* Return to bsd_init() so vfs_mountroot() and load_init_program() mount OS + SystemOS! */
    return;
  }

  panic("SM6125-EMMC-BLK ok=%u mode=%s shng=%u devs=%u rc=%d\n"
        "IO req=%u sec=%llu kib=%u us=%llu kibs=%u err=%u wr_rej=%u\n"
        "VR blk=%u err=%u os=%s(%u) sys=%s(%u)\n"
        "IOKIT drv=%u media=%u bsd=%u root=%s ud=%s parts=%u",
        all_ok ? 1u : 0u,
        blk_mode_name(ctrl->core.active_mode),
        has_shngroot ? manifest.version : 0u,
        published_devs,
        init_rc,
        reqs,
        static_cast<unsigned long long>(secs),
        kib,
        static_cast<unsigned long long>(us),
        kibs,
        errs,
        wrej,
        vr_blks,
        vr_errs,
        obs.os_bsd[0] ? obs.os_bsd : "none",
        obs.os_auth ? 1u : 0u,
        obs.sysos_bsd[0] ? obs.sysos_bsd : "none",
        obs.sysos_auth ? 1u : 0u,
        obs.saw_driver ? 1u : 0u,
        obs.media_count,
        obs.bsd_count,
        obs.first_bsd[0] ? obs.first_bsd : "none",
        obs.target_bsd[0] ? obs.target_bsd : "none",
        obs.part_count);
}

extern "C" void sm6125_emmc_post_mount_check(
    int cache_error,
    int launchd_ok,
    int dyld_ok,
    int dsc_ok)
{
  Sm6125SharedEmmcCtrl *ctrl = g_shared_ctrl;
  if (!ctrl || ctrl->block_mode != SM6125_EMMC_BLOCK_MODE_MOUNT_DIAG) {
    return;
  }

  const uint32_t reqs = __atomic_load_n(&ctrl->io_requests, __ATOMIC_RELAXED);
  const uint32_t errs = __atomic_load_n(&ctrl->io_errors, __ATOMIC_RELAXED);
  const uint32_t vr_blks = __atomic_load_n(&ctrl->vr_verified_blocks, __ATOMIC_RELAXED);
  const uint32_t vr_errs = __atomic_load_n(&ctrl->vr_integrity_errors, __ATOMIC_RELAXED);
  const uint64_t secs = __atomic_load_n(&ctrl->io_sectors, __ATOMIC_RELAXED);
  const uint64_t us = __atomic_load_n(&ctrl->io_time_us, __ATOMIC_RELAXED);
  const uint32_t kib = static_cast<uint32_t>((secs * 512ULL) / 1024ULL);
  const uint32_t kibs = (us > 0ULL)
      ? static_cast<uint32_t>(((secs * 512ULL / 1024ULL) * 1000000ULL) / us)
      : 0u;

  const uint32_t man_tag =
      (static_cast<uint32_t>(native_owner_boot_manifest_hash[0]) << 24u) |
      (static_cast<uint32_t>(native_owner_boot_manifest_hash[1]) << 16u) |
      (static_cast<uint32_t>(native_owner_boot_manifest_hash[2]) << 8u) |
      static_cast<uint32_t>(native_owner_boot_manifest_hash[3]);

  const bool all_ok =
      cache_error == 0 &&
      native_owner_root_accepted == 1 &&
      native_owner_cache_stage == 6 &&
      native_owner_cache_status == 0 &&
      native_owner_cache_native_union == 1 &&
      native_owner_boot_manifest_status == 0 &&
      launchd_ok == 1 &&
      dyld_ok == 1 &&
      dsc_ok == 1 &&
      errs == 0u &&
      vr_errs == 0u &&
      vr_blks > 0u;

  panic("SM6125-ROOT-MNT ok=%u os=/dev/%s sys=%s union=%d\n"
        "AUTH root=%d(ap=%d) sys_rc=%d stg=%d man=%d(%08x)\n"
        "VFS launchd=%d dyld=%d dsc=%d\n"
        "VR blk=%u err=%u IO req=%u kib=%u kibs=%u io_err=%u",
        all_ok ? 1u : 0u,
        ctrl->os_bsd[0] ? ctrl->os_bsd : "none",
        native_owner_cache_device_path[0] ? native_owner_cache_device_path : "none",
        native_owner_cache_native_union,
        native_owner_root_accepted,
        native_owner_root_apple_autherr,
        cache_error,
        native_owner_cache_stage,
        native_owner_boot_manifest_status,
        man_tag,
        launchd_ok,
        dyld_ok,
        dsc_ok,
        vr_blks,
        vr_errs,
        reqs,
        kib,
        kibs,
        errs);
}
