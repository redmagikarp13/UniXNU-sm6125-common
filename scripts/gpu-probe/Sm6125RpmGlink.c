// SPDX-License-Identifier: BSD-2-Clause-Patent
// Implementacao pura em C do protocolo GLINK-RPM (qcom_glink_rpm + smd-rpm) para SM6125.
#include "Sm6125RpmGlink.h"
#include <stdbool.h>
#include <string.h>

enum {
  GLINK_CMD_VERSION = 0,
  GLINK_CMD_VERSION_ACK = 1,
  GLINK_CMD_OPEN = 2,
  GLINK_CMD_CLOSE = 3,
  GLINK_CMD_OPEN_ACK = 4,
  GLINK_CMD_TX_DATA = 9,
  GLINK_FEATURE_INTENTLESS = 0x04u
};

#define RPM_SERVICE_REQ 0x00716572u /* "req\0" */

static uint32_t align8(uint32_t n) { return (n + 7u) & ~7u; }
static uint32_t align4(uint32_t n) { return (n + 3u) & ~3u; }

static void pipe_write_bytes(const sm6125_rpm_glink_t *g, uint32_t pipe_off, uint32_t pipe_len,
                             uint32_t *head, const void *src, uint32_t len)
{
  const uint8_t *p = (const uint8_t *)src;
  uint32_t fifo_off = pipe_off + 8u;
  uint32_t pos = *head;
  for (uint32_t off = 0u; off < len; off += 4u) {
    uint32_t w = 0u;
    uint32_t rem = len - off;
    if (rem > 4u) rem = 4u;
    memcpy(&w, p + off, rem);
    g->wr32(g->ctx, g->msg_ram_va, fifo_off + pos, w);
    pos += 4u;
    if (pos >= pipe_len) pos -= pipe_len;
  }
  *head = pos;
}

static void pipe_send_msg(const sm6125_rpm_glink_t *g, uint16_t cmd, uint16_t p1, uint32_t p2,
                          const void *data, uint32_t dlen)
{
  uint8_t hdr[8];
  hdr[0] = (uint8_t)(cmd & 0xFFu);
  hdr[1] = (uint8_t)(cmd >> 8);
  hdr[2] = (uint8_t)(p1 & 0xFFu);
  hdr[3] = (uint8_t)(p1 >> 8);
  memcpy(hdr + 4, &p2, 4);

  uint32_t head = g->rd32(g->ctx, g->msg_ram_va, g->tx_off + 4u);
  pipe_write_bytes(g, g->tx_off, g->tx_len, &head, hdr, 8u);
  if (dlen > 0u && data) {
    uint32_t a4 = align4(dlen);
    uint8_t tmp[128];
    if (a4 > sizeof(tmp)) a4 = sizeof(tmp);
    memset(tmp, 0, a4);
    memcpy(tmp, data, dlen < a4 ? dlen : a4);
    pipe_write_bytes(g, g->tx_off, g->tx_len, &head, tmp, a4);
  }
  uint32_t total = 8u + dlen;
  uint32_t pad = align8(total) - ((8u + align4(dlen)));
  if (pad > 0u) {
    uint32_t zero = 0u;
    pipe_write_bytes(g, g->tx_off, g->tx_len, &head, &zero, pad);
  }
  g->wr32(g->ctx, g->msg_ram_va, g->tx_off + 4u, head);
  g->wr32(g->ctx, g->apcs_va, SM6125_APCS_IPC_OFFSET, SM6125_APCS_RPM_BIT);
}

static uint32_t rx_avail(const sm6125_rpm_glink_t *g)
{
  uint32_t tail = g->rd32(g->ctx, g->msg_ram_va, g->rx_off + 0u);
  uint32_t head = g->rd32(g->ctx, g->msg_ram_va, g->rx_off + 4u);
  if (head >= tail) return head - tail;
  return g->rx_len - tail + head;
}

static void rx_peek_bytes(const sm6125_rpm_glink_t *g, uint32_t off_in_msg, void *dst, uint32_t len)
{
  uint32_t tail = g->rd32(g->ctx, g->msg_ram_va, g->rx_off + 0u);
  uint32_t pos = (tail + off_in_msg) % g->rx_len;
  uint8_t *out = (uint8_t *)dst;
  uint32_t fifo_off = g->rx_off + 8u;
  for (uint32_t i = 0u; i < len; i += 4u) {
    uint32_t w = g->rd32(g->ctx, g->msg_ram_va, fifo_off + pos);
    uint32_t rem = len - i;
    if (rem > 4u) rem = 4u;
    memcpy(out + i, &w, rem);
    pos += 4u;
    if (pos >= g->rx_len) pos -= g->rx_len;
  }
}

