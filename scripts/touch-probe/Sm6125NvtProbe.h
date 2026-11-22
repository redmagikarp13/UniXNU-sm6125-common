// SPDX-License-Identifier: MIT
// Protocolo SPI somente-identificação para o controlador Novatek NT36672A (ginkgo, TDDI host-download).
#ifndef SM6125_NVT_PROBE_H
#define SM6125_NVT_PROBE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Endereços Novatek NT36672A (backup-ginkgo/stock-fdt.dts e NT36672A/nt36xxx_mem_map.h) */
#define SM6125_NVT_ENG_RST_ADDR            0x7FFF80u
#define SM6125_NVT_SWRST_N8_ADDR           0x3F0FEu
#define SM6125_NVT_SPI_RD_FAST_ADDR        0x3F310u
#define SM6125_NVT_CHIP_VER_TRIM_ADDR      0x3F004u
#define SM6125_NVT_CHIP_VER_TRIM_OLD_ADDR  0x1F64Eu
#define SM6125_NVT_MAX_RW_LEN              64u
#define SM6125_NVT_ID_BYTES                6u

#define SM6125_NVT_ERR_PARAM (-22)
#define SM6125_NVT_ERR_BUS   (-5)
#define SM6125_NVT_ERR_CHIP  (-19)

typedef struct sm6125_nvt_bus {
  void *ctx;
  int (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
  void (*delay_ms)(void *ctx, unsigned ms);
} sm6125_nvt_bus_t;

typedef struct sm6125_nvt_ident_result {
  int matched;
  unsigned attempts;
  uint32_t addr_used;
  uint8_t id[SM6125_NVT_ID_BYTES];
  uint8_t alt_id[SM6125_NVT_ID_BYTES];
  const char *chip_name;
} sm6125_nvt_ident_result_t;

int sm6125_nvt_set_page(const sm6125_nvt_bus_t *bus, uint32_t addr);
int sm6125_nvt_write_byte(const sm6125_nvt_bus_t *bus, uint32_t addr, uint8_t val);
int sm6125_nvt_read(const sm6125_nvt_bus_t *bus, uint32_t addr, uint8_t *out, size_t len);
int sm6125_nvt_bootloader_reset(const sm6125_nvt_bus_t *bus);
int sm6125_nvt_match_trim_id(const uint8_t id[SM6125_NVT_ID_BYTES], const char **out_name);
int sm6125_nvt_identify(const sm6125_nvt_bus_t *bus, sm6125_nvt_ident_result_t *res);

#ifdef __cplusplus
}
#endif

#endif
