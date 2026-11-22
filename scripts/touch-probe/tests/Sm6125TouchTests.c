// SPDX-License-Identifier: MIT
// Teste unitario em host para Sm6125TouchBoard.h, GeniSpi.c e Sm6125NvtProbe.c.
#include "../Sm6125TouchBoard.h"
#include "../Sm6125NvtProbe.h"
#include "GeniSpi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { \
  if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    exit(1); \
  } \
} while (0)

struct sim_se {
  uint32_t reg[0x1000 / 4];
  uint32_t wrap_ver;
  uint32_t hw_param0;
  uint32_t fw_rev;
  uint32_t if_disable;

  uint32_t tx_fifo[32];
  unsigned tx_cnt;
  uint32_t rx_fifo[64];
  unsigned rx_head;
  unsigned rx_tail;

  uint8_t last_page_h;
  uint8_t last_page_l;
  uint32_t reset_seq;
  int fast_read_disabled;
  int nvt_alt_addr;

  uint8_t cur_tx[128];
  size_t cur_len;
  uint32_t cmd_op;
  int active;
  uint32_t irq;
};

static uint8_t nvt_slave_byte(struct sim_se *m, size_t idx, uint8_t tx_b) {
  if (m->reg[SE_SPI_LOOPBACK / 4] & GENI_LOOPBACK_ENABLE) {
    return tx_b;
  }
  uint32_t page = ((uint32_t)m->last_page_h << 15) | ((uint32_t)m->last_page_l << 7);
  uint32_t full = page | (m->cur_tx[0] & 0x7Fu);
  if (idx >= 2u) {
    static const uint8_t kChipId[6] = {0x0Au, 0x00u, 0x00u, 0x72u, 0x66u, 0x03u};
    uint32_t want = m->nvt_alt_addr ? SM6125_NVT_CHIP_VER_TRIM_ADDR : SM6125_NVT_CHIP_VER_TRIM_OLD_ADDR;
    if (full == want && (idx - 2u) < 6u) {
      return kChipId[idx - 2u];
    }
  }
  return 0xFFu;
}

static void finish_cmd(struct sim_se *m) {
  if (!m->active) {
    return;
  }
  size_t len = (m->cmd_op == GENI_M_CMD_TX_ONLY) ? m->reg[SE_SPI_TX_TRANS_LEN / 4]
                                                  : m->reg[SE_SPI_RX_TRANS_LEN / 4];
  for (size_t i = 0; i < len; ++i) {
    uint32_t w = m->tx_fifo[i / 4u];
    m->cur_tx[i] = (uint8_t)((w >> (8u * (i % 4u))) & 0xFFu);
  }
  m->cur_len = len;
  if (!(m->reg[SE_SPI_LOOPBACK / 4] & GENI_LOOPBACK_ENABLE) && m->cmd_op == GENI_M_CMD_TX_ONLY) {
    if (len == 3u && m->cur_tx[0] == 0xFFu) {
      m->last_page_h = m->cur_tx[1];
      m->last_page_l = m->cur_tx[2];
    } else if (len == 2u) {
      uint32_t page = ((uint32_t)m->last_page_h << 15) | ((uint32_t)m->last_page_l << 7);
      uint32_t full = page | (m->cur_tx[0] & 0x7Fu);
      if (full == SM6125_NVT_ENG_RST_ADDR && m->cur_tx[1] == 0x5Au && m->reset_seq == 0u) {
        m->reset_seq = 1u;
      } else if (full == SM6125_NVT_SWRST_N8_ADDR && m->cur_tx[1] == 0x69u && m->reset_seq >= 1u) {
        m->reset_seq = 2u;
      } else if (full == SM6125_NVT_SPI_RD_FAST_ADDR && m->cur_tx[1] == 0x00u && m->reset_seq >= 2u) {
        m->fast_read_disabled = 1;
      }
    }
  }
  if (m->cmd_op == GENI_M_CMD_TX_RX || m->cmd_op == GENI_M_CMD_RX_ONLY) {
    m->rx_head = m->rx_tail = 0;
    uint32_t w = 0;
    for (size_t i = 0; i < len; ++i) {
      uint8_t rb = nvt_slave_byte(m, i, m->cur_tx[i]);
      w |= (uint32_t)rb << (8u * (i % 4u));
      if ((i % 4u) == 3u || i + 1u == len) {
        m->rx_fifo[m->rx_tail++] = w;
        w = 0;
      }
    }
    m->irq |= GENI_M_RX_FIFO_LAST;
  }
  m->irq |= GENI_M_CMD_DONE;
  m->active = 0;
}

