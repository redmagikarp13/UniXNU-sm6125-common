// SPDX-License-Identifier: MIT
// Teste unitario em host para Sm6125TouchBoard.h, GeniSpi.c, Sm6125GeniFifo.c e Sm6125NvtProbe.c.
// Modela hardware QUPv3 GENI FIFO (profundidade 16 palavras, watermarks latched-level e TX_FIFO_WR_ERR).
#include "../Sm6125TouchBoard.h"
#include "../Sm6125GeniFifo.h"
#include "../Sm6125NvtProbe.h"
#include "../Sm6125Buttons.h"
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

#define SIM_FIFO_DEPTH 16u

struct sim_se {
  uint32_t reg[0x1000 / 4];
  uint32_t wrap_ver;
  uint32_t hw_param0;
  uint32_t fw_rev;
  uint32_t if_disable;

  /* Hardware FIFO TX: profundidade real = 16 palavras de 32 bits */
  uint32_t tx_fifo[SIM_FIFO_DEPTH];
  unsigned tx_cnt;
  unsigned max_tx_cnt;
  unsigned tx_head_byte;
  unsigned tx_wr_err_count;
  size_t tx_received;

  /* Hardware FIFO RX */
  uint32_t rx_fifo[2048];
  unsigned rx_head;
  unsigned rx_tail;
  uint32_t rx_cur_word;
  unsigned rx_cur_bytes;
  unsigned rx_last_valid;

  uint8_t last_page_h;
  uint8_t last_page_l;
  uint32_t reset_seq;
  int fast_read_disabled;
  int nvt_alt_addr;

  uint32_t sram_writes;
  uint32_t sram_bytes;
  uint32_t xfer_calls;
  uint8_t crc_reg[0x400];
  uint8_t ev_reg[0x100];

  uint8_t cur_tx[SM6125_NVT_CHUNK_LEN + 8u];
  size_t tx_total;
  size_t rx_total;
  size_t rx_produced;
  uint32_t cmd_op;
  int active;
  int tx_ops;
  int rx_ops;
  int inject_error;
  uint32_t irq;
  uint32_t tx_wm_reassert_count;
};

static uint8_t nvt_slave_byte(struct sim_se *m, size_t idx, uint8_t tx_b) {
  if (m->reg[SE_SPI_LOOPBACK / 4] & GENI_LOOPBACK_ENABLE) {
    return tx_b;
  }
  uint32_t page = ((uint32_t)m->last_page_h << 15) | ((uint32_t)m->last_page_l << 7);
  uint32_t full = page | (m->cur_tx[0] & 0x7Fu);
  if (idx >= 2u) {
    size_t off = idx - 2u;
    static const uint8_t kChipId[6] = {0x0Au, 0x00u, 0x00u, 0x72u, 0x66u, 0x03u};
    uint32_t want = m->nvt_alt_addr ? SM6125_NVT_CHIP_VER_TRIM_ADDR : SM6125_NVT_CHIP_VER_TRIM_OLD_ADDR;
    if (full == want && off < 6u) {
      return kChipId[off];
    }
    if (full + off >= 0x3F000u && full + off < 0x3F400u) {
      return m->crc_reg[(full + off) - 0x3F000u];
    }
    if (full + off >= SM6125_NVT_EVENT_BUF_ADDR && full + off < SM6125_NVT_EVENT_BUF_ADDR + 0x100u) {
      return m->ev_reg[(full + off) - SM6125_NVT_EVENT_BUF_ADDR];
    }
  }
  return 0xFFu;
}

static void refresh_level_irqs(struct sim_se *m) {
  if (!m->active) {
    return;
  }
  uint32_t rx_wm = m->reg[SE_GENI_RX_WATERMARK_REG / 4];
  unsigned rx_wc = m->rx_tail - m->rx_head;
  if (rx_wm && rx_wc >= rx_wm) {
    m->irq |= GENI_M_RX_FIFO_WATERMARK;
  }
  uint32_t tx_wm = m->reg[SE_GENI_TX_WATERMARK_REG / 4];
  if (m->tx_ops && tx_wm && m->tx_cnt <= tx_wm && m->tx_received < m->tx_total) {
    m->irq |= GENI_M_TX_FIFO_WATERMARK;
  }
}

