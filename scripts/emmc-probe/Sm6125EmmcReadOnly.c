// SPDX-License-Identifier: MIT
#include "Sm6125EmmcReadOnly.h"

typedef struct {
  char *buf;
  size_t cap;
  size_t len;
} fmt_cursor_t;

static void fmt_putc(fmt_cursor_t *c, char ch) {
  if (c->len + 1u < c->cap) {
    c->buf[c->len] = ch;
    c->len++;
  }
}

static void fmt_puts(fmt_cursor_t *c, const char *s) {
  if (!s) {
    return;
  }
  while (*s != '\0') {
    fmt_putc(c, *s);
    s++;
  }
}

static void fmt_hex(fmt_cursor_t *c, uint64_t value, unsigned digits) {
  static const char hex[] = "0123456789abcdef";
  unsigned shift = digits * 4u;
  while (shift > 0u) {
    shift -= 4u;
    fmt_putc(c, hex[(value >> shift) & 0xFu]);
  }
}

static void fmt_kv_u32(fmt_cursor_t *c, const char *key, uint32_t value, unsigned digits) {
  fmt_puts(c, key);
  fmt_putc(c, '=');
  fmt_hex(c, value, digits);
}

static void fmt_kv_pa(fmt_cursor_t *c, const char *key, uint64_t value) {
  fmt_puts(c, key);
  fmt_puts(c, "=0x");
  fmt_hex(c, value, value <= 0xFFFFFFFFULL ? 8u : 16u);
}

static const char *err_name(uint32_t err) {
  switch (err) {
    case SM6125_EMMC_RO_ERR_NONE:
      return "none";
    case SM6125_EMMC_RO_ERR_INVALID_MODE:
      return "invalid-mode";
    case SM6125_EMMC_RO_ERR_ADT_INVALID:
      return "adt-invalid";
    case SM6125_EMMC_RO_ERR_MAP_GCC:
      return "map-gcc";
    case SM6125_EMMC_RO_ERR_CLOCK_GATED:
      return "clock-gated";
    case SM6125_EMMC_RO_ERR_MAP_SDHCI:
      return "map-sdhci";
    default:
      return "unknown";
  }
}

uint32_t sm6125_emmc_probe_select_mode(bool has_arg, uint32_t arg_value) {
  if (!has_arg) {
    return SM6125_EMMC_PROBE_MODE_OFF;
  }
  if (arg_value <= SM6125_EMMC_PROBE_MODE_IDENT_GPT) {
    return arg_value;
  }
  return SM6125_EMMC_PROBE_MODE_INVALID;
}

bool sm6125_emmc_translate_region(
    const uint64_t bus[3],
    uint64_t off,
    uint64_t len,
    uint64_t min_len,
    uint64_t *out_pa) {
  if (!bus || !out_pa) {
    return false;
  }
  const uint64_t bus_child = bus[0];
  const uint64_t bus_phys = bus[1];
  const uint64_t bus_size = bus[2];

  if (bus_phys == 0ULL || bus_size == 0ULL || len < min_len) {
    return false;
  }
  if (off < bus_child) {
    return false;
  }
  const uint64_t rel = off - bus_child;
  if (rel > bus_size || len > (bus_size - rel)) {
    return false;
  }
  if (rel > (UINT64_MAX - bus_phys)) {
    return false;
  }
  const uint64_t pa = bus_phys + rel;
  if (len > (UINT64_MAX - pa)) {
    return false;
  }
  *out_pa = pa;
  return true;
}

bool sm6125_emmc_gcc_gate(
    uint32_t bcr,
    uint32_t apps,
    uint32_t ahb,
    uint32_t ice,
    uint32_t *out_flags) {
  uint32_t flags = 0u;
  if ((bcr & 1u) == 0u) {
    flags |= SM6125_EMMC_GATE_BCR_CLEAR;
  }
  if ((ahb & 0x80000001u) == 1u) {
    flags |= SM6125_EMMC_GATE_AHB_ON;
  }
  if ((apps & 0x80000001u) == 1u) {
    flags |= SM6125_EMMC_GATE_APPS_ON;
  }
  if ((ice & 0x80000001u) == 1u) {
    flags |= SM6125_EMMC_GATE_ICE_ON;
  }
  if (out_flags) {
    *out_flags = flags;
  }
  return (flags & SM6125_EMMC_GATE_REQUIRED_MASK) == SM6125_EMMC_GATE_REQUIRED_MASK;
}

