// SPDX-License-Identifier: MIT
// Constantes de hardware do controlador QUPv3 Wrap0 SE2 SPI e TLMM WEST no Qualcomm SM6125 (ginkgo).
// Fontes: backup-ginkgo/stock-fdt.dts, Linux mainline sm6125.dtsi / gcc-sm6125.c / pinctrl-sm6125.c,
// e uefi-ginkgo MemoryMapLib.c. Somente helpers puros sem MMIO; testável em host.
#ifndef SM6125_TOUCH_BOARD_H
#define SM6125_TOUCH_BOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* QUPv3 Wrapper 0 e Serial Engine 2 (spi@4a88000) */
#define SM6125_QUP0_WRAPPER_BASE 0x04ac0000ULL
#define SM6125_QUP0_WRAPPER_SIZE 0x00002000ULL
#define SM6125_QUP0_SE2_BASE     0x04a88000ULL
#define SM6125_QUP0_SE2_SIZE     0x00004000ULL

/* GCC base 0x01400000 (linux/drivers/clk/qcom/gcc-sm6125.c) */
#define SM6125_GCC_BASE          0x01400000ULL
#define SM6125_GCC_GPLL0_MODE    0x00000u
#define SM6125_GCC_GPLL6_MODE    0x06000u
#define SM6125_GCC_PLL_LOCK_DET  (1u << 31)

#define SM6125_GCC_GPLL_VOTE     0x79000u
#define SM6125_VOTE_GPLL0        (1u << 0)
#define SM6125_VOTE_GPLL6        (1u << 6)

#define SM6125_GCC_QUP0_VOTE     0x7900cu
#define SM6125_VOTE_QUP0_M_AHB   (1u << 6)   /* halt_reg = 0x1f004 */
#define SM6125_VOTE_QUP0_S_AHB   (1u << 7)   /* halt_reg = 0x1f008, hwcg_bit = 1 */
#define SM6125_VOTE_QUP0_CORE    (1u << 8)   /* halt_reg = 0x1f00c */
#define SM6125_VOTE_QUP0_CORE_2X (1u << 9)   /* halt_reg = 0x1f014 */
#define SM6125_VOTE_QUP0_S2      (1u << 12)  /* halt_reg = 0x1f3a4 */
#define SM6125_VOTE_QUP0_SE2_ALL \
  (SM6125_VOTE_QUP0_M_AHB | SM6125_VOTE_QUP0_S_AHB | \
   SM6125_VOTE_QUP0_CORE | SM6125_VOTE_QUP0_CORE_2X | SM6125_VOTE_QUP0_S2)

#define SM6125_GCC_QUP0_M_AHB_CBCR   0x1f004u
#define SM6125_GCC_QUP0_S_AHB_CBCR   0x1f008u
#define SM6125_GCC_QUP0_CORE_CBCR    0x1f00cu
#define SM6125_GCC_QUP0_CORE_2X_CBCR 0x1f014u
#define SM6125_GCC_QUP0_S2_CBCR      0x1f3a4u
#define SM6125_CBCR_CLK_OFF          (1u << 31)
#define SM6125_CBCR_HW_CTL           (1u << 1)

/* gcc_qupv3_wrap0_s2_clk_src: cmd_rcgr = 0x1f3a8, mnd_width = 16, hid_width = 5 */
#define SM6125_S2_RCG            0x1f3a8u
#define SM6125_RCG_CMD           0x00u
#define SM6125_RCG_CFG           0x04u
#define SM6125_RCG_M             0x08u
#define SM6125_RCG_N             0x0cu
#define SM6125_RCG_D             0x10u
#define SM6125_RCG_DFSR_CMD      0x14u
#define SM6125_RCG_PERF_CFG(l)   (0x1cu + 4u * (l))
#define SM6125_RCG_PERF_M(l)     (0x5cu + 4u * (l))
#define SM6125_RCG_PERF_N(l)     (0x9cu + 4u * (l))
#define SM6125_DFS_LEVELS        8u
#define SM6125_RCG_CMD_UPDATE    (1u << 0)
#define SM6125_RCG_CMD_ROOT_OFF  (1u << 31)
#define SM6125_DFSR_DFS_EN       (1u << 0)

/* TLMM WEST tile (linux/drivers/pinctrl/qcom/pinctrl-sm6125.c + sm6125.dtsi) */
#define SM6125_TLMM_WEST_BASE    0x00500000ULL
#define SM6125_GPIO_STRIDE       0x00001000u
#define SM6125_GPIO_SPI_MISO     6u
#define SM6125_GPIO_SPI_MOSI     7u
#define SM6125_GPIO_SPI_CLK      8u
#define SM6125_GPIO_SPI_CS       9u
#define SM6125_GPIO_TOUCH_RST    87u
#define SM6125_GPIO_TOUCH_IRQ    88u