static void commit_tx_cmd(struct sim_se *m) {
  size_t len = m->tx_total;
  if ((m->reg[SE_SPI_LOOPBACK / 4] & GENI_LOOPBACK_ENABLE) || m->cmd_op != GENI_M_CMD_TX_ONLY) {
    return;
  }
  if (len == 3u && m->cur_tx[0] == 0xFFu) {
    m->last_page_h = m->cur_tx[1];
    m->last_page_l = m->cur_tx[2];
    return;
  }
  if (len < 2u) {
    return;
  }
  uint32_t page = ((uint32_t)m->last_page_h << 15) | ((uint32_t)m->last_page_l << 7);
  uint32_t full = page | (m->cur_tx[0] & 0x7Fu);
  size_t pay = len - 1u;
  if (full == SM6125_NVT_ENG_RST_ADDR && m->cur_tx[1] == 0x5Au && m->reset_seq == 0u) {
    m->reset_seq = 1u;
  } else if (full == SM6125_NVT_SWRST_N8_ADDR && m->cur_tx[1] == 0x69u && m->reset_seq >= 1u) {
    m->reset_seq = 2u;
  } else if (full == SM6125_NVT_SPI_RD_FAST_ADDR && m->cur_tx[1] == 0x00u && m->reset_seq >= 2u) {
    m->fast_read_disabled = 1;
  } else if (full >= 0x3F000u && full + pay <= 0x3F400u) {
    memcpy(m->crc_reg + (full - 0x3F000u), m->cur_tx + 1u, pay);
    if (full == SM6125_NVT_BOOT_RDY_ADDR && m->cur_tx[1] == 0x01u) {
      m->crc_reg[SM6125_NVT_BLD_ILM_DLM_CRC_ADDR - 0x3F000u] = 0x04u;
      memcpy(m->crc_reg + (SM6125_NVT_R_ILM_CHECKSUM_ADDR - 0x3F000u),
             m->crc_reg + (SM6125_NVT_G_ILM_CHECKSUM_ADDR - 0x3F000u), 4u);
      memcpy(m->crc_reg + (SM6125_NVT_R_DLM_CHECKSUM_ADDR - 0x3F000u),
             m->crc_reg + (SM6125_NVT_G_DLM_CHECKSUM_ADDR - 0x3F000u), 4u);
      m->ev_reg[SM6125_NVT_EVENT_RESET_COMPLETE] = 0xA0u;
      m->ev_reg[SM6125_NVT_EVENT_FWINFO + 0] = 0x19u;
      m->ev_reg[SM6125_NVT_EVENT_FWINFO + 1] = 0xE6u;
      m->ev_reg[SM6125_NVT_EVENT_FWINFO + 2] = 18u;
      m->ev_reg[SM6125_NVT_EVENT_FWINFO + 3] = 36u;
      m->ev_reg[SM6125_NVT_EVENT_FWINFO + 4] = 0x04u;
      m->ev_reg[SM6125_NVT_EVENT_FWINFO + 5] = 0x38u; /* 1080 */
      m->ev_reg[SM6125_NVT_EVENT_FWINFO + 6] = 0x09u;
      m->ev_reg[SM6125_NVT_EVENT_FWINFO + 7] = 0x24u; /* 2340 */
      m->ev_reg[SM6125_NVT_EVENT_PROJECTID + 0] = 0x23u;
      m->ev_reg[SM6125_NVT_EVENT_PROJECTID + 1] = 0x59u; /* 0x5923 */
    }
  } else if (full >= SM6125_NVT_EVENT_BUF_ADDR && full + pay <= SM6125_NVT_EVENT_BUF_ADDR + 0x100u) {
    memcpy(m->ev_reg + (full - SM6125_NVT_EVENT_BUF_ADDR), m->cur_tx + 1u, pay);
  } else if (full <= 0x30000u) {
    m->sram_writes++;
    m->sram_bytes += (uint32_t)pay;
  }
}

