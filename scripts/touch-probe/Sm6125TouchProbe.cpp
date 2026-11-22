// SPDX-License-Identifier: MIT
// Sonda em estagios (desligada por padrao) para QUPv3 SE2 SPI + Novatek NT36672A no SM6125 (ginkgo).
#include "Sm6125Buttons.h"
#include "Sm6125GeniFifo.h"
#include "Sm6125NvtProbe.h"
#include "Sm6125TouchBoard.h"
extern "C" {
#include "GeniSpi.h"
extern const uint8_t sm6125_touch_fw_data[];
extern const uint32_t sm6125_touch_fw_size;
extern const char sm6125_touch_fw_name[];
}
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" {
void __GLOBAL__sub_I_Sm6125HidInput_cpp(void) __asm("__GLOBAL__sub_I_Sm6125HidInput.cpp");
IOService *sm6125_hid_input_allocate(void);
kern_return_t OSKextLoadKextWithIdentifier(const char *identifier);
void sm6125_touch_notify_sink(void);
void sm6125_buttons_notify_sink(uint32_t mask);
extern volatile uint32_t sm6125_touch_seq;
extern volatile uint32_t sm6125_touch_count;
extern volatile uint32_t sm6125_touch_hid_reports;
extern volatile uint32_t sm6125_touch_max_x;
extern volatile uint32_t sm6125_touch_max_y;
extern volatile int sm6125_touch_xs[12];
extern volatile int sm6125_touch_ys[12];
extern volatile int sm6125_touch_ids[12];
extern volatile uint32_t sm6125_hid_diag[8];
}

extern "C" {

__attribute__((noinline)) static uint32_t touch_mmio_read32(uintptr_t base, uint32_t offset)
{
  uint32_t val = 0;
  const uintptr_t addr = base + offset;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(val) : "r"(addr) : "memory");
  return val;
}

__attribute__((noinline)) static void touch_mmio_write32(uintptr_t base, uint32_t offset, uint32_t val)
{
  const uintptr_t addr = base + offset;
  __asm__ volatile("str %w0, [%1]\n\tdsb sy" : : "r"(val), "r"(addr) : "memory");
}

}  // extern "C"

namespace {

static uint8_t s_nvt_scratch[SM6125_NVT_CHUNK_LEN + 8u];

struct MappedWindow {
  IODeviceMemory *mem = nullptr;
  IOMemoryMap *map = nullptr;
  uintptr_t va = 0;
};

bool map_window(uint64_t phys, uint64_t length, MappedWindow *w)
{
  IODeviceMemory *mem = IODeviceMemory::withRange(phys, length);
  if (!mem) {
    return false;
  }
  IOMemoryMap *map = mem->map(kIOMapAnywhere | kIOMapInhibitCache);
  if (!map || !map->getVirtualAddress()) {
    if (map) map->release();
    mem->release();
    return false;
  }
  w->mem = mem;
  w->map = map;
  w->va = static_cast<uintptr_t>(map->getVirtualAddress());
  return true;
}

void unmap_window(MappedWindow *w)
{
  if (w->map) {
    w->map->release();
    w->map = nullptr;
  }
  if (w->mem) {
    w->mem->release();
    w->mem = nullptr;
  }
  w->va = 0;
}

uint32_t io_rd(void *ctx, uint32_t reg)
{
  return touch_mmio_read32(reinterpret_cast<uintptr_t>(ctx), reg);
}

void io_wr(void *ctx, uint32_t reg, uint32_t val)
{
  touch_mmio_write32(reinterpret_cast<uintptr_t>(ctx), reg, val);
}

void io_delay_us(void *, unsigned us)
{
  while (us > 1000u) {
    IODelay(1000u);
    us -= 1000u;
  }
  if (us) {
    IODelay(us);
  }
}

void delay_ms_spin(void *, unsigned ms)
{
  for (unsigned i = 0; i < ms; ++i) {
    IODelay(1000u);
  }
}

struct TouchProbeState {
  uint32_t stage_req;
  uint32_t stage_done;
  uint32_t step;
  int32_t err;

  uint32_t gpll_vote_0;
  uint32_t qup_vote_0;
  uint32_t qup_vote_1;
  uint32_t gpll0_mode;
  uint32_t gpll6_mode;
  uint32_t dfsr_cmd;
  uint32_t rcg_cmd;
  uint32_t rcg_cfg;
  uint32_t cbcr_m_ahb;
  uint32_t cbcr_s_ahb;
  uint32_t cbcr_core;
  uint32_t cbcr_core_2x;
  uint32_t cbcr_s2;
  uint64_t dfs_hz[SM6125_DFS_LEVELS];
  unsigned num_levels;

  uint32_t pin0_spi[4];
  uint32_t pin1_spi[4];
  uint32_t pin0_rst_ctl;
  uint32_t pin0_rst_io;
  uint32_t pin0_irq_ctl;
  uint32_t pin0_irq_io;
  uint32_t pin1_rst_ctl;
  uint32_t pin1_rst_io;
  uint32_t rst_applied;

  struct geni_spi spi;
  uint32_t sel_level;
  uint32_t sel_div;
  uint64_t sel_sclk_hz;
  uint64_t sel_actual_hz;

  int32_t loop_rc;
  uint32_t loop_ok;
  uint8_t loop_rx[8];

  uint32_t trace_capture;
  uint32_t trace_n;
  uint32_t trace_len[6];
  uint8_t trace_tx[6][4];
  uint8_t trace_rx[6][8];

  uint32_t modes_tried;
  uint32_t mode_val[5];
  int32_t mode_rc[5];
  uint8_t mode_id[5][6];
  uint32_t mode_win;
  sm6125_nvt_ident_result_t nvt;

  uint32_t dl_attempted;
  sm6125_nvt_dl_diag_t dl;
  uint32_t last_err_len;
  int32_t last_err_rc;
  sm6125_nvt_fw_info_t fw_info;
  uint32_t irq_rise;
  uint32_t irq_fall;
  uint32_t pkt_reads;
  uint32_t pkt_ok;
  uint32_t pkt_bad;
  uint32_t contacts;
  uint32_t max_fingers;
  int32_t first_x;
  int32_t first_y;
  int32_t last_x;
  int32_t last_y;
  int32_t min_x;
  int32_t max_x;
  int32_t min_y;
  int32_t max_y;
  uint8_t first_pkt[12];
};

int traced_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
  auto *s = static_cast<TouchProbeState *>(ctx);
  int rc = sm6125_geni_fifo_transfer(&s->spi, tx, rx, len, 0);
  if (rc != 0) {
    s->last_err_len = static_cast<uint32_t>(len);
    s->last_err_rc = rc;
  }
  if (s->trace_capture && (rx != nullptr || rc != 0) && s->trace_n < 6u) {
    unsigned idx = s->trace_n++;
    s->trace_len[idx] = static_cast<uint32_t>(len);
    for (unsigned i = 0; i < 4u; ++i) {
      s->trace_tx[idx][i] = (tx && i < len) ? tx[i] : 0u;
    }
    for (unsigned i = 0; i < 8u; ++i) {
      s->trace_rx[idx][i] = (!rc && rx && i < len) ? rx[i] : 0u;
    }
  }
  return rc;
}

