// SPDX-License-Identifier: MIT
// Loop de transferencia GENI SPI em modo FIFO PIO para SM6125 (seguindo spi-geni-qcom.c + geni-se.h).
#include "Sm6125GeniFifo.h"

static uint32_t fifo_rd(struct geni_spi *s, uint32_t reg) {
  return s->se.rd(s->se.ctx, reg);
}

static void fifo_wr(struct geni_spi *s, uint32_t reg, uint32_t v) {
  s->se.wr(s->se.ctx, reg, v);
}

static unsigned fifo_bytes_per_word(const struct geni_spi *s) {
  return s->info.fifo_width_bits / 8u;
}

static void fifo_set_word_len_8(struct geni_spi *s, int msb_first) {
  uint32_t cfg[GENI_PACK_VECTORS];
  unsigned dir = msb_first ? 1u : 0u;
  unsigned idx_start = msb_first ? 7u : 0u;
  for (unsigned i = 0; i < GENI_PACK_VECTORS; ++i) {
    unsigned idx = i * 8u + idx_start;
    cfg[i] = (idx << GENI_PACK_START_SHIFT) |
             (dir << GENI_PACK_DIR_SHIFT) |
             (7u << GENI_PACK_LEN_SHIFT);
  }
  cfg[GENI_PACK_VECTORS - 1u] |= GENI_PACK_STOP;
  uint32_t cfg0 = cfg[0] | (cfg[1] << GENI_PACK_VECTOR_SHIFT);
  uint32_t cfg1 = cfg[2] | (cfg[3] << GENI_PACK_VECTOR_SHIFT);
  fifo_wr(s, SE_GENI_TX_PACKING_CFG0, cfg0);
  fifo_wr(s, SE_GENI_TX_PACKING_CFG1, cfg1);
  fifo_wr(s, SE_GENI_RX_PACKING_CFG0, cfg0);
  fifo_wr(s, SE_GENI_RX_PACKING_CFG1, cfg1);
  fifo_wr(s, SE_GENI_BYTE_GRAN, 0u);
  fifo_wr(s, SE_SPI_WORD_LEN, (8u - GENI_WORD_LEN_MIN) & 0x3FFu);
  s->cur_bpw = 8u;
}

struct fifo_xfer {
  const uint8_t *tx;
  uint8_t *rx;
  size_t tx_rem;
  size_t rx_rem;
  size_t len;
};

static int fifo_handle_tx(struct geni_spi *s, struct fifo_xfer *x) {
  if (!x->tx || !x->tx_rem) {
    fifo_wr(s, SE_GENI_TX_WATERMARK_REG, 0u);
    return 0;
  }

  unsigned bpw = fifo_bytes_per_word(s);
  uint32_t tx_wc = fifo_rd(s, SE_GENI_TX_FIFO_STATUS) & SM6125_GENI_TX_WC_MASK;
  uint32_t depth = s->info.tx_fifo_depth;
  uint32_t avail = (tx_wc < depth) ? (depth - tx_wc) : 0u;
  uint32_t cap = (depth > s->tx_wm) ? (depth - s->tx_wm) : 1u;
  uint32_t words = (avail < cap) ? avail : cap;
  if (words == 0u) {
    return 1;
  }

  size_t max_bytes = (size_t)words * (size_t)bpw;
  if (x->tx_rem < max_bytes) {
    max_bytes = x->tx_rem;
  }

  const uint8_t *p = x->tx + (x->len - x->tx_rem);
  size_t i = 0;
  while (i < max_bytes) {
    uint32_t word = 0u;
    size_t n = (max_bytes - i < bpw) ? (max_bytes - i) : (size_t)bpw;
    for (size_t j = 0; j < n; ++j) {
      word |= (uint32_t)p[i++] << (8u * j);
    }
    fifo_wr(s, SE_GENI_TX_FIFOn, word);
  }

  x->tx_rem -= max_bytes;
  if (!x->tx_rem) {
    fifo_wr(s, SE_GENI_TX_WATERMARK_REG, 0u);
    return 0;
  }
  return 1;
}

static void fifo_handle_rx(struct geni_spi *s, struct fifo_xfer *x) {
  unsigned bpw = fifo_bytes_per_word(s);
  uint32_t rxst = fifo_rd(s, SE_GENI_RX_FIFO_STATUS);
  uint32_t words = rxst & GENI_RX_WC_MASK;
  size_t bytes = (size_t)words * (size_t)bpw;
  if (bytes == 0u) {
    return;
  }

  if (rxst & GENI_RX_LAST) {
    unsigned valid = (rxst >> GENI_RX_LAST_VALID_SHIFT) & GENI_RX_LAST_VALID_MASK;
    if (valid && valid < bpw && bytes >= (size_t)(bpw - valid)) {
      bytes -= (size_t)(bpw - valid);
    }
  }

  if (!x->rx) {
    for (uint32_t i = 0; i < words; ++i) {
      (void)fifo_rd(s, SE_GENI_RX_FIFOn);
    }
    return;
  }

  if (x->rx_rem < bytes) {
    bytes = x->rx_rem;
  }

  uint8_t *p = x->rx + (x->len - x->rx_rem);
  size_t i = 0;
  while (i < bytes) {
    uint32_t word = fifo_rd(s, SE_GENI_RX_FIFOn);
    size_t n = (bytes - i < bpw) ? (bytes - i) : (size_t)bpw;
    for (size_t j = 0; j < n; ++j) {
      p[i++] = (uint8_t)((word >> (8u * j)) & 0xFFu);
    }
  }
  x->rx_rem -= bytes;
}

