// SPDX-License-Identifier: MIT
// Protocolo SPI completo (identificacao + host-download FW + coordenadas) para Novatek NT36672A (ginkgo).
#ifndef SM6125_NVT_PROBE_H
#define SM6125_NVT_PROBE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Enderecos Novatek NT36672A (stock-fdt.dts, NT36672A/nt36xxx_mem_map.h e nt36xxx.h) */
#define SM6125_NVT_ENG_RST_ADDR            0x7FFF80u
#define SM6125_NVT_SWRST_N8_ADDR           0x3F0FEu
#define SM6125_NVT_SPI_RD_FAST_ADDR        0x3F310u
#define SM6125_NVT_CHIP_VER_TRIM_ADDR      0x3F004u
#define SM6125_NVT_CHIP_VER_TRIM_OLD_ADDR  0x1F64Eu

#define SM6125_NVT_EVENT_BUF_ADDR          0x21C00u
#define SM6125_NVT_EVENT_HOST_CMD          0x50u
#define SM6125_NVT_EVENT_RESET_COMPLETE    0x60u
#define SM6125_NVT_EVENT_FWINFO            0x78u
#define SM6125_NVT_EVENT_PROJECTID         0x9Au

#define SM6125_NVT_BOOT_RDY_ADDR           0x3F10Du
#define SM6125_NVT_ILM_DES_ADDR            0x3F128u
#define SM6125_NVT_ILM_LENGTH_ADDR         0x3F118u
#define SM6125_NVT_G_ILM_CHECKSUM_ADDR     0x3F100u
#define SM6125_NVT_R_ILM_CHECKSUM_ADDR     0x3F120u
#define SM6125_NVT_DLM_DES_ADDR            0x3F12Cu
#define SM6125_NVT_DLM_LENGTH_ADDR         0x3F130u
#define SM6125_NVT_G_DLM_CHECKSUM_ADDR     0x3F104u
#define SM6125_NVT_R_DLM_CHECKSUM_ADDR     0x3F124u
#define SM6125_NVT_BLD_CRC_EN_ADDR         0x3F30Eu
#define SM6125_NVT_BLD_ILM_DLM_CRC_ADDR    0x3F133u

#define SM6125_NVT_RESET_STATE_INIT        0xA0u
#define SM6125_NVT_RESET_STATE_MAX         0xAFu

#define SM6125_NVT_CHUNK_LEN               4096u
#define SM6125_NVT_POINT_DATA_LEN          65u
#define SM6125_NVT_MAX_FINGERS             10u
#define SM6125_NVT_MAX_PARTITIONS          32u
#define SM6125_NVT_DATA_PROTOCOL           30u
#define SM6125_NVT_MAX_RW_LEN              128u
#define SM6125_NVT_ID_BYTES                6u

#define SM6125_NVT_ERR_PARAM   (-22)
#define SM6125_NVT_ERR_BUS     (-5)
#define SM6125_NVT_ERR_CHIP    (-19)
#define SM6125_NVT_ERR_FW      (-71)
#define SM6125_NVT_ERR_TIMEOUT (-110)

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

typedef struct sm6125_nvt_fw_partition {
  uint32_t sram_addr;
  uint32_t size;
  uint32_t bin_addr;
  uint32_t crc;
  uint8_t is_header;
} sm6125_nvt_fw_partition_t;

typedef struct sm6125_nvt_fw {
  const uint8_t *data;
  size_t size;
  uint32_t count;
  sm6125_nvt_fw_partition_t part[SM6125_NVT_MAX_PARTITIONS];
} sm6125_nvt_fw_t;

typedef struct sm6125_nvt_dl_diag {
  int ok;
  uint32_t attempt;
  uint32_t stage;
  int32_t last_rc;
  uint8_t reset_state[5];
  uint8_t crc_flags;
  uint32_t r_ilm_crc;
  uint32_t r_dlm_crc;
} sm6125_nvt_dl_diag_t;

typedef struct sm6125_nvt_fw_info {
  uint8_t fw_ver;
  uint8_t fw_ver_bar;
  uint8_t x_num;
  uint8_t y_num;
  uint16_t abs_x_max;
  uint16_t abs_y_max;
  uint8_t max_btn;
  uint8_t fw_type;
  uint16_t pid;
  int valid;
} sm6125_nvt_fw_info_t;

typedef struct sm6125_nvt_finger {
  uint8_t id;
  uint8_t status;
  uint16_t x;
  uint16_t y;
  uint8_t w;
  uint16_t p;
} sm6125_nvt_finger_t;

typedef struct sm6125_nvt_points {
  int csum_ok;
  int recovery;
  uint8_t palm_state;
  uint32_t count;
  sm6125_nvt_finger_t fingers[SM6125_NVT_MAX_FINGERS];
} sm6125_nvt_points_t;

int sm6125_nvt_set_page(const sm6125_nvt_bus_t *bus, uint32_t addr);
int sm6125_nvt_write_byte(const sm6125_nvt_bus_t *bus, uint32_t addr, uint8_t val);
int sm6125_nvt_write_buf(
    const sm6125_nvt_bus_t *bus,
    uint32_t addr,
    const uint8_t *data,
    size_t len,
    uint8_t *scratch);
int sm6125_nvt_read(const sm6125_nvt_bus_t *bus, uint32_t addr, uint8_t *out, size_t len);
int sm6125_nvt_read_cur_page(const sm6125_nvt_bus_t *bus, uint32_t addr, uint8_t *out, size_t len);
int sm6125_nvt_bootloader_reset(const sm6125_nvt_bus_t *bus);
int sm6125_nvt_match_trim_id(const uint8_t id[SM6125_NVT_ID_BYTES], const char **out_name);
int sm6125_nvt_identify(const sm6125_nvt_bus_t *bus, sm6125_nvt_ident_result_t *res);

int sm6125_nvt_fw_parse(const uint8_t *data, size_t size, sm6125_nvt_fw_t *fw);
int sm6125_nvt_download_firmware(
    const sm6125_nvt_bus_t *bus,
    const sm6125_nvt_fw_t *fw,
    uint8_t *scratch,
    sm6125_nvt_dl_diag_t *diag);
int sm6125_nvt_get_fw_info(const sm6125_nvt_bus_t *bus, sm6125_nvt_fw_info_t *info);
void sm6125_nvt_parse_points(
    const uint8_t pkt[SM6125_NVT_POINT_DATA_LEN],
    uint16_t max_x,
    uint16_t max_y,
    sm6125_nvt_points_t *out);
int sm6125_nvt_read_points(
    const sm6125_nvt_bus_t *bus,
    uint16_t max_x,
    uint16_t max_y,
    uint8_t raw[SM6125_NVT_POINT_DATA_LEN],
    sm6125_nvt_points_t *out);
int sm6125_nvt_read_points_fast(
    const sm6125_nvt_bus_t *bus,
    uint16_t max_x,
    uint16_t max_y,
    uint8_t raw[SM6125_NVT_POINT_DATA_LEN],
    sm6125_nvt_points_t *out);

#ifdef __cplusplus
}
#endif

#endif
