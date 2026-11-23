// SPDX-License-Identifier: BSD-2-Clause-Patent
// Testes host ASan+UBSan para Sm6125GpuProbe (GCC, GPUCC, GDSCs, A610 RBBM, SMMU v500)
// e Sm6125RpmGlink (handshake GLINK-RPM contra um RPM simulado).
#include "../Sm6125GpuBoard.h"
#include "../Sm6125GpuProbe.h"
#include "../Sm6125RpmGlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { \
  if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    exit(1); \
  } \
} while (0)

/* ------------------------------------------------------------------ */
/* Simulador de GCC/GPUCC/RBBM/SMMU                                    */
/* ------------------------------------------------------------------ */
struct wr_log { uintptr_t base; uint32_t off; uint32_t val; };

struct sim_gpu {
  uint32_t gcc_vote;
  uint32_t gpucc[0x6000 / 4];
  uint32_t rbbm[0x1000 / 4];
  uint32_t smmu[0x100 / 4];
  int stuck_cx;           /* GDSC CX nunca liga (rail sem tensao) */
  int hung;               /* leitura de GPUCC antes do voto GCC = bus hang no silicio */
  struct wr_log log[64];
  unsigned nwrites;
};

static uint32_t sim_rd32(void *ctx, uintptr_t base, uint32_t off) {
  struct sim_gpu *g = (struct sim_gpu *)ctx;
  if (base == 0x1000u) {
    if (off == SM6125_GCC_GPU_VOTE_REG) return g->gcc_vote;
    return 0u;
  }
  if (base == 0x2000u && off < sizeof(g->gpucc)) {
    /* Evidencia de silicio (Marco 8A): GPUCC so responde com GPLL0/GPLL0_DIV votados no GCC. */
    if ((g->gcc_vote & (SM6125_GCC_GPU_GPLL0_BIT | SM6125_GCC_GPU_GPLL0_DIV_BIT)) !=
        (SM6125_GCC_GPU_GPLL0_BIT | SM6125_GCC_GPU_GPLL0_DIV_BIT)) {
      g->hung = 1;
    }
    return g->gpucc[off / 4];
  }
  if (base == 0x3000u && off < sizeof(g->rbbm)) return g->rbbm[off / 4];
  if (base == 0x4000u && off < sizeof(g->smmu)) return g->smmu[off / 4];
  return 0u;
}

static void sim_wr32(void *ctx, uintptr_t base, uint32_t off, uint32_t val) {
  struct sim_gpu *g = (struct sim_gpu *)ctx;
  if (g->nwrites < 64u) {
    g->log[g->nwrites].base = base;
    g->log[g->nwrites].off = off;
    g->log[g->nwrites].val = val;
  }
  g->nwrites++;
  if (base == 0x1000u) {
    if (off == SM6125_GCC_GPU_VOTE_REG) g->gcc_vote = val;
    return;
  }
  if (base == 0x2000u && off < sizeof(g->gpucc)) {
    if (off == SM6125_GPUCC_CX_GDSCR || off == SM6125_GPUCC_GX_GDSCR) {
      if (g->stuck_cx && off == SM6125_GPUCC_CX_GDSCR) {
        g->gpucc[off / 4] = val & ~SM6125_GDSCR_PWR_ON;
      } else {
        g->gpucc[off / 4] = (val & SM6125_GDSCR_SW_COLLAPSE) ? (val & ~SM6125_GDSCR_PWR_ON) : (val | SM6125_GDSCR_PWR_ON);
      }
    } else {
      g->gpucc[off / 4] = (val & SM6125_CBCR_CLK_ENABLE) ? (val & ~SM6125_CBCR_CLK_OFF) : (val | SM6125_CBCR_CLK_OFF);
    }
    return;
  }
  if (base == 0x3000u && off < sizeof(g->rbbm)) {
    g->rbbm[off / 4] = val;
  }
}

