// SPDX-License-Identifier: BSD-2-Clause-Patent
// Habilitacao de clocks/GDSCs e leitura diagnostica de Adreno 610 + SMMU v500 (SM6125).
#include "Sm6125GpuProbe.h"
#include "Sm6125GpuBoard.h"

static int enable_cbcr(const sm6125_gpu_bus_t *bus, uintptr_t base, uint32_t reg)
{
  uint32_t v = bus->rd32(bus->ctx, base, reg);
  v |= SM6125_CBCR_CLK_ENABLE;
  bus->wr32(bus->ctx, base, reg, v);
  for (unsigned i = 0; i < 500u; ++i) {
    if ((bus->rd32(bus->ctx, base, reg) & SM6125_CBCR_CLK_OFF) == 0u) {
      return 0;
    }
    if (bus->delay_us) bus->delay_us(bus->ctx, 10u);
  }
  return -1;
}

static int enable_gdsc(const sm6125_gpu_bus_t *bus, uint32_t reg, uint32_t *final_val)
{
  uint32_t v = bus->rd32(bus->ctx, bus->gpucc_va, reg);
  v &= ~SM6125_GDSCR_SW_COLLAPSE;
  bus->wr32(bus->ctx, bus->gpucc_va, reg, v);
  for (unsigned i = 0; i < 1000u; ++i) {
    uint32_t cur = bus->rd32(bus->ctx, bus->gpucc_va, reg);
    if (cur & SM6125_GDSCR_PWR_ON) {
      if (final_val) *final_val = cur;
      return 0;
    }
    if (bus->delay_us) bus->delay_us(bus->ctx, 10u);
  }
  if (final_val) *final_val = bus->rd32(bus->ctx, bus->gpucc_va, reg);
  return -1;
}

int sm6125_gpu_enable_power_and_clocks(const sm6125_gpu_bus_t *bus, sm6125_gpu_probe_result_t *out)
{
  if (!bus || !bus->rd32 || !bus->wr32 || !bus->gcc_va || !bus->gpucc_va || !out) {
    return -1;
  }

  out->step = 1;
  uint32_t vote = bus->rd32(bus->ctx, bus->gcc_va, SM6125_GCC_GPU_VOTE_REG);
  vote |= SM6125_GCC_GPU_GPLL0_BIT | SM6125_GCC_GPU_GPLL0_DIV_BIT;
  bus->wr32(bus->ctx, bus->gcc_va, SM6125_GCC_GPU_VOTE_REG, vote);
  if (bus->delay_us) bus->delay_us(bus->ctx, 20u);

  out->step = 2;
  (void)enable_cbcr(bus, bus->gpucc_va, SM6125_GPUCC_CXO_AON_CBCR);
  (void)enable_cbcr(bus, bus->gpucc_va, SM6125_GPUCC_CXO_CBCR);
  (void)enable_cbcr(bus, bus->gpucc_va, SM6125_GPUCC_AHB_CBCR);

  out->step = 3;
  if (enable_gdsc(bus, SM6125_GPUCC_CX_GDSCR, &out->cx_gdscr) != 0) {
    out->err = -13;
    return -1;
  }

  (void)enable_cbcr(bus, bus->gpucc_va, SM6125_GPUCC_CRC_AHB_CBCR);
  (void)enable_cbcr(bus, bus->gpucc_va, SM6125_GPUCC_CX_APB_CBCR);
  (void)enable_cbcr(bus, bus->gpucc_va, SM6125_GPUCC_CX_SNOC_DVM_CBCR);
  (void)enable_cbcr(bus, bus->gpucc_va, SM6125_GPUCC_CX_GFX3D_CBCR);
  out->cx_gfx3d_cbcr = bus->rd32(bus->ctx, bus->gpucc_va, SM6125_GPUCC_CX_GFX3D_CBCR);

  (void)enable_cbcr(bus, bus->gpucc_va, SM6125_GPUCC_HLOS1_SMMU_CBCR);

  out->step = 4;
  if (enable_gdsc(bus, SM6125_GPUCC_GX_GDSCR, &out->gx_gdscr) != 0) {
    out->err = -14;
    return -1;
  }
  (void)enable_cbcr(bus, bus->gpucc_va, SM6125_GPUCC_GX_GFX3D_CBCR);

  out->cx_gfx3d_cbcr = bus->rd32(bus->ctx, bus->gpucc_va, SM6125_GPUCC_CX_GFX3D_CBCR);
  out->gx_gfx3d_cbcr = bus->rd32(bus->ctx, bus->gpucc_va, SM6125_GPUCC_GX_GFX3D_CBCR);
  return 0;
}

