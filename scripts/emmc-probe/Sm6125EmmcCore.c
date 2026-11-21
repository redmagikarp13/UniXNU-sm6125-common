// SPDX-License-Identifier: MIT
#include "Sm6125EmmcCore.h"
#include "Sm6125EmmcReadOnly.h"

/* SDHCI Command & Status Bits */
enum {
  SDHCI_CMD_INHIBIT = 0x00000001u,
  SDHCI_DATA_INHIBIT = 0x00000002u,
  SDHCI_SPACE_AVAIL_READ = 0x00000800u,

  SDHCI_INT_CMD_COMPLETE = 0x00000001u,
  SDHCI_INT_XFER_COMPLETE = 0x00000002u,
  SDHCI_INT_DATA_AVAIL = 0x00000020u,
  SDHCI_INT_CMD_TIMEOUT = 0x00010000u,
  SDHCI_INT_CMD_ERR_MASK = 0x000F0000u,
  SDHCI_INT_DATA_TIMEOUT = 0x00100000u,
  SDHCI_INT_DATA_ERR_MASK = 0x00F00000u,

  SDHCI_CMD_RESP_NONE = 0x00u,
  SDHCI_CMD_RESP_SHORT = 0x02u,
  SDHCI_CMD_RESP_LONG = 0x01u,
  SDHCI_CMD_CRC = 0x08u,
  SDHCI_CMD_INDEX = 0x10u,
  SDHCI_CMD_DATA = 0x20u,

  SDHCI_TM_READ = 0x0010u,