static void sim_gpu_reset(struct sim_gpu *g) {
  memset(g, 0, sizeof(*g));
  g->gcc_vote = 0x00905d7bu;
  g->gpucc[SM6125_GPUCC_CX_GDSCR / 4] = SM6125_GDSCR_SW_COLLAPSE;
  g->gpucc[SM6125_GPUCC_GX_GDSCR / 4] = SM6125_GDSCR_SW_COLLAPSE;
  g->gpucc[SM6125_GPUCC_AHB_CBCR / 4] = SM6125_CBCR_CLK_OFF;
  g->gpucc[SM6125_GPUCC_CX_GFX3D_CBCR / 4] = SM6125_CBCR_CLK_OFF;
  g->gpucc[SM6125_GPUCC_GX_GFX3D_CBCR / 4] = SM6125_CBCR_CLK_OFF;
  g->rbbm[SM6125_A6XX_RBBM_STATUS / 4] = 0x80000001u;
  g->rbbm[SM6125_A6XX_RBBM_SP_HYST_CNT / 4] = 0x00000040u;
  g->smmu[SM6125_SMMU_IDR0 / 4] = 0x00010010u;
  g->smmu[SM6125_SMMU_IDR7 / 4] = 0x00000050u;
}

static sm6125_gpu_bus_t make_bus(struct sim_gpu *g) {
  sm6125_gpu_bus_t b = { g, sim_rd32, sim_wr32, NULL, 0x1000u, 0x2000u, 0x3000u, 0x4000u };
  return b;
}

static void test_probe_hardware_happy(void) {
  struct sim_gpu g;
  sim_gpu_reset(&g);
  sm6125_gpu_bus_t bus = make_bus(&g);
  sm6125_gpu_probe_result_t res;
  memset(&res, 0, sizeof(res));

  CHECK(sm6125_gpu_probe_hardware(&bus, &res) == 0);
  CHECK(res.step == 7 && res.err == 0);
  CHECK((res.cx_gdscr & SM6125_GDSCR_PWR_ON) != 0u);
  CHECK((res.gx_gdscr & SM6125_GDSCR_PWR_ON) != 0u);
  CHECK(res.rbbm_status == 0x80000001u);
  CHECK(res.hyst_before == 0x00000040u && res.hyst_readback == 0x12345678u);
  CHECK(g.rbbm[SM6125_A6XX_RBBM_SP_HYST_CNT / 4] == 0x00000040u); /* restaurado */
  CHECK(res.smmu_idr7 == 0x00000050u);
  /* O voto GCC preserva os bits anteriores e liga GPLL0 + GPLL0_DIV. */
  CHECK((g.gcc_vote & 0x00905d7bu) == 0x00905d7bu);
  CHECK((g.gcc_vote & (SM6125_GCC_GPU_GPLL0_BIT | SM6125_GCC_GPU_GPLL0_DIV_BIT)) ==
        (SM6125_GCC_GPU_GPLL0_BIT | SM6125_GCC_GPU_GPLL0_DIV_BIT));
}

static void test_probe_hardware_cx_stuck(void) {
  struct sim_gpu g;
  sim_gpu_reset(&g);
  g.stuck_cx = 1;
  sm6125_gpu_bus_t bus = make_bus(&g);
  sm6125_gpu_probe_result_t res;
  memset(&res, 0, sizeof(res));
  CHECK(sm6125_gpu_probe_hardware(&bus, &res) == -1);
  CHECK(res.err == -13 && res.step == 3);
}

static void test_probe_null_args(void) {
  struct sim_gpu g;
  sim_gpu_reset(&g);
  sm6125_gpu_bus_t bus = make_bus(&g);
  sm6125_gpu_probe_result_t res;
  memset(&res, 0, sizeof(res));
  CHECK(sm6125_gpu_probe_hardware(NULL, &res) == -1);
  CHECK(sm6125_gpu_probe_hardware(&bus, NULL) == -1);
  CHECK(sm6125_gpu_probe_stage(NULL, 1u, &res) == -1);
  CHECK(sm6125_gpu_probe_stage(&bus, 1u, NULL) == -1);
  CHECK(sm6125_gpu_probe_stage(&bus, 0u, &res) == -1);
  CHECK(sm6125_gpu_probe_stage(&bus, 4u, &res) == -1);
  bus.gpu_va = 0u;
  CHECK(sm6125_gpu_probe_hardware(&bus, &res) == -1);
  CHECK(g.nwrites == 0u); /* argumento invalido nunca toca o hardware */
}

