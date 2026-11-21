// SPDX-License-Identifier: MIT
#include "Sm6125EmmcCore.h"
#include "Sm6125EmmcReadOnly.h"

/* SDHCI Command & Status Bits */
enum {
  SDHCI_CMD_INHIBIT = 0x00000001u,
  SDHCI_DATA_INHIBIT = 0x00000002u,
  SDHCI_SPACE_AVAIL_READ = 0x00000800u,
  SDHCI_PRESENT_DAT0 = 0x00100000u,

  SDHCI_INT_CMD_COMPLETE = 0x00000001u,
  SDHCI_INT_XFER_COMPLETE = 0x00000002u,
  SDHCI_INT_DATA_AVAIL = 0x00000020u,
  SDHCI_INT_CMD_TIMEOUT = 0x00010000u,
  SDHCI_INT_CMD_ERR_MASK = 0x000F0000u,
  SDHCI_INT_DATA_TIMEOUT = 0x00100000u,
  SDHCI_INT_DATA_ERR_MASK = 0x00F00000u,
  SDHCI_INT_AUTO_CMD_ERR = 0x01000000u,
  SDHCI_INT_ADMA_ERR = 0x02000000u,

  SDHCI_CMD_RESP_NONE = 0x00u,
  SDHCI_CMD_RESP_SHORT = 0x02u,
  SDHCI_CMD_RESP_LONG = 0x01u,
  SDHCI_CMD_CRC = 0x08u,
  SDHCI_CMD_INDEX = 0x10u,
  SDHCI_CMD_DATA = 0x20u,

  SDHCI_TM_READ = 0x0010u,
  SDHCI_TM_SINGLE_WRITE_ADMA = 0x0001u,
  SDHCI_TM_SINGLE_ADMA = 0x0011u,
  SDHCI_TM_MULTI_WRITE_ADMA = 0x0027u,
  SDHCI_TM_MULTI_PIO = 0x0036u,
  SDHCI_TM_MULTI_ADMA = 0x0037u,

  SDHCI_HC_HISPD = 0x04u,
  SDHCI_HC_ADMA32 = 0x10u,
  SDHCI_HC_EXT_8BIT = 0x20u,

  SDHCI_HC2_VDD_180 = 0x0008u,
  SDHCI_HC2_UHS_DDR50 = 0x0004u,

  CORE_VENDOR_SPEC_INIT_VAL = 0x00018A9Cu,
  CORE_DDR_CONFIG_POR_VAL = 0x80040873u,
  CORE_PWRSAVE_DLL_BIT = 1u << 3,
  CORE_DLL_PDN_BIT = 1u << 29,
  CORE_DLL_RST_BIT = 1u << 30,

  CORE_PWRCTL_BUS_OFF = 0x01u,
  CORE_PWRCTL_BUS_ON = 0x02u,
  CORE_PWRCTL_IO_LOW = 0x04u,
  CORE_PWRCTL_IO_HIGH = 0x08u,
  CORE_PWRCTL_BUS_SUCCESS = 0x01u,
  CORE_PWRCTL_IO_SUCCESS = 0x04u,

  MMC_R1_ERR_MASK = 0xFDF90000u,
  MMC_R1_SWITCH_ERROR = 0x00000080u,
  MMC_R1_READY_FOR_DATA = 0x00000100u,
  MMC_R1_STATE_MASK = 0x00001E00u,
  MMC_R1_STATE_TRAN = 0x00000800u,

  XFER_MAX_SECTORS = 64u,
  XFER_BUF_BYTES = XFER_MAX_SECTORS * 512u,
  MIN_CANARY_BYTES = 4096u,
  BENCH_PIO_SECTORS = 4096u,   /* 2 MiB */
  BENCH_ADMA_SECTORS = 32768u, /* 16 MiB */
};

typedef struct {
  uint64_t part_lba;
  uint32_t array_bytes;
  uint32_t ext_csd_ro_crc;
} emmc_gpt_meta_t;

typedef struct {
  bool fail_valid;
  uint32_t cmd;
  uint32_t arg;
  uint16_t reg;
  uint32_t is;
  uint32_t ps;
  uint32_t r0;
  uint16_t acmd;
  uint8_t adma_err;
} cmd_fail_t;

static const uint32_t kCrc32Table[256] = {
    0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu, 0xe963a535u, 0x9e6495a3u,
    0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u, 0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u, 0x90bf1d91u,
    0x1db71064u, 0x6ab020f2u, 0xf3b97148u, 0x84be41deu, 0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u,
    0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu, 0x14015c4fu, 0x63066cd9u, 0xfa0f3d63u, 0x8d080df5u,
    0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u, 0xa2677172u, 0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu,
    0x35b5a8fau, 0x42b2986cu, 0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u,
    0x26d930acu, 0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u, 0xcfba9599u, 0xb8bda50fu,
    0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u, 0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du,
    0x76dc4190u, 0x01db7106u, 0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu, 0x9fbfe4a5u, 0xe8b8d433u,
    0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu, 0x086d3d2du, 0x91646c97u, 0xe6635c01u,
    0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu, 0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u,
    0x65b0d9c6u, 0x12b7e950u, 0x8bbeb8eau, 0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u,
    0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u, 0xa4d1c46du, 0xd3d6f4fbu,
    0x4369e96au, 0x346ed9fcu, 0xad678846u, 0xda60b8d0u, 0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u,
    0x5005713cu, 0x270241aau, 0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
    0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u, 0xb7bd5c3bu, 0xc0ba6cadu,
    0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au, 0xead54739u, 0x9dd277afu, 0x04db2615u, 0x73dc1683u,
    0xe3630b12u, 0x94643b84u, 0x0d6d6a3eu, 0x7a6a5aa8u, 0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u,
    0xf00f9344u, 0x8708a3d2u, 0x1e01f268u, 0x6906c2feu, 0xf762575du, 0x806567cbu, 0x196c3671u, 0x6e6b06e7u,
    0xfed41b76u, 0x89d32be0u, 0x10da7a5au, 0x67dd4accu, 0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u,
    0xd6d6a3e8u, 0xa1d1937eu, 0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
    0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u, 0x316e8eefu, 0x4669be79u,
    0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u, 0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu,
    0xc5ba3bbeu, 0xb2bd0b28u, 0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u, 0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du,
    0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au, 0x9c0906a9u, 0xeb0e363fu, 0x72076785u, 0x05005713u,
    0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u, 0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u,
    0x86d3d2d4u, 0xf1d4e242u, 0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u,
    0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu, 0x8f659effu, 0xf862ae69u, 0x616bffd3u, 0x166ccf45u,
    0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u, 0xa7672661u, 0xd06016f7u, 0x4969474du, 0x3e6e77dbu,
    0xaed16a4au, 0xd9d65adcu, 0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
    0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u, 0xcdd70693u, 0x54de5729u, 0x23d967bfu,
    0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u, 0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du,
};

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
  for (size_t i = 0u; i < len; i++) {
    crc = (crc >> 8u) ^ kCrc32Table[(crc ^ data[i]) & 0xFFu];
  }
  return crc;
}

static uint32_t crc32_bytes(const uint8_t *data, size_t len) {
  return ~crc32_update(0xFFFFFFFFu, data, len);
}

static uint16_t rd_le16(const uint8_t *p) {
  return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8u);
}

static uint32_t rd_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
         ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static uint64_t rd_le64(const uint8_t *p) {
  return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32(p + 4u) << 32u);
}

static void wr_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8u) & 0xFFu);
}

static void wr_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8u) & 0xFFu);
  p[2] = (uint8_t)((v >> 16u) & 0xFFu);
  p[3] = (uint8_t)((v >> 24u) & 0xFFu);
}

static void fill_buf(void *ptr, uint8_t val, size_t len) {
  uint8_t *p = (uint8_t *)ptr;
  for (size_t i = 0u; i < len; i++) {
    p[i] = val;
  }
}

static void zero_buf(void *ptr, size_t len) {
  fill_buf(ptr, 0u, len);
}

static void copy_buf(uint8_t *dst, const uint8_t *src, size_t len) {
  for (size_t i = 0u; i < len; i++) {
    dst[i] = src[i];
  }
}

