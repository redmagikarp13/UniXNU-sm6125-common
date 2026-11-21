// SPDX-License-Identifier: MIT
// Driver de bloco somente-leitura IOBlockStorageDevice para eMMC SM6125 (SDCC1 DDR52 + 32-bit ADMA2).
#include "Sm6125EmmcCore.h"
#include "Sm6125EmmcReadOnly.h"
#include "Sm6125ShngRoot.h"
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

extern "C" {

extern kmod_info_t g_kernel_kmod_info;
kern_return_t OSKextLoadKextWithIdentifier(const char *kext_id);

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
  volatile uint32_t io_requests;
  volatile uint32_t io_errors;
  volatile uint32_t write_rejects;
  volatile uint64_t io_sectors;
  volatile uint64_t io_time_us;
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

}  // namespace

class Sm6125EmmcBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(Sm6125EmmcBlock)

 private:
  Sm6125SharedEmmcCtrl *ctrl_ = nullptr;
  uint64_t baseLba_ = 0;
  uint64_t sectorCount_ = 0;
  bool online_ = false;
  char windowName_[32] = {};
  char productName_[48] = {};

 public:
  bool configureWindow(
      Sm6125SharedEmmcCtrl *ctrl,
      const char *name,
      uint64_t base_lba,
      uint64_t sector_count)
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
    strlcpy(windowName_, name, sizeof(windowName_));
    snprintf(productName_, sizeof(productName_), "SM6125 eMMC %s", windowName_);
    setName(productName_);
    setProperty("SM6125Window", windowName_);
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
    return const_cast<char *>("SDCC1 DDR52 ADMA2 RO");
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

    uint64_t actual_bytes = 0;
    IOReturn status = kIOReturnSuccess;

    IOLockLock(ctrl_->gate);
    const uint64_t t0 = blk_now_us();
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
    }
    const uint64_t t1 = blk_now_us();
    if (status == kIOReturnSuccess) {
      __atomic_fetch_add(&ctrl_->io_requests, 1u, __ATOMIC_RELAXED);
      __atomic_fetch_add(&ctrl_->io_sectors, done_sectors, __ATOMIC_RELAXED);
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
    } else if (uuid && uuid->getLength() == 36) {
      const char *ustr = uuid->getCStringNoCopy();
      if (strcmp(ustr, "04769D26-2CA0-4825-B443-E2E5F88300F2") == 0 ||
          obs->target_uuid[0] == '\0') {
        strlcpy(obs->target_uuid, ustr, sizeof(obs->target_uuid));
        if (bsd && bsd->getLength() > 0) {
          strlcpy(obs->target_bsd, bsd->getCStringNoCopy(), sizeof(obs->target_bsd));
        }
      }
    }
  }
  iter->release();
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