/* ------------------------------------------------------------------ */
/* Estagios: a sonda no silicio avanca de 1 em 1 para isolar hangs.    */
/* ------------------------------------------------------------------ */
static void test_stage1_is_gcc_read_only(void) {
  struct sim_gpu g;
  sim_gpu_reset(&g);
  sm6125_gpu_bus_t bus = make_bus(&g);
  sm6125_gpu_probe_result_t res;
  memset(&res, 0, sizeof(res));
  CHECK(sm6125_gpu_probe_stage(&bus, 1u, &res) == 0);
  CHECK(g.nwrites == 0u);
  CHECK(!g.hung);                      /* nao toca GPUCC antes do voto */
  CHECK(res.step == 1 && res.err == 0);
  CHECK(res.gcc_vote_before == 0x00905d7bu);
}

static void test_stage2_votes_gcc_then_reads_gpucc(void) {
  struct sim_gpu g;
  sim_gpu_reset(&g);
  sm6125_gpu_bus_t bus = make_bus(&g);
  sm6125_gpu_probe_result_t res;
  memset(&res, 0, sizeof(res));
  CHECK(sm6125_gpu_probe_stage(&bus, 2u, &res) == 0);
  CHECK(g.nwrites == 1u);              /* unica escrita: voto GCC */
  CHECK(g.log[0].base == 0x1000u && g.log[0].off == SM6125_GCC_GPU_VOTE_REG);
  CHECK(!g.hung);                      /* GPUCC lido so depois do voto */
  CHECK(res.step == 2);
  CHECK(res.gcc_vote_after == (0x00905d7bu | SM6125_GCC_GPU_GPLL0_BIT | SM6125_GCC_GPU_GPLL0_DIV_BIT));
  CHECK(res.cx_gdscr == SM6125_GDSCR_SW_COLLAPSE);
  CHECK(res.gx_gdscr == SM6125_GDSCR_SW_COLLAPSE);
  CHECK(res.ahb_cbcr == SM6125_CBCR_CLK_OFF);
  CHECK((g.gpucc[SM6125_GPUCC_CX_GDSCR / 4] & SM6125_GDSCR_PWR_ON) == 0u);
}

static void test_stage3_powers_cx_only(void) {
  struct sim_gpu g;
  sim_gpu_reset(&g);
  sm6125_gpu_bus_t bus = make_bus(&g);
  sm6125_gpu_probe_result_t res;
  memset(&res, 0, sizeof(res));
  CHECK(sm6125_gpu_probe_stage(&bus, 3u, &res) == 0);
  CHECK(!g.hung);
  CHECK(g.nwrites == 2u); /* voto GCC + CX_GDSCR; GX e CBCRs intocados */
  CHECK(g.log[1].base == 0x2000u && g.log[1].off == SM6125_GPUCC_CX_GDSCR);
  CHECK((g.log[1].val & SM6125_GDSCR_SW_COLLAPSE) == 0u);
  CHECK(res.step == 3 && res.err == 0);
  CHECK((res.cx_gdscr & SM6125_GDSCR_PWR_ON) != 0u);
  CHECK((g.gpucc[SM6125_GPUCC_GX_GDSCR / 4] & SM6125_GDSCR_PWR_ON) == 0u);
}

