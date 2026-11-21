// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  SM6125_EMMC_BLOCK_MODE_OFF = 0u,
  SM6125_EMMC_BLOCK_MODE_DIAG = 1u,
  SM6125_EMMC_BLOCK_MODE_PUBLISH = 2u,
  SM6125_EMMC_BLOCK_MODE_INVALID = 0xFFFFFFFFu,
};

enum {
  SM6125_SHNGROOT_HEADER_BYTES = 4096u,
  SM6125_SHNGROOT_MAX_ENTRIES = 8u,
  SM6125_SHNGROOT_MAX_WINDOWS = 4u,
  SM6125_SHNGROOT_MAX_OBJECTS = 4u,
  SM6125_SHNGROOT_FLAG_RO_WINDOW = 1u,
  SM6125_SHNGROOT_FLAG_OBJECT = 3u,
  SM6125_SHNGROOT_MIN_OFFSET = 0x100000ULL,
};

enum {
  SM6125_SHNGROOT_RC_OK = 0,
  SM6125_SHNGROOT_RC_BAD_ARG = -1,
  SM6125_SHNGROOT_RC_BAD_MAGIC = -2,
  SM6125_SHNGROOT_RC_BAD_VERSION = -3,
  SM6125_SHNGROOT_RC_BAD_COUNT = -4,
  SM6125_SHNGROOT_RC_BAD_HASH = -5,
  SM6125_SHNGROOT_RC_BAD_IMAGE_SIZE = -6,
  SM6125_SHNGROOT_RC_BAD_ENTRY = -7,
  SM6125_SHNGROOT_RC_OVERLAP = -8,
  SM6125_SHNGROOT_RC_DUPLICATE_NAME = -9,
};

typedef void (*sm6125_sha256_fn)(const uint8_t *data, size_t len, uint8_t out_digest[32]);

typedef struct {
  char name[32];
  uint64_t offset;
  uint64_t size;
  uint32_t block_size;
  uint32_t flags;
  uint8_t sha256[32];
} sm6125_shngroot_entry_t;

typedef struct {
  uint32_t version;
  uint32_t entry_count;
  uint32_t window_count;
  uint32_t object_count;
  uint64_t image_bytes;
  uint8_t header_sha256[32];
  sm6125_shngroot_entry_t entries[SM6125_SHNGROOT_MAX_ENTRIES];
} sm6125_shngroot_manifest_t;

uint32_t sm6125_emmc_block_select_mode(bool has_arg, uint32_t arg_value);

int32_t sm6125_shngroot_parse(
    const uint8_t header[SM6125_SHNGROOT_HEADER_BYTES],
    uint64_t partition_bytes,
    sm6125_sha256_fn sha256_fn,
    sm6125_shngroot_manifest_t *out);

#ifdef __cplusplus
}
#endif