static uint32_t sim_rd(void *ctx, uint32_t r) {
  struct sim_se *m = (struct sim_se *)ctx;
  switch (r) {
  case GENI_FW_REVISION_RO: return m->fw_rev;
  case GENI_IF_DISABLE_RO:  return m->if_disable;
  case SE_HW_PARAM_0:       return m->hw_param0;
  case SE_GENI_M_IRQ_STATUS:
    finish_cmd(m);
    return m->irq;
  case SE_GENI_RX_FIFO_STATUS: {
    unsigned wc = m->rx_tail - m->rx_head;
    size_t len = m->reg[SE_SPI_RX_TRANS_LEN / 4];
    unsigned last_valid = (len % 4u);
    return wc | ((m->irq & GENI_M_RX_FIFO_LAST) && wc <= 1u ? GENI_RX_LAST | (last_valid << GENI_RX_LAST_VALID_SHIFT) : 0u);
  }
  case SE_GENI_RX_FIFOn:
    return (m->rx_head < m->rx_tail) ? m->rx_fifo[m->rx_head++] : 0u;
  default:
    return m->reg[r / 4];
  }
}

static void sim_wr(void *ctx, uint32_t r, uint32_t v) {
  struct sim_se *m = (struct sim_se *)ctx;
  switch (r) {
  case SE_GENI_M_IRQ_CLEAR: m->irq &= ~v; return;
  case SE_GENI_TX_FIFOn:    m->tx_fifo[m->tx_cnt++] = v; return;
  case SE_GENI_M_CMD0:
    m->cmd_op = v >> GENI_M_OPCODE_SHIFT;
    m->tx_cnt = 0;
    m->active = 1;
    return;
  default:
    m->reg[r / 4] = v;
    return;
  }
}

static uint32_t sim_wrap_rd(void *ctx, uint32_t r) {
  struct sim_se *m = (struct sim_se *)ctx;
  CHECK(r == QUPV3_HW_VER_REG);
  return m->wrap_ver;
}

static void sim_wrap_wr(void *ctx, uint32_t r, uint32_t v) {
  (void)ctx; (void)r; (void)v;
  CHECK(0);
}

static int bus_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
  return geni_spi_transfer((struct geni_spi *)ctx, tx, rx, len, 0);
}

static void bus_delay(void *ctx, unsigned ms) {
  (void)ctx; (void)ms;
}

static void test_board_constants_and_clocks(void) {
  CHECK(SM6125_QUP0_WRAPPER_BASE == 0x04ac0000ULL);
  CHECK(SM6125_QUP0_SE2_BASE == 0x04a88000ULL);
  CHECK(SM6125_GCC_BASE == 0x01400000ULL);
  CHECK(SM6125_GCC_QUP0_VOTE == 0x7900cu);
  CHECK(SM6125_VOTE_QUP0_SE2_ALL == 0x000013c0u);
  CHECK(SM6125_S2_RCG == 0x1f3a8u);
  CHECK(SM6125_TLMM_WEST_BASE == 0x00500000ULL);
  CHECK(sm6125_tlmm_ctl(6u) == 0x6000u);
  CHECK(sm6125_tlmm_ctl(87u) == 0x57000u);
  CHECK(sm6125_tlmm_io(88u) == 0x58004u);
  CHECK(sm6125_tlmm_pin_cfg(SM6125_FUNC_QUP02, SM6125_DRIVE_6MA, SM6125_PULL_NONE, 0u) == 0x00000084u);

  CHECK(sm6125_cbcr_running(0x00000001u, 0) == 1);
  CHECK(sm6125_cbcr_running(0x80000000u, 0) == 0);
  CHECK(sm6125_cbcr_running(0x80000002u, 1) == 1);
  CHECK(sm6125_cbcr_running(0x80000002u, 0) == 0);

  /* TCXO 19.2 MHz (src=0, div=1, mode=0, hid=0) */
  CHECK(sm6125_dfs_level_hz(0x00000000u, 0u, 0u) == 19200000ULL);
  /* GPLL0_AUX2 300 MHz * 8 / 75 = 32 MHz (src=2, mode=2, m=8, n_reg=~67 & 0xffff = 0xffbc) */
  CHECK(sm6125_dfs_level_hz((2u << 12) | (2u << 8) | 1u, 8u, 0xffbcu) == 32000000ULL);
  /* GPLL0_AUX2 300 MHz / 4 = 75 MHz (src=2, mode=0, hid=7 -> 2/(7+1) = 1/4) */
  CHECK(sm6125_dfs_level_hz((2u << 8) | 7u, 0u, 0u) == 75000000ULL);
  /* GPLL0_EARLY 600 MHz / 6 = 100 MHz (src=1, mode=0, hid=11) */
  CHECK(sm6125_dfs_level_hz((1u << 8) | 11u, 0u, 0u) == 100000000ULL);
  /* GPLL6_MAIN 384 MHz / 3 = 128 MHz (src=4, mode=0, hid=5) */
  CHECK(sm6125_dfs_level_hz((4u << 8) | 5u, 0u, 0u) == 128000000ULL);

  uint64_t lvls[8] = {19200000ULL, 32000000ULL, 48000000ULL, 64000000ULL, 96000000ULL, 100000000ULL, 128000000ULL, 0ULL};
  struct sm6125_clock_choice ch = {};
  CHECK(sm6125_choose_clock(lvls, 8u, 1u, 1000000u, &ch) == 0);
  CHECK(ch.actual_hz == 1000000ULL);
  CHECK(ch.level == 1u && ch.div == 32u); /* menor div que atinge exato 1.0 MHz: 32M/32 < 48M/48 */
  CHECK(sm6125_choose_clock(lvls, 8u, 1u, 8000000u, &ch) == 0);
  CHECK(ch.actual_hz == 8000000ULL);
  CHECK(sm6125_choose_clock(lvls, 1u, 2u, 1000000u, &ch) == 0);
  CHECK(ch.level == 0u && ch.div == 10u && ch.actual_hz == 960000ULL);
}

