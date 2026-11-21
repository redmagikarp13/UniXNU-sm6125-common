// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Sm6125EmmcCore.h"
#include "Sm6125EmmcReadOnly.h"

#define TEST_SECTORS 122142720ULL
#define BAK_GPT_LBA (TEST_SECTORS - 1ULL)

typedef struct {
  /* GCC state */
  uint32_t gcc_bcr;
  uint32_t gcc_apps_cbcr;
  uint32_t gcc_ahb_cbcr;
  uint32_t gcc_ice_cbcr;
  uint32_t gcc_apps_cmd;
  uint32_t gcc_apps_cfg;
  uint32_t gcc_apps_m;
  uint32_t gcc_apps_n;
  uint32_t gcc_apps_d;

  bool gcc_saw_bcr_1;
  bool gcc_saw_bcr_0_after_1;
  uint64_t bcr_assert_us;
  uint64_t time_us;
  uint32_t ahb_sticky_off_polls;
  uint32_t rcg_sticky_polls;
  uint32_t apps_sticky_off_polls;
  bool fail_ahb_forever;
  bool fail_rcg_forever;
  bool fail_apps_forever;
  bool block_sdhci_if_clocks_off;

  /* SDHCI state */
  uint16_t block_size;
  uint16_t block_count;
  uint32_t argument;
  uint16_t transfer_mode;
  uint16_t command;
  uint32_t resp[4];
  uint32_t present_state;
  uint8_t host_control;
  uint8_t power_control;
  uint16_t clock_control;
  uint8_t timeout_control;
  uint8_t software_reset;
  uint32_t int_status;
  uint32_t int_enable;
  uint32_t signal_enable;
  uint16_t host_control2;

  uint32_t dll_config;
  uint32_t vendor_spec;
  uint32_t pwrctl_status;
  uint32_t pwrctl_mask;
  uint32_t pwrctl_ctl;
  uint32_t vendor_spec3;

  /* Fault injections */
  uint32_t reset_sticky_polls;
  bool fail_reset_forever;
  uint32_t pwr_clear_sticky;
  bool fail_pwr_clear_forever;
  bool omit_bus_on_pwr_status;
  uint32_t clk_stable_sticky;
  bool fail_clk_stable_forever;
  uint32_t cmd1_busy_polls;
  bool fail_cmd1_forever;
  bool inject_cmd2_crc;
  bool inject_cmd3_r1_err;
  bool inject_cmd7_timeout;
  bool inject_cmd8_data_crc;
  bool inject_cmd17_brr_timeout;
  bool inject_cmd17_data_timeout;
  bool inject_cmd17_data_crc;

  /* Sequencing tracking */
  bool saw_vendor_spec_por_pad;
  bool saw_pwrsave_clear;
  uint32_t dll_write_seq;
  bool saw_dll_rst;
  bool saw_dll_pdn_after_rst;
  bool saw_pwr_ctl_bus_io;
  uint64_t cmd1_ready_time_us;
  uint64_t clk_enable_time_us;

  /* Active PIO transfer */
  uint8_t pio_buf[512];
  size_t pio_off;
  bool pio_active;

  /* Card fixtures */
  uint32_t cid_words[4];
  uint32_t csd_words[4];
  uint8_t ext_csd[512];
  uint8_t gpt_primary[65536];
  size_t gpt_primary_len;
  uint8_t gpt_backup[512];
} sim_hw_t;

static sim_hw_t g_hw;
static uint8_t g_gpt_real[65536];
static size_t g_gpt_real_len = 0;

