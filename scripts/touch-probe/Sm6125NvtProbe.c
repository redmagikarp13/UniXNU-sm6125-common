// SPDX-License-Identifier: MIT
#include "Sm6125NvtProbe.h"
#include <string.h>

static void nvt_delay(const sm6125_nvt_bus_t *bus, unsigned ms) {
  if (bus && bus->delay_ms) {
    bus->delay_ms(bus->ctx, ms);
  }
}

int sm6125_nvt_set_page(const sm6125_nvt_bus_t *bus, uint32_t addr) {
  if (!bus || !bus->xfer) {
    return SM6125_NVT_ERR_PARAM;
  }
  uint8_t tx[3] = {
    0xFFu,
    (uint8_t)((addr >> 15) & 0xFFu),
    (uint8_t)((addr >> 7) & 0xFFu),
  };
  return bus->xfer(bus->ctx, tx, NULL, sizeof(tx)) ? SM6125_NVT_ERR_BUS : 0;
}

int sm6125_nvt_write_byte(const sm6125_nvt_bus_t *bus, uint32_t addr, uint8_t val) {
  int r = sm6125_nvt_set_page(bus, addr);
  if (r) {
    return r;
  }
  uint8_t tx[2] = {
    (uint8_t)((addr & 0x7Fu) | 0x80u),
    val,
  };
  return bus->xfer(bus->ctx, tx, NULL, sizeof(tx)) ? SM6125_NVT_ERR_BUS : 0;
}

int sm6125_nvt_read(const sm6125_nvt_bus_t *bus, uint32_t addr, uint8_t *out, size_t len) {
  if (!bus || !bus->xfer || !out || !len || len > SM6125_NVT_MAX_RW_LEN) {
    return SM6125_NVT_ERR_PARAM;
  }
  int r = sm6125_nvt_set_page(bus, addr);
  if (r) {
    return r;
  }
  uint8_t tx[SM6125_NVT_MAX_RW_LEN + 2];
  uint8_t rx[SM6125_NVT_MAX_RW_LEN + 2];
  memset(tx, 0, len + 2u);
  memset(rx, 0, len + 2u);
  tx[0] = (uint8_t)(addr & 0x7Fu);  /* SPI read clears bit 7, followed by 1 dummy byte */
  if (bus->xfer(bus->ctx, tx, rx, len + 2u)) {
    return SM6125_NVT_ERR_BUS;
  }
  memcpy(out, rx + 2u, len);
  return 0;
}

int sm6125_nvt_bootloader_reset(const sm6125_nvt_bus_t *bus) {
  int r = sm6125_nvt_write_byte(bus, SM6125_NVT_SWRST_N8_ADDR, 0x69u);
  if (r) {
    return r;
  }
  nvt_delay(bus, 5u);
  return sm6125_nvt_write_byte(bus, SM6125_NVT_SPI_RD_FAST_ADDR, 0x00u);
}

int sm6125_nvt_match_trim_id(const uint8_t id[SM6125_NVT_ID_BYTES], const char **out_name) {
  struct trim_rule {
    uint8_t id[SM6125_NVT_ID_BYTES];
    uint8_t mask[SM6125_NVT_ID_BYTES];
    const char *name;
  };
  static const struct trim_rule rules[] = {
    {{0x0A, 0xFF, 0xFF, 0x72, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A"},
    {{0x0B, 0xFF, 0xFF, 0x72, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A"},
    {{0x0A, 0xFF, 0xFF, 0x82, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A"},
    {{0x0B, 0xFF, 0xFF, 0x82, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A"},
    {{0x0A, 0xFF, 0xFF, 0x70, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A"},
    {{0x0B, 0xFF, 0xFF, 0x70, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A"},
    {{0x0B, 0xFF, 0xFF, 0x25, 0x65, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A-6525"},
    {{0x0C, 0xFF, 0xFF, 0x25, 0x65, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A-6525"},
    {{0x0A, 0xFF, 0xFF, 0x72, 0x65, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A-6572"},
    {{0x0A, 0xFF, 0xFF, 0x72, 0x67, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36672A-6772"},
    {{0x0C, 0xFF, 0xFF, 0x72, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36675"},
    {{0x0D, 0xFF, 0xFF, 0x72, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36675"},
    {{0x0E, 0xFF, 0xFF, 0x72, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36675"},
    {{0x20, 0xFF, 0xFF, 0x72, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36675"},
    {{0xFF, 0xFF, 0xFF, 0x75, 0x66, 0x03}, {0, 0, 0, 1, 1, 1}, "NT36675"},
    {{0xFF, 0xFF, 0xFF, 0x77, 0x66, 0x03}, {0, 0, 0, 1, 1, 1}, "NT36675"},
    {{0x00, 0xFF, 0xFF, 0x80, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, "NT36675"},
  };
  if (!id) {
    return 0;
  }
  for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); ++i) {
    int ok = 1;
    for (size_t k = 0; k < SM6125_NVT_ID_BYTES; ++k) {
      if (rules[i].mask[k] && id[k] != rules[i].id[k]) {
        ok = 0;
        break;
      }
    }
    if (ok) {
      if (out_name) {
        *out_name = rules[i].name;
      }
      return 1;
    }
  }
  if (id[5] == 0x03u && (id[4] == 0x65u || id[4] == 0x66u || id[4] == 0x67u) &&
      id[3] != 0x00u && id[3] != 0xFFu) {
    if (out_name) {
      *out_name = "NT36xxx";
    }
    return 1;
  }
  return 0;
}

int sm6125_nvt_identify(const sm6125_nvt_bus_t *bus, sm6125_nvt_ident_result_t *res) {
  if (!bus || !res) {
    return SM6125_NVT_ERR_PARAM;
  }
  memset(res, 0, sizeof(*res));
  res->chip_name = "-";

  int r = sm6125_nvt_write_byte(bus, SM6125_NVT_ENG_RST_ADDR, 0x5Au);
  if (r) {
    return r;
  }
  nvt_delay(bus, 1u);
  nvt_delay(bus, 10u);

  for (unsigned attempt = 1u; attempt <= 5u; ++attempt) {
    res->attempts = attempt;
    r = sm6125_nvt_bootloader_reset(bus);
    if (r) {
      return r;
    }
    r = sm6125_nvt_read(bus, SM6125_NVT_CHIP_VER_TRIM_OLD_ADDR, res->id, SM6125_NVT_ID_BYTES);
    if (r) {
      return r;
    }
    if (sm6125_nvt_match_trim_id(res->id, &res->chip_name)) {
      res->matched = 1;
      res->addr_used = SM6125_NVT_CHIP_VER_TRIM_OLD_ADDR;
      return 0;
    }
    nvt_delay(bus, 10u);
  }

  r = sm6125_nvt_read(bus, SM6125_NVT_CHIP_VER_TRIM_ADDR, res->alt_id, SM6125_NVT_ID_BYTES);
  if (r == 0 && sm6125_nvt_match_trim_id(res->alt_id, &res->chip_name)) {
    res->matched = 1;
    res->addr_used = SM6125_NVT_CHIP_VER_TRIM_ADDR;
    return 0;
  }
  return r ? r : SM6125_NVT_ERR_CHIP;
}