static void test_geni_and_nvt_identify(void) {
  struct sim_se m;
  struct geni_spi s;
  memset(&m, 0, sizeof(m));
  memset(&s, 0, sizeof(s));
  m.wrap_ver = 0x20010000u;
  m.hw_param0 = (32u << 24) | (16u << 16);
  m.fw_rev = (GENI_PROTO_SPI << GENI_FW_PROTO_SHIFT) | 0x20u;
  m.if_disable = GENI_FIFO_IF_DISABLE;

  s.wrapper = (struct geni_io){&m, sim_wrap_rd, sim_wrap_wr, 0};
  s.se = (struct geni_io){&m, sim_rd, sim_wr, 0};
  s.poll_only = 1;
  s.poll_limit = 1000;

  CHECK(geni_spi_read_info(&s) == 0);
  CHECK(s.info.protocol == GENI_PROTO_SPI && s.info.fifo_disabled == 1);
  CHECK(geni_spi_init(&s) == GENI_ERR_NOTSUP);

  m.if_disable = 0;
  CHECK(geni_spi_init(&s) == 0);
  CHECK(s.oversampling == 1 && m.reg[SE_IRQ_EN / 4] == 0);
  CHECK(geni_spi_set_clock(&s, 1000000u, 19200000u, 0u) == 0);

  uint8_t pat[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  uint8_t lrx[8] = {};
  CHECK(geni_spi_setup(&s, GENI_SPI_LOOP, 0u) == 0);
  CHECK(geni_spi_transfer(&s, pat, lrx, sizeof(pat), 0) == 0);
  CHECK(memcmp(pat, lrx, sizeof(pat)) == 0);

  CHECK(geni_spi_setup(&s, 0u, 0u) == 0);
  sm6125_nvt_bus_t bus = {&s, bus_xfer, bus_delay};
  sm6125_nvt_ident_result_t res;
  CHECK(sm6125_nvt_identify(&bus, &res) == 0);
  CHECK(res.matched == 1 && res.attempts == 1u);
  CHECK(res.addr_used == SM6125_NVT_CHIP_VER_TRIM_OLD_ADDR);
  CHECK(strcmp(res.chip_name, "NT36672A") == 0);
  CHECK(m.reset_seq == 2u && m.fast_read_disabled == 1);

  memset(&m, 0, sizeof(m));
  m.wrap_ver = 0x20010000u;
  m.hw_param0 = (32u << 24) | (16u << 16);
  m.fw_rev = (GENI_PROTO_SPI << GENI_FW_PROTO_SHIFT);
  m.nvt_alt_addr = 1;
  memset(&s, 0, sizeof(s));
  s.wrapper = (struct geni_io){&m, sim_wrap_rd, sim_wrap_wr, 0};
  s.se = (struct geni_io){&m, sim_rd, sim_wr, 0};
  s.poll_only = 1;
  s.poll_limit = 1000;
  CHECK(geni_spi_init(&s) == 0);
  CHECK(geni_spi_set_clock(&s, 1000000u, 19200000u, 0u) == 0);
  CHECK(sm6125_nvt_identify(&bus, &res) == 0);
  CHECK(res.addr_used == SM6125_NVT_CHIP_VER_TRIM_ADDR);
}

int main(void) {
  test_board_constants_and_clocks();
  test_geni_and_nvt_identify();
  printf("PASS: Sm6125TouchTests (board/GCC/TLMM/GENI-SPI/NT36672A)\n");
  return 0;
}
