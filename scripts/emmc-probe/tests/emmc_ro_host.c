// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Sm6125EmmcReadOnly.h"

#define MAX_EVENTS 128

enum {
  SPACE_GCC = 1,
  SPACE_SDHCI = 2,
};

typedef struct {
  int space;
  uint32_t offset;
  uint8_t width;
} mmio_read_event_t;

typedef struct {
  uint32_t bcr;
  uint32_t apps_cbcr;
  uint32_t ahb_cbcr;
  uint32_t ice_cbcr;
  uint32_t ice_cmd;
  uint32_t ice_cfg;
  uint32_t apps_cmd;
  uint32_t apps_cfg;
  uint32_t apps_m;
  uint32_t apps_n;
  uint32_t apps_d;
} sim_gcc_t;

typedef struct {
  uint32_t mci_version;
  uint16_t host_version;
  uint32_t cap0;
  uint32_t cap1;
  uint32_t present_state;
  uint16_t clock_control;
  uint8_t power_control;
  uint8_t host_control;
  uint16_t host_control2;
  uint8_t software_reset;
  uint32_t int_status;
  uint32_t int_enable;
  uint32_t signal_enable;
  uint32_t vendor_spec;
  uint32_t vendor_spec3;
  uint32_t vendor_cap0;
  uint32_t pwrctl_status;
  uint32_t pwrctl_mask;
  uint32_t generics;
  uint32_t dll_config;
  uint32_t dll_config_2;
  uint32_t dll_status;
  uint32_t ddr_config;
} sim_sdhci_t;

static sim_gcc_t g_gcc;
static sim_sdhci_t g_sdhci;
static mmio_read_event_t g_events[MAX_EVENTS];
static size_t g_event_count = 0;
static size_t g_sdhci_read_count = 0;

static void reset_sim(void) {
  memset(&g_gcc, 0, sizeof(g_gcc));
  memset(&g_sdhci, 0, sizeof(g_sdhci));
  memset(g_events, 0, sizeof(g_events));
  g_event_count = 0;
  g_sdhci_read_count = 0;
}

static void record_read(uintptr_t base, uint32_t offset, uint8_t width) {
  int space = 0;
  if (base == (uintptr_t)&g_gcc) {
    space = SPACE_GCC;
  } else if (base == (uintptr_t)&g_sdhci) {
    space = SPACE_SDHCI;
    g_sdhci_read_count++;
  } else {
    fprintf(stderr, "Unexpected MMIO base %p offset 0x%x width %u\n", (void *)base, offset, width);
    abort();
  }
  assert(g_event_count < MAX_EVENTS);
  g_events[g_event_count++] = (mmio_read_event_t){
      .space = space,
      .offset = offset,
      .width = width,
  };
}

static uint8_t sim_read8(uintptr_t base, uint32_t offset) {
  record_read(base, offset, 8);
  assert(base == (uintptr_t)&g_sdhci);
  switch (offset) {
    case SM6125_SDHCI_HOST_CONTROL:
      return g_sdhci.host_control;
    case SM6125_SDHCI_POWER_CONTROL:
      return g_sdhci.power_control;
    case SM6125_SDHCI_SOFTWARE_RESET:
      return g_sdhci.software_reset;
    default:
      fprintf(stderr, "Disallowed 8-bit SDHCI read offset 0x%x\n", offset);
      abort();
  }
}

static uint16_t sim_read16(uintptr_t base, uint32_t offset) {
  record_read(base, offset, 16);
  assert(base == (uintptr_t)&g_sdhci);
  switch (offset) {
    case SM6125_SDHCI_CLOCK_CONTROL:
      return g_sdhci.clock_control;
    case SM6125_SDHCI_HOST_CONTROL2:
      return g_sdhci.host_control2;
    case SM6125_SDHCI_HOST_VERSION:
      return g_sdhci.host_version;
    default:
      fprintf(stderr, "Disallowed 16-bit SDHCI read offset 0x%x\n", offset);
      abort();
  }
}

