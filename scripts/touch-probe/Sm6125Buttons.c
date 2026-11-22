// SPDX-License-Identifier: BSD-2-Clause-Patent
// Leitor SPMI PMIC Arbiter v5 (Observer) para Power, Volume+ e Volume- no SM6125/PM6125.
#include "Sm6125Buttons.h"

static int find_apid_for_ppid(const sm6125_spmi_buttons_t *dev, uint16_t target_ppid)
{
  for (uint32_t apid = 0; apid < SM6125_SPMI_MAX_APIDS; ++apid) {
    uint32_t entry = dev->rd32(dev->ctx, dev->core_va, SM6125_SPMI_APID_MAP_V5(apid));
    uint16_t ppid = (uint16_t)((entry >> 8) & 0x0FFFu);
    if (ppid == target_ppid) {
      return (int)apid;
    }
  }
  return -1;
}

int sm6125_spmi_buttons_init(sm6125_spmi_buttons_t *dev)
{
  if (!dev || !dev->rd32 || !dev->wr32 || !dev->core_va || !dev->obsrvr_va) {
    return -1;
  }
  uint32_t ver = dev->rd32(dev->ctx, dev->core_va, SM6125_SPMI_ARB_VERSION_REG);
  if (ver < SM6125_SPMI_ARB_VERSION_V5) {
    return -2;
  }
  dev->apid_pon = (int16_t)find_apid_for_ppid(dev, (uint16_t)((SM6125_PMIC_SID0 << 8) | (SM6125_PMIC_PON_RT_STS >> 8)));
  dev->apid_volup = (int16_t)find_apid_for_ppid(dev, (uint16_t)((SM6125_PMIC_SID0 << 8) | (SM6125_PMIC_GPIO5_STATUS >> 8)));
  if (dev->apid_pon < 0 && dev->apid_volup < 0) {
    return -3;
  }
  return 0;
}

int sm6125_spmi_obs_read_byte(const sm6125_spmi_buttons_t *dev, uint16_t apid, uint8_t sid, uint16_t addr, uint8_t *out)
{
  if (!dev || !dev->rd32 || !dev->wr32 || !dev->obsrvr_va || !out) {
    return -1;
  }
  uint32_t base = SM6125_SPMI_OBS_OFFSET(dev->ee, apid);
  uint32_t cmd = ((uint32_t)SM6125_SPMI_OP_EXT_READL << 27) |
                 (((uint32_t)(sid & 0x0Fu)) << 20) |
                 (((uint32_t)addr) << 4) |
                 0u; /* bc=0 => 1 byte */
  dev->wr32(dev->ctx, dev->obsrvr_va, base + SM6125_SPMI_CMD_REG, cmd);

  for (unsigned i = 0; i < 200u; ++i) {
    uint32_t st = dev->rd32(dev->ctx, dev->obsrvr_va, base + SM6125_SPMI_STATUS_REG);
    if (st & SM6125_SPMI_STATUS_DONE) {
      if (st & SM6125_SPMI_STATUS_ERR) {
        return -4;
      }
      uint32_t rdata = dev->rd32(dev->ctx, dev->obsrvr_va, base + SM6125_SPMI_RDATA0_REG);
      *out = (uint8_t)(rdata & 0xFFu);
      return 0;
    }
    if (dev->delay_us) {
      dev->delay_us(dev->ctx, 5u);
    }
  }
  return -5;
}

uint32_t sm6125_buttons_decode_mask(uint8_t pon_rt_sts, uint8_t gpio5_rt_sts)
{
  uint32_t mask = 0u;
  if (pon_rt_sts & SM6125_PMIC_PON_KPDPWR_BIT) {
    mask |= SM6125_HID_BTN_POWER;
  }
  if (pon_rt_sts & SM6125_PMIC_PON_RESIN_BIT) {
    mask |= SM6125_HID_BTN_VOLDN;
  }
  if ((gpio5_rt_sts & SM6125_PMIC_GPIO_VAL_BIT) == 0u) {
    mask |= SM6125_HID_BTN_VOLUP;
  }
  return mask;
}

int sm6125_spmi_buttons_poll(const sm6125_spmi_buttons_t *dev, uint32_t *out_mask)
{
  if (!dev || !out_mask) {
    return -1;
  }
  uint8_t pon = 0u;
  uint8_t gpio5 = SM6125_PMIC_GPIO_VAL_BIT; /* solto = HIGH */
  int rc = 0;

  if (dev->apid_pon >= 0) {
    int r = sm6125_spmi_obs_read_byte(dev, (uint16_t)dev->apid_pon, SM6125_PMIC_SID0, SM6125_PMIC_PON_RT_STS, &pon);
    if (r != 0) rc = r;
  }
  if (dev->apid_volup >= 0) {
    int r = sm6125_spmi_obs_read_byte(dev, (uint16_t)dev->apid_volup, SM6125_PMIC_SID0, SM6125_PMIC_GPIO5_STATUS, &gpio5);
    if (r != 0 && rc == 0) rc = r;
  }
  *out_mask = sm6125_buttons_decode_mask(pon, gpio5);
  return rc;
}
