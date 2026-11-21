// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Additional SDHCI / MSM v5 register offsets for host init + PIO read. */
enum {
  SM6125_SDHCI_BLOCK_SIZE = 0x004u,
  SM6125_SDHCI_BLOCK_COUNT = 0x006u,
  SM6125_SDHCI_ARGUMENT = 0x008u,
  SM6125_SDHCI_TRANSFER_MODE = 0x00Cu,
  SM6125_SDHCI_COMMAND = 0x00Eu,
  SM6125_SDHCI_RESPONSE0 = 0x010u,
  SM6125_SDHCI_RESPONSE1 = 0x014u,
  SM6125_SDHCI_RESPONSE2 = 0x018u,
  SM6125_SDHCI_RESPONSE3 = 0x01Cu,
  SM6125_SDHCI_BUFFER = 0x020u,
  SM6125_SDHCI_TIMEOUT_CONTROL = 0x02Eu,
  SM6125_SDHCI_AUTO_CMD_STATUS = 0x03Cu,
  SM6125_SDHCI_ADMA_ERROR = 0x054u,
  SM6125_SDHCI_ADMA_ADDRESS = 0x058u,
  SM6125_SDHCI_ADMA_ADDRESS_HI = 0x05Cu,

  SM6125_SDHCI_V5_PWRCTL_CLEAR = 0x248u,
  SM6125_SDHCI_V5_PWRCTL_CTL = 0x24Cu,
};

enum {
  SM6125_EMMC_STEP_NONE = 0u,
  SM6125_EMMC_STEP_GCC_RESET = 1u,
  SM6125_EMMC_STEP_GCC_AHB = 2u,
  SM6125_EMMC_STEP_GCC_RCG_400K = 3u,
  SM6125_EMMC_STEP_GCC_APPS = 4u,
  SM6125_EMMC_STEP_SDHCI_VENDOR_INIT = 5u,
  SM6125_EMMC_STEP_SDHCI_RESET = 6u,
  SM6125_EMMC_STEP_SDHCI_PWR_18V = 7u,
  SM6125_EMMC_STEP_SDHCI_CLK_400K = 8u,
  SM6125_EMMC_STEP_CMD0 = 9u,
  SM6125_EMMC_STEP_CMD1 = 10u,
  SM6125_EMMC_STEP_CMD2 = 11u,
  SM6125_EMMC_STEP_CMD3 = 12u,
  SM6125_EMMC_STEP_CMD9 = 13u,
  SM6125_EMMC_STEP_CMD7 = 14u,
  SM6125_EMMC_STEP_CMD8 = 15u,
  SM6125_EMMC_STEP_READ_LBA0 = 16u,
  SM6125_EMMC_STEP_READ_LBA1 = 17u,
  SM6125_EMMC_STEP_READ_GPT_ARRAY = 18u,
  SM6125_EMMC_STEP_READ_GPT_BACKUP = 19u,
  SM6125_EMMC_STEP_DONE = 20u,
  SM6125_EMMC_STEP_HS_CMD6 = 21u,
  SM6125_EMMC_STEP_HS_8BIT_CMD6 = 22u,
  SM6125_EMMC_STEP_GCC_50M = 23u,
  SM6125_EMMC_STEP_HS_PIO_GPT = 24u,
  SM6125_EMMC_STEP_HS_PIO_BENCH = 25u,
  SM6125_EMMC_STEP_HS_ADMA_GPT = 26u,
  SM6125_EMMC_STEP_HS_ADMA_BENCH = 27u,
  SM6125_EMMC_STEP_DDR_CMD6 = 28u,
  SM6125_EMMC_STEP_GCC_100M = 29u,
  SM6125_EMMC_STEP_DDR_ADMA_GPT = 30u,
  SM6125_EMMC_STEP_DDR_ADMA_BENCH = 31u,
  SM6125_EMMC_STEP_FAST_DONE = 32u,
};

