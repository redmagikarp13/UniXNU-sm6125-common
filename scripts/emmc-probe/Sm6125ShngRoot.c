// SPDX-License-Identifier: MIT
#include "Sm6125ShngRoot.h"

static uint32_t shng_rd_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
         ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static uint64_t shng_rd_le64(const uint8_t *p) {
  return (uint64_t)shng_rd_le32(p) | ((uint64_t)shng_rd_le32(p + 4u) << 32u);
}

static void shng_zero(void *ptr, size_t len) {
  uint8_t *p = (uint8_t *)ptr;
  for (size_t i = 0u; i < len; i++) {
    p[i] = 0u;
  }
}

static void shng_copy(uint8_t *dst, const uint8_t *src, size_t len) {
  for (size_t i = 0u; i < len; i++) {
    dst[i] = src[i];
  }
}

static bool shng_all_zero(const uint8_t *p, size_t len) {
  uint8_t acc = 0u;
  for (size_t i = 0u; i < len; i++) {
    acc |= p[i];
  }
  return acc == 0u;
}

static bool shng_names_equal(const char a[32], const char b[32]) {
  for (size_t i = 0u; i < 32u; i++) {
    if (a[i] != b[i]) {
      return false;
    }
    if (a[i] == '\0') {
      return true;
    }
  }
  return true;
}

static bool shng_name_valid(const uint8_t raw[32], char out_name[32]) {
  shng_zero(out_name, 32u);
  if (raw[0] == 0u) {
    return false;
  }
  size_t nul_pos = 32u;
  for (size_t i = 0u; i < 32u; i++) {
    if (raw[i] == 0u) {
      nul_pos = i;
      break;
    }
    if (raw[i] < 0x20u || raw[i] > 0x7Eu) {
      return false;
    }
    out_name[i] = (char)raw[i];
  }
  if (nul_pos == 0u || nul_pos >= 32u) {
    return false;
  }
  /* Require trailing bytes after first NUL in the 32-byte field to be zero. */
  return shng_all_zero(raw + nul_pos, 32u - nul_pos);
}

uint32_t sm6125_emmc_block_select_mode(bool has_arg, uint32_t arg_value) {
  if (!has_arg || arg_value == 0u) {
    return SM6125_EMMC_BLOCK_MODE_OFF;
  }
  if (arg_value == 1u) {
    return SM6125_EMMC_BLOCK_MODE_DIAG;
  }
  if (arg_value == 2u) {
    return SM6125_EMMC_BLOCK_MODE_PUBLISH;
  }
  return SM6125_EMMC_BLOCK_MODE_INVALID;
}