int sm6125_gpu_probe_stage(const sm6125_gpu_bus_t *bus, uint32_t stage, sm6125_gpu_probe_result_t *out)
{
  if (!bus || !bus->rd32 || !bus->wr32 || !bus->gcc_va || !bus->gpucc_va || !out ||
      stage < 1u || stage > 3u) {
    return -1;
  }

  /* GPUCC so responde (sem bus hang no EL1) depois do voto GCC de GPLL0/GPLL0_DIV. */
  out->step = 1;
  out->err = 0;
  out->gcc_vote_before = bus->rd32(bus->ctx, bus->gcc_va, SM6125_GCC_GPU_VOTE_REG);
  if (stage == 1u) {
    return 0;
  }

  out->step = 2;
  bus->wr32(bus->ctx, bus->gcc_va, SM6125_GCC_GPU_VOTE_REG,
            out->gcc_vote_before | SM6125_GCC_GPU_GPLL0_BIT | SM6125_GCC_GPU_GPLL0_DIV_BIT);
  if (bus->delay_us) bus->delay_us(bus->ctx, 20u);
  out->gcc_vote_after = bus->rd32(bus->ctx, bus->gcc_va, SM6125_GCC_GPU_VOTE_REG);
  out->cx_gdscr = bus->rd32(bus->ctx, bus->gpucc_va, SM6125_GPUCC_CX_GDSCR);
  out->gx_gdscr = bus->rd32(bus->ctx, bus->gpucc_va, SM6125_GPUCC_GX_GDSCR);
  out->ahb_cbcr = bus->rd32(bus->ctx, bus->gpucc_va, SM6125_GPUCC_AHB_CBCR);
  if (stage == 2u) {
    return 0;
  }

  out->step = 3;
  if (enable_gdsc(bus, SM6125_GPUCC_CX_GDSCR, &out->cx_gdscr) != 0) {
    out->err = -13;
    return -1;
  }
  return 0;
}

int sm6125_gpu_probe_hardware(const sm6125_gpu_bus_t *bus, sm6125_gpu_probe_result_t *out)
{
  if (!bus || !bus->gpu_va || !bus->smmu_va || !out) {
    return -1;
  }
  if (sm6125_gpu_enable_power_and_clocks(bus, out) != 0) {
    return -1;
  }

  out->step = 5;
  out->smmu_idr0 = bus->rd32(bus->ctx, bus->smmu_va, SM6125_SMMU_IDR0);
  out->smmu_idr1 = bus->rd32(bus->ctx, bus->smmu_va, SM6125_SMMU_IDR1);
  out->smmu_idr7 = bus->rd32(bus->ctx, bus->smmu_va, SM6125_SMMU_IDR7);

  out->step = 6;
  out->rbbm_status = bus->rd32(bus->ctx, bus->gpu_va, SM6125_A6XX_RBBM_STATUS);
  out->rbbm_int0 = bus->rd32(bus->ctx, bus->gpu_va, SM6125_A6XX_RBBM_INT_0_STATUS);
  out->rbbm_clk_cntl = bus->rd32(bus->ctx, bus->gpu_va, SM6125_A6XX_RBBM_CLOCK_CNTL);

  /* Teste seguro de escrita/leitura no RBBM_SP_HYST_CNT restaurando valor original. */
  out->hyst_before = bus->rd32(bus->ctx, bus->gpu_va, SM6125_A6XX_RBBM_SP_HYST_CNT);
  bus->wr32(bus->ctx, bus->gpu_va, SM6125_A6XX_RBBM_SP_HYST_CNT, 0x12345678u);
  out->hyst_readback = bus->rd32(bus->ctx, bus->gpu_va, SM6125_A6XX_RBBM_SP_HYST_CNT);
  bus->wr32(bus->ctx, bus->gpu_va, SM6125_A6XX_RBBM_SP_HYST_CNT, out->hyst_before);

  out->step = 7;
  out->err = 0;
  return 0;
}