static uint32_t sim_read32(uintptr_t base, uint32_t offset) {
  record_read(base, offset, 32);
  if (base == (uintptr_t)&g_gcc) {
    switch (offset) {
      case SM6125_GCC_SDCC1_BCR_OFF:
        return g_gcc.bcr;
      case SM6125_GCC_SDCC1_APPS_CBCR_OFF:
        return g_gcc.apps_cbcr;
      case SM6125_GCC_SDCC1_AHB_CBCR_OFF:
        return g_gcc.ahb_cbcr;
      case SM6125_GCC_SDCC1_ICE_CBCR_OFF:
        return g_gcc.ice_cbcr;
      case SM6125_GCC_SDCC1_ICE_CMD_RCGR_OFF:
        return g_gcc.ice_cmd;
      case SM6125_GCC_SDCC1_ICE_CFG_RCGR_OFF:
        return g_gcc.ice_cfg;
      case SM6125_GCC_SDCC1_APPS_CMD_RCGR_OFF:
        return g_gcc.apps_cmd;
      case SM6125_GCC_SDCC1_APPS_CFG_RCGR_OFF:
        return g_gcc.apps_cfg;
      case SM6125_GCC_SDCC1_APPS_M_OFF:
        return g_gcc.apps_m;
      case SM6125_GCC_SDCC1_APPS_N_OFF:
        return g_gcc.apps_n;
      case SM6125_GCC_SDCC1_APPS_D_OFF:
        return g_gcc.apps_d;
      default:
        fprintf(stderr, "Disallowed 32-bit GCC read offset 0x%x\n", offset);
        abort();
    }
  }
  assert(base == (uintptr_t)&g_sdhci);
  switch (offset) {
    case SM6125_SDHCI_PRESENT_STATE:
      return g_sdhci.present_state;
    case SM6125_SDHCI_INT_STATUS:
      return g_sdhci.int_status;
    case SM6125_SDHCI_INT_ENABLE:
      return g_sdhci.int_enable;
    case SM6125_SDHCI_SIGNAL_ENABLE:
      return g_sdhci.signal_enable;
    case SM6125_SDHCI_CAPABILITIES:
      return g_sdhci.cap0;
    case SM6125_SDHCI_CAPABILITIES_1:
      return g_sdhci.cap1;
    case SM6125_SDHCI_V5_DLL_CONFIG:
      return g_sdhci.dll_config;
    case SM6125_SDHCI_V5_DLL_STATUS:
      return g_sdhci.dll_status;
    case SM6125_SDHCI_V5_VENDOR_SPEC:
      return g_sdhci.vendor_spec;
    case SM6125_SDHCI_V5_VENDOR_CAP0:
      return g_sdhci.vendor_cap0;
    case SM6125_SDHCI_V5_PWRCTL_STATUS:
      return g_sdhci.pwrctl_status;
    case SM6125_SDHCI_V5_PWRCTL_MASK:
      return g_sdhci.pwrctl_mask;
    case SM6125_SDHCI_V5_VENDOR_SPEC3:
      return g_sdhci.vendor_spec3;
    case SM6125_SDHCI_V5_DLL_CONFIG_2:
      return g_sdhci.dll_config_2;
    case SM6125_SDHCI_V5_DDR_CONFIG:
      return g_sdhci.ddr_config;
    case SM6125_SDHCI_V5_MCI_VERSION:
      return g_sdhci.mci_version;
    case SM6125_SDHCI_V5_GENERICS:
      return g_sdhci.generics;
    default:
      fprintf(stderr, "Disallowed 32-bit SDHCI read offset 0x%x\n", offset);
      abort();
  }
}

static const sm6125_mmio_ro_ops_t g_ops = {
    .read8 = sim_read8,
    .read16 = sim_read16,
    .read32 = sim_read32,
};

static int test_boot_arg_policy(void) {
  assert(sm6125_emmc_probe_select_mode(false, 1u) == SM6125_EMMC_PROBE_MODE_OFF);
  assert(sm6125_emmc_probe_select_mode(true, 0u) == SM6125_EMMC_PROBE_MODE_OFF);
  assert(sm6125_emmc_probe_select_mode(true, 1u) == SM6125_EMMC_PROBE_MODE_RO_SNAPSHOT);
  assert(sm6125_emmc_probe_select_mode(true, 2u) == SM6125_EMMC_PROBE_MODE_INVALID);
  assert(sm6125_emmc_probe_select_mode(true, 0xFFFFFFFFu) == SM6125_EMMC_PROBE_MODE_INVALID);
  return 5;
}

