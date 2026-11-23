// SPDX-License-Identifier: BSD-2-Clause-Patent
// Cliente minimo GLINK-RPM (rpm_msg_ram + APCS IPC) para votar reguladores RPM no SM6125.
#ifndef SM6125_RPM_GLINK_H
#define SM6125_RPM_GLINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SM6125_RPM_MSG_RAM_BASE   0x045F0000u
#define SM6125_RPM_MSG_RAM_SIZE   0x00007000u
#define SM6125_APCS_GLB_BASE      0x0F111000u
#define SM6125_APCS_GLB_SIZE      0x00001000u
#define SM6125_APCS_IPC_OFFSET    0x00000008u
#define SM6125_APCS_RPM_BIT       (1u << 0)

#define SM6125_RPM_TOC_OFFSET     (SM6125_RPM_MSG_RAM_SIZE - 256u)
#define SM6125_RPM_TOC_MAGIC      0x67727430u /* "grt0" */
#define SM6125_RPM_TX_FIFO_ID     0x61703272u /* "ap2r" */
#define SM6125_RPM_RX_FIFO_ID     0x72326170u /* "r2ap" */

#define SM6125_RPM_TYPE_RWCX      0x78637772u /* "rwcx" */
#define SM6125_RPM_TYPE_RWMX      0x786d7772u /* "rwmx" */
#define SM6125_RPM_KEY_SWEN       0x6E657773u /* "swen" */
#define SM6125_RPM_KEY_VLVL       0x6C766C76u /* "vlvl" */

typedef struct {
  void *ctx;
  uint32_t (*rd32)(void *ctx, uintptr_t base, uint32_t off);
  void (*wr32)(void *ctx, uintptr_t base, uint32_t off, uint32_t val);
  void (*delay_us)(void *ctx, uint32_t us);
  uintptr_t msg_ram_va;
  uintptr_t apcs_va;
  uint32_t tx_off;
  uint32_t tx_len;
  uint32_t rx_off;
  uint32_t rx_len;
  uint16_t lcid;
  uint16_t rcid;
  uint32_t next_msg_id;
} sm6125_rpm_glink_t;

int sm6125_rpm_glink_init(sm6125_rpm_glink_t *g);
int sm6125_rpm_glink_open(sm6125_rpm_glink_t *g, uint32_t *out_ver, uint32_t *out_ack);
int sm6125_rpm_glink_vote_rail(sm6125_rpm_glink_t *g, uint32_t type, uint32_t id, uint32_t level, uint32_t enable, uint32_t *out_ack);

#ifdef __cplusplus
}
#endif

#endif /* SM6125_RPM_GLINK_H */
