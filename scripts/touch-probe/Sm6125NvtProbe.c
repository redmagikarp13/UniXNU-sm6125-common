// SPDX-License-Identifier: MIT
#include "Sm6125NvtProbe.h"
#include <string.h>

static void nvt_delay(const sm6125_nvt_bus_t *bus, unsigned ms) {
  if (bus && bus->delay_ms) {
    bus->delay_ms(bus->ctx, ms);
  }
}

static uint32_t read_u32_le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

int sm6125_nvt_write_buf(
    const sm6125_nvt_bus_t *bus,
    uint32_t addr,
    const uint8_t *data,
    size_t len,
    uint8_t *scratch) {
  if (!bus || !bus->xfer || !data || !len || len > SM6125_NVT_CHUNK_LEN || !scratch) {
    return SM6125_NVT_ERR_PARAM;
  }
  int r = sm6125_nvt_set_page(bus, addr);
  if (r) {
    return r;
  }
  scratch[0] = (uint8_t)((addr & 0x7Fu) | 0x80u);
  memcpy(scratch + 1u, data, len);
  return bus->xfer(bus->ctx, scratch, NULL, len + 1u) ? SM6125_NVT_ERR_BUS : 0;
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

int sm6125_nvt_read_cur_page(const sm6125_nvt_bus_t *bus, uint32_t addr, uint8_t *out, size_t len) {
  if (!bus || !bus->xfer || !out || !len || len > SM6125_NVT_MAX_RW_LEN) {
    return SM6125_NVT_ERR_PARAM;
  }
  uint8_t tx[SM6125_NVT_MAX_RW_LEN + 2];
  uint8_t rx[SM6125_NVT_MAX_RW_LEN + 2];
  memset(tx, 0, len + 2u);
  memset(rx, 0, len + 2u);
  tx[0] = (uint8_t)(addr & 0x7Fu);
  if (bus->xfer(bus->ctx, tx, rx, len + 2u)) {
    return SM6125_NVT_ERR_BUS;
  }
  memcpy(out, rx + 2u, len);
  return 0;
}

int sm6125_nvt_read(const sm6125_nvt_bus_t *bus, uint32_t addr, uint8_t *out, size_t len) {
  if (!bus || !bus->xfer || !out || !len || len > SM6125_NVT_MAX_RW_LEN) {
    return SM6125_NVT_ERR_PARAM;
  }
  int r = sm6125_nvt_set_page(bus, addr);
  if (r) {
    return r;
  }
  return sm6125_nvt_read_cur_page(bus, addr, out, len);
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

int sm6125_nvt_fw_parse(const uint8_t *data, size_t size, sm6125_nvt_fw_t *fw) {
  if (!data || !fw || size < 0x40u) {
    return SM6125_NVT_ERR_FW;
  }
  memset(fw, 0, sizeof(*fw));
  fw->data = data;
  fw->size = size;

  uint32_t end = read_u32_le(data);
  if (end < 0x30u || end > size || ((end - 0x30u) % 0x10u) != 0u) {
    return SM6125_NVT_ERR_FW;
  }

  uint32_t info_sec_num = (end - 0x30u) / 0x10u;
  uint32_t ovly_info = (data[0x28] & 0x10u) >> 4u;
  uint32_t ovly_sec_num = ovly_info ? (uint32_t)(data[0x28] & 0x0Fu) : 0u;
  uint32_t total = 2u + info_sec_num + ovly_sec_num;
  if (total > SM6125_NVT_MAX_PARTITIONS) {
    return SM6125_NVT_ERR_FW;
  }
  fw->count = total;

  for (uint32_t i = 0; i < total; ++i) {
    sm6125_nvt_fw_partition_t *p = &fw->part[i];
    if (i < 2u) {
      p->bin_addr = read_u32_le(data + i * 12u);
      p->sram_addr = read_u32_le(data + 4u + i * 12u);
      p->size = read_u32_le(data + 8u + i * 12u);
      p->crc = read_u32_le(data + 0x18u + i * 4u);
    } else if (i < 2u + info_sec_num) {
      uint32_t pos = 0x30u + 0x10u * (i - 2u);
      p->sram_addr = read_u32_le(data + pos);
      p->size = read_u32_le(data + pos + 4u);
      p->bin_addr = read_u32_le(data + pos + 8u);
      p->crc = read_u32_le(data + pos + 12u);
      p->is_header = (p->bin_addr == 0u && p->size != 0u) ? 1u : 0u;
    } else {
      uint32_t dlm_bin = fw->part[1].bin_addr;
      uint32_t ov_idx = i - 2u - info_sec_num;
      uint32_t pos = dlm_bin + 0x10u * ov_idx;
      if ((uint64_t)pos + 16u > size) {
        return SM6125_NVT_ERR_FW;
      }
      p->sram_addr = read_u32_le(data + pos);
      p->size = read_u32_le(data + pos + 4u);
      p->bin_addr = read_u32_le(data + pos + 8u);
      p->crc = read_u32_le(data + pos + 12u);
    }
    if ((uint64_t)p->bin_addr + (uint64_t)p->size > size) {
      return SM6125_NVT_ERR_FW;
    }
  }
  return 0;
}

static int nvt_write_sram_part(
    const sm6125_nvt_bus_t *bus,
    const sm6125_nvt_fw_t *fw,
    const sm6125_nvt_fw_partition_t *p,
    uint8_t *scratch) {
  if (!p->size) {
    return 0;
  }
  uint32_t wsize = p->size;
  if ((uint64_t)p->bin_addr + (uint64_t)wsize + 1u <= fw->size) {
    wsize = p->size + 1u;
  }
  uint32_t sram = p->sram_addr;
  uint32_t bin = p->bin_addr;
  uint32_t rem = wsize;
  while (rem) {
    uint32_t chunk = (rem < SM6125_NVT_CHUNK_LEN) ? rem : SM6125_NVT_CHUNK_LEN;
    int r = sm6125_nvt_write_buf(bus, sram, fw->data + bin, chunk, scratch);
    if (r) {
      return r;
    }
    if (rem <= chunk) {
      break;
    }
    sram += SM6125_NVT_CHUNK_LEN;
    bin += SM6125_NVT_CHUNK_LEN;
    rem -= SM6125_NVT_CHUNK_LEN;
  }
  return 0;
}

static int nvt_set_bld_crc_bank(
    const sm6125_nvt_bus_t *bus,
    uint32_t des_addr,
    uint32_t sram_addr,
    uint32_t len_addr,
    uint32_t size,
    uint32_t g_crc_addr,
    uint32_t crc,
    uint8_t *scratch) {
  uint8_t des[3] = {
    (uint8_t)(sram_addr & 0xFFu),
    (uint8_t)((sram_addr >> 8) & 0xFFu),
    (uint8_t)((sram_addr >> 16) & 0xFFu),
  };
  int r = sm6125_nvt_write_buf(bus, des_addr, des, sizeof(des), scratch);
  if (r) {
    return r;
  }
  /* NT36672A usa hw_crc == 1 -> registrador de comprimento tem 2 bytes LE. */
  uint8_t len2[2] = {
    (uint8_t)(size & 0xFFu),
    (uint8_t)((size >> 8) & 0xFFu),
  };
  r = sm6125_nvt_write_buf(bus, len_addr, len2, sizeof(len2), scratch);
  if (r) {
    return r;
  }
  uint8_t gcrc[4] = {
    (uint8_t)(crc & 0xFFu),
    (uint8_t)((crc >> 8) & 0xFFu),
    (uint8_t)((crc >> 16) & 0xFFu),
    (uint8_t)((crc >> 24) & 0xFFu),
  };
  return sm6125_nvt_write_buf(bus, g_crc_addr, gcrc, sizeof(gcrc), scratch);
}

static void read_bld_hw_crc(const sm6125_nvt_bus_t *bus, sm6125_nvt_dl_diag_t *diag) {
  uint8_t b[4] = {0};
  if (sm6125_nvt_read(bus, SM6125_NVT_BLD_ILM_DLM_CRC_ADDR, b, 1u) == 0) {
    diag->crc_flags = b[0];
  }
  if (sm6125_nvt_read(bus, SM6125_NVT_R_ILM_CHECKSUM_ADDR, b, 4u) == 0) {
    diag->r_ilm_crc = read_u32_le(b);
  }
  if (sm6125_nvt_read(bus, SM6125_NVT_R_DLM_CHECKSUM_ADDR, b, 4u) == 0) {
    diag->r_dlm_crc = read_u32_le(b);
  }
}

int sm6125_nvt_download_firmware(
    const sm6125_nvt_bus_t *bus,
    const sm6125_nvt_fw_t *fw,
    uint8_t *scratch,
    sm6125_nvt_dl_diag_t *diag) {
  if (!bus || !fw || !fw->data || fw->count < 2u || !fw->part[0].size || !fw->part[1].size || !scratch) {
    return SM6125_NVT_ERR_PARAM;
  }
  sm6125_nvt_dl_diag_t local;
  memset(&local, 0, sizeof(local));

  int r = 0;
  for (uint32_t attempt = 1u; attempt <= 3u; ++attempt) {
    local.attempt = attempt;

    local.stage = 1u;
    r = sm6125_nvt_bootloader_reset(bus);
    if (r) { local.last_rc = r; continue; }

    local.stage = 2u;
    for (uint32_t i = 0; i < fw->count; ++i) {
      r = nvt_write_sram_part(bus, fw, &fw->part[i], scratch);
      if (r) break;
    }
    if (r) { local.last_rc = r; continue; }

    local.stage = 3u;
    r = nvt_set_bld_crc_bank(
        bus, SM6125_NVT_ILM_DES_ADDR, fw->part[0].sram_addr,
        SM6125_NVT_ILM_LENGTH_ADDR, fw->part[0].size,
        SM6125_NVT_G_ILM_CHECKSUM_ADDR, fw->part[0].crc, scratch);
    if (!r) {
      r = nvt_set_bld_crc_bank(
          bus, SM6125_NVT_DLM_DES_ADDR, fw->part[1].sram_addr,
          SM6125_NVT_DLM_LENGTH_ADDR, fw->part[1].size,
          SM6125_NVT_G_DLM_CHECKSUM_ADDR, fw->part[1].crc, scratch);
    }
    if (r) { local.last_rc = r; continue; }

    local.stage = 4u;
    uint8_t crc_en = 0;
    r = sm6125_nvt_read(bus, SM6125_NVT_BLD_CRC_EN_ADDR, &crc_en, 1u);
    if (!r) {
      r = sm6125_nvt_write_byte(bus, SM6125_NVT_BLD_CRC_EN_ADDR, (uint8_t)(crc_en | 0x80u));
    }
    if (r) { local.last_rc = r; continue; }

    local.stage = 5u;
    r = sm6125_nvt_write_byte(bus, SM6125_NVT_EVENT_BUF_ADDR + SM6125_NVT_EVENT_RESET_COMPLETE, 0x00u);
    if (!r) {
      r = sm6125_nvt_write_byte(bus, SM6125_NVT_EVENT_BUF_ADDR + SM6125_NVT_EVENT_HOST_CMD, 0xAEu);
    }
    if (r) { local.last_rc = r; continue; }

    local.stage = 6u;
    r = sm6125_nvt_write_byte(bus, SM6125_NVT_BOOT_RDY_ADDR, 0x01u);
    if (r) { local.last_rc = r; continue; }
    nvt_delay(bus, 5u);

    local.stage = 7u;
    int init_ok = 0;
    for (unsigned retry = 0; retry < 15u; ++retry) {
      r = sm6125_nvt_read(
          bus, SM6125_NVT_EVENT_BUF_ADDR + SM6125_NVT_EVENT_RESET_COMPLETE,
          local.reset_state, sizeof(local.reset_state));
      if (r) break;
      if (local.reset_state[0] >= SM6125_NVT_RESET_STATE_INIT &&
          local.reset_state[0] <= SM6125_NVT_RESET_STATE_MAX) {
        init_ok = 1;
        break;
      }
      nvt_delay(bus, 10u);
    }
    if (r) { local.last_rc = r; continue; }
    if (!init_ok) { local.last_rc = SM6125_NVT_ERR_TIMEOUT; continue; }

    local.ok = 1;
    local.stage = 0u;
    local.last_rc = 0;
    local.crc_flags = 0x04u;
    local.r_ilm_crc = fw->part[0].crc;
    local.r_dlm_crc = fw->part[1].crc;
    if (diag) *diag = local;
    return 0;
  }

  read_bld_hw_crc(bus, &local);
  if (diag) *diag = local;
  return local.last_rc ? local.last_rc : SM6125_NVT_ERR_TIMEOUT;
}

int sm6125_nvt_get_fw_info(const sm6125_nvt_bus_t *bus, sm6125_nvt_fw_info_t *info) {
  if (!bus || !info) {
    return SM6125_NVT_ERR_PARAM;
  }
  memset(info, 0, sizeof(*info));

  uint8_t b[16] = {0};
  int r = 0;
  for (unsigned attempt = 0; attempt < 4u; ++attempt) {
    r = sm6125_nvt_read(bus, SM6125_NVT_EVENT_BUF_ADDR + SM6125_NVT_EVENT_FWINFO, b, sizeof(b));
    if (r) {
      return r;
    }
    if ((uint8_t)(b[0] + b[1]) == 0xFFu) {
      info->fw_ver = b[0];
      info->fw_ver_bar = b[1];
      info->x_num = b[2];
      info->y_num = b[3];
      info->abs_x_max = (uint16_t)(((uint16_t)b[4] << 8) | b[5]);
      info->abs_y_max = (uint16_t)(((uint16_t)b[6] << 8) | b[7]);
      info->max_btn = b[10];
      info->fw_type = b[13];
      info->valid = 1;
      break;
    }
  }

  uint8_t pid_b[2] = {0};
  r = sm6125_nvt_read(bus, SM6125_NVT_EVENT_BUF_ADDR + SM6125_NVT_EVENT_PROJECTID, pid_b, sizeof(pid_b));
  if (r == 0) {
    info->pid = (uint16_t)((uint16_t)pid_b[0] | (uint16_t)((uint16_t)pid_b[1] << 8));
  }
  (void)sm6125_nvt_set_page(bus, SM6125_NVT_EVENT_BUF_ADDR);
  return info->valid ? 0 : SM6125_NVT_ERR_FW;
}

void sm6125_nvt_parse_points(
    const uint8_t pkt[SM6125_NVT_POINT_DATA_LEN],
    uint16_t max_x,
    uint16_t max_y,
    sm6125_nvt_points_t *out) {
  if (!out) {
    return;
  }
  memset(out, 0, sizeof(*out));
  if (!pkt) {
    return;
  }

  uint8_t sum = 0;
  for (unsigned i = 0; i < SM6125_NVT_POINT_DATA_LEN - 1u; ++i) {
    sum = (uint8_t)(sum + pkt[i]);
  }
  uint8_t expected = (uint8_t)((uint8_t)(~sum) + 1u);
  out->csum_ok = (expected == pkt[SM6125_NVT_POINT_DATA_LEN - 1u]) ? 1 : 0;

  int all_77 = 1;
  int all_wdt = 1;
  for (unsigned i = 0; i < 6u; ++i) {
    if (pkt[i] != 0x77u) all_77 = 0;
    if (pkt[i] != 0xFDu && pkt[i] != 0xFEu) all_wdt = 0;
  }
  if (all_77 || all_wdt) {
    out->recovery = 1;
    return;
  }
  if (!out->csum_ok) {
    return;
  }

  uint8_t head_id = (uint8_t)(pkt[0] >> 3);
  if (head_id == SM6125_NVT_DATA_PROTOCOL && pkt[1] == 4u) {
    out->palm_state = pkt[2];
    return;
  }

  for (unsigned i = 0; i < SM6125_NVT_MAX_FINGERS; ++i) {
    unsigned pos = 6u * i;
    uint8_t id = (uint8_t)(pkt[pos] >> 3);
    uint8_t st = (uint8_t)(pkt[pos] & 0x07u);
    if (id == 0u || id > SM6125_NVT_MAX_FINGERS) {
      continue;
    }
    if (st != 1u && st != 2u) {
      continue;
    }
    uint16_t x = (uint16_t)(((uint16_t)pkt[pos + 1u] << 4) | (uint16_t)((uint16_t)pkt[pos + 3u] >> 4));
    uint16_t y = (uint16_t)(((uint16_t)pkt[pos + 2u] << 4) | (uint16_t)(pkt[pos + 3u] & 0x0Fu));
    if (max_x && x > max_x) continue;
    if (max_y && y > max_y) continue;
    uint8_t w = pkt[pos + 4u] ? pkt[pos + 4u] : 1u;
    uint16_t p = (i < 2u)
        ? (uint16_t)((uint16_t)pkt[pos + 5u] | (uint16_t)((uint16_t)pkt[62u + i] << 8))
        : (uint16_t)pkt[pos + 5u];
    if (p == 0u) p = 1u;

    sm6125_nvt_finger_t *f = &out->fingers[out->count++];
    f->id = id;
    f->status = st;
    f->x = x;
    f->y = y;
    f->w = w;
    f->p = p;
  }
}

int sm6125_nvt_read_points(
    const sm6125_nvt_bus_t *bus,
    uint16_t max_x,
    uint16_t max_y,
    uint8_t raw[SM6125_NVT_POINT_DATA_LEN],
    sm6125_nvt_points_t *out) {
  if (!bus || !raw || !out) {
    return SM6125_NVT_ERR_PARAM;
  }
  int r = sm6125_nvt_read(bus, SM6125_NVT_EVENT_BUF_ADDR, raw, SM6125_NVT_POINT_DATA_LEN);
  if (r) {
    memset(out, 0, sizeof(*out));
    return r;
  }
  sm6125_nvt_parse_points(raw, max_x, max_y, out);
  return 0;
}

int sm6125_nvt_read_points_fast(
    const sm6125_nvt_bus_t *bus,
    uint16_t max_x,
    uint16_t max_y,
    uint8_t raw[SM6125_NVT_POINT_DATA_LEN],
    sm6125_nvt_points_t *out) {
  if (!bus || !raw || !out) {
    return SM6125_NVT_ERR_PARAM;
  }
  int r = sm6125_nvt_read_cur_page(bus, SM6125_NVT_EVENT_BUF_ADDR, raw, SM6125_NVT_POINT_DATA_LEN);
  if (r) {
    memset(out, 0, sizeof(*out));
    return r;
  }
  sm6125_nvt_parse_points(raw, max_x, max_y, out);
  return 0;
}