static Sm6125EmmcBlock *publish_window_device(
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
  if (!dev->init(nullptr) || !dev->configureWindow(ctrl, name, base_lba, sector_count)) {
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

  const kern_return_t kext_rc = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
  const OSMetaClass *parent = lookup_meta("IOBlockStorageDevice");
  if (kext_rc != KERN_SUCCESS || !parent || parent->getClassSize() != 0x90u) {
    if (mode == SM6125_EMMC_BLOCK_MODE_DIAG) {
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
      if (mode == SM6125_EMMC_BLOCK_MODE_DIAG) {
        panic("SM6125-EMMC-BLK ok=0 err=premodload");
      }
      return;
    }
    sm6125_ioblockstoragedevice_static_init();
    sm6125_emmc_block_static_init();
    const OSReturn post_rc = OSMetaClass::postModLoad(handle);
    if (post_rc != kOSReturnSuccess) {
      if (mode == SM6125_EMMC_BLOCK_MODE_DIAG) {
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
    if (mode == SM6125_EMMC_BLOCK_MODE_DIAG) {
      panic("SM6125-EMMC-BLK ok=0 err=dt-geometry");
    }
    return;
  }

  auto *ctrl = static_cast<Sm6125SharedEmmcCtrl *>(IOMallocZero(sizeof(Sm6125SharedEmmcCtrl)));
  uint8_t *shng_hdr = static_cast<uint8_t *>(IOMallocZero(SM6125_SHNGROOT_HEADER_BYTES));
  if (!ctrl || !shng_hdr) {
    if (mode == SM6125_EMMC_BLOCK_MODE_DIAG) {
      panic("SM6125-EMMC-BLK ok=0 err=alloc-ctrl");
    }
    return;
  }
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
    if (mode == SM6125_EMMC_BLOCK_MODE_DIAG) {
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
    if (mode == SM6125_EMMC_BLOCK_MODE_DIAG) {
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
    for (uint32_t i = 0; i < manifest.entry_count; i++) {
      const sm6125_shngroot_entry_t &ent = manifest.entries[i];
      if (ent.flags != SM6125_SHNGROOT_FLAG_RO_WINDOW) {
        continue;
      }
      const uint64_t win_base = ctrl->core.userdata_first + (ent.offset / 512ULL);
      const uint64_t win_secs = ent.size / 512ULL;
      Sm6125EmmcBlock *dev = publish_window_device(ctrl, ent.name, win_base, win_secs);
      if (dev) {
        if (!first_dev) {
          first_dev = dev;
        }
        published_devs++;
      }
    }
  } else if (mode == SM6125_EMMC_BLOCK_MODE_DIAG) {
    /* Before flashing userdata-root.img, publish whole read-only eMMC so Apple's
     * IOBlockStorageDriver + IOGUIDPartitionScheme + IOMediaBSDClient attach and parse real GPT! */
    first_dev = publish_window_device(ctrl, "whole-emmc", 0ULL, ctrl->core.sec_count);
    if (first_dev) {
      published_devs = 1u;
    }
  } else {
    IOLog("Sm6125EmmcBlock: no valid SHNGROOT manifest in userdata (rc=%d)\n", shng_rc);
    return;
  }

  if (mode == SM6125_EMMC_BLOCK_MODE_PUBLISH) {
    return;
  }

  const bool wr_refused = first_dev ? verify_write_protection_refusal(first_dev) : false;

  IokitStorageObservation obs = {};
  for (unsigned poll = 0; poll < 100u; poll++) {
    observe_iokit_storage(has_shngroot, &obs);
    if (has_shngroot) {
      if (obs.saw_driver && obs.bsd_count >= published_devs && obs.target_uuid[0] != '\0') {
        break;
      }
    } else {
      if (obs.saw_driver && obs.part_count >= 30u && obs.target_bsd[0] != '\0') {
        break;
      }
    }
    IOSleep(50);
  }

  const uint32_t reqs = __atomic_load_n(&ctrl->io_requests, __ATOMIC_RELAXED);
  const uint32_t errs = __atomic_load_n(&ctrl->io_errors, __ATOMIC_RELAXED);
  const uint32_t wrej = __atomic_load_n(&ctrl->write_rejects, __ATOMIC_RELAXED);
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
      reqs > 0u &&
      obs.saw_driver &&
      obs.media_count >= 2u &&
      obs.bsd_count >= 2u &&
      obs.target_bsd[0] != '\0';

  panic("SM6125-EMMC-BLK ok=%u mode=%s shng=%u devs=%u rc=%d\n"
        "IO req=%u sec=%llu kib=%u us=%llu kibs=%u err=%u wr_rej=%u\n"
        "IOKIT drv=%u media=%u bsd=%u root=%s ud=%s parts=%u\n"
        "CARD sec=%08x ud=%08llx..%08llx cfg=%08x hc=%02x hc2=%04x",
        all_ok ? 1u : 0u,
        blk_mode_name(ctrl->core.active_mode),
        has_shngroot ? 1u : 0u,
        published_devs,
        init_rc,
        reqs,
        static_cast<unsigned long long>(secs),
        kib,
        static_cast<unsigned long long>(us),
        kibs,
        errs,
        wrej,
        obs.saw_driver ? 1u : 0u,
        obs.media_count,
        obs.bsd_count,
        obs.first_bsd[0] ? obs.first_bsd : "none",
        obs.target_bsd[0] ? obs.target_bsd : "none",
        obs.part_count,
        ctrl->core.sec_count,
        static_cast<unsigned long long>(ctrl->core.userdata_first),
        static_cast<unsigned long long>(ctrl->core.userdata_last),
        ctrl->core.acfg,
        ctrl->core.hc,
        ctrl->core.hc2);
}