static bool has_efi_sig(const uint8_t *sec) {
  static const uint8_t kSig[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
  for (size_t i = 0u; i < 8u; i++) {
    if (sec[i] != kSig[i]) {
      return false;
    }
  }
  return true;
}

static bool gpt_entry_nonzero(const uint8_t *ent) {
  for (size_t i = 0u; i < 16u; i++) {
    if (ent[i] != 0u) {
      return true;
    }
  }
  return false;
}

static bool gpt_entry_is_userdata(const uint8_t *ent) {
  static const char kTarget[] = "userdata";
  const uint8_t *name = ent + 56u;
  for (size_t i = 0u; i < 8u; i++) {
    if (rd_le16(name + i * 2u) != (uint16_t)(uint8_t)kTarget[i]) {
      return false;
    }
  }
  return rd_le16(name + 16u) == 0u;
}

static void capture_regs(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t gcc_va,
    uintptr_t sdhci_va,
    bool read_sdhci,
    sm6125_emmc_ident_report_t *out) {
  out->acfg = ops->read32(gcc_va, SM6125_GCC_SDCC1_APPS_CFG_RCGR_OFF);
  if (read_sdhci) {
    out->clk = ops->read16(sdhci_va, SM6125_SDHCI_CLOCK_CONTROL);
    out->pwr = ops->read8(sdhci_va, SM6125_SDHCI_POWER_CONTROL);
    out->hc2 = ops->read16(sdhci_va, SM6125_SDHCI_HOST_CONTROL2);
    out->vs = ops->read32(sdhci_va, SM6125_SDHCI_V5_VENDOR_SPEC);
    out->pst = ops->read32(sdhci_va, SM6125_SDHCI_V5_PWRCTL_STATUS);
    out->dll = ops->read32(sdhci_va, SM6125_SDHCI_V5_DLL_CONFIG);
  }
}

static void capture_fast_regs(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t gcc_va,
    uintptr_t sdhci_va,
    bool read_sdhci,
    sm6125_emmc_fast_report_t *out) {
  out->acfg = ops->read32(gcc_va, SM6125_GCC_SDCC1_APPS_CFG_RCGR_OFF);
  if (read_sdhci) {
    out->clk = ops->read16(sdhci_va, SM6125_SDHCI_CLOCK_CONTROL);
    out->hc = ops->read8(sdhci_va, SM6125_SDHCI_HOST_CONTROL);
    out->hc2 = ops->read16(sdhci_va, SM6125_SDHCI_HOST_CONTROL2);
    out->vs = ops->read32(sdhci_va, SM6125_SDHCI_V5_VENDOR_SPEC);
    out->ddr_cfg = ops->read32(sdhci_va, SM6125_SDHCI_V5_DDR_CONFIG);
    out->dll = ops->read32(sdhci_va, SM6125_SDHCI_V5_DLL_CONFIG);
  }
}

static int32_t handle_pwrctl_pending(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t sdhci_va,
    uint32_t *out_seen) {
  uint32_t pst = ops->read32(sdhci_va, SM6125_SDHCI_V5_PWRCTL_STATUS) & 0x0Fu;
  if (pst == 0u) {
    return SM6125_EMMC_RC_OK;
  }
  uint32_t retry = 10u;
  ops->write32(sdhci_va, SM6125_SDHCI_V5_PWRCTL_CLEAR, pst);
  while ((ops->read32(sdhci_va, SM6125_SDHCI_V5_PWRCTL_STATUS) & pst) != 0u) {
    if (retry == 0u) {
      return SM6125_EMMC_RC_PWR_CLEAR_TIMEOUT;
    }
    retry--;
    ops->write32(sdhci_va, SM6125_SDHCI_V5_PWRCTL_CLEAR, pst);
    ops->delay_us(10u);
  }
  uint32_t ack = 0u;
  if ((pst & (CORE_PWRCTL_BUS_ON | CORE_PWRCTL_BUS_OFF)) != 0u) {
    ack |= CORE_PWRCTL_BUS_SUCCESS | CORE_PWRCTL_IO_SUCCESS;
  }
  if ((pst & (CORE_PWRCTL_IO_LOW | CORE_PWRCTL_IO_HIGH)) != 0u) {
    ack |= CORE_PWRCTL_IO_SUCCESS;
  }
  ops->write32(sdhci_va, SM6125_SDHCI_V5_PWRCTL_CTL, ack);
  if (out_seen) {
    *out_seen |= pst;
  }
  return SM6125_EMMC_RC_OK;
}

static int32_t gcc_program_apps_rcg(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t gcc_va,
    uint32_t cfg,
    uint32_t m,
    uint32_t n,
    uint32_t d) {
  ops->write32(gcc_va, SM6125_GCC_SDCC1_APPS_M_OFF, m);
  ops->write32(gcc_va, SM6125_GCC_SDCC1_APPS_N_OFF, n);
  ops->write32(gcc_va, SM6125_GCC_SDCC1_APPS_D_OFF, d);
  ops->write32(gcc_va, SM6125_GCC_SDCC1_APPS_CFG_RCGR_OFF, cfg);
  const uint32_t rcg_cmd = ops->read32(gcc_va, SM6125_GCC_SDCC1_APPS_CMD_RCGR_OFF);
  ops->write32(gcc_va, SM6125_GCC_SDCC1_APPS_CMD_RCGR_OFF, rcg_cmd | 1u);
  for (unsigned i = 0u; i < 500u; i++) {
    if ((ops->read32(gcc_va, SM6125_GCC_SDCC1_APPS_CMD_RCGR_OFF) & 1u) == 0u) {
      return SM6125_EMMC_RC_OK;
    }
    ops->delay_us(1u);
  }
  return SM6125_EMMC_RC_RCG_TIMEOUT;
}

static int32_t sdhci_enable_card_clk(const sm6125_emmc_hw_ops_t *ops, uintptr_t sdhci_va) {
  ops->write16(sdhci_va, SM6125_SDHCI_CLOCK_CONTROL, 0x0001u);
  for (unsigned i = 0u; i < 2000u; i++) {
    if ((ops->read16(sdhci_va, SM6125_SDHCI_CLOCK_CONTROL) & 0x0002u) != 0u) {
      ops->write16(sdhci_va, SM6125_SDHCI_CLOCK_CONTROL, 0x0007u);
      ops->delay_us(1000u);
      return SM6125_EMMC_RC_OK;
    }
    ops->delay_us(10u);
  }
  return SM6125_EMMC_RC_CLK_TIMEOUT;
}

static int32_t init_gcc_400k(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t gcc_va,
    sm6125_emmc_ident_report_t *out) {
  out->step = SM6125_EMMC_STEP_GCC_RESET;
  ops->write32(gcc_va, SM6125_GCC_SDCC1_BCR_OFF, 1u);
  ops->delay_us(200u);
  ops->write32(gcc_va, SM6125_GCC_SDCC1_BCR_OFF, 0u);
  ops->delay_us(200u);

  out->step = SM6125_EMMC_STEP_GCC_AHB;
  ops->write32(gcc_va, SM6125_GCC_SDCC1_AHB_CBCR_OFF, 1u);
  bool ready = false;
  for (unsigned i = 0u; i < 50u; i++) {
    if ((ops->read32(gcc_va, SM6125_GCC_SDCC1_AHB_CBCR_OFF) & 0x80000001u) == 1u) {
      ready = true;
      break;
    }
    ops->delay_us(10u);
  }
  if (!ready) {
    return SM6125_EMMC_RC_AHB_TIMEOUT;
  }

  out->step = SM6125_EMMC_STEP_GCC_RCG_400K;
  const int32_t rc = gcc_program_apps_rcg(ops, gcc_va, 0x2017u, 0x01u, 0xFCu, 0xFBu);
  if (rc != SM6125_EMMC_RC_OK) {
    return rc;
  }

  out->step = SM6125_EMMC_STEP_GCC_APPS;
  ops->write32(gcc_va, SM6125_GCC_SDCC1_APPS_CBCR_OFF, 1u);
  ready = false;
  for (unsigned i = 0u; i < 50u; i++) {
    if ((ops->read32(gcc_va, SM6125_GCC_SDCC1_APPS_CBCR_OFF) & 0x80000001u) == 1u) {
      ready = true;
      break;
    }
    ops->delay_us(10u);
  }
  return ready ? SM6125_EMMC_RC_OK : SM6125_EMMC_RC_APPS_TIMEOUT;
}

static int32_t init_sdhci_host(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t sdhci_va,
    sm6125_emmc_ident_report_t *out) {
  out->step = SM6125_EMMC_STEP_SDHCI_VENDOR_INIT;
  ops->write32(sdhci_va, SM6125_SDHCI_V5_VENDOR_SPEC, CORE_VENDOR_SPEC_INIT_VAL);
  const uint32_t vs3 = ops->read32(sdhci_va, SM6125_SDHCI_V5_VENDOR_SPEC3);
  ops->write32(sdhci_va, SM6125_SDHCI_V5_VENDOR_SPEC3, vs3 & ~CORE_PWRSAVE_DLL_BIT);
  uint32_t dll = ops->read32(sdhci_va, SM6125_SDHCI_V5_DLL_CONFIG);
  ops->write32(sdhci_va, SM6125_SDHCI_V5_DLL_CONFIG, dll | CORE_DLL_RST_BIT);
  dll = ops->read32(sdhci_va, SM6125_SDHCI_V5_DLL_CONFIG);
  ops->write32(sdhci_va, SM6125_SDHCI_V5_DLL_CONFIG, dll | CORE_DLL_PDN_BIT);
  int32_t rc = handle_pwrctl_pending(ops, sdhci_va, NULL);
  if (rc != SM6125_EMMC_RC_OK) {
    return rc;
  }
  ops->write32(sdhci_va, SM6125_SDHCI_V5_PWRCTL_MASK, 0u);

  out->step = SM6125_EMMC_STEP_SDHCI_RESET;
  ops->write8(sdhci_va, SM6125_SDHCI_SOFTWARE_RESET, 0x01u);
  bool ready = false;
  for (unsigned i = 0u; i < 1000u; i++) {
    if ((ops->read8(sdhci_va, SM6125_SDHCI_SOFTWARE_RESET) & 0x01u) == 0u) {
      ready = true;
      break;
    }
    ops->delay_us(10u);
  }
  if (!ready) {
    return SM6125_EMMC_RC_RESET_TIMEOUT;
  }

  out->step = SM6125_EMMC_STEP_SDHCI_PWR_18V;
  ops->write32(sdhci_va, SM6125_SDHCI_V5_VENDOR_SPEC, CORE_VENDOR_SPEC_INIT_VAL);
  ops->write16(sdhci_va, SM6125_SDHCI_HOST_CONTROL2, SDHCI_HC2_VDD_180);
  uint32_t pwr_seen = 0u;
  rc = handle_pwrctl_pending(ops, sdhci_va, &pwr_seen);
  if (rc != SM6125_EMMC_RC_OK) {
    return rc;
  }
  ops->write8(sdhci_va, SM6125_SDHCI_POWER_CONTROL, 0x0Fu);
  for (unsigned i = 0u; i < 5000u; i++) {
    rc = handle_pwrctl_pending(ops, sdhci_va, &pwr_seen);
    if (rc != SM6125_EMMC_RC_OK) {
      return rc;
    }
    if ((pwr_seen & CORE_PWRCTL_BUS_ON) != 0u) {
      break;
    }
    ops->delay_us(10u);
  }
  if ((pwr_seen & CORE_PWRCTL_BUS_ON) == 0u) {
    return SM6125_EMMC_RC_PWR_BUS_TIMEOUT;
  }
  ops->write32(sdhci_va, SM6125_SDHCI_V5_VENDOR_SPEC, CORE_VENDOR_SPEC_INIT_VAL);

  out->step = SM6125_EMMC_STEP_SDHCI_CLK_400K;
  ops->write8(sdhci_va, SM6125_SDHCI_TIMEOUT_CONTROL, 0x0Eu);
  ops->write32(sdhci_va, SM6125_SDHCI_INT_ENABLE, 0x00FF003Fu);
  ops->write32(sdhci_va, SM6125_SDHCI_SIGNAL_ENABLE, 0u);
  return sdhci_enable_card_clk(ops, sdhci_va);
}

static void latch_cmd_fail(
    cmd_fail_t *fail,
    uint32_t cmd,
    uint32_t arg,
    uint16_t reg,
    uint32_t int_stat,
    uint32_t pstate,
    uint32_t r0,
    uint16_t acmd,
    uint8_t adma_err) {
  if (!fail) {
    return;
  }
  fail->fail_valid = true;
  fail->cmd = cmd;
  fail->arg = arg;
  fail->reg = reg;
  fail->is = int_stat;
  fail->ps = pstate;
  fail->r0 = r0;
  fail->acmd = acmd;
  fail->adma_err = adma_err;
}

static void sdhci_reset_cmd_dat(const sm6125_emmc_hw_ops_t *ops, uintptr_t sdhci_va) {
  ops->write8(sdhci_va, SM6125_SDHCI_SOFTWARE_RESET, 0x06u);
  for (unsigned i = 0u; i < 1000u; i++) {
    if ((ops->read8(sdhci_va, SM6125_SDHCI_SOFTWARE_RESET) & 0x06u) == 0u) {
      return;
    }
    ops->delay_us(10u);
  }
}

static int32_t wait_inhibit_clear(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t sdhci_va,
    uint32_t cmd,
    uint32_t arg,
    uint16_t cmd_reg,
    bool has_data,
    cmd_fail_t *fail) {
  const uint32_t inh_mask = has_data ? (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT) : SDHCI_CMD_INHIBIT;
  uint32_t ps = 0u;
  for (unsigned i = 0u; i < 5000u; i++) {
    ps = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
    if ((ps & inh_mask) == 0u) {
      return SM6125_EMMC_RC_OK;
    }
    ops->delay_us(10u);
  }
  latch_cmd_fail(fail, cmd, arg, cmd_reg, 0u, ps, 0u, 0u, 0u);
  return SM6125_EMMC_RC_CMD_INH_TIMEOUT;
}

static int32_t wait_cmd_complete(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t sdhci_va,
    uint32_t cmd,
    uint32_t arg,
    uint16_t cmd_reg,
    uint8_t flags,
    bool check_r1,
    uint32_t resp_out[4],
    uint32_t *out_r0,
    cmd_fail_t *fail) {
  uint32_t is = 0u;
  bool ready = false;
  for (unsigned i = 0u; i < 100000u; i++) {
    is = ops->read32(sdhci_va, SM6125_SDHCI_INT_STATUS);
    if ((is & SDHCI_INT_CMD_ERR_MASK) != 0u) {
      const uint32_t ps = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
      latch_cmd_fail(fail, cmd, arg, cmd_reg, is, ps, 0u, 0u, 0u);
      ops->write32(sdhci_va, SM6125_SDHCI_INT_STATUS, is);
      return (is & SDHCI_INT_CMD_TIMEOUT) ? SM6125_EMMC_RC_CMD_TIMEOUT : SM6125_EMMC_RC_CMD_CRC;
    }
    if ((is & SDHCI_INT_CMD_COMPLETE) != 0u) {
      ready = true;
      break;
    }
    ops->delay_us(10u);
  }
  if (!ready) {
    const uint32_t ps = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
    latch_cmd_fail(fail, cmd, arg, cmd_reg, is, ps, 0u, 0u, 0u);
    return SM6125_EMMC_RC_CMD_TIMEOUT;
  }

  const uint32_t r0 = ops->read32(sdhci_va, SM6125_SDHCI_RESPONSE0);
  if (out_r0) {
    *out_r0 = r0;
  }
  if (resp_out) {
    if ((flags & 0x03u) == SDHCI_CMD_RESP_LONG) {
      const uint32_t r1 = ops->read32(sdhci_va, SM6125_SDHCI_RESPONSE1);
      const uint32_t r2 = ops->read32(sdhci_va, SM6125_SDHCI_RESPONSE2);
      const uint32_t r3 = ops->read32(sdhci_va, SM6125_SDHCI_RESPONSE3);
      resp_out[0] = (r3 << 8u) | (r2 >> 24u);
      resp_out[1] = (r2 << 8u) | (r1 >> 24u);
      resp_out[2] = (r1 << 8u) | (r0 >> 24u);
      resp_out[3] = r0 << 8u;
    } else {
      resp_out[0] = r0;
    }
  }

  if (check_r1 && (r0 & MMC_R1_ERR_MASK) != 0u) {
    const uint32_t ps = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
    latch_cmd_fail(fail, cmd, arg, cmd_reg, is, ps, r0, 0u, 0u);
    ops->write32(sdhci_va, SM6125_SDHCI_INT_STATUS, is);
    return SM6125_EMMC_RC_R1_ERROR;
  }
  return SM6125_EMMC_RC_OK;
}

static int32_t send_cmd_pio(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t sdhci_va,
    uint32_t cmd,
    uint32_t arg,
    uint8_t flags,
    bool check_r1,
    uint32_t resp_out[4],
    uint8_t *sector_out,
    cmd_fail_t *fail) {
  const uint16_t cmd_reg = (uint16_t)(((cmd & 0x3Fu) << 8u) | flags);
  const bool has_data = (sector_out != NULL);

  const uint32_t stale = ops->read32(sdhci_va, SM6125_SDHCI_INT_STATUS);
  if (stale != 0u) {
    ops->write32(sdhci_va, SM6125_SDHCI_INT_STATUS, stale);
  }

  int32_t rc = wait_inhibit_clear(ops, sdhci_va, cmd, arg, cmd_reg, has_data, fail);
  if (rc != SM6125_EMMC_RC_OK) {
    return rc;
  }

  if (has_data) {
    ops->write16(sdhci_va, SM6125_SDHCI_BLOCK_SIZE, 512u);
    ops->write16(sdhci_va, SM6125_SDHCI_BLOCK_COUNT, 1u);
    ops->write16(sdhci_va, SM6125_SDHCI_TRANSFER_MODE, SDHCI_TM_READ);
  }
  ops->write32(sdhci_va, SM6125_SDHCI_ARGUMENT, arg);
  ops->write16(sdhci_va, SM6125_SDHCI_COMMAND, cmd_reg);

  uint32_t r0 = 0u;
  rc = wait_cmd_complete(ops, sdhci_va, cmd, arg, cmd_reg, flags, check_r1, resp_out, &r0, fail);
  if (rc != SM6125_EMMC_RC_OK) {
    return rc;
  }

  if (has_data) {
    uint32_t is = 0u;
    uint32_t ps = 0u;
    bool ready = false;
    for (unsigned i = 0u; i < 100000u; i++) {
      is = ops->read32(sdhci_va, SM6125_SDHCI_INT_STATUS);
      ps = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
      if ((is & SDHCI_INT_DATA_ERR_MASK) != 0u) {
        latch_cmd_fail(fail, cmd, arg, cmd_reg, is, ps, r0, 0u, 0u);
        ops->write32(sdhci_va, SM6125_SDHCI_INT_STATUS, is);
        return (is & SDHCI_INT_DATA_TIMEOUT) ? SM6125_EMMC_RC_DATA_TIMEOUT : SM6125_EMMC_RC_DATA_ERROR;
      }
      if ((is & SDHCI_INT_DATA_AVAIL) != 0u || (ps & SDHCI_SPACE_AVAIL_READ) != 0u) {
        ready = true;
        break;
      }
      ops->delay_us(10u);
    }
    if (!ready) {
      latch_cmd_fail(fail, cmd, arg, cmd_reg, is, ps, r0, 0u, 0u);
      return SM6125_EMMC_RC_DATA_TIMEOUT;
    }

    for (size_t w = 0u; w < 128u; w++) {
      const uint32_t word = ops->read32(sdhci_va, SM6125_SDHCI_BUFFER);
      wr_le32(sector_out + w * 4u, word);
    }

    ready = false;
    for (unsigned i = 0u; i < 20000u; i++) {
      is = ops->read32(sdhci_va, SM6125_SDHCI_INT_STATUS);
      if ((is & SDHCI_INT_DATA_ERR_MASK) != 0u) {
        ps = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
        latch_cmd_fail(fail, cmd, arg, cmd_reg, is, ps, r0, 0u, 0u);
        ops->write32(sdhci_va, SM6125_SDHCI_INT_STATUS, is);
        return (is & SDHCI_INT_DATA_TIMEOUT) ? SM6125_EMMC_RC_DATA_TIMEOUT : SM6125_EMMC_RC_DATA_ERROR;
      }
      if ((is & SDHCI_INT_XFER_COMPLETE) != 0u) {
        ready = true;
        break;
      }
      ops->delay_us(10u);
    }
    if (!ready) {
      ps = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
      latch_cmd_fail(fail, cmd, arg, cmd_reg, is, ps, r0, 0u, 0u);
      return SM6125_EMMC_RC_DATA_TIMEOUT;
    }
  }

  const uint32_t clear_bits = ops->read32(sdhci_va, SM6125_SDHCI_INT_STATUS);
  if (clear_bits != 0u) {
    ops->write32(sdhci_va, SM6125_SDHCI_INT_STATUS, clear_bits);
  }
  return SM6125_EMMC_RC_OK;
}

static int32_t read_sector_cmd17(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t sdhci_va,
    uint64_t lba,
    uint8_t sector[512],
    cmd_fail_t *fail) {
  if (lba > 0xFFFFFFFFULL) {
    return SM6125_EMMC_RC_BAD_ARG;
  }
  return send_cmd_pio(
      ops, sdhci_va, 17u, (uint32_t)lba,
      (uint8_t)(SDHCI_CMD_DATA | SDHCI_CMD_INDEX | SDHCI_CMD_CRC | SDHCI_CMD_RESP_SHORT),
      true, NULL, sector, fail);
}

static bool validate_gpt_header(const uint8_t sec[512], uint64_t expected_lba, uint32_t *out_crc) {
  if (!has_efi_sig(sec)) {
    return false;
  }
  const uint32_t rev = rd_le32(sec + 8u);
  const uint32_t hdr_sz = rd_le32(sec + 12u);
  const uint32_t hdr_crc = rd_le32(sec + 16u);
  const uint64_t cur_lba = rd_le64(sec + 24u);
  if (rev != 0x00010000u || hdr_sz < 92u || hdr_sz > 512u || cur_lba != expected_lba) {
    return false;
  }
  uint8_t hdr_copy[512];
  copy_buf(hdr_copy, sec, hdr_sz);
  zero_buf(hdr_copy + 16u, 4u);
  if (crc32_bytes(hdr_copy, hdr_sz) != hdr_crc) {
    return false;
  }
  if (out_crc) {
    *out_crc = hdr_crc;
  }
  return true;
}

static int32_t ident_and_gpt_core(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t gcc_sdcc1_va,
    uintptr_t sdhci_va,
    sm6125_emmc_ident_report_t *out,
    emmc_gpt_meta_t *meta) {
  cmd_fail_t fail;
  zero_buf(&fail, sizeof(fail));
  zero_buf(out, sizeof(*out));
  if (meta) {
    zero_buf(meta, sizeof(*meta));
  }

  int32_t rc = init_gcc_400k(ops, gcc_sdcc1_va, out);
  if (rc != SM6125_EMMC_RC_OK) {
    out->rc = rc;
    capture_regs(ops, gcc_sdcc1_va, sdhci_va, false, out);
    return rc;
  }

  rc = init_sdhci_host(ops, sdhci_va, out);
  if (rc != SM6125_EMMC_RC_OK) {
    out->rc = rc;
    capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return rc;
  }

  out->step = SM6125_EMMC_STEP_CMD0;
  rc = send_cmd_pio(ops, sdhci_va, 0u, 0u, SDHCI_CMD_RESP_NONE, false, NULL, NULL, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    goto fail_sdhci;
  }
  ops->delay_us(2000u);

  out->step = SM6125_EMMC_STEP_CMD1;
  uint32_t ocr = 0u;
  bool card_ready = false;
  for (unsigned i = 0u; i < 500u; i++) {
    rc = send_cmd_pio(ops, sdhci_va, 1u, 0x40FF8080u, SDHCI_CMD_RESP_SHORT, false, &ocr, NULL, &fail);
    if (rc != SM6125_EMMC_RC_OK) {
      goto fail_sdhci;
    }
    if ((ocr & 0x80000000u) != 0u) {
      card_ready = true;
      break;
    }
    ops->delay_us(2000u);
  }
  out->ocr = ocr;
  out->has_ocr = true;
  if (!card_ready) {
    latch_cmd_fail(&fail, 1u, 0x40FF8080u, 0x0102u, 0u,
                   ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE), ocr, 0u, 0u);
    rc = SM6125_EMMC_RC_CMD_TIMEOUT;
    goto fail_sdhci;
  }
  ops->delay_us(8u);

  out->step = SM6125_EMMC_STEP_CMD2;
  rc = send_cmd_pio(ops, sdhci_va, 2u, 0u,
                    (uint8_t)(SDHCI_CMD_CRC | SDHCI_CMD_RESP_LONG), false, out->cid, NULL, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    goto fail_sdhci;
  }
  out->has_cid = true;

  out->step = SM6125_EMMC_STEP_CMD3;
  rc = send_cmd_pio(ops, sdhci_va, 3u, 0x00010000u,
                    (uint8_t)(SDHCI_CMD_INDEX | SDHCI_CMD_CRC | SDHCI_CMD_RESP_SHORT),
                    true, NULL, NULL, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    goto fail_sdhci;
  }

  out->step = SM6125_EMMC_STEP_CMD9;
  rc = send_cmd_pio(ops, sdhci_va, 9u, 0x00010000u,
                    (uint8_t)(SDHCI_CMD_CRC | SDHCI_CMD_RESP_LONG), false, out->csd, NULL, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    goto fail_sdhci;
  }
  out->has_csd = true;

  out->step = SM6125_EMMC_STEP_CMD7;
  rc = send_cmd_pio(ops, sdhci_va, 7u, 0x00010000u,
                    (uint8_t)(SDHCI_CMD_INDEX | SDHCI_CMD_CRC | SDHCI_CMD_RESP_SHORT),
                    true, NULL, NULL, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    goto fail_sdhci;
  }

  uint8_t sector[512];
  out->step = SM6125_EMMC_STEP_CMD8;
  rc = send_cmd_pio(ops, sdhci_va, 8u, 0u,
                    (uint8_t)(SDHCI_CMD_DATA | SDHCI_CMD_INDEX | SDHCI_CMD_CRC | SDHCI_CMD_RESP_SHORT),
                    true, NULL, sector, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    goto fail_sdhci;
  }
  out->bus_width = sector[183];
  out->hs_timing = sector[185];
  out->ext_csd_rev = sector[192];
  out->device_type = sector[196];
  out->sec_count = rd_le32(&sector[212]);
  out->has_ext_csd = true;
  if (meta) {
    meta->ext_csd_ro_crc = crc32_bytes(&sector[192], 320u);
  }

  out->step = SM6125_EMMC_STEP_READ_LBA0;
  rc = read_sector_cmd17(ops, sdhci_va, 0ULL, sector, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    goto fail_sdhci;
  }
  if (rd_le16(&sector[510]) != 0xAA55u || sector[450] != 0xEEu) {
    rc = SM6125_EMMC_RC_BAD_MBR;
    goto fail_sdhci;
  }
  out->lba0_crc = crc32_bytes(sector, 512u);

  out->step = SM6125_EMMC_STEP_READ_LBA1;
  rc = read_sector_cmd17(ops, sdhci_va, 1ULL, sector, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    goto fail_sdhci;
  }
  if (!validate_gpt_header(sector, 1ULL, &out->gpt_hdr_crc)) {
    rc = SM6125_EMMC_RC_BAD_GPT_HDR;
    goto fail_sdhci;
  }
  const uint64_t bak_lba = rd_le64(sector + 32u);
  const uint64_t part_lba = rd_le64(sector + 72u);
  const uint32_t num_parts = rd_le32(sector + 80u);
  const uint32_t part_sz = rd_le32(sector + 84u);
  const uint32_t expected_parts_crc = rd_le32(sector + 88u);
  if (part_lba < 2ULL || part_lba >= 34ULL || num_parts == 0u || num_parts > 512u || part_sz != 128u) {
    rc = SM6125_EMMC_RC_BAD_GPT_HDR;
    goto fail_sdhci;
  }
  out->lba1_crc = crc32_bytes(sector, 512u);
  out->gpt_parts_crc = expected_parts_crc;

  out->step = SM6125_EMMC_STEP_READ_GPT_ARRAY;
  const uint32_t array_bytes = num_parts * 128u;
  const uint32_t array_sectors = (array_bytes + 511u) / 512u;
  uint32_t running_crc = 0xFFFFFFFFu;
  uint32_t remaining = array_bytes;
  for (uint32_t s = 0u; s < array_sectors; s++) {
    rc = read_sector_cmd17(ops, sdhci_va, part_lba + s, sector, &fail);
    if (rc != SM6125_EMMC_RC_OK) {
      goto fail_sdhci;
    }
    const uint32_t take = remaining < 512u ? remaining : 512u;
    running_crc = crc32_update(running_crc, sector, take);
    remaining -= take;
    const uint32_t entries_in_sec = take / 128u;
    for (uint32_t e = 0u; e < entries_in_sec; e++) {
      const uint8_t *ent = sector + (size_t)e * 128u;
      if (gpt_entry_nonzero(ent) && gpt_entry_is_userdata(ent)) {
        out->has_userdata = true;
        out->userdata_first = rd_le64(ent + 32u);
        out->userdata_last = rd_le64(ent + 40u);
      }
    }
  }
  if (~running_crc != expected_parts_crc) {
    rc = SM6125_EMMC_RC_BAD_GPT_PARTS;
    goto fail_sdhci;
  }
  if (!out->has_userdata) {
    rc = SM6125_EMMC_RC_NO_USERDATA;
    goto fail_sdhci;
  }

  out->step = SM6125_EMMC_STEP_READ_GPT_BACKUP;
  rc = read_sector_cmd17(ops, sdhci_va, bak_lba, sector, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    goto fail_sdhci;
  }
  if (!validate_gpt_header(sector, bak_lba, NULL) || rd_le32(sector + 88u) != expected_parts_crc) {
    rc = SM6125_EMMC_RC_BAD_GPT_BACKUP;
    goto fail_sdhci;
  }

  out->bak_ok = true;
  out->has_gpt = true;
  out->step = SM6125_EMMC_STEP_DONE;
  out->rc = SM6125_EMMC_RC_OK;
  out->ok = true;
  if (meta) {
    meta->part_lba = part_lba;
    meta->array_bytes = array_bytes;
  }
  capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
  return SM6125_EMMC_RC_OK;

fail_sdhci:
  if (fail.fail_valid) {
    out->fail_valid = true;
    out->fail_cmd = fail.cmd;
    out->fail_arg = fail.arg;
    out->fail_reg = fail.reg;
    out->fail_is = fail.is;
    out->fail_ps = fail.ps;
    out->fail_r0 = fail.r0;
  }
  out->rc = rc;
  capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
  return rc;
}

