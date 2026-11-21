// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CommonCrypto/CommonDigest.h>

#include "Sm6125EmmcCore.h"
#include "Sm6125EmmcReadOnly.h"
#include "Sm6125ShngRoot.h"

#define TEST_SECTORS 122142720ULL
#define BAK_GPT_LBA (TEST_SECTORS - 1ULL)

#define SIM_DESC_PA 0x50000000ULL
#define SIM_DATA_PA 0x50100000ULL
#define SIM_XFER_MAX_SECTORS 64u
#define SIM_XFER_BUF_BYTES (SIM_XFER_MAX_SECTORS * 512u)
#define SIM_CANARY_BYTES 4096u

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
  uint16_t auto_cmd_status;
  uint16_t host_control2;
  uint8_t adma_error;
  uint32_t adma_address;
  uint32_t adma_address_hi;

  uint32_t dll_config;
  uint32_t vendor_spec;
  uint32_t pwrctl_status;
  uint32_t pwrctl_mask;
  uint32_t pwrctl_ctl;
  uint32_t vendor_spec3;
  uint32_t ddr_config;

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

  /* Fase 3.3A fault injections */
  bool pending_switch_err;
  bool inject_cmd6_hs_err;
  bool inject_cmd6_bw_err;
  bool inject_cmd6_ddr_err;
  uint32_t cmd13_prg_polls;
  bool inject_hs_ext_csd_corrupt;
  bool inject_hs_pio_crc_corrupt;
  bool inject_adma_err;
  bool inject_auto_cmd_err;
  bool inject_adma_canary_overrun;
  bool inject_ddr_bitflip;
  bool inject_ddr_gpt_corrupt;
  bool inject_skip_ddr52_cap;
  bool card_selected;

  /* Tail write simulation & cache model */
  bool allow_write_cmds;
  bool simulate_cache_ctrl;
  bool inject_write_r1_err;
  bool inject_flush_switch_err;
  uint64_t tail_lba_base;
  uint32_t tail_sector_count;
  uint32_t write_cmd_count;
  uint32_t flush_cmd_count;
  uint8_t tail_nvm[512u * 512u];
  uint8_t tail_staged[512u * 512u];

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
  uint8_t pio_multi_buf[SIM_XFER_BUF_BYTES];
  uint32_t pio_block_idx;
  size_t pio_off;
  bool pio_active;

  /* Simulated CPU vs DMA DRAM cache coherence buffers */
  uint8_t sim_desc_cpu[4096];
  uint8_t sim_desc_dram[4096];
  uint8_t sim_data_cpu[SIM_XFER_BUF_BYTES + SIM_CANARY_BYTES];
  uint8_t sim_data_dram[SIM_XFER_BUF_BYTES + SIM_CANARY_BYTES];
  bool desc_cpu_dirty;
  bool data_cpu_dirty;
  bool data_cache_stale;
  uint32_t clean_desc_calls;
  uint32_t flush_data_calls;
  uint32_t inv_data_calls;

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

static void fill_synth_sector(uint32_t lba, uint8_t out[512]) {
  for (size_t i = 0u; i < 512u; i++) {
    out[i] = (uint8_t)(((lba * 131u) + (i * 17u)) ^ (lba >> 8u));
  }
}

static uint16_t dma_rd_le16(const uint8_t *p) {
  return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8u);
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
  g_hw.ddr_config = 0x80040873u;
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
  g_hw.tail_lba_base = 0x02438000ULL;
  g_hw.tail_sector_count = 512u;
  memset(g_hw.tail_nvm, 0, sizeof(g_hw.tail_nvm));
  memset(g_hw.tail_staged, 0, sizeof(g_hw.tail_staged));

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

static uint64_t sim_now_us(void) {
  return g_hw.time_us;
}

static void sim_cache_clean_desc(void *ctx, size_t offset, size_t len) {
  (void)ctx;
  assert(offset + len <= sizeof(g_hw.sim_desc_dram));
  memcpy(g_hw.sim_desc_dram + offset, g_hw.sim_desc_cpu + offset, len);
  g_hw.desc_cpu_dirty = false;
  g_hw.clean_desc_calls++;
}

static void sim_cache_flush_data(void *ctx, size_t offset, size_t len) {
  (void)ctx;
  assert(offset + len <= sizeof(g_hw.sim_data_dram));
  memcpy(g_hw.sim_data_dram + offset, g_hw.sim_data_cpu + offset, len);
  g_hw.data_cpu_dirty = false;
  g_hw.data_cache_stale = false;
  g_hw.flush_data_calls++;
}

static void sim_cache_invalidate_data(void *ctx, size_t offset, size_t len) {
  (void)ctx;
  assert(offset + len <= sizeof(g_hw.sim_data_dram));
  /* Simulate eviction overwriting DRAM if CPU dirtied cache without flush */
  if (!g_hw.data_cpu_dirty) {
    memcpy(g_hw.sim_data_cpu + offset, g_hw.sim_data_dram + offset, len);
    g_hw.data_cache_stale = false;
  }
  g_hw.inv_data_calls++;
}

