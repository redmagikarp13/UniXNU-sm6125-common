// SPDX-License-Identifier: BSD-2-Clause-Patent
// Constantes MMIO verificadas para SM6125 (ginkgo) GPU: GCC, GPUCC, Adreno 610 RBBM e KGSL SMMU v500.
#ifndef SM6125_GPU_BOARD_H
#define SM6125_GPU_BOARD_H

#include <stdint.h>

/* GCC APCS branch vote window (0x01400000 + 0x79000 = 0x01479000, size 0x1000) — gcc-sm6125.c (0x79004 bits 15, 16). */
#define SM6125_GCC_GPU_VOTE_BASE        0x01479000u
#define SM6125_GCC_GPU_VOTE_SIZE        0x00001000u
#define SM6125_GCC_GPU_VOTE_REG         0x0004u
#define SM6125_GCC_GPU_GPLL0_BIT        (1u << 15)
#define SM6125_GCC_GPU_GPLL0_DIV_BIT    (1u << 16)

/* GPUCC (0x05990000, size 0x9000) — stock-fdt.dts + gpucc-sm6125.c. */
#define SM6125_GPUCC_BASE               0x05990000u
#define SM6125_GPUCC_SIZE               0x00009000u
#define SM6125_GPUCC_CXO_AON_CBCR       0x1004u
#define SM6125_GPUCC_GX_GDSCR           0x100Cu
#define SM6125_GPUCC_GX_GFX3D_CBCR      0x1054u
#define SM6125_GPUCC_CX_GDSCR           0x106Cu
#define SM6125_GPUCC_AHB_CBCR           0x1078u
#define SM6125_GPUCC_CRC_AHB_CBCR       0x107Cu
#define SM6125_GPUCC_CX_APB_CBCR        0x1088u
#define SM6125_GPUCC_CX_SNOC_DVM_CBCR   0x108Cu
#define SM6125_GPUCC_CXO_CBCR           0x109Cu
#define SM6125_GPUCC_CX_GFX3D_CBCR      0x10A4u
#define SM6125_GPUCC_HLOS1_SMMU_CBCR    0x5000u

#define SM6125_CBCR_CLK_ENABLE          (1u << 0)
#define SM6125_CBCR_CLK_OFF             (1u << 31)
#define SM6125_GDSCR_SW_COLLAPSE        (1u << 0)
#define SM6125_GDSCR_PWR_ON             (1u << 31)

/* Adreno 610 KGSL 3D0 (0x05900000, size 0x90000) — stock-fdt.dts + a6xx.xml (dword * 4). */
#define SM6125_GPU_BASE                 0x05900000u
#define SM6125_GPU_SIZE                 0x00010000u
#define SM6125_A6XX_RBBM_INT_0_MASK     0x00E0u /* dword 0x0038 */
#define SM6125_A6XX_RBBM_SP_HYST_CNT    0x0108u /* dword 0x0042 */
#define SM6125_A6XX_RBBM_SW_RESET_CMD   0x010Cu /* dword 0x0043 */
#define SM6125_A6XX_RBBM_CLOCK_CNTL     0x02B8u /* dword 0x00AE */
#define SM6125_A6XX_RBBM_INT_0_STATUS   0x0804u /* dword 0x0201 */
#define SM6125_A6XX_RBBM_STATUS         0x0840u /* dword 0x0210 */
#define SM6125_A6XX_RBBM_STATUS1        0x0844u /* dword 0x0211 */

/* KGSL SMMU v500 (0x059A0000, size 0x10000) — stock-fdt.dts. */
#define SM6125_GPU_SMMU_BASE            0x059A0000u
#define SM6125_GPU_SMMU_SIZE            0x00010000u
#define SM6125_SMMU_IDR0                0x0020u
#define SM6125_SMMU_IDR1                0x0024u
#define SM6125_SMMU_IDR7                0x003Cu

#endif /* SM6125_GPU_BOARD_H */