int32_t sm6125_emmc_ident_gpt_run(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t gcc_sdcc1_va,
    uintptr_t sdhci_va,
    sm6125_emmc_ident_report_t *out) {
  if (!ops || !ops->read8 || !ops->read16 || !ops->read32 ||
      !ops->write8 || !ops->write16 || !ops->write32 || !ops->delay_us ||
      !gcc_sdcc1_va || !sdhci_va || !out) {
    return SM6125_EMMC_RC_BAD_ARG;
  }
  return ident_and_gpt_core(ops, gcc_sdcc1_va, sdhci_va, out, NULL);
}

static void record_fast_fail(
    sm6125_emmc_fast_report_t *out,
    uint32_t rung,
    uint32_t step,
    int32_t rc,
    const cmd_fail_t *fail) {
  out->fail_valid = true;
  out->fail_rung = rung;
  out->fail_step = step;
  out->fail_rc = rc;
  if (fail && fail->fail_valid) {
    out->fail_cmd = fail->cmd;
    out->fail_arg = fail->arg;
    out->fail_is = fail->is;
    out->fail_ps = fail->ps;
    out->fail_r0 = fail->r0;
    out->fail_acmd = fail->acmd;
    out->fail_adma_err = fail->adma_err;
  }
}

static int32_t wait_card_tran_ready(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t sdhci_va,
    unsigned max_polls,
    uint32_t poll_delay_us,
    uint32_t extra_err_mask,
    bool switch_mode,
    cmd_fail_t *fail) {
  for (unsigned i = 0u; i < max_polls; i++) {
    uint32_t r0 = 0u;
    const int32_t rc = send_cmd_pio(
        ops, sdhci_va, 13u, 0x00010000u,
        (uint8_t)(SDHCI_CMD_INDEX | SDHCI_CMD_CRC | SDHCI_CMD_RESP_SHORT),
        false, &r0, NULL, fail);
    if (rc != SM6125_EMMC_RC_OK) {
      return rc;
    }
    if ((r0 & (MMC_R1_ERR_MASK | extra_err_mask)) != 0u) {
      latch_cmd_fail(fail, 13u, 0x00010000u, 0x0D1Au,
                     ops->read32(sdhci_va, SM6125_SDHCI_INT_STATUS),
                     ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE),
                     r0, 0u, 0u);
      return switch_mode ? SM6125_EMMC_RC_SWITCH_ERROR : SM6125_EMMC_RC_R1_ERROR;
    }
    const uint32_t ps = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
    if ((r0 & MMC_R1_STATE_MASK) == MMC_R1_STATE_TRAN &&
        (r0 & MMC_R1_READY_FOR_DATA) != 0u &&
        (ps & SDHCI_PRESENT_DAT0) != 0u) {
      return SM6125_EMMC_RC_OK;
    }
    ops->delay_us(poll_delay_us);
  }
  return switch_mode ? SM6125_EMMC_RC_CMD_TIMEOUT : SM6125_EMMC_RC_CARD_BUSY;
}

