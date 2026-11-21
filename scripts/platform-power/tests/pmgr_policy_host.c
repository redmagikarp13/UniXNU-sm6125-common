// SPDX-License-Identifier: MIT
// Host unit tests for SM6125 PMGR boot-arg mode selection policy.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "Sm6125PlatformPowerPolicy.h"

static unsigned checks;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #cond);       \
      exit(1);                                                               \
    }                                                                        \
    ++checks;                                                                \
  } while (0)

static void returns_off_when_no_args_present(void)
{
  CHECK(sm6125_pmgr_select_mode(false, 0u, false, 0u) == SM6125_PMGR_MODE_OFF);
  CHECK(sm6125_pmgr_select_mode(false, 2u, false, 2u) == SM6125_PMGR_MODE_OFF);
}

static void accepts_sheng_pmgr_alone(void)
{
  CHECK(sm6125_pmgr_select_mode(true, 0u, false, 0u) == 0u);
  CHECK(sm6125_pmgr_select_mode(true, 1u, false, 0u) == 1u);
  CHECK(sm6125_pmgr_select_mode(true, 2u, false, 0u) == 2u);
  CHECK(sm6125_pmgr_select_mode(true, 3u, false, 0u) == 3u);
}

static void accepts_pmgr_alone(void)
{
  CHECK(sm6125_pmgr_select_mode(false, 0u, true, 0u) == 0u);
  CHECK(sm6125_pmgr_select_mode(false, 0u, true, 1u) == 1u);
  CHECK(sm6125_pmgr_select_mode(false, 0u, true, 2u) == 2u);
  CHECK(sm6125_pmgr_select_mode(false, 0u, true, 3u) == 3u);
}

static void accepts_matching_sheng_pmgr_and_pmgr(void)
{
  CHECK(sm6125_pmgr_select_mode(true, 0u, true, 0u) == 0u);
  CHECK(sm6125_pmgr_select_mode(true, 2u, true, 2u) == 2u);
}

static void rejects_conflicting_or_out_of_range_modes(void)
{
  CHECK(sm6125_pmgr_select_mode(true, 2u, true, 1u) == SM6125_PMGR_MODE_INVALID);
  CHECK(sm6125_pmgr_select_mode(true, 2u, true, 0u) == SM6125_PMGR_MODE_INVALID);
  CHECK(sm6125_pmgr_select_mode(true, 0u, true, 2u) == SM6125_PMGR_MODE_INVALID);
  CHECK(sm6125_pmgr_select_mode(true, 4u, false, 0u) == SM6125_PMGR_MODE_INVALID);
  CHECK(sm6125_pmgr_select_mode(false, 0u, true, 99u) == SM6125_PMGR_MODE_INVALID);
  CHECK(sm6125_pmgr_select_mode(true, 0xFFFFFFFFu, false, 0u) == SM6125_PMGR_MODE_INVALID);
  CHECK(sm6125_pmgr_select_mode(false, 0u, true, 0xFFFFFFFFu) == SM6125_PMGR_MODE_INVALID);
}

int main(void)
{
  returns_off_when_no_args_present();
  accepts_sheng_pmgr_alone();
  accepts_pmgr_alone();
  accepts_matching_sheng_pmgr_and_pmgr();
  rejects_conflicting_or_out_of_range_modes();
  printf("checks %u\n", checks);
  return 0;
}