bool sm6125_emmc_ro_read_gcc(
    const sm6125_mmio_ro_ops_t *ops,
    uintptr_t gcc_sdcc1_va,
    sm6125_emmc_ro_snapshot_t *snap) {
  if (!ops || !ops->read32 || !gcc_sdcc1_va || !snap) {
    return false;
  }
  snap->gcc_bcr = ops->read32(gcc_sdcc1_va, SM6125_GCC_SDCC1_BCR_OFF);
  snap->gcc_apps_cbcr = ops->read32(gcc_sdcc1_va, SM6125_GCC_SDCC1_APPS_CBCR_OFF);
  snap->gcc_ahb_cbcr = ops->read32(gcc_sdcc1_va, SM6125_GCC_SDCC1_AHB_CBCR_OFF);
  snap->gcc_ice_cbcr = ops->read32(gcc_sdcc1_va, SM6125_GCC_SDCC1_ICE_CBCR_OFF);
  snap->gcc_ice_cmd = ops->read32(gcc_sdcc1_va, SM6125_GCC_SDCC1_ICE_CMD_RCGR_OFF);
  snap->gcc_ice_cfg = ops->read32(gcc_sdcc1_va, SM6125_GCC_SDCC1_ICE_CFG_RCGR_OFF);
  snap->gcc_apps_cmd = ops->read32(gcc_sdcc1_va, SM6125_GCC_SDCC1_APPS_CMD_RCGR_OFF);
  snap->gcc_apps_cfg = ops->read32(gcc_sdcc1_va, SM6125_GCC_SDCC1_APPS_CFG_RCGR_OFF);
  snap->gcc_apps_m = ops->read32(gcc_sdcc1_va, SM6125_GCC_SDCC1_APPS_M_OFF);
  snap->gcc_apps_n = ops->read32(gcc_sdcc1_va, SM6125_GCC_SDCC1_APPS_N_OFF);
  snap->gcc_apps_d = ops->read32(gcc_sdcc1_va, SM6125_GCC_SDCC1_APPS_D_OFF);
  snap->gcc_valid = true;
  return true;
}

bool sm6125_emmc_ro_read_sdhci(
    const sm6125_mmio_ro_ops_t *ops,
    uintptr_t sdhci_va,
    sm6125_emmc_ro_snapshot_t *snap) {
  if (!ops || !ops->read8 || !ops->read16 || !ops->read32 || !sdhci_va || !snap) {
    return false;
  }
  snap->sdhci_present_state = ops->read32(sdhci_va, SM6125_SDHCI_PRESENT_STATE);
  snap->sdhci_host_control = ops->read8(sdhci_va, SM6125_SDHCI_HOST_CONTROL);
  snap->sdhci_power_control = ops->read8(sdhci_va, SM6125_SDHCI_POWER_CONTROL);
  snap->sdhci_clock_control = ops->read16(sdhci_va, SM6125_SDHCI_CLOCK_CONTROL);
  snap->sdhci_software_reset = ops->read8(sdhci_va, SM6125_SDHCI_SOFTWARE_RESET);
  snap->sdhci_int_status = ops->read32(sdhci_va, SM6125_SDHCI_INT_STATUS);
  snap->sdhci_int_enable = ops->read32(sdhci_va, SM6125_SDHCI_INT_ENABLE);
  snap->sdhci_signal_enable = ops->read32(sdhci_va, SM6125_SDHCI_SIGNAL_ENABLE);
  snap->sdhci_host_control2 = ops->read16(sdhci_va, SM6125_SDHCI_HOST_CONTROL2);
  snap->sdhci_cap0 = ops->read32(sdhci_va, SM6125_SDHCI_CAPABILITIES);
  snap->sdhci_cap1 = ops->read32(sdhci_va, SM6125_SDHCI_CAPABILITIES_1);
  snap->sdhci_spec_version = ops->read16(sdhci_va, SM6125_SDHCI_HOST_VERSION);

  snap->sdhci_dll_config = ops->read32(sdhci_va, SM6125_SDHCI_V5_DLL_CONFIG);
  snap->sdhci_dll_status = ops->read32(sdhci_va, SM6125_SDHCI_V5_DLL_STATUS);
  snap->sdhci_vendor_spec = ops->read32(sdhci_va, SM6125_SDHCI_V5_VENDOR_SPEC);
  snap->sdhci_vendor_cap0 = ops->read32(sdhci_va, SM6125_SDHCI_V5_VENDOR_CAP0);
  snap->sdhci_pwrctl_status = ops->read32(sdhci_va, SM6125_SDHCI_V5_PWRCTL_STATUS);
  snap->sdhci_pwrctl_mask = ops->read32(sdhci_va, SM6125_SDHCI_V5_PWRCTL_MASK);
  snap->sdhci_vendor_spec3 = ops->read32(sdhci_va, SM6125_SDHCI_V5_VENDOR_SPEC3);
  snap->sdhci_dll_config_2 = ops->read32(sdhci_va, SM6125_SDHCI_V5_DLL_CONFIG_2);
  snap->sdhci_ddr_config = ops->read32(sdhci_va, SM6125_SDHCI_V5_DDR_CONFIG);
  snap->sdhci_mci_version = ops->read32(sdhci_va, SM6125_SDHCI_V5_MCI_VERSION);
  snap->sdhci_generics = ops->read32(sdhci_va, SM6125_SDHCI_V5_GENERICS);
  snap->sdhci_valid = true;
  return true;
}