static uint32_t host_crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
  }
  return ~crc;
}

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void init_hw_from_real_gpt(void) {
  memset(&g_hw, 0, sizeof(g_hw));
  /* Hardware-observed initial state from SM6125-EMMC-RO photo */
  g_hw.gcc_bcr = 0x00000000u;
  g_hw.gcc_apps_cbcr = 0x00004221u;
  g_hw.gcc_ahb_cbcr = 0x00000001u;
  g_hw.gcc_ice_cbcr = 0x00004221u;
  g_hw.gcc_apps_cfg = 0x00000401u;
  g_hw.gcc_apps_m = 0x00000001u;
  g_hw.gcc_apps_n = 0x000000FEu;
  g_hw.gcc_apps_d = 0x000000FDu;

  g_hw.vendor_spec = 0x00300A0Cu;
  g_hw.vendor_spec3 = 0x02626040u;
  g_hw.dll_config = 0x6000642Cu;
  g_hw.present_state = 0x01F800F0u;
  g_hw.clock_control = 0x0003u;

  /* Micron S0J9K8 from /sys/block/mmcblk0/device/{cid,csd} */
  g_hw.cid_words[0] = 0x13014E53u;
  g_hw.cid_words[1] = 0x304A394Bu;
  g_hw.cid_words[2] = 0x381013BAu;
  g_hw.cid_words[3] = 0xFBAA9600u;

  g_hw.csd_words[0] = 0xD05F0132u;
  g_hw.csd_words[1] = 0x8F5913FFu;
  g_hw.csd_words[2] = 0xFFFFFFEFu;
  g_hw.csd_words[3] = 0x96400000u;

  g_hw.ext_csd[183] = 0x00u; /* BUS_WIDTH 1-bit initially after reset */
  g_hw.ext_csd[185] = 0x00u; /* HS_TIMING legacy */
  g_hw.ext_csd[192] = 0x08u; /* EXT_CSD_REV */
  g_hw.ext_csd[196] = 0x57u; /* DEVICE_TYPE */
  put_le32(&g_hw.ext_csd[212], (uint32_t)TEST_SECTORS);

  assert(g_gpt_real_len == 65536u);
  memcpy(g_hw.gpt_primary, g_gpt_real, g_gpt_real_len);
  g_hw.gpt_primary_len = g_gpt_real_len;

  /* Construct valid backup GPT header at LBA (TEST_SECTORS - 1) */
  memcpy(g_hw.gpt_backup, g_gpt_real + 512u, 512u);
  /* swap current_lba <-> backup_lba (offsets 24..39) */
  memcpy(g_hw.gpt_backup + 24u, g_gpt_real + 512u + 32u, 8u);
  memcpy(g_hw.gpt_backup + 32u, g_gpt_real + 512u + 24u, 8u);
  put_le32(g_hw.gpt_backup + 16u, 0u);
  uint32_t bak_crc = host_crc32(g_hw.gpt_backup, 92u);
  put_le32(g_hw.gpt_backup + 16u, bak_crc);
}

static void pack_r2_from_128(const uint32_t words[4]) {
  /* Inverse of SDHCI 136-bit response shift ((r[3]<<8)|(r[2]>>24), ...) */
  g_hw.resp[3] = words[0] >> 8;
  g_hw.resp[2] = ((words[0] & 0xFFu) << 24) | (words[1] >> 8);
  g_hw.resp[1] = ((words[1] & 0xFFu) << 24) | (words[2] >> 8);
  g_hw.resp[0] = ((words[2] & 0xFFu) << 24) | (words[3] >> 8);
}

static bool clocks_running(void) {
  return (g_hw.gcc_bcr & 1u) == 0u &&
         (g_hw.gcc_ahb_cbcr & 0x80000001u) == 1u &&
         (g_hw.gcc_apps_cbcr & 0x80000001u) == 1u;
}

static void sim_delay_us(uint32_t us) {
  g_hw.time_us += us;
}

