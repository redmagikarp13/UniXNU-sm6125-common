// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef ARMCPU_TIMER_H
#define ARMCPU_TIMER_H

#include <stddef.h>
#include <stdint.h>

#define ARMCPU_DEFAULT_VIRTUAL_TIMER_PPI 27u

/* Extract virtual timer GIC INTID from the 3rd tuple (index 6..8) of arm,armv8-timer `interrupts`. */
static inline uint32_t armcpu_virtual_timer_ppi_from_dt(const uint32_t *cells, size_t cell_count)
{
    if (cells == NULL || cell_count < 9 || (cell_count % 3) != 0) {
        return ARMCPU_DEFAULT_VIRTUAL_TIMER_PPI;
    }
    if (cells[6] != 1u || cells[7] >= 16u) {
        return ARMCPU_DEFAULT_VIRTUAL_TIMER_PPI;
    }
    return 16u + cells[7];
}

/* Enable SGI 0 (kernel IPI) and the validated virtual timer PPI bit without shifting out of range. */
static inline uint32_t armcpu_sgi_ppi_enable_mask(uint32_t timer_ppi)
{
    const uint32_t ppi = (timer_ppi >= 16u && timer_ppi < 32u) ? timer_ppi : ARMCPU_DEFAULT_VIRTUAL_TIMER_PPI;
    return 1u | (1u << ppi);
}

#endif