enum {
  SM6125_EMMC_RC_OK = 0,
  SM6125_EMMC_RC_BAD_ARG = -1,
  SM6125_EMMC_RC_AHB_TIMEOUT = -2,
  SM6125_EMMC_RC_RCG_TIMEOUT = -3,
  SM6125_EMMC_RC_APPS_TIMEOUT = -4,
  SM6125_EMMC_RC_RESET_TIMEOUT = -5,
  SM6125_EMMC_RC_PWR_CLEAR_TIMEOUT = -6,
  SM6125_EMMC_RC_PWR_BUS_TIMEOUT = -7,
  SM6125_EMMC_RC_CLK_TIMEOUT = -8,
  SM6125_EMMC_RC_CMD_INH_TIMEOUT = -9,
  SM6125_EMMC_RC_CMD_TIMEOUT = -10,
  SM6125_EMMC_RC_CMD_CRC = -11,
  SM6125_EMMC_RC_R1_ERROR = -12,
  SM6125_EMMC_RC_DATA_TIMEOUT = -13,
  SM6125_EMMC_RC_DATA_ERROR = -14,
  SM6125_EMMC_RC_BAD_MBR = -15,
  SM6125_EMMC_RC_BAD_GPT_HDR = -16,
  SM6125_EMMC_RC_BAD_GPT_PARTS = -17,
  SM6125_EMMC_RC_NO_USERDATA = -18,
  SM6125_EMMC_RC_BAD_GPT_BACKUP = -19,
  SM6125_EMMC_RC_SWITCH_ERROR = -20,
  SM6125_EMMC_RC_EXT_CSD_MISMATCH = -21,
  SM6125_EMMC_RC_ADMA_ERROR = -22,
  SM6125_EMMC_RC_AUTO_CMD_ERROR = -23,
  SM6125_EMMC_RC_CANARY_CORRUPT = -24,
  SM6125_EMMC_RC_DATA_MISMATCH = -25,
};

typedef struct {
  uint8_t (*read8)(uintptr_t base, uint32_t offset);
  uint16_t (*read16)(uintptr_t base, uint32_t offset);
  uint32_t (*read32)(uintptr_t base, uint32_t offset);
  void (*write8)(uintptr_t base, uint32_t offset, uint8_t value);
  void (*write16)(uintptr_t base, uint32_t offset, uint16_t value);
  void (*write32)(uintptr_t base, uint32_t offset, uint32_t value);
  void (*delay_us)(uint32_t microseconds);
  uint64_t (*now_us)(void);
} sm6125_emmc_hw_ops_t;

enum {
  SM6125_EMMC_FAST_MODE_NONE = 0u,
  SM6125_EMMC_FAST_MODE_HS50_PIO = 1u,
  SM6125_EMMC_FAST_MODE_HS50_ADMA = 2u,
  SM6125_EMMC_FAST_MODE_DDR52_ADMA = 3u,
};

typedef struct {
  uint8_t *desc_va;
  uint64_t desc_pa;
  size_t desc_size;
  uint8_t *data_va;
  uint64_t data_pa;
  size_t data_size;
  size_t canary_offset;
  size_t canary_size;
  void (*cache_clean_desc)(void *ctx, size_t offset, size_t len);
  void (*cache_flush_data)(void *ctx, size_t offset, size_t len);
  void (*cache_invalidate_data)(void *ctx, size_t offset, size_t len);
  void *cache_ctx;
} sm6125_emmc_dma_buf_t;

typedef struct {
  bool ok;
  uint32_t active_mode;
  uint32_t step;
  int32_t rc;

  bool hs50_pio_ok;
  int32_t hs50_pio_rc;
  uint32_t hs50_pio_kibs;
  uint32_t hs50_pio_us;

  bool hs50_adma_ok;
  int32_t hs50_adma_rc;
  uint32_t hs50_adma_kibs;
  uint32_t hs50_adma_us;
  uint32_t hs50_adma_crc;

  bool ddr52_ok;
  int32_t ddr52_rc;
  uint32_t ddr52_kibs;
  uint32_t ddr52_us;
  uint32_t ddr52_crc;

  uint8_t hs_timing;
  uint8_t device_type;
  uint32_t ext_csd_ro_crc;
  uint32_t gpt_parts_crc;
  uint32_t acfg;
  uint16_t clk;
  uint8_t hc;
  uint16_t hc2;
  uint32_t vs;
  uint32_t ddr_cfg;
  uint32_t dll;

  bool fail_valid;
  uint32_t fail_rung;
  uint32_t fail_step;
  int32_t fail_rc;
  uint32_t fail_cmd;
  uint32_t fail_arg;
  uint32_t fail_is;
  uint32_t fail_ps;
  uint32_t fail_r0;
  uint16_t fail_acmd;
  uint8_t fail_adma_err;
} sm6125_emmc_fast_report_t;