void read_cbcrs(uintptr_t qup0_va, TouchProbeState *s)
{
  s->cbcr_m_ahb = touch_mmio_read32(qup0_va, SM6125_GCC_QUP0_M_AHB_CBCR - 0x1f000u);
  s->cbcr_s_ahb = touch_mmio_read32(qup0_va, SM6125_GCC_QUP0_S_AHB_CBCR - 0x1f000u);
  s->cbcr_core = touch_mmio_read32(qup0_va, SM6125_GCC_QUP0_CORE_CBCR - 0x1f000u);
  s->cbcr_core_2x = touch_mmio_read32(qup0_va, SM6125_GCC_QUP0_CORE_2X_CBCR - 0x1f000u);
  s->cbcr_s2 = touch_mmio_read32(qup0_va, SM6125_GCC_QUP0_S2_CBCR - 0x1f000u);
}

void take_snapshot(
    uintptr_t gpll0_va,
    uintptr_t gpll6_va,
    uintptr_t qup0_va,
    uintptr_t vote_va,
    uintptr_t spi_pins_va,
    uintptr_t touch_pins_va,
    TouchProbeState *s)
{
  s->step = 1u;
  s->gpll0_mode = touch_mmio_read32(gpll0_va, 0u);
  s->gpll6_mode = touch_mmio_read32(gpll6_va, 0u);
  s->gpll_vote_0 = touch_mmio_read32(vote_va, 0u);
  s->qup_vote_0 = touch_mmio_read32(vote_va, SM6125_GCC_QUP0_VOTE - 0x79000u);
  s->qup_vote_1 = s->qup_vote_0;

  read_cbcrs(qup0_va, s);
  const uint32_t s2_off = SM6125_S2_RCG - 0x1f000u;
  s->rcg_cmd = touch_mmio_read32(qup0_va, s2_off + SM6125_RCG_CMD);
  s->rcg_cfg = touch_mmio_read32(qup0_va, s2_off + SM6125_RCG_CFG);
  s->dfsr_cmd = touch_mmio_read32(qup0_va, s2_off + SM6125_RCG_DFSR_CMD);

  for (unsigned l = 0; l < SM6125_DFS_LEVELS; ++l) {
    uint32_t cfg = touch_mmio_read32(qup0_va, s2_off + SM6125_RCG_PERF_CFG(l));
    uint32_t m = touch_mmio_read32(qup0_va, s2_off + SM6125_RCG_PERF_M(l));
    uint32_t n = touch_mmio_read32(qup0_va, s2_off + SM6125_RCG_PERF_N(l));
    s->dfs_hz[l] = sm6125_dfs_level_hz(cfg, m, n);
  }
  s->num_levels = SM6125_DFS_LEVELS;

  for (unsigned i = 0; i < 4u; ++i) {
    s->pin0_spi[i] = touch_mmio_read32(spi_pins_va, i * SM6125_GPIO_STRIDE);
    s->pin1_spi[i] = s->pin0_spi[i];
  }
  s->pin0_rst_ctl = touch_mmio_read32(touch_pins_va, 0u);
  s->pin0_rst_io = touch_mmio_read32(touch_pins_va, 4u);
  s->pin0_irq_ctl = touch_mmio_read32(touch_pins_va, SM6125_GPIO_STRIDE);
  s->pin0_irq_io = touch_mmio_read32(touch_pins_va, SM6125_GPIO_STRIDE + 4u);
  s->pin1_rst_ctl = s->pin0_rst_ctl;
  s->pin1_rst_io = s->pin0_rst_io;
  s->stage_done = 1u;
}

bool enable_clocks_and_pins(
    uintptr_t gpll0_va,
    uintptr_t gpll6_va,
    uintptr_t qup0_va,
    uintptr_t vote_va,
    uintptr_t spi_pins_va,
    uintptr_t touch_pins_va,
    uint32_t do_reset,
    TouchProbeState *s)
{
  s->step = 2u;
  const uint32_t s2_off = SM6125_S2_RCG - 0x1f000u;

  if (s->dfsr_cmd & SM6125_DFSR_DFS_EN) {
    uint32_t need_pll = SM6125_VOTE_GPLL0;
    for (unsigned l = 0; l < SM6125_DFS_LEVELS; ++l) {
      uint32_t cfg = touch_mmio_read32(qup0_va, s2_off + SM6125_RCG_PERF_CFG(l));
      if (((cfg >> 8) & 7u) == 4u) {
        need_pll |= SM6125_VOTE_GPLL6;
      }
    }
    uint32_t gv = touch_mmio_read32(vote_va, 0u);
    if ((gv & need_pll) != need_pll) {
      touch_mmio_write32(vote_va, 0u, gv | need_pll);
    }
    for (unsigned i = 0; i < 5000u; ++i) {
      s->gpll0_mode = touch_mmio_read32(gpll0_va, 0u);
      s->gpll6_mode = touch_mmio_read32(gpll6_va, 0u);
      bool ok0 = (s->gpll0_mode & SM6125_GCC_PLL_LOCK_DET) != 0u;
      bool ok6 = !(need_pll & SM6125_VOTE_GPLL6) || ((s->gpll6_mode & SM6125_GCC_PLL_LOCK_DET) != 0u);
      if (ok0 && ok6) {
        break;
      }
      IODelay(10u);
    }
  } else {
    touch_mmio_write32(qup0_va, s2_off + SM6125_RCG_CFG, 0u);
    touch_mmio_write32(qup0_va, s2_off + SM6125_RCG_CMD, SM6125_RCG_CMD_UPDATE);
    for (unsigned i = 0; i < 1000u; ++i) {
      s->rcg_cmd = touch_mmio_read32(qup0_va, s2_off + SM6125_RCG_CMD);
      if ((s->rcg_cmd & SM6125_RCG_CMD_UPDATE) == 0u) {
        break;
      }
      IODelay(10u);
    }
    s->rcg_cfg = touch_mmio_read32(qup0_va, s2_off + SM6125_RCG_CFG);
    s->dfs_hz[0] = 19200000ULL;
    for (unsigned l = 1u; l < SM6125_DFS_LEVELS; ++l) {
      s->dfs_hz[l] = 0ULL;
    }
    s->num_levels = 1u;
  }

  uint32_t qv = touch_mmio_read32(vote_va, SM6125_GCC_QUP0_VOTE - 0x79000u);
  touch_mmio_write32(vote_va, SM6125_GCC_QUP0_VOTE - 0x79000u, qv | SM6125_VOTE_QUP0_SE2_ALL);
  s->qup_vote_1 = touch_mmio_read32(vote_va, SM6125_GCC_QUP0_VOTE - 0x79000u);

  bool clocks_ok = false;
  for (unsigned i = 0; i < 2000u && !clocks_ok; ++i) {
    read_cbcrs(qup0_va, s);
    clocks_ok =
        sm6125_cbcr_running(s->cbcr_m_ahb, 0) &&
        sm6125_cbcr_running(s->cbcr_s_ahb, 1) &&
        sm6125_cbcr_running(s->cbcr_core, 0) &&
        sm6125_cbcr_running(s->cbcr_core_2x, 0) &&
        sm6125_cbcr_running(s->cbcr_s2, 0);
    if (!clocks_ok) {
      IODelay(10u);
    }
  }
  if (!clocks_ok) {
    s->err = -110;
    return false;
  }

  const uint32_t spi_cfg = sm6125_tlmm_pin_cfg(SM6125_FUNC_QUP02, SM6125_DRIVE_6MA, SM6125_PULL_NONE, 0u);
  for (unsigned i = 0; i < 4u; ++i) {
    touch_mmio_write32(spi_pins_va, i * SM6125_GPIO_STRIDE, spi_cfg);
    s->pin1_spi[i] = touch_mmio_read32(spi_pins_va, i * SM6125_GPIO_STRIDE);
  }

  /* Configura GPIO 88 (touch IRQ) como entrada com pull-up 8mA (ts_int_active no stock-fdt.dts:9237). */
  const uint32_t irq_cfg = sm6125_tlmm_pin_cfg(SM6125_FUNC_GPIO, SM6125_DRIVE_8MA, SM6125_PULL_UP, 0u);
  touch_mmio_write32(touch_pins_va, SM6125_GPIO_STRIDE, irq_cfg);

  if (do_reset == 1u) {
    const uint32_t rst_cfg = sm6125_tlmm_pin_cfg(SM6125_FUNC_GPIO, SM6125_DRIVE_8MA, SM6125_PULL_UP, 1u);
    touch_mmio_write32(touch_pins_va, 4u, 0u);
    touch_mmio_write32(touch_pins_va, 0u, rst_cfg);
    delay_ms_spin(nullptr, 10u);
    touch_mmio_write32(touch_pins_va, 4u, 2u);
    delay_ms_spin(nullptr, 20u);
    s->pin1_rst_ctl = touch_mmio_read32(touch_pins_va, 0u);
    s->pin1_rst_io = touch_mmio_read32(touch_pins_va, 4u);
    s->rst_applied = 1u;
  }
  return true;
}