static void rx_advance(const sm6125_rpm_glink_t *g, uint32_t bytes)
{
  uint32_t tail = g->rd32(g->ctx, g->msg_ram_va, g->rx_off + 0u);
  tail = (tail + bytes) % g->rx_len;
  g->wr32(g->ctx, g->msg_ram_va, g->rx_off + 0u, tail);
}

int sm6125_rpm_glink_init(sm6125_rpm_glink_t *g)
{
  if (!g || !g->rd32 || !g->wr32 || !g->msg_ram_va || !g->apcs_va) {
    return -1;
  }
  uint32_t magic = g->rd32(g->ctx, g->msg_ram_va, SM6125_RPM_TOC_OFFSET);
  if (magic != SM6125_RPM_TOC_MAGIC) {
    return -2;
  }
  uint32_t count = g->rd32(g->ctx, g->msg_ram_va, SM6125_RPM_TOC_OFFSET + 4u);
  if (count > 20u) count = 20u;
  g->tx_off = g->rx_off = 0u;
  for (uint32_t i = 0u; i < count; ++i) {
    uint32_t base = SM6125_RPM_TOC_OFFSET + 8u + i * 12u;
    uint32_t id = g->rd32(g->ctx, g->msg_ram_va, base + 0u);
    uint32_t off = g->rd32(g->ctx, g->msg_ram_va, base + 4u);
    uint32_t sz = g->rd32(g->ctx, g->msg_ram_va, base + 8u);
    if (id == SM6125_RPM_TX_FIFO_ID) {
      g->tx_off = off;
      g->tx_len = sz;
    } else if (id == SM6125_RPM_RX_FIFO_ID) {
      g->rx_off = off;
      g->rx_len = sz;
    }
  }
  if (!g->tx_off || !g->rx_off) {
    return -3;
  }
  g->lcid = 1u;
  g->rcid = 0u;
  g->next_msg_id = 1u;
  return 0;
}

static int poll_one_rx(sm6125_rpm_glink_t *g, uint16_t *cmd, uint16_t *p1, uint32_t *p2,
                       uint8_t *payload, uint32_t max_pay, uint32_t *pay_len)
{
  for (unsigned t = 0u; t < 2000u; ++t) {
    if (rx_avail(g) >= 8u) {
      uint8_t hdr[8];
      rx_peek_bytes(g, 0u, hdr, 8u);
      *cmd = (uint16_t)(hdr[0] | ((uint16_t)hdr[1] << 8));
      *p1 = (uint16_t)(hdr[2] | ((uint16_t)hdr[3] << 8));
      memcpy(p2, hdr + 4, 4);
      uint32_t extra = 0u;
      if (*cmd == GLINK_CMD_OPEN) {
        extra = *p2;
      } else if (*cmd == GLINK_CMD_TX_DATA) {
        uint32_t chunk = 0u;
        if (rx_avail(g) < 16u) {
          if (g->delay_us) g->delay_us(g->ctx, 50u);
          continue;
        }
        rx_peek_bytes(g, 8u, &chunk, 4u);
        extra = 8u + chunk;
      }
      uint32_t total = align8(8u + extra);
      if (rx_avail(g) < total) {
        if (g->delay_us) g->delay_us(g->ctx, 50u);
        continue;
      }
      if (payload && max_pay > 0u && extra > 0u) {
        uint32_t copy_n = (extra < max_pay) ? extra : max_pay;
        rx_peek_bytes(g, 8u, payload, copy_n);
        if (pay_len) *pay_len = copy_n;
      } else if (pay_len) {
        *pay_len = 0u;
      }
      rx_advance(g, total);
      return 0;
    }
    if (g->delay_us) g->delay_us(g->ctx, 50u);
  }
  return -1;
}