static int test_adt_translation_validation(void) {
  const uint64_t bus[3] = {0ULL, 0x01000000ULL, 0x0F000000ULL};
  uint64_t pa = 0;

  assert(sm6125_emmc_translate_region(bus, 0x00400000ULL, 0x001F0000ULL, 0x39000ULL, &pa));
  assert(pa == 0x01400000ULL);
  assert(sm6125_emmc_translate_region(bus, 0x03744000ULL, 0x1000ULL, 0x1000ULL, &pa));
  assert(pa == 0x04744000ULL);

  // Insufficient length, out-of-window, overflow, and zero bus physical base
  assert(!sm6125_emmc_translate_region(bus, 0x00400000ULL, 0x38000ULL, 0x39000ULL, &pa));
  assert(!sm6125_emmc_translate_region(bus, 0x03744000ULL, 0x0800ULL, 0x1000ULL, &pa));
  assert(!sm6125_emmc_translate_region(bus, 0x10000000ULL, 0x1000ULL, 0x1000ULL, &pa));
  assert(!sm6125_emmc_translate_region(bus, 0x00000000ULL, UINT64_MAX, 0x1000ULL, &pa));
  const uint64_t bad_bus[3] = {0ULL, 0ULL, 0x0F000000ULL};
  assert(!sm6125_emmc_translate_region(bad_bus, 0x00400000ULL, 0x001F0000ULL, 0x39000ULL, &pa));
  return 7;
}

static int test_gcc_gate_and_full_capture(void) {
  reset_sim();
  g_gcc = (sim_gcc_t){
      .bcr = 0x00000000u,
      .apps_cbcr = 0x00004221u,
      .ahb_cbcr = 0x00000001u,
      .ice_cbcr = 0x00004229u,
      .ice_cmd = 0x00000000u,
      .ice_cfg = 0x00000100u,
      .apps_cmd = 0x00000000u,
      .apps_cfg = 0x00000401u,
      .apps_m = 0x00000001u,
      .apps_n = 0x000000feu,
      .apps_d = 0x000000fdu,
  };
  g_sdhci = (sim_sdhci_t){
      .mci_version = 0x10000072u,
      .host_version = 0x7202u,
      .cap0 = 0x742dc8b2u,
      .cap1 = 0x0000808fu,
      .present_state = 0x01f800f0u,
      .clock_control = 0x0003u,
      .power_control = 0x0fu,
      .host_control = 0x20u,
      .host_control2 = 0x0008u,
      .software_reset = 0x00u,
      .int_status = 0x00000001u,
      .int_enable = 0x00000000u,
      .signal_enable = 0x00000000u,
      .vendor_spec = 0x00300a0cu,
      .vendor_spec3 = 0x00000000u,
      .vendor_cap0 = 0x00000000u,
      .pwrctl_status = 0x00000000u,
      .pwrctl_mask = 0x00000000u,
      .generics = 0x00f04003u,
      .dll_config = 0x6000642cu,
      .dll_config_2 = 0x00010800u,
      .dll_status = 0x00000000u,
      .ddr_config = 0x80040873u,
  };

  sm6125_emmc_ro_snapshot_t snap;
  memset(&snap, 0, sizeof(snap));
  snap.gcc_pa = 0x01400000ULL;
  snap.sdhci_pa = 0x04744000ULL;

  assert(sm6125_emmc_ro_read_gcc(&g_ops, (uintptr_t)&g_gcc, &snap));
  assert(snap.gcc_valid);
  assert(g_event_count == 11);
  assert(g_sdhci_read_count == 0);

  uint32_t gate_flags = 0;
  assert(sm6125_emmc_gcc_gate(snap.gcc_bcr, snap.gcc_apps_cbcr, snap.gcc_ahb_cbcr, snap.gcc_ice_cbcr, &gate_flags));
  snap.gate_flags = gate_flags;
  assert(gate_flags == 0x0fu);

  assert(sm6125_emmc_ro_read_sdhci(&g_ops, (uintptr_t)&g_sdhci, &snap));
  snap.err = SM6125_EMMC_RO_ERR_NONE;
  assert(snap.sdhci_valid);
  assert(g_sdhci_read_count == 23);
  assert(g_event_count == 34);

  char buf[640];
  size_t len = sm6125_emmc_ro_format_report(&snap, buf, sizeof(buf));
  assert(len < 512);
  assert(len == strlen(buf));
  assert(strstr(buf, "SM6125-EMMC-RO ok=1 err=none gate=0x0f gcc=0x01400000 sdhci=0x04744000\n") == buf);
  assert(strstr(buf, "GCC bcr=00000000 apps=00004221 ahb=00000001 ice=00004229") != NULL);
  assert(strstr(buf, "STD mci=10000072 sv=7202 cap0=742dc8b2 cap1=0000808f ps=01f800f0 clk=0003 pwr=0f hc=20 hc2=0008 rst=00") != NULL);
  assert(strstr(buf, "VND vs=00300a0c vs3=00000000 vc0=00000000 pst=00000000 pm=00000000 gen=00f04003 dll=6000642c dll2=00010800 dst=00000000 ddr=80040873") != NULL);
  return 10;
}