static void test_stage3_cx_timeout(void) {
  struct sim_gpu g;
  sim_gpu_reset(&g);
  g.stuck_cx = 1;
  sm6125_gpu_bus_t bus = make_bus(&g);
  sm6125_gpu_probe_result_t res;
  memset(&res, 0, sizeof(res));
  CHECK(sm6125_gpu_probe_stage(&bus, 3u, &res) == -1);
  CHECK(res.err == -13 && res.step == 3);
}

/* ------------------------------------------------------------------ */
/* RPM simulado (ap2r/r2ap em msg_ram + doorbell APCS)                 */
/* ------------------------------------------------------------------ */
enum rpm_mode { RPM_OK = 0, RPM_SILENT, RPM_REJECT, RPM_NO_OPEN_ACK };

#define SIM_MSG_RAM 0x5000u
#define SIM_APCS    0x6000u
#define TX_OFF      0x200u
#define RX_OFF      0x900u
#define FIFO_LEN    0x6F8u

struct sim_rpm {
  uint8_t ram[SM6125_RPM_MSG_RAM_SIZE];
  enum rpm_mode mode;
  unsigned doorbells;
  unsigned votes;
  uint32_t vote_type[4];
  uint32_t vote_level[4];
  uint32_t vote_enable[4];
};

static uint32_t ram_u32(struct sim_rpm *r, uint32_t off) {
  uint32_t v;
  memcpy(&v, r->ram + off, 4);
  return v;
}

static void ram_set_u32(struct sim_rpm *r, uint32_t off, uint32_t v) {
  memcpy(r->ram + off, &v, 4);
}

static void rx_push(struct sim_rpm *r, const void *data, uint32_t len) {
  uint32_t head = ram_u32(r, RX_OFF + 4u);
  const uint8_t *p = (const uint8_t *)data;
  for (uint32_t i = 0u; i < len; i += 4u) {
    uint32_t w = 0u;
    uint32_t rem = len - i;
    memcpy(&w, p + i, rem > 4u ? 4u : rem);
    ram_set_u32(r, RX_OFF + 8u + head, w);
    head = (head + 4u) % FIFO_LEN;
  }
  ram_set_u32(r, RX_OFF + 4u, head);
}

static void rx_push_hdr(struct sim_rpm *r, uint16_t cmd, uint16_t p1, uint32_t p2,
                        const void *extra, uint32_t extra_len) {
  uint8_t buf[128];
  memset(buf, 0, sizeof(buf));
  buf[0] = (uint8_t)cmd; buf[1] = (uint8_t)(cmd >> 8);
  buf[2] = (uint8_t)p1;  buf[3] = (uint8_t)(p1 >> 8);
  memcpy(buf + 4, &p2, 4);
  if (extra_len) memcpy(buf + 8, extra, extra_len);
  uint32_t total = (8u + extra_len + 7u) & ~7u;
  rx_push(r, buf, total);
}

static void rpm_reply_vote(struct sim_rpm *r, uint32_t msg_id, int reject) {
  uint32_t w[10];
  uint32_t n = 0u;
  w[n++] = 0u;                /* chunk (preenchido abaixo) */
  w[n++] = 0u;                /* left */
  w[n++] = 0x00716572u;       /* "req\0" */
  w[n++] = 0u;                /* length (preenchido abaixo) */
  w[n++] = 0x2367736du;       /* "msg#" */
  w[n++] = 4u;
  w[n++] = msg_id;
  if (reject) {
    w[n++] = 0x00727265u;     /* "err\0" */
    w[n++] = 0u;
  }
  uint32_t payload = (n - 2u) * 4u;
  w[0] = payload;
  w[3] = payload - 8u;
  rx_push_hdr(r, 9u, 1u, 0u, w, n * 4u);
}