static int fifo_wait_bit(struct geni_spi *s, uint32_t bit) {
  for (unsigned n = 0; n < s->poll_limit; ++n) {
    uint32_t st = fifo_rd(s, SE_GENI_M_IRQ_STATUS);
    if (st & bit) {
      fifo_wr(s, SE_GENI_M_IRQ_CLEAR, st);
      return 0;
    }
    if (s->se.delay_us) {
      s->se.delay_us(s->se.ctx, 1u);
    }
  }
  return GENI_ERR_TIMEOUT;
}

static void fifo_recover(struct geni_spi *s) {
  fifo_wr(s, SE_GENI_TX_WATERMARK_REG, 0u);
  fifo_wr(s, SE_GENI_M_CMD_CTRL_REG, GENI_M_CMD_CANCEL);
  if (fifo_wait_bit(s, GENI_M_CMD_CANCEL_DONE | GENI_M_CMD_DONE) == 0) {
    return;
  }
  fifo_wr(s, SE_GENI_M_CMD_CTRL_REG, GENI_M_CMD_ABORT);
  (void)fifo_wait_bit(s, GENI_M_CMD_ABORT_DONE | GENI_M_CMD_DONE);
}

static void fifo_capture_snap(struct geni_spi *s, uint32_t irq_status) {
  s->snap_dma_tx = fifo_rd(s, SE_DMA_TX_IRQ_STAT);
  s->snap_dma_rx = fifo_rd(s, SE_DMA_RX_IRQ_STAT);
  s->snap_dma_txin = fifo_rd(s, SE_DMA_TX_LEN_IN);
  s->snap_dma_rxin = fifo_rd(s, SE_DMA_RX_LEN_IN);
  s->snap_irq = irq_status;
  s->snap_status = fifo_rd(s, SE_GENI_STATUS);
  s->snap_txfs = fifo_rd(s, SE_GENI_TX_FIFO_STATUS);
  s->snap_rxfs = fifo_rd(s, SE_GENI_RX_FIFO_STATUS);
  s->snap_valid = 1;
}

int sm6125_geni_fifo_transfer(
    struct geni_spi *s,
    const uint8_t *tx,
    uint8_t *rx,
    size_t len,
    int keep_cs) {
  if (!s || !s->se.rd || !s->se.wr || !len || len > GENI_TRANS_LEN_MASK || (!tx && !rx) || !s->cur_speed_hz) {
    return GENI_ERR_PARAM;
  }
  if (s->use_dma) {
    return GENI_ERR_NOTSUP;
  }
  if (s->info.fifo_width_bits != 32u || s->info.tx_fifo_depth < 4u) {
    return GENI_ERR_NODEV;
  }

  if (s->cur_bpw != 8u) {
    fifo_set_word_len_8(s, !(s->last_mode & GENI_SPI_LSB_FIRST));
  }

  s->snap_valid = 0;
  struct fifo_xfer x = {tx, rx, tx ? len : 0u, rx ? len : 0u, len};

  uint32_t cmd = 0u;
  if (tx) {
    cmd |= GENI_M_CMD_TX_ONLY;
    fifo_wr(s, SE_SPI_TX_TRANS_LEN, (uint32_t)len);
  }
  if (rx) {
    cmd |= GENI_M_CMD_RX_ONLY;
    fifo_wr(s, SE_SPI_RX_TRANS_LEN, (uint32_t)len);
  }
  if (cmd == (GENI_M_CMD_TX_ONLY | GENI_M_CMD_RX_ONLY)) {
    cmd = GENI_M_CMD_TX_RX;
  }

  fifo_wr(s, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFFu);
  fifo_wr(s, SE_GENI_M_CMD0, (cmd << GENI_M_OPCODE_SHIFT) | (keep_cs ? GENI_M_PARAM_FRAGMENTATION : 0u));
  if (tx && fifo_handle_tx(s, &x)) {
    fifo_wr(s, SE_GENI_TX_WATERMARK_REG, s->tx_wm);
  }

  int status = GENI_ERR_TIMEOUT;
  for (unsigned n = 0; n < s->poll_limit; ++n) {
    uint32_t st = fifo_rd(s, SE_GENI_M_IRQ_STATUS);
    if (!st) {
      if (s->se.delay_us) {
        s->se.delay_us(s->se.ctx, 1u);
      }
      continue;
    }

    if (st & GENI_M_ERRORS) {
      fifo_capture_snap(s, st);
      fifo_wr(s, SE_GENI_M_IRQ_CLEAR, st);
      status = GENI_ERR_IO;
      break;
    }

    if (st & (GENI_M_RX_FIFO_WATERMARK | GENI_M_RX_FIFO_LAST)) {
      fifo_handle_rx(s, &x);
    }
    if ((st & GENI_M_TX_FIFO_WATERMARK) && tx) {
      (void)fifo_handle_tx(s, &x);
    }

    /* Limpar watermarks latched-level somente APOS drenar RX e reabastecer TX (spi-geni-qcom.c:1055-1060). */
    fifo_wr(s, SE_GENI_M_IRQ_CLEAR, st);

    if (st & GENI_M_CMD_DONE) {
      if (rx && x.rx_rem) {
        fifo_handle_rx(s, &x);
      }
      if (x.tx_rem) {
        fifo_wr(s, SE_GENI_TX_WATERMARK_REG, 0u);
      }
      if (x.tx_rem || x.rx_rem) {
        fifo_capture_snap(s, st);
        status = GENI_ERR_IO;
        break;
      }
      return 0;
    }
  }

  if (!s->snap_valid) {
    fifo_capture_snap(s, fifo_rd(s, SE_GENI_M_IRQ_STATUS));
  }
  fifo_recover(s);
  return status;
}