  CORE_VENDOR_SPEC_INIT_VAL = 0x00018A9Cu,
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
};

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
  for (size_t i = 0u; i < len; i++) {
    crc ^= data[i];
    for (unsigned b = 0u; b < 8u; b++) {
      crc = (crc & 1u) ? ((crc >> 1u) ^ 0xEDB88320u) : (crc >> 1u);
    }
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

static void zero_buf(void *ptr, size_t len) {
  uint8_t *p = (uint8_t *)ptr;
  for (size_t i = 0u; i < len; i++) {
    p[i] = 0u;
  }
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
  ops->write32(gcc_va, SM6125_GCC_SDCC1_APPS_M_OFF, 0x01u);
  ops->write32(gcc_va, SM6125_GCC_SDCC1_APPS_N_OFF, 0xFCu);
  ops->write32(gcc_va, SM6125_GCC_SDCC1_APPS_D_OFF, 0xFBu);
  ops->write32(gcc_va, SM6125_GCC_SDCC1_APPS_CFG_RCGR_OFF, 0x2017u);
  const uint32_t rcg_cmd = ops->read32(gcc_va, SM6125_GCC_SDCC1_APPS_CMD_RCGR_OFF);
  ops->write32(gcc_va, SM6125_GCC_SDCC1_APPS_CMD_RCGR_OFF, rcg_cmd | 1u);
  ready = false;
  for (unsigned i = 0u; i < 500u; i++) {
    if ((ops->read32(gcc_va, SM6125_GCC_SDCC1_APPS_CMD_RCGR_OFF) & 1u) == 0u) {
      ready = true;
      break;
    }
    ops->delay_us(1u);
  }
  if (!ready) {
    return SM6125_EMMC_RC_RCG_TIMEOUT;
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
  ops->write16(sdhci_va, SM6125_SDHCI_HOST_CONTROL2, 0x0008u);
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
  ops->write16(sdhci_va, SM6125_SDHCI_CLOCK_CONTROL, 0x0001u);
  ready = false;
  for (unsigned i = 0u; i < 2000u; i++) {
    if ((ops->read16(sdhci_va, SM6125_SDHCI_CLOCK_CONTROL) & 0x0002u) != 0u) {
      ready = true;
      break;
    }
    ops->delay_us(10u);
  }
  if (!ready) {
    return SM6125_EMMC_RC_CLK_TIMEOUT;
  }
  ops->write16(sdhci_va, SM6125_SDHCI_CLOCK_CONTROL, 0x0007u);
  ops->delay_us(1000u);
  return SM6125_EMMC_RC_OK;
}

static void latch_cmd_fail(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t sdhci_va,
    uint32_t cmd,
    uint32_t arg,
    uint16_t reg,
    uint32_t int_stat,
    uint32_t pstate,
    uint32_t r0,
    sm6125_emmc_ident_report_t *out) {
  (void)ops;
  (void)sdhci_va;
  out->fail_valid = true;
  out->fail_cmd = cmd;
  out->fail_arg = arg;
  out->fail_reg = reg;
  out->fail_is = int_stat;
  out->fail_ps = pstate;
  out->fail_r0 = r0;
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
    sm6125_emmc_ident_report_t *out) {
  const uint16_t cmd_reg = (uint16_t)(((cmd & 0x3Fu) << 8u) | flags);
  const bool has_data = (sector_out != NULL);
  const uint32_t inh_mask = has_data ? (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT) : SDHCI_CMD_INHIBIT;

  const uint32_t stale = ops->read32(sdhci_va, SM6125_SDHCI_INT_STATUS);
  if (stale != 0u) {
    ops->write32(sdhci_va, SM6125_SDHCI_INT_STATUS, stale);
  }

  uint32_t ps = 0u;
  bool ready = false;
  for (unsigned i = 0u; i < 5000u; i++) {
    ps = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
    if ((ps & inh_mask) == 0u) {
      ready = true;
      break;
    }
    ops->delay_us(10u);
  }
  if (!ready) {
    latch_cmd_fail(ops, sdhci_va, cmd, arg, cmd_reg, 0u, ps, 0u, out);
    return SM6125_EMMC_RC_CMD_INH_TIMEOUT;
  }

  if (has_data) {
    ops->write16(sdhci_va, SM6125_SDHCI_BLOCK_SIZE, 512u);
    ops->write16(sdhci_va, SM6125_SDHCI_BLOCK_COUNT, 1u);
    ops->write16(sdhci_va, SM6125_SDHCI_TRANSFER_MODE, SDHCI_TM_READ);
  }
  ops->write32(sdhci_va, SM6125_SDHCI_ARGUMENT, arg);
  ops->write16(sdhci_va, SM6125_SDHCI_COMMAND, cmd_reg);

  uint32_t is = 0u;
  ready = false;
  for (unsigned i = 0u; i < 100000u; i++) {
    is = ops->read32(sdhci_va, SM6125_SDHCI_INT_STATUS);
    if ((is & SDHCI_INT_CMD_ERR_MASK) != 0u) {
      ps = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
      latch_cmd_fail(ops, sdhci_va, cmd, arg, cmd_reg, is, ps, 0u, out);
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
    ps = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
    latch_cmd_fail(ops, sdhci_va, cmd, arg, cmd_reg, is, ps, 0u, out);
    return SM6125_EMMC_RC_CMD_TIMEOUT;
  }

  const uint32_t r0 = ops->read32(sdhci_va, SM6125_SDHCI_RESPONSE0);
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
    ps = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
    latch_cmd_fail(ops, sdhci_va, cmd, arg, cmd_reg, is, ps, r0, out);
    ops->write32(sdhci_va, SM6125_SDHCI_INT_STATUS, is);
    return SM6125_EMMC_RC_R1_ERROR;
  }

  if (has_data) {
    ready = false;
    for (unsigned i = 0u; i < 100000u; i++) {
      is = ops->read32(sdhci_va, SM6125_SDHCI_INT_STATUS);
      ps = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
      if ((is & SDHCI_INT_DATA_ERR_MASK) != 0u) {
        latch_cmd_fail(ops, sdhci_va, cmd, arg, cmd_reg, is, ps, r0, out);
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
      latch_cmd_fail(ops, sdhci_va, cmd, arg, cmd_reg, is, ps, r0, out);
      return SM6125_EMMC_RC_DATA_TIMEOUT;
    }

    for (size_t w = 0u; w < 128u; w++) {
      const uint32_t word = ops->read32(sdhci_va, SM6125_SDHCI_BUFFER);
      sector_out[w * 4u + 0u] = (uint8_t)(word & 0xFFu);
      sector_out[w * 4u + 1u] = (uint8_t)((word >> 8u) & 0xFFu);
      sector_out[w * 4u + 2u] = (uint8_t)((word >> 16u) & 0xFFu);
      sector_out[w * 4u + 3u] = (uint8_t)((word >> 24u) & 0xFFu);
    }

    ready = false;
    for (unsigned i = 0u; i < 20000u; i++) {
      is = ops->read32(sdhci_va, SM6125_SDHCI_INT_STATUS);
      if ((is & SDHCI_INT_DATA_ERR_MASK) != 0u) {
        ps = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
        latch_cmd_fail(ops, sdhci_va, cmd, arg, cmd_reg, is, ps, r0, out);
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
      latch_cmd_fail(ops, sdhci_va, cmd, arg, cmd_reg, is, ps, r0, out);
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
    sm6125_emmc_ident_report_t *out) {
  if (lba > 0xFFFFFFFFULL) {
    return SM6125_EMMC_RC_BAD_ARG;
  }
  return send_cmd_pio(
      ops, sdhci_va, 17u, (uint32_t)lba,
      (uint8_t)(SDHCI_CMD_DATA | SDHCI_CMD_INDEX | SDHCI_CMD_CRC | SDHCI_CMD_RESP_SHORT),
      true, NULL, sector, out);
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
  zero_buf(out, sizeof(*out));

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
  rc = send_cmd_pio(ops, sdhci_va, 0u, 0u, SDHCI_CMD_RESP_NONE, false, NULL, NULL, out);
  if (rc != SM6125_EMMC_RC_OK) {
    out->rc = rc;
    capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return rc;
  }
  ops->delay_us(2000u);

  out->step = SM6125_EMMC_STEP_CMD1;
  uint32_t ocr = 0u;
  bool card_ready = false;
  for (unsigned i = 0u; i < 500u; i++) {
    rc = send_cmd_pio(ops, sdhci_va, 1u, 0x40FF8080u, SDHCI_CMD_RESP_SHORT, false, &ocr, NULL, out);
    if (rc != SM6125_EMMC_RC_OK) {
      out->rc = rc;
      capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
      return rc;
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
    latch_cmd_fail(ops, sdhci_va, 1u, 0x40FF8080u, 0x0102u, 0u, ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE), ocr, out);
    out->rc = SM6125_EMMC_RC_CMD_TIMEOUT;
    capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return SM6125_EMMC_RC_CMD_TIMEOUT;
  }
  ops->delay_us(8u);

  out->step = SM6125_EMMC_STEP_CMD2;
  rc = send_cmd_pio(ops, sdhci_va, 2u, 0u, (uint8_t)(SDHCI_CMD_CRC | SDHCI_CMD_RESP_LONG), false, out->cid, NULL, out);
  if (rc != SM6125_EMMC_RC_OK) {
    out->rc = rc;
    capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return rc;
  }
  out->has_cid = true;

  out->step = SM6125_EMMC_STEP_CMD3;
  rc = send_cmd_pio(ops, sdhci_va, 3u, 0x00010000u, (uint8_t)(SDHCI_CMD_INDEX | SDHCI_CMD_CRC | SDHCI_CMD_RESP_SHORT), true, NULL, NULL, out);
  if (rc != SM6125_EMMC_RC_OK) {
    out->rc = rc;
    capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return rc;
  }

  out->step = SM6125_EMMC_STEP_CMD9;
  rc = send_cmd_pio(ops, sdhci_va, 9u, 0x00010000u, (uint8_t)(SDHCI_CMD_CRC | SDHCI_CMD_RESP_LONG), false, out->csd, NULL, out);
  if (rc != SM6125_EMMC_RC_OK) {
    out->rc = rc;
    capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return rc;
  }
  out->has_csd = true;

  out->step = SM6125_EMMC_STEP_CMD7;
  rc = send_cmd_pio(ops, sdhci_va, 7u, 0x00010000u, (uint8_t)(SDHCI_CMD_INDEX | SDHCI_CMD_CRC | SDHCI_CMD_RESP_SHORT), true, NULL, NULL, out);
  if (rc != SM6125_EMMC_RC_OK) {
    out->rc = rc;
    capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return rc;
  }

  uint8_t sector[512];
  out->step = SM6125_EMMC_STEP_CMD8;
  rc = send_cmd_pio(ops, sdhci_va, 8u, 0u, (uint8_t)(SDHCI_CMD_DATA | SDHCI_CMD_INDEX | SDHCI_CMD_CRC | SDHCI_CMD_RESP_SHORT), true, NULL, sector, out);
  if (rc != SM6125_EMMC_RC_OK) {
    out->rc = rc;
    capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return rc;
  }
  out->bus_width = sector[183];
  out->hs_timing = sector[185];
  out->ext_csd_rev = sector[192];
  out->device_type = sector[196];
  out->sec_count = rd_le32(&sector[212]);
  out->has_ext_csd = true;

  out->step = SM6125_EMMC_STEP_READ_LBA0;
  rc = read_sector_cmd17(ops, sdhci_va, 0ULL, sector, out);
  if (rc != SM6125_EMMC_RC_OK) {
    out->rc = rc;
    capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return rc;
  }
  if (rd_le16(&sector[510]) != 0xAA55u || sector[450] != 0xEEu) {
    out->rc = SM6125_EMMC_RC_BAD_MBR;
    capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return SM6125_EMMC_RC_BAD_MBR;
  }
  out->lba0_crc = crc32_bytes(sector, 512u);

  out->step = SM6125_EMMC_STEP_READ_LBA1;
  rc = read_sector_cmd17(ops, sdhci_va, 1ULL, sector, out);
  if (rc != SM6125_EMMC_RC_OK) {
    out->rc = rc;
    capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return rc;
  }
  if (!validate_gpt_header(sector, 1ULL, &out->gpt_hdr_crc)) {
    out->rc = SM6125_EMMC_RC_BAD_GPT_HDR;
    capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return SM6125_EMMC_RC_BAD_GPT_HDR;
  }
  const uint64_t bak_lba = rd_le64(sector + 32u);
  const uint64_t part_lba = rd_le64(sector + 72u);
  const uint32_t num_parts = rd_le32(sector + 80u);
  const uint32_t part_sz = rd_le32(sector + 84u);
  const uint32_t expected_parts_crc = rd_le32(sector + 88u);
  if (part_lba < 2ULL || part_lba >= 34ULL || num_parts == 0u || num_parts > 512u || part_sz != 128u) {
    out->rc = SM6125_EMMC_RC_BAD_GPT_HDR;
    capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return SM6125_EMMC_RC_BAD_GPT_HDR;
  }
  out->lba1_crc = crc32_bytes(sector, 512u);
  out->gpt_parts_crc = expected_parts_crc;

  out->step = SM6125_EMMC_STEP_READ_GPT_ARRAY;
  const uint32_t array_bytes = num_parts * 128u;
  const uint32_t array_sectors = (array_bytes + 511u) / 512u;
  uint32_t running_crc = 0xFFFFFFFFu;
  uint32_t remaining = array_bytes;
  for (uint32_t s = 0u; s < array_sectors; s++) {
    rc = read_sector_cmd17(ops, sdhci_va, part_lba + s, sector, out);
    if (rc != SM6125_EMMC_RC_OK) {
      out->rc = rc;
      capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
      return rc;
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
    out->rc = SM6125_EMMC_RC_BAD_GPT_PARTS;
    capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return SM6125_EMMC_RC_BAD_GPT_PARTS;
  }
  if (!out->has_userdata) {
    out->rc = SM6125_EMMC_RC_NO_USERDATA;
    capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return SM6125_EMMC_RC_NO_USERDATA;
  }

  out->step = SM6125_EMMC_STEP_READ_GPT_BACKUP;
  rc = read_sector_cmd17(ops, sdhci_va, bak_lba, sector, out);
  if (rc != SM6125_EMMC_RC_OK) {
    out->rc = rc;
    capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return rc;
  }
  if (!validate_gpt_header(sector, bak_lba, NULL) || rd_le32(sector + 88u) != expected_parts_crc) {
    out->rc = SM6125_EMMC_RC_BAD_GPT_BACKUP;
    capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
    return SM6125_EMMC_RC_BAD_GPT_BACKUP;
  }

  out->bak_ok = true;
  out->has_gpt = true;
  out->step = SM6125_EMMC_STEP_DONE;
  out->rc = SM6125_EMMC_RC_OK;
  out->ok = true;
  capture_regs(ops, gcc_sdcc1_va, sdhci_va, true, out);
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

static void id_i32(id_fmt_t *c, int32_t v) {
  uint32_t mag;
  if (v < 0) {
    id_putc(c, '-');
    mag = (uint32_t)(-(v + 1)) + 1u;
  } else {
    mag = (uint32_t)v;
  }
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