static void rpm_handle_doorbell(struct sim_rpm *r) {
  r->doorbells++;
  if (r->mode == RPM_SILENT) return;
  uint32_t tail = ram_u32(r, TX_OFF + 0u);
  uint32_t head = ram_u32(r, TX_OFF + 4u);
  while (tail != head) {
    uint8_t hdr[8];
    for (uint32_t i = 0u; i < 8u; i += 4u) {
      uint32_t w = ram_u32(r, TX_OFF + 8u + ((tail + i) % FIFO_LEN));
      memcpy(hdr + i, &w, 4);
    }
    uint16_t cmd = (uint16_t)(hdr[0] | (hdr[1] << 8));
    uint16_t p1 = (uint16_t)(hdr[2] | (hdr[3] << 8));
    uint32_t p2;
    memcpy(&p2, hdr + 4, 4);
    uint32_t extra = 0u;
    if (cmd == 2u) {
      extra = p2;
    } else if (cmd == 9u) {
      extra = 8u + ram_u32(r, TX_OFF + 8u + ((tail + 8u) % FIFO_LEN));
    }
    uint32_t total = (8u + extra + 7u) & ~7u;

    if (cmd == 0u) {            /* VERSION do AP -> VERSION_ACK do RPM */
      rx_push_hdr(r, 1u, p1, p2, NULL, 0u);
    } else if (cmd == 2u) {     /* OPEN do AP -> OPEN_ACK + OPEN remoto */
      if (r->mode != RPM_NO_OPEN_ACK) rx_push_hdr(r, 4u, p1, 0u, NULL, 0u);
      static const char name[] = "rpm_requests";
      rx_push_hdr(r, 2u, 5u, (uint32_t)sizeof(name), name, (uint32_t)sizeof(name));
    } else if (cmd == 9u) {     /* TX_DATA do AP: requisicao de voto */
      uint32_t base = TX_OFF + 8u;
      uint32_t msg_id = ram_u32(r, base + ((tail + 24u) % FIFO_LEN));
      uint32_t type = ram_u32(r, base + ((tail + 32u) % FIFO_LEN));
      uint32_t lvl = ram_u32(r, base + ((tail + 52u) % FIFO_LEN));
      uint32_t en = ram_u32(r, base + ((tail + 64u) % FIFO_LEN));
      if (r->votes < 4u) {
        r->vote_type[r->votes] = type;
        r->vote_level[r->votes] = lvl;
        r->vote_enable[r->votes] = en;
      }
      r->votes++;
      rpm_reply_vote(r, msg_id, r->mode == RPM_REJECT);
    }
    tail = (tail + total) % FIFO_LEN;
  }
  ram_set_u32(r, TX_OFF + 0u, tail);
}

static uint32_t rpm_rd32(void *ctx, uintptr_t base, uint32_t off) {
  struct sim_rpm *r = (struct sim_rpm *)ctx;
  CHECK(base == SIM_MSG_RAM && off + 4u <= SM6125_RPM_MSG_RAM_SIZE);
  return ram_u32(r, off);
}

static void rpm_wr32(void *ctx, uintptr_t base, uint32_t off, uint32_t val) {
  struct sim_rpm *r = (struct sim_rpm *)ctx;
  if (base == SIM_APCS) {
    CHECK(off == SM6125_APCS_IPC_OFFSET && val == SM6125_APCS_RPM_BIT);
    rpm_handle_doorbell(r);
    return;
  }
  CHECK(base == SIM_MSG_RAM && off + 4u <= SM6125_RPM_MSG_RAM_SIZE);
  ram_set_u32(r, off, val);
}

static void rpm_init(struct sim_rpm *r, enum rpm_mode mode) {
  memset(r, 0, sizeof(*r));
  r->mode = mode;
  uint32_t toc = SM6125_RPM_TOC_OFFSET;
  ram_set_u32(r, toc + 0u, SM6125_RPM_TOC_MAGIC);
  ram_set_u32(r, toc + 4u, 2u);
  ram_set_u32(r, toc + 8u, SM6125_RPM_TX_FIFO_ID);
  ram_set_u32(r, toc + 12u, TX_OFF);
  ram_set_u32(r, toc + 16u, FIFO_LEN);
  ram_set_u32(r, toc + 20u, SM6125_RPM_RX_FIFO_ID);
  ram_set_u32(r, toc + 24u, RX_OFF);
  ram_set_u32(r, toc + 28u, FIFO_LEN);
}

