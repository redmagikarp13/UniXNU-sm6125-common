// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  SM6125_EMMC_PROBE_MODE_OFF = 0u,
  SM6125_EMMC_PROBE_MODE_RO_SNAPSHOT = 1u,
  SM6125_EMMC_PROBE_MODE_IDENT_GPT = 2u,
  SM6125_EMMC_PROBE_MODE_INVALID = 0xFFFFFFFFu,
};

enum {
  SM6125_EMMC_RO_ERR_NONE = 0u,
  SM6125_EMMC_RO_ERR_INVALID_MODE = 1u,
  SM6125_EMMC_RO_ERR_ADT_INVALID = 2u,
  SM6125_EMMC_RO_ERR_MAP_GCC = 3u,
  SM6125_EMMC_RO_ERR_CLOCK_GATED = 4u,
  SM6125_EMMC_RO_ERR_MAP_SDHCI = 5u,
};

enum {
  SM6125_EMMC_GATE_BCR_CLEAR = 1u << 0,
  SM6125_EMMC_GATE_AHB_ON = 1u << 1,
  SM6125_EMMC_GATE_APPS_ON = 1u << 2,
  SM6125_EMMC_GATE_ICE_ON = 1u << 3,
  SM6125_EMMC_GATE_REQUIRED_MASK = 0x07u,
};

/* GCC SDCC1 register offsets (relative to gcc_pa + 0x38000; Linux gcc-sm6125.c). */
enum {
  SM6125_GCC_SDCC1_PAGE_OFFSET = 0x38000u,
  SM6125_GCC_SDCC1_WINDOW_SIZE = 0x1000u,
  SM6125_GCC_MIN_REGION_SIZE = 0x39000u,
  SM6125_SDHCI_MIN_REGION_SIZE = 0x1000u,

  SM6125_GCC_SDCC1_BCR_OFF = 0x000u,
  SM6125_GCC_SDCC1_APPS_CBCR_OFF = 0x004u,
  SM6125_GCC_SDCC1_AHB_CBCR_OFF = 0x008u,
  SM6125_GCC_SDCC1_ICE_CBCR_OFF = 0x00Cu,
  SM6125_GCC_SDCC1_ICE_CMD_RCGR_OFF = 0x010u,
  SM6125_GCC_SDCC1_ICE_CFG_RCGR_OFF = 0x014u,
  SM6125_GCC_SDCC1_APPS_CMD_RCGR_OFF = 0x028u,
  SM6125_GCC_SDCC1_APPS_CFG_RCGR_OFF = 0x02Cu,
  SM6125_GCC_SDCC1_APPS_M_OFF = 0x030u,
  SM6125_GCC_SDCC1_APPS_N_OFF = 0x034u,
  SM6125_GCC_SDCC1_APPS_D_OFF = 0x038u,
};

/* Standard SDHCI + Qualcomm sdhci_msm_v5_offset registers (Linux sdhci-msm.c). */
enum {
  SM6125_SDHCI_PRESENT_STATE = 0x024u,
  SM6125_SDHCI_HOST_CONTROL = 0x028u,
  SM6125_SDHCI_POWER_CONTROL = 0x029u,
  SM6125_SDHCI_CLOCK_CONTROL = 0x02Cu,
  SM6125_SDHCI_SOFTWARE_RESET = 0x02Fu,
  SM6125_SDHCI_INT_STATUS = 0x030u,
  SM6125_SDHCI_INT_ENABLE = 0x034u,
  SM6125_SDHCI_SIGNAL_ENABLE = 0x038u,
  SM6125_SDHCI_HOST_CONTROL2 = 0x03Eu,
  SM6125_SDHCI_CAPABILITIES = 0x040u,
  SM6125_SDHCI_CAPABILITIES_1 = 0x044u,
  SM6125_SDHCI_HOST_VERSION = 0x0FEu,