bool configure_spi_clock(TouchProbeState *s, uint32_t target_hz)
{
  struct sm6125_clock_choice choice = {};
  if (sm6125_choose_clock(s->dfs_hz, s->num_levels, s->spi.oversampling, target_hz, &choice)) {
    return false;
  }
  if (geni_spi_set_clock(&s->spi, target_hz, static_cast<unsigned>(choice.sclk_hz), choice.level)) {
    return false;
  }
  s->sel_level = choice.level;
  s->sel_div = choice.div;
  s->sel_sclk_hz = choice.sclk_hz;
  s->sel_actual_hz = choice.actual_hz;
  return true;
}

static uintptr_t s_fb_va = 0;

void fb_paint_band(uint32_t y0, uint32_t y1, uint32_t color)
{
  uintptr_t base = s_fb_va;
  uint32_t rb = static_cast<uint32_t>(PE_state.video.v_rowBytes);
  uint32_t w = static_cast<uint32_t>(PE_state.video.v_width);
  uint32_t h = static_cast<uint32_t>(PE_state.video.v_height);
  if (!base || !rb || !w || !h || y0 >= h) {
    return;
  }
  if (y1 > h) y1 = h;
  for (uint32_t r = y0; r < y1; ++r) {
    volatile uint32_t *row = reinterpret_cast<volatile uint32_t *>(base + static_cast<uintptr_t>(r) * rb);
    for (uint32_t c = 0; c < w; ++c) {
      row[c] = color;
    }
  }
  __asm__ volatile("dsb sy" : : : "memory");
}

void fb_paint_dot(int32_t cx, int32_t cy, uint32_t color)
{
  uintptr_t base = s_fb_va;
  uint32_t rb = static_cast<uint32_t>(PE_state.video.v_rowBytes);
  int32_t w = static_cast<int32_t>(PE_state.video.v_width);
  int32_t h = static_cast<int32_t>(PE_state.video.v_height);
  if (!base || !rb || w <= 0 || h <= 0) {
    return;
  }
  int32_t y0 = (cy - 10 > h / 4) ? (cy - 10) : (h / 4);
  int32_t y1 = (cy + 10 < h) ? (cy + 10) : (h - 1);
  int32_t x0 = (cx - 10 >= 0) ? (cx - 10) : 0;
  int32_t x1 = (cx + 10 < w) ? (cx + 10) : (w - 1);
  for (int32_t r = y0; r <= y1; ++r) {
    volatile uint32_t *row = reinterpret_cast<volatile uint32_t *>(base + static_cast<uintptr_t>(r) * rb);
    for (int32_t c = x0; c <= x1; ++c) {
      row[c] = color;
    }
  }
  __asm__ volatile("dsb sy" : : : "memory");
}