static void glink_setup(sm6125_rpm_glink_t *g, struct sim_rpm *r) {
  memset(g, 0, sizeof(*g));
  g->ctx = r;
  g->rd32 = rpm_rd32;
  g->wr32 = rpm_wr32;
  g->msg_ram_va = SIM_MSG_RAM;
  g->apcs_va = SIM_APCS;
}

static void test_glink_init_errors(void) {
  struct sim_rpm r;
  sm6125_rpm_glink_t g;
  rpm_init(&r, RPM_OK);
  glink_setup(&g, &r);
  CHECK(sm6125_rpm_glink_init(NULL) == -1);
  g.rd32 = NULL;
  CHECK(sm6125_rpm_glink_init(&g) == -1);
  glink_setup(&g, &r);
  g.apcs_va = 0u;
  CHECK(sm6125_rpm_glink_init(&g) == -1);

  glink_setup(&g, &r);
  ram_set_u32(&r, SM6125_RPM_TOC_OFFSET, 0u);       /* magic invalido */
  CHECK(sm6125_rpm_glink_init(&g) == -2);

  rpm_init(&r, RPM_OK);
  ram_set_u32(&r, SM6125_RPM_TOC_OFFSET + 20u, 0u); /* sem FIFO r2ap */
  glink_setup(&g, &r);
  CHECK(sm6125_rpm_glink_init(&g) == -3);
}

static void test_glink_init_parses_toc(void) {
  struct sim_rpm r;
  sm6125_rpm_glink_t g;
  rpm_init(&r, RPM_OK);
  glink_setup(&g, &r);
  CHECK(sm6125_rpm_glink_init(&g) == 0);
  CHECK(g.tx_off == TX_OFF && g.tx_len == FIFO_LEN);
  CHECK(g.rx_off == RX_OFF && g.rx_len == FIFO_LEN);
  CHECK(g.lcid == 1u && g.next_msg_id == 1u);
}

static void test_glink_open_and_vote(void) {
  struct sim_rpm r;
  sm6125_rpm_glink_t g;
  rpm_init(&r, RPM_OK);
  glink_setup(&g, &r);
  uint32_t ver = 0u, oack = 0u, ack = 0u;
  CHECK(sm6125_rpm_glink_init(&g) == 0);
  CHECK(sm6125_rpm_glink_open(&g, &ver, &oack) == 0);
  CHECK(ver == 0x00010001u);          /* VERSION_ACK v1 */
  CHECK(oack == 0x00040001u);         /* OPEN_ACK lcid=1 */

  CHECK(sm6125_rpm_glink_vote_rail(&g, SM6125_RPM_TYPE_RWCX, 0u, 0x100u, 1u, &ack) == 0);
  CHECK(ack == 0x2367736du);          /* "msg#" */
  CHECK(g.rcid == 5u);                /* OPEN remoto pendente consumido no 1o voto */
  CHECK(sm6125_rpm_glink_vote_rail(&g, SM6125_RPM_TYPE_RWMX, 0u, 0x100u, 1u, &ack) == 0);
  CHECK(r.votes == 2u);
  CHECK(r.vote_type[0] == SM6125_RPM_TYPE_RWCX && r.vote_type[1] == SM6125_RPM_TYPE_RWMX);
  CHECK(r.vote_level[0] == 0x100u && r.vote_enable[0] == 1u);
  CHECK(g.next_msg_id == 3u);         /* msg_id incrementa por voto */
}