static int32_t mmc_switch_cmd6(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t sdhci_va,
    uint8_t index,
    uint8_t value,
    cmd_fail_t *fail) {
  /* Hard defense-in-depth allowlist: HS_TIMING=1, BUS_WIDTH=2|6, FLUSH_CACHE=1 */
  const bool is_flush = (index == 32u && value == 1u);
  const bool allowed = (index == 185u && value == 1u) ||
                       (index == 183u && (value == 2u || value == 6u)) ||
                       is_flush;
  if (!allowed) {
    return SM6125_EMMC_RC_BAD_ARG;
  }
  const uint32_t arg = (0x03u << 24u) | ((uint32_t)index << 16u) | ((uint32_t)value << 8u);
  const int32_t rc = send_cmd_pio(
      ops, sdhci_va, 6u, arg,
      (uint8_t)(SDHCI_CMD_INDEX | SDHCI_CMD_CRC | SDHCI_CMD_RESP_SHORT),
      false, NULL, NULL, fail);
  if (rc != SM6125_EMMC_RC_OK) {
    return rc;
  }

  return wait_card_tran_ready(
      ops, sdhci_va, is_flush ? 50000u : 5000u, 10u,
      MMC_R1_SWITCH_ERROR, true, fail);
}

static int32_t switch_controller_bus_speed(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t gcc_va,
    uintptr_t sdhci_va,
    uint32_t cfg_rcgr,
    bool ddr52,
    cmd_fail_t *fail) {
  int32_t rc = wait_inhibit_clear(ops, sdhci_va, 0u, 0u, 0u, true, fail);
  if (rc != SM6125_EMMC_RC_OK) {
    return rc;
  }
  ops->write16(sdhci_va, SM6125_SDHCI_CLOCK_CONTROL, 0x0001u);
  ops->delay_us(10u);
  if (ddr52) {
    ops->write32(sdhci_va, SM6125_SDHCI_V5_DDR_CONFIG, CORE_DDR_CONFIG_POR_VAL);
    ops->write16(sdhci_va, SM6125_SDHCI_HOST_CONTROL2, SDHCI_HC2_VDD_180 | SDHCI_HC2_UHS_DDR50);
  } else {
    ops->write16(sdhci_va, SM6125_SDHCI_HOST_CONTROL2, SDHCI_HC2_VDD_180);
  }
  rc = gcc_program_apps_rcg(ops, gcc_va, cfg_rcgr, 0u, 0u, 0u);
  if (rc != SM6125_EMMC_RC_OK) {
    return rc;
  }
  return sdhci_enable_card_clk(ops, sdhci_va);
}

static int32_t read_multi_pio_stream(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t sdhci_va,
    uint64_t lba,
    uint16_t block_count,
    uint8_t *dst_out,
    uint32_t *running_crc,
    uint32_t *remaining_crc_bytes,
    cmd_fail_t *fail) {
  if (lba > 0xFFFFFFFFULL || block_count == 0u || block_count > XFER_MAX_SECTORS) {
    return SM6125_EMMC_RC_BAD_ARG;
  }
  if (block_count == 1u) {
    uint8_t sec1[512];
    uint8_t *target = dst_out ? dst_out : sec1;
    const int32_t rc1 = read_sector_cmd17(ops, sdhci_va, lba, target, fail);
    if (rc1 != SM6125_EMMC_RC_OK) {
      sdhci_reset_cmd_dat(ops, sdhci_va);
      return rc1;
    }
    if (running_crc && remaining_crc_bytes && *remaining_crc_bytes > 0u) {
      const uint32_t take = *remaining_crc_bytes < 512u ? *remaining_crc_bytes : 512u;
      *running_crc = crc32_update(*running_crc, target, take);
      *remaining_crc_bytes -= take;
    }
    return SM6125_EMMC_RC_OK;
  }
  const uint16_t cmd_reg =
      (uint16_t)((18u << 8u) | SDHCI_CMD_DATA | SDHCI_CMD_INDEX | SDHCI_CMD_CRC | SDHCI_CMD_RESP_SHORT);

  const uint32_t stale = ops->read32(sdhci_va, SM6125_SDHCI_INT_STATUS);
  if (stale != 0u) {
    ops->write32(sdhci_va, SM6125_SDHCI_INT_STATUS, stale);
  }
  int32_t rc = wait_inhibit_clear(ops, sdhci_va, 18u, (uint32_t)lba, cmd_reg, true, fail);
  if (rc != SM6125_EMMC_RC_OK) {
    return rc;
  }

  ops->write16(sdhci_va, SM6125_SDHCI_BLOCK_SIZE, 512u);
  ops->write16(sdhci_va, SM6125_SDHCI_BLOCK_COUNT, block_count);
  ops->write16(sdhci_va, SM6125_SDHCI_TRANSFER_MODE, SDHCI_TM_MULTI_PIO);
  ops->write32(sdhci_va, SM6125_SDHCI_ARGUMENT, (uint32_t)lba);
  ops->write16(sdhci_va, SM6125_SDHCI_COMMAND, cmd_reg);

  uint32_t r0 = 0u;
  rc = wait_cmd_complete(ops, sdhci_va, 18u, (uint32_t)lba, cmd_reg,
                         (uint8_t)(SDHCI_CMD_DATA | SDHCI_CMD_INDEX | SDHCI_CMD_CRC | SDHCI_CMD_RESP_SHORT),
                         true, NULL, &r0, fail);
  if (rc != SM6125_EMMC_RC_OK) {
    return rc;
  }
  /* Clear ONLY Command Complete so Block Read Ready is preserved cleanly. */
  ops->write32(sdhci_va, SM6125_SDHCI_INT_STATUS, SDHCI_INT_CMD_COMPLETE);

  uint8_t sec[512];
  for (uint16_t b = 0u; b < block_count; b++) {
    uint32_t is = 0u;
    uint32_t ps = 0u;
    bool ready = false;
    for (unsigned i = 0u; i < 100000u; i++) {
      is = ops->read32(sdhci_va, SM6125_SDHCI_INT_STATUS);
      ps = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
      if ((is & (SDHCI_INT_DATA_ERR_MASK | SDHCI_INT_AUTO_CMD_ERR)) != 0u) {
        const uint16_t ac = ops->read16(sdhci_va, SM6125_SDHCI_AUTO_CMD_STATUS);
        latch_cmd_fail(fail, 18u, (uint32_t)lba, cmd_reg, is, ps, r0, ac, 0u);
        ops->write32(sdhci_va, SM6125_SDHCI_INT_STATUS, is);
        sdhci_reset_cmd_dat(ops, sdhci_va);
        if ((is & SDHCI_INT_AUTO_CMD_ERR) != 0u) {
          return SM6125_EMMC_RC_AUTO_CMD_ERROR;
        }
        return (is & SDHCI_INT_DATA_TIMEOUT) ? SM6125_EMMC_RC_DATA_TIMEOUT : SM6125_EMMC_RC_DATA_ERROR;
      }
      if ((is & SDHCI_INT_DATA_AVAIL) != 0u || (ps & SDHCI_SPACE_AVAIL_READ) != 0u) {
        ready = true;
        break;
      }
      ops->delay_us(10u);
    }
    if (!ready) {
      latch_cmd_fail(fail, 18u, (uint32_t)lba, cmd_reg, is, ps, r0, 0u, 0u);
      sdhci_reset_cmd_dat(ops, sdhci_va);
      return SM6125_EMMC_RC_DATA_TIMEOUT;
    }

    if ((is & SDHCI_INT_DATA_AVAIL) != 0u) {
      ops->write32(sdhci_va, SM6125_SDHCI_INT_STATUS, SDHCI_INT_DATA_AVAIL);
    }
    uint8_t *blk_dst = dst_out ? (dst_out + (size_t)b * 512u) : sec;
    for (size_t w = 0u; w < 128u; w++) {
      const uint32_t word = ops->read32(sdhci_va, SM6125_SDHCI_BUFFER);
      wr_le32(blk_dst + w * 4u, word);
    }
    if (running_crc && remaining_crc_bytes && *remaining_crc_bytes > 0u) {
      const uint32_t take = *remaining_crc_bytes < 512u ? *remaining_crc_bytes : 512u;
      *running_crc = crc32_update(*running_crc, blk_dst, take);
      *remaining_crc_bytes -= take;
    }
  }

  uint32_t is = 0u;
  bool tc = false;
  for (unsigned i = 0u; i < 20000u; i++) {
    is = ops->read32(sdhci_va, SM6125_SDHCI_INT_STATUS);
    if ((is & (SDHCI_INT_DATA_ERR_MASK | SDHCI_INT_AUTO_CMD_ERR)) != 0u) {
      const uint32_t ps = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
      const uint16_t ac = ops->read16(sdhci_va, SM6125_SDHCI_AUTO_CMD_STATUS);
      latch_cmd_fail(fail, 18u, (uint32_t)lba, cmd_reg, is, ps, r0, ac, 0u);
      ops->write32(sdhci_va, SM6125_SDHCI_INT_STATUS, is);
      sdhci_reset_cmd_dat(ops, sdhci_va);
      if ((is & SDHCI_INT_AUTO_CMD_ERR) != 0u) {
        return SM6125_EMMC_RC_AUTO_CMD_ERROR;
      }
      return (is & SDHCI_INT_DATA_TIMEOUT) ? SM6125_EMMC_RC_DATA_TIMEOUT : SM6125_EMMC_RC_DATA_ERROR;
    }
    if ((is & SDHCI_INT_XFER_COMPLETE) != 0u) {
      tc = true;
      break;
    }
    ops->delay_us(10u);
  }
  if (!tc) {
    latch_cmd_fail(fail, 18u, (uint32_t)lba, cmd_reg, is,
                   ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE), r0, 0u, 0u);
    sdhci_reset_cmd_dat(ops, sdhci_va);
    return SM6125_EMMC_RC_DATA_TIMEOUT;
  }

  const uint32_t rem_bits = ops->read32(sdhci_va, SM6125_SDHCI_INT_STATUS);
  if (rem_bits != 0u) {
    ops->write32(sdhci_va, SM6125_SDHCI_INT_STATUS, rem_bits);
  }
  return SM6125_EMMC_RC_OK;
}