  SM6125_SDHCI_V5_DLL_CONFIG = 0x200u,
  SM6125_SDHCI_V5_DLL_STATUS = 0x208u,
  SM6125_SDHCI_V5_VENDOR_SPEC = 0x20Cu,
  SM6125_SDHCI_V5_VENDOR_CAP0 = 0x21Cu,
  SM6125_SDHCI_V5_PWRCTL_STATUS = 0x240u,
  SM6125_SDHCI_V5_PWRCTL_MASK = 0x244u,
  SM6125_SDHCI_V5_VENDOR_SPEC3 = 0x250u,
  SM6125_SDHCI_V5_DLL_CONFIG_2 = 0x254u,
  SM6125_SDHCI_V5_DDR_CONFIG = 0x25Cu,
  SM6125_SDHCI_V5_MCI_VERSION = 0x318u,
  SM6125_SDHCI_V5_GENERICS = 0x320u,
};

typedef struct {
  uint8_t (*read8)(uintptr_t base, uint32_t offset);
  uint16_t (*read16)(uintptr_t base, uint32_t offset);
  uint32_t (*read32)(uintptr_t base, uint32_t offset);
} sm6125_mmio_ro_ops_t;

typedef struct {
  uint32_t err;
  uint32_t gate_flags;
  uint64_t gcc_pa;
  uint64_t sdhci_pa;
  bool gcc_valid;
  bool sdhci_valid;

  uint32_t gcc_bcr;
  uint32_t gcc_apps_cbcr;
  uint32_t gcc_ahb_cbcr;
  uint32_t gcc_ice_cbcr;
  uint32_t gcc_ice_cmd;
  uint32_t gcc_ice_cfg;
  uint32_t gcc_apps_cmd;
  uint32_t gcc_apps_cfg;
  uint32_t gcc_apps_m;
  uint32_t gcc_apps_n;
  uint32_t gcc_apps_d;

  uint32_t sdhci_mci_version;
  uint16_t sdhci_spec_version;
  uint32_t sdhci_cap0;
  uint32_t sdhci_cap1;
  uint32_t sdhci_present_state;
  uint16_t sdhci_clock_control;
  uint8_t sdhci_power_control;
  uint8_t sdhci_host_control;
  uint16_t sdhci_host_control2;
  uint8_t sdhci_software_reset;
  uint32_t sdhci_int_status;
  uint32_t sdhci_int_enable;
  uint32_t sdhci_signal_enable;
  uint32_t sdhci_vendor_spec;
  uint32_t sdhci_vendor_spec3;
  uint32_t sdhci_vendor_cap0;
  uint32_t sdhci_pwrctl_status;
  uint32_t sdhci_pwrctl_mask;
  uint32_t sdhci_generics;
  uint32_t sdhci_dll_config;
  uint32_t sdhci_dll_config_2;
  uint32_t sdhci_dll_status;
  uint32_t sdhci_ddr_config;
} sm6125_emmc_ro_snapshot_t;

uint32_t sm6125_emmc_probe_select_mode(bool has_arg, uint32_t arg_value);

bool sm6125_emmc_translate_region(
    const uint64_t bus[3],
    uint64_t off,
    uint64_t len,
    uint64_t min_len,
    uint64_t *out_pa);

bool sm6125_emmc_gcc_gate(
    uint32_t bcr,
    uint32_t apps,
    uint32_t ahb,
    uint32_t ice,
    uint32_t *out_flags);

bool sm6125_emmc_ro_read_gcc(
    const sm6125_mmio_ro_ops_t *ops,
    uintptr_t gcc_sdcc1_va,
    sm6125_emmc_ro_snapshot_t *snap);

bool sm6125_emmc_ro_read_sdhci(
    const sm6125_mmio_ro_ops_t *ops,
    uintptr_t sdhci_va,
    sm6125_emmc_ro_snapshot_t *snap);

size_t sm6125_emmc_ro_format_report(
    const sm6125_emmc_ro_snapshot_t *snap,
    char *buf,
    size_t cap);

#ifdef __cplusplus
}
#endif