static void step_cmd(struct sim_se *m) {
  if (!m->active) {
    return;
  }
  if (m->inject_error) {
    m->irq |= GENI_M_CMD_OVERRUN;
    return;
  }

  /* Desloca no maximo 8 bytes por tick para exercitar multiplos ciclos de TX_WATERMARK e RX_WATERMARK */
  for (unsigned k = 0; k < 8u; ++k) {
    size_t pos = m->tx_ops ? m->tx_received : m->rx_produced;
    uint8_t in_b = 0u;
    if (m->tx_ops) {
      if (m->tx_received >= m->tx_total || m->tx_cnt == 0u) {
        break;
      }
      in_b = (uint8_t)((m->tx_fifo[0] >> (8u * m->tx_head_byte)) & 0xFFu);
      m->cur_tx[m->tx_received++] = in_b;
      m->tx_head_byte++;
      if (m->tx_head_byte == 4u || m->tx_received == m->tx_total) {
        for (unsigned i = 1u; i < m->tx_cnt; ++i) {
          m->tx_fifo[i - 1u] = m->tx_fifo[i];
        }
        m->tx_cnt--;
        m->tx_head_byte = 0u;
      }
    } else if (m->rx_produced >= m->rx_total) {
      break;
    }

    if (m->rx_ops && m->rx_produced < m->rx_total) {
      uint8_t out_b = nvt_slave_byte(m, pos, in_b);
      m->rx_cur_word |= (uint32_t)out_b << (8u * m->rx_cur_bytes);
      m->rx_cur_bytes++;
      m->rx_produced++;
      if (m->rx_cur_bytes == 4u) {
        m->rx_fifo[m->rx_tail++] = m->rx_cur_word;
        m->rx_cur_word = 0u;
        m->rx_cur_bytes = 0u;
      } else if (m->rx_produced == m->rx_total) {
        m->rx_last_valid = m->rx_cur_bytes;
        m->rx_fifo[m->rx_tail++] = m->rx_cur_word;
        m->rx_cur_word = 0u;
        m->rx_cur_bytes = 0u;
      }
      if (m->rx_produced == m->rx_total) {
        m->irq |= GENI_M_RX_FIFO_LAST;
      }
    }
  }

  refresh_level_irqs(m);

  int tx_done = !m->tx_ops || (m->tx_received == m->tx_total);
  int rx_done = !m->rx_ops || (m->rx_produced == m->rx_total);
  if (tx_done && rx_done) {
    commit_tx_cmd(m);
    m->irq |= GENI_M_CMD_DONE;
    m->active = 0;
  }
}

