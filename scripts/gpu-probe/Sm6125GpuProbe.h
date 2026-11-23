// SPDX-License-Identifier: BSD-2-Clause-Patent
// Sonda pura em C para energia, clocks, RBBM (Adreno 610) e SMMU v500 no SM6125.
#ifndef SM6125_GPU_PROBE_H
#define SM6125_GPU_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  void *ctx;
  uint32_t (*rd32)(void *ctx, uintptr_t base, uint32_t off);
  void (*wr32)(void *ctx, uintptr_t base, uint32_t off, uint32_t val);
  void (*delay_us)(void *ctx, uint32_t us);
  uintptr_t gcc_va;
  uintptr_t gpucc_va;
  uintptr_t gpu_va;
  uintptr_t smmu_va;
} sm6125_gpu_bus_t;

typedef struct {
  int step;
  int err;
  uint32_t cx_gdscr;
  uint32_t gx_gdscr;
  uint32_t cx_gfx3d_cbcr;
  uint32_t gx_gfx3d_cbcr;
  uint32_t rbbm_status;
  uint32_t rbbm_int0;
  uint32_t rbbm_clk_cntl;
  uint32_t hyst_before;
  uint32_t hyst_readback;
  uint32_t smmu_idr0;
  uint32_t smmu_idr1;
  uint32_t smmu_idr7;
  uint32_t gcc_vote_before;
  uint32_t gcc_vote_after;
  uint32_t ahb_cbcr;
} sm6125_gpu_probe_result_t;

int sm6125_gpu_enable_power_and_clocks(const sm6125_gpu_bus_t *bus, sm6125_gpu_probe_result_t *out);
int sm6125_gpu_probe_hardware(const sm6125_gpu_bus_t *bus, sm6125_gpu_probe_result_t *out);

/*
 * Sonda incremental para isolar hangs de bus no EL1 (um estagio por boot):
 *   1 = somente leitura do voto GCC (nao toca GPUCC);
 *   2 = estagio 1 + voto GCC (GPLL0/GPLL0_DIV) e so entao le CX/GX_GDSCR e GPUCC_AHB_CBCR;
 *   3 = estagio 2 + religa somente o CX_GDSCR (GX e CBCRs intocados).
 * Retorna 0 em sucesso, -1 em argumento invalido ou timeout do CX (out->err = -13).
 */
int sm6125_gpu_probe_stage(const sm6125_gpu_bus_t *bus, uint32_t stage, sm6125_gpu_probe_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SM6125_GPU_PROBE_H */