size_t sm6125_emmc_ro_format_report(
    const sm6125_emmc_ro_snapshot_t *snap,
    char *buf,
    size_t cap) {
  if (!buf || cap == 0u) {
    return 0u;
  }
  buf[0] = '\0';
  if (!snap || cap == 1u) {
    return 0u;
  }

  fmt_cursor_t c = {.buf = buf, .cap = cap, .len = 0u};
  fmt_puts(&c, "SM6125-EMMC-RO ok=");
  fmt_putc(&c, snap->err == SM6125_EMMC_RO_ERR_NONE ? '1' : '0');
  fmt_puts(&c, " err=");
  fmt_puts(&c, err_name(snap->err));
  fmt_puts(&c, " gate=0x");
  fmt_hex(&c, snap->gate_flags & 0xFFu, 2u);
  fmt_putc(&c, ' ');
  fmt_kv_pa(&c, "gcc", snap->gcc_pa);
  fmt_putc(&c, ' ');
  fmt_kv_pa(&c, "sdhci", snap->sdhci_pa);

  if (snap->gcc_valid) {
    fmt_puts(&c, "\nGCC ");
    fmt_kv_u32(&c, "bcr", snap->gcc_bcr, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "apps", snap->gcc_apps_cbcr, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "ahb", snap->gcc_ahb_cbcr, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "ice", snap->gcc_ice_cbcr, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "icmd", snap->gcc_ice_cmd, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "icfg", snap->gcc_ice_cfg, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "acmd", snap->gcc_apps_cmd, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "acfg", snap->gcc_apps_cfg, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "m", snap->gcc_apps_m, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "n", snap->gcc_apps_n, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "d", snap->gcc_apps_d, 8u);
  }

  if (snap->sdhci_valid) {
    fmt_puts(&c, "\nSTD ");
    fmt_kv_u32(&c, "mci", snap->sdhci_mci_version, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "sv", snap->sdhci_spec_version, 4u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "cap0", snap->sdhci_cap0, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "cap1", snap->sdhci_cap1, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "ps", snap->sdhci_present_state, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "clk", snap->sdhci_clock_control, 4u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "pwr", snap->sdhci_power_control, 2u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "hc", snap->sdhci_host_control, 2u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "hc2", snap->sdhci_host_control2, 4u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "rst", snap->sdhci_software_reset, 2u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "is", snap->sdhci_int_status, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "ie", snap->sdhci_int_enable, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "se", snap->sdhci_signal_enable, 8u);

    fmt_puts(&c, "\nVND ");
    fmt_kv_u32(&c, "vs", snap->sdhci_vendor_spec, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "vs3", snap->sdhci_vendor_spec3, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "vc0", snap->sdhci_vendor_cap0, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "pst", snap->sdhci_pwrctl_status, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "pm", snap->sdhci_pwrctl_mask, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "gen", snap->sdhci_generics, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "dll", snap->sdhci_dll_config, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "dll2", snap->sdhci_dll_config_2, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "dst", snap->sdhci_dll_status, 8u);
    fmt_putc(&c, ' ');
    fmt_kv_u32(&c, "ddr", snap->sdhci_ddr_config, 8u);
  }

  c.buf[c.len] = '\0';
  return c.len;
}