static int test_ice_off_still_allows_gate(void) {
  uint32_t gate_flags = 0;
  assert(sm6125_emmc_gcc_gate(0x0u, 0x00004221u, 0x00000001u, 0x80000000u, &gate_flags));
  assert(gate_flags == 0x07u);
  return 2;
}

static int test_clock_gated_blocks_sdhci_reads(void) {
  struct {
    uint32_t bcr;
    uint32_t apps;
    uint32_t ahb;
    uint32_t ice;
    uint32_t expected_flags;
  } cases[] = {
      {0x00000001u, 0x00004221u, 0x00000001u, 0x00004229u, 0x0eu},
      {0x00000000u, 0x00004221u, 0x80000000u, 0x00004229u, 0x0du},
      {0x00000000u, 0x00004221u, 0x00000000u, 0x00004229u, 0x0du},
      {0x00000000u, 0x80000001u, 0x00000001u, 0x00004229u, 0x0bu},
      {0x00000000u, 0x00000000u, 0x00000001u, 0x00004229u, 0x0bu},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    reset_sim();
    g_gcc.bcr = cases[i].bcr;
    g_gcc.apps_cbcr = cases[i].apps;
    g_gcc.ahb_cbcr = cases[i].ahb;
    g_gcc.ice_cbcr = cases[i].ice;

    sm6125_emmc_ro_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.gcc_pa = 0x01400000ULL;
    snap.sdhci_pa = 0x04744000ULL;

    assert(sm6125_emmc_ro_read_gcc(&g_ops, (uintptr_t)&g_gcc, &snap));
    uint32_t flags = 0;
    bool ok = sm6125_emmc_gcc_gate(snap.gcc_bcr, snap.gcc_apps_cbcr, snap.gcc_ahb_cbcr, snap.gcc_ice_cbcr, &flags);
    assert(!ok);
    assert(flags == cases[i].expected_flags);
    assert(g_sdhci_read_count == 0);
    assert(g_event_count == 11);

    snap.gate_flags = flags;
    snap.err = SM6125_EMMC_RO_ERR_CLOCK_GATED;
    char buf[640];
    size_t len = sm6125_emmc_ro_format_report(&snap, buf, sizeof(buf));
    assert(len < 512);
    assert(strstr(buf, "ok=0 err=clock-gated") != NULL);
    assert(strstr(buf, "GCC ") != NULL);
    assert(strstr(buf, "STD ") == NULL);
    assert(strstr(buf, "VND ") == NULL);
  }
  return 20;
}

static int test_format_bounds_and_worst_case(void) {
  sm6125_emmc_ro_snapshot_t worst;
  memset(&worst, 0xff, sizeof(worst));
  worst.err = SM6125_EMMC_RO_ERR_NONE;
  worst.gate_flags = 0x0fu;
  worst.gcc_valid = true;
  worst.sdhci_valid = true;

  char big[640];
  size_t len = sm6125_emmc_ro_format_report(&worst, big, sizeof(big));
  assert(len < 512);
  assert(len == strlen(big));

  char small16[16];
  memset(small16, 'A', sizeof(small16));
  size_t trunc16 = sm6125_emmc_ro_format_report(&worst, small16, sizeof(small16));
  assert(trunc16 == 15);
  assert(small16[15] == '\0');
  assert(strncmp(small16, big, 15) == 0);

  char tiny1[1] = {'X'};
  assert(sm6125_emmc_ro_format_report(&worst, tiny1, sizeof(tiny1)) == 0);
  assert(tiny1[0] == '\0');
  assert(sm6125_emmc_ro_format_report(&worst, NULL, 0) == 0);
  return 8;
}

int main(void) {
  int checks = 0;
  checks += test_boot_arg_policy();
  checks += test_adt_translation_validation();
  checks += test_gcc_gate_and_full_capture();
  checks += test_ice_off_still_allows_gate();
  checks += test_clock_gated_blocks_sdhci_reads();
  checks += test_format_bounds_and_worst_case();
  printf("emmc_ro_host: %d checks passed\n", checks);
  return 0;
}