void capture_points_loop(
    const sm6125_nvt_bus_t *bus,
    uintptr_t touch_pins_va,
    uint32_t duration_ms,
    TouchProbeState *s)
{
  uint16_t max_x = s->fw_info.abs_x_max ? s->fw_info.abs_x_max : 1080u;
  uint16_t max_y = s->fw_info.abs_y_max ? s->fw_info.abs_y_max : 2340u;

  /* Aguarda calibracao de baseline (RESET_STATE_REK 0xA1 -> 0xA2/0xA3, nt36xxx.c:2478) ate 400 ms */
  for (unsigned retry = 0; retry < 40u; ++retry) {
    uint8_t st[2] = {0};
    if (sm6125_nvt_read(bus, SM6125_NVT_EVENT_BUF_ADDR + SM6125_NVT_EVENT_RESET_COMPLETE, st, sizeof(st)) == 0) {
      if (st[0] >= SM6125_NVT_RESET_STATE_INIT && st[0] <= SM6125_NVT_RESET_STATE_MAX) {
        s->dl.reset_state[0] = st[0];
      }
      if (st[0] >= 0xA2u && st[0] <= SM6125_NVT_RESET_STATE_MAX) {
        break;
      }
    }
    delay_ms_spin(nullptr, 10u);
  }
  (void)sm6125_nvt_set_page(bus, SM6125_NVT_EVENT_BUF_ADDR);

  /* Faixa AMARELA (banda 2) avisa visualmente na tela: "TOQUE E DESLIZE AGORA!" */
  uint32_t h = static_cast<uint32_t>(PE_state.video.v_height);
  if (h >= 16u) {
    fb_paint_band(h / 8u, (h * 3u) / 16u, 0xFFFFFF00u);
  }

  /* Poll de GPIO 88 a cada 100 us (10 kHz) para nao perder pulsos curtos de IRQ_TYPE_EDGE_RISING */
  uint32_t ticks = duration_ms * 10u;
  if (!ticks) ticks = 1u;

  uint32_t prev_lvl = touch_mmio_read32(touch_pins_va, SM6125_GPIO_STRIDE + 4u) & 1u;
  uint32_t printed = 0;
  int prev_down = 0;
  int banner_green = 0;
  int have_any_pkt = 0;
  int32_t prev_x = -1, prev_y = -1;
  uint32_t quiet_after_touch_ticks = 0;

  for (uint32_t t = 0; t < ticks; ++t) {
    IODelay(100u);
    uint32_t lvl = touch_mmio_read32(touch_pins_va, SM6125_GPIO_STRIDE + 4u) & 1u;
    int rising = 0;
    if (lvl != prev_lvl) {
      if (lvl) {
        s->irq_rise++;
        rising = 1;
      } else {
        s->irq_fall++;
      }
      prev_lvl = lvl;
    }

    /* Leitura de 67 bytes somente em borda de subida do GPIO 88 (ou fallback suave a cada 25 ms = 250 ticks) */
    if (!rising && (t % 250u) != 0u) {
      if (s->contacts >= 12u && !prev_down) {
        quiet_after_touch_ticks++;
        if (quiet_after_touch_ticks >= 4000u) { /* 400 ms apos soltar o dedo com >= 12 amostras */
          break;
        }
      }
      continue;
    }

    uint8_t raw[SM6125_NVT_POINT_DATA_LEN];
    sm6125_nvt_points_t pts;
    int r = sm6125_nvt_read_points(bus, max_x, max_y, raw, &pts);
    s->pkt_reads++;
    if (!have_any_pkt) {
      memcpy(s->first_pkt, raw, sizeof(s->first_pkt));
      have_any_pkt = 1;
    }
    if (r || !pts.csum_ok || pts.recovery) {
      s->pkt_bad++;
      continue;
    }
    s->pkt_ok++;
    if (pts.count == 0u) {
      if (prev_down && printed < 24u) {
        printf("SM6125-TOUCH: UP (%d,%d)\n", prev_x, prev_y);
        printed++;
      }
      prev_down = 0;
      continue;
    }

    quiet_after_touch_ticks = 0;
    if (!banner_green && h >= 16u) {
      fb_paint_band(h / 8u, (h * 3u) / 16u, 0xFF00FF00u);
      banner_green = 1;
    }

    if (pts.count > s->max_fingers) {
      s->max_fingers = pts.count;
    }
    if (s->contacts == 0u) {
      memcpy(s->first_pkt, raw, sizeof(s->first_pkt));
      s->first_x = pts.fingers[0].x;
      s->first_y = pts.fingers[0].y;
    }
    for (uint32_t k = 0; k < pts.count; ++k) {
      int32_t x = pts.fingers[k].x;
      int32_t y = pts.fingers[k].y;
      if (s->min_x < 0 || x < s->min_x) s->min_x = x;
      if (x > s->max_x) s->max_x = x;
      if (s->min_y < 0 || y < s->min_y) s->min_y = y;
      if (y > s->max_y) s->max_y = y;
      fb_paint_dot(x, y, 0xFF00FF00u);
    }
    s->contacts += pts.count;
    s->last_x = pts.fingers[0].x;
    s->last_y = pts.fingers[0].y;

    int dx = prev_down ? (s->last_x - prev_x) : 999;
    int dy = prev_down ? (s->last_y - prev_y) : 999;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (!prev_down || dx >= 25 || dy >= 25) {
      if (printed < 24u) {
        printf("SM6125-TOUCH: n=%u id=%u (%d,%d) w=%u p=%u\n",
               pts.count, pts.fingers[0].id, s->last_x, s->last_y,
               pts.fingers[0].w, pts.fingers[0].p);
        printed++;
      }
      prev_x = s->last_x;
      prev_y = s->last_y;
    }
    prev_down = 1;
  }
}

void format_bytes(char *dst, size_t cap, const uint8_t *src, unsigned n)
{
  static const char hex[] = "0123456789abcdef";
  size_t pos = 0;
  for (unsigned i = 0; i < n && pos + 2u < cap; ++i) {
    dst[pos++] = hex[(src[i] >> 4) & 0xFu];
    dst[pos++] = hex[src[i] & 0xFu];
  }
  if (pos < cap) {
    dst[pos] = '\0';
  }
}