static bool dma_buf_valid(const sm6125_emmc_dma_buf_t *dma) {
  if (!dma || !dma->desc_va || !dma->data_va ||
      !dma->cache_clean_desc || !dma->cache_flush_data || !dma->cache_invalidate_data) {
    return false;
  }
  if (dma->desc_size < 4096u || dma->desc_size > 0x100000u ||
      dma->canary_offset < XFER_BUF_BYTES || dma->canary_offset > 0x1000000u ||
      dma->canary_size < MIN_CANARY_BYTES || dma->canary_size > 0x100000u ||
      dma->data_size < dma->canary_offset + dma->canary_size ||
      dma->data_size > 0x1000000u) {
    return false;
  }
  if ((dma->desc_pa & 3ULL) != 0ULL || (dma->data_pa & 3ULL) != 0ULL ||
      dma->desc_pa == 0ULL || dma->data_pa == 0ULL ||
      dma->desc_pa > 0xFFFFFFFFULL - dma->desc_size + 1ULL ||
      dma->data_pa > 0xFFFFFFFFULL - dma->data_size + 1ULL) {
    return false;
  }
  return true;
}

static int32_t adma_xfer_chunk(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t sdhci_va,
    const sm6125_emmc_dma_buf_t *dma,
    uint64_t lba,
    uint16_t block_count,
    bool is_write,
    bool poison_data,
    const uint8_t *src_in,
    cmd_fail_t *fail) {
  if (lba > 0xFFFFFFFFULL || block_count == 0u || block_count > XFER_MAX_SECTORS) {
    return SM6125_EMMC_RC_BAD_ARG;
  }
  const uint32_t total_bytes = (uint32_t)block_count * 512u;
  const uint32_t cmd_idx = is_write
      ? ((block_count == 1u) ? 24u : 25u)
      : ((block_count == 1u) ? 17u : 18u);
  const uint16_t tm_reg = is_write
      ? ((block_count == 1u) ? SDHCI_TM_SINGLE_WRITE_ADMA : SDHCI_TM_MULTI_WRITE_ADMA)
      : ((block_count == 1u) ? SDHCI_TM_SINGLE_ADMA : SDHCI_TM_MULTI_ADMA);
  const uint16_t cmd_reg =
      (uint16_t)((cmd_idx << 8u) | SDHCI_CMD_DATA | SDHCI_CMD_INDEX | SDHCI_CMD_CRC | SDHCI_CMD_RESP_SHORT);

  const uint32_t stale = ops->read32(sdhci_va, SM6125_SDHCI_INT_STATUS);
  if (stale != 0u) {
    ops->write32(sdhci_va, SM6125_SDHCI_INT_STATUS, stale);
  }
  int32_t rc = wait_inhibit_clear(ops, sdhci_va, cmd_idx, (uint32_t)lba, cmd_reg, true, fail);
  if (rc != SM6125_EMMC_RC_OK) {
    return rc;
  }

  if (is_write) {
    if (!src_in) {
      return SM6125_EMMC_RC_BAD_ARG;
    }
    copy_buf(dma->data_va, src_in, total_bytes);
  } else if (poison_data) {
    fill_buf(dma->data_va, 0x5Au, total_bytes);
  }
  wr_le16(dma->desc_va + 0u, 0x0023u); /* TRAN | END | VALID */
  wr_le16(dma->desc_va + 2u, (uint16_t)total_bytes);
  wr_le32(dma->desc_va + 4u, (uint32_t)dma->data_pa);

  dma->cache_clean_desc(dma->cache_ctx, 0u, 64u);
  dma->cache_flush_data(dma->cache_ctx, 0u, dma->canary_offset + dma->canary_size);

  ops->write32(sdhci_va, SM6125_SDHCI_ADMA_ADDRESS, (uint32_t)dma->desc_pa);
  ops->write32(sdhci_va, SM6125_SDHCI_ADMA_ADDRESS_HI, 0u);
  ops->write16(sdhci_va, SM6125_SDHCI_BLOCK_SIZE, 512u);
  ops->write16(sdhci_va, SM6125_SDHCI_BLOCK_COUNT, block_count);
  ops->write16(sdhci_va, SM6125_SDHCI_TRANSFER_MODE, tm_reg);
  ops->write32(sdhci_va, SM6125_SDHCI_ARGUMENT, (uint32_t)lba);
  ops->write16(sdhci_va, SM6125_SDHCI_COMMAND, cmd_reg);

  uint32_t r0 = 0u;
  uint32_t is = 0u;
  bool cmd_done = false;
  bool xfer_done = false;
  for (unsigned i = 0u; i < 200000u; i++) {
    is = ops->read32(sdhci_va, SM6125_SDHCI_INT_STATUS);
    if ((is & (SDHCI_INT_CMD_ERR_MASK | SDHCI_INT_DATA_ERR_MASK |
               SDHCI_INT_AUTO_CMD_ERR | SDHCI_INT_ADMA_ERR)) != 0u) {
      const uint32_t ps = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
      const uint16_t ac = ops->read16(sdhci_va, SM6125_SDHCI_AUTO_CMD_STATUS);
      const uint8_t ae = ops->read8(sdhci_va, SM6125_SDHCI_ADMA_ERROR);
      latch_cmd_fail(fail, cmd_idx, (uint32_t)lba, cmd_reg, is, ps, r0, ac, ae);
      ops->write32(sdhci_va, SM6125_SDHCI_INT_STATUS, is);
      sdhci_reset_cmd_dat(ops, sdhci_va);
      if ((is & SDHCI_INT_ADMA_ERR) != 0u) {
        return SM6125_EMMC_RC_ADMA_ERROR;
      }
      if ((is & SDHCI_INT_AUTO_CMD_ERR) != 0u) {
        return SM6125_EMMC_RC_AUTO_CMD_ERROR;
      }
      if ((is & SDHCI_INT_CMD_ERR_MASK) != 0u) {
        return (is & SDHCI_INT_CMD_TIMEOUT) ? SM6125_EMMC_RC_CMD_TIMEOUT : SM6125_EMMC_RC_CMD_CRC;
      }
      return (is & SDHCI_INT_DATA_TIMEOUT) ? SM6125_EMMC_RC_DATA_TIMEOUT : SM6125_EMMC_RC_DATA_ERROR;
    }
    if (!cmd_done && (is & SDHCI_INT_CMD_COMPLETE) != 0u) {
      r0 = ops->read32(sdhci_va, SM6125_SDHCI_RESPONSE0);
      cmd_done = true;
      if ((r0 & MMC_R1_ERR_MASK) != 0u) {
        const uint32_t ps = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
        latch_cmd_fail(fail, cmd_idx, (uint32_t)lba, cmd_reg, is, ps, r0, 0u, 0u);
        ops->write32(sdhci_va, SM6125_SDHCI_INT_STATUS, is);
        sdhci_reset_cmd_dat(ops, sdhci_va);
        return SM6125_EMMC_RC_R1_ERROR;
      }
    }
    if (cmd_done && (is & SDHCI_INT_XFER_COMPLETE) != 0u) {
      xfer_done = true;
      break;
    }
    ops->delay_us(5u);
  }
  if (!xfer_done) {
    latch_cmd_fail(fail, cmd_idx, (uint32_t)lba, cmd_reg, is,
                   ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE), r0, 0u, 0u);
    sdhci_reset_cmd_dat(ops, sdhci_va);
    return cmd_done ? SM6125_EMMC_RC_DATA_TIMEOUT : SM6125_EMMC_RC_CMD_TIMEOUT;
  }

  ops->write32(sdhci_va, SM6125_SDHCI_INT_STATUS, is);
  if (is_write) {
    dma->cache_invalidate_data(dma->cache_ctx, dma->canary_offset, dma->canary_size);
  } else {
    dma->cache_invalidate_data(dma->cache_ctx, 0u, dma->canary_offset + dma->canary_size);
  }

  for (size_t i = 0u; i < dma->canary_size; i++) {
    if (dma->data_va[dma->canary_offset + i] != 0xA5u) {
      latch_cmd_fail(fail, cmd_idx, (uint32_t)lba, cmd_reg, is, 0u, r0, 0u, 0u);
      return SM6125_EMMC_RC_CANARY_CORRUPT;
    }
  }

  if (is_write) {
    return wait_card_tran_ready(ops, sdhci_va, 50000u, 10u, 0u, false, fail);
  }
  return SM6125_EMMC_RC_OK;
}

static int32_t read_multi_adma(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t sdhci_va,
    const sm6125_emmc_dma_buf_t *dma,
    uint64_t lba,
    uint16_t block_count,
    bool poison_data,
    uint8_t *dst_out,
    uint32_t *running_crc,
    uint32_t *remaining_crc_bytes,
    cmd_fail_t *fail) {
  const int32_t rc = adma_xfer_chunk(
      ops, sdhci_va, dma, lba, block_count, false, poison_data, NULL, fail);
  if (rc != SM6125_EMMC_RC_OK) {
    return rc;
  }

  const uint32_t total_bytes = (uint32_t)block_count * 512u;
  if (dst_out) {
    copy_buf(dst_out, dma->data_va, total_bytes);
  }

  if (running_crc && remaining_crc_bytes && *remaining_crc_bytes > 0u) {
    const uint32_t take = *remaining_crc_bytes < total_bytes ? *remaining_crc_bytes : total_bytes;
    *running_crc = crc32_update(*running_crc, dma->data_va, take);
    *remaining_crc_bytes -= take;
  }
  return SM6125_EMMC_RC_OK;
}

static uint32_t calc_kibs(uint32_t sectors, uint32_t elapsed_us) {
  if (elapsed_us == 0u) {
    return 0u;
  }
  const uint64_t kib = ((uint64_t)sectors * 512ULL) / 1024ULL;
  return (uint32_t)((kib * 1000000ULL) / (uint64_t)elapsed_us);
}

static int32_t verify_gpt_and_bench(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t sdhci_va,
    const sm6125_emmc_dma_buf_t *dma,
    uint64_t part_lba,
    uint32_t array_bytes,
    uint32_t expected_parts_crc,
    uint64_t userdata_first,
    uint32_t bench_sectors,
    uint32_t gpt_step,
    uint32_t bench_step,
    sm6125_emmc_fast_report_t *out,
    cmd_fail_t *fail,
    uint32_t *out_kibs,
    uint32_t *out_us,
    uint32_t *out_bench_crc) {
  if (dma) {
    fill_buf(dma->data_va + dma->canary_offset, 0xA5u, dma->canary_size);
  }
  out->step = gpt_step;
  const uint32_t array_sectors = (array_bytes + 511u) / 512u;
  uint32_t rem_gpt = array_bytes;
  uint32_t gpt_crc = 0xFFFFFFFFu;
  for (uint32_t done = 0u; done < array_sectors;) {
    const uint32_t left = array_sectors - done;
    const uint16_t chunk = (uint16_t)(left < XFER_MAX_SECTORS ? left : XFER_MAX_SECTORS);
    const int32_t rc = dma
        ? read_multi_adma(ops, sdhci_va, dma, part_lba + done, chunk, true, NULL, &gpt_crc, &rem_gpt, fail)
        : read_multi_pio_stream(ops, sdhci_va, part_lba + done, chunk, NULL, &gpt_crc, &rem_gpt, fail);
    if (rc != SM6125_EMMC_RC_OK) {
      return rc;
    }
    done += chunk;
  }
  if (~gpt_crc != expected_parts_crc) {
    return SM6125_EMMC_RC_BAD_GPT_PARTS;
  }

  out->step = bench_step;
  uint32_t rem_bench = bench_sectors * 512u;
  uint32_t bench_crc = 0xFFFFFFFFu;
  uint64_t io_us = 0ULL;
  for (uint32_t done = 0u; done < bench_sectors;) {
    const uint32_t left = bench_sectors - done;
    const uint16_t chunk = (uint16_t)(left < XFER_MAX_SECTORS ? left : XFER_MAX_SECTORS);
    const uint64_t c0 = ops->now_us();
    const int32_t rc = dma
        ? read_multi_adma(ops, sdhci_va, dma, userdata_first + done, chunk,
                          done == 0u, NULL, NULL, NULL, fail)
        : read_multi_pio_stream(ops, sdhci_va, userdata_first + done, chunk,
                                NULL, NULL, NULL, fail);
    const uint64_t c1 = ops->now_us();
    if (rc != SM6125_EMMC_RC_OK) {
      return rc;
    }
    io_us += (c1 >= c0) ? (c1 - c0) : 0ULL;
    if (dma && out_bench_crc && rem_bench > 0u) {
      const uint32_t chunk_bytes = (uint32_t)chunk * 512u;
      const uint32_t take = rem_bench < chunk_bytes ? rem_bench : chunk_bytes;
      bench_crc = crc32_update(bench_crc, dma->data_va, take);
      rem_bench -= take;
    }
    done += chunk;
  }
  const uint32_t dt = io_us > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)io_us;
  *out_us = dt;
  *out_kibs = calc_kibs(bench_sectors, dt);
  if (out_bench_crc) {
    *out_bench_crc = ~bench_crc;
  }
  return SM6125_EMMC_RC_OK;
}

