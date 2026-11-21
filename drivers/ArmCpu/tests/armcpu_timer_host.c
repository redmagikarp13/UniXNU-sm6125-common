// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests for device-tree virtual timer PPI resolution and GIC SGI/PPI mask generation.
#include <stdio.h>
#include <stdlib.h>
#include "../ArmCpuSleep.h"
#include "../ArmCpuTimer.h"

static unsigned checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #x); exit(1); } ++checks; } while (0)

static void parses_sm6125_virtual_timer_ppi19(void)
{
    const uint32_t cells[12] = {
        1u, 1u, 0xf08u,
        1u, 2u, 0xf08u,
        1u, 3u, 0xf08u,
        1u, 0u, 0xf08u,
    };
    CHECK(armcpu_virtual_timer_ppi_from_dt(cells, 12) == 19u);
}

static void parses_standard_arm_virtual_timer_ppi27(void)
{
    const uint32_t cells[12] = {
        1u, 13u, 0x8u,
        1u, 14u, 0x8u,
        1u, 11u, 0x8u,
        1u, 10u, 0x8u,
    };
    CHECK(armcpu_virtual_timer_ppi_from_dt(cells, 12) == 27u);
}

static void parses_nine_cells_without_hyp_timer(void)
{
    const uint32_t cells[9] = {
        1u, 1u, 0xf08u,
        1u, 2u, 0xf08u,
        1u, 3u, 0xf08u,
    };
    CHECK(armcpu_virtual_timer_ppi_from_dt(cells, 9) == 19u);
}

static void falls_back_on_null_or_short_or_unaligned(void)
{
    const uint32_t short_cells[12] = {
        1u, 1u, 0xf08u,
        1u, 2u, 0xf08u,
        1u, 3u, 0xf08u,
        1u, 0u, 0xf08u,
    };
    CHECK(armcpu_virtual_timer_ppi_from_dt(NULL, 0) == 27u);
    CHECK(armcpu_virtual_timer_ppi_from_dt(NULL, 12) == 27u);
    CHECK(armcpu_virtual_timer_ppi_from_dt(short_cells, 6) == 27u);
    CHECK(armcpu_virtual_timer_ppi_from_dt(short_cells, 8) == 27u);
    CHECK(armcpu_virtual_timer_ppi_from_dt(short_cells, 10) == 27u);
    CHECK(armcpu_virtual_timer_ppi_from_dt(short_cells, 11) == 27u);
}

static void falls_back_on_non_ppi_or_invalid_ppi_index(void)
{
    const uint32_t spi_third[9] = {
        1u, 1u, 0xf08u,
        1u, 2u, 0xf08u,
        0u, 3u, 0xf08u,
    };
    const uint32_t type2_third[9] = {
        1u, 1u, 0xf08u,
        1u, 2u, 0xf08u,
        2u, 3u, 0xf08u,
    };
    const uint32_t out_of_range_16[9] = {
        1u, 1u, 0xf08u,
        1u, 2u, 0xf08u,
        1u, 16u, 0xf08u,
    };
    const uint32_t out_of_range_27[9] = {
        1u, 1u, 0xf08u,
        1u, 2u, 0xf08u,
        1u, 27u, 0xf08u,
    };
    CHECK(armcpu_virtual_timer_ppi_from_dt(spi_third, 9) == 27u);
    CHECK(armcpu_virtual_timer_ppi_from_dt(type2_third, 9) == 27u);
    CHECK(armcpu_virtual_timer_ppi_from_dt(out_of_range_16, 9) == 27u);
    CHECK(armcpu_virtual_timer_ppi_from_dt(out_of_range_27, 9) == 27u);
}

static void builds_sgi0_and_ppi_enable_mask(void)
{
    CHECK(armcpu_sgi_ppi_enable_mask(19u) == 0x00080001u);
    CHECK(armcpu_sgi_ppi_enable_mask(27u) == 0x08000001u);
    CHECK(armcpu_sgi_ppi_enable_mask(16u) == 0x00010001u);
    CHECK(armcpu_sgi_ppi_enable_mask(31u) == 0x80000001u);
    CHECK(armcpu_sgi_ppi_enable_mask(0u) == 0x08000001u);
    CHECK(armcpu_sgi_ppi_enable_mask(15u) == 0x08000001u);
    CHECK(armcpu_sgi_ppi_enable_mask(32u) == 0x08000001u);
    CHECK(armcpu_sgi_ppi_enable_mask(1023u) == 0x08000001u);
}

static void integrates_with_sleep_wake_decision(void)
{
    const uint32_t cells[12] = {
        1u, 1u, 0xf08u,
        1u, 2u, 0xf08u,
        1u, 3u, 0xf08u,
        1u, 0u, 0xf08u,
    };
    const uint32_t ppi = armcpu_virtual_timer_ppi_from_dt(cells, 12);
    CHECK(armcpu_sleep_decide(19u, 0, ppi) == ArmcpuWakeTimer);
    CHECK(armcpu_sleep_decide(27u, 0, ppi) == ArmcpuWakeNone);
    CHECK(armcpu_sleep_decide(19u, 1, ppi) == ArmcpuWakeSource);
}

int main(void)
{
    parses_sm6125_virtual_timer_ppi19();
    parses_standard_arm_virtual_timer_ppi27();
    parses_nine_cells_without_hyp_timer();
    falls_back_on_null_or_short_or_unaligned();
    falls_back_on_non_ppi_or_invalid_ppi_index();
    builds_sgi0_and_ppi_enable_mask();
    integrates_with_sleep_wake_decision();
    printf("checks %u\n", checks);
    return 0;
}