static void dispatch_command(uint16_t cmd_reg) {
  const uint32_t cmd = (cmd_reg >> 8) & 0x3Fu;
  const uint32_t flags = cmd_reg & 0xFFu;

  /* Strictly enforce read-only command allowlist */
  switch (cmd) {
    case 0u:
    case 1u:
    case 2u:
    case 3u:
    case 7u:
    case 8u:
    case 9u:
    case 17u:
      break;
    default:
      fprintf(stderr, "FORBIDDEN EMMC CMD %u\n", cmd);
      abort();
  }

  /* Verify card clock has stabilized >= 1000 us */
  assert(g_hw.clk_enable_time_us > 0u);
  assert(g_hw.time_us - g_hw.clk_enable_time_us >= 1000u);

  if (cmd == 2u || cmd == 3u || cmd == 7u || cmd == 8u || cmd == 9u || cmd == 17u) {
    assert((flags & 0x08u) != 0u); /* CRC check must ALWAYS be enabled */
  }

  g_hw.pio_active = false;
  g_hw.pio_off = 0u;
  g_hw.present_state &= ~0x00000800u; /* BRR clear */

  switch (cmd) {
    case 0u:
      g_hw.int_status |= 0x1u;
      break;
    case 1u:
      g_hw.int_status |= 0x1u;
      if (g_hw.fail_cmd1_forever || g_hw.cmd1_busy_polls > 0u) {
        if (!g_hw.fail_cmd1_forever) {
          g_hw.cmd1_busy_polls--;
        }
        g_hw.resp[0] = 0x40FF8080u; /* busy (bit 31 = 0) */
      } else {
        g_hw.resp[0] = 0xC0FF8080u; /* ready (bit 31 = 1) + sector mode */
        g_hw.cmd1_ready_time_us = g_hw.time_us;
      }
      break;
    case 2u:
      assert(g_hw.cmd1_ready_time_us > 0u);
      assert(g_hw.time_us - g_hw.cmd1_ready_time_us >= 8u);
      if (g_hw.inject_cmd2_crc) {
        g_hw.int_status |= 0x00028000u; /* ERROR_INT | CMD_CRC */
      } else {
        g_hw.int_status |= 0x1u;
        pack_r2_from_128(g_hw.cid_words);
      }
      break;
    case 3u:
      g_hw.int_status |= 0x1u;
      g_hw.resp[0] = g_hw.inject_cmd3_r1_err ? 0x00400500u : 0x00000500u;
      break;
    case 9u:
      g_hw.int_status |= 0x1u;
      pack_r2_from_128(g_hw.csd_words);
      break;
    case 7u:
      assert((flags & 0x03u) == 0x02u); /* 48-bit R1, NOT R1B (0x03) */
      if (g_hw.inject_cmd7_timeout) {
        g_hw.int_status |= 0x00018000u;
      } else {
        g_hw.int_status |= 0x1u;
        g_hw.resp[0] = 0x00000700u;
      }
      break;
    case 8u:
      assert((flags & 0x20u) != 0u);
      assert((g_hw.transfer_mode & 0x0010u) != 0u);
      assert(g_hw.block_size == 512u && g_hw.block_count == 1u);
      if (g_hw.inject_cmd8_data_crc) {
        g_hw.int_status |= 0x00208001u; /* CC + ERROR_INT | DATA_CRC */
      } else {
        g_hw.int_status |= 0x00000023u; /* CC + TC + BRR */
        g_hw.present_state |= 0x00000800u;
        memcpy(g_hw.pio_buf, g_hw.ext_csd, 512u);
        g_hw.pio_active = true;
      }
      g_hw.resp[0] = 0x00000900u;
      break;
    case 17u: {
      assert((flags & 0x20u) != 0u);
      assert((g_hw.transfer_mode & 0x0010u) != 0u);
      assert(g_hw.block_size == 512u && g_hw.block_count == 1u);
      g_hw.resp[0] = 0x00000900u;
      if (g_hw.inject_cmd17_brr_timeout) {
        g_hw.int_status |= 0x00000001u; /* CC only, no BRR */
        break;
      }
      if (g_hw.inject_cmd17_data_timeout || g_hw.inject_cmd17_data_crc) {
        g_hw.int_status |= g_hw.inject_cmd17_data_timeout ? 0x00108021u : 0x00208021u;
        g_hw.present_state |= 0x00000800u;
        memset(g_hw.pio_buf, 0, 512u);
        g_hw.pio_active = true;
        break;
      }
      const uint32_t lba = g_hw.argument;
      if ((size_t)(lba + 1u) * 512u <= g_hw.gpt_primary_len) {
        memcpy(g_hw.pio_buf, g_hw.gpt_primary + (size_t)lba * 512u, 512u);
      } else if (lba == (uint32_t)BAK_GPT_LBA) {
        memcpy(g_hw.pio_buf, g_hw.gpt_backup, 512u);
      } else {
        fprintf(stderr, "Unexpected CMD17 LBA %u\n", lba);
        abort();
      }
      g_hw.int_status |= 0x00000023u; /* CC + TC + BRR */
      g_hw.present_state |= 0x00000800u;
      g_hw.pio_active = true;
      break;
    }
    default:
      abort();
  }
}

static uint8_t sim_r8(uintptr_t base, uint32_t off) {
  assert(base == (uintptr_t)&g_hw + 0x1000u);
  if (g_hw.block_sdhci_if_clocks_off) assert(clocks_running());
  switch (off) {
    case SM6125_SDHCI_POWER_CONTROL: return g_hw.power_control;
    case SM6125_SDHCI_SOFTWARE_RESET:
      if (g_hw.software_reset != 0u && !g_hw.fail_reset_forever) {
        if (g_hw.reset_sticky_polls > 0u) g_hw.reset_sticky_polls--;
        else g_hw.software_reset = 0u;
      }
      return g_hw.software_reset;
    case SM6125_SDHCI_HOST_CONTROL: return g_hw.host_control;
    case SM6125_SDHCI_TIMEOUT_CONTROL: return g_hw.timeout_control;
    default: abort();
  }
}