int32_t sm6125_emmc_fast_bench_run(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t gcc_sdcc1_va,
    uintptr_t sdhci_va,
    const sm6125_emmc_dma_buf_t *dma_buf,
    sm6125_emmc_fast_report_t *out) {
  if (!ops || !ops->read8 || !ops->read16 || !ops->read32 ||
      !ops->write8 || !ops->write16 || !ops->write32 || !ops->delay_us ||
      !ops->now_us || !gcc_sdcc1_va || !sdhci_va || !out) {
    return SM6125_EMMC_RC_BAD_ARG;
  }
  zero_buf(out, sizeof(*out));

  sm6125_emmc_ident_report_t id;
  emmc_gpt_meta_t meta;
  int32_t rc = ident_and_gpt_core(ops, gcc_sdcc1_va, sdhci_va, &id, &meta);
  out->device_type = id.device_type;
  out->ext_csd_ro_crc = meta.ext_csd_ro_crc;
  out->gpt_parts_crc = id.gpt_parts_crc;
  if (rc != SM6125_EMMC_RC_OK) {
    out->step = id.step;
    out->rc = rc;
    out->hs50_pio_rc = rc;
    if (id.fail_valid) {
      cmd_fail_t f = {.fail_valid = true, .cmd = id.fail_cmd, .arg = id.fail_arg,
                      .reg = id.fail_reg, .is = id.fail_is, .ps = id.fail_ps, .r0 = id.fail_r0};
      record_fast_fail(out, 1u, id.step, rc, &f);
    }
    capture_fast_regs(ops, gcc_sdcc1_va, sdhci_va, id.step >= SM6125_EMMC_STEP_SDHCI_VENDOR_INIT, out);
    return rc;
  }

  if (id.userdata_last < id.userdata_first ||
      (id.userdata_last - id.userdata_first + 1ULL) < BENCH_ADMA_SECTORS) {
    out->step = SM6125_EMMC_STEP_READ_GPT_ARRAY;
    out->rc = SM6125_EMMC_RC_NO_USERDATA;
    out->hs50_pio_rc = SM6125_EMMC_RC_NO_USERDATA;
    capture_fast_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return SM6125_EMMC_RC_NO_USERDATA;
  }

  /* Enable ADMA and AutoCMD12 error status reporting in INT_ENABLE. */
  ops->write32(sdhci_va, SM6125_SDHCI_INT_ENABLE, 0x03FF003Fu);

  /* --- RUNG 1: HS 50 MHz + 8-bit SDR + CMD18 PIO (mandatory) --- */
  cmd_fail_t fail;
  zero_buf(&fail, sizeof(fail));

  if ((id.device_type & 0x02u) == 0u) {
    rc = SM6125_EMMC_RC_SWITCH_ERROR;
    out->step = SM6125_EMMC_STEP_HS_CMD6;
    goto fail_rung1;
  }

  out->step = SM6125_EMMC_STEP_HS_CMD6;
  rc = mmc_switch_cmd6(ops, sdhci_va, 185u, 1u, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    goto fail_rung1;
  }

  ops->write8(sdhci_va, SM6125_SDHCI_HOST_CONTROL, SDHCI_HC_HISPD);
  out->step = SM6125_EMMC_STEP_GCC_50M;
  rc = switch_controller_bus_speed(ops, gcc_sdcc1_va, sdhci_va, 0x0000020Bu, false, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    goto fail_rung1;
  }

  out->step = SM6125_EMMC_STEP_HS_8BIT_CMD6;
  rc = mmc_switch_cmd6(ops, sdhci_va, 183u, 2u, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    goto fail_rung1;
  }
  ops->write8(sdhci_va, SM6125_SDHCI_HOST_CONTROL, SDHCI_HC_EXT_8BIT | SDHCI_HC_HISPD);

  uint8_t ext_csd[512];
  rc = send_cmd_pio(ops, sdhci_va, 8u, 0u,
                    (uint8_t)(SDHCI_CMD_DATA | SDHCI_CMD_INDEX | SDHCI_CMD_CRC | SDHCI_CMD_RESP_SHORT),
                    true, NULL, ext_csd, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    goto fail_rung1;
  }
  out->hs_timing = ext_csd[185];
  if (ext_csd[185] != 1u || crc32_bytes(&ext_csd[192], 320u) != meta.ext_csd_ro_crc) {
    rc = SM6125_EMMC_RC_EXT_CSD_MISMATCH;
    goto fail_rung1;
  }

  rc = verify_gpt_and_bench(
      ops, sdhci_va, NULL, meta.part_lba, meta.array_bytes, id.gpt_parts_crc,
      id.userdata_first, BENCH_PIO_SECTORS,
      SM6125_EMMC_STEP_HS_PIO_GPT, SM6125_EMMC_STEP_HS_PIO_BENCH,
      out, &fail, &out->hs50_pio_kibs, &out->hs50_pio_us, NULL);
  if (rc != SM6125_EMMC_RC_OK) {
    goto fail_rung1;
  }
  out->hs50_pio_ok = true;
  out->hs50_pio_rc = SM6125_EMMC_RC_OK;
  out->ok = true;
  out->active_mode = SM6125_EMMC_FAST_MODE_HS50_PIO;

  /* --- RUNG 2: HS 50 MHz + 8-bit SDR + 32-bit ADMA2 (16 MiB) --- */
  if (!dma_buf_valid(dma_buf)) {
    out->hs50_adma_rc = SM6125_EMMC_RC_BAD_ARG;
    out->step = SM6125_EMMC_STEP_FAST_DONE;
    capture_fast_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return SM6125_EMMC_RC_OK;
  }

  zero_buf(&fail, sizeof(fail));
  ops->write8(sdhci_va, SM6125_SDHCI_HOST_CONTROL,
              SDHCI_HC_EXT_8BIT | SDHCI_HC_ADMA32 | SDHCI_HC_HISPD);
  rc = verify_gpt_and_bench(
      ops, sdhci_va, dma_buf, meta.part_lba, meta.array_bytes, id.gpt_parts_crc,
      id.userdata_first, BENCH_ADMA_SECTORS,
      SM6125_EMMC_STEP_HS_ADMA_GPT, SM6125_EMMC_STEP_HS_ADMA_BENCH,
      out, &fail, &out->hs50_adma_kibs, &out->hs50_adma_us, &out->hs50_adma_crc);
  if (rc != SM6125_EMMC_RC_OK) {
    out->hs50_adma_rc = rc;
    record_fast_fail(out, 2u, out->step, rc, &fail);
    ops->write8(sdhci_va, SM6125_SDHCI_HOST_CONTROL, SDHCI_HC_EXT_8BIT | SDHCI_HC_HISPD);
    capture_fast_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return SM6125_EMMC_RC_OK;
  }
  out->hs50_adma_ok = true;
  out->hs50_adma_rc = SM6125_EMMC_RC_OK;
  out->active_mode = SM6125_EMMC_FAST_MODE_HS50_ADMA;

  /* --- RUNG 3: DDR52 (8-bit DDR @ 50 MHz / MCLK 100 MHz + 32-bit ADMA2) --- */
  zero_buf(&fail, sizeof(fail));
  if ((id.device_type & 0x04u) == 0u) {
    out->ddr52_rc = SM6125_EMMC_RC_SWITCH_ERROR;
    out->step = SM6125_EMMC_STEP_FAST_DONE;
    capture_fast_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return SM6125_EMMC_RC_OK;
  }

  out->step = SM6125_EMMC_STEP_DDR_CMD6;
  rc = mmc_switch_cmd6(ops, sdhci_va, 183u, 6u, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    goto fail_rung3;
  }

  out->step = SM6125_EMMC_STEP_GCC_100M;
  rc = switch_controller_bus_speed(ops, gcc_sdcc1_va, sdhci_va, 0x00000205u, true, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    goto fail_rung3;
  }

  rc = verify_gpt_and_bench(
      ops, sdhci_va, dma_buf, meta.part_lba, meta.array_bytes, id.gpt_parts_crc,
      id.userdata_first, BENCH_ADMA_SECTORS,
      SM6125_EMMC_STEP_DDR_ADMA_GPT, SM6125_EMMC_STEP_DDR_ADMA_BENCH,
      out, &fail, &out->ddr52_kibs, &out->ddr52_us, &out->ddr52_crc);
  if (rc != SM6125_EMMC_RC_OK) {
    goto fail_rung3;
  }
  if (out->ddr52_crc != out->hs50_adma_crc) {
    rc = SM6125_EMMC_RC_DATA_MISMATCH;
    goto fail_rung3;
  }

  out->ddr52_ok = true;
  out->ddr52_rc = SM6125_EMMC_RC_OK;
  out->active_mode = SM6125_EMMC_FAST_MODE_DDR52_ADMA;
  out->step = SM6125_EMMC_STEP_FAST_DONE;
  out->rc = SM6125_EMMC_RC_OK;
  capture_fast_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
  return SM6125_EMMC_RC_OK;

fail_rung1:
  out->ok = false;
  out->active_mode = SM6125_EMMC_FAST_MODE_NONE;
  out->rc = rc;
  out->hs50_pio_rc = rc;
  record_fast_fail(out, 1u, out->step, rc, &fail);
  capture_fast_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
  return rc;

fail_rung3:
  out->ddr52_ok = false;
  out->ddr52_rc = rc;
  record_fast_fail(out, 3u, out->step, rc, &fail);
  capture_fast_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
  return SM6125_EMMC_RC_OK;
}

typedef struct {
  char *buf;
  size_t cap;
  size_t len;
} id_fmt_t;

static void id_putc(id_fmt_t *c, char ch) {
  if (c->len + 1u < c->cap) {
    c->buf[c->len] = ch;
    c->len++;
  }
}

static void id_puts(id_fmt_t *c, const char *s) {
  if (!s) {
    return;
  }
  while (*s != '\0') {
    id_putc(c, *s);
    s++;
  }
}

static void id_hex(id_fmt_t *c, uint64_t val, unsigned digits) {
  static const char h[] = "0123456789abcdef";
  unsigned s = digits * 4u;
  while (s > 0u) {
    s -= 4u;
    id_putc(c, h[(val >> s) & 0xFu]);
  }
}

static void id_u32(id_fmt_t *c, uint32_t mag) {
  char tmp[12];
  unsigned n = 0u;
  do {
    tmp[n++] = (char)('0' + (mag % 10u));
    mag /= 10u;
  } while (mag > 0u);
  while (n > 0u) {
    id_putc(c, tmp[--n]);
  }
}

static void id_i32(id_fmt_t *c, int32_t v) {
  uint32_t mag;
  if (v < 0) {
    id_putc(c, '-');
    mag = (uint32_t)(-(v + 1)) + 1u;
  } else {
    mag = (uint32_t)v;
  }
  id_u32(c, mag);
}

size_t sm6125_emmc_ident_format_report(
    const sm6125_emmc_ident_report_t *rep,
    char *buf,
    size_t cap) {
  if (!buf || cap == 0u) {
    return 0u;
  }
  buf[0] = '\0';
  if (!rep || cap == 1u) {
    return 0u;
  }
  id_fmt_t c = {.buf = buf, .cap = cap, .len = 0u};

  id_puts(&c, "SM6125-EMMC-ID ok=");
  id_putc(&c, rep->ok ? '1' : '0');
  id_puts(&c, " step=");
  id_i32(&c, (int32_t)rep->step);
  id_puts(&c, " rc=");
  id_i32(&c, rep->rc);
  id_puts(&c, " cfg=");
  id_hex(&c, rep->acfg, 8u);
  id_puts(&c, " clk=");
  id_hex(&c, rep->clk, 4u);
  id_puts(&c, " pwr=");
  id_hex(&c, rep->pwr, 2u);
  id_puts(&c, " hc2=");
  id_hex(&c, rep->hc2, 4u);
  id_puts(&c, " vs=");
  id_hex(&c, rep->vs, 8u);
  id_puts(&c, " pst=");
  id_hex(&c, rep->pst, 8u);

  if (rep->fail_valid) {
    id_puts(&c, "\nFAIL cmd=");
    id_hex(&c, (rep->fail_cmd / 10u) * 16u + (rep->fail_cmd % 10u), 2u);
    id_puts(&c, " arg=");
    id_hex(&c, rep->fail_arg, 8u);
    id_puts(&c, " reg=");
    id_hex(&c, rep->fail_reg, 4u);
    id_puts(&c, " is=");
    id_hex(&c, rep->fail_is, 8u);
    id_puts(&c, " ps=");
    id_hex(&c, rep->fail_ps, 8u);
    id_puts(&c, " r0=");
    id_hex(&c, rep->fail_r0, 8u);
  }

  if (rep->has_ocr || rep->has_cid || rep->has_csd) {
    id_puts(&c, "\nID ocr=");
    id_hex(&c, rep->ocr, 8u);
    id_puts(&c, " cid=");
    for (unsigned i = 0u; i < 4u; i++) {
      id_hex(&c, rep->cid[i], 8u);
    }
    id_puts(&c, " csd=");
    for (unsigned i = 0u; i < 4u; i++) {
      id_hex(&c, rep->csd[i], 8u);
    }
  }

  if (rep->has_ext_csd) {
    id_puts(&c, "\nEXT sec=");
    id_hex(&c, rep->sec_count, 8u);
    id_puts(&c, " rev=");
    id_hex(&c, rep->ext_csd_rev, 2u);
    id_puts(&c, " typ=");
    id_hex(&c, rep->device_type, 2u);
    id_puts(&c, " bw=");
    id_hex(&c, rep->bus_width, 2u);
    id_puts(&c, " hs=");
    id_hex(&c, rep->hs_timing, 2u);
  }

  if (rep->has_gpt) {
    id_puts(&c, "\nGPT l0=");
    id_hex(&c, rep->lba0_crc, 8u);
    id_puts(&c, " l1=");
    id_hex(&c, rep->lba1_crc, 8u);
    id_puts(&c, " hcrc=");
    id_hex(&c, rep->gpt_hdr_crc, 8u);
    id_puts(&c, " pcrc=");
    id_hex(&c, rep->gpt_parts_crc, 8u);
    id_puts(&c, " bak=");
    id_putc(&c, rep->bak_ok ? '1' : '0');
    id_puts(&c, " ud=");
    id_hex(&c, rep->userdata_first, rep->userdata_first <= 0xFFFFFFFFULL ? 8u : 16u);
    id_puts(&c, "..");
    id_hex(&c, rep->userdata_last, rep->userdata_last <= 0xFFFFFFFFULL ? 8u : 16u);
  }

  c.buf[c.len] = '\0';
  return c.len;
}