int32_t sm6125_shngroot_parse(
    const uint8_t header[SM6125_SHNGROOT_HEADER_BYTES],
    uint64_t partition_bytes,
    sm6125_sha256_fn sha256_fn,
    sm6125_shngroot_manifest_t *out) {
  if (!header || !sha256_fn || !out) {
    return SM6125_SHNGROOT_RC_BAD_ARG;
  }
  shng_zero(out, sizeof(*out));

  static const uint8_t kMagic[8] = {'S', 'H', 'N', 'G', 'R', 'O', 'O', 'T'};
  for (size_t i = 0u; i < 8u; i++) {
    if (header[i] != kMagic[i]) {
      return SM6125_SHNGROOT_RC_BAD_MAGIC;
    }
  }

  const uint32_t version = shng_rd_le32(header + 8u);
  if (version != 1u && version != 2u) {
    return SM6125_SHNGROOT_RC_BAD_VERSION;
  }

  const uint32_t count = shng_rd_le32(header + 12u);
  if (count == 0u || count > SM6125_SHNGROOT_MAX_ENTRIES) {
    return SM6125_SHNGROOT_RC_BAD_COUNT;
  }

  const uint64_t image_bytes = shng_rd_le64(header + 16u);
  if (image_bytes < SM6125_SHNGROOT_MIN_OFFSET ||
      (image_bytes & 511ULL) != 0ULL ||
      image_bytes > partition_bytes) {
    return SM6125_SHNGROOT_RC_BAD_IMAGE_SIZE;
  }

  uint8_t calc_hash[32];
  shng_zero(calc_hash, sizeof(calc_hash));
  sha256_fn(header, 4064u, calc_hash);
  uint8_t diff = 0u;
  for (size_t i = 0u; i < 32u; i++) {
    diff |= (uint8_t)(calc_hash[i] ^ header[4064u + i]);
  }
  if (diff != 0u) {
    return SM6125_SHNGROOT_RC_BAD_HASH;
  }

  const size_t entries_end = 24u + (size_t)count * 128u;
  if (!shng_all_zero(header + entries_end, 4064u - entries_end)) {
    return SM6125_SHNGROOT_RC_BAD_ENTRY;
  }

  uint32_t win_count = 0u;
  uint32_t obj_count = 0u;

  for (uint32_t i = 0u; i < count; i++) {
    const uint8_t *row = header + 24u + (size_t)i * 128u;
    sm6125_shngroot_entry_t *ent = &out->entries[i];

    if (!shng_name_valid(row, ent->name)) {
      shng_zero(out, sizeof(*out));
      return SM6125_SHNGROOT_RC_BAD_ENTRY;
    }
    if (!shng_all_zero(row + 88u, 40u)) {
      shng_zero(out, sizeof(*out));
      return SM6125_SHNGROOT_RC_BAD_ENTRY;
    }

    ent->offset = shng_rd_le64(row + 32u);
    ent->size = shng_rd_le64(row + 40u);
    ent->block_size = shng_rd_le32(row + 48u);
    ent->flags = shng_rd_le32(row + 52u);
    shng_copy(ent->sha256, row + 56u, 32u);

    if (ent->offset < SM6125_SHNGROOT_MIN_OFFSET ||
        ent->size == 0ULL ||
        ent->offset > image_bytes ||
        ent->size > image_bytes - ent->offset) {
      shng_zero(out, sizeof(*out));
      return SM6125_SHNGROOT_RC_BAD_ENTRY;
    }

    if (ent->flags == SM6125_SHNGROOT_FLAG_RO_WINDOW) {
      if (ent->block_size != 512u ||
          (ent->offset & 511ULL) != 0ULL ||
          (ent->size & 511ULL) != 0ULL) {
        shng_zero(out, sizeof(*out));
        return SM6125_SHNGROOT_RC_BAD_ENTRY;
      }
      win_count++;
      if (win_count > SM6125_SHNGROOT_MAX_WINDOWS) {
        shng_zero(out, sizeof(*out));
        return SM6125_SHNGROOT_RC_BAD_COUNT;
      }
    } else if (ent->flags == SM6125_SHNGROOT_FLAG_OBJECT) {
      if (version < 2u ||
          ent->block_size != 4096u ||
          (ent->offset & 4095ULL) != 0ULL) {
        shng_zero(out, sizeof(*out));
        return SM6125_SHNGROOT_RC_BAD_ENTRY;
      }
      obj_count++;
      if (obj_count > SM6125_SHNGROOT_MAX_OBJECTS) {
        shng_zero(out, sizeof(*out));
        return SM6125_SHNGROOT_RC_BAD_COUNT;
      }
    } else {
      shng_zero(out, sizeof(*out));
      return SM6125_SHNGROOT_RC_BAD_ENTRY;
    }

    for (uint32_t j = 0u; j < i; j++) {
      const sm6125_shngroot_entry_t *prev = &out->entries[j];
      if (shng_names_equal(prev->name, ent->name)) {
        shng_zero(out, sizeof(*out));
        return SM6125_SHNGROOT_RC_DUPLICATE_NAME;
      }
      const uint64_t a0 = prev->offset;
      const uint64_t a1 = prev->offset + prev->size;
      const uint64_t b0 = ent->offset;
      const uint64_t b1 = ent->offset + ent->size;
      if (a0 < b1 && b0 < a1) {
        shng_zero(out, sizeof(*out));
        return SM6125_SHNGROOT_RC_OVERLAP;
      }
    }
  }

  if (win_count == 0u) {
    shng_zero(out, sizeof(*out));
    return SM6125_SHNGROOT_RC_BAD_COUNT;
  }

  out->version = version;
  out->entry_count = count;
  out->window_count = win_count;
  out->object_count = obj_count;
  out->image_bytes = image_bytes;
  shng_copy(out->header_sha256, calc_hash, 32u);
  return SM6125_SHNGROOT_RC_OK;
}