static void test_glink_votes_wrap_fifo(void) {
  /* Muitos votos forcam o wrap-around do ap2r/r2ap (0x6F8 bytes). */
  struct sim_rpm r;
  sm6125_rpm_glink_t g;
  rpm_init(&r, RPM_OK);
  glink_setup(&g, &r);
  uint32_t ack = 0u;
  CHECK(sm6125_rpm_glink_init(&g) == 0);
  CHECK(sm6125_rpm_glink_open(&g, NULL, NULL) == 0);
  for (unsigned i = 0u; i < 80u; ++i) {
    CHECK(sm6125_rpm_glink_vote_rail(&g, SM6125_RPM_TYPE_RWCX, 0u, 0x100u, 1u, &ack) == 0);
  }
  CHECK(r.votes == 80u);
}

static void test_glink_open_timeouts(void) {
  struct sim_rpm r;
  sm6125_rpm_glink_t g;
  rpm_init(&r, RPM_SILENT);
  glink_setup(&g, &r);
  CHECK(sm6125_rpm_glink_init(&g) == 0);
  CHECK(sm6125_rpm_glink_open(&g, NULL, NULL) == -10);   /* sem VERSION_ACK */
  CHECK(r.doorbells == 1u);

  rpm_init(&r, RPM_NO_OPEN_ACK);
  glink_setup(&g, &r);
  CHECK(sm6125_rpm_glink_init(&g) == 0);
  CHECK(sm6125_rpm_glink_open(&g, NULL, NULL) != 0);     /* OPEN_ACK nunca chega */
}

static void test_glink_vote_timeout(void) {
  struct sim_rpm r;
  sm6125_rpm_glink_t g;
  uint32_t ack = 0u;
  rpm_init(&r, RPM_OK);
  glink_setup(&g, &r);
  CHECK(sm6125_rpm_glink_init(&g) == 0);
  CHECK(sm6125_rpm_glink_open(&g, NULL, NULL) == 0);
  r.mode = RPM_SILENT;                                    /* RPM para de responder */
  CHECK(sm6125_rpm_glink_vote_rail(&g, SM6125_RPM_TYPE_RWCX, 0u, 0x100u, 1u, &ack) == -20);
}

static void test_glink_vote_rejected(void) {
  struct sim_rpm r;
  sm6125_rpm_glink_t g;
  uint32_t ack = 0u;
  rpm_init(&r, RPM_OK);
  glink_setup(&g, &r);
  CHECK(sm6125_rpm_glink_init(&g) == 0);
  CHECK(sm6125_rpm_glink_open(&g, NULL, NULL) == 0);
  r.mode = RPM_REJECT;                                    /* RPM responde com "err\0" */
  CHECK(sm6125_rpm_glink_vote_rail(&g, SM6125_RPM_TYPE_RWCX, 0u, 0x100u, 1u, &ack) == -22);
}

static void test_board_mmio_bases(void) {
  /* Evita regressao de base GCC errada (ex: 0x00179000 de SM8150 vs 0x01479000 do SM6125). */
  CHECK(SM6125_GCC_GPU_VOTE_BASE == 0x01479000u);
  CHECK(SM6125_GPUCC_BASE == 0x05990000u);
  CHECK(SM6125_GPU_BASE == 0x05900000u);
  CHECK(SM6125_GPU_SMMU_BASE == 0x059A0000u);
  CHECK(SM6125_RPM_MSG_RAM_BASE == 0x045F0000u);
  CHECK(SM6125_APCS_GLB_BASE == 0x0F111000u);
}

int main(void) {
  test_board_mmio_bases();
  test_probe_hardware_happy();
  test_probe_hardware_cx_stuck();
  test_probe_null_args();
  test_stage1_is_gcc_read_only();
  test_stage2_votes_gcc_then_reads_gpucc();
  test_stage3_powers_cx_only();
  test_stage3_cx_timeout();
  test_glink_init_errors();
  test_glink_init_parses_toc();
  test_glink_open_and_vote();
  test_glink_votes_wrap_fifo();
  test_glink_open_timeouts();
  test_glink_vote_timeout();
  test_glink_vote_rejected();
  printf("PASS: Sm6125GpuTests (GCC/GPUCC/GDSCs/A610-RBBM/SMMUv500/GLINK-RPM/stages)\n");
  return 0;
}