static uint16_t sim_r16(uintptr_t base, uint32_t off) {
  assert(base == (uintptr_t)&g_hw + 0x1000u);
  if (g_hw.block_sdhci_if_clocks_off) assert(clocks_running());
  switch (off) {
    case SM6125_SDHCI_CLOCK_CONTROL:
      if ((g_hw.clock_control & 1u) != 0u && (g_hw.clock_control & 2u) == 0u && !g_hw.fail_clk_stable_forever) {
        if (g_hw.clk_stable_sticky > 0u) g_hw.clk_stable_sticky--;
        else g_hw.clock_control |= 2u;
      }
      return g_hw.clock_control;
    case SM6125_SDHCI_HOST_CONTROL2: return g_hw.host_control2;
    default: abort();
  }
}

static uint32_t sim_r32(uintptr_t base, uint32_t off) {
  if (base == (uintptr_t)&g_hw) {
    switch (off) {
      case SM6125_GCC_SDCC1_BCR_OFF: return g_hw.gcc_bcr;
      case SM6125_GCC_SDCC1_AHB_CBCR_OFF:
        if ((g_hw.gcc_ahb_cbcr & 1u) != 0u && !g_hw.fail_ahb_forever) {
          if (g_hw.ahb_sticky_off_polls > 0u) g_hw.ahb_sticky_off_polls--;
          else g_hw.gcc_ahb_cbcr &= ~0x80000000u;
        }
        return g_hw.gcc_ahb_cbcr;
      case SM6125_GCC_SDCC1_APPS_CMD_RCGR_OFF:
        if ((g_hw.gcc_apps_cmd & 1u) != 0u && !g_hw.fail_rcg_forever) {
          if (g_hw.rcg_sticky_polls > 0u) g_hw.rcg_sticky_polls--;
          else g_hw.gcc_apps_cmd &= ~1u;
        }
        return g_hw.gcc_apps_cmd;
      case SM6125_GCC_SDCC1_APPS_CFG_RCGR_OFF: return g_hw.gcc_apps_cfg;
      case SM6125_GCC_SDCC1_APPS_M_OFF: return g_hw.gcc_apps_m;
      case SM6125_GCC_SDCC1_APPS_N_OFF: return g_hw.gcc_apps_n;
      case SM6125_GCC_SDCC1_APPS_D_OFF: return g_hw.gcc_apps_d;
      case SM6125_GCC_SDCC1_APPS_CBCR_OFF:
        if ((g_hw.gcc_apps_cbcr & 1u) != 0u && !g_hw.fail_apps_forever) {
          if (g_hw.apps_sticky_off_polls > 0u) g_hw.apps_sticky_off_polls--;
          else g_hw.gcc_apps_cbcr &= ~0x80000000u;
        }
        return g_hw.gcc_apps_cbcr;
      default: abort();
    }
  }
  assert(base == (uintptr_t)&g_hw + 0x1000u);
  if (g_hw.block_sdhci_if_clocks_off) assert(clocks_running());
  switch (off) {
    case SM6125_SDHCI_RESPONSE0: return g_hw.resp[0];
    case SM6125_SDHCI_RESPONSE1: return g_hw.resp[1];
    case SM6125_SDHCI_RESPONSE2: return g_hw.resp[2];
    case SM6125_SDHCI_RESPONSE3: return g_hw.resp[3];
    case SM6125_SDHCI_BUFFER: {
      assert(g_hw.pio_active && g_hw.pio_off + 4u <= 512u);
      uint32_t v = le32(g_hw.pio_buf + g_hw.pio_off);
      g_hw.pio_off += 4u;
      if (g_hw.pio_off == 512u) g_hw.present_state &= ~0x00000800u;
      return v;
    }
    case SM6125_SDHCI_PRESENT_STATE: return g_hw.present_state;
    case SM6125_SDHCI_INT_STATUS: return g_hw.int_status;
    case SM6125_SDHCI_V5_DLL_CONFIG: return g_hw.dll_config;
    case SM6125_SDHCI_V5_VENDOR_SPEC: return g_hw.vendor_spec;
    case SM6125_SDHCI_V5_PWRCTL_STATUS: return g_hw.pwrctl_status;
    case SM6125_SDHCI_V5_PWRCTL_CLEAR: return g_hw.pwrctl_status;
    case SM6125_SDHCI_V5_VENDOR_SPEC3: return g_hw.vendor_spec3;
    default: abort();
  }
}

static void sim_w8(uintptr_t base, uint32_t off, uint8_t val) {
  assert(base == (uintptr_t)&g_hw + 0x1000u);
  if (g_hw.block_sdhci_if_clocks_off) assert(clocks_running());
  switch (off) {
    case SM6125_SDHCI_POWER_CONTROL:
      g_hw.power_control = val;
      if ((val & 1u) != 0u && !g_hw.omit_bus_on_pwr_status) {
        g_hw.pwrctl_status |= 0x2u; /* CORE_PWRCTL_BUS_ON */
      }
      break;
    case SM6125_SDHCI_TIMEOUT_CONTROL: g_hw.timeout_control = val; break;
    case SM6125_SDHCI_SOFTWARE_RESET: g_hw.software_reset = val; break;
    default: abort();
  }
}