__attribute__((noreturn)) void report_and_panic(const TouchProbeState &s)
{
  char loop_hex[20], id_hex[16], alt_hex[16], pkt_hex[28];
  char xf_str[3][32];
  char m_hex[5][16];
  format_bytes(loop_hex, sizeof(loop_hex), s.loop_rx, 8u);
  format_bytes(id_hex, sizeof(id_hex), s.nvt.id, 6u);
  format_bytes(alt_hex, sizeof(alt_hex), s.nvt.alt_id, 6u);
  format_bytes(pkt_hex, sizeof(pkt_hex), s.first_pkt, 12u);

  for (unsigned i = 0; i < 3u; ++i) {
    if (i < s.trace_n) {
      char tx_h[10], rx_h[18];
      unsigned tn = s.trace_len[i] < 4u ? s.trace_len[i] : 4u;
      unsigned rn = s.trace_len[i] < 8u ? s.trace_len[i] : 8u;
      format_bytes(tx_h, sizeof(tx_h), s.trace_tx[i], tn);
      format_bytes(rx_h, sizeof(rx_h), s.trace_rx[i], rn);
      snprintf(xf_str[i], sizeof(xf_str[i]), "#%u/%s>%s", s.trace_len[i], tx_h, rx_h);
    } else {
      snprintf(xf_str[i], sizeof(xf_str[i]), "-");
    }
  }
  for (unsigned i = 0; i < 5u; ++i) {
    if (i < s.modes_tried) {
      format_bytes(m_hex[i], sizeof(m_hex[i]), s.mode_id[i], 6u);
    } else {
      snprintf(m_hex[i], sizeof(m_hex[i]), "-");
    }
  }

  static char s_panic_buf[1024];

  if (s.dl_attempted) {
    snprintf(
        s_panic_buf, sizeof(s_panic_buf),
        "SM6125-TOUCH-PROBE st=%u/%u step=%u err=%d fw=%u\n"
        "GENI p=%u ifd=%d L=%u d=%u a=%uK LOOP ok=%u\n"
        "NVT ok=%d %s md=%u ad=%x id=%s\n"
        "DL ok=%d try=%u st=%u rc=%d flg=%02x i=%08x d=%08x\n"
        "XF n=%u rc=%d iq=%08x gs=%08x tx=%08x rx=%08x\n"
        "FW v=%02x/%02x pid=%04x %ux%u ch=%ux%u rst=%02x\n"
        "TCH irq=%u/%u rd=%u ok=%u/%u cnt=%u mf=%u\n"
        "XY first=%d,%d last=%d,%d box=%d..%d,%d..%d\n"
        "PKT %s",
        s.stage_done, s.stage_req, s.step, s.err, sm6125_touch_fw_size,
        s.spi.info.protocol, s.spi.info.fifo_disabled,
        s.sel_level, s.sel_div, static_cast<unsigned>(s.sel_actual_hz / 1000ULL), s.loop_ok,
        s.nvt.matched, s.nvt.chip_name ? s.nvt.chip_name : "-", s.mode_win, s.nvt.addr_used, id_hex,
        s.dl.ok, s.dl.attempt, s.dl.stage, s.dl.last_rc, s.dl.crc_flags, s.dl.r_ilm_crc, s.dl.r_dlm_crc,
        s.last_err_len, s.last_err_rc, s.spi.snap_irq, s.spi.snap_status, s.spi.snap_txfs, s.spi.snap_rxfs,
        s.fw_info.fw_ver, s.fw_info.fw_ver_bar, s.fw_info.pid,
        s.fw_info.abs_x_max, s.fw_info.abs_y_max, s.fw_info.x_num, s.fw_info.y_num, s.dl.reset_state[0],
        s.irq_rise, s.irq_fall, s.pkt_reads, s.pkt_ok, s.pkt_bad, s.contacts, s.max_fingers,
        s.first_x, s.first_y, s.last_x, s.last_y, s.min_x, s.max_x, s.min_y, s.max_y,
        pkt_hex);
    panic("%s", s_panic_buf);
  }

  snprintf(
      s_panic_buf, sizeof(s_panic_buf),
      "SM6125-TOUCH-PROBE st=%u/%u step=%u err=%d\n"
      "GCC v0=%08x v1=%08x g0=%08x df=%08x c=%08x f=%08x\n"
      "CBCR m=%08x s=%08x c=%08x c2=%08x s2=%08x\n"
      "DFS %uM %uM %uM %uM | %uM %uM %uM %uM\n"
      "PIN0 %03x %03x %03x %03x r87=%03x/%x q88=%03x/%x\n"
      "PIN1 %03x %03x %03x %03x rst=%u\n"
      "GENI q=%08x fw=%08x p=%u ifd=%d hw0=%08x gs=%08x\n"
      "CLK L=%u d=%u s=%uM a=%uK LOOP rc=%d ok=%u %s\n"
      "NVT ok=%d %s md=%u ad=%x id=%s alt=%s\n"
      "XF %s %s %s\n"
      "M %u:%s %u:%s %u:%s %u:%s %u:%s",
      s.stage_done, s.stage_req, s.step, s.err,
      s.qup_vote_0, s.qup_vote_1, s.gpll0_mode, s.dfsr_cmd, s.rcg_cmd, s.rcg_cfg,
      s.cbcr_m_ahb, s.cbcr_s_ahb, s.cbcr_core, s.cbcr_core_2x, s.cbcr_s2,
      static_cast<unsigned>(s.dfs_hz[0] / 1000000ULL),
      static_cast<unsigned>(s.dfs_hz[1] / 1000000ULL),
      static_cast<unsigned>(s.dfs_hz[2] / 1000000ULL),
      static_cast<unsigned>(s.dfs_hz[3] / 1000000ULL),
      static_cast<unsigned>(s.dfs_hz[4] / 1000000ULL),
      static_cast<unsigned>(s.dfs_hz[5] / 1000000ULL),
      static_cast<unsigned>(s.dfs_hz[6] / 1000000ULL),
      static_cast<unsigned>(s.dfs_hz[7] / 1000000ULL),
      s.pin0_spi[0], s.pin0_spi[1], s.pin0_spi[2], s.pin0_spi[3],
      s.pin0_rst_ctl, s.pin0_rst_io, s.pin0_irq_ctl, s.pin0_irq_io,
      s.pin1_spi[0], s.pin1_spi[1], s.pin1_spi[2], s.pin1_spi[3], s.rst_applied,
      s.spi.info.qup_hw_version, s.spi.info.fw_revision, s.spi.info.protocol,
      s.spi.info.fifo_disabled, s.spi.info.hw_param0, s.spi.info.geni_status,
      s.sel_level, s.sel_div, static_cast<unsigned>(s.sel_sclk_hz / 1000000ULL),
      static_cast<unsigned>(s.sel_actual_hz / 1000ULL),
      s.loop_rc, s.loop_ok, loop_hex,
      s.nvt.matched, s.nvt.chip_name ? s.nvt.chip_name : "-", s.mode_win, s.nvt.addr_used, id_hex, alt_hex,
      xf_str[0], xf_str[1], xf_str[2],
      s.mode_val[0], m_hex[0], s.mode_val[1], m_hex[1], s.mode_val[2], m_hex[2],
      s.mode_val[3], m_hex[3], s.mode_val[4], m_hex[4]);
  panic("%s", s_panic_buf);
}

}  // namespace

static void sleep_ms_yield(void *, unsigned ms)
{
  IOSleep(ms ? ms : 1u);
}

static void publish_touch_points(const sm6125_nvt_points_t &pts)
{
  uint32_t cnt = pts.count;
  if (cnt > SM6125_NVT_MAX_FINGERS) cnt = SM6125_NVT_MAX_FINGERS;

  uint32_t seq = sm6125_touch_seq;
  sm6125_touch_seq = (seq + 1u) | 1u;
  __asm__ volatile("dmb ish" : : : "memory");
  sm6125_touch_count = cnt;
  for (uint32_t i = 0; i < cnt; ++i) {
    sm6125_touch_ids[i] = static_cast<int>(pts.fingers[i].id);
    sm6125_touch_xs[i] = static_cast<int>(pts.fingers[i].x);
    sm6125_touch_ys[i] = static_cast<int>(pts.fingers[i].y);
  }
  __asm__ volatile("dmb ish" : : : "memory");
  sm6125_touch_seq = (seq + 2u) & ~1u;
  __asm__ volatile("dmb ish" : : : "memory");
  sm6125_touch_notify_sink();
}

static volatile uint32_t s_hid_stage = 0;
static volatile int32_t s_hid_rc = 0;

static bool register_sm6125_hid_service(void)
{
  s_hid_stage = 10;
  for (unsigned i = 0; i < 200u; ++i) {
    if (IOService::getResourceService() != nullptr) {
      break;
    }
    IOSleep(50u);
  }
  IOService *parent = IOService::getResourceService();
  if (!parent) {
    s_hid_stage = 91;
    IOLog("Sm6125TouchHID: resource service unavailable\n");
    return false;
  }

  s_hid_stage = 11;
  kern_return_t ks = OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
  if (ks != KERN_SUCCESS) {
    s_hid_stage = 92;
    s_hid_rc = ks;
    IOLog("Sm6125TouchHID: IOHIDFamily load failed (%d)\n", ks);
    return false;
  }
  (void)OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDEventDriver");

  s_hid_stage = 12;
  void *meta_handle = OSMetaClass::preModLoad("com.apple.iokit.IOHIDFamily");
  if (!meta_handle) {
    s_hid_stage = 93;
    IOLog("Sm6125TouchHID: preModLoad failed\n");
    return false;
  }
  __GLOBAL__sub_I_Sm6125HidInput_cpp();
  OSReturn post_rc = OSMetaClass::postModLoad(meta_handle);
  if (post_rc != kOSReturnSuccess) {
    s_hid_stage = 94;
    s_hid_rc = post_rc;
    IOLog("Sm6125TouchHID: postModLoad failed (%d)\n", post_rc);
    return false;
  }

  s_hid_stage = 13;
  IOService *nub = new IOService;
  IOService *hid = sm6125_hid_input_allocate();
  if (!nub || !hid || !nub->init() || !hid->init()) {
    OSSafeReleaseNULL(hid);
    OSSafeReleaseNULL(nub);
    s_hid_stage = 95;
    IOLog("Sm6125TouchHID: allocate/init failed\n");
    return false;
  }

  nub->setName("Sm6125TouchSPI");
  if (!nub->attach(parent) || !nub->start(parent)) {
    nub->detach(parent);
    OSSafeReleaseNULL(hid);
    OSSafeReleaseNULL(nub);
    s_hid_stage = 96;
    IOLog("Sm6125TouchHID: nub start failed\n");
    return false;
  }

  if (!hid->attach(nub) || !hid->start(nub)) {
    hid->detach(nub);
    nub->stop(parent);
    nub->detach(parent);
    OSSafeReleaseNULL(hid);
    OSSafeReleaseNULL(nub);
    s_hid_stage = 97;
    IOLog("Sm6125TouchHID: hid start failed\n");
    return false;
  }

  hid->registerService();
  s_hid_stage = 20;
  IOLog("Sm6125TouchHID: registered Sm6125HidInput service=%p\n", hid);
  return true;
}