int sm6125_rpm_glink_open(sm6125_rpm_glink_t *g, uint32_t *out_ver, uint32_t *out_ack)
{
  pipe_send_msg(g, GLINK_CMD_VERSION, 1u, GLINK_FEATURE_INTENTLESS, NULL, 0u);
  uint16_t cmd = 0u, p1 = 0u;
  uint32_t p2 = 0u, plen = 0u;
  uint8_t buf[64];
  bool ver_ok = false;

  for (int i = 0; i < 8 && !ver_ok; ++i) {
    if (poll_one_rx(g, &cmd, &p1, &p2, buf, sizeof(buf), &plen) != 0) {
      return -10;
    }
    if (out_ver) *out_ver = ((uint32_t)cmd << 16) | p1;
    if (cmd == GLINK_CMD_VERSION) {
      pipe_send_msg(g, GLINK_CMD_VERSION_ACK, p1, p2 & GLINK_FEATURE_INTENTLESS, NULL, 0u);
    } else if (cmd == GLINK_CMD_VERSION_ACK) {
      ver_ok = true;
    }
  }
  if (!ver_ok) return -11;

  static const char name[] = "rpm_requests";
  pipe_send_msg(g, GLINK_CMD_OPEN, g->lcid, (uint32_t)sizeof(name), name, (uint32_t)sizeof(name));

  bool open_ack = false;
  for (int i = 0; i < 8 && !open_ack; ++i) {
    if (poll_one_rx(g, &cmd, &p1, &p2, buf, sizeof(buf), &plen) != 0) {
      return -12;
    }
    if (out_ack) *out_ack = ((uint32_t)cmd << 16) | p1;
    if (cmd == GLINK_CMD_OPEN_ACK && p1 == g->lcid) {
      open_ack = true;
    } else if (cmd == GLINK_CMD_OPEN) {
      g->rcid = p1;
      pipe_send_msg(g, GLINK_CMD_OPEN_ACK, p1, 0u, NULL, 0u);
    }
  }
  return open_ack ? 0 : -13;
}

int sm6125_rpm_glink_vote_rail(sm6125_rpm_glink_t *g, uint32_t type, uint32_t id,
                               uint32_t level, uint32_t enable, uint32_t *out_ack)
{
  uint8_t pkt[8 + 28 + 24];
  uint32_t msg_id = g->next_msg_id++;
  uint32_t kvp_len = 24u;
  uint32_t req_len = 20u + kvp_len;
  uint32_t rpm_len = 8u + req_len; /* 52 bytes */

  uint32_t w[15];
  w[0] = rpm_len; /* TX_DATA chunk_size */
  w[1] = 0u;      /* TX_DATA left_size */
  w[2] = RPM_SERVICE_REQ;
  w[3] = req_len;
  w[4] = msg_id;
  w[5] = 0u;      /* active state */
  w[6] = type;
  w[7] = id;
  w[8] = kvp_len;
  w[9] = SM6125_RPM_KEY_VLVL;
  w[10] = 4u;
  w[11] = level;
  w[12] = SM6125_RPM_KEY_SWEN;
  w[13] = 4u;
  w[14] = enable;
  memcpy(pkt, w, sizeof(w));

  pipe_send_msg(g, GLINK_CMD_TX_DATA, g->lcid, 0u, pkt, 8u + rpm_len);

  uint16_t cmd = 0u, p1 = 0u;
  uint32_t p2 = 0u, plen = 0u;
  uint8_t buf[96];
  for (int i = 0; i < 8; ++i) {
    if (poll_one_rx(g, &cmd, &p1, &p2, buf, sizeof(buf), &plen) != 0) {
      return -20;
    }
    if (cmd == GLINK_CMD_OPEN) {
      g->rcid = p1;
      pipe_send_msg(g, GLINK_CMD_OPEN_ACK, p1, 0u, NULL, 0u);
    } else if (cmd == GLINK_CMD_TX_DATA && plen >= 20u) {
      uint32_t hdr = 0u, rlen = 0u;
      memcpy(&hdr, buf + 8, 4);
      memcpy(&rlen, buf + 12, 4);
      if (out_ack) {
        uint32_t tag = 0u;
        memcpy(&tag, buf + 16, 4);
        *out_ack = tag;
      }
      /* Se houver KVP "err\0" (0x00727265) na resposta do RPM, retorna erro -22. */
      for (uint32_t off = 16u; off + 4u <= plen && off < 16u + rlen; off += 4u) {
        uint32_t k = 0u;
        memcpy(&k, buf + off, 4);
        if (k == 0x00727265u) {
          return -22;
        }
      }
      return (hdr == RPM_SERVICE_REQ) ? 0 : -23;
    }
  }
  return -21;
}
