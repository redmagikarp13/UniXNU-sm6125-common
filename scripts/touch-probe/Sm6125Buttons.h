// SPDX-License-Identifier: BSD-2-Clause-Patent
// Leitura de botões físicos do SM6125/PM6125 via SPMI PMIC Arbiter v5 (Observer).
#ifndef SM6125_BUTTONS_H
#define SM6125_BUTTONS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Endereços físicos do spmi@1c40000 no SM6125 (ginkgo stock-fdt.dts). */
#define SM6125_SPMI_CORE_BASE    0x01C40000u
#define SM6125_SPMI_CORE_SIZE    0x00002000u
#define SM6125_SPMI_OBSRVR_BASE  0x03E00000u
#define SM6125_SPMI_OBSRVR_SIZE  0x00020000u

/* PMIC Arbiter v5: tabela APID->PPID em core + 0x0900 e canais observer EE=0 em 0x80 * apid. */
#define SM6125_SPMI_ARB_VERSION_REG   0x0000u
#define SM6125_SPMI_ARB_VERSION_V5    0x50000000u
#define SM6125_SPMI_APID_MAP_V5(n)    (0x0900u + 4u * (uint32_t)(n))
#define SM6125_SPMI_MAX_APIDS         512u
#define SM6125_SPMI_OBS_OFFSET(ee, a) (0x10000u * (uint32_t)(ee) + 0x80u * (uint32_t)(a))

#define SM6125_SPMI_CMD_REG     0x00u
#define SM6125_SPMI_CONFIG_REG  0x04u
#define SM6125_SPMI_STATUS_REG  0x08u
#define SM6125_SPMI_RDATA0_REG  0x18u

#define SM6125_SPMI_OP_EXT_READL 0x01u
#define SM6125_SPMI_STATUS_DONE  0x01u
#define SM6125_SPMI_STATUS_ERR   0x0Eu

/* Periféricos PM6125 (SID 0) no ginkgo: PON (0x800) e GPIO5 Volume Up (0xC400). */
#define SM6125_PMIC_SID0           0u
#define SM6125_PMIC_PON_RT_STS     0x0810u
#define SM6125_PMIC_PON_KPDPWR_BIT 0x01u
#define SM6125_PMIC_PON_RESIN_BIT  0x02u
#define SM6125_PMIC_GPIO5_STATUS   0xC408u
#define SM6125_PMIC_GPIO_VAL_BIT   0x01u

/* Máscara HID Report 2 (Consumer): bit 1 = Power, bit 2 = Vol+, bit 3 = Vol-. */
#define SM6125_HID_BTN_MENU  0x01u
#define SM6125_HID_BTN_POWER 0x02u
#define SM6125_HID_BTN_VOLUP 0x04u
#define SM6125_HID_BTN_VOLDN 0x08u

typedef struct {
  void *ctx;
  uint32_t (*rd32)(void *ctx, uintptr_t base, uint32_t off);
  void (*wr32)(void *ctx, uintptr_t base, uint32_t off, uint32_t val);
  void (*delay_us)(void *ctx, uint32_t us);
  uintptr_t core_va;
  uintptr_t obsrvr_va;
  uint32_t ee;
  int16_t apid_pon;
  int16_t apid_volup;
} sm6125_spmi_buttons_t;

int sm6125_spmi_buttons_init(sm6125_spmi_buttons_t *dev);
int sm6125_spmi_obs_read_byte(const sm6125_spmi_buttons_t *dev, uint16_t apid, uint8_t sid, uint16_t addr, uint8_t *out);
uint32_t sm6125_buttons_decode_mask(uint8_t pon_rt_sts, uint8_t gpio5_rt_sts);
int sm6125_spmi_buttons_poll(const sm6125_spmi_buttons_t *dev, uint32_t *out_mask);

#ifdef __cplusplus
}
#endif

#endif /* SM6125_BUTTONS_H */