typedef struct {
  bool ok;
  uint32_t step;
  int32_t rc;

  /* Final or failing state snapshot */
  uint32_t acfg;
  uint16_t clk;
  uint8_t pwr;
  uint16_t hc2;
  uint32_t vs;
  uint32_t pst;
  uint32_t dll;

  /* Last/failing command tuple */
  bool fail_valid;
  uint32_t fail_cmd;
  uint32_t fail_arg;
  uint16_t fail_reg;
  uint32_t fail_is;
  uint32_t fail_ps;
  uint32_t fail_r0;

  /* Card identity */
  bool has_ocr;
  uint32_t ocr;
  bool has_cid;
  uint32_t cid[4];
  bool has_csd;
  uint32_t csd[4];
  bool has_ext_csd;
  uint32_t sec_count;
  uint8_t ext_csd_rev;
  uint8_t device_type;
  uint8_t bus_width;
  uint8_t hs_timing;

  /* GPT validation (PIO read-only) */
  bool has_gpt;
  uint32_t lba0_crc;
  uint32_t lba1_crc;
  uint32_t gpt_hdr_crc;
  uint32_t gpt_parts_crc;
  bool bak_ok;
  bool has_userdata;
  uint64_t userdata_first;
  uint64_t userdata_last;
} sm6125_emmc_ident_report_t;

int32_t sm6125_emmc_ident_gpt_run(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t gcc_sdcc1_va,
    uintptr_t sdhci_va,
    sm6125_emmc_ident_report_t *out);

size_t sm6125_emmc_ident_format_report(
    const sm6125_emmc_ident_report_t *rep,
    char *buf,
    size_t cap);

int32_t sm6125_emmc_fast_bench_run(
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t gcc_sdcc1_va,
    uintptr_t sdhci_va,
    const sm6125_emmc_dma_buf_t *dma_buf,
    sm6125_emmc_fast_report_t *out);

size_t sm6125_emmc_fast_format_report(
    const sm6125_emmc_fast_report_t *rep,
    char *buf,
    size_t cap);

typedef struct {
  const sm6125_emmc_hw_ops_t *ops;
  uintptr_t gcc_sdcc1_va;
  uintptr_t sdhci_va;
  sm6125_emmc_dma_buf_t dma;
  bool has_dma;
  bool online;
  uint32_t active_mode;
  uint32_t step;
  int32_t last_rc;
  uint32_t sec_count;
  uint8_t device_type;
  uint8_t hs_timing;
  uint32_t ext_csd_ro_crc;
  uint32_t gpt_parts_crc;
  uint64_t userdata_first;
  uint64_t userdata_last;
  uint32_t acfg;
  uint8_t hc;
  uint16_t hc2;
  uint32_t ddr_cfg;
} sm6125_emmc_ctrl_t;

int32_t sm6125_emmc_ctrl_init(
    sm6125_emmc_ctrl_t *ctrl,
    const sm6125_emmc_hw_ops_t *ops,
    uintptr_t gcc_sdcc1_va,
    uintptr_t sdhci_va,
    const sm6125_emmc_dma_buf_t *dma_buf);

int32_t sm6125_emmc_ctrl_read_sectors(
    sm6125_emmc_ctrl_t *ctrl,
    uint64_t lba,
    uint32_t sector_count,
    uint8_t *dst_va);

#ifdef __cplusplus
}
#endif