static void sm6125_touch_hid_thread(void *, wait_result_t)
{
  uint32_t diag_secs = 0;
  PE_parse_boot_argn("sm6125-touch-diag", &diag_secs, sizeof(diag_secs));

  if (sm6125_touch_fw_size == 0u) {
    s_hid_stage = 1;
    IOLog("Sm6125TouchHID: no firmware embedded\n");
    return;
  }

  TouchProbeState s;
  memset(&s, 0, sizeof(s));
  s.stage_req = 5u;

  MappedWindow gpll0, gpll6, qup0, vote, spi_pins, touch_pins, wrapper, se;
  if (!map_window(SM6125_GCC_BASE + 0x0000u, 0x1000u, &gpll0) ||
      !map_window(SM6125_GCC_BASE + 0x6000u, 0x1000u, &gpll6) ||
      !map_window(SM6125_GCC_BASE + 0x1f000u, 0x1000u, &qup0) ||
      !map_window(SM6125_GCC_BASE + 0x79000u, 0x1000u, &vote) ||
      !map_window(SM6125_TLMM_WEST_BASE + sm6125_tlmm_ctl(SM6125_GPIO_SPI_MISO), 4u * SM6125_GPIO_STRIDE, &spi_pins) ||
      !map_window(SM6125_TLMM_WEST_BASE + sm6125_tlmm_ctl(SM6125_GPIO_TOUCH_RST), 2u * SM6125_GPIO_STRIDE, &touch_pins) ||
      !map_window(SM6125_QUP0_WRAPPER_BASE, SM6125_QUP0_WRAPPER_SIZE, &wrapper) ||
      !map_window(SM6125_QUP0_SE2_BASE, SM6125_QUP0_SE2_SIZE, &se)) {
    s_hid_stage = 2;
    IOLog("Sm6125TouchHID: map_window failed\n");
    return;
  }

  take_snapshot(gpll0.va, gpll6.va, qup0.va, vote.va, spi_pins.va, touch_pins.va, &s);
  if (!enable_clocks_and_pins(gpll0.va, gpll6.va, qup0.va, vote.va, spi_pins.va, touch_pins.va, 0u, &s)) {
    s_hid_stage = 3;
    IOLog("Sm6125TouchHID: clocks/pins failed\n");
    return;
  }

  s.spi.wrapper = (struct geni_io){reinterpret_cast<void *>(wrapper.va), io_rd, io_wr, io_delay_us};
  s.spi.se = (struct geni_io){reinterpret_cast<void *>(se.va), io_rd, io_wr, io_delay_us};
  s.spi.poll_only = 1;
  s.spi.poll_limit = 200000u;

  if (geni_spi_init(&s.spi) != 0 || !configure_spi_clock(&s, SM6125_SPI_DEFAULT_HZ)) {
    s_hid_stage = 4;
    IOLog("Sm6125TouchHID: GENI SPI init failed\n");
    return;
  }

  const sm6125_nvt_bus_t bus = {&s, traced_xfer, sleep_ms_yield};
  geni_spi_setup(&s.spi, 0u, 0u);
  if (sm6125_nvt_identify(&bus, &s.nvt) != 0) {
    s_hid_stage = 5;
    IOLog("Sm6125TouchHID: NT36672A identify failed\n");
    return;
  }

  (void)configure_spi_clock(&s, SM6125_SPI_MAX_HZ);
  sm6125_nvt_fw_t fw;
  if (sm6125_nvt_fw_parse(sm6125_touch_fw_data, sm6125_touch_fw_size, &fw) != 0 ||
      sm6125_nvt_download_firmware(&bus, &fw, s_nvt_scratch, &s.dl) != 0 ||
      sm6125_nvt_get_fw_info(&bus, &s.fw_info) != 0) {
    s_hid_stage = 6;
    IOLog("Sm6125TouchHID: firmware load failed\n");
    return;
  }

  for (unsigned retry = 0; retry < 40u; ++retry) {
    uint8_t st[2] = {0};
    if (sm6125_nvt_read(&bus, SM6125_NVT_EVENT_BUF_ADDR + SM6125_NVT_EVENT_RESET_COMPLETE, st, sizeof(st)) == 0 &&
        st[0] >= 0xA2u && st[0] <= SM6125_NVT_RESET_STATE_MAX) {
      break;
    }
    IOSleep(10u);
  }
  (void)sm6125_nvt_set_page(&bus, SM6125_NVT_EVENT_BUF_ADDR);

  uint16_t max_x = s.fw_info.abs_x_max ? s.fw_info.abs_x_max : 1080u;
  uint16_t max_y = s.fw_info.abs_y_max ? s.fw_info.abs_y_max : 2340u;
  sm6125_touch_max_x = max_x;
  sm6125_touch_max_y = max_y;

  if (!register_sm6125_hid_service()) {
    if (diag_secs) {
      IOSleep(diag_secs * 1000u);
      panic("SM6125-HID-DIAG st=%u rc=%d hid=%u/%u/%u/%x",
            s_hid_stage, s_hid_rc,
            sm6125_hid_diag[0], sm6125_hid_diag[1], sm6125_hid_diag[2], sm6125_hid_diag[3]);
    }
    return;
  }

  IOLog("Sm6125TouchHID: ready %ux%u fw=0x%02x pid=0x%04x\n",
        max_x, max_y, s.fw_info.fw_ver, s.fw_info.pid);

  MappedWindow spmi_core, spmi_obsrvr;
  sm6125_spmi_buttons_t btns;
  memset(&btns, 0, sizeof(btns));
  bool btns_ok = false;
  if (map_window(SM6125_SPMI_CORE_BASE, SM6125_SPMI_CORE_SIZE, &spmi_core) &&
      map_window(SM6125_SPMI_OBSRVR_BASE, SM6125_SPMI_OBSRVR_SIZE, &spmi_obsrvr)) {
    btns.ctx = nullptr;
    btns.rd32 = [](void *, uintptr_t base, uint32_t off) -> uint32_t { return touch_mmio_read32(base, off); };
    btns.wr32 = [](void *, uintptr_t base, uint32_t off, uint32_t val) { touch_mmio_write32(base, off, val); };
    btns.delay_us = [](void *, uint32_t us) { IODelay(us); };
    btns.core_va = spmi_core.va;
    btns.obsrvr_va = spmi_obsrvr.va;
    btns.ee = 0u;
    if (sm6125_spmi_buttons_init(&btns) == 0) {
      btns_ok = true;
      IOLog("Sm6125TouchHID: SPMI buttons ready (pon_apid=%d volup_apid=%d)\n",
            btns.apid_pon, btns.apid_volup);
    }
  }
  uint32_t btn_ticks = 0u;
  uint32_t prev_btn_mask = 0u;

  uint32_t prev_lvl = touch_mmio_read32(touch_pins.va, SM6125_GPIO_STRIDE + 4u) & 1u;
  uint32_t prev_down_cnt = 0u;
  uint32_t idle_ticks = 0u;
  uint32_t rec_streak = 0u;
  uint32_t total_ticks = 0u;
  uint32_t rd_ok = 0u, rd_bad = 0u, rd_rec = 0u, contacts = 0u;
  uint8_t bad_pkt[8] = {};
  int have_bad = 0;

  for (;;) {
    IOSleep(1u);
    total_ticks++;
    if (btns_ok && (++btn_ticks >= 20u)) {
      btn_ticks = 0u;
      uint32_t btn_mask = 0u;
      if (sm6125_spmi_buttons_poll(&btns, &btn_mask) == 0 && btn_mask != prev_btn_mask) {
        prev_btn_mask = btn_mask;
        sm6125_buttons_notify_sink(btn_mask);
      }
    }
    if (diag_secs && total_ticks >= diag_secs * 1000u) {
      panic("SM6125-HID-DIAG st=%u rc=%d hid=%u/%u/%u/%x ed=%u/%u op=%x pu=%x rep=%u irq=%u rd=%u ok=%u bad=%u rec=%u cnt=%u xy=%d,%d",
            s_hid_stage, s_hid_rc,
            sm6125_hid_diag[0], sm6125_hid_diag[1], sm6125_hid_diag[2], sm6125_hid_diag[3],
            sm6125_hid_diag[4], sm6125_hid_diag[5], sm6125_hid_diag[6], sm6125_hid_diag[7],
            sm6125_touch_hid_reports, s.irq_rise, s.pkt_reads, rd_ok, rd_bad, rd_rec, contacts,
            s.last_x, s.last_y);
    }

    uint32_t lvl = touch_mmio_read32(touch_pins.va, SM6125_GPIO_STRIDE + 4u) & 1u;
    bool rising = (lvl != prev_lvl) && (lvl != 0u);
    prev_lvl = lvl;
    if (rising) s.irq_rise++;

    if (!rising) {
      idle_ticks++;
      if (!prev_down_cnt || idle_ticks < 200u) {
        continue;
      }
    }
    idle_ticks = 0u;

    uint8_t raw[SM6125_NVT_POINT_DATA_LEN];
    sm6125_nvt_points_t pts;
    int r = sm6125_nvt_read_points_fast(&bus, max_x, max_y, raw, &pts);
    s.pkt_reads++;
    if (r != 0) {
      rd_bad++;
      continue;
    }
    if (pts.recovery) {
      rd_rec++;
      rec_streak++;
      if (!have_bad) { memcpy(bad_pkt, raw, sizeof(bad_pkt)); have_bad = 1; }
      if (rec_streak >= 10u) {
        rec_streak = 0u;
        if (sm6125_nvt_download_firmware(&bus, &fw, s_nvt_scratch, &s.dl) == 0) {
          (void)sm6125_nvt_set_page(&bus, SM6125_NVT_EVENT_BUF_ADDR);
        }
      }
      continue;
    }
    rec_streak = 0u;
    if (!pts.csum_ok) {
      rd_bad++;
      if (!have_bad) { memcpy(bad_pkt, raw, sizeof(bad_pkt)); have_bad = 1; }
      continue;
    }
    rd_ok++;
    if (pts.count > 0u) {
      contacts += pts.count;
      s.last_x = pts.fingers[0].x;
      s.last_y = pts.fingers[0].y;
    }
    if (pts.count > 0u || prev_down_cnt > 0u) {
      publish_touch_points(pts);
      prev_down_cnt = pts.count;
    }
  }
}