static void sim_w16(uintptr_t base, uint32_t off, uint16_t val) {
  assert(base == (uintptr_t)&g_hw + 0x1000u);
  if (g_hw.block_sdhci_if_clocks_off) assert(clocks_running());
  switch (off) {
    case SM6125_SDHCI_BLOCK_SIZE: g_hw.block_size = val; break;
    case SM6125_SDHCI_BLOCK_COUNT: g_hw.block_count = val; break;
    case SM6125_SDHCI_TRANSFER_MODE: g_hw.transfer_mode = val; break;
    case SM6125_SDHCI_COMMAND: g_hw.command = val; dispatch_command(val); break;
    case SM6125_SDHCI_CLOCK_CONTROL:
      g_hw.clock_control = (uint16_t)(val & ~2u);
      if ((val & 1u) != 0u && !g_hw.fail_clk_stable_forever && g_hw.clk_stable_sticky == 0u) {
        g_hw.clock_control |= 2u;
      }
      if ((val & 4u) != 0u && g_hw.clk_enable_time_us == 0u) {
        g_hw.clk_enable_time_us = g_hw.time_us;
      }
      break;
    case SM6125_SDHCI_HOST_CONTROL2: g_hw.host_control2 = val; break;
    default: abort();
  }
}

static void sim_w32(uintptr_t base, uint32_t off, uint32_t val) {
  if (base == (uintptr_t)&g_hw) {
    switch (off) {
      case SM6125_GCC_SDCC1_BCR_OFF:
        g_hw.gcc_bcr = val;
        if (val & 1u) {
          g_hw.gcc_saw_bcr_1 = true;
          g_hw.bcr_assert_us = g_hw.time_us;
          g_hw.gcc_ahb_cbcr = 0x80000000u;
          g_hw.gcc_apps_cbcr = 0x80000000u;
          g_hw.gcc_apps_cfg = 0u;
          g_hw.gcc_apps_m = 0u;
          g_hw.gcc_apps_n = 0u;
          g_hw.gcc_apps_d = 0u;
        } else if (g_hw.gcc_saw_bcr_1) {
          assert(g_hw.time_us - g_hw.bcr_assert_us >= 200u);
          g_hw.gcc_saw_bcr_0_after_1 = true;
        }
        break;
      case SM6125_GCC_SDCC1_AHB_CBCR_OFF: g_hw.gcc_ahb_cbcr = (g_hw.gcc_ahb_cbcr & ~1u) | (val & 1u); break;
      case SM6125_GCC_SDCC1_APPS_CBCR_OFF: g_hw.gcc_apps_cbcr = (g_hw.gcc_apps_cbcr & ~1u) | (val & 1u); break;
      case SM6125_GCC_SDCC1_APPS_CMD_RCGR_OFF: g_hw.gcc_apps_cmd = val; break;
      case SM6125_GCC_SDCC1_APPS_CFG_RCGR_OFF: g_hw.gcc_apps_cfg = val; break;
      case SM6125_GCC_SDCC1_APPS_M_OFF: g_hw.gcc_apps_m = val; break;
      case SM6125_GCC_SDCC1_APPS_N_OFF: g_hw.gcc_apps_n = val; break;
      case SM6125_GCC_SDCC1_APPS_D_OFF: g_hw.gcc_apps_d = val; break;
      default: abort();
    }
    return;
  }
  assert(base == (uintptr_t)&g_hw + 0x1000u);
  if (g_hw.block_sdhci_if_clocks_off) assert(clocks_running());
  switch (off) {
    case SM6125_SDHCI_ARGUMENT: g_hw.argument = val; break;
    case SM6125_SDHCI_INT_STATUS: g_hw.int_status &= ~val; break;
    case SM6125_SDHCI_INT_ENABLE: g_hw.int_enable = val; break;
    case SM6125_SDHCI_SIGNAL_ENABLE: g_hw.signal_enable = val; break;
    case SM6125_SDHCI_V5_DLL_CONFIG:
      g_hw.dll_write_seq++;
      if (g_hw.dll_write_seq == 1u && (val & (1u << 30)) != 0u) g_hw.saw_dll_rst = true;
      if (g_hw.dll_write_seq >= 2u && g_hw.saw_dll_rst && (val & (3u << 29)) == (3u << 29)) g_hw.saw_dll_pdn_after_rst = true;
      g_hw.dll_config = val;
      break;
    case SM6125_SDHCI_V5_VENDOR_SPEC:
      if (val == 0x00018A9Cu) g_hw.saw_vendor_spec_por_pad = true;
      g_hw.vendor_spec = val;
      break;
    case SM6125_SDHCI_V5_VENDOR_SPEC3:
      if ((val & (1u << 3)) == 0u) g_hw.saw_pwrsave_clear = true;
      g_hw.vendor_spec3 = val;
      break;
    case SM6125_SDHCI_V5_PWRCTL_MASK: g_hw.pwrctl_mask = val; break;
    case SM6125_SDHCI_V5_PWRCTL_CLEAR:
      if (!g_hw.fail_pwr_clear_forever) {
        if (g_hw.pwr_clear_sticky > 0u) g_hw.pwr_clear_sticky--;
        else g_hw.pwrctl_status &= ~val;
      }
      break;
    case SM6125_SDHCI_V5_PWRCTL_CTL:
      g_hw.pwrctl_ctl = val;
      if ((val & 0x5u) == 0x5u) g_hw.saw_pwr_ctl_bus_io = true;
      break;
    default: abort();
  }
}

