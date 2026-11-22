// SPDX-License-Identifier: MIT
// Sonda em estagios (desligada por padrao) para QUPv3 SE2 SPI + Novatek NT36672A no SM6125 (ginkgo).
#include "Sm6125NvtProbe.h"
#include "Sm6125TouchBoard.h"
extern "C" {
#include "GeniSpi.h"
}
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <string.h>

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
};

int traced_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
  auto *s = static_cast<TouchProbeState *>(ctx);
  int rc = geni_spi_transfer(&s->spi, tx, rx, len, 0);
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
  char loop_hex[20], id_hex[16], old_hex[16];
  char xf_str[3][32];
  char m_hex[5][16];
  format_bytes(loop_hex, sizeof(loop_hex), s.loop_rx, 8u);
  format_bytes(id_hex, sizeof(id_hex), s.nvt.id, 6u);
  format_bytes(old_hex, sizeof(old_hex), s.nvt.alt_id, 6u);

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

  panic(
      "SM6125-TOUCH-PROBE st=%u/%u step=%u err=%d\n"
      "GCC v0=%08x v1=%08x g0=%08x df=%08x c=%08x f=%08x\n"
      "CBCR m=%08x s=%08x c=%08x c2=%08x s2=%08x\n"
      "DFS %uM %uM %uM %uM | %uM %uM %uM %uM\n"
      "PIN0 %03x %03x %03x %03x r87=%03x/%x q88=%03x/%x\n"
      "PIN1 %03x %03x %03x %03x rst=%u\n"
      "GENI q=%08x fw=%08x p=%u ifd=%d hw0=%08x gs=%08x\n"
      "CLK L=%u d=%u s=%uM a=%uK LOOP rc=%d ok=%u %s\n"
      "NVT ok=%d %s md=%u ad=%x id=%s old=%s\n"
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
      s.nvt.matched, s.nvt.chip_name ? s.nvt.chip_name : "-", s.mode_win, s.nvt.addr_used, id_hex, old_hex,
      xf_str[0], xf_str[1], xf_str[2],
      s.mode_val[0], m_hex[0], s.mode_val[1], m_hex[1], s.mode_val[2], m_hex[2],
      s.mode_val[3], m_hex[3], s.mode_val[4], m_hex[4]);
}

}  // namespace

extern "C" void sm6125_touch_probe_run(void)
{
  uint32_t stage_req = 0;
  if (!PE_parse_boot_argn("sm6125-touch", &stage_req, sizeof(stage_req)) || stage_req == 0u) {
    return;
  }

  uint32_t target_hz = SM6125_SPI_DEFAULT_HZ;
  uint32_t do_reset = 0u;
  uint32_t forced_mode = 0xFFu;
  PE_parse_boot_argn("sm6125-touch-hz", &target_hz, sizeof(target_hz));
  if (target_hz < SM6125_SPI_MIN_HZ || target_hz > SM6125_SPI_MAX_HZ) {
    target_hz = SM6125_SPI_DEFAULT_HZ;
  }
  PE_parse_boot_argn("sm6125-touch-rst", &do_reset, sizeof(do_reset));
  PE_parse_boot_argn("sm6125-touch-mode", &forced_mode, sizeof(forced_mode));

  TouchProbeState s;
  memset(&s, 0, sizeof(s));
  s.stage_req = stage_req;
  s.loop_rc = 99;
  s.nvt.chip_name = "-";

  MappedWindow gpll0, gpll6, qup0, vote, spi_pins, touch_pins, wrapper, se;
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
    struct sm6125_clock_choice choice = {};
    if (sm6125_choose_clock(s.dfs_hz, s.num_levels, s.spi.oversampling, target_hz, &choice)) {
      s.err = -22;
      goto out;
    }
    s.sel_level = choice.level;
    s.sel_div = choice.div;
    s.sel_sclk_hz = choice.sclk_hz;
    s.sel_actual_hz = choice.actual_hz;

    r = geni_spi_set_clock(&s.spi, target_hz, static_cast<unsigned>(choice.sclk_hz), choice.level);
    if (r) {
      s.err = r;
      goto out;
    }

    s.step = 6u;
    static const uint8_t kLoopPat[16] = {
      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
      0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
    };
    uint8_t loop_rx[16] = {};
    geni_spi_setup(&s.spi, GENI_SPI_LOOP, 0u);
    s.loop_rc = geni_spi_transfer(&s.spi, kLoopPat, loop_rx, sizeof(kLoopPat), 0);
    for (unsigned k = 0; k < 8u; ++k) {
      s.loop_rx[k] = loop_rx[k];
    }
    if (s.loop_rc == 0) {
      for (unsigned k = 0; k < 16u; ++k) {
        if (loop_rx[k] == kLoopPat[k]) {
          s.loop_ok++;
        }
      }
    }
    geni_spi_setup(&s.spi, 0u, 0u);
    if (s.loop_rc != 0 || s.loop_ok != 16u) {
      s.err = s.loop_rc ? s.loop_rc : -5;
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
        goto out;
      }
    }
    s.err = SM6125_NVT_ERR_CHIP;
  }

out:
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