#define SM6125_FUNC_GPIO         0u
#define SM6125_FUNC_QUP02        1u
#define SM6125_PULL_NONE         0u
#define SM6125_PULL_DOWN         1u
#define SM6125_PULL_UP           3u
#define SM6125_DRIVE_2MA         0u
#define SM6125_DRIVE_6MA         2u
#define SM6125_DRIVE_8MA         3u

#define SM6125_SPI_DEFAULT_HZ    1000000u
#define SM6125_SPI_MIN_HZ        100000u
#define SM6125_SPI_MAX_HZ        8000000u  /* stock-fdt.dts spi-max-frequency = <0x7a1200> */

static inline uint32_t sm6125_tlmm_ctl(unsigned gpio) {
  return (uint32_t)gpio * SM6125_GPIO_STRIDE;
}

static inline uint32_t sm6125_tlmm_io(unsigned gpio) {
  return (uint32_t)gpio * SM6125_GPIO_STRIDE + 4u;
}

/* Bits pinctrl-sm6125: pull[1:0], mux[5:2], drv[8:6], oe[9]. */
static inline uint32_t sm6125_tlmm_pin_cfg(unsigned func, unsigned drv, unsigned pull, unsigned oe) {
  return (pull & 3u) | ((func & 15u) << 2) | ((drv & 7u) << 6) | ((oe & 1u) << 9);
}

static inline int sm6125_cbcr_running(uint32_t cbcr, int has_hwcg) {
  if ((cbcr & SM6125_CBCR_CLK_OFF) == 0u) {
    return 1;
  }
  return has_hwcg && ((cbcr & SM6125_CBCR_HW_CTL) != 0u);
}

/* gcc_parent_map_1 em gcc-sm6125.c:
 * 0 = bi_tcxo (19.2 MHz), 1 = gpll0_out_early (600 MHz), 2 = gpll0_out_aux2 (300 MHz), 4 = gpll6_out_main (384 MHz).
 */
static inline uint64_t sm6125_gcc_src_hz(unsigned sel) {
  switch (sel) {
  case 0u: return 19200000ULL;
  case 1u: return 600000000ULL;
  case 2u: return 300000000ULL;
  case 4u: return 384000000ULL;
  default: return 0ULL;
  }
}

static inline uint64_t sm6125_rcg_calc_rate(uint64_t prate, uint32_t m, uint32_t n, unsigned mode, unsigned hid) {
  if (!prate) {
    return 0ULL;
  }
  if (hid) {
    prate *= 2ULL;
    prate /= (uint64_t)(hid + 1u);
  }
  if (mode && n) {
    prate *= (uint64_t)m;
    prate /= (uint64_t)n;
  }
  return prate;
}

/* Fórmula clk-rcg2 para DFS/RCG2 de 16 bits MND e 5 bits HID: src=[10:8], mode=[13:12], hid=[4:0],
 * n = ((~nreg) & 0xffff) + m.
 */
static inline uint64_t sm6125_dfs_level_hz(uint32_t cfg, uint32_t mreg, uint32_t nreg) {
  unsigned src = (cfg >> 8) & 7u;
  unsigned mode = (cfg >> 12) & 3u;
  unsigned hid = cfg & 0x1fu;
  uint32_t m = 0u, n = 0u;
  if (mode) {
    m = mreg & 0xffffu;
    n = ((~nreg) & 0xffffu) + m;
  }
  return sm6125_rcg_calc_rate(sm6125_gcc_src_hz(src), m, n, mode, hid);
}

struct sm6125_clock_choice {
  unsigned level;
  unsigned div;
  uint64_t sclk_hz;
  uint64_t actual_hz;
};

static inline int sm6125_choose_clock(
    const uint64_t *level_hz,
    unsigned count,
    unsigned os,
    unsigned target_hz,
    struct sm6125_clock_choice *out) {
  int found = 0;
  if (!level_hz || !out || !os || !target_hz) {
    return -1;
  }
  for (unsigned l = 0; l < count; ++l) {
    uint64_t f = level_hz[l];
    if (!f) {
      continue;
    }
    uint64_t denom = (uint64_t)os * (uint64_t)target_hz;
    uint64_t d = (f + denom - 1ULL) / denom;
    if (d < 1ULL) {
      d = 1ULL;
    }
    if (d > 0xfffULL) {
      continue;
    }
    uint64_t actual = f / ((uint64_t)os * d);
    if (actual > target_hz) {
      continue;
    }
    if (!found || actual > out->actual_hz || (actual == out->actual_hz && d < out->div)) {
      out->level = l;
      out->div = (unsigned)d;
      out->sclk_hz = f;
      out->actual_hz = actual;
      found = 1;
    }
  }
  return found ? 0 : -1;
}

#ifdef __cplusplus
}
#endif

#endif