static const sm6125_emmc_hw_ops_t g_hw_ops = {
    .read8 = sim_r8,
    .read16 = sim_r16,
    .read32 = sim_r32,
    .write8 = sim_w8,
    .write16 = sim_w16,
    .write32 = sim_w32,
    .delay_us = sim_delay_us,
};

static int run_sim(sm6125_emmc_ident_report_t *rep) {
  memset(rep, 0, sizeof(*rep));
  return sm6125_emmc_ident_gpt_run(&g_hw_ops, (uintptr_t)&g_hw, (uintptr_t)&g_hw + 0x1000u, rep);
}

static int test_happy_path_real_gpt(void) {
  init_hw_from_real_gpt();
  g_hw.block_sdhci_if_clocks_off = true;
  g_hw.ahb_sticky_off_polls = 2u;
  g_hw.rcg_sticky_polls = 2u;
  g_hw.apps_sticky_off_polls = 2u;
  g_hw.reset_sticky_polls = 2u;
  g_hw.pwr_clear_sticky = 3u;
  g_hw.clk_stable_sticky = 2u;
  g_hw.cmd1_busy_polls = 3u;

  sm6125_emmc_ident_report_t rep;
  assert(run_sim(&rep) == SM6125_EMMC_RC_OK);
  assert(rep.ok);
  assert(rep.step == SM6125_EMMC_STEP_DONE);
  assert(g_hw.gcc_saw_bcr_1 && g_hw.gcc_saw_bcr_0_after_1);
  assert(g_hw.saw_vendor_spec_por_pad && g_hw.saw_pwrsave_clear && g_hw.saw_dll_rst && g_hw.saw_dll_pdn_after_rst);
  assert(g_hw.saw_pwr_ctl_bus_io);
  assert(g_hw.gcc_apps_cfg == 0x2017u && g_hw.gcc_apps_m == 1u && g_hw.gcc_apps_n == 0xFCu && g_hw.gcc_apps_d == 0xFBu);
  assert(rep.cid[0] == 0x13014E53u && rep.cid[1] == 0x304A394Bu && rep.cid[2] == 0x381013BAu && rep.cid[3] == 0xFBAA9600u);
  assert(rep.csd[0] == 0xD05F0132u && rep.csd[1] == 0x8F5913FFu && rep.csd[2] == 0xFFFFFFEFu && rep.csd[3] == 0x96400000u);
  assert(rep.sec_count == (uint32_t)TEST_SECTORS);
  assert(rep.lba0_crc == 0xA22AB8BAu);
  assert(rep.lba1_crc == 0x3573B839u);
  assert(rep.gpt_hdr_crc == 0xD73DF5EEu);
  assert(rep.gpt_parts_crc == 0x8AB9C302u);
  assert(rep.bak_ok);
  assert(rep.has_userdata && rep.userdata_first == 0x01298000ULL && rep.userdata_last == 0x0747BFDEULL);

  char buf[768];
  size_t len = sm6125_emmc_ident_format_report(&rep, buf, sizeof(buf));
  assert(len > 0 && len < 700 && len == strlen(buf));
  assert(strstr(buf, "SM6125-EMMC-ID ok=1 step=20 rc=0 ") == buf);
  assert(strstr(buf, "13014e53304a394b381013bafbaa9600") != NULL);
  assert(strstr(buf, "l0=a22ab8ba l1=3573b839 hcrc=d73df5ee pcrc=8ab9c302 bak=1 ud=01298000..0747bfde") != NULL);

  /* Also verify DLL_RST comes strictly before DLL_PDN when starting with active high-speed DLL (bits 30:29 clear) */
  init_hw_from_real_gpt();
  g_hw.dll_config = 0x0000642Cu;
  assert(run_sim(&rep) == SM6125_EMMC_RC_OK);
  assert(g_hw.saw_dll_rst && g_hw.saw_dll_pdn_after_rst);
  return 18;
}