extern "C" void sm6125_touch_probe_run(void)
{
  uint32_t stage_req = 0;
  if (!PE_parse_boot_argn("sm6125-touch", &stage_req, sizeof(stage_req)) || stage_req == 0u) {
    return;
  }

  if (stage_req == 5u) {
    thread_t th = THREAD_NULL;
    if (kernel_thread_start(sm6125_touch_hid_thread, nullptr, &th) == KERN_SUCCESS && th != THREAD_NULL) {
      thread_deallocate(th);
    }
    return;
  }

  uint32_t target_hz = SM6125_SPI_DEFAULT_HZ;
  uint32_t dl_hz = SM6125_SPI_MAX_HZ;
  uint32_t do_reset = 0u;
  uint32_t forced_mode = 0xFFu;
  uint32_t duration_ms = 20000u;
  if (PE_parse_boot_argn("sm6125-touch-hz", &target_hz, sizeof(target_hz))) {
    if (target_hz < SM6125_SPI_MIN_HZ || target_hz > SM6125_SPI_MAX_HZ) {
      target_hz = SM6125_SPI_DEFAULT_HZ;
    }
    dl_hz = target_hz;
  }
  PE_parse_boot_argn("sm6125-touch-rst", &do_reset, sizeof(do_reset));
  PE_parse_boot_argn("sm6125-touch-mode", &forced_mode, sizeof(forced_mode));
  PE_parse_boot_argn("sm6125-touch-ms", &duration_ms, sizeof(duration_ms));
  if (duration_ms > 30000u) duration_ms = 30000u;

  TouchProbeState s;
  memset(&s, 0, sizeof(s));
  s.stage_req = stage_req;
  s.loop_rc = 99;
  s.nvt.chip_name = "-";
  s.first_x = s.first_y = s.last_x = s.last_y = -1;
  s.min_x = s.max_x = s.min_y = s.max_y = -1;

  MappedWindow gpll0, gpll6, qup0, vote, spi_pins, touch_pins, wrapper, se, fb;
  s_fb_va = 0;
  if (PE_state.video.v_baseAddr && PE_state.video.v_rowBytes && PE_state.video.v_height) {
    if (PE_state.video.v_baseAddr >= 0xFFFF000000000000ULL) {
      s_fb_va = static_cast<uintptr_t>(PE_state.video.v_baseAddr);
    } else {
      uint64_t fb_sz = static_cast<uint64_t>(PE_state.video.v_rowBytes) * PE_state.video.v_height;
      if (map_window(PE_state.video.v_baseAddr, fb_sz, &fb)) {
        s_fb_va = fb.va;
      }
    }
  }
  bool mapped_base =
      map_window(SM6125_GCC_BASE + 0x0000u, 0x1000u, &gpll0) &&
      map_window(SM6125_GCC_BASE + 0x6000u, 0x1000u, &gpll6) &&
      map_window(SM6125_GCC_BASE + 0x1f000u, 0x1000u, &qup0) &&
      map_window(SM6125_GCC_BASE + 0x79000u, 0x1000u, &vote) &&
      map_window(SM6125_TLMM_WEST_BASE + sm6125_tlmm_ctl(SM6125_GPIO_SPI_MISO), 4u * SM6125_GPIO_STRIDE, &spi_pins) &&
      map_window(SM6125_TLMM_WEST_BASE + sm6125_tlmm_ctl(SM6125_GPIO_TOUCH_RST), 2u * SM6125_GPIO_STRIDE, &touch_pins);
  if (!mapped_base) {
    s.step = 1u;
    s.err = -12;
    goto out;
  }

  take_snapshot(gpll0.va, gpll6.va, qup0.va, vote.va, spi_pins.va, touch_pins.va, &s);
  if (stage_req == 1u) {
    goto out;
  }

  if (!enable_clocks_and_pins(gpll0.va, gpll6.va, qup0.va, vote.va, spi_pins.va, touch_pins.va, do_reset, &s)) {
    goto out;
  }

  s.step = 3u;
  if (!map_window(SM6125_QUP0_WRAPPER_BASE, SM6125_QUP0_WRAPPER_SIZE, &wrapper) ||
      !map_window(SM6125_QUP0_SE2_BASE, SM6125_QUP0_SE2_SIZE, &se)) {
    s.err = -12;
    goto out;
  }

  s.spi.wrapper = (struct geni_io){reinterpret_cast<void *>(wrapper.va), io_rd, io_wr, io_delay_us};
  s.spi.se = (struct geni_io){reinterpret_cast<void *>(se.va), io_rd, io_wr, io_delay_us};
  s.spi.poll_only = 1;
  s.spi.poll_limit = 200000u;

  {
    int r = geni_spi_read_info(&s.spi);
    if (r) {
      s.err = r;
      goto out;
    }
    s.stage_done = 2u;
    if (s.spi.info.protocol != GENI_PROTO_SPI) {
      s.err = GENI_ERR_PROTO;
      goto out;
    }
    if (s.spi.info.fifo_disabled) {
      s.err = GENI_ERR_NOTSUP;
      goto out;
    }
    if (stage_req == 2u) {
      goto out;
    }

    s.step = 4u;
    r = geni_spi_init(&s.spi);
    if (r) {
      s.err = r;
      goto out;
    }

    s.step = 5u;
    if (!configure_spi_clock(&s, target_hz)) {
      s.err = -22;
      goto out;
    }

    s.step = 6u;
    uint8_t loop_tx[128] = {};
    uint8_t loop_rx[128] = {};
    for (unsigned k = 0; k < 128u; ++k) {
      loop_tx[k] = (k < 15u) ? static_cast<uint8_t>((k + 1u) * 0x11u)
                             : static_cast<uint8_t>((k * 0x1Du + 0x5Au) & 0xFFu);
    }
    geni_spi_setup(&s.spi, GENI_SPI_LOOP, 0u);
    s.loop_rc = sm6125_geni_fifo_transfer(&s.spi, loop_tx, loop_rx, sizeof(loop_tx), 0);
    for (unsigned k = 0; k < 8u; ++k) {
      s.loop_rx[k] = loop_rx[k];
    }
    if (s.loop_rc == 0) {
      for (unsigned k = 0; k < 128u; ++k) {
        if (loop_rx[k] == loop_tx[k]) {
          s.loop_ok++;
        }
      }
    }
    geni_spi_setup(&s.spi, 0u, 0u);
    if (s.loop_rc != 0 || s.loop_ok != 128u) {
      s.last_err_len = sizeof(loop_tx);
      s.last_err_rc = s.loop_rc ? s.loop_rc : -5;
      s.err = s.last_err_rc;
      goto out;
    }

    s.step = 7u;
    static const uint32_t kModeOrder[5] = {0u, 3u, 1u, 2u, 4u};
    const uint32_t count = (forced_mode <= 7u) ? 1u : 5u;
    const sm6125_nvt_bus_t bus = {&s, traced_xfer, delay_ms_spin};

    for (uint32_t i = 0; i < count; ++i) {
      uint32_t m = (forced_mode <= 7u) ? forced_mode : kModeOrder[i];
      uint32_t geni_flags = ((m & 1u) ? GENI_SPI_CPHA : 0u) |
                            ((m & 2u) ? GENI_SPI_CPOL : 0u) |
                            ((m & 4u) ? GENI_SPI_CS_HIGH : 0u);
      geni_spi_setup(&s.spi, geni_flags, 0u);
      s.trace_capture = (i == 0u) ? 1u : 0u;

      sm6125_nvt_ident_result_t res;
      int id_rc = sm6125_nvt_identify(&bus, &res);
      s.modes_tried = i + 1u;
      s.mode_val[i] = m;
      s.mode_rc[i] = id_rc;
      memcpy(s.mode_id[i], res.id, 6u);
      s.nvt = res;
      if (id_rc == 0) {
        s.mode_win = m;
        s.stage_done = 3u;
        s.err = 0;
        break;
      }
    }
    if (!s.nvt.matched) {
      s.err = SM6125_NVT_ERR_CHIP;
      goto out;
    }
    if (stage_req < 4u) {
      goto out;
    }

    s.step = 8u;
    s.dl_attempted = 1u;
    s.trace_capture = 0u;
    if (sm6125_touch_fw_size == 0u) {
      s.err = SM6125_NVT_ERR_FW;
      goto out;
    }
    (void)configure_spi_clock(&s, dl_hz);

    sm6125_nvt_fw_t fw;
    r = sm6125_nvt_fw_parse(sm6125_touch_fw_data, sm6125_touch_fw_size, &fw);
    if (r) {
      s.err = r;
      goto out;
    }
    r = sm6125_nvt_download_firmware(&bus, &fw, s_nvt_scratch, &s.dl);
    if (r) {
      s.err = r;
      goto out;
    }

    s.step = 9u;
    r = sm6125_nvt_get_fw_info(&bus, &s.fw_info);
    if (r) {
      s.err = r;
      goto out;
    }

    printf("SM6125-TOUCH: FW OK (%s) v=%02x pid=%04x %ux%u (%ux%u) - TOQUE NA TELA AGORA (%us)\n",
           sm6125_touch_fw_name, s.fw_info.fw_ver, s.fw_info.pid,
           s.fw_info.abs_x_max, s.fw_info.abs_y_max,
           s.fw_info.x_num, s.fw_info.y_num, duration_ms / 1000u);

    s.step = 10u;
    capture_points_loop(&bus, touch_pins.va, duration_ms, &s);
    s.stage_done = 4u;
    s.err = 0;
  }

out:
  s_fb_va = 0;
  unmap_window(&fb);
  unmap_window(&se);
  unmap_window(&wrapper);
  unmap_window(&touch_pins);
  unmap_window(&spi_pins);
  unmap_window(&vote);
  unmap_window(&qup0);
  unmap_window(&gpll6);
  unmap_window(&gpll0);
  report_and_panic(s);
}