static const char *fast_mode_name(uint32_t mode) {
  switch (mode) {
    case SM6125_EMMC_FAST_MODE_HS50_PIO: return "hs50-pio";
    case SM6125_EMMC_FAST_MODE_HS50_ADMA: return "hs50-adma";
    case SM6125_EMMC_FAST_MODE_DDR52_ADMA: return "ddr52-adma";
    default: return "none";
  }
}

size_t sm6125_emmc_fast_format_report(
    const sm6125_emmc_fast_report_t *rep,
    char *buf,
    size_t cap) {
  if (!buf || cap == 0u) {
    return 0u;
  }
  buf[0] = '\0';
  if (!rep || cap == 1u) {
    return 0u;
  }
  id_fmt_t c = {.buf = buf, .cap = cap, .len = 0u};

  id_puts(&c, "SM6125-EMMC-FAST ok=");
  id_putc(&c, rep->ok ? '1' : '0');
  id_puts(&c, " mode=");
  id_puts(&c, fast_mode_name(rep->active_mode));
  id_puts(&c, " step=");
  id_u32(&c, rep->step);
  id_puts(&c, " rc=");
  id_i32(&c, rep->rc);

  id_puts(&c, "\nR1 pio=");
  id_putc(&c, rep->hs50_pio_ok ? '1' : '0');
  id_puts(&c, " kibs=");
  id_u32(&c, rep->hs50_pio_kibs);
  id_puts(&c, " us=");
  id_u32(&c, rep->hs50_pio_us);
  id_puts(&c, " rc=");
  id_i32(&c, rep->hs50_pio_rc);

  id_puts(&c, "\nR2 adma=");
  id_putc(&c, rep->hs50_adma_ok ? '1' : '0');
  id_puts(&c, " kibs=");
  id_u32(&c, rep->hs50_adma_kibs);
  id_puts(&c, " us=");
  id_u32(&c, rep->hs50_adma_us);
  id_puts(&c, " crc=");
  id_hex(&c, rep->hs50_adma_crc, 8u);
  id_puts(&c, " rc=");
  id_i32(&c, rep->hs50_adma_rc);

  id_puts(&c, "\nR3 ddr=");
  id_putc(&c, rep->ddr52_ok ? '1' : '0');
  id_puts(&c, " kibs=");
  id_u32(&c, rep->ddr52_kibs);
  id_puts(&c, " us=");
  id_u32(&c, rep->ddr52_us);
  id_puts(&c, " crc=");
  id_hex(&c, rep->ddr52_crc, 8u);
  id_puts(&c, " rc=");
  id_i32(&c, rep->ddr52_rc);

  id_puts(&c, "\nCARD typ=");
  id_hex(&c, rep->device_type, 2u);
  id_puts(&c, " hs=");
  id_hex(&c, rep->hs_timing, 2u);
  id_puts(&c, " ecrc=");
  id_hex(&c, rep->ext_csd_ro_crc, 8u);
  id_puts(&c, " pcrc=");
  id_hex(&c, rep->gpt_parts_crc, 8u);
  id_puts(&c, " cfg=");
  id_hex(&c, rep->acfg, 8u);
  id_puts(&c, " hc=");
  id_hex(&c, rep->hc, 2u);
  id_puts(&c, " hc2=");
  id_hex(&c, rep->hc2, 4u);
  id_puts(&c, " ddr=");
  id_hex(&c, rep->ddr_cfg, 8u);

  if (rep->fail_valid) {
    id_puts(&c, "\nFAIL r=");
    id_u32(&c, rep->fail_rung);
    id_puts(&c, " st=");
    id_u32(&c, rep->fail_step);
    id_puts(&c, " rc=");
    id_i32(&c, rep->fail_rc);
    id_puts(&c, " c=");
    id_u32(&c, rep->fail_cmd);
    id_puts(&c, " a=");
    id_hex(&c, rep->fail_arg, 8u);
    id_puts(&c, " is=");
    id_hex(&c, rep->fail_is, 8u);
    id_puts(&c, " ps=");
    id_hex(&c, rep->fail_ps, 8u);
    id_puts(&c, " r0=");
    id_hex(&c, rep->fail_r0, 8u);
    id_puts(&c, " ac=");
    id_hex(&c, rep->fail_acmd, 4u);
    id_puts(&c, " ae=");
    id_hex(&c, rep->fail_adma_err, 2u);
  }

  c.buf[c.len] = '\0';
  return c.len;
}

static int32_t verify_mode_gpt_reads(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t sdhci_va,
    const sm6125_emmc_dma_buf_t *dma,
    uint64_t part_lba,
    uint32_t array_bytes,
    uint32_t expected_hdr_crc,
    uint32_t expected_parts_crc,
    cmd_fail_t *fail) {
  if (dma) {
    fill_buf(dma->data_va + dma->canary_offset, 0xA5u, dma->canary_size);
    /* Verify 1-sector CMD17 ADMA2 on LBA 1 (primary GPT header). */
    uint8_t lba1[512];
    uint32_t hdr_crc = 0u;
    const int32_t rc1 = read_multi_adma(
        ops, sdhci_va, dma, 1ULL, 1u, true, lba1, NULL, NULL, fail);
    if (rc1 != SM6125_EMMC_RC_OK) {
      return rc1;
    }
    if (!validate_gpt_header(lba1, 1ULL, &hdr_crc) || hdr_crc != expected_hdr_crc) {
      return SM6125_EMMC_RC_BAD_GPT_HDR;
    }
  }
  const uint32_t array_sectors = (array_bytes + 511u) / 512u;
  uint32_t rem_gpt = array_bytes;
  uint32_t gpt_crc = 0xFFFFFFFFu;
  for (uint32_t done = 0u; done < array_sectors;) {
    const uint32_t left = array_sectors - done;
    const uint16_t chunk = (uint16_t)(left < XFER_MAX_SECTORS ? left : XFER_MAX_SECTORS);
    const int32_t rc = dma
        ? read_multi_adma(ops, sdhci_va, dma, part_lba + done, chunk, true, NULL, &gpt_crc, &rem_gpt, fail)
        : read_multi_pio_stream(ops, sdhci_va, part_lba + done, chunk, NULL, &gpt_crc, &rem_gpt, fail);
    if (rc != SM6125_EMMC_RC_OK) {
      return rc;
    }
    done += chunk;
  }
  if (~gpt_crc != expected_parts_crc) {
    return SM6125_EMMC_RC_BAD_GPT_PARTS;
  }
  return SM6125_EMMC_RC_OK;
}

static void capture_ctrl_regs(sm6125_emmc_ctrl_t *ctrl) {
  ctrl->acfg = ctrl->ops->read32(ctrl->gcc_sdcc1_va, SM6125_GCC_SDCC1_APPS_CFG_RCGR_OFF);
  ctrl->hc = ctrl->ops->read8(ctrl->sdhci_va, SM6125_SDHCI_HOST_CONTROL);
  ctrl->hc2 = ctrl->ops->read16(ctrl->sdhci_va, SM6125_SDHCI_HOST_CONTROL2);
  ctrl->ddr_cfg = ctrl->ops->read32(ctrl->sdhci_va, SM6125_SDHCI_V5_DDR_CONFIG);
}

int32_t sm6125_emmc_ctrl_init(
    sm6125_emmc_ctrl_t *ctrl,
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t gcc_sdcc1_va,
    uintptr_t sdhci_va,
    const sm6125_emmc_dma_buf_t *dma_buf) {
  if (!ctrl || !ops || !ops->read8 || !ops->read16 || !ops->read32 ||
      !ops->write8 || !ops->write16 || !ops->write32 || !ops->delay_us ||
      !gcc_sdcc1_va || !sdhci_va) {
    return SM6125_EMMC_RC_BAD_ARG;
  }
  zero_buf(ctrl, sizeof(*ctrl));
  ctrl->ops = ops;
  ctrl->gcc_sdcc1_va = gcc_sdcc1_va;
  ctrl->sdhci_va = sdhci_va;

  sm6125_emmc_ident_report_t id;
  emmc_gpt_meta_t meta;
  int32_t rc = ident_and_gpt_core(ops, gcc_sdcc1_va, sdhci_va, &id, &meta);
  ctrl->step = id.step;
  ctrl->last_rc = rc;
  ctrl->sec_count = id.sec_count;
  ctrl->device_type = id.device_type;
  ctrl->ext_csd_ro_crc = meta.ext_csd_ro_crc;
  ctrl->gpt_parts_crc = id.gpt_parts_crc;
  ctrl->userdata_first = id.userdata_first;
  ctrl->userdata_last = id.userdata_last;
  if (rc != SM6125_EMMC_RC_OK) {
    return rc;
  }
  if (id.userdata_last < id.userdata_first) {
    ctrl->last_rc = SM6125_EMMC_RC_NO_USERDATA;
    return SM6125_EMMC_RC_NO_USERDATA;
  }

  ops->write32(sdhci_va, SM6125_SDHCI_INT_ENABLE, 0x03FF003Fu);

  cmd_fail_t fail;
  zero_buf(&fail, sizeof(fail));

  if ((id.device_type & 0x02u) == 0u) {
    ctrl->step = SM6125_EMMC_STEP_HS_CMD6;
    ctrl->last_rc = SM6125_EMMC_RC_SWITCH_ERROR;
    return SM6125_EMMC_RC_SWITCH_ERROR;
  }

  ctrl->step = SM6125_EMMC_STEP_HS_CMD6;
  rc = mmc_switch_cmd6(ops, sdhci_va, 185u, 1u, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    ctrl->last_rc = rc;
    return rc;
  }

  ops->write8(sdhci_va, SM6125_SDHCI_HOST_CONTROL, SDHCI_HC_HISPD);
  ctrl->step = SM6125_EMMC_STEP_GCC_50M;
  rc = switch_controller_bus_speed(ops, gcc_sdcc1_va, sdhci_va, 0x0000020Bu, false, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    ctrl->last_rc = rc;
    return rc;
  }

  ctrl->step = SM6125_EMMC_STEP_HS_8BIT_CMD6;
  rc = mmc_switch_cmd6(ops, sdhci_va, 183u, 2u, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    ctrl->last_rc = rc;
    return rc;
  }
  ops->write8(sdhci_va, SM6125_SDHCI_HOST_CONTROL, SDHCI_HC_EXT_8BIT | SDHCI_HC_HISPD);

  uint8_t ext_csd[512];
  rc = send_cmd_pio(ops, sdhci_va, 8u, 0u,
                    (uint8_t)(SDHCI_CMD_DATA | SDHCI_CMD_INDEX | SDHCI_CMD_CRC | SDHCI_CMD_RESP_SHORT),
                    true, NULL, ext_csd, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    ctrl->last_rc = rc;
    return rc;
  }
  ctrl->hs_timing = ext_csd[185];
  ctrl->has_cache_ctrl =
      (ext_csd[33] & 1u) != 0u &&
      ((uint32_t)ext_csd[249] | (uint32_t)ext_csd[250] |
       (uint32_t)ext_csd[251] | (uint32_t)ext_csd[252]) != 0u;
  if (ext_csd[185] != 1u || crc32_bytes(&ext_csd[192], 320u) != meta.ext_csd_ro_crc) {
    ctrl->last_rc = SM6125_EMMC_RC_EXT_CSD_MISMATCH;
    return SM6125_EMMC_RC_EXT_CSD_MISMATCH;
  }

  ctrl->step = SM6125_EMMC_STEP_HS_PIO_GPT;
  rc = verify_mode_gpt_reads(
      ops, sdhci_va, NULL, meta.part_lba, meta.array_bytes,
      id.gpt_hdr_crc, id.gpt_parts_crc, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    ctrl->last_rc = rc;
    return rc;
  }

  ctrl->online = true;
  ctrl->active_mode = SM6125_EMMC_FAST_MODE_HS50_PIO;
  ctrl->last_rc = SM6125_EMMC_RC_OK;

  if (!dma_buf_valid(dma_buf)) {
    ctrl->step = SM6125_EMMC_STEP_FAST_DONE;
    capture_ctrl_regs(ctrl);
    return SM6125_EMMC_RC_OK;
  }

  ctrl->dma = *dma_buf;
  ctrl->has_dma = true;
  fill_buf(ctrl->dma.data_va + ctrl->dma.canary_offset, 0xA5u, ctrl->dma.canary_size);

  ops->write8(sdhci_va, SM6125_SDHCI_HOST_CONTROL,
              SDHCI_HC_EXT_8BIT | SDHCI_HC_ADMA32 | SDHCI_HC_HISPD);
  ctrl->step = SM6125_EMMC_STEP_HS_ADMA_GPT;
  rc = verify_mode_gpt_reads(
      ops, sdhci_va, &ctrl->dma, meta.part_lba, meta.array_bytes,
      id.gpt_hdr_crc, id.gpt_parts_crc, &fail);
  if (rc != SM6125_EMMC_RC_OK) {
    sdhci_reset_cmd_dat(ops, sdhci_va);
    ops->write8(sdhci_va, SM6125_SDHCI_HOST_CONTROL, SDHCI_HC_EXT_8BIT | SDHCI_HC_HISPD);
    ctrl->has_dma = false;
    ctrl->step = SM6125_EMMC_STEP_FAST_DONE;
    capture_ctrl_regs(ctrl);
    return SM6125_EMMC_RC_OK;
  }

  ctrl->active_mode = SM6125_EMMC_FAST_MODE_HS50_ADMA;

  if ((id.device_type & 0x04u) == 0u) {
    ctrl->step = SM6125_EMMC_STEP_FAST_DONE;
    capture_ctrl_regs(ctrl);
    return SM6125_EMMC_RC_OK;
  }

  ctrl->step = SM6125_EMMC_STEP_DDR_CMD6;
  rc = mmc_switch_cmd6(ops, sdhci_va, 183u, 6u, &fail);
  if (rc == SM6125_EMMC_RC_OK) {
    ctrl->step = SM6125_EMMC_STEP_GCC_100M;
    rc = switch_controller_bus_speed(ops, gcc_sdcc1_va, sdhci_va, 0x00000205u, true, &fail);
  }
  if (rc == SM6125_EMMC_RC_OK) {
    ctrl->step = SM6125_EMMC_STEP_DDR_ADMA_GPT;
    rc = verify_mode_gpt_reads(
        ops, sdhci_va, &ctrl->dma, meta.part_lba, meta.array_bytes,
        id.gpt_hdr_crc, id.gpt_parts_crc, &fail);
  }
  if (rc == SM6125_EMMC_RC_OK) {
    ctrl->active_mode = SM6125_EMMC_FAST_MODE_DDR52_ADMA;
    ctrl->step = SM6125_EMMC_STEP_FAST_DONE;
    capture_ctrl_regs(ctrl);
    return SM6125_EMMC_RC_OK;
  }

  /* Clean fallback from DDR52 attempt back to HS50 8-bit SDR */
  sdhci_reset_cmd_dat(ops, sdhci_va);
  (void)switch_controller_bus_speed(ops, gcc_sdcc1_va, sdhci_va, 0x0000020Bu, false, &fail);
  (void)mmc_switch_cmd6(ops, sdhci_va, 183u, 2u, &fail);
  ops->write8(sdhci_va, SM6125_SDHCI_HOST_CONTROL,
              SDHCI_HC_EXT_8BIT | SDHCI_HC_ADMA32 | SDHCI_HC_HISPD);
  if (verify_mode_gpt_reads(
          ops, sdhci_va, &ctrl->dma, meta.part_lba, meta.array_bytes,
          id.gpt_hdr_crc, id.gpt_parts_crc, &fail) == SM6125_EMMC_RC_OK) {
    ctrl->active_mode = SM6125_EMMC_FAST_MODE_HS50_ADMA;
  } else {
    sdhci_reset_cmd_dat(ops, sdhci_va);
    ops->write8(sdhci_va, SM6125_SDHCI_HOST_CONTROL, SDHCI_HC_EXT_8BIT | SDHCI_HC_HISPD);
    ctrl->has_dma = false;
    ctrl->active_mode = SM6125_EMMC_FAST_MODE_HS50_PIO;
  }
  ctrl->step = SM6125_EMMC_STEP_FAST_DONE;
  capture_ctrl_regs(ctrl);
  return SM6125_EMMC_RC_OK;
}