static int test_error_and_corruption_cases(void) {
  sm6125_emmc_ident_report_t rep;
  char buf[768];

  /* 1. AHB clock stuck off (must NOT touch SDHCI) */
  init_hw_from_real_gpt();
  g_hw.block_sdhci_if_clocks_off = true;
  g_hw.fail_ahb_forever = true;
  assert(run_sim(&rep) == SM6125_EMMC_RC_AHB_TIMEOUT);
  assert(!rep.ok && rep.step == SM6125_EMMC_STEP_GCC_AHB);

  /* 2. RCG update stuck */
  init_hw_from_real_gpt();
  g_hw.fail_rcg_forever = true;
  assert(run_sim(&rep) == SM6125_EMMC_RC_RCG_TIMEOUT);
  assert(rep.step == SM6125_EMMC_STEP_GCC_RCG_400K);

  /* 3. APPS clock stuck off */
  init_hw_from_real_gpt();
  g_hw.block_sdhci_if_clocks_off = true;
  g_hw.fail_apps_forever = true;
  assert(run_sim(&rep) == SM6125_EMMC_RC_APPS_TIMEOUT);
  assert(rep.step == SM6125_EMMC_STEP_GCC_APPS);

  /* 4. SDHCI reset timeout */
  init_hw_from_real_gpt();
  g_hw.fail_reset_forever = true;
  assert(run_sim(&rep) == SM6125_EMMC_RC_RESET_TIMEOUT);
  assert(rep.step == SM6125_EMMC_STEP_SDHCI_RESET);

  /* 5. PWRCTL clear stuck */
  init_hw_from_real_gpt();
  g_hw.fail_pwr_clear_forever = true;
  assert(run_sim(&rep) == SM6125_EMMC_RC_PWR_CLEAR_TIMEOUT);
  assert(rep.step == SM6125_EMMC_STEP_SDHCI_PWR_18V);

  /* 6. PWRCTL BUS_ON never arrives */
  init_hw_from_real_gpt();
  g_hw.omit_bus_on_pwr_status = true;
  assert(run_sim(&rep) == SM6125_EMMC_RC_PWR_BUS_TIMEOUT);
  assert(rep.step == SM6125_EMMC_STEP_SDHCI_PWR_18V);

  /* 7. Clock INT_STABLE timeout */
  init_hw_from_real_gpt();
  g_hw.fail_clk_stable_forever = true;
  assert(run_sim(&rep) == SM6125_EMMC_RC_CLK_TIMEOUT);
  assert(rep.step == SM6125_EMMC_STEP_SDHCI_CLK_400K);

  /* 8. CMD1 OCR timeout */
  init_hw_from_real_gpt();
  g_hw.fail_cmd1_forever = true;
  assert(run_sim(&rep) == SM6125_EMMC_RC_CMD_TIMEOUT);
  assert(rep.step == SM6125_EMMC_STEP_CMD1);

  /* 9. CMD2 CRC error */
  init_hw_from_real_gpt();
  g_hw.inject_cmd2_crc = true;
  assert(run_sim(&rep) == SM6125_EMMC_RC_CMD_CRC);
  assert(rep.step == SM6125_EMMC_STEP_CMD2 && rep.fail_valid && rep.fail_cmd == 2u);
  assert(sm6125_emmc_ident_format_report(&rep, buf, sizeof(buf)) < 700);
  assert(strstr(buf, "FAIL cmd=02 ") != NULL);

  /* 10. CMD3 R1 error bit */
  init_hw_from_real_gpt();
  g_hw.inject_cmd3_r1_err = true;
  assert(run_sim(&rep) == SM6125_EMMC_RC_R1_ERROR);
  assert(rep.step == SM6125_EMMC_STEP_CMD3 && rep.fail_cmd == 3u);

  /* 11. CMD7 timeout */
  init_hw_from_real_gpt();
  g_hw.inject_cmd7_timeout = true;
  assert(run_sim(&rep) == SM6125_EMMC_RC_CMD_TIMEOUT);
  assert(rep.step == SM6125_EMMC_STEP_CMD7 && rep.fail_cmd == 7u);

  /* 12. CMD8 data CRC */
  init_hw_from_real_gpt();
  g_hw.inject_cmd8_data_crc = true;
  assert(run_sim(&rep) == SM6125_EMMC_RC_DATA_ERROR);
  assert(rep.step == SM6125_EMMC_STEP_CMD8 && rep.fail_cmd == 8u);

  /* 13. CMD17 BRR timeout, DATA_TIMEOUT, and DATA_CRC error */
  init_hw_from_real_gpt();
  g_hw.inject_cmd17_brr_timeout = true;
  assert(run_sim(&rep) == SM6125_EMMC_RC_DATA_TIMEOUT);
  assert(rep.step == SM6125_EMMC_STEP_READ_LBA0);
  init_hw_from_real_gpt();
  g_hw.inject_cmd17_data_timeout = true;
  assert(run_sim(&rep) == SM6125_EMMC_RC_DATA_TIMEOUT);
  assert(rep.step == SM6125_EMMC_STEP_READ_LBA0);
  init_hw_from_real_gpt();
  g_hw.inject_cmd17_data_crc = true;
  assert(run_sim(&rep) == SM6125_EMMC_RC_DATA_ERROR);
  assert(rep.step == SM6125_EMMC_STEP_READ_LBA0);

  /* 14. MBR corruption */
  init_hw_from_real_gpt();
  g_hw.gpt_primary[510] = 0x00u;
  assert(run_sim(&rep) == SM6125_EMMC_RC_BAD_MBR);

  /* 15. Primary GPT header CRC mismatch */
  init_hw_from_real_gpt();
  g_hw.gpt_primary[512 + 24] ^= 1u;
  assert(run_sim(&rep) == SM6125_EMMC_RC_BAD_GPT_HDR);

  /* 16. Primary GPT partition array CRC mismatch */
  init_hw_from_real_gpt();
  g_hw.gpt_primary[1024 + 50] ^= 1u;
  assert(run_sim(&rep) == SM6125_EMMC_RC_BAD_GPT_PARTS);

  /* 17. Missing userdata entry (recompute parts_crc cleanly) */
  init_hw_from_real_gpt();
  g_hw.gpt_primary[1024 + 86 * 128 + 56] = 'X';
  put_le32(&g_hw.gpt_primary[512 + 88], host_crc32(&g_hw.gpt_primary[1024], 88u * 128u));
  put_le32(&g_hw.gpt_primary[512 + 16], 0u);
  put_le32(&g_hw.gpt_primary[512 + 16], host_crc32(&g_hw.gpt_primary[512], 92u));
  assert(run_sim(&rep) == SM6125_EMMC_RC_NO_USERDATA);

  /* 18. Backup GPT header corruption */
  init_hw_from_real_gpt();
  g_hw.gpt_backup[16] ^= 0x55u;
  assert(run_sim(&rep) == SM6125_EMMC_RC_BAD_GPT_BACKUP);

  /* 19. Format truncation & worst-case length */
  sm6125_emmc_ident_report_t worst;
  memset(&worst, 0xFF, sizeof(worst));
  worst.ok = false;
  worst.rc = -19;
  worst.step = 20u;
  worst.fail_valid = true;
  worst.has_ocr = true;
  worst.has_cid = true;
  worst.has_csd = true;
  worst.has_ext_csd = true;
  worst.has_gpt = true;
  worst.bak_ok = true;
  worst.has_userdata = true;
  assert(sm6125_emmc_ident_format_report(&worst, buf, sizeof(buf)) < 700);
  char s16[16];
  assert(sm6125_emmc_ident_format_report(&worst, s16, sizeof(s16)) == 15);
  assert(s16[15] == '\0');
  char tiny[1] = {'Z'};
  assert(sm6125_emmc_ident_format_report(&worst, tiny, sizeof(tiny)) == 0 && tiny[0] == '\0');
  assert(sm6125_emmc_ident_format_report(&worst, NULL, 0) == 0);
  return 24;
}

int main(int argc, char **argv) {
  const char *gpt_path = (argc > 1) ? argv[1] : "/Volumes/Leo2/backup-ginkgo/gpt_primary.bin";
  FILE *f = fopen(gpt_path, "rb");
  assert(f != NULL);
  g_gpt_real_len = fread(g_gpt_real, 1u, sizeof(g_gpt_real), f);
  fclose(f);
  assert(g_gpt_real_len == 65536u);

  int checks = 0;
  checks += test_happy_path_real_gpt();
  checks += test_error_and_corruption_cases();
  printf("emmc_core_host: %d checks passed\n", checks);
  return 0;
}