static void load_sector_for_lba(uint32_t lba, uint8_t out[512]) {
  if ((size_t)(lba + 1u) * 512u <= g_hw.gpt_primary_len) {
    memcpy(out, g_hw.gpt_primary + (size_t)lba * 512u, 512u);
    if (g_hw.inject_ddr_gpt_corrupt && (g_hw.host_control2 & 0x7u) == 4u && lba == 2u) {
      out[0] ^= 0xFFu;
    }
  } else if (lba == (uint32_t)BAK_GPT_LBA) {
    memcpy(out, g_hw.gpt_backup, 512u);
  } else if (g_hw.tail_sector_count > 0u &&
             lba >= g_hw.tail_lba_base &&
             lba < g_hw.tail_lba_base + g_hw.tail_sector_count) {
    const size_t idx = (size_t)(lba - g_hw.tail_lba_base);
    memcpy(out, g_hw.tail_nvm + idx * 512u, 512u);
  } else if (lba >= 0x01298000u && lba <= 0x0747BFDEu) {
    fill_synth_sector(lba, out);
    if (g_hw.inject_ddr_bitflip && (g_hw.host_control2 & 0x7u) == 4u && lba == 0x01298100u) {
      out[17] ^= 0x04u;
    }
  } else {
    fprintf(stderr, "Unexpected eMMC read LBA 0x%08x\n", lba);
    abort();
  }
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
    case 6u:
    case 7u:
    case 8u:
    case 9u:
    case 13u:
    case 17u:
    case 18u:
      break;
    case 24u:
    case 25u:
      if (!g_hw.allow_write_cmds) {
        fprintf(stderr, "FORBIDDEN EMMC WRITE CMD %u LBA 0x%08x\n", cmd, g_hw.argument);
        abort();
      }
      break;
    default:
      fprintf(stderr, "FORBIDDEN EMMC CMD %u\n", cmd);
      abort();
  }

  /* Verify card clock has stabilized >= 1000 us and SD_CLK_EN is active */
  assert(g_hw.clk_enable_time_us > 0u);
  assert(g_hw.time_us - g_hw.clk_enable_time_us >= 1000u);
  assert((g_hw.clock_control & 0x0007u) == 0x0007u);

  if (cmd == 2u || cmd == 3u || cmd == 6u || cmd == 7u || cmd == 8u ||
      cmd == 9u || cmd == 13u || cmd == 17u || cmd == 18u ||
      cmd == 24u || cmd == 25u) {
    assert((flags & 0x08u) != 0u); /* CRC check must ALWAYS be enabled */
  }

  g_hw.pio_active = false;
  g_hw.pio_block_idx = 0u;
  g_hw.pio_off = 0u;
  g_hw.present_state &= ~0x00000800u; /* BRR clear */

  switch (cmd) {
    case 0u:
      g_hw.card_selected = false;
      g_hw.ext_csd[183] = 0x00u;
      g_hw.ext_csd[185] = 0x00u;
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
        g_hw.card_selected = true;
        g_hw.int_status |= 0x1u;
        g_hw.resp[0] = 0x00000700u;
      }
      break;
    case 6u: {
      assert(g_hw.card_selected);
      assert((flags & 0x03u) == 0x02u || (flags & 0x03u) == 0x03u);
      g_hw.int_status |= 0x1u;
      g_hw.resp[0] = 0x00000900u;
      /* Strictly allowlist non-destructive timing/width CMD6 switches */
      if (g_hw.argument == 0x03B90100u) {
        if (g_hw.inject_cmd6_hs_err) {
          g_hw.pending_switch_err = true;
        } else {
          g_hw.ext_csd[185] = 0x01u;
          g_hw.cmd13_prg_polls = 2u;
        }
      } else if (g_hw.argument == 0x03B70200u) {
        if (g_hw.inject_cmd6_bw_err) {
          g_hw.pending_switch_err = true;
        } else {
          g_hw.ext_csd[183] = 0x02u;
          g_hw.cmd13_prg_polls = 2u;
        }
      } else if (g_hw.argument == 0x03B70600u) {
        if (g_hw.inject_cmd6_ddr_err) {
          g_hw.pending_switch_err = true;
        } else {
          g_hw.ext_csd[183] = 0x06u;
          g_hw.cmd13_prg_polls = 2u;
        }
      } else if (g_hw.argument == 0x03200100u) {
        assert(g_hw.allow_write_cmds);
        g_hw.flush_cmd_count++;
        if (g_hw.inject_flush_switch_err) {
          g_hw.pending_switch_err = true;
        } else {
          memcpy(g_hw.tail_nvm, g_hw.tail_staged, sizeof(g_hw.tail_nvm));
          g_hw.cmd13_prg_polls = 2u;
        }
      } else {
        fprintf(stderr, "FORBIDDEN EMMC CMD6 ARG 0x%08x\n", g_hw.argument);
        abort();
      }
      break;
    }
    case 13u:
      assert(g_hw.argument == 0x00010000u);
      g_hw.int_status |= 0x1u;
      if (g_hw.cmd13_prg_polls > 0u) {
        g_hw.cmd13_prg_polls--;
        g_hw.resp[0] = 0x00000E00u; /* PRG, not ready */
      } else {
        g_hw.resp[0] = 0x00000900u |
                       (g_hw.pending_switch_err ? 0x00000080u : 0u) |
                       (g_hw.inject_write_r1_err ? 0x04000000u : 0u); /* WP_VIOLATION */
        g_hw.pending_switch_err = false;
        g_hw.inject_write_r1_err = false;
      }
      break;
    case 8u:
      assert((flags & 0x20u) != 0u);
      assert((g_hw.transfer_mode & 0x0010u) != 0u);
      assert(g_hw.block_size == 512u && g_hw.block_count == 1u);
      if (g_hw.ext_csd[183] == 2u) {
        assert((g_hw.host_control & 0x20u) != 0u);
        assert((g_hw.host_control & 0x04u) != 0u);
        assert(g_hw.gcc_apps_cfg == 0x0000020Bu);
      }
      if (g_hw.inject_cmd8_data_crc) {
        g_hw.int_status |= 0x00208001u; /* CC + ERROR_INT | DATA_CRC */
      } else {
        g_hw.int_status |= 0x00000021u; /* CC + BRR; TC fires after 1 block drained */
        g_hw.present_state |= 0x00000800u;
        memcpy(g_hw.pio_multi_buf, g_hw.ext_csd, 512u);
        if (g_hw.ext_csd[185] == 1u) {
          /* Note JESD84-B51: BUS_WIDTH [183] reads back 0 even in 8-bit mode */
          g_hw.pio_multi_buf[183] = 0x00u;
          if (g_hw.inject_hs_ext_csd_corrupt) {
            g_hw.pio_multi_buf[212] ^= 0xFFu;
          }
        }
        g_hw.pio_active = true;
      }
      g_hw.resp[0] = 0x00000900u;
      break;
    case 17u: {
      assert((flags & 0x20u) != 0u);
      assert((g_hw.transfer_mode & 0x0010u) != 0u);
      assert(g_hw.block_size == 512u && g_hw.block_count == 1u);
      g_hw.resp[0] = 0x00000900u;
      if ((g_hw.transfer_mode & 0x0001u) != 0u) {
        /* Single-block 32-bit ADMA2 */
        assert(g_hw.transfer_mode == 0x0011u);
        assert((g_hw.host_control & 0x3Cu) == 0x34u);
        assert((g_hw.int_enable & 0x03000000u) == 0x03000000u);
        assert(g_hw.adma_address_hi == 0u);
        assert(g_hw.adma_address == (uint32_t)SIM_DESC_PA);
        assert(!g_hw.desc_cpu_dirty);
        assert(!g_hw.data_cpu_dirty);
        const uint16_t attr = dma_rd_le16(g_hw.sim_desc_dram + 0u);
        const uint16_t len = dma_rd_le16(g_hw.sim_desc_dram + 2u);
        const uint32_t addr = le32(g_hw.sim_desc_dram + 4u);
        assert(attr == 0x0023u);
        assert(len == 512u);
        assert(addr == (uint32_t)SIM_DATA_PA);
        if (g_hw.inject_adma_err) {
          g_hw.adma_error = 0x05u;
          g_hw.int_status |= 0x02008001u;
          break;
        }
        load_sector_for_lba(g_hw.argument, g_hw.sim_data_dram);
        if (g_hw.inject_adma_canary_overrun) {
          g_hw.sim_data_dram[SIM_XFER_BUF_BYTES] = 0x00u;
        }
        g_hw.data_cache_stale = true;
        g_hw.int_status |= 0x00000003u; /* CC + TC */
        break;
      }
      if (g_hw.inject_cmd17_brr_timeout) {
        g_hw.int_status |= 0x00000001u; /* CC only, no BRR */
        break;
      }
      if (g_hw.inject_cmd17_data_timeout || g_hw.inject_cmd17_data_crc) {
        g_hw.int_status |= g_hw.inject_cmd17_data_timeout ? 0x00108021u : 0x00208021u;
        g_hw.present_state |= 0x00000800u;
        memset(g_hw.pio_multi_buf, 0, 512u);
        g_hw.pio_active = true;
        break;
      }
      load_sector_for_lba(g_hw.argument, g_hw.pio_multi_buf);
      g_hw.int_status |= 0x00000021u; /* CC + BRR; TC fires on 128th word */
      g_hw.present_state |= 0x00000800u;
      g_hw.pio_active = true;
      break;
    }
    case 18u: {
      assert((flags & 0x20u) != 0u);
      assert(g_hw.block_size == 512u && g_hw.block_count >= 1u && g_hw.block_count <= SIM_XFER_MAX_SECTORS);
      assert((g_hw.dll_config & (3u << 29)) == (3u << 29));
      assert(g_hw.vendor_spec == 0x00018A9Cu);
      assert(g_hw.ext_csd[185] == 1u);
      g_hw.resp[0] = 0x00000900u;

      if ((g_hw.transfer_mode & 0x0001u) == 0u) {
        /* Multi-block PIO with Auto CMD12 */
        assert(g_hw.transfer_mode == 0x0036u);
        assert((g_hw.host_control & 0x3Cu) == 0x24u);
        assert(g_hw.gcc_apps_cfg == 0x0000020Bu);
        assert(g_hw.host_control2 == 0x0008u);
        for (uint32_t s = 0u; s < g_hw.block_count; s++) {
          load_sector_for_lba(g_hw.argument + s, g_hw.pio_multi_buf + (size_t)s * 512u);
        }
        if (g_hw.inject_hs_pio_crc_corrupt && g_hw.argument == 2u) {
          g_hw.pio_multi_buf[17] ^= 0x55u;
        }
        g_hw.time_us += (uint64_t)g_hw.block_count * 12u;
        g_hw.int_status |= 0x00000021u; /* CC + first sector BRR */
        g_hw.present_state |= 0x00000800u;
        g_hw.pio_active = true;
      } else {
        /* 32-bit ADMA2 with Auto CMD12 */
        assert(g_hw.transfer_mode == 0x0037u);
        assert((g_hw.host_control & 0x3Cu) == 0x34u);
        assert((g_hw.int_enable & 0x03000000u) == 0x03000000u);
        assert(g_hw.adma_address_hi == 0u);
        assert(g_hw.adma_address == (uint32_t)SIM_DESC_PA);
        assert(!g_hw.desc_cpu_dirty);
        assert(!g_hw.data_cpu_dirty);

        if ((g_hw.host_control2 & 0x0007u) == 0x0004u) {
          assert(g_hw.gcc_apps_cfg == 0x00000205u);
          assert(g_hw.ddr_config == 0x80040873u);
          assert(g_hw.ext_csd[183] == 6u);
          g_hw.time_us += (uint64_t)g_hw.block_count * 4u;
        } else {
          assert(g_hw.host_control2 == 0x0008u);
          assert(g_hw.gcc_apps_cfg == 0x0000020Bu);
          assert(g_hw.ext_csd[183] == 2u);
          g_hw.time_us += (uint64_t)g_hw.block_count * 7u;
        }

        const uint16_t attr = dma_rd_le16(g_hw.sim_desc_dram + 0u);
        const uint16_t len = dma_rd_le16(g_hw.sim_desc_dram + 2u);
        const uint32_t addr = le32(g_hw.sim_desc_dram + 4u);
        assert(attr == 0x0023u);
        assert(len == g_hw.block_count * 512u);
        assert(addr == (uint32_t)SIM_DATA_PA);

        if (g_hw.inject_adma_err) {
          g_hw.adma_error = 0x05u;
          g_hw.int_status |= 0x02008001u; /* ERROR | ADMA_ERR | CC */
          break;
        }
        if (g_hw.inject_auto_cmd_err) {
          g_hw.auto_cmd_status = 0x0002u;
          g_hw.int_status |= 0x01008001u; /* ERROR | ACMD12ERR | CC */
          break;
        }
        for (uint32_t s = 0u; s < g_hw.block_count; s++) {
          load_sector_for_lba(g_hw.argument + s, g_hw.sim_data_dram + (size_t)s * 512u);
        }
        if (g_hw.inject_adma_canary_overrun) {
          g_hw.sim_data_dram[SIM_XFER_BUF_BYTES] = 0x00u;
        }
        g_hw.data_cache_stale = true;
        g_hw.int_status |= 0x00000003u; /* CC + TC */
      }
      break;
    }
    case 24u:
    case 25u: {
      assert(g_hw.allow_write_cmds);
      assert((flags & 0x20u) != 0u);
      const uint16_t expect_bc = (cmd == 24u) ? 1u : g_hw.block_count;
      assert(g_hw.block_size == 512u && g_hw.block_count == expect_bc &&
             g_hw.block_count >= 1u && g_hw.block_count <= SIM_XFER_MAX_SECTORS);
      if (cmd == 24u) {
        assert(g_hw.transfer_mode == 0x0001u);
      } else {
        assert(g_hw.transfer_mode == 0x0027u);
      }
      assert((g_hw.host_control & 0x3Cu) == 0x34u);
      assert(g_hw.adma_address_hi == 0u);
      assert(g_hw.adma_address == (uint32_t)SIM_DESC_PA);
      assert(!g_hw.desc_cpu_dirty);
      assert(!g_hw.data_cpu_dirty);
      assert(g_hw.argument >= g_hw.tail_lba_base);
      assert((uint64_t)g_hw.argument + g_hw.block_count <=
             g_hw.tail_lba_base + g_hw.tail_sector_count);

      const uint16_t attr = dma_rd_le16(g_hw.sim_desc_dram + 0u);
      const uint16_t len = dma_rd_le16(g_hw.sim_desc_dram + 2u);
      const uint32_t addr = le32(g_hw.sim_desc_dram + 4u);
      assert(attr == 0x0023u);
      assert(len == g_hw.block_count * 512u);
      assert(addr == (uint32_t)SIM_DATA_PA);

      g_hw.resp[0] = 0x00000900u;
      if (g_hw.inject_adma_err) {
        g_hw.adma_error = 0x05u;
        g_hw.int_status |= 0x02008001u;
        break;
      }
      const size_t off = (size_t)(g_hw.argument - g_hw.tail_lba_base) * 512u;
      memcpy(g_hw.tail_staged + off, g_hw.sim_data_dram, len);
      if (!g_hw.simulate_cache_ctrl) {
        memcpy(g_hw.tail_nvm + off, g_hw.sim_data_dram, len);
      }
      if (g_hw.inject_adma_canary_overrun) {
        g_hw.sim_data_dram[SIM_XFER_BUF_BYTES] = 0x00u;
      }
      g_hw.cmd13_prg_polls = 2u;
      g_hw.write_cmd_count++;
      g_hw.int_status |= 0x00000003u;
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
    case SM6125_SDHCI_ADMA_ERROR: return g_hw.adma_error;
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
    case SM6125_SDHCI_AUTO_CMD_STATUS: return g_hw.auto_cmd_status;
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
      assert(g_hw.pio_active && g_hw.pio_block_idx < g_hw.block_count && g_hw.pio_off + 4u <= 512u);
      uint32_t v = le32(g_hw.pio_multi_buf + (size_t)g_hw.pio_block_idx * 512u + g_hw.pio_off);
      g_hw.pio_off += 4u;
      if (g_hw.pio_off == 512u) {
        g_hw.pio_block_idx++;
        g_hw.pio_off = 0u;
        if (g_hw.pio_block_idx < g_hw.block_count) {
          g_hw.int_status |= 0x00000020u;
          g_hw.present_state |= 0x00000800u;
        } else {
          g_hw.present_state &= ~0x00000800u;
          g_hw.int_status |= 0x00000002u;
        }
      }
      return v;
    }
    case SM6125_SDHCI_PRESENT_STATE: return g_hw.present_state;
    case SM6125_SDHCI_INT_STATUS: return g_hw.int_status;
    case SM6125_SDHCI_ADMA_ADDRESS: return g_hw.adma_address;
    case SM6125_SDHCI_ADMA_ADDRESS_HI: return g_hw.adma_address_hi;
    case SM6125_SDHCI_V5_DLL_CONFIG: return g_hw.dll_config;
    case SM6125_SDHCI_V5_VENDOR_SPEC: return g_hw.vendor_spec;
    case SM6125_SDHCI_V5_PWRCTL_STATUS: return g_hw.pwrctl_status;
    case SM6125_SDHCI_V5_PWRCTL_CLEAR: return g_hw.pwrctl_status;
    case SM6125_SDHCI_V5_VENDOR_SPEC3: return g_hw.vendor_spec3;
    case SM6125_SDHCI_V5_DDR_CONFIG: return g_hw.ddr_config;
    default: abort();
  }
}