static uint32_t sim_rd(void *ctx, uint32_t r) {
  struct sim_se *m = (struct sim_se *)ctx;
  switch (r) {
  case GENI_FW_REVISION_RO:    return m->fw_rev;
  case GENI_IF_DISABLE_RO:     return m->if_disable;
  case SE_HW_PARAM_0:          return m->hw_param0;
  case SE_GENI_STATUS:         return m->active ? GENI_STATUS_M_ACTIVE : 0u;
  case SE_GENI_TX_FIFO_STATUS: return m->tx_cnt;
  case SE_GENI_M_IRQ_STATUS:
    step_cmd(m);
    return m->irq;
  case SE_GENI_RX_FIFO_STATUS: {
    unsigned wc = m->rx_tail - m->rx_head;
    return wc | ((m->irq & GENI_M_RX_FIFO_LAST) && wc <= 1u
                     ? (GENI_RX_LAST | (m->rx_last_valid << GENI_RX_LAST_VALID_SHIFT))
                     : 0u);
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
  case SE_GENI_M_IRQ_CLEAR: {
    uint32_t before = m->irq;
    m->irq &= ~v;
    /* Hardware latched-level: se TX/RX watermark nao foi tratado antes do clear, re-latch imediato! */
    refresh_level_irqs(m);
    if ((before & v & GENI_M_TX_FIFO_WATERMARK) && (m->irq & GENI_M_TX_FIFO_WATERMARK)) {
      m->tx_wm_reassert_count++;
    }
    return;
  }
  case SE_GENI_TX_FIFOn:
    if (m->tx_cnt >= SIM_FIFO_DEPTH) {
      m->irq |= GENI_M_TX_FIFO_WR_ERR;
      m->tx_wr_err_count++;
      return;
    }
    m->tx_fifo[m->tx_cnt++] = v;
    if (m->tx_cnt > m->max_tx_cnt) {
      m->max_tx_cnt = m->tx_cnt;
    }
    return;
  case SE_GENI_M_CMD0: {
    uint32_t op = v >> GENI_M_OPCODE_SHIFT;
    CHECK(op == GENI_M_CMD_TX_ONLY || op == GENI_M_CMD_RX_ONLY || op == GENI_M_CMD_TX_RX);
    m->cmd_op = op;
    m->tx_ops = (op == GENI_M_CMD_TX_ONLY || op == GENI_M_CMD_TX_RX);
    m->rx_ops = (op == GENI_M_CMD_RX_ONLY || op == GENI_M_CMD_TX_RX);
    m->tx_total = m->tx_ops ? m->reg[SE_SPI_TX_TRANS_LEN / 4] : 0u;
    m->rx_total = m->rx_ops ? m->reg[SE_SPI_RX_TRANS_LEN / 4] : 0u;
    CHECK(m->tx_total <= sizeof(m->cur_tx));
    m->tx_cnt = 0u;
    m->tx_head_byte = 0u;
    m->tx_received = 0u;
    m->rx_head = 0u;
    m->rx_tail = 0u;
    m->rx_cur_word = 0u;
    m->rx_cur_bytes = 0u;
    m->rx_last_valid = 0u;
    m->rx_produced = 0u;
    m->active = 1;
    m->xfer_calls++;
    return;
  }
  case SE_GENI_M_CMD_CTRL_REG:
    if (v & GENI_M_CMD_CANCEL) {
      m->irq |= GENI_M_CMD_CANCEL_DONE;
      m->active = 0;
    }
    if (v & GENI_M_CMD_ABORT) {
      m->irq |= GENI_M_CMD_ABORT_DONE;
      m->active = 0;
    }
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

static void init_sim(struct sim_se *m, struct geni_spi *s, int alt_addr) {
  memset(m, 0, sizeof(*m));
  memset(s, 0, sizeof(*s));
  m->wrap_ver = 0x20010000u;
  m->hw_param0 = (32u << 24) | (SIM_FIFO_DEPTH << 16);
  m->fw_rev = (GENI_PROTO_SPI << GENI_FW_PROTO_SHIFT) | 0x20u;
  m->nvt_alt_addr = alt_addr;
  s->wrapper = (struct geni_io){m, sim_wrap_rd, sim_wrap_wr, 0};
  s->se = (struct geni_io){m, sim_rd, sim_wr, 0};
  s->poll_only = 1;
  s->poll_limit = 50000u;
}

static int bus_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
  return sm6125_geni_fifo_transfer((struct geni_spi *)ctx, tx, rx, len, 0);
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

  CHECK(sm6125_dfs_level_hz(0x00000000u, 0u, 0u) == 19200000ULL);
  CHECK(sm6125_dfs_level_hz((2u << 12) | (2u << 8) | 1u, 8u, 0xffbcu) == 32000000ULL);
  CHECK(sm6125_dfs_level_hz((2u << 8) | 7u, 0u, 0u) == 75000000ULL);
  CHECK(sm6125_dfs_level_hz((1u << 8) | 11u, 0u, 0u) == 100000000ULL);
  CHECK(sm6125_dfs_level_hz((4u << 8) | 5u, 0u, 0u) == 128000000ULL);

  uint64_t lvls[8] = {19200000ULL, 32000000ULL, 48000000ULL, 64000000ULL, 96000000ULL, 100000000ULL, 128000000ULL, 0ULL};
  struct sm6125_clock_choice ch = {};
  CHECK(sm6125_choose_clock(lvls, 8u, 1u, 1000000u, &ch) == 0);
  CHECK(ch.actual_hz == 1000000ULL && ch.level == 1u && ch.div == 32u);
  CHECK(sm6125_choose_clock(lvls, 8u, 1u, 8000000u, &ch) == 0);
  CHECK(ch.actual_hz == 8000000ULL && ch.level == 1u && ch.div == 4u);
  CHECK(sm6125_choose_clock(lvls, 1u, 2u, 1000000u, &ch) == 0);
  CHECK(ch.level == 0u && ch.div == 10u && ch.actual_hz == 960000ULL);
}

static void add_pkt_csum(uint8_t pkt[SM6125_NVT_POINT_DATA_LEN]) {
  uint8_t sum = 0;
  for (unsigned i = 0; i < SM6125_NVT_POINT_DATA_LEN - 1u; ++i) {
    sum = (uint8_t)(sum + pkt[i]);
  }
  pkt[SM6125_NVT_POINT_DATA_LEN - 1u] = (uint8_t)((uint8_t)(~sum) + 1u);
}

static void test_genispi_latched_watermark_regression_and_fifo_fix(void) {
  struct sim_se m;
  struct geni_spi s;
  init_sim(&m, &s, 0);
  CHECK(geni_spi_init(&s) == 0);
  CHECK(geni_spi_set_clock(&s, 8000000u, 32000000u, 1u) == 0);
  CHECK(geni_spi_setup(&s, 0u, 0u) == 0);

  uint8_t big_tx[256];
  uint8_t big_rx[256];
  for (unsigned i = 0; i < 256u; ++i) {
    big_tx[i] = (uint8_t)(i ^ 0xA5u);
  }

  /* Prova de regressao: GeniSpi.c limpa IRQ antes de handle_tx(), re-latcha TX_WM e causa TX_FIFO_WR_ERR (-5). */
  int buggy_rc = geni_spi_transfer(&s, big_tx, NULL, sizeof(big_tx), 0);
  CHECK(buggy_rc == GENI_ERR_IO);
  CHECK(m.tx_wr_err_count > 0u);
  CHECK(m.tx_wm_reassert_count > 0u);

  /* O novo sm6125_geni_fifo_transfer() drena/reabastece antes de limpar IRQs e checa TX_FIFO_STATUS. */
  init_sim(&m, &s, 0);
  CHECK(geni_spi_init(&s) == 0);
  CHECK(geni_spi_set_clock(&s, 8000000u, 32000000u, 1u) == 0);
  CHECK(geni_spi_setup(&s, GENI_SPI_LOOP, 0u) == 0);

  memset(big_rx, 0, sizeof(big_rx));
  CHECK(sm6125_geni_fifo_transfer(&s, big_tx, big_rx, sizeof(big_tx), 0) == 0);
  CHECK(memcmp(big_tx, big_rx, sizeof(big_tx)) == 0);
  CHECK(m.max_tx_cnt <= SIM_FIFO_DEPTH);
  CHECK(m.tx_wr_err_count == 0u);
  CHECK(m.tx_wm_reassert_count == 0u);

  /* Erro injetado preserva snap_irq ANTES do clear */
  m.inject_error = 1;
  CHECK(sm6125_geni_fifo_transfer(&s, big_tx, NULL, 64u, 0) == GENI_ERR_IO);
  CHECK(s.snap_valid == 1);
  CHECK((s.snap_irq & GENI_M_CMD_OVERRUN) != 0u);
  m.inject_error = 0;
}

static void test_geni_and_nvt_protocol(const char *fw_path) {
  struct sim_se m;
  struct geni_spi s;
  init_sim(&m, &s, 0);
  m.if_disable = GENI_FIFO_IF_DISABLE;

  CHECK(geni_spi_read_info(&s) == 0);
  CHECK(s.info.protocol == GENI_PROTO_SPI && s.info.fifo_disabled == 1);
  CHECK(geni_spi_init(&s) == GENI_ERR_NOTSUP);

  m.if_disable = 0;
  CHECK(geni_spi_init(&s) == 0);
  CHECK(s.oversampling == 1 && m.reg[SE_IRQ_EN / 4] == 0);
  CHECK(geni_spi_set_clock(&s, 8000000u, 32000000u, 1u) == 0);

  uint8_t pat[128], lrx[128];
  for (unsigned i = 0; i < 128u; ++i) {
    pat[i] = (uint8_t)(0x11u + i * 7u);
  }
  memset(lrx, 0, sizeof(lrx));
  CHECK(geni_spi_setup(&s, GENI_SPI_LOOP, 0u) == 0);
  CHECK(sm6125_geni_fifo_transfer(&s, pat, lrx, sizeof(pat), 0) == 0);
  CHECK(memcmp(pat, lrx, sizeof(pat)) == 0);
  CHECK(m.max_tx_cnt <= SIM_FIFO_DEPTH && m.tx_wr_err_count == 0u);

  CHECK(geni_spi_setup(&s, 0u, 0u) == 0);
  sm6125_nvt_bus_t bus = {&s, bus_xfer, bus_delay};
  sm6125_nvt_ident_result_t res;
  CHECK(sm6125_nvt_identify(&bus, &res) == 0);
  CHECK(res.matched == 1 && res.attempts == 1u);
  CHECK(res.addr_used == SM6125_NVT_CHIP_VER_TRIM_OLD_ADDR);
  CHECK(strcmp(res.chip_name, "NT36672A") == 0);
  CHECK(m.reset_seq == 2u && m.fast_read_disabled == 1);
  CHECK(m.max_tx_cnt <= SIM_FIFO_DEPTH && m.tx_wr_err_count == 0u);

  /* Fallback em 0x3F004 */
  init_sim(&m, &s, 1);
  CHECK(geni_spi_init(&s) == 0);
  CHECK(geni_spi_set_clock(&s, 8000000u, 32000000u, 1u) == 0);
  CHECK(sm6125_nvt_identify(&bus, &res) == 0);
  CHECK(res.addr_used == SM6125_NVT_CHIP_VER_TRIM_ADDR);

  /* Parse de firmware real (ou sintetico) e host-download completo em chunks de 4097 bytes */
  static uint8_t scratch[SM6125_NVT_CHUNK_LEN + 8u];
  sm6125_nvt_fw_t fw;
  uint8_t *fw_buf = NULL;
  size_t fw_sz = 0;
  if (fw_path && fw_path[0]) {
    FILE *fp = fopen(fw_path, "rb");
    CHECK(fp != NULL);
    fseek(fp, 0, SEEK_END);
    fw_sz = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    fw_buf = (uint8_t *)malloc(fw_sz);
    CHECK(fw_buf && fread(fw_buf, 1, fw_sz, fp) == fw_sz);
    fclose(fp);
    CHECK(sm6125_nvt_fw_parse(fw_buf, fw_sz, &fw) == 0);
    CHECK(fw.count == 15u);
    CHECK(fw.part[0].sram_addr == 0u && fw.part[0].size == 0xe85fu && fw.part[0].crc == 0x1c49d636u);
    CHECK(fw.part[1].sram_addr == 0x27700u && fw.part[1].size == 0x317u && fw.part[1].crc == 0xbc8958c2u);
    CHECK(fw.part[fw.count - 1u].is_header == 1u);
  } else {
    fw_sz = 512u;
    fw_buf = (uint8_t *)calloc(1, fw_sz);
    fw_buf[0] = 0x30u; /* end */
    fw_buf[4] = 0x00u; /* ILM sram=0 */
    fw_buf[8] = 0x80u; /* ILM size=128 */
    fw_buf[12] = 0x00u; fw_buf[13] = 0x01u; /* DLM bin=0x100 */
    fw_buf[16] = 0x00u; fw_buf[17] = 0x70u; fw_buf[18] = 0x02u; /* DLM sram=0x27000 */
    fw_buf[20] = 0x40u; /* DLM size=64 */
    fw_buf[0x18] = 0x78u; fw_buf[0x19] = 0x56u; fw_buf[0x1A] = 0x34u; fw_buf[0x1B] = 0x12u;
    fw_buf[0x1C] = 0xEFu; fw_buf[0x1D] = 0xCDu; fw_buf[0x1E] = 0xABu; fw_buf[0x1F] = 0x89u;
    CHECK(sm6125_nvt_fw_parse(fw_buf, fw_sz, &fw) == 0);
  }

  m.crc_reg[SM6125_NVT_BLD_CRC_EN_ADDR - 0x3F000u] = 0x05u;
  sm6125_nvt_dl_diag_t dl;
  CHECK(sm6125_nvt_download_firmware(&bus, &fw, scratch, &dl) == 0);
  CHECK(dl.ok == 1 && dl.attempt == 1u && dl.reset_state[0] == 0xA0u);
  CHECK((m.crc_reg[SM6125_NVT_BLD_CRC_EN_ADDR - 0x3F000u] & 0x85u) == 0x85u);
  CHECK(m.ev_reg[SM6125_NVT_EVENT_HOST_CMD] == 0xAEu);
  CHECK(m.sram_writes > 0u && m.sram_bytes > 0u);
  CHECK(dl.r_ilm_crc == fw.part[0].crc && dl.r_dlm_crc == fw.part[1].crc);
  CHECK(m.max_tx_cnt <= SIM_FIFO_DEPTH && m.tx_wr_err_count == 0u && m.tx_wm_reassert_count == 0u);

  sm6125_nvt_fw_info_t finfo;
  CHECK(sm6125_nvt_get_fw_info(&bus, &finfo) == 0);
  CHECK(finfo.valid == 1 && finfo.fw_ver == 0x19u && finfo.pid == 0x5923u);
  CHECK(finfo.abs_x_max == 1080u && finfo.abs_y_max == 2340u);

  /* Pacote de 2 dedos carregado no event buffer simulado + leitura SPI real de 67 bytes (65 + 2) */
  uint8_t pkt[SM6125_NVT_POINT_DATA_LEN];
  memset(pkt, 0, sizeof(pkt));
  pkt[0] = (1u << 3) | 1u;
  pkt[1] = 0x21u; pkt[2] = 0x49u; pkt[3] = 0xC2u; pkt[4] = 8u; pkt[5] = 0x40u; pkt[62] = 0x01u;
  pkt[6] = (2u << 3) | 2u;
  pkt[7] = 0x43u; pkt[8] = 0x92u; pkt[9] = 0x84u; pkt[10] = 0u; pkt[11] = 0u; pkt[63] = 0x00u;
  add_pkt_csum(pkt);
  memcpy(m.ev_reg, pkt, sizeof(pkt));

  uint8_t raw[SM6125_NVT_POINT_DATA_LEN];
  sm6125_nvt_points_t pts;
  CHECK(sm6125_nvt_read_points(&bus, 1080u, 2340u, raw, &pts) == 0);
  CHECK(pts.csum_ok == 1 && pts.recovery == 0 && pts.count == 2u);
  CHECK(pts.fingers[0].id == 1u && pts.fingers[0].x == 540u && pts.fingers[0].y == 1170u && pts.fingers[0].p == 0x140u);
  CHECK(pts.fingers[1].id == 2u && pts.fingers[1].x == 1080u && pts.fingers[1].y == 2340u && pts.fingers[1].w == 1u);
  CHECK(m.max_tx_cnt <= SIM_FIFO_DEPTH && m.tx_wr_err_count == 0u && m.tx_wm_reassert_count == 0u);

  pkt[64] ^= 1u;
  sm6125_nvt_parse_points(pkt, 1080u, 2340u, &pts);
  CHECK(pts.csum_ok == 0 && pts.count == 0u);

  memset(pkt, 0x77, sizeof(pkt));
  sm6125_nvt_parse_points(pkt, 1080u, 2340u, &pts);
  CHECK(pts.recovery == 1 && pts.count == 0u);

  free(fw_buf);
}

struct sim_spmi {
  uint32_t ver;
  uint32_t apid_map[SM6125_SPMI_MAX_APIDS];
  uint8_t pon_rt_sts;
  uint8_t gpio5_rt_sts;
  uint32_t last_cmd;
  uint32_t obs_status;
  uint32_t obs_rdata0;
};

static uint32_t sim_spmi_rd32(void *ctx, uintptr_t base, uint32_t off) {
  struct sim_spmi *sp = (struct sim_spmi *)ctx;
  if (base == 0x1000u) {
    if (off == SM6125_SPMI_ARB_VERSION_REG) return sp->ver;
    if (off >= SM6125_SPMI_APID_MAP_V5(0) && off < SM6125_SPMI_APID_MAP_V5(SM6125_SPMI_MAX_APIDS)) {
      return sp->apid_map[(off - SM6125_SPMI_APID_MAP_V5(0)) / 4u];
    }
    return 0u;
  }
  uint32_t ch_off = off & 0x7Fu;
  if (ch_off == SM6125_SPMI_STATUS_REG) return sp->obs_status;
  if (ch_off == SM6125_SPMI_RDATA0_REG) return sp->obs_rdata0;
  return 0u;
}

static void sim_spmi_wr32(void *ctx, uintptr_t base, uint32_t off, uint32_t val) {
  struct sim_spmi *sp = (struct sim_spmi *)ctx;
  (void)base;
  if ((off & 0x7Fu) == SM6125_SPMI_CMD_REG) {
    sp->last_cmd = val;
    uint16_t addr = (uint16_t)((val >> 4) & 0xFFFFu);
    sp->obs_status = SM6125_SPMI_STATUS_DONE;
    if (addr == SM6125_PMIC_PON_RT_STS) {
      sp->obs_rdata0 = sp->pon_rt_sts;
    } else if (addr == SM6125_PMIC_GPIO5_STATUS) {
      sp->obs_rdata0 = sp->gpio5_rt_sts;
    } else {
      sp->obs_status |= 0x02u;
    }
  }
}

static void test_spmi_buttons_v5_and_decode(void) {
  struct sim_spmi sp;
  memset(&sp, 0, sizeof(sp));
  sp.ver = SM6125_SPMI_ARB_VERSION_V5;
  sp.apid_map[12] = (0x008u << 8); /* PON SID 0, 0x800 */
  sp.apid_map[47] = (0x0C4u << 8); /* GPIO5 SID 0, 0xC400 */

  sm6125_spmi_buttons_t dev;
  memset(&dev, 0, sizeof(dev));
  dev.ctx = &sp;
  dev.rd32 = sim_spmi_rd32;
  dev.wr32 = sim_spmi_wr32;
  dev.core_va = 0x1000u;
  dev.obsrvr_va = 0x2000u;
  dev.ee = 0u;

  CHECK(sm6125_spmi_buttons_init(&dev) == 0);
  CHECK(dev.apid_pon == 12 && dev.apid_volup == 47);

  uint32_t mask = 0xFFu;
  sp.pon_rt_sts = 0u;
  sp.gpio5_rt_sts = 1u; /* active-LOW: 1 = solto */
  CHECK(sm6125_spmi_buttons_poll(&dev, &mask) == 0);
  CHECK(mask == 0u);

  sp.pon_rt_sts = SM6125_PMIC_PON_KPDPWR_BIT | SM6125_PMIC_PON_RESIN_BIT;
  sp.gpio5_rt_sts = 0u; /* active-LOW: 0 = pressionado */
  CHECK(sm6125_spmi_buttons_poll(&dev, &mask) == 0);
  CHECK(mask == (SM6125_HID_BTN_POWER | SM6125_HID_BTN_VOLDN | SM6125_HID_BTN_VOLUP));
}

int main(int argc, char **argv) {
  const char *fw_path = (argc > 1) ? argv[1] : NULL;
  test_board_constants_and_clocks();
  test_genispi_latched_watermark_regression_and_fifo_fix();
  test_geni_and_nvt_protocol(fw_path);
  test_spmi_buttons_v5_and_decode();
  printf("PASS: Sm6125TouchTests (board/GCC/TLMM/GENI-FIFO/NT36672A/SPMI-Buttons%s)\n", fw_path ? "+real-fw" : "");
  return 0;
}
