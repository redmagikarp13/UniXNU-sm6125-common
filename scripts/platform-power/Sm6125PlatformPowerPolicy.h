// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
  SM6125_PMGR_MODE_OFF = 0u,
  SM6125_PMGR_MODE_UNPUBLISHED = 1u,
  SM6125_PMGR_MODE_PUBLISH = 2u,
  SM6125_PMGR_MODE_REFUSAL_TEST = 3u,
  SM6125_PMGR_MODE_INVALID = 0xFFFFFFFFu,
};

static inline uint32_t sm6125_pmgr_select_mode(
    bool has_sheng_pmgr,
    uint32_t sheng_pmgr,
    bool has_pmgr,
    uint32_t pmgr)
{
  if (!has_sheng_pmgr && !has_pmgr) {
    return SM6125_PMGR_MODE_OFF;
  }
  if (has_sheng_pmgr && sheng_pmgr > SM6125_PMGR_MODE_REFUSAL_TEST) {
    return SM6125_PMGR_MODE_INVALID;
  }
  if (has_pmgr && pmgr > SM6125_PMGR_MODE_REFUSAL_TEST) {
    return SM6125_PMGR_MODE_INVALID;
  }
  if (has_sheng_pmgr && has_pmgr && sheng_pmgr != pmgr) {
    return SM6125_PMGR_MODE_INVALID;
  }
  return has_pmgr ? pmgr : sheng_pmgr;
}