static void sim_w8(uintptr_t base, uint32_t off, uint8_t val) {
  assert(base == (uintptr_t)&g_hw + 0x1000u);
  if (g_hw.block_sdhci_if_clocks_off) assert(clocks_running());
  switch (off) {
    case SM6125_SDHCI_HOST_CONTROL: g_hw.host_control = val; break;
    case SM6125_SDHCI_POWER_CONTROL:
      g_hw.power_control = val;
      if ((val & 1u) != 0u && !g_hw.omit_bus_on_pwr_status) {
        g_hw.pwrctl_status |= 0x2u; /* CORE_PWRCTL_BUS_ON */
      }
      break;
    case SM6125_SDHCI_TIMEOUT_CONTROL: g_hw.timeout_control = val; break;
    case SM6125_SDHCI_SOFTWARE_RESET:
      g_hw.software_reset = val;
      if ((val & 0x06u) != 0u) {
        g_hw.present_state &= ~0x00000803u;
      }
      break;
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
    case SM6125_SDHCI_INT_STATUS:
      g_hw.int_status &= ~val;
      if ((val & 0x02000000u) != 0u) g_hw.adma_error = 0u;
      if ((val & 0x01000000u) != 0u) g_hw.auto_cmd_status = 0u;
      break;
    case SM6125_SDHCI_INT_ENABLE: g_hw.int_enable = val; break;
    case SM6125_SDHCI_SIGNAL_ENABLE: g_hw.signal_enable = val; break;
    case SM6125_SDHCI_ADMA_ADDRESS: g_hw.adma_address = val; break;
    case SM6125_SDHCI_ADMA_ADDRESS_HI: g_hw.adma_address_hi = val; break;
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
    case SM6125_SDHCI_V5_DDR_CONFIG: g_hw.ddr_config = val; break;
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
    .now_us = sim_now_us,
};

static sm6125_emmc_dma_buf_t make_sim_dma_buf(void) {
  sm6125_emmc_dma_buf_t dma = {
      .desc_va = g_hw.sim_desc_cpu,
      .desc_pa = SIM_DESC_PA,
      .desc_size = sizeof(g_hw.sim_desc_cpu),
      .data_va = g_hw.sim_data_cpu,
      .data_pa = SIM_DATA_PA,
      .data_size = sizeof(g_hw.sim_data_cpu),
      .canary_offset = SIM_XFER_BUF_BYTES,
      .canary_size = SIM_CANARY_BYTES,
      .cache_clean_desc = sim_cache_clean_desc,
      .cache_flush_data = sim_cache_flush_data,
      .cache_invalidate_data = sim_cache_invalidate_data,
      .cache_ctx = NULL,
  };
  return dma;
}

static int run_sim(sm6125_emmc_ident_report_t *rep) {
  memset(rep, 0, sizeof(*rep));
  return sm6125_emmc_ident_gpt_run(&g_hw_ops, (uintptr_t)&g_hw, (uintptr_t)&g_hw + 0x1000u, rep);
}

static int run_fast_sim(sm6125_emmc_fast_report_t *rep, bool with_dma) {
  memset(rep, 0, sizeof(*rep));
  g_hw.desc_cpu_dirty = true;
  g_hw.data_cpu_dirty = true;
  sm6125_emmc_dma_buf_t dma = make_sim_dma_buf();
  return sm6125_emmc_fast_bench_run(
      &g_hw_ops, (uintptr_t)&g_hw, (uintptr_t)&g_hw + 0x1000u,
      with_dma ? &dma : NULL, rep);
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

static int test_fast_bench_suite(void) {
  sm6125_emmc_fast_report_t rep;
  char buf[768];

  /* 1. Full happy path: Rung 1 (HS50 PIO) + Rung 2 (HS50 ADMA2) + Rung 3 (DDR52 ADMA2) */
  init_hw_from_real_gpt();
  g_hw.block_sdhci_if_clocks_off = true;
  assert(run_fast_sim(&rep, true) == SM6125_EMMC_RC_OK);
  assert(rep.ok);
  assert(rep.active_mode == SM6125_EMMC_FAST_MODE_DDR52_ADMA);
  assert(rep.step == SM6125_EMMC_STEP_FAST_DONE);
  assert(rep.hs50_pio_ok && rep.hs50_pio_rc == SM6125_EMMC_RC_OK);
  assert(rep.hs50_adma_ok && rep.hs50_adma_rc == SM6125_EMMC_RC_OK);
  assert(rep.ddr52_ok && rep.ddr52_rc == SM6125_EMMC_RC_OK);
  assert(rep.hs50_pio_kibs > 0u && rep.hs50_adma_kibs > 0u && rep.ddr52_kibs > 0u);
  assert(rep.hs50_adma_crc != 0u && rep.hs50_adma_crc == rep.ddr52_crc);
  assert(rep.gpt_parts_crc == 0x8AB9C302u);
  assert(rep.acfg == 0x00000205u && rep.hc == 0x34u && rep.hc2 == 0x000Cu && rep.ddr_cfg == 0x80040873u);
  /* 2 x (1 GPT + 512 benchmark) ADMA2 transfers */
  assert(g_hw.clean_desc_calls == 1026u);
  assert(g_hw.flush_data_calls == 1026u);
  assert(g_hw.inv_data_calls == 1026u);
  for (size_t i = 0u; i < SIM_CANARY_BYTES; i++) {
    assert(g_hw.sim_data_cpu[SIM_XFER_BUF_BYTES + i] == 0xA5u);
  }
  size_t flen = sm6125_emmc_fast_format_report(&rep, buf, sizeof(buf));
  assert(flen > 0u && flen < 640u && flen == strlen(buf));
  assert(strstr(buf, "SM6125-EMMC-FAST ok=1 mode=ddr52-adma ") == buf);

  /* 2. Rung 3 non-fatal fail: DDR52 1-bit flip triggers DATA_MISMATCH, falls back to hs50-adma */
  init_hw_from_real_gpt();
  g_hw.inject_ddr_bitflip = true;
  assert(run_fast_sim(&rep, true) == SM6125_EMMC_RC_OK);
  assert(rep.ok && rep.active_mode == SM6125_EMMC_FAST_MODE_HS50_ADMA);
  assert(rep.hs50_pio_ok && rep.hs50_adma_ok && !rep.ddr52_ok);
  assert(rep.ddr52_rc == SM6125_EMMC_RC_DATA_MISMATCH);
  assert(rep.hs50_adma_crc != rep.ddr52_crc);
  assert(sm6125_emmc_fast_format_report(&rep, buf, sizeof(buf)) < 640u);
  assert(strstr(buf, "mode=hs50-adma") != NULL && strstr(buf, "FAIL r=3") != NULL);

  /* 3. Rung 3 non-fatal fail: DDR52 CMD6 switch error & missing capability */
  init_hw_from_real_gpt();
  g_hw.inject_cmd6_ddr_err = true;
  assert(run_fast_sim(&rep, true) == SM6125_EMMC_RC_OK);
  assert(rep.ok && rep.active_mode == SM6125_EMMC_FAST_MODE_HS50_ADMA);
  assert(!rep.ddr52_ok && rep.ddr52_rc == SM6125_EMMC_RC_SWITCH_ERROR);

  init_hw_from_real_gpt();
  g_hw.ext_csd[196] = 0x02u; /* HS52 only, no DDR52 */
  assert(run_fast_sim(&rep, true) == SM6125_EMMC_RC_OK);
  assert(rep.ok && rep.active_mode == SM6125_EMMC_FAST_MODE_HS50_ADMA);
  assert(!rep.ddr52_ok && rep.ddr52_rc == SM6125_EMMC_RC_SWITCH_ERROR);

  /* 4. Rung 2 fails (with Rung 1 ok): falls back to hs50-pio and skips Rung 3 */
  init_hw_from_real_gpt();
  g_hw.inject_adma_err = true;
  assert(run_fast_sim(&rep, true) == SM6125_EMMC_RC_OK);
  assert(rep.ok && rep.active_mode == SM6125_EMMC_FAST_MODE_HS50_PIO);
  assert(rep.hs50_pio_ok && !rep.hs50_adma_ok && !rep.ddr52_ok);
  assert(rep.hs50_adma_rc == SM6125_EMMC_RC_ADMA_ERROR);
  assert(g_hw.ext_csd[183] == 2u); /* Rung 3 never switched to DDR52 */

  init_hw_from_real_gpt();
  g_hw.inject_auto_cmd_err = true;
  assert(run_fast_sim(&rep, true) == SM6125_EMMC_RC_OK);
  assert(rep.ok && rep.active_mode == SM6125_EMMC_FAST_MODE_HS50_PIO);
  assert(!rep.hs50_adma_ok && rep.hs50_adma_rc == SM6125_EMMC_RC_AUTO_CMD_ERROR);

  init_hw_from_real_gpt();
  g_hw.inject_adma_canary_overrun = true;
  assert(run_fast_sim(&rep, true) == SM6125_EMMC_RC_OK);
  assert(rep.ok && rep.active_mode == SM6125_EMMC_FAST_MODE_HS50_PIO);
  assert(!rep.hs50_adma_ok && rep.hs50_adma_rc == SM6125_EMMC_RC_CANARY_CORRUPT);

  init_hw_from_real_gpt();
  assert(run_fast_sim(&rep, false) == SM6125_EMMC_RC_OK);
  assert(rep.ok && rep.active_mode == SM6125_EMMC_FAST_MODE_HS50_PIO);
  assert(!rep.hs50_adma_ok && rep.hs50_adma_rc == SM6125_EMMC_RC_BAD_ARG);

  /* 5. Rung 1 fail-closed cases */
  init_hw_from_real_gpt();
  g_hw.inject_cmd6_hs_err = true;
  assert(run_fast_sim(&rep, true) == SM6125_EMMC_RC_SWITCH_ERROR);
  assert(!rep.ok && rep.active_mode == SM6125_EMMC_FAST_MODE_NONE);

  init_hw_from_real_gpt();
  g_hw.inject_cmd6_bw_err = true;
  assert(run_fast_sim(&rep, true) == SM6125_EMMC_RC_SWITCH_ERROR);
  assert(!rep.ok && rep.active_mode == SM6125_EMMC_FAST_MODE_NONE);

  init_hw_from_real_gpt();
  g_hw.inject_hs_ext_csd_corrupt = true;
  assert(run_fast_sim(&rep, true) == SM6125_EMMC_RC_EXT_CSD_MISMATCH);
  assert(!rep.ok && rep.active_mode == SM6125_EMMC_FAST_MODE_NONE);

  init_hw_from_real_gpt();
  g_hw.inject_hs_pio_crc_corrupt = true;
  assert(run_fast_sim(&rep, true) == SM6125_EMMC_RC_BAD_GPT_PARTS);
  assert(!rep.ok && rep.active_mode == SM6125_EMMC_FAST_MODE_NONE);

  /* 6. Fast formatter bounds */
  sm6125_emmc_fast_report_t worst;
  memset(&worst, 0xFF, sizeof(worst));
  worst.ok = false;
  worst.active_mode = SM6125_EMMC_FAST_MODE_HS50_PIO;
  worst.hs50_pio_ok = true;
  worst.hs50_adma_ok = true;
  worst.ddr52_ok = true;
  worst.rc = -25;
  worst.hs50_pio_rc = -25;
  worst.hs50_adma_rc = -24;
  worst.ddr52_rc = -22;
  worst.fail_rc = -24;
  worst.fail_valid = true;
  assert(sm6125_emmc_fast_format_report(&worst, buf, sizeof(buf)) < 640u);
  char s16[16];
  assert(sm6125_emmc_fast_format_report(&worst, s16, sizeof(s16)) == 15u);
  assert(s16[15] == '\0');
  char tiny[1] = {'Q'};
  assert(sm6125_emmc_fast_format_report(&worst, tiny, sizeof(tiny)) == 0u && tiny[0] == '\0');
  assert(sm6125_emmc_fast_format_report(&worst, NULL, 0u) == 0u);
  return 28;
}

static void host_sha256(const uint8_t *data, size_t len, uint8_t out_digest[32]) {
  CC_SHA256(data, (CC_LONG)len, out_digest);
}

static void put_le64(uint8_t *p, uint64_t v) {
  put_le32(p, (uint32_t)(v & 0xFFFFFFFFULL));
  put_le32(p + 4u, (uint32_t)(v >> 32u));
}

static void build_test_shngroot(
    uint8_t hdr[4096],
    uint32_t version,
    uint32_t count,
    uint64_t image_bytes) {
  memset(hdr, 0, 4096u);
  memcpy(hdr, "SHNGROOT", 8u);
  put_le32(hdr + 8u, version);
  put_le32(hdr + 12u, count);
  put_le64(hdr + 16u, image_bytes);
}

static void set_shngroot_entry(
    uint8_t hdr[4096],
    uint32_t idx,
    const char *name,
    uint64_t offset,
    uint64_t size,
    uint32_t block_size,
    uint32_t flags,
    uint8_t fill_sha) {
  uint8_t *row = hdr + 24u + (size_t)idx * 128u;
  memset(row, 0, 128u);
  size_t nlen = strlen(name);
  if (nlen > 31u) nlen = 31u;
  memcpy(row, name, nlen);
  put_le64(row + 32u, offset);
  put_le64(row + 40u, size);
  put_le32(row + 48u, block_size);
  put_le32(row + 52u, flags);
  memset(row + 56u, fill_sha, 32u);
}

static void seal_test_shngroot(uint8_t hdr[4096]) {
  host_sha256(hdr, 4064u, hdr + 4064u);
}

static int test_ctrl_and_shngroot_suite(void) {
  /* 1. Mode selector for sm6125-emmc-block */
  assert(sm6125_emmc_block_select_mode(false, 0u) == SM6125_EMMC_BLOCK_MODE_OFF);
  assert(sm6125_emmc_block_select_mode(true, 0u) == SM6125_EMMC_BLOCK_MODE_OFF);
  assert(sm6125_emmc_block_select_mode(true, 1u) == SM6125_EMMC_BLOCK_MODE_DIAG);
  assert(sm6125_emmc_block_select_mode(true, 2u) == SM6125_EMMC_BLOCK_MODE_MOUNT_DIAG);
  assert(sm6125_emmc_block_select_mode(true, 3u) == SM6125_EMMC_BLOCK_MODE_PUBLISH);
  assert(sm6125_emmc_block_select_mode(true, 4u) == SM6125_EMMC_BLOCK_MODE_INVALID);

  /* 2. sm6125_emmc_ctrl_init happy path (DDR52 + ADMA2) + single & multi-chunk read_sectors */
  init_hw_from_real_gpt();
  g_hw.block_sdhci_if_clocks_off = true;
  sm6125_emmc_dma_buf_t dma = make_sim_dma_buf();
  sm6125_emmc_ctrl_t ctrl;
  assert(sm6125_emmc_ctrl_init(
             &ctrl, &g_hw_ops, (uintptr_t)&g_hw, (uintptr_t)&g_hw + 0x1000u, &dma) ==
         SM6125_EMMC_RC_OK);
  assert(ctrl.online);
  assert(ctrl.active_mode == SM6125_EMMC_FAST_MODE_DDR52_ADMA);
  assert(ctrl.sec_count == (uint32_t)TEST_SECTORS);
  assert(ctrl.userdata_first == 0x01298000ULL && ctrl.userdata_last == 0x0747BFDEULL);
  assert(ctrl.hc == 0x34u && ctrl.hc2 == 0x000Cu && ctrl.acfg == 0x00000205u);

  /* Single-sector read via CMD17 ADMA2 (LBA 0 and LBA 1) */
  uint8_t sec_buf[512];
  assert(sm6125_emmc_ctrl_read_sectors(&ctrl, 0ULL, 1u, sec_buf) == SM6125_EMMC_RC_OK);
  assert(host_crc32(sec_buf, 512u) == 0xA22AB8BAu);
  assert(sm6125_emmc_ctrl_read_sectors(&ctrl, 1ULL, 1u, sec_buf) == SM6125_EMMC_RC_OK);
  assert(host_crc32(sec_buf, 512u) == 0x3573B839u);

  /* Multi-chunk read: 130 sectors (64 + 64 + 2 sectors = 66560 bytes) from userdata_first */
  uint8_t *multi_buf = (uint8_t *)malloc(130u * 512u);
  assert(multi_buf != NULL);
  assert(sm6125_emmc_ctrl_read_sectors(&ctrl, ctrl.userdata_first, 130u, multi_buf) ==
         SM6125_EMMC_RC_OK);
  uint8_t expect_sec[512];
  for (uint32_t s = 0u; s < 130u; s++) {
    fill_synth_sector((uint32_t)(ctrl.userdata_first + s), expect_sec);
    assert(memcmp(multi_buf + (size_t)s * 512u, expect_sec, 512u) == 0);
  }

  /* Out-of-bounds and bad argument rejections */
  assert(sm6125_emmc_ctrl_read_sectors(&ctrl, 0ULL, 0u, sec_buf) == SM6125_EMMC_RC_BAD_ARG);
  assert(sm6125_emmc_ctrl_read_sectors(&ctrl, ctrl.sec_count, 1u, sec_buf) == SM6125_EMMC_RC_BAD_ARG);
  assert(sm6125_emmc_ctrl_read_sectors(&ctrl, ctrl.sec_count - 1ULL, 2u, multi_buf) ==
         SM6125_EMMC_RC_BAD_ARG);
  assert(sm6125_emmc_ctrl_read_sectors(&ctrl, 0ULL, 1u, NULL) == SM6125_EMMC_RC_BAD_ARG);

  /* Canary corruption detected during read_sectors */
  g_hw.inject_adma_canary_overrun = true;
  assert(sm6125_emmc_ctrl_read_sectors(&ctrl, 0ULL, 1u, sec_buf) == SM6125_EMMC_RC_CANARY_CORRUPT);

  /* 3. Fallback DDR52 -> HS50_ADMA on DDR52 GPT corruption or CMD6 failure */
  init_hw_from_real_gpt();
  g_hw.inject_ddr_gpt_corrupt = true;
  assert(sm6125_emmc_ctrl_init(
             &ctrl, &g_hw_ops, (uintptr_t)&g_hw, (uintptr_t)&g_hw + 0x1000u, &dma) ==
         SM6125_EMMC_RC_OK);
  assert(ctrl.online && ctrl.active_mode == SM6125_EMMC_FAST_MODE_HS50_ADMA);
  assert(ctrl.hc == 0x34u && ctrl.hc2 == 0x0008u && ctrl.acfg == 0x0000020Bu);
  assert(sm6125_emmc_ctrl_read_sectors(&ctrl, 0ULL, 1u, sec_buf) == SM6125_EMMC_RC_OK);
  assert(host_crc32(sec_buf, 512u) == 0xA22AB8BAu);

  /* 4. Fallback HS50_ADMA -> HS50_PIO when DMA is NULL or ADMA errors */
  init_hw_from_real_gpt();
  g_hw.inject_adma_err = true;
  assert(sm6125_emmc_ctrl_init(
             &ctrl, &g_hw_ops, (uintptr_t)&g_hw, (uintptr_t)&g_hw + 0x1000u, &dma) ==
         SM6125_EMMC_RC_OK);
  assert(ctrl.online && ctrl.active_mode == SM6125_EMMC_FAST_MODE_HS50_PIO);
  assert(ctrl.hc == 0x24u);
  assert(sm6125_emmc_ctrl_read_sectors(&ctrl, ctrl.userdata_first, 130u, multi_buf) ==
         SM6125_EMMC_RC_OK);
  for (uint32_t s = 0u; s < 130u; s++) {
    fill_synth_sector((uint32_t)(ctrl.userdata_first + s), expect_sec);
    assert(memcmp(multi_buf + (size_t)s * 512u, expect_sec, 512u) == 0);
  }
  free(multi_buf);

  /* 5. SHNGROOT parser v1 & v2 positive and negative cases */
  uint8_t hdr[4096];
  sm6125_shngroot_manifest_t man;
  const uint64_t part_cap = (0x0747BFDEULL - 0x01298000ULL + 1ULL) * 512ULL;

  /* Valid v2 with 2 RO windows + 2 data objects */
  build_test_shngroot(hdr, 2u, 4u, 0x240000000ULL);
  set_shngroot_entry(hdr, 0u, "OS", 0x100000ULL, 0x16A400000ULL, 512u, 1u, 0x11u);
  set_shngroot_entry(hdr, 1u, "SystemOS", 0x16A600000ULL, 0xC4E00000ULL, 512u, 1u, 0x22u);
  set_shngroot_entry(hdr, 2u, "OS.vroot", 0x230000000ULL, 0x2D48220ULL, 4096u, 3u, 0x33u);
  set_shngroot_entry(hdr, 3u, "SystemOS.vroot", 0x233000000ULL, 0x189C220ULL, 4096u, 3u, 0x44u);
  seal_test_shngroot(hdr);
  assert(sm6125_shngroot_parse(hdr, part_cap, host_sha256, &man) == SM6125_SHNGROOT_RC_OK);
  assert(man.version == 2u && man.entry_count == 4u && man.window_count == 2u && man.object_count == 2u);
  assert(strcmp(man.entries[0].name, "OS") == 0 && man.entries[0].offset == 0x100000ULL);
  assert(strcmp(man.entries[1].name, "SystemOS") == 0 && man.entries[1].flags == 1u);
  assert(strcmp(man.entries[2].name, "OS.vroot") == 0 && man.entries[2].flags == 3u);

  /* Corrupt magic */
  hdr[0] = 'X';
  assert(sm6125_shngroot_parse(hdr, part_cap, host_sha256, &man) == SM6125_SHNGROOT_RC_BAD_MAGIC);
  hdr[0] = 'S';

  /* Corrupt hash */
  hdr[4095] ^= 0x01u;
  assert(sm6125_shngroot_parse(hdr, part_cap, host_sha256, &man) == SM6125_SHNGROOT_RC_BAD_HASH);
  hdr[4095] ^= 0x01u;

  /* Data object in version 1 -> rejected */
  put_le32(hdr + 8u, 1u);
  seal_test_shngroot(hdr);
  assert(sm6125_shngroot_parse(hdr, part_cap, host_sha256, &man) == SM6125_SHNGROOT_RC_BAD_ENTRY);

  /* Overlapping windows -> rejected */
  build_test_shngroot(hdr, 1u, 2u, 0x240000000ULL);
  set_shngroot_entry(hdr, 0u, "OS", 0x100000ULL, 0x200000ULL, 512u, 1u, 0x11u);
  set_shngroot_entry(hdr, 1u, "SystemOS", 0x200000ULL, 0x200000ULL, 512u, 1u, 0x22u);
  seal_test_shngroot(hdr);
  assert(sm6125_shngroot_parse(hdr, part_cap, host_sha256, &man) == SM6125_SHNGROOT_RC_OVERLAP);

  /* Duplicate name -> rejected */
  build_test_shngroot(hdr, 1u, 2u, 0x240000000ULL);
  set_shngroot_entry(hdr, 0u, "OS", 0x100000ULL, 0x100000ULL, 512u, 1u, 0x11u);
  set_shngroot_entry(hdr, 1u, "OS", 0x300000ULL, 0x100000ULL, 512u, 1u, 0x22u);
  seal_test_shngroot(hdr);
  assert(sm6125_shngroot_parse(hdr, part_cap, host_sha256, &man) == SM6125_SHNGROOT_RC_DUPLICATE_NAME);

  /* Image larger than partition -> rejected */
  seal_test_shngroot(hdr);
  assert(sm6125_shngroot_parse(hdr, 0x100000ULL, host_sha256, &man) ==
         SM6125_SHNGROOT_RC_BAD_IMAGE_SIZE);

  /* Validate real on-disk userdata-root.img header if available */
  const char *real_ud =
      "/Volumes/Leo2/unixnu-ginkgo/UniXNU/out/ginkgo/ipados-17.7.11-21H461/ginkgo/userdata/userdata-root.img";
  FILE *uf = fopen(real_ud, "rb");
  if (uf != NULL) {
    assert(fread(hdr, 1u, 4096u, uf) == 4096u);
    fclose(uf);
    assert(sm6125_shngroot_parse(hdr, part_cap, host_sha256, &man) == SM6125_SHNGROOT_RC_OK);
    assert(man.version == 2u && man.entry_count == 4u);
    assert(man.window_count == 2u && man.object_count == 2u);
    assert(strcmp(man.entries[0].name, "OS") == 0 && man.entries[0].offset == 0x100000ULL &&
           man.entries[0].size == 6077580800ULL);
    assert(strcmp(man.entries[1].name, "SystemOS") == 0 && man.entries[1].offset == 0x16A600000ULL &&
           man.entries[1].size == 3303014400ULL);
    assert(strcmp(man.entries[2].name, "OS.vroot") == 0 && man.entries[2].offset == 0x22F400000ULL &&
           man.entries[2].size == 47481120ULL && man.entries[2].flags == 3u);
    assert(strcmp(man.entries[3].name, "SystemOS.vroot") == 0 && man.entries[3].offset == 0x232200000ULL &&
           man.entries[3].size == 25804800ULL && man.entries[3].flags == 3u);
  }
  return 26;
}

static int test_write_sandbox_suite(void) {
  init_hw_from_real_gpt();
  g_hw.block_sdhci_if_clocks_off = true;
  g_hw.simulate_cache_ctrl = true;
  g_hw.ext_csd[33] = 0x01u;
  g_hw.ext_csd[249] = 0x00u;
  g_hw.ext_csd[250] = 0x08u; /* 2 MiB volatile cache */

  sm6125_emmc_dma_buf_t dma = make_sim_dma_buf();
  sm6125_emmc_ctrl_t ctrl;
  assert(sm6125_emmc_ctrl_init(
             &ctrl, &g_hw_ops, (uintptr_t)&g_hw, (uintptr_t)&g_hw + 0x1000u, &dma) ==
         SM6125_EMMC_RC_OK);
  assert(ctrl.has_cache_ctrl);
  assert(!ctrl.shngroot_bounds_set && !ctrl.write_window_armed);

  uint8_t sec[512];
  memset(sec, 0x5Cu, sizeof(sec));

  /* Reads-only mode: before sandbox configuration every write/flush is refused without touching SDHCI */
  assert(sm6125_emmc_ctrl_write_sectors(&ctrl, g_hw.tail_lba_base, 1u, sec) ==
         SM6125_EMMC_RC_WRITE_PROTECTED);
  assert(sm6125_emmc_ctrl_set_write_window(&ctrl, g_hw.tail_lba_base, g_hw.tail_lba_base + 511ULL) ==
         SM6125_EMMC_RC_WRITE_PROTECTED);
  assert(sm6125_emmc_ctrl_flush_cache(&ctrl) == SM6125_EMMC_RC_WRITE_PROTECTED);

  /* Invalid SHNGROOT bounds refused */
  assert(sm6125_emmc_ctrl_set_shngroot_bounds(&ctrl, 0ULL) == SM6125_EMMC_RC_WRITE_PROTECTED);
  assert(sm6125_emmc_ctrl_set_shngroot_bounds(&ctrl, 4095ULL) == SM6125_EMMC_RC_WRITE_PROTECTED);
  assert(sm6125_emmc_ctrl_set_shngroot_bounds(&ctrl, 0x10000000000ULL) ==
         SM6125_EMMC_RC_WRITE_PROTECTED);

  /* Real SHNGROOT v2 image size: 0x233b00000 B => shngroot_end_lba = 0x02435800 */
  const uint64_t real_shngroot_bytes = 0x233b00000ULL;
  assert(sm6125_emmc_ctrl_set_shngroot_bounds(&ctrl, real_shngroot_bytes) == SM6125_EMMC_RC_OK);
  assert(ctrl.shngroot_first_lba == 0x01298000ULL);
  assert(ctrl.shngroot_end_lba == 0x02435800ULL);

  /* Write window touching SHNGROOT, guard gap (<+8192), or > userdata_last is refused */
  assert(sm6125_emmc_ctrl_set_write_window(&ctrl, ctrl.shngroot_first_lba, g_hw.tail_lba_base + 511ULL) ==
         SM6125_EMMC_RC_WRITE_PROTECTED);
  assert(sm6125_emmc_ctrl_set_write_window(
             &ctrl, ctrl.shngroot_end_lba + SM6125_EMMC_WRITE_GUARD_SECTORS - 1ULL,
             g_hw.tail_lba_base + 511ULL) == SM6125_EMMC_RC_WRITE_PROTECTED);
  assert(sm6125_emmc_ctrl_set_write_window(
             &ctrl, g_hw.tail_lba_base, ctrl.userdata_last + 1ULL) == SM6125_EMMC_RC_WRITE_PROTECTED);

  /* Arm exact tail window [tail_lba_base, tail_lba_base + 511] */
  assert(sm6125_emmc_ctrl_set_write_window(
             &ctrl, g_hw.tail_lba_base, g_hw.tail_lba_base + 511ULL) == SM6125_EMMC_RC_OK);

  /* With allow_write_cmds == false, verify negative LBA checks NEVER issue CMD24/25 */
  assert(sm6125_emmc_ctrl_write_sectors(&ctrl, 0ULL, 1u, sec) == SM6125_EMMC_RC_WRITE_PROTECTED);
  assert(sm6125_emmc_ctrl_write_sectors(&ctrl, 1ULL, 1u, sec) == SM6125_EMMC_RC_WRITE_PROTECTED);
  assert(sm6125_emmc_ctrl_write_sectors(&ctrl, BAK_GPT_LBA, 1u, sec) == SM6125_EMMC_RC_WRITE_PROTECTED);
  assert(sm6125_emmc_ctrl_write_sectors(&ctrl, ctrl.userdata_first, 8u, sec) ==
         SM6125_EMMC_RC_WRITE_PROTECTED);
  assert(sm6125_emmc_ctrl_write_sectors(
             &ctrl, ctrl.shngroot_end_lba + SM6125_EMMC_WRITE_GUARD_SECTORS - 1ULL, 1u, sec) ==
         SM6125_EMMC_RC_WRITE_PROTECTED);
  assert(sm6125_emmc_ctrl_write_sectors(&ctrl, g_hw.tail_lba_base - 1ULL, 2u, sec) ==
         SM6125_EMMC_RC_WRITE_PROTECTED);
  assert(sm6125_emmc_ctrl_write_sectors(&ctrl, g_hw.tail_lba_base + 510ULL, 3u, sec) ==
         SM6125_EMMC_RC_WRITE_PROTECTED);
  assert(g_hw.write_cmd_count == 0u);

  /* Positive CMD24 single-sector write + CMD25 multi-chunk write + volatile cache + CMD6 FLUSH_CACHE */
  g_hw.allow_write_cmds = true;
  fill_synth_sector(0x90000001u, sec);
  assert(sm6125_emmc_ctrl_write_sectors(&ctrl, g_hw.tail_lba_base, 1u, sec) == SM6125_EMMC_RC_OK);
  assert(g_hw.write_cmd_count == 1u && g_hw.cmd13_prg_polls == 0u);

  uint8_t *wbuf = (uint8_t *)malloc(130u * 512u);
  uint8_t *rbuf = (uint8_t *)malloc(130u * 512u);
  assert(wbuf != NULL && rbuf != NULL);
  for (uint32_t s = 0u; s < 130u; s++) {
    fill_synth_sector(0xA5000000u + s, wbuf + (size_t)s * 512u);
  }
  assert(sm6125_emmc_ctrl_write_sectors(&ctrl, g_hw.tail_lba_base + 8ULL, 130u, wbuf) ==
         SM6125_EMMC_RC_OK);
  assert(ctrl.write_req_count == 2ULL && ctrl.write_sectors_total == 131ULL);

  /* Volatile write cache model: before flush_cache, NVM is still zero; after flush_cache, NVM matches */
  memset(rbuf, 0xFF, 512u);
  assert(sm6125_emmc_ctrl_read_sectors(&ctrl, g_hw.tail_lba_base, 1u, rbuf) == SM6125_EMMC_RC_OK);
  assert(rbuf[0] == 0u);

  assert(sm6125_emmc_ctrl_flush_cache(&ctrl) == SM6125_EMMC_RC_OK);
  assert(g_hw.flush_cmd_count == 1u && ctrl.flush_count == 1ULL);

  memset(rbuf, 0, 130u * 512u);
  assert(sm6125_emmc_ctrl_read_sectors(&ctrl, g_hw.tail_lba_base, 1u, rbuf) == SM6125_EMMC_RC_OK);
  assert(memcmp(rbuf, sec, 512u) == 0);
  assert(sm6125_emmc_ctrl_read_sectors(&ctrl, g_hw.tail_lba_base + 8ULL, 130u, rbuf) ==
         SM6125_EMMC_RC_OK);
  assert(memcmp(rbuf, wbuf, 130u * 512u) == 0);

  /* Fault injections on write & flush */
  g_hw.inject_adma_canary_overrun = true;
  assert(sm6125_emmc_ctrl_write_sectors(&ctrl, g_hw.tail_lba_base, 1u, sec) ==
         SM6125_EMMC_RC_CANARY_CORRUPT);
  g_hw.inject_adma_canary_overrun = false;
  memset(dma.data_va + dma.canary_offset, 0xA5, dma.canary_size);
  memcpy(g_hw.sim_data_dram + dma.canary_offset, dma.data_va + dma.canary_offset, dma.canary_size);

  g_hw.inject_adma_err = true;
  assert(sm6125_emmc_ctrl_write_sectors(&ctrl, g_hw.tail_lba_base, 2u, wbuf) ==
         SM6125_EMMC_RC_ADMA_ERROR);
  g_hw.inject_adma_err = false;

  g_hw.inject_write_r1_err = true;
  assert(sm6125_emmc_ctrl_write_sectors(&ctrl, g_hw.tail_lba_base, 1u, sec) ==
         SM6125_EMMC_RC_R1_ERROR);

  g_hw.inject_flush_switch_err = true;
  assert(sm6125_emmc_ctrl_flush_cache(&ctrl) == SM6125_EMMC_RC_SWITCH_ERROR);

  /* Verify no-cache-ctrl mode commits writes and flushes cleanly without CMD6(32,1) */
  init_hw_from_real_gpt();
  g_hw.block_sdhci_if_clocks_off = true;
  g_hw.allow_write_cmds = true;
  g_hw.simulate_cache_ctrl = false;
  assert(sm6125_emmc_ctrl_init(
             &ctrl, &g_hw_ops, (uintptr_t)&g_hw, (uintptr_t)&g_hw + 0x1000u, &dma) ==
         SM6125_EMMC_RC_OK);
  assert(!ctrl.has_cache_ctrl);
  assert(sm6125_emmc_ctrl_set_shngroot_bounds(&ctrl, real_shngroot_bytes) == SM6125_EMMC_RC_OK);
  assert(sm6125_emmc_ctrl_set_write_window(
             &ctrl, g_hw.tail_lba_base, g_hw.tail_lba_base + 511ULL) == SM6125_EMMC_RC_OK);
  assert(sm6125_emmc_ctrl_write_sectors(&ctrl, g_hw.tail_lba_base + 2ULL, 1u, sec) ==
         SM6125_EMMC_RC_OK);
  assert(sm6125_emmc_ctrl_flush_cache(&ctrl) == SM6125_EMMC_RC_OK);
  assert(g_hw.flush_cmd_count == 0u && ctrl.flush_count == 1ULL);
  assert(sm6125_emmc_ctrl_read_sectors(&ctrl, g_hw.tail_lba_base + 2ULL, 1u, rbuf) ==
         SM6125_EMMC_RC_OK);
  assert(memcmp(rbuf, sec, 512u) == 0);

  free(wbuf);
  free(rbuf);
  return 31;
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
  checks += test_fast_bench_suite();
  checks += test_ctrl_and_shngroot_suite();
  checks += test_write_sandbox_suite();
  printf("emmc_core_host: %d checks passed\n", checks);
  return 0;
}