int32_t sm6125_emmc_ctrl_read_sectors(
    sm6125_emmc_ctrl_t *ctrl,
    uint64_t lba,
    uint32_t sector_count,
    uint8_t *dst_va) {
  if (!ctrl || !ctrl->online || !ctrl->ops || !dst_va || sector_count == 0u ||
      lba >= (uint64_t)ctrl->sec_count ||
      (uint64_t)sector_count > (uint64_t)ctrl->sec_count - lba) {
    return SM6125_EMMC_RC_BAD_ARG;
  }

  const bool use_adma =
      ctrl->has_dma &&
      (ctrl->active_mode == SM6125_EMMC_FAST_MODE_DDR52_ADMA ||
       ctrl->active_mode == SM6125_EMMC_FAST_MODE_HS50_ADMA);

  cmd_fail_t fail;
  zero_buf(&fail, sizeof(fail));

  for (uint32_t done = 0u; done < sector_count;) {
    const uint32_t left = sector_count - done;
    const uint16_t chunk = (uint16_t)(left < XFER_MAX_SECTORS ? left : XFER_MAX_SECTORS);
    uint8_t *chunk_dst = dst_va + (size_t)done * 512u;
    const int32_t rc = use_adma
        ? read_multi_adma(ctrl->ops, ctrl->sdhci_va, &ctrl->dma,
                          lba + done, chunk, false, chunk_dst, NULL, NULL, &fail)
        : read_multi_pio_stream(ctrl->ops, ctrl->sdhci_va,
                                lba + done, chunk, chunk_dst, NULL, NULL, &fail);
    if (rc != SM6125_EMMC_RC_OK) {
      ctrl->last_rc = rc;
      return rc;
    }
    done += chunk;
  }
  ctrl->last_rc = SM6125_EMMC_RC_OK;
  return SM6125_EMMC_RC_OK;
}

static bool ctrl_adma_ready(const sm6125_emmc_ctrl_t *ctrl) {
  return ctrl && ctrl->online && ctrl->ops && ctrl->has_dma &&
         (ctrl->active_mode == SM6125_EMMC_FAST_MODE_DDR52_ADMA ||
          ctrl->active_mode == SM6125_EMMC_FAST_MODE_HS50_ADMA);
}

int32_t sm6125_emmc_ctrl_set_shngroot_bounds(
    sm6125_emmc_ctrl_t *ctrl,
    uint64_t shngroot_bytes) {
  if (!ctrl || !ctrl->online) {
    return SM6125_EMMC_RC_BAD_ARG;
  }
  ctrl->shngroot_bounds_set = false;
  ctrl->write_window_armed = false;
  ctrl->shngroot_first_lba = 0u;
  ctrl->shngroot_end_lba = 0u;
  ctrl->write_min_lba = 0u;
  ctrl->write_max_lba = 0u;

  if (shngroot_bytes == 0u || (shngroot_bytes & 511ULL) != 0u ||
      ctrl->userdata_first < 34ULL || ctrl->userdata_last < ctrl->userdata_first ||
      ctrl->userdata_last >= (uint64_t)ctrl->sec_count) {
    ctrl->last_rc = SM6125_EMMC_RC_WRITE_PROTECTED;
    return SM6125_EMMC_RC_WRITE_PROTECTED;
  }

  const uint64_t shng_sectors = shngroot_bytes / 512ULL;
  const uint64_t userdata_sectors = (ctrl->userdata_last - ctrl->userdata_first) + 1ULL;
  if (shng_sectors >= userdata_sectors ||
      userdata_sectors - shng_sectors <= SM6125_EMMC_WRITE_GUARD_SECTORS) {
    ctrl->last_rc = SM6125_EMMC_RC_WRITE_PROTECTED;
    return SM6125_EMMC_RC_WRITE_PROTECTED;
  }

  ctrl->shngroot_first_lba = ctrl->userdata_first;
  ctrl->shngroot_end_lba = ctrl->userdata_first + shng_sectors;
  ctrl->shngroot_bounds_set = true;
  ctrl->last_rc = SM6125_EMMC_RC_OK;
  return SM6125_EMMC_RC_OK;
}

int32_t sm6125_emmc_ctrl_set_write_window(
    sm6125_emmc_ctrl_t *ctrl,
    uint64_t first_lba,
    uint64_t last_lba) {
  if (!ctrl || !ctrl->online || !ctrl->shngroot_bounds_set) {
    if (ctrl) {
      ctrl->write_window_armed = false;
      ctrl->last_rc = SM6125_EMMC_RC_WRITE_PROTECTED;
    }
    return SM6125_EMMC_RC_WRITE_PROTECTED;
  }
  ctrl->write_window_armed = false;
  ctrl->write_min_lba = 0u;
  ctrl->write_max_lba = 0u;

  if (ctrl->userdata_first < 34ULL || ctrl->userdata_last < ctrl->userdata_first ||
      ctrl->userdata_last >= (uint64_t)ctrl->sec_count) {
    ctrl->last_rc = SM6125_EMMC_RC_WRITE_PROTECTED;
    return SM6125_EMMC_RC_WRITE_PROTECTED;
  }

  const uint64_t min_allowed = ctrl->shngroot_end_lba + SM6125_EMMC_WRITE_GUARD_SECTORS;
  if (min_allowed > ctrl->userdata_last ||
      first_lba > last_lba ||
      first_lba < min_allowed ||
      last_lba > ctrl->userdata_last) {
    ctrl->last_rc = SM6125_EMMC_RC_WRITE_PROTECTED;
    return SM6125_EMMC_RC_WRITE_PROTECTED;
  }

  ctrl->write_min_lba = first_lba;
  ctrl->write_max_lba = last_lba;
  ctrl->write_window_armed = true;
  ctrl->last_rc = SM6125_EMMC_RC_OK;
  return SM6125_EMMC_RC_OK;
}

int32_t sm6125_emmc_ctrl_write_sectors(
    sm6125_emmc_ctrl_t *ctrl,
    uint64_t lba,
    uint32_t sector_count,
    const uint8_t *src_va) {
  if (!ctrl || !ctrl->online || !ctrl->ops || !src_va || sector_count == 0u ||
      !ctrl_adma_ready(ctrl)) {
    return SM6125_EMMC_RC_BAD_ARG;
  }
  if (!ctrl->shngroot_bounds_set || !ctrl->write_window_armed ||
      ctrl->userdata_first < 34ULL ||
      ctrl->write_min_lba < ctrl->shngroot_end_lba + SM6125_EMMC_WRITE_GUARD_SECTORS ||
      ctrl->write_max_lba > ctrl->userdata_last ||
      lba < ctrl->write_min_lba ||
      lba > ctrl->write_max_lba ||
      (uint64_t)sector_count - 1ULL > ctrl->write_max_lba - lba) {
    ctrl->last_rc = SM6125_EMMC_RC_WRITE_PROTECTED;
    return SM6125_EMMC_RC_WRITE_PROTECTED;
  }

  cmd_fail_t fail;
  zero_buf(&fail, sizeof(fail));

  for (uint32_t done = 0u; done < sector_count;) {
    const uint32_t left = sector_count - done;
    const uint16_t chunk = (uint16_t)(left < XFER_MAX_SECTORS ? left : XFER_MAX_SECTORS);
    const uint8_t *chunk_src = src_va + (size_t)done * 512u;
    const int32_t rc = adma_xfer_chunk(
        ctrl->ops, ctrl->sdhci_va, &ctrl->dma,
        lba + done, chunk, true, false, chunk_src, &fail);
    if (rc != SM6125_EMMC_RC_OK) {
      ctrl->last_rc = rc;
      return rc;
    }
    done += chunk;
  }
  ctrl->write_req_count += 1ULL;
  ctrl->write_sectors_total += (uint64_t)sector_count;
  ctrl->last_rc = SM6125_EMMC_RC_OK;
  return SM6125_EMMC_RC_OK;
}

int32_t sm6125_emmc_ctrl_flush_cache(sm6125_emmc_ctrl_t *ctrl) {
  if (!ctrl || !ctrl->online || !ctrl->ops) {
    return SM6125_EMMC_RC_BAD_ARG;
  }
  if (!ctrl->shngroot_bounds_set || !ctrl->write_window_armed) {
    ctrl->last_rc = SM6125_EMMC_RC_WRITE_PROTECTED;
    return SM6125_EMMC_RC_WRITE_PROTECTED;
  }

  cmd_fail_t fail;
  zero_buf(&fail, sizeof(fail));
  int32_t rc = SM6125_EMMC_RC_OK;
  if (ctrl->has_cache_ctrl) {
    rc = mmc_switch_cmd6(ctrl->ops, ctrl->sdhci_va, 32u, 1u, &fail);
  } else {
    rc = wait_card_tran_ready(
        ctrl->ops, ctrl->sdhci_va, 50000u, 10u, 0u, false, &fail);
  }
  ctrl->last_rc = rc;
  if (rc == SM6125_EMMC_RC_OK) {
    ctrl->flush_count += 1ULL;
  }
  return rc;
}
